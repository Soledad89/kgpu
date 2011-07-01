/*
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the GPL-COPYING file in the top-level directory.
 *
 * Copyright (c) 2010-2011 University of Utah and the Flux Group.
 * All rights reserved.
 *
 * Internal header used by KGPU only
 *
 */

#ifndef ___KKGPU_H__
#define ___KKGPU_H__

#include "kgpu.h"
#include <linux/types.h>

/*
 * Buffer management stuff, put them here in case we may
 * create a kgpu_buf.c for buffer related functions.
 */
#define KGPU_BUF_UNIT_SIZE (128*1024)
#define KGPU_BUF_NR_FRAMES_PER_UNIT (KGPU_BUF_UNIT_SIZE/PAGE_SIZE)

struct kgpu_mgmt_buffer {
    struct gpu_buffer gb;
    void **paddrs;
    unsigned int npages;
    unsigned int nunits;
    unsigned long *bitmap;
};

struct kgpu_allocated_buffer {
    struct kgpu_buffer buf;
    int mgmt_buf_idx;
};

/* memory ops */
extern unsigned long kgpu_virt2phy(unsigned long vaddr);
extern int
kgpu_check_phy_consecutiveness(unsigned long vaddr, size_t sz, size_t framesz);
extern void kgpu_dump_pages(unsigned long vaddr, unsigned long sz);
extern void kgpu_test_memory_pages(unsigned long vaddr, unsigned long sz);

#endif
