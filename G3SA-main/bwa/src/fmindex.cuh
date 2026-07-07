#ifndef _FMINDEX_CUH
#define _FMINDEX_CUH

#include <stdint.h>
#include "bwa.h"
#include "bwt.h"

#ifndef _FM_INDEX
#define _FM_INDEX
typedef struct checkpoint_occ_scalar
{
    int64_t cp_count[4];
    uint64_t one_hot_bwt_str[4];
}CP_OCC;

typedef struct checkpoint_occ2_scalar
{
    int64_t cp_count[16];
    uint64_t one_hot_bwt_str[16];
}CP_OCC2;
// indices
typedef struct {
    uint64_t *oneHot;
    CP_OCC *cpOcc;
    CP_OCC2 *cpOcc2;
    int64_t cpOccSize;
    int64_t *count;
    int64_t *count2;
    uint8_t *firstBase;
    int64_t *sentinelIndex;
    bwt_t *bwt;
	bntseq_t *bns;
	uint8_t *pac;
    int8_t *suffixArrayMsByte;
    uint32_t *suffixArrayLsWord;
    int64_t *referenceLen;
    uint8_t *packedBwt;
} fmIndex;
#endif

// sets *rb as suffixArray[k]. it works for the compressed SA.
extern __device__ void sa_lookup(const fmIndex *devFmIndex, uint64_t k, int64_t *rb);


extern __device__ void bwt_smem_rightWIP(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_intv, uint64_t max_intv, int min_seed_len, bwtintv_t *mem_a, kmers_bucket_t *d_kmersHashTab, uint64_t *d_one_hot, CP_OCC *d_cp_occ, int64_t *d_count, CP_OCC2 *d_cp_occ2, int64_t *d_count2, uint8_t *d_first_base);

extern __device__ void bwt_smem_right(const bwt_t *bwt, int len, const uint8_t *q, int x, int min_intv, uint64_t max_intv, int min_seed_len, bwtintv_t *mem_a, kmers_bucket_t *d_kmersHashTab, uint64_t *d_one_hot, CP_OCC *d_cp_occ, int64_t *count);


extern __device__ bwtint_t bwt_sa_gpu(const bwt_t *bwt, bwtint_t k);

extern __device__ void backwardExt(const int64_t sentinel_index, const bwtintv_t *smem, uint8_t base, bwtintv_t *nextSmem, const uint64_t *d_one_hot, const CP_OCC *d_cp_occ, const int64_t *d_count);


// smem -> base1, base0, smem
extern __device__ void backwardExt2(const int64_t sentinel_index, const bwtintv_t *smem, uint8_t base0, uint8_t base1, bwtintv_t *nextSmem, const uint64_t *d_one_hot, const CP_OCC *d_cp_occ, const int64_t *d_count, const CP_OCC2 *d_cp_occ2, const int64_t *d_count2, const uint8_t *d_first_base);

extern __device__ void backwardExtBackward(const int64_t sentinel_index, const bwtintv_t *smem, uint8_t base, bwtintv_t *nextSmem, const uint64_t *d_one_hot, const CP_OCC *d_cp_occ, const int64_t *d_count);


// smem -> base1, base0, smem
extern __device__ void backwardExt2Backward(const int64_t sentinel_index, const bwtintv_t *smem, uint8_t base0, uint8_t base1, bwtintv_t *nextSmem, const uint64_t *d_one_hot, const CP_OCC *d_cp_occ, const int64_t *d_count, const CP_OCC2 *d_cp_occ2, const int64_t *d_count2, const uint8_t *d_first_base);
#endif
