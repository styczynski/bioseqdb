#include "common_simd.h"
#include "common_tests.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

import bioseq.highway.dispatch_contract;

namespace simd_dispatch = bioseq::simd_dispatch;

TEST_CASE("Highway dispatch contract has an exact coarse-kernel inventory",
          "[highway][dispatch][contract]") {
    using simd_dispatch::HighwayKernelId;
    using simd_dispatch::HighwayKernelSet;

    STATIC_REQUIRE(std::is_same_v<
        std::underlying_type_t<HighwayKernelId>, std::uint8_t>);
    STATIC_REQUIRE(simd_dispatch::highway_kernel_count == 9u);
    STATIC_REQUIRE(std::is_standard_layout_v<
        simd_dispatch::HighwayTargetBinding>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<
        simd_dispatch::HighwayTargetBinding>);
    STATIC_REQUIRE(std::is_standard_layout_v<
        simd_dispatch::HighwayDispatchInfo>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<
        simd_dispatch::HighwayDispatchInfo>);

    simd_dispatch::HighwayKernelTable table{};
    HighwayKernelSet expected{};
    constexpr std::array<std::string_view,
                         simd_dispatch::highway_kernel_count>
        expected_names{
            "minimizer::reference_minimap_int",
            "minimizer::two_stack_nthash",
            "minimizer::paper_nthash",
            "indexing::grouped32_validate",
            "chaining::chain_anchors_simd",
            "prefilter::sneaky_snake_simd_batch",
            "alignment::ksw2",
            "alignment::ksw2_batched",
            "alignment::block_aligner",
        };

    REQUIRE_FALSE(table.available_kernels.any());
    REQUIRE(table.contains_all(expected));
    REQUIRE(table.common.sneaky_snake_simd_batch == nullptr);
    REQUIRE(table.chaining.chain_anchors_simd == nullptr);
    REQUIRE(table.indexing.validate_non_scatter_local32_w2 == nullptr);

    for (std::size_t index = 0;
         index < simd_dispatch::highway_kernel_count;
         ++index) {
        const auto kernel = static_cast<HighwayKernelId>(index);
        const HighwayKernelSet singleton =
            simd_dispatch::highway_kernel_set(kernel);
        CAPTURE(index, simd_dispatch::highway_kernel_name(kernel));
        REQUIRE(singleton.count() == 1u);
        REQUIRE(simd_dispatch::highway_kernel_name(kernel)
                == expected_names[index]);
        REQUIRE_FALSE(table.contains(kernel));

        expected |= singleton;
        table.available_kernels |= singleton;
        REQUIRE(table.contains(kernel));
        REQUIRE(table.contains_all(expected));
    }

    REQUIRE(table.available_kernels.count()
            == simd_dispatch::highway_kernel_count);
    REQUIRE(simd_dispatch::highway_kernel_set(HighwayKernelId::count).none());
    REQUIRE(simd_dispatch::highway_kernel_name(HighwayKernelId::count)
            == "unknown");
    REQUIRE_FALSE(table.contains(HighwayKernelId::count));
}

TEST_CASE("Highway dispatch inventory preserves supported and runnable masks",
          "[highway][dispatch][inventory]") {
    const auto generated_and_supported =
        hwy::SupportedAndGeneratedTargets();
    REQUIRE_FALSE(generated_and_supported.empty());

    std::int64_t compiled_targets = 0;
    for (const std::int64_t target : generated_and_supported) {
        compiled_targets |= target;
    }
    const simd_dispatch::HighwayDispatchInfo info =
        simd_dispatch::highway_dispatch_info(compiled_targets);

    REQUIRE(info.compiled_targets == compiled_targets);
    REQUIRE(info.cpu_supported_targets == hwy::SupportedTargets());
    REQUIRE(info.runnable_targets
            == (info.compiled_targets & info.cpu_supported_targets));
    REQUIRE(info.runnable_targets == compiled_targets);
    REQUIRE(info.planned_target == generated_and_supported.front());

    const auto planned_names =
        simd_dispatch::highway_target_names(info.planned_target);
    REQUIRE(planned_names.size() == 1u);
    REQUIRE(planned_names.front() == hwy::TargetName(info.planned_target));

    const auto runnable_names =
        simd_dispatch::highway_target_names(info.runnable_targets);
    REQUIRE(runnable_names.size() == generated_and_supported.size());
    for (std::size_t index = 0; index < runnable_names.size(); ++index) {
        CAPTURE(index, runnable_names[index]);
        REQUIRE(runnable_names[index]
                == hwy::TargetName(generated_and_supported[index]));
    }

    const auto supported_names =
        simd_dispatch::highway_target_names(info.cpu_supported_targets);
    REQUIRE(supported_names.size() == static_cast<std::size_t>(std::popcount(
        static_cast<std::uint64_t>(info.cpu_supported_targets))));
    for (const std::string_view name : supported_names) {
        CAPTURE(name);
        REQUIRE_FALSE(name.empty());
        REQUIRE(name != "Unknown");
    }

    const simd_dispatch::HighwayDispatchInfo empty =
        simd_dispatch::highway_dispatch_info(0);
    REQUIRE(empty.compiled_targets == 0);
    REQUIRE(empty.runnable_targets == 0);
    REQUIRE(empty.planned_target == 0);
    REQUIRE(simd_dispatch::highway_target_names(0).empty());
}
