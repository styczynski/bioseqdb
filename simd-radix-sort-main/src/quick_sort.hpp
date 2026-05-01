#pragma once

#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <string>
#include <tuple>

#include "cmp_sorters.hpp"
#include "common.hpp"
#include "data.hpp"
#include "simd.hpp"

#if __has_include("../bramas/sort512.hpp")
#include "../bramas/sort512.hpp"
#endif

#if __has_include("../bramas/sort512kv.hpp")
#include "../bramas/sort512kv.hpp"
#endif

namespace simd_sort::quick_sort {

template <typename K>
struct PartitionResult {
  SortIndex splitIndex;
  K smallestKey;
  K largestKey;
};

struct PartitionerSequential {
  static std::string name() { return "PartitionerSequential"; }

  template <bool Up, typename K, typename... Ps>
  static inline PartitionResult<KeyType<K>> partition(const KeyType<K> pivot,
                                                      const SortIndex left,
                                                      const SortIndex right,
                                                      K *const keys,
                                                      Ps *const... payloads) {
    KeyType<K> smallestKey = std::numeric_limits<KeyType<K>>::max();
    KeyType<K> largestKey = std::numeric_limits<KeyType<K>>::lowest();
    SortIndex l = left;
    SortIndex r = right;
    while (l <= r) {
      while (l <= r &&
             (Up ? (getKey(keys[l]) <= pivot) : (getKey(keys[l]) > pivot))) {
        smallestKey = std::min(smallestKey, getKey(keys[l]));
        largestKey = std::max(largestKey, getKey(keys[l]));
        l++;
      }
      while (l <= r &&
             (Up ? (getKey(keys[r]) > pivot) : (getKey(keys[r]) <= pivot))) {
        smallestKey = std::min(smallestKey, getKey(keys[r]));
        largestKey = std::max(largestKey, getKey(keys[r]));
        r--;
      }
      if (l < r) {
        std::swap(keys[l], keys[r]);
        (std::swap(payloads[l], payloads[r]), ...);
      }
    }
    return {l, smallestKey, largestKey};
  }
};

struct PartitionerSIMD {
  static std::string name() { return "PartitionerSIMD"; }

  template <bool Up, typename K, typename... Ps>
  static inline PartitionResult<KeyType<K>> partition(const KeyType<K> pivot,
                                                      const SortIndex left,
                                                      const SortIndex right,
                                                      K *const keys,
                                                      Ps *const... payloads) {
    static constexpr SortIndex numElemsPerVec = simd::Vec<K>::numElems;

    const SortIndex numElems = right - left + 1;

    SortIndex readPosLeft = left;
    SortIndex readPosRight = right - numElemsPerVec + 1;
    SortIndex writePosLeft = left;
    SortIndex writePosRight = right;

    simd::Vec<K, numElemsPerVec * sizeof(K)> keyVecStore;
    simd::Vec<KeyType<K>, numElemsPerVec * sizeof(K)> minVec, maxVec;
    std::tuple<simd::Vec<Ps, numElemsPerVec * sizeof(Ps)>...> payloadVecStore;
    if (numElems >= numElemsPerVec) {
      keyVecStore = simd::loadu<numElemsPerVec * sizeof(K)>(&keys[readPosLeft]);
      minVec = simd::reinterpret<KeyType<K>>(keyVecStore);
      maxVec = simd::reinterpret<KeyType<K>>(keyVecStore);
      payloadVecStore = std::make_tuple(
          simd::loadu<numElemsPerVec * sizeof(Ps)>(&payloads[readPosLeft])...);
      readPosLeft += numElemsPerVec;
    }

    while (readPosLeft <= readPosRight) {
      const auto keyVec = keyVecStore;
      const auto payloadVec = payloadVecStore;
      const auto [sortMaskLeft, sortMaskRight] =
          getSortMasks<Up>(keyVec, pivot);
      const SortIndex numElemsToLeft = simd::kpopcnt(sortMaskLeft);
      const SortIndex numElemsToRight = numElemsPerVec - numElemsToLeft;
      const bool areEnoughElemsFreeLeft =
          (readPosLeft - writePosLeft) >= numElemsToLeft;
      if (areEnoughElemsFreeLeft) {
        keyVecStore =
            simd::loadu<numElemsPerVec * sizeof(K)>(&keys[readPosRight]);
        minVec = simd::min(minVec, simd::reinterpret<KeyType<K>>(keyVecStore));
        maxVec = simd::max(maxVec, simd::reinterpret<KeyType<K>>(keyVecStore));
        payloadVecStore =
            std::make_tuple(simd::loadu<numElemsPerVec * sizeof(Ps)>(
                &payloads[readPosRight])...);
        readPosRight -= numElemsPerVec;
      } else {
        keyVecStore =
            simd::loadu<numElemsPerVec * sizeof(K)>(&keys[readPosLeft]);
        minVec = simd::min(minVec, simd::reinterpret<KeyType<K>>(keyVecStore));
        maxVec = simd::max(maxVec, simd::reinterpret<KeyType<K>>(keyVecStore));
        payloadVecStore =
            std::make_tuple(simd::loadu<numElemsPerVec * sizeof(Ps)>(
                &payloads[readPosLeft])...);
        readPosLeft += numElemsPerVec;
      }
      compress_store_left_right(
          writePosLeft, writePosRight - numElemsToRight + 1, sortMaskLeft,
          sortMaskRight, keyVec, payloadVec, keys, payloads...);
      writePosLeft += numElemsToLeft;
      writePosRight -= numElemsToRight;
    }

    const SortIndex numElemsRest = readPosRight + numElemsPerVec - readPosLeft;

    simd::Mask<numElemsPerVec> restMask = 0;
    simd::Vec<K, numElemsPerVec * sizeof(K)> keyVecRest;
    std::tuple<simd::Vec<Ps, numElemsPerVec * sizeof(Ps)>...> payloadVecRest;
    if (numElemsRest != 0) {
      restMask = simd::kshiftr(simd::knot(simd::Mask<numElemsPerVec>(0)),
                               numElemsPerVec - numElemsRest);
      keyVecRest = simd::maskz_loadu<numElemsPerVec * sizeof(K)>(
          restMask, &keys[readPosLeft]);
      payloadVecRest =
          std::make_tuple(simd::maskz_loadu<numElemsPerVec * sizeof(Ps)>(
              restMask, &payloads[readPosLeft])...);
      readPosLeft += numElemsRest;
    }

    if (numElems >= numElemsPerVec) {
      const auto [sortMaskLeft, sortMaskRight] =
          getSortMasks<Up>(keyVecStore, pivot);
      const SortIndex numElemsToLeft = simd::kpopcnt(sortMaskLeft);
      const SortIndex numElemsToRight = numElemsPerVec - numElemsToLeft;
      compress_store_left_right(
          writePosLeft, writePosRight - numElemsToRight + 1, sortMaskLeft,
          sortMaskRight, keyVecStore, payloadVecStore, keys, payloads...);
      writePosLeft += numElemsToLeft;
      writePosRight -= numElemsToRight;
    }

    if (numElemsRest != 0) {
      auto [sortMaskLeftRest, sortMaskRightRest] =
          getSortMasks<Up>(keyVecRest, pivot);
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

    KeyType<K> smallestKey = std::numeric_limits<KeyType<K>>::max();
    KeyType<K> largestKey = std::numeric_limits<KeyType<K>>::lowest();
    if (numElems >= numElemsPerVec) {
      smallestKey =
          simd::reduce_keys_min<sizeof(K) / sizeof(KeyType<K>)>(minVec);
      largestKey =
          simd::reduce_keys_max<sizeof(K) / sizeof(KeyType<K>)>(maxVec);
    }
    if (numElemsRest != 0) {
      smallestKey = std::min(
          smallestKey,
          simd::reduce_keys_min<sizeof(K) / sizeof(KeyType<K>)>(
              simd::reinterpret<KeyType<K>>(keyVecRest), numElemsRest));
      largestKey = std::max(
          largestKey,
          simd::reduce_keys_max<sizeof(K) / sizeof(KeyType<K>)>(
              simd::reinterpret<KeyType<K>>(keyVecRest), numElemsRest));
    }
    return {writePosLeft, smallestKey, largestKey};
  }

 private:
  template <bool Up, std::size_t Bytes, typename K>
  static inline std::tuple<simd::Mask<simd::Vec<K, Bytes>::numElems>,
                           simd::Mask<simd::Vec<K, Bytes>::numElems>>
  getSortMasks(const simd::Vec<K, Bytes> keyVec, const KeyType<K> pivot) {
    const auto mask = simd::cmple_keys(keyVec, pivot);
    const auto invMask = simd::knot(mask);
    if constexpr (Up) {
      return std::make_tuple(mask, invMask);
    } else {
      return std::make_tuple(invMask, mask);
    }
  }

  template <std::size_t NumElemsPerVec, typename K, typename... Ps>
  static inline void compress_store_left_right(
      const SortIndex leftPos, const SortIndex rightPos,
      const simd::Mask<NumElemsPerVec> leftMask,
      const simd::Mask<NumElemsPerVec> rightMask,
      const simd::Vec<K, NumElemsPerVec * sizeof(K)> keyVec,
      const std::tuple<simd::Vec<Ps, NumElemsPerVec * sizeof(Ps)>...>
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

static inline auto nextVal(const std::integral auto val) -> decltype(val) {
  if (val == std::numeric_limits<decltype(val)>::max()) {
    return val;
  }
  return val + 1;
}
static inline auto nextVal(const std::floating_point auto val) {
  return std::nextafter(val, std::numeric_limits<decltype(val)>::infinity());
}

static inline auto getAverage(const std::integral auto a,
                              const std::integral auto b) {
  return (a & b) + ((a ^ b) >> 1);
}
static inline auto getAverage(const std::floating_point auto a,
                              const std::floating_point auto b) {
  return (a + b) / 2;
}

template <typename K>
static inline KeyType<K> median(const K a, const K b, const K c) {
  if (a < b) {
    if (b < c) {
      return b;
    } else if (a < c) {
      return c;
    } else {
      return a;
    }
  } else {
    if (a < c) {
      return a;
    } else if (b < c) {
      return c;
    } else {
      return b;
    }
  }
}

template <typename K>
static inline KeyType<K> getMedianOf3(const SortIndex left,
                                      const SortIndex right,
                                      const K *const keys) {
  const auto mid = left + (right - left) / 2;
  return median(getKey(keys[left]), getKey(keys[mid]), getKey(keys[right]));
}

template <typename K>
static inline KeyType<K> getMedianOf9(const SortIndex left,
                                      const SortIndex right,
                                      const K *const keys) {
  const auto leftMid = left + (right - left) / 3;
  const auto rightMid = left + 2 * (right - left) / 3;
  return median(getMedianOf3(left, leftMid, keys),
                getMedianOf3(leftMid + 1, rightMid, keys),
                getMedianOf3(rightMid + 1, right, keys));
}

template <bool Up, typename Partitioner, typename CmpSorter, typename K,
          typename... Ps>
void quickRecursion(const SortIndex cmpSortThreshold, const SortIndex left,
                    const SortIndex right, const bool chooseAvg,
                    const KeyType<K> avg, K *const keys,
                    Ps *const... payloads) {
  if (right - left <= 0) {
    return;
  }
  if (right - left < cmpSortThreshold) {
    CmpSorter::template sort<Up, K, Ps...>(left, right, keys, payloads...);
    return;
  }

  const auto pivot = chooseAvg ? avg : getMedianOf9(left, right, keys);

  const auto [split, smallestKey, largestKey] =
      Partitioner::template partition<Up>(pivot, left, right, keys,
                                          payloads...);

  const double ratio =
      (std::min(split - left, right - split + 1) / double(right - left + 1));

  const bool nextChooseAvg = ratio < 0.2 ? !chooseAvg : chooseAvg;

  if (Up ? pivot > smallestKey : nextVal(pivot) < largestKey) {
    const auto avg = getAverage(pivot, Up ? smallestKey : largestKey);
    quickRecursion<Up, Partitioner, CmpSorter>(cmpSortThreshold, left,
                                               split - 1, nextChooseAvg, avg,
                                               keys, payloads...);
  }
  if (Up ? nextVal(pivot) < largestKey : pivot > smallestKey) {
    const auto avg = getAverage(pivot, Up ? largestKey : smallestKey);
    quickRecursion<Up, Partitioner, CmpSorter>(
        cmpSortThreshold, split, right, nextChooseAvg, avg, keys, payloads...);
  }
}

template <bool Up = true, typename Partitioner, typename CmpSorter, typename K,
          typename... Ps>
void sort(SortIndex cmpSortThreshold, const SortIndex num, K *const keys,
          Ps *const... payloads) {
#if defined(SORT512_HPP) && defined(SORT512KV_HPP)
  if constexpr (std::is_same_v<CmpSorter, CmpSorterBramasSmallSort>) {
    // bramas small sort only supports as many elements as fit in 16 avx512
    // registers
    cmpSortThreshold =
        std::min(cmpSortThreshold, (SortIndex)(16 * 64 / sizeof(K)));
  }
#endif  // SORT512_HPP && SORT512KV_HPP
  quickRecursion<Up, Partitioner, CmpSorter>(
      cmpSortThreshold, 0, num - 1, false,
      getAverage(std::numeric_limits<KeyType<K>>::lowest(),
                 std::numeric_limits<KeyType<K>>::max()),
      keys, payloads...);
}

template <bool Up = true, typename Partitioner, typename CmpSorter, typename K,
          typename... Ps>
void sort(const SortIndex num, K *const keys, Ps *const... payloads) {
  quickRecursion<Up, Partitioner, CmpSorter>(
      16, 0, num - 1, false,
      getAverage(std::numeric_limits<KeyType<K>>::lowest(),
                 std::numeric_limits<KeyType<K>>::max()),
      keys, payloads...);
}
}  // namespace simd_sort::quick_sort
