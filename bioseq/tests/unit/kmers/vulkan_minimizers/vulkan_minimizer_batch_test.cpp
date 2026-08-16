/**
 * @file vulkan_minimizer_batch_test.cpp
 * @brief Vulkan minimizer batch materialization tests.
 */
#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.tests.unit.kmers.vulkan_minimizers_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

namespace vk = ::bioseq::gpu::vulkan;
namespace idx = ::bioseq::indexing;

using namespace vulkan_minimizers_test_support;

TEST_CASE("vulkan minimizer: dna4 batch supports exact device-only materialization",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    std::vector<seq::dna4> second{dna4_fixture.begin() + 17, dna4_fixture.end() - 11};
    require_exact_device_only_batch_parity(dna4_fixture, std::move(second));
}

TEST_CASE("vulkan minimizer: dna5 batch supports exact device-only materialization",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    require_exact_device_only_batch_parity(dna5_with_t, dna5_with_n);
}

TEST_CASE("vulkan minimizer: device-only zero emission is dispatched and sealed",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    std::vector<seq::dna5> too_short = "NNNNNNNNNNNNNN"_dna5;
    auto too_short_sequence = compact_sequence_range<seq_type>(too_short);
    const std::array<vk::sequence_upload_view, 1> too_short_uploads{
        vk::make_sequence_upload_view(too_short_sequence),
    };
    const auto rank_lut = vk::make_vulkan_rank_lut<decltype(too_short_sequence)>();
    auto nondispatchable = vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
        std::span<const vk::sequence_upload_view>{too_short_uploads.data(),
                                                  too_short_uploads.size()},
        rank_lut,
        window_size,
        kmer_size,
        vk::vulkan_sketch_host_materialization::device_only);
    REQUIRE_FALSE(nondispatchable.has_value());

    std::vector<seq::dna5> all_n = "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN"
                                   "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN"_dna5;
    REQUIRE(all_n.size() >= kmer_size);
    auto all_n_sequence = compact_sequence_range<seq_type>(all_n);
    const std::array<vk::sequence_upload_view, 1> all_n_uploads{
        vk::make_sequence_upload_view(all_n_sequence),
    };
    const auto all_n_tile_plan =
        vk::make_sketch_tile_plan(all_n.size(),
                                  static_cast<std::uint32_t>(kmer_size),
                                  static_cast<std::uint32_t>(window_size),
                                  all_n_uploads[0].letters_per_unit,
                                  vk::runtime::instance().features().max_workgroup_count_x);
    REQUIRE(all_n_tile_plan.is_dispatchable);
    const std::uint64_t pessimistic_count =
        vk::estimate_vulkan_minimizer_capacity(all_n.size(), window_size);
    const auto allocation_requirements =
        vk::runtime::instance().sketch_batch_allocation_requirements(
            all_n_uploads[0].padded_byte_size,
            all_n_tile_plan.tile_count,
            static_cast<std::uint32_t>(vk::minimizer_wire_traits<minimap_sketch_data>::k_word_kind),
            vk::minimizer_wire_traits<minimap_sketch_data>::k_words32_per_minimizer,
            pessimistic_count);
    REQUIRE(allocation_requirements.valid);
    REQUIRE(allocation_requirements.arena_requested_bytes > 0u);
    REQUIRE(allocation_requirements.arena_allocation_bytes >=
            allocation_requirements.arena_requested_bytes);
    REQUIRE(allocation_requirements.pessimistic_output_requested_bytes ==
            std::max<std::uint64_t>(
                vk::minimizer_wire_traits<minimap_sketch_data>::device_bytes_for_count(
                    pessimistic_count),
                4u));
    REQUIRE(allocation_requirements.pessimistic_output_allocation_bytes >=
            allocation_requirements.pessimistic_output_requested_bytes);
    auto result = vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
        std::span<const vk::sequence_upload_view>{all_n_uploads.data(), all_n_uploads.size()},
        rank_lut,
        window_size,
        kmer_size,
        vk::vulkan_sketch_host_materialization::device_only);

    REQUIRE(result.has_value());
    REQUIRE_FALSE(vk::is_runtime_metal_archive_configuration_installed());
    REQUIRE(result->query_authority.does_cover_device_output());
    // Structural coverage cannot mint the causal immutable-pipeline stats
    // required by the persisted grouped-SoA consumer.
    REQUIRE_FALSE(result->query_authority.is_valid());
    REQUIRE(result->dispatch_stats.is_internally_consistent());
    REQUIRE(result->dispatch_stats.materialization() ==
            vk::vulkan_sketch_host_materialization::device_only);
    REQUIRE(result->dispatch_stats.count_dispatches() == 1u);
    REQUIRE(result->dispatch_stats.prefix_scan_sequences() == 1u);
    REQUIRE(result->dispatch_stats.prefix_scan_dispatches() == 3u);
    REQUIRE(result->dispatch_stats.emit_dispatches() == 1u);
    REQUIRE(result->dispatch_stats.completed_roundtrips() == 2u);
    REQUIRE(result->query_records.size() == 1u);
    REQUIRE(result->query_records[0].minimizer_offset == 0u);
    REQUIRE(result->query_records[0].minimizer_count == 0u);
    REQUIRE(result->query_records[0].query_len == all_n.size());
    REQUIRE(result->query_records[0].flags == 0u);
    REQUIRE(result->host_minimizers.empty());
    REQUIRE(result->host_minimizers.capacity() == 0u);
    REQUIRE(result->device != nullptr);
    REQUIRE(result->memory_retention != nullptr);
    REQUIRE(result->device->batch_memory_retention == result->memory_retention);
    REQUIRE(result->device_minimizer_count == 0u);
    REQUIRE(result->device->count == 0u);
    REQUIRE(result->device->capacity == 0u);
    REQUIRE(result->device->words.bytes == 4u);
    REQUIRE(result->query_authority.owned_capacity_bytes() ==
            sizeof(vk::gpu_seed_query_record_wire) + sizeof(std::uint32_t));
    REQUIRE(result->memory_retention->permit.bytes() ==
            result->device->words.allocation_bytes +
                static_cast<std::uint64_t>(result->query_records.capacity()) *
                    sizeof(vk::gpu_seed_query_record_wire) +
                result->query_authority.owned_capacity_bytes());
    REQUIRE(result->memory_retention->permit.bytes() >= result->device->words.allocation_bytes);

    std::vector<std::byte> downloaded;
    vk::runtime::instance().download_slice(
        result->device->words, 0u, std::span<std::byte>{downloaded.data(), downloaded.size()});
    REQUIRE(downloaded.empty());

    // A malformed post-submit authority record must fail closed without
    // leaving the shared arena cursor live for the next independent request.
    std::vector<std::byte> packed_sequence(
        static_cast<std::size_t>(all_n_uploads[0].padded_byte_size));
    std::memcpy(packed_sequence.data(),
                all_n_uploads[0].bytes,
                static_cast<std::size_t>(all_n_uploads[0].byte_size));
    std::vector<vk::gpu_sketch_tile_wire> tiles;
    tiles.reserve(all_n_tile_plan.tile_count);
    const std::uint32_t overlap = vk::sketch_replay_overlap_bp(
        static_cast<std::uint32_t>(kmer_size), static_cast<std::uint32_t>(window_size));
    for (std::uint32_t tile = 0u; tile < all_n_tile_plan.tile_count; ++tile) {
        const std::uint32_t lo = tile * all_n_tile_plan.tile_bp;
        const std::uint32_t hi = std::min<std::uint32_t>(static_cast<std::uint32_t>(all_n.size()),
                                                         (tile + 1u) * all_n_tile_plan.tile_bp);
        tiles.push_back(vk::gpu_sketch_tile_wire{
            .seq_word_offset = 0u,
            .bp_count = static_cast<std::uint32_t>(all_n.size()),
            .replay_lo = lo > overlap ? lo - overlap : 0u,
            .tile_lo = lo,
            .tile_hi = hi,
            .out_base = 0u,
        });
    }
    const std::array malformed_layouts{
        vk::gpu_sketch_query_layout_wire{
            .begin = 0u,
            .count = static_cast<std::uint32_t>(tiles.size()),
            .query_len = 0u,
        },
    };
    const vk::vulkan_sketch_params malformed_params{
        .bp_size = static_cast<std::uint32_t>(all_n.size()),
        .window_size = static_cast<std::uint32_t>(window_size),
        .kmer_size = static_cast<std::uint32_t>(kmer_size),
        .hash_bits = vk::minimizer_wire_traits<minimap_sketch_data>::k_hash_bits,
        .hash_shift = vk::minimizer_wire_traits<minimap_sketch_data>::k_hash_shift,
        .pos_bits = vk::minimizer_wire_traits<minimap_sketch_data>::k_position_bits,
        .minimizer_word_kind =
            static_cast<std::uint32_t>(vk::minimizer_wire_traits<minimap_sketch_data>::k_word_kind),
        .minimizer_words32 =
            vk::minimizer_wire_traits<minimap_sketch_data>::k_words32_per_minimizer,
        .alphabet_kind = static_cast<std::uint32_t>(all_n_uploads[0].alphabet_kind),
        .unit_bits = all_n_uploads[0].unit_bits,
        .bits_per_char = all_n_uploads[0].bits_per_char,
        .letters_per_unit = all_n_uploads[0].letters_per_unit,
        .alphabet_size = all_n_uploads[0].alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = all_n_uploads[0].char_mask,
        .padding_letters = all_n_uploads[0].padding_letters,
    };
    bool malformed_authority_rejected = false;
    try {
        (void)vk::runtime::instance().run_sketch_tile_batch_roundtrip(
            std::span<const std::byte>{packed_sequence.data(), packed_sequence.size()},
            rank_lut,
            malformed_params,
            std::span<const vk::gpu_sketch_tile_wire>{tiles.data(), tiles.size()},
            std::span<const vk::gpu_sketch_query_layout_wire>{malformed_layouts.data(),
                                                              malformed_layouts.size()},
            pessimistic_count,
            vk::vulkan_sketch_host_materialization::device_only);
    } catch (const vk::vulkan_runtime_error& error) {
        malformed_authority_rejected = std::string_view{error.what()} ==
                                       "gpu sketch query authority does not cover device output";
    }
    REQUIRE(malformed_authority_rejected);
    REQUIRE(vk::runtime::instance().reusable_gpu_arena().release_idle_capacity());
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
