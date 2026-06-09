#include "FMI_wrapper.h"
#include "FMI_search.h"

struct FMI_wrapper {
	FMI_search* instance;
};

FMI_wrapper* FMI_wrapper_create(const char *filename)
{
	//fprintf(stderr, "[FMI wrapper] entered with filename %s\n", filename);
	FMI_wrapper *obj = new FMI_wrapper{new FMI_search(filename)};
	return obj;
}

void FMI_wrapper_load_index(FMI_wrapper *obj, fmIndex *loadedIndex)
{
	//fprintf(stderr, "[FMI wrapper] Loading index\n");
    obj->instance->load_index();
	//fprintf(stderr, "[FMI wrapper] Loaded index\n");
    loadedIndex->oneHot = obj->instance->one_hot_mask_array;
    loadedIndex->cpOcc = obj->instance->cp_occ;
    loadedIndex->cpOcc2 = obj->instance->cp_occ2;
    loadedIndex->cpOccSize = obj->instance->cp_occ_size;
    loadedIndex->count = obj->instance->count;
    loadedIndex->count2 = obj->instance->count2;
    loadedIndex->firstBase = &(obj->instance->first_base);
    loadedIndex->sentinelIndex = &(obj->instance->sentinel_index);
#define USE_BWA_MEM_GPU_SA2REF
#ifndef USE_BWA_MEM_GPU_SA2REF
    loadedIndex->suffixArrayMsByte = obj->instance->sa_ms_byte;
    loadedIndex->suffixArrayLsWord = obj->instance->sa_ls_word;
    loadedIndex->referenceLen = &(obj->instance->reference_seq_len);
    loadedIndex->packedBwt = obj->instance->packed_bwt;
#endif
}

uint64_t* FMI_wrapper_get_one_hot(FMI_wrapper *obj)
{
	//fprintf(stderr, "[FMI wrapper] retrieving one_hot\n");
	return obj->instance->one_hot_mask_array;
}

CP_OCC* FMI_wrapper_get_cp_occ(FMI_wrapper *obj, int64_t *cp_occ_size)
{
	//fprintf(stderr, "[FMI wrapper] retrieving cp_occ\n");
	*cp_occ_size = obj->instance->cp_occ_size;
	//fprintf(stderr, "[FMI wrapper] retrieved cp_occ_size\n");
    CP_OCC* cp_occ = obj->instance->cp_occ;
	//fprintf(stderr, "[FMI wrapper] retrieved cp_occ\n");

            //fprintf(stderr, "Just retrieved cp_occ[0] %ld %ld %ld %ld %lu %lu %lu %lu \n",\
                                cp_occ[0].cp_count[0],\
                                cp_occ[0].cp_count[1],\
                                cp_occ[0].cp_count[2],\
                                cp_occ[0].cp_count[3],\
                                cp_occ[0].one_hot_bwt_str[0],\
                                cp_occ[0].one_hot_bwt_str[1],\
                                cp_occ[0].one_hot_bwt_str[2],\
                                cp_occ[0].one_hot_bwt_str[3]);
	return cp_occ;
}

int64_t* FMI_wrapper_get_count(FMI_wrapper *obj)
{
	//fprintf(stderr, "[FMI wrapper] retrieving count\n");
	return obj->instance->count;
}

CP_OCC2* FMI_wrapper_get_cp_occ2(FMI_wrapper *obj)
{
	//fprintf(stderr, "[FMI wrapper] retrieving cp_occ2\n");
    CP_OCC2* cp_occ2 = obj->instance->cp_occ2;

    /*
            fprintf(stderr, "Just retrieved cp_occ[0] %ld %ld %ld %ld %lu %lu %lu %lu \n",\
                                cp_occ[0].cp_count[0],\
                                cp_occ[0].cp_count[1],\
                                cp_occ[0].cp_count[2],\
                                cp_occ[0].cp_count[3],\
                                cp_occ[0].one_hot_bwt_str[0],\
                                cp_occ[0].one_hot_bwt_str[1],\
                                cp_occ[0].one_hot_bwt_str[2],\
                                cp_occ[0].one_hot_bwt_str[3]);
                                */
	return cp_occ2;
}

int64_t* FMI_wrapper_get_count2(FMI_wrapper *obj)
{
	//fprintf(stderr, "[FMI wrapper] retrieving count2\n");
	return obj->instance->count2;
}

uint8_t FMI_wrapper_get_first_base(FMI_wrapper *obj)
{
	//fprintf(stderr, "[FMI wrapper] retrieving first_base\n");
	return obj->instance->first_base;
}

void FMI_wrapper_destroy(FMI_wrapper *obj)
{
	delete obj->instance;
	delete obj;
}

void FMI_wrapper_build_index(FMI_wrapper *obj)
{
	fprintf(stderr, "[FMI wrapper] Building index\n");
    obj->instance->build_index();
}
