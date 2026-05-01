//////////////////////////////////////////////////////////
/// Code to sort an 2 arrays of integers
/// using avx 512 (targeting intel KNL/SKL).
/// By berenger.bramas@mpcdf.mpg.de 2017.
/// Licence is MIT.
/// Comes without any warranty.
///
///
/// Functions to call:
/// Sort512kv::Sort(); to sort an array
/// Sort512kv::SortOmp(); to sort in parallel
/// Sort512kv::Partition512(); to partition
/// Sort512kv::SmallSort16V(); to sort a small array
/// (should be less than 16 AVX512 vectors)
///
/// To compile such flags can be used to enable avx 512 and openmp:
/// - KNL
/// Gcc : -mavx512f -mavx512pf -mavx512er -mavx512cd -fopenmp
/// Intel : -xCOMMON-AVX512 -xMIC-AVX512 -qopenmp
/// - SKL
/// Gcc : -mavx512f -mavx512cd -mavx512vl -mavx512bw -mavx512dq -fopenmp
/// Intel : -xCOMMON-AVX512 -xCORE-AVX512 -qopenmp
///
/// Or use "-march=native -mtune=native" if you are already on the right platform ("native can be replaced by "knl" or "skylake")
//////////////////////////////////////////////////////////
#ifndef SORT512KV_HPP
#define SORT512KV_HPP

#include <immintrin.h>
#include <climits>
#include <cfloat>
#include <algorithm>

#if defined(_OPENMP)
#include <omp.h>
#endif

namespace Sort512kv {

///////////////////////////////////////////////////////////
/// AVX Sort functions
///////////////////////////////////////////////////////////

/// Int

inline void CoreSmallSort(__m512i& input, __m512i& values){
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(12, 13, 14, 15, 8, 9, 10, 11,
                                              4, 5, 6, 7, 0, 1, 2, 3);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(8, 9, 10, 11, 12, 13, 14, 15,
                                              0, 1, 2, 3, 4, 5, 6, 7);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input  = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
}

inline void CoreSmallSort(int* __restrict__ ptr1, int* __restrict__ ptrVal){
    __m512i v = _mm512_loadu_si512(ptr1);
    __m512i v_val = _mm512_loadu_si512(ptrVal);
    CoreSmallSort(v, v_val);
    _mm512_storeu_si512(ptr1, v);
    _mm512_storeu_si512(ptrVal, v_val);
}



inline void CoreExchangeSort2V(__m512i& input, __m512i& input2,
                                 __m512i& input_val, __m512i& input2_val){
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i tmp_input = _mm512_min_epi32(input2, permNeigh);
        __m512i tmp_input2 = _mm512_max_epi32(input2, permNeigh);

        __m512i input_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input_val);
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, permNeigh, _MM_CMPINT_EQ ),
                                       input_val_perm);
        input2_val = _mm512_mask_mov_epi32(input_val_perm, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
         __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
         __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);

         input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                        input_val);
         input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                        input2_val);

         input = tmp_input;
         input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
         __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
         __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);

         input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                        input_val);
         input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                        input2_val);

         input = tmp_input;
         input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
         __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
         __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);

         input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                        input_val);
         input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                        input2_val);

         input = tmp_input;
         input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
         __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
         __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);

         input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                        input_val);
         input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                        input2_val);

         input = tmp_input;
         input2 = tmp_input2;
    }
}



inline void CoreSmallSort2(__m512i& input, __m512i& input2,
                                 __m512i& input_val, __m512i& input2_val){
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(12, 13, 14, 15, 8, 9, 10, 11,
                                              4, 5, 6, 7, 0, 1, 2, 3);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(8, 9, 10, 11, 12, 13, 14, 15,
                                              0, 1, 2, 3, 4, 5, 6, 7);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    CoreExchangeSort2V(input,input2,input_val,input2_val);
}


inline void CoreSmallSort2(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr1+16);
    __m512i input1_val = _mm512_loadu_si512(values);
    __m512i input2_val = _mm512_loadu_si512(values+16);
    CoreSmallSort2(input1, input2, input1_val, input2_val);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr1+16, input2);
    _mm512_storeu_si512(values, input1_val);
    _mm512_storeu_si512(values+16, input2_val);
}


inline void CoreSmallSort3(__m512i& input, __m512i& input2, __m512i& input3,
                                 __m512i& input_val, __m512i& input2_val, __m512i& input3_val){
    CoreSmallSort2(input, input2, input_val, input2_val);
    CoreSmallSort(input3, input3_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i tmp_input3 = _mm512_max_epi32(input2, permNeigh);
        __m512i tmp_input2 = _mm512_min_epi32(input2, permNeigh);

        __m512i input3_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input3_val);
        input3_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input3, permNeigh, _MM_CMPINT_EQ ),
                                       input3_val_perm);
        input2_val = _mm512_mask_mov_epi32(input3_val_perm, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input3 = tmp_input3;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);

        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
    }
}


inline void CoreSmallSort3(int* __restrict__ ptr1, int* __restrict__ values){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr1+16);
    __m512i input3 = _mm512_loadu_si512(ptr1+32);
    __m512i input1_val = _mm512_loadu_si512(values);
    __m512i input2_val = _mm512_loadu_si512(values+16);
    __m512i input3_val = _mm512_loadu_si512(values+32);
    CoreSmallSort3(input1, input2, input3,
                         input1_val, input2_val, input3_val);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr1+16, input2);
    _mm512_storeu_si512(ptr1+32, input3);
    _mm512_storeu_si512(values, input1_val);
    _mm512_storeu_si512(values+16, input2_val);
    _mm512_storeu_si512(values+32, input3_val);
}



inline void CoreSmallSort4(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                                 __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val){
    CoreSmallSort2(input, input2, input_val, input2_val);
    CoreSmallSort2(input3, input4, input3_val, input4_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);

        __m512i tmp_input4 = _mm512_max_epi32(input, permNeigh4);
        __m512i tmp_input = _mm512_min_epi32(input, permNeigh4);

        __m512i tmp_input3 = _mm512_max_epi32(input2, permNeigh3);
        __m512i tmp_input2 = _mm512_min_epi32(input2, permNeigh3);


        __m512i input4_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input4_val);
        input4_val = _mm512_mask_mov_epi32(input_val, _mm512_cmp_epi32_mask(tmp_input4, permNeigh4, _MM_CMPINT_EQ ),
                                       input4_val_perm);
        input_val = _mm512_mask_mov_epi32(input4_val_perm, _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);

        __m512i input3_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input3_val);
        input3_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input3, permNeigh3, _MM_CMPINT_EQ ),
                                       input3_val_perm);
        input2_val = _mm512_mask_mov_epi32(input3_val_perm, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);

        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);

        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
}


inline void CoreSmallSort4(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr1+16);
    __m512i input3 = _mm512_loadu_si512(ptr1+32);
    __m512i input4 = _mm512_loadu_si512(ptr1+48);
    __m512i input1_val = _mm512_loadu_si512(values);
    __m512i input2_val = _mm512_loadu_si512(values+16);
    __m512i input3_val = _mm512_loadu_si512(values+32);
    __m512i input4_val = _mm512_loadu_si512(values+48);
    CoreSmallSort4(input1, input2, input3, input4,
                         input1_val, input2_val, input3_val, input4_val);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr1+16, input2);
    _mm512_storeu_si512(ptr1+32, input3);
    _mm512_storeu_si512(ptr1+48, input4);
    _mm512_storeu_si512(values, input1_val);
    _mm512_storeu_si512(values+16, input2_val);
    _mm512_storeu_si512(values+32, input3_val);
    _mm512_storeu_si512(values+48, input4_val);
}


inline void CoreSmallSort5(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4, __m512i& input5,
                                 __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val, __m512i& input5_val){
    CoreSmallSort4(input, input2, input3, input4,
                         input_val, input2_val, input3_val, input4_val);
    CoreSmallSort(input5, input5_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);

        __m512i tmp_input5 = _mm512_max_epi32(input4, permNeigh5);
        __m512i tmp_input4 = _mm512_min_epi32(input4, permNeigh5);

        __m512i input5_val_copy = _mm512_permutexvar_epi32(idxNoNeigh, input5_val);
        input5_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input5, permNeigh5, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input4_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input5 = tmp_input5;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);

        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);

        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);

        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);

        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
    }
}


inline void CoreSmallSort5(int* __restrict__ ptr1, int* __restrict__ values){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input1_val = _mm512_loadu_si512(values);
    __m512i input2_val = _mm512_loadu_si512(values+1*16);
    __m512i input3_val = _mm512_loadu_si512(values+2*16);
    __m512i input4_val = _mm512_loadu_si512(values+3*16);
    __m512i input5_val = _mm512_loadu_si512(values+4*16);
    CoreSmallSort5(input1, input2, input3, input4, input5,
                    input1_val, input2_val, input3_val, input4_val, input5_val);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr1+1*16, input2);
    _mm512_storeu_si512(ptr1+2*16, input3);
    _mm512_storeu_si512(ptr1+3*16, input4);
    _mm512_storeu_si512(ptr1+4*16, input5);
    _mm512_storeu_si512(values, input1_val);
    _mm512_storeu_si512(values+1*16, input2_val);
    _mm512_storeu_si512(values+2*16, input3_val);
    _mm512_storeu_si512(values+3*16, input4_val);
    _mm512_storeu_si512(values+4*16, input5_val);
}



inline void CoreSmallSort6(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6,
                                 __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                                             __m512i& input5_val, __m512i& input6_val){
    CoreSmallSort4(input, input2, input3, input4,
                         input_val, input2_val, input3_val, input4_val);
    CoreSmallSort2(input5, input6, input5_val, input6_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);

        __m512i tmp_input5 = _mm512_max_epi32(input4, permNeigh5);
        __m512i tmp_input4 = _mm512_min_epi32(input4, permNeigh5);

        __m512i tmp_input6 = _mm512_max_epi32(input3, permNeigh6);
        __m512i tmp_input3 = _mm512_min_epi32(input3, permNeigh6);


        __m512i input5_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input5_val);
        input5_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input5, permNeigh5, _MM_CMPINT_EQ ),
                                       input5_val_perm);
        input4_val = _mm512_mask_mov_epi32(input5_val_perm, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        __m512i input6_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input6_val);
        input6_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input6, permNeigh6, _MM_CMPINT_EQ ),
                                       input6_val_perm);
        input3_val = _mm512_mask_mov_epi32(input6_val_perm, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input5 = tmp_input5;
        input4 = tmp_input4;

        input6 = tmp_input6;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input6_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
    }
}


inline void CoreSmallSort6(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    CoreSmallSort6(input0,input1,input2,input3,input4,input5,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
}



inline void CoreSmallSort7(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7,
                                 __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                __m512i& input5_val, __m512i& input6_val, __m512i& input7_val){
    CoreSmallSort4(input, input2, input3, input4,
                         input_val, input2_val, input3_val, input4_val);
    CoreSmallSort3(input5, input6, input7,
                         input5_val, input6_val, input7_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);

        __m512i tmp_input5 = _mm512_max_epi32(input4, permNeigh5);
        __m512i tmp_input4 = _mm512_min_epi32(input4, permNeigh5);

        __m512i tmp_input6 = _mm512_max_epi32(input3, permNeigh6);
        __m512i tmp_input3 = _mm512_min_epi32(input3, permNeigh6);

        __m512i tmp_input7 = _mm512_max_epi32(input2, permNeigh7);
        __m512i tmp_input2 = _mm512_min_epi32(input2, permNeigh7);


        __m512i input5_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input5_val);
        input5_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input5, permNeigh5, _MM_CMPINT_EQ ),
                                       input5_val_perm);
        input4_val = _mm512_mask_mov_epi32(input5_val_perm, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        __m512i input6_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input6_val);
        input6_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input6, permNeigh6, _MM_CMPINT_EQ ),
                                       input6_val_perm);
        input3_val = _mm512_mask_mov_epi32(input6_val_perm, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        __m512i input7_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input7_val);
        input7_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input7, permNeigh7, _MM_CMPINT_EQ ),
                                       input7_val_perm);
        input2_val = _mm512_mask_mov_epi32(input7_val_perm, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);


        input5 = tmp_input5;
        input4 = tmp_input4;

        input6 = tmp_input6;
        input3 = tmp_input3;

        input7 = tmp_input7;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input7, inputCopy);
        __m512i tmp_input7 = _mm512_max_epi32(input7, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input7_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        input5 = tmp_input5;
        input7 = tmp_input7;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input6_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xFF00, permNeighMax7);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xF0F0, permNeighMax7);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xCCCC, permNeighMax7);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xAAAA, permNeighMax7);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
    }
}


inline void CoreSmallSort7(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    CoreSmallSort7(input0,input1,input2,input3,input4,input5,input6,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
}



inline void CoreSmallSort8(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                                 __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                 __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val){
    CoreSmallSort4(input, input2, input3, input4,
                         input_val, input2_val, input3_val, input4_val);
    CoreSmallSort4(input5, input6, input7, input8,
                         input5_val, input6_val, input7_val, input8_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);

        __m512i tmp_input5 = _mm512_max_epi32(input4, permNeigh5);
        __m512i tmp_input4 = _mm512_min_epi32(input4, permNeigh5);

        __m512i tmp_input6 = _mm512_max_epi32(input3, permNeigh6);
        __m512i tmp_input3 = _mm512_min_epi32(input3, permNeigh6);

        __m512i tmp_input7 = _mm512_max_epi32(input2, permNeigh7);
        __m512i tmp_input2 = _mm512_min_epi32(input2, permNeigh7);

        __m512i tmp_input8 = _mm512_max_epi32(input, permNeigh8);
        __m512i tmp_input = _mm512_min_epi32(input, permNeigh8);


        __m512i input5_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input5_val);
        input5_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input5, permNeigh5, _MM_CMPINT_EQ ),
                                       input5_val_perm);
        input4_val = _mm512_mask_mov_epi32(input5_val_perm, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        __m512i input6_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input6_val);
        input6_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input6, permNeigh6, _MM_CMPINT_EQ ),
                                       input6_val_perm);
        input3_val = _mm512_mask_mov_epi32(input6_val_perm, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        __m512i input7_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input7_val);
        input7_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input7, permNeigh7, _MM_CMPINT_EQ ),
                                       input7_val_perm);
        input2_val = _mm512_mask_mov_epi32(input7_val_perm, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        __m512i input8_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input8_val);
        input8_val = _mm512_mask_mov_epi32(input_val, _mm512_cmp_epi32_mask(tmp_input8, permNeigh8, _MM_CMPINT_EQ ),
                                       input8_val_perm);
        input_val = _mm512_mask_mov_epi32(input8_val_perm, _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);


        input5 = tmp_input5;
        input4 = tmp_input4;

        input6 = tmp_input6;
        input3 = tmp_input3;

        input7 = tmp_input7;
        input2 = tmp_input2;

        input8 = tmp_input8;
        input = tmp_input;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input7, inputCopy);
        __m512i tmp_input7 = _mm512_max_epi32(input7, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input7_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        input5 = tmp_input5;
        input7 = tmp_input7;
    }
    {
        __m512i inputCopy = input6;
        __m512i tmp_input6 = _mm512_min_epi32(input8, inputCopy);
        __m512i tmp_input8 = _mm512_max_epi32(input8, inputCopy);
        __m512i input6_val_copy = input6_val;
        input6_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input6, inputCopy, _MM_CMPINT_EQ ),
                                       input6_val_copy);
        input8_val = _mm512_mask_mov_epi32(input6_val_copy, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        input6 = tmp_input6;
        input8 = tmp_input8;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input6_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i inputCopy = input7;
        __m512i tmp_input7 = _mm512_min_epi32(input8, inputCopy);
        __m512i tmp_input8 = _mm512_max_epi32(input8, inputCopy);
        __m512i input7_val_copy = input7_val;
        input7_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input7, inputCopy, _MM_CMPINT_EQ ),
                                       input7_val_copy);
        input8_val = _mm512_mask_mov_epi32(input7_val_copy, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        input7 = tmp_input7;
        input8 = tmp_input8;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMin8 = _mm512_min_epi32(permNeigh8, input8);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i permNeighMax8 = _mm512_max_epi32(permNeigh8, input8);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xFF00, permNeighMax7);
        __m512i tmp_input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xFF00, permNeighMax8);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);
        input8_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input8_val), _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
        input8_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
        input8 = tmp_input8;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMin8 = _mm512_min_epi32(permNeigh8, input8);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i permNeighMax8 = _mm512_max_epi32(permNeigh8, input8);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xF0F0, permNeighMax7);
        __m512i tmp_input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xF0F0, permNeighMax8);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);
        input8_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input8_val), _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
        input8_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
        input8 = tmp_input8;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMin8 = _mm512_min_epi32(permNeigh8, input8);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i permNeighMax8 = _mm512_max_epi32(permNeigh8, input8);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xCCCC, permNeighMax7);
        __m512i tmp_input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xCCCC, permNeighMax8);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);
        input8_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input8_val), _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
        input8_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
        input8 = tmp_input8;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMin8 = _mm512_min_epi32(permNeigh8, input8);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i permNeighMax8 = _mm512_max_epi32(permNeigh8, input8);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xAAAA, permNeighMax7);
        __m512i tmp_input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xAAAA, permNeighMax8);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);
        input8_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input8_val), _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
        input8_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
        input8 = tmp_input8;
    }
}

inline void CoreSmallSort8(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    CoreSmallSort8(input0,input1,input2,input3,input4,input5,input6,input7,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
}



inline void CoreSmallEnd1(__m512i& input, __m512i& values){
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);

        values = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, values), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       values);

        input = tmp_input;
    }
}

inline void CoreSmallEnd2(__m512i& input, __m512i& input2,
                                   __m512i& input_val, __m512i& input2_val){
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
}

inline void CoreSmallEnd3(__m512i& input, __m512i& input2, __m512i& input3,
                                   __m512i& input_val, __m512i& input2_val, __m512i& input3_val){
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
    }
}

inline void CoreSmallEnd4(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                                   __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val){
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
    }
}

inline void CoreSmallEnd5(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5,
                                   __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                   __m512i& input5_val){
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input5, inputCopy);
        __m512i tmp_input5 = _mm512_max_epi32(input5, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input5_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input = tmp_input;
        input5 = tmp_input5;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
    }
}

inline void CoreSmallEnd6(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6,
                                   __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                   __m512i& input5_val, __m512i& input6_val){
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input5, inputCopy);
        __m512i tmp_input5 = _mm512_max_epi32(input5, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input5_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input = tmp_input;
        input5 = tmp_input5;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input6_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input2 = tmp_input2;
        input6 = tmp_input6;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input6_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
    }
}



inline void CoreSmallEnd7(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7,
                                   __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                   __m512i& input5_val, __m512i& input6_val, __m512i& input7_val){
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input5, inputCopy);
        __m512i tmp_input5 = _mm512_max_epi32(input5, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input5_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input = tmp_input;
        input5 = tmp_input5;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input6_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input2 = tmp_input2;
        input6 = tmp_input6;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input7, inputCopy);
        __m512i tmp_input7 = _mm512_max_epi32(input7, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input7_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        input3 = tmp_input3;
        input7 = tmp_input7;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input7, inputCopy);
        __m512i tmp_input7 = _mm512_max_epi32(input7, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input7_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        input5 = tmp_input5;
        input7 = tmp_input7;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input7, inputCopy);
        __m512i tmp_input7 = _mm512_max_epi32(input7, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input7_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        input5 = tmp_input5;
        input7 = tmp_input7;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input6_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xFF00, permNeighMax7);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xF0F0, permNeighMax7);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xCCCC, permNeighMax7);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xAAAA, permNeighMax7);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
    }
}



inline void CoreSmallEnd8(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                                   __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                   __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val){
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input5, inputCopy);
        __m512i tmp_input5 = _mm512_max_epi32(input5, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input5_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input = tmp_input;
        input5 = tmp_input5;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input6_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input2 = tmp_input2;
        input6 = tmp_input6;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input7, inputCopy);
        __m512i tmp_input7 = _mm512_max_epi32(input7, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input7_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        input3 = tmp_input3;
        input7 = tmp_input7;
    }
    {
        __m512i inputCopy = input4;
        __m512i tmp_input4 = _mm512_min_epi32(input8, inputCopy);
        __m512i tmp_input8 = _mm512_max_epi32(input8, inputCopy);
        __m512i input4_val_copy = input4_val;
        input4_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input4, inputCopy, _MM_CMPINT_EQ ),
                                       input4_val_copy);
        input8_val = _mm512_mask_mov_epi32(input4_val_copy, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        input4 = tmp_input4;
        input8 = tmp_input8;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input7, inputCopy);
        __m512i tmp_input7 = _mm512_max_epi32(input7, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input7_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        input5 = tmp_input5;
        input7 = tmp_input7;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input3, inputCopy);
        __m512i tmp_input3 = _mm512_max_epi32(input3, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input3_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input = tmp_input;
        input3 = tmp_input3;
    }
    {
        __m512i inputCopy = input2;
        __m512i tmp_input2 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input2_val_copy = input2_val;
        input2_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input2, inputCopy, _MM_CMPINT_EQ ),
                                       input2_val_copy);
        input4_val = _mm512_mask_mov_epi32(input2_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input2 = tmp_input2;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input;
        __m512i tmp_input = _mm512_min_epi32(input2, inputCopy);
        __m512i tmp_input2 = _mm512_max_epi32(input2, inputCopy);
        __m512i input_val_copy = input_val;
        input_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input, inputCopy, _MM_CMPINT_EQ ),
                                       input_val_copy);
        input2_val = _mm512_mask_mov_epi32(input_val_copy, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input = tmp_input;
        input2 = tmp_input2;
    }
    {
        __m512i inputCopy = input3;
        __m512i tmp_input3 = _mm512_min_epi32(input4, inputCopy);
        __m512i tmp_input4 = _mm512_max_epi32(input4, inputCopy);
        __m512i input3_val_copy = input3_val;
        input3_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input3, inputCopy, _MM_CMPINT_EQ ),
                                       input3_val_copy);
        input4_val = _mm512_mask_mov_epi32(input3_val_copy, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input3 = tmp_input3;
        input4 = tmp_input4;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input7, inputCopy);
        __m512i tmp_input7 = _mm512_max_epi32(input7, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input7_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        input5 = tmp_input5;
        input7 = tmp_input7;
    }
    {
        __m512i inputCopy = input6;
        __m512i tmp_input6 = _mm512_min_epi32(input8, inputCopy);
        __m512i tmp_input8 = _mm512_max_epi32(input8, inputCopy);
        __m512i input6_val_copy = input6_val;
        input6_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input6, inputCopy, _MM_CMPINT_EQ ),
                                       input6_val_copy);
        input8_val = _mm512_mask_mov_epi32(input6_val_copy, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        input6 = tmp_input6;
        input8 = tmp_input8;
    }
    {
        __m512i inputCopy = input5;
        __m512i tmp_input5 = _mm512_min_epi32(input6, inputCopy);
        __m512i tmp_input6 = _mm512_max_epi32(input6, inputCopy);
        __m512i input5_val_copy = input5_val;
        input5_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input5, inputCopy, _MM_CMPINT_EQ ),
                                       input5_val_copy);
        input6_val = _mm512_mask_mov_epi32(input5_val_copy, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        input5 = tmp_input5;
        input6 = tmp_input6;
    }
    {
        __m512i inputCopy = input7;
        __m512i tmp_input7 = _mm512_min_epi32(input8, inputCopy);
        __m512i tmp_input8 = _mm512_max_epi32(input8, inputCopy);
        __m512i input7_val_copy = input7_val;
        input7_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input7, inputCopy, _MM_CMPINT_EQ ),
                                       input7_val_copy);
        input8_val = _mm512_mask_mov_epi32(input7_val_copy, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        input7 = tmp_input7;
        input8 = tmp_input8;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMin8 = _mm512_min_epi32(permNeigh8, input8);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i permNeighMax8 = _mm512_max_epi32(permNeigh8, input8);__m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xFF00, permNeighMax7);
        __m512i tmp_input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xFF00, permNeighMax8);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);
        input8_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input8_val), _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
        input8_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
        input8 = tmp_input8;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMin8 = _mm512_min_epi32(permNeigh8, input8);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i permNeighMax8 = _mm512_max_epi32(permNeigh8, input8);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xF0F0, permNeighMax7);
        __m512i tmp_input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xF0F0, permNeighMax8);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);
        input8_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input8_val), _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
        input8_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
        input8 = tmp_input8;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMin8 = _mm512_min_epi32(permNeigh8, input8);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i permNeighMax8 = _mm512_max_epi32(permNeigh8, input8);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xCCCC, permNeighMax7);
        __m512i tmp_input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xCCCC, permNeighMax8);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);
        input8_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input8_val), _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
        input8_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
        input8 = tmp_input8;
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMin3 = _mm512_min_epi32(permNeigh3, input3);
        __m512i permNeighMin4 = _mm512_min_epi32(permNeigh4, input4);
        __m512i permNeighMin5 = _mm512_min_epi32(permNeigh5, input5);
        __m512i permNeighMin6 = _mm512_min_epi32(permNeigh6, input6);
        __m512i permNeighMin7 = _mm512_min_epi32(permNeigh7, input7);
        __m512i permNeighMin8 = _mm512_min_epi32(permNeigh8, input8);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        __m512i permNeighMax3 = _mm512_max_epi32(permNeigh3, input3);
        __m512i permNeighMax4 = _mm512_max_epi32(permNeigh4, input4);
        __m512i permNeighMax5 = _mm512_max_epi32(permNeigh5, input5);
        __m512i permNeighMax6 = _mm512_max_epi32(permNeigh6, input6);
        __m512i permNeighMax7 = _mm512_max_epi32(permNeigh7, input7);
        __m512i permNeighMax8 = _mm512_max_epi32(permNeigh8, input8);
        __m512i tmp_input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        __m512i tmp_input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        __m512i tmp_input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        __m512i tmp_input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        __m512i tmp_input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        __m512i tmp_input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
        __m512i tmp_input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xAAAA, permNeighMax7);
        __m512i tmp_input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xAAAA, permNeighMax8);

        input_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input_val), _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
        input_val);
        input2_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input2_val), _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
        input2_val);
        input3_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input3_val), _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
        input3_val);
        input4_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input4_val), _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
        input4_val);
        input5_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input5_val), _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
        input5_val);
        input6_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input6_val), _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
        input6_val);
        input7_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input7_val), _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
        input7_val);
        input8_val = _mm512_mask_mov_epi32(_mm512_permutexvar_epi32(idxNoNeigh, input8_val), _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
        input8_val);

        input = tmp_input;
        input2 = tmp_input2;
        input3 = tmp_input3;
        input4 = tmp_input4;
        input5 = tmp_input5;
        input6 = tmp_input6;
        input7 = tmp_input7;
        input8 = tmp_input8;
    }
}


inline void CoreSmallSort9(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9,
                                 __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                 __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val,
                                 __m512i& input9_val){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8,
                         input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallSort(input9, input9_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);

        __m512i tmp_input9 = _mm512_max_epi32(input8, permNeigh9);
        __m512i tmp_input8 = _mm512_min_epi32(input8, permNeigh9);


        __m512i input9_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input9_val);
        input9_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input9, permNeigh9, _MM_CMPINT_EQ ),
                                       input9_val_perm);
        input8_val = _mm512_mask_mov_epi32(input9_val_perm, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        input9 = tmp_input9;
        input8 = tmp_input8;
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8,
                      input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallEnd1(input9, input9_val);
}


inline void CoreSmallSort9(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input8 = _mm512_loadu_si512(ptr1+8*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    __m512i input8_val = _mm512_loadu_si512(values+8*16);
    CoreSmallSort9(input0,input1,input2,input3,input4,input5,input6,input7,input8,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val,input8_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(ptr1+8*16, input8);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
    _mm512_storeu_si512(values+8*16, input8_val);
}


inline void CoreSmallSort10(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9, __m512i& input10,
                             __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                             __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val,
                             __m512i& input9_val, __m512i& input10_val){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8,
                         input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallSort2(input9, input10, input9_val, input10_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);

        __m512i tmp_input9 = _mm512_max_epi32(input8, permNeigh9);
        __m512i tmp_input8 = _mm512_min_epi32(input8, permNeigh9);

        __m512i tmp_input10 = _mm512_max_epi32(input7, permNeigh10);
        __m512i tmp_input7 = _mm512_min_epi32(input7, permNeigh10);


        __m512i input9_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input9_val);
        input9_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input9, permNeigh9, _MM_CMPINT_EQ ),
                                       input9_val_perm);
        input8_val = _mm512_mask_mov_epi32(input9_val_perm, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        __m512i input10_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input10_val);
        input10_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input10, permNeigh10, _MM_CMPINT_EQ ),
                                       input10_val_perm);
        input7_val = _mm512_mask_mov_epi32(input10_val_perm, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);


        input9 = tmp_input9;
        input8 = tmp_input8;

        input10 = tmp_input10;
        input7 = tmp_input7;
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8,
                           input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallEnd2(input9, input10, input9_val, input10_val);
}


inline void CoreSmallSort10(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input8 = _mm512_loadu_si512(ptr1+8*16);
    __m512i input9 = _mm512_loadu_si512(ptr1+9*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    __m512i input8_val = _mm512_loadu_si512(values+8*16);
    __m512i input9_val = _mm512_loadu_si512(values+9*16);
    CoreSmallSort10(input0,input1,input2,input3,input4,input5,input6,input7,input8,input9,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val,input8_val,input9_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(ptr1+8*16, input8);
    _mm512_storeu_si512(ptr1+9*16, input9);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
    _mm512_storeu_si512(values+8*16, input8_val);
    _mm512_storeu_si512(values+9*16, input9_val);
}



inline void CoreSmallSort11(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9, __m512i& input10, __m512i& input11,
                                  __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                  __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val,
                                  __m512i& input9_val, __m512i& input10_val, __m512i& input11_val){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8,
                    input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallSort3(input9, input10, input11,
                    input9_val, input10_val, input11_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);

        __m512i tmp_input9 = _mm512_max_epi32(input8, permNeigh9);
        __m512i tmp_input8 = _mm512_min_epi32(input8, permNeigh9);

        __m512i tmp_input10 = _mm512_max_epi32(input7, permNeigh10);
        __m512i tmp_input7 = _mm512_min_epi32(input7, permNeigh10);

        __m512i tmp_input11 = _mm512_max_epi32(input6, permNeigh11);
        __m512i tmp_input6 = _mm512_min_epi32(input6, permNeigh11);


        __m512i input9_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input9_val);
        input9_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input9, permNeigh9, _MM_CMPINT_EQ ),
                                       input9_val_perm);
        input8_val = _mm512_mask_mov_epi32(input9_val_perm, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        __m512i input10_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input10_val);
        input10_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input10, permNeigh10, _MM_CMPINT_EQ ),
                                       input10_val_perm);
        input7_val = _mm512_mask_mov_epi32(input10_val_perm, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        __m512i input11_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input11_val);
        input11_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input11, permNeigh11, _MM_CMPINT_EQ ),
                                       input11_val_perm);
        input6_val = _mm512_mask_mov_epi32(input11_val_perm, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);


        input9 = tmp_input9;
        input8 = tmp_input8;

        input10 = tmp_input10;
        input7 = tmp_input7;

        input11 = tmp_input11;
        input6 = tmp_input6;
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8,
                      input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallEnd3(input9, input10, input11,
                      input9_val, input10_val, input11_val);
}

inline void CoreSmallSort11(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input8 = _mm512_loadu_si512(ptr1+8*16);
    __m512i input9 = _mm512_loadu_si512(ptr1+9*16);
    __m512i input10 = _mm512_loadu_si512(ptr1+10*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    __m512i input8_val = _mm512_loadu_si512(values+8*16);
    __m512i input9_val = _mm512_loadu_si512(values+9*16);
    __m512i input10_val = _mm512_loadu_si512(values+10*16);
    CoreSmallSort11(input0,input1,input2,input3,input4,input5,input6,input7,input8,input9,input10,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val,input8_val,input9_val,input10_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(ptr1+8*16, input8);
    _mm512_storeu_si512(ptr1+9*16, input9);
    _mm512_storeu_si512(ptr1+10*16, input10);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
    _mm512_storeu_si512(values+8*16, input8_val);
    _mm512_storeu_si512(values+9*16, input9_val);
    _mm512_storeu_si512(values+10*16, input10_val);
}

inline void CoreSmallSort12(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                                  __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                  __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val,
                                  __m512i& input9_val, __m512i& input10_val, __m512i& input11_val ,
                                  __m512i& input12_val){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8,
                    input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallSort4(input9, input10, input11, input12,
                    input9_val, input10_val, input11_val, input12_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);
        __m512i permNeigh12 = _mm512_permutexvar_epi32(idxNoNeigh, input12);

        __m512i tmp_input9 = _mm512_max_epi32(input8, permNeigh9);
        __m512i tmp_input8 = _mm512_min_epi32(input8, permNeigh9);

        __m512i tmp_input10 = _mm512_max_epi32(input7, permNeigh10);
        __m512i tmp_input7 = _mm512_min_epi32(input7, permNeigh10);

        __m512i tmp_input11 = _mm512_max_epi32(input6, permNeigh11);
        __m512i tmp_input6 = _mm512_min_epi32(input6, permNeigh11);

        __m512i tmp_input12 = _mm512_max_epi32(input5, permNeigh12);
        __m512i tmp_input5 = _mm512_min_epi32(input5, permNeigh12);

        __m512i input9_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input9_val);
        input9_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input9, permNeigh9, _MM_CMPINT_EQ ),
                                       input9_val_perm);
        input8_val = _mm512_mask_mov_epi32(input9_val_perm, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        __m512i input10_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input10_val);
        input10_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input10, permNeigh10, _MM_CMPINT_EQ ),
                                       input10_val_perm);
        input7_val = _mm512_mask_mov_epi32(input10_val_perm, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        __m512i input11_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input11_val);
        input11_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input11, permNeigh11, _MM_CMPINT_EQ ),
                                       input11_val_perm);
        input6_val = _mm512_mask_mov_epi32(input11_val_perm, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        __m512i input12_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input12_val);
        input12_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input12, permNeigh12, _MM_CMPINT_EQ ),
                                       input12_val_perm);
        input5_val = _mm512_mask_mov_epi32(input12_val_perm, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        input9 = tmp_input9;
        input8 = tmp_input8;

        input10 = tmp_input10;
        input7 = tmp_input7;

        input11 = tmp_input11;
        input6 = tmp_input6;

        input12 = tmp_input12;
        input5 = tmp_input5;
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8,
                      input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallEnd4(input9, input10, input11, input12,
                      input9_val, input10_val, input11_val, input12_val);
}


inline void CoreSmallSort12(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input8 = _mm512_loadu_si512(ptr1+8*16);
    __m512i input9 = _mm512_loadu_si512(ptr1+9*16);
    __m512i input10 = _mm512_loadu_si512(ptr1+10*16);
    __m512i input11 = _mm512_loadu_si512(ptr1+11*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    __m512i input8_val = _mm512_loadu_si512(values+8*16);
    __m512i input9_val = _mm512_loadu_si512(values+9*16);
    __m512i input10_val = _mm512_loadu_si512(values+10*16);
    __m512i input11_val = _mm512_loadu_si512(values+11*16);
    CoreSmallSort12(input0,input1,input2,input3,input4,input5,input6,input7,input8,input9,input10,input11,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val,input8_val,input9_val,input10_val,input11_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(ptr1+8*16, input8);
    _mm512_storeu_si512(ptr1+9*16, input9);
    _mm512_storeu_si512(ptr1+10*16, input10);
    _mm512_storeu_si512(ptr1+11*16, input11);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
    _mm512_storeu_si512(values+8*16, input8_val);
    _mm512_storeu_si512(values+9*16, input9_val);
    _mm512_storeu_si512(values+10*16, input10_val);
    _mm512_storeu_si512(values+11*16, input11_val);
}



inline void CoreSmallSort13(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                            __m512i& input13,
                                  __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                  __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val,
                                  __m512i& input9_val, __m512i& input10_val, __m512i& input11_val ,
                                  __m512i& input12_val, __m512i& input13_val){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8,
                    input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallSort5(input9, input10, input11, input12, input13,
                    input9_val, input10_val, input11_val, input12_val, input13_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);
        __m512i permNeigh12 = _mm512_permutexvar_epi32(idxNoNeigh, input12);
        __m512i permNeigh13 = _mm512_permutexvar_epi32(idxNoNeigh, input13);

        __m512i tmp_input9 = _mm512_max_epi32(input8, permNeigh9);
        __m512i tmp_input8 = _mm512_min_epi32(input8, permNeigh9);

        __m512i tmp_input10 = _mm512_max_epi32(input7, permNeigh10);
        __m512i tmp_input7 = _mm512_min_epi32(input7, permNeigh10);

        __m512i tmp_input11 = _mm512_max_epi32(input6, permNeigh11);
        __m512i tmp_input6 = _mm512_min_epi32(input6, permNeigh11);

        __m512i tmp_input12 = _mm512_max_epi32(input5, permNeigh12);
        __m512i tmp_input5 = _mm512_min_epi32(input5, permNeigh12);

        __m512i tmp_input13 = _mm512_max_epi32(input4, permNeigh13);
        __m512i tmp_input4 = _mm512_min_epi32(input4, permNeigh13);

        __m512i input9_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input9_val);
        input9_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input9, permNeigh9, _MM_CMPINT_EQ ),
                                       input9_val_perm);
        input8_val = _mm512_mask_mov_epi32(input9_val_perm, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        __m512i input10_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input10_val);
        input10_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input10, permNeigh10, _MM_CMPINT_EQ ),
                                       input10_val_perm);
        input7_val = _mm512_mask_mov_epi32(input10_val_perm, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        __m512i input11_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input11_val);
        input11_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input11, permNeigh11, _MM_CMPINT_EQ ),
                                       input11_val_perm);
        input6_val = _mm512_mask_mov_epi32(input11_val_perm, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        __m512i input12_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input12_val);
        input12_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input12, permNeigh12, _MM_CMPINT_EQ ),
                                       input12_val_perm);
        input5_val = _mm512_mask_mov_epi32(input12_val_perm, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        __m512i input13_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input13_val);
        input13_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input13, permNeigh13, _MM_CMPINT_EQ ),
                                       input13_val_perm);
        input4_val = _mm512_mask_mov_epi32(input13_val_perm, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        input9 = tmp_input9;
        input8 = tmp_input8;

        input10 = tmp_input10;
        input7 = tmp_input7;

        input11 = tmp_input11;
        input6 = tmp_input6;

        input12 = tmp_input12;
        input5 = tmp_input5;

        input13 = tmp_input13;
        input4 = tmp_input4;
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8,
                      input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallEnd5(input9, input10, input11, input12, input13,
                      input9_val, input10_val, input11_val, input12_val, input13_val);
}


inline void CoreSmallSort13(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input8 = _mm512_loadu_si512(ptr1+8*16);
    __m512i input9 = _mm512_loadu_si512(ptr1+9*16);
    __m512i input10 = _mm512_loadu_si512(ptr1+10*16);
    __m512i input11 = _mm512_loadu_si512(ptr1+11*16);
    __m512i input12 = _mm512_loadu_si512(ptr1+12*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    __m512i input8_val = _mm512_loadu_si512(values+8*16);
    __m512i input9_val = _mm512_loadu_si512(values+9*16);
    __m512i input10_val = _mm512_loadu_si512(values+10*16);
    __m512i input11_val = _mm512_loadu_si512(values+11*16);
    __m512i input12_val = _mm512_loadu_si512(values+12*16);
    CoreSmallSort13(input0,input1,input2,input3,input4,input5,input6,input7,input8,input9,input10,input11,input12,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val,input8_val,input9_val,input10_val,input11_val,input12_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(ptr1+8*16, input8);
    _mm512_storeu_si512(ptr1+9*16, input9);
    _mm512_storeu_si512(ptr1+10*16, input10);
    _mm512_storeu_si512(ptr1+11*16, input11);
    _mm512_storeu_si512(ptr1+12*16, input12);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
    _mm512_storeu_si512(values+8*16, input8_val);
    _mm512_storeu_si512(values+9*16, input9_val);
    _mm512_storeu_si512(values+10*16, input10_val);
    _mm512_storeu_si512(values+11*16, input11_val);
    _mm512_storeu_si512(values+12*16, input12_val);
}



inline void CoreSmallSort14(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                            __m512i& input13, __m512i& input14,
                                  __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                  __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val,
                                  __m512i& input9_val, __m512i& input10_val, __m512i& input11_val ,
                                  __m512i& input12_val, __m512i& input13_val, __m512i& input14_val){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8,
                    input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallSort6(input9, input10, input11, input12, input13, input14,
                    input9_val, input10_val, input11_val, input12_val, input13_val, input14_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);
        __m512i permNeigh12 = _mm512_permutexvar_epi32(idxNoNeigh, input12);
        __m512i permNeigh13 = _mm512_permutexvar_epi32(idxNoNeigh, input13);
        __m512i permNeigh14 = _mm512_permutexvar_epi32(idxNoNeigh, input14);

        __m512i tmp_input9 = _mm512_max_epi32(input8, permNeigh9);
        __m512i tmp_input8 = _mm512_min_epi32(input8, permNeigh9);

        __m512i tmp_input10 = _mm512_max_epi32(input7, permNeigh10);
        __m512i tmp_input7 = _mm512_min_epi32(input7, permNeigh10);

        __m512i tmp_input11 = _mm512_max_epi32(input6, permNeigh11);
        __m512i tmp_input6 = _mm512_min_epi32(input6, permNeigh11);

        __m512i tmp_input12 = _mm512_max_epi32(input5, permNeigh12);
        __m512i tmp_input5 = _mm512_min_epi32(input5, permNeigh12);

        __m512i tmp_input13 = _mm512_max_epi32(input4, permNeigh13);
        __m512i tmp_input4 = _mm512_min_epi32(input4, permNeigh13);

        __m512i tmp_input14 = _mm512_max_epi32(input3, permNeigh14);
        __m512i tmp_input3 = _mm512_min_epi32(input3, permNeigh14);

        __m512i input9_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input9_val);
        input9_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input9, permNeigh9, _MM_CMPINT_EQ ),
                                       input9_val_perm);
        input8_val = _mm512_mask_mov_epi32(input9_val_perm, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        __m512i input10_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input10_val);
        input10_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input10, permNeigh10, _MM_CMPINT_EQ ),
                                       input10_val_perm);
        input7_val = _mm512_mask_mov_epi32(input10_val_perm, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        __m512i input11_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input11_val);
        input11_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input11, permNeigh11, _MM_CMPINT_EQ ),
                                       input11_val_perm);
        input6_val = _mm512_mask_mov_epi32(input11_val_perm, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        __m512i input12_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input12_val);
        input12_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input12, permNeigh12, _MM_CMPINT_EQ ),
                                       input12_val_perm);
        input5_val = _mm512_mask_mov_epi32(input12_val_perm, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        __m512i input13_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input13_val);
        input13_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input13, permNeigh13, _MM_CMPINT_EQ ),
                                       input13_val_perm);
        input4_val = _mm512_mask_mov_epi32(input13_val_perm, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        __m512i input14_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input14_val);
        input14_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input14, permNeigh14, _MM_CMPINT_EQ ),
                                       input14_val_perm);
        input3_val = _mm512_mask_mov_epi32(input14_val_perm, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        input9 = tmp_input9;
        input8 = tmp_input8;

        input10 = tmp_input10;
        input7 = tmp_input7;

        input11 = tmp_input11;
        input6 = tmp_input6;

        input12 = tmp_input12;
        input5 = tmp_input5;

        input13 = tmp_input13;
        input4 = tmp_input4;

        input14 = tmp_input14;
        input3 = tmp_input3;
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8,
                      input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallEnd6(input9, input10, input11, input12, input13, input14,
                      input9_val, input10_val, input11_val, input12_val, input13_val, input14_val);
}


inline void CoreSmallSort14(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input8 = _mm512_loadu_si512(ptr1+8*16);
    __m512i input9 = _mm512_loadu_si512(ptr1+9*16);
    __m512i input10 = _mm512_loadu_si512(ptr1+10*16);
    __m512i input11 = _mm512_loadu_si512(ptr1+11*16);
    __m512i input12 = _mm512_loadu_si512(ptr1+12*16);
    __m512i input13 = _mm512_loadu_si512(ptr1+13*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    __m512i input8_val = _mm512_loadu_si512(values+8*16);
    __m512i input9_val = _mm512_loadu_si512(values+9*16);
    __m512i input10_val = _mm512_loadu_si512(values+10*16);
    __m512i input11_val = _mm512_loadu_si512(values+11*16);
    __m512i input12_val = _mm512_loadu_si512(values+12*16);
    __m512i input13_val = _mm512_loadu_si512(values+13*16);
    CoreSmallSort14(input0,input1,input2,input3,input4,input5,input6,input7,input8,input9,input10,input11,input12,input13,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val,input8_val,input9_val,input10_val,input11_val,input12_val,input13_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(ptr1+8*16, input8);
    _mm512_storeu_si512(ptr1+9*16, input9);
    _mm512_storeu_si512(ptr1+10*16, input10);
    _mm512_storeu_si512(ptr1+11*16, input11);
    _mm512_storeu_si512(ptr1+12*16, input12);
    _mm512_storeu_si512(ptr1+13*16, input13);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
    _mm512_storeu_si512(values+8*16, input8_val);
    _mm512_storeu_si512(values+9*16, input9_val);
    _mm512_storeu_si512(values+10*16, input10_val);
    _mm512_storeu_si512(values+11*16, input11_val);
    _mm512_storeu_si512(values+12*16, input12_val);
    _mm512_storeu_si512(values+13*16, input13_val);
}


inline void CoreSmallSort15(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                            __m512i& input13, __m512i& input14, __m512i& input15,
                                  __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                  __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val,
                                  __m512i& input9_val, __m512i& input10_val, __m512i& input11_val ,
                                  __m512i& input12_val, __m512i& input13_val, __m512i& input14_val,
                                  __m512i& input15_val){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8,
                    input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallSort7(input9, input10, input11, input12, input13, input14, input15,
                    input9_val, input10_val, input11_val, input12_val, input13_val, input14_val, input15_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);
        __m512i permNeigh12 = _mm512_permutexvar_epi32(idxNoNeigh, input12);
        __m512i permNeigh13 = _mm512_permutexvar_epi32(idxNoNeigh, input13);
        __m512i permNeigh14 = _mm512_permutexvar_epi32(idxNoNeigh, input14);
        __m512i permNeigh15 = _mm512_permutexvar_epi32(idxNoNeigh, input15);

        __m512i tmp_input9 = _mm512_max_epi32(input8, permNeigh9);
        __m512i tmp_input8 = _mm512_min_epi32(input8, permNeigh9);

        __m512i tmp_input10 = _mm512_max_epi32(input7, permNeigh10);
        __m512i tmp_input7 = _mm512_min_epi32(input7, permNeigh10);

        __m512i tmp_input11 = _mm512_max_epi32(input6, permNeigh11);
        __m512i tmp_input6 = _mm512_min_epi32(input6, permNeigh11);

        __m512i tmp_input12 = _mm512_max_epi32(input5, permNeigh12);
        __m512i tmp_input5 = _mm512_min_epi32(input5, permNeigh12);

        __m512i tmp_input13 = _mm512_max_epi32(input4, permNeigh13);
        __m512i tmp_input4 = _mm512_min_epi32(input4, permNeigh13);

        __m512i tmp_input14 = _mm512_max_epi32(input3, permNeigh14);
        __m512i tmp_input3 = _mm512_min_epi32(input3, permNeigh14);

        __m512i tmp_input15 = _mm512_max_epi32(input2, permNeigh15);
        __m512i tmp_input2 = _mm512_min_epi32(input2, permNeigh15);

        __m512i input9_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input9_val);
        input9_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input9, permNeigh9, _MM_CMPINT_EQ ),
                                       input9_val_perm);
        input8_val = _mm512_mask_mov_epi32(input9_val_perm, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        __m512i input10_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input10_val);
        input10_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input10, permNeigh10, _MM_CMPINT_EQ ),
                                       input10_val_perm);
        input7_val = _mm512_mask_mov_epi32(input10_val_perm, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        __m512i input11_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input11_val);
        input11_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input11, permNeigh11, _MM_CMPINT_EQ ),
                                       input11_val_perm);
        input6_val = _mm512_mask_mov_epi32(input11_val_perm, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        __m512i input12_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input12_val);
        input12_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input12, permNeigh12, _MM_CMPINT_EQ ),
                                       input12_val_perm);
        input5_val = _mm512_mask_mov_epi32(input12_val_perm, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        __m512i input13_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input13_val);
        input13_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input13, permNeigh13, _MM_CMPINT_EQ ),
                                       input13_val_perm);
        input4_val = _mm512_mask_mov_epi32(input13_val_perm, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        __m512i input14_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input14_val);
        input14_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input14, permNeigh14, _MM_CMPINT_EQ ),
                                       input14_val_perm);
        input3_val = _mm512_mask_mov_epi32(input14_val_perm, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        __m512i input15_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input15_val);
        input15_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input15, permNeigh15, _MM_CMPINT_EQ ),
                                       input15_val_perm);
        input2_val = _mm512_mask_mov_epi32(input15_val_perm, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        input9 = tmp_input9;
        input8 = tmp_input8;

        input10 = tmp_input10;
        input7 = tmp_input7;

        input11 = tmp_input11;
        input6 = tmp_input6;

        input12 = tmp_input12;
        input5 = tmp_input5;

        input13 = tmp_input13;
        input4 = tmp_input4;

        input14 = tmp_input14;
        input3 = tmp_input3;

        input15 = tmp_input15;
        input2 = tmp_input2;
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8,
                      input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallEnd7(input9, input10, input11, input12, input13, input14, input15,
                      input9_val, input10_val, input11_val, input12_val, input13_val, input14_val, input15_val);
}


inline void CoreSmallSort15(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input8 = _mm512_loadu_si512(ptr1+8*16);
    __m512i input9 = _mm512_loadu_si512(ptr1+9*16);
    __m512i input10 = _mm512_loadu_si512(ptr1+10*16);
    __m512i input11 = _mm512_loadu_si512(ptr1+11*16);
    __m512i input12 = _mm512_loadu_si512(ptr1+12*16);
    __m512i input13 = _mm512_loadu_si512(ptr1+13*16);
    __m512i input14 = _mm512_loadu_si512(ptr1+14*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    __m512i input8_val = _mm512_loadu_si512(values+8*16);
    __m512i input9_val = _mm512_loadu_si512(values+9*16);
    __m512i input10_val = _mm512_loadu_si512(values+10*16);
    __m512i input11_val = _mm512_loadu_si512(values+11*16);
    __m512i input12_val = _mm512_loadu_si512(values+12*16);
    __m512i input13_val = _mm512_loadu_si512(values+13*16);
    __m512i input14_val = _mm512_loadu_si512(values+14*16);
    CoreSmallSort15(input0,input1,input2,input3,input4,input5,input6,input7,input8,input9,input10,input11,input12,input13,input14,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val,input8_val,input9_val,input10_val,input11_val,input12_val,input13_val,input14_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(ptr1+8*16, input8);
    _mm512_storeu_si512(ptr1+9*16, input9);
    _mm512_storeu_si512(ptr1+10*16, input10);
    _mm512_storeu_si512(ptr1+11*16, input11);
    _mm512_storeu_si512(ptr1+12*16, input12);
    _mm512_storeu_si512(ptr1+13*16, input13);
    _mm512_storeu_si512(ptr1+14*16, input14);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
    _mm512_storeu_si512(values+8*16, input8_val);
    _mm512_storeu_si512(values+9*16, input9_val);
    _mm512_storeu_si512(values+10*16, input10_val);
    _mm512_storeu_si512(values+11*16, input11_val);
    _mm512_storeu_si512(values+12*16, input12_val);
    _mm512_storeu_si512(values+13*16, input13_val);
    _mm512_storeu_si512(values+14*16, input14_val);
}



inline void CoreSmallSort16(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                            __m512i& input13, __m512i& input14, __m512i& input15, __m512i& input16,
                                  __m512i& input_val, __m512i& input2_val, __m512i& input3_val, __m512i& input4_val,
                                  __m512i& input5_val, __m512i& input6_val, __m512i& input7_val, __m512i& input8_val,
                                  __m512i& input9_val, __m512i& input10_val, __m512i& input11_val ,
                                  __m512i& input12_val, __m512i& input13_val, __m512i& input14_val,
                                  __m512i& input15_val,__m512i& input16_val){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8,
                    input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallSort8(input9, input10, input11, input12, input13, input14, input15, input16,
                    input9_val, input10_val, input11_val, input12_val, input13_val, input14_val, input15_val, input16_val);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);
        __m512i permNeigh12 = _mm512_permutexvar_epi32(idxNoNeigh, input12);
        __m512i permNeigh13 = _mm512_permutexvar_epi32(idxNoNeigh, input13);
        __m512i permNeigh14 = _mm512_permutexvar_epi32(idxNoNeigh, input14);
        __m512i permNeigh15 = _mm512_permutexvar_epi32(idxNoNeigh, input15);
        __m512i permNeigh16 = _mm512_permutexvar_epi32(idxNoNeigh, input16);

        __m512i tmp_input9 = _mm512_max_epi32(input8, permNeigh9);
        __m512i tmp_input8 = _mm512_min_epi32(input8, permNeigh9);

        __m512i tmp_input10 = _mm512_max_epi32(input7, permNeigh10);
        __m512i tmp_input7 = _mm512_min_epi32(input7, permNeigh10);

        __m512i tmp_input11 = _mm512_max_epi32(input6, permNeigh11);
        __m512i tmp_input6 = _mm512_min_epi32(input6, permNeigh11);

        __m512i tmp_input12 = _mm512_max_epi32(input5, permNeigh12);
        __m512i tmp_input5 = _mm512_min_epi32(input5, permNeigh12);

        __m512i tmp_input13 = _mm512_max_epi32(input4, permNeigh13);
        __m512i tmp_input4 = _mm512_min_epi32(input4, permNeigh13);

        __m512i tmp_input14 = _mm512_max_epi32(input3, permNeigh14);
        __m512i tmp_input3 = _mm512_min_epi32(input3, permNeigh14);

        __m512i tmp_input15 = _mm512_max_epi32(input2, permNeigh15);
        __m512i tmp_input2 = _mm512_min_epi32(input2, permNeigh15);

        __m512i tmp_input16 = _mm512_max_epi32(input, permNeigh16);
        __m512i tmp_input = _mm512_min_epi32(input, permNeigh16);


        __m512i input9_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input9_val);
        input9_val = _mm512_mask_mov_epi32(input8_val, _mm512_cmp_epi32_mask(tmp_input9, permNeigh9, _MM_CMPINT_EQ ),
                                       input9_val_perm);
        input8_val = _mm512_mask_mov_epi32(input9_val_perm, _mm512_cmp_epi32_mask(tmp_input8, input8, _MM_CMPINT_EQ ),
                                       input8_val);

        __m512i input10_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input10_val);
        input10_val = _mm512_mask_mov_epi32(input7_val, _mm512_cmp_epi32_mask(tmp_input10, permNeigh10, _MM_CMPINT_EQ ),
                                       input10_val_perm);
        input7_val = _mm512_mask_mov_epi32(input10_val_perm, _mm512_cmp_epi32_mask(tmp_input7, input7, _MM_CMPINT_EQ ),
                                       input7_val);

        __m512i input11_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input11_val);
        input11_val = _mm512_mask_mov_epi32(input6_val, _mm512_cmp_epi32_mask(tmp_input11, permNeigh11, _MM_CMPINT_EQ ),
                                       input11_val_perm);
        input6_val = _mm512_mask_mov_epi32(input11_val_perm, _mm512_cmp_epi32_mask(tmp_input6, input6, _MM_CMPINT_EQ ),
                                       input6_val);

        __m512i input12_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input12_val);
        input12_val = _mm512_mask_mov_epi32(input5_val, _mm512_cmp_epi32_mask(tmp_input12, permNeigh12, _MM_CMPINT_EQ ),
                                       input12_val_perm);
        input5_val = _mm512_mask_mov_epi32(input12_val_perm, _mm512_cmp_epi32_mask(tmp_input5, input5, _MM_CMPINT_EQ ),
                                       input5_val);

        __m512i input13_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input13_val);
        input13_val = _mm512_mask_mov_epi32(input4_val, _mm512_cmp_epi32_mask(tmp_input13, permNeigh13, _MM_CMPINT_EQ ),
                                       input13_val_perm);
        input4_val = _mm512_mask_mov_epi32(input13_val_perm, _mm512_cmp_epi32_mask(tmp_input4, input4, _MM_CMPINT_EQ ),
                                       input4_val);

        __m512i input14_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input14_val);
        input14_val = _mm512_mask_mov_epi32(input3_val, _mm512_cmp_epi32_mask(tmp_input14, permNeigh14, _MM_CMPINT_EQ ),
                                       input14_val_perm);
        input3_val = _mm512_mask_mov_epi32(input14_val_perm, _mm512_cmp_epi32_mask(tmp_input3, input3, _MM_CMPINT_EQ ),
                                       input3_val);

        __m512i input15_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input15_val);
        input15_val = _mm512_mask_mov_epi32(input2_val, _mm512_cmp_epi32_mask(tmp_input15, permNeigh15, _MM_CMPINT_EQ ),
                                       input15_val_perm);
        input2_val = _mm512_mask_mov_epi32(input15_val_perm, _mm512_cmp_epi32_mask(tmp_input2, input2, _MM_CMPINT_EQ ),
                                       input2_val);

        __m512i input16_val_perm = _mm512_permutexvar_epi32(idxNoNeigh, input16_val);
        input16_val = _mm512_mask_mov_epi32(input_val, _mm512_cmp_epi32_mask(tmp_input16, permNeigh16, _MM_CMPINT_EQ ),
                                       input16_val_perm);
        input_val = _mm512_mask_mov_epi32(input16_val_perm, _mm512_cmp_epi32_mask(tmp_input, input, _MM_CMPINT_EQ ),
                                       input_val);

        input9 = tmp_input9;
        input8 = tmp_input8;

        input10 = tmp_input10;
        input7 = tmp_input7;

        input11 = tmp_input11;
        input6 = tmp_input6;

        input12 = tmp_input12;
        input5 = tmp_input5;

        input13 = tmp_input13;
        input4 = tmp_input4;

        input14 = tmp_input14;
        input3 = tmp_input3;

        input15 = tmp_input15;
        input2 = tmp_input2;

        input16 = tmp_input16;
        input = tmp_input;
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8,
                      input_val, input2_val, input3_val, input4_val, input5_val, input6_val, input7_val, input8_val);
    CoreSmallEnd8(input9, input10, input11, input12, input13, input14, input15, input16,
                      input9_val, input10_val, input11_val, input12_val, input13_val, input14_val, input15_val, input16_val);
}


inline void CoreSmallSort16(int* __restrict__ ptr1, int* __restrict__ values ){
    __m512i input0 = _mm512_loadu_si512(ptr1+0*16);
    __m512i input1 = _mm512_loadu_si512(ptr1+1*16);
    __m512i input2 = _mm512_loadu_si512(ptr1+2*16);
    __m512i input3 = _mm512_loadu_si512(ptr1+3*16);
    __m512i input4 = _mm512_loadu_si512(ptr1+4*16);
    __m512i input5 = _mm512_loadu_si512(ptr1+5*16);
    __m512i input6 = _mm512_loadu_si512(ptr1+6*16);
    __m512i input7 = _mm512_loadu_si512(ptr1+7*16);
    __m512i input8 = _mm512_loadu_si512(ptr1+8*16);
    __m512i input9 = _mm512_loadu_si512(ptr1+9*16);
    __m512i input10 = _mm512_loadu_si512(ptr1+10*16);
    __m512i input11 = _mm512_loadu_si512(ptr1+11*16);
    __m512i input12 = _mm512_loadu_si512(ptr1+12*16);
    __m512i input13 = _mm512_loadu_si512(ptr1+13*16);
    __m512i input14 = _mm512_loadu_si512(ptr1+14*16);
    __m512i input15 = _mm512_loadu_si512(ptr1+15*16);
    __m512i input0_val = _mm512_loadu_si512(values+0*16);
    __m512i input1_val = _mm512_loadu_si512(values+1*16);
    __m512i input2_val = _mm512_loadu_si512(values+2*16);
    __m512i input3_val = _mm512_loadu_si512(values+3*16);
    __m512i input4_val = _mm512_loadu_si512(values+4*16);
    __m512i input5_val = _mm512_loadu_si512(values+5*16);
    __m512i input6_val = _mm512_loadu_si512(values+6*16);
    __m512i input7_val = _mm512_loadu_si512(values+7*16);
    __m512i input8_val = _mm512_loadu_si512(values+8*16);
    __m512i input9_val = _mm512_loadu_si512(values+9*16);
    __m512i input10_val = _mm512_loadu_si512(values+10*16);
    __m512i input11_val = _mm512_loadu_si512(values+11*16);
    __m512i input12_val = _mm512_loadu_si512(values+12*16);
    __m512i input13_val = _mm512_loadu_si512(values+13*16);
    __m512i input14_val = _mm512_loadu_si512(values+14*16);
    __m512i input15_val = _mm512_loadu_si512(values+15*16);
    CoreSmallSort16(input0,input1,input2,input3,input4,input5,input6,input7,input8,input9,input10,input11,input12,input13,input14,input15,
        input0_val,input1_val,input2_val,input3_val,input4_val,input5_val,input6_val,input7_val,input8_val,input9_val,input10_val,input11_val,input12_val,input13_val,input14_val,input15_val);
    _mm512_storeu_si512(ptr1+0*16, input0);
    _mm512_storeu_si512(ptr1+1*16, input1);
    _mm512_storeu_si512(ptr1+2*16, input2);
    _mm512_storeu_si512(ptr1+3*16, input3);
    _mm512_storeu_si512(ptr1+4*16, input4);
    _mm512_storeu_si512(ptr1+5*16, input5);
    _mm512_storeu_si512(ptr1+6*16, input6);
    _mm512_storeu_si512(ptr1+7*16, input7);
    _mm512_storeu_si512(ptr1+8*16, input8);
    _mm512_storeu_si512(ptr1+9*16, input9);
    _mm512_storeu_si512(ptr1+10*16, input10);
    _mm512_storeu_si512(ptr1+11*16, input11);
    _mm512_storeu_si512(ptr1+12*16, input12);
    _mm512_storeu_si512(ptr1+13*16, input13);
    _mm512_storeu_si512(ptr1+14*16, input14);
    _mm512_storeu_si512(ptr1+15*16, input15);
    _mm512_storeu_si512(values+0*16, input0_val);
    _mm512_storeu_si512(values+1*16, input1_val);
    _mm512_storeu_si512(values+2*16, input2_val);
    _mm512_storeu_si512(values+3*16, input3_val);
    _mm512_storeu_si512(values+4*16, input4_val);
    _mm512_storeu_si512(values+5*16, input5_val);
    _mm512_storeu_si512(values+6*16, input6_val);
    _mm512_storeu_si512(values+7*16, input7_val);
    _mm512_storeu_si512(values+8*16, input8_val);
    _mm512_storeu_si512(values+9*16, input9_val);
    _mm512_storeu_si512(values+10*16, input10_val);
    _mm512_storeu_si512(values+11*16, input11_val);
    _mm512_storeu_si512(values+12*16, input12_val);
    _mm512_storeu_si512(values+13*16, input13_val);
    _mm512_storeu_si512(values+14*16, input14_val);
    _mm512_storeu_si512(values+15*16, input15_val);
}



inline void SmallSort16V(int* __restrict__ ptr, int* __restrict__ values, const size_t length){
    // length is limited to 4 times size of a vec
    const int nbValuesInVec = 16;
    const int nbVecs = (length+nbValuesInVec-1)/nbValuesInVec;
    const int rest = nbVecs*nbValuesInVec-length;
    const int lastVecSize = nbValuesInVec-rest;
    switch(nbVecs){
    case 1:
    {
        __m512i v1 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr),
                        _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v1_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values),
                        _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort(v1, v1_val);
        _mm512_mask_compressstoreu_epi32(ptr, 0xFFFF>>rest, v1);
        _mm512_mask_compressstoreu_epi32(values, 0xFFFF>>rest, v1_val);
    }
        break;
    case 2:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+16),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v2_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+16),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort2(v1,v2,
                             v1_val,v2_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_mask_compressstoreu_epi32(ptr+16, 0xFFFF>>rest, v2);
        _mm512_mask_compressstoreu_epi32(values+16, 0xFFFF>>rest, v2_val);
    }
        break;
    case 3:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+32),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v3_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+32),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort3(v1,v2,v3,
                             v1_val,v2_val,v3_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_mask_compressstoreu_epi32(ptr+32, 0xFFFF>>rest, v3);
        _mm512_mask_compressstoreu_epi32(values+32, 0xFFFF>>rest, v3_val);
    }
        break;
    case 4:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+48),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v4_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+48),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort4(v1,v2,v3,v4,
                             v1_val,v2_val,v3_val,v4_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_mask_compressstoreu_epi32(ptr+48, 0xFFFF>>rest, v4);
        _mm512_mask_compressstoreu_epi32(values+48, 0xFFFF>>rest, v4_val);
    }
        break;
    case 5:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+64),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v5_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+64),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort5(v1,v2,v3,v4,v5,
                             v1_val,v2_val,v3_val,v4_val,v5_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_mask_compressstoreu_epi32(ptr+64, 0xFFFF>>rest, v5);
        _mm512_mask_compressstoreu_epi32(values+64, 0xFFFF>>rest, v5_val);
    }
        break;
    case 6:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+80),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v6_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+80),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort6(v1,v2,v3,v4,v5,v6,
                             v1_val,v2_val,v3_val,v4_val,v5_val,v6_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_mask_compressstoreu_epi32(ptr+80, 0xFFFF>>rest, v6);
        _mm512_mask_compressstoreu_epi32(values+80, 0xFFFF>>rest, v6_val);
    }
        break;
    case 7:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+96),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v7_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+96),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort7(v1,v2,v3,v4,v5,v6,v7,
                             v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_mask_compressstoreu_epi32(ptr+96, 0xFFFF>>rest, v7);
        _mm512_mask_compressstoreu_epi32(values+96, 0xFFFF>>rest, v7_val);
    }
        break;
    case 8:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+112),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v8_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+112),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort8(v1,v2,v3,v4,v5,v6,v7,v8,
                             v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_mask_compressstoreu_epi32(ptr+112, 0xFFFF>>rest, v8);
        _mm512_mask_compressstoreu_epi32(values+112, 0xFFFF>>rest, v8_val);
    }
        break;
    case 9:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v8_val = _mm512_loadu_si512(values+112);
        __m512i v9 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+128),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v9_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+128),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort9(v1,v2,v3,v4,v5,v6,v7,v8,v9,
                             v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val,v9_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(values+112, v8_val);
        _mm512_mask_compressstoreu_epi32(ptr+128, 0xFFFF>>rest, v9);
        _mm512_mask_compressstoreu_epi32(values+128, 0xFFFF>>rest, v9_val);
    }
        break;
    case 10:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v8_val = _mm512_loadu_si512(values+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v9_val = _mm512_loadu_si512(values+128);
        __m512i v10 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+144),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v10_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+144),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort10(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,
                              v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val,v9_val,v10_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(values+112, v8_val);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(values+128, v9_val);
        _mm512_mask_compressstoreu_epi32(ptr+144, 0xFFFF>>rest, v10);
        _mm512_mask_compressstoreu_epi32(values+144, 0xFFFF>>rest, v10_val);
    }
        break;
    case 11:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v8_val = _mm512_loadu_si512(values+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v9_val = _mm512_loadu_si512(values+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v10_val = _mm512_loadu_si512(values+144);
        __m512i v11 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+160),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v11_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+160),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort11(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,
                              v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val,v9_val,v10_val,v11_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(values+112, v8_val);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(values+128, v9_val);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(values+144, v10_val);
        _mm512_mask_compressstoreu_epi32(ptr+160, 0xFFFF>>rest, v11);
        _mm512_mask_compressstoreu_epi32(values+160, 0xFFFF>>rest, v11_val);
    }
        break;
    case 12:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v8_val = _mm512_loadu_si512(values+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v9_val = _mm512_loadu_si512(values+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v10_val = _mm512_loadu_si512(values+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v11_val = _mm512_loadu_si512(values+160);
        __m512i v12 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+176),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v12_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+176),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort12(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,
                              v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val,v9_val,v10_val,v11_val,v12_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(values+112, v8_val);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(values+128, v9_val);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(values+144, v10_val);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(values+160, v11_val);
        _mm512_mask_compressstoreu_epi32(ptr+176, 0xFFFF>>rest, v12);
        _mm512_mask_compressstoreu_epi32(values+176, 0xFFFF>>rest, v12_val);
    }
        break;
    case 13:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v8_val = _mm512_loadu_si512(values+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v9_val = _mm512_loadu_si512(values+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v10_val = _mm512_loadu_si512(values+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v11_val = _mm512_loadu_si512(values+160);
        __m512i v12 = _mm512_loadu_si512(ptr+176);
        __m512i v12_val = _mm512_loadu_si512(values+176);
        __m512i v13 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+192),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v13_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+192),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort13(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,
                              v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val,v9_val,v10_val,v11_val,v12_val,v13_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(values+112, v8_val);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(values+128, v9_val);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(values+144, v10_val);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(values+160, v11_val);
        _mm512_storeu_si512(ptr+176, v12);
        _mm512_storeu_si512(values+176, v12_val);
        _mm512_mask_compressstoreu_epi32(ptr+192, 0xFFFF>>rest, v13);
        _mm512_mask_compressstoreu_epi32(values+192, 0xFFFF>>rest, v13_val);
    }
        break;
    case 14:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v8_val = _mm512_loadu_si512(values+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v9_val = _mm512_loadu_si512(values+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v10_val = _mm512_loadu_si512(values+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v11_val = _mm512_loadu_si512(values+160);
        __m512i v12 = _mm512_loadu_si512(ptr+176);
        __m512i v12_val = _mm512_loadu_si512(values+176);
        __m512i v13 = _mm512_loadu_si512(ptr+192);
        __m512i v13_val = _mm512_loadu_si512(values+192);
        __m512i v14 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+208),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v14_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+208),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort14(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,
                              v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val,v9_val,v10_val,v11_val,v12_val,v13_val,v14_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(values+112, v8_val);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(values+128, v9_val);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(values+144, v10_val);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(values+160, v11_val);
        _mm512_storeu_si512(ptr+176, v12);
        _mm512_storeu_si512(values+176, v12_val);
        _mm512_storeu_si512(ptr+192, v13);
        _mm512_storeu_si512(values+192, v13_val);
        _mm512_mask_compressstoreu_epi32(ptr+208, 0xFFFF>>rest, v14);
        _mm512_mask_compressstoreu_epi32(values+208, 0xFFFF>>rest, v14_val);
    }
        break;
    case 15:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v8_val = _mm512_loadu_si512(values+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v9_val = _mm512_loadu_si512(values+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v10_val = _mm512_loadu_si512(values+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v11_val = _mm512_loadu_si512(values+160);
        __m512i v12 = _mm512_loadu_si512(ptr+176);
        __m512i v12_val = _mm512_loadu_si512(values+176);
        __m512i v13 = _mm512_loadu_si512(ptr+192);
        __m512i v13_val = _mm512_loadu_si512(values+192);
        __m512i v14 = _mm512_loadu_si512(ptr+208);
        __m512i v14_val = _mm512_loadu_si512(values+208);
        __m512i v15 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+224),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v15_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+224),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort15(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,
                              v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val,v9_val,v10_val,v11_val,v12_val,v13_val,v14_val,v15_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(values+112, v8_val);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(values+128, v9_val);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(values+144, v10_val);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(values+160, v11_val);
        _mm512_storeu_si512(ptr+176, v12);
        _mm512_storeu_si512(values+176, v12_val);
        _mm512_storeu_si512(ptr+192, v13);
        _mm512_storeu_si512(values+192, v13_val);
        _mm512_storeu_si512(ptr+208, v14);
        _mm512_storeu_si512(values+208, v14_val);
        _mm512_mask_compressstoreu_epi32(ptr+224, 0xFFFF>>rest, v15);
        _mm512_mask_compressstoreu_epi32(values+224, 0xFFFF>>rest, v15_val);
    }
        break;
    //case 16:
    default:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v1_val = _mm512_loadu_si512(values);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v2_val = _mm512_loadu_si512(values+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v3_val = _mm512_loadu_si512(values+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v4_val = _mm512_loadu_si512(values+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v5_val = _mm512_loadu_si512(values+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v6_val = _mm512_loadu_si512(values+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v7_val = _mm512_loadu_si512(values+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v8_val = _mm512_loadu_si512(values+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v9_val = _mm512_loadu_si512(values+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v10_val = _mm512_loadu_si512(values+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v11_val = _mm512_loadu_si512(values+160);
        __m512i v12 = _mm512_loadu_si512(ptr+176);
        __m512i v12_val = _mm512_loadu_si512(values+176);
        __m512i v13 = _mm512_loadu_si512(ptr+192);
        __m512i v13_val = _mm512_loadu_si512(values+192);
        __m512i v14 = _mm512_loadu_si512(ptr+208);
        __m512i v14_val = _mm512_loadu_si512(values+208);
        __m512i v15 = _mm512_loadu_si512(ptr+224);
        __m512i v15_val = _mm512_loadu_si512(values+224);
        __m512i v16 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+240),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        __m512i v16_val = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, values+240),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort16(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,v16,
                              v1_val,v2_val,v3_val,v4_val,v5_val,v6_val,v7_val,v8_val,v9_val,v10_val,v11_val,v12_val,v13_val,v14_val,v15_val,v16_val);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(values, v1_val);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(values+16, v2_val);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(values+32, v3_val);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(values+48, v4_val);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(values+64, v5_val);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(values+80, v6_val);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(values+96, v7_val);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(values+112, v8_val);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(values+128, v9_val);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(values+144, v10_val);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(values+160, v11_val);
        _mm512_storeu_si512(ptr+176, v12);
        _mm512_storeu_si512(values+176, v12_val);
        _mm512_storeu_si512(ptr+192, v13);
        _mm512_storeu_si512(values+192, v13_val);
        _mm512_storeu_si512(ptr+208, v14);
        _mm512_storeu_si512(values+208, v14_val);
        _mm512_storeu_si512(ptr+224, v15);
        _mm512_storeu_si512(values+224, v15_val);
        _mm512_mask_compressstoreu_epi32(ptr+240, 0xFFFF>>rest, v16);
        _mm512_mask_compressstoreu_epi32(values+240, 0xFFFF>>rest, v16_val);
    }
    }
}





////////////////////////////////////////////////////////////////////////////////
/// Partitions
////////////////////////////////////////////////////////////////////////////////

template <class SortType, class IndexType>
static inline IndexType CoreScalarPartition(SortType array[], SortType values[], IndexType left, IndexType right,
                                    const SortType pivot){

    for(; left <= right
         && array[left] <= pivot ; ++left){
    }

    for(IndexType idx = left ; idx <= right ; ++idx){
        if( array[idx] <= pivot ){
            std::swap(array[idx],array[left]);
            std::swap(values[idx],values[left]);
            left += 1;
        }
    }

    return left;
}


inline int popcount(__mmask16 mask){
    //    int res = int(mask);
    //    res = (0x5555 & res) + (0x5555 & (res >> 1));
    //    res = (res & 0x3333) + ((res>>2) & 0x3333);
    //    res = (res & 0x0F0F) + ((res>>4) & 0x0F0F);
    //    return (res & 0xFF) + ((res>>8) & 0xFF);
#ifdef __INTEL_COMPILER
    return _mm_countbits_32(mask);
#else
    return __builtin_popcount(mask);
#endif
}


/* a sequential qs */
template <class IndexType>
static inline IndexType Partition512(int array[], int values[], IndexType left, IndexType right,
                                         const int pivot){
    const IndexType S = 16;//(512/8)/sizeof(int);

    if(right-left+1 < 2*S){
        return CoreScalarPartition<int,IndexType>(array, values, left, right, pivot);
    }

    __m512i pivotvec = _mm512_set1_epi32(pivot);

    __m512i left_val = _mm512_loadu_si512(&array[left]);
    __m512i left_val_val = _mm512_loadu_si512(&values[left]);
    IndexType left_w = left;
    left += S;

    IndexType right_w = right+1;
    right -= S-1;
    __m512i right_val = _mm512_loadu_si512(&array[right]);
    __m512i right_val_val = _mm512_loadu_si512(&values[right]);

    while(left + S <= right){
        const IndexType free_left = left - left_w;
        const IndexType free_right = right_w - right;

        __m512i val;
        __m512i val_val;
        if( free_left <= free_right ){
            val = _mm512_loadu_si512(&array[left]);
            val_val = _mm512_loadu_si512(&values[left]);
            left += S;
        }
        else{
            right -= S;
            val = _mm512_loadu_si512(&array[right]);
            val_val = _mm512_loadu_si512(&values[right]);
        }

        __mmask16 mask = _mm512_cmp_epi32_mask(val, pivotvec, _MM_CMPINT_LE);

        const IndexType nb_low = popcount(mask); // count mask
        // intel _popcnt32 or _mm_countbits_32 or __builtin_popcount(mask)
        const IndexType nb_high = S-nb_low; // S-nb_low

        //if(mask){// if nb_low
            _mm512_mask_compressstoreu_epi32(&array[left_w],mask,val);
            _mm512_mask_compressstoreu_epi32(&values[left_w],mask,val_val);
            left_w += nb_low;
        //}
        //if(mask != 0xFFFF){// if nb_high
            right_w -= nb_high;
            _mm512_mask_compressstoreu_epi32(&array[right_w],~mask,val);
            _mm512_mask_compressstoreu_epi32(&values[right_w],~mask,val_val);
        //}
    }

    {
        const IndexType remaining = right - left;
        __m512i val = _mm512_loadu_si512(&array[left]);
        __m512i val_val = _mm512_loadu_si512(&values[left]);
        left = right;

        __mmask16 mask = _mm512_cmp_epi32_mask(val, pivotvec, _MM_CMPINT_LE);

        __mmask16 mask_low = mask & ~(0xFFFF << remaining);
        __mmask16 mask_high = (~mask) & ~(0xFFFF << remaining);

        const IndexType nb_low = popcount(mask_low); // count mask
        // intel _popcnt32 or _mm_countbits_32 or __builtin_popcount(mask)
        const IndexType nb_high = popcount(mask_high); // S-nb_low

        //if(mask_low){// if nb_low
            _mm512_mask_compressstoreu_epi32(&array[left_w],mask_low,val);
            _mm512_mask_compressstoreu_epi32(&values[left_w],mask_low,val_val);
            left_w += nb_low;
        //}
        //if(mask_high){// if nb_high
            right_w -= nb_high;
            _mm512_mask_compressstoreu_epi32(&array[right_w],mask_high,val);
            _mm512_mask_compressstoreu_epi32(&values[right_w],mask_high,val_val);
        //}
    }
    {
        __mmask16 mask = _mm512_cmp_epi32_mask(left_val, pivotvec, _MM_CMPINT_LE);

        const IndexType nb_low = popcount(mask); // count mask
        // intel _popcnt32 or _mm_countbits_32 or __builtin_popcount(mask)
        const IndexType nb_high = S-nb_low; // S-nb_low

        //if(mask){// if nb_low
            _mm512_mask_compressstoreu_epi32(&array[left_w],mask,left_val);
            _mm512_mask_compressstoreu_epi32(&values[left_w],mask,left_val_val);
            left_w += nb_low;
        //}
        //if(mask != 0xFFFF){// if nb_high
            right_w -= nb_high;
            _mm512_mask_compressstoreu_epi32(&array[right_w],~mask,left_val);
            _mm512_mask_compressstoreu_epi32(&values[right_w],~mask,left_val_val);
        //}
    }
    {
        __mmask16 mask = _mm512_cmp_epi32_mask(right_val, pivotvec, _MM_CMPINT_LE);

        const IndexType nb_low = popcount(mask); // count mask
        // intel _popcnt32 or _mm_countbits_32 or __builtin_popcount(mask)
        const IndexType nb_high = S-nb_low; // S-nb_low

        //if(mask){// if nb_low
            _mm512_mask_compressstoreu_epi32(&array[left_w],mask,right_val);
            _mm512_mask_compressstoreu_epi32(&values[left_w],mask,right_val_val);
            left_w += nb_low;
        //}
        //if(mask != 0xFFFF){// if nb_high
            right_w -= nb_high;
            _mm512_mask_compressstoreu_epi32(&array[right_w],~mask,right_val);
            _mm512_mask_compressstoreu_epi32(&values[right_w],~mask,right_val_val);
         //}
    }
    return left_w;
}



////////////////////////////////////////////////////////////////////////////////
/// Main functions
////////////////////////////////////////////////////////////////////////////////

template <class SortType, class IndexType = size_t>
static inline IndexType CoreSortGetPivot(const SortType array[], const IndexType left, const IndexType right){
    const IndexType middle = ((right-left)/2) + left;
    if(array[left] <= array[middle] && array[middle] <= array[right]){
        return middle;
    }
    else if(array[middle] <= array[left] && array[left] <= array[right]){
        return left;
    }
    else return right;
}


template <class SortType, class IndexType = size_t>
static inline IndexType CoreSortPivotPartition(SortType array[], SortType values[], const IndexType left, const IndexType right){
    if(right-left > 1){
        const IndexType pivotIdx = CoreSortGetPivot(array, left, right);
        std::swap(array[pivotIdx], array[right]);
        std::swap(values[pivotIdx], values[right]);
        const IndexType part = Partition512(array, values, left, right-1, array[right]);
        std::swap(array[part], array[right]);
        std::swap(values[part], values[right]);
        return part;
    }
    return left;
}

template <class SortType, class IndexType = size_t>
static inline IndexType CoreSortPartition(SortType array[], SortType values[],  const IndexType left, const IndexType right,
                                  const SortType pivot){
    return  Partition512(array, values, left, right, pivot);
}

template <class SortType, class IndexType = size_t>
static void CoreSort(SortType array[], SortType values[], const IndexType left, const IndexType right){
    static const int SortLimite = 16*64/sizeof(SortType);
    if(right-left < SortLimite){
        SmallSort16V(array+left, values+left, right-left+1);
    }
    else{
        const IndexType part = CoreSortPivotPartition<SortType,IndexType>(array, values, left, right);
        if(part+1 < right) CoreSort<SortType,IndexType>(array,values,part+1,right);
        if(part && left < part-1)  CoreSort<SortType,IndexType>(array,values,left,part - 1);
    }
}

template <class SortType, class IndexType = size_t>
static inline void Sort(SortType array[], SortType values[], const IndexType size){
    CoreSort<SortType,IndexType>(array, values, 0, size-1);
}


#if defined(_OPENMP)

template <class SortType, class IndexType = size_t>
static inline void CoreSortTaskPartition(SortType array[], SortType values[], const IndexType left, const IndexType right, const int deep){
    static const int SortLimite = 16*64/sizeof(SortType);
    if(right-left < SortLimite){
        SmallSort16V(array+left, values+left, right-left+1);
    }
    else{
        const IndexType part = CoreSortPivotPartition<SortType,IndexType>(array, values, left, right);
        if( deep ){
            // default(none) has been removed for clang compatibility
            if(part+1 < right){
                #pragma omp task default(shared) firstprivate(array, values, part, right, deep)
                CoreSortTaskPartition<SortType,IndexType>(array,values, part+1,right, deep - 1);
            }
            // not task needed, let the current thread compute it
            if(part && left < part-1)  CoreSortTaskPartition<SortType,IndexType>(array,values, left,part - 1, deep - 1);
        }
        else {
            if(part+1 < right) CoreSort<SortType,IndexType>(array,values, part+1,right);
            if(part && left < part-1)  CoreSort<SortType,IndexType>(array,values, left,part - 1);
        }
    }
}

template <class SortType, class IndexType = size_t>
static inline void SortOmpPartition(SortType array[], SortType values[], const IndexType size){
    // const int nbTasksRequiere = (omp_get_max_threads() * 5);
    // int deep = 0;
    // while( (1 << deep) < nbTasksRequiere ) deep += 1;
    int deep = 0;
    while( (IndexType(1) << deep) < size ) deep += 1;

#pragma omp parallel
    {
#pragma omp master
        {
            CoreSortTaskPartition<SortType,IndexType>(array, values, 0, size - 1 , deep);
        }
    }
}
#endif

}


#endif
