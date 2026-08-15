/**
 * @file test_support.cppm
 * @brief Provides fragment and spill producer fixtures for unit tests.
 */
module;

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

export module bioseq.tests.unit.indexing.minimizer_hashmap.fragment_producer_support;

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

export namespace fragment_producer_test_support {

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

} // namespace fragment_producer_test_support
} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
