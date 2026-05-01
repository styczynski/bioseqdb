#pragma once

#include <cstdint>

#ifdef IPP_RADIX_IS_PRESENT_

namespace simd_sort::ipp_radix {

#include "ipp.h"  // IWYU pragma: keep IWYU pragma: export
#include "ipp/ippdefs.h"
#include "ipp/ipps.h"

inline void sortAsc(uint8_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp8u, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_8u_I((Ipp8u *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortAsc(uint16_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp16u, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_16u_I((Ipp16u *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortAsc(int16_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp16s, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_16s_I((Ipp16s *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortAsc(uint32_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp32u, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_32u_I((Ipp32u *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortAsc(int32_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp32s, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_32s_I((Ipp32s *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortAsc(float *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp32f, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_32f_I((Ipp32f *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortAsc(uint64_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp64u, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_64u_I((Ipp64u *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortAsc(int64_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp64s, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_64s_I((Ipp64s *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortAsc(double *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp64f, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixAscend_64f_I((Ipp64f *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(uint8_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp8u, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_8u_I((Ipp8u *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(uint16_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp16u, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_16u_I((Ipp16u *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(int16_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp16s, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_16s_I((Ipp16s *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(uint32_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp32u, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_32u_I((Ipp32u *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(int32_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp32s, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_32s_I((Ipp32s *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(float *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp32f, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_32f_I((Ipp32f *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(uint64_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp64u, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_64u_I((Ipp64u *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(int64_t *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp64s, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_64s_I((Ipp64s *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

inline void sortDesc(double *pSrcDst, int len) {
  int pBufferSize;
  ippsSortRadixGetBufferSize(len, IppDataType::ipp64f, &pBufferSize);
  Ipp8u *pBuffer = new Ipp8u[pBufferSize];
  ippsSortRadixDescend_64f_I((Ipp64f *)pSrcDst, len, pBuffer);
  delete[] pBuffer;
}

template <bool Up = true, typename K>
inline void sort(K *pSrcDst, int len) {
  if constexpr (Up) {
    sortAsc(pSrcDst, len);
  } else {
    sortDesc(pSrcDst, len);
  }
}

}  // namespace simd_sort::ipp_radix

#endif  // IPP_RADIX_IS_PRESENT_
