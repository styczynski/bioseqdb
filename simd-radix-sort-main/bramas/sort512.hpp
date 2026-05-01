//////////////////////////////////////////////////////////
/// Code to sort an array of integer or double
/// using avx 512 (targeting intel KNL/SKL).
/// By berenger.bramas@mpcdf.mpg.de 2017.
/// Licence is MIT.
/// Comes without any warranty.
///
///
/// Functions to call:
/// Sort512::Sort(); to sort an array
/// Sort512::SortOmp(); to sort in parallel
/// Sort512::Partition512(); to partition
/// Sort512::SmallSort16V(); to sort a small array
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
#ifndef SORT512_HPP
#define SORT512_HPP

#include <immintrin.h>
#include <climits>
#include <cfloat>
#include <algorithm>
#include <cassert>
#include <bit> // 01. Dec 2023 (Jonas Keller): for std::bit_cast below

#if defined(_OPENMP)
#include <omp.h>
#include "parallelInplace.hpp"
#endif

namespace Sort512 {

///////////////////////////////////////////////////////////
/// AVX Sort functions
///////////////////////////////////////////////////////////

/// Double

inline __m512d CoreSmallSort(__m512d input){
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(4, 5, 6, 7, 0, 1, 2, 3);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
    }

    return input;
}

inline void CoreSmallSort(double* __restrict__ ptr1){
    _mm512_storeu_pd(ptr1, CoreSmallSort(_mm512_loadu_pd(ptr1)));
}


inline void CoreExchangeSort2V(__m512d& input, __m512d& input2){
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        input = _mm512_min_pd(input2, permNeigh);
        input2 = _mm512_max_pd(input2, permNeigh);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
    }
}

inline void CoreSmallSort2(__m512d& input, __m512d& input2){
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(4, 5, 6, 7, 0, 1, 2, 3);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
    }
    CoreExchangeSort2V(input, input2);
}

inline void CoreSmallSort2(double* __restrict__ ptr1, double* __restrict__ ptr2 ){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    CoreSmallSort2(input1, input2);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
}


inline void CoreSmallSort3(__m512d& input, __m512d& input2, __m512d& input3 ){
    CoreSmallSort2(input, input2);
    input3 = CoreSmallSort(input3);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input3);
        input3 = _mm512_max_pd(input2, permNeigh);
        input2 = _mm512_min_pd(input2, permNeigh);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
    }
}

inline void CoreSmallSort3(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3  ){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    CoreSmallSort3(input1, input2, input3);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
}


inline void CoreSmallSort4(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4 ){
    CoreSmallSort2(input, input2);
    CoreSmallSort2(input3, input4);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);

        input4 = _mm512_max_pd(input, permNeigh4);
        input = _mm512_min_pd(input, permNeigh4);

        input3 = _mm512_max_pd(input2, permNeigh3);
        input2 = _mm512_min_pd(input2, permNeigh3);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
    }
}


inline void CoreSmallSort4(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3, double* __restrict__ ptr4  ){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    CoreSmallSort4(input1, input2, input3, input4);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
}


inline void CoreSmallSort5(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4, __m512d& input5 ){
    CoreSmallSort4(input, input2, input3, input4);
    input5 = CoreSmallSort(input5);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);

        input5 = _mm512_max_pd(input4, permNeigh5);
        input4 = _mm512_min_pd(input4, permNeigh5);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xF0, permNeighMax5);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xCC, permNeighMax5);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xAA, permNeighMax5);
    }
}


inline void CoreSmallSort5(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                            double* __restrict__ ptr4, double* __restrict__ ptr5 ){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    CoreSmallSort5(input1, input2, input3, input4, input5);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
}


inline void CoreSmallSort6(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4, __m512d& input5, __m512d& input6 ){
    CoreSmallSort4(input, input2, input3, input4);
    CoreSmallSort2(input5, input6);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);

        input5 = _mm512_max_pd(input4, permNeigh5);
        input6 = _mm512_max_pd(input3, permNeigh6);

        input4 = _mm512_min_pd(input4, permNeigh5);
        input3 = _mm512_min_pd(input3, permNeigh6);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xF0, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xF0, permNeighMax6);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xCC, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xCC, permNeighMax6);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xAA, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xAA, permNeighMax6);
    }
}


inline void CoreSmallSort6(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                            double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6 ){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    CoreSmallSort6(input1, input2, input3, input4, input5, input6);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
}


inline void CoreSmallSort7(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                            __m512d& input5, __m512d& input6, __m512d& input7 ){
    CoreSmallSort4(input, input2, input3, input4);
    CoreSmallSort3(input5, input6, input7);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);

        input5 = _mm512_max_pd(input4, permNeigh5);
        input6 = _mm512_max_pd(input3, permNeigh6);
        input7 = _mm512_max_pd(input2, permNeigh7);

        input4 = _mm512_min_pd(input4, permNeigh5);
        input3 = _mm512_min_pd(input3, permNeigh6);
        input2 = _mm512_min_pd(input2, permNeigh7);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input7, inputCopy);
        input7 = _mm512_max_pd(input7, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xF0, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xF0, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xF0, permNeighMax7);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xCC, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xCC, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xCC, permNeighMax7);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xAA, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xAA, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xAA, permNeighMax7);
    }
}


inline void CoreSmallSort7(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                            double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                            double* __restrict__ ptr7){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    CoreSmallSort7(input1, input2, input3, input4, input5, input6, input7);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
}


inline void CoreSmallSort8(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                            __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8 ){
    CoreSmallSort4(input, input2, input3, input4);
    CoreSmallSort4(input5, input6, input7, input8);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeigh8 = _mm512_permutexvar_pd(idxNoNeigh, input8);

        input5 = _mm512_max_pd(input4, permNeigh5);
        input6 = _mm512_max_pd(input3, permNeigh6);
        input7 = _mm512_max_pd(input2, permNeigh7);
        input8 = _mm512_max_pd(input, permNeigh8);

        input4 = _mm512_min_pd(input4, permNeigh5);
        input3 = _mm512_min_pd(input3, permNeigh6);
        input2 = _mm512_min_pd(input2, permNeigh7);
        input = _mm512_min_pd(input, permNeigh8);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input7, inputCopy);
        input7 = _mm512_max_pd(input7, inputCopy);
    }
    {
        __m512d inputCopy = input6;
        input6 = _mm512_min_pd(input8, inputCopy);
        input8 = _mm512_max_pd(input8, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512d inputCopy = input7;
        input7 = _mm512_min_pd(input8, inputCopy);
        input8 = _mm512_max_pd(input8, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeigh8 = _mm512_permutexvar_pd(idxNoNeigh, input8);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMin8 = _mm512_min_pd(permNeigh8, input8);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        __m512d permNeighMax8 = _mm512_max_pd(permNeigh8, input8);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xF0, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xF0, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xF0, permNeighMax7);
        input8 = _mm512_mask_mov_pd(permNeighMin8, 0xF0, permNeighMax8);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeigh8 = _mm512_permutexvar_pd(idxNoNeigh, input8);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMin8 = _mm512_min_pd(permNeigh8, input8);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        __m512d permNeighMax8 = _mm512_max_pd(permNeigh8, input8);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xCC, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xCC, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xCC, permNeighMax7);
        input8 = _mm512_mask_mov_pd(permNeighMin8, 0xCC, permNeighMax8);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeigh8 = _mm512_permutexvar_pd(idxNoNeigh, input8);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMin8 = _mm512_min_pd(permNeigh8, input8);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        __m512d permNeighMax8 = _mm512_max_pd(permNeigh8, input8);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xAA, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xAA, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xAA, permNeighMax7);
        input8 = _mm512_mask_mov_pd(permNeighMin8, 0xAA, permNeighMax8);
    }
}



inline void CoreSmallSort8(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                            double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                            double* __restrict__ ptr7, double* __restrict__ ptr8 ){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    CoreSmallSort8(input1, input2, input3, input4, input5, input6, input7, input8);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
}


inline void CoreSmallEnd1(__m512d& input){
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
    }
}

inline void CoreSmallEnd2(__m512d& input, __m512d& input2){
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
    }
}

inline void CoreSmallEnd3(__m512d& input, __m512d& input2, __m512d& input3){
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
    }
}

inline void CoreSmallEnd4(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4){
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
    }
}

inline void CoreSmallEnd5(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                              __m512d& input5){
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input5, inputCopy);
        input5 = _mm512_max_pd(input5, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xF0, permNeighMax5);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xCC, permNeighMax5);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xAA, permNeighMax5);
    }
}

inline void CoreSmallEnd6(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                              __m512d& input5, __m512d& input6){
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input5, inputCopy);
        input5 = _mm512_max_pd(input5, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xF0, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xF0, permNeighMax6);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xCC, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xCC, permNeighMax6);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xAA, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xAA, permNeighMax6);
    }
}

inline void CoreSmallEnd7(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                              __m512d& input5, __m512d& input6, __m512d& input7){
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input5, inputCopy);
        input5 = _mm512_max_pd(input5, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input7, inputCopy);
        input7 = _mm512_max_pd(input7, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input7, inputCopy);
        input7 = _mm512_max_pd(input7, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xF0, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xF0, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xF0, permNeighMax7);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xCC, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xCC, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xCC, permNeighMax7);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xAA, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xAA, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xAA, permNeighMax7);
    }
}


inline void CoreSmallEnd8(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                              __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8 ){
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input5, inputCopy);
        input5 = _mm512_max_pd(input5, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input7, inputCopy);
        input7 = _mm512_max_pd(input7, inputCopy);
    }
    {
        __m512d inputCopy = input4;
        input4 = _mm512_min_pd(input8, inputCopy);
        input8 = _mm512_max_pd(input8, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input3, inputCopy);
        input3 = _mm512_max_pd(input3, inputCopy);
    }
    {
        __m512d inputCopy = input2;
        input2 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input;
        input = _mm512_min_pd(input2, inputCopy);
        input2 = _mm512_max_pd(input2, inputCopy);
    }
    {
        __m512d inputCopy = input3;
        input3 = _mm512_min_pd(input4, inputCopy);
        input4 = _mm512_max_pd(input4, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input7, inputCopy);
        input7 = _mm512_max_pd(input7, inputCopy);
    }
    {
        __m512d inputCopy = input6;
        input6 = _mm512_min_pd(input8, inputCopy);
        input8 = _mm512_max_pd(input8, inputCopy);
    }
    {
        __m512d inputCopy = input5;
        input5 = _mm512_min_pd(input6, inputCopy);
        input6 = _mm512_max_pd(input6, inputCopy);
    }
    {
        __m512d inputCopy = input7;
        input7 = _mm512_min_pd(input8, inputCopy);
        input8 = _mm512_max_pd(input8, inputCopy);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(3, 2, 1, 0, 7, 6, 5, 4);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeigh8 = _mm512_permutexvar_pd(idxNoNeigh, input8);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMin8 = _mm512_min_pd(permNeigh8, input8);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        __m512d permNeighMax8 = _mm512_max_pd(permNeigh8, input8);
        input = _mm512_mask_mov_pd(permNeighMin, 0xF0, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xF0, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xF0, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xF0, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xF0, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xF0, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xF0, permNeighMax7);
        input8 = _mm512_mask_mov_pd(permNeighMin8, 0xF0, permNeighMax8);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(5, 4, 7, 6, 1, 0, 3, 2);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeigh8 = _mm512_permutexvar_pd(idxNoNeigh, input8);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMin8 = _mm512_min_pd(permNeigh8, input8);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        __m512d permNeighMax8 = _mm512_max_pd(permNeigh8, input8);
        input = _mm512_mask_mov_pd(permNeighMin, 0xCC, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xCC, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xCC, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xCC, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xCC, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xCC, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xCC, permNeighMax7);
        input8 = _mm512_mask_mov_pd(permNeighMin8, 0xCC, permNeighMax8);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi64(6, 7, 4, 5, 2, 3, 0, 1);
        __m512d permNeigh = _mm512_permutexvar_pd(idxNoNeigh, input);
        __m512d permNeigh2 = _mm512_permutexvar_pd(idxNoNeigh, input2);
        __m512d permNeigh3 = _mm512_permutexvar_pd(idxNoNeigh, input3);
        __m512d permNeigh4 = _mm512_permutexvar_pd(idxNoNeigh, input4);
        __m512d permNeigh5 = _mm512_permutexvar_pd(idxNoNeigh, input5);
        __m512d permNeigh6 = _mm512_permutexvar_pd(idxNoNeigh, input6);
        __m512d permNeigh7 = _mm512_permutexvar_pd(idxNoNeigh, input7);
        __m512d permNeigh8 = _mm512_permutexvar_pd(idxNoNeigh, input8);
        __m512d permNeighMin = _mm512_min_pd(permNeigh, input);
        __m512d permNeighMin2 = _mm512_min_pd(permNeigh2, input2);
        __m512d permNeighMin3 = _mm512_min_pd(permNeigh3, input3);
        __m512d permNeighMin4 = _mm512_min_pd(permNeigh4, input4);
        __m512d permNeighMin5 = _mm512_min_pd(permNeigh5, input5);
        __m512d permNeighMin6 = _mm512_min_pd(permNeigh6, input6);
        __m512d permNeighMin7 = _mm512_min_pd(permNeigh7, input7);
        __m512d permNeighMin8 = _mm512_min_pd(permNeigh8, input8);
        __m512d permNeighMax = _mm512_max_pd(permNeigh, input);
        __m512d permNeighMax2 = _mm512_max_pd(permNeigh2, input2);
        __m512d permNeighMax3 = _mm512_max_pd(permNeigh3, input3);
        __m512d permNeighMax4 = _mm512_max_pd(permNeigh4, input4);
        __m512d permNeighMax5 = _mm512_max_pd(permNeigh5, input5);
        __m512d permNeighMax6 = _mm512_max_pd(permNeigh6, input6);
        __m512d permNeighMax7 = _mm512_max_pd(permNeigh7, input7);
        __m512d permNeighMax8 = _mm512_max_pd(permNeigh8, input8);
        input = _mm512_mask_mov_pd(permNeighMin, 0xAA, permNeighMax);
        input2 = _mm512_mask_mov_pd(permNeighMin2, 0xAA, permNeighMax2);
        input3 = _mm512_mask_mov_pd(permNeighMin3, 0xAA, permNeighMax3);
        input4 = _mm512_mask_mov_pd(permNeighMin4, 0xAA, permNeighMax4);
        input5 = _mm512_mask_mov_pd(permNeighMin5, 0xAA, permNeighMax5);
        input6 = _mm512_mask_mov_pd(permNeighMin6, 0xAA, permNeighMax6);
        input7 = _mm512_mask_mov_pd(permNeighMin7, 0xAA, permNeighMax7);
        input8 = _mm512_mask_mov_pd(permNeighMin8, 0xAA, permNeighMax8);
    }
}

inline void CoreSmallSort9(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                            __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8,
                            __m512d& input9){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    input9 = CoreSmallSort(input9);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh9 = _mm512_permutexvar_pd(idxNoNeigh, input9);

        input9 = _mm512_max_pd(input8, permNeigh9);

        input8 = _mm512_min_pd(input8, permNeigh9);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd1(input9);
}



inline void CoreSmallSort9(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                            double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                            double* __restrict__ ptr7, double* __restrict__ ptr8,
                            double* __restrict__ ptr9){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    __m512d input9 = _mm512_loadu_pd(ptr9);
    CoreSmallSort9(input1, input2, input3, input4, input5, input6, input7, input8,
                    input9);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
    _mm512_storeu_pd(ptr9, input9);
}


inline void CoreSmallSort10(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                             __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8,
                             __m512d& input9, __m512d& input10){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort2(input9, input10);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh9 = _mm512_permutexvar_pd(idxNoNeigh, input9);
        __m512d permNeigh10 = _mm512_permutexvar_pd(idxNoNeigh, input10);

        input9 = _mm512_max_pd(input8, permNeigh9);
        input10 = _mm512_max_pd(input7, permNeigh10);

        input8 = _mm512_min_pd(input8, permNeigh9);
        input7 = _mm512_min_pd(input7, permNeigh10);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd2(input9, input10);
}



inline void CoreSmallSort10(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                             double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                             double* __restrict__ ptr7, double* __restrict__ ptr8,
                             double* __restrict__ ptr9, double* __restrict__ ptr10){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    __m512d input9 = _mm512_loadu_pd(ptr9);
    __m512d input10 = _mm512_loadu_pd(ptr10);
    CoreSmallSort10(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
    _mm512_storeu_pd(ptr9, input9);
    _mm512_storeu_pd(ptr10, input10);
}

inline void CoreSmallSort11(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                             __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8,
                             __m512d& input9, __m512d& input10, __m512d& input11){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort3(input9, input10, input11);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh9 = _mm512_permutexvar_pd(idxNoNeigh, input9);
        __m512d permNeigh10 = _mm512_permutexvar_pd(idxNoNeigh, input10);
        __m512d permNeigh11 = _mm512_permutexvar_pd(idxNoNeigh, input11);

        input9 = _mm512_max_pd(input8, permNeigh9);
        input10 = _mm512_max_pd(input7, permNeigh10);
        input11 = _mm512_max_pd(input6, permNeigh11);

        input8 = _mm512_min_pd(input8, permNeigh9);
        input7 = _mm512_min_pd(input7, permNeigh10);
        input6 = _mm512_min_pd(input6, permNeigh11);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd3(input9, input10, input11);
}



inline void CoreSmallSort11(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                             double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                             double* __restrict__ ptr7, double* __restrict__ ptr8,
                             double* __restrict__ ptr9, double* __restrict__ ptr10, double* __restrict__ ptr11){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    __m512d input9 = _mm512_loadu_pd(ptr9);
    __m512d input10 = _mm512_loadu_pd(ptr10);
    __m512d input11 = _mm512_loadu_pd(ptr11);
    CoreSmallSort11(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
    _mm512_storeu_pd(ptr9, input9);
    _mm512_storeu_pd(ptr10, input10);
    _mm512_storeu_pd(ptr11, input11);
}

inline void CoreSmallSort12(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                             __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8,
                             __m512d& input9, __m512d& input10, __m512d& input11, __m512d& input12){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort4(input9, input10, input11, input12);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh9 = _mm512_permutexvar_pd(idxNoNeigh, input9);
        __m512d permNeigh10 = _mm512_permutexvar_pd(idxNoNeigh, input10);
        __m512d permNeigh11 = _mm512_permutexvar_pd(idxNoNeigh, input11);
        __m512d permNeigh12 = _mm512_permutexvar_pd(idxNoNeigh, input12);

        input9 = _mm512_max_pd(input8, permNeigh9);
        input10 = _mm512_max_pd(input7, permNeigh10);
        input11 = _mm512_max_pd(input6, permNeigh11);
        input12 = _mm512_max_pd(input5, permNeigh12);

        input8 = _mm512_min_pd(input8, permNeigh9);
        input7 = _mm512_min_pd(input7, permNeigh10);
        input6 = _mm512_min_pd(input6, permNeigh11);
        input5 = _mm512_min_pd(input5, permNeigh12);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd4(input9, input10, input11, input12);
}



inline void CoreSmallSort12(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                             double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                             double* __restrict__ ptr7, double* __restrict__ ptr8,
                             double* __restrict__ ptr9, double* __restrict__ ptr10, double* __restrict__ ptr11,
                             double* __restrict__ ptr12){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    __m512d input9 = _mm512_loadu_pd(ptr9);
    __m512d input10 = _mm512_loadu_pd(ptr10);
    __m512d input11 = _mm512_loadu_pd(ptr11);
    __m512d input12 = _mm512_loadu_pd(ptr12);
    CoreSmallSort12(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
    _mm512_storeu_pd(ptr9, input9);
    _mm512_storeu_pd(ptr10, input10);
    _mm512_storeu_pd(ptr11, input11);
    _mm512_storeu_pd(ptr12, input12);
}

inline void CoreSmallSort13(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                             __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8,
                             __m512d& input9, __m512d& input10, __m512d& input11, __m512d& input12,
                             __m512d& input13){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort5(input9, input10, input11, input12, input13);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh9 = _mm512_permutexvar_pd(idxNoNeigh, input9);
        __m512d permNeigh10 = _mm512_permutexvar_pd(idxNoNeigh, input10);
        __m512d permNeigh11 = _mm512_permutexvar_pd(idxNoNeigh, input11);
        __m512d permNeigh12 = _mm512_permutexvar_pd(idxNoNeigh, input12);
        __m512d permNeigh13 = _mm512_permutexvar_pd(idxNoNeigh, input13);

        input9 = _mm512_max_pd(input8, permNeigh9);
        input10 = _mm512_max_pd(input7, permNeigh10);
        input11 = _mm512_max_pd(input6, permNeigh11);
        input12 = _mm512_max_pd(input5, permNeigh12);
        input13 = _mm512_max_pd(input4, permNeigh13);

        input8 = _mm512_min_pd(input8, permNeigh9);
        input7 = _mm512_min_pd(input7, permNeigh10);
        input6 = _mm512_min_pd(input6, permNeigh11);
        input5 = _mm512_min_pd(input5, permNeigh12);
        input4 = _mm512_min_pd(input4, permNeigh13);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd5(input9, input10, input11, input12, input13);
}



inline void CoreSmallSort13(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                             double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                             double* __restrict__ ptr7, double* __restrict__ ptr8,
                             double* __restrict__ ptr9, double* __restrict__ ptr10, double* __restrict__ ptr11,
                             double* __restrict__ ptr12, double* __restrict__ ptr13){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    __m512d input9 = _mm512_loadu_pd(ptr9);
    __m512d input10 = _mm512_loadu_pd(ptr10);
    __m512d input11 = _mm512_loadu_pd(ptr11);
    __m512d input12 = _mm512_loadu_pd(ptr12);
    __m512d input13 = _mm512_loadu_pd(ptr13);
    CoreSmallSort13(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12, input13);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
    _mm512_storeu_pd(ptr9, input9);
    _mm512_storeu_pd(ptr10, input10);
    _mm512_storeu_pd(ptr11, input11);
    _mm512_storeu_pd(ptr12, input12);
    _mm512_storeu_pd(ptr13, input13);
}

inline void CoreSmallSort14(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                             __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8,
                             __m512d& input9, __m512d& input10, __m512d& input11, __m512d& input12,
                             __m512d& input13, __m512d& input14){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort6(input9, input10, input11, input12, input13, input14);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh9 = _mm512_permutexvar_pd(idxNoNeigh, input9);
        __m512d permNeigh10 = _mm512_permutexvar_pd(idxNoNeigh, input10);
        __m512d permNeigh11 = _mm512_permutexvar_pd(idxNoNeigh, input11);
        __m512d permNeigh12 = _mm512_permutexvar_pd(idxNoNeigh, input12);
        __m512d permNeigh13 = _mm512_permutexvar_pd(idxNoNeigh, input13);
        __m512d permNeigh14 = _mm512_permutexvar_pd(idxNoNeigh, input14);

        input9 = _mm512_max_pd(input8, permNeigh9);
        input10 = _mm512_max_pd(input7, permNeigh10);
        input11 = _mm512_max_pd(input6, permNeigh11);
        input12 = _mm512_max_pd(input5, permNeigh12);
        input13 = _mm512_max_pd(input4, permNeigh13);
        input14 = _mm512_max_pd(input3, permNeigh14);

        input8 = _mm512_min_pd(input8, permNeigh9);
        input7 = _mm512_min_pd(input7, permNeigh10);
        input6 = _mm512_min_pd(input6, permNeigh11);
        input5 = _mm512_min_pd(input5, permNeigh12);
        input4 = _mm512_min_pd(input4, permNeigh13);
        input3 = _mm512_min_pd(input3, permNeigh14);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd6(input9, input10, input11, input12, input13, input14);
}



inline void CoreSmallSort14(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                             double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                             double* __restrict__ ptr7, double* __restrict__ ptr8,
                             double* __restrict__ ptr9, double* __restrict__ ptr10, double* __restrict__ ptr11,
                             double* __restrict__ ptr12, double* __restrict__ ptr13, double* __restrict__ ptr14){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    __m512d input9 = _mm512_loadu_pd(ptr9);
    __m512d input10 = _mm512_loadu_pd(ptr10);
    __m512d input11 = _mm512_loadu_pd(ptr11);
    __m512d input12 = _mm512_loadu_pd(ptr12);
    __m512d input13 = _mm512_loadu_pd(ptr13);
    __m512d input14 = _mm512_loadu_pd(ptr14);
    CoreSmallSort14(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12, input13, input14);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
    _mm512_storeu_pd(ptr9, input9);
    _mm512_storeu_pd(ptr10, input10);
    _mm512_storeu_pd(ptr11, input11);
    _mm512_storeu_pd(ptr12, input12);
    _mm512_storeu_pd(ptr13, input13);
    _mm512_storeu_pd(ptr14, input14);
}

inline void CoreSmallSort15(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                             __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8,
                             __m512d& input9, __m512d& input10, __m512d& input11, __m512d& input12,
                             __m512d& input13, __m512d& input14, __m512d& input15){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort7(input9, input10, input11, input12, input13, input14, input15);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh9 = _mm512_permutexvar_pd(idxNoNeigh, input9);
        __m512d permNeigh10 = _mm512_permutexvar_pd(idxNoNeigh, input10);
        __m512d permNeigh11 = _mm512_permutexvar_pd(idxNoNeigh, input11);
        __m512d permNeigh12 = _mm512_permutexvar_pd(idxNoNeigh, input12);
        __m512d permNeigh13 = _mm512_permutexvar_pd(idxNoNeigh, input13);
        __m512d permNeigh14 = _mm512_permutexvar_pd(idxNoNeigh, input14);
        __m512d permNeigh15 = _mm512_permutexvar_pd(idxNoNeigh, input15);

        input9 = _mm512_max_pd(input8, permNeigh9);
        input10 = _mm512_max_pd(input7, permNeigh10);
        input11 = _mm512_max_pd(input6, permNeigh11);
        input12 = _mm512_max_pd(input5, permNeigh12);
        input13 = _mm512_max_pd(input4, permNeigh13);
        input14 = _mm512_max_pd(input3, permNeigh14);
        input15 = _mm512_max_pd(input2, permNeigh15);

        input8 = _mm512_min_pd(input8, permNeigh9);
        input7 = _mm512_min_pd(input7, permNeigh10);
        input6 = _mm512_min_pd(input6, permNeigh11);
        input5 = _mm512_min_pd(input5, permNeigh12);
        input4 = _mm512_min_pd(input4, permNeigh13);
        input3 = _mm512_min_pd(input3, permNeigh14);
        input2 = _mm512_min_pd(input2, permNeigh15);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd7(input9, input10, input11, input12, input13, input14, input15);
}



inline void CoreSmallSort15(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                             double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                             double* __restrict__ ptr7, double* __restrict__ ptr8,
                             double* __restrict__ ptr9, double* __restrict__ ptr10, double* __restrict__ ptr11,
                             double* __restrict__ ptr12, double* __restrict__ ptr13, double* __restrict__ ptr14,
                             double* __restrict__ ptr15){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    __m512d input9 = _mm512_loadu_pd(ptr9);
    __m512d input10 = _mm512_loadu_pd(ptr10);
    __m512d input11 = _mm512_loadu_pd(ptr11);
    __m512d input12 = _mm512_loadu_pd(ptr12);
    __m512d input13 = _mm512_loadu_pd(ptr13);
    __m512d input14 = _mm512_loadu_pd(ptr14);
    __m512d input15 = _mm512_loadu_pd(ptr15);
    CoreSmallSort15(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12, input13, input14, input15);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
    _mm512_storeu_pd(ptr9, input9);
    _mm512_storeu_pd(ptr10, input10);
    _mm512_storeu_pd(ptr11, input11);
    _mm512_storeu_pd(ptr12, input12);
    _mm512_storeu_pd(ptr13, input13);
    _mm512_storeu_pd(ptr14, input14);
    _mm512_storeu_pd(ptr15, input15);
}


inline void CoreSmallSort16(__m512d& input, __m512d& input2, __m512d& input3, __m512d& input4,
                             __m512d& input5, __m512d& input6, __m512d& input7, __m512d& input8,
                             __m512d& input9, __m512d& input10, __m512d& input11, __m512d& input12,
                             __m512d& input13, __m512d& input14, __m512d& input15, __m512d& input16){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort8(input9, input10, input11, input12, input13, input14, input15, input16);
    {
        __m512i idxNoNeigh = _mm512_set_epi64(0, 1, 2, 3, 4, 5, 6, 7);
        __m512d permNeigh9 = _mm512_permutexvar_pd(idxNoNeigh, input9);
        __m512d permNeigh10 = _mm512_permutexvar_pd(idxNoNeigh, input10);
        __m512d permNeigh11 = _mm512_permutexvar_pd(idxNoNeigh, input11);
        __m512d permNeigh12 = _mm512_permutexvar_pd(idxNoNeigh, input12);
        __m512d permNeigh13 = _mm512_permutexvar_pd(idxNoNeigh, input13);
        __m512d permNeigh14 = _mm512_permutexvar_pd(idxNoNeigh, input14);
        __m512d permNeigh15 = _mm512_permutexvar_pd(idxNoNeigh, input15);
        __m512d permNeigh16 = _mm512_permutexvar_pd(idxNoNeigh, input16);

        input9 = _mm512_max_pd(input8, permNeigh9);
        input10 = _mm512_max_pd(input7, permNeigh10);
        input11 = _mm512_max_pd(input6, permNeigh11);
        input12 = _mm512_max_pd(input5, permNeigh12);
        input13 = _mm512_max_pd(input4, permNeigh13);
        input14 = _mm512_max_pd(input3, permNeigh14);
        input15 = _mm512_max_pd(input2, permNeigh15);
        input16 = _mm512_max_pd(input, permNeigh16);

        input8 = _mm512_min_pd(input8, permNeigh9);
        input7 = _mm512_min_pd(input7, permNeigh10);
        input6 = _mm512_min_pd(input6, permNeigh11);
        input5 = _mm512_min_pd(input5, permNeigh12);
        input4 = _mm512_min_pd(input4, permNeigh13);
        input3 = _mm512_min_pd(input3, permNeigh14);
        input2 = _mm512_min_pd(input2, permNeigh15);
        input = _mm512_min_pd(input, permNeigh16);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd8(input9, input10, input11, input12, input13, input14, input15, input16);
}



inline void CoreSmallSort16(double* __restrict__ ptr1, double* __restrict__ ptr2, double* __restrict__ ptr3,
                             double* __restrict__ ptr4, double* __restrict__ ptr5, double* __restrict__ ptr6,
                             double* __restrict__ ptr7, double* __restrict__ ptr8,
                             double* __restrict__ ptr9, double* __restrict__ ptr10, double* __restrict__ ptr11,
                             double* __restrict__ ptr12, double* __restrict__ ptr13, double* __restrict__ ptr14,
                             double* __restrict__ ptr15, double* __restrict__ ptr16){
    __m512d input1 = _mm512_loadu_pd(ptr1);
    __m512d input2 = _mm512_loadu_pd(ptr2);
    __m512d input3 = _mm512_loadu_pd(ptr3);
    __m512d input4 = _mm512_loadu_pd(ptr4);
    __m512d input5 = _mm512_loadu_pd(ptr5);
    __m512d input6 = _mm512_loadu_pd(ptr6);
    __m512d input7 = _mm512_loadu_pd(ptr7);
    __m512d input8 = _mm512_loadu_pd(ptr8);
    __m512d input9 = _mm512_loadu_pd(ptr9);
    __m512d input10 = _mm512_loadu_pd(ptr10);
    __m512d input11 = _mm512_loadu_pd(ptr11);
    __m512d input12 = _mm512_loadu_pd(ptr12);
    __m512d input13 = _mm512_loadu_pd(ptr13);
    __m512d input14 = _mm512_loadu_pd(ptr14);
    __m512d input15 = _mm512_loadu_pd(ptr15);
    __m512d input16 = _mm512_loadu_pd(ptr16);
    CoreSmallSort16(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12, input13, input14, input15, input16);
    _mm512_storeu_pd(ptr1, input1);
    _mm512_storeu_pd(ptr2, input2);
    _mm512_storeu_pd(ptr3, input3);
    _mm512_storeu_pd(ptr4, input4);
    _mm512_storeu_pd(ptr5, input5);
    _mm512_storeu_pd(ptr6, input6);
    _mm512_storeu_pd(ptr7, input7);
    _mm512_storeu_pd(ptr8, input8);
    _mm512_storeu_pd(ptr9, input9);
    _mm512_storeu_pd(ptr10, input10);
    _mm512_storeu_pd(ptr11, input11);
    _mm512_storeu_pd(ptr12, input12);
    _mm512_storeu_pd(ptr13, input13);
    _mm512_storeu_pd(ptr14, input14);
    _mm512_storeu_pd(ptr15, input15);
    _mm512_storeu_pd(ptr16, input16);
}



inline void SmallSort16V(double* __restrict__ ptr, const size_t length){
    // length is limited to 4 times size of a vec
    const int nbValuesInVec = 8;
    const int nbVecs = (length+nbValuesInVec-1)/nbValuesInVec;
    const int rest = nbVecs*nbValuesInVec-length;
    const int lastVecSize = nbValuesInVec-rest;
    // 01. Dec 2023 (Jonas Keller):
    // changed to use std::bit_cast instead of reinterpret_cast to avoid
    // warnings about strict aliasing (requires C++20 now)
    /*
    const double temp_DBL_MAX = DBL_MAX;
    const long int double_max = reinterpret_cast<const long int&>(temp_DBL_MAX);
    */
    const long int double_max = std::bit_cast<long int>(DBL_MAX);

    switch(nbVecs){
    case 1:
    {
        __m512d v1 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        v1 = CoreSmallSort(v1);
        _mm512_mask_compressstoreu_pd(ptr, 0xFF>>rest, v1);
    }
        break;
    case 2:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+8)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort2(v1,v2);
        _mm512_storeu_pd(ptr, v1);
        _mm512_mask_compressstoreu_pd(ptr+8, 0xFF>>rest, v2);
    }
        break;
    case 3:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+16)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort3(v1,v2,v3);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_mask_compressstoreu_pd(ptr+16, 0xFF>>rest, v3);
    }
        break;
    case 4:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+24)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort4(v1,v2,v3,v4);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_mask_compressstoreu_pd(ptr+24, 0xFF>>rest, v4);
    }
        break;
    case 5:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+32)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort5(v1,v2,v3,v4,v5);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_mask_compressstoreu_pd(ptr+32, 0xFF>>rest, v5);
    }
        break;
    case 6:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+40)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort6(v1,v2,v3,v4,v5, v6);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_mask_compressstoreu_pd(ptr+40, 0xFF>>rest, v6);
    }
        break;
    case 7:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+48)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort7(v1,v2,v3,v4,v5,v6,v7);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_mask_compressstoreu_pd(ptr+48, 0xFF>>rest, v7);
    }
        break;
    case 8:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+56)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort8(v1,v2,v3,v4,v5,v6,v7,v8);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_mask_compressstoreu_pd(ptr+56, 0xFF>>rest, v8);
    }
        break;
    case 9:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_loadu_pd(ptr+56);
        __m512d v9 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+64)),
                                                         _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort9(v1,v2,v3,v4,v5,v6,v7,v8,v9);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_storeu_pd(ptr+56, v8);
        _mm512_mask_compressstoreu_pd(ptr+64, 0xFF>>rest, v9);
    }
        break;
    case 10:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_loadu_pd(ptr+56);
        __m512d v9 = _mm512_loadu_pd(ptr+64);
        __m512d v10 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+72)),
                                                          _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort10(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_storeu_pd(ptr+56, v8);
        _mm512_storeu_pd(ptr+64, v9);
        _mm512_mask_compressstoreu_pd(ptr+72, 0xFF>>rest, v10);
    }
        break;
    case 11:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_loadu_pd(ptr+56);
        __m512d v9 = _mm512_loadu_pd(ptr+64);
        __m512d v10 = _mm512_loadu_pd(ptr+72);
        __m512d v11 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+80)),
                                                          _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort11(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_storeu_pd(ptr+56, v8);
        _mm512_storeu_pd(ptr+64, v9);
        _mm512_storeu_pd(ptr+72, v10);
        _mm512_mask_compressstoreu_pd(ptr+80, 0xFF>>rest, v11);
    }
        break;
    case 12:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_loadu_pd(ptr+56);
        __m512d v9 = _mm512_loadu_pd(ptr+64);
        __m512d v10 = _mm512_loadu_pd(ptr+72);
        __m512d v11 = _mm512_loadu_pd(ptr+80);
        __m512d v12 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+88)),
                                                          _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort12(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_storeu_pd(ptr+56, v8);
        _mm512_storeu_pd(ptr+64, v9);
        _mm512_storeu_pd(ptr+72, v10);
        _mm512_storeu_pd(ptr+80, v11);
        _mm512_mask_compressstoreu_pd(ptr+88, 0xFF>>rest, v12);
    }
        break;
    case 13:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_loadu_pd(ptr+56);
        __m512d v9 = _mm512_loadu_pd(ptr+64);
        __m512d v10 = _mm512_loadu_pd(ptr+72);
        __m512d v11 = _mm512_loadu_pd(ptr+80);
        __m512d v12 = _mm512_loadu_pd(ptr+88);
        __m512d v13 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+96)),
                                                          _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort13(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_storeu_pd(ptr+56, v8);
        _mm512_storeu_pd(ptr+64, v9);
        _mm512_storeu_pd(ptr+72, v10);
        _mm512_storeu_pd(ptr+80, v11);
        _mm512_storeu_pd(ptr+88, v12);
        _mm512_mask_compressstoreu_pd(ptr+96, 0xFF>>rest, v13);
    }
        break;
    case 14:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_loadu_pd(ptr+56);
        __m512d v9 = _mm512_loadu_pd(ptr+64);
        __m512d v10 = _mm512_loadu_pd(ptr+72);
        __m512d v11 = _mm512_loadu_pd(ptr+80);
        __m512d v12 = _mm512_loadu_pd(ptr+88);
        __m512d v13 = _mm512_loadu_pd(ptr+96);
        __m512d v14 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+104)),
                                                          _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort14(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_storeu_pd(ptr+56, v8);
        _mm512_storeu_pd(ptr+64, v9);
        _mm512_storeu_pd(ptr+72, v10);
        _mm512_storeu_pd(ptr+80, v11);
        _mm512_storeu_pd(ptr+88, v12);
        _mm512_storeu_pd(ptr+96, v13);
        _mm512_mask_compressstoreu_pd(ptr+104, 0xFF>>rest, v14);
    }
        break;
    case 15:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_loadu_pd(ptr+56);
        __m512d v9 = _mm512_loadu_pd(ptr+64);
        __m512d v10 = _mm512_loadu_pd(ptr+72);
        __m512d v11 = _mm512_loadu_pd(ptr+80);
        __m512d v12 = _mm512_loadu_pd(ptr+88);
        __m512d v13 = _mm512_loadu_pd(ptr+96);
        __m512d v14 = _mm512_loadu_pd(ptr+104);
        __m512d v15 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+112)),
                                                          _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort15(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_storeu_pd(ptr+56, v8);
        _mm512_storeu_pd(ptr+64, v9);
        _mm512_storeu_pd(ptr+72, v10);
        _mm512_storeu_pd(ptr+80, v11);
        _mm512_storeu_pd(ptr+88, v12);
        _mm512_storeu_pd(ptr+96, v13);
        _mm512_storeu_pd(ptr+104, v14);
        _mm512_mask_compressstoreu_pd(ptr+112, 0xFF>>rest, v15);
    }
        break;
        //case 16:
    default:
    {
        __m512d v1 = _mm512_loadu_pd(ptr);
        __m512d v2 = _mm512_loadu_pd(ptr+8);
        __m512d v3 = _mm512_loadu_pd(ptr+16);
        __m512d v4 = _mm512_loadu_pd(ptr+24);
        __m512d v5 = _mm512_loadu_pd(ptr+32);
        __m512d v6 = _mm512_loadu_pd(ptr+40);
        __m512d v7 = _mm512_loadu_pd(ptr+48);
        __m512d v8 = _mm512_loadu_pd(ptr+56);
        __m512d v9 = _mm512_loadu_pd(ptr+64);
        __m512d v10 = _mm512_loadu_pd(ptr+72);
        __m512d v11 = _mm512_loadu_pd(ptr+80);
        __m512d v12 = _mm512_loadu_pd(ptr+88);
        __m512d v13 = _mm512_loadu_pd(ptr+96);
        __m512d v14 = _mm512_loadu_pd(ptr+104);
        __m512d v15 = _mm512_loadu_pd(ptr+112);
        __m512d v16 = _mm512_castsi512_pd(_mm512_or_si512(_mm512_castpd_si512(_mm512_maskz_loadu_pd(0xFF>>rest, ptr+120)),
                                                          _mm512_maskz_set1_epi64(0xFF<<lastVecSize, double_max)));
        CoreSmallSort16(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,v16);
        _mm512_storeu_pd(ptr, v1);
        _mm512_storeu_pd(ptr+8, v2);
        _mm512_storeu_pd(ptr+16, v3);
        _mm512_storeu_pd(ptr+24, v4);
        _mm512_storeu_pd(ptr+32, v5);
        _mm512_storeu_pd(ptr+40, v6);
        _mm512_storeu_pd(ptr+48, v7);
        _mm512_storeu_pd(ptr+56, v8);
        _mm512_storeu_pd(ptr+64, v9);
        _mm512_storeu_pd(ptr+72, v10);
        _mm512_storeu_pd(ptr+80, v11);
        _mm512_storeu_pd(ptr+88, v12);
        _mm512_storeu_pd(ptr+96, v13);
        _mm512_storeu_pd(ptr+104, v14);
        _mm512_storeu_pd(ptr+112, v15);
        _mm512_mask_compressstoreu_pd(ptr+120, 0xFF>>rest, v16);
    }
    }
}

/// Int

inline __m512i CoreSmallSort(__m512i input){
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(12, 13, 14, 15, 8, 9, 10, 11,
                                              4, 5, 6, 7, 0, 1, 2, 3);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(8, 9, 10, 11, 12, 13, 14, 15,
                                              0, 1, 2, 3, 4, 5, 6, 7);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                               3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
    }

    return input;
}

inline void CoreSmallSort(int* __restrict__ ptr1){
    _mm512_storeu_si512(ptr1, CoreSmallSort(_mm512_loadu_si512(ptr1)));
}


inline void CoreExchangeSort2V(__m512i& input, __m512i& input2 ){
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        input = _mm512_min_epi32(input2, permNeigh);
        input2 = _mm512_max_epi32(input2, permNeigh);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
    }
}

inline void CoreSmallSort2(__m512i& input, __m512i& input2 ){
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeigh2 = _mm512_permutexvar_epi32(idxNoNeigh, input2);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMin2 = _mm512_min_epi32(permNeigh2, input2);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        __m512i permNeighMax2 = _mm512_max_epi32(permNeigh2, input2);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
    }
    CoreExchangeSort2V(input,input2);
}

inline void CoreSmallSort2(int* __restrict__ ptr1, int* __restrict__ ptr2 ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    CoreSmallSort2(input1, input2);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
}


inline void CoreSmallSort3(__m512i& input, __m512i& input2, __m512i& input3 ){
    CoreSmallSort2(input, input2);
    input3 = CoreSmallSort(input3);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        input3 = _mm512_max_epi32(input2, permNeigh);
        input2 = _mm512_min_epi32(input2, permNeigh);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
    }
}

inline void CoreSmallSort3(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3 ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    CoreSmallSort3(input1, input2, input3);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
}

inline void CoreSmallSort4(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4 ){
    CoreSmallSort2(input, input2);
    CoreSmallSort2(input3, input4);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh3 = _mm512_permutexvar_epi32(idxNoNeigh, input3);
        __m512i permNeigh4 = _mm512_permutexvar_epi32(idxNoNeigh, input4);

        input4 = _mm512_max_epi32(input, permNeigh4);
        input = _mm512_min_epi32(input, permNeigh4);

        input3 = _mm512_max_epi32(input2, permNeigh3);
        input2 = _mm512_min_epi32(input2, permNeigh3);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
    }
}

inline void CoreSmallSort4(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4 ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    CoreSmallSort4(input1, input2, input3, input4);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
}


inline void CoreSmallSort5(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4, __m512i& input5 ){
    CoreSmallSort4(input, input2, input3, input4);
    input5 = CoreSmallSort(input5);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);

        input5 = _mm512_max_epi32(input4, permNeigh5);
        input4 = _mm512_min_epi32(input4, permNeigh5);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
    }
}

inline void CoreSmallSort5(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4, int* __restrict__ ptr5 ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    CoreSmallSort5(input1, input2, input3, input4, input5);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
}


inline void CoreSmallSort6(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6){
    CoreSmallSort4(input, input2, input3, input4);
    CoreSmallSort2(input5, input6);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);

        input5 = _mm512_max_epi32(input4, permNeigh5);
        input6 = _mm512_max_epi32(input3, permNeigh6);

        input4 = _mm512_min_epi32(input4, permNeigh5);
        input3 = _mm512_min_epi32(input3, permNeigh6);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
    }
}

inline void CoreSmallSort6(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                            int* __restrict__ ptr5, int* __restrict__ ptr6 ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    CoreSmallSort6(input1, input2, input3, input4, input5, input6);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
}


inline void CoreSmallSort7(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7){
    CoreSmallSort4(input, input2, input3, input4);
    CoreSmallSort3(input5, input6, input7);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);

        input5 = _mm512_max_epi32(input4, permNeigh5);
        input6 = _mm512_max_epi32(input3, permNeigh6);
        input7 = _mm512_max_epi32(input2, permNeigh7);

        input4 = _mm512_min_epi32(input4, permNeigh5);
        input3 = _mm512_min_epi32(input3, permNeigh6);
        input2 = _mm512_min_epi32(input2, permNeigh7);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input7, inputCopy);
        input7 = _mm512_max_epi32(input7, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xFF00, permNeighMax7);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xF0F0, permNeighMax7);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xCCCC, permNeighMax7);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xAAAA, permNeighMax7);
    }
}

inline void CoreSmallSort7(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                            int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7 ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    CoreSmallSort7(input1, input2, input3, input4, input5, input6, input7);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
}

inline void CoreSmallSort8(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8 ){
    CoreSmallSort4(input, input2, input3, input4);
    CoreSmallSort4(input5, input6, input7, input8);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh5 = _mm512_permutexvar_epi32(idxNoNeigh, input5);
        __m512i permNeigh6 = _mm512_permutexvar_epi32(idxNoNeigh, input6);
        __m512i permNeigh7 = _mm512_permutexvar_epi32(idxNoNeigh, input7);
        __m512i permNeigh8 = _mm512_permutexvar_epi32(idxNoNeigh, input8);

        input5 = _mm512_max_epi32(input4, permNeigh5);
        input6 = _mm512_max_epi32(input3, permNeigh6);
        input7 = _mm512_max_epi32(input2, permNeigh7);
        input8 = _mm512_max_epi32(input, permNeigh8);

        input4 = _mm512_min_epi32(input4, permNeigh5);
        input3 = _mm512_min_epi32(input3, permNeigh6);
        input2 = _mm512_min_epi32(input2, permNeigh7);
        input = _mm512_min_epi32(input, permNeigh8);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input7, inputCopy);
        input7 = _mm512_max_epi32(input7, inputCopy);
    }
    {
        __m512i inputCopy = input6;
        input6 = _mm512_min_epi32(input8, inputCopy);
        input8 = _mm512_max_epi32(input8, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
    }
    {
        __m512i inputCopy = input7;
        input7 = _mm512_min_epi32(input8, inputCopy);
        input8 = _mm512_max_epi32(input8, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xFF00, permNeighMax7);
        input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xFF00, permNeighMax8);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xF0F0, permNeighMax7);
        input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xF0F0, permNeighMax8);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xCCCC, permNeighMax7);
        input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xCCCC, permNeighMax8);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xAAAA, permNeighMax7);
        input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xAAAA, permNeighMax8);
    }
}

inline void CoreSmallSort8(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                            int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8 ){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    CoreSmallSort8(input1, input2, input3, input4, input5, input6, input7, input8);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
}

inline void CoreSmallEnd1(__m512i& input){
    {
        __m512i idxNoNeigh = _mm512_set_epi32(7, 6, 5, 4, 3, 2, 1, 0,
                                              15, 14, 13, 12, 11, 10, 9, 8);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32( 11, 10, 9, 8, 15, 14, 13, 12,
                                               3, 2, 1, 0, 7, 6, 5, 4);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(13, 12, 15, 14, 9, 8, 11, 10,
                                              5, 4, 7, 6, 1, 0, 3, 2);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
    }
    {
        __m512i idxNoNeigh = _mm512_set_epi32(14, 15, 12, 13, 10, 11, 8, 9,
                                              6, 7, 4, 5, 2, 3, 0, 1);
        __m512i permNeigh = _mm512_permutexvar_epi32(idxNoNeigh, input);
        __m512i permNeighMin = _mm512_min_epi32(permNeigh, input);
        __m512i permNeighMax = _mm512_max_epi32(permNeigh, input);
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
    }
}

inline void CoreSmallEnd2(__m512i& input, __m512i& input2){
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
    }
}

inline void CoreSmallEnd3(__m512i& input, __m512i& input2, __m512i& input3){
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
    }
}

inline void CoreSmallEnd4(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4){
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
    }
}

inline void CoreSmallEnd5(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                              __m512i& input5){
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input5, inputCopy);
        input5 = _mm512_max_epi32(input5, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
    }
}

inline void CoreSmallEnd6(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                              __m512i& input5, __m512i& input6){
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input5, inputCopy);
        input5 = _mm512_max_epi32(input5, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
    }
}

inline void CoreSmallEnd7(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                              __m512i& input5, __m512i& input6, __m512i& input7){
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input5, inputCopy);
        input5 = _mm512_max_epi32(input5, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input7, inputCopy);
        input7 = _mm512_max_epi32(input7, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input7, inputCopy);
        input7 = _mm512_max_epi32(input7, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input7, inputCopy);
        input7 = _mm512_max_epi32(input7, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xFF00, permNeighMax7);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xF0F0, permNeighMax7);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xCCCC, permNeighMax7);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xAAAA, permNeighMax7);
    }
}

inline void CoreSmallEnd8(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                              __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8 ){
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input5, inputCopy);
        input5 = _mm512_max_epi32(input5, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input7, inputCopy);
        input7 = _mm512_max_epi32(input7, inputCopy);
    }
    {
        __m512i inputCopy = input4;
        input4 = _mm512_min_epi32(input8, inputCopy);
        input8 = _mm512_max_epi32(input8, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input7, inputCopy);
        input7 = _mm512_max_epi32(input7, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input3, inputCopy);
        input3 = _mm512_max_epi32(input3, inputCopy);
    }
    {
        __m512i inputCopy = input2;
        input2 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input;
        input = _mm512_min_epi32(input2, inputCopy);
        input2 = _mm512_max_epi32(input2, inputCopy);
    }
    {
        __m512i inputCopy = input3;
        input3 = _mm512_min_epi32(input4, inputCopy);
        input4 = _mm512_max_epi32(input4, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input7, inputCopy);
        input7 = _mm512_max_epi32(input7, inputCopy);
    }
    {
        __m512i inputCopy = input6;
        input6 = _mm512_min_epi32(input8, inputCopy);
        input8 = _mm512_max_epi32(input8, inputCopy);
    }
    {
        __m512i inputCopy = input5;
        input5 = _mm512_min_epi32(input6, inputCopy);
        input6 = _mm512_max_epi32(input6, inputCopy);
    }
    {
        __m512i inputCopy = input7;
        input7 = _mm512_min_epi32(input8, inputCopy);
        input8 = _mm512_max_epi32(input8, inputCopy);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xFF00, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xFF00, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xFF00, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xFF00, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xFF00, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xFF00, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xFF00, permNeighMax7);
        input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xFF00, permNeighMax8);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xF0F0, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xF0F0, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xF0F0, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xF0F0, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xF0F0, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xF0F0, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xF0F0, permNeighMax7);
        input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xF0F0, permNeighMax8);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xCCCC, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xCCCC, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xCCCC, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xCCCC, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xCCCC, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xCCCC, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xCCCC, permNeighMax7);
        input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xCCCC, permNeighMax8);
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
        input = _mm512_mask_mov_epi32(permNeighMin, 0xAAAA, permNeighMax);
        input2 = _mm512_mask_mov_epi32(permNeighMin2, 0xAAAA, permNeighMax2);
        input3 = _mm512_mask_mov_epi32(permNeighMin3, 0xAAAA, permNeighMax3);
        input4 = _mm512_mask_mov_epi32(permNeighMin4, 0xAAAA, permNeighMax4);
        input5 = _mm512_mask_mov_epi32(permNeighMin5, 0xAAAA, permNeighMax5);
        input6 = _mm512_mask_mov_epi32(permNeighMin6, 0xAAAA, permNeighMax6);
        input7 = _mm512_mask_mov_epi32(permNeighMin7, 0xAAAA, permNeighMax7);
        input8 = _mm512_mask_mov_epi32(permNeighMin8, 0xAAAA, permNeighMax8);
    }
}

inline void CoreSmallSort9(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                            __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                            __m512i& input9){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    input9 = CoreSmallSort(input9);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);

        input9 = _mm512_max_epi32(input8, permNeigh9);

        input8 = _mm512_min_epi32(input8, permNeigh9);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd1(input9);
}

inline void CoreSmallSort9(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                            int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8,
                            int* __restrict__ ptr9){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    __m512i input9 = _mm512_loadu_si512(ptr9);
    CoreSmallSort9(input1, input2, input3, input4, input5, input6, input7, input8,
                    input9);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
    _mm512_storeu_si512(ptr9, input9);
}

inline void CoreSmallSort10(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                             __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                             __m512i& input9, __m512i& input10){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort2(input9, input10);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);

        input9 = _mm512_max_epi32(input8, permNeigh9);
        input10 = _mm512_max_epi32(input7, permNeigh10);

        input8 = _mm512_min_epi32(input8, permNeigh9);
        input7 = _mm512_min_epi32(input7, permNeigh10);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd2(input9, input10);
}

inline void CoreSmallSort10(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                             int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8,
                             int* __restrict__ ptr9, int* __restrict__ ptr10){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    __m512i input9 = _mm512_loadu_si512(ptr9);
    __m512i input10 = _mm512_loadu_si512(ptr10);
    CoreSmallSort10(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
    _mm512_storeu_si512(ptr9, input9);
    _mm512_storeu_si512(ptr10, input10);
}

inline void CoreSmallSort11(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                             __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                             __m512i& input9, __m512i& input10, __m512i& input11){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort3(input9, input10, input11);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);

        input9 = _mm512_max_epi32(input8, permNeigh9);
        input10 = _mm512_max_epi32(input7, permNeigh10);
        input11 = _mm512_max_epi32(input6, permNeigh11);

        input8 = _mm512_min_epi32(input8, permNeigh9);
        input7 = _mm512_min_epi32(input7, permNeigh10);
        input6 = _mm512_min_epi32(input6, permNeigh11);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd3(input9, input10, input11);
}

inline void CoreSmallSort11(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                             int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8,
                             int* __restrict__ ptr9, int* __restrict__ ptr10, int* __restrict__ ptr11){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    __m512i input9 = _mm512_loadu_si512(ptr9);
    __m512i input10 = _mm512_loadu_si512(ptr10);
    __m512i input11 = _mm512_loadu_si512(ptr11);
    CoreSmallSort11(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
    _mm512_storeu_si512(ptr9, input9);
    _mm512_storeu_si512(ptr10, input10);
    _mm512_storeu_si512(ptr11, input11);
}

inline void CoreSmallSort12(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                             __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                             __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12 ){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort4(input9, input10, input11, input12);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);
        __m512i permNeigh12 = _mm512_permutexvar_epi32(idxNoNeigh, input12);

        input9 = _mm512_max_epi32(input8, permNeigh9);
        input10 = _mm512_max_epi32(input7, permNeigh10);
        input11 = _mm512_max_epi32(input6, permNeigh11);
        input12 = _mm512_max_epi32(input5, permNeigh12);

        input8 = _mm512_min_epi32(input8, permNeigh9);
        input7 = _mm512_min_epi32(input7, permNeigh10);
        input6 = _mm512_min_epi32(input6, permNeigh11);
        input5 = _mm512_min_epi32(input5, permNeigh12);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd4(input9, input10, input11, input12);
}

inline void CoreSmallSort12(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                             int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8,
                             int* __restrict__ ptr9, int* __restrict__ ptr10, int* __restrict__ ptr11, int* __restrict__ ptr12){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    __m512i input9 = _mm512_loadu_si512(ptr9);
    __m512i input10 = _mm512_loadu_si512(ptr10);
    __m512i input11 = _mm512_loadu_si512(ptr11);
    __m512i input12 = _mm512_loadu_si512(ptr12);
    CoreSmallSort12(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
    _mm512_storeu_si512(ptr9, input9);
    _mm512_storeu_si512(ptr10, input10);
    _mm512_storeu_si512(ptr11, input11);
    _mm512_storeu_si512(ptr12, input12);
}

inline void CoreSmallSort13(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                             __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                             __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                             __m512i& input13 ){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort5(input9, input10, input11, input12, input13);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);
        __m512i permNeigh12 = _mm512_permutexvar_epi32(idxNoNeigh, input12);
        __m512i permNeigh13 = _mm512_permutexvar_epi32(idxNoNeigh, input13);

        input9 = _mm512_max_epi32(input8, permNeigh9);
        input10 = _mm512_max_epi32(input7, permNeigh10);
        input11 = _mm512_max_epi32(input6, permNeigh11);
        input12 = _mm512_max_epi32(input5, permNeigh12);
        input13 = _mm512_max_epi32(input4, permNeigh13);

        input8 = _mm512_min_epi32(input8, permNeigh9);
        input7 = _mm512_min_epi32(input7, permNeigh10);
        input6 = _mm512_min_epi32(input6, permNeigh11);
        input5 = _mm512_min_epi32(input5, permNeigh12);
        input4 = _mm512_min_epi32(input4, permNeigh13);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd5(input9, input10, input11, input12, input13);
}

inline void CoreSmallSort13(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                             int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8,
                             int* __restrict__ ptr9, int* __restrict__ ptr10, int* __restrict__ ptr11, int* __restrict__ ptr12,
                             int* __restrict__ ptr13){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    __m512i input9 = _mm512_loadu_si512(ptr9);
    __m512i input10 = _mm512_loadu_si512(ptr10);
    __m512i input11 = _mm512_loadu_si512(ptr11);
    __m512i input12 = _mm512_loadu_si512(ptr12);
    __m512i input13 = _mm512_loadu_si512(ptr13);
    CoreSmallSort13(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12, input13);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
    _mm512_storeu_si512(ptr9, input9);
    _mm512_storeu_si512(ptr10, input10);
    _mm512_storeu_si512(ptr11, input11);
    _mm512_storeu_si512(ptr12, input12);
    _mm512_storeu_si512(ptr13, input13);
}


inline void CoreSmallSort14(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                             __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                             __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                             __m512i& input13, __m512i& input14 ){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort6(input9, input10, input11, input12, input13, input14);
    {
        __m512i idxNoNeigh = _mm512_set_epi32(0, 1, 2, 3, 4, 5, 6, 7,
                                              8, 9, 10, 11, 12, 13, 14, 15);
        __m512i permNeigh9 = _mm512_permutexvar_epi32(idxNoNeigh, input9);
        __m512i permNeigh10 = _mm512_permutexvar_epi32(idxNoNeigh, input10);
        __m512i permNeigh11 = _mm512_permutexvar_epi32(idxNoNeigh, input11);
        __m512i permNeigh12 = _mm512_permutexvar_epi32(idxNoNeigh, input12);
        __m512i permNeigh13 = _mm512_permutexvar_epi32(idxNoNeigh, input13);
        __m512i permNeigh14 = _mm512_permutexvar_epi32(idxNoNeigh, input14);

        input9 = _mm512_max_epi32(input8, permNeigh9);
        input10 = _mm512_max_epi32(input7, permNeigh10);
        input11 = _mm512_max_epi32(input6, permNeigh11);
        input12 = _mm512_max_epi32(input5, permNeigh12);
        input13 = _mm512_max_epi32(input4, permNeigh13);
        input14 = _mm512_max_epi32(input3, permNeigh14);

        input8 = _mm512_min_epi32(input8, permNeigh9);
        input7 = _mm512_min_epi32(input7, permNeigh10);
        input6 = _mm512_min_epi32(input6, permNeigh11);
        input5 = _mm512_min_epi32(input5, permNeigh12);
        input4 = _mm512_min_epi32(input4, permNeigh13);
        input3 = _mm512_min_epi32(input3, permNeigh14);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd6(input9, input10, input11, input12, input13, input14);
}

inline void CoreSmallSort14(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                             int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8,
                             int* __restrict__ ptr9, int* __restrict__ ptr10, int* __restrict__ ptr11, int* __restrict__ ptr12,
                             int* __restrict__ ptr13, int* __restrict__ ptr14){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    __m512i input9 = _mm512_loadu_si512(ptr9);
    __m512i input10 = _mm512_loadu_si512(ptr10);
    __m512i input11 = _mm512_loadu_si512(ptr11);
    __m512i input12 = _mm512_loadu_si512(ptr12);
    __m512i input13 = _mm512_loadu_si512(ptr13);
    __m512i input14 = _mm512_loadu_si512(ptr14);
    CoreSmallSort14(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12, input13, input14);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
    _mm512_storeu_si512(ptr9, input9);
    _mm512_storeu_si512(ptr10, input10);
    _mm512_storeu_si512(ptr11, input11);
    _mm512_storeu_si512(ptr12, input12);
    _mm512_storeu_si512(ptr13, input13);
    _mm512_storeu_si512(ptr14, input14);
}


inline void CoreSmallSort15(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                             __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                             __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                             __m512i& input13, __m512i& input14, __m512i& input15 ){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort7(input9, input10, input11, input12, input13, input14, input15);
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

        input9 = _mm512_max_epi32(input8, permNeigh9);
        input10 = _mm512_max_epi32(input7, permNeigh10);
        input11 = _mm512_max_epi32(input6, permNeigh11);
        input12 = _mm512_max_epi32(input5, permNeigh12);
        input13 = _mm512_max_epi32(input4, permNeigh13);
        input14 = _mm512_max_epi32(input3, permNeigh14);
        input15 = _mm512_max_epi32(input2, permNeigh15);

        input8 = _mm512_min_epi32(input8, permNeigh9);
        input7 = _mm512_min_epi32(input7, permNeigh10);
        input6 = _mm512_min_epi32(input6, permNeigh11);
        input5 = _mm512_min_epi32(input5, permNeigh12);
        input4 = _mm512_min_epi32(input4, permNeigh13);
        input3 = _mm512_min_epi32(input3, permNeigh14);
        input2 = _mm512_min_epi32(input2, permNeigh15);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd7(input9, input10, input11, input12, input13, input14, input15);
}

inline void CoreSmallSort15(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                             int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8,
                             int* __restrict__ ptr9, int* __restrict__ ptr10, int* __restrict__ ptr11, int* __restrict__ ptr12,
                             int* __restrict__ ptr13, int* __restrict__ ptr14, int* __restrict__ ptr15){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    __m512i input9 = _mm512_loadu_si512(ptr9);
    __m512i input10 = _mm512_loadu_si512(ptr10);
    __m512i input11 = _mm512_loadu_si512(ptr11);
    __m512i input12 = _mm512_loadu_si512(ptr12);
    __m512i input13 = _mm512_loadu_si512(ptr13);
    __m512i input14 = _mm512_loadu_si512(ptr14);
    __m512i input15 = _mm512_loadu_si512(ptr15);
    CoreSmallSort15(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12, input13, input14, input15);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
    _mm512_storeu_si512(ptr9, input9);
    _mm512_storeu_si512(ptr10, input10);
    _mm512_storeu_si512(ptr11, input11);
    _mm512_storeu_si512(ptr12, input12);
    _mm512_storeu_si512(ptr13, input13);
    _mm512_storeu_si512(ptr14, input14);
    _mm512_storeu_si512(ptr15, input15);
}


inline void CoreSmallSort16(__m512i& input, __m512i& input2, __m512i& input3, __m512i& input4,
                             __m512i& input5, __m512i& input6, __m512i& input7, __m512i& input8,
                             __m512i& input9, __m512i& input10, __m512i& input11, __m512i& input12,
                             __m512i& input13, __m512i& input14, __m512i& input15, __m512i& input16 ){
    CoreSmallSort8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallSort8(input9, input10, input11, input12, input13, input14, input15, input16);
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

        input9 = _mm512_max_epi32(input8, permNeigh9);
        input10 = _mm512_max_epi32(input7, permNeigh10);
        input11 = _mm512_max_epi32(input6, permNeigh11);
        input12 = _mm512_max_epi32(input5, permNeigh12);
        input13 = _mm512_max_epi32(input4, permNeigh13);
        input14 = _mm512_max_epi32(input3, permNeigh14);
        input15 = _mm512_max_epi32(input2, permNeigh15);
        input16 = _mm512_max_epi32(input, permNeigh16);

        input8 = _mm512_min_epi32(input8, permNeigh9);
        input7 = _mm512_min_epi32(input7, permNeigh10);
        input6 = _mm512_min_epi32(input6, permNeigh11);
        input5 = _mm512_min_epi32(input5, permNeigh12);
        input4 = _mm512_min_epi32(input4, permNeigh13);
        input3 = _mm512_min_epi32(input3, permNeigh14);
        input2 = _mm512_min_epi32(input2, permNeigh15);
        input = _mm512_min_epi32(input, permNeigh16);
    }
    CoreSmallEnd8(input, input2, input3, input4, input5, input6, input7, input8);
    CoreSmallEnd8(input9, input10, input11, input12, input13, input14, input15, input16);
}

inline void CoreSmallSort16(int* __restrict__ ptr1, int* __restrict__ ptr2, int* __restrict__ ptr3, int* __restrict__ ptr4,
                             int* __restrict__ ptr5, int* __restrict__ ptr6, int* __restrict__ ptr7, int* __restrict__ ptr8,
                             int* __restrict__ ptr9, int* __restrict__ ptr10, int* __restrict__ ptr11, int* __restrict__ ptr12,
                             int* __restrict__ ptr13, int* __restrict__ ptr14, int* __restrict__ ptr15, int* __restrict__ ptr16){
    __m512i input1 = _mm512_loadu_si512(ptr1);
    __m512i input2 = _mm512_loadu_si512(ptr2);
    __m512i input3 = _mm512_loadu_si512(ptr3);
    __m512i input4 = _mm512_loadu_si512(ptr4);
    __m512i input5 = _mm512_loadu_si512(ptr5);
    __m512i input6 = _mm512_loadu_si512(ptr6);
    __m512i input7 = _mm512_loadu_si512(ptr7);
    __m512i input8 = _mm512_loadu_si512(ptr8);
    __m512i input9 = _mm512_loadu_si512(ptr9);
    __m512i input10 = _mm512_loadu_si512(ptr10);
    __m512i input11 = _mm512_loadu_si512(ptr11);
    __m512i input12 = _mm512_loadu_si512(ptr12);
    __m512i input13 = _mm512_loadu_si512(ptr13);
    __m512i input14 = _mm512_loadu_si512(ptr14);
    __m512i input15 = _mm512_loadu_si512(ptr15);
    __m512i input16 = _mm512_loadu_si512(ptr16);
    CoreSmallSort16(input1, input2, input3, input4, input5, input6, input7, input8,
                     input9, input10, input11, input12, input13, input14, input15, input16);
    _mm512_storeu_si512(ptr1, input1);
    _mm512_storeu_si512(ptr2, input2);
    _mm512_storeu_si512(ptr3, input3);
    _mm512_storeu_si512(ptr4, input4);
    _mm512_storeu_si512(ptr5, input5);
    _mm512_storeu_si512(ptr6, input6);
    _mm512_storeu_si512(ptr7, input7);
    _mm512_storeu_si512(ptr8, input8);
    _mm512_storeu_si512(ptr9, input9);
    _mm512_storeu_si512(ptr10, input10);
    _mm512_storeu_si512(ptr11, input11);
    _mm512_storeu_si512(ptr12, input12);
    _mm512_storeu_si512(ptr13, input13);
    _mm512_storeu_si512(ptr14, input14);
    _mm512_storeu_si512(ptr15, input15);
    _mm512_storeu_si512(ptr16, input16);
}



inline void SmallSort16V(int* __restrict__ ptr, const size_t length){
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
        v1 = CoreSmallSort(v1);
        _mm512_mask_compressstoreu_epi32(ptr, 0xFFFF>>rest, v1);
    }
        break;
    case 2:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+16),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort2(v1,v2);
        _mm512_storeu_si512(ptr, v1);
        _mm512_mask_compressstoreu_epi32(ptr+16, 0xFFFF>>rest, v2);
    }
        break;
    case 3:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+32),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort3(v1,v2,v3);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_mask_compressstoreu_epi32(ptr+32, 0xFFFF>>rest, v3);
    }
        break;
    case 4:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+48),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort4(v1,v2,v3,v4);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_mask_compressstoreu_epi32(ptr+48, 0xFFFF>>rest, v4);
    }
        break;
    case 5:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+64),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort5(v1,v2,v3,v4,v5);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_mask_compressstoreu_epi32(ptr+64, 0xFFFF>>rest, v5);
    }
        break;
    case 6:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+80),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort6(v1,v2,v3,v4,v5,v6);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_mask_compressstoreu_epi32(ptr+80, 0xFFFF>>rest, v6);
    }
        break;
    case 7:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+96),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort7(v1,v2,v3,v4,v5,v6,v7);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_mask_compressstoreu_epi32(ptr+96, 0xFFFF>>rest, v7);
    }
        break;
    case 8:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+112),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort8(v1,v2,v3,v4,v5,v6,v7,v8);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_mask_compressstoreu_epi32(ptr+112, 0xFFFF>>rest, v8);
    }
        break;
    case 9:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v9 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+128),
                                     _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort9(v1,v2,v3,v4,v5,v6,v7,v8,v9);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_mask_compressstoreu_epi32(ptr+128, 0xFFFF>>rest, v9);
    }
        break;
    case 10:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v10 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+144),
                                      _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort10(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_mask_compressstoreu_epi32(ptr+144, 0xFFFF>>rest, v10);
    }
        break;
    case 11:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v11 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+160),
                                      _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort11(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_mask_compressstoreu_epi32(ptr+160, 0xFFFF>>rest, v11);
    }
        break;
    case 12:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v12 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+176),
                                      _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort12(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_mask_compressstoreu_epi32(ptr+176, 0xFFFF>>rest, v12);
    }
        break;
    case 13:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v12 = _mm512_loadu_si512(ptr+176);
        __m512i v13 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+192),
                                      _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort13(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(ptr+176, v12);
        _mm512_mask_compressstoreu_epi32(ptr+192, 0xFFFF>>rest, v13);
    }
        break;
    case 14:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v12 = _mm512_loadu_si512(ptr+176);
        __m512i v13 = _mm512_loadu_si512(ptr+192);
        __m512i v14 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+208),
                                      _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort14(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(ptr+176, v12);
        _mm512_storeu_si512(ptr+192, v13);
        _mm512_mask_compressstoreu_epi32(ptr+208, 0xFFFF>>rest, v14);
    }
        break;
    case 15:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v12 = _mm512_loadu_si512(ptr+176);
        __m512i v13 = _mm512_loadu_si512(ptr+192);
        __m512i v14 = _mm512_loadu_si512(ptr+208);
        __m512i v15 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+224),
                                      _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort15(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(ptr+176, v12);
        _mm512_storeu_si512(ptr+192, v13);
        _mm512_storeu_si512(ptr+208, v14);
        _mm512_mask_compressstoreu_epi32(ptr+224, 0xFFFF>>rest, v15);
    }
        break;
        //case 16:
    default:
    {
        __m512i v1 = _mm512_loadu_si512(ptr);
        __m512i v2 = _mm512_loadu_si512(ptr+16);
        __m512i v3 = _mm512_loadu_si512(ptr+32);
        __m512i v4 = _mm512_loadu_si512(ptr+48);
        __m512i v5 = _mm512_loadu_si512(ptr+64);
        __m512i v6 = _mm512_loadu_si512(ptr+80);
        __m512i v7 = _mm512_loadu_si512(ptr+96);
        __m512i v8 = _mm512_loadu_si512(ptr+112);
        __m512i v9 = _mm512_loadu_si512(ptr+128);
        __m512i v10 = _mm512_loadu_si512(ptr+144);
        __m512i v11 = _mm512_loadu_si512(ptr+160);
        __m512i v12 = _mm512_loadu_si512(ptr+176);
        __m512i v13 = _mm512_loadu_si512(ptr+192);
        __m512i v14 = _mm512_loadu_si512(ptr+208);
        __m512i v15 = _mm512_loadu_si512(ptr+224);
        __m512i v16 = _mm512_or_si512(_mm512_maskz_loadu_epi32(0xFFFF>>rest, ptr+240),
                                      _mm512_maskz_set1_epi32(0xFFFF<<lastVecSize, INT_MAX));
        CoreSmallSort16(v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,v16);
        _mm512_storeu_si512(ptr, v1);
        _mm512_storeu_si512(ptr+16, v2);
        _mm512_storeu_si512(ptr+32, v3);
        _mm512_storeu_si512(ptr+48, v4);
        _mm512_storeu_si512(ptr+64, v5);
        _mm512_storeu_si512(ptr+80, v6);
        _mm512_storeu_si512(ptr+96, v7);
        _mm512_storeu_si512(ptr+112, v8);
        _mm512_storeu_si512(ptr+128, v9);
        _mm512_storeu_si512(ptr+144, v10);
        _mm512_storeu_si512(ptr+160, v11);
        _mm512_storeu_si512(ptr+176, v12);
        _mm512_storeu_si512(ptr+192, v13);
        _mm512_storeu_si512(ptr+208, v14);
        _mm512_storeu_si512(ptr+224, v15);
        _mm512_mask_compressstoreu_epi32(ptr+240, 0xFFFF>>rest, v16);
    }
    }
}


////////////////////////////////////////////////////////////////////////////////
/// Partitions
////////////////////////////////////////////////////////////////////////////////

template <class SortType, class IndexType>
static inline IndexType CoreScalarPartition(SortType array[], IndexType left, IndexType right,
                                    const SortType pivot){

    for(; left <= right
         && array[left] <= pivot ; ++left){
    }

    for(IndexType idx = left ; idx <= right ; ++idx){
        if( array[idx] <= pivot ){
            std::swap(array[idx],array[left]);
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
static inline IndexType Partition512(int array[], IndexType left, IndexType right,
                                         const int pivot){
    const IndexType S = 16;//(512/8)/sizeof(int);

    if(right-left+1 < 2*S){
        return CoreScalarPartition<int,IndexType>(array, left, right, pivot);
    }

    __m512i pivotvec = _mm512_set1_epi32(pivot);

    __m512i left_val = _mm512_loadu_si512(&array[left]);
    IndexType left_w = left;
    left += S;

    IndexType right_w = right+1;
    right -= S-1;
    __m512i right_val = _mm512_loadu_si512(&array[right]);

    while(left + S <= right){
        const IndexType free_left = left - left_w;
        const IndexType free_right = right_w - right;

        __m512i val;
        if( free_left <= free_right ){
            val = _mm512_loadu_si512(&array[left]);
            left += S;
        }
        else{
            right -= S;
            val = _mm512_loadu_si512(&array[right]);
        }

        __mmask16 mask = _mm512_cmp_epi32_mask(val, pivotvec, _MM_CMPINT_LE);

        const IndexType nb_low = popcount(mask);
        const IndexType nb_high = S-nb_low;

        _mm512_mask_compressstoreu_epi32(&array[left_w],mask,val);
        left_w += nb_low;

        right_w -= nb_high;
        _mm512_mask_compressstoreu_epi32(&array[right_w],~mask,val);
    }

    {
        const IndexType remaining = right - left;
        __m512i val = _mm512_loadu_si512(&array[left]);
        left = right;

        __mmask16 mask = _mm512_cmp_epi32_mask(val, pivotvec, _MM_CMPINT_LE);

        __mmask16 mask_low = mask & ~(0xFFFF << remaining);
        __mmask16 mask_high = (~mask) & ~(0xFFFF << remaining);

        const IndexType nb_low = popcount(mask_low);
        const IndexType nb_high = popcount(mask_high);

        _mm512_mask_compressstoreu_epi32(&array[left_w],mask_low,val);
        left_w += nb_low;

        right_w -= nb_high;
        _mm512_mask_compressstoreu_epi32(&array[right_w],mask_high,val);
    }
    {
        __mmask16 mask = _mm512_cmp_epi32_mask(left_val, pivotvec, _MM_CMPINT_LE);

        const IndexType nb_low = popcount(mask);
        const IndexType nb_high = S-nb_low;

        _mm512_mask_compressstoreu_epi32(&array[left_w],mask,left_val);
        left_w += nb_low;

        right_w -= nb_high;
        _mm512_mask_compressstoreu_epi32(&array[right_w],~mask,left_val);
    }
    {
        __mmask16 mask = _mm512_cmp_epi32_mask(right_val, pivotvec, _MM_CMPINT_LE);

        const IndexType nb_low = popcount(mask);
        const IndexType nb_high = S-nb_low;

        _mm512_mask_compressstoreu_epi32(&array[left_w],mask,right_val);
        left_w += nb_low;

        right_w -= nb_high;
        _mm512_mask_compressstoreu_epi32(&array[right_w],~mask,right_val);
    }
    return left_w;
}


template <class IndexType>
static inline IndexType Partition512(double array[], IndexType left, IndexType right,
                                         const double pivot){
    const IndexType S = 8;//(512/8)/sizeof(double);

    if(right-left+1 < 2*S){
        return CoreScalarPartition<double,IndexType>(array, left, right, pivot);
    }

    __m512d pivotvec = _mm512_set1_pd(pivot);

    __m512d left_val = _mm512_loadu_pd(&array[left]);
    IndexType left_w = left;
    left += S;

    IndexType right_w = right+1;
    right -= S-1;
    __m512d right_val = _mm512_loadu_pd(&array[right]);

    while(left + S <= right){
        const IndexType free_left = left - left_w;
        const IndexType free_right = right_w - right;

        __m512d val;
        if( free_left <= free_right ){
            val = _mm512_loadu_pd(&array[left]);
            left += S;
        }
        else{
            right -= S;
            val = _mm512_loadu_pd(&array[right]);
        }

        __mmask8 mask = _mm512_cmp_pd_mask(val, pivotvec, _CMP_LE_OQ);

        const IndexType nb_low = popcount(mask);
        const IndexType nb_high = S-nb_low;

        _mm512_mask_compressstoreu_pd(&array[left_w],mask,val);
        left_w += nb_low;

        right_w -= nb_high;
        _mm512_mask_compressstoreu_pd(&array[right_w],~mask,val);
    }

    {
        const IndexType remaining = right - left;
        __m512d val = _mm512_loadu_pd(&array[left]);
        left = right;

        __mmask8 mask = _mm512_cmp_pd_mask(val, pivotvec, _CMP_LE_OQ);

        __mmask8 mask_low = mask & ~(0xFF << remaining);
        __mmask8 mask_high = (~mask) & ~(0xFF << remaining);

        const IndexType nb_low = popcount(mask_low);
        const IndexType nb_high = popcount(mask_high);

        _mm512_mask_compressstoreu_pd(&array[left_w],mask_low,val);
        left_w += nb_low;

        right_w -= nb_high;
        _mm512_mask_compressstoreu_pd(&array[right_w],mask_high,val);
    }
    {
        __mmask8 mask = _mm512_cmp_pd_mask(left_val, pivotvec, _CMP_LE_OQ);

        const IndexType nb_low = popcount(mask);
        const IndexType nb_high = S-nb_low;

        _mm512_mask_compressstoreu_pd(&array[left_w],mask,left_val);
        left_w += nb_low;

        right_w -= nb_high;
        _mm512_mask_compressstoreu_pd(&array[right_w],~mask,left_val);
    }
    {
        __mmask8 mask = _mm512_cmp_pd_mask(right_val, pivotvec, _CMP_LE_OQ);

        const IndexType nb_low = popcount(mask);
        const IndexType nb_high = S-nb_low;

        _mm512_mask_compressstoreu_pd(&array[left_w],mask,right_val);
        left_w += nb_low;

        right_w -= nb_high;
        _mm512_mask_compressstoreu_pd(&array[right_w],~mask,right_val);
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
static inline IndexType CoreSortPivotPartition(SortType array[], const IndexType left, const IndexType right){
    if(right-left > 1){
        const IndexType pivotIdx = CoreSortGetPivot(array, left, right);
        std::swap(array[pivotIdx], array[right]);
        const IndexType part = Partition512(array, left, right-1, array[right]);
        std::swap(array[part], array[right]);
        return part;
    }
    return left;
}

template <class SortType, class IndexType = size_t>
static inline IndexType CoreSortPartition(SortType array[], const IndexType left, const IndexType right,
                                  const SortType pivot){
    return  Partition512(array, left, right, pivot);
}

template <class SortType, class IndexType = size_t>
static void CoreSort(SortType array[], const IndexType left, const IndexType right){
    static const int SortLimite = 16*64/sizeof(SortType);
    if(right-left < SortLimite){
        SmallSort16V(array+left, right-left+1);
    }
    else{
        const IndexType part = CoreSortPivotPartition<SortType,IndexType>(array, left, right);
        if(part+1 < right) CoreSort<SortType,IndexType>(array,part+1,right);
        if(part && left < part-1)  CoreSort<SortType,IndexType>(array,left,part - 1);
    }
}

template <class SortType, class IndexType = size_t>
static inline void Sort(SortType array[], const IndexType size){
    CoreSort<SortType,IndexType>(array, 0, size-1);
}


#if defined(_OPENMP)

template <class SortType, class IndexType = size_t>
static inline void CoreSortTaskPartition(SortType array[], const IndexType left, const IndexType right, const int deep){
    static const int SortLimite = 16*64/sizeof(SortType);
    if(right-left < SortLimite){
        SmallSort16V(array+left, right-left+1);
    }
    else{
        const IndexType part = CoreSortPivotPartition<SortType,IndexType>(array, left, right);
        if( deep ){
            // default(none) has been removed for clang compatibility
            if(part+1 < right){
                #pragma omp task default(shared) firstprivate(array, part, right, deep)
                CoreSortTaskPartition<SortType,IndexType>(array,part+1,right, deep - 1);
            }
            // not task needed, let the current thread compute it
            if(part && left < part-1)  CoreSortTaskPartition<SortType,IndexType>(array,left,part - 1, deep - 1);
        }
        else {
            if(part+1 < right) CoreSort<SortType,IndexType>(array,part+1,right);
            if(part && left < part-1)  CoreSort<SortType,IndexType>(array,left,part - 1);
        }
    }
}

template <class SortType, class IndexType = size_t>
static inline void SortOmpPartition(SortType array[], const IndexType size){
    int deep = 0;
    while( (IndexType(1) << deep) < size ) deep += 1;

#pragma omp parallel
    {
#pragma omp master
        {
            CoreSortTaskPartition<SortType,IndexType>(array, 0, size - 1 , deep);
        }
    }
}

template <class SortType, class IndexType = size_t>
static inline void SortOmpMerge(SortType array[], const IndexType size){
    const long int MAX_THREADS = 128;
    const long int LOG2_MAX_THREADS = 7;
    int done[LOG2_MAX_THREADS][MAX_THREADS] = {0};

    assert(((omp_get_num_threads()-1) & omp_get_num_threads()) == 0); // Must be power of 2

#pragma omp parallel
    {
        const IndexType chunk = (size + omp_get_num_threads() - 1)/omp_get_num_threads();
        {
            const IndexType first = std::min(IndexType(size), chunk * omp_get_thread_num());
            const IndexType last = std::min(IndexType(size), chunk * (omp_get_thread_num() + 1));

            if(first < last) CoreSort<SortType,IndexType>(array,first,last-1);
        }
        {
            int& mydone = done[0][omp_get_thread_num()];
            #pragma omp atomic write
            mydone = 1;
        }

        int level = 1;
        while(!(omp_get_thread_num() & (1<<(level-1))) && (1<<level) <= omp_get_num_threads()){
            while(true){
                int otherIsDone;
                #pragma omp atomic read
                otherIsDone = done[level-1][(omp_get_thread_num()>>(level-1))+1];
                if(otherIsDone){
                    break;
                }
            }

            const IndexType nbOriginalPartsToMerge = (1 << level);
            const IndexType first = std::min(size, (omp_get_thread_num())*chunk);
            const IndexType middle = std::min(size, first + (nbOriginalPartsToMerge/2)*chunk);
            const IndexType last = std::min(size, first + nbOriginalPartsToMerge*chunk);

            std::inplace_merge(&array[first],
                               &array[middle],
                               &array[last]);

            {
                int& mydone = done[level][(omp_get_thread_num()>>level)];
                #pragma omp atomic write
                mydone = 1;
            }

            level += 1;
        }
    }
}

template <class SortType, class IndexType = size_t>
static inline void SortOmpMergeDeps(SortType array[], const IndexType size){
    int nbParts = 1;
    while(nbParts < omp_get_max_threads()){
        nbParts <<= 1;
    }
#pragma omp parallel
    {
#pragma omp master
        {
            const IndexType chunk = (size + nbParts - 1)/nbParts;
            for(long int idxPart = 0 ; idxPart < nbParts ; ++idxPart){
                const IndexType first = std::min(IndexType(size), chunk * idxPart);
                const IndexType last = std::min(IndexType(size), chunk * (idxPart + 1));

                if(first < last){
#pragma omp task depend(inout:array[first]) firstprivate(first, last)
                    CoreSort<SortType,IndexType>(array,first,last-1);
                }
            }

            int level = 1;
            while((1 << level) <= nbParts){
                const IndexType nbPartsAtLevel = nbParts/(1<<level);
                const IndexType nbOriginalPartsToMerge = (1 << level);

                for(IndexType idxPart = 0 ; idxPart < nbPartsAtLevel ; ++idxPart){
                    const IndexType first = std::min(size, (idxPart * nbOriginalPartsToMerge)*chunk);
                    const IndexType middle = std::min(size, first + (nbOriginalPartsToMerge/2)*chunk);
                    const IndexType last = std::min(size, first + nbOriginalPartsToMerge*chunk);

    #pragma omp task depend(inout:array[first],array[middle]) firstprivate(first, middle,last)
                    std::inplace_merge(&array[first],
                                       &array[middle],
                                       &array[last]);
                }
                level += 1;
            }

#pragma omp taskwait
        }
    }
}

template <class SortType, class IndexType = size_t>
static inline void SortOmpParMerge(SortType array[], const IndexType size){
    if(size < omp_get_max_threads()){
        CoreSort<SortType,IndexType>(array,0,size-1);
        return;
    }

    const long int MAX_THREADS = 128;
    const long int LOG2_MAX_THREADS = 7;
    int done[LOG2_MAX_THREADS][MAX_THREADS] = {0};

    ParallelInplace::WorkingInterval<SortType> intervals[MAX_THREADS] = {0};
    int barrier[MAX_THREADS] = {0};

    assert(((omp_get_num_threads()-1) & omp_get_num_threads()) == 0); // Must be power of 2

#pragma omp parallel
    {
        const IndexType chunk = (size + omp_get_num_threads() - 1)/omp_get_num_threads();
        {
            const IndexType first = std::min(IndexType(size), chunk * omp_get_thread_num());
            const IndexType last = std::min(IndexType(size), chunk * (omp_get_thread_num() + 1));

            if(first < last) CoreSort<SortType,IndexType>(array,first,last-1);
        }
        {
            int& mydone = done[0][omp_get_thread_num()];
            #pragma omp atomic write
            mydone = 1;
        }

        int level = 1;
        while((1<<level) <= omp_get_num_threads()){
            const bool threadInCharge = (((omp_get_thread_num()>>level)<<level) == omp_get_thread_num());
            const IndexType firstThread = ((omp_get_thread_num() >> level) << level);
            assert(threadInCharge == false || firstThread == omp_get_thread_num());

            {
                const IndexType firstThreadPreviousLevel = ((omp_get_thread_num() >> (level-1)) << (level-1));
                const IndexType previousPartToWait = (firstThreadPreviousLevel >> (level-1)) +
                        (firstThread == firstThreadPreviousLevel ? 1 : -1);

                while(true){
                    int otherIsDone;
                    #pragma omp atomic read
                    otherIsDone = done[level-1][previousPartToWait];
                    if(otherIsDone){
                        break;
                    }
                }
            }

            const IndexType nbOriginalPartsToMerge = (1 << level);
            const IndexType numThreadsInvolved = nbOriginalPartsToMerge;
            const IndexType first = std::min(size, (firstThread)*chunk);
            const IndexType middle = std::min(size, first + (nbOriginalPartsToMerge/2)*chunk);
            const IndexType last = std::min(size, first + nbOriginalPartsToMerge*chunk);

            ParallelInplace::parallelMergeInPlace(&array[first], last-first, middle-first,
                                 numThreadsInvolved, firstThread,
                                 intervals, barrier);

            if(threadInCharge){
                int& mydone = done[level][(omp_get_thread_num()>>level)];
                #pragma omp atomic write
                mydone = 1;
            }
            level += 1;
        }
    }
}

#endif

}


#endif
