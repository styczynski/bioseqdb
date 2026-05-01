#pragma once

#include <string>
#include <tuple>

#include "common.hpp"

#if __has_include("../bramas/sort512.hpp")
#include "../bramas/sort512.hpp"
#endif

#if __has_include("../bramas/sort512kv.hpp")
#include "../bramas/sort512kv.hpp"
#endif

namespace simd_sort {

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

#if defined(SORT512_HPP) && defined(SORT512KV_HPP)
struct CmpSorterBramasSmallSort {
  static std::string name() { return "CmpSorterBramasSmallSort"; }

  template <bool Up, typename K, typename... Ps>
  static inline void sort(const SortIndex left, const SortIndex right,
                          K *const keys, Ps *const... payloads) {
    static_assert(std::is_same_v<K, double> || std::is_same_v<K, int>,
                  "BramasSmallSort only supports int and double");
    static_assert(Up, "BramasSmallSort only supports sorting up");

    if constexpr (sizeof...(Ps) == 0) {
      Sort512::SmallSort16V(keys + left, right - left + 1);
    } else if constexpr (sizeof...(Ps) == 1) {
      static_assert(std::is_same_v<K, Ps...>,
                    "key and payload must have the same type");
      Sort512kv::SmallSort16V(keys + left,
                              std::get<0>(std::make_tuple(payloads...)) + left,
                              right - left + 1);
    } else {
      static_assert(always_false_v<K>,
                    "BramasSmallSort only supports one or zero payloads");
    }
  }
};
#endif  // defined(SORT512_HPP) && defined(SORT512KV_HPP)

struct CmpSorterNoSort {
  static std::string name() { return "CmpSorterNoSort"; }

  template <bool Up, typename K, typename... Ps>
  static inline void sort(const SortIndex left, const SortIndex right,
                          K *const keys, Ps *const... payloads) {
    // do nothing
    (void)left;
    (void)right;
    (void)keys;
    ((void)payloads, ...);
  }
};
}  // namespace simd_sort