#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

#include "../blacher/avx2sort.h"
#include "../bramas/sort512.hpp"
#include "../bramas/sort512kv.hpp"
#include "common.hpp"
#include "data.hpp"
#include "ipp_radix.hpp"
#include "quick_sort.hpp"
#include "radix_sort.hpp"

#if __has_include("../moeller/SIMDRadixSortGeneric.H")
#include "../moeller/SIMDRadixSortGeneric.H"
#endif

namespace simd_sort {

template <typename BitSorter, typename CmpSorter, bool Combined = false>
struct SortMethodRadixSort {
  static std::string name() {
    std::string result = "Radix";
    if constexpr (std::is_same_v<BitSorter, radix_sort::BitSorterSequential>) {
      result += "Seq";
    } else if constexpr (std::is_same_v<BitSorter,
                                        radix_sort::BitSorterSIMD<false>>) {
      result += "SIMD";
    } else if constexpr (std::is_same_v<BitSorter,
                                        radix_sort::BitSorterSIMD<true>>) {
      result += "SIMDOneReg";
    } else {
      static_assert(always_false_v<BitSorter>, "Unknown BitSorter");
    }
    if constexpr (std::is_same_v<CmpSorter, CmpSorterInsertionSort>) {
      // don't add anything
    } else if constexpr (std::is_same_v<CmpSorter, CmpSorterBramasSmallSort>) {
      result += "BramSmall";
    } else if constexpr (std::is_same_v<CmpSorter, CmpSorterNoSort>) {
      result += "NoCmp";
    } else {
      static_assert(always_false_v<CmpSorter>, "Unknown CmpSorter");
    }
    if constexpr (Combined) {
      result += "Combined";
    }
    return result;
  }

  static constexpr bool areKeyAndPayloadSeparate = !Combined;
  static constexpr bool hasThreshold = true;

  template <bool Up, typename K, typename... Ps>
  static constexpr bool isSupported() {
    if constexpr (Combined && sizeof...(Ps) != 0) {
      return false;
    }
    if constexpr (std::is_same_v<CmpSorter, CmpSorterBramasSmallSort>) {
      if constexpr (!Up) {
        return false;
      }
      if constexpr (sizeof...(Ps) == 0) {
        return std::is_same_v<K, int> || std::is_same_v<K, double>;
      }
      if constexpr (sizeof...(Ps) == 1) {
        return (std::is_same_v<K, int> && std::is_same_v<int, Ps...>);
      }
      return false;
    }
    return true;
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sort(const SortIndex num, K *const keys, Ps *const... payloads) {
    if constexpr (std::is_same_v<CmpSorter, CmpSorterBramasSmallSort>) {
      sortThresh<Up>(16 * 64 / sizeof(K), num, keys, payloads...);
    } else {
      if constexpr (std::is_same_v<BitSorter,
                                   radix_sort::BitSorterSequential>) {
        sortThresh<Up>(64, num, keys, payloads...);
      } else {
        sortThresh<Up>(16, num, keys, payloads...);
      }
    }
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sortThresh(const SortIndex cmpSortThresh, const SortIndex num,
                         K *const keys, Ps *const... payloads) {
    static_assert(isSupported<Up, K, Ps...>(), "Unsupported type combination");
    radix_sort::sort<Up, BitSorter, CmpSorter>(cmpSortThresh, num, keys,
                                               payloads...);
  }
};

template <typename Partitioner, typename CmpSorter, bool Combined = false>
struct SortMethodQuickSort {
  static std::string name() {
    std::string result = "Quick";
    if constexpr (std::is_same_v<Partitioner,
                                 quick_sort::PartitionerSequential>) {
      result += "Seq";
    } else if constexpr (std::is_same_v<Partitioner,
                                        quick_sort::PartitionerSIMD>) {
      result += "SIMD";
    } else {
      static_assert(always_false_v<Partitioner>, "Unknown Partitioner");
    }
    if constexpr (std::is_same_v<CmpSorter, CmpSorterInsertionSort>) {
      // don't add anything
    } else if constexpr (std::is_same_v<CmpSorter, CmpSorterBramasSmallSort>) {
      result += "BramSmall";
    } else if constexpr (std::is_same_v<CmpSorter, CmpSorterNoSort>) {
      result += "NoCmp";
    } else {
      static_assert(always_false_v<CmpSorter>, "Unknown CmpSorter");
    }
    if constexpr (Combined) {
      result += "Combined";
    }
    return result;
  }

  static constexpr bool areKeyAndPayloadSeparate = !Combined;
  static constexpr bool hasThreshold = true;

  template <bool Up, typename K, typename... Ps>
  static constexpr bool isSupported() {
    // if constexpr (Combined && sizeof...(Ps) != 0) {
    //   return false;
    // }
    if constexpr (std::is_same_v<CmpSorter, CmpSorterBramasSmallSort>) {
      if constexpr (!Up) {
        return false;
      }
      if constexpr (sizeof...(Ps) == 0) {
        return std::is_same_v<K, int> || std::is_same_v<K, double>;
      }
      if constexpr (sizeof...(Ps) == 1) {
        return (std::is_same_v<K, int> && std::is_same_v<int, Ps...>);
      }
      return false;
    }
    return true;
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sort(const SortIndex num, K *const keys, Ps *const... payloads) {
    if constexpr (std::is_same_v<CmpSorter, CmpSorterBramasSmallSort>) {
      sortThresh<Up>(16 * 64 / sizeof(K), num, keys, payloads...);
    } else {
      sortThresh<Up>(16, num, keys, payloads...);
    }
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sortThresh(const SortIndex cmpSortThresh, const SortIndex num,
                         K *const keys, Ps *const... payloads) {
    static_assert(isSupported<Up, K, Ps...>(), "Unsupported type combination");
    quick_sort::sort<Up, Partitioner, CmpSorter>(cmpSortThresh, num, keys,
                                                 payloads...);
  }
};

#ifdef _SIMD_RADIX_SORT_GENERIC_H_
struct SortMethodMoellerCompress {
  static std::string name() { return "MoellerCompress"; }
  static constexpr bool areKeyAndPayloadSeparate = false;
  static constexpr bool hasThreshold = true;

  template <bool Up, typename K, typename... Ps>
  static constexpr bool isSupported() {
    if constexpr (sizeof...(Ps) > 1) {
      return false;
    }
    if constexpr (!((sizeof(K) >= sizeof(Ps)) && ...)) {
      return false;
    }
    return true;
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sort(const SortIndex num, DataElement<K, Ps...> *const elems) {
    sortThresh<Up>(16, num, elems);
  }

  template <bool Up = true, typename K>
  static void sortThresh(const std::size_t cmpSortThresh, const std::size_t num,
                         DataElement<K> *const elems) {
    radix::simdRadixSortCompress<K, Up>(
        (typename radix::UInt<sizeof(K)>::T *)elems, 0, num - 1, cmpSortThresh);
  }

  template <bool Up = true, typename K, typename P>
  static void sortThresh(const std::size_t cmpSortThresh, const std::size_t num,
                         DataElement<K, P> *const elems) {
    static_assert(sizeof(K) >= sizeof(P), "Unsupported type combination");
    radix::simdRadixSortCompress<K, Up>(
        (typename radix::UInt<sizeof(K)>::T2 *)elems, 0, num - 1,
        cmpSortThresh);
  }
};

struct SortMethodMoellerSeq {
  static std::string name() { return "MoellerSeq"; }
  static constexpr bool areKeyAndPayloadSeparate = false;
  static constexpr bool hasThreshold = true;

  template <bool Up, typename K, typename... Ps>
  static constexpr bool isSupported() {
    if constexpr (sizeof...(Ps) > 1) {
      return false;
    }
    if constexpr (!((sizeof(K) == sizeof(Ps)) && ...)) {
      return false;
    }
    return true;
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sort(const SortIndex num, DataElement<K, Ps...> *const elems) {
    sortThresh<Up>(64, num, elems);
  }

  template <bool Up = true, typename K>
  static void sortThresh(const std::size_t cmpSortThresh, const std::size_t num,
                         DataElement<K> *const elems) {
    radix::seqRadixSort<K, Up>((typename radix::UInt<sizeof(K)>::T *)elems, 0,
                               num - 1, cmpSortThresh);
  }

  template <bool Up = true, typename K, typename P>
  static void sortThresh(const std::size_t cmpSortThresh, const std::size_t num,
                         DataElement<K, P> *const elems) {
    static_assert(sizeof(K) >= sizeof(P), "Unsupported type combination");
    radix::seqRadixSort<K, Up>((typename radix::UInt<sizeof(K)>::T2 *)elems, 0,
                               num - 1, cmpSortThresh);
  }
};
#endif  // _SIMD_RADIX_SORT_GENERIC_H_

struct SortMethodSTLSort {
  static std::string name() { return "STLSort"; }
  static constexpr bool areKeyAndPayloadSeparate = false;
  static constexpr bool hasThreshold = false;

  template <bool Up, typename K, typename... Ps>
  static constexpr bool isSupported() {
    return true;
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sort(const SortIndex num,
                   DataElement<K, Ps...> *const keysAndPayloads) {
    static_assert(isSupported<Up, K, Ps...>(), "Unsupported type combination");
    if constexpr (Up) {
      std::sort(keysAndPayloads, keysAndPayloads + num,
                std::less<DataElement<K, Ps...>>());
    } else {
      std::sort(keysAndPayloads, keysAndPayloads + num,
                std::greater<DataElement<K, Ps...>>());
    }
  }
};

#ifdef IPP_RADIX_IS_PRESENT_
struct SortMethodIPPRadix {
  static std::string name() { return "IPPRadix"; }
  static constexpr bool areKeyAndPayloadSeparate = false;
  static constexpr bool hasThreshold = false;

  template <bool Up, typename K, typename... Ps>
  static constexpr bool isSupported() {
    return sizeof...(Ps) == 0 && !std::is_same_v<K, int8_t>;
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sort(const SortIndex num,
                   DataElement<K, Ps...> *const keysAndPayloads) {
    static_assert(sizeof...(Ps) == 0, "IPPRadix does not support payloads");
    static_assert(isSupported<Up, K, Ps...>(), "Unsupported type combination");
    ipp_radix::sort<Up>((K *)keysAndPayloads, num);
  }
};
#endif  // IPP_RADIX_IS_PRESENT_

struct SortMethodBramas {
  static std::string name() { return "BramasSort"; }
  static constexpr bool areKeyAndPayloadSeparate = true;
  static constexpr bool hasThreshold = false;

  template <bool Up, typename K, typename... Ps>
  static constexpr bool isSupported() {
    if (!Up) {
      return false;
    }
    if constexpr (sizeof...(Ps) == 0) {
      return std::is_same_v<K, int> || std::is_same_v<K, double>;
    }
    if constexpr (sizeof...(Ps) == 1) {
      return std::is_same_v<K, int> && std::is_same_v<int, Ps...>;
    }
    return false;
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sort(const SortIndex num, K *const keys, Ps *const... payloads) {
    static_assert(Up, "Unsupported sort direction");
    static_assert(isSupported<Up, K, Ps...>(), "Unsupported type combination");
    static_assert(sizeof...(Ps) == 0 || sizeof...(Ps) == 1,
                  "SortMethodBramas does not support more than 1 payload");
    if constexpr (sizeof...(Ps) == 1) {
      static_assert(
          std::is_same_v<K, Ps...>,
          "SortMethodBramas requires the same type for key and payload");
    }
    if constexpr (sizeof...(Ps) == 0) {
      Sort512::Sort(keys, num);
    } else if constexpr (sizeof...(Ps) == 1) {
      Sort512kv::Sort(keys, payloads..., num);
    }
  }
};

struct SortMethodBlacher {
  static std::string name() { return "BlacherSort"; }
  static constexpr bool areKeyAndPayloadSeparate = true;
  static constexpr bool hasThreshold = false;

  template <bool Up, typename K, typename... Ps>
  static constexpr bool isSupported() {
    return Up && sizeof...(Ps) == 0 && std::is_same_v<K, int32_t>;
  }

  template <bool Up = true, typename K, typename... Ps>
  static void sort(const SortIndex num, K *const keys, Ps *const...) {
    static_assert(sizeof...(Ps) == 0 && std::is_same_v<K, int32_t>,
                  "SortMethodBlacher only supports int32_t key and no payload");
    static_assert(Up, "Unsupported sort direction");
    static_assert(isSupported<Up, K, Ps...>(), "Unsupported type combination");
    avx2::quicksort(keys, num);
  }
};
}  // namespace simd_sort
