/**
 * @file fragment_producer_test.cpp
 * @brief Tests fragment and spill minimizer generation producers.
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

/// Packed DNA4 alphabet used to exercise 16-bit storage units.
using dna4_u16 = sequence_custom_alphabet<seq::dna4, std::uint16_t>;
/// Resident range used by the 16-bit DNA4 fixtures.
using dna4_u16_range_t = compact_sequence_range<dna4_u16>;
/// Describe a resident packed range for the fixed-target kernel bridge.
template <class p_range>
[[nodiscard]] ::bioseq::minimizers::packed_sequence_kernel_view
fixed_packed_sequence_view(const p_range& range) {
    using traits = typename p_range::traits;
    return {
        .storage = reinterpret_cast<const std::byte*>(range.read_cptr(range.cbegin())),
        .storage_units = range.size(),
        .begin_symbol = 0u,
        .end_symbol = range.bp_size(),
        .alphabet_code = fixed_alphabet_code<typename traits::alphabet_type>(),
        .bits_per_symbol = static_cast<std::uint8_t>(traits::bits_per_char),
        .storage_unit_bytes = static_cast<std::uint8_t>(sizeof(typename traits::storage_unit)),
        .final_segment = true,
    };
}
/// Group canonical events by hash and remove duplicate positions.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
canonical_event_lookup_map(std::span<const bioseq::indexing::canonical_minimizer_event> events) {
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
/// Change the replayed position after the first completed scan.
struct drifting_replay_minimizer_producer {
    std::size_t scan_count = 0;

    /// Return the fixed minimap scheme identity.
    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity scheme_identity() const noexcept {
        return bioseq::indexing::k_minimap_scheme_identity;
    }

    /// Return the fixed scalar producer identity.
    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return bioseq::indexing::minimizer_producer_backend_identity_for_backend(
            bioseq::simd::minimizer_sketch_backend::scalar);
    }

    /// Emit a position that changes after the first replay.
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    scan_sorted_events(std::uint64_t span,
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

/// Select which producer identity changes after a scan boundary.
enum class identity_drift_axis : std::uint8_t {
    scheme,
    backend,
};

/// Change one qualified identity after a selected completed grouped-SoA scan.
struct boundary_identity_drifting_producer {
    std::size_t scan_count = 0;
    std::size_t drift_after_scan = 1;
    identity_drift_axis axis = identity_drift_axis::scheme;

    /// Return the scheme identity for the active scan ordinal.
    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity scheme_identity() const noexcept {
        if (axis == identity_drift_axis::scheme && scan_count >= drift_after_scan) {
            return bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_identity;
        }
        return bioseq::indexing::k_minimap_scheme_identity;
    }

    /// Return the producer identity for the active scan ordinal.
    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        const auto backend = axis == identity_drift_axis::backend && scan_count >= drift_after_scan
                                 ? bioseq::simd::minimizer_sketch_backend::exact_simd
                                 : bioseq::simd::minimizer_sketch_backend::scalar;
        return bioseq::indexing::minimizer_producer_backend_identity_for_backend(backend);
    }

    /// Emit one event and advance the scan ordinal.
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    scan_sorted_events(std::uint64_t span,
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
/// Decode every canonical event from captured grouped-SoA rows.
[[nodiscard]] std::vector<bioseq::indexing::canonical_minimizer_event>
decode_generation_events(const bioseq::indexing::encoded_grouped_soa_generation& generation,
                         std::span<const collected_minimizer_row> rows) {
    using namespace bioseq::indexing;
    std::vector<bioseq::indexing::canonical_minimizer_event> events;
    const auto& profile = generation.directory.profile();
    for (const auto& entry : generation.directory.entries()) {
        const auto control_bytes = collect_encoded_extent(rows, entry, true);
        const auto position_bytes = collect_encoded_extent(rows, entry, false);
        const auto core = derive_minimizer_position_segment_bounds(
            generation.manifest, static_cast<std::uint32_t>(entry.position_segment));
        REQUIRE(core.has_value());
        const auto controls = decode_grouped_soa_controls(control_bytes, entry, profile);
        REQUIRE(controls.has_value());
        const auto positions =
            decode_grouped_soa_positions(position_bytes, *controls, entry, *core, profile);
        REQUIRE(positions.has_value());
        for (const auto& control : *controls) {
            for (std::uint64_t position = control.position_begin; position < control.position_end;
                 ++position) {
                events.push_back({
                    .hash = control.hash,
                    .encoded_position = (*positions)[static_cast<std::size_t>(position)],
                });
            }
        }
    }
    return events;
}
} // namespace

TEST_CASE("independent fragment halo and spill-run merge producers are canonical",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::size_t fragment_bp = 32768;
    constexpr std::size_t halo = 10u + 15u - 2u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
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
        if (lhs.hash != rhs.hash)
            return lhs.hash < rhs.hash;
        return lhs.encoded_position < rhs.encoded_position;
    });
    REQUIRE(std::adjacent_find(oracle.begin(), oracle.end()) == oracle.end());
    REQUIRE(std::ranges::any_of(oracle, [](const auto& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        return position + halo >= fragment_bp && position <= fragment_bp + halo;
    }));

    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto fragment_producer = index_type::make_generation_replay_producer(
        lazy_view, minimizer_sketch_backend::scalar, nullptr);
    collecting_minimizer_row_sink fragment_sink;
    auto fragment_options = test_generation_options(range.bp_size());
    auto fragment_encoding =
        encode_minimizer_generation(fragment_producer, fragment_sink, fragment_options);
    REQUIRE(fragment_encoding.has_value());
    REQUIRE(fragment_producer.scan_count() == 3);
    const minimizer_fragment_generation_execution_receipt scalar_execution =
        fragment_producer.execution_stats();
    REQUIRE(scalar_execution.is_accepted());
    REQUIRE(scalar_execution.selected_backend == minimizer_sketch_backend::scalar);
    REQUIRE(scalar_execution.scalar_core_runs > 0);
    REQUIRE(scalar_execution.exact_simd_core_runs == 0);
    REQUIRE(scalar_execution.bounded_nthash_core_runs == 0);
    REQUIRE(scalar_execution.vulkan_core_runs == 0);
    REQUIRE(fragment_producer.peak_buffered_core_bp() <= k_minimizer_preferred_segment_span_bp);

    fake_fragment_storage<> spill_storage;
    auto spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        spill_storage, lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count() >= 3);
    const std::uint64_t scalar_spill_run_count = spilled->run_count();
    auto spill_producer = spill_run_index_type::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spilled));
    collecting_minimizer_row_sink spill_sink;
    auto spill_options = fragment_options;
    ++spill_options.generation_id;
    auto spill_encoding = encode_minimizer_generation(spill_producer, spill_sink, spill_options);
    REQUIRE(spill_encoding.has_value());
    REQUIRE(spill_producer.scan_count() == 3);
    REQUIRE(spill_producer.merge_pass_count() == 3);
    REQUIRE(spill_storage.segment_merge_calls() == 4);
    REQUIRE(spill_storage.segment_merge_run_visits() ==
            4u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited() ==
            4u * spill_storage.segment_spill_bytes());
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
    lazy_sequence<dna4_u64, chunked> lazy_exact{chunked::from_range(range)};
    const auto lazy_exact_view = bioseq::storage::make_lazy_sequence_view(lazy_exact);
    auto exact_fragment_producer =
        index_type::make_generation_replay_producer(lazy_exact_view,
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
    REQUIRE(exact_execution.selected_backend == minimizer_sketch_backend::exact_simd);
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
    auto exact_spill_encoding =
        encode_minimizer_generation(exact_spill_producer, exact_spill_sink, exact_spill_options);
    REQUIRE(exact_spill_encoding.has_value());

    require_encoded_generations_equal_normalized(
        *fragment_encoding, fragment_sink.rows, *exact_fragment_encoding, exact_fragment_sink.rows);
    require_encoded_generations_equal_normalized(
        *spill_encoding, spill_sink.rows, *exact_spill_encoding, exact_spill_sink.rows);
    REQUIRE(decode_generation_events(*exact_fragment_encoding, exact_fragment_sink.rows) == oracle);
    REQUIRE(decode_generation_events(*exact_spill_encoding, exact_spill_sink.rows) == oracle);
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
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;
    auto data = make_dna4(18013, 0x4D5053464D543556ULL);
    range_t range(data);

    const auto spill_exact = [&range](fake_fragment_storage<>& storage) {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
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

        const auto options = test_grouped_soa_generation_options(range.bp_size());
        collecting_minimizer_row_sink sink;
        minimizer_sequence_generation_execution_receipt execution;
        const auto encoded = spill_run_index_type::encode_persisted_generation(
            storage, sink, std::move(*spill), options, {}, &execution);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->manifest.layout == minimizer_generation_layout::grouped_soa_segment_local);
        REQUIRE(encoded->manifest.alphabet_code == k_minimizer_persisted_dna4_alphabet_code);
        REQUIRE(encoded->manifest.gpu_abi_revision == k_grouped_soa_gpu_abi_revision);
        REQUIRE(encoded->manifest.max_occ_cap == options.max_occ_cap);

        const auto canonical_profile =
            derive_grouped_soa_minimizer_profile(k_minimap_scheme_identity,
                                                 options.alphabet_code,
                                                 options.kmer_size,
                                                 encoded->manifest.position_segment_span_bp,
                                                 options.max_occ_cap);
        REQUIRE(canonical_profile.has_value());
        REQUIRE(encoded->directory.profile() == *canonical_profile);
        const auto manifest_profile = derive_grouped_soa_minimizer_profile(encoded->manifest);
        REQUIRE(manifest_profile.has_value());
        REQUIRE(*manifest_profile == *canonical_profile);

        REQUIRE(execution.is_internally_consistent());
        REQUIRE(execution.generation_layout == options.layout);
        REQUIRE(execution.selected_backend == minimizer_sketch_backend::exact_simd);
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
        REQUIRE(execution.replay_visited_event_count == 3u * execution.spilled_event_count);
        REQUIRE(sink.grouped_soa_payload_write_calls > 0u);
        REQUIRE(sink.grouped_soa_row_crc_bytes == encoded->manifest.payload_bytes);
        require_complete_generation_rows(*encoded, sink.rows);
    }

    const auto require_profile_rejection =
        [&spill_exact, &require_wire_error](grouped_soa_generation_options options) {
            fake_fragment_storage<> storage;
            auto spill = spill_exact(storage);
            REQUIRE(spill.has_value());
            collecting_minimizer_row_sink sink;
            minimizer_sequence_generation_execution_receipt execution;
            require_wire_error(spill_run_index_type::encode_persisted_generation(
                                   storage, sink, std::move(*spill), options, {}, &execution),
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
        options.max_occ_cap = (std::numeric_limits<std::uint64_t>::max)();
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
                (nibble(expected[index * 2u]) << 4u) | nibble(expected[index * 2u + 1u]));
            REQUIRE(std::to_integer<std::uint8_t>(digest[index]) == value);
        }
    };

    bioseq::common::sha256_hasher empty;
    require_hex(empty.finalize(),
                "e3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855");

    const auto bytes = [](std::string_view value) {
        return std::as_bytes(std::span{value.data(), value.size()});
    };
    bioseq::common::sha256_hasher contiguous;
    contiguous.update(bytes("abc"));
    const direct_minimizer_content_digest contiguous_digest = contiguous.finalize();
    require_hex(contiguous_digest,
                "ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad");

    bioseq::common::sha256_hasher split;
    split.update(bytes("a"));
    split.update(bytes("b"));
    split.update(bytes("c"));
    REQUIRE(split.finalize() == contiguous_digest);

    //  promoted helpers: lowercase-hex rendering and the one-shot convenience
    // share the same digest and match the canonical "abc" vector.
    REQUIRE(bioseq::common::to_hex(contiguous_digest) == "ba7816bf8f01cfea414140de5dae2223"
                                                         "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(bioseq::common::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223"
                                                 "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(bioseq::common::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb924"
                                              "27ae41e4649b934ca495991b7852b855");

    // update_little_endian defines the byte order used by the spill-run digest.
    bioseq::common::sha256_hasher little_endian;
    little_endian.update_little_endian(0x0807060504030201ull);
    bioseq::common::sha256_hasher raw_bytes;
    const std::array<std::byte, 8> le_expected{std::byte{0x01},
                                               std::byte{0x02},
                                               std::byte{0x03},
                                               std::byte{0x04},
                                               std::byte{0x05},
                                               std::byte{0x06},
                                               std::byte{0x07},
                                               std::byte{0x08}};
    raw_bytes.update(le_expected);
    REQUIRE(little_endian.finalize() == raw_bytes.finalize());
}

TEST_CASE("inactive spill-run storage incarnation rejects duplicate runs ABA and reuse",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;
    auto data = make_dna4(4096, 0x494E4341524E4154ULL);
    range_t range(data);

    SECTION("duplicate physical run advances the storage mutation revision") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage, lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
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
        require_wire_error(spill_run_index_type::encode_persisted_generation(
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
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> original;
        fake_fragment_storage<> replacement;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original, lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
        REQUIRE(spill.has_value());
        collecting_minimizer_row_sink wrong_sink;
        require_wire_error(spill_run_index_type::encode_persisted_generation(
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
        require_wire_error(spill_run_index_type::encode_persisted_generation(
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
        lazy_sequence<dna4_u64, chunked> empty_lazy{chunked::from_range(empty_range)};
        const auto empty_lazy_view = bioseq::storage::make_lazy_sequence_view(empty_lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage, empty_lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
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
                storage, sink, std::move(*spill), test_grouped_soa_generation_options(0)),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("moving storage cannot preserve address-bound authority") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> original;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original, lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding issued =
            original.segment_spill_storage_binding();
        fake_fragment_storage<> moved{std::move(original)};
        REQUIRE_FALSE(moved.segment_spill_storage_binding().valid());
        collecting_minimizer_row_sink sink;
        require_wire_error(spill_run_index_type::encode_persisted_generation(
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
        lazy_sequence<dna4_u64, chunked> empty_lazy{chunked::from_range(empty_range)};
        const auto empty_lazy_view = bioseq::storage::make_lazy_sequence_view(empty_lazy);
        fake_fragment_storage<> original;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original, empty_lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
        REQUIRE(spill.has_value());
        REQUIRE(original.segment_spill_storage_binding().valid());

        fake_fragment_storage<> moved{std::move(original)};
        REQUIRE_FALSE(original.segment_spill_storage_binding().valid());
        REQUIRE_FALSE(moved.segment_spill_storage_binding().valid());
        collecting_minimizer_row_sink sink;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                original, sink, std::move(*spill), test_grouped_soa_generation_options(0)),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("representation-only run compaction preserves logical incarnation") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage, lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding before =
            storage.segment_spill_storage_binding();
        const std::uint64_t physical_runs_before = storage.segment_spill_run_count();
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
        REQUIRE(execution.allows_ready_release(storage.segment_spill_storage_binding()));
        const auto segment = storage.segment_runs_.begin()->first;
        const auto post_encode_splice = storage.segment_runs_.begin()->second.front();
        storage.spill_sorted_segment_run(segment, post_encode_splice);
        REQUIRE_FALSE(execution.allows_ready_release(storage.segment_spill_storage_binding()));
        require_complete_generation_rows(*encoded, sink.rows);
    }

    SECTION("bound producer and consumed spill are single-use") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage, lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
        REQUIRE(spill.has_value());
        auto producer = spill_run_index_type::make_generation_replay_producer(
            storage, range.bp_size(), std::move(*spill));
        collecting_minimizer_row_sink first_sink;
        const auto first = encode_minimizer_generation(
            producer, first_sink, test_generation_options(range.bp_size()));
        REQUIRE(first.has_value());
        collecting_minimizer_row_sink second_sink;
        require_wire_error(encode_minimizer_generation(
                               producer, second_sink, test_generation_options(range.bp_size())),
                           minimizer_generation_encoding_error::segment_span);

        auto moved_from_producer = spill_run_index_type::make_generation_replay_producer(
            storage, range.bp_size(), std::move(*spill));
        collecting_minimizer_row_sink moved_from_sink;
        require_wire_error(encode_minimizer_generation(moved_from_producer,
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
    REQUIRE(maximum.generation_token == (std::numeric_limits<std::uint64_t>::max)());
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
        (void)collect_exact_simd_index_range_or_throw(exact_collector,
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
        view, fixed_alphabet_code<::seq::dna4>(), 2u, 1u, 0u, 10u, 15u, nullptr, bounded_collector);
    REQUIRE(bounded_lazy.status == packed_bounded_nthash_index_status::invalid_range);
    REQUIRE(bounded_collector.extract_sketches_data().empty());

    const exact_simd_index_bridge_request unsupported_alphabet{
        .alphabet_code = 77u,
        .bits_per_symbol = 2u,
        .storage_unit_bytes = sizeof(std::uint32_t),
    };
    REQUIRE(collect_exact_simd_index_erased(unsupported_alphabet).status ==
            exact_simd_index_status::unsupported_alphabet);
    auto unsupported_storage = unsupported_alphabet;
    unsupported_storage.alphabet_code = fixed_alphabet_code<::seq::dna4>();
    unsupported_storage.storage_unit_bytes = 3u;
    REQUIRE(collect_exact_simd_index_erased(unsupported_storage).status ==
            exact_simd_index_status::unsupported_storage);
    auto missing_exact_state = unsupported_storage;
    missing_exact_state.storage_unit_bytes = sizeof(std::uint32_t);
    REQUIRE(collect_exact_simd_index_erased(missing_exact_state).status ==
            exact_simd_index_status::invalid_state);

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
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_configuration).status ==
            packed_bounded_nthash_index_status::invalid_configuration);

    auto invalid_alphabet = bounded_request;
    invalid_alphabet.sequence.alphabet_code = fixed_alphabet_code<::seq::dna5>();
    invalid_alphabet.storage_unit_bytes = sizeof(std::uint16_t);
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_alphabet).status ==
            packed_bounded_nthash_index_status::unsupported_alphabet);

    auto invalid_storage = bounded_request;
    invalid_storage.storage_unit_bytes = sizeof(std::uint16_t);
    invalid_storage.sequence.storage_unit_bytes = sizeof(std::uint16_t);
    invalid_storage.letters_per_unit = 8u;
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_storage).status ==
            packed_bounded_nthash_index_status::unsupported_storage);

    auto invalid_range = bounded_request;
    invalid_range.sequence.end_symbol = range.bp_size() + 1u;
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_range).status ==
            packed_bounded_nthash_index_status::invalid_range);
    REQUIRE(collect_packed_bounded_nthash_index_erased(bounded_request).status ==
            packed_bounded_nthash_index_status::invalid_state);
}

TEST_CASE(
    "bounded ntHash index stats aggregation rejects mixed source widths without rewriting identity",
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
    REQUIRE(view.try_resident_packed(0u, view.bp_size(), sizeof(std::uint64_t)).has_value());
    REQUIRE_FALSE(view.try_resident_packed(0u, view.bp_size(), sizeof(std::uint32_t)).has_value());

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t admission_before = admission.reserved_bytes();
    sketch_collector collector;
    std::uint64_t materialized_window_bp = 0u;
    const auto result = collect_packed_bounded_nthash_index_lazy(view,
                                                                 fixed_alphabet_code<::seq::dna4>(),
                                                                 0u,
                                                                 view.bp_size(),
                                                                 view.bp_size(),
                                                                 10u,
                                                                 15u,
                                                                 fixed_minimizer_kernels(),
                                                                 collector,
                                                                 &materialized_window_bp);
    if (result.status == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        REQUIRE(admission.reserved_bytes() == admission_before);
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(result.stats.is_internally_consistent(true));
    REQUIRE(result.stats.source_storage_unit_bytes == sizeof(std::uint32_t));
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
    using chunked = chunked_span_sequence_scanner<std::uint64_t, source_fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    auto data = make_dna4(98'417u, 0x4D504656554C4B4EULL);
    range_t range(data);
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    fake_fragment_storage<> storage;
    storage.run_buffer_bytes_ = 64u << 20u;
    minimizer_fragment_generation_execution_receipt evidence;
    index_type::build(storage, view, minimizer_sketch_backend::vulkan_segment, nullptr, &evidence);

    REQUIRE_FALSE(evidence.is_accepted());
    REQUIRE(evidence.direct_build_accepted());
    REQUIRE(evidence.selected_backend == minimizer_sketch_backend::vulkan_segment);
    REQUIRE(evidence.backend_bound);
    REQUIRE(evidence.target_length == range.bp_size());
    REQUIRE(evidence.scalar_core_runs == 0u);
    REQUIRE(evidence.exact_simd_core_runs == 0u);
    REQUIRE(evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(evidence.vulkan_core_runs > 0u);
    REQUIRE(evidence.vulkan_stats.is_internally_consistent(true));
    REQUIRE(evidence.vulkan_stats.completed_roundtrips == evidence.vulkan_core_runs);
    const std::size_t expected_fragments = static_cast<std::size_t>(
        (range.bp_size() + persisted_fragment_bp - 1u) / persisted_fragment_bp);
    REQUIRE(storage.number_of_chunks(k_directory_sub_key) == 1u);
    REQUIRE(*storage.template read_chunk<std::int64_t>(k_directory_sub_key, 0u) ==
            static_cast<std::int64_t>(expected_fragments));
}

TEST_CASE("bounded ntHash fragment replay matches one checked resident execution",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = 32'768u;
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    auto data = make_dna4(70013, 0x50415045524E5448ULL);
    range_t range(data);
    sketch_collector resident_collector;
    const auto resident_result =
        collect_packed_bounded_nthash_index_erased(packed_bounded_nthash_index_bridge_request{
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
    if (resident_result.status == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(resident_result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(resident_result.stats.is_internally_consistent(true));

    std::vector<canonical_minimizer_event> oracle;
    oracle.reserve(resident_collector.extract_sketches_data().size());
    for (const minimap_sketch_data& minimizer : resident_collector.extract_sketches_data()) {
        oracle.push_back({
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    std::ranges::sort(oracle, {}, [](const canonical_minimizer_event& event) {
        return std::pair{event.hash, event.encoded_position};
    });

    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto producer =
        index_type::make_generation_replay_producer(lazy_view,
                                                    minimizer_sketch_backend::bounded_nthash_simd,
                                                    fixed_minimizer_kernels(),
                                                    fixed_alphabet_code<::seq::dna4>());
    auto options = test_generation_options(range.bp_size());
    options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
    options.minimizer_scheme_revision = k_bounded_nthash_u32_r7_sum_scheme_revision;
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(producer, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(producer.scan_count() == 3);
    const minimizer_fragment_generation_execution_receipt execution = producer.execution_stats();
    REQUIRE(execution.is_accepted());
    REQUIRE(execution.selected_backend == minimizer_sketch_backend::bounded_nthash_simd);
    REQUIRE(execution.scalar_core_runs == 0);
    REQUIRE(execution.exact_simd_core_runs == 0);
    REQUIRE(execution.bounded_nthash_core_runs > 0);
    REQUIRE(execution.vulkan_core_runs == 0);
    REQUIRE(execution.packed_bounded_stats_present);
    REQUIRE(execution.packed_bounded_status == packed_bounded_nthash_index_status::ok);
    REQUIRE(execution.packed_bounded_stats.is_internally_consistent(true));
    REQUIRE(execution.packed_bounded_stats.fallback_calls == 0);
    REQUIRE(execution.packed_bounded_stats.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE(producer.scheme_identity() == k_bounded_nthash_u32_r7_sum_scheme_identity);
    REQUIRE(producer.packed_bounded_stats_present());
    REQUIRE(producer.packed_bounded_status() == packed_bounded_nthash_index_status::ok);
    REQUIRE(producer.packed_bounded_stats().is_internally_consistent(true));
    REQUIRE(producer.packed_bounded_stats().fallback_calls == 0);
    REQUIRE_FALSE(producer.exact_simd_stats_present());
    REQUIRE(producer.peak_bounded_materialized_window_bp() <=
            fragment_bp + 2u * halo + range_t::traits::letters_per_unit - 1u);
    REQUIRE(producer.peak_bounded_materialized_window_bp() < range.bp_size());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == oracle);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("bounded ntHash fragment canonicalizes tied periodic events identically across replays",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
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
    const auto raw_result =
        collect_packed_bounded_nthash_index_erased(packed_bounded_nthash_index_bridge_request{
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
    if (raw_result.status == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(raw_result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(raw_result.stats.is_internally_consistent(true));

    std::vector<canonical_minimizer_event> raw;
    raw.reserve(raw_collector.extract_sketches_data().size());
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> first_seen;
    bool saw_nonadjacent_duplicate = false;
    for (const minimap_sketch_data& minimizer : raw_collector.extract_sketches_data()) {
        const canonical_minimizer_event event{
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        };
        const auto key = std::pair{event.hash, event.encoded_position};
        if (const auto found = first_seen.find(key); found != first_seen.end()) {
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
    canonical.erase(std::ranges::unique(canonical).begin(), canonical.end());
    REQUIRE(raw.size() > canonical.size());
    REQUIRE(saw_nonadjacent_duplicate);

    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto replay =
        index_type::make_generation_replay_producer(lazy_view,
                                                    minimizer_sketch_backend::bounded_nthash_simd,
                                                    fixed_minimizer_kernels(),
                                                    fixed_alphabet_code<::seq::dna4>());
    const auto collect_replay = [](void* context, const canonical_minimizer_event& event)
        -> std::expected<void, minimizer_generation_encoding_error> {
        static_cast<std::vector<canonical_minimizer_event>*>(context)->push_back(event);
        return {};
    };
    std::vector<canonical_minimizer_event> first_replay;
    std::vector<canonical_minimizer_event> second_replay;
    REQUIRE(replay
                .scan_sorted_events(k_minimizer_preferred_segment_span_bp,
                                    {.context = &first_replay, .emit_fn = collect_replay})
                .has_value());
    REQUIRE(replay
                .scan_sorted_events(k_minimizer_preferred_segment_span_bp,
                                    {.context = &second_replay, .emit_fn = collect_replay})
                .has_value());
    REQUIRE(first_replay == canonical);
    REQUIRE(second_replay == canonical);
    REQUIRE(first_replay == second_replay);
    REQUIRE(replay.scan_count() == 2u);
    REQUIRE(replay.execution_stats().is_accepted());
    REQUIRE(replay.execution_stats().selected_backend ==
            minimizer_sketch_backend::bounded_nthash_simd);
    REQUIRE(replay.execution_stats().scalar_core_runs == 0u);
    REQUIRE(replay.execution_stats().exact_simd_core_runs == 0u);
    REQUIRE(replay.execution_stats().vulkan_core_runs == 0u);
    REQUIRE(replay.execution_stats().bounded_nthash_core_runs > 0u);

    auto encoder_replay =
        index_type::make_generation_replay_producer(lazy_view,
                                                    minimizer_sketch_backend::bounded_nthash_simd,
                                                    fixed_minimizer_kernels(),
                                                    fixed_alphabet_code<::seq::dna4>());
    auto options = test_generation_options(range.bp_size());
    options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
    options.minimizer_scheme_revision = k_bounded_nthash_u32_r7_sum_scheme_revision;
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(encoder_replay, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(encoder_replay.scan_count() == 3u);
    REQUIRE(encoder_replay.execution_stats().is_accepted());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == canonical);
    require_complete_generation_rows(*encoded, sink.rows);
}

// clang-format off
TEST_CASE("bounded ntHash fragment replay canonicalizes storage and rejects incompatible biological shapes",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    // clang-format on
    using namespace bioseq::indexing;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    SECTION("DNA5 fails closed without scalar substitution") {
        using chunked = chunked_span_sequence_scanner<std::uint64_t, 64>;
        std::vector<seq::dna5> data(4096);
        for (std::size_t index = 0; index < data.size(); ++index) {
            data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
        }
        dna5_range_t range(data);
        lazy_sequence<dna5_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        auto producer = index_type::make_generation_replay_producer(
            lazy_view,
            minimizer_sketch_backend::bounded_nthash_simd,
            fixed_minimizer_kernels(),
            fixed_alphabet_code<::seq::dna5>());
        auto options = test_generation_options(range.bp_size());
        options.alphabet_code = 5;
        options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
        options.minimizer_scheme_revision = k_bounded_nthash_u32_r7_sum_scheme_revision;
        collecting_minimizer_row_sink sink;
        const std::uint64_t admission_before =
            bioseq::parallelism::memory_admission_controller::instance().reserved_bytes();
        require_wire_error(encode_minimizer_generation(producer, sink, options),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
        REQUIRE(producer.scan_count() == 0u);
        REQUIRE_FALSE(producer.packed_bounded_stats_present());
        REQUIRE_FALSE(producer.exact_simd_stats_present());
        REQUIRE(bioseq::parallelism::memory_admission_controller::instance().reserved_bytes() ==
                admission_before);
    }

    SECTION("u16 packed storage is admitted into one canonical u32 bounded ntHash window") {
        using chunked = chunked_span_sequence_scanner<std::uint16_t, 64>;
        auto data = make_dna4(4096, 0x16B17u);
        dna4_u16_range_t range(data);
        lazy_sequence<dna4_u16, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        auto producer = index_type::make_generation_replay_producer(
            lazy_view,
            minimizer_sketch_backend::bounded_nthash_simd,
            fixed_minimizer_kernels(),
            fixed_alphabet_code<::seq::dna4>());
        auto options = test_generation_options(range.bp_size());
        options.storage_unit_byte_width = sizeof(std::uint16_t);
        options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
        options.minimizer_scheme_revision = k_bounded_nthash_u32_r7_sum_scheme_revision;
        collecting_minimizer_row_sink sink;
        const auto encoded = encode_minimizer_generation(producer, sink, options);
        REQUIRE(encoded.has_value());
        REQUIRE_FALSE(sink.rows.empty());
        REQUIRE(producer.packed_bounded_stats_present());
        REQUIRE(producer.packed_bounded_status() == packed_bounded_nthash_index_status::ok);
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
        const std::size_t local_bp_size =
            static_cast<std::size_t>(minimap_sketch_data::k_position_field_mask) + 2u;
        const std::size_t storage_units = (local_bp_size + range_t::traits::letters_per_unit - 1u) /
                                          range_t::traits::letters_per_unit;
        const auto rejected =
            collect_packed_bounded_nthash_index_erased(packed_bounded_nthash_index_bridge_request{
                .kernels = fixed_minimizer_kernels(),
                .sequence =
                    {
                        .storage = &sentinel,
                        .storage_units = storage_units,
                        .begin_symbol = 0u,
                        .end_symbol = local_bp_size,
                        .alphabet_code = fixed_alphabet_code<::seq::dna4>(),
                        .bits_per_symbol =
                            static_cast<std::uint8_t>(range_t::traits::bits_per_char),
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
        REQUIRE(rejected.status == packed_bounded_nthash_index_status::unsupported_representation);
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
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    auto data = make_dna4(static_cast<std::size_t>(span + 8192u + 7u), 0x8B1B0A4DULL);
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
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
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
        spill_storage, lazy_view, minimizer_sketch_backend::scalar, nullptr, fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count() > 256);
    REQUIRE(spill_storage.runs_.empty());
    REQUIRE(spill_storage.segment_spill_run_count() == spilled->run_count());
    REQUIRE(spill_storage.segment_runs_.size() == 2);
    constexpr std::uint64_t half_span = span / 2u;
    std::vector<std::uint64_t> half_counts(range.bp_size() / half_span +
                                           (range.bp_size() % half_span != 0 ? 1u : 0u));
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
    const std::uint64_t half_max = *std::ranges::max_element(half_counts);
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
    REQUIRE(execution.replay_first_completed_content_digest ==
            execution.replay_last_completed_content_digest);
    REQUIRE(execution.replay_final_span_bp == half_span);
    REQUIRE(execution.replay_final_storage_binding.generation_token ==
            execution.spill_storage_binding.generation_token);
    REQUIRE(execution.replay_final_storage_binding.mutation_revision ==
            execution.spill_storage_binding.mutation_revision + 1u);
    REQUIRE(encoded->manifest.position_segment_span_bp == half_span);
    REQUIRE(encoded->manifest.position_segment_count == 3);
    REQUIRE(encoded->manifest.position_count == fragment_plan->canonical_event_count);
    REQUIRE(spill_storage.segment_reroute_calls() == 1);
    REQUIRE(spill_storage.segment_merge_calls() == 5);
    require_complete_generation_rows(*encoded, spill_sink.rows);
}

TEST_CASE("actual producers preserve DNA5 N reset and packed tail",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::uint64_t fragment_bp = fragment_units * 21u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
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
    for (const std::size_t boundary : {static_cast<std::size_t>(fragment_bp - 1u),
                                       static_cast<std::size_t>(fragment_bp),
                                       target_size - 80u}) {
        data[boundary].assign_rank(3u);
        n_positions.push_back(boundary);
    }
    dna5_range_t range(data);
    REQUIRE(range.padding() != 0);

    std::vector<canonical_minimizer_event> oracle = baseline_dna5_events(data, 10, 15);
    std::ranges::sort(oracle, {}, [](const canonical_minimizer_event& event) {
        return std::pair{event.hash, event.encoded_position};
    });

    lazy_sequence<dna5_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto collection_range = spill_run_index_type::make_generation_collection_range(
        lazy_view, fragment_bp, fragment_bp + 100u);
    REQUIRE(collection_range.has_value());
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    REQUIRE(collection_range->cs_u == fragment_bp - halo);
    REQUIRE(collection_range->ce_u == fragment_bp + 100u + halo);
    REQUIRE(collection_range->core_lo_bp == fragment_bp);
    REQUIRE(collection_range->core_hi_bp == fragment_bp + 100u);
    auto fragment_producer =
        index_type::make_generation_replay_producer(lazy_view,
                                                    minimizer_sketch_backend::exact_simd,
                                                    fixed_minimizer_kernels(),
                                                    fixed_alphabet_code<::seq::dna5>());
    const auto collect_replay = [](void* context, const canonical_minimizer_event& event)
        -> std::expected<void, minimizer_generation_encoding_error> {
        static_cast<std::vector<canonical_minimizer_event>*>(context)->push_back(event);
        return {};
    };
    std::vector<canonical_minimizer_event> fragment_events;
    REQUIRE(fragment_producer
                .scan_sorted_events(k_minimizer_preferred_segment_span_bp,
                                    {.context = &fragment_events, .emit_fn = collect_replay})
                .has_value());

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
    const exact_simd_index_execution_stats spill_exact_stats = spilled->exact_simd_stats();
    auto spill_producer = spill_run_index_type::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spilled));
    std::vector<canonical_minimizer_event> spill_events;
    REQUIRE(spill_producer
                .scan_sorted_events(k_minimizer_preferred_segment_span_bp,
                                    {.context = &spill_events, .emit_fn = collect_replay})
                .has_value());
    REQUIRE(fragment_producer.exact_simd_stats_present());
    REQUIRE(fragment_producer.exact_simd_stats().is_internally_consistent(true));
    REQUIRE(fragment_producer.exact_simd_stats().ambiguous_symbols > 0);
    REQUIRE(spill_exact_stats.is_internally_consistent(true));
    REQUIRE(spill_exact_stats.ambiguous_symbols > 0);
    REQUIRE(spill_producer.merge_pass_count() == 1);
    REQUIRE(spill_storage.segment_merge_calls() == 2);
    REQUIRE(spill_storage.segment_merge_run_visits() ==
            2u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited() ==
            2u * spill_storage.segment_spill_bytes());
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
        const std::uint64_t first = last + 1u >= 15u ? last + 1u - 15u : 0u;
        REQUIRE(std::ranges::none_of(
            n_positions, [first, last](std::size_t n) { return first <= n && n <= last; }));
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
    const auto selected =
        plan_minimizer_generation(producer, test_generation_options(preferred), two_per_core);
    REQUIRE(selected.has_value());
    REQUIRE(selected->position_segment_span_bp == preferred / 2u);
    REQUIRE(producer.scan_count == 2);

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> indivisible{
        {9, 2u},
        {9, 3u},
    };
    const grouped_soa_generation_limits one_per_hash{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    vector_replay_minimizer_producer impossible_producer{indivisible};
    const auto impossible = plan_minimizer_generation(
        impossible_producer, test_generation_options(preferred), one_per_hash);
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
    storage.spill_sorted_segment_run(0,
                                     std::span<const std::pair<std::uint64_t, std::uint64_t>>{run});
    const std::uint64_t spill_bytes = storage.segment_spill_bytes();
    REQUIRE(spill_bytes == run.size() * sizeof(run.front()));

    direct_minimizer_unbound_external_spill_replay_producer<fake_fragment_storage<>> producer{
        storage, preferred, preferred, k_minimap_scheme_identity};
    collecting_minimizer_row_sink sink;
    const grouped_soa_generation_limits two_per_core{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(preferred), two_per_core);
    require_wire_error(encoded, minimizer_generation_encoding_error::storage_contract_mismatch);
    const minimizer_sequence_generation_execution_receipt execution = producer.execution_receipt();
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
        storage, preferred, preferred, k_minimap_scheme_identity};
    int receiver_context = 0;
    const auto rejected = producer.scan_sorted_events(
        preferred / 4u,
        canonical_minimizer_event_receiver{
            .context = &receiver_context,
            .emit_fn = +[](void*, const canonical_minimizer_event&)
                -> std::expected<void, minimizer_generation_encoding_error> { return {}; },
        });
    require_wire_error(rejected, minimizer_generation_encoding_error::segment_span);
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
        storage, 0, k_minimizer_preferred_segment_span_bp, k_minimap_scheme_identity};
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(producer, sink, test_generation_options(0));
    require_wire_error(encoded, minimizer_generation_encoding_error::storage_contract_mismatch);
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
        {7, 20},
        {7, 20},
    };
    vector_replay_minimizer_producer producer{duplicated};
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(producer, sink, test_generation_options(100));
    require_wire_error(encoded, minimizer_generation_encoding_error::duplicate_event);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("encoder rejects a drifting replay before READY publication",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    drifting_replay_minimizer_producer producer;
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(producer, sink, test_generation_options(100u));
    require_wire_error(encoded, minimizer_generation_encoding_error::replay_mismatch);
    REQUIRE(producer.scan_count == 2u);
    REQUIRE(std::ranges::none_of(
        sink.rows, [](const auto& row) { return row.address.sub_key == k_directory_sub_key; }));
}

TEST_CASE("grouped-SoA rejects scheme and backend drift at every scan boundary",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    for (const identity_drift_axis axis :
         {identity_drift_axis::scheme, identity_drift_axis::backend}) {
        for (const std::size_t boundary : {1u, 2u, 3u}) {
            INFO("axis=" << static_cast<unsigned>(axis) << " completed scan=" << boundary);
            boundary_identity_drifting_producer producer{
                .drift_after_scan = boundary,
                .axis = axis,
            };
            collecting_minimizer_row_sink sink;
            const auto encoded = encode_minimizer_generation(
                producer, sink, test_grouped_soa_generation_options(100u, 10u));
            require_wire_error(encoded, minimizer_generation_encoding_error::replay_mismatch);
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
    auto generation =
        encode_minimizer_generation(producer, sink, test_generation_options(target_length));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 2);
    const auto& heavy = generation->directory.entries()[0];
    REQUIRE(heavy.lo_hash == 42);
    REQUIRE(heavy.hi_hash == 42);
    REQUIRE(heavy.control_element_count == 1);
    REQUIRE(heavy.position_element_count == heavy_position_count);
    REQUIRE(heavy.position_encoded_bytes > k_minimizer_partition_target_bytes);
    REQUIRE(heavy.position_chunk_count > 1);
    const auto heavy_positions = collect_encoded_extent(sink.rows, heavy, false);
    REQUIRE(heavy_positions.size() ==
            heavy_position_count *
                static_cast<std::uint8_t>(generation->directory.profile().position_width));
    REQUIRE(generation->directory.entries()[1].lo_hash == 43);
    REQUIRE(sink.max_callback_bytes == k_grouped_soa_extent_payload_bytes);
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("grouped-SoA fragment and external-spill replays are identical",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 256u;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    const auto verify = [](std::vector<seq::dna4> data, bool require_offset_widening) {
        range_t range(data);
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        auto fragment_producer =
            index_type::make_generation_replay_producer(lazy_view,
                                                        minimizer_sketch_backend::exact_simd,
                                                        fixed_minimizer_kernels(),
                                                        fixed_alphabet_code<::seq::dna4>());
        collecting_minimizer_row_sink fragment_sink;
        auto fragment_options = test_grouped_soa_generation_options(range.bp_size(), 65'534u);
        const auto fragment_generation =
            encode_minimizer_generation(fragment_producer, fragment_sink, fragment_options);
        REQUIRE(fragment_generation.has_value());
        REQUIRE(fragment_producer.scan_count() == 3u);
        const auto fragment_stats = fragment_producer.execution_stats();
        INFO("grouped-SoA fragment evidence scans="
             << fragment_stats.producer_scan_count
             << " required_successful=" << fragment_stats.required_successful_scan_count()
             << " geometry_retries=" << fragment_stats.geometry_retry_count
             << " saturated=" << fragment_stats.saturated);
        REQUIRE(fragment_stats.generation_layout ==
                minimizer_generation_layout::grouped_soa_segment_local);
        REQUIRE(fragment_stats.required_successful_scan_count() == 3u);
        REQUIRE(fragment_stats.is_accepted());
        REQUIRE(fragment_stats.selected_backend == minimizer_sketch_backend::exact_simd);
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
        REQUIRE(spill_execution.selected_backend == minimizer_sketch_backend::exact_simd);
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
        const auto spill_generation =
            encode_minimizer_generation(spill_producer, spill_sink, spill_options);
        REQUIRE(spill_generation.has_value());
        REQUIRE(spill_producer.scan_count() == 3u);
        REQUIRE(spill_producer.merge_pass_count() == 3u);
        const auto spill_replay_stats = spill_producer.execution_receipt();
        REQUIRE(spill_replay_stats.generation_layout ==
                minimizer_generation_layout::grouped_soa_segment_local);
        REQUIRE(spill_replay_stats.required_successful_replay_count() == 3u);
        REQUIRE(spill_replay_stats.replay_scan_count == 3u);
        REQUIRE(spill_replay_stats.replay_completed_scan_count == 3u);
        REQUIRE(spill_replay_stats.replay_failed_scan_count == 0u);
        REQUIRE(spill_replay_stats.replay_reroute_pass_count == 0u);
        REQUIRE(spill_replay_stats.replay_visited_event_count ==
                3u * spill_replay_stats.spilled_event_count);
        require_encoded_generations_equal_normalized(
            *fragment_generation, fragment_sink.rows, *spill_generation, spill_sink.rows);
        REQUIRE(decode_generation_events(*fragment_generation, fragment_sink.rows) ==
                decode_generation_events(*spill_generation, spill_sink.rows));
        require_complete_generation_rows(*fragment_generation, fragment_sink.rows);
        require_complete_generation_rows(*spill_generation, spill_sink.rows);

        if (require_offset_widening) {
            REQUIRE(std::ranges::any_of(
                fragment_generation->directory.entries(), [](const auto& entry) {
                    return entry.shape_offset_width == minimizer_scalar_width::u32;
                }));
            REQUIRE(
                std::ranges::any_of(spill_generation->directory.entries(), [](const auto& entry) {
                    return entry.shape_offset_width == minimizer_scalar_width::u32;
                }));
        }

        const auto fourth_replay = spill_producer.scan_sorted_events(
            spill_generation->manifest.position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = nullptr,
                .emit_fn = [](void*, const canonical_minimizer_event&)
                    -> std::expected<void, minimizer_generation_encoding_error> { return {}; },
            });
        require_wire_error(fourth_replay, minimizer_generation_encoding_error::segment_span);
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
        for (auto& base : tandem)
            base.assign_rank(0u);
        verify(std::move(tandem), true);
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
