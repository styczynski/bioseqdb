/**
 * @file vulkan_minimizer_index_test.cpp
 * @brief Vulkan minimizer target-index admission tests.
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

TEST_CASE("vulkan minimizer: target-index count admission covers persistent arena high-water",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& runtime = vk::runtime::instance();
    constexpr std::uint64_t high_water_request = std::uint64_t{16} << 20;
    REQUIRE(runtime.reusable_gpu_arena().ensure_capacity(high_water_request, 0u, 0u));
    const std::uint64_t high_water = runtime.reusable_gpu_arena().device_buffer().allocation_bytes;
    REQUIRE(high_water >= high_water_request);
    REQUIRE(runtime.gpu_arena_allocation_requirement(1024u) >= high_water);

    auto mutable_gate_words = std::make_shared<std::vector<std::uint32_t>>(
        vk::k_gpu_query_gate_word_count, (std::numeric_limits<std::uint32_t>::max)());
    const std::shared_ptr<const std::vector<std::uint32_t>> gate_words =
        std::move(mutable_gate_words);
    const auto gate_identity = vk::canonical_vulkan_sketch_query_gate_identity(
        std::span<const std::uint32_t>{*gate_words}, vk::k_gpu_query_gate_mask, gate_words);
    const vk::vulkan_sketch_query_gate gate{std::span<const std::uint32_t>{*gate_words},
                                            vk::k_gpu_query_gate_mask,
                                            gate_identity,
                                            true};
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built =
        vk::build_gpu_index_on_device<minimap_sketch_data>(sequence_view, 10u, 15u, gate, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->ownership.count_submit_admissions == 1u);
    REQUIRE(built->ownership.execution_peak_admissions == 1u);
    REQUIRE(built->ownership.idle_arena_trim_attempts_before_count == 1u);
    REQUIRE(built->ownership.idle_arena_trim_successes_before_count == 1u);
    REQUIRE(built->ownership.idle_arena_released_bytes_before_count >= high_water);
    REQUIRE(built->ownership.pretrim_peak_observed_bytes >= high_water);
    REQUIRE(built->ownership.peak_observed_bytes >= built->ownership.pretrim_peak_observed_bytes);
    REQUIRE(built->ownership.direct_sequence_arena_copies == 1u);
    REQUIRE(built->ownership.host_sequence_capacity_releases_before_execution == 0u);
    REQUIRE(built->ownership.direct_tile_arena_fills == 1u);
    REQUIRE(built->ownership.count_submit_host_transient_bytes == 0u);
    REQUIRE(built->ownership.execution_submit_host_transient_bytes == 0u);
    REQUIRE(built->ownership.host_transient_released_before_execution_bytes ==
            built->ownership.host_tile_staging_elided_bytes);
    REQUIRE(built->ownership.exact_count_dispatches == 1u);
    REQUIRE(built->ownership.exact_count_prefix_scan_dispatches == 3u);
    REQUIRE(built->ownership.exact_count_control_readbacks == 1u);
    REQUIRE(built->ownership.deterministic_tile_emit_dispatches == 1u);
    REQUIRE(built->ownership.atomic_append_dispatches == 0u);
    REQUIRE(built->ownership.emit_validation_control_readbacks == 1u);
    REQUIRE(built->ownership.count_emit_parity_verified == 1u);
    REQUIRE(built->ownership.arena_trimmed_before_permit_shrink == 1u);
    REQUIRE(built->ownership.gate_count_dispatches == 1u);
    REQUIRE(built->ownership.gated_emit_dispatches == 1u);
    REQUIRE(built->ownership.ungated_emit_dispatches == 0u);
    REQUIRE(built->ownership.retained_minimizer_count > 0u);
    REQUIRE(built->ownership.distinct_minimizer_count > 0u);
    REQUIRE(built->ownership.peak_observed_bytes <= built->ownership.peak_admitted_bytes);
}

TEST_CASE("vulkan minimizer: exact ungated count and seed arena trim preserve adopted index",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    auto& runtime = vk::runtime::instance();
    auto& arena = runtime.reusable_gpu_arena();

    // A live slice makes trim fail closed.  Once its mark is rewound, trim
    // destroys the backing allocation.  This directly guards the cursor rule
    // used by the request-boundary cleanup below.
    REQUIRE(arena.release_idle_capacity());
    constexpr std::uint64_t arena_probe_bytes = std::uint64_t{1} << 20u;
    REQUIRE(arena.ensure_capacity(arena_probe_bytes, 0u, 0u));
    const auto arena_mark = arena.mark();
    const auto arena_probe = arena.allocate_device(4096u, 16u);
    REQUIRE(arena_probe.is_valid());
    const auto arena_handle = arena.device_buffer().buffer_handle;
    REQUIRE_FALSE(arena.release_idle_capacity());
    REQUIRE(arena.device_buffer().is_valid());
    REQUIRE(arena.device_buffer().buffer_handle == arena_handle);
    arena.rewind(arena_mark);
    REQUIRE(arena.release_idle_capacity());
    REQUIRE_FALSE(arena.device_buffer().is_valid());

    // The same cleanup must run during stack unwinding, before a caller can
    // release or shrink its admission permit.
    bool exception_observed = false;
    try {
        vk::gpu_arena_trim_guard trim_on_exception{arena};
        REQUIRE(arena.ensure_capacity(arena_probe_bytes, 0u, 0u));
        const auto exception_probe = arena.allocate_device(4096u, 16u);
        REQUIRE(exception_probe.is_valid());
        throw std::runtime_error("intentional arena cleanup probe");
    } catch (const std::runtime_error&) {
        exception_observed = true;
    }
    REQUIRE(exception_observed);
    REQUIRE_FALSE(arena.device_buffer().is_valid());
    REQUIRE_FALSE(arena.upload_buffer().is_valid());
    REQUIRE_FALSE(arena.download_buffer().is_valid());

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t cache_metadata_before = runtime.pipeline_cache_metadata_reserved_bytes();
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    const vk::sequence_upload_view upload = vk::make_sequence_upload_view(sequence);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    const vk::vulkan_sketch_params target_index_params{
        .bp_size = static_cast<std::uint32_t>(upload.bp_size),
        .window_size = 10u,
        .kmer_size = 15u,
        .hash_bits = vk::minimizer_wire_traits<minimap_sketch_data>::k_hash_bits,
        .hash_shift = vk::minimizer_wire_traits<minimap_sketch_data>::k_hash_shift,
        .pos_bits = vk::minimizer_wire_traits<minimap_sketch_data>::k_position_bits,
        .minimizer_word_kind =
            static_cast<std::uint32_t>(vk::minimizer_wire_traits<minimap_sketch_data>::k_word_kind),
        .minimizer_words32 =
            vk::minimizer_wire_traits<minimap_sketch_data>::k_words32_per_minimizer,
        .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
        .unit_bits = upload.unit_bits,
        .bits_per_char = upload.bits_per_char,
        .letters_per_unit = upload.letters_per_unit,
        .alphabet_size = upload.alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = upload.char_mask,
        .padding_letters = upload.padding_letters,
    };
    runtime.prepare_target_index_pipelines(target_index_params);
    const std::uint64_t target_index_pipeline_metadata =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(target_index_pipeline_metadata >= cache_metadata_before);
    runtime.prepare_target_index_pipelines(target_index_params);
    REQUIRE(runtime.pipeline_cache_metadata_reserved_bytes() == target_index_pipeline_metadata);
    const std::uint64_t pessimistic_capacity =
        vk::estimate_vulkan_minimizer_capacity(sequence.bp_size(), 10u);
    vk::vulkan_sketch_decline decline{};
    auto built =
        vk::build_gpu_index_on_device<minimap_sketch_data>(sequence_view, 10u, 15u, {}, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    // The build's count/radix/mark/emit route must not discover any pipeline
    // metadata after the caller has acquired its transient allocation permit.
    REQUIRE(runtime.pipeline_cache_metadata_reserved_bytes() == target_index_pipeline_metadata);
    REQUIRE(built->ownership.count_submit_admissions == 1u);
    REQUIRE(built->ownership.execution_peak_admissions == 1u);
    REQUIRE(built->ownership.exact_count_dispatches == 1u);
    REQUIRE(built->ownership.exact_count_prefix_scan_dispatches == 3u);
    REQUIRE(built->ownership.exact_count_control_readbacks == 1u);
    REQUIRE(built->ownership.deterministic_tile_emit_dispatches == 1u);
    REQUIRE(built->ownership.atomic_append_dispatches == 0u);
    REQUIRE(built->ownership.emit_validation_control_readbacks == 1u);
    REQUIRE(built->ownership.count_emit_parity_verified == 1u);
    REQUIRE(built->ownership.arena_trimmed_before_permit_shrink == 1u);
    REQUIRE_FALSE(arena.device_buffer().is_valid());
    REQUIRE_FALSE(arena.upload_buffer().is_valid());
    REQUIRE_FALSE(arena.download_buffer().is_valid());
    REQUIRE(built->ownership.gate_count_dispatches == 0u);
    REQUIRE(built->ownership.gated_emit_dispatches == 0u);
    REQUIRE(built->ownership.ungated_emit_dispatches == 1u);
    REQUIRE(built->ownership.retained_minimizer_count > 0u);
    REQUIRE(built->ownership.retained_minimizer_count < pessimistic_capacity);
    REQUIRE(built->position_count == built->ownership.retained_minimizer_count);
    REQUIRE(built->position_count == built->ownership.counted_minimizer_count);
    REQUIRE(built->position_count == built->ownership.verified_emit_count);
    REQUIRE(built->ownership.peak_observed_bytes <= built->ownership.peak_admitted_bytes);

    const auto slots_handle = built->slots.buffer_handle;
    const auto positions_handle = built->positions.buffer_handle;
    const std::uint64_t slots_allocation = built->slots.allocation_bytes;
    const std::uint64_t positions_allocation = built->positions.allocation_bytes;
    const std::uint64_t resident_bytes = slots_allocation + positions_allocation;
    REQUIRE(built->ownership.resident_bytes == resident_bytes);
    const bool materialization_permit_released = built->materialization.is_permit_released;
    if (materialization_permit_released) {
        REQUIRE_FALSE(static_cast<bool>(built->permit));
    } else {
        REQUIRE(built->permit.bytes() == resident_bytes);
    }

    idx::gpu_minimizer_index_cache_key key{
        .source_id = 0u,
        .caller_identity = 0x0371u,
        .target_len = built->target_len,
        .table_size = built->occupied_slots,
        .position_count = built->position_count,
    };
    auto adopted = idx::adopt_gpu_built_index(runtime, std::move(*built), key);
    REQUIRE(adopted.has_value());
    auto resident = std::move(adopted).value();
    REQUIRE(resident != nullptr);
    REQUIRE(resident->ownership.resident_transfers == 1u);
    REQUIRE(resident->ownership.adoption_rereserve_attempts == 0u);
    REQUIRE(resident->ownership.adoption_successes == 1u);
    REQUIRE(resident->slots.buffer_handle == slots_handle);
    REQUIRE(resident->positions.buffer_handle == positions_handle);
    REQUIRE(resident->slots.allocation_bytes == slots_allocation);
    REQUIRE(resident->positions.allocation_bytes == positions_allocation);
    const std::uint64_t adopted_reservation = admission.reserved_bytes();
    const std::uint64_t cache_metadata_at_adoption =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(cache_metadata_at_adoption >= cache_metadata_before);

    // Seed twice through the same resident buffers.  Each completed fence copies
    // anchors to owned host storage and then trims only the arena; the adopted
    // standalone slot/position allocations must retain identity and ownership.
    const auto query = cpu_sketch<minimap_sketch_data>(dna4_fixture, 10u, 15u);
    REQUIRE_FALSE(query.empty());
    const vk::gpu_seed_inputs inputs{
        .host_minimizer_bytes =
            std::as_bytes(std::span<const minimap_sketch_data>{query.data(), query.size()}),
        .device_minimizer_words = nullptr,
        .slots = &resident->slots,
        .positions = &resident->positions,
        .target_len = resident->target_len,
        .slot_mask = resident->slot_mask,
        .position_count = resident->position_count,
        .minimizer_count = static_cast<std::uint32_t>(query.size()),
        .minimizer_word_kind =
            static_cast<std::uint32_t>(vk::minimizer_wire_traits<minimap_sketch_data>::k_word_kind),
        .minimizer_words32_per_record =
            vk::minimizer_wire_traits<minimap_sketch_data>::k_words32_per_minimizer,
        .hash_shift = vk::minimizer_wire_traits<minimap_sketch_data>::k_hash_shift,
        .pos_bits = vk::minimizer_wire_traits<minimap_sketch_data>::k_position_bits,
        .bucket_count = resident->bucket_count,
        .target_position_offset = resident->target_position_offset,
    };
    const auto first = runtime.run_seed_anchor_roundtrip(inputs, sequence.bp_size(), 15u, 500);
    REQUIRE(first.count > 0u);
    REQUIRE(first.host_bytes.size() == first.count * 24u);
    REQUIRE_FALSE(arena.device_buffer().is_valid());
    REQUIRE_FALSE(arena.upload_buffer().is_valid());
    REQUIRE_FALSE(arena.download_buffer().is_valid());
    REQUIRE(resident->slots.buffer_handle == slots_handle);
    REQUIRE(resident->positions.buffer_handle == positions_handle);
    const std::uint64_t steady_cache_metadata = runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(steady_cache_metadata >= cache_metadata_at_adoption);
    const std::uint64_t steady_adopted_reservation = bioseq::common::saturating_add(
        adopted_reservation, steady_cache_metadata - cache_metadata_at_adoption);
    REQUIRE(admission.reserved_bytes() == steady_adopted_reservation);

    const auto second = runtime.run_seed_anchor_roundtrip(inputs, sequence.bp_size(), 15u, 500);
    REQUIRE(second.count == first.count);
    REQUIRE(second.host_bytes == first.host_bytes);
    REQUIRE_FALSE(arena.device_buffer().is_valid());
    REQUIRE(resident->slots.buffer_handle == slots_handle);
    REQUIRE(resident->positions.buffer_handle == positions_handle);
    REQUIRE(resident->slots.allocation_bytes == slots_allocation);
    REQUIRE(resident->positions.allocation_bytes == positions_allocation);
    REQUIRE(runtime.pipeline_cache_metadata_reserved_bytes() == steady_cache_metadata);
    REQUIRE(admission.reserved_bytes() == steady_adopted_reservation);

    resident.reset();
    REQUIRE(
        admission.reserved_bytes() ==
        bioseq::common::saturating_add(baseline, steady_cache_metadata - cache_metadata_before));
}

TEST_CASE("vulkan minimizer: ungated all-N target publishes a validated empty index",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    std::vector<seq::dna5> all_n = "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN"_dna5;
    auto sequence = compact_sequence_range<seq_type>(all_n);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built =
        vk::build_gpu_index_on_device<minimap_sketch_data>(sequence_view, 10u, 15u, {}, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->position_count == 0u);
    REQUIRE(built->occupied_slots == 0u);
    REQUIRE(built->ownership.exact_count_dispatches == 1u);
    REQUIRE(built->ownership.exact_count_prefix_scan_dispatches == 3u);
    REQUIRE(built->ownership.exact_count_control_readbacks == 1u);
    REQUIRE(built->ownership.deterministic_tile_emit_dispatches == 0u);
    REQUIRE(built->ownership.atomic_append_dispatches == 0u);
    REQUIRE(built->ownership.emit_validation_control_readbacks == 0u);
    REQUIRE(built->ownership.counted_minimizer_count == 0u);
    REQUIRE(built->ownership.verified_emit_count == 0u);
    REQUIRE(built->ownership.count_emit_parity_verified == 1u);
    REQUIRE(built->ownership.gate_count_dispatches == 0u);
    REQUIRE(built->ownership.gated_emit_dispatches == 0u);
    REQUIRE(built->ownership.ungated_emit_dispatches == 0u);
    REQUIRE(built->ownership.arena_trimmed_before_permit_shrink == 1u);

    idx::gpu_minimizer_index_cache_key key{
        .target_len = built->target_len,
        .table_size = built->occupied_slots,
        .position_count = built->position_count,
    };
    auto adopted = idx::adopt_gpu_built_index(vk::runtime::instance(), std::move(*built), key);
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == 0u);
    REQUIRE((*adopted)->bucket_count == 0u);
}

TEST_CASE("vulkan minimizer: gated empty target retains count-dominant pretrim peak stats",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    auto& runtime = vk::runtime::instance();
    auto& arena = runtime.reusable_gpu_arena();
    REQUIRE(arena.release_idle_capacity());
    constexpr std::uint64_t high_water_request = std::uint64_t{64} << 20u;
    REQUIRE(arena.ensure_capacity(high_water_request, 0u, 0u));
    const std::uint64_t high_water = arena.device_buffer().allocation_bytes;

    auto mutable_gate_words =
        std::make_shared<std::vector<std::uint32_t>>(vk::k_gpu_query_gate_word_count, 0u);
    const std::shared_ptr<const std::vector<std::uint32_t>> gate_words =
        std::move(mutable_gate_words);
    const auto gate_identity = vk::canonical_vulkan_sketch_query_gate_identity(
        std::span<const std::uint32_t>{*gate_words}, vk::k_gpu_query_gate_mask, gate_words);
    const vk::vulkan_sketch_query_gate gate{std::span<const std::uint32_t>{*gate_words},
                                            vk::k_gpu_query_gate_mask,
                                            gate_identity,
                                            true};

    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    std::vector<seq::dna5> all_n(256u);
    for (auto& letter : all_n)
        letter.assign_rank(4u);
    compact_sequence_range<seq_type> sequence{all_n};
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    auto built = vk::build_gpu_index_on_device<minimap_sketch_data>(sequence_view, 10u, 15u, gate);
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->position_count == 0u);
    REQUIRE(built->ownership.gate_count_dispatches == 1u);
    REQUIRE(built->ownership.pretrim_peak_observed_bytes >= high_water);
    REQUIRE(built->ownership.peak_observed_bytes == built->ownership.pretrim_peak_observed_bytes);
    REQUIRE(built->ownership.peak_observed_bytes <= built->ownership.peak_admitted_bytes);
    REQUIRE(built->ownership.execution_submit_host_transient_bytes == 0u);
    REQUIRE(built->ownership.arena_trimmed_before_permit_shrink == 1u);
}

TEST_CASE("vulkan minimizer: exact target count may exceed planning estimate",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    constexpr std::size_t dense_bp = 8'192u;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;
    std::vector<seq::dna4> repeated_a(dense_bp);
    for (auto& base : repeated_a) {
        base.assign_rank(0u);
    }
    const std::uint64_t estimate = vk::estimate_vulkan_minimizer_capacity(dense_bp, window_size);
    const auto scalar = cpu_sketch<minimap_sketch_data>(repeated_a, window_size, kmer_size);
    REQUIRE(scalar.size() > estimate);

    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(repeated_a);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        sequence_view, window_size, kmer_size, {}, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->position_count > estimate);
    REQUIRE(built->position_count == scalar.size());
    REQUIRE(built->ownership.target_minimizer_capacity_estimate == estimate);
    REQUIRE(built->ownership.capacity_estimate_underestimated == 1u);
    REQUIRE(built->ownership.counted_minimizer_count == built->position_count);
    REQUIRE(built->ownership.verified_emit_count == built->position_count);
    REQUIRE(built->ownership.count_emit_parity_verified == 1u);
    REQUIRE(built->ownership.atomic_append_dispatches == 0u);

    idx::gpu_minimizer_index_cache_key key{
        .target_len = built->target_len,
        .table_size = built->occupied_slots,
        .position_count = built->position_count,
    };
    auto adopted = idx::adopt_gpu_built_index(vk::runtime::instance(), std::move(*built), key);
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == scalar.size());
    REQUIRE((*adopted)->ownership.capacity_estimate_underestimated == 1u);
}

TEST_CASE(
    "vulkan minimizer: fragmented facade admission preserves exact index without scanner scratch",
    "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    using configuration_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<configuration_type>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_type = bioseq::storage::chunked_span_sequence_scanner<
        storage_unit,
        bioseq::storage::default_fragment_storage_units>;
    using lazy_type = bioseq::storage::lazy_sequence<configuration_type, scanner_type>;

    range_type contiguous{dna4_fixture};
    auto contiguous_owner = bioseq::storage::make_resident_lazy_sequence(contiguous);
    const auto contiguous_view = bioseq::storage::make_lazy_sequence_view(contiguous_owner);
    vk::vulkan_sketch_decline contiguous_decline{};
    auto contiguous_built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        contiguous_view, 10u, 15u, {}, &contiguous_decline);
    INFO(vk::format_vulkan_sketch_decline(contiguous_decline));
    REQUIRE(contiguous_built.has_value());
    REQUIRE(contiguous_built->valid);
    const std::size_t slot_bytes = static_cast<std::size_t>(contiguous_built->occupied_slots) * 16u;
    const std::size_t position_bytes =
        static_cast<std::size_t>(contiguous_built->position_count) * 8u;
    std::vector<std::byte> expected_slots(slot_bytes);
    std::vector<std::byte> expected_positions(position_bytes);
    std::memcpy(expected_slots.data(), contiguous_built->slots.mapped, slot_bytes);
    std::memcpy(expected_positions.data(), contiguous_built->positions.mapped, position_bytes);
    const std::uint32_t expected_slot_count = contiguous_built->occupied_slots;
    const std::uint32_t expected_position_count = contiguous_built->position_count;
    contiguous_built.reset();

    lazy_type fragmented{scanner_type::from_range(contiguous)};
    const auto fragmented_view = bioseq::storage::make_lazy_sequence_view(fragmented);
    vk::vulkan_sketch_decline fragmented_decline{};
    auto fragmented_built = vk::build_gpu_index_on_device<minimap_sketch_data>(
        fragmented_view, 10u, 15u, {}, &fragmented_decline);
    INFO(vk::format_vulkan_sketch_decline(fragmented_decline));
    REQUIRE(fragmented_built.has_value());
    REQUIRE(fragmented_built->valid);
    REQUIRE(fragmented_built->occupied_slots == expected_slot_count);
    REQUIRE(fragmented_built->position_count == expected_position_count);
    REQUIRE(std::memcmp(
                fragmented_built->slots.mapped, expected_slots.data(), expected_slots.size()) == 0);
    REQUIRE(std::memcmp(fragmented_built->positions.mapped,
                        expected_positions.data(),
                        expected_positions.size()) == 0);

    const auto& stats = fragmented_built->ownership;
    REQUIRE_FALSE(
        fragmented_view.try_resident_packed(0u, fragmented_view.bp_size(), sizeof(std::uint32_t)));
    REQUIRE(stats.direct_sequence_arena_copies == 1u);
    REQUIRE(stats.host_sequence_capacity_releases_before_execution == 0u);
    REQUIRE(stats.host_sequence_capacity_released_bytes == 0u);
    REQUIRE(stats.direct_tile_arena_fills == 1u);
    REQUIRE(stats.count_submit_host_transient_bytes == 0u);
    REQUIRE(stats.execution_submit_host_transient_bytes == 0u);
    REQUIRE(stats.host_transient_released_before_execution_bytes ==
            stats.host_tile_staging_elided_bytes);
    REQUIRE(stats.arena_trimmed_before_permit_shrink == 1u);
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
