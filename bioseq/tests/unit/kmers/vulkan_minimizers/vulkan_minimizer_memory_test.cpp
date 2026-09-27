/**
 * @file vulkan_minimizer_memory_test.cpp
 * @brief Check memory budgets as minimizer  data moves between GPU results and host indexes.
 */
#include <hwy/highway.h>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.unit.kmers.vulkan_minimizers_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

namespace vk = ::bioseq::gpu::vulkan;
namespace idx = ::bioseq::indexing;

using namespace vulkan_minimizers_test_support;

#if defined(BIOSEQ_TEST_INDEXES)
TEST_CASE("MPS authorized readback admits and resizes the exact typed destination before transfer",
          "[component:bioseq][layer:unit][feature:minimizer][feature:gpu_runtime]") {
    const auto audit = vk::self_test_authorized_typed_readback_contract();
    CHECK(audit.checked_multiplication_overflow_rejected);
    CHECK(audit.maximum_u32_count_accepted);
    CHECK(audit.wider_host_count_preserved);
    CHECK(audit.admission_denied_before_destination_or_transfer);
    CHECK(audit.pre_resize_span_rejected);
    CHECK(audit.actual_capacity_reconciled);
    CHECK(audit.exact_resized_destination_seen);
    CHECK(audit.backing_padding_excluded);
    CHECK(audit.successful_destination_extent_unchanged);
    CHECK(audit.mutated_destination_extent_rejected);
    CHECK(audit.negative_branch_storage_before_permit_cleanup);
    CHECK(vk::runtime::self_test_authorized_readback_receipt_contract());
}
#endif

TEST_CASE("Vulkan owned segment pairs transfer capacity with their permit",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0;
        ~restore_cap_guard() {
            admission.set_index_memory_cap_bytes(previous);
        }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(
        std::max<std::uint64_t>(previous_cap, baseline + (std::uint64_t{1} << 20)));

    constexpr std::size_t count = 32u;
    using pair_type = vk::vulkan_owned_segment_pairs::value_type;
    const std::uint64_t headroom =
        count * sizeof(pair_type) * bioseq::parallelism::k_vector_preallocation_headroom_factor;
    auto permit = admission.try_reserve(headroom);
    REQUIRE(permit.has_value());
    vk::vulkan_owned_segment_pairs source;
    source.permit = std::move(*permit);
    source.pairs.reserve(count);
    const std::uint64_t exact = source.pairs.capacity() * sizeof(pair_type);
    REQUIRE(exact <= source.permit.bytes());
    source.permit.shrink_to(exact);
    source.pairs.emplace_back(7u, 11u);

    vk::vulkan_owned_segment_pairs destination{std::move(source)};
    REQUIRE(source.pairs.capacity() == 0u);
    REQUIRE_FALSE(static_cast<bool>(source.permit));
    REQUIRE((destination.pairs.front() == pair_type{7u, 11u}));
    REQUIRE(destination.permit.bytes() == destination.pairs.capacity() * sizeof(pair_type));
    destination.release_capacity_before_permit();
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("vulkan minimizer: target-index host scratch ends before exact main admission",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    constexpr std::uint64_t bp_size = 500'000'000u;
    constexpr std::uint64_t packed_sequence_bytes = 125'000'000u;
    const vk::gpu_sketch_memory_plan plan =
        vk::make_gpu_sketch_memory_plan(bp_size,
                                        packed_sequence_bytes,
                                        sizeof(vk::vulkan_rank_lut),
                                        93'308'896u,
                                        0u,
                                        vk::gpu_sketch_purpose::target_index_build,
                                        15u,
                                        10u,
                                        32u);
    REQUIRE(plan.is_valid);
    REQUIRE(plan.host_sequence_staging_bytes == 125'000'000u);
    REQUIRE(plan.host_tile_staging_bytes == 46'875'000u);
    REQUIRE(plan.execution_submit_host_transient_bytes == 0u);
    REQUIRE(plan.host_transient_released_before_execution_bytes == 171'875'000u);

    const auto fragmented =
        vk::make_gpu_target_index_host_scratch_plan(plan, packed_sequence_bytes);
    REQUIRE(fragmented.is_valid);
    REQUIRE(fragmented.count_submit_bytes == 125'000'000u);
    REQUIRE(fragmented.execution_submit_bytes == 0u);
    REQUIRE(fragmented.released_before_execution_bytes == 171'875'000u);

    const auto contiguous = vk::make_gpu_target_index_host_scratch_plan(plan, 0u);
    REQUIRE(contiguous.is_valid);
    REQUIRE(contiguous.count_submit_bytes == 0u);
    REQUIRE(contiguous.execution_submit_bytes == 0u);
    REQUIRE(contiguous.released_before_execution_bytes == 46'875'000u);

    const vk::gpu_sketch_memory_plan small_words =
        vk::make_gpu_sketch_memory_plan(64u,
                                        16u,
                                        sizeof(vk::vulkan_rank_lut),
                                        64u,
                                        512u,
                                        vk::gpu_sketch_purpose::minimizer_words,
                                        15u,
                                        10u,
                                        32u);
    REQUIRE(small_words.is_valid);
    REQUIRE(small_words.count_submit_host_transient_bytes ==
            small_words.execution_submit_host_transient_bytes);
    REQUIRE(small_words.host_transient_released_before_execution_bytes == 0u);
}

TEST_CASE("vulkan minimizer: 500-Mbp w10 dispatch excludes pessimistic host result",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    constexpr std::uint64_t bp_size = 500'000'000u;
    constexpr std::uint64_t window_size = 10u;
    constexpr std::uint32_t kmer_size = 15u;
    constexpr std::uint32_t letters_per_unit = 32u;
    constexpr std::uint64_t packed_sequence_bytes = bp_size / 4u;

    const std::uint64_t max_minimizers =
        vk::estimate_vulkan_minimizer_capacity(bp_size, window_size);
    REQUIRE(max_minimizers == 150'000'192u);

    const std::uint64_t pessimistic_initial_host_bytes = bioseq::common::saturating_mul(
        bioseq::common::saturating_mul(max_minimizers,
                                       static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
    REQUIRE(pessimistic_initial_host_bytes == 2'400'003'072u);

    const std::uint64_t resident_requested_bytes =
        vk::vulkan_sketch_resident_bytes<minimap_sketch_data>(max_minimizers);
    REQUIRE(resident_requested_bytes == 1'200'001'536u);
    const vk::gpu_sketch_memory_plan plan =
        vk::make_gpu_sketch_memory_plan(bp_size,
                                        packed_sequence_bytes,
                                        sizeof(vk::vulkan_rank_lut),
                                        max_minimizers,
                                        resident_requested_bytes,
                                        vk::gpu_sketch_purpose::minimizer_words,
                                        kmer_size,
                                        static_cast<std::uint32_t>(window_size),
                                        letters_per_unit);
    REQUIRE(plan.is_valid);

    const std::uint64_t growable_vector_headroom =
        bioseq::common::saturating_mul(plan.count_submit_host_transient_bytes,
                                       bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t host_dispatch_headroom =
        vk::vulkan_exact_host_allocation_admission_bytes(plan.count_submit_host_transient_bytes);
    REQUIRE(host_dispatch_headroom != std::numeric_limits<std::uint64_t>::max());
    REQUIRE(growable_vector_headroom != std::numeric_limits<std::uint64_t>::max());
    CHECK(host_dispatch_headroom == plan.count_submit_host_transient_bytes +
                                        vk::k_vulkan_exact_host_allocation_headroom_bytes);
    CHECK(host_dispatch_headroom < growable_vector_headroom);
    CHECK(vk::vulkan_exact_host_allocation_admission_bytes(
              std::numeric_limits<std::uint64_t>::max()) ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(vk::vulkan_exact_host_allocation_admission_bytes(
              std::numeric_limits<std::uint64_t>::max() -
              vk::k_vulkan_exact_host_allocation_headroom_bytes + 1u) ==
          std::numeric_limits<std::uint64_t>::max());
    const std::uint64_t split_dispatch_bytes = vk::vulkan_sketch_dispatch_admission_bytes(
        plan.arena_scaffold_bytes, host_dispatch_headroom, 0u, resident_requested_bytes);
    const std::uint64_t dispatch_without_host_result = bioseq::common::saturating_add(
        resident_requested_bytes,
        vk::vulkan_sketch_transient_reserved_bytes(bp_size,
                                                   packed_sequence_bytes,
                                                   letters_per_unit,
                                                   window_size,
                                                   kmer_size,
                                                   max_minimizers,
                                                   resident_requested_bytes));
    const std::uint64_t simultaneous_reservation_bytes = bioseq::common::saturating_add(
        dispatch_without_host_result, pessimistic_initial_host_bytes);

    REQUIRE(split_dispatch_bytes < simultaneous_reservation_bytes);
    REQUIRE(simultaneous_reservation_bytes - dispatch_without_host_result == 2'400'003'072u);

    // Keep the GPU allocations fixed and compare host staging for the same
    // 500-million-letter dna4 input. A fixed-size allocation needs less spare
    // capacity than a growing vector.
    constexpr std::uint64_t captured_required_bytes = 1'492'942'336u;
    constexpr std::uint64_t captured_allocatable_bytes = 1'419'423'232u;
    constexpr std::uint64_t captured_packed_sequence_bytes = 125'000'000u;
    const std::uint64_t padded_fixed_staging_bytes =
        bioseq::common::saturating_mul(captured_packed_sequence_bytes,
                                       bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t exact_fixed_staging_bytes =
        vk::vulkan_exact_host_allocation_admission_bytes(captured_packed_sequence_bytes);
    REQUIRE(captured_required_bytes >= padded_fixed_staging_bytes);
    const std::uint64_t captured_corrected_required_bytes =
        captured_required_bytes - padded_fixed_staging_bytes + exact_fixed_staging_bytes;
    REQUIRE(captured_allocatable_bytes >= captured_corrected_required_bytes);
    const std::uint64_t captured_corrected_slack_bytes =
        captured_allocatable_bytes - captured_corrected_required_bytes;

    CHECK(captured_required_bytes > captured_allocatable_bytes);
    CHECK(padded_fixed_staging_bytes == 250'000'000u);
    CHECK(exact_fixed_staging_bytes == 125'065'536u);
    CHECK(captured_corrected_required_bytes == 1'368'007'872u);
    CHECK(captured_corrected_required_bytes <= captured_allocatable_bytes);
    CHECK(captured_corrected_slack_bytes == 51'415'360u);
}

TEST_CASE("vulkan minimizer: zero-copy handoff fits captured memory pressure",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    STATIC_REQUIRE(vk::vulkan_fixed_host_result_admission_bytes(0u) == 0u);
    STATIC_REQUIRE(vk::vulkan_fixed_host_result_admission_bytes(1u) ==
                   1u + vk::k_vulkan_exact_host_allocation_headroom_bytes);
    constexpr std::uint64_t emitted_minimizers = 93'308'896u;
    constexpr std::uint64_t device_output_bytes = 1'200'001'536u;
    constexpr std::uint64_t persistent_runtime_bytes = 676'076u;
    constexpr std::uint64_t captured_budget_ceiling = 2'601'156'608u;
    constexpr std::uint64_t captured_reserved_before_host =
        device_output_bytes + persistent_runtime_bytes;
    constexpr std::uint64_t captured_allocatable_before_host =
        captured_budget_ceiling - captured_reserved_before_host;

    const std::uint64_t host_payload_bytes = emitted_minimizers * sizeof(minimap_sketch_data);
    const std::uint64_t fixed_host_admission =
        vk::vulkan_fixed_host_result_admission_bytes(host_payload_bytes);
    const std::uint64_t growable_collector_host_admission = bioseq::common::saturating_mul(
        host_payload_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    REQUIRE(host_payload_bytes == 746'471'168u);
    REQUIRE(fixed_host_admission == 746'536'704u);
    REQUIRE(growable_collector_host_admission == 1'492'942'336u);
    REQUIRE(captured_allocatable_before_host == 1'400'478'996u);
    REQUIRE(fixed_host_admission <= captured_allocatable_before_host);
    REQUIRE(growable_collector_host_admission > captured_allocatable_before_host);
    REQUIRE(captured_allocatable_before_host - fixed_host_admission == 653'942'292u);
    REQUIRE(growable_collector_host_admission - captured_allocatable_before_host == 92'463'340u);

    // Check a tighter process budget too. Fixed-size staging still fits,
    // while reserving twice the payload for vector growth exceeds the budget.
    constexpr std::uint64_t captured_tight_budget_ceiling = 2'479'079'424u;
    constexpr std::uint64_t captured_tight_allocatable_before_host =
        captured_tight_budget_ceiling - captured_reserved_before_host;
    REQUIRE(captured_tight_allocatable_before_host == 1'278'401'812u);
    REQUIRE(captured_tight_allocatable_before_host - fixed_host_admission == 531'865'108u);
    REQUIRE(growable_collector_host_admission - captured_tight_allocatable_before_host ==
            214'540'524u);

    // Reserve the hash table nce for all possible distinct minimizers.
    // Starting at half that size would need space  for both, the old table and
    // its replacement during growth.
    const auto table_plan = idx::plan_minimizer_hashmap_table_allocation(emitted_minimizers);
    REQUIRE(table_plan.valid);
    REQUIRE(table_plan.group_count == 8'388'608u);
    REQUIRE(table_plan.capacity == 125'829'119u);
    REQUIRE(table_plan.allocation_bytes == 2'147'483'648u);
    const auto half_table_plan =
        idx::plan_minimizer_hashmap_table_allocation(emitted_minimizers / 2u + 16u);
    REQUIRE(half_table_plan.valid);
    REQUIRE(half_table_plan.allocation_bytes == 1'073'741'824u);

    // Model the handoff after the downloaded host vector is counted in process
    // memory and the device result is freed. Reserve new memory for positions
    // and the hash table.
    const std::uint64_t position_headroom = vk::vulkan_exact_host_allocation_admission_bytes(
        emitted_minimizers * sizeof(std::uint64_t));
    const std::uint64_t table_headroom =
        vk::vulkan_exact_host_allocation_admission_bytes(table_plan.allocation_bytes);
    const std::uint64_t target_index_build_permit =
        bioseq::common::saturating_add(position_headroom, table_headroom);
    REQUIRE(target_index_build_permit == 2'894'085'888u);

    // Update the available budget for the host vector now in process memory,
    // the freed device result, and the released arena allocation
    constexpr std::uint64_t captured_arena_high_water_bytes = 187'533'824u;
    const std::uint64_t reconciled_build_ceiling = captured_budget_ceiling - host_payload_bytes +
                                                   device_output_bytes +
                                                   captured_arena_high_water_bytes;
    REQUIRE(reconciled_build_ceiling == 3'242'220'800u);
    REQUIRE(target_index_build_permit + persistent_runtime_bytes <= reconciled_build_ceiling);
    REQUIRE(reconciled_build_ceiling - target_index_build_permit - persistent_runtime_bytes ==
            347'458'836u);

    const std::uint64_t retained_index_bytes = host_payload_bytes + table_plan.allocation_bytes;
    REQUIRE(retained_index_bytes == 2'893'954'816u);
    REQUIRE(target_index_build_permit <
            target_index_build_permit + half_table_plan.allocation_bytes);
}

TEST_CASE("vulkan minimizer: replacement authority admits dense resident upload",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    // Keeping separate reservations for both CPU index arrays leaves too little
    // room for the GPU upload in  this captured budget.
    constexpr std::uint64_t retained_position_bytes = 746'471'168u;
    constexpr std::uint64_t retained_table_bytes = 2'147'483'648u;
    constexpr std::uint64_t retained_index_bytes = 2'893'954'816u;
    constexpr std::uint64_t runtime_metadata_bytes = 546u;
    constexpr std::uint64_t budget_ceiling = 4'658'036'736u;
    constexpr std::uint64_t resident_upload_bytes = 1'870'460'992u;
    constexpr std::uint64_t full_index_reserved_bytes = 2'893'955'362u;
    constexpr std::uint64_t full_index_allocatable_bytes = 1'764'081'374u;
    constexpr std::uint64_t full_index_shortfall_bytes = 106'379'618u;

    STATIC_REQUIRE(retained_index_bytes == retained_position_bytes + retained_table_bytes);
    STATIC_REQUIRE(full_index_reserved_bytes == retained_index_bytes + runtime_metadata_bytes);
    STATIC_REQUIRE(full_index_allocatable_bytes == budget_ceiling - full_index_reserved_bytes);
    STATIC_REQUIRE(full_index_shortfall_bytes ==
                   resident_upload_bytes - full_index_allocatable_bytes);
    STATIC_REQUIRE(resident_upload_bytes > full_index_allocatable_bytes);

    // Model replacing a freed host vector with a fully initialized positions
    // array of the same size. Its bytes are already counted in process memory,
    // so only the hash table keeps a separate reservation.
    constexpr std::uint64_t transferred_position_footprint_bytes = retained_position_bytes;
    constexpr std::uint64_t retained_index_permit_bytes =
        retained_index_bytes - transferred_position_footprint_bytes;
    constexpr std::uint64_t transferred_reserved_bytes =
        retained_index_permit_bytes + runtime_metadata_bytes;
    constexpr std::uint64_t transferred_allocatable_bytes =
        budget_ceiling - transferred_reserved_bytes;
    constexpr std::uint64_t transferred_margin_bytes =
        transferred_allocatable_bytes - resident_upload_bytes;

    STATIC_REQUIRE(retained_index_permit_bytes == 2'147'483'648u);
    STATIC_REQUIRE(transferred_reserved_bytes == 2'147'484'194u);
    STATIC_REQUIRE(transferred_allocatable_bytes == 2'510'552'542u);
    STATIC_REQUIRE(resident_upload_bytes <= transferred_allocatable_bytes);
    STATIC_REQUIRE(transferred_margin_bytes == 640'091'550u);
}

TEST_CASE("vulkan target index: 500-Mbp ungated count replaces C-sized main peak",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    constexpr std::uint64_t bp_size = 500'000'000u;
    constexpr std::uint64_t packed_sequence_bytes = 125'000'000u;
    constexpr std::uint32_t window_size = 10u;
    constexpr std::uint32_t kmer_size = 15u;
    constexpr std::uint32_t letters_per_unit = 32u;
    constexpr std::uint64_t exact_minimizer_count = 93'308'896u;
    constexpr std::uint64_t distinct_minimizer_count = 70'249'364u;
    constexpr std::uint64_t safe_memory_ceiling = 5'102'125'056u;

    const std::uint64_t semantic_hard_bound =
        vk::target_minimizer_count_upper_bound(bp_size, kmer_size);
    REQUIRE(semantic_hard_bound == 499'999'986u);
    REQUIRE(exact_minimizer_count <= semantic_hard_bound);
    REQUIRE(semantic_hard_bound <= (std::numeric_limits<std::uint32_t>::max)());
    STATIC_REQUIRE(vk::target_minimizer_count_upper_bound(14u, 15u) == 0u);
    STATIC_REQUIRE(
        vk::target_minimizer_count_upper_bound((std::numeric_limits<std::uint32_t>::max)(), 1u) ==
        (std::numeric_limits<std::uint32_t>::max)());

    const std::uint64_t pessimistic_capacity =
        vk::estimate_vulkan_minimizer_capacity(bp_size, window_size);
    REQUIRE(pessimistic_capacity == 150'000'192u);

    // Compare buffers sized from the estimated capacity with buffers sized from
    // the measured minimizer count. The estimate exceeds this memory budgel.
    const std::uint64_t capacity_stream_floor =
        bioseq::common::saturating_mul(pessimistic_capacity, 40u);
    const std::uint64_t exact_count_stream_floor =
        bioseq::common::saturating_mul(exact_minimizer_count, 40u);
    REQUIRE(capacity_stream_floor == 6'000'007'680u);
    REQUIRE(exact_count_stream_floor == 3'732'355'840u);

    const auto capacity_plan =
        vk::make_gpu_sketch_memory_plan(bp_size,
                                        packed_sequence_bytes,
                                        sizeof(vk::vulkan_rank_lut),
                                        pessimistic_capacity,
                                        0u,
                                        vk::gpu_sketch_purpose::target_index_build,
                                        kmer_size,
                                        window_size,
                                        letters_per_unit,
                                        65'535u);
    const auto exact_plan =
        vk::make_gpu_sketch_memory_plan(bp_size,
                                        packed_sequence_bytes,
                                        sizeof(vk::vulkan_rank_lut),
                                        exact_minimizer_count,
                                        0u,
                                        vk::gpu_sketch_purpose::target_index_build,
                                        kmer_size,
                                        window_size,
                                        letters_per_unit,
                                        65'535u);
    REQUIRE(capacity_plan.is_valid);
    REQUIRE(exact_plan.is_valid);
    REQUIRE(exact_plan.tiles.tile_count == 1'562'500u);
    REQUIRE(exact_plan.tiles.scan_block_count == 6'104u);
    constexpr std::uint32_t max_dispatch_x = 65'535u;
    const std::uint32_t raw_element_groups = bioseq::common::ceil_div(exact_minimizer_count, 128u);
    const std::uint32_t raw_scan_block_groups =
        bioseq::common::ceil_div(exact_minimizer_count, vk::k_prefix_scan_block_size);
    REQUIRE(raw_element_groups == 728'976u);
    REQUIRE(raw_scan_block_groups == 364'488u);
    REQUIRE(raw_element_groups > max_dispatch_x);
    REQUIRE(raw_scan_block_groups > max_dispatch_x);
    REQUIRE(vk::grid_stride_dispatch_group_count(exact_minimizer_count, 128u, max_dispatch_x) ==
            max_dispatch_x);
    REQUIRE(vk::grid_stride_dispatch_group_count(raw_scan_block_groups, 1u, max_dispatch_x) ==
            max_dispatch_x);
    REQUIRE(capacity_plan.admission_bytes > safe_memory_ceiling);
    REQUIRE(exact_plan.admission_bytes < safe_memory_ceiling);
    REQUIRE(exact_plan.standalone_device_bytes < capacity_plan.standalone_device_bytes);

    const std::uint64_t adopted_resident_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_mul(exact_minimizer_count, 8u),
        bioseq::common::saturating_mul(distinct_minimizer_count, 16u));
    REQUIRE(adopted_resident_bytes == 1'870'460'992u);
    REQUIRE(adopted_resident_bytes < exact_count_stream_floor);
}

TEST_CASE("vulkan minimizer: split admission retains exact device and host bytes",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("hardware Vulkan unavailable; explicit split-admission hardware "
                "validation not executed");
        return;
    }

    using minimizer_type = minimap_sketch_data;
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    auto& runtime = vk::runtime::instance();
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t cache_metadata_before = runtime.pipeline_cache_metadata_reserved_bytes();
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0u;
        ~restore_cap_guard() {
            admission.set_index_memory_cap_bytes(previous);
        }
    } restore{admission, previous_cap};

    constexpr std::size_t letter_count = 1'000'000u;
    std::vector<seq::dna4> test_data;
    test_data.reserve(letter_count);
    std::uint64_t lcg = 0x243f6a8885a308d3ULL;
    for (std::size_t i = 0; i < letter_count; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 value{};
        value.assign_rank(static_cast<std::uint8_t>((lcg >> 33u) & 3u));
        test_data.push_back(value);
    }

    auto sequence = compact_sequence_range<seq_type>(test_data);
    const vk::sequence_upload_view upload = vk::make_sequence_upload_view(sequence);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    const std::uint64_t max_minimizers =
        vk::estimate_vulkan_minimizer_capacity(upload.bp_size, window_size);

    const auto dispatch_admission_bytes =
        [&runtime, &upload, max_minimizers, window_size, kmer_size]() {
            const std::uint64_t resident_requested_bytes =
                vk::vulkan_sketch_resident_bytes<minimizer_type>(max_minimizers);
            const vk::gpu_sketch_memory_plan plan =
                vk::make_gpu_sketch_memory_plan(upload.bp_size,
                                                upload.padded_byte_size,
                                                sizeof(vk::vulkan_rank_lut),
                                                max_minimizers,
                                                resident_requested_bytes,
                                                vk::gpu_sketch_purpose::minimizer_words,
                                                static_cast<std::uint32_t>(kmer_size),
                                                static_cast<std::uint32_t>(window_size),
                                                upload.letters_per_unit,
                                                runtime.features().max_workgroup_count_x);
            REQUIRE(plan.is_valid);
            const auto allocation_requirements = runtime.sketch_batch_allocation_requirements(
                upload.padded_byte_size,
                plan.tiles.tile_count,
                static_cast<std::uint32_t>(vk::minimizer_wire_traits<minimizer_type>::k_word_kind),
                vk::minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
                max_minimizers);
            REQUIRE(allocation_requirements.valid);
            const std::uint64_t physical_arena_bytes =
                runtime.gpu_arena_allocation_requirement(plan.arena_scaffold_bytes);
            const std::uint64_t host_transient_headroom_bytes =
                vk::vulkan_exact_host_allocation_admission_bytes(
                    plan.count_submit_host_transient_bytes);
            return vk::vulkan_sketch_dispatch_admission_bytes(
                physical_arena_bytes,
                host_transient_headroom_bytes,
                0u,
                allocation_requirements.pessimistic_output_allocation_bytes);
        };

    const std::uint64_t initial_dispatch_peak = dispatch_admission_bytes();
    const std::uint64_t pessimistic_initial_host_bytes = bioseq::common::saturating_mul(
        bioseq::common::saturating_mul(max_minimizers,
                                       static_cast<std::uint64_t>(sizeof(minimizer_type))),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t permissive_cap = bioseq::common::saturating_add(
        baseline,
        bioseq::common::saturating_add(initial_dispatch_peak, pessimistic_initial_host_bytes));
    admission.set_index_memory_cap_bytes(permissive_cap);

    auto calibration =
        vk::collect_minimizers_vulkan_result<minimizer_type>(sequence_view, window_size, kmer_size);
    REQUIRE(calibration.has_value());
    REQUIRE(calibration->device != nullptr);
    REQUIRE(calibration->host_permit.has_value());
    REQUIRE_FALSE(calibration->host_minimizers.empty());
    REQUIRE(calibration->arena_trimmed_before_host_materialization);
    REQUIRE_FALSE(runtime.reusable_gpu_arena().device_buffer().is_valid());
    REQUIRE_FALSE(runtime.reusable_gpu_arena().upload_buffer().is_valid());
    REQUIRE_FALSE(runtime.reusable_gpu_arena().download_buffer().is_valid());

    const std::uint64_t actual_device_allocation_bytes =
        calibration->device->words.allocation_bytes;
    const std::uint64_t exact_host_size_bytes =
        static_cast<std::uint64_t>(calibration->host_minimizers.size()) * sizeof(minimizer_type);
    const std::uint64_t exact_host_headroom_bytes = bioseq::common::saturating_mul(
        exact_host_size_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    calibration.reset();
    const std::uint64_t cache_metadata_after = runtime.pipeline_cache_metadata_reserved_bytes();
    // Destroying the result must release its host and device reservations.
    // Pipeline-cache entries stay with the process runtime.
    REQUIRE(cache_metadata_after > 0u);
    REQUIRE(cache_metadata_after >= cache_metadata_before);
    const std::uint64_t cache_metadata_delta = cache_metadata_after - cache_metadata_before;
    REQUIRE(admission.reserved_bytes() ==
            bioseq::common::saturating_add(baseline, cache_metadata_delta));
    const std::uint64_t operation_baseline = admission.reserved_bytes();

    // Recompute the required memory  after calibration has populated the pipeline
    // cache. The request's arena allocations should already be freed.
    const std::uint64_t dispatch_peak = dispatch_admission_bytes();
    const std::uint64_t host_materialization_peak =
        vk::vulkan_sketch_host_materialization_admission_bytes(actual_device_allocation_bytes,
                                                               exact_host_headroom_bytes);
    const std::uint64_t split_admission_peak = std::max(dispatch_peak, host_materialization_peak);
    // Use the same dispatch-memory requirement for both lifetime models.
    // Reserving the host result during dispatch raises the peak. Copying it
    // afterward allows the temporary GPU arena to be freed first.
    const std::uint64_t simultaneous_reservation_peak =
        bioseq::common::saturating_add(dispatch_peak, pessimistic_initial_host_bytes);
    REQUIRE(split_admission_peak < simultaneous_reservation_peak);

    admission.set_index_memory_cap_bytes(
        bioseq::common::saturating_add(operation_baseline, split_admission_peak));
    bioseq::parallelism::memory_admission_failure simultaneous_failure{};
    auto simultaneous_permit =
        admission.try_reserve(simultaneous_reservation_peak, &simultaneous_failure);
    REQUIRE_FALSE(simultaneous_permit.has_value());
    REQUIRE(simultaneous_failure.reason ==
            bioseq::parallelism::memory_admission_failure_reason::budget_exhausted);

    auto result =
        vk::collect_minimizers_vulkan_result<minimizer_type>(sequence_view, window_size, kmer_size);
    REQUIRE(result.has_value());
    REQUIRE(result->dispatch_token.is_internally_consistent());
    REQUIRE(result->device != nullptr);
    REQUIRE(result->host_permit.has_value());
    REQUIRE(result->arena_trimmed_before_host_materialization);
    REQUIRE_FALSE(runtime.reusable_gpu_arena().device_buffer().is_valid());
    REQUIRE_FALSE(runtime.reusable_gpu_arena().upload_buffer().is_valid());
    REQUIRE_FALSE(runtime.reusable_gpu_arena().download_buffer().is_valid());

    const std::uint64_t exact_host_capacity_bytes =
        static_cast<std::uint64_t>(result->host_minimizers.capacity()) * sizeof(minimizer_type);
    REQUIRE(result->device->permit.bytes() == result->device->words.allocation_bytes);
    REQUIRE(result->host_permit->bytes() == exact_host_capacity_bytes);
    REQUIRE(admission.reserved_bytes() == operation_baseline +
                                              result->device->words.allocation_bytes +
                                              exact_host_capacity_bytes);

    const auto scalar = cpu_sketch<minimizer_type>(test_data, window_size, kmer_size);
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        scalar,
        result->host_minimizers,
        vk::make_sketch_tile_plan(test_data.size(),
                                  static_cast<std::uint32_t>(kmer_size),
                                  static_cast<std::uint32_t>(window_size),
                                  upload.letters_per_unit,
                                  runtime.features().max_workgroup_count_x),
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size));

    result.reset();
    REQUIRE(admission.reserved_bytes() == operation_baseline);
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
