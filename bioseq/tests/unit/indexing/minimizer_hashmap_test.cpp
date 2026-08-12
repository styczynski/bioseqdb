// bioseq/tests/unit/indexing/minimizer_hashmap_test.cpp
//
// minimizer_hashmap_fragment_index (the storage-free pipeline IndexT)
// + the storage-bound minimizers_per_fragment_index<S> builder/assembler.
//
// This file reaches collect_minimizers_view / minimap_sketch_data / minimap_int_hasher
// / sketch_collector, so its body lives inside bioseq::simd::HWY_NAMESPACE and
// reaches those symbols UNQUALIFIED — exactly like lazy_minimizer_parity_test.cpp.
// The bioseq::indexing::* types are named fully-qualified.

#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
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
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/mman.h>

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
import bioseq.common.hashing;                        //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using bioseq::storage::chunked_span_sequence_scanner;
using bioseq::storage::lazy_sequence;
using bioseq::indexing::test::fake_fragment_storage;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;   // 32 letters / uint64 unit
using range_t  = compact_sequence_range<dna4_u64>;
using dna4_u16 = sequence_custom_alphabet<seq::dna4, std::uint16_t>;
using dna4_u16_range_t = compact_sequence_range<dna4_u16>;
using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
using dna5_range_t = compact_sequence_range<dna5_u64>;

[[nodiscard]] const ::bioseq::minimizers::minimizer_kernels*
fixed_minimizer_kernels() {
    return &::bioseq::simd_dispatch::highway_test_kernel_table().minimizers;
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint8_t fixed_alphabet_code() {
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity = table.is_complete()
        ? table.template resolve_alphabet_identity<p_alphabet>() : nullptr;
    if (identity == nullptr
        || table.resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "generated test table has no requested alphabet identity"};
    }
    return identity->alphabet_code;
}

template <class p_range>
[[nodiscard]] ::bioseq::minimizers::packed_sequence_kernel_view
fixed_packed_sequence_view(const p_range& range) {
    using traits = typename p_range::traits;
    return {
        .storage = reinterpret_cast<const std::byte*>(
            range.read_cptr(range.cbegin())),
        .storage_units = range.size(),
        .begin_symbol = 0u,
        .end_symbol = range.bp_size(),
        .alphabet_code = fixed_alphabet_code<
            typename traits::alphabet_type>(),
        .bits_per_symbol = static_cast<std::uint8_t>(
            traits::bits_per_char),
        .storage_unit_bytes = static_cast<std::uint8_t>(
            sizeof(typename traits::storage_unit)),
        .final_segment = true,
    };
}

[[nodiscard]] const ::bioseq::minimizers::minimizer_kernels*
minimizer_kernels_for_backend(const minimizer_sketch_backend backend) {
    return backend == minimizer_sketch_backend::exact_simd
            || backend == minimizer_sketch_backend::bounded_nthash_simd
        ? fixed_minimizer_kernels()
        : nullptr;
}

inline constexpr auto k_test_grouped_soa_profile_result =
    bioseq::indexing::derive_grouped_soa_minimizer_profile(
        bioseq::indexing::k_minimap_scheme_identity,
        bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
        15u,
        32'768u,
        65'534u);
static_assert(k_test_grouped_soa_profile_result.has_value());
inline constexpr auto k_test_grouped_soa_profile =
    *k_test_grouped_soa_profile_result;
inline constexpr auto k_test_bounded_grouped_soa_profile_result =
    bioseq::indexing::derive_grouped_soa_minimizer_profile(
        bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_identity,
        bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
        15u,
        32'768u,
        65'534u);
static_assert(k_test_bounded_grouped_soa_profile_result.has_value());
inline constexpr auto k_test_bounded_grouped_soa_profile =
    *k_test_bounded_grouped_soa_profile_result;

constexpr void assign_grouped_soa_profile(
        bioseq::indexing::minimizer_generation_manifest& manifest,
        const bioseq::indexing::grouped_soa_minimizer_profile& profile) {
    manifest.layout = profile.layout;
    manifest.alphabet_code = profile.alphabet_code;
    manifest.kmer_size = profile.kmer_size;
    manifest.gpu_abi_revision = profile.gpu_abi_revision;
    manifest.minimizer_scheme_id = profile.minimizer_scheme.id;
    manifest.minimizer_scheme_revision = profile.minimizer_scheme.revision;
    manifest.hash_width = profile.hash_width;
    manifest.position_width = profile.position_width;
    manifest.count_width = profile.count_width;
    manifest.offset_policy = profile.offset_policy;
    manifest.control_layout = profile.control_layout;
    manifest.count_semantics = profile.count_semantics;
    manifest.position_semantics = profile.position_semantics;
    manifest.max_hash = profile.max_hash;
    manifest.max_occ_cap = profile.max_occ_cap;
}

static_assert(std::is_standard_layout_v<
    bioseq::indexing::exact_simd_index_packed_fragment>);
static_assert(std::is_trivially_copyable_v<
    bioseq::indexing::exact_simd_index_packed_fragment>);
static_assert(std::is_standard_layout_v<
    bioseq::indexing::exact_simd_index_bridge_request>);
static_assert(std::is_trivially_copyable_v<
    bioseq::indexing::exact_simd_index_bridge_request>);
static_assert(std::is_standard_layout_v<
    bioseq::indexing::exact_simd_index_bridge_result>);
static_assert(std::is_same_v<
    decltype(bioseq::indexing::collect_exact_simd_index_erased(
        std::declval<const bioseq::indexing::exact_simd_index_bridge_request&>())),
    bioseq::indexing::exact_simd_index_bridge_result>);
static_assert(std::is_standard_layout_v<
    bioseq::indexing::packed_bounded_nthash_index_bridge_request>);
static_assert(std::is_trivially_copyable_v<
    bioseq::indexing::packed_bounded_nthash_index_bridge_request>);
static_assert(std::is_standard_layout_v<
    bioseq::indexing::packed_bounded_nthash_index_bridge_result>);
static_assert(std::is_same_v<
    decltype(bioseq::indexing::collect_packed_bounded_nthash_index_erased(
        std::declval<const
            bioseq::indexing::packed_bounded_nthash_index_bridge_request&>())),
    bioseq::indexing::packed_bounded_nthash_index_bridge_result>);

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::vector<seq::dna4> v(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>((s >> 33) & 0x3));
        v[i] = c;
    }
    return v;
}

[[nodiscard]] std::vector<bioseq::indexing::canonical_minimizer_event>
baseline_dna5_events(
        const std::vector<seq::dna5>& data,
        std::size_t window_size,
        std::size_t kmer_size) {
    std::string bases;
    bases.reserve(data.size());
    for (const seq::dna5 symbol : data) bases.push_back(symbol.to_char());
    const auto entries =
        ::bioseq::minimap2_sketch_baseline_detail::collect_minimap2_sketch(
        bases.data(),
        static_cast<int>(bases.size()),
        static_cast<int>(window_size),
        static_cast<int>(kmer_size));
    std::vector<bioseq::indexing::canonical_minimizer_event> events;
    events.reserve(entries.size());
    for (const auto& entry : entries) {
        events.push_back({
            .hash = entry.x >> 8,
            .encoded_position = entry.y & std::uint64_t{0xFFFFFFFFu},
        });
    }
    return events;
}

// (kmer -> sorted encoded positions) view of an index over an explicit kmer set.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
lookup_map(const bioseq::indexing::minimizer_hashmap_fragment_index& idx,
           const std::vector<std::uint64_t>& kmers) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (std::uint64_t kmer : kmers) {
        if (out.count(kmer)) continue;
        auto span = idx.lookup(kmer);
        std::vector<std::uint64_t> v(span.begin(), span.end());
        std::sort(v.begin(), v.end());
        out.emplace(kmer, std::move(v));
    }
    return out;
}

[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
canonical_event_lookup_map(
        std::span<const bioseq::indexing::canonical_minimizer_event> events) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> lookup;
    for (const auto& event : events) {
        lookup[event.hash].push_back(event.encoded_position);
    }
    for (auto& [hash, positions] : lookup) {
        (void)hash;
        std::ranges::sort(positions);
        const auto unique_end = std::ranges::unique(positions).begin();
        positions.erase(unique_end, positions.end());
    }
    return lookup;
}

// Whole-sequence minimizers, globalised the same way the builders do (collect
// over the entire range, take last_pos as the global position).
template <class p_range>
[[nodiscard]] std::vector<minimap_sketch_data>
whole_sequence_minimizers(p_range& range, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        collector, range, range.begin(), range.end(), w, k);
    return collector.extract_sketches_data();
}

template <class p_uint, std::size_t p_size>
[[nodiscard]] constexpr p_uint test_read_le(
        const std::array<std::byte, p_size>& bytes,
        std::size_t offset) noexcept {
    static_assert(std::is_unsigned_v<p_uint>);
    p_uint value = 0;
    for (std::size_t i = 0; i < sizeof(p_uint); ++i) {
        value |= static_cast<p_uint>(std::to_integer<std::uint8_t>(bytes[offset + i]))
            << (i * 8u);
    }
    return value;
}

template <class p_uint>
[[nodiscard]] constexpr p_uint test_read_le(
        std::span<const std::byte> bytes,
        std::size_t offset) noexcept {
    static_assert(std::is_unsigned_v<p_uint>);
    p_uint value = 0;
    for (std::size_t i = 0; i < sizeof(p_uint); ++i) {
        value |= static_cast<p_uint>(std::to_integer<std::uint8_t>(bytes[offset + i]))
            << (i * 8u);
    }
    return value;
}

template <class p_uint, std::size_t p_size>
constexpr void test_write_le(
        std::array<std::byte, p_size>& bytes,
        std::size_t offset,
        p_uint value) noexcept {
    static_assert(std::is_unsigned_v<p_uint>);
    for (std::size_t i = 0; i < sizeof(p_uint); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & p_uint{0xff});
        value >>= 8u;
    }
}

template <class p_uint>
constexpr void test_write_le(
        std::span<std::byte> bytes,
        std::size_t offset,
        p_uint value) noexcept {
    static_assert(std::is_unsigned_v<p_uint>);
    for (std::size_t i = 0; i < sizeof(p_uint); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & p_uint{0xff});
        value >>= 8u;
    }
}

template <class p_t>
void require_wire_error(
        const std::expected<p_t, bioseq::indexing::minimizer_generation_encoding_error>& value,
        bioseq::indexing::minimizer_generation_encoding_error expected) {
    REQUIRE_FALSE(value.has_value());
    REQUIRE(value.error() == expected);
}

template <class p_t>
void require_wire_value(
        const std::expected<p_t, bioseq::indexing::minimizer_generation_encoding_error>& value,
        const p_t& expected) {
    REQUIRE(value.has_value());
    REQUIRE(*value == expected);
}

[[nodiscard]] constexpr bioseq::indexing::minimizer_generation_head
golden_generation_head() noexcept {
    return {
        .layout = bioseq::indexing::minimizer_generation_layout::
            grouped_soa_segment_local,
        .generation_id = 0x0102030405060708ULL,
        .source_identity = 0x1112131415161718ULL,
        .storage_contract_checksum = 0x2122232425262728ULL,
        .manifest_checksum = 0x3132333435363738ULL,
        .source_content_checksum = 0x4142434445464748ULL,
    };
}

[[nodiscard]] constexpr bioseq::indexing::minimizer_generation_manifest
golden_generation_manifest() noexcept {
    auto manifest = bioseq::indexing::minimizer_generation_manifest{
        .layout = bioseq::indexing::minimizer_generation_layout::
            grouped_soa_segment_local,
        .generation_id = 0x0102030405060708ULL,
        .source_identity = 0x1112131415161718ULL,
        .source_content_checksum = 0x2122232425262728ULL,
        .target_length = 20,
        .control_count = 0x3132333435363738ULL,
        .position_count = 0x4142434445464748ULL,
        .payload_bytes = 0x5152535455565758ULL,
        .payload_checksum = 0x6162636465666768ULL,
        .directory_bytes = 0x7172737475767778ULL,
        .directory_checksum = 0x090A0B0C0D0E0F10ULL,
        .position_segment_count = 3,
        .extent_count = 0x11223344U,
        .directory_chunk_count = 0x55667788U,
        .alphabet_code = 0x1122U,
        .storage_unit_byte_width = 0x3344U,
        .kmer_size = 0x5566U,
        .window_size = 0x7788U,
        .gpu_abi_revision = 0x99AABBCCU,
        .position_segment_span_bp = 8,
        .segment_geometry_revision =
            bioseq::indexing::minimizer_generation_manifest::uniform_segment_geometry_revision,
        .producer_backend_id = bioseq::indexing::
            minimizer_producer_backend::scalar,
        .producer_identity_revision = bioseq::indexing::
            k_minimizer_producer_identity_revision,
    };
    assign_grouped_soa_profile(manifest, k_test_grouped_soa_profile);
    return manifest;
}

[[nodiscard]] constexpr bioseq::indexing::grouped_soa_generation_options
test_generation_options(std::uint64_t target_length) noexcept {
    return {
        .layout = bioseq::indexing::minimizer_generation_layout::
            grouped_soa_segment_local,
        .generation_id = 37,
        .source_identity = 0x1122334455667788ULL,
        .source_content_checksum = 0x8877665544332211ULL,
        .target_length = target_length,
        .alphabet_code =
            bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
        .storage_unit_byte_width = 8,
        .kmer_size = 15,
        .window_size = 10,
        .minimizer_scheme_id =
            bioseq::indexing::k_minimap_scheme_id,
        .minimizer_scheme_revision =
            bioseq::indexing::k_minimap_scheme_revision,
    };
}

[[nodiscard]] constexpr bioseq::indexing::grouped_soa_generation_options
test_grouped_soa_generation_options(
        std::uint64_t target_length,
        std::uint64_t max_occ_cap = 65'534u) noexcept {
    auto options = test_generation_options(target_length);
    options.layout = bioseq::indexing::minimizer_generation_layout::
        grouped_soa_segment_local;
    options.alphabet_code =
        bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code;
    options.max_occ_cap = max_occ_cap;
    return options;
}

[[nodiscard]] constexpr bioseq::indexing::expected_minimizer_generation_identity
test_expected_storage_contract() noexcept {
    return {
        .layout = bioseq::indexing::minimizer_generation_layout::
            grouped_soa_segment_local,
        .alphabet_code =
            bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
        .storage_unit_byte_width = 8,
        .kmer_size = 15,
        .window_size = 10,
        .gpu_abi_revision =
            bioseq::indexing::k_grouped_soa_gpu_abi_revision,
        .minimizer_scheme_id =
            bioseq::indexing::k_minimap_scheme_id,
        .minimizer_scheme_revision =
            bioseq::indexing::k_minimap_scheme_revision,
        .producer_backend_id = bioseq::indexing::
            minimizer_producer_backend::scalar,
        .producer_identity_revision = bioseq::indexing::
            k_minimizer_producer_identity_revision,
        .max_occ_cap = k_test_grouped_soa_profile.max_occ_cap,
    };
}

[[nodiscard]] constexpr bioseq::indexing::expected_minimizer_generation_identity
test_grouped_soa_expected_storage_contract(
        std::uint64_t max_occ_cap = 65'534u) noexcept {
    auto expected = test_expected_storage_contract();
    expected.layout = bioseq::indexing::minimizer_generation_layout::
        grouped_soa_segment_local;
    expected.alphabet_code =
        bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code;
    expected.gpu_abi_revision =
        bioseq::indexing::k_grouped_soa_gpu_abi_revision;
    expected.max_occ_cap = max_occ_cap;
    return expected;
}

struct collected_minimizer_row {
    bioseq::indexing::index_chunk_address address{};
    std::vector<std::byte> bytes;
    std::uint64_t row_checksum = 0;
};

struct collecting_minimizer_row_sink {
    std::vector<collected_minimizer_row> rows;
    std::size_t max_callback_bytes = 0;
    std::uint64_t grouped_soa_payload_write_calls = 0;
    std::uint64_t grouped_soa_row_crc_bytes = 0;

    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    write(const bioseq::indexing::minimizer_generation_row_view& row) {
        max_callback_bytes = std::max(max_callback_bytes, row.bytes.size());
        rows.push_back(collected_minimizer_row{
            .address = row.address,
            .bytes = std::vector<std::byte>(row.bytes.begin(), row.bytes.end()),
            .row_checksum = row.row_checksum,
        });
        return {};
    }

    [[nodiscard]] std::expected<
        bioseq::indexing::minimizer_generation_row_write_receipt,
        bioseq::indexing::minimizer_generation_encoding_error>
    write_grouped_soa_payload(
            const bioseq::indexing::
                unchecked_minimizer_generation_row_view& row) {
        const std::uint64_t row_checksum =
            bioseq::indexing::crc64_index_chunk_row(
                row.address.generation_id,
                row.address.position_segment,
                row.address.sub_key,
                row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()),
                row.bytes);
        if (auto written = write(
                bioseq::indexing::minimizer_generation_row_view{
                    .address = row.address,
                    .bytes = row.bytes,
                    .row_checksum = row_checksum,
                }); !written) {
            return std::unexpected(written.error());
        }
        ++grouped_soa_payload_write_calls;
        grouped_soa_row_crc_bytes += row.bytes.size();
        return bioseq::indexing::minimizer_generation_row_write_receipt{
            .row_checksum = row_checksum,
        };
    }
};

/// Row-only baseline for the relation/mock sink API without the optional
/// planned and direct-payload callbacks.
struct row_only_collecting_minimizer_sink {
    std::vector<collected_minimizer_row> rows;

    [[nodiscard]] std::expected<
        void,
        bioseq::indexing::minimizer_generation_encoding_error>
    write(const bioseq::indexing::minimizer_generation_row_view& row) {
        rows.push_back(collected_minimizer_row{
            .address = row.address,
            .bytes = std::vector<std::byte>(
                row.bytes.begin(), row.bytes.end()),
            .row_checksum = row.row_checksum,
        });
        return {};
    }
};

enum class planned_direct_sink_event : std::uint8_t {
    begin,
    payload,
    row,
};

/// Test double for mapped storage. Payload callbacks never delegate to
/// write(), which is the callback that would form ordinary relation tuples.
struct planned_direct_minimizer_sink {
    std::optional<bioseq::indexing::grouped_soa_storage_plan_view>
        storage_plan;
    std::vector<collected_minimizer_row> payload_rows;
    std::vector<collected_minimizer_row> ordinary_rows;
    std::vector<planned_direct_sink_event> events;
    std::uint64_t ordinary_payload_row_callbacks = 0u;

    [[nodiscard]] std::expected<
        void,
        bioseq::indexing::minimizer_generation_encoding_error>
    begin_planned_generation(
            const bioseq::indexing::
                grouped_soa_storage_plan_view& plan) {
        if (storage_plan.has_value() || !events.empty()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::row_order);
        }
        storage_plan = plan;
        events.push_back(planned_direct_sink_event::begin);
        return {};
    }

    [[nodiscard]] std::expected<
        void,
        bioseq::indexing::minimizer_generation_encoding_error>
    write(const bioseq::indexing::minimizer_generation_row_view& row) {
        if (!storage_plan.has_value()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::row_order);
        }
        if (row.address.position_segment >= 0) {
            ++ordinary_payload_row_callbacks;
        }
        ordinary_rows.push_back(collected_minimizer_row{
            .address = row.address,
            .bytes = std::vector<std::byte>(
                row.bytes.begin(), row.bytes.end()),
            .row_checksum = row.row_checksum,
        });
        events.push_back(planned_direct_sink_event::row);
        return {};
    }

    [[nodiscard]] std::expected<
        bioseq::indexing::minimizer_generation_row_write_receipt,
        bioseq::indexing::minimizer_generation_encoding_error>
    write_grouped_soa_payload(
            const bioseq::indexing::
                unchecked_minimizer_generation_row_view& row) {
        if (!storage_plan.has_value()
            || row.address.generation_id != storage_plan->generation_id
            || row.address.position_segment < 0) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::row_order);
        }
        const std::uint64_t row_checksum =
            bioseq::indexing::crc64_index_chunk_row(
                row.address.generation_id,
                row.address.position_segment,
                row.address.sub_key,
                row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()),
                row.bytes);
        payload_rows.push_back(collected_minimizer_row{
            .address = row.address,
            .bytes = std::vector<std::byte>(
                row.bytes.begin(), row.bytes.end()),
            .row_checksum = row_checksum,
        });
        events.push_back(planned_direct_sink_event::payload);
        return bioseq::indexing::minimizer_generation_row_write_receipt{
            .row_checksum = row_checksum,
        };
    }
};

struct vector_replay_minimizer_producer {
    std::vector<bioseq::indexing::canonical_minimizer_event> events;
    std::size_t scan_count = 0;
    bioseq::indexing::minimizer_scheme_identity scheme =
        bioseq::indexing::k_minimap_scheme_identity;
    bioseq::indexing::minimizer_producer_identity producer_backend =
        bioseq::indexing::minimizer_producer_backend_identity_for_backend(
            bioseq::simd::minimizer_sketch_backend::scalar);

    explicit vector_replay_minimizer_producer(
            std::span<const std::pair<std::uint64_t, std::uint64_t>> pairs,
            bioseq::indexing::minimizer_scheme_identity scheme_identity =
                bioseq::indexing::k_minimap_scheme_identity,
            bioseq::indexing::minimizer_producer_identity
                producer_backend_identity = {})
        : scheme(scheme_identity),
          producer_backend(
              bioseq::indexing::
                  is_supported_minimizer_producer_backend_identity(
                      producer_backend_identity)
                  ? producer_backend_identity
                  : bioseq::indexing::
                      minimizer_producer_backend_identity_for_backend(
                          scheme_identity == bioseq::indexing::
                                  k_bounded_nthash_u32_r7_sum_scheme_identity
                              ? bioseq::simd::
                                  minimizer_sketch_backend::bounded_nthash_simd
                              : bioseq::simd::
                                  minimizer_sketch_backend::scalar)) {
        events.reserve(pairs.size());
        for (const auto& pair : pairs) {
            events.push_back({pair.first, pair.second});
        }
    }

    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity
    scheme_identity() const noexcept {
        return scheme;
    }

    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return producer_backend;
    }

    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    scan_sorted_events(
            std::uint64_t span,
            bioseq::indexing::canonical_minimizer_event_receiver receiver) {
        ++scan_count;
        if (span == 0) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::segment_span);
        }
        std::vector<bioseq::indexing::canonical_minimizer_event> ordered = events;
        std::sort(ordered.begin(), ordered.end(),
            [span](const auto& lhs, const auto& rhs) {
                const std::uint64_t lhs_segment =
                    (lhs.encoded_position >> 1u) / span;
                const std::uint64_t rhs_segment =
                    (rhs.encoded_position >> 1u) / span;
                if (lhs_segment != rhs_segment) return lhs_segment < rhs_segment;
                if (lhs.hash != rhs.hash) return lhs.hash < rhs.hash;
                return lhs.encoded_position < rhs.encoded_position;
            });
        for (const auto& event : ordered) {
            if (auto accepted = receiver(event); !accepted) {
                return accepted;
            }
        }
        return {};
    }
};

struct fragment_compact_direct_run {
    bioseq::indexing::compact_hash32_sorted_run_header header{};
    std::vector<std::uint64_t> encoded_positions{};
};

struct fragment_compact_direct_source {
    std::vector<fragment_compact_direct_run> runs{};
};

[[nodiscard]] std::expected<void,
    bioseq::indexing::direct_inverted_index_error>
scan_fragment_compact_direct_runs(
        void* opaque,
        const bioseq::indexing::compact_hash32_sorted_run_receiver& receiver) {
    const auto& source = *static_cast<
        const fragment_compact_direct_source*>(opaque);
    for (const fragment_compact_direct_run& run : source.runs) {
        if (auto begun = receiver.begin_run(run.header); !begun) {
            return std::unexpected(begun.error());
        }
        if (auto consumed = receiver.consume_positions(
                std::span<const std::uint64_t>{
                    run.encoded_positions}); !consumed) {
            return std::unexpected(consumed.error());
        }
    }
    return {};
}

struct fragment_compact_direct_test_index {
    std::shared_ptr<std::vector<std::byte>> image{};
    bioseq::indexing::compact_hash32_direct_index_view view{};
};

[[nodiscard]] fragment_compact_direct_test_index
make_fragment_compact_direct_test_index(
        const bioseq::indexing::minimizer_generation_head& head,
        const bioseq::indexing::minimizer_generation_manifest& manifest,
        std::span<const std::pair<std::uint64_t, std::uint64_t>> pairs,
        std::uint32_t max_occ_cap) {
    using namespace bioseq::indexing;
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    REQUIRE(profile.has_value());
    REQUIRE(profile->max_hash
        <= (std::numeric_limits<std::uint32_t>::max)());
    const std::uint32_t geometry_max_hash =
        static_cast<std::uint32_t>(profile->max_hash);
    const std::uint8_t hash_domain_bits = static_cast<std::uint8_t>(
        std::bit_width(geometry_max_hash));
    REQUIRE(hash_domain_bits >= 9u);
    REQUIRE(hash_domain_bits <= 32u);
    REQUIRE((hash_domain_bits == 32u
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>(
                (std::uint64_t{1u} << hash_domain_bits) - 1u))
        == geometry_max_hash);
    std::map<std::uint32_t, std::vector<std::uint64_t>> grouped;
    for (const auto [hash, encoded] : pairs) {
        REQUIRE(hash <= geometry_max_hash);
        grouped[static_cast<std::uint32_t>(hash)].push_back(encoded);
    }
    fragment_compact_direct_source source;
    source.runs.reserve(grouped.size());
    for (auto& [hash, positions] : grouped) {
        std::sort(
            positions.begin(),
            positions.end(),
            [](std::uint64_t left, std::uint64_t right) noexcept {
                return std::pair{left & 1u, left >> 1u}
                    < std::pair{right & 1u, right >> 1u};
            });
        const std::uint64_t strand_zero_count =
            static_cast<std::uint64_t>(std::ranges::count_if(
                positions,
                [](std::uint64_t encoded) noexcept {
                    return (encoded & 1u) == 0u;
                }));
        source.runs.push_back(fragment_compact_direct_run{
            .header = compact_hash32_sorted_run_header{
                .hash = hash,
                .global_count = positions.size(),
                .target_strand_zero_count = strand_zero_count,
            },
            .encoded_positions = std::move(positions),
        });
    }
    const direct_index_identity identity{
        .generation_id = head.generation_id,
        .source_identity = head.source_identity,
        .storage_contract_checksum = head.storage_contract_checksum,
        .target_length = manifest.target_length,
    };
    const compact_hash32_geometry geometry{
        .hash_domain_bits = hash_domain_bits,
        .max_hash = geometry_max_hash,
    };
    const compact_hash32_build_options options{
        .identity = identity,
        .max_occ_cap = max_occ_cap,
        .geometry = geometry,
    };
    const compact_hash32_sorted_run_source source_ref{
        .context = &source,
        .scan_sorted_runs_fn = &scan_fragment_compact_direct_runs,
    };
    auto built = build_compact_hash32_direct_index(source_ref, options);
    REQUIRE(built.has_value());
    auto image = std::make_shared<std::vector<std::byte>>(
        std::move(*built));
    const auto validated = validate_compact_hash32_direct_index(
        *image, identity, max_occ_cap, geometry);
    REQUIRE(validated.has_value());
    return fragment_compact_direct_test_index{
        .image = std::move(image),
        .view = *validated,
    };
}

[[nodiscard]] bioseq::indexing::owned_compact_hash32_direct_index_view
own_fragment_compact_direct_test_index(
        const fragment_compact_direct_test_index& compact,
        std::optional<bioseq::indexing::compact_hash32_direct_index_view>
            override_view = std::nullopt) {
    REQUIRE(compact.image != nullptr);
    const auto view = override_view.value_or(compact.view);
    std::shared_ptr<const std::byte> image_alias{
        compact.image, compact.image->data()};
    auto owned = bioseq::indexing::owned_compact_hash32_direct_index_view::bind(
        std::move(image_alias), view);
    REQUIRE(owned.has_value());
    return std::move(*owned);
}

struct selected_page_advice_recorder {
    static constexpr std::size_t capacity = 128u;
    std::array<bioseq::indexing::compact_direct_selected_page_range,
        capacity> records{};
    std::atomic<std::size_t> record_count{0u};
    std::atomic<bool> overflow{false};
    std::atomic<bool> drop_advice{false};

    static void advise(
            void* const opaque,
            const std::uintptr_t page_begin,
            const std::uintptr_t page_end) noexcept {
        auto& recorder = *static_cast<selected_page_advice_recorder*>(opaque);
        if (recorder.drop_advice.load(std::memory_order_relaxed)) return;
        const std::size_t ordinal = recorder.record_count.fetch_add(
            1u, std::memory_order_relaxed);
        if (ordinal >= recorder.records.size()) {
            recorder.overflow.store(true, std::memory_order_relaxed);
            return;
        }
        recorder.records[ordinal] =
            bioseq::indexing::compact_direct_selected_page_range{
                .begin = page_begin,
                .end = page_end,
            };
    }

    [[nodiscard]] bioseq::indexing::compact_direct_selected_page_adviser token(
            const std::size_t page_bytes = 1u) noexcept {
        return bioseq::indexing::compact_direct_selected_page_adviser{
            .context = this,
            .page_bytes = page_bytes,
            .advise_fn = &selected_page_advice_recorder::advise,
        };
    }
};

struct drifting_replay_minimizer_producer {
    std::size_t scan_count = 0;

    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity
    scheme_identity() const noexcept {
        return bioseq::indexing::k_minimap_scheme_identity;
    }

    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(
                bioseq::simd::minimizer_sketch_backend::scalar);
    }

    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    scan_sorted_events(
            std::uint64_t span,
            bioseq::indexing::canonical_minimizer_event_receiver receiver) {
        ++scan_count;
        if (span == 0) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::segment_span);
        }
        const std::array events{
            bioseq::indexing::canonical_minimizer_event{.hash = 1, .encoded_position = 2},
            bioseq::indexing::canonical_minimizer_event{
                .hash = 2,
                .encoded_position = scan_count == 1 ? 4u : 6u,
            },
        };
        for (const auto& event : events) {
            if (auto accepted = receiver(event); !accepted) {
                return accepted;
            }
        }
        return {};
    }
};

enum class identity_drift_axis : std::uint8_t {
    scheme,
    backend,
};

/// Changes one qualified identity immediately after a selected completed
/// scan. grouped-SoA has three distinct scans: Plan=1, Control=2, Position=3.
struct boundary_identity_drifting_producer {
    std::size_t scan_count = 0;
    std::size_t drift_after_scan = 1;
    identity_drift_axis axis = identity_drift_axis::scheme;

    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity
    scheme_identity() const noexcept {
        if (axis == identity_drift_axis::scheme
            && scan_count >= drift_after_scan) {
            return bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_identity;
        }
        return bioseq::indexing::k_minimap_scheme_identity;
    }

    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        const auto backend = axis == identity_drift_axis::backend
                && scan_count >= drift_after_scan
            ? bioseq::simd::minimizer_sketch_backend::exact_simd
            : bioseq::simd::minimizer_sketch_backend::scalar;
        return bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(backend);
    }

    [[nodiscard]] std::expected<void,
        bioseq::indexing::minimizer_generation_encoding_error>
    scan_sorted_events(
            std::uint64_t span,
            bioseq::indexing::canonical_minimizer_event_receiver receiver) {
        ++scan_count;
        if (span == 0u) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::segment_span);
        }
        return receiver(bioseq::indexing::canonical_minimizer_event{
            .hash = 1u,
            .encoded_position = 2u,
        });
    }
};

[[nodiscard]] std::vector<std::byte> collect_encoded_extent(
        std::span<const collected_minimizer_row> rows,
        const bioseq::indexing::grouped_soa_bucket_directory_entry& entry,
        bool controls) {
    const std::uint32_t first = controls
        ? entry.control_first_chunk : entry.position_first_chunk;
    const std::uint32_t count = controls
        ? entry.control_chunk_count : entry.position_chunk_count;
    const auto position_sub_key =
        bioseq::indexing::derive_minimizer_position_extent_sub_key(
            entry.sub_key);
    REQUIRE(position_sub_key.has_value());
    const bioseq::indexing::index_sub_key_t physical_sub_key = controls
        ? entry.sub_key : *position_sub_key;
    std::vector<std::byte> bytes;
    for (std::uint32_t chunk = first; chunk < first + count; ++chunk) {
        const auto row = std::find_if(
            rows.begin(), rows.end(),
            [entry, physical_sub_key, chunk](
                    const collected_minimizer_row& value) {
                return value.address.position_segment == entry.position_segment
                    && value.address.sub_key == physical_sub_key
                    && value.address.chunk_no == chunk;
            });
        REQUIRE(row != rows.end());
        bytes.insert(bytes.end(), row->bytes.begin(), row->bytes.end());
    }
    REQUIRE(bytes.size() == (controls
        ? entry.control_encoded_bytes : entry.position_encoded_bytes));
    return bytes;
}

[[nodiscard]] std::shared_ptr<std::vector<std::byte>>
make_grouped_soa_mapped_payload(
        const bioseq::indexing::encoded_grouped_soa_generation& generation,
        std::span<const collected_minimizer_row> rows) {
    auto payload = std::make_shared<std::vector<std::byte>>();
    payload->reserve(static_cast<std::size_t>(
        generation.manifest.payload_bytes));
    for (const auto& entry : generation.directory.entries()) {
        const auto controls = collect_encoded_extent(rows, entry, true);
        payload->insert(
            payload->end(), controls.begin(), controls.end());
    }
    for (const auto& entry : generation.directory.entries()) {
        const auto positions = collect_encoded_extent(rows, entry, false);
        payload->insert(
            payload->end(), positions.begin(), positions.end());
    }
    REQUIRE(payload->size() == generation.manifest.payload_bytes);
    return payload;
}

struct sparse_mapped_test_payload {
    std::shared_ptr<std::byte> owner{};
    std::size_t size = 0u;

    [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        return {owner.get(), size};
    }
};

/// Reserve a zero-filled virtual address range without materializing its
/// untouched pages. O64/C64 boundary fixtures read only the small control
/// prefix and demonstrate that the multi-gigabyte position extent is
/// not visited after max-occ rejection.
[[nodiscard]] std::optional<sparse_mapped_test_payload>
make_sparse_mapped_test_payload(std::uint64_t byte_count) {
    if (byte_count == 0u
        || byte_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::nullopt;
    }
    const std::size_t size = static_cast<std::size_t>(byte_count);
    void* const mapping = mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON,
        -1,
        0);
    if (mapping == MAP_FAILED) return std::nullopt;
    return sparse_mapped_test_payload{
        .owner = std::shared_ptr<std::byte>{
            static_cast<std::byte*>(mapping),
            [size](std::byte* address) noexcept {
                (void)munmap(address, size);
            }},
        .size = size,
    };
}

[[nodiscard]] std::expected<
    bioseq::indexing::prepared_minimizer_fragment_reader,
    bioseq::indexing::minimizer_generation_encoding_error>
open_grouped_soa_fragment_query_reader(
        bioseq::parallelism::memory_admission_controller& admission,
        const bioseq::indexing::encoded_grouped_soa_generation& generation,
        std::span<const collected_minimizer_row> rows,
        std::uint64_t max_occ,
        std::optional<bioseq::indexing::owned_compact_hash32_direct_index_view>
            compact_direct = std::nullopt) {
    auto payload = make_grouped_soa_mapped_payload(generation, rows);
    std::shared_ptr<const void> payload_owner = payload;
    return bioseq::indexing::prepared_minimizer_fragment_reader::open(
        admission,
        generation.head,
        generation.manifest,
        test_grouped_soa_expected_storage_contract(
            generation.manifest.max_occ_cap),
        generation.directory.encoded_bytes(),
        max_occ,
        std::move(payload_owner),
        std::span<const std::byte>{payload->data(), payload->size()},
        std::move(compact_direct));
}

[[nodiscard]] std::vector<bioseq::chaining::anchor>
ordinary_seed_anchors(
        std::span<const std::pair<std::uint64_t, std::uint64_t>> pairs,
        std::uint64_t target_length,
        std::span<const minimap_sketch_data> query_minimizers,
        bioseq::storage::sequence_size_t query_length,
        std::int32_t max_occ,
        std::int32_t kmer_size) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> owned_pairs{
        pairs.begin(), pairs.end()};
    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build_from_pairs(owned_pairs, target_length);

    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto anchors = bioseq::chaining::make_arena_slab<
        bioseq::chaining::anchor>();
    collect_seed_hits(
        query_minimizers,
        index,
        query_length,
        max_occ,
        kmer_size,
        anchors);
    if (anchors.size() == 0u) return {};
    return std::vector<bioseq::chaining::anchor>{
        anchors.data(), anchors.data() + anchors.size()};
}

void require_anchor_bytes_equal(
        std::span<const bioseq::chaining::anchor> actual,
        std::span<const bioseq::chaining::anchor> expected) {
    REQUIRE(actual.size() == expected.size());
    REQUIRE(std::ranges::equal(
        std::as_bytes(actual), std::as_bytes(expected)));
    REQUIRE(std::ranges::is_sorted(
        actual, bioseq::chaining::canonical_anchor_less{}));
}

void require_hot_loop_differential_evidence_equal(
        const bioseq::indexing::prepared_fragment_anchor_batch_stats& actual,
        const bioseq::indexing::prepared_fragment_anchor_batch_stats& oracle) {
    REQUIRE(actual.waves == oracle.waves);
    REQUIRE(actual.queries == oracle.queries);
    REQUIRE(actual.query_occurrences == oracle.query_occurrences);
    REQUIRE(actual.unique_query_plans == oracle.unique_query_plans);
    REQUIRE(actual.packed_occurrence_waves
        == oracle.packed_occurrence_waves);
    REQUIRE(actual.wide_occurrence_waves
        == oracle.wide_occurrence_waves);
    REQUIRE(actual.control_count_bytes == oracle.control_count_bytes);
    REQUIRE(actual.rejected_control_validation_bytes
        == oracle.rejected_control_validation_bytes);
    REQUIRE(actual.pass2_control_bytes == oracle.pass2_control_bytes);
    REQUIRE(actual.pass2_accepted_prefix_bytes
        == oracle.pass2_accepted_prefix_bytes);
    REQUIRE(actual.pass2_position_bytes == oracle.pass2_position_bytes);
    REQUIRE(actual.pass3_control_bytes == oracle.pass3_control_bytes);
    REQUIRE(actual.pass3_accepted_prefix_bytes
        == oracle.pass3_accepted_prefix_bytes);
    REQUIRE(actual.pass3_position_bytes == oracle.pass3_position_bytes);
    REQUIRE(actual.anchors_constructed == oracle.anchors_constructed);
    REQUIRE(actual.local_sort_bytes == oracle.local_sort_bytes);
    REQUIRE(actual.tie_normalization_bytes
        == oracle.tie_normalization_bytes);
    REQUIRE(actual.exact_final_bytes == oracle.exact_final_bytes);
    REQUIRE(actual.peak_exact_final_bytes
        == oracle.peak_exact_final_bytes);
    REQUIRE(actual.global_sort_calls_invoked
        == oracle.global_sort_calls_invoked);
    REQUIRE(actual.global_sort_calls_skipped
        == oracle.global_sort_calls_skipped);
    REQUIRE(actual.compositional_sortedness_receipts
        == oracle.compositional_sortedness_receipts);
    REQUIRE(actual.final_sortedness_validation_bytes
        == oracle.final_sortedness_validation_bytes);
    REQUIRE(actual.forbidden_buffers_are_zero());
    REQUIRE(oracle.forbidden_buffers_are_zero());
    REQUIRE(actual.saturated == oracle.saturated);
}

void require_encoded_generations_equal_normalized(
        const bioseq::indexing::encoded_grouped_soa_generation& lhs,
        std::span<const collected_minimizer_row> lhs_rows,
        const bioseq::indexing::encoded_grouped_soa_generation& rhs,
        std::span<const collected_minimizer_row> rhs_rows) {
    auto lhs_manifest = lhs.manifest;
    auto rhs_manifest = rhs.manifest;
    lhs_manifest.generation_id = 1;
    rhs_manifest.generation_id = 1;
    lhs_manifest.payload_checksum = 0;
    rhs_manifest.payload_checksum = 0;
    // This helper compares canonical biological payloads across physical
    // producers. Producer identity is independently attested by each original
    // head/manifest and is therefore an intentional difference for scalar
    // versus Exact SIMD comparisons.
    REQUIRE(lhs.head.storage_contract_checksum
        == compute_minimizer_generation_storage_contract_checksum(lhs.manifest));
    REQUIRE(rhs.head.storage_contract_checksum
        == compute_minimizer_generation_storage_contract_checksum(rhs.manifest));
    lhs_manifest.producer_backend_id =
        bioseq::indexing::minimizer_producer_backend::scalar;
    rhs_manifest.producer_backend_id =
        bioseq::indexing::minimizer_producer_backend::scalar;
    lhs_manifest.producer_identity_revision =
        bioseq::indexing::k_minimizer_producer_identity_revision;
    rhs_manifest.producer_identity_revision =
        bioseq::indexing::k_minimizer_producer_identity_revision;
    REQUIRE(lhs_manifest == rhs_manifest);
    REQUIRE(lhs.head.source_identity == rhs.head.source_identity);
    REQUIRE(compute_minimizer_generation_storage_contract_checksum(lhs_manifest)
        == compute_minimizer_generation_storage_contract_checksum(rhs_manifest));
    REQUIRE(lhs.head.source_content_checksum == rhs.head.source_content_checksum);
    REQUIRE(std::ranges::equal(
        lhs.directory.encoded_bytes(), rhs.directory.encoded_bytes()));
    REQUIRE(lhs_rows.size() == rhs_rows.size());
    REQUIRE_FALSE(lhs_rows.empty());
    for (std::size_t index = 0; index + 1u < lhs_rows.size(); ++index) {
        REQUIRE(lhs_rows[index].address.position_segment
            == rhs_rows[index].address.position_segment);
        REQUIRE(lhs_rows[index].address.sub_key
            == rhs_rows[index].address.sub_key);
        REQUIRE(lhs_rows[index].address.chunk_no
            == rhs_rows[index].address.chunk_no);
        REQUIRE(lhs_rows[index].bytes == rhs_rows[index].bytes);
    }
}

[[nodiscard]] bioseq::indexing::minimizer_generation_row_view row_view(
        const collected_minimizer_row& row) noexcept {
    return {
        .address = row.address,
        .bytes = row.bytes,
        .row_checksum = row.row_checksum,
    };
}

[[nodiscard]] bioseq::indexing::validated_minimizer_generation_row_receipt
trusted_row_receipt(const collected_minimizer_row& row) {
    auto validated = bioseq::indexing::
        validate_minimizer_generation_row_receipt(row_view(row));
    REQUIRE(validated.has_value());
    return std::move(*validated);
}

void recompute_row_checksum(collected_minimizer_row& row) noexcept {
    row.row_checksum = bioseq::indexing::crc64_index_chunk_row(
        row.address.generation_id,
        row.address.position_segment,
        row.address.sub_key,
        row.address.chunk_no,
        static_cast<std::uint32_t>(row.bytes.size()),
        row.bytes);
}

[[nodiscard]] bioseq::indexing::minimizer_generation_row_validator
make_generation_row_validator(
        const bioseq::indexing::encoded_grouped_soa_generation& generation) {
    auto made = bioseq::indexing::minimizer_generation_row_validator::create(
        generation);
    REQUIRE(made.has_value());
    return std::move(*made);
}

void require_complete_generation_rows(
        const bioseq::indexing::encoded_grouped_soa_generation& generation,
        std::span<const collected_minimizer_row> rows) {
    auto validator = make_generation_row_validator(generation);
    require_wire_error(
        validator.validate_publication_head(
            bioseq::indexing::minimizer_generation_row_view{
                .address = generation.unpublished_head_row.address,
                .bytes = generation.unpublished_head_row.bytes(),
                .row_checksum = generation.unpublished_head_row.row_checksum,
            }),
        bioseq::indexing::minimizer_generation_encoding_error::row_count);
    for (const auto& row : rows) {
        REQUIRE(validator.accept(row_view(row)).has_value());
    }
    REQUIRE(validator.finish_ready().has_value());
    REQUIRE(validator.validate_publication_head(
        bioseq::indexing::minimizer_generation_row_view{
            .address = generation.unpublished_head_row.address,
            .bytes = generation.unpublished_head_row.bytes(),
            .row_checksum = generation.unpublished_head_row.row_checksum,
        }).has_value());
}

[[nodiscard]] std::vector<bioseq::indexing::canonical_minimizer_event>
decode_generation_events(
        const bioseq::indexing::encoded_grouped_soa_generation& generation,
        std::span<const collected_minimizer_row> rows) {
    using namespace bioseq::indexing;
    std::vector<bioseq::indexing::canonical_minimizer_event> events;
    const auto& profile = generation.directory.profile();
    for (const auto& entry : generation.directory.entries()) {
        const auto control_bytes = collect_encoded_extent(rows, entry, true);
        const auto position_bytes = collect_encoded_extent(rows, entry, false);
        const auto core = derive_minimizer_position_segment_bounds(
            generation.manifest,
            static_cast<std::uint32_t>(entry.position_segment));
        REQUIRE(core.has_value());
        const auto controls = decode_grouped_soa_controls(
            control_bytes, entry, profile);
        REQUIRE(controls.has_value());
        const auto positions = decode_grouped_soa_positions(
            position_bytes, *controls, entry, *core, profile);
        REQUIRE(positions.has_value());
        for (const auto& control : *controls) {
            for (std::uint64_t position = control.position_begin;
                 position < control.position_end; ++position) {
                events.push_back({
                    .hash = control.hash,
                    .encoded_position = (*positions)[
                        static_cast<std::size_t>(position)],
                });
            }
        }
    }
    return events;
}

struct rebuilt_generation_metadata {
    bioseq::indexing::minimizer_generation_head head{};
    bioseq::indexing::minimizer_generation_manifest manifest{};
    std::vector<std::byte> directory_bytes;
};

[[nodiscard]] rebuilt_generation_metadata rebuild_generation_metadata(
        const bioseq::indexing::encoded_grouped_soa_generation& original,
        std::span<const bioseq::indexing::grouped_soa_bucket_directory_entry> entries) {
    using namespace bioseq::indexing;
    auto directory_bytes = encode_minimizer_bucket_directory(
        entries, original.directory.profile());
    REQUIRE(directory_bytes.has_value());
    auto manifest = original.manifest;
    manifest.directory_bytes = directory_bytes->size();
    manifest.directory_checksum = crc64_ecma182(*directory_bytes);
    const auto manifest_bytes =
        encode_minimizer_generation_manifest_record(manifest);
    REQUIRE(manifest_bytes.has_value());
    auto head = original.head;
    head.storage_contract_checksum =
        compute_minimizer_generation_storage_contract_checksum(manifest);
    head.manifest_checksum = crc64_ecma182(
        std::span<const std::byte>{*manifest_bytes});
    return {
        .head = head,
        .manifest = manifest,
        .directory_bytes = std::move(*directory_bytes),
    };
}

[[nodiscard]] bioseq::indexing::encoded_minimizer_fixed_row make_test_fixed_row(
        const bioseq::indexing::index_chunk_address& address,
        std::span<const std::byte> bytes) {
    bioseq::indexing::encoded_minimizer_fixed_row row{
        .address = address,
        .byte_count = static_cast<std::uint32_t>(bytes.size()),
    };
    std::copy(bytes.begin(), bytes.end(), row.storage.begin());
    row.row_checksum = bioseq::indexing::crc64_index_chunk_row(
        address.generation_id,
        address.position_segment,
        address.sub_key,
        address.chunk_no,
        row.byte_count,
        row.bytes());
    return row;
}

template <std::size_t p_size>
[[nodiscard]] bioseq::indexing::encoded_minimizer_fixed_row make_test_fixed_row(
        const bioseq::indexing::index_chunk_address& address,
        const std::array<std::byte, p_size>& bytes) {
    return make_test_fixed_row(
        address, std::span<const std::byte>{bytes});
}

void refresh_test_generation_fixed_rows(
        bioseq::indexing::encoded_grouped_soa_generation& generation) {
    using namespace bioseq::indexing;
    const auto manifest_bytes =
        encode_minimizer_generation_manifest(generation.manifest);
    REQUIRE(manifest_bytes.has_value());
    generation.head.storage_contract_checksum =
        compute_minimizer_generation_storage_contract_checksum(generation.manifest);
    generation.head.manifest_checksum = crc64_ecma182(
        std::span<const std::byte>{*manifest_bytes});
    const auto head_bytes = encode_minimizer_generation_head(generation.head);
    REQUIRE(head_bytes.has_value());
    generation.manifest_row = make_test_fixed_row(
        index_chunk_address{
            .generation_id = generation.manifest.generation_id,
            .position_segment = -1,
            .sub_key = k_directory_sub_key,
            .chunk_no = 0,
        },
        std::span<const std::byte>{*manifest_bytes});
    generation.unpublished_head_row = make_test_fixed_row(
        index_chunk_address{
            .generation_id = 0,
            .position_segment = -1,
            .sub_key = k_generation_head_sub_key,
            .chunk_no = 0,
        },
        *head_bytes);
}

}  // namespace

TEST_CASE("exact_simd indexing bridge is linked, exact, and instrumented",
          "[component:bioseq][layer:unit][feature:index][resource:highway]") {
    auto data = make_dna4(4097, 0xB81D6EULL);
    range_t range(data);
    auto range_owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto range_view = bioseq::storage::make_lazy_sequence_view(range_owner);
    sketch_collector exact_collector;
#if HWY_TARGET == HWY_SCALAR
    REQUIRE_THROWS_AS(
        (void)bioseq::indexing::collect_exact_simd_index_range_or_throw(
            exact_collector,
            range_view,
            fixed_alphabet_code<::seq::dna4>(),
            0,
            range_view.bp_size(),
            10,
            15,
            0,
            fixed_minimizer_kernels(),
            "bridge contract"),
        bioseq::indexing::exact_simd_index_runtime_error);
#else
    const auto stats =
        bioseq::indexing::collect_exact_simd_index_range_or_throw(
            exact_collector,
            range_view,
            fixed_alphabet_code<::seq::dna4>(),
            0,
            range_view.bp_size(),
            10,
            15,
            0,
            fixed_minimizer_kernels(),
            "bridge contract");
    const auto scalar = whole_sequence_minimizers(range, 10, 15);
    const auto& exact = exact_collector.extract_sketches_data();
    REQUIRE(exact.size() == scalar.size());
    for (std::size_t index = 0; index < exact.size(); ++index) {
        REQUIRE(exact[index].raw_value() == scalar[index].raw_value());
    }
    REQUIRE(stats.vector_batches > 0);
    REQUIRE(stats.packed_decode_batches > 0);
    REQUIRE(stats.packed_decode_lanes == stats.bp_consumed);
    REQUIRE(stats.scalar_backend_branches == 0);
    REQUIRE(stats.scalar_tail_symbols == 0);
    REQUIRE(stats.scalar_hash_symbols == 0);
    REQUIRE(stats.scalar_window_transitions == 0);
    REQUIRE(stats.scalar_event_records == 0);
    REQUIRE(stats.scalar_sort_records == 0);
    REQUIRE(stats.scalar_dedup_records == 0);
#endif
}

TEST_CASE("exact_simd indexing bridge preserves DNA5 nt4 reset behavior and stats",
          "[component:bioseq][layer:unit][feature:index][resource:highway]") {
    constexpr std::size_t w = 10;
    constexpr std::size_t k = 15;
    std::vector<seq::dna5> data(4097);
    constexpr std::array<std::uint8_t, 4> acgt_ranks{0, 1, 2, 4};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(acgt_ranks[(index * 7u + index / 11u) % 4u]);
    }
    dna5_range_t no_n_range(data);
    auto no_n_owner = bioseq::storage::make_resident_lazy_sequence(no_n_range);
    const auto no_n_view = bioseq::storage::make_lazy_sequence_view(no_n_owner);
    sketch_collector no_n_exact_collector;
    sketch_collector mismatched_alphabet_collector;
    REQUIRE_THROWS_AS(
        (void)bioseq::indexing::collect_exact_simd_index_range_or_throw(
            mismatched_alphabet_collector,
            no_n_view,
            fixed_alphabet_code<::seq::dna4>(),
            0,
            no_n_view.bp_size(),
            w,
            k,
            0,
            fixed_minimizer_kernels(),
            "DNA5 bridge mismatched alphabet evidence"),
        bioseq::indexing::exact_simd_index_runtime_error);
#if HWY_TARGET == HWY_SCALAR
    REQUIRE_THROWS_AS(
        (void)bioseq::indexing::collect_exact_simd_index_range_or_throw(
            no_n_exact_collector,
            no_n_view,
            fixed_alphabet_code<::seq::dna5>(),
            0,
            no_n_view.bp_size(),
            w,
            k,
            0,
            fixed_minimizer_kernels(),
            "DNA5 bridge contract"),
        bioseq::indexing::exact_simd_index_runtime_error);
#else
    const auto no_n_stats =
        bioseq::indexing::collect_exact_simd_index_range_or_throw(
            no_n_exact_collector,
            no_n_view,
            fixed_alphabet_code<::seq::dna5>(),
            0,
            no_n_view.bp_size(),
            w,
            k,
            0,
            fixed_minimizer_kernels(),
            "DNA5 bridge contract");
    const auto no_n_scalar = whole_sequence_minimizers(no_n_range, w, k);
    const auto& no_n_exact = no_n_exact_collector.extract_sketches_data();
    REQUIRE(no_n_exact.size() == no_n_scalar.size());
    for (std::size_t index = 0; index < no_n_exact.size(); ++index) {
        REQUIRE(no_n_exact[index].raw_value()
            == no_n_scalar[index].raw_value());
    }
    REQUIRE(no_n_stats.ambiguous_symbols == 0);
    REQUIRE(no_n_stats.ambiguous_reset_batches == 0);
    REQUIRE(no_n_stats.vector_batches > 0);
    REQUIRE(no_n_stats.packed_decode_lanes == no_n_stats.bp_consumed);

    constexpr std::array<std::size_t, 5> n_positions{
        97, 1023, 2048, 3071, 4095};
    for (const std::size_t position : n_positions) {
        data[position].assign_rank(3); // SeqAn dna5 N rank
    }
    dna5_range_t with_n_range(data);
    auto with_n_owner = bioseq::storage::make_resident_lazy_sequence(with_n_range);
    const auto with_n_view = bioseq::storage::make_lazy_sequence_view(
        with_n_owner);
    sketch_collector with_n_collector;
    const auto with_n_stats =
        bioseq::indexing::collect_exact_simd_index_range_or_throw(
            with_n_collector,
            with_n_view,
            fixed_alphabet_code<::seq::dna5>(),
            0,
            with_n_view.bp_size(),
            w,
            k,
            0,
            fixed_minimizer_kernels(),
            "DNA5 bridge N-reset contract");
    REQUIRE(with_n_stats.ambiguous_symbols == n_positions.size());
    REQUIRE(with_n_stats.ambiguous_reset_batches > 0);
    REQUIRE(with_n_stats.scalar_backend_branches == 0);
    REQUIRE(with_n_stats.scalar_hash_symbols == 0);
    REQUIRE(with_n_stats.scalar_window_transitions == 0);
    REQUIRE(with_n_stats.scalar_event_records == 0);
    REQUIRE(with_n_stats.scalar_sort_records == 0);
    REQUIRE(with_n_stats.scalar_dedup_records == 0);
    const auto expected_events = baseline_dna5_events(data, w, k);
    const auto& exact_events = with_n_collector.extract_sketches_data();
    REQUIRE(exact_events.size() == expected_events.size());
    for (std::size_t index = 0; index < exact_events.size(); ++index) {
        REQUIRE(exact_events[index].kmer() == expected_events[index].hash);
        REQUIRE(exact_events[index].pos_strand_encoded()
            == expected_events[index].encoded_position);
    }
#endif
}

TEST_CASE("canonical reserved generation-head sub-key is range-pinned",
          "[component:bioseq][layer:unit][feature:storage]") {
    STATIC_REQUIRE(bioseq::indexing::k_generation_head_sub_key
        == (std::numeric_limits<bioseq::indexing::index_sub_key_t>::max)() - 1);
    STATIC_REQUIRE(bioseq::indexing::k_generation_head_sub_key
        > bioseq::indexing::k_temporary_run_sub_key_base);
    STATIC_REQUIRE(bioseq::indexing::k_generation_head_sub_key
        < bioseq::indexing::k_directory_sub_key);
}

TEST_CASE("CRC-64/ECMA-182 is exact and incrementally composable",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::indexing::crc64_ecma182;
    using bioseq::indexing::crc64_ecma182_accumulator;

    STATIC_REQUIRE(crc64_ecma182(std::string_view{}) == 0);
    STATIC_REQUIRE(crc64_ecma182("123456789") == 0x6C40DF5F0B497347ULL);

    crc64_ecma182_accumulator incremental;
    incremental.update("1234");
    incremental.update("56789");
    REQUIRE(incremental.checksum() == 0x6C40DF5F0B497347ULL);

    constexpr std::array<std::byte, 9> bytes{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
        std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
        std::byte{'7'}, std::byte{'8'}, std::byte{'9'},
    };
    STATIC_REQUIRE(crc64_ecma182(std::span<const std::byte>{bytes})
        == 0x6C40DF5F0B497347ULL);
}

TEST_CASE("validated row CRC algebra reconstructs raw extent CRC",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;

    REQUIRE(crc64_ecma182_advance_zero_bytes(0, 0) == 0);
    REQUIRE(crc64_ecma182_concatenate(
        crc64_ecma182("control"), crc64_ecma182("position"), 8u)
        == crc64_ecma182("controlposition"));

    const index_chunk_address signed_address{
        .generation_id = 0xFEDCBA9876543210ULL,
        .position_segment = (std::numeric_limits<std::int32_t>::min)() + 17,
        .sub_key = (std::numeric_limits<index_sub_key_t>::min)() + 29,
        .chunk_no = 0x89ABCDEFu,
    };
    constexpr std::uint32_t signed_length = 0x10203040u;
    crc64_ecma182_accumulator independently_encoded_prefix;
    independently_encoded_prefix.update_little_endian(
        signed_address.generation_id);
    independently_encoded_prefix.update_little_endian(
        std::bit_cast<std::uint32_t>(signed_address.position_segment));
    independently_encoded_prefix.update_little_endian(
        std::bit_cast<std::uint64_t>(signed_address.sub_key));
    independently_encoded_prefix.update_little_endian(signed_address.chunk_no);
    independently_encoded_prefix.update_little_endian(signed_length);
    REQUIRE(crc64_index_chunk_row_prefix(
        signed_address.generation_id,
        signed_address.position_segment,
        signed_address.sub_key,
        signed_address.chunk_no,
        signed_length) == independently_encoded_prefix.checksum());

    std::uint64_t algebraic_extent = 0;
    crc64_ecma182_accumulator direct_extent;
    std::uint64_t random_state = 0xD1B54A32D192ED03ULL;
    const auto next_random = [&random_state]() noexcept {
        random_state ^= random_state >> 12u;
        random_state ^= random_state << 25u;
        random_state ^= random_state >> 27u;
        return random_state * 0x2545F4914F6CDD1DULL;
    };
    const auto append_case = [&](std::size_t length, std::uint32_t ordinal) {
        INFO("payload length=" << length << " ordinal=" << ordinal);
        std::vector<std::byte> payload(length);
        for (std::byte& byte : payload) {
            byte = static_cast<std::byte>(next_random() & 0xffu);
        }
        const index_chunk_address address{
            .generation_id = 0x8000000000000000ULL + ordinal,
            .position_segment = ordinal % 2u == 0
                ? (std::numeric_limits<std::int32_t>::min)()
                    + static_cast<std::int32_t>(ordinal)
                : (std::numeric_limits<std::int32_t>::max)()
                    - static_cast<std::int32_t>(ordinal),
            .sub_key = ordinal % 2u == 0
                ? (std::numeric_limits<index_sub_key_t>::min)()
                    + static_cast<index_sub_key_t>(ordinal)
                : (std::numeric_limits<index_sub_key_t>::max)()
                    - static_cast<index_sub_key_t>(ordinal),
            .chunk_no = 0xF0000000u + ordinal,
        };
        const auto bytes = std::span<const std::byte>{payload};
        const auto chunk_bytes = static_cast<std::uint32_t>(bytes.size());
        const std::uint64_t row_checksum = crc64_index_chunk_row(
            address.generation_id,
            address.position_segment,
            address.sub_key,
            address.chunk_no,
            chunk_bytes,
            bytes);
        direct_extent.update(bytes);
        algebraic_extent = crc64_append_validated_index_chunk_payload(
            algebraic_extent, address, chunk_bytes, row_checksum);
        REQUIRE(algebraic_extent == direct_extent.checksum());
    };

    const std::array<std::size_t, 6> boundary_lengths{
        0u, 1u, 17u, 8063u, 8064u, 31u};
    std::uint32_t ordinal = 0;
    for (const std::size_t length : boundary_lengths) {
        append_case(length, ordinal++);
    }
    for (std::size_t iteration = 0; iteration < 128u; ++iteration) {
        append_case(static_cast<std::size_t>(next_random() % 8065u), ordinal++);
    }

    REQUIRE(algebraic_extent == direct_extent.checksum());
}

TEST_CASE("grouped-SoA cached ownership accounting replaces and moves exactly",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::indexing::protocol_owned_bytes_ledger;
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
        protocol_owned_bytes_ledger>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<
        protocol_owned_bytes_ledger>);

    protocol_owned_bytes_ledger accounting;
    const auto replace = [&accounting](std::uint64_t& field,
                                   std::uint64_t value) {
        REQUIRE(accounting.replace_checked(field, value));
        field = value;
        REQUIRE(accounting.field_sum_holds());
    };
    replace(accounting.directory_bytes, 101u);
    replace(accounting.query_hash_bytes, 211u);
    replace(accounting.query_record_bytes, 307u);
    replace(accounting.selected_extent_owner_bytes, 401u);
    replace(accounting.query_record_position_bytes, 503u);
    replace(accounting.control_scratch_bytes, 601u);
    replace(accounting.selected_extent_owner_bytes, 17u);
    REQUIRE(accounting.add_temporary_peak_checked(8192u));

    const std::uint64_t total_before_failure = accounting.total_bytes;
    REQUIRE_FALSE(accounting.replace_checked(total_before_failure + 1u, 0u));
    REQUIRE(accounting.total_bytes == total_before_failure);
    REQUIRE(accounting.field_sum_holds());

    protocol_owned_bytes_ledger moved{std::move(accounting)};
    REQUIRE(moved.total_bytes == total_before_failure);
    REQUIRE(moved.field_sum_holds());
    REQUIRE(accounting.total_bytes == 0u);
    REQUIRE(accounting.field_sum_holds());
    moved.reset();
    REQUIRE(moved.total_bytes == 0u);
    REQUIRE(moved.field_sum_holds());

    protocol_owned_bytes_ledger overflow;
    overflow.directory_bytes =
        (std::numeric_limits<std::uint64_t>::max)() - 3u;
    overflow.total_bytes = overflow.directory_bytes;
    REQUIRE_FALSE(overflow.replace_checked(0u, 4u));
    REQUIRE_FALSE(overflow.add_temporary_peak_checked(4u));
    REQUIRE(overflow.field_sum_holds());
}

TEST_CASE("canonical manifests distinguish bounded ntHash while preserving minimap identity",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr grouped_soa_generation_options unbound_writer{};
    STATIC_REQUIRE_FALSE(is_supported_minimizer_scheme_identity(
        unbound_writer.minimizer_scheme_id,
        unbound_writer.minimizer_scheme_revision));
    constexpr expected_minimizer_generation_identity unbound_reader{};
    STATIC_REQUIRE_FALSE(is_supported_minimizer_scheme_identity(
        unbound_reader.minimizer_scheme_id,
        unbound_reader.minimizer_scheme_revision));
    STATIC_REQUIRE_FALSE(is_supported_minimizer_producer_backend_identity(
        minimizer_producer_identity{
            .id = unbound_reader.producer_backend_id,
            .revision = unbound_reader.producer_identity_revision,
        }));
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
        minimizer_sketch_backend::scalar) == k_minimap_scheme_identity);
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
        minimizer_sketch_backend::exact_simd) == k_minimap_scheme_identity);
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
        minimizer_sketch_backend::vulkan_segment)
        == k_minimap_scheme_identity);
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
        minimizer_sketch_backend::bounded_nthash_simd)
        == k_bounded_nthash_u32_r7_sum_scheme_identity);
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
        minimizer_sketch_backend::scalar).id
        == minimizer_producer_backend::scalar);
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
        minimizer_sketch_backend::exact_simd).id
        == minimizer_producer_backend::exact_simd);
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
        minimizer_sketch_backend::bounded_nthash_simd).id
        == minimizer_producer_backend::bounded_nthash_simd);
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
        minimizer_sketch_backend::vulkan_segment).id
        == minimizer_producer_backend::vulkan_segment);
    constexpr auto minimap_manifest = golden_generation_manifest();
    constexpr auto minimap_bytes = encode_minimizer_generation_manifest(minimap_manifest);
    STATIC_REQUIRE(minimap_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*minimap_bytes, 122)
        == k_minimap_scheme_id);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*minimap_bytes, 124)
        == k_minimap_scheme_revision);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*minimap_bytes, 126)
        == static_cast<std::uint8_t>(minimizer_producer_backend::scalar));
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*minimap_bytes, 127)
        == k_minimizer_producer_identity_revision);

    constexpr auto bounded = [] {
        auto value = golden_generation_manifest();
        assign_grouped_soa_profile(
            value, k_test_bounded_grouped_soa_profile);
        value.producer_backend_id =
            minimizer_producer_backend::bounded_nthash_simd;
        return value;
    }();
    constexpr auto bounded_bytes = encode_minimizer_generation_manifest(bounded);
    STATIC_REQUIRE(bounded_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*bounded_bytes, 122)
        == k_bounded_nthash_u32_r7_sum_scheme_id);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*bounded_bytes, 124)
        == k_bounded_nthash_u32_r7_sum_scheme_revision);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*bounded_bytes, 126)
        == static_cast<std::uint8_t>(
            minimizer_producer_backend::bounded_nthash_simd));
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*bounded_bytes, 127)
        == k_minimizer_producer_identity_revision);
    STATIC_REQUIRE(*bounded_bytes != *minimap_bytes);
    constexpr auto decoded_bounded = decode_minimizer_generation_manifest(
        std::span<const std::byte>{*bounded_bytes});
    STATIC_REQUIRE(decoded_bounded.has_value());
    STATIC_REQUIRE(*decoded_bounded == bounded);
    STATIC_REQUIRE(compute_minimizer_generation_storage_contract_checksum(bounded)
        != compute_minimizer_generation_storage_contract_checksum(minimap_manifest));

    constexpr auto canonical_minimap = [] {
        auto value = golden_generation_manifest();
        value.gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
        return value;
    }();
    constexpr auto canonical_bounded = [] {
        auto value = golden_generation_manifest();
        assign_grouped_soa_profile(
            value, k_test_bounded_grouped_soa_profile);
        value.producer_backend_id =
            minimizer_producer_backend::bounded_nthash_simd;
        return value;
    }();

    constexpr expected_minimizer_generation_identity minimap_reader{
        .layout = minimizer_generation_layout::grouped_soa_segment_local,
        .alphabet_code = canonical_minimap.alphabet_code,
        .storage_unit_byte_width = canonical_minimap.storage_unit_byte_width,
        .kmer_size = canonical_minimap.kmer_size,
        .window_size = canonical_minimap.window_size,
        .gpu_abi_revision = canonical_minimap.gpu_abi_revision,
        .minimizer_scheme_id = k_minimap_scheme_id,
        .minimizer_scheme_revision = k_minimap_scheme_revision,
        .producer_backend_id = minimizer_producer_backend::scalar,
        .producer_identity_revision =
            k_minimizer_producer_identity_revision,
        .max_occ_cap = k_test_grouped_soa_profile.max_occ_cap,
    };
    constexpr expected_minimizer_generation_identity bounded_reader{
        .layout = minimizer_generation_layout::grouped_soa_segment_local,
        .alphabet_code = canonical_bounded.alphabet_code,
        .storage_unit_byte_width = canonical_bounded.storage_unit_byte_width,
        .kmer_size = canonical_bounded.kmer_size,
        .window_size = canonical_bounded.window_size,
        .gpu_abi_revision = canonical_bounded.gpu_abi_revision,
        .minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id,
        .minimizer_scheme_revision = k_bounded_nthash_u32_r7_sum_scheme_revision,
        .producer_backend_id = minimizer_producer_backend::bounded_nthash_simd,
        .producer_identity_revision =
            k_minimizer_producer_identity_revision,
        .max_occ_cap = k_test_bounded_grouped_soa_profile.max_occ_cap,
    };
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(
        canonical_minimap, minimap_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(
        canonical_bounded, bounded_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_storage_contract(
        canonical_minimap, bounded_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(
        canonical_minimap, bounded_reader).error()
        == minimizer_generation_encoding_error::storage_contract_mismatch);
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_storage_contract(
        canonical_bounded, minimap_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(
        canonical_bounded, minimap_reader).error()
        == minimizer_generation_encoding_error::storage_contract_mismatch);

    constexpr auto canonical_exact = [=] {
        auto value = canonical_minimap;
        value.producer_backend_id = minimizer_producer_backend::exact_simd;
        return value;
    }();
    constexpr auto canonical_vulkan = [=] {
        auto value = canonical_minimap;
        value.producer_backend_id = minimizer_producer_backend::vulkan_segment;
        return value;
    }();
    constexpr auto exact_reader = [=] {
        auto value = minimap_reader;
        value.producer_backend_id = minimizer_producer_backend::exact_simd;
        return value;
    }();
    constexpr auto vulkan_reader = [=] {
        auto value = minimap_reader;
        value.producer_backend_id = minimizer_producer_backend::vulkan_segment;
        return value;
    }();
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(
        canonical_exact, exact_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(
        canonical_vulkan, vulkan_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_storage_contract(
        canonical_minimap, exact_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_storage_contract(
        canonical_exact, minimap_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_storage_contract(
        canonical_minimap, vulkan_reader).has_value());
    STATIC_REQUIRE_FALSE(validate_minimizer_generation_storage_contract(
        canonical_vulkan, minimap_reader).has_value());
    STATIC_REQUIRE(compute_minimizer_generation_storage_contract_checksum(
        canonical_minimap) != compute_minimizer_generation_storage_contract_checksum(
            canonical_exact));
    STATIC_REQUIRE(compute_minimizer_generation_storage_contract_checksum(
        canonical_minimap) != compute_minimizer_generation_storage_contract_checksum(
            canonical_vulkan));

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1, 2}, {2, 4}, {2, 6},
    };
    vector_replay_minimizer_producer producer{
        minimizers, k_bounded_nthash_u32_r7_sum_scheme_identity};
    collecting_minimizer_row_sink sink;
    auto options = test_generation_options(100);
    options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
    options.minimizer_scheme_revision = k_bounded_nthash_u32_r7_sum_scheme_revision;
    const auto generated = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generated.has_value());
    REQUIRE(generated->manifest.minimizer_scheme_id
        == k_bounded_nthash_u32_r7_sum_scheme_id);
    REQUIRE(generated->manifest.minimizer_scheme_revision
        == k_bounded_nthash_u32_r7_sum_scheme_revision);
    REQUIRE(generated->manifest.producer_backend_id
        == minimizer_producer_backend::bounded_nthash_simd);
    REQUIRE(generated->manifest.producer_identity_revision
        == k_minimizer_producer_identity_revision);

    for (const minimizer_sketch_backend backend : {
             minimizer_sketch_backend::exact_simd,
             minimizer_sketch_backend::vulkan_segment}) {
        const auto backend_identity =
            minimizer_producer_backend_identity_for_backend(backend);
        vector_replay_minimizer_producer physical_producer{
            minimizers, k_minimap_scheme_identity, backend_identity};
        collecting_minimizer_row_sink physical_sink;
        const auto physical = encode_minimizer_generation(
            physical_producer, physical_sink, test_generation_options(100));
        REQUIRE(physical.has_value());
        REQUIRE(physical->manifest.producer_backend_id
            == backend_identity.id);
        REQUIRE(physical->manifest.producer_identity_revision
            == backend_identity.revision);
        const auto matching = expected_grouped_soa_generation_identity_for_backend(
            minimizer_generation_layout::grouped_soa_segment_local,
            backend,
            k_minimizer_persisted_dna4_alphabet_code,
            8,
            15,
            10,
            65'534u);
        REQUIRE(matching.has_value());
        REQUIRE(validate_minimizer_generation_storage_contract(
            physical->manifest, *matching).has_value());
        REQUIRE_FALSE(validate_minimizer_generation_storage_contract(
            physical->manifest, test_expected_storage_contract()).has_value());
    }

    vector_replay_minimizer_producer invalid_producer{
        minimizers, k_bounded_nthash_u32_r7_sum_scheme_identity};
    collecting_minimizer_row_sink invalid_sink;
    options.minimizer_scheme_revision = k_minimap_scheme_revision;
    require_wire_error(
        encode_minimizer_generation(invalid_producer, invalid_sink, options),
        minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(invalid_producer.scan_count == 0);
    REQUIRE(invalid_sink.rows.empty());

    vector_replay_minimizer_producer mismatched_producer{
        minimizers, k_bounded_nthash_u32_r7_sum_scheme_identity};
    collecting_minimizer_row_sink mismatched_sink;
    auto baseline_options = test_generation_options(100);
    require_wire_error(
        encode_minimizer_generation(
            mismatched_producer, mismatched_sink, baseline_options),
        minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(mismatched_producer.scan_count == 0);
    REQUIRE(mismatched_sink.rows.empty());
}

TEST_CASE("generation manifest rejects corrupt headers and invalid geometry",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const auto manifest = golden_generation_manifest();
    const auto good = *encode_minimizer_generation_manifest(manifest);

    require_wire_error(
        decode_minimizer_generation_manifest(
            std::span<const std::byte>{good.data(), good.size() - 1}),
        minimizer_generation_encoding_error::encoded_size);
    std::array<std::byte, 161> oversized{};
    std::copy(good.begin(), good.end(), oversized.begin());
    require_wire_error(decode_minimizer_generation_manifest(oversized),
                       minimizer_generation_encoding_error::encoded_size);

    auto corrupted = good;
    corrupted[0] ^= std::byte{1};
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::magic);
    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 4, 2);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::unsupported_storage_layout);
    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 6, 159);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::declared_size);

    // Malformed or unknown producer identity namespaces fail closed.
    corrupted = good;
    corrupted[126] = std::byte{0};
    corrupted[127] = std::byte{0};
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::storage_contract_mismatch);
    for (const std::uint8_t unsupported_backend : {0u, 5u, 255u}) {
        corrupted = good;
        test_write_le<std::uint8_t>(corrupted, 126, unsupported_backend);
        require_wire_error(decode_minimizer_generation_manifest(corrupted),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    for (const std::uint8_t unsupported_revision : {0u, 2u, 255u}) {
        corrupted = good;
        test_write_le<std::uint8_t>(corrupted, 127, unsupported_revision);
        require_wire_error(decode_minimizer_generation_manifest(corrupted),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    corrupted = good;
    test_write_le<std::uint8_t>(corrupted, 126,
        static_cast<std::uint8_t>(
            minimizer_producer_backend::bounded_nthash_simd));
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::storage_contract_mismatch);

    constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 4>
        unsupported_schemes{{
            {k_minimap_scheme_id, k_bounded_nthash_u32_r7_sum_scheme_revision},
            {k_bounded_nthash_u32_r7_sum_scheme_id, k_minimap_scheme_revision},
            {2u, k_bounded_nthash_u32_r7_sum_scheme_revision},
            {k_bounded_nthash_u32_r7_sum_scheme_id, 2u},
        }};
    for (const auto [scheme_id, scheme_revision] : unsupported_schemes) {
        INFO("unsupported minimizer scheme " << scheme_id
            << "/" << scheme_revision);
        auto invalid_scheme = manifest;
        invalid_scheme.minimizer_scheme_id = scheme_id;
        invalid_scheme.minimizer_scheme_revision = scheme_revision;
        require_wire_error(
            encode_minimizer_generation_manifest(invalid_scheme),
            minimizer_generation_encoding_error::storage_contract_mismatch);

        corrupted = good;
        test_write_le<std::uint16_t>(corrupted, 122, scheme_id);
        test_write_le<std::uint16_t>(corrupted, 124, scheme_revision);
        require_wire_error(
            decode_minimizer_generation_manifest(corrupted),
            minimizer_generation_encoding_error::storage_contract_mismatch);
    }

    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 8, 0);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::generation_id);
    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 8,
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::generation_id);

    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 120, 1);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::segment_geometry_revision);
    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 112, 0);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::segment_span);
    corrupted = good;
    test_write_le<std::uint32_t>(corrupted, 88, 2);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::segment_count);
    corrupted = good;
    test_write_le<std::uint32_t>(corrupted, 96,
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::directory_chunk_count);

    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 32,
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    test_write_le<std::uint64_t>(corrupted, 112, 1);
    test_write_le<std::uint32_t>(corrupted, 88,
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::segment_count);

    auto invalid = manifest;
    invalid.layout = minimizer_generation_layout::invalid;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::storage_contract_mismatch);
    invalid = manifest;
    invalid.generation_id = 0;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::generation_id);
    invalid = manifest;
    invalid.position_segment_count = 4;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::segment_count);
    invalid = manifest;
    invalid.position_segment_span_bp = 0;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::segment_span);
    invalid = manifest;
    invalid.segment_geometry_revision = 1;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::segment_geometry_revision);
    invalid = manifest;
    invalid.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::directory_chunk_count);
    invalid = manifest;
    invalid.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::generation_id);

    auto signed_boundary = manifest;
    signed_boundary.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    signed_boundary.target_length =
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
    signed_boundary.position_segment_span_bp = 1;
    signed_boundary.position_segment_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    signed_boundary.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    const auto signed_boundary_bytes =
        encode_minimizer_generation_manifest(signed_boundary);
    REQUIRE(signed_boundary_bytes.has_value());
    require_wire_value(
        decode_minimizer_generation_manifest(*signed_boundary_bytes),
        signed_boundary);

    auto unrestricted_words = manifest;
    unrestricted_words.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    unrestricted_words.source_identity = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.source_content_checksum =
        (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.target_length = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.control_count = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.position_count = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.payload_bytes = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.payload_checksum = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.directory_bytes = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.directory_checksum = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.position_segment_count = 2;
    unrestricted_words.extent_count = (std::numeric_limits<std::uint32_t>::max)();
    unrestricted_words.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    unrestricted_words.position_segment_span_bp = std::uint64_t{1} << 63u;
    unrestricted_words.position_width = minimizer_scalar_width::u64;
    const auto unrestricted_bytes =
        encode_minimizer_generation_manifest(unrestricted_words);
    REQUIRE(unrestricted_bytes.has_value());
    require_wire_value(
        decode_minimizer_generation_manifest(*unrestricted_bytes),
        unrestricted_words);

    auto empty = manifest;
    empty.target_length = 0;
    empty.position_segment_count = 0;
    REQUIRE(encode_minimizer_generation_manifest(empty).has_value());
    empty.position_segment_span_bp = 0;
    REQUIRE(encode_minimizer_generation_manifest(empty).has_value());
    empty.position_segment_count = 1;
    require_wire_error(encode_minimizer_generation_manifest(empty),
                       minimizer_generation_encoding_error::segment_count);
}

TEST_CASE("standalone manifest codec defers referenced-object consistency",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;

    auto manifest = golden_generation_manifest();
    // These combinations are intentionally nonsensical without the referenced
    // objects. The fixed codec preserves them; generation validation must match
    // byte counts, checksums, extents, storage contract, and head identity against
    // the actual directory and payload before publication or use.
    manifest.payload_bytes = 0;
    manifest.payload_checksum = (std::numeric_limits<std::uint64_t>::max)();
    manifest.directory_bytes = 1;
    manifest.directory_checksum = (std::numeric_limits<std::uint64_t>::max)();
    manifest.extent_count = (std::numeric_limits<std::uint32_t>::max)();
    manifest.directory_chunk_count = 0;

    const auto encoded = encode_minimizer_generation_manifest(manifest);
    REQUIRE(encoded.has_value());
    require_wire_value(decode_minimizer_generation_manifest(*encoded), manifest);
}

TEST_CASE("uniform persisted segment cores and bp ranges are exact at boundaries",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const auto manifest = golden_generation_manifest();

    require_wire_value(derive_minimizer_position_segment_bounds(manifest, 0),
                       minimizer_position_segment_bounds{0, 8});
    require_wire_value(derive_minimizer_position_segment_bounds(manifest, 1),
                       minimizer_position_segment_bounds{8, 16});
    require_wire_value(derive_minimizer_position_segment_bounds(manifest, 2),
                       minimizer_position_segment_bounds{16, 20});
    require_wire_error(derive_minimizer_position_segment_bounds(manifest, 3),
                       minimizer_generation_encoding_error::segment_ordinal);

    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 0, 0),
                       minimizer_position_segment_range{0, 0});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 0, 1),
                       minimizer_position_segment_range{0, 1});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 7, 9),
                       minimizer_position_segment_range{0, 2});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 16, 20),
                       minimizer_position_segment_range{2, 3});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 20, 20),
                       minimizer_position_segment_range{3, 3});
    require_wire_error(minimizer_position_segments_for_bp_range(manifest, 9, 8),
                       minimizer_generation_encoding_error::bp_range);
    require_wire_error(minimizer_position_segments_for_bp_range(manifest, 0, 21),
                       minimizer_generation_encoding_error::bp_range);

    auto huge = manifest;
    huge.target_length = (std::numeric_limits<std::uint64_t>::max)();
    huge.position_segment_span_bp = std::uint64_t{1} << 63u;
    huge.position_segment_count = 2;
    huge.position_width = minimizer_scalar_width::u64;
    const auto last = derive_minimizer_position_segment_bounds(huge, 1);
    REQUIRE(last.has_value());
    REQUIRE(last->begin_bp == (std::uint64_t{1} << 63u));
    REQUIRE(last->end_bp == (std::numeric_limits<std::uint64_t>::max)());

    auto empty = manifest;
    empty.target_length = 0;
    empty.position_segment_span_bp = 0;
    empty.position_segment_count = 0;
    require_wire_value(minimizer_position_segments_for_bp_range(empty, 0, 0),
                       minimizer_position_segment_range{0, 0});
    require_wire_error(derive_minimizer_position_segment_bounds(empty, 0),
                       minimizer_generation_encoding_error::segment_ordinal);

    STATIC_REQUIRE(sql_bigint_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())));
    STATIC_REQUIRE_FALSE(sql_bigint_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u));
    STATIC_REQUIRE(sql_integer_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())));
    STATIC_REQUIRE_FALSE(sql_integer_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) + 1u));

    STATIC_REQUIRE(k_minimizer_segment_geometry_revision == 2u);
    STATIC_REQUIRE(k_position_extent_sub_key_base
        == (index_sub_key_t{1} << 61));
    STATIC_REQUIRE(k_position_extent_sub_key_base * 2
        == k_temporary_run_sub_key_base);
    constexpr auto first_position_key =
        derive_minimizer_position_extent_sub_key(0);
    constexpr auto last_position_key =
        derive_minimizer_position_extent_sub_key(
            k_position_extent_sub_key_base - 1);
    STATIC_REQUIRE(first_position_key.has_value());
    STATIC_REQUIRE(*first_position_key == k_position_extent_sub_key_base);
    STATIC_REQUIRE(last_position_key.has_value());
    STATIC_REQUIRE(*last_position_key == k_temporary_run_sub_key_base - 1);
    STATIC_REQUIRE_FALSE(
        derive_minimizer_position_extent_sub_key(-1).has_value());
    STATIC_REQUIRE_FALSE(derive_minimizer_position_extent_sub_key(
        k_position_extent_sub_key_base).has_value());
    STATIC_REQUIRE(project_minimizer_extent_sub_key(0, false).has_value());
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(0, false) == 0);
    STATIC_REQUIRE(project_minimizer_extent_sub_key(
        k_position_extent_sub_key_base, true).has_value());
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(
        k_position_extent_sub_key_base, true) == 0);
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(
        k_temporary_run_sub_key_base - 1, true)
        == k_position_extent_sub_key_base - 1);
    STATIC_REQUIRE_FALSE(project_minimizer_extent_sub_key(
        (std::numeric_limits<index_sub_key_t>::min)(), true).has_value());
    STATIC_REQUIRE_FALSE(project_minimizer_extent_sub_key(
        k_position_extent_sub_key_base - 1, true).has_value());
    STATIC_REQUIRE_FALSE(project_minimizer_extent_sub_key(
        k_position_extent_sub_key_base, false).has_value());
    STATIC_REQUIRE_FALSE(project_minimizer_extent_sub_key(
        k_temporary_run_sub_key_base, true).has_value());
}

TEST_CASE("lookup and persistence minimizer roles are atomically split",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(minimizer_lookup_index<minimizer_hashmap_fragment_index>);
    STATIC_REQUIRE(persistable_minimizer_index<minimizer_hashmap_fragment_index>);
    STATIC_REQUIRE(minimizer_lookup_index<prepared_grouped_soa_query_reader>);
    STATIC_REQUIRE_FALSE(
        persistable_minimizer_index<prepared_grouped_soa_query_reader>);

    constexpr segment_unit_range range{1, 9, 32, 256};
    STATIC_REQUIRE(range.cs_u == 1);
    STATIC_REQUIRE(range.ce_u == 9);
    STATIC_REQUIRE(range.core_lo_bp == 32);
    STATIC_REQUIRE(range.core_hi_bp == 256);
}

TEST_CASE("multi-span lookup carries one global occurrence decision",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    struct segmented_fixture_index {
        minimizer_lookup_spans result{};
        [[nodiscard]] minimizer_lookup_spans lookup_spans(
                std::uint64_t) const noexcept {
            return result;
        }
        [[nodiscard]] bioseq::storage::sequence_size_t target_length()
                const noexcept {
            return 4096;
        }
    };
    struct unsafe_owning_lookup_index {
        [[nodiscard]] std::vector<std::uint64_t> lookup(
                std::uint64_t) const {
            return {};
        }
        [[nodiscard]] bioseq::storage::sequence_size_t target_length()
                const noexcept {
            return 0;
        }
    };
    STATIC_REQUIRE(minimizer_lookup_index<segmented_fixture_index>);
    STATIC_REQUIRE(segmented_minimizer_lookup_index<segmented_fixture_index>);
    STATIC_REQUIRE_FALSE(contiguous_minimizer_lookup_index<segmented_fixture_index>);
    STATIC_REQUIRE_FALSE(contiguous_minimizer_lookup_index<
        unsafe_owning_lookup_index>);
    STATIC_REQUIRE_FALSE(minimizer_lookup_index<unsafe_owning_lookup_index>);

    const std::array<std::uint64_t, 2> first{4, 8};
    const std::array<std::uint64_t, 3> second{12, 16, 20};
    const std::array<minimizer_position_span, 2> spans{
        minimizer_position_span{first},
        minimizer_position_span{second},
    };
    const auto complete = minimizer_lookup_spans::segmented(
        spans, /*global_occurrence_count=*/5);
    REQUIRE(complete.positions_complete());
    REQUIRE(complete.global_occurrence_count() == 5);
    REQUIRE(complete.contiguous_positions().empty());
    REQUIRE(complete.segmented_positions().size() == 2);
    REQUIRE(complete.well_formed());

    const auto malformed_count = minimizer_lookup_spans::segmented(
        spans, /*global_occurrence_count=*/4);
    REQUIRE_FALSE(malformed_count.well_formed());
    const auto unavailable =
        minimizer_lookup_spans::positions_unavailable(1'000);
    REQUIRE_FALSE(unavailable.positions_complete());
    REQUIRE(unavailable.global_occurrence_count() == 1'000);
    REQUIRE(unavailable.well_formed());

    segmented_fixture_index index{complete};
    const auto adapted = lookup_minimizer_spans(index, 7);
    REQUIRE(adapted.well_formed());
    REQUIRE(adapted.global_occurrence_count() == 5);
}

TEST_CASE("directory entry has an exact 128-byte little-endian layout",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr grouped_soa_bucket_directory_entry entry{
        .partition_id = 0x11223344U,
        .position_segment = 0x10203040,
        .lo_hash = 0x01020304ULL,
        .hi_hash = 0x11121314ULL,
        .sub_key = 0x11223344LL,
        .control_first_chunk = 0,
        .control_chunk_count = 2,
        .position_first_chunk = 0,
        .position_chunk_count = 3,
        .control_element_count = 768,
        .position_element_count = 2304,
        .control_encoded_bytes = 6152,
        .position_encoded_bytes = 4608,
        .control_checksum = 0x3132333435363738ULL,
        .position_checksum = 0x4142434445464748ULL,
        .shape_offset_width = minimizer_scalar_width::u16,
    };
    constexpr auto encoded = encode_minimizer_bucket_dir_entry_record(
        entry, k_test_grouped_soa_profile);
    STATIC_REQUIRE(encoded.has_value());
    STATIC_REQUIRE(encoded->size() == 128);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 0) == entry.partition_id);
    STATIC_REQUIRE(std::bit_cast<std::int32_t>(
        test_read_le<std::uint32_t>(*encoded, 4)) == entry.position_segment);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 8) == entry.lo_hash);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 16) == entry.hi_hash);
    STATIC_REQUIRE(std::bit_cast<std::int64_t>(
        test_read_le<std::uint64_t>(*encoded, 24)) == entry.sub_key);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 32) == 0);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 36) == 2);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 40) == 0);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 44) == 3);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 48) == 768);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 56) == 2304);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 64) == 6152);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 72) == 4608);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 80)
        == entry.control_checksum);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 88)
        == entry.position_checksum);
    constexpr auto decoded = decode_minimizer_bucket_dir_entry(
        *encoded, k_test_grouped_soa_profile);
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE(*decoded == entry);

    require_wire_error(
        decode_minimizer_bucket_dir_entry(
            std::span<const std::byte>{encoded->data(), encoded->size() - 1u},
            k_test_grouped_soa_profile),
        minimizer_generation_encoding_error::encoded_size);
    auto invalid = entry;
    invalid.position_segment = -1;
    require_wire_error(encode_minimizer_bucket_dir_entry_record(
                           invalid, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::position_segment);
    invalid = entry;
    invalid.lo_hash = invalid.hi_hash + 1u;
    require_wire_error(encode_minimizer_bucket_dir_entry_record(
                           invalid, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::hash_range);
    invalid = entry;
    invalid.sub_key = k_position_extent_sub_key_base;
    require_wire_error(encode_minimizer_bucket_dir_entry_record(
                           invalid, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::sub_key);
    invalid = entry;
    invalid.control_encoded_bytes -= 16u;
    require_wire_error(encode_minimizer_bucket_dir_entry_record(
                           invalid, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::byte_count);
    invalid = entry;
    invalid.position_first_chunk = 1;
    require_wire_error(encode_minimizer_bucket_dir_entry_record(
                           invalid, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::chunk_range);
}

TEST_CASE("grouped_soa_directory_view owns bytes and rejects unstable directory order",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const grouped_soa_bucket_directory_entry first{
        .partition_id = 0,
        .position_segment = 0,
        .lo_hash = 10,
        .hi_hash = 20,
        .sub_key = 0,
        .control_first_chunk = 0,
        .control_chunk_count = 1,
        .position_first_chunk = 0,
        .position_chunk_count = 1,
        .control_element_count = 1,
        .position_element_count = 1,
        .control_encoded_bytes = 16,
        .position_encoded_bytes = 2,
        .control_checksum = 1,
        .position_checksum = 2,
        .shape_offset_width = minimizer_scalar_width::u16,
    };
    auto second = first;
    second.partition_id = 1;
    second.lo_hash = 21;
    second.hi_hash = 30;
    second.sub_key = 1;
    const std::array entries{first, second};
    auto bytes = encode_minimizer_bucket_directory(
        entries, k_test_grouped_soa_profile);
    REQUIRE(bytes.has_value());
    auto view = grouped_soa_directory_view::decode(
        *bytes, k_test_grouped_soa_profile);
    REQUIRE(view.has_value());
    REQUIRE(view->entries().size() == 2);
    REQUIRE(view->find(0, 10)->partition_id == 0);
    REQUIRE(view->find(0, 30)->partition_id == 1);
    REQUIRE(view->find(0, 31) == nullptr);
    REQUIRE(view->find(1, 10) == nullptr);

    // Source mutation cannot affect the owning view or its GPU-upload bytes.
    (*bytes)[8] ^= std::byte{0xff};
    REQUIRE(view->entries()[0].lo_hash == 10);
    REQUIRE(view->encoded_bytes()[8] != (*bytes)[8]);

    auto overlap = second;
    overlap.lo_hash = first.hi_hash;
    const std::array overlapping{first, overlap};
    require_wire_error(encode_minimizer_bucket_directory(
                           overlapping, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::directory_order);
    auto wrong_ordinal = second;
    wrong_ordinal.partition_id = 7;
    const std::array wrong_ordinals{first, wrong_ordinal};
    require_wire_error(encode_minimizer_bucket_directory(
                           wrong_ordinals, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::partition_id);
    std::array<std::byte, 127> truncated{};
    require_wire_error(grouped_soa_directory_view::decode(
                           truncated, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::directory_size);
}

TEST_CASE("deterministic encoder fixes core ownership and policy bytes",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = k_minimizer_preferred_segment_span_bp;
    constexpr auto encoded_position = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {100, encoded_position(span, 1)},
        {7, encoded_position(span - 1u, 0)},
        {100, encoded_position(9, 1)},
        {9, encoded_position(span + 3u, 0)},
        {100, encoded_position(3, 0)},
    };
    const auto options = test_generation_options(span + 100u);

    vector_replay_minimizer_producer direct_producer{minimizers};
    collecting_minimizer_row_sink direct_sink;
    auto direct = encode_minimizer_generation(
        direct_producer, direct_sink, options);
    REQUIRE(direct.has_value());
    REQUIRE(direct_producer.scan_count == 3);

    std::reverse(minimizers.begin(), minimizers.end());
    vector_replay_minimizer_producer reversed_producer{minimizers};
    collecting_minimizer_row_sink reversed_sink;
    auto reversed_input = encode_minimizer_generation(
        reversed_producer, reversed_sink, options);
    REQUIRE(reversed_input.has_value());
    require_encoded_generations_equal_normalized(
        *direct, direct_sink.rows, *reversed_input, reversed_sink.rows);

    REQUIRE(direct->manifest.position_segment_span_bp == span);
    REQUIRE(direct->manifest.position_segment_count == 2);
    REQUIRE(direct->manifest.segment_geometry_revision
        == k_minimizer_segment_geometry_revision);
    REQUIRE(direct->manifest.gpu_abi_revision
        == k_grouped_soa_gpu_abi_revision);
    REQUIRE(direct->head.storage_contract_checksum
        == compute_minimizer_generation_storage_contract_checksum(direct->manifest));
    REQUIRE(validate_minimizer_generation_metadata(
        direct->head, direct->manifest, direct->directory).has_value());

    bool saw_first_core = false;
    bool saw_second_core = false;
    for (const grouped_soa_bucket_directory_entry& entry : direct->directory.entries()) {
        saw_first_core = saw_first_core || entry.position_segment == 0;
        saw_second_core = saw_second_core || entry.position_segment == 1;
        REQUIRE(entry.sub_key >= 0);
        REQUIRE(entry.sub_key < k_position_extent_sub_key_base);
        REQUIRE(entry.control_first_chunk == 0);
        REQUIRE(entry.position_first_chunk == 0);
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        REQUIRE(position_sub_key.has_value());
        const auto control_first_row = std::ranges::find_if(
            direct_sink.rows,
            [entry](const collected_minimizer_row& row) {
                return row.address.position_segment == entry.position_segment
                    && row.address.sub_key == entry.sub_key
                    && row.address.chunk_no == 0;
            });
        const auto position_first_row = std::ranges::find_if(
            direct_sink.rows,
            [entry, position_sub_key](const collected_minimizer_row& row) {
                return row.address.position_segment == entry.position_segment
                    && row.address.sub_key == *position_sub_key
                    && row.address.chunk_no == 0;
            });
        REQUIRE(control_first_row != direct_sink.rows.end());
        REQUIRE(position_first_row != direct_sink.rows.end());
        const auto core = derive_minimizer_position_segment_bounds(
            direct->manifest, static_cast<std::uint32_t>(entry.position_segment));
        REQUIRE(core.has_value());
        const auto position_bytes = collect_encoded_extent(
            direct_sink.rows, entry, false);
        for (std::size_t offset = 0; offset < position_bytes.size(); offset += 4u) {
            const std::uint32_t local = test_read_le<std::uint32_t>(
                position_bytes, offset);
            const std::uint64_t local_bp = local >> 1u;
            REQUIRE(local_bp < core->end_bp - core->begin_bp);
            const std::uint64_t global_bp = core->begin_bp + local_bp;
            REQUIRE(global_bp >= core->begin_bp);
            REQUIRE(global_bp < core->end_bp);
        }
        REQUIRE(entry.control_encoded_bytes % 8u == 0);
        REQUIRE(entry.position_encoded_bytes % 4u == 0);
    }
    REQUIRE(saw_first_core);
    REQUIRE(saw_second_core);
    REQUIRE(direct_sink.max_callback_bytes <= k_grouped_soa_extent_payload_bytes);
    for (const collected_minimizer_row& row : direct_sink.rows) {
        REQUIRE(row.bytes.size() <= k_grouped_soa_extent_payload_bytes);
        REQUIRE(row.row_checksum == crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            static_cast<std::uint32_t>(row.bytes.size()),
            row.bytes));
    }
    REQUIRE(direct_sink.rows.back().address == direct->manifest_row.address);
    REQUIRE(std::ranges::none_of(
        direct_sink.rows,
        [direct](const collected_minimizer_row& row) {
            return row.address == direct->unpublished_head_row.address;
        }));
    require_complete_generation_rows(*direct, direct_sink.rows);
}

TEST_CASE("grouped-SoA planned metadata precedes the direct grouped-SoA payload",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {11u, 4u << 1u},
        {7u, 12u << 1u},
        {11u, 70u << 1u},
        {19u, 76u << 1u},
    };
    auto options = test_grouped_soa_generation_options(128u, 10u);
    options.position_segment_span_bp = 64u;
    vector_replay_minimizer_producer producer{minimizers};
    planned_direct_minimizer_sink sink;

    STATIC_REQUIRE(planned_grouped_soa_generation_sink<
        planned_direct_minimizer_sink>);
    STATIC_REQUIRE(grouped_soa_minimizer_payload_sink<
        planned_direct_minimizer_sink>);
    const auto generation = encode_minimizer_generation(
        producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(sink.storage_plan.has_value());
    REQUIRE_FALSE(sink.payload_rows.empty());
    REQUIRE(sink.events.size()
        == 1u + sink.payload_rows.size() + sink.ordinary_rows.size());
    REQUIRE(sink.events.front() == planned_direct_sink_event::begin);
    REQUIRE(std::ranges::count(
        sink.events, planned_direct_sink_event::begin) == 1u);
    for (std::size_t index = 0u; index < sink.payload_rows.size(); ++index) {
        REQUIRE(sink.events[index + 1u]
            == planned_direct_sink_event::payload);
    }
    for (std::size_t index = 1u + sink.payload_rows.size();
         index < sink.events.size(); ++index) {
        REQUIRE(sink.events[index] == planned_direct_sink_event::row);
    }

    const auto align_up_8 = [](std::uint64_t value) noexcept {
        return (value + 7u) & ~std::uint64_t{7u};
    };
    const std::uint64_t expected_directory_offset = align_up_8(
        4096u + generation->manifest.payload_bytes);
    const std::uint64_t expected_manifest_offset = align_up_8(
        expected_directory_offset
            + generation->directory.encoded_bytes().size());
    const grouped_soa_storage_plan_view expected_plan{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .payload_bytes = generation->manifest.payload_bytes,
        .directory_bytes = generation->directory.encoded_bytes().size(),
        .manifest_bytes =
            minimizer_generation_manifest::encoded_bytes,
        .directory_offset = expected_directory_offset,
        .manifest_offset = expected_manifest_offset,
        .file_bytes = expected_manifest_offset
            + minimizer_generation_manifest::encoded_bytes,
    };
    REQUIRE(*sink.storage_plan == expected_plan);
}

TEST_CASE("grouped-SoA direct payload is row-exact without tuple callbacks",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {11u, 4u << 1u},
        {7u, 12u << 1u},
        {11u, 70u << 1u},
        {19u, 76u << 1u},
    };
    auto options = test_grouped_soa_generation_options(128u, 10u);
    options.position_segment_span_bp = 64u;

    STATIC_REQUIRE_FALSE(planned_grouped_soa_generation_sink<
        row_only_collecting_minimizer_sink>);
    STATIC_REQUIRE_FALSE(grouped_soa_minimizer_payload_sink<
        row_only_collecting_minimizer_sink>);
    vector_replay_minimizer_producer row_producer{minimizers};
    row_only_collecting_minimizer_sink row_sink;
    const auto row_generation = encode_minimizer_generation(
        row_producer, row_sink, options);
    REQUIRE(row_generation.has_value());

    vector_replay_minimizer_producer direct_producer{minimizers};
    planned_direct_minimizer_sink direct_sink;
    const auto direct_generation = encode_minimizer_generation(
        direct_producer, direct_sink, options);
    REQUIRE(direct_generation.has_value());
    REQUIRE(row_generation->head == direct_generation->head);
    REQUIRE(row_generation->manifest == direct_generation->manifest);
    REQUIRE(std::ranges::equal(
        row_generation->directory.encoded_bytes(),
        direct_generation->directory.encoded_bytes()));

    std::vector<collected_minimizer_row> direct_rows{
        direct_sink.payload_rows.begin(), direct_sink.payload_rows.end()};
    direct_rows.insert(
        direct_rows.end(),
        direct_sink.ordinary_rows.begin(),
        direct_sink.ordinary_rows.end());
    REQUIRE(row_sink.rows.size() == direct_rows.size());
    REQUIRE(direct_sink.ordinary_payload_row_callbacks == 0u);
    REQUIRE(std::ranges::all_of(
        direct_sink.ordinary_rows,
        [](const collected_minimizer_row& row) {
            return row.address.position_segment == -1;
        }));

    crc64_ecma182_accumulator direct_payload_row_checksums;
    for (std::size_t index = 0u; index < direct_rows.size(); ++index) {
        const collected_minimizer_row& expected = row_sink.rows[index];
        const collected_minimizer_row& actual = direct_rows[index];
        REQUIRE(actual.address == expected.address);
        REQUIRE(actual.bytes == expected.bytes);
        REQUIRE(actual.row_checksum == expected.row_checksum);
        REQUIRE(actual.row_checksum == crc64_index_chunk_row(
            actual.address.generation_id,
            actual.address.position_segment,
            actual.address.sub_key,
            actual.address.chunk_no,
            static_cast<std::uint32_t>(actual.bytes.size()),
            actual.bytes));
        if (index < direct_sink.payload_rows.size()) {
            direct_payload_row_checksums.update_little_endian(
                actual.row_checksum);
        }
    }
    REQUIRE(direct_payload_row_checksums.checksum()
        == direct_generation->manifest.payload_checksum);
    require_complete_generation_rows(*row_generation, row_sink.rows);
    require_complete_generation_rows(*direct_generation, direct_rows);
}

TEST_CASE("independent fragment halo and spill-run merge producers are canonical",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::size_t fragment_bp = 32768;
    constexpr std::size_t halo = 10u + 15u - 2u;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;
    auto data = make_dna4(70000, 0x371C0DEULL);
    range_t range(data);

    auto oracle_sketches = whole_sequence_minimizers(range, 10, 15);
    std::vector<canonical_minimizer_event> oracle;
    oracle.reserve(oracle_sketches.size());
    for (const minimap_sketch_data& minimizer : oracle_sketches) {
        oracle.push_back({
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    std::sort(oracle.begin(), oracle.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.hash != rhs.hash) return lhs.hash < rhs.hash;
        return lhs.encoded_position < rhs.encoded_position;
    });
    REQUIRE(std::adjacent_find(oracle.begin(), oracle.end()) == oracle.end());
    REQUIRE(std::ranges::any_of(oracle, [](const auto& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        return position + halo >= fragment_bp
            && position <= fragment_bp + halo;
    }));

    lazy_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto fragment_producer = index_type::make_generation_replay_producer(
        lazy_view, minimizer_sketch_backend::scalar, nullptr);
    collecting_minimizer_row_sink fragment_sink;
    auto fragment_options = test_generation_options(range.bp_size());
    auto fragment_encoding = encode_minimizer_generation(
        fragment_producer, fragment_sink, fragment_options);
    REQUIRE(fragment_encoding.has_value());
    REQUIRE(fragment_producer.scan_count() == 3);
    const minimizer_fragment_generation_execution_receipt scalar_execution =
        fragment_producer.execution_stats();
    REQUIRE(scalar_execution.is_accepted());
    REQUIRE(scalar_execution.selected_backend
        == minimizer_sketch_backend::scalar);
    REQUIRE(scalar_execution.scalar_core_runs > 0);
    REQUIRE(scalar_execution.exact_simd_core_runs == 0);
    REQUIRE(scalar_execution.bounded_nthash_core_runs == 0);
    REQUIRE(scalar_execution.vulkan_core_runs == 0);
    REQUIRE(fragment_producer.peak_buffered_core_bp()
        <= k_minimizer_preferred_segment_span_bp);

    fake_fragment_storage<> spill_storage;
    auto spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        spill_storage,
        lazy_view,
        minimizer_sketch_backend::scalar,
        nullptr,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count() >= 3);
    const std::uint64_t scalar_spill_run_count = spilled->run_count();
    auto spill_producer = spill_run_index_type::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spilled));
    collecting_minimizer_row_sink spill_sink;
    auto spill_options = fragment_options;
    ++spill_options.generation_id;
    auto spill_encoding = encode_minimizer_generation(
        spill_producer, spill_sink, spill_options);
    REQUIRE(spill_encoding.has_value());
    REQUIRE(spill_producer.scan_count() == 3);
    REQUIRE(spill_producer.merge_pass_count() == 3);
    REQUIRE(spill_storage.segment_merge_calls() == 4);
    REQUIRE(spill_storage.segment_merge_run_visits()
        == 4u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited()
        == 4u * spill_storage.segment_spill_bytes());
    STATIC_REQUIRE(decltype(spill_producer)::owned_event_capacity() == 0);
    require_encoded_generations_equal_normalized(
        *fragment_encoding, fragment_sink.rows, *spill_encoding, spill_sink.rows);

    const auto fragment_events = decode_generation_events(*fragment_encoding, fragment_sink.rows);
    const auto spill_events = decode_generation_events(*spill_encoding, spill_sink.rows);
    REQUIRE(fragment_events == oracle);
    REQUIRE(spill_events == oracle);
    const auto oracle_lookup = canonical_event_lookup_map(oracle);
    REQUIRE(canonical_event_lookup_map(fragment_events) == oracle_lookup);
    REQUIRE(canonical_event_lookup_map(spill_events) == oracle_lookup);

    // The internal all-SIMD backend must traverse the same fragmented index
    // cores and external spill runs without scalar sketch fallback. Encoded
    // bytes are compared after normalising generation identity.
    lazy_sequence<dna4_u64, chunked> lazy_exact{
        chunked::from_range(range)};
    const auto lazy_exact_view =
        bioseq::storage::make_lazy_sequence_view(lazy_exact);
    auto exact_fragment_producer = index_type::make_generation_replay_producer(
        lazy_exact_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        fixed_alphabet_code<::seq::dna4>());
    collecting_minimizer_row_sink exact_fragment_sink;
    auto exact_fragment_options = fragment_options;
    exact_fragment_options.generation_id += 2;
    auto exact_fragment_encoding = encode_minimizer_generation(
        exact_fragment_producer, exact_fragment_sink, exact_fragment_options);
    REQUIRE(exact_fragment_encoding.has_value());
    const minimizer_fragment_generation_execution_receipt exact_execution =
        exact_fragment_producer.execution_stats();
    REQUIRE(exact_execution.is_accepted());
    REQUIRE(exact_execution.selected_backend
        == minimizer_sketch_backend::exact_simd);
    REQUIRE(exact_execution.scalar_core_runs == 0);
    REQUIRE(exact_execution.exact_simd_core_runs > 0);
    REQUIRE(exact_execution.bounded_nthash_core_runs == 0);
    REQUIRE(exact_execution.vulkan_core_runs == 0);
    REQUIRE(exact_execution.exact_simd_stats_present);
    REQUIRE(exact_execution.exact_simd_stats.is_internally_consistent(true));

    fake_fragment_storage<> exact_spill_storage;
    auto exact_spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        exact_spill_storage,
        lazy_exact_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        fragment_bp,
        k_minimizer_preferred_segment_span_bp,
        nullptr,
        fixed_alphabet_code<::seq::dna4>());
    REQUIRE(exact_spilled.has_value());
    REQUIRE(exact_spilled->run_count() == scalar_spill_run_count);
    auto exact_spill_producer = spill_run_index_type::make_generation_replay_producer(
        exact_spill_storage, range.bp_size(), std::move(*exact_spilled));
    collecting_minimizer_row_sink exact_spill_sink;
    auto exact_spill_options = fragment_options;
    exact_spill_options.generation_id += 3;
    auto exact_spill_encoding = encode_minimizer_generation(
        exact_spill_producer, exact_spill_sink, exact_spill_options);
    REQUIRE(exact_spill_encoding.has_value());

    require_encoded_generations_equal_normalized(
        *fragment_encoding, fragment_sink.rows, *exact_fragment_encoding, exact_fragment_sink.rows);
    require_encoded_generations_equal_normalized(
        *spill_encoding, spill_sink.rows, *exact_spill_encoding, exact_spill_sink.rows);
    REQUIRE(decode_generation_events(*exact_fragment_encoding, exact_fragment_sink.rows)
        == oracle);
    REQUIRE(decode_generation_events(*exact_spill_encoding, exact_spill_sink.rows)
        == oracle);
    require_complete_generation_rows(*fragment_encoding, fragment_sink.rows);
    require_complete_generation_rows(*spill_encoding, spill_sink.rows);
    require_complete_generation_rows(*exact_fragment_encoding, exact_fragment_sink.rows);
    require_complete_generation_rows(*exact_spill_encoding, exact_spill_sink.rows);
}

TEST_CASE("grouped-SoA spill-run exact SIMD binds the canonical shape profile",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 128;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, fragment_units>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;
    auto data = make_dna4(18013, 0x4D5053464D543556ULL);
    range_t range(data);

    const auto spill_exact = [&](fake_fragment_storage<>& storage) {
        lazy_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        return spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            lazy_view,
            minimizer_sketch_backend::exact_simd,
            fixed_minimizer_kernels(),
            fragment_bp,
            k_minimizer_preferred_segment_span_bp,
            nullptr,
            fixed_alphabet_code<::seq::dna4>());
    };

    SECTION("canonical grouped-SoA profile is published with native SIMD stats") {
        fake_fragment_storage<> storage;
        auto spill = spill_exact(storage);
        REQUIRE(spill.has_value());
        REQUIRE(spill->execution_receipt().is_spill_valid());

        const auto options =
            test_grouped_soa_generation_options(range.bp_size());
        collecting_minimizer_row_sink sink;
        minimizer_sequence_generation_execution_receipt execution;
        const auto encoded = spill_run_index_type::encode_persisted_generation(
            storage,
            sink,
            std::move(*spill),
            options,
            {},
            &execution);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->manifest.layout
            == minimizer_generation_layout::
                grouped_soa_segment_local);
        REQUIRE(encoded->manifest.alphabet_code
            == k_minimizer_persisted_dna4_alphabet_code);
        REQUIRE(encoded->manifest.gpu_abi_revision
            == k_grouped_soa_gpu_abi_revision);
        REQUIRE(encoded->manifest.max_occ_cap
            == options.max_occ_cap);

        const auto canonical_profile = derive_grouped_soa_minimizer_profile(
            k_minimap_scheme_identity,
            options.alphabet_code,
            options.kmer_size,
            encoded->manifest.position_segment_span_bp,
            options.max_occ_cap);
        REQUIRE(canonical_profile.has_value());
        REQUIRE(encoded->directory.profile() == *canonical_profile);
        const auto manifest_profile =
            derive_grouped_soa_minimizer_profile(encoded->manifest);
        REQUIRE(manifest_profile.has_value());
        REQUIRE(*manifest_profile == *canonical_profile);

        REQUIRE(execution.is_internally_consistent());
        REQUIRE(execution.generation_layout
            == options.layout);
        REQUIRE(execution.selected_backend
            == minimizer_sketch_backend::exact_simd);
        REQUIRE(execution.scalar_core_runs == 0u);
        REQUIRE(execution.exact_simd_core_runs > 0u);
        REQUIRE(execution.exact_simd_stats_available);
        REQUIRE(execution.exact_simd_stats.is_internally_consistent(true));
        REQUIRE(execution.bounded_nthash_core_runs == 0u);
        REQUIRE(execution.vulkan_core_runs == 0u);
        REQUIRE(execution.replay_scan_count == 3u);
        REQUIRE(execution.replay_merge_pass_count == 3u);
        REQUIRE(execution.replay_completed_scan_count == 3u);
        REQUIRE(execution.replay_failed_scan_count == 0u);
        REQUIRE(execution.replay_visited_event_count
            == 3u * execution.spilled_event_count);
        REQUIRE(sink.grouped_soa_payload_write_calls > 0u);
        REQUIRE(sink.grouped_soa_row_crc_bytes
            == encoded->manifest.payload_bytes);
        require_complete_generation_rows(*encoded, sink.rows);
    }

    const auto require_profile_rejection = [
            &](grouped_soa_generation_options options) {
        fake_fragment_storage<> storage;
        auto spill = spill_exact(storage);
        REQUIRE(spill.has_value());
        collecting_minimizer_row_sink sink;
        minimizer_sequence_generation_execution_receipt execution;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                storage,
                sink,
                std::move(*spill),
                options,
                {},
                &execution),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
        REQUIRE_FALSE(execution.is_internally_consistent());
    };

    SECTION("noncanonical DNA4 cardinality cannot masquerade as pack code") {
        auto options = test_grouped_soa_generation_options(range.bp_size());
        options.alphabet_code = 4u;
        require_profile_rejection(options);
    }

    SECTION("invalid grouped-SoA count profile has no alternate fallback") {
        auto options = test_grouped_soa_generation_options(range.bp_size());
        options.max_occ_cap =
            (std::numeric_limits<std::uint64_t>::max)();
        require_profile_rejection(options);
    }

    SECTION("unsupported spill-run layouts remain rejected") {
        auto options = test_grouped_soa_generation_options(range.bp_size());
        options.layout = minimizer_generation_layout::invalid;
        require_profile_rejection(options);
    }
}

TEST_CASE("spill-run SHA-256 binding matches standard known vectors and split updates",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const auto require_hex = [](const direct_minimizer_content_digest& digest,
                                std::string_view expected) {
        REQUIRE(expected.size() == digest.size() * 2u);
        const auto nibble = [](char value) -> std::uint8_t {
            if (value >= '0' && value <= '9') {
                return static_cast<std::uint8_t>(value - '0');
            }
            return static_cast<std::uint8_t>(value - 'a' + 10);
        };
        for (std::size_t index = 0; index < digest.size(); ++index) {
            const std::uint8_t value = static_cast<std::uint8_t>(
                (nibble(expected[index * 2u]) << 4u)
                | nibble(expected[index * 2u + 1u]));
            REQUIRE(std::to_integer<std::uint8_t>(digest[index]) == value);
        }
    };

    bioseq::common::sha256_hasher empty;
    require_hex(
        empty.finalize(),
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855");

    const auto bytes = [](std::string_view value) {
        return std::as_bytes(std::span{value.data(), value.size()});
    };
    bioseq::common::sha256_hasher contiguous;
    contiguous.update(bytes("abc"));
    const direct_minimizer_content_digest contiguous_digest = contiguous.finalize();
    require_hex(
        contiguous_digest,
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");

    bioseq::common::sha256_hasher split;
    split.update(bytes("a"));
    split.update(bytes("b"));
    split.update(bytes("c"));
    REQUIRE(split.finalize() == contiguous_digest);

    //  promoted helpers: lowercase-hex rendering and the one-shot convenience
    // share the same digest and match the canonical "abc" vector.
    REQUIRE(bioseq::common::to_hex(contiguous_digest)
            == "ba7816bf8f01cfea414140de5dae2223"
               "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(bioseq::common::sha256_hex("abc")
            == "ba7816bf8f01cfea414140de5dae2223"
               "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(bioseq::common::sha256_hex("")
            == "e3b0c44298fc1c149afbf4c8996fb924"
               "27ae41e4649b934ca495991b7852b855");

    // update_little_endian defines the byte order used by the spill-run digest.
    bioseq::common::sha256_hasher little_endian;
    little_endian.update_little_endian(0x0807060504030201ull);
    bioseq::common::sha256_hasher raw_bytes;
    const std::array<std::byte, 8> le_expected{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
    raw_bytes.update(le_expected);
    REQUIRE(little_endian.finalize() == raw_bytes.finalize());
}

TEST_CASE("inactive spill-run storage incarnation rejects duplicate runs ABA and reuse",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, fragment_units>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;
    auto data = make_dna4(4096, 0x494E4341524E4154ULL);
    range_t range(data);

    SECTION("duplicate physical run advances the storage mutation revision") {
        lazy_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding before =
            storage.segment_spill_storage_binding();
        REQUIRE_FALSE(storage.segment_runs_.empty());
        const auto segment = storage.segment_runs_.begin()->first;
        REQUIRE_FALSE(storage.segment_runs_.begin()->second.empty());
        const auto duplicate = storage.segment_runs_.begin()->second.front();
        storage.spill_sorted_segment_run(segment, duplicate);
        const direct_minimizer_spill_storage_binding after =
            storage.segment_spill_storage_binding();
        REQUIRE(after.generation_token == before.generation_token);
        REQUIRE(after.mutation_revision == before.mutation_revision + 1u);
        collecting_minimizer_row_sink sink;
        minimizer_sequence_generation_execution_receipt execution;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                storage,
                sink,
                std::move(*spill),
                test_grouped_soa_generation_options(range.bp_size()),
                {},
                &execution),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE_FALSE(execution.is_internally_consistent());
        REQUIRE(sink.rows.empty());
    }

    SECTION("wrong storage and moved-from authority cannot be laundered") {
        lazy_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> original;
        fake_fragment_storage<> replacement;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            fragment_bp);
        REQUIRE(spill.has_value());
        collecting_minimizer_row_sink wrong_sink;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                replacement,
                wrong_sink,
                std::move(*spill),
                test_grouped_soa_generation_options(range.bp_size())),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(wrong_sink.rows.empty());

        collecting_minimizer_row_sink accepted_sink;
        minimizer_sequence_generation_execution_receipt execution;
        const auto accepted = spill_run_index_type::encode_persisted_generation(
            original,
            accepted_sink,
            std::move(*spill),
            test_grouped_soa_generation_options(range.bp_size()),
            {},
            &execution);
        REQUIRE(accepted.has_value());
        REQUIRE(execution.is_internally_consistent());
        REQUIRE(spill->run_count() == 0);
        REQUIRE(spill->target_length() == 0);
        REQUIRE(spill->position_segment_span_bp() == 0);
        REQUIRE_FALSE(spill->execution_receipt().is_spill_valid());

        collecting_minimizer_row_sink reused_sink;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                original,
                reused_sink,
                std::move(*spill),
                test_grouped_soa_generation_options(range.bp_size())),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(reused_sink.rows.empty());
    }

    SECTION("release and new begin invalidate even an empty authority") {
        std::vector<seq::dna4> empty_data;
        range_t empty_range(empty_data);
        lazy_sequence<dna4_u64, chunked> empty_lazy{
            chunked::from_range(empty_range)};
        const auto empty_lazy_view =
            bioseq::storage::make_lazy_sequence_view(empty_lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            empty_lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding issued =
            storage.segment_spill_storage_binding();
        REQUIRE(issued.valid());
        storage.release_runs_noexcept();
        const direct_minimizer_spill_storage_binding released =
            storage.segment_spill_storage_binding();
        REQUIRE(released.generation_token == issued.generation_token);
        REQUIRE(released.mutation_revision == issued.mutation_revision + 1u);
        const direct_minimizer_spill_storage_binding replacement =
            storage.begin_segment_spill_generation();
        REQUIRE(replacement.valid());
        REQUIRE(replacement.generation_token != issued.generation_token);
        collecting_minimizer_row_sink sink;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                storage,
                sink,
                std::move(*spill),
                test_grouped_soa_generation_options(0)),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("moving storage cannot preserve address-bound authority") {
        lazy_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> original;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding issued =
            original.segment_spill_storage_binding();
        fake_fragment_storage<> moved{std::move(original)};
        REQUIRE_FALSE(moved.segment_spill_storage_binding().valid());
        collecting_minimizer_row_sink sink;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                moved,
                sink,
                std::move(*spill),
                test_grouped_soa_generation_options(range.bp_size())),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("moving an empty spill invalidates the moved-from authority") {
        std::vector<seq::dna4> empty_data;
        range_t empty_range(empty_data);
        lazy_sequence<dna4_u64, chunked> empty_lazy{
            chunked::from_range(empty_range)};
        const auto empty_lazy_view =
            bioseq::storage::make_lazy_sequence_view(empty_lazy);
        fake_fragment_storage<> original;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original,
            empty_lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            fragment_bp);
        REQUIRE(spill.has_value());
        REQUIRE(original.segment_spill_storage_binding().valid());

        fake_fragment_storage<> moved{std::move(original)};
        REQUIRE_FALSE(original.segment_spill_storage_binding().valid());
        REQUIRE_FALSE(moved.segment_spill_storage_binding().valid());
        collecting_minimizer_row_sink sink;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                original,
                sink,
                std::move(*spill),
                test_grouped_soa_generation_options(0)),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("representation-only run compaction preserves logical incarnation") {
        lazy_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding before =
            storage.segment_spill_storage_binding();
        const std::uint64_t physical_runs_before =
            storage.segment_spill_run_count();
        REQUIRE(physical_runs_before > storage.segment_runs_.size());

        storage.compact_segment_spill_representation_preserving();
        REQUIRE(storage.segment_spill_storage_binding() == before);
        REQUIRE(storage.segment_spill_run_count() < physical_runs_before);

        collecting_minimizer_row_sink sink;
        minimizer_sequence_generation_execution_receipt execution;
        const auto encoded = spill_run_index_type::encode_persisted_generation(
            storage,
            sink,
            std::move(*spill),
            test_grouped_soa_generation_options(range.bp_size()),
            {},
            &execution);
        REQUIRE(encoded.has_value());
        REQUIRE(execution.is_internally_consistent());
        REQUIRE(execution.allows_ready_release(
            storage.segment_spill_storage_binding()));
        const auto segment = storage.segment_runs_.begin()->first;
        const auto post_encode_splice =
            storage.segment_runs_.begin()->second.front();
        storage.spill_sorted_segment_run(segment, post_encode_splice);
        REQUIRE_FALSE(execution.allows_ready_release(
            storage.segment_spill_storage_binding()));
        require_complete_generation_rows(*encoded, sink.rows);
    }

    SECTION("bound producer and consumed spill are single-use") {
        lazy_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            fragment_bp);
        REQUIRE(spill.has_value());
        auto producer = spill_run_index_type::make_generation_replay_producer(
            storage, range.bp_size(), std::move(*spill));
        collecting_minimizer_row_sink first_sink;
        const auto first = encode_minimizer_generation(
            producer,
            first_sink,
            test_generation_options(range.bp_size()));
        REQUIRE(first.has_value());
        collecting_minimizer_row_sink second_sink;
        require_wire_error(
            encode_minimizer_generation(
                producer,
                second_sink,
                test_generation_options(range.bp_size())),
            minimizer_generation_encoding_error::segment_span);

        auto moved_from_producer = spill_run_index_type::make_generation_replay_producer(
            storage, range.bp_size(), std::move(*spill));
        collecting_minimizer_row_sink moved_from_sink;
        require_wire_error(
            encode_minimizer_generation(
                moved_from_producer,
                moved_from_sink,
                test_generation_options(range.bp_size())),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(moved_from_sink.rows.empty());
    }
}

TEST_CASE("inactive spill-run storage incarnation counters fail closed at wrap",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    // The uncommon maximum chunk parameter gives this test an isolated static
    // token namespace, independent of every ordinary fake_fragment_storage<>.
    using wrap_storage_type = fake_fragment_storage<123457u>;

    wrap_storage_type::set_next_segment_spill_generation_token_for_test(
        (std::numeric_limits<std::uint64_t>::max)());
    wrap_storage_type last_valid;
    const direct_minimizer_spill_storage_binding maximum =
        last_valid.begin_segment_spill_generation();
    REQUIRE(maximum.valid());
    REQUIRE(maximum.generation_token
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(maximum.mutation_revision == 0);

    wrap_storage_type exhausted;
    REQUIRE_FALSE(exhausted.begin_segment_spill_generation().valid());
    REQUIRE_FALSE(exhausted.begin_segment_spill_generation().valid());

    last_valid.set_segment_spill_mutation_revision_for_test(
        (std::numeric_limits<std::uint64_t>::max)());
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> run{{
        {1u, 2u},
    }};
    (void)last_valid.spill_sorted_segment_run(0u, run);
    REQUIRE_FALSE(last_valid.segment_spill_storage_binding().valid());
}

TEST_CASE("minimizer index facades preserve biological rejection precedence",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    auto data = make_dna4(257u, 0x399500u);
    range_t range(data);
    auto owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto view = bioseq::storage::make_lazy_sequence_view(owner);

    sketch_collector exact_collector;
    try {
        (void)collect_exact_simd_index_range_or_throw(
            exact_collector,
            view,
            fixed_alphabet_code<::seq::dna4>(),
            2u,
            1u,
            10u,
            15u,
            0u,
            nullptr,
            "precedence probe");
        FAIL("Exact lazy facade accepted a reversed range");
    } catch (const exact_simd_index_runtime_error& error) {
        REQUIRE(error.status() == exact_simd_index_status::invalid_range);
    }

    sketch_collector bounded_collector;
    const auto bounded_lazy = collect_packed_bounded_nthash_index_lazy(
        view,
        fixed_alphabet_code<::seq::dna4>(),
        2u,
        1u,
        0u,
        10u,
        15u,
        nullptr,
        bounded_collector);
    REQUIRE(bounded_lazy.status
            == packed_bounded_nthash_index_status::invalid_range);
    REQUIRE(bounded_collector.extract_sketches_data().empty());

    const exact_simd_index_bridge_request unsupported_alphabet{
        .alphabet_code = 77u,
        .bits_per_symbol = 2u,
        .storage_unit_bytes = sizeof(std::uint32_t),
    };
    REQUIRE(collect_exact_simd_index_erased(unsupported_alphabet).status
            == exact_simd_index_status::unsupported_alphabet);
    auto unsupported_storage = unsupported_alphabet;
    unsupported_storage.alphabet_code =
        fixed_alphabet_code<::seq::dna4>();
    unsupported_storage.storage_unit_bytes = 3u;
    REQUIRE(collect_exact_simd_index_erased(unsupported_storage).status
            == exact_simd_index_status::unsupported_storage);
    auto missing_exact_state = unsupported_storage;
    missing_exact_state.storage_unit_bytes = sizeof(std::uint32_t);
    REQUIRE(collect_exact_simd_index_erased(missing_exact_state).status
            == exact_simd_index_status::invalid_state);

    packed_bounded_nthash_index_bridge_request bounded_request{
        .kernels = nullptr,
        .sequence = fixed_packed_sequence_view(range),
        .sequence_context = &range,
        .collector_context = &bounded_collector,
        .storage_unit_bytes = sizeof(std::uint64_t),
        .letters_per_unit = range_t::traits::letters_per_unit,
        .storage_units = range.size(),
        .local_bp_size = range.bp_size(),
        .window_size = 10u,
        .kmer_size = 15u,
    };
    auto invalid_configuration = bounded_request;
    invalid_configuration.window_size = 9u;
    invalid_configuration.sequence.alphabet_code = 77u;
    invalid_configuration.storage_unit_bytes = sizeof(std::uint16_t);
    REQUIRE(collect_packed_bounded_nthash_index_erased(
                invalid_configuration).status
            == packed_bounded_nthash_index_status::invalid_configuration);

    auto invalid_alphabet = bounded_request;
    invalid_alphabet.sequence.alphabet_code =
        fixed_alphabet_code<::seq::dna5>();
    invalid_alphabet.storage_unit_bytes = sizeof(std::uint16_t);
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_alphabet).status
            == packed_bounded_nthash_index_status::unsupported_alphabet);

    auto invalid_storage = bounded_request;
    invalid_storage.storage_unit_bytes = sizeof(std::uint16_t);
    invalid_storage.sequence.storage_unit_bytes = sizeof(std::uint16_t);
    invalid_storage.letters_per_unit = 8u;
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_storage).status
            == packed_bounded_nthash_index_status::unsupported_storage);

    auto invalid_range = bounded_request;
    invalid_range.sequence.end_symbol = range.bp_size() + 1u;
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_range).status
            == packed_bounded_nthash_index_status::invalid_range);
    REQUIRE(collect_packed_bounded_nthash_index_erased(bounded_request).status
            == packed_bounded_nthash_index_status::invalid_state);
}

TEST_CASE("bounded ntHash index stats aggregation rejects mixed source widths without rewriting identity",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    packed_bounded_nthash_index_execution_stats u32{
        .scheme_id = k_packed_bounded_nthash_kernel_scheme_id,
        .kmer_size = 15u,
        .window_size = 10u,
        .nthash_rotation = 7u,
        .source_storage_unit_bytes = sizeof(std::uint32_t),
        .segments_combined = 1u,
        .logical_streams = 8u,
        .physical_vector_lanes = 4u,
        .vector_registers_per_step = 2u,
        .positions_are_segment_local = true,
    };
    auto second_u32 = u32;
    auto same_width = u32;
    same_width.merge(second_u32);
    REQUIRE_FALSE(same_width.aggregation_mismatch);
    REQUIRE(same_width.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE(same_width.segments_combined == 2u);

    auto u64 = u32;
    u64.source_storage_unit_bytes = sizeof(std::uint64_t);
    auto mixed_width = u32;
    mixed_width.merge(u64);
    REQUIRE(mixed_width.aggregation_mismatch);
    REQUIRE(mixed_width.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE_FALSE(mixed_width.is_internally_consistent(false));
}

TEST_CASE("bounded ntHash lazy index admits resident u64 through one canonical u32 window",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    auto data = make_dna4(4096u, 0x5533325041504552ULL);
    range_t range(data);
    auto owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto view = bioseq::storage::make_lazy_sequence_view(owner);
    REQUIRE(view.try_resident_packed(
        0u, view.bp_size(), sizeof(std::uint64_t)).has_value());
    REQUIRE_FALSE(view.try_resident_packed(
        0u, view.bp_size(), sizeof(std::uint32_t)).has_value());

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t admission_before = admission.reserved_bytes();
    sketch_collector collector;
    std::uint64_t materialized_window_bp = 0u;
    const auto result = collect_packed_bounded_nthash_index_lazy(
        view,
        fixed_alphabet_code<::seq::dna4>(),
        0u,
        view.bp_size(),
        view.bp_size(),
        10u,
        15u,
        fixed_minimizer_kernels(),
        collector,
        &materialized_window_bp);
    if (result.status
        == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        REQUIRE(admission.reserved_bytes() == admission_before);
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(result.stats.is_internally_consistent(true));
    REQUIRE(result.stats.source_storage_unit_bytes
        == sizeof(std::uint32_t));
    REQUIRE(materialized_window_bp == view.bp_size());
    REQUIRE_FALSE(collector.extract_sketches_data().empty());
    REQUIRE(admission.reserved_bytes() == admission_before);
}

TEST_CASE("Vulkan fragment build records accepted operation-originated batched stats",
          "[component:bioseq][layer:unit][feature:storage][resource:gpu]") {
    using namespace bioseq::indexing;
    if (!::bioseq::gpu::vulkan::hardware_device_available(
            ::bioseq::gpu::vulkan::runtime::instance())) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::size_t source_fragment_units = 64u;
    constexpr std::size_t persisted_fragment_bp = 32'768u;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, source_fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    auto data = make_dna4(98'417u, 0x4D504656554C4B4EULL);
    range_t range(data);
    lazy_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    fake_fragment_storage<> storage;
    storage.run_buffer_bytes_ = 64u << 20u;
    minimizer_fragment_generation_execution_receipt evidence;
    index_type::build(
        storage,
        view,
        minimizer_sketch_backend::vulkan_segment,
        nullptr,
        &evidence);

    REQUIRE_FALSE(evidence.is_accepted());
    REQUIRE(evidence.direct_build_accepted());
    REQUIRE(evidence.selected_backend
        == minimizer_sketch_backend::vulkan_segment);
    REQUIRE(evidence.backend_bound);
    REQUIRE(evidence.target_length == range.bp_size());
    REQUIRE(evidence.scalar_core_runs == 0u);
    REQUIRE(evidence.exact_simd_core_runs == 0u);
    REQUIRE(evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(evidence.vulkan_core_runs > 0u);
    REQUIRE(evidence.vulkan_stats.is_internally_consistent(true));
    REQUIRE(evidence.vulkan_stats.completed_roundtrips
        == evidence.vulkan_core_runs);
    const std::size_t expected_fragments = static_cast<std::size_t>(
        (range.bp_size() + persisted_fragment_bp - 1u)
        / persisted_fragment_bp);
    REQUIRE(storage.number_of_chunks(k_directory_sub_key) == 1u);
    REQUIRE(*storage.template read_chunk<std::int64_t>(
        k_directory_sub_key, 0u)
        == static_cast<std::int64_t>(expected_fragments));
}

TEST_CASE("bounded ntHash fragment replay matches one checked resident execution",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = 32'768u;
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    auto data = make_dna4(70013, 0x50415045524E5448ULL);
    range_t range(data);
    sketch_collector resident_collector;
    const auto resident_result = collect_packed_bounded_nthash_index_erased(
        packed_bounded_nthash_index_bridge_request{
            .kernels = fixed_minimizer_kernels(),
            .sequence = fixed_packed_sequence_view(range),
            .sequence_context = &range,
            .collector_context = &resident_collector,
            .storage_unit_bytes = sizeof(std::uint64_t),
            .letters_per_unit = range_t::traits::letters_per_unit,
            .storage_units = range.size(),
            .local_bp_size = range.bp_size(),
            .window_size = 10,
            .kmer_size = 15,
        });
    if (resident_result.status
        == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(resident_result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(resident_result.stats.is_internally_consistent(true));

    std::vector<canonical_minimizer_event> oracle;
    oracle.reserve(resident_collector.extract_sketches_data().size());
    for (const minimap_sketch_data& minimizer :
         resident_collector.extract_sketches_data()) {
        oracle.push_back({
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    std::ranges::sort(oracle, {}, [](const canonical_minimizer_event& event) {
        return std::pair{event.hash, event.encoded_position};
    });

    lazy_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto producer = index_type::make_generation_replay_producer(
        lazy_view,
        minimizer_sketch_backend::bounded_nthash_simd,
        fixed_minimizer_kernels(),
        fixed_alphabet_code<::seq::dna4>());
    auto options = test_generation_options(range.bp_size());
    options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
    options.minimizer_scheme_revision =
        k_bounded_nthash_u32_r7_sum_scheme_revision;
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(producer.scan_count() == 3);
    const minimizer_fragment_generation_execution_receipt execution =
        producer.execution_stats();
    REQUIRE(execution.is_accepted());
    REQUIRE(execution.selected_backend
        == minimizer_sketch_backend::bounded_nthash_simd);
    REQUIRE(execution.scalar_core_runs == 0);
    REQUIRE(execution.exact_simd_core_runs == 0);
    REQUIRE(execution.bounded_nthash_core_runs > 0);
    REQUIRE(execution.vulkan_core_runs == 0);
    REQUIRE(execution.packed_bounded_stats_present);
    REQUIRE(execution.packed_bounded_status
        == packed_bounded_nthash_index_status::ok);
    REQUIRE(execution.packed_bounded_stats.is_internally_consistent(true));
    REQUIRE(execution.packed_bounded_stats.fallback_calls == 0);
    REQUIRE(execution.packed_bounded_stats.source_storage_unit_bytes
        == sizeof(std::uint32_t));
    REQUIRE(producer.scheme_identity()
        == k_bounded_nthash_u32_r7_sum_scheme_identity);
    REQUIRE(producer.packed_bounded_stats_present());
    REQUIRE(producer.packed_bounded_status()
        == packed_bounded_nthash_index_status::ok);
    REQUIRE(producer.packed_bounded_stats().is_internally_consistent(true));
    REQUIRE(producer.packed_bounded_stats().fallback_calls == 0);
    REQUIRE_FALSE(producer.exact_simd_stats_present());
    REQUIRE(producer.peak_bounded_materialized_window_bp()
        <= fragment_bp + 2u * halo
            + range_t::traits::letters_per_unit - 1u);
    REQUIRE(producer.peak_bounded_materialized_window_bp()
        < range.bp_size());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == oracle);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("bounded ntHash fragment canonicalizes tied periodic events identically across replays",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    // This is the periodic shape that exposed duplicate_event in the live fragment
    // coordinator. Tied adjacent windows can report the same canonical bounded ntHash
    // minimizer more than once, including after intervening raw records.
    std::vector<seq::dna4> data(65565);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index & 3u));
    }
    range_t range(data);
    sketch_collector raw_collector;
    const auto raw_result = collect_packed_bounded_nthash_index_erased(
        packed_bounded_nthash_index_bridge_request{
            .kernels = fixed_minimizer_kernels(),
            .sequence = fixed_packed_sequence_view(range),
            .sequence_context = &range,
            .collector_context = &raw_collector,
            .storage_unit_bytes = sizeof(std::uint64_t),
            .letters_per_unit = range_t::traits::letters_per_unit,
            .storage_units = range.size(),
            .local_bp_size = range.bp_size(),
            .window_size = 10,
            .kmer_size = 15,
        });
    if (raw_result.status
        == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(raw_result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(raw_result.stats.is_internally_consistent(true));

    std::vector<canonical_minimizer_event> raw;
    raw.reserve(raw_collector.extract_sketches_data().size());
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> first_seen;
    bool saw_nonadjacent_duplicate = false;
    for (const minimap_sketch_data& minimizer :
         raw_collector.extract_sketches_data()) {
        const canonical_minimizer_event event{
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        };
        const auto key = std::pair{event.hash, event.encoded_position};
        if (const auto found = first_seen.find(key);
            found != first_seen.end()) {
            saw_nonadjacent_duplicate |= raw.size() > found->second + 1u;
        } else {
            first_seen.emplace(key, raw.size());
        }
        raw.push_back(event);
    }
    auto canonical = raw;
    std::ranges::sort(canonical, {}, [](const canonical_minimizer_event& event) {
        return std::pair{event.hash, event.encoded_position};
    });
    canonical.erase(
        std::ranges::unique(canonical).begin(), canonical.end());
    REQUIRE(raw.size() > canonical.size());
    REQUIRE(saw_nonadjacent_duplicate);

    lazy_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto replay = index_type::make_generation_replay_producer(
        lazy_view,
        minimizer_sketch_backend::bounded_nthash_simd,
        fixed_minimizer_kernels(),
        fixed_alphabet_code<::seq::dna4>());
    const auto collect_replay = [](void* context,
            const canonical_minimizer_event& event)
            -> std::expected<void, minimizer_generation_encoding_error> {
        static_cast<std::vector<canonical_minimizer_event>*>(context)
            ->push_back(event);
        return {};
    };
    std::vector<canonical_minimizer_event> first_replay;
    std::vector<canonical_minimizer_event> second_replay;
    REQUIRE(replay.scan_sorted_events(
        k_minimizer_preferred_segment_span_bp,
        {.context = &first_replay, .emit_fn = collect_replay}).has_value());
    REQUIRE(replay.scan_sorted_events(
        k_minimizer_preferred_segment_span_bp,
        {.context = &second_replay, .emit_fn = collect_replay}).has_value());
    REQUIRE(first_replay == canonical);
    REQUIRE(second_replay == canonical);
    REQUIRE(first_replay == second_replay);
    REQUIRE(replay.scan_count() == 2u);
    REQUIRE(replay.execution_stats().is_accepted());
    REQUIRE(replay.execution_stats().selected_backend
        == minimizer_sketch_backend::bounded_nthash_simd);
    REQUIRE(replay.execution_stats().scalar_core_runs == 0u);
    REQUIRE(replay.execution_stats().exact_simd_core_runs == 0u);
    REQUIRE(replay.execution_stats().vulkan_core_runs == 0u);
    REQUIRE(replay.execution_stats().bounded_nthash_core_runs > 0u);

    auto encoder_replay = index_type::make_generation_replay_producer(
        lazy_view,
        minimizer_sketch_backend::bounded_nthash_simd,
        fixed_minimizer_kernels(),
        fixed_alphabet_code<::seq::dna4>());
    auto options = test_generation_options(range.bp_size());
    options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
    options.minimizer_scheme_revision =
        k_bounded_nthash_u32_r7_sum_scheme_revision;
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(
        encoder_replay, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(encoder_replay.scan_count() == 3u);
    REQUIRE(encoder_replay.execution_stats().is_accepted());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == canonical);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("bounded ntHash fragment replay canonicalizes storage and rejects incompatible biological shapes",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    SECTION("DNA5 fails closed without scalar substitution") {
        using chunked = chunked_span_sequence_scanner<std::uint64_t, 64>;
        std::vector<seq::dna5> data(4096);
        for (std::size_t index = 0; index < data.size(); ++index) {
            data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
        }
        dna5_range_t range(data);
        lazy_sequence<dna5_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        auto producer = index_type::make_generation_replay_producer(
            lazy_view,
            minimizer_sketch_backend::bounded_nthash_simd,
            fixed_minimizer_kernels(),
            fixed_alphabet_code<::seq::dna5>());
        auto options = test_generation_options(range.bp_size());
        options.alphabet_code = 5;
        options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
        options.minimizer_scheme_revision =
            k_bounded_nthash_u32_r7_sum_scheme_revision;
        collecting_minimizer_row_sink sink;
        const std::uint64_t admission_before =
            bioseq::parallelism::memory_admission_controller::instance()
                .reserved_bytes();
        require_wire_error(
            encode_minimizer_generation(producer, sink, options),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
        REQUIRE(producer.scan_count() == 0u);
        REQUIRE_FALSE(producer.packed_bounded_stats_present());
        REQUIRE_FALSE(producer.exact_simd_stats_present());
        REQUIRE(bioseq::parallelism::memory_admission_controller::instance()
            .reserved_bytes() == admission_before);
    }

    SECTION("u16 packed storage is admitted into one canonical u32 bounded ntHash window") {
        using chunked = chunked_span_sequence_scanner<std::uint16_t, 64>;
        auto data = make_dna4(4096, 0x16B17u);
        dna4_u16_range_t range(data);
        lazy_sequence<dna4_u16, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        auto producer = index_type::make_generation_replay_producer(
            lazy_view,
            minimizer_sketch_backend::bounded_nthash_simd,
            fixed_minimizer_kernels(),
            fixed_alphabet_code<::seq::dna4>());
        auto options = test_generation_options(range.bp_size());
        options.storage_unit_byte_width = sizeof(std::uint16_t);
        options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
        options.minimizer_scheme_revision =
            k_bounded_nthash_u32_r7_sum_scheme_revision;
        collecting_minimizer_row_sink sink;
        const auto encoded = encode_minimizer_generation(
            producer, sink, options);
        REQUIRE(encoded.has_value());
        REQUIRE_FALSE(sink.rows.empty());
        REQUIRE(producer.packed_bounded_stats_present());
        REQUIRE(producer.packed_bounded_status()
            == packed_bounded_nthash_index_status::ok);
        REQUIRE(producer.packed_bounded_stats().is_internally_consistent(true));
        REQUIRE(producer.packed_bounded_stats().fallback_calls == 0u);
        REQUIRE(producer.peak_bounded_materialized_window_bp() > 0u);
        REQUIRE(producer.execution_stats().is_accepted());
        REQUIRE_FALSE(producer.exact_simd_stats_present());
        require_complete_generation_rows(*encoded, sink.rows);
    }

    SECTION("a local position wider than minimap_sketch_data fails before dereference") {
        std::byte sentinel{};
        sketch_collector collector;
        const std::size_t local_bp_size = static_cast<std::size_t>(
            minimap_sketch_data::k_position_field_mask) + 2u;
        const std::size_t storage_units =
            (local_bp_size + range_t::traits::letters_per_unit - 1u)
            / range_t::traits::letters_per_unit;
        const auto rejected = collect_packed_bounded_nthash_index_erased(
            packed_bounded_nthash_index_bridge_request{
                .kernels = fixed_minimizer_kernels(),
                .sequence = {
                    .storage = &sentinel,
                    .storage_units = storage_units,
                    .begin_symbol = 0u,
                    .end_symbol = local_bp_size,
                    .alphabet_code = fixed_alphabet_code<::seq::dna4>(),
                    .bits_per_symbol = static_cast<std::uint8_t>(
                        range_t::traits::bits_per_char),
                    .storage_unit_bytes = sizeof(std::uint64_t),
                    .final_segment = true,
                },
                .sequence_context = &sentinel,
                .collector_context = &collector,
                .storage_unit_bytes = sizeof(std::uint64_t),
                .letters_per_unit = range_t::traits::letters_per_unit,
                .storage_units = storage_units,
                .local_bp_size = local_bp_size,
                .window_size = 10,
                .kmer_size = 15,
            });
        REQUIRE(rejected.status
            == packed_bounded_nthash_index_status::unsupported_representation);
        REQUIRE(rejected.stats.fail_closed_rejections == 1);
        REQUIRE(collector.extract_sketches_data().empty());
    }
}

TEST_CASE("actual producers cross the persisted 8MiB core edge bounded",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = k_minimizer_preferred_segment_span_bp;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    auto data = make_dna4(static_cast<std::size_t>(span + 8192u + 7u),
                          0x8B1B0A4DULL);
    // A mirror-symmetric boundary fixture with a homopolymer centre exercises
    // palindrome strand ties and rightmost equal-minimum selection on both
    // sides of the immutable persisted-core edge.
    for (std::size_t offset = 0; offset < 192; ++offset) {
        seq::dna4 value{};
        value.assign_rank(static_cast<std::uint8_t>((offset * 3u) & 3u));
        data[static_cast<std::size_t>(span) - 1u - offset] = value;
        data[static_cast<std::size_t>(span) + offset] = value;
    }
    for (std::size_t position = static_cast<std::size_t>(span) - 24u;
         position < static_cast<std::size_t>(span) + 24u;
         ++position) {
        data[position].assign_rank(0);
    }
    range_t range(data);
    lazy_sequence<dna4_u64, chunked> lazy{
        chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);

    auto fragment_producer = index_type::make_generation_replay_producer(
        lazy_view, minimizer_sketch_backend::scalar, nullptr);
    const auto options = test_generation_options(range.bp_size());
    const auto fragment_plan = plan_minimizer_generation(fragment_producer, options);
    REQUIRE(fragment_plan.has_value());
    REQUIRE(fragment_plan->position_segment_count == 2);
    REQUIRE(fragment_producer.peak_buffered_core_bp() <= span);
    REQUIRE(fragment_producer.scan_count() == 1);

    fake_fragment_storage<> spill_storage;
    auto spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        spill_storage,
        lazy_view,
        minimizer_sketch_backend::scalar,
        nullptr,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count() > 256);
    REQUIRE(spill_storage.runs_.empty());
    REQUIRE(spill_storage.segment_spill_run_count() == spilled->run_count());
    REQUIRE(spill_storage.segment_runs_.size() == 2);
    constexpr std::uint64_t half_span = span / 2u;
    std::vector<std::uint64_t> half_counts(
        range.bp_size() / half_span
            + (range.bp_size() % half_span != 0 ? 1u : 0u));
    for (const auto& [segment, runs] : spill_storage.segment_runs_) {
        (void)segment;
        for (const auto& run : runs) {
            for (const auto& event : run) {
                ++half_counts[(event.second >> 1u) / half_span];
            }
        }
    }
    REQUIRE(half_counts.size() == 3);
    REQUIRE(half_counts[0] > 0);
    REQUIRE(half_counts[1] > 0);
    const std::uint64_t half_max =
        *std::ranges::max_element(half_counts);
    REQUIRE(half_counts[0] + half_counts[1] > half_max);
    grouped_soa_generation_limits one_half_per_segment;
    one_half_per_segment.max_segment_position_count = half_max;
    collecting_minimizer_row_sink spill_sink;
    minimizer_sequence_generation_execution_receipt execution;
    const auto encoded = spill_run_index_type::encode_persisted_generation(
        spill_storage,
        spill_sink,
        std::move(*spilled),
        test_grouped_soa_generation_options(range.bp_size()),
        one_half_per_segment,
        &execution);
    REQUIRE(encoded.has_value());
    REQUIRE(execution.is_internally_consistent());
    REQUIRE(execution.replay_scan_count == 4);
    REQUIRE(execution.replay_merge_pass_count == 4);
    REQUIRE(execution.replay_reroute_pass_count == 1);
    REQUIRE(execution.replay_failed_scan_count == 1);
    REQUIRE(execution.replay_binding_checked_failed_scan_count == 1);
    REQUIRE(execution.replay_completed_scan_count == 3);
    REQUIRE(execution.replay_first_completed_content_digest
        == execution.replay_last_completed_content_digest);
    REQUIRE(execution.replay_final_span_bp == half_span);
    REQUIRE(execution.replay_final_storage_binding.generation_token
        == execution.spill_storage_binding.generation_token);
    REQUIRE(execution.replay_final_storage_binding.mutation_revision
        == execution.spill_storage_binding.mutation_revision + 1u);
    REQUIRE(encoded->manifest.position_segment_span_bp == half_span);
    REQUIRE(encoded->manifest.position_segment_count == 3);
    REQUIRE(encoded->manifest.position_count
        == fragment_plan->canonical_event_count);
    REQUIRE(spill_storage.segment_reroute_calls() == 1);
    REQUIRE(spill_storage.segment_merge_calls() == 5);
    require_complete_generation_rows(*encoded, spill_sink.rows);
}

TEST_CASE("actual producers preserve DNA5 N reset and packed tail",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::uint64_t fragment_bp = fragment_units * 21u;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    constexpr std::size_t target_size = 70013;
    std::vector<seq::dna5> data(target_size);
    std::uint64_t state = 0xD5A5C0DEULL;
    std::vector<std::size_t> n_positions;
    for (std::size_t index = 0; index < data.size(); ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        std::uint8_t rank = static_cast<std::uint8_t>((state >> 33u) % 4u);
        if (index % 211u == 97u) {
            rank = 3u; // dna5 N: rolling hash/window reset sentinel
            n_positions.push_back(index);
        } else if (rank == 3u) {
            rank = 4u; // T is the fifth dna5 rank, but normal nt4 base 3
        }
        data[index].assign_rank(rank);
    }
    for (const std::size_t boundary : {
            static_cast<std::size_t>(fragment_bp - 1u),
            static_cast<std::size_t>(fragment_bp),
            target_size - 80u}) {
        data[boundary].assign_rank(3u);
        n_positions.push_back(boundary);
    }
    dna5_range_t range(data);
    REQUIRE(range.padding() != 0);

    std::vector<canonical_minimizer_event> oracle =
        baseline_dna5_events(data, 10, 15);
    std::ranges::sort(oracle, {}, [](const canonical_minimizer_event& event) {
        return std::pair{event.hash, event.encoded_position};
    });

    lazy_sequence<dna5_u64, chunked> lazy{
        chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto collection_range = spill_run_index_type::make_generation_collection_range(
        lazy_view,
        fragment_bp,
        fragment_bp + 100u);
    REQUIRE(collection_range.has_value());
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    REQUIRE(collection_range->cs_u == fragment_bp - halo);
    REQUIRE(collection_range->ce_u == fragment_bp + 100u + halo);
    REQUIRE(collection_range->core_lo_bp == fragment_bp);
    REQUIRE(collection_range->core_hi_bp == fragment_bp + 100u);
    auto fragment_producer = index_type::make_generation_replay_producer(
        lazy_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        fixed_alphabet_code<::seq::dna5>());
    const auto collect_replay = [](void* context,
            const canonical_minimizer_event& event)
            -> std::expected<void, minimizer_generation_encoding_error> {
        static_cast<std::vector<canonical_minimizer_event>*>(context)
            ->push_back(event);
        return {};
    };
    std::vector<canonical_minimizer_event> fragment_events;
    REQUIRE(fragment_producer.scan_sorted_events(
        k_minimizer_preferred_segment_span_bp,
        {.context = &fragment_events, .emit_fn = collect_replay}).has_value());

    fake_fragment_storage<> spill_storage;
    auto spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        spill_storage,
        lazy_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        fragment_bp,
        k_minimizer_preferred_segment_span_bp,
        nullptr,
        fixed_alphabet_code<::seq::dna5>());
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->exact_simd_stats_available());
    const exact_simd_index_execution_stats spill_exact_stats =
        spilled->exact_simd_stats();
    auto spill_producer = spill_run_index_type::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spilled));
    std::vector<canonical_minimizer_event> spill_events;
    REQUIRE(spill_producer.scan_sorted_events(
        k_minimizer_preferred_segment_span_bp,
        {.context = &spill_events, .emit_fn = collect_replay}).has_value());
    REQUIRE(fragment_producer.exact_simd_stats_present());
    REQUIRE(fragment_producer.exact_simd_stats().is_internally_consistent(true));
    REQUIRE(fragment_producer.exact_simd_stats().ambiguous_symbols > 0);
    REQUIRE(spill_exact_stats.is_internally_consistent(true));
    REQUIRE(spill_exact_stats.ambiguous_symbols > 0);
    REQUIRE(spill_producer.merge_pass_count() == 1);
    REQUIRE(spill_storage.segment_merge_calls() == 2);
    REQUIRE(spill_storage.segment_merge_run_visits()
        == 2u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited()
        == 2u * spill_storage.segment_spill_bytes());
    REQUIRE(fragment_events == oracle);
    REQUIRE(spill_events == oracle);
    REQUIRE(std::ranges::any_of(fragment_events, [](const auto& event) {
        return (event.encoded_position >> 1u) > target_size - 40u;
    }));
    const auto baseline_lookup = canonical_event_lookup_map(oracle);
    REQUIRE(canonical_event_lookup_map(fragment_events) == baseline_lookup);
    REQUIRE(canonical_event_lookup_map(spill_events) == baseline_lookup);
    for (const canonical_minimizer_event& event : fragment_events) {
        const std::uint64_t last = event.encoded_position >> 1u;
        const std::uint64_t first = last + 1u >= 15u
            ? last + 1u - 15u : 0u;
        REQUIRE(std::ranges::none_of(n_positions, [first, last](std::size_t n) {
            return first <= n && n <= last;
        }));
    }
}

TEST_CASE("segment overflow retry halves deterministically",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t preferred = k_minimizer_preferred_segment_span_bp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> distributed{
        {1, 2u},
        {2, 4u},
        {3, ((preferred / 2u + 1u) << 1u)},
        {4, ((preferred / 2u + 2u) << 1u)},
    };
    const grouped_soa_generation_limits two_per_core{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 2,
    };
    vector_replay_minimizer_producer producer{distributed};
    const auto selected = plan_minimizer_generation(
        producer, test_generation_options(preferred), two_per_core);
    REQUIRE(selected.has_value());
    REQUIRE(selected->position_segment_span_bp == preferred / 2u);
    REQUIRE(producer.scan_count == 2);

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> indivisible{
        {9, 2u}, {9, 3u},
    };
    const grouped_soa_generation_limits one_per_hash{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    vector_replay_minimizer_producer impossible_producer{indivisible};
    const auto impossible = plan_minimizer_generation(
        impossible_producer,
        test_generation_options(preferred),
        one_per_hash);
    require_wire_error(impossible, minimizer_generation_encoding_error::element_count);
}

TEST_CASE("unbound spill-run replay cannot claim a producer backend",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t preferred = k_minimizer_preferred_segment_span_bp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> run{
        {1, 2u},
        {2, 4u},
        {3, ((preferred / 2u + 1u) << 1u)},
        {4, ((preferred / 2u + 2u) << 1u)},
    };
    fake_fragment_storage<> storage;
    storage.spill_sorted_segment_run(
        0,
        std::span<const std::pair<std::uint64_t, std::uint64_t>>{run});
    const std::uint64_t spill_bytes = storage.segment_spill_bytes();
    REQUIRE(spill_bytes == run.size() * sizeof(run.front()));

    direct_minimizer_unbound_external_spill_replay_producer<fake_fragment_storage<>> producer{
        storage,
        preferred,
        preferred,
        k_minimap_scheme_identity};
    collecting_minimizer_row_sink sink;
    const grouped_soa_generation_limits two_per_core{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    const auto encoded = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(preferred),
        two_per_core);
    require_wire_error(
        encoded, minimizer_generation_encoding_error::storage_contract_mismatch);
    const minimizer_sequence_generation_execution_receipt execution =
        producer.execution_receipt();
    REQUIRE_FALSE(execution.is_internally_consistent());
    REQUIRE(execution.replay_scan_count == 0);
    REQUIRE(execution.replay_merge_pass_count == 0);
    REQUIRE(execution.replay_reroute_pass_count == 0);
    REQUIRE(execution.replay_completed_scan_count == 0);
    REQUIRE(execution.replay_failed_scan_count == 0);
    REQUIRE(execution.replay_illegal_span_transition_count == 0);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
    REQUIRE(storage.segment_spill_bytes() == spill_bytes);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("raw spill-run replay rejects a non-halving span transition",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t preferred = k_minimizer_preferred_segment_span_bp;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> run{{
        {1u, 2u},
    }};
    fake_fragment_storage<> storage;
    storage.spill_sorted_segment_run(0, run);
    direct_minimizer_unbound_external_spill_replay_producer<fake_fragment_storage<>> producer{
        storage,
        preferred,
        preferred,
        k_minimap_scheme_identity};
    int receiver_context = 0;
    const auto rejected = producer.scan_sorted_events(
        preferred / 4u,
        canonical_minimizer_event_receiver{
            .context = &receiver_context,
            .emit_fn = +[](void*, const canonical_minimizer_event&)
                -> std::expected<void, minimizer_generation_encoding_error> {
                return {};
            },
        });
    require_wire_error(
        rejected, minimizer_generation_encoding_error::segment_span);
    REQUIRE(producer.scan_count() == 1);
    REQUIRE(producer.merge_pass_count() == 0);
    REQUIRE(producer.reroute_pass_count() == 0);
    REQUIRE(producer.failed_scan_count() == 1);
    REQUIRE(producer.illegal_span_transition_count() == 1);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
}

TEST_CASE("empty unbound spill-run replay also fails producer identity",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    fake_fragment_storage<> storage;
    direct_minimizer_unbound_external_spill_replay_producer<fake_fragment_storage<>> producer{
        storage,
        0,
        k_minimizer_preferred_segment_span_bp,
        k_minimap_scheme_identity};
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_generation_options(0));
    require_wire_error(
        encoded, minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(producer.scan_count() == 0);
    REQUIRE(producer.merge_pass_count() == 0);
    REQUIRE(producer.reroute_pass_count() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("exact duplicate canonical events fail before row publication",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> duplicated{
        {7, 20}, {7, 20},
    };
    vector_replay_minimizer_producer producer{duplicated};
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_generation_options(100));
    require_wire_error(encoded, minimizer_generation_encoding_error::duplicate_event);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("encoder rejects a drifting replay before READY publication",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    drifting_replay_minimizer_producer producer;
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_generation_options(100u));
    require_wire_error(
        encoded, minimizer_generation_encoding_error::replay_mismatch);
    REQUIRE(producer.scan_count == 2u);
    REQUIRE(std::ranges::none_of(sink.rows, [](const auto& row) {
        return row.address.sub_key == k_directory_sub_key;
    }));
}

TEST_CASE("grouped-SoA rejects scheme and backend drift at every scan boundary",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    for (const identity_drift_axis axis : {
             identity_drift_axis::scheme,
             identity_drift_axis::backend}) {
        for (const std::size_t boundary : {1u, 2u, 3u}) {
            INFO("axis=" << static_cast<unsigned>(axis)
                << " completed scan=" << boundary);
            boundary_identity_drifting_producer producer{
                .drift_after_scan = boundary,
                .axis = axis,
            };
            collecting_minimizer_row_sink sink;
            const auto encoded = encode_minimizer_generation(
                producer,
                sink,
                test_grouped_soa_generation_options(100u, 10u));
            require_wire_error(
                encoded, minimizer_generation_encoding_error::replay_mismatch);
            REQUIRE(producer.scan_count == boundary);
            REQUIRE(std::ranges::none_of(sink.rows, [](const auto& row) {
                return row.address.sub_key == k_directory_sub_key;
            }));
        }
    }
}

TEST_CASE("heavy hash is never split across partitions or page extents",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t target_length = 100000;
    constexpr std::uint64_t heavy_position_count = 70000;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(heavy_position_count + 1u);
    for (std::uint64_t index = 0; index < heavy_position_count; ++index) {
        minimizers.emplace_back(42, (index << 1u) | (index & 1u));
    }
    minimizers.emplace_back(43, 90000u << 1u);
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    auto generation = encode_minimizer_generation(
        producer, sink, test_generation_options(target_length));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 2);
    const auto& heavy = generation->directory.entries()[0];
    REQUIRE(heavy.lo_hash == 42);
    REQUIRE(heavy.hi_hash == 42);
    REQUIRE(heavy.control_element_count == 1);
    REQUIRE(heavy.position_element_count == heavy_position_count);
    REQUIRE(heavy.position_encoded_bytes > k_minimizer_partition_target_bytes);
    REQUIRE(heavy.position_chunk_count > 1);
    const auto heavy_positions = collect_encoded_extent(
        sink.rows, heavy, false);
    REQUIRE(heavy_positions.size()
        == heavy_position_count * static_cast<std::uint8_t>(
            generation->directory.profile().position_width));
    REQUIRE(generation->directory.entries()[1].lo_hash == 43);
    REQUIRE(sink.max_callback_bytes == k_grouped_soa_extent_payload_bytes);
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("grouped-SoA derives exact H P C O boundaries and SoA offsets",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr auto shape = [](std::uint16_t k, std::uint64_t span,
                              std::uint64_t cap) {
        return derive_grouped_soa_minimizer_profile(
            k_minimap_scheme_identity,
            k_minimizer_persisted_dna4_alphabet_code,
            k,
            span,
            cap);
    };
    STATIC_REQUIRE(shape(8u, 32'768u, 65'534u)->hash_width
        == minimizer_scalar_width::u16);
    STATIC_REQUIRE(shape(9u, 32'768u, 65'534u)->hash_width
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(15u, 32'768u, 65'534u)->hash_width
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(16u, 32'768u, 65'534u)->hash_width
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(17u, 32'768u, 65'534u)->hash_width
        == minimizer_scalar_width::u64);
    STATIC_REQUIRE(shape(28u, 32'768u, 65'534u).has_value());
    STATIC_REQUIRE_FALSE(shape(29u, 32'768u, 65'534u).has_value());
    STATIC_REQUIRE_FALSE(derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity, 1u, 15u, 32'768u, 65'534u)
        .has_value());
    STATIC_REQUIRE_FALSE(derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity, 4u, 15u, 32'768u, 65'534u)
        .has_value());
    STATIC_REQUIRE(derive_grouped_soa_minimizer_profile(
        k_bounded_nthash_u32_r7_sum_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        15u, 32'768u, 65'534u)->hash_width
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE_FALSE(derive_grouped_soa_minimizer_profile(
        k_bounded_nthash_u32_r7_sum_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        14u, 32'768u, 65'534u).has_value());
    STATIC_REQUIRE_FALSE(derive_grouped_soa_minimizer_profile(
        k_bounded_nthash_u32_r7_sum_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        16u, 32'768u, 65'534u).has_value());

    constexpr auto scalar_expected =
        expected_grouped_soa_generation_identity_for_backend(
            minimizer_generation_layout::grouped_soa_segment_local,
            minimizer_sketch_backend::scalar,
            k_minimizer_persisted_dna4_alphabet_code,
            8u,
            15u,
            10u,
            65'534u);
    STATIC_REQUIRE(scalar_expected.has_value());
    constexpr auto exact_simd_expected =
        expected_grouped_soa_generation_identity_for_backend(
            minimizer_generation_layout::grouped_soa_segment_local,
            minimizer_sketch_backend::exact_simd,
            k_minimizer_persisted_dna4_alphabet_code,
            8u,
            15u,
            10u,
            65'534u);
    STATIC_REQUIRE(exact_simd_expected.has_value());
    STATIC_REQUIRE(shape(15u, 32'768u, 65'534u)->position_width
        == minimizer_scalar_width::u16);
    STATIC_REQUIRE(shape(15u, 32'769u, 65'534u)->position_width
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(15u, std::uint64_t{1} << 31u, 65'534u)
        ->position_width == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(15u, (std::uint64_t{1} << 31u) + 1u, 65'534u)
        ->position_width == minimizer_scalar_width::u64);
    STATIC_REQUIRE(shape(15u, 1u, 65'534u)->count_width
        == minimizer_scalar_width::u16);
    STATIC_REQUIRE(shape(15u, 1u, 65'535u)->count_width
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(
        15u, 1u,
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)()) - 1u)->count_width
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE(shape(
        15u, 1u,
        (std::numeric_limits<std::uint32_t>::max)())->count_width
        == minimizer_scalar_width::u64);
    STATIC_REQUIRE_FALSE(shape(
        15u, 1u,
        (std::numeric_limits<std::uint64_t>::max)()).has_value());
    STATIC_REQUIRE(select_grouped_soa_offset_width(65'535u)
        == minimizer_scalar_width::u16);
    STATIC_REQUIRE(select_grouped_soa_offset_width(65'536u)
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE(select_grouped_soa_offset_width(
        (std::numeric_limits<std::uint32_t>::max)())
        == minimizer_scalar_width::u32);
    STATIC_REQUIRE(select_grouped_soa_offset_width(
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)()) + 1u)
        == minimizer_scalar_width::u64);

    constexpr auto layout = plan_grouped_soa_control_plane(
        2u,
        minimizer_scalar_width::u32,
        minimizer_scalar_width::u16,
        minimizer_scalar_width::u16);
    STATIC_REQUIRE(layout.has_value());
    STATIC_REQUIRE(layout->hash_offset == 0u);
    STATIC_REQUIRE(layout->prefix_offset == 8u);
    STATIC_REQUIRE(layout->count_offset == 16u);
    STATIC_REQUIRE(layout->encoded_bytes == 24u);
    STATIC_REQUIRE_FALSE(plan_grouped_soa_control_plane(
        (std::numeric_limits<std::uint64_t>::max)(),
        minimizer_scalar_width::u64,
        minimizer_scalar_width::u64,
        minimizer_scalar_width::u64).has_value());

    constexpr auto profile = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        15u, 8u, 65'534u);
    STATIC_REQUIRE(profile.has_value());
    constexpr auto chunks = [](std::uint64_t bytes) {
        constexpr std::uint32_t extent_payload_bytes =
            minimizer_extent_payload_bytes(
                minimizer_generation_layout::
                    grouped_soa_segment_local);
        return static_cast<std::uint32_t>(
            bytes / extent_payload_bytes
            + (bytes % extent_payload_bytes != 0u ? 1u : 0u));
    };
    constexpr auto make_wide_entry = [chunks](std::uint64_t positions) {
        const auto control_layout = plan_grouped_soa_control_plane(
            1u,
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
    constexpr auto o32_bytes = encode_grouped_soa_bucket_directory_entry(
        o32_entry, *profile);
    STATIC_REQUIRE(o32_bytes.has_value());
    STATIC_REQUIRE((*o32_bytes)[97] == std::byte{4});
    constexpr auto decoded_o32 = decode_minimizer_bucket_dir_entry(
        *o32_bytes, *profile);
    STATIC_REQUIRE(decoded_o32.has_value());
    STATIC_REQUIRE(*decoded_o32 == o32_entry);
    constexpr std::uint64_t o64_span =
        (std::uint64_t{1} << 31u) + 1u;
    constexpr auto o64_profile = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        15u, o64_span, 65'534u);
    STATIC_REQUIRE(o64_profile.has_value());
    STATIC_REQUIRE(o64_profile->position_width
        == minimizer_scalar_width::u64);
    constexpr auto o64_entry = [make_wide_entry, chunks] {
        auto value = make_wide_entry(
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)()) + 1u);
        value.position_encoded_bytes = value.position_element_count * 8u;
        value.position_chunk_count = chunks(value.position_encoded_bytes);
        return value;
    }();
    constexpr auto o64_bytes = encode_grouped_soa_bucket_directory_entry(
        o64_entry, *o64_profile);
    STATIC_REQUIRE(o64_bytes.has_value());
    STATIC_REQUIRE((*o64_bytes)[97] == std::byte{8});
    constexpr auto decoded_o64 = decode_minimizer_bucket_dir_entry(
        *o64_bytes, *o64_profile);
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
    REQUIRE(o64_plan.entries.front().position_element_count
        == o64_entry.position_element_count);
    REQUIRE(o64_plan.entries.front().shape_offset_width
        == minimizer_scalar_width::u64);
    auto planned_o64_expected = o64_entry;
    planned_o64_expected.control_checksum = 0u;
    planned_o64_expected.position_checksum = 0u;
    REQUIRE(o64_plan.entries.front() == planned_o64_expected);

    const auto o64_layout = plan_grouped_soa_control_plane(
        o64_entry.control_element_count,
        o64_profile->hash_width,
        o64_entry.shape_offset_width,
        o64_profile->count_width);
    REQUIRE(o64_layout.has_value());
    std::vector<std::byte> o64_controls(
        static_cast<std::size_t>(o64_layout->encoded_bytes));
    test_write_le<std::uint32_t>(o64_controls, 0u, 1u);
    test_write_le<std::uint64_t>(
        o64_controls,
        static_cast<std::size_t>(o64_layout->prefix_offset),
        0u);
    test_write_le<std::uint64_t>(
        o64_controls,
        static_cast<std::size_t>(o64_layout->prefix_offset + 8u),
        o64_entry.position_element_count);
    test_write_le<std::uint16_t>(
        o64_controls,
        static_cast<std::size_t>(o64_layout->count_offset),
        65'535u);
    const auto decoded_o64_controls = decode_grouped_soa_controls(
        o64_controls, o64_entry, *o64_profile);
    REQUIRE(decoded_o64_controls.has_value());
    REQUIRE(decoded_o64_controls->size() == 1u);
    REQUIRE(decoded_o64_controls->front().position_begin == 0u);
    REQUIRE(decoded_o64_controls->front().position_end
        == o64_entry.position_element_count);
    REQUIRE(decoded_o64_controls->front().saturated_count == 65'535u);
}

TEST_CASE("grouped-SoA fixed records use exact 160-byte and 128-byte layouts",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr auto b = [](unsigned value) {
        return static_cast<std::byte>(value);
    };
    constexpr auto shape = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        15u, 8u, 10u);
    STATIC_REQUIRE(shape.has_value());
    constexpr auto profile = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        15u, 8u, 10u);
    STATIC_REQUIRE(profile.has_value());
    constexpr auto manifest = [shape] {
        auto value = golden_generation_manifest();
        value.layout = minimizer_generation_layout::
            grouped_soa_segment_local;
        value.alphabet_code = k_minimizer_persisted_dna4_alphabet_code;
        value.kmer_size = 15u;
        value.gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
        assign_grouped_soa_profile(value, *shape);
        return value;
    }();
    constexpr auto encoded_manifest =
        encode_minimizer_generation_manifest(manifest);
    STATIC_REQUIRE(encoded_manifest.has_value());
    STATIC_REQUIRE(encoded_manifest->size() == 160u);
    constexpr std::array<std::byte, 160> expected_manifest{
        b(0x47), b(0x4f), b(0x49), b(0x42), b(0x05), b(0x00), b(0xa0), b(0x00),
        b(0x08), b(0x07), b(0x06), b(0x05), b(0x04), b(0x03), b(0x02), b(0x01),
        b(0x18), b(0x17), b(0x16), b(0x15), b(0x14), b(0x13), b(0x12), b(0x11),
        b(0x28), b(0x27), b(0x26), b(0x25), b(0x24), b(0x23), b(0x22), b(0x21),
        b(0x14), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x38), b(0x37), b(0x36), b(0x35), b(0x34), b(0x33), b(0x32), b(0x31),
        b(0x48), b(0x47), b(0x46), b(0x45), b(0x44), b(0x43), b(0x42), b(0x41),
        b(0x58), b(0x57), b(0x56), b(0x55), b(0x54), b(0x53), b(0x52), b(0x51),
        b(0x68), b(0x67), b(0x66), b(0x65), b(0x64), b(0x63), b(0x62), b(0x61),
        b(0x78), b(0x77), b(0x76), b(0x75), b(0x74), b(0x73), b(0x72), b(0x71),
        b(0x10), b(0x0f), b(0x0e), b(0x0d), b(0x0c), b(0x0b), b(0x0a), b(0x09),
        b(0x03), b(0x00), b(0x00), b(0x00), b(0x44), b(0x33), b(0x22), b(0x11),
        b(0x88), b(0x77), b(0x66), b(0x55), b(0x00), b(0x00), b(0x44), b(0x33),
        b(0x0f), b(0x00), b(0x88), b(0x77), b(0x03), b(0x00), b(0x00), b(0x00),
        b(0x08), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x02), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x01),
        b(0x53), b(0x4f), b(0x49), b(0x42), b(0x01), b(0x00), b(0x20), b(0x00),
        b(0x04), b(0x02), b(0x02), b(0x01), b(0x01), b(0x01), b(0x02), b(0x00),
        b(0xff), b(0xff), b(0xff), b(0x3f), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x0a), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
    };
    STATIC_REQUIRE(*encoded_manifest == expected_manifest);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 4) == 5u);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*encoded_manifest, 6) == 160u);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded_manifest, 108) == 3u);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded_manifest, 128)
        == k_grouped_soa_extension_magic);
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
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded_manifest, 144)
        == shape->max_hash);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded_manifest, 152) == 10u);
    constexpr auto decoded_manifest = decode_minimizer_generation_manifest(
        std::span<const std::byte>{*encoded_manifest});
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
    constexpr auto encoded_entry = encode_grouped_soa_bucket_directory_entry(
        entry, *profile);
    STATIC_REQUIRE(encoded_entry.has_value());
    STATIC_REQUIRE(encoded_entry->size() == 128u);
    constexpr std::array<std::byte, 128> expected_entry{
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x01), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x02), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x01), b(0x00), b(0x00), b(0x00),
        b(0x02), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x03), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x18), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x06), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x08), b(0x07), b(0x06), b(0x05), b(0x04), b(0x03), b(0x02), b(0x01),
        b(0x18), b(0x17), b(0x16), b(0x15), b(0x14), b(0x13), b(0x12), b(0x11),
        b(0x04), b(0x02), b(0x02), b(0x02), b(0x01), b(0x01), b(0x02), b(0x01),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
        b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
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
    constexpr auto decoded_entry = decode_minimizer_bucket_dir_entry(
        std::span<const std::byte>{*encoded_entry}, *profile);
    STATIC_REQUIRE(decoded_entry.has_value());
    STATIC_REQUIRE(*decoded_entry == entry);
}

TEST_CASE("grouped-SoA fixed records reject every extension corruption class",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const auto shape = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        15u, 8u, 10u);
    REQUIRE(shape.has_value());
    const auto profile = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        15u, 8u, 10u);
    REQUIRE(profile.has_value());
    auto manifest = golden_generation_manifest();
    manifest.layout = minimizer_generation_layout::
        grouped_soa_segment_local;
    manifest.alphabet_code = k_minimizer_persisted_dna4_alphabet_code;
    manifest.kmer_size = 15u;
    manifest.gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
    assign_grouped_soa_profile(manifest, *shape);
    const auto encoded_manifest =
        encode_minimizer_generation_manifest(manifest);
    REQUIRE(encoded_manifest.has_value());
    const auto good_manifest = *encoded_manifest;

    for (const std::size_t offset : {128u, 132u, 134u}) {
        INFO("grouped-SoA extension header offset " << offset);
        auto corrupt = good_manifest;
        corrupt[offset] ^= std::byte{1};
        require_wire_error(
            decode_minimizer_generation_manifest(corrupt),
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
        require_wire_error(
            decode_minimizer_generation_manifest(corrupt),
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
            minimizer_generation_layout::
                grouped_soa_segment_local,
            minimizer_sketch_backend::scalar,
            manifest.alphabet_code,
            manifest.storage_unit_byte_width,
            manifest.kmer_size,
            manifest.window_size,
            shape->max_occ_cap);
        REQUIRE(expected.has_value());
        REQUIRE(validate_minimizer_generation_storage_contract(
            manifest, *expected).has_value());
        auto changed_cap = good_manifest;
        changed_cap[152] ^= std::byte{1};
        const auto decoded = decode_minimizer_generation_manifest(changed_cap);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->max_occ_cap
            == (shape->max_occ_cap ^ 1u));
        require_wire_error(
            validate_minimizer_generation_storage_contract(
                *decoded, *expected),
            minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    {
        /* Crossing the u16/u32 count-width boundary while retaining the
         * encoded u16 shape is intrinsically inconsistent and fails decode. */
        auto incompatible_cap_width = good_manifest;
        test_write_le<std::uint64_t>(
            incompatible_cap_width, 152u, 65'535u);
        require_wire_error(
            decode_minimizer_generation_manifest(incompatible_cap_width),
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
    const auto encoded_entry = encode_grouped_soa_bucket_directory_entry(
        entry, *profile);
    REQUIRE(encoded_entry.has_value());
    const auto good_entry = *encoded_entry;
    for (std::size_t offset = 96u; offset <= 103u; ++offset) {
        INFO("grouped-SoA directory shape offset " << offset);
        auto corrupt = good_entry;
        corrupt[offset] = std::byte{0};
        require_wire_error(
            decode_minimizer_bucket_dir_entry(corrupt, *profile),
            minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    for (std::size_t offset = 104u; offset < 128u; ++offset) {
        INFO("grouped-SoA directory reserved offset " << offset);
        auto corrupt = good_entry;
        corrupt[offset] = std::byte{1};
        require_wire_error(
            decode_minimizer_bucket_dir_entry(corrupt, *profile),
            minimizer_generation_encoding_error::reserved_nonzero);
    }

}

TEST_CASE("grouped-SoA emits exact SoA and prepared reader rejects corruption",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(std::is_move_constructible_v<
        prepared_grouped_soa_query_reader>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<
        prepared_grouped_soa_query_reader>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<
        prepared_grouped_soa_query_reader>);
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
    const auto generation = encode_minimizer_generation(
        producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(producer.scan_count == 3u);
    REQUIRE(generation->manifest_row.byte_count == 160u);
    REQUIRE(generation->directory.encoded_bytes().size()
        == generation->directory.entries().size() * 128u);
    REQUIRE(generation->directory.entries().size() == 1u);
    const auto& entry = generation->directory.entries().front();
    REQUIRE(entry.shape_offset_width == minimizer_scalar_width::u16);
    REQUIRE(sink.rows.size()
        >= entry.control_chunk_count + entry.position_chunk_count);
    REQUIRE(sink.rows.front().address.sub_key == entry.sub_key);
    REQUIRE(sink.rows[entry.control_chunk_count].address.sub_key
        == *derive_minimizer_position_extent_sub_key(entry.sub_key));
    const auto control_bytes = collect_encoded_extent(sink.rows, entry, true);
    const auto position_bytes = collect_encoded_extent(sink.rows, entry, false);
    const auto controls = decode_grouped_soa_controls(
        control_bytes, entry, generation->directory.profile());
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
    auto reader = prepared_grouped_soa_query_reader::begin(
        admission,
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
    REQUIRE((std::vector<std::uint64_t>{
        hash1.positions.begin(), hash1.positions.end()}
        == std::vector<std::uint64_t>{7u, 18u}));
    REQUIRE(reader->lookup(2u).positions.front() == 3u);
    REQUIRE(reader->lookup(999u).positions.empty());

    SECTION("padding byte") {
        auto corrupt = control_bytes;
        // H32*2 ends at 8; prefixes occupy [8,14), so [14,16) is padding.
        corrupt[14] = std::byte{1};
        require_wire_error(
            decode_grouped_soa_controls(
                corrupt, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::reserved_nonzero);
    }
    SECTION("terminal prefix") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 12u, 2u);
        require_wire_error(
            decode_grouped_soa_controls(
                corrupt, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::control_range);
    }
    SECTION("interior prefix cannot overlap") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 10u, 0u);
        require_wire_error(
            decode_grouped_soa_controls(
                corrupt, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::control_range);
    }
    SECTION("saturated count") {
        auto corrupt = control_bytes;
        test_write_le<std::uint16_t>(corrupt, 16u, 1u);
        require_wire_error(
            decode_grouped_soa_controls(
                corrupt, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::control_range);
    }
    SECTION("directory hash endpoints are exact") {
        auto corrupt_first = control_bytes;
        test_write_le<std::uint32_t>(corrupt_first, 0u, 0u);
        require_wire_error(
            decode_grouped_soa_controls(
                corrupt_first, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::hash_range);
        auto corrupt_last = control_bytes;
        test_write_le<std::uint32_t>(corrupt_last, 4u, 3u);
        require_wire_error(
            decode_grouped_soa_controls(
                corrupt_last, entry, generation->directory.profile()),
            minimizer_generation_encoding_error::hash_range);
    }
    SECTION("positions are strictly ordered inside one hash") {
        auto corrupt = position_bytes;
        test_write_le<std::uint16_t>(
            corrupt, 2u, test_read_le<std::uint16_t>(corrupt, 0u));
        require_wire_error(
            decode_grouped_soa_positions(
                corrupt, *controls, entry,
                *derive_minimizer_position_segment_bounds(
                    generation->manifest, 0u),
                generation->directory.profile()),
            minimizer_generation_encoding_error::position_range);
    }
    SECTION("prepared reader retains strict order state") {
        auto corrupt = sink.rows[entry.control_chunk_count];
        test_write_le<std::uint16_t>(
            corrupt.bytes, 2u,
            test_read_le<std::uint16_t>(corrupt.bytes, 0u));
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
        for (std::size_t index = 0;
             index < entry.control_chunk_count; ++index) {
            REQUIRE(made->accept(row_view(sink.rows[index])).has_value());
        }
        require_wire_error(
            made->accept(row_view(corrupt)),
            minimizer_generation_encoding_error::position_range);
    }
    SECTION("positions cannot escape their persisted segment") {
        auto corrupt = position_bytes;
        test_write_le<std::uint16_t>(corrupt, 0u, 200u);
        require_wire_error(
            decode_grouped_soa_positions(
                corrupt, *controls, entry,
                *derive_minimizer_position_segment_bounds(
                    generation->manifest, 0u),
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
        require_wire_error(
            made->accept(row_view(corrupt)),
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
        require_wire_error(
            prepared_grouped_soa_query_reader::begin(
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
        const std::array<std::uint64_t, 1> too_wide{
            generation->directory.profile().max_hash + 1u};
        require_wire_error(
            prepared_grouped_soa_query_reader::begin(
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
        minimizers.emplace_back(
            query_hash, (segment_span + index + 1u) << 1u);
    }

    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink,
        test_grouped_soa_generation_options(
            2u * segment_span, persisted_cap));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_count == exact_count);
    const std::array<std::uint64_t, 1> queries{query_hash};

    bioseq::parallelism::memory_admission_controller retained_admission;
    bioseq::parallelism::memory_admission_controller pruned_admission;
    const auto make_reader = [&](
            bioseq::parallelism::memory_admission_controller& admission,
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
    REQUIRE(retained.positions[per_segment]
        == (segment_span + 1u) << 1u);
    REQUIRE(retained.positions.back()
        == (segment_span + per_segment) << 1u);
    const auto retained_evidence = retained_reader->memory_stats();
    REQUIRE(retained_evidence.query_record_size_bytes
        == sizeof(std::uint32_t));
    REQUIRE(retained_evidence.query_record_capacity_bytes
        == sizeof(std::uint32_t) + sizeof(std::uint64_t));

    auto pruned_reader = make_reader(
        pruned_admission,
        (std::numeric_limits<std::uint16_t>::max)());
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
        return (index % segment_count) * segment_span
            + index / segment_count;
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
        const std::uint64_t hash =
            ((index + 1u) * 2'654'435'761ULL) & ((1ULL << 30u) - 1u);
        minimizers.emplace_back(hash, retained_bp(index) << 1u);
        queries.push_back(hash);
    }

    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink,
        test_grouped_soa_generation_options(
            segment_count * segment_span, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == segment_count);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = prepared_grouped_soa_query_reader::begin(
        admission,
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
    REQUIRE(evidence.query_record_capacity_bytes
        == query_count
            * (sizeof(std::uint16_t) + sizeof(std::uint64_t)));
    REQUIRE(evidence.query_route_radix_bits == 9u);
    REQUIRE(evidence.query_route_radix_bucket_count == 512u);
    REQUIRE(evidence.query_route_radix_nonempty_bucket_count > 0u);
    REQUIRE(evidence.query_route_radix_nonempty_bucket_count
        <= evidence.query_route_radix_bucket_count);
    REQUIRE(evidence.query_route_radix_max_bucket_size <= 8u);
    REQUIRE(evidence.query_hash_capacity_bytes
        == query_count * sizeof(std::uint32_t)
            + (evidence.query_route_radix_bucket_count + 1u)
                * sizeof(std::uint32_t));
    REQUIRE(evidence.released_query_hash_capacity_bytes
        >= query_count * sizeof(std::uint64_t));
    REQUIRE(evidence.decoded_control_records == query_count);
    REQUIRE(evidence.query_route_control_lookups
        == evidence.decoded_control_records);
    REQUIRE(evidence.query_route_matches == query_count);
    const std::uint64_t global_lower_bound_probe_ceiling =
        evidence.query_route_control_lookups
        * (std::bit_width(query_count - 1u) + 1u);
    REQUIRE(evidence.query_route_hash_probes * 2u
        < global_lower_bound_probe_ceiling);
    REQUIRE(evidence.query_route_hash_probes
        <= evidence.query_route_control_lookups * 5u);
    REQUIRE(evidence.retained_selected_control_records == query_count);
    REQUIRE(evidence.selected_control_size_bytes == 8u);
    // The decoded wire record is eight bytes, but this fixture's immutable
    // segment geometry permits the retained position ends to narrow to u16.
    // The final SoA therefore owns one u16 end plus one u32 dense route per
    // selected query; it does not retain the decoded u64 control image.
    REQUIRE(evidence.retained_selected_control_capacity_bytes
        == query_count
            * (sizeof(std::uint16_t) + sizeof(std::uint32_t)));
    REQUIRE(evidence.current_selected_control_capacity_bytes == 0u);
    REQUIRE(evidence.global_position_capacity_bytes
        == evidence.retained_position_capacity * sizeof(std::uint64_t));
    REQUIRE(evidence.control_payload_capacity_bytes == 0u);
    REQUIRE(evidence.retained_layout_equation_holds());
    REQUIRE(evidence.owned_capacity_bytes == reader->admitted_bytes());
    // Full owner walks are now barrier-only: initialized, controls complete,
    // exact position image published, and final ready. Extent count no longer
    // appears in this equation.
    constexpr std::uint64_t expected_barrier_audits = 4u;
    REQUIRE(evidence.capacity_accounting_passes
        == expected_barrier_audits);
    REQUIRE_FALSE(evidence.saturated);
    REQUIRE(reader->lookup(queries.front()).positions.front()
        == retained_bp(0u) << 1u);
    REQUIRE(reader->lookup(queries.back()).positions.front()
        == retained_bp(query_count - 1u) << 1u);

    // Exercise the statement-level aggregate equation with a second reader
    // that selects only the first of the three control extents. Production
    // merges these per-reader counters, so the aggregate must retain the same
    // 7R + E + 4S shape even when readers select different extent counts.
    const std::array<std::uint64_t, 1> sparse_queries{queries.front()};
    const std::uint64_t sparse_selected_extent_count =
        static_cast<std::uint64_t>(std::ranges::count_if(
            generation->directory.entries(),
            [&](const grouped_soa_bucket_directory_entry& entry) {
                return entry.lo_hash <= sparse_queries.front()
                    && sparse_queries.front() <= entry.hi_hash;
            }));
    REQUIRE(sparse_selected_extent_count > 0u);
    auto sparse_reader = prepared_grouped_soa_query_reader::begin(
        admission,
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
    REQUIRE(sparse_evidence.query_record_size_bytes
        == sizeof(std::uint16_t));
    REQUIRE(sparse_evidence.query_record_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_bits == 0u);
    REQUIRE(sparse_evidence.query_route_radix_bucket_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_nonempty_bucket_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_max_bucket_size == 1u);
    REQUIRE(sparse_evidence.query_hash_capacity_bytes
        == sizeof(std::uint32_t) + 2u * sizeof(std::uint32_t));
    REQUIRE(sparse_evidence.released_query_hash_capacity_bytes
        >= sizeof(std::uint64_t));
    REQUIRE(sparse_evidence.decoded_control_records > 1u);
    REQUIRE(sparse_evidence.query_route_control_lookups > 0u);
    REQUIRE(sparse_evidence.query_route_control_lookups
        <= sparse_evidence.decoded_control_records);
    REQUIRE(sparse_evidence.query_route_matches == 1u);
    REQUIRE(sparse_evidence.query_route_hash_probes
        <= sparse_evidence.query_route_control_lookups * 2u);
    REQUIRE(sparse_evidence.retained_selected_control_records
        == sparse_evidence.decoded_control_records);
    REQUIRE(sparse_evidence.retained_selected_control_capacity_bytes
        == sparse_evidence.decoded_control_records * sizeof(std::uint16_t)
            + sizeof(std::uint32_t) * 2u);
    REQUIRE(sparse_evidence.current_selected_control_capacity_bytes == 0u);
    REQUIRE(sparse_evidence.retained_layout_equation_holds());
    REQUIRE(sparse_evidence.owned_capacity_bytes
        == sparse_reader->admitted_bytes());
    REQUIRE(sparse_evidence.capacity_accounting_passes
        == expected_barrier_audits);
    REQUIRE_FALSE(sparse_evidence.saturated);

    // A selected query absent from the target still crosses the same global
    // controls/positions boundary, but exact P=0 must remain allocation-free.
    const std::array<std::uint64_t, 1> absent_queries{
        queries.front() ^ 1u};
    REQUIRE(std::ranges::find(queries, absent_queries.front())
        == queries.end());
    auto empty_reader = prepared_grouped_soa_query_reader::begin(
        admission,
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
    REQUIRE(empty_evidence.owned_capacity_bytes
        == empty_reader->admitted_bytes());
    REQUIRE(empty_reader->lookup(absent_queries.front()).positions.empty());

    constexpr std::uint64_t aggregate_sessions = 2u;
    const std::uint64_t aggregate_passes =
        evidence.capacity_accounting_passes
        + sparse_evidence.capacity_accounting_passes;
    REQUIRE(aggregate_passes
        == aggregate_sessions * expected_barrier_audits);
}

TEST_CASE("grouped-SoA admission failures name the exact owner substage",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(static_cast<std::uint8_t>(
        minimizer_generation_encoding_error::admission) == 28u);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, 2u},
        {2u, 4u},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    const std::array<std::uint64_t, 2> queries{1u, 2u};
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) payload.push_back(&row);
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
        INFO("begin admission substage "
            << grouped_soa_query_admission_substage_name(wanted));
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
        require_wire_error(
            made, minimizer_generation_encoding_error::admission);
        REQUIRE(evidence.substage == wanted);
        REQUIRE(evidence.injected_failure);
        REQUIRE(grouped_soa_query_admission_substage_name(wanted)
            != std::string_view{"unknown"});
        if (wanted != substage_type::identity_and_directory_reservation) {
            REQUIRE(evidence.projected_bytes > 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    for (const substage_type wanted : payload_substages) {
        INFO("payload admission substage "
            << grouped_soa_query_admission_substage_name(wanted));
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
                REQUIRE(accepted.error()
                    == minimizer_generation_encoding_error::admission);
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
        if (wanted == substage_type::position_image_reservation
            || wanted == substage_type::position_image_allocation
            || wanted == substage_type::position_image_reconcile) {
            REQUIRE(evidence.retained_position_count == queries.size());
            REQUIRE(evidence.retained_position_bytes
                == queries.size() * sizeof(std::uint64_t));
            REQUIRE(evidence.allocation_headroom_bytes == 0u);
            if (wanted == substage_type::position_image_reconcile) {
                REQUIRE(evidence.projected_bytes
                    == evidence.owned_capacity_bytes);
            } else {
                const std::uint64_t compact_final_bytes =
                    queries.size() * sizeof(std::uint64_t)
                    + queries.size() * sizeof(std::uint8_t);
                REQUIRE(evidence.projected_bytes
                    == evidence.owned_capacity_bytes
                        + evidence.retained_position_bytes
                        + compact_final_bytes);
            }
        }
        REQUIRE(made->failed());
        REQUIRE(made->admitted_bytes() == 0u);
        const auto released = made->memory_stats();
        REQUIRE(released.retained_position_count == 0u);
        REQUIRE(released.retained_position_capacity == 0u);
        REQUIRE(released.global_position_capacity_bytes == 0u);
        REQUIRE(released.retained_layout_equation_holds());
        REQUIRE(released.global_position_storage_allocations
            == (wanted == substage_type::position_image_reconcile ? 1u : 0u));
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
        require_wire_error(
            made, minimizer_generation_encoding_error::admission);
        REQUIRE(evidence.substage
            == substage_type::identity_and_directory_reservation);
        REQUIRE_FALSE(evidence.injected_failure);
        REQUIRE(evidence.controller_failure_snapshot_present);
        REQUIRE(evidence.controller_reason
            != bioseq::parallelism::memory_admission_failure_reason::none);
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
        const auto adversarial = plan_grouped_soa_position_image(
            adversarial_count, 17u);
        REQUIRE(adversarial.has_value());
        REQUIRE(adversarial->retained_position_bytes == 800'000'000u);
        REQUIRE(adversarial->allocation_headroom_bytes == 0u);
        REQUIRE(adversarial->projected_bytes == 800'000'017u);
        REQUIRE(adversarial->projected_bytes - 17u
            < adversarial->retained_position_bytes
                * bioseq::parallelism::k_vector_preallocation_headroom_factor);
        constexpr std::uint64_t adversarial_query_count = 100'000'000u;
        constexpr std::uint64_t prior_query_owned_bytes =
            adversarial_query_count * (64u + 2u * sizeof(std::uint64_t));
        constexpr std::uint64_t exact_query_state_and_hash_bytes =
            adversarial_query_count
                * (sizeof(std::uint32_t) + sizeof(std::uint8_t));
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
        STATIC_REQUIRE(prior_query_owned_bytes - exact_query_owned_bytes
            == 7'365'782'268u);
        require_wire_error(
            plan_grouped_soa_position_image(
                (std::numeric_limits<std::uint64_t>::max)(), 0u),
            minimizer_generation_encoding_error::arithmetic_overflow);
        require_wire_error(
            plan_grouped_soa_position_image(
                1u,
                (std::numeric_limits<std::uint64_t>::max)() - 1u),
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
}

TEST_CASE("grouped-SoA selects its exact immutable row quantum",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(minimizer_extent_payload_bytes(
        minimizer_generation_layout::grouped_soa_segment_local)
        == 8064u);

    constexpr std::uint64_t position_count = 5000u;
    constexpr std::uint64_t first_control_count = 8064u / 4u;
    constexpr std::uint64_t queried_control_count =
        position_count - first_control_count - 1u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(position_count);
    for (std::uint64_t position = 0; position < position_count; ++position) {
        const std::uint64_t hash = position < first_control_count ? 6u
            : position < first_control_count + queried_control_count ? 7u
            : 8u;
        minimizers.emplace_back(hash, position << 1u);
    }
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink,
        test_grouped_soa_generation_options(position_count + 1u, 65'534u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest_row.byte_count == 160u);
    REQUIRE(generation->manifest.position_segment_span_bp
        == k_minimizer_preferred_segment_span_bp);
    REQUIRE(generation->manifest.position_width
        == minimizer_scalar_width::u32);
    REQUIRE(generation->directory.entries().size() == 1u);
    const grouped_soa_bucket_directory_entry& entry =
        generation->directory.entries().front();
    REQUIRE(entry.position_encoded_bytes == position_count * 4u);
    REQUIRE(entry.position_chunk_count == 3u);
    REQUIRE(sink.grouped_soa_payload_write_calls
            == static_cast<std::uint64_t>(entry.control_chunk_count)
                + entry.position_chunk_count);
    REQUIRE(sink.grouped_soa_row_crc_bytes
            == generation->manifest.payload_bytes);
    const auto position_sub_key =
        derive_minimizer_position_extent_sub_key(entry.sub_key);
    REQUIRE(position_sub_key.has_value());
    const auto first_position = std::ranges::find_if(
        sink.rows, [position_sub_key](const collected_minimizer_row& row) {
            return row.address.sub_key == *position_sub_key
                && row.address.chunk_no == 0u;
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
    REQUIRE(lookup.positions.back()
        == (first_control_count + queried_control_count - 1u) << 1u);
    const auto evidence = reader->memory_stats();
    REQUIRE(evidence.decoded_control_records == 3u);
    REQUIRE(evidence.retained_selected_control_capacity_bytes
        == 3u * sizeof(std::uint16_t) + 2u * sizeof(std::uint32_t));
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

    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(narrow_span + 1u, 65'534u),
        one_control_per_segment);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_span_bp == narrow_span);
    REQUIRE(generation->manifest.position_segment_count == 2u);
    REQUIRE(generation->manifest.gpu_abi_revision
        == k_grouped_soa_gpu_abi_revision);
    REQUIRE(generation->manifest.position_width
        == minimizer_scalar_width::u16);

    const auto canonical_profile = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity,
        generation->manifest.alphabet_code,
        generation->manifest.kmer_size,
        generation->manifest.position_segment_span_bp,
        generation->manifest.max_occ_cap);
    REQUIRE(canonical_profile.has_value());
    REQUIRE(generation->directory.profile() == *canonical_profile);
    const auto manifest_profile =
        derive_grouped_soa_minimizer_profile(generation->manifest);
    REQUIRE(manifest_profile.has_value());
    REQUIRE(*manifest_profile == *canonical_profile);

    REQUIRE(generation->directory.entries().size() == 2u);
    std::uint64_t position_count = 0u;
    std::uint64_t position_bytes = 0u;
    for (const grouped_soa_bucket_directory_entry& entry :
         generation->directory.entries()) {
        REQUIRE(entry.position_element_count == 1u);
        REQUIRE(entry.position_encoded_bytes == 2u);
        REQUIRE(entry.position_chunk_count == 1u);
        position_count += entry.position_element_count;
        position_bytes += entry.position_encoded_bytes;
    }
    REQUIRE(position_count == generation->manifest.position_count);
    REQUIRE(position_bytes == generation->manifest.position_count * 2u);
    REQUIRE(sink.grouped_soa_row_crc_bytes
        == generation->manifest.payload_bytes);
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
        producer,
        sink,
        test_grouped_soa_generation_options(span + 100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 2u);
    for (std::size_t ordinal = 0;
         ordinal < generation->directory.entries().size(); ++ordinal) {
        const auto& entry = generation->directory.entries()[ordinal];
        REQUIRE(entry.partition_id == ordinal);
        REQUIRE(entry.sub_key == static_cast<index_sub_key_t>(ordinal));
        REQUIRE(entry.control_chunk_count == 1u);
        REQUIRE(entry.position_chunk_count == 1u);
    }
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) payload.push_back(&row);
    }
    REQUIRE(payload.size() == 4u);
    REQUIRE(payload[0]->address.position_segment == 0);
    REQUIRE(payload[0]->address.sub_key == 0);
    REQUIRE(payload[1]->address.position_segment == 1);
    REQUIRE(payload[1]->address.sub_key == 1);
    REQUIRE(payload[2]->address.position_segment == 0);
    REQUIRE(payload[2]->address.sub_key
        == *derive_minimizer_position_extent_sub_key(0));
    REQUIRE(payload[3]->address.position_segment == 1);
    REQUIRE(payload[3]->address.sub_key
        == *derive_minimizer_position_extent_sub_key(1));
    require_complete_generation_rows(*generation, sink.rows);

    auto interleaved = make_generation_row_validator(*generation);
    REQUIRE(interleaved.accept(row_view(*payload[0])).has_value());
    require_wire_error(
        interleaved.accept(row_view(*payload[2])),
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
        REQUIRE((std::vector<std::uint64_t>{
            found.positions.begin(), found.positions.end()}
            == std::vector<std::uint64_t>{
                2u,
                4u,
                (span + 1u) << 1u,
                (span + 2u) << 1u}));
    }

    auto invalid_entry = generation->directory.entries().front();
    invalid_entry.sub_key = 7;
    require_wire_error(
        encode_grouped_soa_bucket_directory_entry(
            invalid_entry, generation->directory.profile()),
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
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) payload.push_back(&row);
    }
    REQUIRE(payload.size() == 2u);
    const std::array<std::uint64_t, 1> query{1u};
    const auto begin = [&](auto& admission) {
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
            require_wire_error(
                validate_minimizer_generation_row_receipt(bad_first_view),
                minimizer_generation_encoding_error::checksum);
            auto validated_first =
                validate_minimizer_generation_row_receipt(first_view);
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
        require_wire_error(
            made->accept(receipt), minimizer_generation_encoding_error::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("ordinary grouped-SoA row with wrong row CRC remains rejected") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto untrusted = row_view(*payload.front());
        untrusted.row_checksum ^= 1u;
        require_wire_error(
            made->accept(untrusted), minimizer_generation_encoding_error::checksum);
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
        require_wire_error(
            made->accept(source), minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("grouped-SoA trusted wrong order still poisons and releases") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto receipt = trusted_row_receipt(*payload.back());
        require_wire_error(
            made->accept(receipt), minimizer_generation_encoding_error::row_order);
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
        require_wire_error(
            made->accept(receipt), minimizer_generation_encoding_error::checksum);
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
            REQUIRE(before_move.global_position_capacity_bytes
                == 2u * sizeof(std::uint64_t));
            REQUIRE(before_move.owned_capacity_bytes
                == made->admitted_bytes());
            const std::uint64_t reserved = admission.reserved_bytes();

            prepared_grouped_soa_query_reader destination{std::move(*made)};
            REQUIRE(made->failed());
            REQUIRE(made->admitted_bytes() == 0u);
            REQUIRE(made->memory_stats().retained_position_count == 0u);
            REQUIRE(made->memory_stats().retained_position_capacity == 0u);
            REQUIRE(admission.reserved_bytes() == reserved);
            REQUIRE(destination.memory_stats().retained_position_count
                == 2u);
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
        require_wire_error(
            made->finish(), minimizer_generation_encoding_error::row_count);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("row corruption poisons and releases immediately") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto corrupt = *payload.front();
        corrupt.bytes.front() ^= std::byte{1};
        require_wire_error(
            made->accept(row_view(corrupt)),
            minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("post-begin allocation denial rolls back and releases") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        admission.set_index_memory_cap_bytes(admission.reserved_bytes());
        require_wire_error(
            made->accept(row_view(*payload.front())),
            minimizer_generation_encoding_error::admission);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

TEST_CASE("packed query occurrence layout is boundary exact",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t u32_max =
        (std::numeric_limits<std::uint32_t>::max)();
    constexpr std::uint64_t two_to_32 = std::uint64_t{1} << 32u;

    STATIC_REQUIRE(sizeof(packed_query_occurrence) == 8u);
    STATIC_REQUIRE(sizeof(wide_query_occurrence) == 12u);

    constexpr auto empty = plan_query_occurrence_packing(0u, 0u, 0u);
    STATIC_REQUIRE(empty.has_value());
    STATIC_REQUIRE(empty->packed());
    STATIC_REQUIRE(empty->query_bits == 0u);
    STATIC_REQUIRE(empty->ordinal_bits == 32u);
    STATIC_REQUIRE(empty->ordinal_capacity == two_to_32);
    STATIC_REQUIRE(empty->ordinal_mask == UINT32_MAX);

    constexpr auto single = plan_query_occurrence_packing(
        1u, u32_max, u32_max);
    STATIC_REQUIRE(single.has_value());
    STATIC_REQUIRE(single->packed());
    STATIC_REQUIRE(single->ordinal_bits == 32u);
    constexpr auto single_word = encode_packed_query_occurrence(
        *single, 0u, UINT32_MAX);
    STATIC_REQUIRE(single_word.has_value());
    STATIC_REQUIRE(*single_word == UINT32_MAX);
    STATIC_REQUIRE((decode_packed_query_occurrence_trusted(
        *single, *single_word) == query_occurrence_reference{
            .batch_query_ordinal = 0u,
            .query_ordinal = UINT32_MAX,
        }));

    constexpr auto maximum_queries = plan_query_occurrence_packing(
        u32_max, 1u, 1u);
    STATIC_REQUIRE(maximum_queries.has_value());
    STATIC_REQUIRE(maximum_queries->packed());
    STATIC_REQUIRE(maximum_queries->query_bits == 32u);
    STATIC_REQUIRE(maximum_queries->ordinal_bits == 0u);
    STATIC_REQUIRE(maximum_queries->ordinal_capacity == 1u);
    constexpr auto maximum_query_word = encode_packed_query_occurrence(
        *maximum_queries, UINT32_MAX - 1u, 0u);
    STATIC_REQUIRE(maximum_query_word.has_value());
    STATIC_REQUIRE((decode_packed_query_occurrence_trusted(
        *maximum_queries, *maximum_query_word) == query_occurrence_reference{
            .batch_query_ordinal = UINT32_MAX - 1u,
            .query_ordinal = 0u,
        }));

    constexpr auto maximum_occurrences = plan_query_occurrence_packing(
        1u, u32_max, u32_max);
    STATIC_REQUIRE(maximum_occurrences.has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(
        1u, two_to_32, u32_max).has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(
        two_to_32, 0u, 0u).has_value());
    STATIC_REQUIRE_FALSE(plan_query_occurrence_packing(
        1u, two_to_32, two_to_32).has_value());

    constexpr auto maximum_packed = plan_query_occurrence_packing(
        65'536u, 65'537u, 65'536u);
    STATIC_REQUIRE(maximum_packed.has_value());
    STATIC_REQUIRE(maximum_packed->packed());
    STATIC_REQUIRE(maximum_packed->ordinal_bits == 16u);
    constexpr auto forced_wide = plan_query_occurrence_packing(
        65'536u, 65'538u, 65'537u);
    STATIC_REQUIRE(forced_wide.has_value());
    STATIC_REQUIRE(forced_wide->route == query_occurrence_route::wide);

    constexpr auto malformed_shift = [single] {
        auto layout = *single;
        layout.ordinal_bits = 64u;
        return layout;
    }();
    STATIC_REQUIRE_FALSE(valid_query_occurrence_packing_layout(
        malformed_shift));
    STATIC_REQUIRE_FALSE(encode_packed_query_occurrence(
        malformed_shift, 0u, 0u).has_value());
    constexpr auto malformed_mask = [single] {
        auto layout = *single;
        layout.ordinal_mask = 0u;
        return layout;
    }();
    STATIC_REQUIRE_FALSE(valid_query_occurrence_packing_layout(
        malformed_mask));
    STATIC_REQUIRE_FALSE(encode_packed_query_occurrence(
        malformed_mask, 0u, 0u).has_value());
}

TEST_CASE("canonical anchor sort preserves the 63-bit reference boundary",
          "[component:bioseq][layer:unit][feature:seeding]") {
    constexpr std::uint64_t max_reference =
        (std::uint64_t{1} << 63u) - 1u;
    constexpr std::uint64_t low_query_position = 7u;
    constexpr std::uint64_t high_query_position =
        (std::uint64_t{1} << 32u) + low_query_position;
    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto anchors = bioseq::chaining::make_arena_slab<
        bioseq::chaining::anchor>();
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference,
        .query_position = high_query_position,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = 0u,
        .query_position = 1u,
        .is_reverse = 1u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference,
        .query_position = low_query_position,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });
    anchors.push_back(bioseq::chaining::anchor{
        .ref_position = max_reference - 1u,
        .query_position = 99u,
        .is_reverse = 0u,
        .query_span = 15u,
        .flags = 0u,
        .segment_id = 0u,
    });

    sort_seed_hits(anchors);
    REQUIRE(anchors.size() == 4u);
    REQUIRE(anchors.data()[0].ref_position == max_reference - 1u);
    REQUIRE(anchors.data()[1].ref_position == max_reference);
    REQUIRE(anchors.data()[1].query_position == low_query_position);
    REQUIRE(anchors.data()[2].ref_position == max_reference);
    REQUIRE(anchors.data()[2].query_position == high_query_position);
    REQUIRE(anchors.data()[1].query_sort_key()
        == anchors.data()[2].query_sort_key());
    REQUIRE(anchors.data()[3].is_reverse == 1u);
    REQUIRE(std::ranges::is_sorted(
        std::span<const bioseq::chaining::anchor>{
            anchors.data(), anchors.size()},
        bioseq::chaining::canonical_anchor_less{}));
}

TEST_CASE("bounded u32 anchor radix selects the authenticated width exactly",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using bioseq::chaining::anchor;
    const auto make_anchor = [](
            std::uint64_t ref_position,
            std::uint32_t is_reverse) noexcept {
        return anchor{
            .ref_position = ref_position,
            .query_position = ref_position ^ 0x55u,
            .is_reverse = is_reverse,
            .query_span = 15u,
            .flags = 0u,
            .segment_id = 0u,
        };
    };
    const auto require_route = [](
            std::span<anchor> anchors,
            std::uint64_t target_length,
            bool expected_bounded_u32) {
        const auto evidence =
            bioseq::chaining::sort_canonical_anchor_primary_bounded(
                anchors, target_length);
        REQUIRE(evidence.bounded_u32_primary_key
            == expected_bounded_u32);
        REQUIRE(evidence.primary_bytes
            == anchors.size_bytes());
        REQUIRE_FALSE(evidence.saturated);
        REQUIRE(std::ranges::is_sorted(
            anchors, bioseq::chaining::canonical_anchor_less{}));
    };

    SECTION("UINT32_MAX target length uses the four-byte key") {
        constexpr std::uint64_t target_length =
            (std::numeric_limits<std::uint32_t>::max)();
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(17u, 0u),
            make_anchor(target_length / 2u, 0u),
        };
        require_route(anchors, target_length, true);
    }

    SECTION("UINT32_MAX plus one retains the full-width key") {
        constexpr std::uint64_t target_length =
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)()) + 1u;
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(19u, 0u),
            make_anchor(target_length / 2u, 0u),
        };
        require_route(anchors, target_length, false);
    }

    SECTION("a high authenticated reference retains the full-width key") {
        constexpr std::uint64_t target_length =
            (std::uint64_t{1u} << 40u) + 2u;
        std::array anchors{
            make_anchor(target_length - 1u, 0u),
            make_anchor(23u, 0u),
            make_anchor(std::uint64_t{1u} << 39u, 0u),
        };
        require_route(anchors, target_length, false);
    }

    SECTION("a reverse-only slice preserves canonical reference order") {
        constexpr std::uint64_t target_length = 10'000u;
        std::array anchors{
            make_anchor(9'999u, 1u),
            make_anchor(3u, 1u),
            make_anchor(5'001u, 1u),
        };
        require_route(anchors, target_length, true);
        REQUIRE(std::ranges::all_of(anchors, [](const anchor& anchor) {
            return anchor.is_reverse == 1u;
        }));
    }
}

TEST_CASE("fragment-native anchors are canonical and match ordinary lookup",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t segment_span =
        k_minimizer_preferred_segment_span_bp;
    // grouped-SoA directory segments use an int32 ordinal, so a mapped reader
    // fixture cannot encode the synthetic 2^63 boundary above. This position
    // is still well beyond 32 bits and leaves a large run of empty segments.
    constexpr std::uint64_t high_reference_position =
        (std::uint64_t{1} << 40u) + 19u;
    constexpr auto packed = [](
            std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(10u, 0u)},
        {7u, packed(10u, 1u)},
        {7u, packed(segment_span + 3u, 0u)},
        {7u, packed(high_reference_position, 1u)},
        {9u, packed(21u, 0u)},
    };
    constexpr std::uint64_t target_length =
        high_reference_position + 100u;
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(target_length, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_count > 2u);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/20u, /*strand=*/0u},
        // The duplicate produces byte-identical equal canonical keys.
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/20u, /*strand=*/0u},
        minimap_sketch_data{/*kmer=*/7u, /*pos=*/33u, /*strand=*/1u},
        minimap_sketch_data{/*kmer=*/9u, /*pos=*/44u, /*strand=*/0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;

    SECTION("accepted cross-segment positions are byte-identical") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(
            admission, *generation, sink.rows, 4u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected = ordinary_seed_anchors(
            pairs, target_length, query, query_length, 4, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 4, *scratch);
        REQUIRE(batch.query_count() == 1u);
        REQUIRE(batch.admitted_bytes() > 0u);
        REQUIRE(scratch->wave_outstanding());
        const auto actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                query, query_length, 4, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(std::ranges::any_of(actual, [](const auto& anchor) {
            return anchor.is_reverse == 0u;
        }));
        REQUIRE(std::ranges::any_of(actual, [](const auto& anchor) {
            return anchor.is_reverse == 1u;
        }));
        REQUIRE(std::ranges::any_of(actual, [](const auto& anchor) {
            return anchor.ref_position == high_reference_position;
        }));
        bool observed_equal_key_duplicate = false;
        for (std::size_t ordinal = 1u; ordinal < actual.size(); ++ordinal) {
            if (std::ranges::equal(
                    std::as_bytes(actual.subspan(ordinal - 1u, 1u)),
                    std::as_bytes(actual.subspan(ordinal, 1u)))) {
                observed_equal_key_duplicate = true;
                break;
            }
        }
        REQUIRE(observed_equal_key_duplicate);
        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.waves == 1u);
        REQUIRE(evidence.queries == queries.size());
        REQUIRE(evidence.query_occurrences == query.size());
        REQUIRE(evidence.unique_query_plans == 2u);
        REQUIRE(evidence.packed_occurrence_waves == 1u);
        REQUIRE(evidence.wide_occurrence_waves == 0u);
        REQUIRE(evidence.occurrence_fill_ns > 0u);
        REQUIRE(evidence.occurrence_radix_ns > 0u);
        REQUIRE(evidence.control_count_pass_ns > 0u);
        REQUIRE(evidence.orientation_layout_ns > 0u);
        REQUIRE(evidence.direct_emission_ns > 0u);
        REQUIRE(evidence.local_sort_ns > 0u);
        REQUIRE(evidence.tie_normalization_ns > 0u);
        REQUIRE(evidence.control_count_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes
            == evidence.pass3_position_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.local_sort_bytes > 0u);
        REQUIRE(evidence.tie_normalization_bytes > 0u);
        REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
        REQUIRE(batch.admitted_bytes()
            == evidence.exact_final_bytes
                + queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(evidence.peak_exact_final_bytes
            == evidence.exact_final_bytes);
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == queries.size());
        REQUIRE(evidence.compositional_sortedness_receipts
            == queries.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE(evidence.peak_occurrence_capacity_bytes > 0u);
        REQUIRE(evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(evidence.peak_query_radix_capacity_bytes > 0u);
        REQUIRE(evidence.peak_occurrence_count_capacity_bytes > 0u);
        REQUIRE(evidence.peak_block_layout_capacity_bytes > 0u);
        REQUIRE(evidence.peak_block_stats_capacity_bytes > 0u);
        REQUIRE_FALSE(evidence.saturated);
        const query_occurrence_capacity_snapshot occurrence_snapshot =
            scratch->occurrence_capacity_snapshot();
        REQUIRE(occurrence_snapshot.route
            == query_occurrence_route::packed);
        REQUIRE(occurrence_snapshot.packed_size == query.size());
        REQUIRE(occurrence_snapshot.wide_size == 0u);
        REQUIRE(occurrence_snapshot.query_plan_size == 0u);
        REQUIRE(occurrence_snapshot.query_plan_capacity == 0u);
        REQUIRE(occurrence_snapshot.query_plan_reserve_request == 0u);
        REQUIRE(occurrence_snapshot.bucket_direct_matcher_active);
        REQUIRE(occurrence_snapshot.
            query_membership_filter_capacity_bytes == 16u * 1024u * 1024u);
        REQUIRE(occurrence_snapshot.occurrence_radix_invoked);
        REQUIRE(occurrence_snapshot.aggregate_occurrence_capacity_bytes
            == evidence.peak_occurrence_capacity_bytes);
        scratch->complete_wave_after_join();
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("compact-direct P64 decodes postings once into canonical slices") {
        auto compact = make_fragment_compact_direct_test_index(
            generation->head,
            generation->manifest,
            pairs,
            10u);
        REQUIRE(compact.view.header.posting_width_bytes == 8u);
        bioseq::parallelism::memory_admission_controller admission;
        selected_page_advice_recorder selected_page_recorder;
        auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            4u,
            own_fragment_compact_direct_test_index(compact),
            selected_page_recorder.token());
        REQUIRE(reader.has_value());
        REQUIRE(selected_page_recorder.record_count.load() == 0u);
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected = ordinary_seed_anchors(
            pairs, target_length, query, query_length, 4, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            4,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                query, query_length, 4, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto& evidence = batch.stats();
        REQUIRE(evidence.compact_direct_selected_postings == pairs.size());
        REQUIRE(evidence.compact_direct_selection_shards == 2u);
        REQUIRE(evidence.compact_direct_shard_query_layouts == 2u);
        REQUIRE(evidence.compact_direct_layout_shards_completed == 2u);
        REQUIRE(evidence.compact_direct_emission_shards_completed == 2u);
        REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
        REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
        REQUIRE(evidence.compact_direct_emission_postings_visited
            == pairs.size());
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes
            == pairs.size() * compact.view.header.posting_width_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(selected_page_recorder.overflow.load());
        REQUIRE(selected_page_recorder.record_count.load() == 2u);
        const std::uintptr_t postings_begin =
            reinterpret_cast<std::uintptr_t>(compact.view.image.data())
            + compact.view.header.postings_offset;
        const std::uintptr_t postings_end = postings_begin
            + compact.view.header.postings_bytes;
        std::uint64_t advised_bytes = 0u;
        for (std::size_t ordinal = 0u; ordinal < 2u; ++ordinal) {
            const auto range = selected_page_recorder.records[ordinal];
            REQUIRE(range.begin >= postings_begin);
            REQUIRE(range.end <= postings_end);
            REQUIRE(range.begin < range.end);
            advised_bytes += range.end - range.begin;
        }
        REQUIRE(advised_bytes
            == pairs.size() * compact.view.header.posting_width_bytes);
        scratch->complete_wave_after_join();
    }

    SECTION("late cross-segment max-occ rejection drops only the heavy hash") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(
            admission, *generation, sink.rows, 3u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = std::span<const minimap_sketch_data>{query},
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected = ordinary_seed_anchors(
            pairs, target_length, query, query_length, 3, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 3, *scratch);
        const auto actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                query, query_length, 3, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(actual.size() == 1u);
        REQUIRE(actual.front().ref_position == 21u);
        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.pass2_position_bytes > 0u);
        REQUIRE(evidence.pass2_position_bytes
            == evidence.pass3_position_bytes);
        REQUIRE(evidence.anchors_constructed == actual.size());
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == 1u);
        REQUIRE(evidence.forbidden_buffers_are_zero());
        scratch->complete_wave_after_join();
    }

    SECTION("all-heavy rejection attributes validation without layout work") {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(
            admission, *generation, sink.rows, 3u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const auto heavy_query =
            std::span<const minimap_sketch_data>{query}.first(3u);
        const std::array queries{
            fragment_seed_query{
                .minimizers = heavy_query,
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 3, *scratch);
        const auto actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                heavy_query, query_length, 3, kmer_size);
        REQUIRE(actual.empty());

        const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
        REQUIRE(evidence.waves == 1u);
        REQUIRE(evidence.queries == queries.size());
        REQUIRE(evidence.control_count_bytes > 0u);
        REQUIRE(evidence.rejected_control_validation_ns > 0u);
        REQUIRE(evidence.rejected_control_validation_bytes
            > evidence.control_count_bytes);
        REQUIRE(evidence.orientation_layout_ns == 0u);
        REQUIRE(evidence.direct_emission_ns == 0u);
        REQUIRE(evidence.local_sort_ns == 0u);
        REQUIRE(evidence.tie_normalization_ns == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        REQUIRE(evidence.anchors_constructed == 0u);
        REQUIRE(evidence.exact_final_bytes == 0u);
        REQUIRE(evidence.peak_exact_final_bytes == 0u);
        REQUIRE(batch.admitted_bytes()
            == queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(evidence.global_sort_calls_invoked == 0u);
        REQUIRE(evidence.global_sort_calls_skipped == queries.size());
        REQUIRE(evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(evidence.saturated);
        scratch->complete_wave_after_join();
    }
}

TEST_CASE("selective pass byte receipts are exact for accepted rejected and mixed work",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t control_count = 64u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(control_count + 1u);
    for (std::uint64_t hash = 1u; hash <= control_count; ++hash) {
        pairs.emplace_back(hash, (hash + 10u) << 1u);
        if (hash == 7u) {
            pairs.emplace_back(hash, (hash + 100u) << 1u);
        }
    }
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(256u, 65'534u);
    options.position_segment_span_bp = std::uint64_t{1} << 15u;
    const auto generation = encode_minimizer_generation(
        producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width
        == minimizer_scalar_width::u32);
    REQUIRE(generation->manifest.count_width
        == minimizer_scalar_width::u16);
    REQUIRE(generation->manifest.position_width
        == minimizer_scalar_width::u16);
    REQUIRE(generation->directory.entries().size() == 1u);
    REQUIRE(generation->directory.entries().front().shape_offset_width
        == minimizer_scalar_width::u16);

    constexpr std::uint64_t hash_scan_bytes = control_count * 4u;
    constexpr std::uint64_t control_scan_bytes =
        control_count * (4u + 2u);
    constexpr std::uint64_t one_matched_count_bytes = 2u;
    constexpr std::uint64_t one_prefix_pair_bytes = 2u * 2u;
    constexpr std::uint64_t one_position_bytes = 2u;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array accepted_minimizers{
        minimap_sketch_data{control_count, 20u, 0u},
    };
    const std::array rejected_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array mixed_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{9u, 21u, 1u},
    };

    const auto run = [&](std::span<const minimap_sketch_data> minimizers) {
        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(
            admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::array queries{
            fragment_seed_query{
                .minimizers = minimizers,
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 1, *scratch);
        const std::size_t anchor_count = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                minimizers, query_length, 1, kmer_size).size();
        const prepared_fragment_anchor_batch_stats evidence = batch.stats();
        scratch->complete_wave_after_join();
        return std::pair{evidence, anchor_count};
    };

    SECTION("accepted") {
        const auto [evidence, anchor_count] = run(accepted_minimizers);
        REQUIRE(anchor_count == 1u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes == 0u);
        REQUIRE(evidence.pass2_control_bytes
            == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes
            == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_position_bytes == one_position_bytes);
        REQUIRE(evidence.pass3_control_bytes
            == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass3_accepted_prefix_bytes
            == one_prefix_pair_bytes);
        REQUIRE(evidence.pass3_position_bytes == one_position_bytes);
    }

    SECTION("rejected") {
        const auto [evidence, anchor_count] = run(rejected_minimizers);
        REQUIRE(anchor_count == 0u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes
            == hash_scan_bytes + one_matched_count_bytes
                + one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_control_bytes == 0u);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_control_bytes == 0u);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
    }

    SECTION("mixed") {
        const auto [evidence, anchor_count] = run(mixed_minimizers);
        REQUIRE(anchor_count == 1u);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes
            == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_control_bytes
            == hash_scan_bytes + 2u * one_matched_count_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes
            == one_prefix_pair_bytes);
        REQUIRE(evidence.pass2_position_bytes == one_position_bytes);
        REQUIRE(evidence.pass3_control_bytes
            == hash_scan_bytes + one_matched_count_bytes);
        REQUIRE(evidence.pass3_accepted_prefix_bytes
            == one_prefix_pair_bytes);
        REQUIRE(evidence.pass3_position_bytes == one_position_bytes);
    }
}

TEST_CASE("checked dynamic reader covers P and C width boundaries",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{
                query_minimizers},
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    const auto verify = [&](std::uint64_t segment_span,
                            std::uint64_t max_occ_cap,
                            minimizer_scalar_width expected_position_width,
                            minimizer_scalar_width expected_count_width) {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(
            segment_span, max_occ_cap);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(
            producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width
            == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.position_width
            == expected_position_width);
        REQUIRE(generation->manifest.count_width
            == expected_count_width);
        REQUIRE(generation->directory.entries().size() == 1u);
        REQUIRE(generation->directory.entries().front().shape_offset_width
            == minimizer_scalar_width::u16);

        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(
            admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 1, *scratch);
        const auto anchors = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                query_minimizers, query_length, 1, kmer_size);
        REQUIRE(anchors.size() == 1u);
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes =
            4u + static_cast<std::uint8_t>(expected_count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.pass2_control_bytes == control_scan_bytes);
        REQUIRE(evidence.pass3_control_bytes == control_scan_bytes);
        REQUIRE(evidence.pass2_accepted_prefix_bytes == 4u);
        REQUIRE(evidence.pass3_accepted_prefix_bytes == 4u);
        REQUIRE(evidence.pass2_position_bytes
            == static_cast<std::uint8_t>(expected_position_width));
        REQUIRE(evidence.pass3_position_bytes
            == static_cast<std::uint8_t>(expected_position_width));
        scratch->complete_wave_after_join();
    };

    SECTION("P32 lower boundary") {
        verify(
            (std::uint64_t{1} << 15u) + 1u,
            65'534u,
            minimizer_scalar_width::u32,
            minimizer_scalar_width::u16);
    }
    SECTION("P64 lower boundary") {
        verify(
            (std::uint64_t{1} << 31u) + 1u,
            65'534u,
            minimizer_scalar_width::u64,
            minimizer_scalar_width::u16);
    }
    SECTION("C32 lower boundary with P16") {
        verify(
            std::uint64_t{1} << 15u,
            65'535u,
            minimizer_scalar_width::u16,
            minimizer_scalar_width::u32);
    }
}


TEST_CASE("O32 leaf is exact for fixed and dynamic count profiles",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t position_count = 65'536u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(position_count);
    for (std::uint64_t encoded = 0u; encoded < position_count; ++encoded) {
        pairs.emplace_back(7u, encoded);
    }
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{
                query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    const auto verify = [&](std::uint64_t max_occ_cap,
                            minimizer_scalar_width count_width) {
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(
            std::uint64_t{1} << 15u, max_occ_cap);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(
            producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width
            == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.count_width
            == count_width);
        REQUIRE(generation->manifest.position_width
            == minimizer_scalar_width::u16);
        REQUIRE(generation->directory.entries().size() == 1u);
        REQUIRE(generation->directory.entries().front().
            position_element_count == position_count);
        REQUIRE(generation->directory.entries().front().shape_offset_width
            == minimizer_scalar_width::u32);

        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(
            admission, *generation, sink.rows, 1u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 1, *scratch);
        REQUIRE(batch.query_view(0u).ordered_fragment_seed_anchors(
            query_minimizers, 100u, 1, 15).empty());
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes =
            4u + static_cast<std::uint8_t>(count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes
            == control_scan_bytes + 2u * 4u);
        REQUIRE(evidence.pass2_control_bytes == 0u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_control_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        scratch->complete_wave_after_join();
    };

    SECTION("fixed H32 C16 P16 O32 leaf") {
        verify(65'534u, minimizer_scalar_width::u16);
    }
    SECTION("checked dynamic C32 O32 boundary") {
        verify(65'535u, minimizer_scalar_width::u32);
    }
}


TEST_CASE("O64 fast leaf and C64 dynamic rejection never walk positions",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(sizeof(std::size_t) >= sizeof(std::uint64_t));
    constexpr std::uint64_t position_count =
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)()) + 1u;
    constexpr std::uint64_t segment_span = std::uint64_t{1} << 15u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> base_pairs{
        {7u, 0u},
    };
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{
                query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    const auto verify = [&](std::uint64_t max_occ_cap,
                            minimizer_scalar_width expected_count_width,
                            std::uint64_t encoded_local_count) {
        vector_replay_minimizer_producer producer{base_pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(
            100u, max_occ_cap);
        options.position_segment_span_bp = segment_span;
        const auto encoded = encode_minimizer_generation(
            producer, sink, options);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->manifest.hash_width
            == minimizer_scalar_width::u32);
        REQUIRE(encoded->manifest.count_width
            == expected_count_width);
        REQUIRE(encoded->manifest.position_width
            == minimizer_scalar_width::u16);
        REQUIRE(encoded->directory.entries().size() == 1u);

        auto synthetic = *encoded;
        auto entry = synthetic.directory.entries().front();
        entry.position_element_count = position_count;
        entry.shape_offset_width = minimizer_scalar_width::u64;
        const auto layout = plan_grouped_soa_control_plane(
            entry.control_element_count,
            synthetic.manifest.hash_width,
            entry.shape_offset_width,
            synthetic.manifest.count_width);
        REQUIRE(layout.has_value());
        entry.control_encoded_bytes = layout->encoded_bytes;
        entry.position_encoded_bytes = position_count * 2u;
        const std::uint64_t extent_bytes = minimizer_extent_payload_bytes(
            minimizer_generation_layout::
                grouped_soa_segment_local);
        entry.control_chunk_count = static_cast<std::uint32_t>(
            entry.control_encoded_bytes / extent_bytes
            + (entry.control_encoded_bytes % extent_bytes != 0u ? 1u : 0u));
        entry.position_chunk_count = static_cast<std::uint32_t>(
            entry.position_encoded_bytes / extent_bytes
            + (entry.position_encoded_bytes % extent_bytes != 0u ? 1u : 0u));
        const std::array entries{entry};
        synthetic.manifest.control_count = 1u;
        synthetic.manifest.position_count = position_count;
        synthetic.manifest.payload_bytes =
            entry.control_encoded_bytes + entry.position_encoded_bytes;
        const auto metadata = rebuild_generation_metadata(
            synthetic, entries);

        auto payload = make_sparse_mapped_test_payload(
            metadata.manifest.payload_bytes);
        REQUIRE(payload.has_value());
        auto mutable_payload = payload->bytes();
        test_write_le<std::uint32_t>(mutable_payload, 0u, 7u);
        test_write_le<std::uint64_t>(
            mutable_payload,
            static_cast<std::size_t>(layout->prefix_offset),
            0u);
        test_write_le<std::uint64_t>(
            mutable_payload,
            static_cast<std::size_t>(layout->prefix_offset + 8u),
            position_count);
        if (expected_count_width == minimizer_scalar_width::u16) {
            test_write_le<std::uint16_t>(
                mutable_payload,
                static_cast<std::size_t>(layout->count_offset),
                static_cast<std::uint16_t>(encoded_local_count));
        } else {
            REQUIRE(expected_count_width == minimizer_scalar_width::u64);
            test_write_le<std::uint64_t>(
                mutable_payload,
                static_cast<std::size_t>(layout->count_offset),
                encoded_local_count);
        }

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload->owner;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            metadata.head,
            metadata.manifest,
            test_grouped_soa_expected_storage_contract(max_occ_cap),
            metadata.directory_bytes,
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{
                mutable_payload.data(), mutable_payload.size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 1, *scratch);
        REQUIRE(batch.query_view(0u).ordered_fragment_seed_anchors(
            query_minimizers, 100u, 1, 15).empty());
        const auto& evidence = batch.stats();
        const std::uint64_t control_scan_bytes =
            4u + static_cast<std::uint8_t>(expected_count_width);
        REQUIRE(evidence.control_count_bytes == control_scan_bytes);
        REQUIRE(evidence.rejected_control_validation_bytes
            == control_scan_bytes + 2u * 8u);
        REQUIRE(evidence.pass2_position_bytes == 0u);
        REQUIRE(evidence.pass3_position_bytes == 0u);
        scratch->complete_wave_after_join();
    };

    SECTION("fixed H32 C16 P16 O64 leaf") {
        verify(
            65'534u,
            minimizer_scalar_width::u16,
            65'535u);
    }
    SECTION("checked dynamic C64 count above UINT32 rejects before narrowing") {
        verify(
            (std::numeric_limits<std::uint32_t>::max)(),
            minimizer_scalar_width::u64,
            position_count);
    }
}


TEST_CASE("H64 reader open fails before admission or payload access",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {1u, 10u << 1u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(100u, 65'534u);
    options.position_segment_span_bp = std::uint64_t{1} << 15u;
    options.kmer_size = 17u;
    const auto generation = encode_minimizer_generation(
        producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width
        == minimizer_scalar_width::u64);
    REQUIRE(generation->manifest.max_hash
        > (std::numeric_limits<std::uint32_t>::max)());

    auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
    auto expected = test_grouped_soa_expected_storage_contract(65'534u);
    expected.kmer_size = 17u;
    bioseq::parallelism::memory_admission_controller admission;
    REQUIRE(admission.reserved_bytes() == 0u);
    std::shared_ptr<const void> payload_owner = payload;
    const auto opened = prepared_minimizer_fragment_reader::open(
        admission,
        generation->head,
        generation->manifest,
        expected,
        generation->directory.encoded_bytes(),
        1u,
        std::move(payload_owner),
        std::span<const std::byte>{payload->data(), payload->size()});
    require_wire_error(
        opened, minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(admission.reserved_bytes() == 0u);
}


TEST_CASE("fixed and dynamic probes reject selected payload corruption",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::array query_minimizers{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{
                query_minimizers},
            .query_length = 100u,
            .kmer_size = 15,
        },
    };

    SECTION("fixed O16 prefix escapes the planned position extent") {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
            {7u, 20u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(100u, 65'534u);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(
            producer, sink, options);
        REQUIRE(generation.has_value());
        const auto& entry = generation->directory.entries().front();
        REQUIRE(entry.shape_offset_width == minimizer_scalar_width::u16);
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        const auto layout = plan_grouped_soa_control_plane(
            entry.control_element_count,
            generation->manifest.hash_width,
            entry.shape_offset_width,
            generation->manifest.count_width);
        REQUIRE(layout.has_value());
        test_write_le<std::uint16_t>(
            std::span<std::byte>{payload->data(), payload->size()},
            static_cast<std::size_t>(layout->prefix_offset + 2u),
            3u);

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            2u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(
            reader->prepare_fragment_anchor_batch(
                admission, queries, 2, *scratch),
            persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("fixed Pass1 authenticates unmatched H32 and C16 controls") {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
            {9u, 20u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(100u, 65'534u);
        options.position_segment_span_bp = std::uint64_t{1} << 15u;
        const auto generation = encode_minimizer_generation(
            producer, sink, options);
        REQUIRE(generation.has_value());
        const auto& entry = generation->directory.entries().front();
        REQUIRE(entry.control_element_count == 2u);
        REQUIRE(entry.shape_offset_width == minimizer_scalar_width::u16);
        const auto layout = plan_grouped_soa_control_plane(
            entry.control_element_count,
            generation->manifest.hash_width,
            entry.shape_offset_width,
            generation->manifest.count_width);
        REQUIRE(layout.has_value());
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        auto mutable_payload =
            std::span<std::byte>{payload->data(), payload->size()};

        SECTION("unmatched count is zero") {
            test_write_le<std::uint16_t>(
                mutable_payload,
                static_cast<std::size_t>(layout->count_offset)
                    + sizeof(std::uint16_t),
                0u);
        }
        SECTION("hash order is not strict") {
            test_write_le<std::uint32_t>(
                mutable_payload,
                static_cast<std::size_t>(layout->hash_offset)
                    + sizeof(std::uint32_t),
                7u);
        }

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            2u,
            std::move(payload_owner),
            std::span<const std::byte>{
                mutable_payload.data(), mutable_payload.size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(
            reader->prepare_fragment_anchor_batch(
                admission, queries, 2, *scratch),
            persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    SECTION("dynamic P64 position escapes the authenticated segment core") {
        constexpr std::uint64_t segment_span =
            (std::uint64_t{1} << 31u) + 1u;
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
            {7u, 10u << 1u},
        };
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(
            segment_span, 65'534u);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(
            producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.position_width
            == minimizer_scalar_width::u64);
        const auto& entry = generation->directory.entries().front();
        auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
        test_write_le<std::uint64_t>(
            std::span<std::byte>{payload->data(), payload->size()},
            static_cast<std::size_t>(entry.control_encoded_bytes),
            segment_span << 1u);

        bioseq::parallelism::memory_admission_controller admission;
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(65'534u),
            generation->directory.encoded_bytes(),
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        REQUIRE_THROWS_AS(
            reader->prepare_fragment_anchor_batch(
                admission, queries, 1, *scratch),
            persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
    }
}


TEST_CASE("fragment blocks preserve segment runs beyond ordinal 128",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t segment_span =
        k_minimizer_preferred_segment_span_bp;
    constexpr auto packed = [](
            std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(132u);
    for (std::uint64_t hash = 1u; hash <= 130u; ++hash) {
        pairs.emplace_back(hash, packed(hash + 1u, 0u));
    }
    // Hash 1 straddles the oversized segment-0 block and the block after the
    // empty segment-1 gap. max_occ therefore has to be reduced globally.
    pairs.emplace_back(1u, packed(2u * segment_span + 4u, 0u));
    pairs.emplace_back(500u, packed(2u * segment_span + 5u, 1u));

    // The planner's fixed-width limit shrinks physical segment geometry; it
    // does not split one segment into directory entries. Build the exact valid
    // wire image here so the reader really sees an equal-segment run crossing
    // its 128-entry block target.
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(
            2u * segment_span + 100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_span_bp == segment_span);
    const auto profile = generation->directory.profile();
    REQUIRE(profile.hash_width == minimizer_scalar_width::u32);
    REQUIRE(profile.position_width == minimizer_scalar_width::u32);
    REQUIRE(profile.count_width == minimizer_scalar_width::u16);
    constexpr minimizer_scalar_width offset_width =
        minimizer_scalar_width::u16;
    const auto control_layout = plan_grouped_soa_control_plane(
        1u,
        profile.hash_width,
        offset_width,
        profile.count_width);
    REQUIRE(control_layout.has_value());

    std::vector<grouped_soa_bucket_directory_entry> manual_entries;
    std::vector<std::byte> control_image;
    std::vector<std::byte> position_image;
    std::vector<std::uint64_t> control_row_checksums;
    std::vector<std::uint64_t> position_row_checksums;
    manual_entries.reserve(pairs.size());
    control_image.reserve(
        pairs.size() * static_cast<std::size_t>(
            control_layout->encoded_bytes));
    position_image.reserve(pairs.size() * sizeof(std::uint32_t));
    control_row_checksums.reserve(pairs.size());
    position_row_checksums.reserve(pairs.size());

    const auto append_manual_entry = [&](
            std::int32_t position_segment,
            std::uint64_t hash,
            std::uint64_t global_packed_position) {
        const auto core = derive_minimizer_position_segment_bounds(
            generation->manifest,
            static_cast<std::uint32_t>(position_segment));
        REQUIRE(core.has_value());
        const std::uint64_t global_position =
            global_packed_position >> 1u;
        REQUIRE(global_position >= core->begin_bp);
        REQUIRE(global_position < core->end_bp);
        const std::uint64_t local_packed_position =
            ((global_position - core->begin_bp) << 1u)
            | (global_packed_position & 1u);
        REQUIRE(local_packed_position
            <= (std::numeric_limits<std::uint32_t>::max)());

        std::vector<std::byte> controls(
            static_cast<std::size_t>(control_layout->encoded_bytes));
        test_write_le<std::uint32_t>(
            controls,
            static_cast<std::size_t>(control_layout->hash_offset),
            static_cast<std::uint32_t>(hash));
        test_write_le<std::uint16_t>(
            controls,
            static_cast<std::size_t>(control_layout->prefix_offset),
            0u);
        test_write_le<std::uint16_t>(
            controls,
            static_cast<std::size_t>(control_layout->prefix_offset)
                + sizeof(std::uint16_t),
            1u);
        test_write_le<std::uint16_t>(
            controls,
            static_cast<std::size_t>(control_layout->count_offset),
            1u);
        std::vector<std::byte> positions(sizeof(std::uint32_t));
        test_write_le<std::uint32_t>(
            positions,
            0u,
            static_cast<std::uint32_t>(local_packed_position));

        const auto partition = static_cast<std::uint32_t>(
            manual_entries.size());
        grouped_soa_bucket_directory_entry entry{
            .partition_id = partition,
            .position_segment = position_segment,
            .lo_hash = hash,
            .hi_hash = hash,
            .sub_key = static_cast<index_sub_key_t>(partition),
            .control_first_chunk = 0u,
            .control_chunk_count = 1u,
            .position_first_chunk = 0u,
            .position_chunk_count = 1u,
            .control_element_count = 1u,
            .position_element_count = 1u,
            .control_encoded_bytes = controls.size(),
            .position_encoded_bytes = positions.size(),
            .control_checksum = crc64_ecma182(controls),
            .position_checksum = crc64_ecma182(positions),
            .shape_offset_width = offset_width,
        };
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        REQUIRE(position_sub_key.has_value());
        control_row_checksums.push_back(crc64_index_chunk_row(
            generation->manifest.generation_id,
            position_segment,
            entry.sub_key,
            0u,
            static_cast<std::uint32_t>(controls.size()),
            controls));
        position_row_checksums.push_back(crc64_index_chunk_row(
            generation->manifest.generation_id,
            position_segment,
            *position_sub_key,
            0u,
            static_cast<std::uint32_t>(positions.size()),
            positions));
        control_image.insert(
            control_image.end(), controls.begin(), controls.end());
        position_image.insert(
            position_image.end(), positions.begin(), positions.end());
        manual_entries.push_back(entry);
    };
    for (std::uint64_t hash = 1u; hash <= 130u; ++hash) {
        append_manual_entry(0, hash, packed(hash + 1u, 0u));
    }
    append_manual_entry(2, 1u, packed(2u * segment_span + 4u, 0u));
    append_manual_entry(2, 500u, packed(2u * segment_span + 5u, 1u));

    crc64_ecma182_accumulator payload_checksum;
    for (const std::uint64_t checksum : control_row_checksums) {
        payload_checksum.update_little_endian(checksum);
    }
    for (const std::uint64_t checksum : position_row_checksums) {
        payload_checksum.update_little_endian(checksum);
    }
    auto adjusted_generation = *generation;
    adjusted_generation.manifest.control_count = manual_entries.size();
    adjusted_generation.manifest.position_count = manual_entries.size();
    adjusted_generation.manifest.payload_bytes =
        control_image.size() + position_image.size();
    adjusted_generation.manifest.payload_checksum =
        payload_checksum.checksum();
    adjusted_generation.manifest.extent_count =
        static_cast<std::uint32_t>(manual_entries.size() * 2u);
    const std::uint64_t directory_bytes = manual_entries.size()
        * minimizer_directory_entry_encoded_bytes(profile);
    adjusted_generation.manifest.directory_chunk_count =
        static_cast<std::uint32_t>(
            directory_bytes / k_grouped_soa_extent_payload_bytes
            + (directory_bytes % k_grouped_soa_extent_payload_bytes != 0u
                ? 1u : 0u));
    const auto metadata = rebuild_generation_metadata(
        adjusted_generation, manual_entries);
    const auto directory = grouped_soa_directory_view::decode(
        metadata.directory_bytes, profile);
    REQUIRE(directory.has_value());
    REQUIRE(validate_minimizer_generation_metadata(
        metadata.head, metadata.manifest, *directory).has_value());
    auto mapped_payload = std::make_shared<std::vector<std::byte>>();
    mapped_payload->reserve(
        control_image.size() + position_image.size());
    mapped_payload->insert(
        mapped_payload->end(), control_image.begin(), control_image.end());
    mapped_payload->insert(
        mapped_payload->end(), position_image.begin(), position_image.end());
    REQUIRE(mapped_payload->size() == metadata.manifest.payload_bytes);

    const auto entries = directory->entries();
    REQUIRE(entries.size() == pairs.size());
    REQUIRE(entries.size() > 128u);
    REQUIRE(entries[127].position_segment == 0);
    REQUIRE(entries[128].position_segment == 0);
    REQUIRE(entries[129].position_segment == 0);
    REQUIRE(entries[130].position_segment == 2);
    REQUIRE(std::ranges::count_if(entries, [](const auto& entry) {
        return entry.position_segment == 0;
    }) == 130);
    REQUIRE(std::ranges::count_if(entries, [](const auto& entry) {
        return entry.position_segment == 1;
    }) == 0);
    REQUIRE(std::ranges::count_if(entries, [](const auto& entry) {
        return entry.position_segment == 2;
    }) == 2);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{1u, 14u, 0u},
        minimap_sketch_data{127u, 15u, 0u},
        minimap_sketch_data{128u, 16u, 0u},
        minimap_sketch_data{129u, 17u, 0u},
        minimap_sketch_data{130u, 18u, 0u},
        minimap_sketch_data{500u, 19u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    auto compact = make_fragment_compact_direct_test_index(
        metadata.head,
        metadata.manifest,
        pairs,
        10u);
    bioseq::parallelism::memory_admission_controller admission;
    std::shared_ptr<const void> payload_owner = mapped_payload;
    auto reader = prepared_minimizer_fragment_reader::open(
        admission,
        metadata.head,
        metadata.manifest,
        test_grouped_soa_expected_storage_contract(10u),
        metadata.directory_bytes,
        1u,
        std::move(payload_owner),
        std::span<const std::byte>{
            mapped_payload->data(), mapped_payload->size()},
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    const std::array queries{
        fragment_seed_query{
            .minimizers = std::span<const minimap_sketch_data>{query},
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected = ordinary_seed_anchors(
        pairs,
        2u * segment_span + 100u,
        query,
        query_length,
        1,
        kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(
        admission, queries, 1, *scratch);
    const auto actual = batch.query_view(0u).
        ordered_fragment_seed_anchors(
            query, query_length, 1, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == query.size() - 1u);
    REQUIRE(std::ranges::none_of(actual, [segment_span](const auto& anchor) {
        return anchor.ref_position == 2u
            || anchor.ref_position == 2u * segment_span + 4u;
    }));
    REQUIRE(actual.back().ref_position == 2u * segment_span + 5u);
    REQUIRE(actual.back().is_reverse == 1u);
    scratch->complete_wave_after_join();

    auto direct_batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        1,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto direct = direct_batch.query_view(0u).
        ordered_fragment_seed_anchors(
            query, query_length, 1, kmer_size);
    require_anchor_bytes_equal(direct, expected);
    REQUIRE(direct_batch.stats().compact_direct_unique_lookups
        == query.size());
    REQUIRE(direct_batch.stats().compact_direct_selected_hashes == 5u);
    REQUIRE(direct_batch.stats().compact_direct_runtime_rejected_hashes
        == 1u);
    REQUIRE(direct_batch.stats().compact_direct_selected_postings == 5u);
    REQUIRE(direct_batch.stats().compact_direct_selection_shards == 5u);
    REQUIRE(direct_batch.stats().compact_direct_shard_query_layouts == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_shards_completed
        == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_shards_completed
        == 5u);
    REQUIRE(direct_batch.stats().compact_direct_posting_visit_passes
        == 1u);
    REQUIRE(direct_batch.stats().compact_direct_layout_postings_visited
        == 0u);
    REQUIRE(direct_batch.stats().compact_direct_emission_postings_visited
        == 5u);
    REQUIRE(direct_batch.stats().
        compact_direct_layout_selection_callbacks == 5u);
    REQUIRE(direct_batch.stats().
        compact_direct_emission_selection_callbacks == 5u);
    REQUIRE(direct_batch.stats().compact_direct_layout_spans_exposed
        == 5u);
    REQUIRE(direct_batch.stats().compact_direct_emission_spans_exposed
        == 5u);
    REQUIRE(direct_batch.stats().
        compact_direct_layout_descriptors_examined == 5u);
    REQUIRE(direct_batch.stats().
        compact_direct_emission_descriptors_examined == 5u);
    REQUIRE(direct_batch.stats().
        compact_direct_traversal_rank_checkpoints_examined == 0u);
    REQUIRE(direct_batch.stats().
        compact_direct_traversal_membership_words_examined == 0u);
    REQUIRE(direct_batch.stats().pass2_position_bytes == 0u);
    REQUIRE(direct_batch.stats().pass3_position_bytes
        == 5u * compact.view.header.posting_width_bytes);
    REQUIRE(direct_batch.stats().compact_direct_target_control_scan_bytes
        == 0u);
    REQUIRE(direct_batch.stats().compact_direct_fallbacks == 0u);
    scratch->complete_wave_after_join();
}

TEST_CASE("query-wave scratch recycles without stale anchors",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](
            std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(10u, 0u)},
        {7u, packed(20u, 1u)},
        {8u, packed(30u, 0u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(
        admission, *generation, sink.rows, 2u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    const std::vector<minimap_sketch_data> first_query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{7u, 25u, 1u},
    };
    const std::vector<minimap_sketch_data> empty_query{};
    const std::vector<minimap_sketch_data> third_query{
        minimap_sketch_data{8u, 30u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;

    {
        const std::array queries{
            fragment_seed_query{
                std::span<const minimap_sketch_data>{first_query},
                query_length,
                kmer_size},
            fragment_seed_query{
                std::span<const minimap_sketch_data>{empty_query},
                query_length,
                kmer_size},
            fragment_seed_query{
                std::span<const minimap_sketch_data>{third_query},
                query_length,
                kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 2, *scratch);
        REQUIRE(batch.query_count() == queries.size());
        const auto first_expected = ordinary_seed_anchors(
            pairs, 100u, first_query, query_length, 2, kmer_size);
        const auto third_expected = ordinary_seed_anchors(
            pairs, 100u, third_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(
            batch.query_view(0u).ordered_fragment_seed_anchors(
                first_query, query_length, 2, kmer_size),
            first_expected);
        REQUIRE(batch.query_view(1u).ordered_fragment_seed_anchors(
            empty_query, query_length, 2, kmer_size).empty());
        require_anchor_bytes_equal(
            batch.query_view(2u).ordered_fragment_seed_anchors(
                third_query, query_length, 2, kmer_size),
            third_expected);
        scratch->complete_wave_after_join();
    }
    const std::uint64_t retained_after_first_wave =
        scratch->admitted_bytes();
    REQUIRE(retained_after_first_wave > 0u);

    const std::vector<minimap_sketch_data> last_query{
        minimap_sketch_data{8u, 14u, 0u},
    };
    {
        const std::array queries{
            fragment_seed_query{
                std::span<const minimap_sketch_data>{last_query},
                query_length,
                kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 2, *scratch);
        const auto view = batch.query_view(0u);
        const auto actual = view.ordered_fragment_seed_anchors(
            last_query, query_length, 2, kmer_size);
        const auto expected = ordinary_seed_anchors(
            pairs, 100u, last_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        REQUIRE(actual.size() == 1u);

        const std::vector<minimap_sketch_data> copied_query = last_query;
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(
                copied_query, query_length, 2, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(
                last_query, query_length + 1u, 2, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(
                last_query, query_length, 1, kmer_size),
            persisted_minimizer_lookup_error);
        REQUIRE_THROWS_AS(
            view.ordered_fragment_seed_anchors(
                last_query, query_length, 2, kmer_size + 1),
            persisted_minimizer_lookup_error);
        scratch->complete_wave_after_join();
    }
    REQUIRE(scratch->admitted_bytes() == retained_after_first_wave);

    {
        const std::array<fragment_seed_query, 0u> empty_wave{};
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, empty_wave, 2, *scratch);
        REQUIRE(batch.query_count() == 0u);
        scratch->complete_wave_after_join();
        REQUIRE_FALSE(scratch->wave_outstanding());
    }

    constexpr bioseq::storage::sequence_size_t packed_query_limit =
        bioseq::storage::sequence_size_t{1} << 28u;
    const std::vector<minimap_sketch_data> boundary_query{
        minimap_sketch_data{
            8u,
            static_cast<std::uint32_t>(packed_query_limit - 1u),
            0u},
    };
    {
        const std::array queries{
            fragment_seed_query{
                std::span<const minimap_sketch_data>{boundary_query},
                packed_query_limit,
                kmer_size},
        };
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 2, *scratch);
        const auto actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                boundary_query,
                packed_query_limit,
                2,
                kmer_size);
        const auto expected = ordinary_seed_anchors(
            pairs,
            100u,
            boundary_query,
            packed_query_limit,
            2,
            kmer_size);
        require_anchor_bytes_equal(actual, expected);
        scratch->complete_wave_after_join();
    }
    const std::array oversized_query{
        fragment_seed_query{
            std::span<const minimap_sketch_data>{boundary_query},
            packed_query_limit + 1u,
            kmer_size},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(
            admission, oversized_query, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());

    const std::array mismatched_kmer_query{
        fragment_seed_query{
            std::span<const minimap_sketch_data>{last_query},
            query_length,
            kmer_size + 1},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(
            admission, mismatched_kmer_query, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());

    const std::vector<minimap_sketch_data> out_of_profile_hash_query{
        minimap_sketch_data{
            static_cast<std::uint64_t>(k_minimap_k15_max_hash) + 1u,
            14u,
            0u},
    };
    const std::array out_of_profile_hash_queries{
        fragment_seed_query{
            std::span<const minimap_sketch_data>{
                out_of_profile_hash_query},
            query_length,
            kmer_size},
    };
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(
            admission, out_of_profile_hash_queries, 2, *scratch),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
}

TEST_CASE("sixteen-query compact packed wave preserves exact receipts",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {7u, 40u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    auto options = test_grouped_soa_generation_options(100u, 10u);
    options.position_segment_span_bp = std::uint64_t{1u} << 15u;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.hash_width
        == minimizer_scalar_width::u32);
    REQUIRE(generation->manifest.count_width
        == minimizer_scalar_width::u16);
    REQUIRE(generation->manifest.position_width
        == minimizer_scalar_width::u16);
    REQUIRE(generation->directory.entries().size() == 1u);
    REQUIRE(generation->directory.entries().front().shape_offset_width
        == minimizer_scalar_width::u16);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(
        admission, *generation, sink.rows, 2u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    const std::vector<minimap_sketch_data> minimizers{
        minimap_sketch_data{7u, 14u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    constexpr std::size_t query_count = 16u;
    const std::vector<fragment_seed_query> queries(
        query_count,
        fragment_seed_query{
            .minimizers = minimizers,
            .query_length = query_length,
            .kmer_size = kmer_size,
        });
    const auto expected = ordinary_seed_anchors(
        pairs, 100u, minimizers, query_length, 2, kmer_size);
    REQUIRE(expected.size() == 2u);

    auto batch = reader->prepare_fragment_anchor_batch(
        admission, queries, 2, *scratch);
    REQUIRE(batch.query_count() == query_count);
    for (std::size_t query_index = 0u;
         query_index < query_count;
         ++query_index) {
        require_anchor_bytes_equal(
            batch.query_view(query_index).ordered_fragment_seed_anchors(
                minimizers, query_length, 2, kmer_size),
            expected);
    }

    constexpr std::uint64_t control_scan_bytes =
        sizeof(std::uint32_t) + sizeof(std::uint16_t);
    constexpr std::uint64_t accepted_prefix_bytes =
        2u * sizeof(std::uint16_t);
    constexpr std::uint64_t position_bytes =
        2u * sizeof(std::uint16_t);
    const prepared_fragment_anchor_batch_stats& evidence = batch.stats();
    REQUIRE(evidence.waves == 1u);
    REQUIRE(evidence.queries == query_count);
    REQUIRE(evidence.query_occurrences == query_count);
    REQUIRE(evidence.unique_query_plans == 1u);
    REQUIRE(evidence.packed_occurrence_waves == 1u);
    REQUIRE(evidence.wide_occurrence_waves == 0u);
    REQUIRE(evidence.control_count_bytes == control_scan_bytes);
    REQUIRE(evidence.rejected_control_validation_bytes == 0u);
    REQUIRE(evidence.pass2_control_bytes == control_scan_bytes);
    REQUIRE(evidence.pass2_accepted_prefix_bytes == accepted_prefix_bytes);
    REQUIRE(evidence.pass2_position_bytes == position_bytes);
    REQUIRE(evidence.pass3_control_bytes == control_scan_bytes);
    REQUIRE(evidence.pass3_accepted_prefix_bytes == accepted_prefix_bytes);
    REQUIRE(evidence.pass3_position_bytes == position_bytes);
    REQUIRE(evidence.anchors_constructed == query_count * expected.size());
    REQUIRE(evidence.exact_final_bytes
        == evidence.anchors_constructed * sizeof(bioseq::chaining::anchor));
    REQUIRE(evidence.peak_exact_final_bytes == evidence.exact_final_bytes);
    REQUIRE(batch.admitted_bytes()
        == evidence.exact_final_bytes
            + query_count * sizeof(prepared_query_anchors));
    REQUIRE(evidence.global_sort_calls_invoked == 0u);
    REQUIRE(evidence.global_sort_calls_skipped == query_count);
    REQUIRE(evidence.compositional_sortedness_receipts == query_count);
    REQUIRE(evidence.forbidden_buffers_are_zero());
    REQUIRE_FALSE(evidence.saturated);

    const query_occurrence_capacity_snapshot occurrence_snapshot =
        scratch->occurrence_capacity_snapshot();
    REQUIRE(occurrence_snapshot.route == query_occurrence_route::packed);
    REQUIRE(occurrence_snapshot.packed_size == query_count);
    REQUIRE(occurrence_snapshot.wide_size == 0u);
    REQUIRE(occurrence_snapshot.query_plan_size == 0u);
    REQUIRE(occurrence_snapshot.query_plan_capacity == 0u);
    REQUIRE(occurrence_snapshot.bucket_direct_matcher_active);
    scratch->complete_wave_after_join();
    REQUIRE_FALSE(scratch->wave_outstanding());
}

TEST_CASE("bucket-direct Bloom matcher is byte-identical to its test oracle",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {511u, 40u},
        {512u, 60u},
        // Low-9 mask 63 is covered by the union of query masks 0 and 28.
        // This absent hash therefore exercises Bloom-positive exact fallback.
        {8'192u + 63u, 70u},
        {1'048'575u, 80u},
        {1'048'576u, 100u},
        {1'073'741'822u, 120u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(256u, 10u));
    REQUIRE(generation.has_value());

    const std::vector<minimap_sketch_data> minimizers{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{511u, 30u, 1u},
        minimap_sketch_data{512u, 40u, 0u},
        minimap_sketch_data{513u, 50u, 1u},
        minimap_sketch_data{8'192u + 0u, 52u, 0u},
        minimap_sketch_data{8'192u + 28u, 54u, 1u},
        minimap_sketch_data{1'048'575u, 60u, 0u},
        minimap_sketch_data{1'048'576u, 70u, 1u},
        minimap_sketch_data{1'073'741'822u, 80u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 128u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
        fragment_seed_query{
            .minimizers = minimizers,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(
        admission, *generation, sink.rows, 10u);
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    std::vector<bioseq::chaining::anchor> bucket_direct_anchors;
    prepared_fragment_anchor_batch_stats bucket_direct_evidence{};
    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            10,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true);
        const auto anchors = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                minimizers, query_length, 10, kmer_size);
        bucket_direct_anchors.assign(anchors.begin(), anchors.end());
        bucket_direct_evidence = batch.stats();
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_group_offset_size
            == bucket_direct_evidence.unique_query_plans + 1u);
        REQUIRE(snapshot.query_membership_filter_capacity_bytes
            == 16u * 1024u * 1024u);
        REQUIRE(bucket_direct_evidence.
                peak_query_membership_filter_capacity_bytes
            == snapshot.query_membership_filter_capacity_bytes);
        REQUIRE(bucket_direct_evidence.
            query_membership_filter_allocations == 1u);
        REQUIRE(bucket_direct_evidence.
            bucket_direct_matcher_view_receipts == 1u);
        REQUIRE(bucket_direct_evidence.query_plan_allocations == 0u);
        REQUIRE(bucket_direct_evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(bucket_direct_evidence.
                peak_occurrence_count_capacity_bytes
            == bucket_direct_evidence.unique_query_plans
                * sizeof(std::atomic<std::uint32_t>));
        REQUIRE(bucket_direct_evidence.
                peak_query_group_offset_capacity_bytes
            >= snapshot.query_group_offset_size * sizeof(std::uint32_t));
        REQUIRE(bucket_direct_evidence.compositional_sortedness_receipts
            == queries.size());
        REQUIRE(bucket_direct_evidence.final_sortedness_validation_bytes
            == bucket_direct_evidence.exact_final_bytes);
        REQUIRE(batch.admitted_bytes()
            == bucket_direct_evidence.exact_final_bytes
                + queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(admission.reserved_bytes()
            == reader->admitted_bytes() + scratch->admitted_bytes()
                + batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }

    {
        auto oracle_batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            10,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/true,
            /*verify_compositional_order_for_testing=*/true);
        const auto oracle_anchors = oracle_batch.query_view(0u).
            ordered_fragment_seed_anchors(
                minimizers, query_length, 10, kmer_size);
        require_anchor_bytes_equal(oracle_anchors, bucket_direct_anchors);
        const auto& oracle_evidence = oracle_batch.stats();
        REQUIRE(oracle_evidence.
            bucket_direct_matcher_view_receipts == 0u);
        REQUIRE(oracle_evidence.unique_query_plans
            == bucket_direct_evidence.unique_query_plans);
        REQUIRE(oracle_evidence.control_count_bytes
            == bucket_direct_evidence.control_count_bytes);
        REQUIRE(oracle_evidence.pass2_control_bytes
            == bucket_direct_evidence.pass2_control_bytes);
        REQUIRE(oracle_evidence.pass2_accepted_prefix_bytes
            == bucket_direct_evidence.pass2_accepted_prefix_bytes);
        REQUIRE(oracle_evidence.pass2_position_bytes
            == bucket_direct_evidence.pass2_position_bytes);
        REQUIRE(oracle_evidence.pass3_control_bytes
            == bucket_direct_evidence.pass3_control_bytes);
        REQUIRE(oracle_evidence.pass3_accepted_prefix_bytes
            == bucket_direct_evidence.pass3_accepted_prefix_bytes);
        REQUIRE(oracle_evidence.pass3_position_bytes
            == bucket_direct_evidence.pass3_position_bytes);
        REQUIRE(oracle_evidence.anchors_constructed
            == bucket_direct_evidence.anchors_constructed);
        REQUIRE(oracle_evidence.exact_final_bytes
            == bucket_direct_evidence.exact_final_bytes);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.query_plan_size
            == bucket_direct_evidence.unique_query_plans);
        REQUIRE(snapshot.query_plan_reserve_request
            == bucket_direct_evidence.unique_query_plans);
        REQUIRE(oracle_batch.admitted_bytes()
            == oracle_evidence.exact_final_bytes
                + queries.size() * sizeof(prepared_query_anchors));
        REQUIRE(admission.reserved_bytes()
            == reader->admitted_bytes() + scratch->admitted_bytes()
                + oracle_batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
}

TEST_CASE("scalar matcher differential covers counter mapping and dynamic leaves",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr auto packed_target = [](
            std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed_target(10u, 0u)},
        {7u, packed_target(20u, 1u)},
        {7u, packed_target(30u, 0u)},
        {9u, packed_target(40u, 0u)},
        {9u, packed_target(50u, 1u)},
        {11u, packed_target(60u, 1u)},
    };
    const std::array query0{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{9u, 30u, 1u},
        minimap_sketch_data{13u, 40u, 0u},
    };
    const std::array query1{
        minimap_sketch_data{7u, 22u, 1u},
        minimap_sketch_data{9u, 31u, 0u},
        minimap_sketch_data{11u, 42u, 1u},
        minimap_sketch_data{11u, 50u, 0u},
    };
    const std::array heavy_query0{
        minimap_sketch_data{7u, 20u, 0u},
    };
    const std::array heavy_query1{
        minimap_sketch_data{7u, 22u, 1u},
        minimap_sketch_data{7u, 35u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 128u;
    constexpr std::int32_t kmer_size = 15;
    constexpr std::int32_t max_occ = 2;
    const std::array mixed_queries{
        fragment_seed_query{
            .minimizers = query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const std::array all_heavy_queries{
        fragment_seed_query{
            .minimizers = heavy_query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = heavy_query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    struct differential_run {
        std::vector<std::vector<bioseq::chaining::anchor>> anchors{};
        prepared_fragment_anchor_batch_stats evidence{};
        std::uint64_t admitted_bytes = 0u;
    };

    const auto verify_fixture = [&](std::uint64_t target_length,
                                    std::uint64_t segment_span,
                                    minimizer_scalar_width position_width) {
        vector_replay_minimizer_producer producer{pairs};
        collecting_minimizer_row_sink sink;
        auto options = test_grouped_soa_generation_options(
            target_length, 10u);
        options.position_segment_span_bp = segment_span;
        const auto generation = encode_minimizer_generation(
            producer, sink, options);
        REQUIRE(generation.has_value());
        REQUIRE(generation->manifest.hash_width
            == minimizer_scalar_width::u32);
        REQUIRE(generation->manifest.count_width
            == minimizer_scalar_width::u16);
        REQUIRE(generation->manifest.position_width
            == position_width);
        REQUIRE(std::ranges::all_of(
            generation->directory.entries(),
            [](const grouped_soa_bucket_directory_entry& entry) noexcept {
                return entry.shape_offset_width
                    == minimizer_scalar_width::u16;
            }));

        bioseq::parallelism::memory_admission_controller admission;
        auto reader = open_grouped_soa_fragment_query_reader(
            admission, *generation, sink.rows, max_occ);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());

        const auto run = [&](std::span<const fragment_seed_query> queries,
                             bool oracle) {
            differential_run result;
            auto batch = reader->prepare_fragment_anchor_batch(
                admission,
                queries,
                max_occ,
                *scratch,
                /*baseline_query_plan_matcher_for_testing=*/oracle,
                /*verify_compositional_order_for_testing=*/true);
            result.anchors.resize(queries.size());
            for (std::size_t query_ordinal = 0u;
                 query_ordinal < queries.size();
                 ++query_ordinal) {
                const fragment_seed_query& query = queries[query_ordinal];
                const auto anchors = batch.query_view(query_ordinal).
                    ordered_fragment_seed_anchors(
                        query.minimizers,
                        query.query_length,
                        max_occ,
                        query.kmer_size);
                result.anchors[query_ordinal].assign(
                    anchors.begin(), anchors.end());
            }
            result.evidence = batch.stats();
            result.admitted_bytes = batch.admitted_bytes();
            REQUIRE(admission.reserved_bytes()
                == reader->admitted_bytes() + scratch->admitted_bytes()
                    + batch.admitted_bytes());
            scratch->complete_wave_after_join();
            REQUIRE_FALSE(scratch->wave_outstanding());
            return result;
        };

        const auto compare_runs = [&](const differential_run& actual,
                                      const differential_run& oracle) {
            REQUIRE(actual.anchors.size() == oracle.anchors.size());
            for (std::size_t query_ordinal = 0u;
                 query_ordinal < actual.anchors.size();
                 ++query_ordinal) {
                require_anchor_bytes_equal(
                    actual.anchors[query_ordinal],
                    oracle.anchors[query_ordinal]);
            }
            require_hot_loop_differential_evidence_equal(
                actual.evidence, oracle.evidence);
            REQUIRE(actual.admitted_bytes == oracle.admitted_bytes);
        };

        const differential_run mixed_actual = run(mixed_queries, false);
        const differential_run mixed_oracle = run(mixed_queries, true);
        compare_runs(mixed_actual, mixed_oracle);
        REQUIRE(mixed_actual.anchors[0u].size() == 2u);
        REQUIRE(mixed_actual.anchors[1u].size() == 4u);
        for (std::size_t query_ordinal = 0u;
             query_ordinal < mixed_queries.size();
             ++query_ordinal) {
            const fragment_seed_query& query = mixed_queries[query_ordinal];
            const auto ordinary = ordinary_seed_anchors(
                pairs,
                target_length,
                query.minimizers,
                query.query_length,
                max_occ,
                query.kmer_size);
            require_anchor_bytes_equal(
                mixed_actual.anchors[query_ordinal], ordinary);
        }
        constexpr std::uint64_t control_count_bytes =
            3u * (sizeof(std::uint32_t) + sizeof(std::uint16_t));
        constexpr std::uint64_t hash_scan_bytes =
            3u * sizeof(std::uint32_t);
        constexpr std::uint64_t accepted_prefix_bytes =
            2u * 2u * sizeof(std::uint16_t);
        constexpr std::uint64_t rejected_prefix_bytes =
            2u * sizeof(std::uint16_t);
        const bool dynamic = position_width != minimizer_scalar_width::u16;
        REQUIRE(mixed_actual.evidence.query_occurrences == 7u);
        REQUIRE(mixed_actual.evidence.unique_query_plans == 4u);
        REQUIRE(mixed_actual.evidence.control_count_bytes
            == control_count_bytes);
        REQUIRE(mixed_actual.evidence.rejected_control_validation_bytes
            == rejected_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass2_control_bytes
            == control_count_bytes);
        REQUIRE(mixed_actual.evidence.pass2_accepted_prefix_bytes
            == accepted_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass2_position_bytes
            == 3u * static_cast<std::uint8_t>(position_width));
        REQUIRE(mixed_actual.evidence.pass3_control_bytes
            == (dynamic
                ? control_count_bytes
                : hash_scan_bytes + 2u * sizeof(std::uint16_t)));
        REQUIRE(mixed_actual.evidence.pass3_accepted_prefix_bytes
            == accepted_prefix_bytes);
        REQUIRE(mixed_actual.evidence.pass3_position_bytes
            == mixed_actual.evidence.pass2_position_bytes);
        REQUIRE(mixed_actual.evidence.anchors_constructed == 6u);
        REQUIRE(mixed_actual.evidence.exact_final_bytes
            == 6u * sizeof(bioseq::chaining::anchor));
        REQUIRE(mixed_actual.admitted_bytes
            == mixed_actual.evidence.exact_final_bytes
                + mixed_queries.size() * sizeof(prepared_query_anchors));

        const differential_run rejected_actual = run(
            all_heavy_queries, false);
        const differential_run rejected_oracle = run(
            all_heavy_queries, true);
        compare_runs(rejected_actual, rejected_oracle);
        REQUIRE(rejected_actual.anchors[0u].empty());
        REQUIRE(rejected_actual.anchors[1u].empty());
        REQUIRE(rejected_actual.evidence.control_count_bytes
            == control_count_bytes);
        REQUIRE(rejected_actual.evidence.
            rejected_control_validation_bytes
            == (dynamic
                ? control_count_bytes + rejected_prefix_bytes
                : hash_scan_bytes + sizeof(std::uint16_t)
                    + rejected_prefix_bytes));
        REQUIRE(rejected_actual.evidence.pass2_control_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass2_position_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_control_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(rejected_actual.evidence.pass3_position_bytes == 0u);
        REQUIRE(rejected_actual.evidence.anchors_constructed == 0u);
        REQUIRE(rejected_actual.evidence.exact_final_bytes == 0u);
        REQUIRE(rejected_actual.admitted_bytes
            == all_heavy_queries.size() * sizeof(prepared_query_anchors));
    };

    SECTION("fixed H32 C16 P16 leaves") {
        verify_fixture(
            256u,
            std::uint64_t{1u} << 15u,
            minimizer_scalar_width::u16);
    }
    SECTION("checked dynamic H32 C16 P32 leaves") {
        verify_fixture(
            (std::uint64_t{1u} << 15u) + 1u,
            (std::uint64_t{1u} << 15u) + 1u,
            minimizer_scalar_width::u32);
    }
}

TEST_CASE("packed and wide query occurrences recycle exact capacities",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {8u, 41u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head,
        generation->manifest,
        pairs,
        10u);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = open_grouped_soa_fragment_query_reader(
        admission,
        *generation,
        sink.rows,
        2u,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    constexpr std::size_t query_count = 65'536u;
    constexpr std::size_t present_query_ordinal = query_count - 1u;
    constexpr std::size_t singleton_query_ordinal = query_count - 2u;
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    std::vector<minimap_sketch_data> skewed_query(
        65'536u, minimap_sketch_data{6u, 14u, 0u});
    skewed_query[0] = minimap_sketch_data{7u, 14u, 0u};
    skewed_query[1] = minimap_sketch_data{7u, 20u, 1u};
    const std::vector<minimap_sketch_data> present_hash{
        minimap_sketch_data{7u, 14u, 0u},
    };
    const std::vector<minimap_sketch_data> singleton_hash{
        minimap_sketch_data{8u, 30u, 1u},
    };
    std::vector<fragment_seed_query> queries(
        query_count,
        fragment_seed_query{
            .minimizers = {},
            .query_length = query_length,
            .kmer_size = kmer_size,
    });
    queries[0].minimizers = skewed_query;
    queries[singleton_query_ordinal].minimizers = singleton_hash;
    queries[present_query_ordinal].minimizers = present_hash;

    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, queries, 2, *scratch);
        const auto actual = batch.query_view(present_query_ordinal).
            ordered_fragment_seed_anchors(
                present_hash, query_length, 2, kmer_size);
        const auto expected = ordinary_seed_anchors(
            pairs, 100u, present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto skewed_actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                skewed_query, query_length, 2, kmer_size);
        const auto skewed_expected = ordinary_seed_anchors(
            pairs, 100u, skewed_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(skewed_actual, skewed_expected);
        REQUIRE(skewed_actual.size() == 2u);
        const auto& evidence = batch.stats();
        REQUIRE(evidence.query_occurrences == 65'538u);
        REQUIRE(evidence.unique_query_plans == 3u);
        REQUIRE(evidence.packed_occurrence_waves == 1u);
        REQUIRE(evidence.wide_occurrence_waves == 0u);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 65'538u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes
            == evidence.peak_occurrence_capacity_bytes);
        scratch->complete_wave_after_join();
    }
    const auto retained_packed = scratch->occurrence_capacity_snapshot();
    REQUIRE(retained_packed.packed_capacity >= 65'538u);

    STATIC_REQUIRE(
        k_compact_direct_parallel_packed_occurrence_radix_min_count == 65'536u);
    std::vector<minimap_sketch_data> split_boundary_query = skewed_query;
    split_boundary_query[2u] = minimap_sketch_data{
        (std::uint32_t{1u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[3u] = minimap_sketch_data{
        std::uint32_t{1u} << 24u, 14u, 0u};
    split_boundary_query[4u] = minimap_sketch_data{
        (std::uint32_t{2u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[5u] = minimap_sketch_data{
        std::uint32_t{2u} << 24u, 14u, 0u};
    split_boundary_query[6u] = minimap_sketch_data{
        (std::uint32_t{3u} << 24u) - 1u, 14u, 0u};
    split_boundary_query[7u] = minimap_sketch_data{
        std::uint32_t{3u} << 24u, 14u, 0u};
    const auto run_compact_packed_boundary = [&, reader = &*reader](
            std::span<const minimap_sketch_data> minimizers,
            bool expected_split_selection) {
        const std::array boundary_queries{
            fragment_seed_query{
                .minimizers = minimizers,
                .query_length = query_length,
                .kmer_size = kmer_size,
            },
        };
        const auto expected = ordinary_seed_anchors(
            pairs, 100u, minimizers, query_length, 2, kmer_size);
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            boundary_queries,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                minimizers, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);

        const std::array<std::uint32_t, 8u> exact_unique_hashes{
            6u,
            7u,
            (std::uint32_t{1u} << 24u) - 1u,
            std::uint32_t{1u} << 24u,
            (std::uint32_t{2u} << 24u) - 1u,
            std::uint32_t{2u} << 24u,
            (std::uint32_t{3u} << 24u) - 1u,
            std::uint32_t{3u} << 24u,
        };
        const compact_direct_query_set_stats expected_query_set_stats{
            .query_count = boundary_queries.size(),
            .query_occurrence_count = minimizers.size(),
            .unique_hash_count = exact_unique_hashes.size(),
            .group_key_set_checksum = compact_direct_query_set_checksum(
                exact_unique_hashes,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(
                    generation->manifest.window_size),
                2u),
            .present = true,
        };
        REQUIRE(batch.compact_direct_query_stats()
            == expected_query_set_stats);
        REQUIRE(batch.stats().unique_query_plans
            == exact_unique_hashes.size());
        REQUIRE(batch.stats().packed_occurrence_waves == 1u);
        REQUIRE(batch.stats().wide_occurrence_waves == 0u);

        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == minimizers.size());
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == exact_unique_hashes.size());
        REQUIRE(snapshot.occurrence_radix_invoked);
        // This receipt records the exact split-route threshold and output
        // parity; it deliberately makes no claim about observed pool workers.
        REQUIRE(snapshot.parallel_occurrence_radix_split_selected
            == expected_split_selection);
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes
            == batch.stats().peak_occurrence_capacity_bytes);
        scratch->complete_wave_after_join();
    };
    const std::span<const minimap_sketch_data> skewed_span{
        split_boundary_query};
    run_compact_packed_boundary(
        skewed_span.first(
            k_compact_direct_parallel_packed_occurrence_radix_min_count - 1u),
        false);
    run_compact_packed_boundary(
        skewed_span.first(
            k_compact_direct_parallel_packed_occurrence_radix_min_count),
        true);

    skewed_query.push_back(
        minimap_sketch_data{6u, 14u, 0u});
    queries[0].minimizers = skewed_query;
    std::vector<bioseq::chaining::anchor> wide_production_query0;
    std::vector<bioseq::chaining::anchor> wide_production_present_query;
    prepared_fragment_anchor_batch_stats wide_production_evidence{};
    std::uint64_t wide_production_admitted_bytes = 0u;
    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true);
        const auto actual = batch.query_view(present_query_ordinal).
            ordered_fragment_seed_anchors(
                present_hash, query_length, 2, kmer_size);
        const auto expected = ordinary_seed_anchors(
            pairs, 100u, present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const auto skewed_actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                skewed_query, query_length, 2, kmer_size);
        const auto skewed_expected = ordinary_seed_anchors(
            pairs, 100u, skewed_query, query_length, 2, kmer_size);
        require_anchor_bytes_equal(skewed_actual, skewed_expected);
        REQUIRE(skewed_actual.size() == 2u);
        const auto& evidence = batch.stats();
        wide_production_query0.assign(
            skewed_actual.begin(), skewed_actual.end());
        wide_production_present_query.assign(
            actual.begin(), actual.end());
        wide_production_evidence = evidence;
        wide_production_admitted_bytes = batch.admitted_bytes();
        REQUIRE(evidence.query_occurrences == 65'539u);
        REQUIRE(evidence.unique_query_plans == 3u);
        REQUIRE(evidence.packed_occurrence_waves == 0u);
        REQUIRE(evidence.wide_occurrence_waves == 1u);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.packed_size == 0u);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.packed_capacity
            == retained_packed.packed_capacity);
        REQUIRE(snapshot.wide_capacity >= 65'539u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes
            == evidence.peak_occurrence_capacity_bytes);
        REQUIRE(admission.reserved_bytes()
            == reader->admitted_bytes() + scratch->admitted_bytes()
                + batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
    {
        auto oracle_batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/true,
            /*verify_compositional_order_for_testing=*/true);
        const auto oracle_query0 = oracle_batch.query_view(0u).
            ordered_fragment_seed_anchors(
                skewed_query, query_length, 2, kmer_size);
        const auto oracle_present_query = oracle_batch.query_view(
            present_query_ordinal).
            ordered_fragment_seed_anchors(
                present_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(
            oracle_query0, wide_production_query0);
        require_anchor_bytes_equal(
            oracle_present_query, wide_production_present_query);
        require_hot_loop_differential_evidence_equal(
            wide_production_evidence, oracle_batch.stats());
        REQUIRE(oracle_batch.admitted_bytes()
            == wide_production_admitted_bytes);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.query_plan_size == 3u);
        REQUIRE(snapshot.query_plan_reserve_request == 3u);
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        REQUIRE(admission.reserved_bytes()
            == reader->admitted_bytes() + scratch->admitted_bytes()
                + oracle_batch.admitted_bytes());
        scratch->complete_wave_after_join();
    }
    {
        auto direct_batch = reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto direct_query0 = direct_batch.query_view(0u).
            ordered_fragment_seed_anchors(
                skewed_query, query_length, 2, kmer_size);
        const auto direct_present_query = direct_batch.query_view(
            present_query_ordinal).ordered_fragment_seed_anchors(
                present_hash, query_length, 2, kmer_size);
        const auto direct_singleton_query = direct_batch.query_view(
            singleton_query_ordinal).ordered_fragment_seed_anchors(
                singleton_hash, query_length, 2, kmer_size);
        const auto expected_singleton_query = ordinary_seed_anchors(
            pairs, 100u, singleton_hash, query_length, 2, kmer_size);
        require_anchor_bytes_equal(
            direct_query0, wide_production_query0);
        require_anchor_bytes_equal(
            direct_present_query, wide_production_present_query);
        require_anchor_bytes_equal(
            direct_singleton_query, expected_singleton_query);
        REQUIRE(direct_singleton_query.size() == 1u);
        REQUIRE(direct_batch.stats().wide_occurrence_waves == 1u);
        REQUIRE(direct_batch.stats().packed_occurrence_waves == 0u);
        REQUIRE(direct_batch.stats().compact_direct_unique_lookups == 3u);
        REQUIRE(direct_batch.stats().compact_direct_selected_hashes == 2u);
        REQUIRE(direct_batch.stats().compact_direct_selected_postings == 2u);
        REQUIRE(direct_batch.stats().compact_direct_posting_visit_passes
            == 1u);
        REQUIRE(direct_batch.stats().
            compact_direct_emission_postings_visited == 2u);
        REQUIRE(direct_batch.stats().compact_direct_rejected_posting_reads
            == 0u);
        REQUIRE(direct_batch.stats().pass2_position_bytes == 0u);
        REQUIRE(direct_batch.stats().pass3_position_bytes
            == 2u * compact.view.header.posting_width_bytes);
        REQUIRE(direct_batch.stats().anchors_constructed == 4u);
        REQUIRE(direct_batch.stats().forbidden_buffers_are_zero());
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::wide);
        REQUIRE(snapshot.wide_size == 65'539u);
        REQUIRE(snapshot.query_plan_size == 3u);
        REQUIRE(snapshot.query_membership_filter_capacity_bytes == 0u);
        REQUIRE(snapshot.query_group_offset_capacity == 0u);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
        scratch->complete_wave_after_join();
    }
    const auto retained_both = scratch->occurrence_capacity_snapshot();
    REQUIRE(retained_both.packed_capacity > 0u);
    REQUIRE(retained_both.wide_capacity > 0u);

    const std::array smaller_wave{
        fragment_seed_query{
            .minimizers = present_hash,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, smaller_wave, 2, *scratch);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 1u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.packed_capacity
            == retained_both.packed_capacity);
        REQUIRE(snapshot.wide_capacity == retained_both.wide_capacity);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE_FALSE(snapshot.occurrence_radix_invoked);
        REQUIRE(snapshot.aggregate_occurrence_capacity_bytes
            == 8u * snapshot.packed_capacity
                + 12u * snapshot.wide_capacity);
        REQUIRE(batch.stats().query_occurrences == 1u);
        REQUIRE(batch.stats().unique_query_plans == 1u);
        REQUIRE(batch.stats().packed_occurrence_waves == 1u);
        REQUIRE(batch.stats().wide_occurrence_waves == 0u);
        scratch->complete_wave_after_join();
    }

    const std::array small_compact_minimizers{
        minimap_sketch_data{8u, 30u, 1u},
        minimap_sketch_data{7u, 14u, 0u},
        minimap_sketch_data{7u, 20u, 1u},
    };
    const std::array small_compact_wave{
        fragment_seed_query{
            .minimizers = small_compact_minimizers,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission,
            small_compact_wave,
            2,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
        const auto actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                small_compact_minimizers, query_length, 2, kmer_size);
        const auto expected = ordinary_seed_anchors(
            pairs,
            100u,
            small_compact_minimizers,
            query_length,
            2,
            kmer_size);
        require_anchor_bytes_equal(actual, expected);
        const std::array<std::uint32_t, 2u> exact_unique_hashes{7u, 8u};
        const compact_direct_query_set_stats expected_query_set_stats{
            .query_count = small_compact_wave.size(),
            .query_occurrence_count = small_compact_minimizers.size(),
            .unique_hash_count = exact_unique_hashes.size(),
            .group_key_set_checksum = compact_direct_query_set_checksum(
                exact_unique_hashes,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(
                    generation->manifest.window_size),
                2u),
            .present = true,
        };
        REQUIRE(batch.compact_direct_query_stats()
            == expected_query_set_stats);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        scratch->complete_wave_after_join();
    }

    constexpr std::size_t unique_count = 1'024u;
    constexpr bioseq::storage::sequence_size_t unique_query_length = 2'048u;
    std::vector<minimap_sketch_data> all_unique;
    all_unique.reserve(unique_count);
    for (std::size_t ordinal = 0u; ordinal < unique_count; ++ordinal) {
        all_unique.push_back(minimap_sketch_data{
            100u + ordinal,
            static_cast<std::uint32_t>(14u + ordinal),
            static_cast<std::uint32_t>(ordinal & 1u),
        });
    }
    const std::array all_unique_wave{
        fragment_seed_query{
            .minimizers = all_unique,
            .query_length = unique_query_length,
            .kmer_size = kmer_size,
        },
    };
    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, all_unique_wave, 2, *scratch);
        const auto actual = batch.query_view(0u).
            ordered_fragment_seed_anchors(
                all_unique, unique_query_length, 2, kmer_size);
        REQUIRE(actual.empty());
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == unique_count);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_capacity == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE(snapshot.bucket_direct_matcher_active);
        REQUIRE(snapshot.occurrence_radix_invoked);
        REQUIRE(batch.stats().query_occurrences == unique_count);
        REQUIRE(batch.stats().unique_query_plans == unique_count);
        scratch->complete_wave_after_join();
    }

    const std::array<fragment_seed_query, 0u> empty_wave{};
    {
        auto batch = reader->prepare_fragment_anchor_batch(
            admission, empty_wave, 2, *scratch);
        const auto snapshot = scratch->occurrence_capacity_snapshot();
        REQUIRE(snapshot.route == query_occurrence_route::packed);
        REQUIRE(snapshot.packed_size == 0u);
        REQUIRE(snapshot.wide_size == 0u);
        REQUIRE(snapshot.query_plan_size == 0u);
        REQUIRE(snapshot.query_plan_reserve_request == 0u);
        REQUIRE_FALSE(snapshot.occurrence_radix_invoked);
        REQUIRE_FALSE(snapshot.parallel_occurrence_radix_split_selected);
        REQUIRE(batch.stats().query_occurrences == 0u);
        REQUIRE(batch.stats().unique_query_plans == 0u);
        REQUIRE(batch.stats().packed_occurrence_waves == 1u);
        REQUIRE(batch.stats().wide_occurrence_waves == 0u);
        scratch->complete_wave_after_join();
    }

	scratch->release_retained_capacity();
	REQUIRE(scratch->admitted_bytes() == 0u);
}

TEST_CASE("compact-direct selected-page advice is exact coalesced and fail open",
        "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;

    SECTION("overlap and touch merge while real gaps flush") {
        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        compact_direct_selected_page_advice_accumulator accumulator;
        REQUIRE(accumulator.accumulate(
            adviser, {.begin = 100u, .end = 200u}));
        REQUIRE(accumulator.accumulate(
            adviser, {.begin = 150u, .end = 240u}));
        REQUIRE(accumulator.accumulate(
            adviser, {.begin = 240u, .end = 300u}));
        REQUIRE(recorder.record_count.load() == 0u);

        REQUIRE(accumulator.accumulate(
            adviser, {.begin = 400u, .end = 450u}));
        REQUIRE(recorder.record_count.load() == 1u);
        REQUIRE(recorder.records[0u]
            == compact_direct_selected_page_range{
                .begin = 100u,
                .end = 300u,
            });
        accumulator.flush(adviser);
        REQUIRE(recorder.record_count.load() == 2u);
        REQUIRE(recorder.records[1u]
            == compact_direct_selected_page_range{
                .begin = 400u,
                .end = 450u,
            });
        REQUIRE_FALSE(accumulator.pending);
        REQUIRE_FALSE(accumulator.accumulate(
            adviser, {.begin = 500u, .end = 500u}));
    }

    SECTION("a callback which drops advice cannot affect control flow") {
        selected_page_advice_recorder recorder;
        recorder.drop_advice.store(true);
        const auto adviser = recorder.token();
        compact_direct_selected_page_advice_accumulator accumulator;
        REQUIRE(accumulator.accumulate(
            adviser, {.begin = 100u, .end = 200u}));
        accumulator.flush(adviser);
        REQUIRE_FALSE(accumulator.pending);
        REQUIRE(recorder.record_count.load() == 0u);
    }

    SECTION("alignment and arithmetic overflow remain release active") {
        const auto aligned = compact_direct_align_selected_page_range(
            0x1003u, 2u, 4096u);
        REQUIRE(aligned.has_value());
        REQUIRE(*aligned == compact_direct_selected_page_range{
            .begin = 0x1000u,
            .end = 0x2000u,
        });
        const auto overflow = compact_direct_align_selected_page_range(
            (std::numeric_limits<std::uintptr_t>::max)() - 1u,
            2u,
            4096u);
        REQUIRE_FALSE(overflow.has_value());
        REQUIRE(overflow.error()
            == direct_inverted_index_error::arithmetic_overflow);
        const auto empty = compact_direct_align_selected_page_range(
            0x1000u, 0u, 4096u);
        REQUIRE_FALSE(empty.has_value());
        REQUIRE(empty.error() == direct_inverted_index_error::query_plan);
    }

    SECTION("P32 inline descriptors are excluded and posting bounds reject") {
        fragment_compact_direct_source source{
            .runs = {
                fragment_compact_direct_run{
                    .header = compact_hash32_sorted_run_header{
                        .hash = 7u,
                        .global_count = 2u,
                        .target_strand_zero_count = 1u,
                    },
                    .encoded_positions = {20u, 23u},
                },
                fragment_compact_direct_run{
                    .header = compact_hash32_sorted_run_header{
                        .hash = 8u,
                        .global_count = 1u,
                        .target_strand_zero_count = 1u,
                    },
                    .encoded_positions = {40u},
                },
            },
        };
        const direct_index_identity identity{
            .generation_id = 1u,
            .source_identity = 2u,
            .storage_contract_checksum = 3u,
            .target_length = 400u,
        };
        const compact_hash32_geometry geometry{
            .hash_domain_bits = 9u,
            .max_hash = 511u,
        };
        const compact_hash32_build_options options{
            .identity = identity,
            .max_occ_cap = 4u,
            .geometry = geometry,
        };
        const compact_hash32_sorted_run_source source_ref{
            .context = &source,
            .scan_sorted_runs_fn = &scan_fragment_compact_direct_runs,
        };
        auto image = build_compact_hash32_direct_index(source_ref, options);
        REQUIRE(image.has_value());
        const auto direct = validate_compact_hash32_direct_index(
            *image, identity, options.max_occ_cap, geometry);
        REQUIRE(direct.has_value());
        REQUIRE(direct->header.posting_width_bytes == 4u);
        REQUIRE(direct->header.postings_bytes == 8u);

        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        const auto posting_span = direct->image.subspan(
            static_cast<std::size_t>(direct->header.postings_offset), 8u);
        const auto selected = compact_direct_select_page_range(
            adviser, *direct, posting_span);
        REQUIRE(selected.has_value());
        REQUIRE(selected->has_value());
        REQUIRE(**selected == compact_direct_selected_page_range{
            .begin = reinterpret_cast<std::uintptr_t>(posting_span.data()),
            .end = reinterpret_cast<std::uintptr_t>(posting_span.data())
                + posting_span.size(),
        });

        const auto inline_descriptor = direct->image.subspan(
            static_cast<std::size_t>(
                direct->header.descriptors_offset
                + direct->header.descriptor_width_bytes),
            4u);
        const auto excluded = compact_direct_select_page_range(
            adviser, *direct, inline_descriptor);
        REQUIRE(excluded.has_value());
        REQUIRE_FALSE(excluded->has_value());

        const auto outside_sections = compact_direct_select_page_range(
            adviser, *direct, direct->image.first(1u));
        REQUIRE_FALSE(outside_sections.has_value());
        REQUIRE(outside_sections.error()
            == direct_inverted_index_error::query_plan);
        auto malformed = *direct;
        malformed.header.postings_bytes = malformed.image.size();
        const auto rejected_bounds = compact_direct_select_page_range(
            adviser, malformed, posting_span);
        REQUIRE_FALSE(rejected_bounds.has_value());
        REQUIRE(rejected_bounds.error()
            == direct_inverted_index_error::section_layout);
    }

    SECTION("P64 singleton postings remain selected") {
        fragment_compact_direct_source source{
            .runs = {
                fragment_compact_direct_run{
                    .header = compact_hash32_sorted_run_header{
                        .hash = 7u,
                        .global_count = 1u,
                        .target_strand_zero_count = 1u,
                    },
                    .encoded_positions = {20u},
                },
            },
        };
        const direct_index_identity identity{
            .generation_id = 4u,
            .source_identity = 5u,
            .storage_contract_checksum = 6u,
            .target_length = (std::uint64_t{1u} << 31u) + 1u,
        };
        const compact_hash32_geometry geometry{
            .hash_domain_bits = 9u,
            .max_hash = 511u,
        };
        const compact_hash32_build_options options{
            .identity = identity,
            .max_occ_cap = 4u,
            .geometry = geometry,
        };
        const compact_hash32_sorted_run_source source_ref{
            .context = &source,
            .scan_sorted_runs_fn = &scan_fragment_compact_direct_runs,
        };
        auto image = build_compact_hash32_direct_index(source_ref, options);
        REQUIRE(image.has_value());
        const auto direct = validate_compact_hash32_direct_index(
            *image, identity, options.max_occ_cap, geometry);
        REQUIRE(direct.has_value());
        REQUIRE(direct->header.posting_width_bytes == 8u);
        REQUIRE(direct->header.postings_bytes == 8u);

        selected_page_advice_recorder recorder;
        const auto adviser = recorder.token();
        const auto posting_span = direct->image.subspan(
            static_cast<std::size_t>(direct->header.postings_offset), 8u);
        const auto selected = compact_direct_select_page_range(
            adviser, *direct, posting_span);
        REQUIRE(selected.has_value());
        REQUIRE(selected->has_value());
        REQUIRE((**selected).end - (**selected).begin == 8u);
    }
}

TEST_CASE("compact-direct query-set checksum preserves canonical-u32 identity",
		"[component:bioseq][layer:unit][feature:index]") {
	using namespace bioseq::indexing;
	constexpr std::uint32_t kmer_size = 15u;
	constexpr std::uint32_t window_size = 10u;
	constexpr std::uint32_t max_occ = 500u;
	std::vector<std::uint32_t> duplicate_heavy{
		(std::numeric_limits<std::uint32_t>::max)(),
		0u, 1u, 0u, 1u, 1u,
		(std::numeric_limits<std::uint32_t>::max)(),
		0x7fffffffu, 0x7fffffffu, 0u};
	std::sort(duplicate_heavy.begin(), duplicate_heavy.end());
	duplicate_heavy.erase(
		std::unique(duplicate_heavy.begin(), duplicate_heavy.end()),
		duplicate_heavy.end());
	const std::vector<std::uint32_t> expected_unique{
		0u, 1u, 0x7fffffffu,
		(std::numeric_limits<std::uint32_t>::max)()};
	REQUIRE(duplicate_heavy == expected_unique);

	const auto reference_mix = [](std::uint64_t hash,
			std::uint64_t value) noexcept {
		for (int byte_index = 0; byte_index < 8; ++byte_index) {
			hash ^= value & UINT64_C(0xff);
			hash *= UINT64_C(1099511628211);
			value >>= 8u;
		}
		return hash;
	};
	std::uint64_t reference_checksum = UINT64_C(1469598103934665603);
	reference_checksum = reference_mix(reference_checksum, kmer_size);
	reference_checksum = reference_mix(reference_checksum, window_size);
	reference_checksum = reference_mix(reference_checksum, max_occ);
	for (const std::uint32_t hash : duplicate_heavy) {
		reference_checksum = reference_mix(reference_checksum, hash);
	}
	REQUIRE(compact_direct_query_set_checksum(
		duplicate_heavy, kmer_size, window_size, max_occ) == reference_checksum);
	REQUIRE(compact_direct_query_set_checksum(
		std::span<const std::uint32_t>{},
		kmer_size,
		window_size,
		max_occ)
		== compact_direct_query_set_checksum_prefix(
			kmer_size, window_size, max_occ));
}

TEST_CASE("compact direct fragment route is exact and scan free",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](
            std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t build_cap = 4u;
    constexpr std::int32_t runtime_max_occ = 2;
    constexpr std::uint64_t target_length = 400u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, packed(20u, 0u)},
        {7u, packed(150u, 1u)},
        {8u, packed(30u, 0u)},
        {8u, packed(31u, 0u)},
        {8u, packed(32u, 1u)},
        {9u, packed(40u, 0u)},
        {9u, packed(41u, 0u)},
        {9u, packed(42u, 0u)},
        {9u, packed(43u, 1u)},
        {9u, packed(44u, 1u)},
        {10u, packed(250u, 0u)},
        {10u, packed(251u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(target_length, build_cap));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head,
        generation->manifest,
        pairs,
        build_cap);

    const std::vector<minimap_sketch_data> query0{
        minimap_sketch_data{7u, 14u, 0u},
        minimap_sketch_data{7u, 20u, 1u},
        minimap_sketch_data{8u, 30u, 0u},
        minimap_sketch_data{9u, 40u, 1u},
        minimap_sketch_data{11u, 50u, 0u},
    };
    const std::vector<minimap_sketch_data> query1{
        minimap_sketch_data{10u, 14u, 0u},
        minimap_sketch_data{7u, 25u, 1u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
        fragment_seed_query{
            .minimizers = query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    {
        bioseq::parallelism::memory_admission_controller no_direct_admission;
        auto no_direct_reader = open_grouped_soa_fragment_query_reader(
            no_direct_admission,
            *generation,
            sink.rows,
            runtime_max_occ);
        REQUIRE(no_direct_reader.has_value());
        auto no_direct_scratch = query_wave_scratch::create(
            no_direct_admission);
        REQUIRE(no_direct_scratch.has_value());
        REQUIRE_THROWS_AS(
            no_direct_reader->prepare_fragment_anchor_batch(
                no_direct_admission,
                queries,
                runtime_max_occ,
                *no_direct_scratch,
                /*baseline_query_plan_matcher_for_testing=*/false,
                /*verify_compositional_order_for_testing=*/false,
                minimizer_fragment_lookup_route::direct_compact_hash_lookup),
            persisted_minimizer_lookup_error);
        REQUIRE_FALSE(no_direct_scratch->wave_outstanding());
    }

    {
        auto subset_view = compact.view;
        REQUIRE(subset_view.header.geometry.hash_domain_bits > 9u);
        --subset_view.header.geometry.hash_domain_bits;
        subset_view.header.geometry.max_hash = static_cast<std::uint32_t>(
            (std::uint64_t{1u}
                << subset_view.header.geometry.hash_domain_bits) - 1u);
        bioseq::parallelism::memory_admission_controller subset_admission;
        const auto subset_reader = open_grouped_soa_fragment_query_reader(
            subset_admission,
            *generation,
            sink.rows,
            runtime_max_occ,
            own_fragment_compact_direct_test_index(
                compact, subset_view));
        REQUIRE_FALSE(subset_reader.has_value());
        REQUIRE(subset_reader.error()
            == minimizer_generation_encoding_error::storage_contract_mismatch);

        auto superset_view = compact.view;
        REQUIRE(superset_view.header.geometry.hash_domain_bits < 32u);
        ++superset_view.header.geometry.hash_domain_bits;
        superset_view.header.geometry.max_hash =
            superset_view.header.geometry.hash_domain_bits == 32u
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>(
                (std::uint64_t{1u}
                    << superset_view.header.geometry.hash_domain_bits) - 1u);
        bioseq::parallelism::memory_admission_controller superset_admission;
        const auto superset_reader = open_grouped_soa_fragment_query_reader(
            superset_admission,
            *generation,
            sink.rows,
            runtime_max_occ,
            own_fragment_compact_direct_test_index(
                compact, superset_view));
        REQUIRE_FALSE(superset_reader.has_value());
        REQUIRE(superset_reader.error()
            == minimizer_generation_encoding_error::storage_contract_mismatch);

        auto unrelated = std::make_shared<std::vector<std::byte>>(1u);
        std::shared_ptr<const std::byte> unrelated_alias{
            unrelated, unrelated->data()};
        REQUIRE_FALSE(owned_compact_hash32_direct_index_view::bind(
            std::move(unrelated_alias), compact.view));
    }

    {
        auto direct_only_compact = make_fragment_compact_direct_test_index(
            generation->head,
            generation->manifest,
            pairs,
            build_cap);
        selected_page_advice_recorder selected_page_recorder;
        auto wrong_segment_manifest = generation->manifest;
        ++wrong_segment_manifest.position_segment_count;
        bioseq::parallelism::memory_admission_controller
            wrong_segment_admission;
        const auto wrong_segment_reader =
            prepared_minimizer_fragment_reader::open_compact_direct(
                wrong_segment_admission,
                generation->head,
                wrong_segment_manifest,
                test_grouped_soa_expected_storage_contract(build_cap),
                runtime_max_occ,
                own_fragment_compact_direct_test_index(
                    direct_only_compact));
        REQUIRE_FALSE(wrong_segment_reader.has_value());
        REQUIRE(wrong_segment_reader.error()
            == minimizer_generation_encoding_error::segment_count);
        REQUIRE(wrong_segment_admission.reserved_bytes() == 0u);

        auto wrong_identity_view = direct_only_compact.view;
        ++wrong_identity_view.header.identity.generation_id;
        bioseq::parallelism::memory_admission_controller
            wrong_identity_admission;
        const auto wrong_identity_reader =
            prepared_minimizer_fragment_reader::open_compact_direct(
                wrong_identity_admission,
                generation->head,
                generation->manifest,
                test_grouped_soa_expected_storage_contract(build_cap),
                runtime_max_occ,
                own_fragment_compact_direct_test_index(
                    direct_only_compact, wrong_identity_view));
        REQUIRE_FALSE(wrong_identity_reader.has_value());
        REQUIRE(wrong_identity_reader.error()
            == minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(wrong_identity_admission.reserved_bytes() == 0u);

        // A protected decoy represents the complete grouped-SoA payload that a
        // minimap_manifest reader would own.  It is protected before open and is never
        // passed to the direct-only API, so any accidental payload dependency
        // would fault instead of being hidden by a warm test allocation.
        auto forbidden_grouped_soa_payload = make_sparse_mapped_test_payload(
            generation->manifest.payload_bytes);
        REQUIRE(forbidden_grouped_soa_payload.has_value());
        REQUIRE(mprotect(
            forbidden_grouped_soa_payload->owner.get(),
            forbidden_grouped_soa_payload->size,
            PROT_NONE) == 0);

        const std::uint8_t direct_only_posting_width =
            direct_only_compact.view.header.posting_width_bytes;
        bioseq::parallelism::memory_admission_controller direct_admission;
        // One byte cannot admit even one fragment_block_plan. Direct open must
        // therefore succeed only by retaining no scan-route address plan.
        direct_admission.set_index_memory_cap_bytes(1u);
        auto direct_reader =
            prepared_minimizer_fragment_reader::open_compact_direct(
                direct_admission,
                generation->head,
                generation->manifest,
                test_grouped_soa_expected_storage_contract(build_cap),
                runtime_max_occ,
                own_fragment_compact_direct_test_index(
                    direct_only_compact),
                selected_page_recorder.token());
        REQUIRE(direct_reader.has_value());
        REQUIRE(selected_page_recorder.record_count.load() == 0u);
        REQUIRE(direct_reader->ready());
        REQUIRE(direct_reader->compact_direct_ready());
        REQUIRE(direct_reader->compact_direct_only());
        REQUIRE(direct_reader->mapped_grouped_soa_payload_bytes() == 0u);
        REQUIRE(direct_reader->grouped_soa_directory_entry_count() == 0u);
        REQUIRE(direct_reader->fragment_block_count()
            == generation->manifest.position_segment_count);
        REQUIRE(direct_reader->largest_fragment_block_entry_count() == 0u);
        REQUIRE(direct_reader->oversized_segment_block_count() == 0u);
        const auto direct_memory = direct_reader->memory_stats();
        REQUIRE(direct_memory.directory_capacity_bytes == 0u);
        REQUIRE(direct_memory.global_position_capacity_bytes == 0u);
        REQUIRE(direct_memory.control_payload_capacity_bytes == 0u);
        REQUIRE(direct_memory.ordinal_route_capacity_bytes == 0u);
        REQUIRE(direct_memory.owned_capacity_bytes == 0u);
        REQUIRE(direct_memory.retained_layout_equation_holds());
        REQUIRE(direct_reader->admitted_bytes() == 0u);
        REQUIRE(direct_admission.reserved_bytes() == 0u);
        direct_admission.set_index_memory_cap_bytes(0u);

        // Leave the reader as the only lifetime authority for its direct
        // image.  The unrelated PROT_NONE grouped-SoA mapping remains live.
        const std::uintptr_t direct_image_begin =
            reinterpret_cast<std::uintptr_t>(
                direct_only_compact.view.image.data());
        const std::uint64_t direct_postings_offset =
            direct_only_compact.view.header.postings_offset;
        direct_only_compact.image.reset();
        direct_only_compact.view = {};
        auto direct_scratch = query_wave_scratch::create(direct_admission);
        REQUIRE(direct_scratch.has_value());
        REQUIRE_THROWS_AS(
            direct_reader->prepare_fragment_anchor_batch(
                direct_admission,
                queries,
                runtime_max_occ,
                *direct_scratch,
                /*baseline_query_plan_matcher_for_testing=*/false,
                /*verify_compositional_order_for_testing=*/false,
                minimizer_fragment_lookup_route::grouped_soa_control_scans),
            persisted_minimizer_lookup_error);
        REQUIRE_FALSE(direct_scratch->wave_outstanding());

        std::array<std::vector<bioseq::chaining::anchor>, 2u>
            direct_expected;
        direct_expected[0u] = ordinary_seed_anchors(
            pairs,
            target_length,
            query0,
            query_length,
            runtime_max_occ,
            kmer_size);
        direct_expected[1u] = ordinary_seed_anchors(
            pairs,
            target_length,
            query1,
            query_length,
            runtime_max_occ,
            kmer_size);
        auto direct_batch = direct_reader->prepare_fragment_anchor_batch(
            direct_admission,
            queries,
            runtime_max_occ,
            *direct_scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
		const std::array<std::uint32_t, 5u> exact_unique_hashes{
			7u, 8u, 9u, 10u, 11u};
		const compact_direct_query_set_stats expected_query_set_stats{
			.query_count = queries.size(),
			.query_occurrence_count = query0.size() + query1.size(),
			.unique_hash_count = exact_unique_hashes.size(),
			.group_key_set_checksum = compact_direct_query_set_checksum(
				exact_unique_hashes,
				static_cast<std::uint32_t>(kmer_size),
				10u,
				static_cast<std::uint32_t>(runtime_max_occ)),
			.present = true,
		};
		REQUIRE(direct_batch.compact_direct_query_stats()
			== expected_query_set_stats);
		auto moved_direct_batch = std::move(direct_batch);
		REQUIRE_FALSE(direct_batch.compact_direct_query_stats().present);
		REQUIRE(moved_direct_batch.compact_direct_query_stats()
			== expected_query_set_stats);
        require_anchor_bytes_equal(
			moved_direct_batch.query_view(0u).ordered_fragment_seed_anchors(
                query0, query_length, runtime_max_occ, kmer_size),
            direct_expected[0u]);
        require_anchor_bytes_equal(
			moved_direct_batch.query_view(1u).ordered_fragment_seed_anchors(
                query1, query_length, runtime_max_occ, kmer_size),
            direct_expected[1u]);
		const auto& direct_evidence = moved_direct_batch.stats();
        REQUIRE(direct_evidence.compact_direct_waves == 1u);
        REQUIRE(direct_evidence.compact_direct_selection_shards == 2u);
        REQUIRE(direct_evidence.compact_direct_shard_query_layouts == 4u);
        REQUIRE(direct_evidence.compact_direct_layout_shards_completed == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_shards_completed
            == 2u);
        REQUIRE(direct_evidence.compact_direct_posting_visit_passes == 1u);
        REQUIRE(direct_evidence.compact_direct_layout_selection_callbacks
            == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_selection_callbacks
            == 2u);
        REQUIRE(direct_evidence.compact_direct_layout_spans_exposed == 4u);
        REQUIRE(direct_evidence.compact_direct_emission_spans_exposed == 4u);
        REQUIRE(direct_evidence.compact_direct_layout_descriptors_examined
            == 2u);
        REQUIRE(direct_evidence.compact_direct_emission_descriptors_examined
            == 2u);
        REQUIRE(direct_evidence.
            compact_direct_traversal_rank_checkpoints_examined == 0u);
        REQUIRE(direct_evidence.
            compact_direct_traversal_membership_words_examined == 0u);
        REQUIRE(direct_evidence.compact_direct_rejected_posting_reads == 0u);
        REQUIRE(direct_evidence.compact_direct_target_control_scan_bytes
            == 0u);
        REQUIRE(direct_evidence.compact_direct_target_control_scan_passes
            == 0u);
        REQUIRE(direct_evidence.compact_direct_fallbacks == 0u);
        REQUIRE(direct_evidence.control_count_pass_ns == 0u);
        REQUIRE(direct_evidence.control_count_bytes == 0u);
        REQUIRE(direct_evidence.pass2_control_bytes == 0u);
        REQUIRE(direct_evidence.pass3_control_bytes == 0u);
        REQUIRE(direct_evidence.pass2_accepted_prefix_bytes == 0u);
        REQUIRE(direct_evidence.pass3_accepted_prefix_bytes == 0u);
        REQUIRE(direct_evidence.pass2_position_bytes == 0u);
        REQUIRE(direct_evidence.pass3_position_bytes
            == 4u * direct_only_posting_width);
        REQUIRE(direct_evidence.anchors_constructed == 8u);
        REQUIRE(direct_evidence.forbidden_buffers_are_zero());
        REQUIRE_FALSE(selected_page_recorder.overflow.load());
        REQUIRE(selected_page_recorder.record_count.load() == 2u);
        std::array<compact_direct_selected_page_range, 2u> advised_ranges{
            selected_page_recorder.records[0u],
            selected_page_recorder.records[1u],
        };
        std::ranges::sort(advised_ranges, {},
            &compact_direct_selected_page_range::begin);
        REQUIRE(advised_ranges[0u]
            == compact_direct_selected_page_range{
                .begin = direct_image_begin + direct_postings_offset,
                .end = direct_image_begin + direct_postings_offset + 8u,
            });
        REQUIRE(advised_ranges[1u]
            == compact_direct_selected_page_range{
                .begin = direct_image_begin + direct_postings_offset + 20u,
                .end = direct_image_begin + direct_postings_offset + 28u,
            });
        const auto direct_memory_after_query =
            direct_reader->memory_stats();
        REQUIRE(direct_memory_after_query.directory_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.ordinal_route_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.owned_capacity_bytes == 0u);
        REQUIRE(direct_memory_after_query.retained_layout_equation_holds());
        REQUIRE(direct_reader->admitted_bytes() == 0u);
        direct_scratch->complete_wave_after_join();

		const std::array<fragment_seed_query, 0u> empty_queries{};
		auto empty_direct_batch =
			direct_reader->prepare_fragment_anchor_batch(
				direct_admission,
				empty_queries,
				runtime_max_occ,
				*direct_scratch,
				/*baseline_query_plan_matcher_for_testing=*/false,
				/*verify_compositional_order_for_testing=*/false,
				minimizer_fragment_lookup_route::direct_compact_hash_lookup);
		const compact_direct_query_set_stats expected_empty_stats{
			.query_count = 0u,
			.query_occurrence_count = 0u,
			.unique_hash_count = 0u,
			.group_key_set_checksum =
				compact_direct_query_set_checksum_prefix(
					static_cast<std::uint32_t>(kmer_size),
					10u,
					static_cast<std::uint32_t>(runtime_max_occ)),
			.present = true,
		};
			REQUIRE(empty_direct_batch.compact_direct_query_stats()
				== expected_empty_stats);
			REQUIRE(selected_page_recorder.record_count.load() == 2u);
			direct_scratch->complete_wave_after_join();
    }

    {
        bioseq::parallelism::memory_admission_controller zero_cap_admission;
        auto zero_cap_reader = open_grouped_soa_fragment_query_reader(
            zero_cap_admission,
            *generation,
            sink.rows,
            0u,
            own_fragment_compact_direct_test_index(compact));
        REQUIRE(zero_cap_reader.has_value());
        auto zero_cap_scratch = query_wave_scratch::create(
            zero_cap_admission);
        REQUIRE(zero_cap_scratch.has_value());
        auto zero_cap_batch = zero_cap_reader->prepare_fragment_anchor_batch(
            zero_cap_admission,
            queries,
            0,
            *zero_cap_scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/true,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup);
		REQUIRE_FALSE(
			zero_cap_batch.compact_direct_query_stats().present);
        for (std::size_t query_ordinal = 0u;
             query_ordinal < queries.size();
             ++query_ordinal) {
            REQUIRE(zero_cap_batch.query_view(query_ordinal).
                ordered_fragment_seed_anchors(
                    queries[query_ordinal].minimizers,
                    queries[query_ordinal].query_length,
                    0,
                    queries[query_ordinal].kmer_size).empty());
        }
        const auto& zero_evidence = zero_cap_batch.stats();
        REQUIRE(zero_evidence.compact_direct_waves == 1u);
        REQUIRE(zero_evidence.compact_direct_unique_query_groups == 0u);
        REQUIRE(zero_evidence.compact_direct_unique_lookups == 0u);
        REQUIRE(zero_evidence.compact_direct_selected_hashes == 0u);
        REQUIRE(zero_evidence.compact_direct_selected_postings == 0u);
        REQUIRE(zero_evidence.compact_direct_selection_shards == 0u);
        REQUIRE(zero_evidence.compact_direct_shard_query_layouts == 0u);
        REQUIRE(zero_evidence.compact_direct_layout_shards_completed == 0u);
        REQUIRE(zero_evidence.compact_direct_emission_shards_completed == 0u);
        REQUIRE(zero_evidence.compact_direct_posting_visit_passes == 0u);
        REQUIRE(zero_evidence.compact_direct_plan_bytes == 0u);
        REQUIRE(zero_evidence.query_plan_allocations == 0u);
        REQUIRE(zero_evidence.peak_query_plan_capacity_bytes == 0u);
        REQUIRE(zero_evidence.compact_direct_lookup_ns == 0u);
        REQUIRE(zero_evidence.control_count_pass_ns == 0u);
        REQUIRE(zero_evidence.control_count_bytes == 0u);
        REQUIRE(zero_evidence.pass2_position_bytes == 0u);
        REQUIRE(zero_evidence.pass3_position_bytes == 0u);
        REQUIRE(zero_evidence.anchors_constructed == 0u);
        REQUIRE(zero_evidence.exact_final_bytes == 0u);
        const auto zero_snapshot =
            zero_cap_scratch->occurrence_capacity_snapshot();
        REQUIRE(zero_snapshot.packed_size == 0u);
        REQUIRE(zero_snapshot.wide_size == 0u);
        REQUIRE(zero_snapshot.query_plan_size == 0u);
        zero_cap_scratch->complete_wave_after_join();
    }

    const std::uint8_t direct_posting_width =
        compact.view.header.posting_width_bytes;
    auto grouped_soa_payload = make_grouped_soa_mapped_payload(
        *generation, sink.rows);
    auto protected_payload = make_sparse_mapped_test_payload(
        grouped_soa_payload->size());
    REQUIRE(protected_payload.has_value());
    std::copy(
        grouped_soa_payload->begin(),
        grouped_soa_payload->end(),
        protected_payload->bytes().begin());
    const auto protected_payload_bytes = std::span<const std::byte>{
        protected_payload->bytes()};
    void* const protected_payload_address =
        protected_payload->owner.get();
    const std::size_t protected_payload_size = protected_payload->size;

    bioseq::parallelism::memory_admission_controller admission;
    std::shared_ptr<const void> protected_payload_owner =
        protected_payload->owner;
    auto reader = prepared_minimizer_fragment_reader::open(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(build_cap),
        generation->directory.encoded_bytes(),
        runtime_max_occ,
        std::move(protected_payload_owner),
        protected_payload_bytes,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    REQUIRE(reader->compact_direct_ready());
    // The reader now owns both mappings.  Drop every external lifetime token,
    // then make the grouped-SoA payload inaccessible: the explicit direct route
    // must remain valid and cannot silently scan controls or positions.
    REQUIRE(mprotect(
        protected_payload_address,
        protected_payload_size,
        PROT_NONE) == 0);
    protected_payload->owner.reset();
    grouped_soa_payload.reset();
    compact.image.reset();
    compact.view = {};
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    std::array<std::vector<bioseq::chaining::anchor>, 2u> expected;
    expected[0u] = ordinary_seed_anchors(
        pairs,
        target_length,
        query0,
        query_length,
        runtime_max_occ,
        kmer_size);
    expected[1u] = ordinary_seed_anchors(
        pairs,
        target_length,
        query1,
        query_length,
        runtime_max_occ,
        kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    require_anchor_bytes_equal(
        batch.query_view(0u).ordered_fragment_seed_anchors(
            query0, query_length, runtime_max_occ, kmer_size),
        expected[0u]);
    require_anchor_bytes_equal(
        batch.query_view(1u).ordered_fragment_seed_anchors(
            query1, query_length, runtime_max_occ, kmer_size),
        expected[1u]);
    REQUIRE(expected[0u].size() == 4u);
    REQUIRE(expected[1u].size() == 4u);
    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_waves == 1u);
    REQUIRE(evidence.compact_direct_unique_query_groups == 5u);
    REQUIRE(evidence.compact_direct_unique_lookups == 5u);
    REQUIRE(evidence.compact_direct_selected_hashes == 2u);
    REQUIRE(evidence.compact_direct_runtime_rejected_hashes == 1u);
    REQUIRE(evidence.compact_direct_runtime_rejected_postings == 3u);
    REQUIRE(evidence.compact_direct_selected_postings == 4u);
    REQUIRE(evidence.compact_direct_selection_shards == 2u);
    REQUIRE(evidence.compact_direct_shard_query_layouts == 4u);
    REQUIRE(evidence.compact_direct_layout_shards_completed == 2u);
    REQUIRE(evidence.compact_direct_emission_shards_completed == 2u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == 4u);
    REQUIRE(evidence.compact_direct_layout_selection_callbacks == 2u);
    REQUIRE(evidence.compact_direct_emission_selection_callbacks == 2u);
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 4u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 4u);
    REQUIRE(evidence.compact_direct_layout_descriptors_examined == 2u);
    REQUIRE(evidence.compact_direct_emission_descriptors_examined == 2u);
    REQUIRE(evidence.compact_direct_traversal_rank_checkpoints_examined
        == 0u);
    REQUIRE(evidence.compact_direct_traversal_membership_words_examined
        == 0u);
    REQUIRE(evidence.compact_direct_rejected_posting_reads == 0u);
    REQUIRE(evidence.compact_direct_target_control_scan_bytes == 0u);
    REQUIRE(evidence.compact_direct_target_control_scan_passes == 0u);
    REQUIRE(evidence.compact_direct_fallbacks == 0u);
    REQUIRE(evidence.compact_direct_plan_bytes
        == 2u * sizeof(compact_hash32_unique_selection));
    REQUIRE(evidence.compact_direct_plan_reserve_calls == 1u);
    REQUIRE(evidence.compact_direct_plan_reserve_requested_elements == 5u);
    REQUIRE(evidence.compact_direct_plan_initial_capacity_elements == 5u);
    REQUIRE(evidence.compact_direct_plan_fill_reallocations == 0u);
    REQUIRE(evidence.compact_direct_plan_compaction_allocations == 1u);
    REQUIRE(evidence.compact_direct_plan_storage_allocations == 2u);
    REQUIRE(evidence.compact_direct_plan_retained_capacity_elements == 2u);
    REQUIRE(evidence.compact_direct_plan_retained_capacity_bytes
        == evidence.compact_direct_plan_bytes);
    REQUIRE(evidence.peak_compact_direct_plan_capacity_bytes
        == 7u * sizeof(compact_hash32_unique_selection));
    REQUIRE(evidence.compact_direct_lookup_ns > 0u);
    REQUIRE(evidence.control_count_pass_ns == 0u);
    REQUIRE(evidence.control_count_bytes == 0u);
    REQUIRE(evidence.pass2_control_bytes == 0u);
    REQUIRE(evidence.pass3_control_bytes == 0u);
    REQUIRE(evidence.pass2_accepted_prefix_bytes == 0u);
    REQUIRE(evidence.pass3_accepted_prefix_bytes == 0u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes
        == 4u * direct_posting_width);
    REQUIRE(evidence.anchors_constructed == 8u);
    REQUIRE(evidence.exact_final_bytes
        == 8u * sizeof(bioseq::chaining::anchor));
    REQUIRE(evidence.forbidden_buffers_are_zero());
    const auto snapshot = scratch->occurrence_capacity_snapshot();
    REQUIRE(snapshot.route == query_occurrence_route::packed);
    REQUIRE(snapshot.query_plan_size == 5u);
    REQUIRE(evidence.peak_query_plan_capacity_bytes
        == static_cast<std::uint64_t>(snapshot.query_plan_capacity) * 16u);
    REQUIRE(snapshot.query_membership_filter_capacity_bytes == 0u);
    REQUIRE(snapshot.query_group_offset_capacity == 0u);
    REQUIRE_FALSE(snapshot.bucket_direct_matcher_active);
    scratch->complete_wave_after_join();

    const auto retained_before_plan_boundary = admission.reserved_bytes();
    const auto capacities_before_plan_boundary =
        scratch->occurrence_capacity_snapshot();
    constexpr std::uint64_t admitted_unique_capacity = 4u;
    const std::uint64_t admitted_plan_peak =
        2u * admitted_unique_capacity
            * sizeof(compact_hash32_unique_selection);
    const std::uint64_t next_plan_metadata =
        queries.size() * sizeof(prepared_query_anchors);
    admission.set_index_memory_cap_bytes(
        retained_before_plan_boundary
            + next_plan_metadata + admitted_plan_peak);
    // This query has c+1=5 unique hashes. Its exact preallocation peak is
    // rejected before the vector reserve, and all wave owners return reusable.
    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            runtime_max_occ,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/false,
            /*verify_compositional_order_for_testing=*/false,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
    REQUIRE(admission.reserved_bytes() == retained_before_plan_boundary);
    const auto capacities_after_plan_boundary =
        scratch->occurrence_capacity_snapshot();
    REQUIRE(capacities_after_plan_boundary.route
        == capacities_before_plan_boundary.route);
    REQUIRE(capacities_after_plan_boundary.packed_size
        == capacities_before_plan_boundary.packed_size);
    REQUIRE(capacities_after_plan_boundary.packed_capacity
        == capacities_before_plan_boundary.packed_capacity);
    REQUIRE(capacities_after_plan_boundary.wide_size
        == capacities_before_plan_boundary.wide_size);
    REQUIRE(capacities_after_plan_boundary.wide_capacity
        == capacities_before_plan_boundary.wide_capacity);
    REQUIRE(capacities_after_plan_boundary.query_plan_size
        == capacities_before_plan_boundary.query_plan_size);
    REQUIRE(capacities_after_plan_boundary.query_plan_capacity
        == capacities_before_plan_boundary.query_plan_capacity);
    REQUIRE(capacities_after_plan_boundary.query_plan_reserve_request
        == capacities_before_plan_boundary.query_plan_reserve_request);
    REQUIRE(capacities_after_plan_boundary.query_group_offset_size
        == capacities_before_plan_boundary.query_group_offset_size);
    REQUIRE(capacities_after_plan_boundary.query_group_offset_capacity
        == capacities_before_plan_boundary.query_group_offset_capacity);
    REQUIRE(capacities_after_plan_boundary.aggregate_occurrence_capacity_bytes
        == capacities_before_plan_boundary.aggregate_occurrence_capacity_bytes);
    REQUIRE(capacities_after_plan_boundary.
            query_membership_filter_capacity_bytes
        == capacities_before_plan_boundary.
            query_membership_filter_capacity_bytes);
    REQUIRE(capacities_after_plan_boundary.occurrence_radix_invoked
        == capacities_before_plan_boundary.occurrence_radix_invoked);
    REQUIRE(capacities_after_plan_boundary.bucket_direct_matcher_active
        == capacities_before_plan_boundary.bucket_direct_matcher_active);

    admission.set_index_memory_cap_bytes(
        (std::numeric_limits<std::uint64_t>::max)());
    auto reused_batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/false,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    REQUIRE(reused_batch.stats().occurrence_allocations == 0u);
    REQUIRE(reused_batch.stats().query_plan_allocations == 0u);
    REQUIRE(reused_batch.stats().compact_direct_plan_fill_reallocations
        == 0u);
    REQUIRE(reused_batch.stats().compact_direct_plan_storage_allocations
        == 2u);
    const auto reused_snapshot = scratch->occurrence_capacity_snapshot();
    REQUIRE(reused_batch.stats().peak_query_plan_capacity_bytes
        == static_cast<std::uint64_t>(reused_snapshot.query_plan_capacity)
            * 16u);
    scratch->complete_wave_after_join();

    REQUIRE_THROWS_AS(
        reader->prepare_fragment_anchor_batch(
            admission,
            queries,
            runtime_max_occ,
            *scratch,
            /*baseline_query_plan_matcher_for_testing=*/true,
            /*verify_compositional_order_for_testing=*/false,
            minimizer_fragment_lookup_route::direct_compact_hash_lookup),
        persisted_minimizer_lookup_error);
    REQUIRE_FALSE(scratch->wave_outstanding());
    scratch->release_retained_capacity();
    REQUIRE(scratch->admitted_bytes() == 0u);
}

TEST_CASE("compact-direct single-query occurrence emission covers P32 spans",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](
            std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t max_occ = 4u;
    constexpr std::uint64_t target_length = 400u;
    constexpr bioseq::storage::sequence_size_t query_length = 200u;
    constexpr std::int32_t kmer_size = 15;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        // Both P32 inline-singleton strand orientations.
        {7u, packed(10u, 0u)},
        {8u, packed(20u, 1u)},
        // One query occurrence expands an out-of-line two-strand list.
        {9u, packed(30u, 0u)},
        {9u, packed(31u, 0u)},
        {9u, packed(32u, 1u)},
        {9u, packed(33u, 1u)},
        // Duplicate query occurrences retain the generic fallback.
        {10u, packed(40u, 0u)},
        {10u, packed(41u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(target_length, max_occ));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head,
        generation->manifest,
        pairs,
        max_occ);
    REQUIRE(compact.view.header.posting_width_bytes == 4u);
    REQUIRE(compact.view.header.inline_singleton_count == 2u);
    REQUIRE(compact.view.header.materialized_posting_count == 6u);

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{8u, 30u, 1u},
        minimap_sketch_data{9u, 40u, 0u},
        minimap_sketch_data{10u, 50u, 0u},
        minimap_sketch_data{10u, 70u, 1u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = query,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected = ordinary_seed_anchors(
        pairs,
        target_length,
        query,
        query_length,
        max_occ,
        kmer_size);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(max_occ),
        max_occ,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
        query, query_length, max_occ, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == 10u);

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_selected_hashes == 4u);
    REQUIRE(evidence.compact_direct_selected_postings == 8u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == 8u);
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 6u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 6u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes == 8u * 4u);
    REQUIRE(evidence.anchors_constructed == actual.size());
    REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    scratch->complete_wave_after_join();
}

TEST_CASE("compact-direct single-query occurrence emission covers P64 spans",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](
            std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t max_occ = 4u;
    constexpr std::uint64_t high_bp =
        (std::uint64_t{1u} << 31u) + 10u;
    constexpr std::uint64_t target_length = high_bp + 1'000u;
    constexpr bioseq::storage::sequence_size_t query_length = 200u;
    constexpr std::int32_t kmer_size = 15;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        // P64 never inlines, including a count-one posting list.
        {7u, packed(high_bp, 0u)},
        // One occurrence expands a multi-position, two-strand P64 list.
        {8u, packed(100u, 0u)},
        {8u, packed(101u, 0u)},
        {8u, packed(high_bp + 1u, 1u)},
        {8u, packed(high_bp + 2u, 1u)},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(target_length, max_occ));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head,
        generation->manifest,
        pairs,
        max_occ);
    REQUIRE(compact.view.header.posting_width_bytes == 8u);
    REQUIRE(compact.view.header.inline_singleton_count == 0u);
    REQUIRE(compact.view.header.materialized_posting_count == pairs.size());

    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
        minimap_sketch_data{8u, 40u, 1u},
    };
    const std::array queries{
        fragment_seed_query{
            .minimizers = query,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };
    const auto expected = ordinary_seed_anchors(
        pairs,
        target_length,
        query,
        query_length,
        max_occ,
        kmer_size);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(max_occ),
        max_occ,
        own_fragment_compact_direct_test_index(compact));
    REQUIRE(reader.has_value());
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual = batch.query_view(0u).ordered_fragment_seed_anchors(
        query, query_length, max_occ, kmer_size);
    require_anchor_bytes_equal(actual, expected);
    REQUIRE(actual.size() == pairs.size());

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_selected_hashes == 2u);
    REQUIRE(evidence.compact_direct_selected_postings == pairs.size());
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited == pairs.size());
    REQUIRE(evidence.compact_direct_layout_spans_exposed == 3u);
    REQUIRE(evidence.compact_direct_emission_spans_exposed == 3u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes == pairs.size() * 8u);
    REQUIRE(evidence.anchors_constructed == actual.size());
    REQUIRE(evidence.exact_final_bytes == actual.size_bytes());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    scratch->complete_wave_after_join();
}

TEST_CASE("compact-direct selection shards preserve P32 differential parity",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    constexpr auto packed = [](
            std::uint64_t bp, std::uint64_t strand) noexcept {
        return (bp << 1u) | strand;
    };
    constexpr std::uint32_t build_cap = 4u;
    constexpr std::int32_t runtime_max_occ = 2;
    constexpr std::uint64_t target_length = 10'000u;
    constexpr std::uint64_t selected_hash_count = 40u;
    constexpr std::uint64_t heavy_hash = 500u;
    constexpr bioseq::storage::sequence_size_t query_length = 300u;
    constexpr std::int32_t kmer_size = 15;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(selected_hash_count + 3u);
    for (std::uint64_t hash = 1u;
         hash <= selected_hash_count;
         ++hash) {
        pairs.emplace_back(
            hash, packed(100u + 3u * hash, hash & 1u));
    }
    pairs.emplace_back(heavy_hash, packed(1'000u, 0u));
    pairs.emplace_back(heavy_hash, packed(1'001u, 0u));
    pairs.emplace_back(heavy_hash, packed(1'002u, 1u));

    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(target_length, build_cap));
    REQUIRE(generation.has_value());
    auto compact = make_fragment_compact_direct_test_index(
        generation->head,
        generation->manifest,
        pairs,
        build_cap);
    REQUIRE(compact.view.header.posting_width_bytes == 4u);

    std::vector<minimap_sketch_data> query0;
    std::vector<minimap_sketch_data> query1;
    query0.reserve(selected_hash_count + 2u);
    query1.reserve(selected_hash_count + 1u);
    for (std::uint64_t hash = 1u;
         hash <= selected_hash_count;
         ++hash) {
        query0.push_back(minimap_sketch_data{
            hash,
            20u + hash,
            static_cast<std::uint8_t>((hash + 1u) & 1u),
        });
    }
    // Repeat one accepted occurrence byte-for-byte. It must expand through
    // its shard-local cursor without introducing an atomic or shared cursor.
    query0.push_back(query0[6u]);
    query0.push_back(minimap_sketch_data{heavy_hash, 90u, 0u});
    for (std::uint64_t hash = selected_hash_count;
         hash != 0u;
         --hash) {
        query1.push_back(minimap_sketch_data{
            hash,
            140u + hash,
            static_cast<std::uint8_t>(hash & 1u),
        });
    }
    query1.push_back(minimap_sketch_data{heavy_hash, 200u, 1u});
    const std::array queries{
        fragment_seed_query{
            .minimizers = query0,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
        fragment_seed_query{
            .minimizers = query1,
            .query_length = query_length,
            .kmer_size = kmer_size,
        },
    };

    bioseq::parallelism::memory_admission_controller admission;
    selected_page_advice_recorder selected_page_recorder;
    auto reader = prepared_minimizer_fragment_reader::open_compact_direct(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(build_cap),
        runtime_max_occ,
        own_fragment_compact_direct_test_index(compact),
        selected_page_recorder.token());
    REQUIRE(reader.has_value());
    REQUIRE(selected_page_recorder.record_count.load() == 0u);
    auto scratch = query_wave_scratch::create(admission);
    REQUIRE(scratch.has_value());

    const auto expected0 = ordinary_seed_anchors(
        pairs,
        target_length,
        query0,
        query_length,
        runtime_max_occ,
        kmer_size);
    const auto expected1 = ordinary_seed_anchors(
        pairs,
        target_length,
        query1,
        query_length,
        runtime_max_occ,
        kmer_size);
    auto batch = reader->prepare_fragment_anchor_batch(
        admission,
        queries,
        runtime_max_occ,
        *scratch,
        /*baseline_query_plan_matcher_for_testing=*/false,
        /*verify_compositional_order_for_testing=*/true,
        minimizer_fragment_lookup_route::direct_compact_hash_lookup);
    const auto actual0 = batch.query_view(0u).
        ordered_fragment_seed_anchors(
            query0, query_length, runtime_max_occ, kmer_size);
    const auto actual1 = batch.query_view(1u).
        ordered_fragment_seed_anchors(
            query1, query_length, runtime_max_occ, kmer_size);
    require_anchor_bytes_equal(actual0, expected0);
    require_anchor_bytes_equal(actual1, expected1);
    REQUIRE(actual0.size() == selected_hash_count + 1u);
    REQUIRE(actual1.size() == selected_hash_count);

    const auto& evidence = batch.stats();
    REQUIRE(evidence.compact_direct_unique_query_groups
        == selected_hash_count + 1u);
    REQUIRE(evidence.compact_direct_selected_hashes == selected_hash_count);
    REQUIRE(evidence.compact_direct_runtime_rejected_hashes == 1u);
    REQUIRE(evidence.compact_direct_runtime_rejected_postings == 3u);
    REQUIRE(evidence.compact_direct_selected_postings == selected_hash_count);
    REQUIRE(evidence.compact_direct_selection_shards == 32u);
    REQUIRE(evidence.compact_direct_shard_query_layouts == 64u);
    REQUIRE(evidence.compact_direct_layout_shards_completed == 32u);
    REQUIRE(evidence.compact_direct_emission_shards_completed == 32u);
    REQUIRE(evidence.compact_direct_posting_visit_passes == 1u);
    REQUIRE(evidence.compact_direct_layout_postings_visited == 0u);
    REQUIRE(evidence.compact_direct_emission_postings_visited
        == selected_hash_count);
    REQUIRE(evidence.compact_direct_layout_selection_callbacks
        == selected_hash_count);
    REQUIRE(evidence.compact_direct_emission_selection_callbacks
        == selected_hash_count);
    REQUIRE(evidence.compact_direct_layout_descriptors_examined
        == selected_hash_count);
    REQUIRE(evidence.compact_direct_emission_descriptors_examined
        == selected_hash_count);
    REQUIRE(evidence.compact_direct_traversal_rank_checkpoints_examined
        == 0u);
    REQUIRE(evidence.compact_direct_traversal_membership_words_examined
        == 0u);
    REQUIRE(evidence.pass2_position_bytes == 0u);
    REQUIRE(evidence.pass3_position_bytes
        == selected_hash_count * compact.view.header.posting_width_bytes);
    REQUIRE(evidence.anchors_constructed
        == actual0.size() + actual1.size());
    REQUIRE(evidence.forbidden_buffers_are_zero());
    REQUIRE_FALSE(selected_page_recorder.overflow.load());
    REQUIRE(selected_page_recorder.record_count.load() == 0u);
    scratch->complete_wave_after_join();
}

TEST_CASE("fragment anchor admission failure leaves reusable owners exact",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {7u, 40u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    const std::vector<minimap_sketch_data> query{
        minimap_sketch_data{7u, 20u, 0u},
    };
    constexpr bioseq::storage::sequence_size_t query_length = 100u;
    constexpr std::int32_t kmer_size = 15;
    const std::array queries{
        fragment_seed_query{
            std::span<const minimap_sketch_data>{query},
            query_length,
            kmer_size},
    };

    bioseq::parallelism::memory_admission_controller admission;
    {
        auto reader = open_grouped_soa_fragment_query_reader(
            admission, *generation, sink.rows, 2u);
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        {
            auto warm = reader->prepare_fragment_anchor_batch(
                admission, queries, 2, *scratch);
            REQUIRE(warm.admitted_bytes() > 0u);
            REQUIRE_FALSE(warm.query_view(0u).
                ordered_fragment_seed_anchors(
                    query, query_length, 2, kmer_size).empty());
            scratch->complete_wave_after_join();
        }
        const std::uint64_t retained_without_batch =
            admission.reserved_bytes();
        REQUIRE(retained_without_batch
            == reader->admitted_bytes() + scratch->admitted_bytes());
        REQUIRE(scratch->admitted_bytes() > 0u);
        admission.set_index_memory_cap_bytes(
            retained_without_batch + sizeof(prepared_query_anchors));

        REQUIRE_THROWS_AS(
            reader->prepare_fragment_anchor_batch(
                admission, queries, 2, *scratch),
            persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
        REQUIRE(admission.reserved_bytes() == retained_without_batch);
        scratch->release_retained_capacity();
        REQUIRE(scratch->admitted_bytes() == 0u);
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("fragment anchor preparation rejects trusted payload corruption",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {7u, 20u},
        {7u, 40u},
    };
    vector_replay_minimizer_producer producer{pairs};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer,
        sink,
        test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 1u);
    auto payload = make_grouped_soa_mapped_payload(*generation, sink.rows);
    const auto& entry = generation->directory.entries().front();
    const auto layout = plan_grouped_soa_control_plane(
        entry.control_element_count,
        generation->directory.profile().hash_width,
        entry.shape_offset_width,
        generation->directory.profile().count_width);
    REQUIRE(layout.has_value());
    REQUIRE(generation->directory.profile().count_width
        == minimizer_scalar_width::u16);
    test_write_le<std::uint16_t>(
        std::span<std::byte>{payload->data(), payload->size()},
        static_cast<std::size_t>(layout->count_offset),
        3u);

    bioseq::parallelism::memory_admission_controller admission;
    {
        std::shared_ptr<const void> payload_owner = payload;
        auto reader = prepared_minimizer_fragment_reader::open(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            1u,
            std::move(payload_owner),
            std::span<const std::byte>{payload->data(), payload->size()});
        REQUIRE(reader.has_value());
        auto scratch = query_wave_scratch::create(admission);
        REQUIRE(scratch.has_value());
        const std::vector<minimap_sketch_data> query{
            minimap_sketch_data{7u, 20u, 0u},
        };
        const std::array queries{
            fragment_seed_query{
                std::span<const minimap_sketch_data>{query}, 100u, 15},
        };
        REQUIRE_THROWS_AS(
            reader->prepare_fragment_anchor_batch(
                admission, queries, 1, *scratch),
            persisted_minimizer_lookup_error);
        REQUIRE_FALSE(scratch->wave_outstanding());
        scratch->release_retained_capacity();
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("grouped-SoA fragment and external-spill replays are identical",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 256u;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<
        std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    const auto verify = [&](std::vector<seq::dna4> data,
                            bool require_offset_widening) {
        range_t range(data);
        lazy_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        auto fragment_producer = index_type::make_generation_replay_producer(
            lazy_view,
            minimizer_sketch_backend::exact_simd,
            fixed_minimizer_kernels(),
            fixed_alphabet_code<::seq::dna4>());
        collecting_minimizer_row_sink fragment_sink;
        auto fragment_options = test_grouped_soa_generation_options(
            range.bp_size(), 65'534u);
        const auto fragment_generation = encode_minimizer_generation(
            fragment_producer, fragment_sink, fragment_options);
        REQUIRE(fragment_generation.has_value());
        REQUIRE(fragment_producer.scan_count() == 3u);
        const auto fragment_stats = fragment_producer.execution_stats();
        INFO("grouped-SoA fragment evidence scans="
            << fragment_stats.producer_scan_count
            << " required_successful="
            << fragment_stats.required_successful_scan_count()
            << " geometry_retries="
            << fragment_stats.geometry_retry_count
            << " saturated=" << fragment_stats.saturated);
        REQUIRE(fragment_stats.generation_layout
            == minimizer_generation_layout::
                grouped_soa_segment_local);
        REQUIRE(fragment_stats.required_successful_scan_count() == 3u);
        REQUIRE(fragment_stats.is_accepted());
        REQUIRE(fragment_stats.selected_backend
            == minimizer_sketch_backend::exact_simd);
        REQUIRE(fragment_stats.scalar_core_runs == 0u);
        REQUIRE(fragment_stats.exact_simd_core_runs > 0u);
        REQUIRE(fragment_stats.bounded_nthash_core_runs == 0u);
        REQUIRE(fragment_stats.vulkan_core_runs == 0u);
        REQUIRE(fragment_stats.exact_simd_stats_present);
        REQUIRE(fragment_stats.exact_simd_stats.is_internally_consistent(true));

        fake_fragment_storage<> spill_storage;
        minimizer_sequence_generation_execution_receipt spill_execution;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            spill_storage,
            lazy_view,
            minimizer_sketch_backend::exact_simd,
            fixed_minimizer_kernels(),
            fragment_bp,
            k_minimizer_preferred_segment_span_bp,
            &spill_execution,
            fixed_alphabet_code<::seq::dna4>());
        REQUIRE(spill.has_value());
        REQUIRE(spill->run_count() >= 2u);
        REQUIRE(spill_execution.is_spill_valid());
        REQUIRE(spill_execution.selected_backend
            == minimizer_sketch_backend::exact_simd);
        REQUIRE(spill_execution.scalar_core_runs == 0u);
        REQUIRE(spill_execution.exact_simd_core_runs > 0u);
        REQUIRE(spill_execution.bounded_nthash_core_runs == 0u);
        REQUIRE(spill_execution.vulkan_core_runs == 0u);
        REQUIRE(spill_execution.exact_simd_stats_available);
        REQUIRE(spill_execution.exact_simd_stats.is_internally_consistent(true));
        auto spill_producer = spill_run_index_type::make_generation_replay_producer(
            spill_storage, range.bp_size(), std::move(*spill));
        collecting_minimizer_row_sink spill_sink;
        auto spill_options = fragment_options;
        ++spill_options.generation_id;
        const auto spill_generation = encode_minimizer_generation(
            spill_producer, spill_sink, spill_options);
        REQUIRE(spill_generation.has_value());
        REQUIRE(spill_producer.scan_count() == 3u);
        REQUIRE(spill_producer.merge_pass_count() == 3u);
        const auto spill_replay_stats =
            spill_producer.execution_receipt();
        REQUIRE(spill_replay_stats.generation_layout
            == minimizer_generation_layout::
                grouped_soa_segment_local);
        REQUIRE(spill_replay_stats.required_successful_replay_count()
            == 3u);
        REQUIRE(spill_replay_stats.replay_scan_count == 3u);
        REQUIRE(spill_replay_stats.replay_completed_scan_count == 3u);
        REQUIRE(spill_replay_stats.replay_failed_scan_count == 0u);
        REQUIRE(spill_replay_stats.replay_reroute_pass_count == 0u);
        REQUIRE(spill_replay_stats.replay_visited_event_count
            == 3u * spill_replay_stats.spilled_event_count);
        require_encoded_generations_equal_normalized(
            *fragment_generation,
            fragment_sink.rows,
            *spill_generation,
            spill_sink.rows);
        REQUIRE(decode_generation_events(*fragment_generation, fragment_sink.rows)
            == decode_generation_events(*spill_generation, spill_sink.rows));
        require_complete_generation_rows(*fragment_generation, fragment_sink.rows);
        require_complete_generation_rows(*spill_generation, spill_sink.rows);

        if (require_offset_widening) {
            REQUIRE(std::ranges::any_of(
                fragment_generation->directory.entries(), [](const auto& entry) {
                    return entry.shape_offset_width
                        == minimizer_scalar_width::u32;
                }));
            REQUIRE(std::ranges::any_of(
                spill_generation->directory.entries(), [](const auto& entry) {
                    return entry.shape_offset_width
                        == minimizer_scalar_width::u32;
                }));
        }

        const auto fourth_replay = spill_producer.scan_sorted_events(
            spill_generation->manifest.position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = nullptr,
                .emit_fn = [](
                        void*, const canonical_minimizer_event&)
                        -> std::expected<
                            void, minimizer_generation_encoding_error> {
                    return {};
                },
            });
        require_wire_error(
            fourth_replay, minimizer_generation_encoding_error::segment_span);
        const auto terminal_evidence = spill_producer.execution_receipt();
        REQUIRE(terminal_evidence.replay_scan_count == 4u);
        REQUIRE(terminal_evidence.replay_completed_scan_count == 3u);
        REQUIRE(terminal_evidence.replay_failed_scan_count == 1u);
    };

    SECTION("random fragmented sequence") {
        verify(make_dna4(18'013u, 0x373352414E444F4DULL), false);
    }
    SECTION("long tandem sequence widens offsets") {
        std::vector<seq::dna4> tandem(70'100u);
        for (auto& base : tandem) base.assign_rank(0u);
        verify(std::move(tandem), true);
    }
}

TEST_CASE("minimizer_hashmap_fragment_index build(vector) + lookup",
          "[component:bioseq][layer:unit][feature:index]") {
    // Hand-built sketches; positions kept small so the 28-bit pos field of
    // minimap_sketch_data{kmer,pos,strand} is never truncated.
    std::vector<minimap_sketch_data> sketches{
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 5,  /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 9,  /*strand*/ 1},
        minimap_sketch_data{/*kmer*/ 7,   /*pos*/ 2,  /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 1,  /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 42,  /*pos*/ 13, /*strand*/ 1},
    };
    bioseq::indexing::minimizer_hashmap_fragment_index idx;
    idx.build(sketches, /*target_length*/ 1000);
    REQUIRE(idx.target_length() == 1000);

    auto enc = [](std::uint64_t pos, std::uint64_t strand) { return (pos << 1) | strand; };

    {  // kmer 100 has three positions
        auto span = idx.lookup(100);
        std::vector<std::uint64_t> got(span.begin(), span.end());
        std::sort(got.begin(), got.end());
        std::vector<std::uint64_t> want{enc(1, 0), enc(5, 0), enc(9, 1)};
        std::sort(want.begin(), want.end());
        REQUIRE(got == want);
    }
    {  // kmer 7 single position
        auto span = idx.lookup(7);
        REQUIRE(span.size() == 1u);
        REQUIRE(span[0] == enc(2, 0));
    }
    {  // kmer 42 single position
        auto span = idx.lookup(42);
        REQUIRE(span.size() == 1u);
        REQUIRE(span[0] == enc(13, 1));
    }
    REQUIRE(idx.lookup(/*absent*/ 999).empty());
}

TEST_CASE("transient minimizer index preserves the XXL 30-bit radix-key domain",
          "[component:bioseq][layer:unit][feature:index]") {
    using xxl_minimizer_type = minimizer_representation<33u, 15u>;
    static_assert(xxl_minimizer_type::layout_type::k_hash_bits == 30u);
    static_assert(sizeof(xxl_minimizer_type) == sizeof(std::uint64_t));

    constexpr std::uint64_t low_key = 0x0000'0021u;
    constexpr std::uint64_t high_key = (std::uint64_t{1} << 30u) - 2u;
    std::vector<xxl_minimizer_type> sketches{
        xxl_minimizer_type{/*kmer*/ high_key, /*pos*/ 499'999'991u, /*strand*/ 1u},
        xxl_minimizer_type{/*kmer*/ low_key,  /*pos*/ 17u,          /*strand*/ 0u},
        xxl_minimizer_type{/*kmer*/ high_key, /*pos*/ 123'456'789u, /*strand*/ 0u},
        xxl_minimizer_type{/*kmer*/ low_key,  /*pos*/ 29u,          /*strand*/ 1u},
    };

    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build(sketches, /*target_length*/ 500'000'000u);

    auto sorted_positions = [&index](std::uint64_t key) {
        const auto hits = index.lookup(key);
        std::vector<std::uint64_t> result{hits.begin(), hits.end()};
        std::ranges::sort(result);
        return result;
    };
    REQUIRE((sorted_positions(low_key)
             == std::vector<std::uint64_t>{17u << 1u, (29u << 1u) | 1u}));
    REQUIRE((sorted_positions(high_key)
             == std::vector<std::uint64_t>{123'456'789u << 1u,
                                           (499'999'991u << 1u) | 1u}));
    REQUIRE(index.lookup((std::uint64_t{1} << 30u) - 3u).empty());
}

TEST_CASE("transient minimizer index preserves the 128-bit representation kmer domain",
          "[component:bioseq][layer:unit][feature:index]") {
    using wide_minimizer_type = minimizer_representation<28u, 30u>;
    static_assert(wide_minimizer_type::layout_type::k_hash_bits > 64u);
    static_assert(sizeof(wide_minimizer_type) == 2u * sizeof(std::uint64_t));
    static_assert(std::is_same_v<decltype(std::declval<const wide_minimizer_type&>().kmer()),
                                 std::uint64_t>);

    // The 128-bit representation's public kmer() word carries 56 significant
    // bits beside its eight-bit span field.  Exercise values above uint32 so
    // this test rejects any accidental narrowing of its radix key.
    constexpr std::uint64_t low_key = 0x0000'0001'2345'6789ULL;
    constexpr std::uint64_t high_key = 0x00FE'DCBA'9876'5432ULL;
    std::vector<wide_minimizer_type> sketches{
        wide_minimizer_type{/*kmer*/ high_key, /*pos*/ 200'000'003u,
                      /*strand*/ 1u, /*span*/ 30u},
        wide_minimizer_type{/*kmer*/ low_key,  /*pos*/ 19u,
                      /*strand*/ 0u, /*span*/ 30u},
        wide_minimizer_type{/*kmer*/ high_key, /*pos*/ 123'456'789u,
                      /*strand*/ 0u, /*span*/ 30u},
    };

    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build(sketches, /*target_length*/ 250'000'000u);

    REQUIRE(index.lookup(low_key).size() == 1u);
    REQUIRE(index.lookup(low_key).front() == (19u << 1u));
    const auto high_hits = index.lookup(high_key);
    REQUIRE(high_hits.size() == 2u);
    std::vector<std::uint64_t> sorted_high{high_hits.begin(), high_hits.end()};
    std::ranges::sort(sorted_high);
    REQUIRE((sorted_high
             == std::vector<std::uint64_t>{123'456'789u << 1u,
                                           (200'000'003u << 1u) | 1u}));
    REQUIRE(index.lookup(0x0000'0002'2345'6789ULL).empty());
}

TEST_CASE("transient minimizer index consumes source and releases storage",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr auto large_table_plan =
        bioseq::indexing::plan_minimizer_hashmap_table_allocation(
            93'308'896u);
    STATIC_REQUIRE(large_table_plan.valid);
    STATIC_REQUIRE(large_table_plan.group_count == 8'388'608u);
    STATIC_REQUIRE(large_table_plan.capacity == 125'829'119u);
    STATIC_REQUIRE(large_table_plan.allocation_bytes == 2'147'483'648u);

    std::vector<minimap_sketch_data> sketches{
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 5,  /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 7,   /*pos*/ 2,  /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 9,  /*strand*/ 1},
    };
    bioseq::indexing::minimizer_hashmap_fragment_index idx;
    idx.build_consuming(std::move(sketches), /*target_length*/ 1000);

    REQUIRE(sketches.empty());
    REQUIRE(sketches.capacity() == 0u);
    REQUIRE(idx.lookup(100).size() == 2u);
    REQUIRE(idx.lookup(7).size() == 1u);
    REQUIRE(idx.target_length() == 1000);
    // This is the deliberately unchanged minimap_manifest half-reserve path, not the
    // admitted one-shot full-table path exercised below.  Its allocator
    // counters still have to report a self-consistent non-zero lifetime.
    REQUIRE(idx.table_allocation_bytes() != 0u);
    REQUIRE(idx.table_peak_allocation_bytes()
            == idx.table_allocation_bytes());

    idx.release_storage();
    REQUIRE(idx.table_empty());
    REQUIRE(idx.table_allocation_bytes() == 0u);
    REQUIRE(idx.table_peak_allocation_bytes() == 0u);
    REQUIRE(idx.positions.empty());
    REQUIRE(idx.positions.capacity() == 0u);
    REQUIRE(idx.target_length() == 1000);
}

TEST_CASE("one-shot transient minimizer index stays inside its retained permit",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::size_t count = 4'096u;
    std::vector<minimap_sketch_data> sketches;
    sketches.reserve(count);
    for (std::size_t i = 0u; i < count; ++i) {
        sketches.emplace_back(
            static_cast<std::uint64_t>(i + 1u),
            static_cast<std::uint64_t>(i),
            static_cast<std::uint64_t>(i & 1u));
    }

    const auto table_plan =
        bioseq::indexing::plan_minimizer_hashmap_table_allocation(count);
    REQUIRE(table_plan.valid);
    const std::uint64_t position_payload =
        count * sizeof(std::uint64_t);
    const std::uint64_t position_headroom = bioseq::gpu::vulkan::
        vulkan_exact_host_allocation_admission_bytes(position_payload);
    const std::uint64_t table_headroom = bioseq::gpu::vulkan::
        vulkan_exact_host_allocation_admission_bytes(
            table_plan.allocation_bytes);
    const std::uint64_t build_permit_bytes =
        bioseq::common::saturating_add(
            position_headroom, table_headroom);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t value = 0u;
        ~restore_cap() { controller.set_index_memory_cap_bytes(value); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(
        bioseq::common::saturating_add(
            baseline, build_permit_bytes + (std::uint64_t{1} << 20u)));
    auto reserved = admission.try_reserve(build_permit_bytes);
    REQUIRE(reserved.has_value());

    // Declaration order mirrors the pipeline caller: index storage dies before
    // the permit can release on implicit unwind.
    bioseq::parallelism::memory_permit permit = std::move(*reserved);
    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build_consuming_full_table(
        std::move(sketches), /*target_length=*/count);
    REQUIRE(sketches.capacity() == 0u);
    REQUIRE(index.table_allocation_bytes() == table_plan.allocation_bytes);
    REQUIRE(index.table_peak_allocation_bytes() == table_plan.allocation_bytes);
    const std::uint64_t retained_position_bytes =
        index.positions.capacity() * sizeof(std::uint64_t);
    REQUIRE(retained_position_bytes <= position_headroom);
    const std::uint64_t retained_bytes =
        retained_position_bytes + index.table_allocation_bytes();
    permit.shrink_to(retained_bytes);
    REQUIRE(admission.reserved_bytes() == baseline + retained_bytes);

    index.release_storage();
    REQUIRE(admission.reserved_bytes() == baseline + retained_bytes);
    permit.release();
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("empty one-shot minimizer index has an exact zero-byte plan",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr auto empty_plan =
        bioseq::indexing::plan_minimizer_hashmap_table_allocation(0u);
    STATIC_REQUIRE(empty_plan.valid);
    STATIC_REQUIRE(empty_plan.distinct_upper_bound == 0u);
    STATIC_REQUIRE(empty_plan.requested_slots == 0u);
    STATIC_REQUIRE(empty_plan.group_count == 0u);
    STATIC_REQUIRE(empty_plan.capacity == 0u);
    STATIC_REQUIRE(empty_plan.allocation_bytes == 0u);

    std::vector<minimap_sketch_data> empty;
    empty.reserve(64u);
    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build_consuming_full_table(
        std::move(empty), /*target_length=*/1'000u);

    REQUIRE(empty.capacity() == 0u);
    REQUIRE(index.positions.empty());
    REQUIRE(index.positions.capacity() == 0u);
    REQUIRE(index.table_empty());
    REQUIRE(index.table_allocation_bytes() == 0u);
    REQUIRE(index.table_peak_allocation_bytes() == 0u);
    REQUIRE(index.target_length() == 1'000u);
}

TEST_CASE("minimizer_hashmap_fragment_index serialize/deserialize round-trip",
          "[component:bioseq][layer:unit][feature:index]") {
    auto data = make_dna4(2000, 0xC0FFEE);
    range_t range(data);
    auto sketches = whole_sequence_minimizers(range, 10, 15);

    bioseq::indexing::minimizer_hashmap_fragment_index idx;
    idx.build(sketches, range.bp_size());

    std::vector<char> buf(idx.serialize_binary_size());
    idx.serialize_to_binary(buf.data());
    auto round = bioseq::indexing::minimizer_hashmap_fragment_index::deserialize_from_binary(
        buf.data(), buf.size());

    REQUIRE(round.target_length() == idx.target_length());
    REQUIRE(round.positions.size() == idx.positions.size());
    REQUIRE(round.table_size() == idx.table_size());

    // Per-kmer position sets must survive the wire round-trip exactly.
    std::vector<std::uint64_t> kmers;
    for (const auto& m : sketches) kmers.push_back(m.kmer());
    REQUIRE(lookup_map(round, kmers) == lookup_map(idx, kmers));
}

TEST_CASE("build_from_pairs equals build(vector) over equivalent pairs",
          "[component:bioseq][layer:unit][feature:index]") {
    auto data = make_dna4(3000, 0x5EED);
    range_t range(data);
    auto sketches = whole_sequence_minimizers(range, 10, 15);

    bioseq::indexing::minimizer_hashmap_fragment_index via_vector;
    via_vector.build(sketches, range.bp_size());

    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(sketches.size());
    for (const auto& m : sketches) pairs.emplace_back(m.kmer(), m.pos_strand_encoded());

    bioseq::indexing::minimizer_hashmap_fragment_index via_pairs;
    via_pairs.build_from_pairs(std::span<std::pair<std::uint64_t, std::uint64_t>>(pairs), range.bp_size());

    REQUIRE(via_pairs.target_length() == via_vector.target_length());
    REQUIRE(via_pairs.positions.size() == via_vector.positions.size());

    std::vector<std::uint64_t> kmers;
    for (const auto& m : sketches) kmers.push_back(m.kmer());
    REQUIRE(lookup_map(via_pairs, kmers) == lookup_map(via_vector, kmers));
}

TEST_CASE("assemble_segment over fragmented storage equals whole-sequence build",
          "[component:bioseq][layer:unit][feature:index]") {
    // frag_units = 1024 -> frag_bp = 1024 * 32 = 32768; a ~40000 bp dna4
    // sequence therefore spans 2 fragments (multi-fragment build path).
    constexpr std::size_t fragment_units = 1024;
    constexpr std::size_t frag_bp   = fragment_units * 32;   // 32768
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;

    auto data = make_dna4(40000, 0xABC123);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
    REQUIRE(nfrag >= 2u);   // multi-fragment

    fake_fragment_storage<> storage;
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    bioseq::indexing::minimizers_per_fragment_index<fake_fragment_storage<>>::build(
        storage, lazy_view);

    bioseq::indexing::minimizer_assembly_scratch scratch;
    auto assembled = bioseq::indexing::minimizers_per_fragment_index<fake_fragment_storage<>>::assemble_segment(
        storage, /*frag_lo*/ 0, /*frag_hi*/ nfrag, /*frag_bp*/ (bioseq::storage::sequence_size_t)frag_bp,
        /*total_bp*/ total_bp, /*clip_lo*/ 0, /*clip_hi*/ total_bp, scratch);

    // The whole-sequence baseline: a single minimizer_hashmap_fragment_index over
    // every minimizer collected from the whole range.
    auto whole = whole_sequence_minimizers(range, 10, 15);
    bioseq::indexing::minimizer_hashmap_fragment_index baseline;
    baseline.build(whole, total_bp);

    REQUIRE(assembled.target_length() == total_bp);

    // dna4 (no N) => EXACT set equality of (kmer -> sorted positions). Build the
    // union of kmers from both sides and compare their lookup maps.
    std::vector<std::uint64_t> kmers;
    for (const auto& m : whole) kmers.push_back(m.kmer());
    REQUIRE(lookup_map(assembled, kmers) == lookup_map(baseline, kmers));
}

TEST_CASE("exact_simd fragmented-index build preserves scalar segment lookups",
          "[component:bioseq][layer:unit][feature:index][resource:highway]") {
    constexpr std::size_t fragment_units = 2;
    // fragment persistence owns fixed 32 KiBP logical fragments independently of
    // the much smaller source-scanner fragments used to stress lazy reads.
    constexpr std::size_t frag_bp = 32'768u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type =
        bioseq::indexing::minimizers_per_fragment_index<fake_fragment_storage<>>;

    auto data = make_dna4(4099, 0xE2AC751DULL);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag = static_cast<std::size_t>(
        (total_bp + frag_bp - 1) / frag_bp);

    fake_fragment_storage<> scalar_storage;
    fake_fragment_storage<> exact_storage;
    lazy_sequence<dna4_u64, chunked> scalar_lazy{
        chunked::from_range(range)};
    lazy_sequence<dna4_u64, chunked> exact_lazy{
        chunked::from_range(range)};
    const auto scalar_view =
        bioseq::storage::make_lazy_sequence_view(scalar_lazy);
    const auto exact_view =
        bioseq::storage::make_lazy_sequence_view(exact_lazy);
    bioseq::indexing::minimizer_fragment_generation_execution_receipt scalar_evidence;
    bioseq::indexing::minimizer_fragment_generation_execution_receipt exact_evidence;
    index_type::build(
        scalar_storage, scalar_view, minimizer_sketch_backend::scalar,
        nullptr,
        &scalar_evidence);
    index_type::build(
        exact_storage, exact_view, minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        &exact_evidence,
        fixed_alphabet_code<::seq::dna4>());
    REQUIRE(scalar_evidence.scalar_core_runs > 0u);
    REQUIRE(scalar_evidence.exact_simd_core_runs == 0u);
    REQUIRE(scalar_evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(scalar_evidence.vulkan_core_runs == 0u);
    REQUIRE(exact_evidence.scalar_core_runs == 0u);
    REQUIRE(exact_evidence.exact_simd_core_runs > 0u);
    REQUIRE(exact_evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(exact_evidence.vulkan_core_runs == 0u);
    REQUIRE(exact_evidence.exact_simd_stats_present);
    REQUIRE(exact_evidence.exact_simd_stats.is_internally_consistent(true));

    bioseq::indexing::minimizer_assembly_scratch scalar_scratch;
    bioseq::indexing::minimizer_assembly_scratch exact_scratch;
    auto scalar = index_type::assemble_segment(
        scalar_storage,
        0,
        nfrag,
        static_cast<bioseq::storage::sequence_size_t>(frag_bp),
        total_bp,
        0,
        total_bp,
        scalar_scratch);
    auto exact = index_type::assemble_segment(
        exact_storage,
        0,
        nfrag,
        static_cast<bioseq::storage::sequence_size_t>(frag_bp),
        total_bp,
        0,
        total_bp,
        exact_scratch);

    auto whole = whole_sequence_minimizers(range, 10, 15);
    std::vector<std::uint64_t> kmers;
    kmers.reserve(whole.size());
    for (const auto& minimizer : whole) kmers.push_back(minimizer.kmer());
    REQUIRE(lookup_map(exact, kmers) == lookup_map(scalar, kmers));
}

struct hash_major_memory_image {
    struct read_event {
        bioseq::indexing::hash_major_minimizer_stream_kind kind{};
        std::uint64_t offset = 0;
        std::size_t byte_count = 0;

        [[nodiscard]] bool operator==(const read_event&) const = default;
    };

    std::array<std::vector<std::byte>, 3> streams{};
    std::vector<bioseq::indexing::hash_major_minimizer_stream_kind> append_order{};
    std::vector<read_event> reads{};
    std::vector<std::vector<
        bioseq::indexing::hash_major_minimizer_authorized_control_range>>
        control_waves{};
    std::vector<std::vector<
        bioseq::indexing::hash_major_minimizer_authorized_posting_range>>
        posting_waves{};
    bioseq::parallelism::memory_admission_controller* cap_controller = nullptr;
    std::optional<std::uint64_t> cap_after_directory_read{};
    std::optional<std::uint64_t> cap_after_control_wave{};
    std::size_t size_calls = 0u;
    bool use_wave_callbacks = true;

    [[nodiscard]] static std::size_t index(
            bioseq::indexing::hash_major_minimizer_stream_kind kind) {
        return static_cast<std::size_t>(kind) - 1u;
    }

    static bool append_callback(
            void* context,
            bioseq::indexing::hash_major_minimizer_stream_kind kind,
            std::span<const std::byte> bytes) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        auto& stream = self.streams[index(kind)];
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        self.append_order.push_back(kind);
        return true;
    }

    static bool read_callback(
            void* context,
            bioseq::indexing::hash_major_minimizer_stream_kind kind,
            std::uint64_t offset,
            std::span<std::byte> output) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        const auto& stream = self.streams[index(kind)];
        if (offset > stream.size() || output.size() > stream.size() - offset) {
            return false;
        }
        std::copy_n(
            stream.begin() + static_cast<std::size_t>(offset),
            output.size(),
            output.begin());
        self.reads.push_back(read_event{
            .kind = kind,
            .offset = offset,
            .byte_count = output.size(),
        });
        if (kind
                == bioseq::indexing::hash_major_minimizer_stream_kind::directory
            && self.cap_controller != nullptr
            && self.cap_after_directory_read) {
            self.cap_controller->set_index_memory_cap_bytes(
                *self.cap_after_directory_read);
        }
        return true;
    }

    static std::uint64_t size_callback(
            void* context,
            bioseq::indexing::hash_major_minimizer_stream_kind kind) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        ++self.size_calls;
        return self.streams[index(kind)].size();
    }

    static bool control_wave_callback(
            void* context,
            std::span<const
                bioseq::indexing::hash_major_minimizer_authorized_control_range>
                ranges,
            void* consumer_context,
            bioseq::indexing::hash_major_minimizer_consume_control_block_fn consume) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        self.control_waves.emplace_back(ranges.begin(), ranges.end());
        const auto& stream = self.streams[index(
            bioseq::indexing::hash_major_minimizer_stream_kind::controls)];
        for (std::size_t ordinal = 0u; ordinal < ranges.size(); ++ordinal) {
            const auto& range = ranges[ordinal];
            if (range.control_byte_offset > stream.size()
                || range.byte_count
                    > stream.size() - range.control_byte_offset) {
                return false;
            }
            const auto bytes = std::span<const std::byte>{stream}.subspan(
                static_cast<std::size_t>(range.control_byte_offset),
                range.byte_count);
            self.reads.push_back(read_event{
                .kind = bioseq::indexing::
                    hash_major_minimizer_stream_kind::controls,
                .offset = range.control_byte_offset,
                .byte_count = range.byte_count,
            });
            if (!consume(
                    consumer_context,
                    static_cast<std::uint32_t>(ordinal),
                    bytes)) {
                return false;
            }
        }
        if (self.cap_controller != nullptr
            && self.cap_after_control_wave) {
            self.cap_controller->set_index_memory_cap_bytes(
                *self.cap_after_control_wave);
        }
        return true;
    }

    static bool posting_wave_callback(
            void* context,
            std::span<const
                bioseq::indexing::hash_major_minimizer_authorized_posting_range>
                ranges,
            void* consumer_context,
            bioseq::indexing::hash_major_minimizer_consume_posting_bytes_fn consume) {
        auto& self = *static_cast<hash_major_memory_image*>(context);
        self.posting_waves.emplace_back(ranges.begin(), ranges.end());
        const auto& stream = self.streams[index(
            bioseq::indexing::hash_major_minimizer_stream_kind::postings)];
        for (std::size_t ordinal = 0u; ordinal < ranges.size(); ++ordinal) {
            const auto& range = ranges[ordinal];
            if (range.posting_byte_offset > stream.size()
                || range.byte_count
                    > stream.size() - range.posting_byte_offset) {
                return false;
            }
            std::uint64_t relative = 0u;
            while (relative < range.byte_count) {
                const std::size_t byte_count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        bioseq::indexing::k_hash_major_minimizer_read_slab_bytes,
                        range.byte_count - relative));
                const std::uint64_t absolute =
                    range.posting_byte_offset + relative;
                const auto bytes = std::span<const std::byte>{stream}.subspan(
                    static_cast<std::size_t>(absolute), byte_count);
                self.reads.push_back(read_event{
                    .kind = bioseq::indexing::
                        hash_major_minimizer_stream_kind::postings,
                    .offset = absolute,
                    .byte_count = byte_count,
                });
                if (!consume(
                        consumer_context,
                        static_cast<std::uint32_t>(ordinal),
                        relative,
                        bytes)) {
                    return false;
                }
                relative += byte_count;
            }
        }
        return true;
    }

    [[nodiscard]] bioseq::indexing::hash_major_minimizer_logical_stream_sink_ref
    sink() {
        return {
            .context = this,
            .append_bytes = &append_callback,
        };
    }

    [[nodiscard]] bioseq::indexing::hash_major_minimizer_logical_stream_source_ref
    source() {
        return {
            .context = this,
            .read_bytes = &read_callback,
            .logical_size = &size_callback,
            .read_control_wave = use_wave_callbacks
                ? &control_wave_callback : nullptr,
            .read_posting_wave = use_wave_callbacks
                ? &posting_wave_callback : nullptr,
        };
    }

    [[nodiscard]] std::vector<read_event> reads_of(
            bioseq::indexing::hash_major_minimizer_stream_kind kind) const {
        std::vector<read_event> selected;
        std::ranges::copy_if(
            reads, std::back_inserter(selected),
            [kind](const read_event& event) { return event.kind == kind; });
        return selected;
    }
};

[[nodiscard]] bioseq::indexing::hash_major_minimizer_generation_options
hash_major_test_options(std::uint64_t target_length) {
    return bioseq::indexing::hash_major_minimizer_generation_options{
        .generation_id = 91u,
        .query_key = 17u,
        .source_epoch = 23u,
        .source_content_crc64 = 0x0123456789abcdefULL,
        .target_length = target_length,
        .storage_profile = bioseq::indexing::hash_major_minimizer_storage_profile{
            .hash_scheme_identity = 0u,
            .hash_scheme_revision = 0u,
            .alphabet_code = 0u,
            .kmer_size = 15u,
            .window_size = 10u,
            .hash_width_bytes = 4u,
            .stored_posting_width_bytes =
                static_cast<std::uint8_t>(
                    target_length <= (std::uint64_t{1} << 31u) ? 4u : 8u),
            .position_encoding_revision = 1u,
            .hash_semantics_revision = 1u,
            .count_semantics_revision = 1u,
            .producer_backend_identity = 1u,
            .producer_backend_revision = 1u,
            .controls_per_block =
                bioseq::indexing::k_hash_major_minimizer_controls_per_block,
            .hash_seed = 0u,
        },
    };
}

[[nodiscard]] bioseq::indexing::expected_hash_major_minimizer_generation
hash_major_expected(
        const bioseq::indexing::hash_major_minimizer_generation_options& options) {
    return {
        .generation_id = options.generation_id,
        .query_key = options.query_key,
        .source_epoch = options.source_epoch,
        .source_content_crc64 = options.source_content_crc64,
        .target_length = options.target_length,
        .storage_profile = options.storage_profile,
    };
}

[[nodiscard]] bioseq::indexing::canonical_query_hash_view hash_major_query_view(
        std::span<const std::uint32_t> hashes,
        const bioseq::indexing::hash_major_minimizer_storage_profile& storage_profile) {
    return {
        .values32 = hashes,
        .width_bytes = 4u,
        .storage_width_bytes = 4u,
        .storage_profile_digest =
            bioseq::indexing::hash_major_minimizer_storage_profile_digest(
                storage_profile),
    };
}

[[nodiscard]] std::string hash_major_hex(std::span<const std::byte> bytes) {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string output;
    output.resize(bytes.size() * 2u);
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[index]);
        output[index * 2u] = digits[value >> 4u];
        output[index * 2u + 1u] = digits[value & 0x0fu];
    }
    return output;
}

void recompute_hash_major_control_crc(std::vector<std::byte>& bytes) {
    using bioseq::indexing::crc64_ecma182_accumulator;
    REQUIRE(bytes.size()
        >= bioseq::indexing::k_hash_major_minimizer_control_header_bytes);
    crc64_ecma182_accumulator crc;
    crc.update(std::span<const std::byte>{bytes}.first(48u));
    crc.update(std::span<const std::byte>{bytes}.subspan(
        bioseq::indexing::k_hash_major_minimizer_control_header_bytes));
    test_write_le<std::uint64_t>(std::span<std::byte>{bytes}, 48u,
        crc.checksum());
}

struct hash_major_fixture {
    hash_major_memory_image image{};
    bioseq::indexing::hash_major_minimizer_generation_options options{};
    bioseq::indexing::hash_major_minimizer_encoded_metadata metadata{};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{};
};

[[nodiscard]] hash_major_fixture make_hash_major_small_fixture() {
    using namespace bioseq::indexing;
    hash_major_fixture fixture;
    fixture.options = hash_major_test_options(100u);
    hash_major_minimizer_logical_encoder encoder{
        fixture.options, fixture.image.sink()};
    const std::array<std::uint64_t, 2> first{2u, 4u};
    const std::array<std::uint64_t, 1> middle{10u};
    const std::array<std::uint64_t, 2> last{20u, 21u};
    REQUIRE(encoder.append_hash(10u, first).has_value());
    REQUIRE(encoder.append_hash(20u, middle).has_value());
    REQUIRE(encoder.append_hash(30u, last).has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    fixture.metadata = *metadata;
    fixture.pairs = {
        {10u, 2u}, {10u, 4u},
        {20u, 10u},
        {30u, 20u}, {30u, 21u},
    };
    return fixture;
}

TEST_CASE("hash-major exact wire codecs preserve the grouped-SoA layout",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(minimizer_generation_head::encoded_bytes == 64u);
    STATIC_REQUIRE(minimizer_generation_manifest::encoded_bytes == 160u);
    STATIC_REQUIRE(k_hash_major_minimizer_head_bytes == 224u);
    STATIC_REQUIRE(k_hash_major_minimizer_manifest_bytes == 336u);
    STATIC_REQUIRE(k_hash_major_minimizer_directory_entry_bytes == 80u);
    STATIC_REQUIRE(sizeof(exact_global_count_exception) == 16u);
    STATIC_REQUIRE(sizeof(posting_request) == 24u);
    STATIC_REQUIRE(sizeof(hash_major_minimizer_authorized_control_range) == 24u);
    STATIC_REQUIRE(sizeof(hash_major_minimizer_authorized_posting_range) == 48u);

    auto fixture = make_hash_major_small_fixture();
    REQUIRE(fixture.metadata.encoded_head.size() == 224u);
    REQUIRE(fixture.metadata.encoded_manifest.size() == 336u);
    REQUIRE(fixture.image.append_order
        == std::vector<hash_major_minimizer_stream_kind>{
            hash_major_minimizer_stream_kind::postings,
            hash_major_minimizer_stream_kind::controls,
            hash_major_minimizer_stream_kind::directory});
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_head[0]) == 'B');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_head[6]) == '6');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_manifest[0]) == 'B');
    REQUIRE(std::to_integer<char>(fixture.metadata.encoded_manifest[6]) == '6');

    const auto decoded_head = decode_hash_major_minimizer_head(
        fixture.metadata.encoded_head);
    const auto decoded_manifest = decode_hash_major_minimizer_manifest(
        fixture.metadata.encoded_manifest);
    REQUIRE(decoded_head.has_value());
    REQUIRE(decoded_manifest.has_value());
    REQUIRE(*decoded_head == fixture.metadata.head);
    REQUIRE(*decoded_manifest == fixture.metadata.manifest);
    const std::array<std::byte, 16> expected_source_identity{
        std::byte{0x9e}, std::byte{0xe7}, std::byte{0x39}, std::byte{0xaa},
        std::byte{0x80}, std::byte{0xf7}, std::byte{0x43}, std::byte{0xdc},
        std::byte{0x1f}, std::byte{0xf3}, std::byte{0x35}, std::byte{0x3e},
        std::byte{0xa5}, std::byte{0x04}, std::byte{0xe3}, std::byte{0xaa},
    };
    const std::array<std::byte, 32> expected_storage_profile_digest{
        std::byte{0x18}, std::byte{0xdb}, std::byte{0x2e}, std::byte{0x20},
        std::byte{0x33}, std::byte{0x2c}, std::byte{0xeb}, std::byte{0x74},
        std::byte{0xbe}, std::byte{0xb2}, std::byte{0x4d}, std::byte{0xda},
        std::byte{0x8e}, std::byte{0xf6}, std::byte{0xa5}, std::byte{0xd2},
        std::byte{0x82}, std::byte{0x17}, std::byte{0x34}, std::byte{0x18},
        std::byte{0x50}, std::byte{0xbe}, std::byte{0x23}, std::byte{0x94},
        std::byte{0x7f}, std::byte{0xd8}, std::byte{0x46}, std::byte{0x35},
        std::byte{0x26}, std::byte{0x2e}, std::byte{0xa1}, std::byte{0xfb},
    };
    REQUIRE(decoded_head->source_identity == expected_source_identity);
    REQUIRE(decoded_head->storage_profile_digest
        == expected_storage_profile_digest);
    REQUIRE(decoded_manifest->unique_hash_count == 3u);
    REQUIRE(decoded_manifest->total_posting_count == 5u);
    REQUIRE(decoded_manifest->streams[0].logical_bytes == 80u);
    REQUIRE(decoded_manifest->streams[2].logical_bytes == 20u);
    REQUIRE(decoded_manifest->streams[0].first_row_ordinal == 0u);
    REQUIRE(decoded_manifest->streams[1].first_row_ordinal == 1u);
    REQUIRE(decoded_manifest->streams[2].first_row_ordinal == 2u);
    REQUIRE(decoded_manifest->streams[0].stream_crc64
        == 0xd9cbbef0ecb8f6a4ULL);
    REQUIRE(decoded_manifest->streams[1].stream_crc64
        == 0xbfd39c3c3e488c19ULL);
    REQUIRE(decoded_manifest->streams[2].stream_crc64
        == 0x5cf59f98c6058b27ULL);
    REQUIRE(decoded_manifest->generation_crc64
        == 0xedd9cd57f12d5423ULL);
    REQUIRE(decoded_head->manifest_payload_crc64
        == 0xe8bab5bdeba98b87ULL);
    REQUIRE(decoded_head->manifest_address_bound_row_crc64
        == 0x8fdbf93d04832856ULL);
    REQUIRE(decoded_head->head_crc64 == 0xdc5d0729f8d20fc8ULL);

    // These are independent full-byte wire images, not values round-tripped
    // through the implementation under test. They pin every reserved byte,
    // descriptor ordinal, checksum field, and packed control tail.
    REQUIRE(hash_major_hex(fixture.metadata.encoded_head)
        == "42534551474836000600e000000000005b000000000000001100000000000000"
           "17000000000000009ee739aa80f743dc1ff3353ea504e3aaefcdab8967452301"
           "18db2e20332ceb74beb24dda8ef6a5d28217341850be23947fd84635262ea1fb"
           "0000000000000000000000000000000000000000000000005001000001000000"
           "562883043df9db8f878ba9ebbdb5bae8c80fd2f829075ddc0000000000000000"
           "0000000000000000000000000000000000000000000000000000000000000000"
           "0000000000000000000000000000000000000000000000000000000000000000");
    REQUIRE(hash_major_hex(fixture.metadata.encoded_manifest)
        == "425345514e4c36000600c000000000005b000000000000001700000000000000"
           "9ee739aa80f743dc1ff3353ea504e3aaefcdab89674523016400000000000000"
           "0300000000000000050000000000000000000000000000000f000a0004040003"
           "0100010001000100010000000010000018db2e20332ceb74beb24dda8ef6a5d2"
           "8217341850be23947fd84635262ea1fba4f6b8ecf0becbd923542df157cdd9ed"
           "4800000000000000140000000000000001000000000000000000000000000000"
           "0100010050000000010000000000000050000000000000000100000000000000"
           "a4f6b8ecf0becbd9000000000000000002000100000000000300000000000000"
           "48000000000000000100000000000000198c483e3c9cd3bf0100000000000000"
           "0300010004000000050000000000000014000000000000000100000000000000"
           "278b05c6989ff55c0200000000000000");
    REQUIRE(hash_major_hex(fixture.image.streams[0])
        == "0a000000000000001e0000000000000000000000000000000000000000000000"
           "4800000003000000000000000000000000000000000000001400000000000000"
           "050000000000000099128aad8d85ba2d");
    REQUIRE(hash_major_hex(fixture.image.streams[1])
        == "0a000000000000000300000005100000050000000000000000000000000000"
           "0002000000060000000000000000000000dc72c00206f5e8fd278b05c6989f"
           "f55c4051020001000200");
    REQUIRE(hash_major_hex(fixture.image.streams[2])
        == "02000000040000000a0000001400000015000000");

    SECTION("borrowed canonical lower_bound is hard-bounded by its radix") {
        const std::array<std::uint32_t, 5> hashes{
            1u, 2u, 0x0001'0000u, 0x0001'0001u, 0x0002'0000u};
        canonical_query_hash_view view{
            .values32 = std::span<const std::uint32_t>{hashes},
            .width_bytes = 4u,
            .storage_width_bytes = 4u,
        };
        REQUIRE(view.lower_bound(2u, 4u, 0x0002'0000u) == 4u);
        REQUIRE(view.lower_bound(2u, 4u, 0x0001'0001u) == 3u);
    }

    SECTION("head and manifest corruption fail closed") {
        auto bad_head = fixture.metadata.encoded_head;
        bad_head[208] ^= std::byte{1};
        REQUIRE_FALSE(decode_hash_major_minimizer_head(bad_head).has_value());

        auto bad_manifest = fixture.metadata.encoded_manifest;
        bad_manifest[198] ^= std::byte{1};
        REQUIRE_FALSE(
            decode_hash_major_minimizer_manifest(bad_manifest).has_value());
    }
}

TEST_CASE("hash-major control codec pins zero delta and exact count exceptions",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::array<std::uint64_t, 1> hashes{42u};
    const std::array<std::uint64_t, 1> counts{65'535u};
    auto encoded = encode_hash_major_minimizer_control_block(
        hashes, counts, 0u, 0x9988776655443322ULL, 4u, 4u);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() == 64u + 2u + 12u);
    auto decoded = decode_hash_major_minimizer_control_block(*encoded, 4u, 4u);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->header.hash_delta_bit_width == 0u);
    REQUIRE(decoded->header.hash_bytes == 0u);
    REQUIRE(decoded->header.count_bytes == 2u);
    REQUIRE(decoded->header.exception_bytes == 12u);
    const std::array<std::byte, 8> expected_control_crc_le{
        std::byte{0x4b}, std::byte{0xac}, std::byte{0x08}, std::byte{0xa1},
        std::byte{0x07}, std::byte{0x6a}, std::byte{0x71}, std::byte{0x29},
    };
    REQUIRE(std::ranges::equal(
        std::span<const std::byte>{*encoded}.subspan(48u, 8u),
        expected_control_crc_le));
    REQUIRE(decoded->hashes == std::vector<std::uint64_t>{42u});
    REQUIRE(decoded->exact_counts == std::vector<std::uint64_t>{65'535u});

    SECTION("payload checksum") {
        auto corrupt = *encoded;
        corrupt[65] ^= std::byte{1};
        REQUIRE_FALSE(
            decode_hash_major_minimizer_control_block(corrupt, 4u, 4u)
                .has_value());
    }

    SECTION("noncanonical delta width") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{1u, 1u};
        auto canonical = encode_hash_major_minimizer_control_block(
            two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());
        auto corrupt = *canonical;
        REQUIRE(std::to_integer<std::uint8_t>(corrupt[12]) == 1u);
        corrupt[12] = std::byte{2};
        corrupt[64] = std::byte{4};
        recompute_hash_major_control_crc(corrupt);
        const auto rejected = decode_hash_major_minimizer_control_block(
            corrupt, 4u, 4u);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error() == hash_major_minimizer_error::control);
    }

    SECTION("reserved flags and packed tail bits") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{1u, 1u};
        auto canonical = encode_hash_major_minimizer_control_block(
            two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());

        auto reserved = *canonical;
        reserved[14] = std::byte{1};
        recompute_hash_major_control_crc(reserved);
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(
            reserved, 4u, 4u));

        auto tail = *canonical;
        tail[64] |= std::byte{0x80};
        recompute_hash_major_control_crc(tail);
        const auto rejected = decode_hash_major_minimizer_control_block(
            tail, 4u, 4u);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error()
            == hash_major_minimizer_error::reserved_nonzero);
    }

    SECTION("exception records are exact, ordered, and exhaustive") {
        const std::array<std::uint64_t, 2> two_hashes{42u, 43u};
        const std::array<std::uint64_t, 2> two_counts{65'535u, 65'536u};
        auto canonical = encode_hash_major_minimizer_control_block(
            two_hashes, two_counts, 0u, 0u, 4u, 4u);
        REQUIRE(canonical.has_value());
        REQUIRE(canonical->size() == 64u + 1u + 4u + 24u);

        auto duplicate = *canonical;
        test_write_le<std::uint16_t>(
            std::span<std::byte>{duplicate}, 81u, 0u);
        recompute_hash_major_control_crc(duplicate);
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(
            duplicate, 4u, 4u));

        auto missing = *canonical;
        missing.resize(missing.size() - 12u);
        test_write_le<std::uint32_t>(
            std::span<std::byte>{missing}, 40u, 12u);
        recompute_hash_major_control_crc(missing);
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(
            missing, 4u, 4u));

        auto trailing = *canonical;
        trailing.push_back(std::byte{0});
        REQUIRE_FALSE(decode_hash_major_minimizer_control_block(
            trailing, 4u, 4u));
    }
}

TEST_CASE("grouped-SoA and hash-major tagged dispatch is exact-length only",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    auto fixture = make_hash_major_small_fixture();
    const auto tagged_hash_major_head = decode_tagged_minimizer_generation_head(
        fixture.metadata.encoded_head);
    const auto tagged_hash_major_manifest =
        decode_tagged_minimizer_generation_manifest(
            fixture.metadata.encoded_manifest);
    REQUIRE(tagged_hash_major_head.has_value());
    REQUIRE(tagged_hash_major_head->index() == 1u);
    REQUIRE(tagged_hash_major_manifest.has_value());
    REQUIRE(tagged_hash_major_manifest->index() == 1u);

    const auto shape = derive_grouped_soa_minimizer_profile(
        k_minimap_scheme_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        15u,
        8u,
        10u);
    REQUIRE(shape.has_value());
    auto grouped_soa_head = golden_generation_head();
    grouped_soa_head.layout = minimizer_generation_layout::
        grouped_soa_segment_local;
    const auto encoded_grouped_soa_head = encode_minimizer_generation_head(grouped_soa_head);
    REQUIRE(encoded_grouped_soa_head.has_value());
    auto grouped_soa_manifest = golden_generation_manifest();
    grouped_soa_manifest.layout = minimizer_generation_layout::
        grouped_soa_segment_local;
    grouped_soa_manifest.alphabet_code = k_minimizer_persisted_dna4_alphabet_code;
    grouped_soa_manifest.kmer_size = 15u;
    grouped_soa_manifest.gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
    assign_grouped_soa_profile(grouped_soa_manifest, *shape);
    const auto encoded_grouped_soa_manifest =
        encode_minimizer_generation_manifest(grouped_soa_manifest);
    REQUIRE(encoded_grouped_soa_manifest.has_value());
    const auto tagged_grouped_soa_head = decode_tagged_minimizer_generation_head(
        *encoded_grouped_soa_head);
    const auto tagged_grouped_soa_manifest =
        decode_tagged_minimizer_generation_manifest(*encoded_grouped_soa_manifest);
    REQUIRE(tagged_grouped_soa_head.has_value());
    REQUIRE(tagged_grouped_soa_head->index() == 0u);
    REQUIRE(tagged_grouped_soa_manifest.has_value());
    REQUIRE(tagged_grouped_soa_manifest->index() == 0u);

    for (const std::size_t wrong_size :
         {128u, 159u, 161u, 224u, 335u, 337u}) {
        INFO("wrong tagged manifest size " << wrong_size);
        std::vector<std::byte> bytes(wrong_size);
        std::copy_n(
            fixture.metadata.encoded_manifest.begin(),
            std::min(wrong_size, fixture.metadata.encoded_manifest.size()),
            bytes.begin());
        const auto decoded = decode_tagged_minimizer_generation_manifest(
            bytes);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error()
            == tagged_minimizer_generation_encoding_error::encoded_size);
    }
    for (const std::size_t wrong_size : {63u, 65u, 223u, 225u}) {
        INFO("wrong tagged head size " << wrong_size);
        std::vector<std::byte> bytes(wrong_size);
        std::copy_n(
            fixture.metadata.encoded_head.begin(),
            std::min(wrong_size, fixture.metadata.encoded_head.size()),
            bytes.begin());
        const auto decoded = decode_tagged_minimizer_generation_head(bytes);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error()
            == tagged_minimizer_generation_encoding_error::encoded_size);
    }

    std::array<std::byte, k_hash_major_minimizer_head_bytes>
        padded_grouped_soa_head{};
    std::copy(encoded_grouped_soa_head->begin(), encoded_grouped_soa_head->end(),
        padded_grouped_soa_head.begin());
    const auto rejected_padded_head =
        decode_tagged_minimizer_generation_head(padded_grouped_soa_head);
    REQUIRE_FALSE(rejected_padded_head.has_value());
    REQUIRE(rejected_padded_head.error()
        == tagged_minimizer_generation_encoding_error::hash_major);
    std::array<std::byte, k_hash_major_minimizer_manifest_bytes>
        padded_grouped_soa_manifest{};
    std::copy(encoded_grouped_soa_manifest->begin(), encoded_grouped_soa_manifest->end(),
        padded_grouped_soa_manifest.begin());
    const auto rejected_padded_manifest =
        decode_tagged_minimizer_generation_manifest(padded_grouped_soa_manifest);
    REQUIRE_FALSE(rejected_padded_manifest.has_value());
    REQUIRE(rejected_padded_manifest.error()
        == tagged_minimizer_generation_encoding_error::hash_major);

}

TEST_CASE("hash-major selective reader requests only authorized postings and matches baseline",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    auto fixture = make_hash_major_small_fixture();
    const std::array<std::uint32_t, 3> queries{10u, 30u, 999u};
    auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        fixture.metadata.encoded_head,
        fixture.metadata.encoded_manifest,
        hash_major_expected(fixture.options),
        fixture.image.source(),
        hash_major_query_view(queries, fixture.options.storage_profile),
        2u);
    REQUIRE(reader.has_value());
    REQUIRE(reader->target_length() == 100u);
    REQUIRE(fixture.image.posting_waves.size() == 1u);
    REQUIRE(fixture.image.posting_waves[0].size() == 2u);
    REQUIRE(fixture.image.posting_waves[0][0].control_block_ordinal == 0u);
    REQUIRE(fixture.image.posting_waves[0][0].posting_byte_offset == 0u);
    REQUIRE(fixture.image.posting_waves[0][0].byte_count == 8u);
    REQUIRE(fixture.image.posting_waves[0][1].control_block_ordinal == 0u);
    REQUIRE(fixture.image.posting_waves[0][1].posting_byte_offset == 12u);
    REQUIRE(fixture.image.posting_waves[0][1].byte_count == 8u);
    REQUIRE(fixture.image.control_waves.size() == 1u);
    REQUIRE(reader->posting_wave_peak_owner_bytes()
        <= k_hash_major_minimizer_read_slab_bytes);
    REQUIRE(reader->control_wave_peak_owner_bytes()
        <= k_hash_major_minimizer_control_read_wave_blocks
            * sizeof(hash_major_minimizer_authorized_control_range));
    REQUIRE(reader->transient_authority_retained_bytes() == 0u);
    const auto posting_reads = fixture.image.reads_of(
        hash_major_minimizer_stream_kind::postings);
    REQUIRE(posting_reads.size() == 2u);
    REQUIRE(posting_reads[0].offset == 0u);
    REQUIRE(posting_reads[0].byte_count == 8u);
    REQUIRE(posting_reads[1].offset == 12u);
    REQUIRE(posting_reads[1].byte_count == 8u);

    minimizer_hashmap_fragment_index baseline;
    auto baseline_pairs = fixture.pairs;
    baseline.build_from_pairs(
        std::span<std::pair<std::uint64_t, std::uint64_t>>{baseline_pairs},
        100u);
    for (const std::uint64_t hash : std::array{10u, 30u, 999u}) {
        const auto spans = reader->lookup_spans(hash);
        REQUIRE(spans.positions_complete());
        REQUIRE(spans.well_formed());
        const auto expected = baseline.lookup(hash);
        REQUIRE(std::ranges::equal(
            spans.contiguous_positions32(), expected));
    }
    REQUIRE_THROWS_AS(
        reader->lookup_spans(20u), persisted_minimizer_lookup_error);

    SECTION("scalar logical-read fallback preserves the same authority") {
        auto scalar_fixture = make_hash_major_small_fixture();
        scalar_fixture.image.use_wave_callbacks = false;
        const auto scalar_reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            scalar_fixture.metadata.encoded_head,
            scalar_fixture.metadata.encoded_manifest,
            hash_major_expected(scalar_fixture.options),
            scalar_fixture.image.source(),
            hash_major_query_view(queries,
                scalar_fixture.options.storage_profile),
            2u);
        REQUIRE(scalar_reader.has_value());
        REQUIRE(scalar_fixture.image.control_waves.empty());
        REQUIRE(scalar_fixture.image.posting_waves.empty());
        REQUIRE(
            scalar_reader->memory_stats().
                generic_source_fallback_calls > 0u);
        REQUIRE(std::ranges::equal(
            scalar_reader->lookup_spans(10u).contiguous_positions32(),
            std::array<std::uint32_t, 2>{2u, 4u}));
        REQUIRE(std::ranges::equal(
            scalar_reader->lookup_spans(30u).contiguous_positions32(),
            std::array<std::uint32_t, 2>{20u, 21u}));
    }
}

TEST_CASE("hash-major applies max_occ before posting IO with exact heavy count",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    hash_major_memory_image image;
    constexpr std::uint64_t count = 65'535u;
    const auto options = hash_major_test_options(70'000u);
    hash_major_minimizer_logical_encoder encoder{
        options, image.sink()};
    REQUIRE(encoder.begin_hash(42u, count).has_value());
    for (std::uint64_t bp = 0u; bp < count; ++bp) {
        const auto appended = encoder.append_posting(bp << 1u);
        if (!appended) {
            FAIL("hash-major streaming posting append failed at " << bp);
        }
    }
    REQUIRE(encoder.end_hash().has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    const std::array<std::uint32_t, 1> query{42u};
    auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        hash_major_expected(options),
        image.source(),
        hash_major_query_view(query, options.storage_profile),
        10u);
    REQUIRE(reader.has_value());
    const auto spans = reader->lookup_spans(42u);
    REQUIRE_FALSE(spans.positions_complete());
    REQUIRE(spans.global_occurrence_count() == count);
    REQUIRE(spans.well_formed());
    REQUIRE(reader->exact_count_exceptions().size() == 1u);
    REQUIRE(image.reads_of(hash_major_minimizer_stream_kind::postings).empty());
}

TEST_CASE("hash-major admission is exact at every pre-I/O barrier",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::array<std::uint32_t, 3> queries{10u, 20u, 30u};
    hash_major_minimizer_memory_stats evidence{};
    {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        const auto reader = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE(reader.has_value());
        evidence = reader->memory_stats();
        REQUIRE(evidence.directory_stage_peak_admitted_bytes != 0u);
        REQUIRE(evidence.control_stage_peak_admitted_bytes != 0u);
        REQUIRE(evidence.posting_stage_peak_admitted_bytes != 0u);
        REQUIRE(evidence.peak_admitted_bytes == std::max({
            evidence.directory_stage_peak_admitted_bytes,
            evidence.control_stage_peak_admitted_bytes,
            evidence.posting_stage_peak_admitted_bytes,
        }));
        REQUIRE(evidence.retained_owner_bytes == reader->admitted_bytes());
        REQUIRE(evidence.directory_stream_crc_receipts == 1u);
        REQUIRE(evidence.control_block_crc_receipts
                == evidence.selected_block_count);
        REQUIRE(evidence.control_semantic_receipts
                == evidence.selected_block_count);
        REQUIRE(evidence.posting_ranges_delivered
                == evidence.posting_semantic_receipts);
        REQUIRE(evidence.posting_range_crc_receipts
                <= evidence.posting_ranges_delivered);
        REQUIRE(evidence.generic_source_fallback_calls == 0u);
        REQUIRE(admission.reserved_bytes() == reader->admitted_bytes());
    }

    SECTION("directory peak minus one denies before source size or read") {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(
            evidence.directory_stage_peak_admitted_bytes - 1u);
        const auto reader = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::admission);
        REQUIRE(fixture.image.size_calls == 0u);
        REQUIRE(fixture.image.reads.empty());
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("control peak minus one denies before a control callback") {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(
            evidence.directory_stage_peak_admitted_bytes);
        fixture.image.cap_controller = &admission;
        fixture.image.cap_after_directory_read =
            evidence.control_stage_peak_admitted_bytes - 1u;
        const auto reader = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::admission);
        REQUIRE(fixture.image.size_calls == 3u);
        REQUIRE(fixture.image.reads_of(
            hash_major_minimizer_stream_kind::directory).size() == 1u);
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("posting peak minus one denies before a posting callback") {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(
            evidence.peak_admitted_bytes);
        fixture.image.cap_controller = &admission;
        fixture.image.cap_after_control_wave =
            evidence.posting_stage_peak_admitted_bytes - 1u;
        const auto reader = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::admission);
        REQUIRE_FALSE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.reads_of(
            hash_major_minimizer_stream_kind::postings).empty());
        REQUIRE(fixture.image.posting_waves.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move construction transfers the sole retained permit") {
        auto fixture = make_hash_major_small_fixture();
        bioseq::parallelism::memory_admission_controller admission;
        auto made = hash_major_minimizer_selective_reader::open(
            admission,
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(queries, fixture.options.storage_profile),
            2u);
        REQUIRE(made.has_value());
        const std::uint64_t retained_bytes = made->admitted_bytes();
        hash_major_minimizer_selective_reader moved{std::move(*made)};
        REQUIRE(made->admitted_bytes() == 0u);
        REQUIRE(made->memory_stats().retained_owner_bytes == 0u);
        REQUIRE(moved.admitted_bytes() == retained_bytes);
        REQUIRE(admission.reserved_bytes() == retained_bytes);
        REQUIRE(std::ranges::equal(
            moved.lookup_spans(10u).contiguous_positions32(),
            std::array<std::uint32_t, 2>{2u, 4u}));
    }
}

TEST_CASE("hash-major authenticates every caller expectation before stream IO",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    auto fixture = make_hash_major_small_fixture();
    const std::array<std::uint32_t, 1> query{10u};
    std::vector<expected_hash_major_minimizer_generation> mismatches;

    auto mismatch = hash_major_expected(fixture.options);
    ++mismatch.generation_id;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    ++mismatch.query_key;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    ++mismatch.source_epoch;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    mismatch.source_content_crc64 ^= 1u;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    ++mismatch.target_length;
    mismatches.push_back(mismatch);
    mismatch = hash_major_expected(fixture.options);
    mismatch.storage_profile.producer_backend_identity = 2u;
    mismatches.push_back(mismatch);

    for (std::size_t index = 0u; index < mismatches.size(); ++index) {
        INFO("expected-generation mismatch ordinal " << index);
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            mismatches[index],
            fixture.image.source(),
            hash_major_query_view(query, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::expected_generation);
        REQUIRE(fixture.image.reads.empty());
        REQUIRE(fixture.image.control_waves.empty());
        REQUIRE(fixture.image.posting_waves.empty());
    }
}

TEST_CASE("hash-major authenticates fully selected blocks and whole streams",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::array<std::uint32_t, 3> all_queries{10u, 20u, 30u};

    SECTION("full selected posting block CRC") {
        auto fixture = make_hash_major_small_fixture();
        auto& postings = fixture.image.streams[
            hash_major_memory_image::index(hash_major_minimizer_stream_kind::postings)];
        test_write_le<std::uint32_t>(
            std::span<std::byte>{postings}, 4u, 6u);
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(all_queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::checksum);
    }

    for (const std::size_t stream_index : {1u, 2u}) {
        INFO("whole-stream CRC descriptor ordinal " << stream_index);
        auto fixture = make_hash_major_small_fixture();
        auto manifest = fixture.metadata.manifest;
        manifest.streams[stream_index].stream_crc64 ^= 1u;
        const auto encoded_manifest = encode_hash_major_minimizer_manifest(
            manifest);
        REQUIRE(encoded_manifest.has_value());
        auto head = fixture.metadata.head;
        head.manifest_payload_crc64 = crc64_ecma182(*encoded_manifest);
        head.manifest_address_bound_row_crc64 = crc64_index_chunk_row(
            head.generation_id,
            0,
            0,
            0u,
            static_cast<std::uint32_t>(encoded_manifest->size()),
            *encoded_manifest);
        const auto encoded_head = encode_hash_major_minimizer_head(head);
        REQUIRE(encoded_head.has_value());
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            *encoded_head,
            *encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(all_queries, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::checksum);
    }
}

TEST_CASE("hash-major selected corruption fails while unselected bytes stay unread",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    SECTION("selected duplicate position fails semantic validation") {
        auto fixture = make_hash_major_small_fixture();
        auto& postings = fixture.image.streams[
            hash_major_memory_image::index(hash_major_minimizer_stream_kind::postings)];
        std::copy_n(postings.begin(), 4u, postings.begin() + 4u);
        const std::array<std::uint32_t, 1> query{10u};
        auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(query, fixture.options.storage_profile),
            2u);
        REQUIRE_FALSE(reader.has_value());
        REQUIRE(reader.error() == hash_major_minimizer_error::order);
    }
    SECTION("corrupt non-query posting is not fetched") {
        auto fixture = make_hash_major_small_fixture();
        auto& postings = fixture.image.streams[
            hash_major_memory_image::index(hash_major_minimizer_stream_kind::postings)];
        postings[8] = std::byte{0xff};
        postings[9] = std::byte{0xff};
        postings[10] = std::byte{0xff};
        postings[11] = std::byte{0xff};
        const std::array<std::uint32_t, 2> query{10u, 30u};
        auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(query, fixture.options.storage_profile),
            2u);
        REQUIRE(reader.has_value());
        for (const auto& read : fixture.image.reads_of(
                hash_major_minimizer_stream_kind::postings)) {
            REQUIRE((read.offset + read.byte_count <= 8u
                || read.offset >= 12u));
        }
    }
    SECTION("empty query reads directory but no controls or postings") {
        auto fixture = make_hash_major_small_fixture();
        const std::array<std::uint32_t, 0> query{};
        auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            fixture.metadata.encoded_head,
            fixture.metadata.encoded_manifest,
            hash_major_expected(fixture.options),
            fixture.image.source(),
            hash_major_query_view(query, fixture.options.storage_profile),
            2u);
        REQUIRE(reader.has_value());
        REQUIRE(fixture.image.reads_of(
            hash_major_minimizer_stream_kind::controls).empty());
        REQUIRE(fixture.image.reads_of(
            hash_major_minimizer_stream_kind::postings).empty());
    }
}

TEST_CASE("hash-major widens authorized u32 and preserves required u64 postings",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    const std::uint64_t target_length = (std::uint64_t{1} << 31u) + 10u;
    hash_major_memory_image image;
    const auto options = hash_major_test_options(target_length);
    hash_major_minimizer_logical_encoder encoder{
        options, image.sink()};
    const std::array<std::uint64_t, 2> positions{
        ((std::uint64_t{1} << 31u) << 1u),
        (((std::uint64_t{1} << 31u) + 1u) << 1u) | 1u,
    };
    REQUIRE(encoder.append_hash(7u, positions).has_value());
    auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->manifest.storage_profile.stored_posting_width_bytes == 8u);
    const std::array<std::uint32_t, 1> query{7u};
    auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        hash_major_expected(options),
        image.source(),
        hash_major_query_view(query, options.storage_profile),
        2u);
    REQUIRE(reader.has_value());
    REQUIRE(std::ranges::equal(
        reader->lookup_spans(7u).contiguous_positions(), positions));
}

TEST_CASE("hash-major posting-width and target-length boundaries are exact",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;

    SECTION("2^31 target retains the largest legal u32 posting") {
        constexpr std::uint64_t target_length = std::uint64_t{1} << 31u;
        hash_major_memory_image image;
        const auto options = hash_major_test_options(target_length);
        REQUIRE(options.storage_profile.stored_posting_width_bytes == 4u);
        hash_major_minimizer_logical_encoder encoder{options, image.sink()};
        const std::array<std::uint64_t, 1> positions{
            (std::numeric_limits<std::uint32_t>::max)()};
        REQUIRE(encoder.append_hash(7u, positions).has_value());
        const auto metadata = encoder.finish();
        REQUIRE(metadata.has_value());
        const std::array<std::uint32_t, 1> query{7u};
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            metadata->encoded_head,
            metadata->encoded_manifest,
            hash_major_expected(options),
            image.source(),
            hash_major_query_view(query, options.storage_profile),
            1u);
        REQUIRE(reader.has_value());
        const auto retained =
            reader->lookup_spans(7u).contiguous_positions32();
        REQUIRE(retained.size() == 1u);
        REQUIRE(retained.front() == positions.front());
    }

    SECTION("2^31+1 target requires u64") {
        constexpr std::uint64_t target_length =
            (std::uint64_t{1} << 31u) + 1u;
        const auto options = hash_major_test_options(target_length);
        REQUIRE(options.storage_profile.stored_posting_width_bytes == 8u);
        REQUIRE(is_valid_hash_major_minimizer_storage_profile(
            options.storage_profile, target_length));
    }

    SECTION("2^63 target retains UINT64_MAX and rejects the next length") {
        constexpr std::uint64_t target_length = std::uint64_t{1} << 63u;
        hash_major_memory_image image;
        const auto options = hash_major_test_options(target_length);
        hash_major_minimizer_logical_encoder encoder{options, image.sink()};
        const std::array<std::uint64_t, 1> positions{
            (std::numeric_limits<std::uint64_t>::max)()};
        REQUIRE(encoder.append_hash(7u, positions).has_value());
        const auto metadata = encoder.finish();
        REQUIRE(metadata.has_value());
        const std::array<std::uint32_t, 1> query{7u};
        const auto reader = hash_major_minimizer_selective_reader::open(
            bioseq::parallelism::memory_admission_controller::instance(),
            metadata->encoded_head,
            metadata->encoded_manifest,
            hash_major_expected(options),
            image.source(),
            hash_major_query_view(query, options.storage_profile),
            1u);
        REQUIRE(reader.has_value());
        const auto retained =
            reader->lookup_spans(7u).contiguous_positions();
        REQUIRE(retained.size() == 1u);
        REQUIRE(retained.front() == positions.front());

        constexpr std::uint64_t invalid_target = target_length + 1u;
        const auto invalid_options = hash_major_test_options(invalid_target);
        REQUIRE_FALSE(is_valid_hash_major_minimizer_storage_profile(
            invalid_options.storage_profile, invalid_target));
        hash_major_memory_image rejected_image;
        hash_major_minimizer_logical_encoder rejected_encoder{
            invalid_options, rejected_image.sink()};
        REQUIRE_FALSE(rejected_encoder.begin_hash(7u, 1u).has_value());

        auto invalid_manifest = metadata->encoded_manifest;
        test_write_le<std::uint64_t>(invalid_manifest, 56u, invalid_target);
        const auto decoded = decode_hash_major_minimizer_manifest(
            invalid_manifest);
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error() == hash_major_minimizer_error::stream_descriptor);
    }
}

TEST_CASE("hash-major row ordinals and bounded waves span physical rows exactly",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    constexpr std::uint32_t hash_count =
        k_hash_major_minimizer_controls_per_block + 1u;
    hash_major_memory_image image;
    const auto options = hash_major_test_options(5'000u);
    hash_major_minimizer_logical_encoder encoder{options, image.sink()};
    for (std::uint64_t hash = 1u; hash <= hash_count; ++hash) {
        const std::array<std::uint64_t, 1> position{
            (hash % options.target_length) << 1u};
        REQUIRE(encoder.append_hash(hash, position).has_value());
    }
    const auto metadata = encoder.finish();
    REQUIRE(metadata.has_value());
    REQUIRE(metadata->manifest.directory_entry_count == 2u);
    REQUIRE(image.append_order
        == std::vector<hash_major_minimizer_stream_kind>{
            hash_major_minimizer_stream_kind::postings,
            hash_major_minimizer_stream_kind::postings,
            hash_major_minimizer_stream_kind::postings,
            hash_major_minimizer_stream_kind::controls,
            hash_major_minimizer_stream_kind::directory,
            hash_major_minimizer_stream_kind::postings,
            hash_major_minimizer_stream_kind::controls,
            hash_major_minimizer_stream_kind::directory});
    REQUIRE(metadata->manifest.streams[0].row_count == 1u);
    REQUIRE(metadata->manifest.streams[0].first_row_ordinal == 0u);
    REQUIRE(metadata->manifest.streams[1].row_count == 2u);
    REQUIRE(metadata->manifest.streams[1].first_row_ordinal == 1u);
    REQUIRE(metadata->manifest.streams[2].row_count == 3u);
    REQUIRE(metadata->manifest.streams[2].first_row_ordinal == 3u);

    const std::array<std::uint32_t, 3> queries{
        1u, k_hash_major_minimizer_controls_per_block, hash_count};
    const auto reader = hash_major_minimizer_selective_reader::open(
        bioseq::parallelism::memory_admission_controller::instance(),
        metadata->encoded_head,
        metadata->encoded_manifest,
        hash_major_expected(options),
        image.source(),
        hash_major_query_view(queries, options.storage_profile),
        1u);
    REQUIRE(reader.has_value());
    REQUIRE(image.control_waves.size() == 1u);
    REQUIRE(image.control_waves.front().size() == 2u);
    REQUIRE(reader->posting_wave_peak_owner_bytes()
        <= k_hash_major_minimizer_read_slab_bytes);
    REQUIRE(reader->transient_authority_retained_bytes() == 0u);

    const auto& directory_bytes = image.streams[
        hash_major_memory_image::index(hash_major_minimizer_stream_kind::directory)];
    for (const auto& wave : image.posting_waves) {
        for (const auto& range : wave) {
            REQUIRE(range.control_block_ordinal < 2u);
            const auto entry = decode_hash_major_minimizer_directory_entry(
                std::span<const std::byte>{directory_bytes}.subspan(
                    static_cast<std::size_t>(range.control_block_ordinal)
                        * k_hash_major_minimizer_directory_entry_bytes,
                    k_hash_major_minimizer_directory_entry_bytes));
            REQUIRE(entry.has_value());
            REQUIRE(range.posting_byte_offset >= entry->posting_byte_offset);
            REQUIRE(range.byte_count
                <= entry->posting_byte_offset + entry->posting_byte_length
                    - range.posting_byte_offset);
        }
    }
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
