/**
 * @file
 * @brief Verifies Highway kernel inventory, alignment tables, and target masks.
 */

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <hwy/highway.h>
#include <limits>
#include <span>
#include <string_view>
#include <tests/support/common.hpp>
#include <type_traits>
#include <vector>

import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;

namespace simd_dispatch = bioseq::simd_dispatch;

TEST_CASE("Highway dispatch contract has an exact coarse-kernel inventory",
          "[component:bioseq][layer:unit][feature:simd_primitives]"
          "[resource:highway]") {
    using simd_dispatch::highway_kernel;
    using simd_dispatch::highway_kernel_mask;

    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<highway_kernel>, std::uint8_t>);
    STATIC_REQUIRE(simd_dispatch::highway_kernel_count == 8u);
    STATIC_REQUIRE(std::is_standard_layout_v<simd_dispatch::highway_target_binding>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<simd_dispatch::highway_target_binding>);
    STATIC_REQUIRE(std::is_standard_layout_v<simd_dispatch::highway_dispatch_plan>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<simd_dispatch::highway_dispatch_plan>);

    simd_dispatch::highway_kernel_table table{};
    highway_kernel_mask expected{};
    constexpr std::array<std::string_view, simd_dispatch::highway_kernel_count> expected_names{
        "minimizer::minimap2_reference",
        "minimizer::two_stack_nthash",
        "minimizer::bounded_nthash",
        "chaining::chain_anchors_simd",
        "prefilter::sneaky_snake_simd_batch",
        "alignment::ksw2",
        "alignment::ksw2_batched",
        "alignment::block_aligner",
    };

    REQUIRE_FALSE(table.available_kernels.any());
    REQUIRE(table.has_all_kernels(expected));
    REQUIRE(table.common.sneaky_snake_simd_batch == nullptr);
    REQUIRE(table.chaining.chain_anchors_simd == nullptr);

    for (std::size_t index = 0; index < simd_dispatch::highway_kernel_count; ++index) {
        const auto kernel = static_cast<highway_kernel>(index);
        const highway_kernel_mask singleton = simd_dispatch::highway_kernel_mask_for(kernel);
        CAPTURE(index, simd_dispatch::highway_kernel_name(kernel));
        REQUIRE(singleton.count() == 1u);
        REQUIRE(simd_dispatch::highway_kernel_name(kernel) == expected_names[index]);
        REQUIRE_FALSE(table.has_kernel(kernel));

        expected |= singleton;
        table.available_kernels |= singleton;
        REQUIRE(table.has_kernel(kernel));
        REQUIRE(table.has_all_kernels(expected));
    }

    REQUIRE(table.available_kernels.count() == simd_dispatch::highway_kernel_count);
    REQUIRE(simd_dispatch::highway_kernel_mask_for(highway_kernel::count).none());
    REQUIRE(simd_dispatch::highway_kernel_name(highway_kernel::count) == "unknown");
    REQUIRE_FALSE(table.has_kernel(highway_kernel::count));
}

TEST_CASE("Highway alignment tables require all six typed callables",
          "[component:bioseq][layer:unit][feature:simd_primitives]"
          "[resource:highway]") {
    using alignment_table_type = bioseq::simd::alignment_kernels<seq::dna4>;

    const auto& binding = simd_dispatch::highway_test_target_binding();
    REQUIRE(binding.target != 0);
    const auto& generated_root = simd_dispatch::highway_test_kernel_table();
    REQUIRE(binding.kernels == &generated_root);
    REQUIRE(generated_root.is_complete());

    const auto* identity = generated_root.resolve_alphabet_identity<seq::dna4>();
    REQUIRE(identity != nullptr);
    const bioseq::simd::alignment_table_entry* generated_entry = nullptr;
    for (const auto& entry : generated_root.alignment_by_alphabet) {
        if (entry.alphabet_code == identity->alphabet_code) {
            REQUIRE(generated_entry == nullptr);
            generated_entry = &entry;
        }
    }
    REQUIRE(generated_entry != nullptr);
    REQUIRE(generated_entry->typed_table_complete);
    REQUIRE(generated_entry->typed_table != nullptr);

    const alignment_table_type complete_table =
        *static_cast<const alignment_table_type*>(generated_entry->typed_table);
    REQUIRE(bioseq::simd::is_alignment_kernel_table_complete(complete_table));

    alignment_table_type incomplete_table = complete_table;
    incomplete_table.align_ksw2_single_affine = nullptr;
    REQUIRE_FALSE(bioseq::simd::is_alignment_kernel_table_complete(incomplete_table));
    incomplete_table = complete_table;
    incomplete_table.align_ksw2_dual_affine = nullptr;
    REQUIRE_FALSE(bioseq::simd::is_alignment_kernel_table_complete(incomplete_table));
    incomplete_table = complete_table;
    incomplete_table.align_ksw2_batched = nullptr;
    REQUIRE_FALSE(bioseq::simd::is_alignment_kernel_table_complete(incomplete_table));
    incomplete_table = complete_table;
    incomplete_table.prepare_block_global_gap_certificate = nullptr;
    REQUIRE_FALSE(bioseq::simd::is_alignment_kernel_table_complete(incomplete_table));
    incomplete_table = complete_table;
    incomplete_table.try_block_certified_gap = nullptr;
    REQUIRE_FALSE(bioseq::simd::is_alignment_kernel_table_complete(incomplete_table));
    incomplete_table = complete_table;
    incomplete_table.align_block = nullptr;
    REQUIRE_FALSE(bioseq::simd::is_alignment_kernel_table_complete(incomplete_table));

    std::vector<bioseq::simd::alignment_table_entry> entries{
        generated_root.alignment_by_alphabet.begin(), generated_root.alignment_by_alphabet.end()};
    simd_dispatch::highway_kernel_table synthetic_root = generated_root;
    synthetic_root.alignment_by_alphabet =
        std::span<const bioseq::simd::alignment_table_entry>{entries};
    REQUIRE(synthetic_root.is_complete());

    entries.front().typed_table_complete = false;
    REQUIRE_FALSE(synthetic_root.is_complete());
    entries.front().typed_table_complete = true;
    entries.front().typed_table = nullptr;
    REQUIRE_FALSE(synthetic_root.is_complete());
}

TEST_CASE("Highway dispatch inventory preserves supported and runnable masks",
          "[component:bioseq][layer:unit][feature:simd_primitives]"
          "[resource:highway]") {
    const auto generated_and_supported = hwy::SupportedAndGeneratedTargets();
    REQUIRE_FALSE(generated_and_supported.empty());

    std::int64_t compiled_targets = 0;
    for (const std::int64_t target : generated_and_supported) {
        compiled_targets |= target;
    }
    const simd_dispatch::highway_dispatch_plan info =
        simd_dispatch::make_highway_dispatch_plan(compiled_targets);

    REQUIRE(info.compiled_targets == compiled_targets);
    REQUIRE(info.cpu_supported_targets == hwy::SupportedTargets());
    REQUIRE(info.runnable_targets == (info.compiled_targets & info.cpu_supported_targets));
    REQUIRE(info.runnable_targets == compiled_targets);
    REQUIRE(info.planned_target == generated_and_supported.front());

    const auto planned_names = simd_dispatch::highway_target_names(info.planned_target);
    REQUIRE(planned_names.size() == 1u);
    REQUIRE(planned_names.front() == hwy::TargetName(info.planned_target));

    const auto runnable_names = simd_dispatch::highway_target_names(info.runnable_targets);
    REQUIRE(runnable_names.size() == generated_and_supported.size());
    for (std::size_t index = 0; index < runnable_names.size(); ++index) {
        CAPTURE(index, runnable_names[index]);
        REQUIRE(runnable_names[index] == hwy::TargetName(generated_and_supported[index]));
    }

    const auto supported_names = simd_dispatch::highway_target_names(info.cpu_supported_targets);
    REQUIRE(supported_names.size() == static_cast<std::size_t>(std::popcount(
                                          static_cast<std::uint64_t>(info.cpu_supported_targets))));
    for (const std::string_view name : supported_names) {
        CAPTURE(name);
        REQUIRE_FALSE(name.empty());
        REQUIRE(name != "Unknown");
    }

    const simd_dispatch::highway_dispatch_plan empty = simd_dispatch::make_highway_dispatch_plan(0);
    REQUIRE(empty.compiled_targets == 0);
    REQUIRE(empty.runnable_targets == 0);
    REQUIRE(empty.planned_target == 0);
    REQUIRE(simd_dispatch::highway_target_names(0).empty());
}

TEST_CASE("Highway dispatch inventory obeys the synthetic mask matrix",
          "[component:bioseq][layer:unit][feature:simd_primitives]"
          "[resource:highway]") {
    constexpr std::uint64_t positive_mask =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    const std::int64_t supported_targets = hwy::SupportedTargets();
    const std::uint64_t supported_bits =
        static_cast<std::uint64_t>(supported_targets) & positive_mask;
    REQUIRE(supported_bits != 0u);

    const std::uint64_t supported_bit = supported_bits & (~supported_bits + std::uint64_t{1});
    const std::uint64_t unsupported_bits = (~supported_bits) & positive_mask;
    REQUIRE(unsupported_bits != 0u);
    const std::uint64_t unsupported_bit = unsupported_bits & (~unsupported_bits + std::uint64_t{1});

    /// Expected dispatch fields for one synthetic compiled-target mask.
    struct mask_case {
        std::string_view name;
        std::uint64_t compiled;
        std::uint64_t runnable;
        std::uint64_t planned;
    };
    const std::array cases{
        mask_case{"empty compiled inventory", 0u, 0u, 0u},
        mask_case{"one compiled and supported target", supported_bit, supported_bit, supported_bit},
        mask_case{"compiled target unsupported by this CPU", unsupported_bit, 0u, 0u},
        mask_case{"unsupported compiled bits are excluded",
                  supported_bit | unsupported_bit,
                  supported_bit,
                  supported_bit},
        mask_case{"all positive compiled bits intersect exactly",
                  positive_mask,
                  supported_bits,
                  supported_bit},
    };

    for (const auto& test_case : cases) {
        CAPTURE(test_case.name, test_case.compiled, test_case.runnable, test_case.planned);
        const auto info = simd_dispatch::make_highway_dispatch_plan(
            static_cast<std::int64_t>(test_case.compiled));
        REQUIRE(info.compiled_targets == static_cast<std::int64_t>(test_case.compiled));
        REQUIRE(info.cpu_supported_targets == supported_targets);
        REQUIRE(info.runnable_targets == static_cast<std::int64_t>(test_case.runnable));
        REQUIRE(info.runnable_targets == (info.compiled_targets & info.cpu_supported_targets));
        REQUIRE(info.planned_target == static_cast<std::int64_t>(test_case.planned));
    }

    const std::uint64_t remaining_supported = supported_bits & (supported_bits - std::uint64_t{1});
    if (remaining_supported != 0u) {
        const std::uint64_t second_supported =
            remaining_supported & (~remaining_supported + std::uint64_t{1});
        const auto two_runnable = simd_dispatch::make_highway_dispatch_plan(
            static_cast<std::int64_t>(supported_bit | second_supported));
        REQUIRE(two_runnable.runnable_targets ==
                static_cast<std::int64_t>(supported_bit | second_supported));
        REQUIRE(two_runnable.planned_target == static_cast<std::int64_t>(supported_bit));
    }
}
