//=============================================================================
//
// Generic AVX-512 implementation of MSB Radix Sort for sorting datasets
// with a key and an arbitrary number of payloads.
//
// Single header file version that has been stripped of classes and functions
// that were only needed for the benchmarking and testing.
//
// By Jonas Richard Keller, 2024.
// This code is licensed under the MIT license.
//
// This code was written for the bachelor's thesis "SIMD Implementation of
// MSB Radix Sort with Separate Key and Payload Datastreams" at the
// Bielefeld University.
//
// All source code and the accompanying thesis can be found at:
// https://github.com/jonicho/simd-radix-sort
//
// The code was heavily inspired by the generic radix sort implementation
// by Prof. Dr.-Ing. Ralf Möller included in the T-SIMD library:
// https://www.ti.uni-bielefeld.de/html/people/moeller/tsimd_warpingsimd.html
//
//
// USAGE:
//
// To sort a dataset, call the sort function like the following:
//
// simd_sort::radix_sort::sort(num, keyArray, payloadArrays...);
//
// where num is the number of elements in the dataset, keyArray is a pointer
// to the key array, and payloadArrays are the pointers to the payload arrays.
//
// Or, if key and payload datastreams are combined into a single array, call
// the sort function like the following:
//
// simd_sort::radix_sort::sort(num, (simd_sort::DataElement<K, Ps...> *)
// combinedArray);
//
// where num is the number of elements in the dataset, combinedArray is a
// pointer to the combined array and K and Ps... are the types of the key and
// the payloads.
//
// By default the sort function sorts in ascending order. To sort in descending
// order, call the sort function with the template parameter "Up" set to false:
// simd_sort::radix_sort::sort<false>(num, keyArray, payloadArrays...);
// or
// simd_sort::radix_sort::sort<false>(num, (simd_sort::DataElement<K, Ps...> *)
// combinedArray);
//
// For compilation with gcc, compile with the following flags:
// -mavx512f -mavx512bw -mavx512dq -mavx512vl -mavx512vbmi -mavx512vbmi2
//
// The library requires at least C++20.
//
//=============================================================================

#pragma once

#ifdef __AVX512F__

#include <emmintrin.h>
#include <immintrin.h>
#include <popcntintrin.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>

namespace simd_sort {

template <typename>
inline constexpr bool always_false_v = false;

using SortIndex = ssize_t;

template <std::size_t Bytes>
struct LargeUInt {
  static_assert(Bytes > 8, "Bytes must be larger than 8");
  static_assert((Bytes & (Bytes - 1)) == 0, "Bytes must be a power of 2");
  uint64_t data[Bytes / 8];

  LargeUInt() = default;
  LargeUInt(const LargeUInt &) = default;
  LargeUInt &operator=(const LargeUInt &) = default;

  LargeUInt(const uint64_t x) {
    data[0] = x;
    for (std::size_t i = 1; i < Bytes / 8; i++) {
      data[i] = 0;
    }
  }
};

namespace internal {

template <std::size_t Bytes>
struct UInt;

template <>
struct UInt<1> {
  using type = uint8_t;
};

template <>
struct UInt<2> {
  using type = uint16_t;
};

template <>
struct UInt<4> {
  using type = uint32_t;
};

template <>
struct UInt<8> {
  using type = uint64_t;
};

template <std::size_t Bytes>
struct UInt {
  using type = LargeUInt<Bytes>;
};

}  // namespace internal

template <std::size_t Bytes>
using UInt = typename internal::UInt<Bytes>::type;

template <std::size_t Bytes>
UInt<Bytes> setBit(const std::size_t n) {
  if constexpr (Bytes <= 8) {
    return UInt<Bytes>(1ULL << n);
  } else {
    UInt<Bytes> x;
    x.data[n / 64] = 1ULL << (n % 64);
    return x;
  }
}

template <std::size_t X>
inline constexpr bool is_power_of_two = X > 0 && (X & (X - 1)) == 0;

struct CmpSorterInsertionSort {
  static std::string name() { return "CmpSorterInsertionSort"; }

  template <bool Up, typename K, typename... Ps>
  static inline void sort(const SortIndex left, const SortIndex right,
                          K *const keys, Ps *const... payloads) {
    for (SortIndex i = left + 1; i <= right; i++) {
      const K key = keys[i];
      const std::tuple<Ps...> payload = std::make_tuple(payloads[i]...);
      SortIndex j = i;
      while (j > left && (Up ? key < keys[j - 1] : key > keys[j - 1])) {
        keys[j] = keys[j - 1];
        ((payloads[j] = payloads[j - 1]), ...);
        j--;
      }
      keys[j] = key;
      std::apply([&](const Ps &...p) { ((payloads[j] = p), ...); }, payload);
    }
  }
};

template <typename K, typename... Ps>
struct DataElement {
  K key;
  std::tuple<Ps...> payloads;
  bool operator<(const DataElement &other) const { return key < other.key; }
  bool operator>(const DataElement &other) const { return key > other.key; }
};

// specialization of DataElement for no payloads, because an empty
// tuple still uses one byte of space, we don't want that
template <typename K>
struct DataElement<K> {
  K key;
  bool operator<(const DataElement &other) const { return key < other.key; }
  bool operator>(const DataElement &other) const { return key > other.key; }
};

namespace simd {
// simd library inspired by
// the T-SIMD library written by Prof. Dr.-Ing. Ralf Möller:
// https://www.ti.uni-bielefeld.de/html/people/moeller/tsimd_warpingsimd.html

namespace internal {
template <typename T, std::size_t Bytes, typename = void>
struct MMRegType {
  static_assert(always_false_v<T>, "Unsupported type or number of bytes");
};
template <typename T>
struct MMRegType<T, 64> {
  using type = __m512i;
};
template <typename T>
struct MMRegType<T, 32> {
  using type = __m256i;
};
template <typename T>
struct MMRegType<T, 16> {
  using type = __m128i;
};
template <typename T>
struct MMRegType<T, 8> {
  using type = __m128i;
};
}  // namespace internal

template <typename T, std::size_t Bytes>
using MMRegType = typename internal::MMRegType<T, Bytes>::type;

template <typename T, std::size_t Bytes = 64, typename = void>
struct Vec;

template <typename T, std::size_t Bytes>
struct Vec<T, Bytes,
           std::enable_if_t<(Bytes <= 64) && is_power_of_two<Bytes>>> {
  MMRegType<T, Bytes> mmReg;
  static constexpr std::size_t numElems = Bytes / sizeof(T);
  Vec() = default;
  Vec(const MMRegType<T, Bytes> x) : mmReg(x) {}
  Vec &operator=(const MMRegType<T, Bytes> x) {
    mmReg = x;
    return *this;
  }
  operator MMRegType<T, Bytes>() const { return mmReg; }
};

template <typename T, std::size_t Bytes>
struct Vec<T, Bytes, std::enable_if_t<(Bytes > 64) && is_power_of_two<Bytes>>> {
  MMRegType<T, 64> mmReg[Bytes / 64];
  static constexpr std::size_t numElems = Bytes / sizeof(T);
  static constexpr std::size_t numRegs = Bytes / 64;
  Vec() = default;
  MMRegType<T, 64> &operator[](const std::size_t i) { return mmReg[i]; }
  const MMRegType<T, 64> &operator[](const std::size_t i) const {
    return mmReg[i];
  }
};

template <std::size_t Size>
struct _MaskType {
  static_assert(always_false_v<_MaskType<Size>>, "Unsupported mask size");
};
template <>
struct _MaskType<64> {
  using type = __mmask64;
};
template <>
struct _MaskType<32> {
  using type = __mmask32;
};
template <>
struct _MaskType<16> {
  using type = __mmask16;
};
template <>
struct _MaskType<8> {
  using type = __mmask8;
};
template <>
struct _MaskType<4> {
  using type = __mmask8;
};
template <>
struct _MaskType<2> {
  using type = __mmask8;
};
template <>
struct _MaskType<1> {
  using type = __mmask8;
};

template <std::size_t Size>
using MaskType = typename _MaskType<Size>::type;

template <std::size_t Size>
struct Mask {
  static constexpr std::size_t size = Size;
  MaskType<Size> k;
  Mask() = default;
  Mask(const MaskType<Size> &x) : k(x) {}
  Mask &operator=(const MaskType<Size> &x) {
    k = x;
    return *this;
  }
  operator MaskType<Size>() const { return k; }
};

template <typename Tdst, typename Tsrc, std::size_t Bytes>
static inline Vec<Tdst, Bytes> reinterpret(const Vec<Tsrc, Bytes> &vec) {
  return reinterpret_cast<const Vec<Tdst, Bytes> &>(vec);
}

template <typename T, std::size_t Bytes = 64>
static inline Vec<T, Bytes> set_bit(const std::size_t bitNo) {
  if constexpr (Bytes == 64) {
    if constexpr (sizeof(T) == 1) {
      return _mm512_set1_epi8(uint8_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 2) {
      return _mm512_set1_epi16(uint16_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 4) {
      return _mm512_set1_epi32(uint32_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 8) {
      return _mm512_set1_epi64(uint64_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 16) {
      return _mm512_maskz_set1_epi64(0b01010101 << (bitNo / 64),
                                     uint64_t(1) << (bitNo % 64));
    } else if constexpr (sizeof(T) == 32) {
      return _mm512_maskz_set1_epi64(0b00010001 << (bitNo / 64),
                                     uint64_t(1) << (bitNo % 64));
    } else if constexpr (sizeof(T) == 64) {
      return _mm512_maskz_set1_epi64(0b00000001 << (bitNo / 64),
                                     uint64_t(1) << (bitNo % 64));
    } else {
      static_assert(always_false_v<T>, "Unsupported type size");
    }
  } else if constexpr (Bytes == 32) {
    if constexpr (sizeof(T) == 1) {
      return _mm256_set1_epi8(uint8_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 2) {
      return _mm256_set1_epi16(uint16_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 4) {
      return _mm256_set1_epi32(uint32_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 8) {
      return _mm256_set1_epi64x(uint64_t(1) << bitNo);
#ifdef __AVX512VL__
    } else if constexpr (sizeof(T) == 16) {
      return _mm256_maskz_set1_epi64(0b0101 << (bitNo / 64),
                                     uint64_t(1) << (bitNo % 64));
    } else if constexpr (sizeof(T) == 32) {
      return _mm256_maskz_set1_epi64(0b0001 << (bitNo / 64),
                                     uint64_t(1) << (bitNo % 64));
#endif  // __AVX512VL__
    } else {
      static_assert(always_false_v<T>, "Unsupported type size");
    }
  } else if constexpr (Bytes == 16 || Bytes == 8) {
    if constexpr (sizeof(T) == 1) {
      return _mm_set1_epi8(uint8_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 2) {
      return _mm_set1_epi16(uint16_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 4) {
      return _mm_set1_epi32(uint32_t(1) << bitNo);
    } else if constexpr (sizeof(T) == 8) {
      return _mm_set1_epi64x(uint64_t(1) << bitNo);
#ifdef __AVX512VL__
    } else if constexpr (sizeof(T) == 16) {
      return _mm_maskz_set1_epi64(0b01 << (bitNo / 64), uint64_t(1)
                                                            << (bitNo % 64));
#endif  // __AVX512VL__
    } else {
      static_assert(always_false_v<T>, "Unsupported type size");
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <std::size_t Bytes = 64, typename T>
static inline Vec<T, Bytes> loadu(const T *const p) {
  if constexpr (Bytes == 64) {
    return _mm512_loadu_si512(p);
  } else if constexpr (Bytes == 32) {
    return _mm256_loadu_si256((__m256i_u *)p);
  } else if constexpr (Bytes == 16) {
    return _mm_loadu_si128((__m128i_u *)p);
#if defined(__AVX512BW__) && defined(__AVX512VL__)
  } else if constexpr (Bytes == 8) {
    if constexpr (sizeof(T) == 1) {
      return _mm_maskz_loadu_epi8(0xff, p);
    } else {
      static_assert(always_false_v<T>, "Unsupported type size");
    }
#endif  // __AVX512BW__ && __AVX512VL__
  } else if constexpr (Bytes == 128 || Bytes == 256 || Bytes == 512) {
    Vec<T, Bytes> result;
    for (std::size_t i = 0; i < Vec<T, Bytes>::numRegs; i++) {
      result[i] = _mm512_loadu_si512(p + i * Vec<T, 64>::numElems);
    }
    return result;
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <std::size_t Bytes = 64, typename T>
static inline Vec<T, Bytes> maskz_loadu(const Mask<Vec<T, Bytes>::numElems> m,
                                        const T *const p) {
  if constexpr (Bytes == 64) {
#ifdef __AVX512BW__
    if constexpr (sizeof(T) == 1) {
      return _mm512_maskz_loadu_epi8(m, p);
    } else if constexpr (sizeof(T) == 2) {
      return _mm512_maskz_loadu_epi16(m, p);
    } else
#endif  // __AVX512BW__
      if constexpr (sizeof(T) == 4) {
        return _mm512_maskz_loadu_epi32(m, p);
      } else if constexpr (sizeof(T) <= 64) {
        return _mm512_maskz_loadu_epi64(m, p);
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
#ifdef __AVX512VL__
  } else if constexpr (Bytes == 32) {
#ifdef __AVX512BW__
    if constexpr (sizeof(T) == 1) {
      return _mm256_maskz_loadu_epi8(m, (__m256i_u *)p);
    } else if constexpr (sizeof(T) == 2) {
      return _mm256_maskz_loadu_epi16(m, (__m256i_u *)p);
    } else
#endif  // __AVX512BW__
      if constexpr (sizeof(T) == 4) {
        return _mm256_maskz_loadu_epi32(m, (__m256i_u *)p);
      } else if constexpr (sizeof(T) <= 32) {
        return _mm256_maskz_loadu_epi64(m, (__m256i_u *)p);
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
  } else if constexpr (Bytes == 16) {
#ifdef __AVX512BW__
    if constexpr (sizeof(T) == 1) {
      return _mm_maskz_loadu_epi8(m, p);
    } else if constexpr (sizeof(T) == 2) {
      return _mm_maskz_loadu_epi16(m, p);
    } else
#endif  // __AVX512BW__
      if constexpr (sizeof(T) == 4) {
        return _mm_maskz_loadu_epi32(m, p);
      } else if constexpr (sizeof(T) <= 16) {
        return _mm_maskz_loadu_epi64(m, p);
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
  } else if constexpr (Bytes == 8) {
#ifdef __AVX512BW__
    if constexpr (sizeof(T) == 1) {
      return _mm_maskz_loadu_epi8(m & 0xff, p);
    } else
#endif  // __AVX512BW__
    {
      static_assert(always_false_v<T>, "Unsupported type size");
    }
#endif  // __AVX512VL__
  } else if constexpr (Bytes == 128 || Bytes == 256 || Bytes == 512) {
    Vec<T, Bytes> result;
    Mask<Vec<T, Bytes>::numElems> mask = m;
    for (std::size_t i = 0; i < Vec<T, Bytes>::numRegs; i++) {
#ifdef __AVX512BW__
      if constexpr (sizeof(T) == 1) {
        result[i] = _mm512_maskz_loadu_epi8(mask, p + i * Vec<T, 64>::numElems);
      } else if constexpr (sizeof(T) == 2) {
        result[i] =
            _mm512_maskz_loadu_epi16(mask, p + i * Vec<T, 64>::numElems);
      } else
#endif  // __AVX512BW__
        if constexpr (sizeof(T) == 4) {
          result[i] =
              _mm512_maskz_loadu_epi32(mask, p + i * Vec<T, 64>::numElems);
        } else if constexpr (sizeof(T) <= 64) {
          result[i] =
              _mm512_maskz_loadu_epi64(mask, p + i * Vec<T, 64>::numElems);
        } else {
          static_assert(always_false_v<T>, "Unsupported type size");
        }
      mask = kshiftr(mask, Vec<T, 64>::numElems);
    }
    return result;
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <typename T, std::size_t Bytes>
static inline void mask_compressstoreu(T *const p,
                                       const Mask<Vec<T, Bytes>::numElems> m,
                                       const Vec<T, Bytes> v) {
  if constexpr (Bytes == 64) {
#ifdef __AVX512VBMI2__
    if constexpr (sizeof(T) == 1) {
      _mm512_mask_compressstoreu_epi8(p, m, v);
    } else if constexpr (sizeof(T) == 2) {
      _mm512_mask_compressstoreu_epi16(p, m, v);
    } else
#endif  // __AVX512VBMI2__
      if constexpr (sizeof(T) == 4) {
        _mm512_mask_compressstoreu_epi32(p, m, v);
      } else if constexpr (sizeof(T) <= 64) {
        _mm512_mask_compressstoreu_epi64(p, m, v);
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
#ifdef __AVX512VL__
  } else if constexpr (Bytes == 32) {
#ifdef __AVX512VBMI2__
    if constexpr (sizeof(T) == 1) {
      _mm256_mask_compressstoreu_epi8(p, m, v);
    } else if constexpr (sizeof(T) == 2) {
      _mm256_mask_compressstoreu_epi16(p, m, v);
    } else
#endif  // __AVX512VBMI2__
      if constexpr (sizeof(T) == 4) {
        _mm256_mask_compressstoreu_epi32(p, m, v);
      } else if constexpr (sizeof(T) <= 32) {
        _mm256_mask_compressstoreu_epi64(p, m, v);
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
  } else if constexpr (Bytes == 16) {
#ifdef __AVX512VBMI2__
    if constexpr (sizeof(T) == 1) {
      _mm_mask_compressstoreu_epi8(p, m, v);
    } else if constexpr (sizeof(T) == 2) {
      _mm_mask_compressstoreu_epi16(p, m, v);
    } else
#endif  // __AVX512VBMI2__
      if constexpr (sizeof(T) == 4) {
        _mm_mask_compressstoreu_epi32(p, m, v);
      } else if constexpr (sizeof(T) <= 16) {
        _mm_mask_compressstoreu_epi64(p, m, v);
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
  } else if constexpr (Bytes == 8) {
#ifdef __AVX512VBMI2__
    if constexpr (sizeof(T) == 1) {
      _mm_mask_compressstoreu_epi8(p, m & 0xff, v);
    } else
#endif  // __AVX512VBMI2__
    {
      static_assert(always_false_v<T>, "Unsupported type size");
    }
#endif  // __AVX512VL__
  } else if constexpr (Bytes == 128 || Bytes == 256 || Bytes == 512) {
    Mask<Vec<T, Bytes>::numElems> mask = m;
    T *pointer = p;
    for (std::size_t i = 0; i < Vec<T, Bytes>::numRegs; i++) {
      if constexpr (sizeof(T) == 2) {
        _mm512_mask_compressstoreu_epi16(pointer, mask, v[i]);
      } else if constexpr (sizeof(T) == 4) {
        _mm512_mask_compressstoreu_epi32(pointer, mask, v[i]);
      } else if constexpr (sizeof(T) <= 64) {
        _mm512_mask_compressstoreu_epi64(pointer, mask, v[i]);
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
      pointer += kpopcnt((Mask<Vec<T, 64>::numElems>)mask);
      mask = kshiftr(mask, Vec<T, 64>::numElems);
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <typename T, std::size_t Bytes>
static inline Mask<Vec<T, Bytes>::numElems> test_bit(const Vec<T, Bytes> v,
                                                     const std::size_t bitNo) {
  if constexpr (Bytes == 64) {
#ifdef __AVX512BW__
    if constexpr (sizeof(T) == 1) {
      return _mm512_test_epi8_mask(v, set_bit<T, Bytes>(bitNo));
    } else if constexpr (sizeof(T) == 2) {
      return _mm512_test_epi16_mask(v, set_bit<T, Bytes>(bitNo));
    } else
#endif  // __AVX512BW__
      if constexpr (sizeof(T) == 4) {
        return _mm512_test_epi32_mask(v, set_bit<T, Bytes>(bitNo));
      } else if constexpr (sizeof(T) == 8) {
        return _mm512_test_epi64_mask(v, set_bit<T, Bytes>(bitNo));
      } else if constexpr (sizeof(T) == 16) {
        const __mmask8 mask =
            _mm512_test_epi64_mask(v, set_bit<T, Bytes>(bitNo));
        if (bitNo < 64) {
          // mask is now 0b0a 0b 0c 0d, we want 0baa bb cc dd
          return mask | (mask << 1);
        } else {
          // mask is now 0ba0 b0 c0 d0, we want 0baa bb cc dd
          return mask | (mask >> 1);
        }
      } else if constexpr (sizeof(T) == 32) {
        const __mmask8 mask =
            _mm512_test_epi64_mask(v, set_bit<T, Bytes>(bitNo));
        if (bitNo < 64) {
          // mask is now 0b000a 000b, we want 0baaaa bbbb
          return mask | (mask << 1) | (mask << 2) | (mask << 3);
        } else if (bitNo < 128) {
          // mask is now 0b00a0 00b0, we want 0baaaa bbbb
          return (mask >> 1) | mask | (mask << 1) | (mask << 2);
        } else if (bitNo < 192) {
          // mask is now 0b0a00 0b00, we want 0baaaa bbbb
          return (mask >> 2) | (mask >> 1) | mask | (mask << 1);
        } else {
          // mask is now 0ba000 b000, we want 0baaaa bbbb
          return (mask >> 3) | (mask >> 2) | (mask >> 1) | mask;
        }
      } else if constexpr (sizeof(T) == 64) {
        const __mmask8 mask =
            _mm512_test_epi64_mask(v, set_bit<T, Bytes>(bitNo));
        // mask is now 0b0000 000a, or 0b0000 00a0, or ...
        // we want 0baaaa aaaa
        return mask == 0 ? 0 : 0xff;
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
#ifdef __AVX512VL__
  } else if constexpr (Bytes == 32) {
#ifdef __AVX512BW__
    if constexpr (sizeof(T) == 1) {
      return _mm256_test_epi8_mask(v, set_bit<T, Bytes>(bitNo));
    } else if constexpr (sizeof(T) == 2) {
      return _mm256_test_epi16_mask(v, set_bit<T, Bytes>(bitNo));
    } else
#endif  // __AVX512BW__
      if constexpr (sizeof(T) == 4) {
        return _mm256_test_epi32_mask(v, set_bit<T, Bytes>(bitNo));
      } else if constexpr (sizeof(T) == 8) {
        return _mm256_test_epi64_mask(v, set_bit<T, Bytes>(bitNo));
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
  } else if constexpr (Bytes == 16 || Bytes == 8) {
#ifdef __AVX512BW__
    if constexpr (sizeof(T) == 1) {
      return _mm_test_epi8_mask(v, set_bit<T, Bytes>(bitNo));
    } else if constexpr (sizeof(T) == 2) {
      return _mm_test_epi16_mask(v, set_bit<T, Bytes>(bitNo));
    } else
#endif  // __AVX512BW__
      if constexpr (sizeof(T) == 4) {
        return _mm_test_epi32_mask(v, set_bit<T, Bytes>(bitNo));
      } else if constexpr (sizeof(T) == 8) {
        return _mm_test_epi64_mask(v, set_bit<T, Bytes>(bitNo));
      } else {
        static_assert(always_false_v<T>, "Unsupported type size");
      }
#endif  // __AVX512VL__
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <typename KeyType, typename T, std::size_t Bytes>
static inline Mask<Vec<T, Bytes>::numElems> cmple_keys(const Vec<T, Bytes> a,
                                                       const KeyType pivot) {
  constexpr auto factor = sizeof(T) / sizeof(KeyType);
  if constexpr (Bytes == 64) {
#ifdef __AVX512BW__
    if constexpr (std::is_same_v<KeyType, uint8_t>) {
      if constexpr (factor == 1) {
        return _mm512_cmple_epu8_mask(a, _mm512_set1_epi8(pivot));
      } else if constexpr (factor == 2) {
        const auto packed_keys = _mm512_castsi512_si256(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 60,
                            58, 56, 54, 52, 50, 48, 46, 44, 42, 40, 38, 36, 34,
                            32, 30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8,
                            6, 4, 2, 0),
            a));
        return _mm256_cmple_epu8_mask(packed_keys, _mm256_set1_epi8(pivot));
      } else if constexpr (factor == 4) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 56,
                            52, 48, 44, 40, 36, 32, 28, 24, 20, 16, 12, 8, 4,
                            0),
            a));
        return _mm_cmple_epu8_mask(packed_keys, _mm_set1_epi8(pivot));
      } else if constexpr (factor == 8) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 56, 48, 40, 32, 24, 16, 8, 0),
            a));
        return __mmask8(_mm_cmple_epu8_mask(packed_keys, _mm_set1_epi8(pivot)));
      } else if constexpr (factor == 16) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 48, 48, 32, 32, 16, 16, 0, 0),
            a));
        return __mmask8(_mm_cmple_epu8_mask(packed_keys, _mm_set1_epi8(pivot)));
      } else if constexpr (factor == 32) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 32, 32, 32, 32, 0, 0, 0, 0),
            a));
        return __mmask8(_mm_cmple_epu8_mask(packed_keys, _mm_set1_epi8(pivot)));
      } else if constexpr (factor == 64) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            a));
        return __mmask8(_mm_cmple_epu8_mask(packed_keys, _mm_set1_epi8(pivot)));
      } else {
        static_assert(always_false_v<T>);
      }
    } else if constexpr (std::is_same_v<KeyType, int8_t>) {
      if constexpr (factor == 1) {
        return _mm512_cmple_epi8_mask(a, _mm512_set1_epi8(pivot));
      } else if constexpr (factor == 2) {
        const auto packed_keys = _mm512_castsi512_si256(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 60,
                            58, 56, 54, 52, 50, 48, 46, 44, 42, 40, 38, 36, 34,
                            32, 30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8,
                            6, 4, 2, 0),
            a));
        return _mm256_cmple_epi8_mask(packed_keys, _mm256_set1_epi8(pivot));
      } else if constexpr (factor == 4) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 56,
                            52, 48, 44, 40, 36, 32, 28, 24, 20, 16, 12, 8, 4,
                            0),
            a));
        return _mm_cmple_epi8_mask(packed_keys, _mm_set1_epi8(pivot));
      } else if constexpr (factor == 8) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 56, 48, 40, 32, 24, 16, 8, 0),
            a));
        return __mmask8(_mm_cmple_epi8_mask(packed_keys, _mm_set1_epi8(pivot)));
      } else if constexpr (factor == 16) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 48, 48, 32, 32, 16, 16, 0, 0),
            a));
        return __mmask8(_mm_cmple_epi8_mask(packed_keys, _mm_set1_epi8(pivot)));
      } else if constexpr (factor == 32) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 32, 32, 32, 32, 0, 0, 0, 0),
            a));
        return __mmask8(_mm_cmple_epi8_mask(packed_keys, _mm_set1_epi8(pivot)));
      } else if constexpr (factor == 64) {
        const auto packed_keys = _mm512_castsi512_si128(_mm512_permutexvar_epi8(
            _mm512_set_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            a));
        return __mmask8(_mm_cmple_epi8_mask(packed_keys, _mm_set1_epi8(pivot)));
      } else {
        static_assert(always_false_v<T>);
      }
    } else if constexpr (std::is_same_v<KeyType, uint16_t>) {
      if constexpr (factor == 1) {
        return _mm512_cmple_epu16_mask(a, _mm512_set1_epi16(pivot));
      } else if constexpr (factor == 2) {
        const auto packed_keys =
            _mm512_castsi512_si256(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8,
                                 6, 4, 2, 0),
                a));
        return _mm256_cmple_epu16_mask(packed_keys, _mm256_set1_epi16(pivot));
      } else if constexpr (factor == 4) {
        const auto packed_keys =
            _mm512_castsi512_si128(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 28, 24, 20, 16, 12, 8,
                                 4, 0),
                a));
        return _mm_cmple_epu16_mask(packed_keys, _mm_set1_epi16(pivot));
      } else if constexpr (factor == 8) {
        const auto packed_keys =
            _mm512_castsi512_si128(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 24, 24, 16, 16, 8, 8,
                                 0, 0),
                a));
        return _mm_cmple_epu16_mask(packed_keys, _mm_set1_epi16(pivot));
      } else if constexpr (factor == 16) {
        const auto packed_keys =
            _mm512_castsi512_si128(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 16, 16, 16, 16, 0, 0,
                                 0, 0),
                a));
        return _mm_cmple_epu16_mask(packed_keys, _mm_set1_epi16(pivot));
      } else if constexpr (factor == 32) {
        const auto packed_keys =
            _mm512_castsi512_si128(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0),
                a));
        return _mm_cmple_epu16_mask(packed_keys, _mm_set1_epi16(pivot));
      } else {
        static_assert(always_false_v<T>);
      }
    } else if constexpr (std::is_same_v<KeyType, int16_t>) {
      if constexpr (factor == 1) {
        return _mm512_cmple_epi16_mask(a, _mm512_set1_epi16(pivot));
      } else if constexpr (factor == 2) {
        const auto packed_keys =
            _mm512_castsi512_si256(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8,
                                 6, 4, 2, 0),
                a));
        return _mm256_cmple_epi16_mask(packed_keys, _mm256_set1_epi16(pivot));
      } else if constexpr (factor == 4) {
        const auto packed_keys =
            _mm512_castsi512_si128(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 28, 24, 20, 16, 12, 8,
                                 4, 0),
                a));
        return _mm_cmple_epi16_mask(packed_keys, _mm_set1_epi16(pivot));
      } else if constexpr (factor == 8) {
        const auto packed_keys =
            _mm512_castsi512_si128(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 24, 24, 16, 16, 8, 8,
                                 0, 0),
                a));
        return _mm_cmple_epi16_mask(packed_keys, _mm_set1_epi16(pivot));
      } else if constexpr (factor == 16) {
        const auto packed_keys =
            _mm512_castsi512_si128(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 16, 16, 16, 16, 0, 0,
                                 0, 0),
                a));
        return _mm_cmple_epi16_mask(packed_keys, _mm_set1_epi16(pivot));
      } else if constexpr (factor == 32) {
        const auto packed_keys =
            _mm512_castsi512_si128(_mm512_permutexvar_epi16(
                _mm512_set_epi16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0),
                a));
        return _mm_cmple_epi16_mask(packed_keys, _mm_set1_epi16(pivot));
      } else {
        static_assert(always_false_v<T>);
      }
    } else
#endif
        if constexpr (std::is_same_v<KeyType, uint32_t>) {
      if constexpr (factor == 1) {
        return _mm512_cmple_epu32_mask(a, _mm512_set1_epi32(pivot));
      } else if constexpr (factor == 2) {
        const auto packed_keys =
            _mm512_castsi512_si256(_mm512_permutexvar_epi32(
                _mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 14, 12, 10, 8, 6, 4, 2,
                                 0),
                a));
        return _mm256_cmple_epu32_mask(packed_keys, _mm256_set1_epi32(pivot));
      } else if constexpr (factor == 4) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0,
                                                      12, 12, 8, 8, 4, 4, 0, 0),
                                     a));
        return _mm256_cmple_epu32_mask(packed_keys, _mm256_set1_epi32(pivot));
      } else if constexpr (factor == 8) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 8,
                                                      8, 8, 8, 0, 0, 0, 0),
                                     a));
        return _mm256_cmple_epu32_mask(packed_keys, _mm256_set1_epi32(pivot));
      } else if constexpr (factor == 16) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                      0, 0, 0, 0, 0, 0, 0),
                                     a));
        return _mm256_cmple_epu32_mask(packed_keys, _mm256_set1_epi32(pivot));
      } else {
        static_assert(always_false_v<T>);
      }
    } else if constexpr (std::is_same_v<KeyType, int32_t>) {
      if constexpr (factor == 1) {
        return _mm512_cmple_epi32_mask(a, _mm512_set1_epi32(pivot));
      } else if constexpr (factor == 2) {
        const auto packed_keys =
            _mm512_castsi512_si256(_mm512_permutexvar_epi32(
                _mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 14, 12, 10, 8, 6, 4, 2,
                                 0),
                a));
        return _mm256_cmple_epi32_mask(packed_keys, _mm256_set1_epi32(pivot));
      } else if constexpr (factor == 4) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0,
                                                      12, 12, 8, 8, 4, 4, 0, 0),
                                     a));
        return _mm256_cmple_epi32_mask(packed_keys, _mm256_set1_epi32(pivot));
      } else if constexpr (factor == 8) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 8,
                                                      8, 8, 8, 0, 0, 0, 0),
                                     a));
        return _mm256_cmple_epi32_mask(packed_keys, _mm256_set1_epi32(pivot));
      } else if constexpr (factor == 16) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                      0, 0, 0, 0, 0, 0, 0),
                                     a));
        return _mm256_cmple_epi32_mask(packed_keys, _mm256_set1_epi32(pivot));
      } else {
        static_assert(always_false_v<T>);
      }
    } else if constexpr (std::is_same_v<KeyType, uint64_t>) {
      if constexpr (factor == 1) {
        return _mm512_cmple_epu64_mask(a, _mm512_set1_epi64(pivot));
      } else if constexpr (factor == 2) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(6, 6, 4, 4, 2, 2, 0, 0), a);
        return _mm512_cmple_epu64_mask(packed_keys, _mm512_set1_epi64(pivot));
      } else if constexpr (factor == 4) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(4, 4, 4, 4, 0, 0, 0, 0), a);
        return _mm512_cmple_epu64_mask(packed_keys, _mm512_set1_epi64(pivot));
      } else if constexpr (factor == 8) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(0, 0, 0, 0, 0, 0, 0, 0), a);
        return _mm512_cmple_epu64_mask(packed_keys, _mm512_set1_epi64(pivot));
      } else {
        static_assert(always_false_v<T>);
      }
    } else if constexpr (std::is_same_v<KeyType, int64_t>) {
      if constexpr (factor == 1) {
        return _mm512_cmple_epi64_mask(a, _mm512_set1_epi64(pivot));
      } else if constexpr (factor == 2) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(6, 6, 4, 4, 2, 2, 0, 0), a);
        return _mm512_cmple_epi64_mask(packed_keys, _mm512_set1_epi64(pivot));
      } else if constexpr (factor == 4) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(4, 4, 4, 4, 0, 0, 0, 0), a);
        return _mm512_cmple_epi64_mask(packed_keys, _mm512_set1_epi64(pivot));
      } else if constexpr (factor == 8) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(0, 0, 0, 0, 0, 0, 0, 0), a);
        return _mm512_cmple_epi64_mask(packed_keys, _mm512_set1_epi64(pivot));
      } else {
        static_assert(always_false_v<T>);
      }
    } else if constexpr (std::is_same_v<KeyType, float>) {
      if constexpr (factor == 1) {
        return _mm512_cmp_ps_mask(_mm512_castsi512_ps(a), _mm512_set1_ps(pivot),
                                  _CMP_LE_OS);
      } else if constexpr (factor == 2) {
        const auto packed_keys =
            _mm512_castsi512_si256(_mm512_permutexvar_epi32(
                _mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 14, 12, 10, 8, 6, 4, 2,
                                 0),
                a));
        return _mm256_cmp_ps_mask(_mm256_castsi256_ps(packed_keys),
                                  _mm256_set1_ps(pivot), _CMP_LE_OS);
      } else if constexpr (factor == 4) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0,
                                                      12, 12, 8, 8, 4, 4, 0, 0),
                                     a));
        return _mm256_cmp_ps_mask(_mm256_castsi256_ps(packed_keys),
                                  _mm256_set1_ps(pivot), _CMP_LE_OS);
      } else if constexpr (factor == 8) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 8,
                                                      8, 8, 8, 0, 0, 0, 0),
                                     a));
        return _mm256_cmp_ps_mask(_mm256_castsi256_ps(packed_keys),
                                  _mm256_set1_ps(pivot), _CMP_LE_OS);
      } else if constexpr (factor == 16) {
        const auto packed_keys = _mm512_castsi512_si256(
            _mm512_permutexvar_epi32(_mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                      0, 0, 0, 0, 0, 0, 0),
                                     a));
        return _mm256_cmp_ps_mask(_mm256_castsi256_ps(packed_keys),
                                  _mm256_set1_ps(pivot), _CMP_LE_OS);
      } else {
        static_assert(always_false_v<T>);
      }
    } else if constexpr (std::is_same_v<KeyType, double>) {
      if constexpr (factor == 1) {
        return _mm512_cmp_pd_mask(_mm512_castsi512_pd(a), _mm512_set1_pd(pivot),
                                  _CMP_LE_OS);
      } else if constexpr (factor == 2) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(6, 6, 4, 4, 2, 2, 0, 0), a);
        return _mm512_cmp_pd_mask(_mm512_castsi512_pd(packed_keys),
                                  _mm512_set1_pd(pivot), _CMP_LE_OS);
      } else if constexpr (factor == 4) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(4, 4, 4, 4, 0, 0, 0, 0), a);
        return _mm512_cmp_pd_mask(_mm512_castsi512_pd(packed_keys),
                                  _mm512_set1_pd(pivot), _CMP_LE_OS);
      } else if constexpr (factor == 8) {
        const auto packed_keys = _mm512_permutexvar_epi64(
            _mm512_set_epi64(0, 0, 0, 0, 0, 0, 0, 0), a);
        return _mm512_cmp_pd_mask(_mm512_castsi512_pd(packed_keys),
                                  _mm512_set1_pd(pivot), _CMP_LE_OS);
      } else {
        static_assert(always_false_v<T>);
      }
    } else {
      static_assert(always_false_v<T>, "Unsupported type");
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <typename T, std::size_t Bytes>
static inline Vec<T, Bytes> min(const Vec<T, Bytes> a, const Vec<T, Bytes> b) {
  if constexpr (Bytes == 64) {
    if constexpr (std::is_same_v<T, uint8_t>) {
      return _mm512_min_epu8(a, b);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
      return _mm512_min_epu16(a, b);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      return _mm512_min_epu32(a, b);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      return _mm512_min_epu64(a, b);
    } else if constexpr (std::is_same_v<T, int8_t>) {
      return _mm512_min_epi8(a, b);
    } else if constexpr (std::is_same_v<T, int16_t>) {
      return _mm512_min_epi16(a, b);
    } else if constexpr (std::is_same_v<T, int32_t>) {
      return _mm512_min_epi32(a, b);
    } else if constexpr (std::is_same_v<T, int64_t>) {
      return _mm512_min_epi64(a, b);
    } else if constexpr (std::is_same_v<T, float>) {
      return _mm512_castps_si512(
          _mm512_min_ps(_mm512_castsi512_ps(a), _mm512_castsi512_ps(b)));
    } else if constexpr (std::is_same_v<T, double>) {
      return _mm512_castpd_si512(
          _mm512_min_pd(_mm512_castsi512_pd(a), _mm512_castsi512_pd(b)));
    } else {
      static_assert(always_false_v<T>, "Unsupported type");
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <typename T, std::size_t Bytes>
static inline Vec<T, Bytes> max(const Vec<T, Bytes> a, const Vec<T, Bytes> b) {
  if constexpr (Bytes == 64) {
    if constexpr (std::is_same_v<T, uint8_t>) {
      return _mm512_max_epu8(a, b);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
      return _mm512_max_epu16(a, b);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      return _mm512_max_epu32(a, b);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      return _mm512_max_epu64(a, b);
    } else if constexpr (std::is_same_v<T, int8_t>) {
      return _mm512_max_epi8(a, b);
    } else if constexpr (std::is_same_v<T, int16_t>) {
      return _mm512_max_epi16(a, b);
    } else if constexpr (std::is_same_v<T, int32_t>) {
      return _mm512_max_epi32(a, b);
    } else if constexpr (std::is_same_v<T, int64_t>) {
      return _mm512_max_epi64(a, b);
    } else if constexpr (std::is_same_v<T, float>) {
      return _mm512_castps_si512(
          _mm512_max_ps(_mm512_castsi512_ps(a), _mm512_castsi512_ps(b)));
    } else if constexpr (std::is_same_v<T, double>) {
      return _mm512_castpd_si512(
          _mm512_max_pd(_mm512_castsi512_pd(a), _mm512_castsi512_pd(b)));
    } else {
      static_assert(always_false_v<T>, "Unsupported type");
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <std::size_t Factor, typename T, std::size_t Bytes>
static inline T reduce_keys_min(const Vec<T, Bytes> v) {
  using KeyType = T;
  if constexpr (Bytes == 64) {
    if constexpr (std::is_same_v<KeyType, uint32_t>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_min_epu32(v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_epu32(0x5555, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_epu32(0x1111, v);
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_min_epu32(0x0101, v);
      } else if constexpr (Factor == 16) {
        return _mm512_cvtsi512_si32(v);
      }
    } else if constexpr (std::is_same_v<KeyType, int32_t>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_min_epi32(v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_epi32(0x5555, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_epi32(0x1111, v);
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_min_epi32(0x0101, v);
      } else if constexpr (Factor == 16) {
        return _mm512_cvtsi512_si32(v);
      }
    } else if constexpr (std::is_same_v<KeyType, uint64_t>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_min_epu64(v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_epu64(0x55, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_epu64(0x11, v);
      } else if constexpr (Factor == 8) {
        return _mm_cvtsi128_si64(_mm512_castsi512_si128(v));
      }
    } else if constexpr (std::is_same_v<KeyType, int64_t>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_min_epi64(v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_epi64(0x55, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_epi64(0x11, v);
      } else if constexpr (Factor == 8) {
        return _mm_cvtsi128_si64(_mm512_castsi512_si128(v));
      }
    } else if constexpr (std::is_same_v<KeyType, float>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_min_ps(_mm512_castsi512_ps(v));
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_ps(0x5555, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_ps(0x1111, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_min_ps(0x0101, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 16) {
        return std::bit_cast<float>(_mm512_cvtsi512_si32(v));
      }
    } else if constexpr (std::is_same_v<KeyType, double>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_min_pd(_mm512_castsi512_pd(v));
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_pd(0x55, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_pd(0x11, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 8) {
        return std::bit_cast<double>(
            _mm_cvtsi128_si64(_mm512_castsi512_si128(v)));
      }
    } else {
      // TODO: more efficient implementation for 8 and 16 bit types
      KeyType tmp[Vec<KeyType, Bytes>::numElems];
      std::memcpy(tmp, &v, Bytes);
      KeyType min = std::numeric_limits<KeyType>::max();
      for (std::size_t i = 0; i < Vec<KeyType, Bytes>::numElems; i += Factor) {
        min = std::min(min, tmp[i]);
      }
      return min;
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <std::size_t Factor, typename T, std::size_t Bytes>
static inline T reduce_keys_max(const Vec<T, Bytes> v) {
  using KeyType = T;
  if constexpr (Bytes == 64) {
    if constexpr (std::is_same_v<KeyType, uint32_t>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_max_epu32(v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_epu32(0x5555, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_epu32(0x1111, v);
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_max_epu32(0x0101, v);
      } else if constexpr (Factor == 16) {
        return _mm512_cvtsi512_si32(v);
      }
    } else if constexpr (std::is_same_v<KeyType, int32_t>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_max_epi32(v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_epi32(0x5555, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_epi32(0x1111, v);
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_max_epi32(0x0101, v);
      } else if constexpr (Factor == 16) {
        return _mm512_cvtsi512_si32(v);
      }
    } else if constexpr (std::is_same_v<KeyType, uint64_t>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_max_epu64(v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_epu64(0x55, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_epu64(0x11, v);
      } else if constexpr (Factor == 8) {
        return _mm_cvtsi128_si64(_mm512_castsi512_si128(v));
      }
    } else if constexpr (std::is_same_v<KeyType, int64_t>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_max_epi64(v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_epi64(0x55, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_epi64(0x11, v);
      } else if constexpr (Factor == 8) {
        return _mm_cvtsi128_si64(_mm512_castsi512_si128(v));
      }
    } else if constexpr (std::is_same_v<KeyType, float>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_max_ps(_mm512_castsi512_ps(v));
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_ps(0x5555, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_ps(0x1111, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_max_ps(0x0101, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 16) {
        return std::bit_cast<float>(_mm512_cvtsi512_si32(v));
      }
    } else if constexpr (std::is_same_v<KeyType, double>) {
      if constexpr (Factor == 1) {
        return _mm512_reduce_max_pd(_mm512_castsi512_pd(v));
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_pd(0x55, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_pd(0x11, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 8) {
        return std::bit_cast<double>(
            _mm_cvtsi128_si64(_mm512_castsi512_si128(v)));
      }
    } else {
      // TODO: more efficient implementation for 8 and 16 bit types
      KeyType tmp[Vec<KeyType, Bytes>::numElems];
      std::memcpy(tmp, &v, Bytes);
      KeyType max = std::numeric_limits<KeyType>::min();
      for (std::size_t i = 0; i < Vec<KeyType, Bytes>::numElems; i += Factor) {
        max = std::max(max, tmp[i]);
      }
      return max;
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <std::size_t Factor, typename T, std::size_t Bytes>
static inline T reduce_keys_min(const Vec<T, Bytes> v,
                                const std::size_t elems) {
  using KeyType = T;
  if (elems == 0) {
    return std::numeric_limits<KeyType>::max();
  } else if (elems == 1) {
    KeyType result;
    std::memcpy(&result, &v, sizeof(KeyType));
    return result;
  }
  const auto mask = kshiftr(knot(Mask<Bytes / sizeof(KeyType)>(0)),
                            (Bytes / sizeof(KeyType)) - elems * Factor);
  if constexpr (Bytes == 64) {
    if constexpr (std::is_same_v<KeyType, uint32_t>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_min_epu32(mask, v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_epu32(0x5555 & mask, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_epu32(0x1111 & mask, v);
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_min_epu32(0x0101 & mask, v);
      } else if constexpr (Factor == 16) {
        return _mm512_cvtsi512_si32(v);
      }
    } else if constexpr (std::is_same_v<KeyType, int32_t>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_min_epi32(mask, v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_epi32(0x5555 & mask, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_epi32(0x1111 & mask, v);
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_min_epi32(0x0101 & mask, v);
      } else if constexpr (Factor == 16) {
        return _mm512_cvtsi512_si32(v);
      }
    } else if constexpr (std::is_same_v<KeyType, uint64_t>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_min_epu64(mask, v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_epu64(0x55 & mask, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_epu64(0x11 & mask, v);
      } else if constexpr (Factor == 8) {
        return _mm_cvtsi128_si64(_mm512_castsi512_si128(v));
      }
    } else if constexpr (std::is_same_v<KeyType, int64_t>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_min_epi64(mask, v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_epi64(0x55 & mask, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_epi64(0x11 & mask, v);
      } else if constexpr (Factor == 8) {
        return _mm_cvtsi128_si64(_mm512_castsi512_si128(v));
      }
    } else if constexpr (std::is_same_v<KeyType, float>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_min_ps(mask, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_ps(0x5555 & mask, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_ps(0x1111 & mask, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_min_ps(0x0101 & mask, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 16) {
        return std::bit_cast<float>(_mm512_cvtsi512_si32(v));
      }
    } else if constexpr (std::is_same_v<KeyType, double>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_min_pd(mask, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_min_pd(0x5555 & mask, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_min_pd(0x1111 & mask, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_min_pd(0x0101 & mask, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 16) {
        return std::bit_cast<double>(
            _mm_cvtsi128_si64(_mm512_castsi512_si128(v)));
      }
    } else {
      // TODO: more efficient implementation for 8 and 16 bit types
      KeyType tmp[Vec<KeyType, Bytes>::numElems];
      std::memcpy(tmp, &v, Bytes);
      KeyType min = std::numeric_limits<KeyType>::max();
      for (std::size_t i = 0;
           i < std::min(Vec<KeyType, Bytes>::numElems, elems * Factor);
           i += Factor) {
        min = std::min(min, tmp[i]);
      }
      return min;
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <std::size_t Factor, typename T, std::size_t Bytes>
static inline T reduce_keys_max(const Vec<T, Bytes> v,
                                const std::size_t elems) {
  using KeyType = T;
  if (elems == 0) {
    return std::numeric_limits<KeyType>::min();
  } else if (elems == 1) {
    KeyType result;
    std::memcpy(&result, &v, sizeof(KeyType));
    return result;
  }
  const auto mask = kshiftr(knot(Mask<Bytes / sizeof(KeyType)>(0)),
                            (Bytes / sizeof(KeyType)) - elems * Factor);
  if constexpr (Bytes == 64) {
    if constexpr (std::is_same_v<KeyType, uint32_t>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_max_epu32(mask, v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_epu32(0x5555 & mask, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_epu32(0x1111 & mask, v);
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_max_epu32(0x0101 & mask, v);
      } else if constexpr (Factor == 16) {
        return _mm512_cvtsi512_si32(v);
      }
    } else if constexpr (std::is_same_v<KeyType, int32_t>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_max_epi32(mask, v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_epi32(0x5555 & mask, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_epi32(0x1111 & mask, v);
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_max_epi32(0x0101 & mask, v);
      } else if constexpr (Factor == 16) {
        return _mm512_cvtsi512_si32(v);
      }
    } else if constexpr (std::is_same_v<KeyType, uint64_t>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_max_epu64(mask, v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_epu64(0x55 & mask, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_epu64(0x11 & mask, v);
      } else if constexpr (Factor == 8) {
        return _mm_cvtsi128_si64(_mm512_castsi512_si128(v));
      }
    } else if constexpr (std::is_same_v<KeyType, int64_t>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_max_epi64(mask, v);
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_epi64(0x55 & mask, v);
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_epi64(0x11 & mask, v);
      } else if constexpr (Factor == 8) {
        return _mm_cvtsi128_si64(_mm512_castsi512_si128(v));
      }
    } else if constexpr (std::is_same_v<KeyType, float>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_max_ps(mask, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_ps(0x5555 & mask, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_ps(0x1111 & mask, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_max_ps(0x0101 & mask, _mm512_castsi512_ps(v));
      } else if constexpr (Factor == 16) {
        return std::bit_cast<float>(_mm512_cvtsi512_si32(v));
      }
    } else if constexpr (std::is_same_v<KeyType, double>) {
      if constexpr (Factor == 1) {
        return _mm512_mask_reduce_max_pd(mask, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 2) {
        return _mm512_mask_reduce_max_pd(0x5555 & mask, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 4) {
        return _mm512_mask_reduce_max_pd(0x1111 & mask, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 8) {
        return _mm512_mask_reduce_max_pd(0x0101 & mask, _mm512_castsi512_pd(v));
      } else if constexpr (Factor == 16) {
        return std::bit_cast<double>(
            _mm_cvtsi128_si64(_mm512_castsi512_si128(v)));
      }
    } else {
      // TODO: more efficient implementation for 8 and 16 bit types
      KeyType tmp[Vec<KeyType, Bytes>::numElems];
      std::memcpy(tmp, &v, Bytes);
      KeyType max = std::numeric_limits<KeyType>::min();
      for (std::size_t i = 0;
           i < std::min(Vec<KeyType, Bytes>::numElems, elems * Factor);
           i += Factor) {
        max = std::max(max, tmp[i]);
      }
      return max;
    }
  } else {
    static_assert(always_false_v<T>, "Unsupported vector size");
  }
}

template <std::size_t Size>
static inline std::size_t kpopcnt(const Mask<Size> m) {
  if constexpr (Size < 8) {
    return _mm_popcnt_u64(m) / (8 / Size);
  } else {
    return _mm_popcnt_u64(m);
  }
}

template <std::size_t Size>
static inline Mask<Size> knot(const Mask<Size> m) {
#ifdef __AVX512DQ__
  if constexpr (Size <= 8) {
    return _knot_mask8(m);
  } else
#endif  // __AVX512DQ__
    if constexpr (Size == 16) {
      return _knot_mask16(m);
#ifdef __AVX512BW__
    } else if constexpr (Size == 32) {
      return _knot_mask32(m);
    } else if constexpr (Size == 64) {
      return _knot_mask64(m);
#endif  // __AVX512BW__
    } else {
      static_assert(always_false_v<Mask<Size>>, "Unsupported mask size");
    }
}

template <std::size_t Size>
static inline Mask<Size> kand(const Mask<Size> m1, const Mask<Size> m2) {
#ifdef __AVX512DQ__
  if constexpr (Size <= 8) {
    return _kand_mask8(m1, m2);
  } else
#endif  // __AVX512DQ__
    if constexpr (Size == 16) {
      return _kand_mask16(m1, m2);
#ifdef __AVX512BW__
    } else if constexpr (Size == 32) {
      return _kand_mask32(m1, m2);
    } else if constexpr (Size == 64) {
      return _kand_mask64(m1, m2);
#endif  // __AVX512BW__
    } else {
      static_assert(always_false_v<Mask<Size>>, "Unsupported mask size");
    }
}

template <std::size_t Size>
static inline Mask<Size> kshiftr(const Mask<Size> m, const std::size_t n) {
  if constexpr (Size <= 8) {
    return m >> (n * (8 / Size));
  } else {
    return m >> n;
  }
}

template <std::size_t Size>
static inline Mask<Size> kshiftl(const Mask<Size> m, const std::size_t n) {
  if constexpr (Size <= 8) {
    return m << (n * (8 / Size));
  } else {
    return m << n;
  }
}
}  // namespace simd

namespace radix_sort {

template <typename T>
static inline bool isBitSet(const std::size_t bitNo, const T val) {
  return (std::bit_cast<UInt<sizeof(T)>>(val) >> bitNo) & 1;
}

template <typename K, typename... Ps>
static inline bool isBitSet(const std::size_t bitNo,
                            const DataElement<K, Ps...> val) {
  return isBitSet(bitNo, val.key);
}

namespace internal {
template <typename T>
struct KeyType {
  using type = T;
};
template <typename K, typename... Ps>
struct KeyType<DataElement<K, Ps...>> {
  using type = K;
};
}  // namespace internal

template <typename T>
using KeyType = typename internal::KeyType<T>::type;

template <typename T, bool Up, bool IsHighestBit, bool IsRightSide>
constexpr bool bitDirUp() {
  using K = KeyType<T>;
  if constexpr (std::is_integral_v<K>) {
    if constexpr (std::is_unsigned_v<K>) {
      return Up;
    } else {
      return IsHighestBit ? !Up : Up;
    }
  } else if constexpr (std::is_floating_point_v<K>) {
    return IsHighestBit ? !Up : IsRightSide;
  }
  return Up;
}

struct BitSorterSIMD {
  template <typename K, typename... Ps>
  static constexpr SortIndex numElemsPerVec = 64 / sizeof(K);

  template <bool Up, bool IsHighestBit, bool IsRightSide, typename K,
            typename... Ps>
  static inline SortIndex sortBit(const std::size_t bitNo, const SortIndex left,
                                  const SortIndex right, K *const keys,
                                  Ps *const... payloads) {
    static constexpr SortIndex _numElemsPerVec = numElemsPerVec<K, Ps...>;

    const SortIndex numElems = right - left + 1;

    SortIndex readPosLeft = left;
    SortIndex readPosRight = right - _numElemsPerVec + 1;
    SortIndex writePosLeft = left;
    SortIndex writePosRight = right;

    simd::Vec<K, _numElemsPerVec * sizeof(K)> keyVecStore;
    std::tuple<simd::Vec<Ps, _numElemsPerVec * sizeof(Ps)>...> payloadVecStore;
    if (numElems >= _numElemsPerVec) {
      keyVecStore =
          simd::loadu<_numElemsPerVec * sizeof(K)>(&keys[readPosLeft]);
      payloadVecStore = std::make_tuple(
          simd::loadu<_numElemsPerVec * sizeof(Ps)>(&payloads[readPosLeft])...);
      readPosLeft += _numElemsPerVec;
    }

    while (readPosLeft <= readPosRight) {
      const auto keyVec = keyVecStore;
      const auto payloadVec = payloadVecStore;
      const auto [sortMaskLeft, sortMaskRight] =
          getSortMasks<Up, IsHighestBit, IsRightSide, K, Ps...>(keyVec, bitNo);
      const SortIndex numElemsToLeft = simd::kpopcnt(sortMaskLeft);
      const SortIndex numElemsToRight = _numElemsPerVec - numElemsToLeft;
      const bool areEnoughElemsFreeLeft =
          (readPosLeft - writePosLeft) >= numElemsToLeft;
      if (areEnoughElemsFreeLeft) {
        keyVecStore =
            simd::loadu<_numElemsPerVec * sizeof(K)>(&keys[readPosRight]);
        payloadVecStore =
            std::make_tuple(simd::loadu<_numElemsPerVec * sizeof(Ps)>(
                &payloads[readPosRight])...);
        readPosRight -= _numElemsPerVec;
      } else {
        keyVecStore =
            simd::loadu<_numElemsPerVec * sizeof(K)>(&keys[readPosLeft]);
        payloadVecStore =
            std::make_tuple(simd::loadu<_numElemsPerVec * sizeof(Ps)>(
                &payloads[readPosLeft])...);
        readPosLeft += _numElemsPerVec;
      }
      compress_store_left_right(
          writePosLeft, writePosRight - numElemsToRight + 1, sortMaskLeft,
          sortMaskRight, keyVec, payloadVec, keys, payloads...);
      writePosLeft += numElemsToLeft;
      writePosRight -= numElemsToRight;
    }

    const SortIndex numElemsRest = readPosRight + _numElemsPerVec - readPosLeft;

    simd::Mask<_numElemsPerVec> restMask = 0;
    simd::Vec<K, _numElemsPerVec * sizeof(K)> keyVecRest;
    std::tuple<simd::Vec<Ps, _numElemsPerVec * sizeof(Ps)>...> payloadVecRest;
    if (numElemsRest != 0) {
      restMask = simd::kshiftr(simd::knot(simd::Mask<_numElemsPerVec>(0)),
                               _numElemsPerVec - numElemsRest);
      keyVecRest = simd::maskz_loadu<_numElemsPerVec * sizeof(K)>(
          restMask, &keys[readPosLeft]);
      payloadVecRest =
          std::make_tuple(simd::maskz_loadu<_numElemsPerVec * sizeof(Ps)>(
              restMask, &payloads[readPosLeft])...);
      readPosLeft += numElemsRest;
    }

    if (numElems >= _numElemsPerVec) {
      const auto [sortMaskLeft, sortMaskRight] =
          getSortMasks<Up, IsHighestBit, IsRightSide, K, Ps...>(keyVecStore,
                                                                bitNo);
      const SortIndex numElemsToLeft = simd::kpopcnt(sortMaskLeft);
      const SortIndex numElemsToRight = _numElemsPerVec - numElemsToLeft;
      compress_store_left_right(
          writePosLeft, writePosRight - numElemsToRight + 1, sortMaskLeft,
          sortMaskRight, keyVecStore, payloadVecStore, keys, payloads...);
      writePosLeft += numElemsToLeft;
      writePosRight -= numElemsToRight;
    }

    if (numElemsRest != 0) {
      auto [sortMaskLeftRest, sortMaskRightRest] =
          getSortMasks<Up, IsHighestBit, IsRightSide, K, Ps...>(keyVecRest,
                                                                bitNo);
      sortMaskLeftRest = simd::kand(sortMaskLeftRest, restMask);
      sortMaskRightRest = simd::kand(sortMaskRightRest, restMask);
      const SortIndex numElemsToLeftRest = simd::kpopcnt(sortMaskLeftRest);
      const SortIndex numElemsToRightRest = numElemsRest - numElemsToLeftRest;
      compress_store_left_right(writePosLeft, writePosLeft + numElemsToLeftRest,
                                sortMaskLeftRest, sortMaskRightRest, keyVecRest,
                                payloadVecRest, keys, payloads...);
      writePosLeft += numElemsToLeftRest;
      writePosRight -= numElemsToRightRest;
    }
    return writePosLeft;
  }

 private:
  template <bool Up, bool IsHighestBit, bool IsRightSide, typename K,
            typename... Ps>
  static inline std::tuple<simd::Mask<numElemsPerVec<K, Ps...>>,
                           simd::Mask<numElemsPerVec<K, Ps...>>>
  getSortMasks(const simd::Vec<K, numElemsPerVec<K, Ps...> * sizeof(K)> keyVec,
               const std::size_t bitNo) {
    if constexpr (bitDirUp<K, Up, IsHighestBit, IsRightSide>()) {
      const auto sortMaskRight = simd::test_bit(keyVec, bitNo);
      const auto sortMaskLeft = simd::knot(sortMaskRight);
      return std::make_tuple(sortMaskLeft, sortMaskRight);
    } else {
      const auto sortMaskLeft = simd::test_bit(keyVec, bitNo);
      const auto sortMaskRight = simd::knot(sortMaskLeft);
      return std::make_tuple(sortMaskLeft, sortMaskRight);
    }
  }

  template <typename K, typename... Ps>
  static inline void compress_store_left_right(
      const SortIndex leftPos, const SortIndex rightPos,
      const simd::Mask<numElemsPerVec<K, Ps...>> leftMask,
      const simd::Mask<numElemsPerVec<K, Ps...>> rightMask,
      const simd::Vec<K, numElemsPerVec<K, Ps...> * sizeof(K)> keyVec,
      const std::tuple<simd::Vec<Ps, numElemsPerVec<K, Ps...> * sizeof(Ps)>...>
          payloadVec,
      K *const keys, Ps *const... payloads) {
    simd::mask_compressstoreu(&keys[leftPos], leftMask, keyVec);
    std::apply(
        [&](const auto... payloadVecs) {
          (simd::mask_compressstoreu(&payloads[leftPos], leftMask, payloadVecs),
           ...);
        },
        payloadVec);

    simd::mask_compressstoreu(&keys[rightPos], rightMask, keyVec);
    std::apply(
        [&](const auto... payloadVecs) {
          (simd::mask_compressstoreu(&payloads[rightPos], rightMask,
                                     payloadVecs),
           ...);
        },
        payloadVec);
  }
};

template <bool Up, typename BitSorter, typename CmpSorter,
          bool IsRightSide = false, bool IsHighestBit = true, typename K,
          typename... Ps>
void radixRecursion(const std::size_t bitNo, const SortIndex cmpSortThreshold,
                    const SortIndex left, const SortIndex right, K *const keys,
                    Ps *const... payloads) {
  if (right - left <= 0) {
    return;
  }
  if (right - left < cmpSortThreshold) {
    CmpSorter::template sort<Up, K, Ps...>(left, right, keys, payloads...);
    return;
  }

  const SortIndex split =
      BitSorter::template sortBit<Up, IsHighestBit, IsRightSide, K, Ps...>(
          bitNo, left, right, keys, payloads...);
  if (bitNo > 0) {
    radixRecursion<Up, BitSorter, CmpSorter, IsHighestBit ? false : IsRightSide,
                   false>(bitNo - 1, cmpSortThreshold, left, split - 1, keys,
                          payloads...);
    radixRecursion<Up, BitSorter, CmpSorter, IsHighestBit ? true : IsRightSide,
                   false>(bitNo - 1, cmpSortThreshold, split, right, keys,
                          payloads...);
  }
}

template <bool Up = true, typename BitSorter = BitSorterSIMD,
          typename CmpSorter = CmpSorterInsertionSort, typename K,
          typename... Ps>
void sort(SortIndex cmpSortThreshold, const SortIndex num, K *const keys,
          Ps *const... payloads) {
  radixRecursion<Up, BitSorter, CmpSorter>(sizeof(K) * 8 - 1, cmpSortThreshold,
                                           0, num - 1, keys, payloads...);
}

template <bool Up, typename BitSorter, typename CmpSorter, typename K,
          typename... Ps>
void sort(SortIndex cmpSortThreshold, const SortIndex num,
          DataElement<K, Ps...> *const elements) {
  static_assert(is_power_of_two<sizeof(DataElement<K, Ps...>)>,
                "size of DataElement<K, Ps...> must be a power of two");
  radixRecursion<Up, BitSorter, CmpSorter>(sizeof(K) * 8 - 1, cmpSortThreshold,
                                           0, num - 1, elements);
}

template <bool Up = true, typename K, typename... Ps>
void sort(const SortIndex num, K *const keys, Ps *const... payloads) {
  sort<Up, BitSorterSIMD, CmpSorterInsertionSort>(16, num, keys, payloads...);
}

}  // namespace radix_sort
}  // namespace simd_sort

#endif  // __AVX512F__
