#include <stdlib.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "cmp_sorters.hpp"
#include "common.hpp"
#include "data.hpp"
#include "quick_sort.hpp"
#include "radix_sort.hpp"
#include "sort_methods.hpp"

using namespace simd_sort;

template <bool Combined, bool Up, typename SortMethod, typename K,
          typename... Ps>
bool test(InputDistribution distribution, const std::size_t num,
          const unsigned int seed = time(NULL)) {
  std::cout << "Testing: " << type_name<K>;
  if constexpr (sizeof...(Ps) > 0) {
    ((std::cout << "-" << type_name<Ps>), ...);
  }
  std::cout << ", ";
  if constexpr (Combined) {
    std::cout << "Combined, ";
  } else {
    std::cout << "Separate, ";
  }
  std::cout << "Distribution: " << inputDistributionToString(distribution)
            << ", "
            << "Up: " << Up << ", " << SortMethod::name() << ", ";
  if constexpr (false && Combined && sizeof(DataElement<K, Ps...>) <= 8) {
    std::cout << "NOT TESTING" << std::endl;
    return true;
  }
  Data<K, Ps...> data(num, distribution, seed);
  const Data<K, Ps...> copyOfData(data);
  // std::cout << std::endl; data.printBinary();
  if constexpr (Combined) {
    DataElement<K, Ps...> *keysAndPayloads = (DataElement<K, Ps...> *)malloc(
        data.num * sizeof(DataElement<K, Ps...>));
    data.convertToSingleArray(keysAndPayloads);
    SortMethod::template sortThresh<Up>(16, data.num, keysAndPayloads);
    data.setFromSingleArray(keysAndPayloads);
    free(keysAndPayloads);
  } else {
    std::apply(
        [&](auto *const... payloads) {
          SortMethod::template sortThresh<Up>(16, data.num, data.keys,
                                              payloads...);
        },
        data.payloads);
  }
  // data.printBinary();
  // data.keys[1] = data.keys[0];
  // std::apply(
  //   [&](auto *const ...payloads) {
  //     ((payloads[1] = payloads[0]), ...);
  //   },
  //   data.payloads);
  const std::string errorMessage = data.checkData(Up, copyOfData);
  if (errorMessage.empty()) {
    std::cout << "passed" << std::endl;
    return true;
  } else {
    std::cout << "FAILED: " << errorMessage << std::endl;
    return false;
  }
}

template <bool Combined, bool Up, typename SortMethod, typename K,
          typename... Ps>
bool testAllDistributions(const std::size_t num,
                          const unsigned int seed = time(NULL)) {
  if constexpr (SortMethod::template isSupported<Up, K, Ps...>()) {
    if constexpr (Combined && !is_power_of_two<sizeof(DataElement<K, Ps...>)>) {
      return true;
    } else {
      bool passed = true;
      for (const auto distribution : std::vector<InputDistribution>{
               InputDistribution::Gaussian, InputDistribution::Uniform,
               InputDistribution::Zero, InputDistribution::ZeroOne,
               InputDistribution::Sorted, InputDistribution::ReverseSorted,
               InputDistribution::AlmostSorted,
               InputDistribution::AlmostReverseSorted}) {
        passed &=
            test<Combined, Up, SortMethod, K, Ps...>(distribution, num, seed);
      }
      return passed;
    }
  }
  return true;
}

template <bool Combined, bool Up, typename SortMethod, typename K>
bool testAllPayloads(const std::size_t num,
                     const unsigned int seed = time(NULL)) {
  bool passed = true;
  passed &= testAllDistributions<Combined, Up, SortMethod, K>(num, seed);
  passed &=
      testAllDistributions<Combined, Up, SortMethod, K, uint8_t>(num, seed);
  passed &=
      testAllDistributions<Combined, Up, SortMethod, K, uint16_t>(num, seed);
  passed &=
      testAllDistributions<Combined, Up, SortMethod, K, uint32_t>(num, seed);
  passed &=
      testAllDistributions<Combined, Up, SortMethod, K, uint64_t>(num, seed);
  passed &=
      testAllDistributions<Combined, Up, SortMethod, K, uint64_t, uint8_t>(
          num, seed);
  passed &=
      testAllDistributions<Combined, Up, SortMethod, K, uint64_t, uint64_t>(
          num, seed);
  passed &= testAllDistributions<Combined, Up, SortMethod, K, uint64_t,
                                 uint64_t, uint64_t>(num, seed);
  passed &=
      testAllDistributions<Combined, Up, SortMethod, K, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>(
          num, seed);
  passed &= testAllDistributions<
      Combined, Up, SortMethod, K, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
      uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
      uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
      uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
      uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
      uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
      uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
      uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
      uint8_t, uint8_t>(num, seed);
  passed &=
      testAllDistributions<Combined, Up, SortMethod, K, uint8_t, uint8_t,
                           uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
                           uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
                           uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
                           uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
                           uint8_t, uint8_t, uint8_t, uint8_t, uint8_t>(num,
                                                                        seed);
  passed &= testAllDistributions<Combined, Up, SortMethod, K, uint8_t, uint8_t,
                                 uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
                                 uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
                                 uint8_t, uint8_t, uint8_t>(num, seed);
  passed &= testAllDistributions<Combined, Up, SortMethod, K, uint8_t, uint8_t,
                                 uint8_t, uint8_t, uint8_t, uint8_t, uint8_t>(
      num, seed);
  passed &= testAllDistributions<Combined, Up, SortMethod, K, uint8_t, uint8_t,
                                 uint8_t>(num, seed);
  return passed;
}

template <bool Combined, bool Up, typename SortMethod>
bool testAllTypes(const std::size_t num, const unsigned int seed = time(NULL)) {
  bool passed = true;
  passed &= testAllPayloads<Combined, Up, SortMethod, uint8_t>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, int8_t>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, uint16_t>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, int16_t>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, uint32_t>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, int32_t>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, uint64_t>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, int64_t>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, float>(num, seed);
  passed &= testAllPayloads<Combined, Up, SortMethod, double>(num, seed);
  return passed;
}

template <typename SortMethod>
bool testAll(const std::size_t num, const unsigned int seed = time(NULL)) {
  bool passed = true;
  passed &= testAllTypes<false, true, SortMethod>(num, seed);
  passed &= testAllTypes<false, false, SortMethod>(num, seed);
  passed &= testAllTypes<true, true, SortMethod>(num, seed);
  passed &= testAllTypes<true, false, SortMethod>(num, seed);
  return passed;
}

int main(int argc, char const *argv[]) {
  const std::size_t maxNum = argc > 1 ? std::stoi(argv[1]) : 10000;
  const unsigned int seed = argc > 2 ? std::stoi(argv[2]) : time(NULL);
  bool passed = true;
  for (std::size_t num = 1; num <= maxNum; num *= 10) {
    std::cout << "Testing " << num << " elements" << std::endl;
    passed &= testAll<SortMethodRadixSort<radix_sort::BitSorterSequential,
                                          CmpSorterInsertionSort>>(num, seed);
    passed &= testAll<SortMethodRadixSort<radix_sort::BitSorterSIMD<false>,
                                          CmpSorterInsertionSort>>(num, seed);
    passed &= testAll<SortMethodRadixSort<radix_sort::BitSorterSIMD<true>,
                                          CmpSorterInsertionSort>>(num, seed);

    passed &=
        testAllTypes<false, true,
                     SortMethodRadixSort<radix_sort::BitSorterSIMD<false>,
                                         CmpSorterBramasSmallSort>>(num, seed);
    passed &=
        testAllTypes<false, false,
                     SortMethodRadixSort<radix_sort::BitSorterSIMD<false>,
                                         CmpSorterBramasSmallSort>>(num, seed);
    passed &=
        testAllTypes<false, true,
                     SortMethodRadixSort<radix_sort::BitSorterSIMD<true>,
                                         CmpSorterBramasSmallSort>>(num, seed);
    passed &=
        testAllTypes<false, false,
                     SortMethodRadixSort<radix_sort::BitSorterSIMD<true>,
                                         CmpSorterBramasSmallSort>>(num, seed);
    passed &= testAll<SortMethodQuickSort<quick_sort::PartitionerSequential,
                                          CmpSorterInsertionSort>>(num, seed);
    passed &=
        testAll<SortMethodQuickSort<quick_sort::PartitionerSIMD,
                                    CmpSorterInsertionSort, false>>(num, seed);
  }

  if (passed) {
    std::cout << "All tests passed" << std::endl;
    return 0;
  } else {
    std::cout << "Tests failed, see above for details" << std::endl;
    return 1;
  }
}
