#ifndef SEEDING_KERNEL_H
#define SEEDING_KERNEL_H

#ifdef __CUDACC__

#include "common.h"
#include "minimap.h"
#include "mmpriv.h"
#include "khash.h"
#include "params.h"

#define FULL_MASK 0xffffffff

typedef struct mm_idx_bucket_s {
	mm128_v a;   // (minimizer, position) array
	int32_t n;   // size of the _p_ array
	uint64_t *p; // position array for minimizers appearing >1 times
	void *h;     // hash table indexing _p_ and minimizers appearing once
} mm_idx_bucket_t;


#define idx_hash(a) ((a)>>1)
#define idx_eq(a, b) ((a)>>1 == (b)>>1)
KHASH_INIT(idx, uint64_t, uint64_t, 1, idx_hash, idx_eq)
typedef khash_t(idx) idxhash_t;

#define __ac_isempty(flag, i) ((flag[i>>4]>>((i&0xfU)<<1))&2)
#define __ac_isdel(flag, i) ((flag[i>>4]>>((i&0xfU)<<1))&1)
#define __ac_iseither(flag, i) ((flag[i>>4]>>((i&0xfU)<<1))&3)
#define __ac_set_isdel_false(flag, i) (flag[i>>4]&=~(1ul<<((i&0xfU)<<1)))
#define __ac_set_isempty_false(flag, i) (flag[i>>4]&=~(2ul<<((i&0xfU)<<1)))
#define __ac_set_isboth_false(flag, i) (flag[i>>4]&=~(3ul<<((i&0xfU)<<1)))
#define __ac_set_isdel_true(flag, i) (flag[i>>4]|=1ul<<((i&0xfU)<<1))

#define khash_val_d(h, x) ((h)->vals[x])
#define khash_key_d(h, x) ((h)->keys[x])

__global__
void mm_sketch_device(mm_idx_t* idx, char* seqs, int* lens, int* offset, int* rids, mm128_t* min, int* n_seed, int n_task);
__global__
void mm_collect_seed_hits_device(mm_idx_t* idx, mm_mapopt_t* opt, mm128_t* mmi, int* lens, int* offset, uint64_t* out_x, uint64_t* out_y, int* n_seed, int* n_match, mm_seed_t* g_tmp_seed, int* anchor_offset, int n_task, int32_t* global_num_match, int* end_ofs);
#endif // __CUDACC__
#endif // SEEDING_KERNEL_H
