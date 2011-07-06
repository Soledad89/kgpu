/* -*- linux-c -*-
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the GPL-COPYING file in the top-level directory.
 *
 * Copyright (c) 2010-2011 University of Utah and the Flux Group.
 * All rights reserved.
 * 
 * GPU accelerated AES-CTR cipher
 * 
 * This module is mostly based on the crypto/ctr.c in Linux kernel.
 *
 * At this time, we support simple coutner mode only. CTR mode for
 * IPSec is not implemented.
 *
 */

#include <crypto/algapi.h>
#include <crypto/ctr.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <crypto/aes.h>
#include "../../../kgpu/kgpu.h"
#include "../gaesk.h"

struct crypto_ctr_ctx {
    struct crypto_cipher *child;
    struct crypto_gaes_ctr_info info;
    u8 key[AES_MAX_KEY_SIZE];	
};

static int _crypto_ctr_setkey(struct crypto_tfm *parent, const u8 *key,
			     unsigned int keylen, int use_lctr)
{
    struct crypto_ctr_ctx *ctx = crypto_tfm_ctx(parent);
    struct crypto_cipher *child = ctx->child;
    struct crypto_gaes_ctr_config *cfg = NULL;
    int err;

    crypto_cipher_clear_flags(child, CRYPTO_TFM_REQ_MASK);
    crypto_cipher_set_flags(child, crypto_tfm_get_flags(parent) &
			    CRYPTO_TFM_REQ_MASK);

    /* local couter cipher, with explict range size */
    if (use_lctr) {
	if (keylen > AES_MAX_KEY_SIZE) {
	    cfg = (struct crypto_gaes_ctr_config*)(key+AES_MAX_KEY_SIZE);
	    keylen = cfg->key_length;
	    ctx->info.ctr_range = cfg->ctr_range;
	    if (cfg->ctr_range > PAGE_SIZE) {
		printk(
		    "[gaes_ctr] Error: local counter range "
		    "%u is larger than PAGE_SIZE!",
		    cfg->ctr_range);
		return -EINVAL;
	    }		
	} else {
	    ctx->info.ctr_range = PAGE_SIZE;
	}
    } else {
	ctx->info.ctr_range = 0;
    }
	
    err = crypto_cipher_setkey(child, key, keylen);
    err = crypto_aes_expand_key(/* yes, the next line is dangerous */
	(struct crypto_aes_ctx*)(&ctx->info),
	key, keylen);

    cvt_endian_u32(ctx->info.key_enc, AES_MAX_KEYLENGTH_U32);
    cvt_endian_u32(ctx->info.key_dec, AES_MAX_KEYLENGTH_U32);

    memcpy(ctx->key, key, keylen);
	
    crypto_tfm_set_flags(parent, crypto_cipher_get_flags(child) &
			 CRYPTO_TFM_RES_MASK);

    return err;
}

static int crypto_ctr_setkey(struct crypto_tfm *parent, const u8 *key,
			     unsigned int keylen)
{
    return _crypto_ctr_setkey(parent, key, keylen, 0);
}

static int crypto_lctr_setkey(struct crypto_tfm *parent, const u8 *key,
			      unsigned int keylen)
{
    return _crypto_ctr_setkey(parent, key, keylen, 1);
}

static void crypto_ctr_crypt_final(struct blkcipher_walk *walk,
				   struct crypto_cipher *tfm,
				   unsigned int donebytes,
				   unsigned int ctr_range)
{
    unsigned int bsize = crypto_cipher_blocksize(tfm);
    unsigned long alignmask = crypto_cipher_alignmask(tfm);
    u8 *ctrblk = walk->iv;
    u8 tmp[bsize + alignmask];
    u8 *keystream = PTR_ALIGN(tmp + 0, alignmask + 1);
    u8 *src = walk->src.virt.addr;
    u8 *dst = walk->dst.virt.addr;
    unsigned int nbytes = walk->nbytes;

    /* for local counter mode */
    if (ctr_range && donebytes%ctr_range==0) {
	memset(ctrblk, 0, bsize);
    }

    crypto_cipher_encrypt_one(tfm, keystream, ctrblk);
    crypto_xor(keystream, src, nbytes);
    memcpy(dst, keystream, nbytes);

    crypto_inc(ctrblk, bsize);
}

static int crypto_ctr_crypt_segment(struct blkcipher_walk *walk,
				    struct crypto_cipher *tfm,
				    unsigned int donebytes,
				    unsigned int ctr_range)
{
    void (*fn)(struct crypto_tfm *, u8 *, const u8 *) =
	crypto_cipher_alg(tfm)->cia_encrypt;
    unsigned int bsize = crypto_cipher_blocksize(tfm);
    u8 *ctrblk = walk->iv;
    u8 *src = walk->src.virt.addr;
    u8 *dst = walk->dst.virt.addr;
    unsigned int nbytes = walk->nbytes;

    do {
	/* for local counter mode */
	if (ctr_range && donebytes%ctr_range==0) {
	    memset(ctrblk, 0, bsize);
	}
	
	/* create keystream */
	fn(crypto_cipher_tfm(tfm), dst, ctrblk);
	crypto_xor(dst, src, bsize);

	/* increment counter in counterblock */
	crypto_inc(ctrblk, bsize);

	src += bsize;
	dst += bsize;
	donebytes += bsize;
    } while ((nbytes -= bsize) >= bsize);

    return nbytes;
}

static int crypto_ctr_crypt_inplace(struct blkcipher_walk *walk,
				    struct crypto_cipher *tfm,
				    unsigned int donebytes,
				    unsigned int ctr_range)
{
    void (*fn)(struct crypto_tfm *, u8 *, const u8 *) =
	crypto_cipher_alg(tfm)->cia_encrypt;
    unsigned int bsize = crypto_cipher_blocksize(tfm);
    unsigned long alignmask = crypto_cipher_alignmask(tfm);
    unsigned int nbytes = walk->nbytes;
    u8 *ctrblk = walk->iv;
    u8 *src = walk->src.virt.addr;
    u8 tmp[bsize + alignmask];
    u8 *keystream = PTR_ALIGN(tmp + 0, alignmask + 1);

    do {
	/* for local counter mode */
	if (ctr_range && donebytes%ctr_range==0) {
	    memset(ctrblk, 0, bsize);
	}
	
	/* create keystream */
	fn(crypto_cipher_tfm(tfm), keystream, ctrblk);
	crypto_xor(src, keystream, bsize);

	/* increment counter in counterblock */
	crypto_inc(ctrblk, bsize);

	src += bsize;
	donebytes += bsize;
    } while ((nbytes -= bsize) >= bsize);

    return nbytes;
}

static int
crypto_ctr_crypt(struct blkcipher_desc *desc,
		 struct scatterlist *dst, struct scatterlist *src,
		 unsigned int nbytes)
{
    struct blkcipher_walk walk;
    struct crypto_blkcipher *tfm = desc->tfm;
    struct crypto_ctr_ctx *ctx = crypto_blkcipher_ctx(tfm);
    struct crypto_cipher *child = ctx->child;
    unsigned int bsize = crypto_cipher_blocksize(child);
    int err;
    unsigned int donebytes = 0;

    blkcipher_walk_init(&walk, dst, src, nbytes);
    err = blkcipher_walk_virt_block(desc, &walk, bsize);

    while (walk.nbytes >= bsize) {
	if (walk.src.virt.addr == walk.dst.virt.addr)
	    nbytes = crypto_ctr_crypt_inplace(
		&walk, child, donebytes, ctx->info.ctr_range);
	else
	    nbytes = crypto_ctr_crypt_segment(
		&walk, child, donebytes, ctx->info.ctr_range);

	donebytes += walk.nbytes-nbytes;
	err = blkcipher_walk_done(desc, &walk, nbytes);
    }

    if (walk.nbytes) {
	if (ctx->info.ctr_range) {
	    printk(
		"[gaes_ctr] Warnning: We got a problem: "
		"the size of data, %u, is not a multiple of "
		"block size ...", nbytes);
	}
	crypto_ctr_crypt_final(&walk, child,
			       donebytes, ctx->info.ctr_range);
	err = blkcipher_walk_done(desc, &walk, 0);
    }

    return err;
}

static int
_crypto_gaes_ctr_crypt(
    struct blkcipher_desc *desc,
    struct scatterlist *dst, struct scatterlist *src,
    unsigned int sz)
{
    int err=0;
    unsigned int rsz = roundup(sz, PAGE_SIZE);
    unsigned int nbytes;
    unsigned long cpdbytes = 0;
    u8* gpos;
    u8 *ctrblk;	
    
    struct kgpu_req *req;
    struct kgpu_resp *resp;
    struct kgpu_buffer *buf;
	
    struct crypto_blkcipher *tfm = desc->tfm;
    struct crypto_ctr_ctx *ctx = crypto_blkcipher_ctx(tfm);
    struct blkcipher_walk walk;

    blkcipher_walk_init(&walk, dst, src, sz);
    
    buf = alloc_gpu_buffer(rsz+2*PAGE_SIZE);
    if (!buf) {
	printk("[gaes_ctr] Error: GPU buffer is null.\n");
	return -EFAULT;
    }
	
    req = alloc_kgpu_request();
    resp = alloc_kgpu_response();
    if (!req || !resp) {
	return -EFAULT;
    }
	
    err = blkcipher_walk_virt(desc, &walk);
    ctrblk = walk.iv;
	
    while ((nbytes = walk.nbytes)) {
	u8 *wsrc = walk.src.virt.addr;
	unsigned long offset = cpdbytes&(PAGE_SIZE-1);
	unsigned long idx = cpdbytes>>PAGE_SHIFT;
	if (nbytes > PAGE_SIZE) {
	    return -EFAULT;
	}

	gpos = (u8*)(buf->pas[idx])+offset;
	while (nbytes > PAGE_SIZE-offset) { /* 'if' should be fine */
	    unsigned long realsz = PAGE_SIZE-offset;
	    memcpy(__va(gpos), wsrc, realsz);
	    cpdbytes += realsz;
	    nbytes   -= realsz;
	    idx       = cpdbytes>>PAGE_SHIFT;
	    offset    = cpdbytes&(PAGE_SIZE-1);
	    wsrc     += realsz;
	    gpos      = (u8*)(buf->pas[idx])+offset;
	}
	memcpy(__va(gpos), wsrc, nbytes);
	cpdbytes += nbytes;	
		
	err = blkcipher_walk_done(desc, &walk, 0);
    }
	
    gpos = (u8*)(buf->pas[rsz>>PAGE_SHIFT])+(rsz&(PAGE_SIZE-1));
    memcpy(__va(gpos), &(ctx->info), sizeof(struct crypto_gaes_ctr_info));
    if (ctrblk)
	memcpy(((struct crypto_gaes_ctr_info*)__va(gpos))->ctrblk, ctrblk,
	       crypto_cipher_blocksize(ctx->child));

    if (ctx->info.ctr_range) {
	strcpy(req->kureq.sname, "gaes_lctr");
	memset(((struct crypto_gaes_ctr_info*)__va(gpos))->ctrblk, 0,
	       crypto_cipher_blocksize(ctx->child));
    }
    else
	strcpy(req->kureq.sname, "gaes_ctr");
    req->kureq.input    = buf->va;
    req->kureq.output   = buf->va;
    req->kureq.insize   = rsz+PAGE_SIZE;
    req->kureq.outsize  = rsz;
    req->kureq.data     = (u8*)(buf->va)+rsz;
    req->kureq.datasize = sizeof(struct crypto_gaes_ctr_info);
	
    if (call_gpu_sync(req, resp)) {
	err = -EFAULT;
	printk("[gaes_ctr] Error: callgpu error\n");
    } else {
	cpdbytes = 0;
	blkcipher_walk_init(&walk, dst, src, sz);
	err = blkcipher_walk_virt(desc, &walk);
		
	while ((nbytes = walk.nbytes)) {
	    u8 *wdst = walk.dst.virt.addr;
	    unsigned long offset = cpdbytes&(PAGE_SIZE-1);
	    unsigned long idx = cpdbytes>>PAGE_SHIFT;
	    if (nbytes > PAGE_SIZE) {
		return -EFAULT;
	    }
			
	    gpos = (u8*)(buf->pas[idx])+offset;
	    /* 'if' should be fine */
	    while (nbytes > PAGE_SIZE-offset) { 
		unsigned long realsz = PAGE_SIZE-offset;
		memcpy(wdst, __va(gpos), realsz);
		cpdbytes += realsz;
		nbytes   -= realsz;
		idx       = cpdbytes>>PAGE_SHIFT;
		offset    = cpdbytes&(PAGE_SIZE-1);
		wdst     += realsz;
		gpos      = (u8*)(buf->pas[idx])+offset;
	    }
	    memcpy(wdst, __va(gpos), nbytes);       
	    cpdbytes += nbytes;
			
	    err = blkcipher_walk_done(desc, &walk, 0);
	}

	/* change counter value */
	if (!ctx->info.ctr_range)
	    big_u128_add(ctrblk, sz/crypto_cipher_blocksize(ctx->child),
			 ctrblk);
    }
	
    free_kgpu_request(req);
    free_kgpu_response(resp);
    free_gpu_buffer(buf);
	
    return err;
}

static int
crypto_gaes_ctr_crypt(
    struct blkcipher_desc *desc,
    struct scatterlist *dst, struct scatterlist *src,
    unsigned int nbytes)
{
    if (/*(nbytes % PAGE_SIZE) ||*/ nbytes <= GAES_CTR_SIZE_THRESHOLD)
	return crypto_ctr_crypt(desc, dst, src, nbytes);
    return _crypto_gaes_ctr_crypt(desc, dst, src, nbytes);
}

static int
crypto_gaes_lctr_crypt(
    struct blkcipher_desc *desc,
    struct scatterlist *dst, struct scatterlist *src,
    unsigned int nbytes)
{
    struct crypto_blkcipher *tfm = desc->tfm;
    struct crypto_ctr_ctx *ctx = crypto_blkcipher_ctx(tfm);
    
    if (nbytes % ctx->info.ctr_range) {
	printk("[gaes_ctr] Warnning: using local "
	       "counter mode, but data size is not "
	       "multiple of %u\n", ctx->info.ctr_range);
	return crypto_ctr_crypt(desc, dst, src, nbytes);
    }		
    return _crypto_gaes_ctr_crypt(desc, dst, src, nbytes);
}

static int crypto_ctr_init_tfm(struct crypto_tfm *tfm)
{
    struct crypto_instance *inst = (void *)tfm->__crt_alg;
    struct crypto_spawn *spawn = crypto_instance_ctx(inst);
    struct crypto_ctr_ctx *ctx = crypto_tfm_ctx(tfm);
    struct crypto_cipher *cipher;

    cipher = crypto_spawn_cipher(spawn);
    if (IS_ERR(cipher))
	return PTR_ERR(cipher);

    ctx->child = cipher;

    return 0;
}

static void crypto_ctr_exit_tfm(struct crypto_tfm *tfm)
{
    struct crypto_ctr_ctx *ctx = crypto_tfm_ctx(tfm);

    crypto_free_cipher(ctx->child);
}

static struct crypto_instance*
_crypto_ctr_alloc(struct rtattr **tb, int use_lctr)
{
    struct crypto_instance *inst;
    struct crypto_alg *alg;
    int err;

    err = crypto_check_attr_type(tb, CRYPTO_ALG_TYPE_BLKCIPHER);
    if (err)
	return ERR_PTR(err);

    alg = crypto_attr_alg(tb[1], CRYPTO_ALG_TYPE_CIPHER,
			  CRYPTO_ALG_TYPE_MASK);
    if (IS_ERR(alg))
	return ERR_CAST(alg);

    /* Block size must be >= 4 bytes. */
    err = -EINVAL;
    if (alg->cra_blocksize < 4)
	goto out_put_alg;

    /* If this is false we'd fail the alignment of crypto_inc. */
    if (alg->cra_blocksize % 4)
	goto out_put_alg;

    if (use_lctr)
	inst = crypto_alloc_instance("gaes_lctr", alg);
    else
	inst = crypto_alloc_instance("gaes_ctr", alg);
    if (IS_ERR(inst))
	goto out;

    inst->alg.cra_flags = CRYPTO_ALG_TYPE_BLKCIPHER;
    inst->alg.cra_priority = alg->cra_priority;
    inst->alg.cra_blocksize = 1;
    inst->alg.cra_alignmask = alg->cra_alignmask | (__alignof__(u32) - 1);
    inst->alg.cra_type = &crypto_blkcipher_type;

    inst->alg.cra_blkcipher.ivsize = alg->cra_blocksize;
    inst->alg.cra_blkcipher.min_keysize = alg->cra_cipher.cia_min_keysize;

    /* not quite sure whether this is OK */
    inst->alg.cra_blkcipher.max_keysize =
	use_lctr?
	alg->cra_cipher.cia_max_keysize+sizeof(struct crypto_gaes_ctr_config)
	:alg->cra_cipher.cia_max_keysize;

    inst->alg.cra_ctxsize = sizeof(struct crypto_ctr_ctx);

    inst->alg.cra_init = crypto_ctr_init_tfm;
    inst->alg.cra_exit = crypto_ctr_exit_tfm;

    inst->alg.cra_blkcipher.setkey =
	use_lctr?crypto_lctr_setkey:crypto_ctr_setkey;
    inst->alg.cra_blkcipher.encrypt =
	use_lctr?crypto_gaes_lctr_crypt:crypto_gaes_ctr_crypt;
    inst->alg.cra_blkcipher.decrypt =
	use_lctr?crypto_gaes_lctr_crypt:crypto_gaes_ctr_crypt;

    inst->alg.cra_blkcipher.geniv = "chainiv";

out:
    crypto_mod_put(alg);
    return inst;

out_put_alg:
    inst = ERR_PTR(err);
    goto out;
}

static struct crypto_instance *crypto_ctr_alloc(struct rtattr **tb)
{
    return _crypto_ctr_alloc(tb, 0);
}

static struct crypto_instance *crypto_lctr_alloc(struct rtattr **tb)
{
    return _crypto_ctr_alloc(tb, 1);
}

static void crypto_ctr_free(struct crypto_instance *inst)
{
    crypto_drop_spawn(crypto_instance_ctx(inst));
    kfree(inst);
}

static struct crypto_template crypto_ctr_tmpl = {
    .name = "gaes_ctr",
    .alloc = crypto_ctr_alloc,
    .free = crypto_ctr_free,
    .module = THIS_MODULE,
};

static struct crypto_template crypto_lctr_tmpl = {
    .name = "gaes_lctr",
    .alloc = crypto_lctr_alloc,
    .free = crypto_ctr_free,
    .module = THIS_MODULE,
};

static int __init crypto_ctr_module_init(void)
{
    int err;

    err = crypto_register_template(&crypto_ctr_tmpl);
    err |= crypto_register_template(&crypto_lctr_tmpl);
    return err;
}

static void __exit crypto_ctr_module_exit(void)
{
    crypto_unregister_template(&crypto_lctr_tmpl);
    crypto_unregister_template(&crypto_ctr_tmpl);
}

module_init(crypto_ctr_module_init);
module_exit(crypto_ctr_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPU AES-CTR Counter block mode");
