#ifndef AVX2SORT_H
#define AVX2SORT_H

#include <immintrin.h>
#include <cstdint>
#include <algorithm>

/* this header contains two vectorized functions for the data type int:
 * 1. avx2::quickselect(int *arr, int n, int k)
 * 2. avx2::quicksort(int *arr, int n)
 * */

namespace avx2{
namespace _internal {

#define LOAD_VECTOR(arr) _mm256_loadu_si256(reinterpret_cast<__m256i *>(arr))
#define STORE_VECTOR(arr, vec)                                                   \
  _mm256_storeu_si256(reinterpret_cast<__m256i *>(arr), vec)


/* vectorized sorting networks
************************************/

#define COEX(a, b){                                                   \
    auto vec_tmp = a;                                                          \
    a = _mm256_min_epi32(a, b);                                                \
    b = _mm256_max_epi32(vec_tmp, b);}

/* shuffle 2 vectors, instruction for int is missing,
 * therefore shuffle with float */
#define SHUFFLE_2_VECS(a, b, mask)                                       \
    _mm256_castps_si256 (_mm256_shuffle_ps(                         \
        _mm256_castsi256_ps (a), _mm256_castsi256_ps (b), mask));

/* optimized sorting network for two vectors, that is 16 ints */
inline void sort_16(__m256i &v1, __m256i &v2) {
  COEX(v1, v2);                                  /* step 1 */

  v2 = _mm256_shuffle_epi32(v2, _MM_SHUFFLE(2, 3, 0, 1)); /* step 2 */
  COEX(v1, v2);

  auto tmp = v1;                                          /* step  3 */
  v1 = SHUFFLE_2_VECS(v1, v2, 0b10001000);
  v2 = SHUFFLE_2_VECS(tmp, v2, 0b11011101);
  COEX(v1, v2);

  v2 = _mm256_shuffle_epi32(v2, _MM_SHUFFLE(0, 1, 2, 3)); /* step  4 */
  COEX(v1, v2);

  tmp = v1;                                               /* step  5 */
  v1 = SHUFFLE_2_VECS(v1, v2, 0b01000100);
  v2 = SHUFFLE_2_VECS(tmp, v2, 0b11101110);
  COEX(v1, v2);

  tmp = v1;                                               /* step  6 */
  v1 = SHUFFLE_2_VECS(v1, v2, 0b11011000);
  v2 = SHUFFLE_2_VECS(tmp, v2, 0b10001101);
  COEX(v1, v2);

  v2 = _mm256_permutevar8x32_epi32(v2, _mm256_setr_epi32(7,6,5,4,3,2,1,0));
  COEX(v1, v2);                                           /* step  7 */

  tmp = v1;                                               /* step  8 */
  v1 = SHUFFLE_2_VECS(v1, v2, 0b11011000);
  v2 = SHUFFLE_2_VECS(tmp, v2, 0b10001101);
  COEX(v1, v2);

  tmp = v1;                                               /* step  9 */
  v1 = SHUFFLE_2_VECS(v1, v2, 0b11011000);
  v2 = SHUFFLE_2_VECS(tmp, v2, 0b10001101);
  COEX(v1, v2);

  /* permute to make it easier to restore order */
  v1 = _mm256_permutevar8x32_epi32(v1, _mm256_setr_epi32(0,4,1,5,6,2,7,3));
  v2 = _mm256_permutevar8x32_epi32(v2, _mm256_setr_epi32(0,4,1,5,6,2,7,3));

  tmp = v1;                                              /* step  10 */
  v1 = SHUFFLE_2_VECS(v1, v2, 0b10001000);
  v2 = SHUFFLE_2_VECS(tmp, v2, 0b11011101);
  COEX(v1, v2);

  /* restore order */
  auto b2 = _mm256_shuffle_epi32(v2,0b10110001);
  auto b1 = _mm256_shuffle_epi32(v1,0b10110001);
  v1 = _mm256_blend_epi32(v1, b2, 0b10101010);
  v2 = _mm256_blend_epi32(b1, v2, 0b10101010);
}

#define ASC(a, b, c, d, e, f, g, h)                                    \
  (((h < 7) << 7) | ((g < 6) << 6) | ((f < 5) << 5) | ((e < 4) << 4) | \
      ((d < 3) << 3) | ((c < 2) << 2) | ((b < 1) << 1) | (a < 0))

#define COEX_PERMUTE(vec, a, b, c, d, e, f, g, h, MASK){               \
    __m256i permute_mask = _mm256_setr_epi32(a, b, c, d, e, f, g, h);  \
    __m256i permuted = _mm256_permutevar8x32_epi32(vec, permute_mask); \
    __m256i min = _mm256_min_epi32(permuted, vec);                     \
    __m256i max = _mm256_max_epi32(permuted, vec);                     \
    constexpr int blend_mask = MASK(a, b, c, d, e, f, g, h);           \
    vec = _mm256_blend_epi32(min, max, blend_mask);}

#define COEX_SHUFFLE(vec, a, b, c, d, e, f, g, h, MASK){               \
    constexpr int shuffle_mask = _MM_SHUFFLE(d, c, b, a);              \
    __m256i shuffled = _mm256_shuffle_epi32(vec, shuffle_mask);        \
    __m256i min = _mm256_min_epi32(shuffled, vec);                     \
    __m256i max = _mm256_max_epi32(shuffled, vec);                     \
    constexpr int blend_mask = MASK(a, b, c, d, e, f, g, h);           \
    vec = _mm256_blend_epi32(min, max, blend_mask);}

#define REVERSE_VEC(vec){                                              \
    vec = _mm256_permutevar8x32_epi32(                                 \
        vec, _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0));}

/* sorting network for 8 int with compare-exchange macros
 * (used for pivot selection in median of the medians) */
#define SORT_8(vec){                                                   \
  COEX_SHUFFLE(vec, 1, 0, 3, 2, 5, 4, 7, 6, ASC);                           \
  COEX_SHUFFLE(vec, 2, 3, 0, 1, 6, 7, 4, 5, ASC);                           \
  COEX_SHUFFLE(vec, 0, 2, 1, 3, 4, 6, 5, 7, ASC);                           \
  COEX_PERMUTE(vec, 7, 6, 5, 4, 3, 2, 1, 0, ASC);                           \
  COEX_SHUFFLE(vec, 2, 3, 0, 1, 6, 7, 4, 5, ASC);                           \
  COEX_SHUFFLE(vec, 1, 0, 3, 2, 5, 4, 7, 6, ASC);}

/* merge N vectors with bitonic merge, N % 2 == 0 and N > 0
 * s = 2 means that two vectors are already sorted */
inline void bitonic_merge_16(__m256i *vecs, const int N, const int s = 2) {
  for (int t = s * 2; t < 2 * N; t *= 2) {
    for (int l = 0; l < N; l += t) {
      for (int j = std::max(l + t - N, 0); j < t/2 ; j += 2) {
        REVERSE_VEC(vecs[l + t - 1 - j]);
        REVERSE_VEC(vecs[l + t - 2 - j]);
        COEX(vecs[l + j], vecs[l + t - 1 - j]);
        COEX(vecs[l + j + 1], vecs[l + t - 2 - j]); }}
    for (int m = t / 2; m > 4; m /= 2) {
      for (int k = 0; k < N - m / 2; k += m) {
        const int bound = std::min((k + m / 2), N - (m / 2));
        for (int j = k; j < bound; j += 2) {
          COEX(vecs[j], vecs[m / 2 + j]);
          COEX(vecs[j + 1], vecs[m / 2 + j + 1]); }}}
    for (int j = 0; j < N-2; j += 4) {
      COEX(vecs[j], vecs[j + 2]);
      COEX(vecs[j + 1], vecs[j + 3]);
    }
    for (int j = 0; j < N; j += 2) {
      COEX(vecs[j], vecs[j + 1]); }
    for (int i = 0; i < N; i += 2) {
      COEX_PERMUTE(vecs[i], 4, 5, 6, 7, 0, 1, 2, 3, ASC);
      COEX_PERMUTE(vecs[i + 1], 4, 5, 6, 7, 0, 1, 2, 3, ASC);
      auto tmp = vecs[i];
      vecs[i] = _mm256_unpacklo_epi32(vecs[i], vecs[i + 1]);
      vecs[i + 1] = _mm256_unpackhi_epi32(tmp, vecs[i + 1]);
      COEX(vecs[i], vecs[i + 1]);
      tmp = vecs[i];
      vecs[i] = _mm256_unpacklo_epi32(vecs[i], vecs[i + 1]);
      vecs[i + 1] = _mm256_unpackhi_epi32(tmp, vecs[i + 1]);
      COEX(vecs[i], vecs[i + 1]);
      tmp = vecs[i];
      vecs[i] = _mm256_unpacklo_epi32(vecs[i], vecs[i + 1]);
      vecs[i + 1] = _mm256_unpackhi_epi32(tmp, vecs[i + 1]); }}}

inline void bitonic_merge_128(__m256i *vecs, const int N, const int s = 16) {
  const int remainder16 = N - N % 16;
  const int remainder8 = N - N % 8;
  for (int t = s * 2; t < 2 * N; t *= 2) {
    for (int l = 0; l < N; l += t) {
      for (int j = std::max(l + t - N, 0); j < t/2 ; j += 2) {
        REVERSE_VEC(vecs[l + t - 1 - j]);
        REVERSE_VEC(vecs[l + t - 2 - j]);
        COEX(vecs[l + j], vecs[l + t - 1 - j]);
        COEX(vecs[l + j + 1], vecs[l + t - 2 - j]); }}
    for (int m = t / 2; m > 16; m /= 2) {
      for (int k = 0; k < N - m / 2; k += m) {
        const int bound = std::min((k + m / 2), N - (m / 2));
        for (int j = k; j < bound; j += 2) {
          COEX(vecs[j], vecs[m / 2 + j]);
          COEX(vecs[j + 1], vecs[m / 2 + j + 1]); }}}
    for (int j = 0; j < remainder16; j += 16) {
      COEX(vecs[j], vecs[j + 8]);
      COEX(vecs[j + 1], vecs[j + 9]);
      COEX(vecs[j + 2], vecs[j + 10]);
      COEX(vecs[j + 3], vecs[j + 11]);
      COEX(vecs[j + 4], vecs[j + 12]);
      COEX(vecs[j + 5], vecs[j + 13]);
      COEX(vecs[j + 6], vecs[j + 14]);
      COEX(vecs[j + 7], vecs[j + 15]);
    }
    for (int j = remainder16 + 8; j < N; j += 1) {
      COEX(vecs[j - 8], vecs[j]);
    }
    for (int j = 0; j < remainder8; j += 8) {
      COEX(vecs[j], vecs[j + 4]);
      COEX(vecs[j + 1], vecs[j + 5]);
      COEX(vecs[j + 2], vecs[j + 6]);
      COEX(vecs[j + 3], vecs[j + 7]);
    }
    for (int j = remainder8 + 4; j < N; j += 1) {
      COEX(vecs[j - 4], vecs[j]);
    }
    for (int j = 0; j < N-2; j += 4) {
      COEX(vecs[j], vecs[j + 2]);
      COEX(vecs[j + 1], vecs[j + 3]);
    }
    for (int j = 0; j < N; j += 2) {
      COEX(vecs[j], vecs[j + 1]); }
    for (int i = 0; i < N; i += 2) {
      COEX_PERMUTE(vecs[i], 4, 5, 6, 7, 0, 1, 2, 3, ASC);
      COEX_PERMUTE(vecs[i + 1], 4, 5, 6, 7, 0, 1, 2, 3, ASC);
      auto tmp = vecs[i];
      vecs[i] = _mm256_unpacklo_epi32(vecs[i], vecs[i + 1]);
      vecs[i + 1] = _mm256_unpackhi_epi32(tmp, vecs[i + 1]);
      COEX(vecs[i], vecs[i + 1]);
      tmp = vecs[i];
      vecs[i] = _mm256_unpacklo_epi32(vecs[i], vecs[i + 1]);
      vecs[i + 1] = _mm256_unpackhi_epi32(tmp, vecs[i + 1]);
      COEX(vecs[i], vecs[i + 1]);
      tmp = vecs[i];
      vecs[i] = _mm256_unpacklo_epi32(vecs[i], vecs[i + 1]);
      vecs[i + 1] = _mm256_unpackhi_epi32(tmp, vecs[i + 1]); }}}

/* sort 8 columns each containing 16 int, with 60 modules */
inline void sort_16_int_vertical(__m256i* vecs){
  COEX(vecs[0], vecs[1]); COEX(vecs[2], vecs[3]);  /* step 1 */
  COEX(vecs[4], vecs[5]); COEX(vecs[6], vecs[7]);
  COEX(vecs[8], vecs[9]); COEX(vecs[10], vecs[11])
  COEX(vecs[12], vecs[13]); COEX(vecs[14], vecs[15])
  COEX(vecs[0], vecs[2]); COEX(vecs[1], vecs[3]);  /* step 2 */
  COEX(vecs[4], vecs[6]); COEX(vecs[5], vecs[7]);
  COEX(vecs[8], vecs[10]); COEX(vecs[9], vecs[11]);
  COEX(vecs[12], vecs[14]); COEX(vecs[13], vecs[15]);
  COEX(vecs[0], vecs[4]); COEX(vecs[1], vecs[5]);  /* step 3 */
  COEX(vecs[2], vecs[6]); COEX(vecs[3], vecs[7]);
  COEX(vecs[8], vecs[12]); COEX(vecs[9], vecs[13]);
  COEX(vecs[10], vecs[14]); COEX(vecs[11], vecs[15]);
  COEX(vecs[0], vecs[8]); COEX(vecs[1], vecs[9])   /* step 4 */
  COEX(vecs[2], vecs[10]); COEX(vecs[3], vecs[11])
  COEX(vecs[4], vecs[12]); COEX(vecs[5], vecs[13])
  COEX(vecs[6], vecs[14]); COEX(vecs[7], vecs[15])
  COEX(vecs[5], vecs[10]); COEX(vecs[6], vecs[9]); /* step 5 */
  COEX(vecs[3], vecs[12]); COEX(vecs[7], vecs[11]);
  COEX(vecs[13], vecs[14]); COEX(vecs[4], vecs[8]);
  COEX(vecs[1], vecs[2]);
  COEX(vecs[1], vecs[4]); COEX(vecs[7], vecs[13]); /* step 6 */
  COEX(vecs[2], vecs[8]); COEX(vecs[11], vecs[14]);
  COEX(vecs[2], vecs[4]); COEX(vecs[5], vecs[6]);  /* step 7 */
  COEX(vecs[9], vecs[10]); COEX(vecs[11], vecs[13]);
  COEX(vecs[3], vecs[8]); COEX(vecs[7], vecs[12]);
  COEX(vecs[3], vecs[5]); COEX(vecs[6], vecs[8]);  /* step 8 */
  COEX(vecs[7], vecs[9]); COEX(vecs[10], vecs[12]);
  COEX(vecs[3], vecs[4]); COEX(vecs[5], vecs[6]);  /* step 9 */
  COEX(vecs[7], vecs[8]); COEX(vecs[9], vecs[10]);
  COEX(vecs[11], vecs[12]);
  COEX(vecs[6], vecs[7]); COEX(vecs[8], vecs[9]); /* step 10 */}

/* auto generated code for merging 8 columns, each column contains 16 elements,
 * without transposition */
void inline merge_8_columns_with_16_elements(__m256i* vecs){
  vecs[8] = _mm256_shuffle_epi32(vecs[8], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[7], vecs[8]);
  vecs[9] = _mm256_shuffle_epi32(vecs[9], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[6], vecs[9]);
  vecs[10] = _mm256_shuffle_epi32(vecs[10], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[5], vecs[10]);
  vecs[11] = _mm256_shuffle_epi32(vecs[11], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[4], vecs[11]);
  vecs[12] = _mm256_shuffle_epi32(vecs[12], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[3], vecs[12]);
  vecs[13] = _mm256_shuffle_epi32(vecs[13], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[2], vecs[13]);
  vecs[14] = _mm256_shuffle_epi32(vecs[14], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[1], vecs[14]);
  vecs[15] = _mm256_shuffle_epi32(vecs[15], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[0], vecs[15]);
  vecs[4] = _mm256_shuffle_epi32(vecs[4], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[3], vecs[4]);
  vecs[5] = _mm256_shuffle_epi32(vecs[5], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[2], vecs[5]);
  vecs[6] = _mm256_shuffle_epi32(vecs[6], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[1], vecs[6]);
  vecs[7] = _mm256_shuffle_epi32(vecs[7], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[0], vecs[7]);
  vecs[12] = _mm256_shuffle_epi32(vecs[12], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[11], vecs[12]);
  vecs[13] = _mm256_shuffle_epi32(vecs[13], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[10], vecs[13]);
  vecs[14] = _mm256_shuffle_epi32(vecs[14], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[9], vecs[14]);
  vecs[15] = _mm256_shuffle_epi32(vecs[15], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[8], vecs[15]);
  vecs[2] = _mm256_shuffle_epi32(vecs[2], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[1], vecs[2]);
  vecs[3] = _mm256_shuffle_epi32(vecs[3], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[0], vecs[3]);
  vecs[6] = _mm256_shuffle_epi32(vecs[6], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[5], vecs[6]);
  vecs[7] = _mm256_shuffle_epi32(vecs[7], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[4], vecs[7]);
  vecs[10] = _mm256_shuffle_epi32(vecs[10], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[9], vecs[10]);
  vecs[11] = _mm256_shuffle_epi32(vecs[11], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[8], vecs[11]);
  vecs[14] = _mm256_shuffle_epi32(vecs[14], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[13], vecs[14]);
  vecs[15] = _mm256_shuffle_epi32(vecs[15], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[12], vecs[15]);
  vecs[1] = _mm256_shuffle_epi32(vecs[1], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[0], vecs[1]);
  vecs[3] = _mm256_shuffle_epi32(vecs[3], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[2], vecs[3]);
  vecs[5] = _mm256_shuffle_epi32(vecs[5], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[4], vecs[5]);
  vecs[7] = _mm256_shuffle_epi32(vecs[7], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[6], vecs[7]);
  vecs[9] = _mm256_shuffle_epi32(vecs[9], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[8], vecs[9]);
  vecs[11] = _mm256_shuffle_epi32(vecs[11], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[10], vecs[11]);
  vecs[13] = _mm256_shuffle_epi32(vecs[13], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[12], vecs[13]);
  vecs[15] = _mm256_shuffle_epi32(vecs[15], _MM_SHUFFLE(2,3,0,1)); COEX(vecs[14], vecs[15]);
  COEX_SHUFFLE(vecs[0], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_SHUFFLE(vecs[1], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[2], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_SHUFFLE(vecs[3], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[4], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_SHUFFLE(vecs[5], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[6], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_SHUFFLE(vecs[7], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[8], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_SHUFFLE(vecs[9], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[10], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_SHUFFLE(vecs[11], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[12], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_SHUFFLE(vecs[13], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[14], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_SHUFFLE(vecs[15], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  vecs[8] = _mm256_shuffle_epi32(vecs[8], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[7], vecs[8]);
  vecs[9] = _mm256_shuffle_epi32(vecs[9], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[6], vecs[9]);
  vecs[10] = _mm256_shuffle_epi32(vecs[10], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[5], vecs[10]);
  vecs[11] = _mm256_shuffle_epi32(vecs[11], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[4], vecs[11]);
  vecs[12] = _mm256_shuffle_epi32(vecs[12], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[3], vecs[12]);
  vecs[13] = _mm256_shuffle_epi32(vecs[13], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[2], vecs[13]);
  vecs[14] = _mm256_shuffle_epi32(vecs[14], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[1], vecs[14]);
  vecs[15] = _mm256_shuffle_epi32(vecs[15], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[0], vecs[15]);
  vecs[4] = _mm256_shuffle_epi32(vecs[4], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[3], vecs[4]);
  vecs[5] = _mm256_shuffle_epi32(vecs[5], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[2], vecs[5]);
  vecs[6] = _mm256_shuffle_epi32(vecs[6], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[1], vecs[6]);
  vecs[7] = _mm256_shuffle_epi32(vecs[7], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[0], vecs[7]);
  vecs[12] = _mm256_shuffle_epi32(vecs[12], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[11], vecs[12]);
  vecs[13] = _mm256_shuffle_epi32(vecs[13], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[10], vecs[13]);
  vecs[14] = _mm256_shuffle_epi32(vecs[14], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[9], vecs[14]);
  vecs[15] = _mm256_shuffle_epi32(vecs[15], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[8], vecs[15]);
  vecs[2] = _mm256_shuffle_epi32(vecs[2], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[1], vecs[2]);
  vecs[3] = _mm256_shuffle_epi32(vecs[3], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[0], vecs[3]);
  vecs[6] = _mm256_shuffle_epi32(vecs[6], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[5], vecs[6]);
  vecs[7] = _mm256_shuffle_epi32(vecs[7], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[4], vecs[7]);
  vecs[10] = _mm256_shuffle_epi32(vecs[10], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[9], vecs[10]);
  vecs[11] = _mm256_shuffle_epi32(vecs[11], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[8], vecs[11]);
  vecs[14] = _mm256_shuffle_epi32(vecs[14], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[13], vecs[14]);
  vecs[15] = _mm256_shuffle_epi32(vecs[15], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[12], vecs[15]);
  vecs[1] = _mm256_shuffle_epi32(vecs[1], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[0], vecs[1]);
  vecs[3] = _mm256_shuffle_epi32(vecs[3], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[2], vecs[3]);
  vecs[5] = _mm256_shuffle_epi32(vecs[5], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[4], vecs[5]);
  vecs[7] = _mm256_shuffle_epi32(vecs[7], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[6], vecs[7]);
  vecs[9] = _mm256_shuffle_epi32(vecs[9], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[8], vecs[9]);
  vecs[11] = _mm256_shuffle_epi32(vecs[11], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[10], vecs[11]);
  vecs[13] = _mm256_shuffle_epi32(vecs[13], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[12], vecs[13]);
  vecs[15] = _mm256_shuffle_epi32(vecs[15], _MM_SHUFFLE(0,1,2,3)); COEX(vecs[14], vecs[15]);
  COEX_SHUFFLE(vecs[0], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[0], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[1], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[1], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[2], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[2], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[3], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[3], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[4], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[4], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[5], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[5], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[6], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[6], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[7], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[7], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[8], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[8], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[9], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[9], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[10], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[10], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[11], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[11], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[12], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[12], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[13], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[13], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[14], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[14], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_SHUFFLE(vecs[15], 3, 2, 1, 0, 7, 6, 5, 4, ASC); COEX_SHUFFLE(vecs[15], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  REVERSE_VEC(vecs[8]); COEX(vecs[7], vecs[8]); REVERSE_VEC(vecs[9]); COEX(vecs[6], vecs[9]);
  REVERSE_VEC(vecs[10]); COEX(vecs[5], vecs[10]); REVERSE_VEC(vecs[11]); COEX(vecs[4], vecs[11]);
  REVERSE_VEC(vecs[12]); COEX(vecs[3], vecs[12]); REVERSE_VEC(vecs[13]); COEX(vecs[2], vecs[13]);
  REVERSE_VEC(vecs[14]); COEX(vecs[1], vecs[14]); REVERSE_VEC(vecs[15]); COEX(vecs[0], vecs[15]);
  REVERSE_VEC(vecs[4]); COEX(vecs[3], vecs[4]); REVERSE_VEC(vecs[5]); COEX(vecs[2], vecs[5]);
  REVERSE_VEC(vecs[6]); COEX(vecs[1], vecs[6]); REVERSE_VEC(vecs[7]); COEX(vecs[0], vecs[7]);
  REVERSE_VEC(vecs[12]); COEX(vecs[11], vecs[12]); REVERSE_VEC(vecs[13]); COEX(vecs[10], vecs[13]);
  REVERSE_VEC(vecs[14]); COEX(vecs[9], vecs[14]); REVERSE_VEC(vecs[15]); COEX(vecs[8], vecs[15]);
  REVERSE_VEC(vecs[2]); COEX(vecs[1], vecs[2]); REVERSE_VEC(vecs[3]); COEX(vecs[0], vecs[3]);
  REVERSE_VEC(vecs[6]); COEX(vecs[5], vecs[6]); REVERSE_VEC(vecs[7]); COEX(vecs[4], vecs[7]);
  REVERSE_VEC(vecs[10]); COEX(vecs[9], vecs[10]); REVERSE_VEC(vecs[11]); COEX(vecs[8], vecs[11]);
  REVERSE_VEC(vecs[14]); COEX(vecs[13], vecs[14]); REVERSE_VEC(vecs[15]); COEX(vecs[12], vecs[15]);
  REVERSE_VEC(vecs[1]); COEX(vecs[0], vecs[1]); REVERSE_VEC(vecs[3]); COEX(vecs[2], vecs[3]);
  REVERSE_VEC(vecs[5]); COEX(vecs[4], vecs[5]); REVERSE_VEC(vecs[7]); COEX(vecs[6], vecs[7]);
  REVERSE_VEC(vecs[9]); COEX(vecs[8], vecs[9]); REVERSE_VEC(vecs[11]); COEX(vecs[10], vecs[11]);
  REVERSE_VEC(vecs[13]); COEX(vecs[12], vecs[13]); REVERSE_VEC(vecs[15]); COEX(vecs[14], vecs[15]);
  COEX_PERMUTE(vecs[0], 7, 6, 5, 4, 3, 2, 1, 0, ASC); COEX_SHUFFLE(vecs[0], 2, 3, 0, 1, 6, 7, 4, 5, ASC);
  COEX_SHUFFLE(vecs[0], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_PERMUTE(vecs[1], 7, 6, 5, 4, 3, 2, 1, 0, ASC);
  COEX_SHUFFLE(vecs[1], 2, 3, 0, 1, 6, 7, 4, 5, ASC); COEX_SHUFFLE(vecs[1], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_PERMUTE(vecs[2], 7, 6, 5, 4, 3, 2, 1, 0, ASC); COEX_SHUFFLE(vecs[2], 2, 3, 0, 1, 6, 7, 4, 5, ASC);
  COEX_SHUFFLE(vecs[2], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_PERMUTE(vecs[3], 7, 6, 5, 4, 3, 2, 1, 0, ASC);
  COEX_SHUFFLE(vecs[3], 2, 3, 0, 1, 6, 7, 4, 5, ASC); COEX_SHUFFLE(vecs[3], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_PERMUTE(vecs[4], 7, 6, 5, 4, 3, 2, 1, 0, ASC); COEX_SHUFFLE(vecs[4], 2, 3, 0, 1, 6, 7, 4, 5, ASC);
  COEX_SHUFFLE(vecs[4], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_PERMUTE(vecs[5], 7, 6, 5, 4, 3, 2, 1, 0, ASC);
  COEX_SHUFFLE(vecs[5], 2, 3, 0, 1, 6, 7, 4, 5, ASC); COEX_SHUFFLE(vecs[5], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_PERMUTE(vecs[6], 7, 6, 5, 4, 3, 2, 1, 0, ASC); COEX_SHUFFLE(vecs[6], 2, 3, 0, 1, 6, 7, 4, 5, ASC);
  COEX_SHUFFLE(vecs[6], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_PERMUTE(vecs[7], 7, 6, 5, 4, 3, 2, 1, 0, ASC);
  COEX_SHUFFLE(vecs[7], 2, 3, 0, 1, 6, 7, 4, 5, ASC); COEX_SHUFFLE(vecs[7], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_PERMUTE(vecs[8], 7, 6, 5, 4, 3, 2, 1, 0, ASC); COEX_SHUFFLE(vecs[8], 2, 3, 0, 1, 6, 7, 4, 5, ASC);
  COEX_SHUFFLE(vecs[8], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_PERMUTE(vecs[9], 7, 6, 5, 4, 3, 2, 1, 0, ASC);
  COEX_SHUFFLE(vecs[9], 2, 3, 0, 1, 6, 7, 4, 5, ASC); COEX_SHUFFLE(vecs[9], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_PERMUTE(vecs[10], 7, 6, 5, 4, 3, 2, 1, 0, ASC); COEX_SHUFFLE(vecs[10], 2, 3, 0, 1, 6, 7, 4, 5, ASC);
  COEX_SHUFFLE(vecs[10], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_PERMUTE(vecs[11], 7, 6, 5, 4, 3, 2, 1, 0, ASC);
  COEX_SHUFFLE(vecs[11], 2, 3, 0, 1, 6, 7, 4, 5, ASC); COEX_SHUFFLE(vecs[11], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_PERMUTE(vecs[12], 7, 6, 5, 4, 3, 2, 1, 0, ASC); COEX_SHUFFLE(vecs[12], 2, 3, 0, 1, 6, 7, 4, 5, ASC);
  COEX_SHUFFLE(vecs[12], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_PERMUTE(vecs[13], 7, 6, 5, 4, 3, 2, 1, 0, ASC);
  COEX_SHUFFLE(vecs[13], 2, 3, 0, 1, 6, 7, 4, 5, ASC); COEX_SHUFFLE(vecs[13], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
  COEX_PERMUTE(vecs[14], 7, 6, 5, 4, 3, 2, 1, 0, ASC); COEX_SHUFFLE(vecs[14], 2, 3, 0, 1, 6, 7, 4, 5, ASC);
  COEX_SHUFFLE(vecs[14], 1, 0, 3, 2, 5, 4, 7, 6, ASC); COEX_PERMUTE(vecs[15], 7, 6, 5, 4, 3, 2, 1, 0, ASC);
  COEX_SHUFFLE(vecs[15], 2, 3, 0, 1, 6, 7, 4, 5, ASC); COEX_SHUFFLE(vecs[15], 1, 0, 3, 2, 5, 4, 7, 6, ASC);
}

inline void sort_int_sorting_network(int *arr, int *buff, int n) {
  if(n < 2) return;
  __m256i *buffer = reinterpret_cast<__m256i *>(buff);

  const auto remainder = int(n % 8 ? n % 8 : 8);
  const int idx_max_pad = n - remainder;
  const auto mask = _mm256_add_epi32(_mm256_set1_epi32(-remainder), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
  auto max_pad_vec = _mm256_blendv_epi8(_mm256_set1_epi32(INT32_MAX),_mm256_maskload_epi32(arr + idx_max_pad, mask), mask);

  for (int i = 0; i < idx_max_pad / 8; ++i) {
    buffer[i] = LOAD_VECTOR(arr + i * 8);
  }
  buffer[idx_max_pad / 8] = max_pad_vec;
  buffer[idx_max_pad / 8 + 1] = _mm256_set1_epi32(INT32_MAX);

  const int N = ((idx_max_pad % 16 == 0) * 8 + idx_max_pad + 8) / 8;

  for (int j = 0; j < N - N % 16; j += 16) {
    sort_16_int_vertical(buffer + j);
    merge_8_columns_with_16_elements(buffer + j);
  }
  for (int i = N - N % 16; i < N; i += 2) {
    sort_16(buffer[i], buffer[i + 1]);
  }
  bitonic_merge_16(buffer + N - N % 16, N % 16, 2);
  bitonic_merge_128(buffer, N, 16);
  for (int i = 0; i < idx_max_pad / 8; i += 1) {
    STORE_VECTOR(arr + i * 8, buffer[i]);
  }
  _mm256_maskstore_epi32(arr + idx_max_pad, mask, buffer[idx_max_pad / 8]);
}
/* end of sorting networks
*********************************************/

/*** vectorized quicksort
**************************************/

/* auto generated permutations masks for quicksort(8 KB..) */
const __m256i permutation_masks[256] = {_mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 0),
                                        _mm256_setr_epi32(0, 2, 3, 4, 5, 6, 7, 1),
                                        _mm256_setr_epi32(2, 3, 4, 5, 6, 7, 0, 1),
                                        _mm256_setr_epi32(0, 1, 3, 4, 5, 6, 7, 2),
                                        _mm256_setr_epi32(1, 3, 4, 5, 6, 7, 0, 2),
                                        _mm256_setr_epi32(0, 3, 4, 5, 6, 7, 1, 2),
                                        _mm256_setr_epi32(3, 4, 5, 6, 7, 0, 1, 2),
                                        _mm256_setr_epi32(0, 1, 2, 4, 5, 6, 7, 3),
                                        _mm256_setr_epi32(1, 2, 4, 5, 6, 7, 0, 3),
                                        _mm256_setr_epi32(0, 2, 4, 5, 6, 7, 1, 3),
                                        _mm256_setr_epi32(2, 4, 5, 6, 7, 0, 1, 3),
                                        _mm256_setr_epi32(0, 1, 4, 5, 6, 7, 2, 3),
                                        _mm256_setr_epi32(1, 4, 5, 6, 7, 0, 2, 3),
                                        _mm256_setr_epi32(0, 4, 5, 6, 7, 1, 2, 3),
                                        _mm256_setr_epi32(4, 5, 6, 7, 0, 1, 2, 3),
                                        _mm256_setr_epi32(0, 1, 2, 3, 5, 6, 7, 4),
                                        _mm256_setr_epi32(1, 2, 3, 5, 6, 7, 0, 4),
                                        _mm256_setr_epi32(0, 2, 3, 5, 6, 7, 1, 4),
                                        _mm256_setr_epi32(2, 3, 5, 6, 7, 0, 1, 4),
                                        _mm256_setr_epi32(0, 1, 3, 5, 6, 7, 2, 4),
                                        _mm256_setr_epi32(1, 3, 5, 6, 7, 0, 2, 4),
                                        _mm256_setr_epi32(0, 3, 5, 6, 7, 1, 2, 4),
                                        _mm256_setr_epi32(3, 5, 6, 7, 0, 1, 2, 4),
                                        _mm256_setr_epi32(0, 1, 2, 5, 6, 7, 3, 4),
                                        _mm256_setr_epi32(1, 2, 5, 6, 7, 0, 3, 4),
                                        _mm256_setr_epi32(0, 2, 5, 6, 7, 1, 3, 4),
                                        _mm256_setr_epi32(2, 5, 6, 7, 0, 1, 3, 4),
                                        _mm256_setr_epi32(0, 1, 5, 6, 7, 2, 3, 4),
                                        _mm256_setr_epi32(1, 5, 6, 7, 0, 2, 3, 4),
                                        _mm256_setr_epi32(0, 5, 6, 7, 1, 2, 3, 4),
                                        _mm256_setr_epi32(5, 6, 7, 0, 1, 2, 3, 4),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 6, 7, 5),
                                        _mm256_setr_epi32(1, 2, 3, 4, 6, 7, 0, 5),
                                        _mm256_setr_epi32(0, 2, 3, 4, 6, 7, 1, 5),
                                        _mm256_setr_epi32(2, 3, 4, 6, 7, 0, 1, 5),
                                        _mm256_setr_epi32(0, 1, 3, 4, 6, 7, 2, 5),
                                        _mm256_setr_epi32(1, 3, 4, 6, 7, 0, 2, 5),
                                        _mm256_setr_epi32(0, 3, 4, 6, 7, 1, 2, 5),
                                        _mm256_setr_epi32(3, 4, 6, 7, 0, 1, 2, 5),
                                        _mm256_setr_epi32(0, 1, 2, 4, 6, 7, 3, 5),
                                        _mm256_setr_epi32(1, 2, 4, 6, 7, 0, 3, 5),
                                        _mm256_setr_epi32(0, 2, 4, 6, 7, 1, 3, 5),
                                        _mm256_setr_epi32(2, 4, 6, 7, 0, 1, 3, 5),
                                        _mm256_setr_epi32(0, 1, 4, 6, 7, 2, 3, 5),
                                        _mm256_setr_epi32(1, 4, 6, 7, 0, 2, 3, 5),
                                        _mm256_setr_epi32(0, 4, 6, 7, 1, 2, 3, 5),
                                        _mm256_setr_epi32(4, 6, 7, 0, 1, 2, 3, 5),
                                        _mm256_setr_epi32(0, 1, 2, 3, 6, 7, 4, 5),
                                        _mm256_setr_epi32(1, 2, 3, 6, 7, 0, 4, 5),
                                        _mm256_setr_epi32(0, 2, 3, 6, 7, 1, 4, 5),
                                        _mm256_setr_epi32(2, 3, 6, 7, 0, 1, 4, 5),
                                        _mm256_setr_epi32(0, 1, 3, 6, 7, 2, 4, 5),
                                        _mm256_setr_epi32(1, 3, 6, 7, 0, 2, 4, 5),
                                        _mm256_setr_epi32(0, 3, 6, 7, 1, 2, 4, 5),
                                        _mm256_setr_epi32(3, 6, 7, 0, 1, 2, 4, 5),
                                        _mm256_setr_epi32(0, 1, 2, 6, 7, 3, 4, 5),
                                        _mm256_setr_epi32(1, 2, 6, 7, 0, 3, 4, 5),
                                        _mm256_setr_epi32(0, 2, 6, 7, 1, 3, 4, 5),
                                        _mm256_setr_epi32(2, 6, 7, 0, 1, 3, 4, 5),
                                        _mm256_setr_epi32(0, 1, 6, 7, 2, 3, 4, 5),
                                        _mm256_setr_epi32(1, 6, 7, 0, 2, 3, 4, 5),
                                        _mm256_setr_epi32(0, 6, 7, 1, 2, 3, 4, 5),
                                        _mm256_setr_epi32(6, 7, 0, 1, 2, 3, 4, 5),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 7, 6),
                                        _mm256_setr_epi32(1, 2, 3, 4, 5, 7, 0, 6),
                                        _mm256_setr_epi32(0, 2, 3, 4, 5, 7, 1, 6),
                                        _mm256_setr_epi32(2, 3, 4, 5, 7, 0, 1, 6),
                                        _mm256_setr_epi32(0, 1, 3, 4, 5, 7, 2, 6),
                                        _mm256_setr_epi32(1, 3, 4, 5, 7, 0, 2, 6),
                                        _mm256_setr_epi32(0, 3, 4, 5, 7, 1, 2, 6),
                                        _mm256_setr_epi32(3, 4, 5, 7, 0, 1, 2, 6),
                                        _mm256_setr_epi32(0, 1, 2, 4, 5, 7, 3, 6),
                                        _mm256_setr_epi32(1, 2, 4, 5, 7, 0, 3, 6),
                                        _mm256_setr_epi32(0, 2, 4, 5, 7, 1, 3, 6),
                                        _mm256_setr_epi32(2, 4, 5, 7, 0, 1, 3, 6),
                                        _mm256_setr_epi32(0, 1, 4, 5, 7, 2, 3, 6),
                                        _mm256_setr_epi32(1, 4, 5, 7, 0, 2, 3, 6),
                                        _mm256_setr_epi32(0, 4, 5, 7, 1, 2, 3, 6),
                                        _mm256_setr_epi32(4, 5, 7, 0, 1, 2, 3, 6),
                                        _mm256_setr_epi32(0, 1, 2, 3, 5, 7, 4, 6),
                                        _mm256_setr_epi32(1, 2, 3, 5, 7, 0, 4, 6),
                                        _mm256_setr_epi32(0, 2, 3, 5, 7, 1, 4, 6),
                                        _mm256_setr_epi32(2, 3, 5, 7, 0, 1, 4, 6),
                                        _mm256_setr_epi32(0, 1, 3, 5, 7, 2, 4, 6),
                                        _mm256_setr_epi32(1, 3, 5, 7, 0, 2, 4, 6),
                                        _mm256_setr_epi32(0, 3, 5, 7, 1, 2, 4, 6),
                                        _mm256_setr_epi32(3, 5, 7, 0, 1, 2, 4, 6),
                                        _mm256_setr_epi32(0, 1, 2, 5, 7, 3, 4, 6),
                                        _mm256_setr_epi32(1, 2, 5, 7, 0, 3, 4, 6),
                                        _mm256_setr_epi32(0, 2, 5, 7, 1, 3, 4, 6),
                                        _mm256_setr_epi32(2, 5, 7, 0, 1, 3, 4, 6),
                                        _mm256_setr_epi32(0, 1, 5, 7, 2, 3, 4, 6),
                                        _mm256_setr_epi32(1, 5, 7, 0, 2, 3, 4, 6),
                                        _mm256_setr_epi32(0, 5, 7, 1, 2, 3, 4, 6),
                                        _mm256_setr_epi32(5, 7, 0, 1, 2, 3, 4, 6),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 7, 5, 6),
                                        _mm256_setr_epi32(1, 2, 3, 4, 7, 0, 5, 6),
                                        _mm256_setr_epi32(0, 2, 3, 4, 7, 1, 5, 6),
                                        _mm256_setr_epi32(2, 3, 4, 7, 0, 1, 5, 6),
                                        _mm256_setr_epi32(0, 1, 3, 4, 7, 2, 5, 6),
                                        _mm256_setr_epi32(1, 3, 4, 7, 0, 2, 5, 6),
                                        _mm256_setr_epi32(0, 3, 4, 7, 1, 2, 5, 6),
                                        _mm256_setr_epi32(3, 4, 7, 0, 1, 2, 5, 6),
                                        _mm256_setr_epi32(0, 1, 2, 4, 7, 3, 5, 6),
                                        _mm256_setr_epi32(1, 2, 4, 7, 0, 3, 5, 6),
                                        _mm256_setr_epi32(0, 2, 4, 7, 1, 3, 5, 6),
                                        _mm256_setr_epi32(2, 4, 7, 0, 1, 3, 5, 6),
                                        _mm256_setr_epi32(0, 1, 4, 7, 2, 3, 5, 6),
                                        _mm256_setr_epi32(1, 4, 7, 0, 2, 3, 5, 6),
                                        _mm256_setr_epi32(0, 4, 7, 1, 2, 3, 5, 6),
                                        _mm256_setr_epi32(4, 7, 0, 1, 2, 3, 5, 6),
                                        _mm256_setr_epi32(0, 1, 2, 3, 7, 4, 5, 6),
                                        _mm256_setr_epi32(1, 2, 3, 7, 0, 4, 5, 6),
                                        _mm256_setr_epi32(0, 2, 3, 7, 1, 4, 5, 6),
                                        _mm256_setr_epi32(2, 3, 7, 0, 1, 4, 5, 6),
                                        _mm256_setr_epi32(0, 1, 3, 7, 2, 4, 5, 6),
                                        _mm256_setr_epi32(1, 3, 7, 0, 2, 4, 5, 6),
                                        _mm256_setr_epi32(0, 3, 7, 1, 2, 4, 5, 6),
                                        _mm256_setr_epi32(3, 7, 0, 1, 2, 4, 5, 6),
                                        _mm256_setr_epi32(0, 1, 2, 7, 3, 4, 5, 6),
                                        _mm256_setr_epi32(1, 2, 7, 0, 3, 4, 5, 6),
                                        _mm256_setr_epi32(0, 2, 7, 1, 3, 4, 5, 6),
                                        _mm256_setr_epi32(2, 7, 0, 1, 3, 4, 5, 6),
                                        _mm256_setr_epi32(0, 1, 7, 2, 3, 4, 5, 6),
                                        _mm256_setr_epi32(1, 7, 0, 2, 3, 4, 5, 6),
                                        _mm256_setr_epi32(0, 7, 1, 2, 3, 4, 5, 6),
                                        _mm256_setr_epi32(7, 0, 1, 2, 3, 4, 5, 6),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 0, 7),
                                        _mm256_setr_epi32(0, 2, 3, 4, 5, 6, 1, 7),
                                        _mm256_setr_epi32(2, 3, 4, 5, 6, 0, 1, 7),
                                        _mm256_setr_epi32(0, 1, 3, 4, 5, 6, 2, 7),
                                        _mm256_setr_epi32(1, 3, 4, 5, 6, 0, 2, 7),
                                        _mm256_setr_epi32(0, 3, 4, 5, 6, 1, 2, 7),
                                        _mm256_setr_epi32(3, 4, 5, 6, 0, 1, 2, 7),
                                        _mm256_setr_epi32(0, 1, 2, 4, 5, 6, 3, 7),
                                        _mm256_setr_epi32(1, 2, 4, 5, 6, 0, 3, 7),
                                        _mm256_setr_epi32(0, 2, 4, 5, 6, 1, 3, 7),
                                        _mm256_setr_epi32(2, 4, 5, 6, 0, 1, 3, 7),
                                        _mm256_setr_epi32(0, 1, 4, 5, 6, 2, 3, 7),
                                        _mm256_setr_epi32(1, 4, 5, 6, 0, 2, 3, 7),
                                        _mm256_setr_epi32(0, 4, 5, 6, 1, 2, 3, 7),
                                        _mm256_setr_epi32(4, 5, 6, 0, 1, 2, 3, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 5, 6, 4, 7),
                                        _mm256_setr_epi32(1, 2, 3, 5, 6, 0, 4, 7),
                                        _mm256_setr_epi32(0, 2, 3, 5, 6, 1, 4, 7),
                                        _mm256_setr_epi32(2, 3, 5, 6, 0, 1, 4, 7),
                                        _mm256_setr_epi32(0, 1, 3, 5, 6, 2, 4, 7),
                                        _mm256_setr_epi32(1, 3, 5, 6, 0, 2, 4, 7),
                                        _mm256_setr_epi32(0, 3, 5, 6, 1, 2, 4, 7),
                                        _mm256_setr_epi32(3, 5, 6, 0, 1, 2, 4, 7),
                                        _mm256_setr_epi32(0, 1, 2, 5, 6, 3, 4, 7),
                                        _mm256_setr_epi32(1, 2, 5, 6, 0, 3, 4, 7),
                                        _mm256_setr_epi32(0, 2, 5, 6, 1, 3, 4, 7),
                                        _mm256_setr_epi32(2, 5, 6, 0, 1, 3, 4, 7),
                                        _mm256_setr_epi32(0, 1, 5, 6, 2, 3, 4, 7),
                                        _mm256_setr_epi32(1, 5, 6, 0, 2, 3, 4, 7),
                                        _mm256_setr_epi32(0, 5, 6, 1, 2, 3, 4, 7),
                                        _mm256_setr_epi32(5, 6, 0, 1, 2, 3, 4, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 6, 5, 7),
                                        _mm256_setr_epi32(1, 2, 3, 4, 6, 0, 5, 7),
                                        _mm256_setr_epi32(0, 2, 3, 4, 6, 1, 5, 7),
                                        _mm256_setr_epi32(2, 3, 4, 6, 0, 1, 5, 7),
                                        _mm256_setr_epi32(0, 1, 3, 4, 6, 2, 5, 7),
                                        _mm256_setr_epi32(1, 3, 4, 6, 0, 2, 5, 7),
                                        _mm256_setr_epi32(0, 3, 4, 6, 1, 2, 5, 7),
                                        _mm256_setr_epi32(3, 4, 6, 0, 1, 2, 5, 7),
                                        _mm256_setr_epi32(0, 1, 2, 4, 6, 3, 5, 7),
                                        _mm256_setr_epi32(1, 2, 4, 6, 0, 3, 5, 7),
                                        _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7),
                                        _mm256_setr_epi32(2, 4, 6, 0, 1, 3, 5, 7),
                                        _mm256_setr_epi32(0, 1, 4, 6, 2, 3, 5, 7),
                                        _mm256_setr_epi32(1, 4, 6, 0, 2, 3, 5, 7),
                                        _mm256_setr_epi32(0, 4, 6, 1, 2, 3, 5, 7),
                                        _mm256_setr_epi32(4, 6, 0, 1, 2, 3, 5, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 6, 4, 5, 7),
                                        _mm256_setr_epi32(1, 2, 3, 6, 0, 4, 5, 7),
                                        _mm256_setr_epi32(0, 2, 3, 6, 1, 4, 5, 7),
                                        _mm256_setr_epi32(2, 3, 6, 0, 1, 4, 5, 7),
                                        _mm256_setr_epi32(0, 1, 3, 6, 2, 4, 5, 7),
                                        _mm256_setr_epi32(1, 3, 6, 0, 2, 4, 5, 7),
                                        _mm256_setr_epi32(0, 3, 6, 1, 2, 4, 5, 7),
                                        _mm256_setr_epi32(3, 6, 0, 1, 2, 4, 5, 7),
                                        _mm256_setr_epi32(0, 1, 2, 6, 3, 4, 5, 7),
                                        _mm256_setr_epi32(1, 2, 6, 0, 3, 4, 5, 7),
                                        _mm256_setr_epi32(0, 2, 6, 1, 3, 4, 5, 7),
                                        _mm256_setr_epi32(2, 6, 0, 1, 3, 4, 5, 7),
                                        _mm256_setr_epi32(0, 1, 6, 2, 3, 4, 5, 7),
                                        _mm256_setr_epi32(1, 6, 0, 2, 3, 4, 5, 7),
                                        _mm256_setr_epi32(0, 6, 1, 2, 3, 4, 5, 7),
                                        _mm256_setr_epi32(6, 0, 1, 2, 3, 4, 5, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(1, 2, 3, 4, 5, 0, 6, 7),
                                        _mm256_setr_epi32(0, 2, 3, 4, 5, 1, 6, 7),
                                        _mm256_setr_epi32(2, 3, 4, 5, 0, 1, 6, 7),
                                        _mm256_setr_epi32(0, 1, 3, 4, 5, 2, 6, 7),
                                        _mm256_setr_epi32(1, 3, 4, 5, 0, 2, 6, 7),
                                        _mm256_setr_epi32(0, 3, 4, 5, 1, 2, 6, 7),
                                        _mm256_setr_epi32(3, 4, 5, 0, 1, 2, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 4, 5, 3, 6, 7),
                                        _mm256_setr_epi32(1, 2, 4, 5, 0, 3, 6, 7),
                                        _mm256_setr_epi32(0, 2, 4, 5, 1, 3, 6, 7),
                                        _mm256_setr_epi32(2, 4, 5, 0, 1, 3, 6, 7),
                                        _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7),
                                        _mm256_setr_epi32(1, 4, 5, 0, 2, 3, 6, 7),
                                        _mm256_setr_epi32(0, 4, 5, 1, 2, 3, 6, 7),
                                        _mm256_setr_epi32(4, 5, 0, 1, 2, 3, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 5, 4, 6, 7),
                                        _mm256_setr_epi32(1, 2, 3, 5, 0, 4, 6, 7),
                                        _mm256_setr_epi32(0, 2, 3, 5, 1, 4, 6, 7),
                                        _mm256_setr_epi32(2, 3, 5, 0, 1, 4, 6, 7),
                                        _mm256_setr_epi32(0, 1, 3, 5, 2, 4, 6, 7),
                                        _mm256_setr_epi32(1, 3, 5, 0, 2, 4, 6, 7),
                                        _mm256_setr_epi32(0, 3, 5, 1, 2, 4, 6, 7),
                                        _mm256_setr_epi32(3, 5, 0, 1, 2, 4, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 5, 3, 4, 6, 7),
                                        _mm256_setr_epi32(1, 2, 5, 0, 3, 4, 6, 7),
                                        _mm256_setr_epi32(0, 2, 5, 1, 3, 4, 6, 7),
                                        _mm256_setr_epi32(2, 5, 0, 1, 3, 4, 6, 7),
                                        _mm256_setr_epi32(0, 1, 5, 2, 3, 4, 6, 7),
                                        _mm256_setr_epi32(1, 5, 0, 2, 3, 4, 6, 7),
                                        _mm256_setr_epi32(0, 5, 1, 2, 3, 4, 6, 7),
                                        _mm256_setr_epi32(5, 0, 1, 2, 3, 4, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(1, 2, 3, 4, 0, 5, 6, 7),
                                        _mm256_setr_epi32(0, 2, 3, 4, 1, 5, 6, 7),
                                        _mm256_setr_epi32(2, 3, 4, 0, 1, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 3, 4, 2, 5, 6, 7),
                                        _mm256_setr_epi32(1, 3, 4, 0, 2, 5, 6, 7),
                                        _mm256_setr_epi32(0, 3, 4, 1, 2, 5, 6, 7),
                                        _mm256_setr_epi32(3, 4, 0, 1, 2, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 4, 3, 5, 6, 7),
                                        _mm256_setr_epi32(1, 2, 4, 0, 3, 5, 6, 7),
                                        _mm256_setr_epi32(0, 2, 4, 1, 3, 5, 6, 7),
                                        _mm256_setr_epi32(2, 4, 0, 1, 3, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 4, 2, 3, 5, 6, 7),
                                        _mm256_setr_epi32(1, 4, 0, 2, 3, 5, 6, 7),
                                        _mm256_setr_epi32(0, 4, 1, 2, 3, 5, 6, 7),
                                        _mm256_setr_epi32(4, 0, 1, 2, 3, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(1, 2, 3, 0, 4, 5, 6, 7),
                                        _mm256_setr_epi32(0, 2, 3, 1, 4, 5, 6, 7),
                                        _mm256_setr_epi32(2, 3, 0, 1, 4, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 3, 2, 4, 5, 6, 7),
                                        _mm256_setr_epi32(1, 3, 0, 2, 4, 5, 6, 7),
                                        _mm256_setr_epi32(0, 3, 1, 2, 4, 5, 6, 7),
                                        _mm256_setr_epi32(3, 0, 1, 2, 4, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(1, 2, 0, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(0, 2, 1, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(2, 0, 1, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(1, 0, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                                        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7)};

/* partition a single vector, return how many values are greater than pivot,
 * update smallest and largest values in smallest_vec and biggest_vec respectively */
inline int partition_vec(__m256i &curr_vec, const __m256i &pivot_vec,
                         __m256i &smallest_vec, __m256i &biggest_vec){
  /* which elements are larger than the pivot */
  __m256i compared = _mm256_cmpgt_epi32(curr_vec, pivot_vec);
  /* update the smallest and largest values of the array */
  smallest_vec = _mm256_min_epi32(curr_vec, smallest_vec);
  biggest_vec = _mm256_max_epi32(curr_vec, biggest_vec);
  /* extract the most significant bit from each integer of the vector */
  int mm = _mm256_movemask_ps(_mm256_castsi256_ps(compared));
  /* how many ones, each 1 stands for an element greater than pivot */
  int amount_gt_pivot = _mm_popcnt_u32((mm));
  /* permute elements larger than pivot to the right, and,
   * smaller than or equal to the pivot, to the left */
  curr_vec = _mm256_permutevar8x32_epi32(curr_vec, permutation_masks[mm]);
  /* return how many elements are greater than pivot */
  return amount_gt_pivot; }

inline int calc_min(__m256i vec) { /* minimum of 8 int */
  auto perm_mask = _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0);
  vec = _mm256_min_epi32(vec, _mm256_permutevar8x32_epi32(vec, perm_mask));
  vec = _mm256_min_epi32(vec, _mm256_shuffle_epi32(vec, 0b10110001));
  vec = _mm256_min_epi32(vec, _mm256_shuffle_epi32(vec, 0b01001110));
  return _mm256_extract_epi32(vec, 0); }

inline int calc_max(__m256i vec){ /* maximum of 8 int */
  auto perm_mask = _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0);
  vec = _mm256_max_epi32(vec, _mm256_permutevar8x32_epi32(vec, perm_mask));
  vec = _mm256_max_epi32(vec, _mm256_shuffle_epi32(vec, 0b10110001));
  vec = _mm256_max_epi32(vec, _mm256_shuffle_epi32(vec, 0b01001110));
  return _mm256_extract_epi32(vec, 0); 
}

inline int partition_vectorized_8(int *arr, int left, int right,
                                  int pivot, int &smallest, int &biggest) {
  /* make array length divisible by eight, shortening the array */
  for (int i = (right - left) % 8; i > 0; --i) {
    smallest = std::min(smallest, arr[left]); biggest = std::max(biggest, arr[left]);
    if (arr[left] > pivot) { std::swap(arr[left], arr[--right]); }
    else { ++left; }}

  if(left == right) return left; /* less than 8 elements in the array */

  auto pivot_vec = _mm256_set1_epi32(pivot); /* fill vector with pivot */
  auto sv = _mm256_set1_epi32(smallest); /* vector for smallest elements */
  auto bv = _mm256_set1_epi32(biggest); /* vector for biggest elements */

  if(right - left == 8){ /* if 8 elements left after shortening */
    auto v = LOAD_VECTOR(arr + left);
    int amount_gt_pivot = partition_vec(v, pivot_vec, sv, bv);
    STORE_VECTOR(arr + left, v);
    smallest = calc_min(sv); biggest = calc_max(bv);
    return left + (8 - amount_gt_pivot); }

  /* first and last 8 values are partitioned at the end */
  auto vec_left = LOAD_VECTOR(arr + left); /* first 8 values */
  auto vec_right = LOAD_VECTOR(arr + (right - 8)); /* last 8 values  */
  /* store points of the vectors */
  int r_store = right - 8; /* right store point */
  int l_store = left; /* left store point */
  /* indices for loading the elements */
  left += 8; /* increase, because first 8 elements are cached */
  right -= 8; /* decrease, because last 8 elements are cached */

  while(right - left != 0) { /* partition 8 elements per iteration */
    __m256i curr_vec; /* vector to be partitioned */
    /* if fewer elements are stored on the right side of the array,
     * then next elements are loaded from the right side,
     * otherwise from the left side */
    if((r_store + 8) - right < left - l_store){
      right -= 8; curr_vec = LOAD_VECTOR(arr + right); }
    else { curr_vec = LOAD_VECTOR(arr + left); left += 8; }
    /* partition the current vector and save it on both sides of the array */
    int amount_gt_pivot = partition_vec(curr_vec, pivot_vec, sv, bv);;
    STORE_VECTOR(arr + l_store, curr_vec); STORE_VECTOR(arr + r_store, curr_vec);
    /* update store points */
    r_store -= amount_gt_pivot; l_store += (8 - amount_gt_pivot); }

  /* partition and save vec_left */
  int amount_gt_pivot = partition_vec(vec_left, pivot_vec, sv, bv);
  STORE_VECTOR(arr + l_store, vec_left); STORE_VECTOR(arr + r_store, vec_left);
  l_store += (8 - amount_gt_pivot);
  /* partition and save vec_right */
  amount_gt_pivot = partition_vec(vec_right, pivot_vec, sv, bv);
  STORE_VECTOR(arr + l_store, vec_right);
  l_store += (8 - amount_gt_pivot);

  smallest = calc_min(sv); /* determine smallest value in vector */
  biggest = calc_max(bv); /* determine largest value in vector */
  return l_store; }

/* simulate wider vector registers to speedup sorting */
inline int partition_vectorized_64(int *arr, int left, int right,
                                   int pivot, int &smallest, int &biggest) {
  if (right - left < 129) { /* do not optimize if less than 129 elements */
    return partition_vectorized_8(arr, left, right, pivot, smallest, biggest); }

    /* make array length divisible by eight, shortening the array */
  for (int i = (right - left) % 8; i > 0; --i) {
    smallest = std::min(smallest, arr[left]); biggest = std::max(biggest, arr[left]);
    if (arr[left] > pivot) { std::swap(arr[left], arr[--right]); }
    else { ++left; }}

    auto pivot_vec = _mm256_set1_epi32(pivot); /* fill vector with pivot */
    auto sv = _mm256_set1_epi32(smallest); /* vector for smallest elements */
    auto bv = _mm256_set1_epi32(biggest); /* vector for biggest elements */

    /* make array length divisible by 64, shortening the array */
  for (int i = ((right - left) % 64) / 8; i > 0; --i) {
    __m256i vec_L = LOAD_VECTOR(arr + left);
    __m256i compared = _mm256_cmpgt_epi32(vec_L, pivot_vec);
    sv = _mm256_min_epi32(vec_L, sv); bv = _mm256_max_epi32(vec_L, bv);
    int mm = _mm256_movemask_ps(_mm256_castsi256_ps(compared));
    int amount_gt_pivot = _mm_popcnt_u32((mm));
    __m256i permuted = _mm256_permutevar8x32_epi32(vec_L, permutation_masks[mm]);

    /* this is a slower way to partition an array with vector instructions */
    __m256i blend_mask = _mm256_cmpgt_epi32(permuted, pivot_vec);
    __m256i vec_R = LOAD_VECTOR(arr + right - 8);
    __m256i vec_L_new = _mm256_blendv_epi8(permuted, vec_R, blend_mask);
    __m256i vec_R_new = _mm256_blendv_epi8(vec_R, permuted, blend_mask);
    STORE_VECTOR(arr + left, vec_L_new); STORE_VECTOR(arr + right - 8, vec_R_new);
    left += (8 - amount_gt_pivot); right -= amount_gt_pivot; }

  /* buffer 8 vectors from both sides of the array */
  auto vec_left = LOAD_VECTOR(arr + left), vec_left2 = LOAD_VECTOR(arr + left + 8);
  auto vec_left3 = LOAD_VECTOR(arr + left + 16), vec_left4 = LOAD_VECTOR(arr + left + 24);
  auto vec_left5 = LOAD_VECTOR(arr + left + 32), vec_left6 = LOAD_VECTOR(arr + left + 40);
  auto vec_left7 = LOAD_VECTOR(arr + left + 48), vec_left8 = LOAD_VECTOR(arr + left + 56);
  auto vec_right = LOAD_VECTOR(arr + (right - 64)), vec_right2 = LOAD_VECTOR(arr + (right - 56));
  auto vec_right3 = LOAD_VECTOR(arr + (right - 48)), vec_right4 = LOAD_VECTOR(arr + (right - 40));
  auto vec_right5 = LOAD_VECTOR(arr + (right - 32)), vec_right6 = LOAD_VECTOR(arr + (right - 24));
  auto vec_right7 = LOAD_VECTOR(arr + (right - 16)), vec_right8 = LOAD_VECTOR(arr + (right - 8));

  /* store points of the vectors */
  int r_store = right - 64; /* right store point */
  int l_store = left; /* left store point */
  /* indices for loading the elements */
  left += 64; /* increase because first 64 elements are cached */
  right -= 64; /* decrease because last 64 elements are cached */

  while (right - left != 0) { /* partition 64 elements per iteration */
    __m256i curr_vec, curr_vec2, curr_vec3, curr_vec4, curr_vec5, curr_vec6, curr_vec7, curr_vec8;

    /* if less elements are stored on the right side of the array,
     * then next 8 vectors load from the right side, otherwise load from the left side */
    if ((r_store + 64) - right < left - l_store) {
      right -= 64;
      curr_vec = LOAD_VECTOR(arr + right); curr_vec2 = LOAD_VECTOR(arr + right + 8);
      curr_vec3 = LOAD_VECTOR(arr + right + 16); curr_vec4 = LOAD_VECTOR(arr + right + 24);
      curr_vec5 = LOAD_VECTOR(arr + right + 32); curr_vec6 = LOAD_VECTOR(arr + right + 40);
      curr_vec7 = LOAD_VECTOR(arr + right + 48); curr_vec8 = LOAD_VECTOR(arr + right + 56); }
    else {
      curr_vec = LOAD_VECTOR(arr + left); curr_vec2 = LOAD_VECTOR(arr + left + 8);
      curr_vec3 = LOAD_VECTOR(arr + left + 16); curr_vec4 = LOAD_VECTOR(arr + left + 24);
      curr_vec5 = LOAD_VECTOR(arr + left + 32); curr_vec6 = LOAD_VECTOR(arr + left + 40);
      curr_vec7 = LOAD_VECTOR(arr + left + 48); curr_vec8 = LOAD_VECTOR(arr + left + 56);
      left += 64; }

    /* partition 8 vectors and store them on both sides of the array */
    int amount_gt_pivot = partition_vec(curr_vec, pivot_vec, sv, bv);
    int amount_gt_pivot2 = partition_vec(curr_vec2, pivot_vec, sv, bv);
    int amount_gt_pivot3 = partition_vec(curr_vec3, pivot_vec, sv, bv);
    int amount_gt_pivot4 = partition_vec(curr_vec4, pivot_vec, sv, bv);
    int amount_gt_pivot5 = partition_vec(curr_vec5, pivot_vec, sv, bv);
    int amount_gt_pivot6 = partition_vec(curr_vec6, pivot_vec, sv, bv);
    int amount_gt_pivot7 = partition_vec(curr_vec7, pivot_vec, sv, bv);
    int amount_gt_pivot8 = partition_vec(curr_vec8, pivot_vec, sv, bv);

    STORE_VECTOR(arr + l_store, curr_vec); l_store += (8 - amount_gt_pivot);
    STORE_VECTOR(arr + l_store, curr_vec2); l_store += (8 - amount_gt_pivot2);
    STORE_VECTOR(arr + l_store, curr_vec3); l_store += (8 - amount_gt_pivot3);
    STORE_VECTOR(arr + l_store, curr_vec4); l_store += (8 - amount_gt_pivot4);
    STORE_VECTOR(arr + l_store, curr_vec5); l_store += (8 - amount_gt_pivot5);
    STORE_VECTOR(arr + l_store, curr_vec6); l_store += (8 - amount_gt_pivot6);
    STORE_VECTOR(arr + l_store, curr_vec7); l_store += (8 - amount_gt_pivot7);
    STORE_VECTOR(arr + l_store, curr_vec8); l_store += (8 - amount_gt_pivot8);

    STORE_VECTOR(arr + r_store + 56, curr_vec); r_store -= amount_gt_pivot;
    STORE_VECTOR(arr + r_store + 56, curr_vec2); r_store -= amount_gt_pivot2;
    STORE_VECTOR(arr + r_store + 56, curr_vec3); r_store -= amount_gt_pivot3;
    STORE_VECTOR(arr + r_store + 56, curr_vec4); r_store -= amount_gt_pivot4;
    STORE_VECTOR(arr + r_store + 56, curr_vec5); r_store -= amount_gt_pivot5;
    STORE_VECTOR(arr + r_store + 56, curr_vec6); r_store -= amount_gt_pivot6;
    STORE_VECTOR(arr + r_store + 56, curr_vec7); r_store -= amount_gt_pivot7;
    STORE_VECTOR(arr + r_store + 56, curr_vec8); r_store -= amount_gt_pivot8;
  }

  /* partition and store 8 vectors coming from the left side of the array */
  int amount_gt_pivot = partition_vec(vec_left, pivot_vec, sv, bv);
  int amount_gt_pivot2 = partition_vec(vec_left2, pivot_vec, sv, bv);
  int amount_gt_pivot3 = partition_vec(vec_left3, pivot_vec, sv, bv);
  int amount_gt_pivot4 = partition_vec(vec_left4, pivot_vec, sv, bv);
  int amount_gt_pivot5 = partition_vec(vec_left5, pivot_vec, sv, bv);
  int amount_gt_pivot6 = partition_vec(vec_left6, pivot_vec, sv, bv);
  int amount_gt_pivot7 = partition_vec(vec_left7, pivot_vec, sv, bv);
  int amount_gt_pivot8 = partition_vec(vec_left8, pivot_vec, sv, bv);

  STORE_VECTOR(arr + l_store, vec_left); l_store += (8 - amount_gt_pivot);
  STORE_VECTOR(arr + l_store, vec_left2); l_store += (8 - amount_gt_pivot2);
  STORE_VECTOR(arr + l_store, vec_left3); l_store += (8 - amount_gt_pivot3);
  STORE_VECTOR(arr + l_store, vec_left4); l_store += (8 - amount_gt_pivot4);
  STORE_VECTOR(arr + l_store, vec_left5); l_store += (8 - amount_gt_pivot5);
  STORE_VECTOR(arr + l_store, vec_left6); l_store += (8 - amount_gt_pivot6);
  STORE_VECTOR(arr + l_store, vec_left7); l_store += (8 - amount_gt_pivot7);
  STORE_VECTOR(arr + l_store, vec_left8); l_store += (8 - amount_gt_pivot8);

  STORE_VECTOR(arr + r_store + 56, vec_left); r_store -= amount_gt_pivot;
  STORE_VECTOR(arr + r_store + 56, vec_left2); r_store -= amount_gt_pivot2;
  STORE_VECTOR(arr + r_store + 56, vec_left3); r_store -= amount_gt_pivot3;
  STORE_VECTOR(arr + r_store + 56, vec_left4); r_store -= amount_gt_pivot4;
  STORE_VECTOR(arr + r_store + 56, vec_left5); r_store -= amount_gt_pivot5;
  STORE_VECTOR(arr + r_store + 56, vec_left6); r_store -= amount_gt_pivot6;
  STORE_VECTOR(arr + r_store + 56, vec_left7); r_store -= amount_gt_pivot7;
  STORE_VECTOR(arr + r_store + 56, vec_left8); r_store -= amount_gt_pivot8;

  /* partition and store 8 vectors coming from the right side of the array */
  amount_gt_pivot = partition_vec(vec_right, pivot_vec, sv, bv);
  amount_gt_pivot2 = partition_vec(vec_right2, pivot_vec, sv, bv);
  amount_gt_pivot3 = partition_vec(vec_right3, pivot_vec, sv, bv);
  amount_gt_pivot4 = partition_vec(vec_right4, pivot_vec, sv, bv);
  amount_gt_pivot5 = partition_vec(vec_right5, pivot_vec, sv, bv);
  amount_gt_pivot6 = partition_vec(vec_right6, pivot_vec, sv, bv);
  amount_gt_pivot7 = partition_vec(vec_right7, pivot_vec, sv, bv);
  amount_gt_pivot8 = partition_vec(vec_right8, pivot_vec, sv, bv);

  STORE_VECTOR(arr + l_store, vec_right); l_store += (8 - amount_gt_pivot);
  STORE_VECTOR(arr + l_store, vec_right2); l_store += (8 - amount_gt_pivot2);
  STORE_VECTOR(arr + l_store, vec_right3); l_store += (8 - amount_gt_pivot3);
  STORE_VECTOR(arr + l_store, vec_right4); l_store += (8 - amount_gt_pivot4);
  STORE_VECTOR(arr + l_store, vec_right5); l_store += (8 - amount_gt_pivot5);
  STORE_VECTOR(arr + l_store, vec_right6); l_store += (8 - amount_gt_pivot6);
  STORE_VECTOR(arr + l_store, vec_right7); l_store += (8 - amount_gt_pivot7);
  STORE_VECTOR(arr + l_store, vec_right8); l_store += (8 - amount_gt_pivot8);

  STORE_VECTOR(arr + r_store + 56, vec_right); r_store -= amount_gt_pivot;
  STORE_VECTOR(arr + r_store + 56, vec_right2); r_store -= amount_gt_pivot2;
  STORE_VECTOR(arr + r_store + 56, vec_right3); r_store -= amount_gt_pivot3;
  STORE_VECTOR(arr + r_store + 56, vec_right4); r_store -= amount_gt_pivot4;
  STORE_VECTOR(arr + r_store + 56, vec_right5); r_store -= amount_gt_pivot5;
  STORE_VECTOR(arr + r_store + 56, vec_right6); r_store -= amount_gt_pivot6;
  STORE_VECTOR(arr + r_store + 56, vec_right7); r_store -= amount_gt_pivot7;
  STORE_VECTOR(arr + r_store + 56, vec_right8);

  smallest = calc_min(sv); biggest = calc_max(bv);
  return l_store; }

/***
 * vectorized pivot selection */

/* vectorized random number generator xoroshiro128+ */
#define VROTL(x, k) /* rotate each uint64_t value in vector */               \
  _mm256_or_si256(_mm256_slli_epi64((x),(k)),_mm256_srli_epi64((x),64-(k)))

inline __m256i vnext(__m256i &s0, __m256i &s1) {
  s1 = _mm256_xor_si256(s0, s1); /* modify vectors s1 and s0 */
  s0 = _mm256_xor_si256(_mm256_xor_si256(VROTL(s0, 24), s1),
                        _mm256_slli_epi64(s1, 16));
  s1 = VROTL(s1, 37);
  return _mm256_add_epi64(s0, s1); } /* return random vector */

/* transform random numbers to the range between 0 and bound - 1 */
inline __m256i rnd_epu32(__m256i rnd_vec, __m256i bound) {
  __m256i even = _mm256_srli_epi64(_mm256_mul_epu32(rnd_vec, bound), 32);
  __m256i odd = _mm256_mul_epu32(_mm256_srli_epi64(rnd_vec, 32), bound);
  return _mm256_blend_epi32(odd, even, 0b01010101); }

/* average of two integers without overflow
 * http://aggregate.org/MAGIC/#Average%20of%20Integers */
inline int average(int a, int b) { return (a & b) + ((a ^ b) >> 1); }

inline int get_pivot(int *arr, const int left, const int right){
  auto bound = _mm256_set1_epi32(right - left + 1);
  auto left_vec = _mm256_set1_epi32(left);

  /* seeds for vectorized random number generator */
  auto s0 = _mm256_setr_epi64x(8265987198341093849, 3762817312854612374,
                               1324281658759788278, 6214952190349879213);
  auto s1 = _mm256_setr_epi64x(2874178529384792648, 1257248936691237653,
                               7874578921548791257, 1998265912745817298);
  s0 = _mm256_add_epi64(s0, _mm256_set1_epi64x(left));
  s1 = _mm256_sub_epi64(s1, _mm256_set1_epi64x(right));

  __m256i v[9];
  for (int i = 0; i < 9; ++i) { /* fill 9 vectors with random numbers */
    auto result = vnext(s0, s1); /* vector with 4 random uint64_t */
    result = rnd_epu32(result, bound); /* random numbers between 0 and bound - 1 */
    result = _mm256_add_epi32(result, left_vec); /* indices for arr */
    v[i] = _mm256_i32gather_epi32(arr, result, sizeof(uint32_t)); }

  /* median network for 9 elements */
  COEX(v[0], v[1]); COEX(v[2], v[3]); /* step 1 */
  COEX(v[4], v[5]); COEX(v[6], v[7]);
  COEX(v[0], v[2]); COEX(v[1], v[3]); /* step 2 */
  COEX(v[4], v[6]); COEX(v[5], v[7]);
  COEX(v[0], v[4]); COEX(v[1], v[2]); /* step 3 */
  COEX(v[5], v[6]); COEX(v[3], v[7]);
  COEX(v[1], v[5]); COEX(v[2], v[6]); /* step 4 */
  COEX(v[3], v[5]); COEX(v[2], v[4]); /* step 5 */
  COEX(v[3], v[4]);                   /* step 6 */
  COEX(v[3], v[8]);                   /* step 7 */
  COEX(v[4], v[8]);                   /* step 8 */

  SORT_8(v[4]); /* sort the eight medians in v[4] */
  return average(_mm256_extract_epi32(v[4], 3), /* compute next pivot */
                 _mm256_extract_epi32(v[4], 4)); }

/* recursion for quicksort */

inline void qs_core(int *arr, int left, int right,
                    bool choose_avg = false, const int avg = 0) {
  if (right - left < 513) { /* use sorting networks for small arrays */
    __m256i buffer[66]; /* buffer for sorting networks */
    int* buff = reinterpret_cast<int *>(buffer);
    sort_int_sorting_network(arr + left, buff, right - left + 1);
    return; }
  /* avg is average of largest and smallest values in array */
  int pivot = choose_avg ? avg : get_pivot(arr, left, right);
  int smallest = INT32_MAX; /* smallest value after partitioning */
  int biggest = INT32_MIN;  /* largest value after partitioning */
  int bound = partition_vectorized_64(arr, left, right + 1, pivot, smallest, biggest);
  /* the ratio of the length of the smaller partition to the array length */
  double ratio = (std::min(right-(bound-1),bound-left)/double(right-left+1));
  /* if unbalanced sub-arrays, change pivot selection strategy */
  if (ratio < 0.2) { choose_avg = !choose_avg; }
  if (pivot != smallest) /* if values in the left sub-array are not identical */
    qs_core(arr, left, bound - 1, choose_avg, average(smallest, pivot));
  if (pivot + 1 != biggest)  /* if values in the right sub-array are not identical */
    qs_core(arr, bound, right, choose_avg, average(biggest, pivot)); }

/* recursion for quickselect */
inline void qsel_core(int *arr, int left, int right, int k,
                      bool choose_avg = false, const int avg = 0) {
  if (right - left < 256) {
    /* for few elements use C++'s nth_element */
    std::nth_element(arr + left, arr + k, arr + right + 1);
    return; }
  /* avg is the average of largest and smallest values in the array */
  int pivot = choose_avg ? avg : get_pivot(arr, left, right);
  int smallest = INT32_MAX; /* smallest value after partitioning */
  int biggest = INT32_MIN;  /* largest value after partitioning */
  int bound = partition_vectorized_64(arr, left, right + 1, pivot,
                                                 smallest, biggest);
  /* the ratio of the length of the smaller partition to the array length */
  double ratio = (std::min(right-(bound-1),bound-left)/double(right-left+1));
  /* if unbalanced sub-arrays, change pivot selection strategy */
  if (ratio < 0.2) { choose_avg = !choose_avg; }
  if(k < bound){ /* k is on the left side of bound */
    if(pivot != smallest) /* if values in the left sub-array are not identical */
      qsel_core(arr, left, bound-1, k, choose_avg, average(smallest, pivot));
  } else { /* k is on the right side of bound */
    if(pivot + 1 != biggest) /* if values in the right sub-array are not identical */
      qsel_core(arr, bound, right, k, choose_avg, average(biggest, pivot));
  }}
}/* namespace _internal end */

/* call this function to determine the k-largest element */
inline void quickselect(int *arr, int n, int k) {
  _internal::qsel_core(arr, 0, n - 1, k); }

/* call this function for sorting */
inline void quicksort(int *arr, int n) { _internal::qs_core(arr, 0, n - 1); }
} /* namespace avx2 end */

#endif /* AVX2SORT_H */
