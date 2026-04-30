#define NDEBUG

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <algorithm>
#include <emmintrin.h>
#include <immintrin.h>
#include <random>

#ifdef _MSC_VER
    #define FORCEINLINE __forceinline
    #define NOINLINE __declspec(noinline)
    #define ALIGN(n) __declspec(align(n))
#else
    #define FORCEINLINE __attribute__((always_inline)) inline
    #define NOINLINE __attribute__((noinline))
    #define ALIGN(n) __attribute__((aligned(n)))
#endif

//equivalent to _MM_SHUFFLE
//(I hate the Intel guy who introduced big-endian notation in intrinsics)
#define SHUF(i0, i1, i2, i3) (i0 + i1*4 + i2*16 + i3*64)

//controls inlining of all search functions being tested
//NOINLINE means that inlining is forbidden:
//in this case benchmarking code is less likely to influence search performance
#define TESTINLINE NOINLINE


//=========================================================================
//======================= position-counting sort ==========================
//=========================================================================

//Simple scalar implementation.
template<bool WithValues, size_t Count, bool AssumeDistinct> TESTINLINE void PCSort_Scalar(
    const int *inKeys, const int *inVals,
    int *dstKeys, int *dstVals
) {
    for (size_t i = 0; i < Count; i++) {
        int elem = inKeys[i];

        size_t cnt = 0;
        for (size_t j = 0; j < Count; j++)
            cnt += (inKeys[j] < elem);
        if (!AssumeDistinct) {
            for (size_t j = 0; j < i; j++)
                cnt += (inKeys[j] == elem);
        }
        
        dstKeys[cnt] = inKeys[i];
        if (WithValues)
            dstVals[cnt] = inVals[i];
    }
}

//Main vectorized implementation.
//Similar to PCSort_Scalar, but with vectorization across both loops.
template<bool WithValues, size_t Count, bool AssumeDistinct> TESTINLINE void PCSort_Main(
    const int *inKeys, const int *inVals,
    int *dstKeys, int *dstVals
) {
    static_assert(Count % 4 == 0, "Unaligned count");

    for (size_t i = 0; i < Count; i += 4) {
        __m128i reg = _mm_load_si128((__m128i*)&inKeys[i]);
        __m128i reg0 = _mm_shuffle_epi32(reg, SHUF(0, 0, 0, 0));
        __m128i reg1 = _mm_shuffle_epi32(reg, SHUF(1, 1, 1, 1));
        __m128i reg2 = _mm_shuffle_epi32(reg, SHUF(2, 2, 2, 2));
        __m128i reg3 = _mm_shuffle_epi32(reg, SHUF(3, 3, 3, 3));

        __m128i cnt0 = _mm_setzero_si128();
        __m128i cnt1 = _mm_setzero_si128();
        __m128i cnt2 = _mm_setzero_si128();
        __m128i cnt3 = _mm_setzero_si128();
        for (size_t j = 0; j < Count; j += 4) {
            __m128i data = _mm_load_si128((__m128i*)&inKeys[j]);
            cnt0 = _mm_sub_epi32(cnt0, _mm_cmplt_epi32(data, reg0));
            cnt1 = _mm_sub_epi32(cnt1, _mm_cmplt_epi32(data, reg1));
            cnt2 = _mm_sub_epi32(cnt2, _mm_cmplt_epi32(data, reg2));
            cnt3 = _mm_sub_epi32(cnt3, _mm_cmplt_epi32(data, reg3));
        }
        if (!AssumeDistinct) {
            for (size_t j = 0; j < i; j += 4) {
                __m128i data = _mm_load_si128((__m128i*)&inKeys[j]);
                cnt0 = _mm_sub_epi32(cnt0, _mm_cmpeq_epi32(data, reg0));
                cnt1 = _mm_sub_epi32(cnt1, _mm_cmpeq_epi32(data, reg1));
                cnt2 = _mm_sub_epi32(cnt2, _mm_cmpeq_epi32(data, reg2));
                cnt3 = _mm_sub_epi32(cnt3, _mm_cmpeq_epi32(data, reg3));
            }
            //cnt0 = _mm_sub_epi32(cnt0, _mm_and_si128(_mm_cmplt_epi32(reg, reg0), _mm_setr_epi32( 0,  0,  0,  0)));
            cnt1 = _mm_sub_epi32(cnt1, _mm_and_si128(_mm_cmpeq_epi32(reg, reg1), _mm_setr_epi32(-1,  0,  0,  0)));
            cnt2 = _mm_sub_epi32(cnt2, _mm_and_si128(_mm_cmpeq_epi32(reg, reg2), _mm_setr_epi32(-1, -1,  0,  0)));
            cnt3 = _mm_sub_epi32(cnt3, _mm_and_si128(_mm_cmpeq_epi32(reg, reg3), _mm_setr_epi32(-1, -1, -1,  0)));
        }
        
        __m128i c01L = _mm_unpacklo_epi32(cnt0, cnt1);
        __m128i c01H = _mm_unpackhi_epi32(cnt0, cnt1);
        __m128i c23L = _mm_unpacklo_epi32(cnt2, cnt3);
        __m128i c23H = _mm_unpackhi_epi32(cnt2, cnt3);
        __m128i cntX = _mm_unpacklo_epi64(c01L, c23L);
        __m128i cntY = _mm_unpackhi_epi64(c01L, c23L);
        __m128i cntZ = _mm_unpacklo_epi64(c01H, c23H);
        __m128i cntW = _mm_unpackhi_epi64(c01H, c23H);
        __m128i cnt = _mm_add_epi32(_mm_add_epi32(cntX, cntY), _mm_add_epi32(cntZ, cntW));

        unsigned k0 = _mm_cvtsi128_si32(cnt   );
        unsigned k1 = _mm_extract_epi32(cnt, 1);
        unsigned k2 = _mm_extract_epi32(cnt, 2);
        unsigned k3 = _mm_extract_epi32(cnt, 3);
        dstKeys[k0] = inKeys[i+0];
        dstKeys[k1] = inKeys[i+1];
        dstKeys[k2] = inKeys[i+2];
        dstKeys[k3] = inKeys[i+3];
        if (WithValues) {
            dstVals[k0] = inVals[i+0];
            dstVals[k1] = inVals[i+1];
            dstVals[k2] = inVals[i+2];
            dstVals[k3] = inVals[i+3];
        }
    }
}

//Optimized version of PCSort_Main: process each inKeys[j] once during each outer iteration
template<bool WithValues, size_t Count, bool AssumeDistinct> TESTINLINE void PCSort_Optimized(
    const int *inKeys, const int *inVals,
    int *dstKeys, int *dstVals
) {
    static_assert(Count % 4 == 0, "Unaligned count");
    static_assert(!AssumeDistinct, "Always handles equal elements properly");

    for (size_t i = 0; i < Count; i += 4) {
        __m128i reg = _mm_load_si128((__m128i*)&inKeys[i]);
        __m128i reg0 = _mm_shuffle_epi32(reg, SHUF(0, 0, 0, 0));
        __m128i reg1 = _mm_shuffle_epi32(reg, SHUF(1, 1, 1, 1));
        __m128i reg2 = _mm_shuffle_epi32(reg, SHUF(2, 2, 2, 2));
        __m128i reg3 = _mm_shuffle_epi32(reg, SHUF(3, 3, 3, 3));

        __m128i cnt0 = _mm_setzero_si128();
        __m128i cnt1 = _mm_and_si128(_mm_cmpeq_epi32(reg, reg1), _mm_setr_epi32(1, 0, 0, 0));
        __m128i cnt2 = _mm_and_si128(_mm_cmpeq_epi32(reg, reg2), _mm_setr_epi32(1, 1, 0, 0));
        __m128i cnt3 = _mm_and_si128(_mm_cmpeq_epi32(reg, reg3), _mm_setr_epi32(1, 1, 1, 0));
        size_t j = 0;
        for (; j < i; j += 4) {
            __m128i data = _mm_load_si128((__m128i*)&inKeys[j]);
            cnt0 = _mm_add_epi32(cnt0, _mm_cmpgt_epi32(data, reg0));
            cnt1 = _mm_add_epi32(cnt1, _mm_cmpgt_epi32(data, reg1));
            cnt2 = _mm_add_epi32(cnt2, _mm_cmpgt_epi32(data, reg2));
            cnt3 = _mm_add_epi32(cnt3, _mm_cmpgt_epi32(data, reg3));
        }
        for (; j < Count; j += 4) {
            __m128i data = _mm_load_si128((__m128i*)&inKeys[j]);
            cnt0 = _mm_sub_epi32(cnt0, _mm_cmplt_epi32(data, reg0));
            cnt1 = _mm_sub_epi32(cnt1, _mm_cmplt_epi32(data, reg1));
            cnt2 = _mm_sub_epi32(cnt2, _mm_cmplt_epi32(data, reg2));
            cnt3 = _mm_sub_epi32(cnt3, _mm_cmplt_epi32(data, reg3));
        }
        
        __m128i c01L = _mm_unpacklo_epi32(cnt0, cnt1);
        __m128i c01H = _mm_unpackhi_epi32(cnt0, cnt1);
        __m128i c23L = _mm_unpacklo_epi32(cnt2, cnt3);
        __m128i c23H = _mm_unpackhi_epi32(cnt2, cnt3);
        __m128i cntX = _mm_unpacklo_epi64(c01L, c23L);
        __m128i cntY = _mm_unpackhi_epi64(c01L, c23L);
        __m128i cntZ = _mm_unpacklo_epi64(c01H, c23H);
        __m128i cntW = _mm_unpackhi_epi64(c01H, c23H);
        __m128i cnt = _mm_add_epi32(_mm_add_epi32(cntX, cntY), _mm_add_epi32(cntZ, cntW));

        unsigned k0 = _mm_cvtsi128_si32(cnt   ) + i;
        unsigned k1 = _mm_extract_epi32(cnt, 1) + i;
        unsigned k2 = _mm_extract_epi32(cnt, 2) + i;
        unsigned k3 = _mm_extract_epi32(cnt, 3) + i;
        dstKeys[k0] = inKeys[i+0];
        dstKeys[k1] = inKeys[i+1];
        dstKeys[k2] = inKeys[i+2];
        dstKeys[k3] = inKeys[i+3];
        if (WithValues) {
            dstVals[k0] = inVals[i+0];
            dstVals[k1] = inVals[i+1];
            dstVals[k2] = inVals[i+2];
            dstVals[k3] = inVals[i+3];
        }
    }
}

//Different vectorization approach.
//Process 16 elements in outer loop, compare with broadcasted version of each element in the inner loop.
template<bool WithValues, size_t Count, bool AssumeDistinct> TESTINLINE void PCSort_WideOuter(
    const int *inKeys, const int *inVals,
    int *dstKeys, int *dstVals
) {
    static_assert(Count % 16 == 0, "Unaligned count");
    static_assert(AssumeDistinct, "Works only for distinct elements");

    for (size_t i = 0; i < Count; i += 16) {
        __m128i reg0 = _mm_load_si128((__m128i*)&inKeys[i + 0]);
        __m128i reg1 = _mm_load_si128((__m128i*)&inKeys[i + 4]);
        __m128i reg2 = _mm_load_si128((__m128i*)&inKeys[i + 8]);
        __m128i reg3 = _mm_load_si128((__m128i*)&inKeys[i + 12]);

        __m128i cnt0 = _mm_setzero_si128();
        __m128i cnt1 = _mm_setzero_si128();
        __m128i cnt2 = _mm_setzero_si128();
        __m128i cnt3 = _mm_setzero_si128();
        for (size_t j = 0; j < Count; j += 4) {
            __m128i data = _mm_load_si128((__m128i*)&inKeys[j]);
            #define CMP(t) \
                __m128i X##t = _mm_shuffle_epi32(data, SHUF(t, t, t, t)); \
                cnt0 = _mm_sub_epi32(cnt0, _mm_cmplt_epi32(X##t, reg0)); \
                cnt1 = _mm_sub_epi32(cnt1, _mm_cmplt_epi32(X##t, reg1)); \
                cnt2 = _mm_sub_epi32(cnt2, _mm_cmplt_epi32(X##t, reg2)); \
                cnt3 = _mm_sub_epi32(cnt3, _mm_cmplt_epi32(X##t, reg3));
            CMP(0);
            CMP(1);
            CMP(2);
            CMP(3);
            #undef CMP
        }
        
        #define MOVE(t) { \
            unsigned k0 = _mm_cvtsi128_si32(cnt##t   ); \
            unsigned k1 = _mm_extract_epi32(cnt##t, 1); \
            unsigned k2 = _mm_extract_epi32(cnt##t, 2); \
            unsigned k3 = _mm_extract_epi32(cnt##t, 3); \
            dstKeys[k0] = inKeys[i + 4*t + 0]; \
            dstKeys[k1] = inKeys[i + 4*t + 1]; \
            dstKeys[k2] = inKeys[i + 4*t + 2]; \
            dstKeys[k3] = inKeys[i + 4*t + 3]; \
            if (WithValues) { \
                dstVals[k0] = inVals[i + 4*t + 0]; \
                dstVals[k1] = inVals[i + 4*t + 1]; \
                dstVals[k2] = inVals[i + 4*t + 2]; \
                dstVals[k3] = inVals[i + 4*t + 3]; \
            } \
        }
        MOVE(0);
        MOVE(1);
        MOVE(2);
        MOVE(3);
        #undef MOVE
    }
}

//Fully unrolled version of PCSort_WideOuter for 16 elements.
template<bool WithValues, size_t Count, bool AssumeDistinct> TESTINLINE void PCSort_WideOuter_U16(
    const int *inKeys, const int *inVals,
    int *dstKeys, int *dstVals
) {
    assert(Count == 16);
    static_assert(AssumeDistinct, "Works only for distinct elements");

    __m128i reg0 = _mm_load_si128((__m128i*)&inKeys[0]);
    __m128i reg1 = _mm_load_si128((__m128i*)&inKeys[4]);
    __m128i reg2 = _mm_load_si128((__m128i*)&inKeys[8]);
    __m128i reg3 = _mm_load_si128((__m128i*)&inKeys[12]);

    __m128i cnt0 = _mm_setzero_si128();
    __m128i cnt1 = _mm_setzero_si128();
    __m128i cnt2 = _mm_setzero_si128();
    __m128i cnt3 = _mm_setzero_si128();
    #define CMP(t) \
        __m128i X##t = _mm_shuffle_epi32(data, SHUF(t, t, t, t)); \
        cnt0 = _mm_sub_epi32(cnt0, _mm_cmplt_epi32(X##t, reg0)); \
        cnt1 = _mm_sub_epi32(cnt1, _mm_cmplt_epi32(X##t, reg1)); \
        cnt2 = _mm_sub_epi32(cnt2, _mm_cmplt_epi32(X##t, reg2)); \
        cnt3 = _mm_sub_epi32(cnt3, _mm_cmplt_epi32(X##t, reg3));
    #define PROCESS(j) { \
        __m128i data = reg##j; \
        CMP(0); \
        CMP(1); \
        CMP(2); \
        CMP(3); \
    }
    PROCESS(0);
    PROCESS(1);
    PROCESS(2);
    PROCESS(3);
    #undef PROCESS
    #undef CMP
    
    #define MOVE(t) { \
        unsigned k0 = _mm_cvtsi128_si32(cnt##t   ); \
        unsigned k1 = _mm_extract_epi32(cnt##t, 1); \
        unsigned k2 = _mm_extract_epi32(cnt##t, 2); \
        unsigned k3 = _mm_extract_epi32(cnt##t, 3); \
        dstKeys[k0] = inKeys[4*t + 0]; \
        dstKeys[k1] = inKeys[4*t + 1]; \
        dstKeys[k2] = inKeys[4*t + 2]; \
        dstKeys[k3] = inKeys[4*t + 3]; \
        if (WithValues) { \
            dstVals[k0] = inVals[4*t + 0]; \
            dstVals[k1] = inVals[4*t + 1]; \
            dstVals[k2] = inVals[4*t + 2]; \
            dstVals[k3] = inVals[4*t + 3]; \
        } \
    }
    MOVE(0);
    MOVE(1);
    MOVE(2);
    MOVE(3);
    #undef MOVE
}

//Yet another vectorization approach (transposed).
//Now the elements "to be moved" are iterated in the inner loop.
//Already optimized version: each element is processed once during each outer iteration.
template<bool WithValues, size_t Count, bool AssumeDistinct> TESTINLINE void PCSort_Trans(
    const int *inKeys, const int *inVals,
    int *dstKeys, int *dstVals
) {
    static_assert(Count % 4 == 0, "Unaligned count");
    static_assert(!AssumeDistinct, "Always handles equal elements properly");

    __m128i cnt[Count/4];
    memset(cnt, 0, sizeof(cnt));

    for (size_t i = 0; i < Count/4; i++) {
        __m128i reg = _mm_load_si128((__m128i*)&inKeys[4*i]);
        __m128i reg0 = _mm_shuffle_epi32(reg, SHUF(0, 0, 0, 0));
        __m128i reg1 = _mm_shuffle_epi32(reg, SHUF(1, 1, 1, 1));
        __m128i reg2 = _mm_shuffle_epi32(reg, SHUF(2, 2, 2, 2));
        __m128i reg3 = _mm_shuffle_epi32(reg, SHUF(3, 3, 3, 3));

        for (size_t j = 0; j <= i; j++) {
            __m128i data = _mm_load_si128((__m128i*)&inKeys[4*j]);
            __m128i cmp0 = _mm_cmplt_epi32(reg0, data);
            __m128i cmp1 = _mm_cmplt_epi32(reg1, data);
            __m128i cmp2 = _mm_cmplt_epi32(reg2, data);
            __m128i cmp3 = _mm_cmplt_epi32(reg3, data);
            __m128i sum = _mm_add_epi32(_mm_add_epi32(cmp0, cmp1), _mm_add_epi32(cmp2, cmp3));
            _mm_store_si128(&cnt[j], _mm_sub_epi32(_mm_load_si128(&cnt[j]), sum));
        }
        __m128i data = _mm_load_si128((__m128i*)&inKeys[4*i]);
        __m128i cmp0 = _mm_and_si128(_mm_cmpeq_epi32(reg0, data), _mm_setr_epi32(0, -1, -1, -1));
        __m128i cmp1 = _mm_and_si128(_mm_cmpeq_epi32(reg1, data), _mm_setr_epi32(0,  0, -1, -1));
        __m128i cmp2 = _mm_and_si128(_mm_cmpeq_epi32(reg2, data), _mm_setr_epi32(0,  0,  0, -1));
        //__m128i cmp3 = _mm_and_si128(_mm_cmpeq_epi32(reg3, data), _mm_setr_epi32(0,  0,  0,  0));
        __m128i sum = _mm_add_epi32(_mm_add_epi32(cmp0, cmp1), cmp2);
        _mm_store_si128(&cnt[i], _mm_sub_epi32(_mm_load_si128(&cnt[i]), sum));
        for (size_t j = i+1; j < Count/4; j++) {
            __m128i data = _mm_load_si128((__m128i*)&inKeys[4*j]);
            __m128i cmp0 = _mm_cmpgt_epi32(reg0, data);
            __m128i cmp1 = _mm_cmpgt_epi32(reg1, data);
            __m128i cmp2 = _mm_cmpgt_epi32(reg2, data);
            __m128i cmp3 = _mm_cmpgt_epi32(reg3, data);
            __m128i sum = _mm_add_epi32(_mm_add_epi32(cmp0, cmp1), _mm_add_epi32(cmp2, cmp3));
            _mm_store_si128(&cnt[j], _mm_add_epi32(_mm_load_si128(&cnt[j]), sum));
        }
    }
        
    for (size_t i = 0; i < Count; i+=4) {
        unsigned k0 = ((unsigned*)cnt)[i+0] + i;
        unsigned k1 = ((unsigned*)cnt)[i+1] + i;
        unsigned k2 = ((unsigned*)cnt)[i+2] + i;
        unsigned k3 = ((unsigned*)cnt)[i+3] + i;
        dstKeys[k0] = inKeys[i+0];
        dstKeys[k1] = inKeys[i+1];
        dstKeys[k2] = inKeys[i+2];
        dstKeys[k3] = inKeys[i+3];
        if (WithValues) {
            dstVals[k0] = inVals[i+0];
            dstVals[k1] = inVals[i+1];
            dstVals[k2] = inVals[i+2];
            dstVals[k3] = inVals[i+3];
        }
    }
}


//=========================================================================
//=========================== sorting networks ============================
//=========================================================================

#ifdef _MSC_VER
    //from http://stackoverflow.com/q/2786899/556899
    //CMOV-s are generated on MSVC
    #define MIN(x, y) (x < y ? x : y)
    #define MAX(x, y) (x < y ? y : x) 
    #define COMPARATOR(i, j) {\
        auto &x = dstKeys[i]; \
        auto &y = dstKeys[j]; \
        auto a = MIN(x, y);\
        auto b = MAX(x, y);\
        x = a;\
        y = b;\
    }
#else
    //GCC is not smart enough to put CMOV-s?
    #define COMPARATOR(x,y) { int tmp; asm( \
        "mov %0, %2 ; cmp %1, %0 ; cmovg %1, %0 ; cmovg %2, %1" \
        : "=r" (dstKeys[x]), "=r" (dstKeys[y]), "=r" (tmp) \
        : "0" (dstKeys[x]), "1" (dstKeys[y]) : "cc" \
    ); }
#endif

//Green's sorting network with 60 comparators in 10 levels.
//Taken from https://github.com/Morwenn/cpp-sort/blob/master/include/cpp-sort/detail/sorting_network/sort16.h
//Note: this is a common piece of two sorting functions.
template<bool WithValues>
FORCEINLINE void _SortingNetwork_16(const int *inKeys, const int *inVals, int *dstKeys, int *dstVals) {
    COMPARATOR(0, 1);
    COMPARATOR(2, 3);
    COMPARATOR(4, 5);
    COMPARATOR(6, 7);
    COMPARATOR(8, 9);
    COMPARATOR(10, 11);
    COMPARATOR(12, 13);
    COMPARATOR(14, 15);
    COMPARATOR(0, 2);
    COMPARATOR(4, 6);
    COMPARATOR(8, 10);
    COMPARATOR(12, 14);
    COMPARATOR(1, 3);
    COMPARATOR(5, 7);
    COMPARATOR(9, 11);
    COMPARATOR(13, 15);
    COMPARATOR(0, 4);
    COMPARATOR(8, 12);
    COMPARATOR(1, 5);
    COMPARATOR(9, 13);
    COMPARATOR(2, 6);
    COMPARATOR(10, 14);
    COMPARATOR(3, 7);
    COMPARATOR(11, 15);
    COMPARATOR(0, 8);
    COMPARATOR(1, 9);
    COMPARATOR(2, 10);
    COMPARATOR(3, 11);
    COMPARATOR(4, 12);
    COMPARATOR(5, 13);
    COMPARATOR(6, 14);
    COMPARATOR(7, 15);
    COMPARATOR(5, 10);
    COMPARATOR(6, 9);
    COMPARATOR(3, 12);
    COMPARATOR(13, 14);
    COMPARATOR(7, 11);
    COMPARATOR(1, 2);
    COMPARATOR(4, 8);
    COMPARATOR(1, 4);
    COMPARATOR(7, 13);
    COMPARATOR(2, 8);
    COMPARATOR(11, 14);
    COMPARATOR(2, 4);
    COMPARATOR(5, 6);
    COMPARATOR(9, 10);
    COMPARATOR(11, 13);
    COMPARATOR(3, 8);
    COMPARATOR(7, 12);
    COMPARATOR(6, 8);
    COMPARATOR(10, 12);
    COMPARATOR(3, 5);
    COMPARATOR(7, 9);
    COMPARATOR(3, 4);
    COMPARATOR(5, 6);
    COMPARATOR(7, 8);
    COMPARATOR(9, 10);
    COMPARATOR(11, 12);
    COMPARATOR(6, 7);
    COMPARATOR(8, 9);
}

//Sorting network for 16 elements.
template<bool WithValues>
TESTINLINE void SortingNetwork_16(const int *inKeys, const int *inVals, int *dstKeys, int *dstVals) {
    assert(!WithValues);
    memcpy(dstKeys, inKeys, 16 * sizeof(inKeys[0]));
    //memset(dstVals, inVals, 16 * sizeof(inVals[0]));
    _SortingNetwork_16<WithValues>(inKeys, inVals, dstKeys, dstVals);
}

//Sorting network for 32 elements.
template<bool WithValues>
TESTINLINE void SortingNetwork_32(const int *inKeys, const int *inVals, int *dstKeys, int *dstVals) {
    assert(!WithValues);
    memcpy(dstKeys, inKeys, 32 * sizeof(inKeys[0]));
    //memset(dstVals, inVals, 32 * sizeof(inVals[0]));
    _SortingNetwork_16<WithValues>(inKeys, inVals, dstKeys, dstVals);
    _SortingNetwork_16<WithValues>(inKeys+16, inVals+16, dstKeys+16, dstVals+16);
    COMPARATOR(0, 16);
    COMPARATOR(8, 24);
    COMPARATOR(8, 16);
    COMPARATOR(4, 20);
    COMPARATOR(12, 28);
    COMPARATOR(12, 20);
    COMPARATOR(4, 8);
    COMPARATOR(12, 16);
    COMPARATOR(20, 24);
    COMPARATOR(2, 18);
    COMPARATOR(10, 26);
    COMPARATOR(10, 18);
    COMPARATOR(6, 22);
    COMPARATOR(14, 30);
    COMPARATOR(14, 22);
    COMPARATOR(6, 10);
    COMPARATOR(14, 18);
    COMPARATOR(22, 26);
    COMPARATOR(2, 4);
    COMPARATOR(6, 8);
    COMPARATOR(10, 12);
    COMPARATOR(14, 16);
    COMPARATOR(18, 20);
    COMPARATOR(22, 24);
    COMPARATOR(26, 28);
    COMPARATOR(1, 17);
    COMPARATOR(9, 25);
    COMPARATOR(9, 17);
    COMPARATOR(5, 21);
    COMPARATOR(13, 29);
    COMPARATOR(13, 21);
    COMPARATOR(5, 9);
    COMPARATOR(13, 17);
    COMPARATOR(21, 25);
    COMPARATOR(3, 19);
    COMPARATOR(11, 27);
    COMPARATOR(11, 19);
    COMPARATOR(7, 23);
    COMPARATOR(15, 31);
    COMPARATOR(15, 23);
    COMPARATOR(7, 11);
    COMPARATOR(15, 19);
    COMPARATOR(23, 27);
    COMPARATOR(3, 5);
    COMPARATOR(7, 9);
    COMPARATOR(11, 13);
    COMPARATOR(15, 17);
    COMPARATOR(19, 21);
    COMPARATOR(23, 25);
    COMPARATOR(27, 29);
    COMPARATOR(1, 2);
    COMPARATOR(3, 4);
    COMPARATOR(5, 6);
    COMPARATOR(7, 8);
    COMPARATOR(9, 10);
    COMPARATOR(11, 12);
    COMPARATOR(13, 14);
    COMPARATOR(15, 16);
    COMPARATOR(17, 18);
    COMPARATOR(19, 20);
    COMPARATOR(21, 22);
    COMPARATOR(23, 24);
    COMPARATOR(25, 26);
    COMPARATOR(27, 28);
    COMPARATOR(29, 30);
}


//=========================================================================
//============================= other sorts ===============================
//=========================================================================

template<bool WithValues, size_t Count>
TESTINLINE void StdSort(const int *inKeys, const int *inVals, int *dstKeys, int *dstVals) {
    assert(!WithValues);
    memcpy(dstKeys, inKeys, Count * sizeof(inKeys[0]));
    std::sort(dstKeys, dstKeys + Count);
}

template<bool WithValues, size_t Count>
TESTINLINE void SimpleSort(const int *inKeys, const int *inVals, int *dstKeys, int *dstVals) {
    memcpy(dstKeys, inKeys, Count * sizeof(inKeys[0]));
    for (size_t i = 0; i < Count; i++)
        for (size_t j = 0; j < i; j++)
            if (dstKeys[j] > dstKeys[i]) {
                std::swap(dstKeys[j], dstKeys[i]);
                if (WithValues) std::swap(dstVals[j], dstVals[i]);
            }
}

template<bool WithValues, size_t Count>
TESTINLINE void SelectionSort(const int *inKeys, const int *inVals, int *dstKeys, int *dstVals) {
    memcpy(dstKeys, inKeys, Count * sizeof(inKeys[0]));
    for (size_t i = 0; i < Count; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < Count; j++)
            best = (dstKeys[best] < dstKeys[j] ? best : j);
        std::swap(dstKeys[best], dstKeys[i]);
        if (WithValues) std::swap(dstVals[best], dstVals[i]);
    }
}

template<bool WithValues, size_t Count>
TESTINLINE void InsertionSort(const int *inKeys, const int *inVals, int *dstKeys, int *dstVals) {
    for (size_t i = 0; i < Count; i++) {
        int x = inKeys[i];

        ptrdiff_t pos = i - 1;
        while (pos >= 0 && dstKeys[pos] > x) {
            dstKeys[pos+1] = dstKeys[pos];
            if (WithValues) dstVals[pos+1] = dstVals[pos];
            pos--;
        }
        dstKeys[pos+1] = x;
        if (WithValues) dstVals[pos+1] = inVals[i];
    }
}


//=========================================================================
//=============================== testing =================================
//=========================================================================

//number of elements in each array
const int PACK = 32;
//if true, then second array of values must also be rearranged
const bool WithValues = false;
//number of test arrays
const int MAXN = 256;
//number of sorts performed
const int TRIES = 1<<20;


//input arrays (keys and values)
ALIGN(32) int arrKeys[MAXN][PACK], arrVals[MAXN][PACK];
//final sorted arrays
ALIGN(32) int resKeys[MAXN][PACK], resVals[MAXN][PACK];

//input arrays generation (random)
std::mt19937 rnd;
uint32_t trandom(uint32_t maxValue) {
  return std::uniform_int_distribution<uint32_t>(0, maxValue)(rnd);
}
bool hasEqual(int *arr) {
    for (int i = 0; i < PACK; i++)
        for (int j = 0; j < i; j++)
            if (arr[i] == arr[j])
                return true;
    return false;
}
void GenInputs(bool assumeDistinct) {
    for (int i = 0; i < MAXN; i++) {
        do {
            for (int j = 0; j < PACK; j++) {
                arrKeys[i][j] = trandom(assumeDistinct ? 1000000000 : 100);
                arrVals[i][j] = trandom(1000000000);
            }
        } while (assumeDistinct && hasEqual(arrKeys[i]));
    }
}

//test and benchmark some sort implementation
void TestSearch(void (*pSort)(const int *, const int *, int *, int *), const char *format, ...) {
    memset(resKeys, -63, sizeof(resKeys));
    memset(resVals, -63, sizeof(resVals));

    int start = clock();
    int check = 0;
    for (int t = 0; t < TRIES; t++) {
        int i = t & (MAXN-1);
        pSort(arrKeys[i], arrVals[i], resKeys[i], resVals[i]);
        check += resKeys[i][0] + resKeys[i][PACK-1];
        assert(std::is_sorted(resKeys[i], resKeys[i] + PACK));  //note: values not checked
    }
    double elapsed = double(clock() - start) / CLOCKS_PER_SEC;

    char funcname[256];
    va_list args;
    va_start(args, format);
    vsprintf(funcname, format, args);
    va_end(args);

    printf("%8.1lf ns : %-40s   (%d)\n", 1e+9 * elapsed / TRIES, funcname, check);
}


int main() {
    #define Test_PC(func, D) TestSearch(func<WithValues, PACK, D>, "%s:%s", #func, (D ? "distinct" : "any"));
    #define Test(func)       TestSearch(func<WithValues, PACK>, "%s", #func);
    #define Test_SZ(func)    TestSearch(func<WithValues>, "%s", #func);

    printf("Number of elements = %d\n", PACK);

    // With equal elements:
    GenInputs(false);
    if (false)
        Test_PC(PCSort_Scalar, false);
    if (true)
        Test_PC(PCSort_Main, false);
    if (true)
        Test_PC(PCSort_Optimized, false);
    if (true)
        Test_PC(PCSort_Trans, false);

    // Only distinct elements:
    GenInputs(true);
    if (false)
        Test_PC(PCSort_Scalar, true);
    if (true)
        Test_PC(PCSort_Main, true);
    if (true)
        Test_PC(PCSort_WideOuter, true);
    if (PACK == 16)
        Test_PC(PCSort_WideOuter_U16, true);

    // Other sorts:
    GenInputs(false);
    if (PACK == 16 && !WithValues)
        Test_SZ(SortingNetwork_16);
    if (PACK == 32 && !WithValues)
        Test_SZ(SortingNetwork_32);
    if (true)
        Test(InsertionSort);
    if (!WithValues)
        Test(StdSort);
    if (false)
        Test(SelectionSort);
    if (false)
        Test(SimpleSort);

    return 0;
}
