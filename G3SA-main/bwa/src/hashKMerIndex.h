#ifndef HASHKMERINDEX_H
#define HASHKMERINDEX_H

#define KMER_K 12

#include "bwt.h"

#define pow4(x) (1<<(2*(x)))  // 4^x

typedef struct {
	bwtint_t x[3]; // same as first 3 elements on bwtintv_t
	// bwtintv_t.info not included here because it contains length of match, which is always KMER_K in this case
} kmers_bucket_t;


#ifdef __cplusplus
extern "C"{
#endif

	kmers_bucket_t *loadKMerIndex(const char* path);

#ifdef __cplusplus
} // end extern "C"
#endif

#endif
