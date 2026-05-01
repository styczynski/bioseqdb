#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <tuple>

#include "common.hpp"
#include "data.hpp"
#include "radix_sort.hpp"
#include "sort_methods.hpp"

namespace simd_sort {

#ifdef __clang__
const std::string dataDir = "/tmp/radixSortData-clang";
#elif __GNUC__
const std::string dataDir = "/tmp/radixSortData-gcc";
#elif
#error "Unknown compiler"
#endif

template <typename SortMethod, typename K, typename... Ps>
double measureTimePerElement(const Data<K, Ps...> &data) {
  Data<K, Ps...> copyOfData(data);
  struct timespec start, end;
  if constexpr (SortMethod::areKeyAndPayloadSeparate) {
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    std::apply(
        [&](auto *const... payloads) {
          SortMethod::template sort(copyOfData.num, copyOfData.keys,
                                    payloads...);
        },
        copyOfData.payloads);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
  } else {
    DataElement<K, Ps...> *keysAndPayloads = (DataElement<K, Ps...> *)malloc(
        copyOfData.num * sizeof(DataElement<K, Ps...>));
    copyOfData.convertToSingleArray(keysAndPayloads);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    SortMethod::template sort(copyOfData.num, keysAndPayloads);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
    copyOfData.setFromSingleArray(keysAndPayloads);
    free(keysAndPayloads);
  }
  if (SortMethod::name().find("NoCmp") == std::string::npos &&
      !copyOfData.isSorted(true)) {
    std::cout << "ERROR: "
              << "not sorted (" + std::to_string(copyOfData.numUnsorted(true)) +
                     " out of " + std::to_string(copyOfData.num - 1) + " pairs)"
              << "(" << data.num << " " << SortMethod::name() << ")"
              << std::endl;
    exit(-1);
  }
  const double nanoSeconds =
      (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
  return nanoSeconds / (double)data.num;
}

template <typename SortMethod, typename K, typename... Ps>
double measureTimePerElementWithRepsAndWarmup(
    const std::size_t num, const InputDistribution distribution) {
  const std::size_t numberOfTests = std::max<std::size_t>(1, (1 << 22) / num);
  const std::size_t numberOfWarmups = std::max<std::size_t>(1, (1 << 18) / num);

  Data<K, Ps...> **const data =
      new Data<K, Ps...> *[numberOfTests + numberOfWarmups];
  for (std::size_t i = 0; i < numberOfTests + numberOfWarmups; i++) {
    data[i] = new Data<K, Ps...>(num, distribution);
  }
  for (std::size_t j = 0; j < numberOfWarmups; j++) {
    measureTimePerElement<SortMethod, K, Ps...>(*data[j]);
  }
  double sum = 0;
  for (std::size_t j = 0; j < numberOfTests; j++) {
    sum +=
        measureTimePerElement<SortMethod, K, Ps...>(*data[j + numberOfWarmups]);
  }
  for (std::size_t i = 0; i < numberOfTests + numberOfWarmups; i++) {
    delete data[i];
  }
  delete[] data;
  return sum / numberOfTests;
}

template <typename SortMethod, typename K, typename... Ps>
double measureTimePerElementThresh(const Data<K, Ps...> &data,
                                   const SortIndex cmpSortThreshold) {
  Data<K, Ps...> copyOfData(data);
  static_assert(SortMethod::hasThreshold, "SortMethod does not have threshold");
  struct timespec start, end;
  if constexpr (SortMethod::areKeyAndPayloadSeparate) {
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    std::apply(
        [&](auto *const... payloads) {
          SortMethod::template sortThresh(cmpSortThreshold, copyOfData.num,
                                          copyOfData.keys, payloads...);
        },
        copyOfData.payloads);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
  } else {
    DataElement<K, Ps...> *const keysAndPayloads =
        (DataElement<K, Ps...> *)malloc(copyOfData.num *
                                        sizeof(DataElement<K, Ps...>));
    copyOfData.convertToSingleArray(keysAndPayloads);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    SortMethod::template sortThresh(cmpSortThreshold, copyOfData.num,
                                    keysAndPayloads);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
    copyOfData.setFromSingleArray(keysAndPayloads);
    free(keysAndPayloads);
  }
  if (SortMethod::name().find("NoCmp") == std::string::npos &&
      !copyOfData.isSorted(true)) {
    std::cout << "ERROR: "
              << "not sorted (" + std::to_string(copyOfData.numUnsorted(true)) +
                     " out of " + std::to_string(copyOfData.num - 1) + " pairs)"
              << "(" << data.num << " " << SortMethod::name() << ")"
              << std::endl;
    exit(-1);
  }
  return ((end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec)) /
         (double)data.num;
}

template <typename SortMethod, typename K, typename... Ps>
double measureTimePerElementThreshWithRepsAndWarmup(
    const std::size_t num, const InputDistribution distribution,
    const SortIndex cmpSortThreshold) {
  const std::size_t numberOfTests = std::max<std::size_t>(1, (1 << 22) / num);
  const std::size_t numberOfWarmups = std::max<std::size_t>(1, (1 << 18) / num);

  Data<K, Ps...> **const data =
      new Data<K, Ps...> *[numberOfTests + numberOfWarmups];
  for (std::size_t i = 0; i < numberOfTests + numberOfWarmups; i++) {
    data[i] = new Data<K, Ps...>(num, distribution);
  }
  for (std::size_t j = 0; j < numberOfWarmups; j++) {
    measureTimePerElementThresh<SortMethod, K, Ps...>(*data[j],
                                                      cmpSortThreshold);
  }
  double sum = 0;
  for (std::size_t j = 0; j < numberOfTests; j++) {
    sum += measureTimePerElementThresh<SortMethod, K, Ps...>(
        *data[j + numberOfWarmups], cmpSortThreshold);
  }
  for (std::size_t i = 0; i < numberOfTests + numberOfWarmups; i++) {
    delete data[i];
  }
  delete[] data;
  return sum / numberOfTests;
}

template <typename SortMethod, InputDistribution Distribution, typename K,
          typename... Ps>
void perfTestThresh() {
  if (system(("mkdir -p " + dataDir).c_str()) != 0) {
    std::cout << "ERROR: Could not create directory " << dataDir << std::endl;
    exit(-1);
  }
  std::string perfDescription = std::string("cmpThresh-") + type_name<K>;
  ((perfDescription += std::string("-") + type_name<Ps>), ...);
  perfDescription += std::string("-") + SortMethod::name() + "-" +
                     inputDistributionToString(Distribution);
  std::ofstream file(dataDir + "/" + perfDescription + ".dat");
  file << std::fixed;
  file << std::setprecision(6);
  std::cout << "Performing perf test for " << perfDescription << " ..."
            << std::endl;
  const std::size_t num = 1 << 18;

  file << "cmpThresh"
       << " " << num << std::endl;
  const SortIndex maxThresh =
      std::is_same_v<SortMethod,
                     SortMethodRadixSort<radix_sort::BitSorterSIMD<false>,
                                         CmpSorterNoSort>>
          ? num
          : 512;
  const SortIndex minThresh =
      std::is_same_v<SortMethod,
                     SortMethodRadixSort<radix_sort::BitSorterSIMD<false>,
                                         CmpSorterNoSort>>
          ? 1
          : 2;
  for (SortIndex thresh = minThresh; thresh <= maxThresh; thresh *= 2) {
    file << thresh;
    if (std::is_same_v<SortMethod, SortMethodBramas> &&
        (Distribution == InputDistribution::Zero ||
         Distribution == InputDistribution::ZeroOne ||
         Distribution == InputDistribution::Sorted) &&
        num > 1 << 18) {
      // bramas uses quicksort wich has a runtime of O(n^2) for constant
      // input, so only execute tests of the number of elements is small
      // enough to avoid taking too long
      file << " " << 0;
      continue;
    }
    file << " "
         << measureTimePerElementThreshWithRepsAndWarmup<SortMethod, K, Ps...>(
                num, Distribution, thresh);
    file << std::endl;
  }

  file.close();
  std::cout << "Perf test for " << perfDescription << " done." << std::endl;
}

template <typename SortMethod, typename SortMethodRelTo,
          InputDistribution Distribution, std::size_t Payloads, typename K,
          typename... Ps>
static std::string perfTestSpeedup(const size_t num) {
  std::string result = "";
  if ((std::is_same_v<SortMethodRelTo, SortMethodBramas> ||
       std::is_same_v<SortMethod,
                      SortMethodBramas>)&&(Distribution ==
                                               InputDistribution::Zero ||
                                           Distribution ==
                                               InputDistribution::ZeroOne ||
                                           Distribution ==
                                               InputDistribution::Sorted) &&
      num > 1 << 18) {
    // bramas uses quicksort wich has a runtime of O(n^2) for constant
    // input, so only execute tests of the number of elements is small
    // enough to avoid taking too long
    return "";
  }
  if constexpr (!SortMethodRelTo::template isSupported<true, K, Ps...>() ||
                !SortMethod::template isSupported<true, K, Ps...>()) {
    return "";
  }
  const double sumTimePerElementRel =
      measureTimePerElementWithRepsAndWarmup<SortMethodRelTo, K, Ps...>(
          num, Distribution);
  const double sumTimePerElement =
      measureTimePerElementWithRepsAndWarmup<SortMethod, K, Ps...>(
          num, Distribution);
  result += type_name<K>;
  if constexpr (Payloads == 1) {
    ((result += std::string("-") + type_name<Ps>), ...);
  }
  result += " ";
  result += std::to_string(sumTimePerElementRel / sumTimePerElement);
  result += "\n";
  return result;
}

template <typename SortMethod, typename SortMethodRelTo,
          InputDistribution Distribution, bool WithoutPayload,
          std::size_t Payloads, std::size_t PayloadFactor = 1>
static void perfTestSpeedupAllKP() {
  const std::size_t num = 1 << 18;
  if (system(("mkdir -p " + dataDir).c_str()) != 0) {
    std::cout << "ERROR: Could not create directory " << dataDir << std::endl;
    exit(-1);
  }
  std::string perfDescription = std::string() + "relTo" +
                                SortMethodRelTo::name() + "-" +
                                SortMethod::name() + "-";
  if constexpr (WithoutPayload) {
    perfDescription += "withoutPayload";
  } else {
    perfDescription += "withPayloadFactor" + std::to_string(PayloadFactor);
  }
  perfDescription += std::string("-") +
                     inputDistributionToString(Distribution) +
                     std::string("-") + std::to_string(num);
  std::ofstream file(dataDir + "/" + perfDescription + ".dat");
  file << std::fixed;
  file << std::setprecision(6);
  std::cout << "Performing perf test for " << perfDescription << " ..."
            << std::endl;
  file << "key_payloads speedup" << std::endl;
  if constexpr (WithoutPayload) {
    file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution, Payloads,
                            float>(num);
    file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution, Payloads,
                            double>(num);
    file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution, Payloads,
                            int8_t>(num);
    file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution, Payloads,
                            int16_t>(num);
    file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution, Payloads,
                            int32_t>(num);
    file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution, Payloads,
                            int64_t>(num);
  } else {
    if constexpr (Payloads == 0) {
      if constexpr (PayloadFactor == 1) {
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, float, int32_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, double, int64_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int8_t, int8_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int16_t, int16_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int32_t, int32_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int64_t, int64_t>(num);
      } else if constexpr (PayloadFactor == 2) {
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, float, int16_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, double, int32_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int16_t, int8_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int32_t, int16_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int64_t, int32_t>(num);
      } else if constexpr (PayloadFactor == 4) {
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, float, int8_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, double, int16_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int32_t, int8_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int64_t, int16_t>(num);
      } else if constexpr (PayloadFactor == 8) {
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, double, int8_t>(num);
        file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                                Payloads, int64_t, int8_t>(num);
      }
    } else if constexpr (Payloads == 1) {
      file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                              Payloads, float, int64_t>(num);
      file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                              Payloads, int8_t, int64_t>(num);
      file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                              Payloads, int16_t, int64_t>(num);
      file << perfTestSpeedup<SortMethod, SortMethodRelTo, Distribution,
                              Payloads, int32_t, int64_t>(num);
    }
  }

  file.close();
  std::cout << "Perf test for " << perfDescription << " done." << std::endl;
}

template <typename SortMethod, typename SortMethodRelTo,
          InputDistribution Distribution, std::size_t Payloads = 0>
static void perfTestSpeedupAll() {
  perfTestSpeedupAllKP<SortMethod, SortMethodRelTo, Distribution, true,
                       Payloads, 0>();
  perfTestSpeedupAllKP<SortMethod, SortMethodRelTo, Distribution, false,
                       Payloads, 1>();
  perfTestSpeedupAllKP<SortMethod, SortMethodRelTo, Distribution, false,
                       Payloads, 2>();
  perfTestSpeedupAllKP<SortMethod, SortMethodRelTo, Distribution, false,
                       Payloads, 4>();
  perfTestSpeedupAllKP<SortMethod, SortMethodRelTo, Distribution, false,
                       Payloads, 8>();
}

template <typename... SortMethods>
struct PerfTest {
  static_assert(sizeof...(SortMethods) > 0, "No sort methods provided");

  template <InputDistribution Distribution, typename K, typename... Ps>
  static void perfTestNum() {
    if (system(("mkdir -p " + dataDir).c_str()) != 0) {
      std::cout << "ERROR: Could not create directory " << dataDir << std::endl;
      exit(-1);
    }
    std::string perfDescription = std::string("tpe-") + type_name<K>;
    ((perfDescription += std::string("-") + type_name<Ps>), ...);
    perfDescription +=
        std::string("-") + inputDistributionToString(Distribution);
    std::ofstream file(dataDir + "/" + perfDescription + ".dat");
    file << std::fixed;
    file << std::setprecision(6);
    std::cout << "Performing perf test for " << perfDescription << " ..."
              << std::endl;
    file << "number_of_elements";
    ((file << " " << SortMethods::name()), ...);
    file << std::endl;
    std::size_t maxNum = 1 << 22;
    for (std::size_t num = 1; num <= maxNum; num *= 2) {
      file << num;
      (
          [&]() {
            if (std::is_same_v<SortMethods, SortMethodBramas> &&
                (Distribution == InputDistribution::Zero ||
                 Distribution == InputDistribution::ZeroOne ||
                 Distribution == InputDistribution::Sorted) &&
                num > 1 << 18) {
              // bramas uses quicksort wich has a runtime of O(n^2) for constant
              // input, so only execute tests of the number of elements is small
              // enough to avoid taking too long
              file << " 0";
              return;
            }
            if constexpr (SortMethods::template isSupported<true, K, Ps...>()) {
              file << " "
                   << measureTimePerElementWithRepsAndWarmup<SortMethods, K,
                                                             Ps...>(
                          num, Distribution);
            } else {
              file << " nan";
            }
          }(),
          ...);
      file << std::endl;
    }
    file.close();
    std::cout << "Perf test for " << perfDescription << " done." << std::endl;
  }

  template <InputDistribution Distribution, typename K, typename... Ps>
  static void perfTest() {
    const std::size_t num = 1 << 18;
    if (system(("mkdir -p " + dataDir).c_str()) != 0) {
      std::cout << "ERROR: Could not create directory " << dataDir << std::endl;
      exit(-1);
    }
    std::string perfDescription = std::string() + type_name<K>;
    ((perfDescription += std::string("-") + type_name<Ps>), ...);
    perfDescription += std::string("-") +
                       inputDistributionToString(Distribution) +
                       std::string("-") + std::to_string(num);
    std::ofstream file(dataDir + "/" + perfDescription + ".dat");
    std::cout << "Performing perf test for " << perfDescription << " ..."
              << std::endl;
    file << std::fixed;
    file << std::setprecision(6);
    file << "sort_method nanoseconds_per_element" << std::endl;
    (
        [&]() {
          if (std::is_same_v<SortMethods, SortMethodBramas> &&
              (Distribution == InputDistribution::Zero ||
               Distribution == InputDistribution::ZeroOne ||
               Distribution == InputDistribution::Sorted) &&
              num > 1 << 18) {
            // bramas uses quicksort wich has a runtime of O(n^2) for constant
            // input, so only execute tests of the number of elements is small
            // enough to avoid taking too long
            file << SortMethods::name() << " " << 0 << std::endl;
            return;
          }
          if constexpr (SortMethods::template isSupported<true, K, Ps...>()) {
            file << SortMethods::name() << " "
                 << measureTimePerElementWithRepsAndWarmup<SortMethods, K,
                                                           Ps...>(num,
                                                                  Distribution)
                 << std::endl;
          }
        }(),
        ...);
    file << std::endl;
    file.close();
    std::cout << "Perf test for " << perfDescription << " done." << std::endl;
  }

  template <InputDistribution Distribution, typename K, typename... Ps>
  static void thresh() {
    (
        [&]() {
          if constexpr (SortMethods::hasThreshold &&
                        SortMethods::template isSupported<true, K, Ps...>()) {
            perfTestThresh<SortMethods, Distribution, K, Ps...>();
          }
        }(),
        ...);
  }
};
}  // namespace simd_sort