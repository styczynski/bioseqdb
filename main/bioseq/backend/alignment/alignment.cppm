module;

#import <stdlib.h>
#import <cstdint>
#import <string.h>

export module alignment;
import std;

#define assert(X) 

#define kmalloc(km, size) malloc((size))
#define kcalloc(km, count, size) calloc((count), (size))
#define krealloc(km, ptr, size) realloc((ptr), (size))
#define kfree(km, ptr) free((ptr))

#define Z2_ARGS void *km, int qlen, const uint8_t *query, int tlen, const uint8_t *target, int8_t m, const int8_t *mat, int8_t q, int8_t e, int w, int zdrop, int end_bonus, int flag, ksw_extz_t *ez
#define D2_ARGS void *km, int qlen, const uint8_t *query, int tlen, const uint8_t *target, int8_t m, const int8_t *mat, int8_t q, int8_t e, int8_t q2, int8_t e2, int w, int zdrop, int end_bonus, int flag, ksw_extz_t *ez
#define S2_ARGS void *km, int qlen, const uint8_t *query, int tlen, const uint8_t *target, int8_t m, const int8_t *mat, int8_t q, int8_t e, int8_t q2, int8_t noncan, int zdrop, int end_bonus, int8_t junc_bonus, int8_t junc_pen, int flag, const uint8_t *junc, ksw_extz_t *ez

#define KSW_NEG_INF -0x40000000

#define KSW_EZ_SCORE_ONLY  0x01 // don't record alignment path/cigar
#define KSW_EZ_RIGHT       0x02 // right-align gaps
#define KSW_EZ_GENERIC_SC  0x04 // without this flag: match/mismatch only; last symbol is a wildcard
#define KSW_EZ_APPROX_MAX  0x08 // approximate max; this is faster with sse
#define KSW_EZ_APPROX_DROP 0x10 // approximate Z-drop; faster with sse
#define KSW_EZ_EXTZ_ONLY   0x40 // only perform extension
#define KSW_EZ_REV_CIGAR   0x80 // reverse CIGAR in the output
#define KSW_EZ_SPLICE_FOR  0x100
#define KSW_EZ_SPLICE_REV  0x200
#define KSW_EZ_SPLICE_FLANK 0x400
#define KSW_EZ_SPLICE_CMPLX 0x800  // use the miniprot splice model
#define KSW_EZ_SPLICE_SCORE 0x1000 // use splice score

// The subset of CIGAR operators used by ksw code.
// Use MM_CIGAR_* from minimap.h if you need the full list.
#define KSW_CIGAR_MATCH  0
#define KSW_CIGAR_INS    1
#define KSW_CIGAR_DEL    2
#define KSW_CIGAR_N_SKIP 3

#define KSW_SPSC_OFFSET  64

#ifdef __ARM_NEON__
#undef KSW2_BUILD_SSE_4_1
#include "sse_neon.h"
#endif
#include "defs.hpp"

#ifdef __ARM_NEON__
#undef KSW2_BUILD_SSE_4_1
#include "sse_neon.h"
namespace ksw2_sse_neon {
	#include "ksw2_extd2_sse.hpp"
	#include "ksw2_exts2_sse.hpp"
	#include "ksw2_extz2_sse.hpp"
}
#undef KSW2_BUILD_SSE_4_1
#undef KSW2_BEST_SUPPORTED_NAMESPACE
#define KSW2_BEST_SUPPORTED_NAMESPACE ksw2_sse_neon
#endif

#ifdef __SSE2__
#undef KSW2_BUILD_SSE_4_1
#include <emmintrin.h>
namespace ksw2_sse2 {
	#include "ksw2_extd2_sse.hpp"
	#include "ksw2_exts2_sse.hpp"
	#include "ksw2_extz2_sse.hpp"
}
#undef KSW2_BUILD_SSE_4_1
#undef KSW2_BEST_SUPPORTED_NAMESPACE
#define KSW2_BEST_SUPPORTED_NAMESPACE ksw2_sse2
#endif

#ifdef __SSE4_1__
#define KSW2_BUILD_SSE_4_1
namespace ksw2_sse41 {
	#include "ksw2_extd2_sse.hpp"
	#include "ksw2_exts2_sse.hpp"
	#include "ksw2_extz2_sse.hpp"
}
#undef KSW2_BUILD_SSE_4_1
#undef KSW2_BEST_SUPPORTED_NAMESPACE
#define KSW2_BEST_SUPPORTED_NAMESPACE ksw2_sse41
#endif

#ifdef KSW_CPU_DISPATCH

#define SIMD_SSE     0x1
#define SIMD_SSE2    0x2
#define SIMD_SSE3    0x4
#define SIMD_SSSE3   0x8
#define SIMD_SSE4_1  0x10
#define SIMD_SSE4_2  0x20
#define SIMD_AVX     0x40
#define SIMD_AVX2    0x80
#define SIMD_AVX512F 0x100

#ifndef _MSC_VER
// adapted from https://github.com/01org/linux-sgx/blob/master/common/inc/internal/linux/cpuid_gnu.h
void __cpuidex(int cpuid[4], int func_id, int subfunc_id)
{
#if defined(__x86_64__)
	__asm__ volatile ("cpuid"
			: "=a" (cpuid[0]), "=b" (cpuid[1]), "=c" (cpuid[2]), "=d" (cpuid[3])
			: "0" (func_id), "2" (subfunc_id));
#else // on 32bit, ebx can NOT be used as PIC code
	__asm__ volatile ("xchgl %%ebx, %1; cpuid; xchgl %%ebx, %1"
			: "=a" (cpuid[0]), "=r" (cpuid[1]), "=c" (cpuid[2]), "=d" (cpuid[3])
			: "0" (func_id), "2" (subfunc_id));
#endif
}
#endif

static int ksw_simd = -1;

static int x86_simd(void)
{
	int flag = 0, cpuid[4], max_id;
	__cpuidex(cpuid, 0, 0);
	max_id = cpuid[0];
	if (max_id == 0) return 0;
	__cpuidex(cpuid, 1, 0);
	if (cpuid[3]>>25&1) flag |= SIMD_SSE;
	if (cpuid[3]>>26&1) flag |= SIMD_SSE2;
	if (cpuid[2]>>0 &1) flag |= SIMD_SSE3;
	if (cpuid[2]>>9 &1) flag |= SIMD_SSSE3;
	if (cpuid[2]>>19&1) flag |= SIMD_SSE4_1;
	if (cpuid[2]>>20&1) flag |= SIMD_SSE4_2;
	if (cpuid[2]>>28&1) flag |= SIMD_AVX;
	if (max_id >= 7) {
		__cpuidex(cpuid, 7, 0);
		if (cpuid[1]>>5 &1) flag |= SIMD_AVX2;
		if (cpuid[1]>>16&1) flag |= SIMD_AVX512F;
	}
	return flag;
}
#endif // KSW_CPU_DISPATCH

export enum SupportedAlignmentAlgorithm { ALIGN_ALGORITHM_KSW2 };

export template <SupportedAlignmentAlgorithm E> class Aligner {
public:
	static void z2(Z2_ARGS);
	static void d2(D2_ARGS);
	static void s2(S2_ARGS);
};

template <> inline void Aligner<ALIGN_ALGORITHM_KSW2> :: z2(Z2_ARGS) {
	#ifdef KSW_CPU_DISPATCH
		if (ksw_simd < 0) ksw_simd = x86_simd();
		if (ksw_simd & SIMD_SSE4_1)
			ksw2_sse41::align_z2(km, qlen, query, tlen, target, m, mat, q, e, w, zdrop, end_bonus, flag, ez);
		else if (ksw_simd & SIMD_SSE2)
			ksw2_sse2::align_z2(km, qlen, query, tlen, target, m, mat, q, e, w, zdrop, end_bonus, flag, ez);
		else abort();
	#else
		KSW2_BEST_SUPPORTED_NAMESPACE::align_z2(km, qlen, query, tlen, target, m, mat, q, e, w, zdrop, end_bonus, flag, ez);
	#endif
}

template <> inline void Aligner<ALIGN_ALGORITHM_KSW2> :: d2(D2_ARGS) {
	#ifdef KSW_CPU_DISPATCH
		if (ksw_simd < 0) ksw_simd = x86_simd();
		if (ksw_simd & SIMD_SSE4_1)
			ksw2_sse41::align_d2(km, qlen, query, tlen, target, m, mat, q, e, q2, e2, w, zdrop, end_bonus, flag, ez);
		else if (ksw_simd & SIMD_SSE2)
			ksw2_sse2::align_d2(km, qlen, query, tlen, target, m, mat, q, e, q2, e2, w, zdrop, end_bonus, flag, ez);
		else abort();
	#else
		KSW2_BEST_SUPPORTED_NAMESPACE::align_d2(km, qlen, query, tlen, target, m, mat, q, e, q2, e2, w, zdrop, end_bonus, flag, ez);
	#endif
}


template <> inline void Aligner<ALIGN_ALGORITHM_KSW2> :: s2(S2_ARGS) {
	#ifdef KSW_CPU_DISPATCH
		if (ksw_simd < 0) ksw_simd = x86_simd();
		if (ksw_simd & SIMD_SSE4_1)
			ksw2_sse41::align_s2(km, qlen, query, tlen, target, m, mat, q, e, q2, noncan, zdrop, end_bonus, junc_bonus, junc_pen, flag, junc, ez);
		else if (ksw_simd & SIMD_SSE2)
			ksw2_sse2::align_s2(km, qlen, query, tlen, target, m, mat, q, e, q2, noncan, zdrop, end_bonus, junc_bonus, junc_pen, flag, junc, ez);
		else abort();
	#else
		KSW2_BEST_SUPPORTED_NAMESPACE::align_s2(km, qlen, query, tlen, target, m, mat, q, e, q2, noncan, zdrop, end_bonus, junc_bonus, junc_pen, flag, junc, ez);
	#endif
}

