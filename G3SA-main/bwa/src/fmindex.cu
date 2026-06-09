#include "bwa.h"
//#include "FMI_search.h"
#include "fmindex.cuh"

__device__ int devicehashK(const uint8_t* s){
    int out = 0;
    for (int i=0; i<KMER_K; i++){
        if (s[i]==4) return -1;
        out += s[i]*pow4(KMER_K-1-i);
    }
    return out;
}

/* ONLY FOR  REFERENCE
// generate initial bwt intervals for sub-sequence of length K starting from position i
// write result to *interval, return success/failure
__device__ bool bwt_KMerHashInit(int qlen, const uint8_t *q, int i, kmers_bucket_t *d_kmersHashTab, bwtintv_lite_t *interval){
	if (i>qlen-1-KMER_K) return false;	// not enough space to the right for extension
	int hashValue = devicehashK(&q[i]);
	if (hashValue==-1) return false;	// hash N in this substring
	kmers_bucket_t entry = d_kmersHashTab[hashValue];
	interval->x0 = entry.x[0]; interval->x1 = entry.x[1]; interval->x2 = entry.x[2];
	interval->start = i;
	interval->end = i+KMER_K-1;
	return true;
}
*/

#define \
GET_OCC_GPU(pp, c, occ_id_pp, y_pp, occ_pp, one_hot_bwt_str_c_pp, match_mask_pp) \
                int64_t occ_id_pp = pp >> CP_SHIFT; \
                int64_t y_pp = pp & CP_MASK; \
                int64_t occ_pp = d_cp_occ[occ_id_pp].cp_count[c]; \
                uint64_t one_hot_bwt_str_c_pp = d_cp_occ[occ_id_pp].one_hot_bwt_str[c]; \
                uint64_t match_mask_pp = one_hot_bwt_str_c_pp & d_one_hot[y_pp]; \
                occ_pp += __popcll(match_mask_pp);

#define \
GET_OCC2_GPU(pp, c, occ_id_pp, y_pp, occ_pp, one_hot_bwt_str_c_pp, match_mask_pp) \
                int64_t occ_id_pp = pp >> CP_SHIFT; \
                int64_t y_pp = pp & CP_MASK; \
                int64_t occ_pp = d_cp_occ2[occ_id_pp].cp_count[c]; \
                uint64_t one_hot_bwt_str_c_pp = d_cp_occ2[occ_id_pp].one_hot_bwt_str[c]; \
                uint64_t match_mask_pp = one_hot_bwt_str_c_pp & d_one_hot[y_pp]; \
                occ_pp += __popcll(match_mask_pp);


// b is guaranteed to be a base among A, C, G, T.
__device__ void LFMap(const fmIndex *devFmIndex, uint64_t k, uint8_t b, uint64_t *bk)
{
    int64_t referenceLen = *(devFmIndex->referenceLen);
    uint8_t *packedBwt = devFmIndex->packedBwt;
    CP_OCC *d_cp_occ = devFmIndex->cpOcc; // d_ prefix to match the MACRO definition for now
    uint64_t *d_one_hot = devFmIndex->oneHot;
    
    GET_OCC_GPU(k, b, occ_id_k, y_k, occ_k, one_hot_bwt_str_b_k, match_mask_k);
    
    int64_t *count = devFmIndex->count;
    *bk = count[b] + occ_k;
}

__device__ void sa_lookup(const fmIndex *devFmIndex, uint64_t k, int64_t *rb)
{
    int64_t referenceLen = *(devFmIndex->referenceLen);
    uint32_t *suffixArrayLsWord = devFmIndex->suffixArrayLsWord;
    int8_t *suffixArrayMsByte = devFmIndex->suffixArrayMsByte;
    uint8_t *packedBwt = devFmIndex->packedBwt;
    int64_t sentinelIndex = *(devFmIndex->sentinelIndex);

    //#if SA_COMPRESSION should be compressed to load on the device memory.
    int offset = 0;
    uint8_t bwt_b;
    uint32_t packed_idx;
    uint8_t packed_offset;
    while(k & SA_COMPX_MASK)
    {
        if(k == sentinelIndex)
        {
            *rb = offset;
            return;
        } 

        // get bwt_b
        packed_idx = k >> 2;
        packed_offset = k & 0x3;
        bwt_b = packedBwt[packed_idx];
        for(uint8_t ii = 0; ii < 3 - packed_offset; ii++)
        {
            bwt_b = bwt_b >> 2;
        }
        bwt_b = bwt_b & 0x3;

        LFMap(devFmIndex, k, bwt_b, &k);
        offset++;
    }

    int64_t sa_entry = suffixArrayMsByte[k >> SA_COMPX];
    sa_entry = sa_entry << 32;
    sa_entry = sa_entry + suffixArrayLsWord[k >> SA_COMPX];
    sa_entry += offset;

    *rb = sa_entry;
}


//#define TID_BID_OF_INTEREST
__device__ void backwardExt(const int64_t sentinel_index, const bwtintv_t *smem, uint8_t base, bwtintv_t *nextSmem, const uint64_t *d_one_hot, const CP_OCC *d_cp_occ, const int64_t *d_count)
{
#ifdef TID_BID_OF_INTEREST
    if(threadIdx.x == 1)
    {
    printf("Before extending: y = %d, P = (k, l, s, m, n) = (%ld, %ld, %ld, %u, %u)\n", base, smem->x[0], smem->x[1], smem->x[2], (uint32_t)((smem->info) >> 32), (uint32_t)(smem->info));
    }
#endif
	uint8_t b;
	int64_t k[4], l[4], s[4];
	for(b = 0; b < 4; b++)
	{
#if 0
        if (b == base)
        {
            printf("[%s] base = %d, before setting start and end SA positions \n", __func__, b);
        }
#endif
		int64_t sp = (int64_t)(smem->x[0]) - 1;
		int64_t ep = (int64_t)(smem->x[0]) + (int64_t)(smem->x[2]) - 1;
		GET_OCC_GPU(sp, b, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);
		GET_OCC_GPU(ep, b, occ_id_ep, y_ep, occ_ep, one_hot_bwt_str_c_ep, match_mask_ep);
		k[b] = d_count[b] + occ_sp;
		s[b] = occ_ep - occ_sp;
#if 0
        if (b == base)
        {
            printf("base: %d, d_count[base] %lu, occ_sp %lu, occ_ep %lu\n", b, d_count[b], occ_sp, occ_ep);
            printf("sp: %ld, occ_sp = d_cp_occ[occ_id_sp].cp_count[base] %ld\n", sp, occ_sp);
        }
#endif
	}

	int64_t sentinel_offset = 0;
	//int64_t sentinel_index = bwt->primary;
    //printf("[%s] before setting the l values \n", __func__);
	if((smem->x[0] <= sentinel_index) && ((smem->x[0] + smem->x[2]) > sentinel_index)) sentinel_offset = 1;
	l[3] = smem->x[1] + sentinel_offset;
	l[2] = l[3] + s[3];
	l[1] = l[2] + s[2];
	l[0] = l[1] + s[1];

    //printf("[%s] before finally setting the nextSmem array \n", __func__);
    nextSmem->x[0] = k[base];
    nextSmem->x[1] = l[base];
    nextSmem->x[2] = s[base];
#ifdef TID_BID_OF_INTEREST
    if(threadIdx.x == 1)
    {
    printf("After extending: y = %d, yP = (k, l, s, m, n) = (%ld, %ld, %ld, %u, %u)\n", base, nextSmem->x[0], nextSmem->x[1], nextSmem->x[2], (uint32_t)((nextSmem->info) >> 32), (uint32_t)(nextSmem->info));
    }
#endif
    return;
}

__device__ void backwardExtBackward(const int64_t sentinel_index, const bwtintv_t *smem, uint8_t base, bwtintv_t *nextSmem, const uint64_t *d_one_hot, const CP_OCC *d_cp_occ, const int64_t *d_count)
{
#if 0
    printf("Before extending: y = %d, P = (k, l, s, m, n) = (%ld, %ld, %ld, %u, %u)\n", base, smem->x[0], smem->x[1], smem->x[2], (uint32_t)((smem->info) >> 32), (uint32_t)(smem->info));
#endif
    int64_t sp = (int64_t)(smem->x[0]) - 1;
    int64_t ep = (int64_t)(smem->x[0]) + (int64_t)(smem->x[2]) - 1;
    GET_OCC_GPU(sp, base, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);
    GET_OCC_GPU(ep, base, occ_id_ep, y_ep, occ_ep, one_hot_bwt_str_c_ep, match_mask_ep);
    uint64_t newK = d_count[base] + occ_sp;
    uint64_t newS = occ_ep - occ_sp;
        
    nextSmem->x[0] = newK;
    nextSmem->x[2] = newS;
#if 0
    printf("After extending: y = %d, yP = (k, l, s, m, n) = (%ld, %ld, %ld, %u, %u)\n", base, nextSmem->x[0], nextSmem->x[1], nextSmem->x[2], (uint32_t)((nextSmem->info) >> 32), (uint32_t)(nextSmem->info));
#endif
    return;
}


/**
 *  extend 2 bases at once backward. also supports forward extend
 *  base pair order:
 *  0   1   2   3   4   5   6   7   8   9   10  11  12  13  14  15
 *  AA  AC  AG  AT  CA  CC  CG  CT  GA  GC  GG  GT  TA  TC  TG  TT
 *  idx: (base1, base0) 
 */
__device__ void backwardExt2(const int64_t sentinel_index, const bwtintv_t *smem, uint8_t base0, uint8_t base1, bwtintv_t *nextSmem, const uint64_t *d_one_hot, const CP_OCC *d_cp_occ, const int64_t *d_count, const CP_OCC2 *d_cp_occ2, const int64_t *d_count2, const uint8_t *d_first_base)
{
#if 0
    printf("Before extending: x, y = %d, %d, P = (k, l, s, m, n) = (%ld, %ld, %ld, %u, %u)\n", base1, base0, smem->x[0], smem->x[1], smem->x[2], (uint32_t)((smem->info) >> 32), (uint32_t)(smem->info));
#endif
    uint8_t b;
    uint8_t basePair = base1 * 4 + base0;
    int64_t k[16], l[16], s[16];
    for (b = 0; b < 16; b++)
	{
        //printf("[%s] b = %d, before setting start and end SA positions \n", __func__, b);
		int64_t sp = (int64_t)(smem->x[0]) - 1;
		int64_t ep = (int64_t)(smem->x[0]) + (int64_t)(smem->x[2]) - 1;

		GET_OCC2_GPU(sp, b, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);
		GET_OCC2_GPU(ep, b, occ_id_ep, y_ep, occ_ep, one_hot_bwt_str_c_ep, match_mask_ep);

		k[b] = d_count2[b] + occ_sp;
		s[b] = occ_ep - occ_sp;
        //printf("b: %d, d_count[b] %lu, occ_sp %lu, occ_ep %lu\n", b, d_count[b], occ_sp, occ_ep);
        //printf("sp: %ld, occ_sp = d_cp_occ[occ_id_sp].cp_count[b] %ld\n", sp, occ_sp);
	}

	int64_t sentinel_offset = 0;
	//int64_t sentinel_index = bwt->primary;
    //printf("[%s] before setting the l values \n", __func__);
	if((smem->x[0] <= sentinel_index) && ((smem->x[0] + smem->x[2]) > sentinel_index))
    {
       sentinel_offset = 1;
    }
	int64_t sentinel_offset2 = 0;
    uint8_t first_base = *d_first_base;
    bwtintv_t check2;
    backwardExt(sentinel_index, smem, first_base, &check2, d_one_hot, d_cp_occ, d_count);
    if((check2.x[0] <= sentinel_index) && (check2.x[0] + check2.x[2]) > sentinel_index)
    {
        sentinel_offset2 = 1;
    }

#define AA_ 0
#define AC_ 1
#define AG_ 2
#define AT_ 3
#define CA_ 4
#define CC_ 5
#define CG_ 6
#define CT_ 7
#define GA_ 8
#define GC_ 9
#define GG_ 10
#define GT_ 11
#define TA_ 12
#define TC_ 13
#define TG_ 14
#define TT_ 15
    l[TT_] = smem->x[1] + sentinel_offset;
    l[GT_] = l[TT_] + s[TT_];
    l[CT_] = l[GT_] + s[GT_];
    l[AT_] = l[CT_] + s[CT_];
    l[TG_] = l[AT_] + s[AT_];
    l[GG_] = l[TG_] + s[TG_];
    l[CG_] = l[GG_] + s[GG_];
    l[AG_] = l[CG_] + s[CG_];
    l[TC_] = l[AG_] + s[AG_];
    l[GC_] = l[TC_] + s[TC_];
    l[CC_] = l[GC_] + s[GC_];
    l[AC_] = l[CC_] + s[CC_];
    l[TA_] = l[AC_] + s[AC_];
    l[GA_] = l[TA_] + s[TA_];
    l[CA_] = l[GA_] + s[GA_];
    l[AA_] = l[CA_] + s[CA_];

    for (int jjj = first_base; jjj >= 0; jjj--)
    {
        for (int iii = 0; iii < 4; iii++)
        {
            b = iii * 4 + jjj;
            l[b] += sentinel_offset2;
        }
    }

    //printf("[%s] before finally setting the nextSmem array \n", __func__);
    nextSmem->x[0] = k[basePair];
    nextSmem->x[1] = l[basePair];
    nextSmem->x[2] = s[basePair];

#if 0
    printf("After extending: x, y = %d, %d, xyP = (k, l, s, m, n) = (%ld, %ld, %ld, %u, %u)\n", base1, base0, nextSmem->x[0], nextSmem->x[1], nextSmem->x[2], (uint32_t)((nextSmem->info) >> 32), (uint32_t)(nextSmem->info));
#endif
    return;
}

//#define TID_OF_INTEREST 55
__device__ void backwardExt2Backward(const int64_t sentinel_index, const bwtintv_t *smem, uint8_t base0, uint8_t base1, bwtintv_t *nextSmem, const uint64_t *d_one_hot, const CP_OCC *d_cp_occ, const int64_t *d_count, const CP_OCC2 *d_cp_occ2, const int64_t *d_count2, const uint8_t *d_first_base)
{
#ifdef TID_OF_INTEREST
    if(threadIdx.x == TID_OF_INTEREST)
    {
    printf("Before extending: x, y = %d, %d, P = (k, l, s, m, n) = (%ld, %ld, %ld, %u, %u)\n", base1, base0, smem->x[0], smem->x[1], smem->x[2], (uint32_t)((smem->info) >> 32), (uint32_t)(smem->info));
    }
#endif
    uint8_t basePair = base1 * 4 + base0;
        
    int64_t sp = (int64_t)(smem->x[0]) - 1;
    int64_t ep = (int64_t)(smem->x[0]) + (int64_t)(smem->x[2]) - 1;

    //GET_OCC2_GPU(sp, basePair, occ_id_sp, y_sp, occ_sp, one_hot_bwt_str_c_sp, match_mask_sp);
    //GET_OCC2_GPU(ep, basePair, occ_id_ep, y_ep, occ_ep, one_hot_bwt_str_c_ep, match_mask_ep);

//GET_OCC2_GPU(pp, c, occ_id_pp, y_pp, occ_pp, one_hot_bwt_str_c_pp, match_mask_pp) 
                int64_t occ_id_sp = sp >> CP_SHIFT; 
                int64_t y_sp = sp & CP_MASK; 
                int64_t occ_sp = d_cp_occ2[occ_id_sp].cp_count[basePair]; 
                uint64_t one_hot_bwt_str_c_sp = d_cp_occ2[occ_id_sp].one_hot_bwt_str[basePair]; 
                uint64_t match_mask_sp = one_hot_bwt_str_c_sp & d_one_hot[y_sp]; 
                occ_sp += __popcll(match_mask_sp);

                int64_t occ_id_ep = ep >> CP_SHIFT; 
                int64_t y_ep = ep & CP_MASK; 
                int64_t occ_ep = d_cp_occ2[occ_id_ep].cp_count[basePair]; 
                uint64_t one_hot_bwt_str_c_ep = d_cp_occ2[occ_id_ep].one_hot_bwt_str[basePair]; 
                uint64_t match_mask_ep = one_hot_bwt_str_c_ep & d_one_hot[y_ep]; 
                occ_ep += __popcll(match_mask_ep);

    uint64_t nextK = d_count2[basePair] + occ_sp;
    uint64_t nextS = occ_ep - occ_sp;
    nextSmem->x[0] = nextK;
    nextSmem->x[2] = nextS;

#ifdef TID_OF_INTEREST 
    if(threadIdx.x == TID_OF_INTEREST)
    {
    printf("After extending: x, y = %d, %d, xyP = (k, l, s, m, n) = (%ld, %ld, %ld, %u, %u)\n", base1, base0, nextSmem->x[0], nextSmem->x[1], nextSmem->x[2], (uint32_t)((nextSmem->info) >> 32), (uint32_t)(nextSmem->info));
    }
#endif
    return;
}
