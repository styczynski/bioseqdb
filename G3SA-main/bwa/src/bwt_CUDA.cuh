#ifndef _BWT_CUDA_H
#define _BWT_CUDA_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "bwt.h"
#include "gmem_alloc.h"
#include "hashKMerIndex.h"

typedef struct // same as bwtintv_t, but use 32-bit for query position (info)
{
    __int128_t x0:35, x1:35, x2:35, start:11, end:11;
    // bwtint_t x[3];
    // uint32_t info;
} bwtintv_lite_t;

// use kMer hash to determine the interval of length K starting from position i. Return true if success. Write interval to *interval
extern __device__ bool bwt_KMerHashInit(int qlen, const uint8_t *q, int i, kmers_bucket_t *d_kmersHashTab, bwtintv_lite_t *interval);

// extend 1 to the right, write result in-place. interval is input and output. Return true if successfully extended, false otherwise
extern __device__ bool bwt_extend_right1(const bwt_t *bwt, int qlen, const uint8_t *q, int min_intv, uint64_t max_intv, bwtintv_lite_t *interval);

extern __device__ void bwt_smem_left(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_intv, uint64_t max_intv, int min_seed_len, bwtintv_v *mem);

extern __device__ void bwt_smem_right(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_intv, uint64_t max_intv, int min_seed_len, bwtintv_t *mem_a, kmers_bucket_t *d_kmersHashTab);

extern __device__ int bwt_smem1a_gpu(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_intv, uint64_t max_intv, bwtintv_v *mem, bwtintv_v *tmpvec[2], void* d_buffer_ptr);

extern __device__ int bwt_seed_strategy1_gpu(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_len, int max_intv, bwtintv_t *mem);

extern __device__ bwtint_t bwt_sa_gpu(const bwt_t *bwt, bwtint_t k);
#endif
