/**
 * @file test_support.cppm
 * @brief Shared fixtures for minimizer hashmap unit tests.
 */
module;

#include <hwy/highway.h>
#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.unit.indexing.minimizer_hashmap_support;

import std;
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
import bioseq.common.hashing;

import bioseq.tests.support.indexing_storage_fakes;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
export namespace minimizer_hashmap_test_support {

using bioseq::indexing::test::fake_fragment_storage;
using bioseq::storage::chunked_span_sequence_scanner;
using bioseq::storage::lazy_sequence;

/// Packed DNA4 alphabet with 32 symbols per storage unit.
using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
/// A sequence range packed into 64-bit DNA4 storage units.
using range_t = compact_sequence_range<dna4_u64>;
/// Packed DNA5 alphabet used for ambiguous-symbol fixtures.
using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
/// A sequence  range packed into 64-bit DNA5 storage units.
using dna5_range_t = compact_sequence_range<dna5_u64>;

/// Return minimizer kernels for the SIMD target selected by the test build.
[[nodiscard]] const ::bioseq::minimizers::minimizer_kernels* fixed_minimizer_kernels() {
    return &::bioseq::simd_dispatch::highway_test_kernel_table().minimizers;
}

/// Find the alphabet code in the selected SIMD kernel table.
template <sequence_alphabet p_alphabet> [[nodiscard]] std::uint8_t fixed_alphabet_code() {
    const auto& table = ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity =
        table.is_complete() ? table.template resolve_alphabet_identity<p_alphabet>() : nullptr;
    if (identity == nullptr ||
        table.resolve_alphabet_identity(identity->alphabet_code) != identity) {
        throw std::logic_error{"generated test table has no requested alphabet identity"};
    }
    return identity->alphabet_code;
}

/// Return test kernels for exact SIMD and bounded ntHash, or null otherwise.
[[nodiscard]] const ::bioseq::minimizers::minimizer_kernels*
minimizer_kernels_for_backend(const minimizer_sketch_backend backend) {
    return backend == minimizer_sketch_backend::exact_simd ||
                   backend == minimizer_sketch_backend::bounded_nthash_simd
               ? fixed_minimizer_kernels()
               : nullptr;
}

/// Derive the minimap layout used by test manifests.
inline constexpr auto k_test_grouped_soa_profile_result =
    bioseq::indexing::derive_grouped_soa_minimizer_profile(
        bioseq::indexing::k_minimap_scheme_identity,
        bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
        15u,
        32'768u,
        65'534u);
static_assert(k_test_grouped_soa_profile_result.has_value());
/// The minimap layout cvecked by the assertion above.
inline constexpr auto k_test_grouped_soa_profile = *k_test_grouped_soa_profile_result;
/// Derive the bounded ntHash layout used by test manifests.
inline constexpr auto k_test_bounded_grouped_soa_profile_result =
    bioseq::indexing::derive_grouped_soa_minimizer_profile(
        bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_identity,
        bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
        15u,
        32'768u,
        65'534u);
static_assert(k_test_bounded_grouped_soa_profile_result.has_value());
/// The bounded ntHash layout checked by the assertion above.
inline constexpr auto k_test_bounded_grouped_soa_profile =
    *k_test_bounded_grouped_soa_profile_result;

/// Copy  the stored layout and minimizer scheme from a profile into a manifest.
constexpr void
assign_grouped_soa_profile(bioseq::indexing::minimizer_generation_manifest& manifest,
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

static_assert(std::is_standard_layout_v<bioseq::indexing::exact_simd_index_packed_fragment>);
static_assert(std::is_trivially_copyable_v<bioseq::indexing::exact_simd_index_packed_fragment>);
static_assert(std::is_standard_layout_v<bioseq::indexing::exact_simd_index_bridge_request>);
static_assert(std::is_trivially_copyable_v<bioseq::indexing::exact_simd_index_bridge_request>);
static_assert(std::is_standard_layout_v<bioseq::indexing::exact_simd_index_bridge_result>);
static_assert(
    std::is_same_v<decltype(bioseq::indexing::collect_exact_simd_index_erased(
                       std::declval<const bioseq::indexing::exact_simd_index_bridge_request&>())),
                   bioseq::indexing::exact_simd_index_bridge_result>);
static_assert(
    std::is_standard_layout_v<bioseq::indexing::packed_bounded_nthash_index_bridge_request>);
static_assert(
    std::is_trivially_copyable_v<bioseq::indexing::packed_bounded_nthash_index_bridge_request>);
static_assert(
    std::is_standard_layout_v<bioseq::indexing::packed_bounded_nthash_index_bridge_result>);
static_assert(
    std::is_same_v<
        decltype(bioseq::indexing::collect_packed_bounded_nthash_index_erased(
            std::declval<const bioseq::indexing::packed_bounded_nthash_index_bridge_request&>())),
        bioseq::indexing::packed_bounded_nthash_index_bridge_result>);

/// Generate repeatable DNA4 test data from a seed.
[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t symbol_count,
                                               std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::vector<seq::dna4> symbols(symbol_count);
    std::uint64_t state = seed;
    for (std::size_t index = 0; index < symbol_count; ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 symbol;
        symbol.assign_rank(static_cast<decltype(symbol.to_rank())>((state >> 33) & 0x3));
        symbols[index] = symbol;
    }
    return symbols;
}

/// Collect DNA5 reference events with the minimap2 baseline.
[[nodiscard]] std::vector<bioseq::indexing::canonical_minimizer_event> baseline_dna5_events(
    const std::vector<seq::dna5>& data, std::size_t window_size, std::size_t kmer_size) {
    std::string bases;
    bases.reserve(data.size());
    for (const seq::dna5 symbol : data)
        bases.push_back(symbol.to_char());
    const auto entries = ::bioseq::minimap2_sketch_baseline_detail::collect_minimap2_sketch(
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

/// Sketch the whole sequence using positions relative to its start.
template <class p_range>
[[nodiscard]] std::vector<minimap_sketch_data>
whole_sequence_minimizers(p_range& range, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        collector, range, range.begin(), range.end(), w, k);
    return collector.extract_sketches_data();
}

/// Decode one little-endian integer from a fixed-size byte array.
template <class p_uint, std::size_t p_size>
[[nodiscard]] constexpr p_uint test_read_le(const std::array<std::byte, p_size>& bytes,
                                            std::size_t offset) noexcept {
    static_assert(std::is_unsigned_v<p_uint>);
    p_uint value = 0;
    for (std::size_t i = 0; i < sizeof(p_uint); ++i) {
        value |= static_cast<p_uint>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8u);
    }
    return value;
}

/// Decode one little-endian integer from a byte span.
template <class p_uint>
[[nodiscard]] constexpr p_uint test_read_le(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept {
    static_assert(std::is_unsigned_v<p_uint>);
    p_uint value = 0;
    for (std::size_t i = 0; i < sizeof(p_uint); ++i) {
        value |= static_cast<p_uint>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8u);
    }
    return value;
}

/// Encode one little-endian integer into a fixed-size byte array.
template <class p_uint, std::size_t p_size>
constexpr void
test_write_le(std::array<std::byte, p_size>& bytes, std::size_t offset, p_uint value) noexcept {
    static_assert(std::is_unsigned_v<p_uint>);
    for (std::size_t i = 0; i < sizeof(p_uint); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & p_uint{0xff});
        value >>= 8u;
    }
}

/// Encode one little-endian integer into a byte span.
template <class p_uint>
constexpr void
test_write_le(std::span<std::byte> bytes, std::size_t offset, p_uint value) noexcept {
    static_assert(std::is_unsigned_v<p_uint>);
    for (std::size_t i = 0; i < sizeof(p_uint); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & p_uint{0xff});
        value >>= 8u;
    }
}

/// Check that a codec operation fails with the expected error.
template <class p_t>
void require_wire_error(
    const std::expected<p_t, bioseq::indexing::minimizer_generation_encoding_error>& value,
    bioseq::indexing::minimizer_generation_encoding_error expected) {
    REQUIRE_FALSE(value.has_value());
    REQUIRE(value.error() == expected);
}

/// Choose the algorithm and record layout for a test producer
template <sequence_alphabet p_alphabet = seq::dna4, class p_storage_unit = std::uint64_t>
[[nodiscard]] constexpr bioseq::indexing::minimizer_generation_algorithm_identity
test_generation_algorithm_identity(
    bioseq::simd::minimizer_algorithm algorithm = bioseq::simd::minimizer_algorithm::scalar,
    std::uint64_t maximum_position_extent_bp = 32'768u,
    std::uint16_t kmer_size = 15u,
    std::uint16_t window_size = 10u) {
    const auto selection =
        bioseq::minimizers::select_minimizer_physical_layout<p_alphabet, p_storage_unit>(
            algorithm, maximum_position_extent_bp, kmer_size, window_size);
    const auto identity =
        bioseq::indexing::make_minimizer_generation_algorithm_identity(selection);
    return *identity;
}

/// Build, a manifest with fixed values for byte-layout tests.
[[nodiscard]] constexpr bioseq::indexing::minimizer_generation_manifest
golden_generation_manifest() noexcept {
    auto manifest = bioseq::indexing::minimizer_generation_manifest{
        .layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
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
        .alphabet_code = bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
        .storage_unit_byte_width = sizeof(std::uint64_t),
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_revision = bioseq::indexing::k_grouped_soa_gpu_abi_revision,
        .position_segment_span_bp = 8,
        .segment_geometry_revision =
            bioseq::indexing::minimizer_generation_manifest::uniform_segment_geometry_revision,
        .producer_backend_id = bioseq::indexing::minimizer_producer_backend::scalar,
        .producer_identity_revision = bioseq::indexing::k_minimizer_producer_identity_revision,
        .algorithm_identity = test_generation_algorithm_identity(),
    };
    assign_grouped_soa_profile(manifest, k_test_grouped_soa_profile);
    return manifest;
}

/// Set up a test  generation for the chosen sequence and minimizer algorithm.
template <sequence_alphabet p_alphabet = seq::dna4, class p_storage_unit = std::uint64_t>
[[nodiscard]] constexpr bioseq::indexing::grouped_soa_generation_options
test_generation_options(
    std::uint64_t target_length,
    bioseq::simd::minimizer_algorithm algorithm = bioseq::simd::minimizer_algorithm::scalar,
    std::uint64_t maximum_position_extent_bp = 32'768u,
    std::uint16_t kmer_size = 15u,
    std::uint16_t window_size = 10u) noexcept {
    constexpr auto alphabet_id =
        bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>();
    const auto scheme = algorithm == bioseq::simd::minimizer_algorithm::bounded_nthash_simd
        ? bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_identity
        : bioseq::indexing::k_minimap_scheme_identity;
    return {
        .layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
        .generation_id = 37,
        .source_identity = 0x1122334455667788ULL,
        .source_content_checksum = 0x8877665544332211ULL,
        .target_length = target_length,
        .alphabet_code = alphabet_id,
        .storage_unit_byte_width = static_cast<std::uint16_t>(sizeof(p_storage_unit)),
        .kmer_size = kmer_size,
        .window_size = window_size,
        .minimizer_scheme_id = scheme.id,
        .minimizer_scheme_revision = scheme.revision,
        .algorithm_identity = test_generation_algorithm_identity<p_alphabet, p_storage_unit>(
            algorithm, maximum_position_extent_bp, kmer_size, window_size),
    };
}

/// Set up a test generation with a chosen maximum occurrence count.
template <sequence_alphabet p_alphabet = seq::dna4, class p_storage_unit = std::uint64_t>
[[nodiscard]] constexpr bioseq::indexing::grouped_soa_generation_options
test_grouped_soa_generation_options(
    std::uint64_t target_length,
    std::uint64_t max_occ_cap = 65'534u,
    bioseq::simd::minimizer_algorithm algorithm = bioseq::simd::minimizer_algorithm::scalar,
    std::uint64_t maximum_position_extent_bp = 32'768u,
    std::uint16_t kmer_size = 15u,
    std::uint16_t window_size = 10u) noexcept {
    auto options = test_generation_options<p_alphabet, p_storage_unit>(
        target_length, algorithm, maximum_position_extent_bp, kmer_size, window_size);
    options.layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local;
    options.max_occ_cap = max_occ_cap;
    return options;
}

/// Describe the scalar minimap index expected by test readers.
[[nodiscard]] constexpr bioseq::indexing::expected_minimizer_generation_identity
test_expected_storage_contract() noexcept {
    return {
        .layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local,
        .alphabet_code = bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
        .storage_unit_byte_width = 8,
        .kmer_size = 15,
        .window_size = 10,
        .gpu_abi_revision = bioseq::indexing::k_grouped_soa_gpu_abi_revision,
        .minimizer_scheme_id = bioseq::indexing::k_minimap_scheme_id,
        .minimizer_scheme_revision = bioseq::indexing::k_minimap_scheme_revision,
        .producer_backend_id = bioseq::indexing::minimizer_producer_backend::scalar,
        .producer_identity_revision = bioseq::indexing::k_minimizer_producer_identity_revision,
        .algorithm_identity = test_generation_algorithm_identity(),
        .max_occ_cap = k_test_grouped_soa_profile.max_occ_cap,
    };
}

/// Describe the test reader layout with a chosen occurrence cap.
[[nodiscard]] constexpr bioseq::indexing::expected_minimizer_generation_identity
test_grouped_soa_expected_storage_contract(std::uint64_t max_occ_cap = 65'534u) noexcept {
    auto expected = test_expected_storage_contract();
    expected.layout = bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local;
    expected.alphabet_code = bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code;
    expected.gpu_abi_revision = bioseq::indexing::k_grouped_soa_gpu_abi_revision;
    expected.max_occ_cap = max_occ_cap;
    return expected;
}

/// Own one encoded row captured by a test sink.
struct collected_minimizer_row {
    bioseq::indexing::index_chunk_address address{};
    std::vector<std::byte> bytes;
    std::uint64_t row_checksum = 0;
};

/// Keep rows from both storage callbacks and count direct payload writes.
struct collecting_minimizer_row_sink {
    std::vector<collected_minimizer_row> rows;
    std::size_t max_callback_bytes = 0;
    std::uint64_t grouped_soa_payload_write_calls = 0;
    std::uint64_t grouped_soa_row_crc_bytes = 0;

    /// Copy a row and record the largest callback size.
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

    /// Compute the row checksum and capture one direct payload row.
    [[nodiscard]] std::expected<bioseq::indexing::minimizer_generation_row_write_receipt,
                                bioseq::indexing::minimizer_generation_encoding_error>
    write_grouped_soa_payload(
        const bioseq::indexing::unchecked_minimizer_generation_row_view& row) {
        const std::uint64_t row_checksum =
            bioseq::indexing::crc64_index_chunk_row(row.address.generation_id,
                                                    row.address.position_segment,
                                                    row.address.sub_key,
                                                    row.address.chunk_no,
                                                    static_cast<std::uint32_t>(row.bytes.size()),
                                                    row.bytes);
        if (auto written = write(bioseq::indexing::minimizer_generation_row_view{
                .address = row.address,
                .bytes = row.bytes,
                .row_checksum = row_checksum,
            });
            !written) {
            return std::unexpected(written.error());
        }
        ++grouped_soa_payload_write_calls;
        grouped_soa_row_crc_bytes += row.bytes.size();
        return bioseq::indexing::minimizer_generation_row_write_receipt{
            .row_checksum = row_checksum,
        };
    }
};

/// Replay stored minimizmr events with the configured scheme and backend.
struct vector_replay_minimizer_producer {
    std::vector<bioseq::indexing::canonical_minimizer_event> events;
    std::size_t scan_count = 0;
    bioseq::indexing::minimizer_scheme_identity scheme =
        bioseq::indexing::k_minimap_scheme_identity;
    bioseq::indexing::minimizer_producer_identity producer_backend =
        bioseq::indexing::minimizer_producer_backend_identity_for_backend(
            bioseq::simd::minimizer_sketch_backend::scalar);

    /// Copy hash-position pairs and choose a backend identity for their scheme.
    explicit vector_replay_minimizer_producer(
        std::span<const std::pair<std::uint64_t, std::uint64_t>> pairs,
        bioseq::indexing::minimizer_scheme_identity scheme_identity =
            bioseq::indexing::k_minimap_scheme_identity,
        bioseq::indexing::minimizer_producer_identity producer_backend_identity = {})
        : scheme(scheme_identity),
          producer_backend(
              bioseq::indexing::is_supported_minimizer_producer_backend_identity(
                  producer_backend_identity)
                  ? producer_backend_identity
                  : bioseq::indexing::minimizer_producer_backend_identity_for_backend(
                        scheme_identity ==
                                bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_identity
                            ? bioseq::simd::minimizer_sketch_backend::bounded_nthash_simd
                            : bioseq::simd::minimizer_sketch_backend::scalar)) {
        events.reserve(pairs.size());
        for (const auto& pair : pairs) {
            events.push_back({pair.first, pair.second});
        }
    }

    /// Return the minimizer scheme attached to each replay.
    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity scheme_identity() const noexcept {
        return scheme;
    }

    /// Return the producer identity attached to each replay.
    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return producer_backend;
    }

    /// Sort a copy by segment, hash, and encoded position, then  emit each event.
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    scan_sorted_events(std::uint64_t span,
                       bioseq::indexing::canonical_minimizer_event_receiver receiver) {
        ++scan_count;
        if (span == 0) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::segment_span);
        }
        std::vector<bioseq::indexing::canonical_minimizer_event> ordered = events;
        std::sort(ordered.begin(), ordered.end(), [span](const auto& lhs, const auto& rhs) {
            const std::uint64_t lhs_segment = (lhs.encoded_position >> 1u) / span;
            const std::uint64_t rhs_segment = (rhs.encoded_position >> 1u) / span;
            if (lhs_segment != rhs_segment)
                return lhs_segment < rhs_segment;
            if (lhs.hash != rhs.hash)
                return lhs.hash < rhs.hash;
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

/// Concatenate the control or position rows named by one directory entry.
[[nodiscard]] std::vector<std::byte>
collect_encoded_extent(std::span<const collected_minimizer_row> rows,
                       const bioseq::indexing::grouped_soa_bucket_directory_entry& entry,
                       bool controls) {
    const std::uint32_t first = controls ? entry.control_first_chunk : entry.position_first_chunk;
    const std::uint32_t count = controls ? entry.control_chunk_count : entry.position_chunk_count;
    const auto position_sub_key =
        bioseq::indexing::derive_minimizer_position_extent_sub_key(entry.sub_key);
    REQUIRE(position_sub_key.has_value());
    const bioseq::indexing::index_sub_key_t physical_sub_key =
        controls ? entry.sub_key : *position_sub_key;
    std::vector<std::byte> bytes;
    for (std::uint32_t chunk = first; chunk < first + count; ++chunk) {
        const auto row =
            std::find_if(rows.begin(),
                         rows.end(),
                         [entry, physical_sub_key, chunk](const collected_minimizer_row& value) {
                             return value.address.position_segment == entry.position_segment &&
                                    value.address.sub_key == physical_sub_key &&
                                    value.address.chunk_no == chunk;
                         });
        REQUIRE(row != rows.end());
        bytes.insert(bytes.end(), row->bytes.begin(), row->bytes.end());
    }
    REQUIRE(bytes.size() ==
            (controls ? entry.control_encoded_bytes : entry.position_encoded_bytes));
    return bytes;
}

/// Compare encoded results after removing generation and producer identity differences.
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
    // First check each original storage checksum. Then remove the producer
    // identity so results from different backends can be compared.
    REQUIRE(lhs.head.storage_contract_checksum ==
            compute_minimizer_generation_storage_contract_checksum(lhs.manifest));
    REQUIRE(rhs.head.storage_contract_checksum ==
            compute_minimizer_generation_storage_contract_checksum(rhs.manifest));
    lhs_manifest.producer_backend_id = bioseq::indexing::minimizer_producer_backend::scalar;
    rhs_manifest.producer_backend_id = bioseq::indexing::minimizer_producer_backend::scalar;
    lhs_manifest.producer_identity_revision =
        bioseq::indexing::k_minimizer_producer_identity_revision;
    rhs_manifest.producer_identity_revision =
        bioseq::indexing::k_minimizer_producer_identity_revision;
    lhs_manifest.algorithm_identity = {};
    rhs_manifest.algorithm_identity = {};
    REQUIRE(lhs_manifest == rhs_manifest);
    REQUIRE(lhs.head.source_identity == rhs.head.source_identity);
    REQUIRE(compute_minimizer_generation_storage_contract_checksum(lhs_manifest) ==
            compute_minimizer_generation_storage_contract_checksum(rhs_manifest));
    REQUIRE(lhs.head.source_content_checksum == rhs.head.source_content_checksum);
    REQUIRE(std::ranges::equal(lhs.directory.encoded_bytes(), rhs.directory.encoded_bytes()));
    REQUIRE(lhs_rows.size() == rhs_rows.size());
    REQUIRE_FALSE(lhs_rows.empty());
    for (std::size_t index = 0; index + 1u < lhs_rows.size(); ++index) {
        REQUIRE(lhs_rows[index].address.position_segment ==
                rhs_rows[index].address.position_segment);
        REQUIRE(lhs_rows[index].address.sub_key == rhs_rows[index].address.sub_key);
        REQUIRE(lhs_rows[index].address.chunk_no == rhs_rows[index].address.chunk_no);
        REQUIRE(lhs_rows[index].bytes == rhs_rows[index].bytes);
    }
}

/// Borrow a captured row as a generation-row view.
[[nodiscard]] bioseq::indexing::minimizer_generation_row_view
row_view(const collected_minimizer_row& row) noexcept {
    return {
        .address = row.address,
        .bytes = row.bytes,
        .row_checksum = row.row_checksum,
    };
}

/// Create a row validator for the generation, failing the test if invalid.
[[nodiscard]] bioseq::indexing::minimizer_generation_row_validator
make_generation_row_validator(const bioseq::indexing::encoded_grouped_soa_generation& generation) {
    auto made = bioseq::indexing::minimizer_generation_row_validator::create(generation);
    REQUIRE(made.has_value());
    return std::move(*made);
}

/// Check that the publication head is accepted only after all rows validate.
void require_complete_generation_rows(
    const bioseq::indexing::encoded_grouped_soa_generation& generation,
    std::span<const collected_minimizer_row> rows) {
    auto validator = make_generation_row_validator(generation);
    require_wire_error(
        validator.validate_publication_head(bioseq::indexing::minimizer_generation_row_view{
            .address = generation.unpublished_head_row.address,
            .bytes = generation.unpublished_head_row.bytes(),
            .row_checksum = generation.unpublished_head_row.row_checksum,
        }),
        bioseq::indexing::minimizer_generation_encoding_error::row_count);
    for (const auto& row : rows) {
        REQUIRE(validator.accept(row_view(row)).has_value());
    }
    REQUIRE(validator.finish_ready().has_value());
    REQUIRE(validator
                .validate_publication_head(bioseq::indexing::minimizer_generation_row_view{
                    .address = generation.unpublished_head_row.address,
                    .bytes = generation.unpublished_head_row.bytes(),
                    .row_checksum = generation.unpublished_head_row.row_checksum,
                })
                .has_value());
}

/// Copy bytes into a fixed row and compute its address-dependent checksum.
[[nodiscard]] bioseq::indexing::encoded_minimizer_fixed_row
make_test_fixed_row(const bioseq::indexing::index_chunk_address& address,
                    std::span<const std::byte> bytes) {
    bioseq::indexing::encoded_minimizer_fixed_row row{
        .address = address,
        .byte_count = static_cast<std::uint32_t>(bytes.size()),
    };
    std::copy(bytes.begin(), bytes.end(), row.storage.begin());
    row.row_checksum = bioseq::indexing::crc64_index_chunk_row(address.generation_id,
                                                               address.position_segment,
                                                               address.sub_key,
                                                               address.chunk_no,
                                                               row.byte_count,
                                                               row.bytes());
    return row;
}

/// Build a checksummed fixed row from a byte array
template <std::size_t p_size>
[[nodiscard]] bioseq::indexing::encoded_minimizer_fixed_row
make_test_fixed_row(const bioseq::indexing::index_chunk_address& address,
                    const std::array<std::byte, p_size>& bytes) {
    return make_test_fixed_row(address, std::span<const std::byte>{bytes});
}

/// Re-encode the manifest and unpublished head  after changing test metadata.
void refresh_test_generation_fixed_rows(
    bioseq::indexing::encoded_grouped_soa_generation& generation) {
    using namespace bioseq::indexing;
    const auto manifest_bytes = encode_minimizer_generation_manifest(generation.manifest);
    REQUIRE(manifest_bytes.has_value());
    generation.head.storage_contract_checksum =
        compute_minimizer_generation_storage_contract_checksum(generation.manifest);
    generation.head.manifest_checksum = crc64_ecma182(std::span<const std::byte>{*manifest_bytes});
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
} // namespace minimizer_hashmap_test_support
} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
