/**
 * @file grouped_soa_test.cpp
 * @brief Tests grouped-SoA codecs and prepared readers.
 *
 * The tests run inside `bioseq::simd::HWY_NAMESPACE` because they call the
 * selected minimizer collectors directly.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <hwy/highway.h>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <tests/support/common.hpp>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.chaining.core;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.pipeline.minimap_port;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.common.hashing; //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

namespace {

/// Validate a captured row and return its move-only receipt.
[[nodiscard]] bioseq::indexing::validated_minimizer_generation_row_receipt
trusted_row_receipt(const collected_minimizer_row& row) {
    auto validated = bioseq::indexing::validate_minimizer_generation_row_receipt(row_view(row));
    REQUIRE(validated.has_value());
    return std::move(*validated);
}

/// Recompute the address-bound checksum after mutating a captured row.
void recompute_row_checksum(collected_minimizer_row& row) noexcept {
    row.row_checksum =
        bioseq::indexing::crc64_index_chunk_row(row.address.generation_id,
                                                row.address.position_segment,
                                                row.address.sub_key,
                                                row.address.chunk_no,
                                                static_cast<std::uint32_t>(row.bytes.size()),
                                                row.bytes);
}
} // namespace

TEST_CASE("grouped-SoA derives exact H P C O boundaries and SoA offsets",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr auto shape = [](std::uint16_t k, std::uint64_t span, std::uint64_t cap) {
        return derive_grouped_soa_minimizer_profile(
            k_minimap_scheme_identity, k_minimizer_persisted_dna4_alphabet_code, k, span, cap);
    };
    STATIC_REQUIRE(shape(8u, 32'768u, 65'534u)->hash_width == minimizer_scalar_width::u16);
    STATIC_REQUIRE(shape(9u, 32'768u, 65'534u)->hash_width == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(15u, 32'768u, 65'534u)->hash_width == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(16u, 32'768u, 65'534u)->hash_width == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(17u, 32'768u, 65'534u)->hash_width == minimizer_scalar_width::u64);
    STATIC_REQUIRE(shape(28u, 32'768u, 65'534u).has_value());
    STATIC_REQUIRE_FALSE(shape(29u, 32'768u, 65'534u).has_value());
    STATIC_REQUIRE_FALSE(
        derive_grouped_soa_minimizer_profile(k_minimap_scheme_identity, 1u, 15u, 32'768u, 65'534u)
            .has_value());
    STATIC_REQUIRE_FALSE(
        derive_grouped_soa_minimizer_profile(k_minimap_scheme_identity, 4u, 15u, 32'768u, 65'534u)
            .has_value());
    STATIC_REQUIRE(derive_grouped_soa_minimizer_profile(k_bounded_nthash_u32_r7_sum_scheme_identity,
                                                        k_minimizer_persisted_dna4_alphabet_code,
                                                        15u,
                                                        32'768u,
                                                        65'534u)
                       ->hash_width == minimizer_scalar_width::u32);
    STATIC_REQUIRE_FALSE(
        derive_grouped_soa_minimizer_profile(k_bounded_nthash_u32_r7_sum_scheme_identity,
                                             k_minimizer_persisted_dna4_alphabet_code,
                                             14u,
                                             32'768u,
                                             65'534u)
            .has_value());
    STATIC_REQUIRE_FALSE(
        derive_grouped_soa_minimizer_profile(k_bounded_nthash_u32_r7_sum_scheme_identity,
                                             k_minimizer_persisted_dna4_alphabet_code,
                                             16u,
                                             32'768u,
                                             65'534u)
            .has_value());

    constexpr auto scalar_expected = expected_grouped_soa_generation_identity_for_backend(
        minimizer_generation_layout::grouped_soa_segment_local,
        minimizer_sketch_backend::scalar,
        k_minimizer_persisted_dna4_alphabet_code,
        8u,
        15u,
        10u,
        65'534u);
    STATIC_REQUIRE(scalar_expected.has_value());
    constexpr auto exact_simd_expected = expected_grouped_soa_generation_identity_for_backend(
        minimizer_generation_layout::grouped_soa_segment_local,
        minimizer_sketch_backend::exact_simd,
        k_minimizer_persisted_dna4_alphabet_code,
        8u,
        15u,
        10u,
        65'534u);
    STATIC_REQUIRE(exact_simd_expected.has_value());
    STATIC_REQUIRE(shape(15u, 32'768u, 65'534u)->position_width == minimizer_scalar_width::u16);
    STATIC_REQUIRE(shape(15u, 32'769u, 65'534u)->position_width == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(15u, std::uint64_t{1} << 31u, 65'534u)->position_width ==
                   minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(15u, (std::uint64_t{1} << 31u) + 1u, 65'534u)->position_width ==
                   minimizer_scalar_width::u64);
    STATIC_REQUIRE(shape(15u, 1u, 65'534u)->count_width == minimizer_scalar_width::u16);
    STATIC_REQUIRE(shape(15u, 1u, 65'535u)->count_width == minimizer_scalar_width::u32);
    STATIC_REQUIRE(
        shape(15u, 1u, static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) - 1u)
            ->count_width == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(15u, 1u, (std::numeric_limits<std::uint32_t>::max)())->count_width ==
                   minimizer_scalar_width::u64);
    STATIC_REQUIRE_FALSE(shape(15u, 1u, (std::numeric_limits<std::uint64_t>::max)()).has_value());
    STATIC_REQUIRE(select_grouped_soa_offset_width(65'535u) == minimizer_scalar_width::u16);
    STATIC_REQUIRE(select_grouped_soa_offset_width(65'536u) == minimizer_scalar_width::u32);
    STATIC_REQUIRE(select_grouped_soa_offset_width((std::numeric_limits<std::uint32_t>::max)()) ==
                   minimizer_scalar_width::u32);
    STATIC_REQUIRE(select_grouped_soa_offset_width(
                       static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) +
                       1u) == minimizer_scalar_width::u64);

    constexpr auto layout = plan_grouped_soa_control_plane(
        2u, minimizer_scalar_width::u32, minimizer_scalar_width::u16, minimizer_scalar_width::u16);
    STATIC_REQUIRE(layout.has_value());
    STATIC_REQUIRE(layout->hash_offset == 0u);
    STATIC_REQUIRE(layout->prefix_offset == 8u);
    STATIC_REQUIRE(layout->count_offset == 16u);
    STATIC_REQUIRE(layout->encoded_bytes == 24u);
    STATIC_REQUIRE_FALSE(plan_grouped_soa_control_plane((std::numeric_limits<std::uint64_t>::max)(),
                                                        minimizer_scalar_width::u64,
                                                        minimizer_scalar_width::u64,
                                                        minimizer_scalar_width::u64)
                             .has_value());

    constexpr auto profile = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity, k_minimizer_persisted_dna4_alphabet_code, 15u, 8u, 65'534u);
    STATIC_REQUIRE(profile.has_value());
    constexpr auto chunks = [](std::uint64_t bytes) {
        constexpr std::uint32_t extent_payload_bytes =
            minimizer_extent_payload_bytes(minimizer_generation_layout::grouped_soa_segment_local);
        return static_cast<std::uint32_t>(bytes / extent_payload_bytes +
                                          (bytes % extent_payload_bytes != 0u ? 1u : 0u));
    };
    constexpr auto make_wide_entry = [chunks](std::uint64_t positions) {
        const auto control_layout =
            plan_grouped_soa_control_plane(1u,
                                           minimizer_scalar_width::u32,
                                           select_grouped_soa_offset_width(positions),
                                           minimizer_scalar_width::u16);
        return grouped_soa_bucket_directory_entry{
            .partition_id = 0u,
            .position_segment = 0,
            .lo_hash = 1u,
            .hi_hash = 1u,
            .sub_key = 0,
            .control_first_chunk = 0u,
            .control_chunk_count = chunks(control_layout->encoded_bytes),
            .position_first_chunk = 0u,
            .position_chunk_count = chunks(positions * 2u),
            .control_element_count = 1u,
            .position_element_count = positions,
            .control_encoded_bytes = control_layout->encoded_bytes,
            .position_encoded_bytes = positions * 2u,
            .control_checksum = 1u,
            .position_checksum = 2u,
            .shape_offset_width = select_grouped_soa_offset_width(positions),
        };
    };
    constexpr auto o32_entry = make_wide_entry(65'536u);
    constexpr auto o32_bytes = encode_grouped_soa_bucket_directory_entry(o32_entry, *profile);
    STATIC_REQUIRE(o32_bytes.has_value());
    STATIC_REQUIRE((*o32_bytes)[97] == std::byte{4});
    constexpr auto decoded_o32 = decode_minimizer_bucket_dir_entry(*o32_bytes, *profile);
    STATIC_REQUIRE(decoded_o32.has_value());
    STATIC_REQUIRE(*decoded_o32 == o32_entry);
    constexpr std::uint64_t o64_span = (std::uint64_t{1} << 31u) + 1u;
    constexpr auto o64_profile =
        derive_grouped_soa_minimizer_profile(k_minimap_scheme_identity,
                                             k_minimizer_persisted_dna4_alphabet_code,
                                             15u,
                                             o64_span,
                                             65'534u);
    STATIC_REQUIRE(o64_profile.has_value());
    STATIC_REQUIRE(o64_profile->position_width == minimizer_scalar_width::u64);
    constexpr auto o64_entry = [make_wide_entry, chunks] {
        auto value = make_wide_entry(
            static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1u);
        value.position_encoded_bytes = value.position_element_count * 8u;
        value.position_chunk_count = chunks(value.position_encoded_bytes);
        return value;
    }();
    constexpr auto o64_bytes = encode_grouped_soa_bucket_directory_entry(o64_entry, *o64_profile);
    STATIC_REQUIRE(o64_bytes.has_value());
    STATIC_REQUIRE((*o64_bytes)[97] == std::byte{8});
    constexpr auto decoded_o64 = decode_minimizer_bucket_dir_entry(*o64_bytes, *o64_profile);
    STATIC_REQUIRE(decoded_o64.has_value());
    STATIC_REQUIRE(*decoded_o64 == o64_entry);

    minimizer_generation_stream_detail::grouped_soa_plan_pass o64_plan{
        .span = o64_span,
        .target_length = o64_span,
        .profile = *o64_profile,
    };
    o64_plan.current_segment = 0u;
    o64_plan.current_hash = 1u;
    o64_plan.group_positions = o64_entry.position_element_count;
    REQUIRE(o64_plan.finish().has_value());
    REQUIRE(o64_plan.entries.size() == 1u);
    REQUIRE(o64_plan.entries.front().position_element_count == o64_entry.position_element_count);
    REQUIRE(o64_plan.entries.front().shape_offset_width == minimizer_scalar_width::u64);
    auto planned_o64_expected = o64_entry;
    planned_o64_expected.control_checksum = 0u;
    planned_o64_expected.position_checksum = 0u;
    REQUIRE(o64_plan.entries.front() == planned_o64_expected);

    const auto o64_layout = plan_grouped_soa_control_plane(o64_entry.control_element_count,
                                                           o64_profile->hash_width,
                                                           o64_entry.shape_offset_width,
                                                           o64_profile->count_width);
    REQUIRE(o64_layout.has_value());
    std::vector<std::byte> o64_controls(static_cast<std::size_t>(o64_layout->encoded_bytes));
    test_write_le<std::uint32_t>(o64_controls, 0u, 1u);
    test_write_le<std::uint64_t>(
        o64_controls, static_cast<std::size_t>(o64_layout->prefix_offset), 0u);
    test_write_le<std::uint64_t>(o64_controls,
                                 static_cast<std::size_t>(o64_layout->prefix_offset + 8u),
                                 o64_entry.position_element_count);
    test_write_le<std::uint16_t>(
        o64_controls, static_cast<std::size_t>(o64_layout->count_offset), 65'535u);
    const auto decoded_o64_controls =
        decode_grouped_soa_controls(o64_controls, o64_entry, *o64_profile);
    REQUIRE(decoded_o64_controls.has_value());
    REQUIRE(decoded_o64_controls->size() == 1u);
    REQUIRE(decoded_o64_controls->front().position_begin == 0u);
    REQUIRE(decoded_o64_controls->front().position_end == o64_entry.position_element_count);
    REQUIRE(decoded_o64_controls->front().saturated_count == 65'535u);
}

TEST_CASE("grouped-SoA fixed records use exact 160-byte and 128-byte layouts",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr auto b = [](unsigned value) { return static_cast<std::byte>(value); };
    constexpr auto shape = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity, k_minimizer_persisted_dna4_alphabet_code, 15u, 8u, 10u);
    STATIC_REQUIRE(shape.has_value());
    constexpr auto profile = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity, k_minimizer_persisted_dna4_alphabet_code, 15u, 8u, 10u);
    STATIC_REQUIRE(profile.has_value());
    constexpr auto manifest = [shape] {
        auto value = golden_generation_manifest();
        value.layout = minimizer_generation_layout::grouped_soa_segment_local;
        value.alphabet_code = k_minimizer_persisted_dna4_alphabet_code;
        value.kmer_size = 15u;
        value.gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
        assign_grouped_soa_profile(value, *shape);
        return value;
    }();
    constexpr auto encoded_manifest = encode_minimizer_generation_manifest(manifest);
    STATIC_REQUIRE(encoded_manifest.has_value());
    STATIC_REQUIRE(encoded_manifest->size() == 160u);
    constexpr std::array<std::byte, 160> expected_manifest{
        b(0x47), b(0x4f), b(0x49), b(0x42), b(0x05), b(0x00), b(0xa0), b(0x00), b(0x08), b(0x07),
        b(0x06), b(0x05), b(0x04), b(0x03), b(0x02), b(0x01), b(0x18), b(0x17), b(0x16), b(0x15),
        b(0x14), b(0x13), b(0x12), b(0x11), b(0x28), b(0x27), b(0x26), b(0x25), b(0x24), b(0x23),
        b(0x22), b(0x21), b(0x14), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x38), b(0x37), b(0x36), b(0x35), b(0x34), b(0x33), b(0x32), b(0x31), b(0x48), b(0x47),
        b(0x46), b(0x45), b(0x44), b(0x43), b(0x42), b(0x41), b(0x58), b(0x57), b(0x56), b(0x55),
        b(0x54), b(0x53), b(0x52), b(0x51), b(0x68), b(0x67), b(0x66), b(0x65), b(0x64), b(0x63),
        b(0x62), b(0x61), b(0x78), b(0x77), b(0x76), b(0x75), b(0x74), b(0x73), b(0x72), b(0x71),
        b(0x10), b(0x0f), b(0x0e), b(0x0d), b(0x0c), b(0x0b), b(0x0a), b(0x09), b(0x03), b(0x00),
        b(0x00), b(0x00), b(0x44), b(0x33), b(0x22), b(0x11), b(0x88), b(0x77), b(0x66), b(0x55),
        b(0x00), b(0x00), b(0x44), b(0x33), b(0x0f), b(0x00), b(0x88), b(0x77), b(0x03), b(0x00),
        b(0x00), b(0x00), b(0x08), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x02), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x01), b(0x53), b(0x4f),
        b(0x49), b(0x42), b(0x01), b(0x00), b(0x20), b(0x00), b(0x04), b(0x02), b(0x02), b(0x01),
        b(0x01), b(0x01), b(0x02), b(0x00), b(0xff), b(0xff), b(0xff), b(0x3f), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x0a), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
    };
    STATIC_REQUIRE(*encoded_manifest == expected_manifest);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 4) == 5u);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 6) == 160u);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded_manifest, 108) == 3u);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded_manifest, 128) ==
                   k_grouped_soa_extension_magic);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 132) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 134) == 32u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 136) == 4u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 137) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 138) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 139) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 140) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 141) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_manifest, 142) == 2u);
    STATIC_REQUIRE((*encoded_manifest)[143] == std::byte{0});
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded_manifest, 144) == shape->max_hash);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded_manifest, 152) == 10u);
    constexpr auto decoded_manifest =
        decode_minimizer_generation_manifest(std::span<const std::byte>{*encoded_manifest});
    STATIC_REQUIRE(decoded_manifest.has_value());
    STATIC_REQUIRE(*decoded_manifest == manifest);

    constexpr grouped_soa_bucket_directory_entry entry{
        .partition_id = 0,
        .position_segment = 0,
        .lo_hash = 1,
        .hi_hash = 2,
        .sub_key = 0,
        .control_chunk_count = 1,
        .position_chunk_count = 1,
        .control_element_count = 2,
        .position_element_count = 3,
        .control_encoded_bytes = 24,
        .position_encoded_bytes = 6,
        .control_checksum = 0x0102030405060708ULL,
        .position_checksum = 0x1112131415161718ULL,
        .shape_offset_width = minimizer_scalar_width::u16,
    };
    constexpr auto encoded_entry = encode_grouped_soa_bucket_directory_entry(entry, *profile);
    STATIC_REQUIRE(encoded_entry.has_value());
    STATIC_REQUIRE(encoded_entry->size() == 128u);
    constexpr std::array<std::byte, 128> expected_entry{
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x02), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x00), b(0x00), b(0x00), b(0x02), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x03), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x18), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x06), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x08), b(0x07), b(0x06), b(0x05), b(0x04), b(0x03), b(0x02), b(0x01), b(0x18), b(0x17),
        b(0x16), b(0x15), b(0x14), b(0x13), b(0x12), b(0x11), b(0x04), b(0x02), b(0x02), b(0x02),
        b(0x01), b(0x01), b(0x02), b(0x01), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
    };
    STATIC_REQUIRE(*encoded_entry == expected_entry);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 96) == 4u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 97) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 98) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 99) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 100) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 101) == 1u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 102) == 2u);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*encoded_entry, 103) == 1u);
    constexpr auto decoded_entry =
        decode_minimizer_bucket_dir_entry(std::span<const std::byte>{*encoded_entry}, *profile);
    STATIC_REQUIRE(decoded_entry.has_value());
    STATIC_REQUIRE(*decoded_entry == entry);
}

TEST_CASE("grouped-SoA fixed records reject every extension corruption class",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const auto shape = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity, k_minimizer_persisted_dna4_alphabet_code, 15u, 8u, 10u);
    REQUIRE(shape.has_value());
    const auto profile = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity, k_minimizer_persisted_dna4_alphabet_code, 15u, 8u, 10u);
    REQUIRE(profile.has_value());
    auto manifest = golden_generation_manifest();
    manifest.layout = minimizer_generation_layout::grouped_soa_segment_local;
    manifest.alphabet_code = k_minimizer_persisted_dna4_alphabet_code;
    manifest.kmer_size = 15u;
    manifest.gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
    assign_grouped_soa_profile(manifest, *shape);
    const auto encoded_manifest = encode_minimizer_generation_manifest(manifest);
    REQUIRE(encoded_manifest.has_value());
    const auto good_manifest = *encoded_manifest;

    for (const std::size_t offset : {128u, 132u, 134u}) {
        INFO("grouped-SoA extension header offset " << offset);
        auto corrupt = good_manifest;
        corrupt[offset] ^= std::byte{1};
        require_wire_error(decode_minimizer_generation_manifest(corrupt),
                           minimizer_generation_encoding_error::unsupported_storage_layout);
    }
    for (std::size_t offset = 136u; offset <= 142u; ++offset) {
        INFO("grouped-SoA shape field offset " << offset);
        auto corrupt = good_manifest;
        corrupt[offset] = std::byte{0};
        REQUIRE_FALSE(decode_minimizer_generation_manifest(corrupt));
    }
    {
        auto corrupt = good_manifest;
        corrupt[143] = std::byte{1};
        require_wire_error(decode_minimizer_generation_manifest(corrupt),
                           minimizer_generation_encoding_error::reserved_nonzero);
    }
    {
        INFO("grouped-SoA derived hash bound offset 144");
        auto corrupt = good_manifest;
        corrupt[144] ^= std::byte{1};
        REQUIRE_FALSE(decode_minimizer_generation_manifest(corrupt));
    }
    {
        /* max_occ_cap is a caller-selected storage-contract parameter, not a
         * bound derivable from K/span.  A same-width value remains a valid
         * standalone manifest but must fail the reader's expected identity. */
        const auto expected = expected_grouped_soa_generation_identity_for_backend(
            minimizer_generation_layout::grouped_soa_segment_local,
            minimizer_sketch_backend::scalar,
            manifest.alphabet_code,
            manifest.storage_unit_byte_width,
            manifest.kmer_size,
            manifest.window_size,
            shape->max_occ_cap);
        REQUIRE(expected.has_value());
        REQUIRE(validate_minimizer_generation_storage_contract(manifest, *expected).has_value());
        auto changed_cap = good_manifest;
        changed_cap[152] ^= std::byte{1};
        const auto decoded = decode_minimizer_generation_manifest(changed_cap);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->max_occ_cap == (shape->max_occ_cap ^ 1u));
        require_wire_error(validate_minimizer_generation_storage_contract(*decoded, *expected),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    {
        /* Crossing the u16/u32 count-width boundary while retaining the
         * encoded u16 shape is intrinsically inconsistent and fails decode. */
        auto incompatible_cap_width = good_manifest;
        test_write_le<std::uint64_t>(incompatible_cap_width, 152u, 65'535u);
        require_wire_error(decode_minimizer_generation_manifest(incompatible_cap_width),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }

    constexpr grouped_soa_bucket_directory_entry entry{
        .partition_id = 0u,
        .position_segment = 0,
        .lo_hash = 1u,
        .hi_hash = 2u,
        .sub_key = 0,
        .control_first_chunk = 0u,
        .control_chunk_count = 1u,
        .position_first_chunk = 0u,
        .position_chunk_count = 1u,
        .control_element_count = 2u,
        .position_element_count = 3u,
        .control_encoded_bytes = 24u,
        .position_encoded_bytes = 6u,
        .control_checksum = 1u,
        .position_checksum = 2u,
        .shape_offset_width = minimizer_scalar_width::u16,
    };
    const auto encoded_entry = encode_grouped_soa_bucket_directory_entry(entry, *profile);
    REQUIRE(encoded_entry.has_value());
    const auto good_entry = *encoded_entry;
    for (std::size_t offset = 96u; offset <= 103u; ++offset) {
        INFO("grouped-SoA directory shape offset " << offset);
        auto corrupt = good_entry;
        corrupt[offset] = std::byte{0};
        require_wire_error(decode_minimizer_bucket_dir_entry(corrupt, *profile),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    for (std::size_t offset = 104u; offset < 128u; ++offset) {
        INFO("grouped-SoA directory reserved offset " << offset);
        auto corrupt = good_entry;
        corrupt[offset] = std::byte{1};
        require_wire_error(decode_minimizer_bucket_dir_entry(corrupt, *profile),
                           minimizer_generation_encoding_error::reserved_nonzero);
    }
}

TEST_CASE("grouped-SoA emits exact SoA and prepared reader rejects corruption",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(std::is_move_constructible_v<prepared_grouped_soa_query_reader>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<prepared_grouped_soa_query_reader>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<prepared_grouped_soa_query_reader>);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, (3u << 1u) | 1u},
        {1u, (9u << 1u)},
        // A new hash may restart below the previous hash's last coordinate.
        // This checks that the reader's strict-order state resets per control.
        {2u, (1u << 1u) | 1u},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(100u, 10u);
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(producer.scan_count == 3u);
    REQUIRE(generation->manifest_row.byte_count == 160u);
    REQUIRE(generation->directory.encoded_bytes().size() ==
            generation->directory.entries().size() * 128u);
    REQUIRE(generation->directory.entries().size() == 1u);
    const auto& entry = generation->directory.entries().front();
    REQUIRE(entry.shape_offset_width == minimizer_scalar_width::u16);
    REQUIRE(sink.rows.size() >= entry.control_chunk_count + entry.position_chunk_count);
    REQUIRE(sink.rows.front().address.sub_key == entry.sub_key);
    REQUIRE(sink.rows[entry.control_chunk_count].address.sub_key ==
            *derive_minimizer_position_extent_sub_key(entry.sub_key));
    const auto control_bytes = collect_encoded_extent(sink.rows, entry, true);
    const auto position_bytes = collect_encoded_extent(sink.rows, entry, false);
    const auto controls =
        decode_grouped_soa_controls(control_bytes, entry, generation->directory.profile());
    REQUIRE(controls.has_value());
    REQUIRE(controls->size() == 2u);
    REQUIRE(((*controls)[0] == grouped_soa_decoded_control{
                                   .hash = 1u,
                                   .position_begin = 0u,
                                   .position_end = 2u,
                                   .saturated_count = 2u,
                               }));
    REQUIRE((*controls)[1].saturated_count == 1u);

    bioseq::parallelism::memory_admission_controller admission;
    const std::array<std::uint64_t, 3> queries{1u, 2u, 999u};
    auto reader =
        prepared_grouped_soa_query_reader::begin(admission,
                                                 generation->head,
                                                 generation->manifest,
                                                 test_grouped_soa_expected_storage_contract(10u),
                                                 generation->directory.encoded_bytes(),
                                                 queries,
                                                 10u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());
    const auto hash1 = reader->lookup(1u);
    REQUIRE(hash1.global_occurrence_count == 2u);
    REQUIRE(hash1.positions_complete);
    REQUIRE((std::vector<std::uint64_t>{hash1.positions.begin(), hash1.positions.end()} ==
             std::vector<std::uint64_t>{7u, 18u}));
    REQUIRE(reader->lookup(2u).positions.front() == 3u);
    REQUIRE(reader->lookup(999u).positions.empty());

    SECTION("padding byte") {
        auto corrupt = control_bytes;
        // H32*2 ends at 8; prefixes occupy [8,14), so [14,16) is padding.
        corrupt[14] = std::byte{1};
        require_wire_error(
            decode_grouped_soa_controls(corrupt, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::reserved_nonzero);
    }
    SECTION("terminal prefix") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 12u, 2u);
        require_wire_error(
            decode_grouped_soa_controls(corrupt, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::control_range);
    }
    SECTION("interior prefix cannot overlap") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 10u, 0u);
        require_wire_error(
            decode_grouped_soa_controls(corrupt, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::control_range);
    }
    SECTION("saturated count") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 16u, 1u);
        require_wire_error(
            decode_grouped_soa_controls(corrupt, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::control_range);
    }
    SECTION("directory hash endpoints are exact") {
        auto corrupt_first = control_bytes;
        test_write_le<std::uint32_t>(corrupt_first, 0u, 0u);
        require_wire_error(
            decode_grouped_soa_controls(corrupt_first, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::hash_range);
        auto corrupt_last = control_bytes;
        test_write_le<std::uint32_t>(corrupt_last, 4u, 3u);
        require_wire_error(
            decode_grouped_soa_controls(corrupt_last, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::hash_range);
    }
    SECTION("positions are strictly ordered inside one hash") {
        auto corrupt = position_bytes;
        test_write_le<std::uint16_t>(corrupt, 2u, test_read_le<std::uint16_t>(corrupt, 0u));
        require_wire_error(decode_grouped_soa_positions(
                               corrupt,
                               *controls,
                               entry,
                               *derive_minimizer_position_segment_bounds(generation->manifest, 0u),
                               generation->directory.profile()),
                           minimizer_generation_encoding_error::position_range);
    }
    SECTION("prepared reader retains strict order state") {
        auto corrupt = sink.rows[entry.control_chunk_count];
        test_write_le<std::uint16_t>(
            corrupt.bytes, 2u, test_read_le<std::uint16_t>(corrupt.bytes, 0u));
        recompute_row_checksum(corrupt);
        // Hash 1 is deliberately absent from the query union: every control
        // boundary must still enforce strict order without retaining the
        // 16-byte route-and-boundary record beside it.
        const std::array<std::uint64_t, 1> unqueried_hash{2u};
        auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            unqueried_hash,
            10u);
        REQUIRE(made.has_value());
        for (std::size_t index = 0; index < entry.control_chunk_count; ++index) {
            REQUIRE(made->accept(row_view(sink.rows[index])).has_value());
        }
        require_wire_error(made->accept(row_view(corrupt)),
                           minimizer_generation_encoding_error::position_range);
    }
    SECTION("positions cannot escape their persisted segment") {
        auto corrupt = position_bytes;
        test_write_le<std::uint16_t>(corrupt, 0u, 200u);
        require_wire_error(decode_grouped_soa_positions(
                               corrupt,
                               *controls,
                               entry,
                               *derive_minimizer_position_segment_bounds(generation->manifest, 0u),
                               generation->directory.profile()),
                           minimizer_generation_encoding_error::position_range);
    }
    SECTION("prepared reader checks extent checksum") {
        auto corrupt = sink.rows.front();
        corrupt.bytes.front() ^= std::byte{1};
        auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u);
        REQUIRE(made.has_value());
        require_wire_error(made->accept(row_view(corrupt)),
                           minimizer_generation_encoding_error::checksum);
    }
    SECTION("prepared reader applies max-occ without losing global count") {
        auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            queries,
            1u);
        REQUIRE(made.has_value());
        for (const auto& row : sink.rows) {
            if (row.address.position_segment >= 0) {
                REQUIRE(made->accept(row_view(row)).has_value());
            }
        }
        REQUIRE(made->finish().has_value());
        const auto pruned = made->lookup(1u);
        REQUIRE(pruned.global_occurrence_count == 2u);
        REQUIRE_FALSE(pruned.positions_complete);
        REQUIRE(pruned.positions.empty());
    }
    SECTION("query occurrence limit cannot exceed persisted cap") {
        require_wire_error(prepared_grouped_soa_query_reader::begin(
                               admission,
                               generation->head,
                               generation->manifest,
                               test_grouped_soa_expected_storage_contract(10u),
                               generation->directory.encoded_bytes(),
                               queries,
                               11u),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    SECTION("query hashes obey the persisted H width") {
        const std::array<std::uint64_t, 1> too_wide{generation->directory.profile().max_hash + 1u};
        require_wire_error(prepared_grouped_soa_query_reader::begin(
                               admission,
                               generation->head,
                               generation->manifest,
                               test_grouped_soa_expected_storage_contract(10u),
                               generation->directory.encoded_bytes(),
                               too_wide,
                               10u),
                           minimizer_generation_encoding_error::hash_range);
    }
}

TEST_CASE("grouped-SoA compact counts remain exact across u16",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t segment_span = 8u * 1024u * 1024u;
    constexpr std::uint64_t per_segment = 35'000u;
    constexpr std::uint64_t exact_count = 2u * per_segment;
    constexpr std::uint64_t persisted_cap = 100'000u;
    constexpr std::uint64_t query_hash = 0x1555'5555u;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(exact_count);
    for (std::uint64_t index = 0u; index < per_segment; ++index) {
        minimizers.emplace_back(query_hash, (index + 1u) << 1u);
    }
    for (std::uint64_t index = 0u; index < per_segment; ++index) {
        minimizers.emplace_back(query_hash, (segment_span + index + 1u) << 1u);
    }

    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(2u * segment_span, persisted_cap));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_count == exact_count);
    const std::array<std::uint64_t, 1> queries{query_hash};

    bioseq::parallelism::memory_admission_controller retained_admission;
    bioseq::parallelism::memory_admission_controller pruned_admission;
    const auto make_reader =
        [&generation, &queries, &sink](bioseq::parallelism::memory_admission_controller& admission,
                                       std::uint64_t max_occ) {
            auto reader = prepared_grouped_soa_query_reader::begin(
                admission,
                generation->head,
                generation->manifest,
                test_grouped_soa_expected_storage_contract(persisted_cap),
                generation->directory.encoded_bytes(),
                queries,
                max_occ);
            REQUIRE(reader.has_value());
            for (const auto& row : sink.rows) {
                if (row.address.position_segment >= 0) {
                    REQUIRE(reader->accept(row_view(row)).has_value());
                }
            }
            REQUIRE(reader->finish().has_value());
            return reader;
        };

    auto retained_reader = make_reader(retained_admission, persisted_cap);
    const auto retained = retained_reader->lookup(query_hash);
    REQUIRE(retained.global_occurrence_count == exact_count);
    REQUIRE(retained.positions_complete);
    REQUIRE(retained.positions.size() == exact_count);
    REQUIRE(retained.positions.front() == 2u);
    REQUIRE(retained.positions[per_segment] == (segment_span + 1u) << 1u);
    REQUIRE(retained.positions.back() == (segment_span + per_segment) << 1u);
    const auto retained_evidence = retained_reader->memory_stats();
    REQUIRE(retained_evidence.query_record_size_bytes == sizeof(std::uint32_t));
    REQUIRE(retained_evidence.query_record_capacity_bytes ==
            sizeof(std::uint32_t) + sizeof(std::uint64_t));

    auto pruned_reader = make_reader(pruned_admission, (std::numeric_limits<std::uint16_t>::max)());
    const auto pruned = pruned_reader->lookup(query_hash);
    REQUIRE(pruned.global_occurrence_count == exact_count);
    REQUIRE_FALSE(pruned.positions_complete);
    REQUIRE(pruned.positions.empty());
    REQUIRE(pruned_reader->memory_stats().retained_position_count == 0u);
}

TEST_CASE("grouped-SoA cold reader admits retained positions once",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    // Model a wide query union whose mostly-unique hashes each retain one
    // target position. Per-QueryRecord vectors would require two complete
    // ownership walks and fresh OS memory probes for each first append.
    constexpr std::uint64_t query_count = 2048u;
    constexpr std::uint64_t segment_span = 8u * 1024u * 1024u;
    constexpr std::uint64_t segment_count = 3u;
    constexpr auto retained_bp = [](std::uint64_t index) {
        return (index % segment_count) * segment_span + index / segment_count;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    std::vector<std::uint64_t> queries;
    minimizers.reserve(query_count);
    queries.reserve(query_count);
    for (std::uint64_t index = 0; index < query_count; ++index) {
        // Odd multiplication is a permutation modulo 2^30 and spreads this
        // mostly-unique fixture across the same 30-bit domain as production
        // ntHash keys. This exercises actual high-bit radix occupancy rather
        // than putting every synthetic key in bucket zero.
        const std::uint64_t hash = ((index + 1u) * 2'654'435'761ULL) & ((1ULL << 30u) - 1u);
        minimizers.emplace_back(hash, retained_bp(index) << 1u);
        queries.push_back(hash);
    }

    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(segment_count * segment_span, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == segment_count);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader =
        prepared_grouped_soa_query_reader::begin(admission,
                                                 generation->head,
                                                 generation->manifest,
                                                 test_grouped_soa_expected_storage_contract(10u),
                                                 generation->directory.encoded_bytes(),
                                                 queries,
                                                 10u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());

    const auto evidence = reader->memory_stats();
    REQUIRE(evidence.global_position_storage_preparations == 1u);
    REQUIRE(evidence.global_position_storage_allocations == 1u);
    REQUIRE(evidence.position_append_growth_reservations == 0u);
    REQUIRE(evidence.retained_position_count == query_count);
    REQUIRE(evidence.retained_position_capacity == query_count);
    REQUIRE(evidence.query_record_size_bytes == sizeof(std::uint16_t));
    REQUIRE(evidence.query_record_count == query_count);
    REQUIRE(evidence.query_record_capacity >= query_count);
    REQUIRE(evidence.query_record_capacity_bytes ==
            query_count * (sizeof(std::uint16_t) + sizeof(std::uint64_t)));
    REQUIRE(evidence.query_route_radix_bits == 9u);
    REQUIRE(evidence.query_route_radix_bucket_count == 512u);
    REQUIRE(evidence.query_route_radix_nonempty_bucket_count > 0u);
    REQUIRE(evidence.query_route_radix_nonempty_bucket_count <=
            evidence.query_route_radix_bucket_count);
    REQUIRE(evidence.query_route_radix_max_bucket_size <= 8u);
    REQUIRE(evidence.query_hash_capacity_bytes ==
            query_count * sizeof(std::uint32_t) +
                (evidence.query_route_radix_bucket_count + 1u) * sizeof(std::uint32_t));
    REQUIRE(evidence.released_query_hash_capacity_bytes >= query_count * sizeof(std::uint64_t));
    REQUIRE(evidence.decoded_control_records == query_count);
    REQUIRE(evidence.query_route_control_lookups == evidence.decoded_control_records);
    REQUIRE(evidence.query_route_matches == query_count);
    const std::uint64_t global_lower_bound_probe_ceiling =
        evidence.query_route_control_lookups * (std::bit_width(query_count - 1u) + 1u);
    REQUIRE(evidence.query_route_hash_probes * 2u < global_lower_bound_probe_ceiling);
    REQUIRE(evidence.query_route_hash_probes <= evidence.query_route_control_lookups * 5u);
    REQUIRE(evidence.retained_selected_control_records == query_count);
    REQUIRE(evidence.selected_control_size_bytes == 8u);
    // The decoded wire record is eight bytes, but this fixture's immutable
    // segment geometry permits the retained position ends to narrow to u16.
    // The final SoA therefore owns one u16 end plus one u32 dense route per
    // selected query; it does not retain the decoded u64 control image.
    REQUIRE(evidence.retained_selected_control_capacity_bytes ==
            query_count * (sizeof(std::uint16_t) + sizeof(std::uint32_t)));
    REQUIRE(evidence.current_selected_control_capacity_bytes == 0u);
    REQUIRE(evidence.global_position_capacity_bytes ==
            evidence.retained_position_capacity * sizeof(std::uint64_t));
    REQUIRE(evidence.control_payload_capacity_bytes == 0u);
    REQUIRE(evidence.retained_layout_equation_holds());
    REQUIRE(evidence.owned_capacity_bytes == reader->admitted_bytes());
    // Full owner walks are now barrier-only: initialized, controls complete,
    // exact position image published, and final ready. Extent count no longer
    // appears in this equation.
    constexpr std::uint64_t expected_barrier_audits = 4u;
    REQUIRE(evidence.capacity_accounting_passes == expected_barrier_audits);
    REQUIRE_FALSE(evidence.saturated);
    REQUIRE(reader->lookup(queries.front()).positions.front() == retained_bp(0u) << 1u);
    REQUIRE(reader->lookup(queries.back()).positions.front() == retained_bp(query_count - 1u)
                                                                    << 1u);

    // Exercise the statement-level aggregate equation with a second reader
    // that selects only the first of the three control extents. Production
    // merges these per-reader counters, so the aggregate must retain the same
    // 7R + E + 4S shape even when readers select different extent counts.
    const std::array<std::uint64_t, 1> sparse_queries{queries.front()};
    const std::uint64_t sparse_selected_extent_count = static_cast<std::uint64_t>(
        std::ranges::count_if(generation->directory.entries(),
                              [&sparse_queries](const grouped_soa_bucket_directory_entry& entry) {
                                  return entry.lo_hash <= sparse_queries.front() &&
                                         sparse_queries.front() <= entry.hi_hash;
                              }));
    REQUIRE(sparse_selected_extent_count > 0u);
    auto sparse_reader =
        prepared_grouped_soa_query_reader::begin(admission,
                                                 generation->head,
                                                 generation->manifest,
                                                 test_grouped_soa_expected_storage_contract(10u),
                                                 generation->directory.encoded_bytes(),
                                                 sparse_queries,
                                                 10u);
    REQUIRE(sparse_reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(sparse_reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(sparse_reader->finish().has_value());
    const auto sparse_evidence = sparse_reader->memory_stats();
    REQUIRE(sparse_evidence.global_position_storage_preparations == 1u);
    REQUIRE(sparse_evidence.global_position_storage_allocations == 1u);
    REQUIRE(sparse_evidence.position_append_growth_reservations == 0u);
    REQUIRE(sparse_evidence.retained_position_count == 1u);
    REQUIRE(sparse_evidence.retained_position_capacity == 1u);
    REQUIRE(sparse_evidence.query_record_size_bytes == sizeof(std::uint16_t));
    REQUIRE(sparse_evidence.query_record_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_bits == 0u);
    REQUIRE(sparse_evidence.query_route_radix_bucket_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_nonempty_bucket_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_max_bucket_size == 1u);
    REQUIRE(sparse_evidence.query_hash_capacity_bytes ==
            sizeof(std::uint32_t) + 2u * sizeof(std::uint32_t));
    REQUIRE(sparse_evidence.released_query_hash_capacity_bytes >= sizeof(std::uint64_t));
    REQUIRE(sparse_evidence.decoded_control_records > 1u);
    REQUIRE(sparse_evidence.query_route_control_lookups > 0u);
    REQUIRE(sparse_evidence.query_route_control_lookups <= sparse_evidence.decoded_control_records);
    REQUIRE(sparse_evidence.query_route_matches == 1u);
    REQUIRE(sparse_evidence.query_route_hash_probes <=
            sparse_evidence.query_route_control_lookups * 2u);
    REQUIRE(sparse_evidence.retained_selected_control_records ==
            sparse_evidence.decoded_control_records);
    REQUIRE(sparse_evidence.retained_selected_control_capacity_bytes ==
            sparse_evidence.decoded_control_records * sizeof(std::uint16_t) +
                sizeof(std::uint32_t) * 2u);
    REQUIRE(sparse_evidence.current_selected_control_capacity_bytes == 0u);
    REQUIRE(sparse_evidence.retained_layout_equation_holds());
    REQUIRE(sparse_evidence.owned_capacity_bytes == sparse_reader->admitted_bytes());
    REQUIRE(sparse_evidence.capacity_accounting_passes == expected_barrier_audits);
    REQUIRE_FALSE(sparse_evidence.saturated);

    // A selected query absent from the target still crosses the same global
    // controls/positions boundary, but exact P=0 must remain allocation-free.
    const std::array<std::uint64_t, 1> absent_queries{queries.front() ^ 1u};
    REQUIRE(std::ranges::find(queries, absent_queries.front()) == queries.end());
    auto empty_reader =
        prepared_grouped_soa_query_reader::begin(admission,
                                                 generation->head,
                                                 generation->manifest,
                                                 test_grouped_soa_expected_storage_contract(10u),
                                                 generation->directory.encoded_bytes(),
                                                 absent_queries,
                                                 10u);
    REQUIRE(empty_reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(empty_reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(empty_reader->finish().has_value());
    const auto empty_evidence = empty_reader->memory_stats();
    REQUIRE(empty_evidence.global_position_storage_preparations == 1u);
    REQUIRE(empty_evidence.global_position_storage_allocations == 0u);
    REQUIRE(empty_evidence.retained_position_count == 0u);
    REQUIRE(empty_evidence.retained_position_capacity == 0u);
    REQUIRE(empty_evidence.global_position_capacity_bytes == 0u);
    REQUIRE(empty_evidence.retained_layout_equation_holds());
    REQUIRE(empty_evidence.owned_capacity_bytes == empty_reader->admitted_bytes());
    REQUIRE(empty_reader->lookup(absent_queries.front()).positions.empty());

    constexpr std::uint64_t aggregate_sessions = 2u;
    const std::uint64_t aggregate_passes =
        evidence.capacity_accounting_passes + sparse_evidence.capacity_accounting_passes;
    REQUIRE(aggregate_passes == aggregate_sessions * expected_barrier_audits);
}

TEST_CASE("grouped-SoA admission failures name the exact owner substage",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(static_cast<std::uint8_t>(minimizer_generation_encoding_error::admission) ==
                   28u);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, 2u},
        {2u, 4u},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    const std::array<std::uint64_t, 2> queries{1u, 2u};
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0)
            payload.push_back(&row);
    }
    REQUIRE(payload.size() == 2u);

    using substage_type = grouped_soa_query_admission_stage;
    constexpr std::array begin_substages{
        substage_type::identity_and_directory_reservation,
        substage_type::query_hash_reservation,
        substage_type::query_hash_allocation,
        substage_type::query_hash_reconcile,
        substage_type::query_route_reservation,
        substage_type::query_route_allocation,
        substage_type::query_route_reconcile,
    };
    constexpr std::array payload_substages{
        substage_type::control_payload_reservation,
        substage_type::control_payload_allocation,
        substage_type::control_payload_reconcile,
        substage_type::control_decode_reservation,
        substage_type::control_decode_allocation,
        substage_type::control_decode_reconcile,
        substage_type::position_image_reservation,
        substage_type::position_image_allocation,
        substage_type::position_image_reconcile,
    };

    for (const substage_type wanted : begin_substages) {
        INFO("begin admission substage " << grouped_soa_query_admission_substage_name(wanted));
        bioseq::parallelism::memory_admission_controller admission;
        grouped_soa_query_admission_stats evidence;
        const auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &evidence,
            grouped_soa_query_reader_options{
                .fail_admission_substage = wanted,
            });
        require_wire_error(made, minimizer_generation_encoding_error::admission);
        REQUIRE(evidence.substage == wanted);
        REQUIRE(evidence.injected_failure);
        REQUIRE(grouped_soa_query_admission_substage_name(wanted) != std::string_view{"unknown"});
        if (wanted != substage_type::identity_and_directory_reservation) {
            REQUIRE(evidence.projected_bytes > 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    for (const substage_type wanted : payload_substages) {
        INFO("payload admission substage " << grouped_soa_query_admission_substage_name(wanted));
        bioseq::parallelism::memory_admission_controller admission;
        grouped_soa_query_admission_stats begin_evidence;
        auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &begin_evidence,
            grouped_soa_query_reader_options{
                .fail_admission_substage = wanted,
            });
        REQUIRE(made.has_value());
        REQUIRE(begin_evidence.substage == substage_type::none);
        bool rejected = false;
        for (const collected_minimizer_row* row : payload) {
            const auto accepted = made->accept(row_view(*row));
            if (!accepted) {
                REQUIRE(accepted.error() == minimizer_generation_encoding_error::admission);
                rejected = true;
                break;
            }
        }
        REQUIRE(rejected);
        const auto evidence = made->admission_failure_stats();
        REQUIRE(evidence.substage == wanted);
        REQUIRE(evidence.injected_failure);
        REQUIRE(evidence.projected_bytes > 0u);
        REQUIRE(evidence.query_record_count == queries.size());
        REQUIRE(evidence.query_record_capacity >= queries.size());
        REQUIRE(evidence.releasable_protocol_query_hash_bytes == 0u);
        if (wanted == substage_type::position_image_reservation ||
            wanted == substage_type::position_image_allocation ||
            wanted == substage_type::position_image_reconcile) {
            REQUIRE(evidence.retained_position_count == queries.size());
            REQUIRE(evidence.retained_position_bytes == queries.size() * sizeof(std::uint64_t));
            REQUIRE(evidence.allocation_headroom_bytes == 0u);
            if (wanted == substage_type::position_image_reconcile) {
                REQUIRE(evidence.projected_bytes == evidence.owned_capacity_bytes);
            } else {
                const std::uint64_t compact_final_bytes =
                    queries.size() * sizeof(std::uint64_t) + queries.size() * sizeof(std::uint8_t);
                REQUIRE(evidence.projected_bytes == evidence.owned_capacity_bytes +
                                                        evidence.retained_position_bytes +
                                                        compact_final_bytes);
            }
        }
        REQUIRE(made->failed());
        REQUIRE(made->admitted_bytes() == 0u);
        const auto released = made->memory_stats();
        REQUIRE(released.retained_position_count == 0u);
        REQUIRE(released.retained_position_capacity == 0u);
        REQUIRE(released.global_position_capacity_bytes == 0u);
        REQUIRE(released.retained_layout_equation_holds());
        REQUIRE(released.global_position_storage_allocations ==
                (wanted == substage_type::position_image_reconcile ? 1u : 0u));
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("real low budget retains the controller predicate snapshot") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(1u);
        grouped_soa_query_admission_stats evidence;
        const auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &evidence);
        require_wire_error(made, minimizer_generation_encoding_error::admission);
        REQUIRE(evidence.substage == substage_type::identity_and_directory_reservation);
        REQUIRE_FALSE(evidence.injected_failure);
        REQUIRE(evidence.controller_failure_snapshot_present);
        REQUIRE(evidence.controller_reason !=
                bioseq::parallelism::memory_admission_failure_reason::none);
        REQUIRE(evidence.requested_growth_bytes > 1u);
        REQUIRE(evidence.controller_budget_ceiling_bytes <= 1u);
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("arithmetic overflow is not relabelled as admission") {
        const auto ordinary = plan_grouped_soa_position_image(3u, 5u);
        REQUIRE(ordinary.has_value());
        REQUIRE(ordinary->retained_position_bytes == 24u);
        REQUIRE(ordinary->allocation_headroom_bytes == 0u);
        REQUIRE(ordinary->projected_bytes == 29u);
        constexpr std::uint64_t adversarial_count = 100'000'000u;
        const auto adversarial = plan_grouped_soa_position_image(adversarial_count, 17u);
        REQUIRE(adversarial.has_value());
        REQUIRE(adversarial->retained_position_bytes == 800'000'000u);
        REQUIRE(adversarial->allocation_headroom_bytes == 0u);
        REQUIRE(adversarial->projected_bytes == 800'000'017u);
        REQUIRE(adversarial->projected_bytes - 17u <
                adversarial->retained_position_bytes *
                    bioseq::parallelism::k_vector_preallocation_headroom_factor);
        constexpr std::uint64_t adversarial_query_count = 100'000'000u;
        constexpr std::uint64_t prior_query_owned_bytes =
            adversarial_query_count * (64u + 2u * sizeof(std::uint64_t));
        constexpr std::uint64_t exact_query_state_and_hash_bytes =
            adversarial_query_count * (sizeof(std::uint32_t) + sizeof(std::uint8_t));
        // ceil(Q / 4) needs 25 high radix bits; the remaining five key bits
        // fit exactly in one bucket-implied suffix byte.
        constexpr std::uint64_t exact_query_radix_bytes =
            ((1ULL << 25u) + 1u) * sizeof(std::uint32_t);
        constexpr std::uint64_t exact_query_owned_bytes =
            exact_query_state_and_hash_bytes + exact_query_radix_bytes;
        STATIC_REQUIRE(prior_query_owned_bytes == 8'000'000'000u);
        STATIC_REQUIRE(exact_query_state_and_hash_bytes == 500'000'000u);
        STATIC_REQUIRE(exact_query_radix_bytes == 134'217'732u);
        STATIC_REQUIRE(exact_query_owned_bytes == 634'217'732u);
        STATIC_REQUIRE(prior_query_owned_bytes - exact_query_owned_bytes == 7'365'782'268u);
        require_wire_error(
            plan_grouped_soa_position_image((std::numeric_limits<std::uint64_t>::max)(), 0u),
            minimizer_generation_encoding_error::arithmetic_overflow);
        require_wire_error(
            plan_grouped_soa_position_image(1u, (std::numeric_limits<std::uint64_t>::max)() - 1u),
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
}

TEST_CASE("grouped-SoA selects its exact immutable row quantum",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(minimizer_extent_payload_bytes(
                       minimizer_generation_layout::grouped_soa_segment_local) == 8064u);

    constexpr std::uint64_t position_count = 5000u;
    constexpr std::uint64_t first_control_count = 8064u / 4u;
    constexpr std::uint64_t queried_control_count = position_count - first_control_count - 1u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(position_count);
    for (std::uint64_t position = 0; position < position_count; ++position) {
        const std::uint64_t hash = position < first_control_count                           ? 6u
                                   : position < first_control_count + queried_control_count ? 7u
                                                                                            : 8u;
        minimizers.emplace_back(hash, position << 1u);
    }
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(position_count + 1u, 65'534u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest_row.byte_count == 160u);
    REQUIRE(generation->manifest.position_segment_span_bp == k_minimizer_preferred_segment_span_bp);
    REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u32);
    REQUIRE(generation->directory.entries().size() == 1u);
    const grouped_soa_bucket_directory_entry& entry = generation->directory.entries().front();
    REQUIRE(entry.position_encoded_bytes == position_count * 4u);
    REQUIRE(entry.position_chunk_count == 3u);
    REQUIRE(sink.grouped_soa_payload_write_calls ==
            static_cast<std::uint64_t>(entry.control_chunk_count) + entry.position_chunk_count);
    REQUIRE(sink.grouped_soa_row_crc_bytes == generation->manifest.payload_bytes);
    const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
    REQUIRE(position_sub_key.has_value());
    const auto first_position =
        std::ranges::find_if(sink.rows, [position_sub_key](const collected_minimizer_row& row) {
            return row.address.sub_key == *position_sub_key && row.address.chunk_no == 0u;
        });
    REQUIRE(first_position != sink.rows.end());
    REQUIRE(first_position->bytes.size() == 8064u);
    REQUIRE(sink.max_callback_bytes == 8064u);
    require_complete_generation_rows(*generation, sink.rows);

    // The first unqueried control ends exactly at the first position-row
    // boundary. Querying only the middle of three controls forces the sparse
    // route image and exercises its late ordinal on the following row.
    bioseq::parallelism::memory_admission_controller admission;
    const std::array<std::uint64_t, 1> query{7u};
    auto reader = prepared_grouped_soa_query_reader::begin(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(65'534u),
        generation->directory.encoded_bytes(),
        query,
        65'534u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());
    const auto lookup = reader->lookup(7u);
    REQUIRE(lookup.positions.size() == queried_control_count);
    REQUIRE(lookup.positions.front() == first_control_count << 1u);
    REQUIRE(lookup.positions.back() == (first_control_count + queried_control_count - 1u) << 1u);
    const auto evidence = reader->memory_stats();
    REQUIRE(evidence.decoded_control_records == 3u);
    REQUIRE(evidence.retained_selected_control_capacity_bytes ==
            3u * sizeof(std::uint16_t) + 2u * sizeof(std::uint32_t));
}

TEST_CASE("grouped-SoA narrows positions only with a narrow immutable span",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t narrow_span = std::uint64_t{1} << 15u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {7u, 0u},
        {8u, narrow_span << 1u},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    grouped_soa_generation_limits one_control_per_segment;
    one_control_per_segment.max_segment_control_count = 1u;

    const auto generation =
        encode_minimizer_generation(producer,
                                    sink,
                                    test_grouped_soa_generation_options(narrow_span + 1u, 65'534u),
                                    one_control_per_segment);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_span_bp == narrow_span);
    REQUIRE(generation->manifest.position_segment_count == 2u);
    REQUIRE(generation->manifest.gpu_abi_revision == k_grouped_soa_gpu_abi_revision);
    REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u16);

    const auto canonical_profile =
        derive_grouped_soa_minimizer_profile(k_minimap_scheme_identity,
                                             generation->manifest.alphabet_code,
                                             generation->manifest.kmer_size,
                                             generation->manifest.position_segment_span_bp,
                                             generation->manifest.max_occ_cap);
    REQUIRE(canonical_profile.has_value());
    REQUIRE(generation->directory.profile() == *canonical_profile);
    const auto manifest_profile = derive_grouped_soa_minimizer_profile(generation->manifest);
    REQUIRE(manifest_profile.has_value());
    REQUIRE(*manifest_profile == *canonical_profile);

    REQUIRE(generation->directory.entries().size() == 2u);
    std::uint64_t position_count = 0u;
    std::uint64_t position_bytes = 0u;
    for (const grouped_soa_bucket_directory_entry& entry : generation->directory.entries()) {
        REQUIRE(entry.position_element_count == 1u);
        REQUIRE(entry.position_encoded_bytes == 2u);
        REQUIRE(entry.position_chunk_count == 1u);
        position_count += entry.position_element_count;
        position_bytes += entry.position_encoded_bytes;
    }
    REQUIRE(position_count == generation->manifest.position_count);
    REQUIRE(position_bytes == generation->manifest.position_count * 2u);
    REQUIRE(sink.grouped_soa_row_crc_bytes == generation->manifest.payload_bytes);
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("grouped-SoA authenticates global controls then global positions",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = k_minimizer_preferred_segment_span_bp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {42u, 2u},
        {42u, 4u},
        {42u, ((span + 1u) << 1u)},
        {42u, ((span + 2u) << 1u)},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(span + 100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 2u);
    for (std::size_t ordinal = 0; ordinal < generation->directory.entries().size(); ++ordinal) {
        const auto& entry = generation->directory.entries()[ordinal];
        REQUIRE(entry.partition_id == ordinal);
        REQUIRE(entry.sub_key == static_cast<index_sub_key_t>(ordinal));
        REQUIRE(entry.control_chunk_count == 1u);
        REQUIRE(entry.position_chunk_count == 1u);
    }
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0)
            payload.push_back(&row);
    }
    REQUIRE(payload.size() == 4u);
    REQUIRE(payload[0]->address.position_segment == 0);
    REQUIRE(payload[0]->address.sub_key == 0);
    REQUIRE(payload[1]->address.position_segment == 1);
    REQUIRE(payload[1]->address.sub_key == 1);
    REQUIRE(payload[2]->address.position_segment == 0);
    REQUIRE(payload[2]->address.sub_key == *derive_minimizer_position_extent_sub_key(0));
    REQUIRE(payload[3]->address.position_segment == 1);
    REQUIRE(payload[3]->address.sub_key == *derive_minimizer_position_extent_sub_key(1));
    require_complete_generation_rows(*generation, sink.rows);

    auto interleaved = make_generation_row_validator(*generation);
    REQUIRE(interleaved.accept(row_view(*payload[0])).has_value());
    require_wire_error(interleaved.accept(row_view(*payload[2])),
                       minimizer_generation_encoding_error::row_order);

    const std::array<std::uint64_t, 1> query{42u};
    SECTION("max-occ is target-global across both segments") {
        bioseq::parallelism::memory_admission_controller admission;
        auto pruned = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            query,
            3u);
        REQUIRE(pruned.has_value());
        for (const collected_minimizer_row* row : payload) {
            REQUIRE(pruned->accept(row_view(*row)).has_value());
        }
        REQUIRE(pruned->finish().has_value());
        const auto found = pruned->lookup(42u);
        REQUIRE(found.global_occurrence_count == 4u);
        REQUIRE_FALSE(found.positions_complete);
        REQUIRE(found.positions.empty());
    }
    SECTION("boundary max-occ retains all cross-segment positions") {
        bioseq::parallelism::memory_admission_controller admission;
        auto retained = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            query,
            4u);
        REQUIRE(retained.has_value());
        for (const collected_minimizer_row* row : payload) {
            REQUIRE(retained->accept(row_view(*row)).has_value());
        }
        REQUIRE(retained->finish().has_value());
        const auto found = retained->lookup(42u);
        REQUIRE(found.global_occurrence_count == 4u);
        REQUIRE(found.positions_complete);
        REQUIRE((std::vector<std::uint64_t>{found.positions.begin(), found.positions.end()} ==
                 std::vector<std::uint64_t>{2u, 4u, (span + 1u) << 1u, (span + 2u) << 1u}));
    }

    auto invalid_entry = generation->directory.entries().front();
    invalid_entry.sub_key = 7;
    require_wire_error(
        encode_grouped_soa_bucket_directory_entry(invalid_entry, generation->directory.profile()),
        minimizer_generation_encoding_error::sub_key);
}

TEST_CASE("grouped-SoA reader releases admission on every terminal path",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, 2u},
        {1u, 4u},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0)
            payload.push_back(&row);
    }
    REQUIRE(payload.size() == 2u);
    const std::array<std::uint64_t, 1> query{1u};
    const auto begin = [&generation, &query](auto& admission) {
        return prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            query,
            10u);
    };

    SECTION("trusted receipt is move-only and grouped-SoA requires authenticated row CRC") {
        bioseq::parallelism::memory_admission_controller admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            auto first_view = row_view(*payload.front());
            auto bad_first_view = first_view;
            bad_first_view.row_checksum ^= 1u;
            require_wire_error(validate_minimizer_generation_row_receipt(bad_first_view),
                               minimizer_generation_encoding_error::checksum);
            auto validated_first = validate_minimizer_generation_row_receipt(first_view);
            REQUIRE(validated_first.has_value());
            auto first = std::move(*validated_first);
            REQUIRE(first.has_valid_authority());
            first_view.row_checksum ^= 1u;
            auto moved = std::move(first);
            REQUIRE_FALSE(first.has_valid_authority());
            REQUIRE(moved.has_valid_authority());
            REQUIRE(made->accept(moved).has_value());
            auto second = trusted_row_receipt(*payload.back());
            REQUIRE(made->accept(second).has_value());
            REQUIRE(made->finish().has_value());
            REQUIRE(made->lookup(1u).positions.size() == 2u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("grouped-SoA trusted wrong address still poisons and releases") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto wrong = *payload.front();
        ++wrong.address.sub_key;
        recompute_row_checksum(wrong);
        auto receipt = trusted_row_receipt(wrong);
        require_wire_error(made->accept(receipt), minimizer_generation_encoding_error::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("ordinary grouped-SoA row with wrong row CRC remains rejected") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto untrusted = row_view(*payload.front());
        untrusted.row_checksum ^= 1u;
        require_wire_error(made->accept(untrusted), minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("moved-from receipt fails runtime authority validation") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto source = trusted_row_receipt(*payload.front());
        auto owner = std::move(source);
        REQUIRE(owner.has_valid_authority());
        REQUIRE_FALSE(source.has_valid_authority());
        require_wire_error(made->accept(source), minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("grouped-SoA trusted wrong order still poisons and releases") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto receipt = trusted_row_receipt(*payload.back());
        require_wire_error(made->accept(receipt), minimizer_generation_encoding_error::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("grouped-SoA trusted row cannot bypass extent CRC") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto corrupt = *payload.front();
        corrupt.bytes.front() ^= std::byte{1};
        recompute_row_checksum(corrupt);
        auto receipt = trusted_row_receipt(corrupt);
        require_wire_error(made->accept(receipt), minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move transfers the sole permit owner") {
        bioseq::parallelism::memory_admission_controller admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            auto source = std::move(*made);
            const std::uint64_t reserved = admission.reserved_bytes();
            REQUIRE(reserved > 0u);
            prepared_grouped_soa_query_reader destination{std::move(source)};
            REQUIRE(admission.reserved_bytes() == reserved);
            for (const collected_minimizer_row* row : payload) {
                REQUIRE(destination.accept(row_view(*row)).has_value());
            }
            REQUIRE(destination.finish().has_value());
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move after exact position allocation transfers array ownership") {
        bioseq::parallelism::memory_admission_controller admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            REQUIRE(made->accept(row_view(*payload.front())).has_value());
            const auto before_move = made->memory_stats();
            REQUIRE(before_move.global_position_storage_preparations == 1u);
            REQUIRE(before_move.global_position_storage_allocations == 1u);
            REQUIRE(before_move.retained_position_count == 2u);
            REQUIRE(before_move.retained_position_capacity == 2u);
            REQUIRE(before_move.global_position_capacity_bytes == 2u * sizeof(std::uint64_t));
            REQUIRE(before_move.owned_capacity_bytes == made->admitted_bytes());
            const std::uint64_t reserved = admission.reserved_bytes();

            prepared_grouped_soa_query_reader destination{std::move(*made)};
            REQUIRE(made->failed());
            REQUIRE(made->admitted_bytes() == 0u);
            REQUIRE(made->memory_stats().retained_position_count == 0u);
            REQUIRE(made->memory_stats().retained_position_capacity == 0u);
            REQUIRE(admission.reserved_bytes() == reserved);
            REQUIRE(destination.memory_stats().retained_position_count == 2u);
            REQUIRE(destination.accept(row_view(*payload.back())).has_value());
            REQUIRE(destination.finish().has_value());
            const auto found = destination.lookup(1u);
            REQUIRE(found.positions.size() == 2u);
            REQUIRE(found.positions[0] == 2u);
            REQUIRE(found.positions[1] == 4u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("incomplete finish poisons and releases immediately") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        require_wire_error(made->finish(), minimizer_generation_encoding_error::row_count);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("row corruption poisons and releases immediately") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto corrupt = *payload.front();
        corrupt.bytes.front() ^= std::byte{1};
        require_wire_error(made->accept(row_view(corrupt)),
                           minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("post-begin allocation denial rolls back and releases") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        admission.set_index_memory_cap_bytes(admission.reserved_bytes());
        require_wire_error(made->accept(row_view(*payload.front())),
                           minimizer_generation_encoding_error::admission);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
