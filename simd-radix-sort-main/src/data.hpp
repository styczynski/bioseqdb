#pragma once

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

namespace simd_sort {

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

template <typename K>
static inline K getKey(const K &key) {
  return key;
}

template <typename K, typename... Ps>
static inline K getKey(const DataElement<K, Ps...> &dataElement) {
  return dataElement.key;
}

template <typename K>
using KeyType = decltype(getKey(std::declval<K>()));

template <typename T>
T getRandom() {
  std::array<uint8_t, sizeof(T)> randomBytes;
  for (std::size_t i = 0; i < sizeof(T); i++) {
    randomBytes[i] = rand();
  }
  return std::bit_cast<T>(randomBytes);
}

enum class InputDistribution {
  Uniform,
  Gaussian,
  Zero,
  ZeroOne,
  Sorted,
  ReverseSorted,
  AlmostSorted,
  AlmostReverseSorted,
};

constexpr const char *inputDistributionToString(
    const InputDistribution distribution) {
  switch (distribution) {
    case InputDistribution::Zero:
      return "Zero";
    case InputDistribution::ZeroOne:
      return "ZeroOne";
    case InputDistribution::Uniform:
      return "Uniform";
    case InputDistribution::Gaussian:
      return "Gaussian";
    case InputDistribution::Sorted:
      return "Sorted";
    case InputDistribution::ReverseSorted:
      return "ReverseSorted";
    case InputDistribution::AlmostSorted:
      return "AlmostSorted";
    case InputDistribution::AlmostReverseSorted:
      return "AlmostReverseSorted";
    default:
      return "Unknown";
  }
}

template <typename K, typename... Ps>
struct Data {
  K *keys;
  std::tuple<Ps *...> payloads;
  std::size_t num;

  Data(const std::size_t num, const InputDistribution distribution,
       const long seed = time(NULL))
      : keys(new K[num]), payloads(std::make_tuple(new Ps[num]...)), num(num) {
    std::mt19937 gen(seed);
    switch (distribution) {
      case InputDistribution::Zero: {
        for (std::size_t i = 0; i < num; i++) {
          keys[i] = 0;
        }
      } break;
      case InputDistribution::ZeroOne: {
        for (std::size_t i = 0; i < num; i++) {
          keys[i] = K(gen() % 2);
        }
      } break;
      case InputDistribution::Uniform: {
        fillKeysUniform(gen);
      } break;
      case InputDistribution::Gaussian: {
        fillKeysGaussian(gen);
      } break;
      case InputDistribution::Sorted: {
        if constexpr (std::is_integral_v<K>) {
          fillKeysUniform(gen);
        } else {
          fillKeysGaussian(gen);
        }
        std::sort(keys, keys + num);
      } break;
      case InputDistribution::ReverseSorted: {
        if constexpr (std::is_integral_v<K>) {
          fillKeysUniform(gen);
        } else {
          fillKeysGaussian(gen);
        }
        std::sort(keys, keys + num, std::greater<K>());
      } break;
      case InputDistribution::AlmostSorted: {
        if constexpr (std::is_integral_v<K>) {
          fillKeysUniform(gen);
        } else {
          fillKeysGaussian(gen);
        }
        std::sort(keys, keys + num);
        const std::size_t numberOfDisplacements = std::exp2(std::log10(num));
        std::uniform_int_distribution<std::size_t> dist(0, num - 1);
        for (std::size_t i = 0; i < numberOfDisplacements; i++) {
          std::swap(keys[dist(gen)], keys[dist(gen)]);
        }
      } break;
      case InputDistribution::AlmostReverseSorted: {
        if constexpr (std::is_integral_v<K>) {
          fillKeysUniform(gen);
        } else {
          fillKeysGaussian(gen);
        }
        std::sort(keys, keys + num, std::greater<K>());
        const std::size_t numberOfDisplacements = std::exp2(std::log10(num));
        std::uniform_int_distribution<std::size_t> dist(0, num - 1);
        for (std::size_t i = 0; i < numberOfDisplacements; i++) {
          std::swap(keys[dist(gen)], keys[dist(gen)]);
        }
      } break;
    }
    makePayloads();
  }

  Data(const Data &other)
      : keys(new K[other.num]),
        payloads(std::make_tuple(new Ps[other.num]...)),
        num(other.num) {
    std::copy(other.keys, other.keys + num, keys);
    std::apply(
        [&](const auto... newPayloads) {
          std::apply(
              [&](const auto... otherPayloads) {
                (std::copy(otherPayloads, otherPayloads + num, newPayloads),
                 ...);
              },
              other.payloads);
        },
        payloads);
  }

  ~Data() {
    delete[] keys;
    std::apply([](const Ps *const... payloads) { (..., delete[] payloads); },
               payloads);
  }

  bool isSorted(const bool up = true) const {
    if (up) {
      for (std::size_t i = 1; i < num; ++i) {
        if (keys[i - 1] > keys[i]) {
          // std::cout << "keys[" << i - 1
          //           << "] = " << std::bitset<sizeof(K) * 8>(keys[i - 1])
          //           << " > keys[" << i
          //           << "] = " << std::bitset<sizeof(K) * 8>(keys[i])
          //           << std::endl;
          return false;
        }
      }
    } else {
      for (std::size_t i = 1; i < num; ++i) {
        if (keys[i - 1] < keys[i]) {
          // std::cout << "keys[" << i - 1
          //           << "] = " << std::bitset<sizeof(K) * 8>(keys[i - 1])
          //           << " < keys[" << i
          //           << "] = " << std::bitset<sizeof(K) * 8>(keys[i])
          //           << std::endl;
          return false;
        }
      }
    }
    return true;
  }

  bool checkThatEveryKeyIsThere(const Data<K, Ps...> &other) const {
    for (std::size_t i = 0; i < other.num; i++) {
      if (std::find(keys, keys + num, other.keys[i]) == keys + num) {
        return false;
      }
    }
    return true;
  }

  std::size_t numUnsorted(const bool up) const {
    std::size_t numUnsorted = 0;
    if (up) {
      for (std::size_t i = 1; i < num; ++i) {
        if (keys[i - 1] > keys[i]) {
          numUnsorted++;
        }
      }
    } else {
      for (std::size_t i = 1; i < num; ++i) {
        if (keys[i - 1] < keys[i]) {
          numUnsorted++;
        }
      }
    }
    return numUnsorted;
  }

  bool checkPayloads() const {
    for (std::size_t i = 0; i < num; ++i) {
      unsigned int seed = 0;
      memcpy(&seed, &keys[i], std::min(sizeof(K), sizeof(seed)));
      srand(seed);
      bool payloadsOk = true;
      const auto checkPayload = [&](const auto *const payload) {
        const auto expectedPayload =
            getRandom<std::remove_reference_t<decltype(*payload)>>();
        return memcmp(&payload[i], &expectedPayload, sizeof(payload[i])) == 0;
      };
      std::apply(
          [&](const Ps *const... payloads) {
            payloadsOk &= (... && checkPayload(payloads));
          },
          payloads);
      if (!payloadsOk) {
        return false;
      }
    }
    return true;
  }

  std::string checkData(const bool up, const Data<K, Ps...> &original,
                        const bool checkSorted = true) const {
    bool passed = true;
    std::string errorMessage = "";
    if (checkSorted && !isSorted(up)) {
      passed = false;
      errorMessage += "not sorted (" + std::to_string(numUnsorted(up)) +
                      " out of " + std::to_string(num - 1) + " pairs)";
    }
    if (!checkPayloads()) {
      if (!passed) {
        errorMessage += ", ";
      }
      passed = false;
      errorMessage += "payloads are not ok";
    }
    // std::is_permutation()
    if (!checkThatEveryKeyIsThere(original)) {
      if (!passed) {
        errorMessage += ", ";
      }
      passed = false;
      errorMessage += "keys are not there";
    }
    if (!original.checkThatEveryKeyIsThere(*this)) {
      if (!passed) {
        errorMessage += ", ";
      }
      passed = false;
      errorMessage += "new keys are there";
    }
    // if not passed see if it was because nothing was done
    if (!passed) {
      if (memcmp(keys, original.keys, num * sizeof(K)) == 0) {
        errorMessage += " (keys are the same)";
      }
    }
    return errorMessage;
  }

  void print() const {
    for (std::size_t i = 0; i < num; ++i) {
      std::cout << "(" << +keys[i];
      std::apply(
          [&](const Ps *const... payloads) {
            (..., (std::cout << ", " << +payloads[i]));
          },
          payloads);
      std::cout << "), ";
    }
    std::cout << std::endl;
  }

  void printBinary() const {
    for (std::size_t i = 0; i < num; ++i) {
      std::cout << std::bitset<sizeof(K) * 8>(keys[i]) << " ";
    }
    std::cout << std::endl;
  }

  void convertToSingleArray(
      DataElement<K, Ps...> *const keysAndPayloads) const {
    for (std::size_t i = 0; i < num; ++i) {
      keysAndPayloads[i].key = keys[i];
      if constexpr (sizeof...(Ps) > 0) {
        std::apply(
            [&](const Ps *const... payloads) {
              keysAndPayloads[i].payloads = std::make_tuple(payloads[i]...);
            },
            payloads);
      }
    }
  }

  void setFromSingleArray(const DataElement<K, Ps...> *const keysAndPayloads) {
    for (std::size_t i = 0; i < num; ++i) {
      keys[i] = keysAndPayloads[i].key;
      if constexpr (sizeof...(Ps) > 0) {
        std::apply(
            [&](const Ps... payloadsIn) {
              std::apply(
                  [&](Ps *const... payloads) {
                    ((payloads[i] = payloadsIn), ...);
                  },
                  payloads);
            },
            keysAndPayloads[i].payloads);
      }
    }
  }

 private:
  void fillKeysUniform(std::mt19937 &gen) {
    if constexpr (std::is_integral_v<K>) {
      std::uniform_int_distribution<K> dist(std::numeric_limits<K>::lowest(),
                                            std::numeric_limits<K>::max());
      for (std::size_t i = 0; i < num; i++) {
        keys[i] = dist(gen);
      }
    } else {
      std::uniform_real_distribution<K> dist(K(-1.0), K(1.0));
      for (std::size_t i = 0; i < num; i++) {
        keys[i] = dist(gen);
      }
    }
  }

  void fillKeysGaussian(std::mt19937 &gen) {
    if constexpr (std::is_integral_v<K>) {
      std::normal_distribution<double> dist(0, 100);
      for (std::size_t i = 0; i < num; i++) {
        keys[i] = round(dist(gen));
      }
    } else {
      std::normal_distribution<K> dist(0, 1.0);
      for (std::size_t i = 0; i < num; i++) {
        keys[i] = dist(gen);
      }
    }
  }

  void makePayloads() {
    for (std::size_t i = 0; i < num; ++i) {
      unsigned int seed = 0;
      memcpy(&seed, &keys[i], std::min(sizeof(K), sizeof(uint)));
      srand(seed);
      std::apply(
          [&](const auto &...payloads) {
            (...,
             (payloads[i] =
                  getRandom<std::remove_reference_t<decltype(payloads[i])>>()));
          },
          payloads);
    }
  }
};
}  // namespace simd_sort