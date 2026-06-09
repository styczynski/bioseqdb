#ifndef _FMI_WRAPPER_H
#define _FMI_WRAPPER_H

#ifndef _FM_INDEX
#define _FM_INDEX
#include "bwt.h"
#include "bntseq.h"
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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FMI_wrapper FMI_wrapper;

FMI_wrapper* FMI_wrapper_create(const char *prefix);
void FMI_wrapper_load_index(FMI_wrapper *obj, fmIndex *loadedIndex);

void FMI_wrapper_destroy(FMI_wrapper *obj);

#ifdef __cplusplus
}
#endif

#endif
