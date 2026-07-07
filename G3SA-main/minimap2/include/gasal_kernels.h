#ifndef __GASAL_KERNELS_H__
#define __GASAL_KERNELS_H__


// Template-meta-programming types construction from Int values
// This allows to cut down kernel code at compilation time.

template <int Val>
struct Int2Type
{
	typedef enum {val_ = Val} val__;
};

template<typename X, typename Y>
struct SameType
{
   enum { result = 0 };
};

template<typename T>
struct SameType<T, T>
{
   enum { result = 1 };
};

#define SAMETYPE(a, b) (SameType<a,b>::result)


// __constant__ int32_t _cudaGapO; /*gap open penalty*/
// __constant__ int32_t _cudaGapOE; /*sum of gap open and extension penalties*/
// __constant__ int32_t _cudaGapExtend; /*sum of gap extend*/
// __constant__ int32_t _cudaMatchScore; /*score for a match*/
// __constant__ int32_t _cudaMismatchScore; /*penalty for a mismatch*/
// __constant__ int32_t _cudaSliceWidth; /*(AGAThA) slice width*/
// __constant__ int32_t _cudaZThreshold; /*(AGAThA) zdrop threshold*/
// __constant__ int32_t _cudaBandWidth; /*(AGAThA) band width*/
__constant__ int32_t _cudaGapO = 4; /*gap open penalty*/
__constant__ int32_t _cudaGapOE = 6; /*sum of gap open and extension penalties*/
__constant__ int32_t _cudaGapExtend = 2; /*sum of gap extend*/
__constant__ int32_t _cudaMatchScore = 2; /*score for a match*/
__constant__ int32_t _cudaMismatchScore = 4; /*penalty for a mismatch*/
__constant__ int32_t _cudaSliceWidth = 3; /*(AGAThA) slice width*/
__constant__ int32_t _cudaZThreshold = 400; /*(AGAThA) zdrop threshold*/
__constant__ int32_t _cudaBandWidth = 30001; /*(AGAThA) band width*/

#define MINUS_INF SHRT_MIN
#define MINUS_INF2 SHRT_MIN/2

//#define N_VALUE (N_CODE & 0xF)

#define N_VALUE 4
#define N_PENALTY 1

#ifdef N_PENALTY
	#define DEV_GET_SUB_SCORE_LOCAL(score, rbase, gbase) \
		score = (rbase == gbase) ?_cudaMatchScore : -_cudaMismatchScore;\
	score = ((rbase == N_VALUE) || (gbase == N_VALUE)) ? -N_PENALTY : score;\

	#define DEV_GET_SUB_SCORE_GLOBAL(score, rbase, gbase) \
		score = (rbase == gbase) ?_cudaMatchScore : -_cudaMismatchScore;\
	score = ((rbase == N_VALUE) || (gbase == N_VALUE)) ? -N_PENALTY : score;\

#else
	#define DEV_GET_SUB_SCORE_LOCAL(score, rbase, gbase) \
		score = (rbase == gbase) ?_cudaMatchScore : -_cudaMismatchScore;\
	score = ((rbase == N_VALUE) || (gbase == N_VALUE)) ? 0 : score;\

	#define DEV_GET_SUB_SCORE_GLOBAL(score, rbase, gbase) \
		score = (rbase == gbase) ?_cudaMatchScore : -_cudaMismatchScore;\

#endif

#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))


#define FIND_MAX(curr, gidx) \
	maxXY_y = (maxHH < curr) ? gidx : maxXY_y;\
maxHH = (maxHH < curr) ? curr : maxHH;


// Kernel files

#include "pack_rc_seqs.h"

#include "agatha_kernel.h"

#endif
