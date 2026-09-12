module;

#include <stdlib.h>
#include <cstdint>
#include <string.h>

#ifdef __ARM_NEON__
#undef KSW2_BUILD_SSE_4_1
#include "sse_neon.h"
#endif

#ifdef __SSE2__
#undef KSW2_BUILD_SSE_4_1
#include <emmintrin.h>
#endif

/**
 * Include the real kalloc allocator so that the alignment module uses the
 * same memory pool as the calling C code (minimap2's align.c).  Without
 * this, the module allocates via stdlib realloc() while the caller frees
 * via kalloc's kfree(), causing a heap-buffer-overflow.
 *
 * minimap2stuff.h also defines KSW_* preprocessor constants that would
 * clash with the constexpr definitions in the module purview, so we
 * undef them immediately after inclusion.
 */
#include "minimap2/common.h"

#undef KSW_NEG_INF
#undef KSW_EZ_SCORE_ONLY
#undef KSW_EZ_RIGHT
#undef KSW_EZ_GENERIC_SC
#undef KSW_EZ_APPROX_MAX
#undef KSW_EZ_APPROX_DROP
#undef KSW_EZ_EXTZ_ONLY
#undef KSW_EZ_REV_CIGAR
#undef KSW_EZ_SPLICE_FOR
#undef KSW_EZ_SPLICE_REV
#undef KSW_EZ_SPLICE_FLANK
#undef KSW_EZ_SPLICE_CMPLX
#undef KSW_EZ_SPLICE_SCORE
#undef KSW_CIGAR_MATCH
#undef KSW_CIGAR_INS
#undef KSW_CIGAR_DEL
#undef KSW_CIGAR_N_SKIP
#undef KSW_SPSC_OFFSET

export module alignment;
//import std;

/**
 * @brief Macro for the common parameter list of single-affine-gap alignment (z2).
 *
 * Parameters:
 *   - km:        Memory pool handle (may be NULL).
 *   - qlen:      Length of the query sequence.
 *   - query:     Query sequence as alphabet-index array.
 *   - tlen:      Length of the target sequence.
 *   - target:    Target sequence as alphabet-index array.
 *   - m:         Alphabet size.
 *   - mat:       Flat m-by-m scoring matrix (row-major).
 *   - q:         Gap open penalty (positive means penalty).
 *   - e:         Gap extension penalty (positive means penalty).
 *   - w:         Band width for banded alignment (-1 for full).
 *   - zdrop:     Z-drop threshold (-1 to disable).
 *   - end_bonus: Bonus score added when alignment reaches the end of the query.
 *   - flag:      Bitwise OR of KSW_EZ_* flags controlling algorithm behavior.
 *   - ez:        [out] Pointer to the result structure.
 */
#define Z2_ARGS void *km, int qlen, const uint8_t *query, int tlen, const uint8_t *target, int8_t m, const int8_t *mat, int8_t q, int8_t e, int w, int zdrop, int end_bonus, int flag, ksw_extz_t *ez

/**
 * @brief Macro for the common parameter list of dual-affine-gap alignment (d2).
 *
 * Extends Z2_ARGS with a second set of gap penalties (q2, e2) for the
 * dual-affine gap model, which uses two gap-cost functions and picks the
 * minimum cost. The first pair (q, e) should satisfy q+e <= q2+e2.
 *
 * Additional parameters beyond Z2_ARGS:
 *   - q2: Second gap open penalty.
 *   - e2: Second gap extension penalty.
 */
#define D2_ARGS void *km, int qlen, const uint8_t *query, int tlen, const uint8_t *target, int8_t m, const int8_t *mat, int8_t q, int8_t e, int8_t q2, int8_t e2, int w, int zdrop, int end_bonus, int flag, ksw_extz_t *ez

/**
 * @brief Macro for the common parameter list of splice-aware alignment (s2).
 *
 * Extends the parameter list with splice-site-specific penalties and
 * bonuses for RNA-to-genome alignment:
 *   - q2:         Intron open penalty (must be > q + e).
 *   - noncan:     Non-canonical splice site penalty.
 *   - junc_bonus: Bonus for splice junctions annotated in junc[].
 *   - junc_pen:   Penalty for unknown junction splice scores.
 *   - junc:       Optional junction annotation array (length tlen).
 */
#define S2_ARGS void *km, int qlen, const uint8_t *query, int tlen, const uint8_t *target, int8_t m, const int8_t *mat, int8_t q, int8_t e, int8_t q2, int8_t noncan, int zdrop, int end_bonus, int8_t junc_bonus, int8_t junc_pen, int flag, const uint8_t *junc, ksw_extz_t *ez

/** @brief Sentinel value representing negative infinity for DP scores. */
constexpr int32_t KSW_NEG_INF = -0x40000000;

/** @name KSW_EZ_* alignment mode flags
 *
 * These flags control the behavior of the KSW2 alignment algorithms.
 * Combine them with bitwise OR in the @c flag parameter.
 * @{
 */

/** @brief Do not record alignment path/CIGAR; compute score only. */
constexpr int KSW_EZ_SCORE_ONLY  = 0x01;

/** @brief Right-align gaps (prefer gaps on the right when scores tie). */
constexpr int KSW_EZ_RIGHT       = 0x02;

/** @brief Use a generic scoring matrix. Without this flag, only
 *  match/mismatch scoring is used and the last alphabet symbol is
 *  treated as a wildcard. */
constexpr int KSW_EZ_GENERIC_SC  = 0x04;

/** @brief Use approximate max-score tracking (faster with SSE). */
constexpr int KSW_EZ_APPROX_MAX  = 0x08;

/** @brief Use approximate Z-drop evaluation (faster with SSE). */
constexpr int KSW_EZ_APPROX_DROP = 0x10;

/** @brief Perform extension alignment only (do not require reaching both ends). */
constexpr int KSW_EZ_EXTZ_ONLY   = 0x40;

/** @brief Output the CIGAR in reverse order (no post-reversal step). */
constexpr int KSW_EZ_REV_CIGAR   = 0x80;

/** @brief Enable forward-strand splice site detection. */
constexpr int KSW_EZ_SPLICE_FOR  = 0x100;

/** @brief Enable reverse-strand splice site detection. */
constexpr int KSW_EZ_SPLICE_REV  = 0x200;

/** @brief Apply flanking splice-site penalty adjustments. */
constexpr int KSW_EZ_SPLICE_FLANK = 0x400;

/** @brief Use the miniprot complex splice model. */
constexpr int KSW_EZ_SPLICE_CMPLX = 0x800;

/** @brief Use splice scores from the junction annotation array. */
constexpr int KSW_EZ_SPLICE_SCORE = 0x1000;

/** @} */

/** @name KSW_CIGAR_* CIGAR operation codes
 *
 * The subset of CIGAR operators used by the KSW2 alignment code.
 * Use MM_CIGAR_* from minimap.h if you need the full BAM CIGAR set.
 * @{
 */
constexpr int KSW_CIGAR_MATCH  = 0;  /**< Alignment match or mismatch (M). */
constexpr int KSW_CIGAR_INS    = 1;  /**< Insertion to the reference (I). */
constexpr int KSW_CIGAR_DEL    = 2;  /**< Deletion from the reference (D). */
constexpr int KSW_CIGAR_N_SKIP = 3;  /**< Skipped region / intron (N). */
/** @} */

/** @brief Offset applied to splice scores stored in the junction array. */
constexpr int KSW_SPSC_OFFSET  = 64;


#include "defs.hpp"

#ifdef __ARM_NEON__
#undef KSW2_BUILD_SSE_4_1
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

constexpr int SIMD_SSE     = 0x1;
constexpr int SIMD_SSE2    = 0x2;
constexpr int SIMD_SSE3    = 0x4;
constexpr int SIMD_SSSE3   = 0x8;
constexpr int SIMD_SSE4_1  = 0x10;
constexpr int SIMD_SSE4_2  = 0x20;
constexpr int SIMD_AVX     = 0x40;
constexpr int SIMD_AVX2    = 0x80;
constexpr int SIMD_AVX512F = 0x100;

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

/**
 * @brief Detect supported x86 SIMD instruction sets via CPUID.
 * @return Bitmask of SIMD_* flags indicating available instruction sets.
 */
static int x86_simd(void)
{
	int flag = 0, cpuid[4], max_id;
	__cpuidex(cpuid, 0, 0);
	max_id = cpuid[0];
	if (max_id == 0) {
		return 0;
	}
	__cpuidex(cpuid, 1, 0);
	if (cpuid[3] >> 25 & 1) { flag |= SIMD_SSE; }
	if (cpuid[3] >> 26 & 1) { flag |= SIMD_SSE2; }
	if (cpuid[2] >> 0  & 1) { flag |= SIMD_SSE3; }
	if (cpuid[2] >> 9  & 1) { flag |= SIMD_SSSE3; }
	if (cpuid[2] >> 19 & 1) { flag |= SIMD_SSE4_1; }
	if (cpuid[2] >> 20 & 1) { flag |= SIMD_SSE4_2; }
	if (cpuid[2] >> 28 & 1) { flag |= SIMD_AVX; }
	if (max_id >= 7) {
		__cpuidex(cpuid, 7, 0);
		if (cpuid[1] >> 5  & 1) { flag |= SIMD_AVX2; }
		if (cpuid[1] >> 16 & 1) { flag |= SIMD_AVX512F; }
	}
	return flag;
}
#endif // KSW_CPU_DISPATCH

/**
 * @brief Enumeration of supported alignment algorithm backends.
 *
 * Currently only KSW2 is supported. This enum is used as a template
 * parameter for the Aligner class to allow compile-time dispatch.
 */
export enum SupportedAlignmentAlgorithm { ALIGN_ALGORITHM_KSW2 };

/**
 * @brief Static alignment dispatcher templated on the algorithm backend.
 *
 * Provides three static methods corresponding to the three KSW2
 * alignment modes. At compile time, the appropriate SIMD implementation
 * (SSE2 or SSE4.1) is selected. When KSW_CPU_DISPATCH is defined,
 * runtime CPU detection picks the best available implementation.
 *
 * @tparam E  The alignment algorithm to use (currently only ALIGN_ALGORITHM_KSW2).
 */
export template <SupportedAlignmentAlgorithm E> class Aligner {
public:
	/**
	 * @brief Single-affine-gap extension alignment (KSW2 extz2).
	 *
	 * Performs banded DP extension alignment with a single affine gap
	 * penalty model: cost(k) = q + k*e for a gap of length k. Supports
	 * Z-drop early termination and optional CIGAR output.
	 *
	 * @see Z2_ARGS for parameter documentation.
	 */
	static void z2(Z2_ARGS);

	/**
	 * @brief Dual-affine-gap extension alignment (KSW2 extd2).
	 *
	 * Performs banded DP extension alignment with two affine gap penalty
	 * functions, choosing the lower cost. This models both short gaps
	 * (q + k*e) and long gaps (q2 + k*e2) efficiently.
	 *
	 * @see D2_ARGS for parameter documentation.
	 */
	static void d2(D2_ARGS);

	/**
	 * @brief Splice-aware extension alignment (KSW2 exts2).
	 *
	 * Performs banded DP extension alignment aware of RNA splice sites.
	 * Uses donor/acceptor site scoring from the target sequence and
	 * optional junction annotation to properly handle introns.
	 *
	 * @see S2_ARGS for parameter documentation.
	 */
	static void s2(S2_ARGS);
};

template <> inline void Aligner<ALIGN_ALGORITHM_KSW2>::z2(Z2_ARGS) {
	#ifdef KSW_CPU_DISPATCH
		if (ksw_simd < 0) {
			ksw_simd = x86_simd();
		}
		if (ksw_simd & SIMD_SSE4_1) {
			ksw2_sse41::align_z2(km, qlen, query, tlen, target, m, mat, q, e, w, zdrop, end_bonus, flag, ez);
		} else if (ksw_simd & SIMD_SSE2) {
			ksw2_sse2::align_z2(km, qlen, query, tlen, target, m, mat, q, e, w, zdrop, end_bonus, flag, ez);
		} else {
			abort();
		}
	#else
		KSW2_BEST_SUPPORTED_NAMESPACE::align_z2(km, qlen, query, tlen, target, m, mat, q, e, w, zdrop, end_bonus, flag, ez);
	#endif
}

template <> inline void Aligner<ALIGN_ALGORITHM_KSW2>::d2(D2_ARGS) {
	#ifdef KSW_CPU_DISPATCH
		if (ksw_simd < 0) {
			ksw_simd = x86_simd();
		}
		if (ksw_simd & SIMD_SSE4_1) {
			ksw2_sse41::align_d2(km, qlen, query, tlen, target, m, mat, q, e, q2, e2, w, zdrop, end_bonus, flag, ez);
		} else if (ksw_simd & SIMD_SSE2) {
			ksw2_sse2::align_d2(km, qlen, query, tlen, target, m, mat, q, e, q2, e2, w, zdrop, end_bonus, flag, ez);
		} else {
			abort();
		}
	#else
		KSW2_BEST_SUPPORTED_NAMESPACE::align_d2(km, qlen, query, tlen, target, m, mat, q, e, q2, e2, w, zdrop, end_bonus, flag, ez);
	#endif
}

template <> inline void Aligner<ALIGN_ALGORITHM_KSW2>::s2(S2_ARGS) {
	#ifdef KSW_CPU_DISPATCH
		if (ksw_simd < 0) {
			ksw_simd = x86_simd();
		}
		if (ksw_simd & SIMD_SSE4_1) {
			ksw2_sse41::align_s2(km, qlen, query, tlen, target, m, mat, q, e, q2, noncan, zdrop, end_bonus, junc_bonus, junc_pen, flag, junc, ez);
		} else if (ksw_simd & SIMD_SSE2) {
			ksw2_sse2::align_s2(km, qlen, query, tlen, target, m, mat, q, e, q2, noncan, zdrop, end_bonus, junc_bonus, junc_pen, flag, junc, ez);
		} else {
			abort();
		}
	#else
		KSW2_BEST_SUPPORTED_NAMESPACE::align_s2(km, qlen, query, tlen, target, m, mat, q, e, q2, noncan, zdrop, end_bonus, junc_bonus, junc_pen, flag, junc, ez);
	#endif
}
