/*******************************************************************
*   sorts.cpp
*   Sorting Networks
*
*	Author: Kareem Omar
*	kareem.omar@uah.edu
*	https://github.com/komrad36
*
*	Last updated Feb 15, 2017
*******************************************************************/
//
// SUMMARY: I present novel and state-of-the-art sorting of 4 int32_t
// and sorting of 6 int8_t, as examples, using SSE4, and some thoughts
// and notes on fast sorting of small fixed-size arrays.
//
// These are the fastest known approaches on modern CPUs.
// They are completely branchless and extremely fast.
// For example, 4 int32_t can be sorted in ~18 cycles on Skylake.
//
// These examples can be modified to sort signed or unsigned data as long
// as the total size of the data is <= 128 bits (16 bytes). Note that
// trying to use AVX2 to push that to 256 bits will NOT HELP
// because of the 3 cycle interlane latency that makes shuffling
// across the 128-bit lane boundary too expensive to be useful.
//
// In that case, or in the case that you can't support SSE4,
// efficient scalar code can also be generated that isn't too much
// slower, but it's not immediately evident what the optimal approach
// is in assembly, nor how to coerce C/C++ compilers to produce that
// assembly from C/C++ code. (I discuss the approach at the end of this
// documentation.)
//
// Not all compilers correctly generate optimal assembly for either the
// SSE or scalar code, so I provide assembly versions too. In fact, this
// is not a strong enough statement; many compilers FAIL MISERABLY
// at both the SSE and scalar cases for n >= 3. CL in particular
// (Microsoft Visual C++) just dies horribly in a fire, so the assembly
// snippets may not be a bad idea. Profile and/or check your disassembly.
//
// Note that these assembly snippets use the Windows x64 calling convention,
// but then proceed to clobber a bunch of registers they shouldn't. Normally
// they'd be inlined. Feel free to callee-save registers that your
// application cannot safely clobber.
//
// These code snippets work on the principle of sorting networks.
// Conventional sorting algorithms like quicksort and mergesort
// are not great at small array sizes. One notices in profiling that
// simpler sorts like insertion and selection tend to win. However,
// even these are not NEARLY as fast as they could be for
// fixed-size, small arrays.
//
// Available sorts and their function names:
//
// >>> SSE Assembly (fast as hell. FASTEST option on modern CPUs.
//					 these are written in MASM for Win64;
//					 but it's Intel syntax and you can make the small
//					 modifications required for other assemblers.)
// Sorting 4 int32_t  |  simdsort4a()
// Sorting 4 int32_t  |  simdsort4a_noconstants()
// Sorting 4 int32_t  |  simdsort4a_nofloat()
//
// >>> SSE C++ (make sure generated assembly matches):
// Sorting 4 int32_t  |  simdsort4()
// Sorting 6 int8_t   |  simdsort6()
//
// >>> Scalar Assembly (these are written in MASM for Win64;
//						but it's Intel syntax and you can make the small
//						modifications required for other assemblers.)
// Sorting 2 int32_t  |  sort2a()
// Sorting 3 int32_t  |  sort3a()
// Sorting 4 int32_t  |  sort4a()
// Sorting 5 int32_t  |  sort5a()
// Sorting 6 int32_t  |  sort6a()
//
// >>> Scalar C++ (make sure generated assembly matches)
// Sorting 2 int32_t  |  sort2()
// Sorting 6 int32_t  |  sort6()
//
//
// Okay, if you've made it this far, let's discuss
// fast conditional swap operations.Conditional swaps
// if the lower element is greater are the heart of sorting
// networks.Given two values,
// 'a', and 'b', leave them as they are if 'a' is less
// than 'b', i.e. if they are in sorted order.However,
// swap them if 'a' is greater than or equal to 'b'.
// Thus after such a conditional swap operation 'a' and 'b' are in sorted
// order no matter what order they came in as.
//
// A series of such operations can deterministically sort
// a fixed - size array. Typically one can optimize for depth
// (minimum number of operations given infinite parallel
// processing) or for size (minimum number of operations given
// no parallel processing). For n == 4 these two optimal solutions
// are actually given by the same network, with depth 3 and
// size 5.
//
// Scalar first : how do you efficiently conditional swap ? Again, note that
// lots of compilers don't produce optimal assembly no matter
// what C++ you give them.But what is the optimal assembly ?
// Well, on modern processors, the answer is conditional moves :
//
// ; inputs: eax, r9d
// ; scratch register: edx
//	cmp     eax, r9d
//	mov     edx, eax
//	cmovg   eax, r9d
//	cmovg   r9d, edx
// ; eax and r9d have been swapped if necessary such that eax is now <= r9d
//
// See the function 'sort6' in 'sorts.cpp' for an attempt at some C++ code
// that has a decent chance of compiling into conditional swaps that look like that.
// Again, they OFTEN DON'T, especially the CL compiler and g++. Use the assembly
// snippets instead, or at least profile and inspect your disassembly to be sure.
//
// The SSE sorts are rather more sophisticated, but the basic principle
// is to branchlessly generate shuffle index masks at runtime and then
// use them to permute the order of the data in parallel, performing
// one complete level of the sorting network at a time.
//
// I provide 3 versions of the assembly for sorting 4 int32_t. The fastest
// should be the plain 'simdsort4a'. It performs float reinterpretation
// and relies on some constant loads, but both of these are USUALLY
// better than the alternatives. However:
//
// Older CPUs may incur too much latency from the reinterpretation to be
// worth it. For such CPUs, try 'simdsort4a_nofloat.asm'.
//
// Applications that cannot afford to have these constants occupying L1
// may wish to try 'simdsort4a_noconstants.asm', which does not eat
// up any cache space with constants - everything is done with immediates
// and some fairly nasty bit twiddling.
//

#include "sorts.h"

void sort2(int* __restrict v) {
	const int a = v[0];
	const int b = v[1];
	v[0] = b > a ? a : b;
	v[1] = b > a ? b : a;
}

void sort6(int* __restrict v) {
#define __kmin(a, b) (a < b ? a : b)
#define __kmax(a, b) (a < b ? b : a)
#define __kswap(x,y) { int a = __kmin(v[x], v[y]); int b = __kmax(v[x], v[y]); v[x] = a; v[y] = b; }
	__kswap(1, 2);
	__kswap(0, 2);
	__kswap(0, 1);
	__kswap(4, 5);
	__kswap(3, 5);
	__kswap(3, 4);
	__kswap(0, 3);
	__kswap(1, 4);
	__kswap(2, 5);
	__kswap(2, 4);
	__kswap(1, 3);
	__kswap(2, 3);
#undef __kswap
#undef __kmax
#undef __kmin
}

// this is the 'no float' version
//const __m128i pass1_add4s = _mm_setr_epi8(4, 5, 6, 7, 4, 5, 6, 7, 12, 13, 14, 15, 12, 13, 14, 15);
//const __m128i pass2_add4s = _mm_setr_epi8(8, 9, 10, 11, 12, 13, 14, 15, 8, 9, 10, 11, 12, 13, 14, 15);
//const __m128i pass3_add4s = _mm_setr_epi8(0, 1, 2, 3, 8, 9, 10, 11, 8, 9, 10, 11, 12, 13, 14, 15);
//void simdsort4(int* __restrict v) {
//	__m128i b, a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(v));
//
//	b = _mm_shuffle_epi32(a, 177);
//	b = _mm_cmpgt_epi32(b, a);
//	b = _mm_and_si128(b, _mm_set1_epi8(-4));
//	b = _mm_add_epi8(b, pass1_add4s);
//	a = _mm_shuffle_epi8(a, b);
//
//	b = _mm_shuffle_epi32(a, 78);
//	b = _mm_cmpgt_epi32(b, a);
//	b = _mm_and_si128(b, _mm_set1_epi8(-8));
//	b = _mm_add_epi8(b, pass2_add4s);
//	a = _mm_shuffle_epi8(a, b);
//
//	b = _mm_shuffle_epi32(a, 216);
//	b = _mm_cmpgt_epi32(b, a);
//	b = _mm_and_si128(b, _mm_set1_epi8(-4));
//	b = _mm_add_epi8(b, pass3_add4s);
//	a = _mm_shuffle_epi8(a, b);
//
//	_mm_storeu_si128(reinterpret_cast<__m128i*>(v), a);
//}

// this is the version that should be the fastest
const __m128i pass1_add4 = _mm_setr_epi32(1, 1, 3, 3);
const __m128i pass2_add4 = _mm_setr_epi32(2, 3, 2, 3);
const __m128i pass3_add4 = _mm_setr_epi32(0, 2, 2, 3);
void simdsort4(int* __restrict v) {
	__m128i b, a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(v));

	b = _mm_shuffle_epi32(a, 177);
	b = _mm_cmpgt_epi32(b, a);
	b = _mm_add_epi32(b, pass1_add4);
	a = _mm_castps_si128(_mm_permutevar_ps(_mm_castsi128_ps(a), b));

	b = _mm_shuffle_epi32(a, 78);
	b = _mm_cmpgt_epi32(b, a);
	b = _mm_add_epi32(b, b);
	b = _mm_add_epi32(b, pass2_add4);
	a = _mm_castps_si128(_mm_permutevar_ps(_mm_castsi128_ps(a), b));

	b = _mm_shuffle_epi32(a, 216);
	b = _mm_cmpgt_epi32(b, a);
	b = _mm_add_epi32(b, pass3_add4);
	__m128 ret = _mm_permutevar_ps(_mm_castsi128_ps(a), b);

	_mm_storeu_ps(reinterpret_cast<float*>(v), ret);
}

const __m128i pass1_shf = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
const __m128i pass1_add = _mm_setr_epi8(1, 1, 3, 3, 5, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
const __m128i pass2_shf = _mm_setr_epi8(2, 4, 0, 5, 1, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
const __m128i pass2_and = _mm_setr_epi8(-2, -3, -2, -2, -3, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
const __m128i pass2_add = _mm_setr_epi8(2, 4, 2, 5, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
const __m128i pass4_shf = _mm_setr_epi8(0, 2, 1, 4, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
const __m128i pass4_add = _mm_setr_epi8(0, 2, 2, 4, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
const __m128i pass5_shf = _mm_setr_epi8(0, 1, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
const __m128i pass5_add = _mm_setr_epi8(0, 1, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
void simdsort6(char* __restrict v) {
	__m128i b, a = _mm_cvtsi32_si128(*reinterpret_cast<const int*>(v));
	a = _mm_insert_epi16(a, *reinterpret_cast<const int*>(v + 4), 2);

	b = _mm_shuffle_epi8(a, pass1_shf);
	b = _mm_cmpgt_epi8(b, a);
	b = _mm_add_epi8(b, pass1_add);
	a = _mm_shuffle_epi8(a, b);

	b = _mm_shuffle_epi8(a, pass2_shf);
	b = _mm_cmpgt_epi8(b, a);
	b = _mm_and_si128(b, pass2_and);
	b = _mm_add_epi8(b, pass2_add);
	a = _mm_shuffle_epi8(a, b);

	b = _mm_shuffle_epi8(a, pass1_shf);
	b = _mm_cmpgt_epi8(b, a);
	b = _mm_add_epi8(b, pass1_add);
	a = _mm_shuffle_epi8(a, b);

	b = _mm_shuffle_epi8(a, pass4_shf);
	b = _mm_cmpgt_epi8(b, a);
	b = _mm_add_epi8(b, pass4_add);
	a = _mm_shuffle_epi8(a, b);

	b = _mm_shuffle_epi8(a, pass5_shf);
	b = _mm_cmpgt_epi8(b, a);
	b = _mm_add_epi8(b, pass5_add);
	a = _mm_shuffle_epi8(a, b);

	*reinterpret_cast<int*>(v) = _mm_cvtsi128_si32(a);
	*reinterpret_cast<int16_t*>(v + 4) = _mm_extract_epi16(a, 2);
}


// works but very slow compared to scalar version because
// of repeated inter-lane (3 cycle latency)

//const __m256i pass1_shfi = _mm256_setr_epi32(1, 0, 3, 2, 5, 4, 6, 7);
//const __m256i pass1_addi = _mm256_setr_epi32(1, 1, 3, 3, 5, 5, 6, 7);
//const __m256i pass2_shfi = _mm256_setr_epi32(2, 4, 0, 5, 1, 3, 6, 7);
//const __m256i pass2_andi = _mm256_setr_epi32(-2, -3, -2, -2, -3, -2, 0, 0);
//const __m256i pass2_addi = _mm256_setr_epi32(2, 4, 2, 5, 4, 5, 6, 7);
//const __m256i pass4_shfi = _mm256_setr_epi32(0, 2, 1, 4, 3, 5, 6, 7);
//const __m256i pass4_addi = _mm256_setr_epi32(0, 2, 2, 4, 4, 5, 6, 7);
//const __m256i pass5_shfi = _mm256_setr_epi32(0, 1, 3, 2, 4, 5, 6, 7);
//const __m256i pass5_addi = _mm256_setr_epi32(0, 1, 3, 3, 4, 5, 6, 7);
//void simdsort6(int* __restrict v) {
//	__m256i b, a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v));
//	
//	b = _mm256_permutevar8x32_epi32(a, pass1_shfi);
//	b = _mm256_cmpgt_epi32(b, a);
//	b = _mm256_add_epi32(b, pass1_addi);
//	a = _mm256_permutevar8x32_epi32(a, b);
//
//	b = _mm256_permutevar8x32_epi32(a, pass2_shfi);
//	b = _mm256_cmpgt_epi32(b, a);
//	b = _mm256_and_si256(b, pass2_andi);
//	b = _mm256_add_epi32(b, pass2_addi);
//	a = _mm256_permutevar8x32_epi32(a, b);
//
//	b = _mm256_permutevar8x32_epi32(a, pass1_shfi);
//	b = _mm256_cmpgt_epi32(b, a);
//	b = _mm256_add_epi32(b, pass1_addi);
//	a = _mm256_permutevar8x32_epi32(a, b);
//
//	b = _mm256_permutevar8x32_epi32(a, pass4_shfi);
//	b = _mm256_cmpgt_epi32(b, a);
//	b = _mm256_add_epi32(b, pass4_addi);
//	a = _mm256_permutevar8x32_epi32(a, b);
//
//	b = _mm256_permutevar8x32_epi32(a, pass5_shfi);
//	b = _mm256_cmpgt_epi32(b, a);
//	b = _mm256_add_epi32(b, pass5_addi);
//	a = _mm256_permutevar8x32_epi32(a, b);
//
//	_mm_storeu_si128((__m128i*)v, _mm256_castsi256_si128(a));
//	*reinterpret_cast<long long*>(v + 4) = _mm_cvtsi128_si64(_mm256_extracti128_si256(a, 1));
//}