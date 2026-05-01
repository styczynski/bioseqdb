#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>

#include "cmp_sorters.hpp"
#include "data.hpp"
#include "perf.hpp"
#include "quick_sort.hpp"
#include "radix_sort.hpp"
#include "sort_methods.hpp"

using namespace simd_sort;

int main() {
  std::cout << std::fixed;
  std::cout << std::setprecision(6);
#ifdef IPP_RADIX_IS_PRESENT_
  std::cout << "Initializing IPP..." << std::endl;
  ipp_radix::ippInit();
#endif
  using AllSorts = PerfTest<
      SortMethodQuickSort<quick_sort::PartitionerSequential,
                          CmpSorterInsertionSort>,
      SortMethodQuickSort<quick_sort::PartitionerSIMD, CmpSorterInsertionSort>,
      SortMethodQuickSort<quick_sort::PartitionerSIMD,
                          CmpSorterBramasSmallSort>,
      SortMethodRadixSort<radix_sort::BitSorterSIMD<false>,
                          CmpSorterInsertionSort>,
      SortMethodRadixSort<radix_sort::BitSorterSIMD<false>,
                          CmpSorterBramasSmallSort>,
#ifdef IPP_RADIX_IS_PRESENT_
      SortMethodIPPRadix,
#endif
      SortMethodSTLSort, SortMethodBramas, SortMethodBlacher>;

  std::function<void()> testFunctions[] = {
      AllSorts::thresh<InputDistribution::Uniform, float>,
      AllSorts::thresh<InputDistribution::Uniform, double>,
      AllSorts::thresh<InputDistribution::Uniform, int32_t>,
      AllSorts::thresh<InputDistribution::Uniform, int32_t, int32_t>,
      AllSorts::thresh<InputDistribution::Uniform, float, int32_t>,
      AllSorts::thresh<InputDistribution::Uniform, float, int64_t>,
      AllSorts::thresh<InputDistribution::Uniform, double, int32_t>,
      AllSorts::thresh<InputDistribution::Uniform, double, int64_t>,
      AllSorts::thresh<InputDistribution::Uniform, float, int32_t, int64_t>,

      AllSorts::perfTest<InputDistribution::Uniform, int32_t, int32_t>,
      AllSorts::perfTest<InputDistribution::Gaussian, int32_t, int32_t>,
      AllSorts::perfTest<InputDistribution::Zero, int32_t, int32_t>,
      AllSorts::perfTest<InputDistribution::ZeroOne, int32_t, int32_t>,
      AllSorts::perfTest<InputDistribution::Sorted, int32_t, int32_t>,
      AllSorts::perfTest<InputDistribution::ReverseSorted, int32_t, int32_t>,
      AllSorts::perfTest<InputDistribution::AlmostSorted, int32_t, int32_t>,
      AllSorts::perfTest<InputDistribution::AlmostReverseSorted, int32_t,
                         int32_t>,

      AllSorts::perfTest<InputDistribution::Uniform, int32_t>,
      AllSorts::perfTest<InputDistribution::Gaussian, int32_t>,
      AllSorts::perfTest<InputDistribution::Zero, int32_t>,
      AllSorts::perfTest<InputDistribution::ZeroOne, int32_t>,
      AllSorts::perfTest<InputDistribution::Sorted, int32_t>,
      AllSorts::perfTest<InputDistribution::ReverseSorted, int32_t>,
      AllSorts::perfTest<InputDistribution::AlmostSorted, int32_t>,
      AllSorts::perfTest<InputDistribution::AlmostReverseSorted, int32_t>,

      AllSorts::perfTest<InputDistribution::Uniform, float>,
      AllSorts::perfTest<InputDistribution::Gaussian, float>,
      AllSorts::perfTest<InputDistribution::Zero, float>,
      AllSorts::perfTest<InputDistribution::ZeroOne, float>,
      AllSorts::perfTest<InputDistribution::Sorted, float>,
      AllSorts::perfTest<InputDistribution::ReverseSorted, float>,
      AllSorts::perfTest<InputDistribution::AlmostSorted, float>,
      AllSorts::perfTest<InputDistribution::AlmostReverseSorted, float>,

      AllSorts::perfTest<InputDistribution::Uniform, double>,
      AllSorts::perfTest<InputDistribution::Gaussian, double>,
      AllSorts::perfTest<InputDistribution::Zero, double>,
      AllSorts::perfTest<InputDistribution::ZeroOne, double>,
      AllSorts::perfTest<InputDistribution::Sorted, double>,
      AllSorts::perfTest<InputDistribution::ReverseSorted, double>,
      AllSorts::perfTest<InputDistribution::AlmostSorted, double>,
      AllSorts::perfTest<InputDistribution::AlmostReverseSorted, double>,

      AllSorts::perfTest<InputDistribution::Uniform, float, int32_t>,
      AllSorts::perfTest<InputDistribution::Gaussian, float, int32_t>,
      AllSorts::perfTest<InputDistribution::Zero, float, int32_t>,
      AllSorts::perfTest<InputDistribution::ZeroOne, float, int32_t>,
      AllSorts::perfTest<InputDistribution::Sorted, float, int32_t>,
      AllSorts::perfTest<InputDistribution::ReverseSorted, float, int32_t>,
      AllSorts::perfTest<InputDistribution::AlmostSorted, float, int32_t>,
      AllSorts::perfTest<InputDistribution::AlmostReverseSorted, float,
                         int32_t>,

      AllSorts::perfTest<InputDistribution::Uniform, uint8_t>,
      AllSorts::perfTest<InputDistribution::Gaussian, uint8_t>,
      AllSorts::perfTest<InputDistribution::Zero, uint8_t>,
      AllSorts::perfTest<InputDistribution::ZeroOne, uint8_t>,
      AllSorts::perfTest<InputDistribution::Sorted, uint8_t>,
      AllSorts::perfTest<InputDistribution::ReverseSorted, uint8_t>,
      AllSorts::perfTest<InputDistribution::AlmostSorted, uint8_t>,
      AllSorts::perfTest<InputDistribution::AlmostReverseSorted, uint8_t>,

      AllSorts::perfTest<InputDistribution::Uniform, int16_t>,
      AllSorts::perfTest<InputDistribution::Gaussian, int16_t>,
      AllSorts::perfTest<InputDistribution::Zero, int16_t>,
      AllSorts::perfTest<InputDistribution::ZeroOne, int16_t>,
      AllSorts::perfTest<InputDistribution::Sorted, int16_t>,
      AllSorts::perfTest<InputDistribution::ReverseSorted, int16_t>,
      AllSorts::perfTest<InputDistribution::AlmostSorted, int16_t>,
      AllSorts::perfTest<InputDistribution::AlmostReverseSorted, int16_t>,

      AllSorts::perfTestNum<InputDistribution::Uniform, int32_t>,
      AllSorts::perfTestNum<InputDistribution::Zero, int32_t>,
  };

  for (auto &testFunction : testFunctions) {
    testFunction();
  }
}
