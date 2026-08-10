/**
 * @file minimizer_hashmap.cppm
 * @brief Fragment minimizer index, grouped-SoA codecs, and readers.
 *
 * NO_POSTGRES. Corruption guards log an error and abort through
 * `index_corruption_panic`,
 * never elog/bioseq_log_pg(no-ops here). Storage-side validation goes through
 * `storage.validate_minimizer_*`/`storage.index_error` (duck-typed on S).
 */
module;
#include <libassert/assert.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <cstring>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
#include <utility>
export module bioseq.indexing.minimizer_hashmap;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;                                // nucleotide alphabet dispatch for the non-HWY ExactSimd bridge contract
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
export import bioseq.indexing.compact_hash32_direct_index;
export import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;                             // sequence_custom_alphabet and compact_sequence_range for bounded ntHash windows
import bioseq.sequences.kmers;                       // collect_minimizers_view, sketch_collector, minimap_sketch_data, minimizer_sketch_backend
export import bioseq.sequences.kmers.highway_minimizer_contract; // selected target's neutral minimizer table and packed-view ABI
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;                       // access-requirement policy
import bioseq.storage.lazy_sequence;                          // canonical lazy_sequence_view
import bioseq.storage.size;                          // sequence_size_t
import bioseq.chaining.core;              // anchor, canonical_anchor_less
import bioseq.common.radix_sort;                  // bioseq::sorting::radix_sort
import bioseq.instrumentation;                                 // instrumentation region scoping
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

namespace bioseq::indexing::detail {
struct minimizer_hashmap_fragment_index_table;
}

export namespace bioseq::indexing {
using bioseq::minimizers::minimap_sketch_data;
using bioseq::simd::collect_minimizers_view;
using bioseq::minimizers::sketch_collector;
using bioseq::minimizers::minimizer_sketch_backend;
using bioseq::storage::sequence_size_t;

// Target-neutral ABI between indexing interfaces and the selected generated
// minimizer table. The implementation unit maps checked family status/stats
// onto these indexing-owned records without importing a Highway target module.
enum class exact_simd_index_status : std::uint8_t {
    ok = 0,
    invalid_window_size,
    invalid_kmer_size,
    invalid_static_configuration,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    not_initialized,
    already_finalized,
    invalid_state,
    invalid_range,
    non_contiguous_range,
    arithmetic_overflow,
    collector_overflow,
};

[[nodiscard]] constexpr std::string_view exact_simd_index_status_name(
        exact_simd_index_status status) noexcept {
    switch (status) {
        case exact_simd_index_status::ok: return "ok";
        case exact_simd_index_status::invalid_window_size:
            return "invalid_window_size";
        case exact_simd_index_status::invalid_kmer_size:
            return "invalid_kmer_size";
        case exact_simd_index_status::invalid_static_configuration:
            return "invalid_static_configuration";
        case exact_simd_index_status::unsupported_alphabet:
            return "unsupported_alphabet";
        case exact_simd_index_status::unsupported_storage:
            return "unsupported_storage";
        case exact_simd_index_status::unsupported_representation:
            return "unsupported_representation";
        case exact_simd_index_status::unsupported_vector_shape:
            return "unsupported_vector_shape";
        case exact_simd_index_status::not_initialized:
            return "not_initialized";
        case exact_simd_index_status::already_finalized:
            return "already_finalized";
        case exact_simd_index_status::invalid_state:
            return "invalid_state";
        case exact_simd_index_status::invalid_range:
            return "invalid_range";
        case exact_simd_index_status::non_contiguous_range:
            return "non_contiguous_range";
        case exact_simd_index_status::arithmetic_overflow:
            return "arithmetic_overflow";
        case exact_simd_index_status::collector_overflow:
            return "collector_overflow";
    }
    return "unknown";
}

class exact_simd_index_runtime_error final : public std::runtime_error {
public:
    exact_simd_index_runtime_error(
            exact_simd_index_status status,
            std::string_view operation)
        : std::runtime_error(
              std::string{operation}
              + ": ExactSimd minimizer execution rejected: "
              + std::string{exact_simd_index_status_name(status)}),
          status_(status) {}

    [[nodiscard]] exact_simd_index_status status() const noexcept {
        return status_;
    }

private:
    exact_simd_index_status status_;
};

struct exact_simd_index_packed_fragment {
    const void* data = nullptr;
    std::size_t storage_units = 0;
};

using exact_simd_index_read_fragment =
    exact_simd_index_packed_fragment (*)(void*, std::size_t);
using exact_simd_index_release_fragment = void (*)(void*, std::size_t) noexcept;
using exact_simd_index_append_records =
    bool (*)(void*, const minimap_sketch_data*, std::size_t);
using exact_simd_index_append_raw =
    bool (*)(void*, const std::uint64_t*, std::size_t);

struct exact_simd_index_bridge_request {
    const bioseq::minimizers::minimizer_kernels* kernels = nullptr;
    void* sequence_context = nullptr;
    exact_simd_index_read_fragment read_fragment = nullptr;
    exact_simd_index_release_fragment release_fragment = nullptr;
    void* collector_context = nullptr;
    exact_simd_index_append_records append_records = nullptr;
    exact_simd_index_append_raw append_raw = nullptr;
    std::uint8_t alphabet_code =
        bioseq::minimizers::unbound_packed_alphabet_code;
    std::uint8_t bits_per_symbol = 0u;
    std::size_t storage_unit_bytes = 0;
    std::size_t letters_per_unit = 0;
    std::size_t total_units = 0;
    std::size_t total_bp = 0;
    std::size_t fragment_units = 0;
    std::size_t fragment_count = 0;
    std::size_t lo_unit = 0;
    std::size_t hi_unit = 0;
    std::size_t window_size = 0;
    std::size_t kmer_size = 0;
    std::size_t emitted_position_base = 0;
};

struct exact_simd_index_execution_stats {
    std::uint64_t bp_consumed = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t vector_batches = 0;
    std::uint64_t masked_tail_lanes = 0;
    std::uint64_t packed_decode_batches = 0;
    std::uint64_t packed_decode_lanes = 0;
    std::uint64_t ambiguous_symbols = 0;
    std::uint64_t ambiguous_reset_batches = 0;
    std::uint64_t scalar_backend_branches = 0;
    std::uint64_t scalar_tail_symbols = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_event_records = 0;
    std::uint64_t scalar_sort_records = 0;
    std::uint64_t scalar_dedup_records = 0;
    std::uint64_t fail_closed_rejections = 0;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const exact_simd_index_execution_stats&) const noexcept = default;

    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    void merge(const exact_simd_index_execution_stats& other) noexcept {
        saturated = saturated || other.saturated;
        add(bp_consumed, other.bp_consumed, saturated);
        add(minimizers_emitted, other.minimizers_emitted, saturated);
        add(vector_batches, other.vector_batches, saturated);
        add(masked_tail_lanes, other.masked_tail_lanes, saturated);
        add(packed_decode_batches, other.packed_decode_batches, saturated);
        add(packed_decode_lanes, other.packed_decode_lanes, saturated);
        add(ambiguous_symbols, other.ambiguous_symbols, saturated);
        add(ambiguous_reset_batches, other.ambiguous_reset_batches, saturated);
        add(scalar_backend_branches, other.scalar_backend_branches, saturated);
        add(scalar_tail_symbols, other.scalar_tail_symbols, saturated);
        add(scalar_hash_symbols, other.scalar_hash_symbols, saturated);
        add(scalar_window_transitions, other.scalar_window_transitions, saturated);
        add(scalar_event_records, other.scalar_event_records, saturated);
        add(scalar_sort_records, other.scalar_sort_records, saturated);
        add(scalar_dedup_records, other.scalar_dedup_records, saturated);
        add(fail_closed_rejections, other.fail_closed_rejections, saturated);
    }

    [[nodiscard]] bool is_internally_consistent(
            bool require_nonempty_vector_work = false) const noexcept {
        const bool exact = !saturated
            && fail_closed_rejections == 0
            && scalar_backend_branches == 0
            && scalar_tail_symbols == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_event_records == 0
            && scalar_sort_records == 0
            && scalar_dedup_records == 0
            && packed_decode_lanes == bp_consumed;
        return exact && (!require_nonempty_vector_work
            || (bp_consumed > 0 && vector_batches > 0
                && packed_decode_batches > 0));
    }
};

struct exact_simd_index_bridge_result {
    exact_simd_index_status status = exact_simd_index_status::ok;
    exact_simd_index_execution_stats stats{};
};

[[nodiscard]] exact_simd_index_bridge_result
collect_exact_simd_index_erased(const exact_simd_index_bridge_request& request);

template<class Collector, sequence_alphabet Alphabet>
[[nodiscard]] inline exact_simd_index_execution_stats
collect_exact_simd_index_range_or_throw(
        Collector& collector,
        bioseq::storage::lazy_sequence_view<Alphabet> sequence,
        std::uint8_t alphabet_code,
        sequence_size_t lo_bp,
        sequence_size_t hi_bp,
        std::size_t runtime_w,
        std::size_t runtime_k,
        std::size_t emitted_position_base,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::string_view operation) {
    static_assert(std::same_as<
        typename minimap_sketch_data::storage_type, std::uint64_t>);

    if constexpr (!std::same_as<Alphabet, seq::dna4>
                  && !std::same_as<Alphabet, seq::dna5>) {
        throw exact_simd_index_runtime_error{
            exact_simd_index_status::unsupported_alphabet, operation};
    } else {
        if (lo_bp > hi_bp || hi_bp > sequence.bp_size()
            || hi_bp - lo_bp
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_range, operation};
        }
        if (kernels == nullptr || kernels->two_stack_nthash == nullptr) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_state, operation};
        }
        constexpr std::uint8_t packed_width = sizeof(std::uint32_t);
        const auto access = bioseq::storage::plan_sequence_access(
            sequence,
            bioseq::storage::sequence_access_requirement::
                contiguous_resident,
            lo_bp,
            hi_bp,
            packed_width);
        std::optional<bioseq::storage::admitted_packed_sequence<Alphabet>> owner;
        typename bioseq::storage::lazy_sequence_view<Alphabet>::
            resident_packed_sequence descriptor{};
        if (access.route
                == bioseq::storage::sequence_access_route::borrow_resident
            && access.resident.has_value()) {
            descriptor = *access.resident;
        } else {
            auto permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(access.admission_bytes);
            if (!permit) {
                throw exact_simd_index_runtime_error{
                    exact_simd_index_status::non_contiguous_range, operation};
            }
            owner.emplace(bioseq::storage::materialize_packed_sequence(
                sequence, lo_bp, hi_bp, packed_width,
                std::move(*permit)));
            descriptor = owner->descriptor();
        }
        if (descriptor.logical_length
                != static_cast<std::size_t>(hi_bp - lo_bp)
            || descriptor.storage_unit_width != packed_width
            || descriptor.bits_per_symbol
                != bioseq::storage::lazy_sequence_traits<Alphabet>::
                    logical_bits_per_symbol
            || descriptor.leading_symbol_offset != 0u
            || descriptor.bit_order
                != bioseq::storage::lazy_sequence_view<Alphabet>::
                    packed_symbol_order::least_significant_symbol_first
            || (descriptor.logical_length != 0u && descriptor.units == nullptr)) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_range, operation};
        }

        struct sequence_context {
            const void* data{};
            std::size_t unit_count{};
        } sequence_context{descriptor.units, descriptor.unit_count};

        const auto read_fragment = +[](void* opaque, std::size_t fragment_no) {
            const auto& context = *static_cast<sequence_context*>(opaque);
            if (fragment_no != 0u || context.unit_count == 0u) {
                return exact_simd_index_packed_fragment{};
            }
            return exact_simd_index_packed_fragment{
                .data = context.data,
                .storage_units = context.unit_count,
            };
        };
        const auto release_fragment = +[](void*, std::size_t) noexcept {
        };
        const auto append_records = +[](void* opaque,
                                        const minimap_sketch_data* records,
                                        std::size_t count) {
            auto& collector = *static_cast<Collector*>(opaque);
            if constexpr (requires {
                { collector.append_records(
                    std::span<const minimap_sketch_data>{records, count}) }
                    -> std::convertible_to<bool>;
            }) {
                return static_cast<bool>(collector.append_records(
                    std::span<const minimap_sketch_data>{records, count}));
            } else if constexpr (requires (const minimap_sketch_data& record) {
                collector.collect(record);
            }) {
                for (std::size_t index = 0u; index < count; ++index) {
                    collector.collect(records[index]);
                }
                if constexpr (requires {
                    { std::as_const(collector).ok() }
                        -> std::convertible_to<bool>;
                }) {
                    return static_cast<bool>(std::as_const(collector).ok());
                }
                return true;
            } else {
                return false;
            }
        };
        const auto append_raw = +[](void* opaque,
                                    const std::uint64_t* raw,
                                    std::size_t count) {
            auto& collector = *static_cast<Collector*>(opaque);
            if constexpr (requires {
                { collector.append_raw_storage(raw, count) }
                    -> std::convertible_to<bool>;
            }) {
                return static_cast<bool>(
                    collector.append_raw_storage(raw, count));
            } else if constexpr (requires (const minimap_sketch_data& record) {
                collector.collect(record);
            }) {
                for (std::size_t index = 0u; index < count; ++index) {
                    collector.collect(
                        minimap_sketch_data::from_raw_storage(raw[index]));
                }
                if constexpr (requires {
                    { std::as_const(collector).ok() }
                        -> std::convertible_to<bool>;
                }) {
                    return static_cast<bool>(std::as_const(collector).ok());
                }
                return true;
            } else {
                return false;
            }
        };

        const exact_simd_index_bridge_result result = collect_exact_simd_index_erased(
            exact_simd_index_bridge_request{
                .kernels = kernels,
                .sequence_context = &sequence_context,
                .read_fragment = read_fragment,
                .release_fragment = release_fragment,
                .collector_context = &collector,
                .append_records = append_records,
                .append_raw = append_raw,
                .alphabet_code = alphabet_code,
                .bits_per_symbol = descriptor.bits_per_symbol,
                .storage_unit_bytes = packed_width,
                .letters_per_unit = packed_width * 8u
                    / bioseq::storage::lazy_sequence_traits<Alphabet>::
                        logical_bits_per_symbol,
                .total_units = descriptor.unit_count,
                .total_bp = static_cast<std::size_t>(hi_bp - lo_bp),
                .fragment_units = std::max<std::size_t>(
                    descriptor.unit_count, 1u),
                .fragment_count = descriptor.unit_count == 0u ? 0u : 1u,
                .lo_unit = 0u,
                .hi_unit = descriptor.unit_count,
                .window_size = runtime_w,
                .kmer_size = runtime_k,
                .emitted_position_base = emitted_position_base,
            });
        if (result.status != exact_simd_index_status::ok) {
            throw exact_simd_index_runtime_error{result.status, operation};
        }
        return result.stats;
    }
}

// Plain ABI mirror of the packed bounded ntHash checked kernel plus one
// facade-owned admission status. The implementation unit imports the Highway
// owner and maps its native status/stats without allowing the indexing
// interface to instantiate or reimplement the algorithm.
enum class packed_bounded_nthash_index_status : std::uint8_t {
    ok = 0,
    invalid_configuration,
    input_too_short,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    invalid_range,
    input_too_large,
    invalid_state,
    admission_denied,
};

[[nodiscard]] constexpr std::string_view
packed_bounded_nthash_index_status_name(
        packed_bounded_nthash_index_status status) noexcept {
    switch (status) {
        case packed_bounded_nthash_index_status::ok: return "ok";
        case packed_bounded_nthash_index_status::invalid_configuration:
            return "invalid_configuration";
        case packed_bounded_nthash_index_status::input_too_short:
            return "input_too_short";
        case packed_bounded_nthash_index_status::unsupported_alphabet:
            return "unsupported_alphabet";
        case packed_bounded_nthash_index_status::unsupported_storage:
            return "unsupported_storage";
        case packed_bounded_nthash_index_status::unsupported_representation:
            return "unsupported_representation";
        case packed_bounded_nthash_index_status::unsupported_vector_shape:
            return "unsupported_vector_shape";
        case packed_bounded_nthash_index_status::invalid_range:
            return "invalid_range";
        case packed_bounded_nthash_index_status::input_too_large:
            return "input_too_large";
        case packed_bounded_nthash_index_status::invalid_state:
            return "invalid_state";
        case packed_bounded_nthash_index_status::admission_denied:
            return "admission_denied";
    }
    return "unknown";
}

class packed_bounded_nthash_index_runtime_error final : public std::runtime_error {
public:
    packed_bounded_nthash_index_runtime_error(
            packed_bounded_nthash_index_status status,
            std::string_view operation)
        : std::runtime_error(
            std::string(operation) + ": checked bounded ntHash route failed ("
                + std::string(packed_bounded_nthash_index_status_name(status))
                + ")"),
          status_(status) {}

    [[nodiscard]] packed_bounded_nthash_index_status status() const noexcept {
        return status_;
    }

private:
    packed_bounded_nthash_index_status status_;
};

// Integration-build-only one-shot. The callable surface is present in every
// module variant so BMIs remain identical, but production builds never retain
// state and therefore cannot force a biological result.
#ifdef BIOSEQ_TEST_INDEXES
inline std::atomic<std::uint8_t> g_forced_packed_bounded_index_status{
    static_cast<std::uint8_t>(packed_bounded_nthash_index_status::ok)};
#endif

inline void arm_forced_packed_bounded_index_status_for_test(
        packed_bounded_nthash_index_status status) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    g_forced_packed_bounded_index_status.store(
        static_cast<std::uint8_t>(status), std::memory_order_release);
#else
    (void)status;
#endif
}

inline void disarm_forced_packed_bounded_index_status_for_test() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    g_forced_packed_bounded_index_status.store(
        static_cast<std::uint8_t>(packed_bounded_nthash_index_status::ok),
        std::memory_order_release);
#endif
}

[[nodiscard]] inline std::optional<packed_bounded_nthash_index_status>
consume_forced_packed_bounded_index_status_for_test() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    const auto status = static_cast<packed_bounded_nthash_index_status>(
        g_forced_packed_bounded_index_status.exchange(
            static_cast<std::uint8_t>(packed_bounded_nthash_index_status::ok),
            std::memory_order_acq_rel));
    if (status != packed_bounded_nthash_index_status::ok) return status;
#endif
    return std::nullopt;
}

inline constexpr std::uint32_t k_packed_bounded_nthash_kernel_scheme_id =
    0x504E5431u;
inline constexpr std::size_t k_packed_bounded_nthash_maximum_window = 64u;

/** Full non-Highway copy of the real checked kernel's execution stats. */
struct packed_bounded_nthash_index_execution_stats {
    std::uint32_t scheme_id = 0;
    std::uint32_t kmer_size = 0;
    std::uint32_t window_size = 0;
    std::uint32_t nthash_rotation = 0;
    std::uint32_t source_storage_unit_bytes = 0;
    std::uint64_t input_symbols = 0;
    std::uint64_t input_storage_units = 0;
    std::uint64_t segments_combined = 0;
    std::uint64_t logical_streams = 0;
    std::uint64_t physical_vector_lanes = 0;
    std::uint64_t vector_registers_per_step = 0;
    std::uint64_t packed_decode_batches = 0;
    std::uint64_t packed_decode_lane_visits = 0;
    std::uint64_t hash_initialization_batches = 0;
    std::uint64_t rolling_hash_batches = 0;
    std::uint64_t hash_table_lookup_batches = 0;
    std::uint64_t canonical_sum_batches = 0;
    std::uint64_t window_update_batches = 0;
    std::uint64_t suffix_rebuild_batches = 0;
    std::uint64_t emission_decision_batches = 0;
    std::uint64_t emission_mask_store_batches = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t tied_endpoint_records_emitted = 0;
    std::uint64_t stream_boundary_records_buffered = 0;
    std::uint64_t stream_boundary_records_committed = 0;
    std::uint64_t stream_boundary_duplicates_suppressed = 0;
    std::uint64_t scalar_collector_appends = 0;
    std::uint64_t partial_chunk_callback_batches = 0;
    std::uint64_t masked_tail_lane_visits = 0;
    std::uint64_t fail_closed_rejections = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_minimum_decisions = 0;
    std::uint64_t fallback_calls = 0;
    bool positions_are_segment_local = false;
    bool aggregation_mismatch = false;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const packed_bounded_nthash_index_execution_stats&) const noexcept = default;

    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    /** Combine independently checked bounded windows without losing stats. */
    void merge(
            const packed_bounded_nthash_index_execution_stats& other) noexcept {
        if (other == packed_bounded_nthash_index_execution_stats{}) return;
        if (*this == packed_bounded_nthash_index_execution_stats{}) {
            *this = other;
            return;
        }
        const bool same_identity = segments_combined > 0
            && other.segments_combined > 0
            && scheme_id == other.scheme_id
            && kmer_size == other.kmer_size
            && window_size == other.window_size
            && nthash_rotation == other.nthash_rotation
            && source_storage_unit_bytes
                == other.source_storage_unit_bytes
            && logical_streams == other.logical_streams
            && physical_vector_lanes == other.physical_vector_lanes
            && vector_registers_per_step
                == other.vector_registers_per_step
            && positions_are_segment_local
                == other.positions_are_segment_local;
        aggregation_mismatch = aggregation_mismatch
            || other.aggregation_mismatch || !same_identity;
        saturated = saturated || other.saturated;
        add(input_symbols, other.input_symbols, saturated);
        add(input_storage_units, other.input_storage_units, saturated);
        add(segments_combined, other.segments_combined, saturated);
        add(packed_decode_batches, other.packed_decode_batches, saturated);
        add(packed_decode_lane_visits,
            other.packed_decode_lane_visits, saturated);
        add(hash_initialization_batches,
            other.hash_initialization_batches, saturated);
        add(rolling_hash_batches, other.rolling_hash_batches, saturated);
        add(hash_table_lookup_batches,
            other.hash_table_lookup_batches, saturated);
        add(canonical_sum_batches, other.canonical_sum_batches, saturated);
        add(window_update_batches, other.window_update_batches, saturated);
        add(suffix_rebuild_batches, other.suffix_rebuild_batches, saturated);
        add(emission_decision_batches,
            other.emission_decision_batches, saturated);
        add(emission_mask_store_batches,
            other.emission_mask_store_batches, saturated);
        add(minimizers_emitted, other.minimizers_emitted, saturated);
        add(tied_endpoint_records_emitted,
            other.tied_endpoint_records_emitted, saturated);
        add(stream_boundary_records_buffered,
            other.stream_boundary_records_buffered, saturated);
        add(stream_boundary_records_committed,
            other.stream_boundary_records_committed, saturated);
        add(stream_boundary_duplicates_suppressed,
            other.stream_boundary_duplicates_suppressed, saturated);
        add(scalar_collector_appends,
            other.scalar_collector_appends, saturated);
        add(partial_chunk_callback_batches,
            other.partial_chunk_callback_batches, saturated);
        add(masked_tail_lane_visits,
            other.masked_tail_lane_visits, saturated);
        add(fail_closed_rejections,
            other.fail_closed_rejections, saturated);
        add(scalar_hash_symbols, other.scalar_hash_symbols, saturated);
        add(scalar_window_transitions,
            other.scalar_window_transitions, saturated);
        add(scalar_minimum_decisions,
            other.scalar_minimum_decisions, saturated);
        add(fallback_calls, other.fallback_calls, saturated);
    }

    [[nodiscard]] constexpr bool is_internally_consistent(
            bool require_nonempty_vector_work = true) const noexcept {
        const bool vector_work_ok = !require_nonempty_vector_work
            || (packed_decode_batches > 0
                && packed_decode_lane_visits > 0
                && hash_initialization_batches > 0
                && hash_table_lookup_batches > 0
                && canonical_sum_batches > 0
                && window_update_batches > 0
                && emission_decision_batches > 0
                && emission_mask_store_batches > 0);
        return !saturated
            && !aggregation_mismatch
            && scheme_id == k_packed_bounded_nthash_kernel_scheme_id
            && kmer_size > 0
            && window_size > 0
            && window_size <= k_packed_bounded_nthash_maximum_window
            && nthash_rotation == 7
            && source_storage_unit_bytes >= sizeof(std::uint32_t)
            && logical_streams == 8
            && (physical_vector_lanes == 4
                || physical_vector_lanes == 8)
            && vector_registers_per_step * physical_vector_lanes == 8
            && segments_combined > 0
            && positions_are_segment_local
            && vector_work_ok
            && canonical_sum_batches == window_update_batches
            && minimizers_emitted == scalar_collector_appends
            && stream_boundary_records_buffered
                == stream_boundary_records_committed
                    + stream_boundary_duplicates_suppressed
            && fail_closed_rejections == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_minimum_decisions == 0
            && fallback_calls == 0;
    }
};

struct packed_bounded_nthash_index_bridge_request {
    const bioseq::minimizers::minimizer_kernels* kernels = nullptr;
    bioseq::minimizers::packed_sequence_kernel_view sequence{};
    void* sequence_context = nullptr;
    void* collector_context = nullptr;
    std::size_t storage_unit_bytes = 0;
    std::size_t letters_per_unit = 0;
    std::size_t storage_units = 0;
    std::size_t local_bp_size = 0;
    std::size_t window_size = 0;
    std::size_t kmer_size = 0;
};

struct packed_bounded_nthash_index_bridge_result {
    packed_bounded_nthash_index_status status = packed_bounded_nthash_index_status::ok;
    packed_bounded_nthash_index_execution_stats stats{};
};

[[nodiscard]] constexpr packed_bounded_nthash_index_bridge_result
rejected_packed_bounded_nthash_index_result(
        packed_bounded_nthash_index_status status) noexcept {
    return {
        .status = status,
        .stats = packed_bounded_nthash_index_execution_stats{
            .fail_closed_rejections = 1,
        },
    };
}

[[nodiscard]] packed_bounded_nthash_index_bridge_result
collect_packed_bounded_nthash_index_erased(
    const packed_bounded_nthash_index_bridge_request& request);

/// Execute one bounded ntHash window through a canonical admitted u32
/// descriptor. Only the selected logical-rank interval is materialized when the source
/// cannot expose stable resident u32 storage; a stable resident interval is
/// borrowed synchronously without allocating or copying it.
template <sequence_alphabet Alphabet>
[[nodiscard]] packed_bounded_nthash_index_bridge_result
collect_packed_bounded_nthash_index_lazy(
        bioseq::storage::lazy_sequence_view<Alphabet> sequence,
        std::uint8_t alphabet_code,
        sequence_size_t lo_bp,
        sequence_size_t hi_bp,
        std::uint64_t local_bp_size,
        std::uint16_t window_size,
        std::uint16_t kmer_size,
        const bioseq::minimizers::minimizer_kernels* kernels,
        sketch_collector& collector,
        std::uint64_t* materialized_window_bp_out = nullptr) {
    if (materialized_window_bp_out != nullptr) {
        *materialized_window_bp_out = 0u;
    }
    if constexpr (!std::same_as<Alphabet, seq::dna4>) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::unsupported_alphabet);
    } else {
        if (lo_bp > hi_bp || hi_bp > sequence.bp_size()
            || local_bp_size != static_cast<std::uint64_t>(hi_bp - lo_bp)) {
            return rejected_packed_bounded_nthash_index_result(
                packed_bounded_nthash_index_status::invalid_range);
        }
        if (local_bp_size == 0u
            || local_bp_size - 1u
                > static_cast<std::uint64_t>(
                    minimap_sketch_data::k_position_field_mask)) {
            return rejected_packed_bounded_nthash_index_result(
                packed_bounded_nthash_index_status::unsupported_representation);
        }
        if (local_bp_size
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
            return rejected_packed_bounded_nthash_index_result(
                packed_bounded_nthash_index_status::input_too_large);
        }
        if (kernels == nullptr || kernels->bounded_nthash == nullptr) {
            return rejected_packed_bounded_nthash_index_result(
                packed_bounded_nthash_index_status::invalid_state);
        }

        constexpr std::uint8_t packed_width = sizeof(std::uint32_t);
        const auto access = bioseq::storage::plan_sequence_access(
            sequence,
            bioseq::storage::sequence_access_requirement::
                contiguous_resident,
            lo_bp,
            hi_bp,
            packed_width);
        std::optional<bioseq::storage::admitted_packed_sequence<Alphabet>> owner;
        typename bioseq::storage::lazy_sequence_view<Alphabet>::
            resident_packed_sequence descriptor{};
        if (access.route
                == bioseq::storage::sequence_access_route::borrow_resident
            && access.resident.has_value()) {
            descriptor = *access.resident;
        } else {
            auto permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(access.admission_bytes);
            if (!permit) {
                return rejected_packed_bounded_nthash_index_result(
                    packed_bounded_nthash_index_status::admission_denied);
            }
            owner.emplace(bioseq::storage::materialize_packed_sequence(
                sequence, lo_bp, hi_bp, packed_width,
                std::move(*permit)));
            descriptor = owner->descriptor();
        }
        if (descriptor.logical_length != local_bp_size
            || descriptor.storage_unit_width != packed_width
            || descriptor.bits_per_symbol
                != bioseq::storage::lazy_sequence_traits<Alphabet>::
                    logical_bits_per_symbol
            || descriptor.leading_symbol_offset != 0u
            || descriptor.bit_order
                != bioseq::storage::lazy_sequence_view<Alphabet>::
                    packed_symbol_order::least_significant_symbol_first
            || (descriptor.logical_length != 0u && descriptor.units == nullptr)) {
            return rejected_packed_bounded_nthash_index_result(
                packed_bounded_nthash_index_status::invalid_range);
        }
        if (materialized_window_bp_out != nullptr) {
            *materialized_window_bp_out = owner.has_value()
                ? local_bp_size : 0u;
            DEBUG_ASSERT(
                (*materialized_window_bp_out != 0u) == owner.has_value(),
                "bounded materialization receipt must name only owned storage");
        }
        if (const auto forced =
                consume_forced_packed_bounded_index_status_for_test()) {
            return rejected_packed_bounded_nthash_index_result(*forced);
        }
        return collect_packed_bounded_nthash_index_erased(
            packed_bounded_nthash_index_bridge_request{
                .kernels = kernels,
                .sequence = bioseq::minimizers::packed_sequence_kernel_view{
                    .storage = reinterpret_cast<const std::byte*>(
                        descriptor.units),
                    .storage_units = descriptor.unit_count,
                    .begin_symbol = 0u,
                    .end_symbol = static_cast<std::size_t>(local_bp_size),
                    .alphabet_code = alphabet_code,
                    .bits_per_symbol = descriptor.bits_per_symbol,
                    .storage_unit_bytes =
                        descriptor.storage_unit_width,
                    .final_segment = true,
                },
                .sequence_context = std::addressof(descriptor),
                .collector_context = std::addressof(collector),
                .storage_unit_bytes = packed_width,
                .letters_per_unit = descriptor.storage_unit_width * 8u
                    / bioseq::storage::lazy_sequence_traits<Alphabet>::
                        logical_bits_per_symbol,
                .storage_units = descriptor.unit_count,
                .local_bp_size = static_cast<std::size_t>(local_bp_size),
                .window_size = window_size,
                .kmer_size = kmer_size,
            });
    }
}

// Current persisted minimizer codecs stay in this wire owner. Their shared
// CRC implementation is imported from the lower chunk-storage module so row,
// manifest, fake, and future PostgreSQL paths cannot drift.

/// Canonical minimap-style hash/tie/strand semantics encoded by current.
inline constexpr std::uint32_t k_minimizer_hash_semantics_revision = 1u;

/// Segment-local `(local_bp << 1) | strand` persisted encoding semantics.
inline constexpr std::uint32_t k_minimizer_segment_local_position_encoding_revision = 2u;
inline constexpr std::uint16_t k_grouped_soa_payload_layout_abi_revision = 1u;
inline constexpr std::uint32_t k_grouped_soa_gpu_abi_revision = 3u;
inline constexpr std::uint32_t k_grouped_soa_extension_magic = 0x42494f53u;
inline constexpr std::uint16_t k_grouped_soa_extension_format_revision = 1u;
inline constexpr std::uint16_t k_grouped_soa_extension_bytes = 32u;
/// Persisted supported_alphabets pack index for DNA4. This is an on-wire code,
/// not the alphabet cardinality.
inline constexpr std::uint16_t k_minimizer_persisted_dna4_alphabet_code = 0u;
inline constexpr std::uint32_t k_legacy_minimap_k15_max_hash = 0x3fffffffu;

// Persisted minimizer scheme identities are frozen wire values. The minimap
// original minimap scheme remains the all-zero pair, independently of the mandatory
// producer identity now stored in bytes 126/127.
inline constexpr std::uint16_t k_legacy_minimap_scheme_id = 0u;
inline constexpr std::uint16_t k_legacy_minimap_scheme_revision = 0u;
inline constexpr std::uint16_t k_bounded_nthash_u32_r7_sum_scheme_id = 1u;
inline constexpr std::uint16_t k_bounded_nthash_u32_r7_sum_scheme_revision = 1u;
inline constexpr std::uint16_t k_invalid_minimizer_scheme_id =
    (std::numeric_limits<std::uint16_t>::max)();
inline constexpr std::uint16_t k_invalid_minimizer_scheme_revision =
    (std::numeric_limits<std::uint16_t>::max)();

/// Frozen biological scheme identity shared by producers, writers and readers.
struct minimizer_scheme_identity {
    std::uint16_t id = k_invalid_minimizer_scheme_id;
    std::uint16_t revision = k_invalid_minimizer_scheme_revision;

    [[nodiscard]] bool operator==(const minimizer_scheme_identity&) const = default;
};

/// Storage-issued incarnation of one MPS canonical replay set.  The token
/// changes on every begin operation; the revision changes whenever a caller can
/// alter the qualified logical replay (including rerouting or physical-run
/// injection). Internal compaction that preserves the exact canonical replay
/// is representation-only and leaves this binding stable. A zero token is
/// never authoritative.
struct direct_minimizer_spill_storage_binding {
    std::uint64_t generation_token = 0;
    std::uint64_t mutation_revision = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation_token != 0;
    }

    [[nodiscard]] constexpr bool operator==(
        const direct_minimizer_spill_storage_binding&) const noexcept = default;
};

inline constexpr minimizer_scheme_identity k_legacy_minimap_scheme_identity{
    .id = k_legacy_minimap_scheme_id,
    .revision = k_legacy_minimap_scheme_revision,
};
inline constexpr minimizer_scheme_identity k_bounded_nthash_u32_r7_sum_scheme_identity{
    .id = k_bounded_nthash_u32_r7_sum_scheme_id,
    .revision = k_bounded_nthash_u32_r7_sum_scheme_revision,
};

/// Frozen physical producer identifiers persisted in manifest byte 126.
/// These values identify the implementation that actually produced the
/// canonical event stream; they are intentionally independent of biological
/// scheme identity because persisted Scalar, chronological Exact SIMD, and
/// Vulkan currently share the minimap scheme. The full-resident
/// vector_stream ExactSimd route is not compatible with chronological/B8
/// selection and must never be persisted or consumed under this identity.
/// Giving that optional resident route a wire identity would require a format
/// revision; until then pipeline provenance confines it to paired transient
/// no-index work and persisted consumers require chronological stats.
enum class minimizer_producer_backend : std::uint8_t {
    invalid = 0u,
    scalar = 1u,
    exact_simd = 2u,
    bounded_nthash_simd = 3u,
    vulkan_segment = 4u,
};

/// Revision of the producer identity namespace persisted in manifest byte 127.
inline constexpr std::uint8_t k_minimizer_producer_identity_revision = 1u;

struct minimizer_producer_identity {
    minimizer_producer_backend id = minimizer_producer_backend::invalid;
    std::uint8_t revision = 0u;

    [[nodiscard]] bool operator==(
        const minimizer_producer_identity&) const = default;
};

[[nodiscard]] constexpr bool is_supported_minimizer_producer_backend_identity(
        minimizer_producer_identity identity) noexcept {
    if (identity.revision != k_minimizer_producer_identity_revision) {
        return false;
    }
    switch (identity.id) {
        case minimizer_producer_backend::scalar:
        case minimizer_producer_backend::exact_simd:
        case minimizer_producer_backend::bounded_nthash_simd:
        case minimizer_producer_backend::vulkan_segment:
            return true;
        case minimizer_producer_backend::invalid:
            return false;
    }
    return false;
}

/// Bind a runtime route to its frozen physical producer identity. Adding a
/// route without extending this switch is fail-closed.
[[nodiscard]] constexpr minimizer_producer_identity
minimizer_producer_backend_identity_for_backend(
        minimizer_sketch_backend backend) noexcept {
    const auto identity = [backend] {
        switch (backend) {
            case minimizer_sketch_backend::scalar:
                return minimizer_producer_backend::scalar;
            case minimizer_sketch_backend::two_stack_nthash:
                return minimizer_producer_backend::exact_simd;
            case minimizer_sketch_backend::bounded_nthash_simd:
                return minimizer_producer_backend::bounded_nthash_simd;
            case minimizer_sketch_backend::vulkan_segment:
                return minimizer_producer_backend::vulkan_segment;
        }
        return minimizer_producer_backend::invalid;
    }();
    return identity == minimizer_producer_backend::invalid
        ? minimizer_producer_identity{}
        : minimizer_producer_identity{
            .id = identity,
            .revision = k_minimizer_producer_identity_revision,
        };
}

/// Return whether an id/revision pair names one frozen current scheme.
[[nodiscard]] constexpr bool is_supported_minimizer_scheme_identity(
        std::uint16_t scheme_id,
        std::uint16_t scheme_revision) noexcept {
    return (scheme_id == k_legacy_minimap_scheme_id
            && scheme_revision == k_legacy_minimap_scheme_revision)
        || (scheme_id == k_bounded_nthash_u32_r7_sum_scheme_id
            && scheme_revision == k_bounded_nthash_u32_r7_sum_scheme_revision);
}

/// Bind every persistable sketch producer to its persisted semantics. ExactSimd
/// here means its chronological continuation/index producer, not the optional
/// paired-resident vector_stream sub-route. Adding a backend without extending
/// this switch is a fail-closed operation.
[[nodiscard]] constexpr minimizer_scheme_identity
minimizer_scheme_identity_for_backend(
        minimizer_sketch_backend backend) noexcept {
    switch (backend) {
        case minimizer_sketch_backend::scalar:
        case minimizer_sketch_backend::vulkan_segment:
        case minimizer_sketch_backend::two_stack_nthash:
            return k_legacy_minimap_scheme_identity;
        case minimizer_sketch_backend::bounded_nthash_simd:
            return k_bounded_nthash_u32_r7_sum_scheme_identity;
    }
    return {};
}

/// Biological scheme implied by one frozen physical producer identity. This
/// preserves the existing scheme mapping while preventing shared-scheme
/// backends from relabelling one another.
[[nodiscard]] constexpr minimizer_scheme_identity
minimizer_scheme_identity_for_producer_backend(
        minimizer_producer_identity identity) noexcept {
    if (!is_supported_minimizer_producer_backend_identity(identity)) {
        return {};
    }
    switch (identity.id) {
        case minimizer_producer_backend::scalar:
        case minimizer_producer_backend::exact_simd:
        case minimizer_producer_backend::vulkan_segment:
            return k_legacy_minimap_scheme_identity;
        case minimizer_producer_backend::bounded_nthash_simd:
            return k_bounded_nthash_u32_r7_sum_scheme_identity;
        case minimizer_producer_backend::invalid:
            break;
    }
    return {};
}

/// Uniform fixed-core segment geometry plus disjoint control/position extent
/// namespaces encoded in current manifests.
inline constexpr std::uint16_t k_minimizer_segment_geometry_revision = 2u;

/// Preferred immutable core span; overflow retries halve this power of two.
inline constexpr std::uint64_t k_minimizer_preferred_segment_span_bp = 8u * 1024u * 1024u;

/// Deterministic hash-partition payload target used by the initial encoder POC.
inline constexpr std::uint64_t k_minimizer_partition_target_bytes = 256u * 1024u;

/// PostgreSQL S1 page-dense quantum selected by grouped-SoA storage.
inline constexpr std::uint32_t k_grouped_soa_extent_payload_bytes = 8064u;

/// Production grouped-SoA count saturation threshold.  The value preserves a
/// 16-bit count lane while reserving 65,535 as the persisted "cap exceeded"
/// sentinel.
inline constexpr std::uint64_t k_grouped_soa_default_max_occurrence_cap = 65'534u;

/// Return whether a non-negative unsigned key fits a PostgreSQL BIGINT key.
[[nodiscard]] constexpr bool sql_bigint_can_represent(
        std::uint64_t value) noexcept {
    // Key axes are non-negative numeric values. CRC words persisted through a
    // signed SQL column use an explicit bit-preserving conversion instead.
    return value <= static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)());
}

/// Return whether a non-negative unsigned key fits a PostgreSQL INTEGER key.
[[nodiscard]] constexpr bool sql_integer_can_represent(
        std::uint64_t value) noexcept {
    return value <= static_cast<std::uint64_t>(
        (std::numeric_limits<std::int32_t>::max)());
}

/// Fail-closed error classes reported by current fixed-wire codecs and geometry.
enum class minimizer_generation_encoding_error : std::uint8_t {
    encoded_size,
    magic,
    format_revision,
    declared_size,
    reserved_nonzero,
    generation_id,
    segment_geometry_revision,
    segment_span,
    segment_count,
    directory_chunk_count,
    arithmetic_overflow,
    segment_ordinal,
    bp_range,
    directory_size,
    directory_order,
    partition_id,
    position_segment,
    hash_range,
    sub_key,
    chunk_range,
    element_count,
    byte_count,
    checksum,
    identity_mismatch,
    compatibility_mismatch,
    control_order,
    control_range,
    position_range,
    admission,
    duplicate_event,
    replay_mismatch,
    row_order,
    row_count,
};

/// Frozen persisted wire/profile selector encoded in every fixed metadata row.
enum class minimizer_generation_layout : std::uint16_t {
    invalid = 0u,
    grouped_soa_segment_local = 5u,
};

/// Return the immutable physical row quantum selected by the wire revision.
/// The value preserves the current 8,064-byte physical row quantum.
[[nodiscard]] constexpr std::uint32_t minimizer_extent_payload_bytes(
        minimizer_generation_layout layout) noexcept {
    return layout == minimizer_generation_layout::grouped_soa_segment_local
        ? k_grouped_soa_extent_payload_bytes : 0u;
}

static_assert(minimizer_extent_payload_bytes(
    minimizer_generation_layout::grouped_soa_segment_local) == 8064u);

/// Return whether a persisted format selector has a canonical local profile.
[[nodiscard]] constexpr bool is_supported_minimizer_generation_layout(
        minimizer_generation_layout layout) noexcept {
    return layout
        == minimizer_generation_layout::grouped_soa_segment_local;
}

/// Exact scalar widths admitted by grouped-SoA storage.
enum class minimizer_scalar_width : std::uint8_t {
    u16 = 2u,
    u32 = 4u,
    u64 = 8u,
};

enum class grouped_soa_offset_policy : std::uint8_t {
    per_extent_narrowest = 1u,
};

enum class grouped_soa_control_layout : std::uint8_t {
    soa_hash_prefix_offset_count = 1u,
};

enum class grouped_soa_count_semantics : std::uint8_t {
    saturating_cap_plus_one = 1u,
};

enum class grouped_soa_position_semantics : std::uint8_t {
    all_segment_local_bp_strand = 2u,
};

/**
 * Complete grouped-SoA payload contract derived from persisted identity.
 * Required scalar widths and semantics live directly in this profile; there
 * is no optional shape wrapper or alternate prior-layout profile.
 */
struct grouped_soa_minimizer_profile {
    minimizer_generation_layout layout =
        minimizer_generation_layout::grouped_soa_segment_local;
    minimizer_scheme_identity minimizer_scheme{};
    std::uint16_t alphabet_code = 0u;
    std::uint16_t kmer_size = 0u;
    minimizer_scalar_width hash_width = minimizer_scalar_width::u32;
    minimizer_scalar_width position_width = minimizer_scalar_width::u32;
    minimizer_scalar_width count_width = minimizer_scalar_width::u32;
    grouped_soa_offset_policy offset_policy =
        grouped_soa_offset_policy::per_extent_narrowest;
    grouped_soa_control_layout control_layout =
        grouped_soa_control_layout::soa_hash_prefix_offset_count;
    grouped_soa_count_semantics count_semantics =
        grouped_soa_count_semantics::saturating_cap_plus_one;
    grouped_soa_position_semantics position_semantics =
        grouped_soa_position_semantics::all_segment_local_bp_strand;
    std::uint16_t payload_layout_abi_revision =
        k_grouped_soa_payload_layout_abi_revision;
    std::uint32_t gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
    std::uint64_t max_hash = 0u;
    std::uint64_t max_occ_cap = 0u;
    bool segment_local_positions = true;

    [[nodiscard]] bool operator==(
        const grouped_soa_minimizer_profile&) const = default;
};

struct minimizer_generation_manifest;
struct grouped_soa_bucket_directory_entry;

[[nodiscard]] constexpr bool is_minimizer_scalar_width(
        minimizer_scalar_width width) noexcept {
    return width == minimizer_scalar_width::u16
        || width == minimizer_scalar_width::u32
        || width == minimizer_scalar_width::u64;
}

[[nodiscard]] constexpr std::uint64_t minimizer_scalar_width_max(
        minimizer_scalar_width width) noexcept {
    switch (width) {
        case minimizer_scalar_width::u16:
            return (std::numeric_limits<std::uint16_t>::max)();
        case minimizer_scalar_width::u32:
            return (std::numeric_limits<std::uint32_t>::max)();
        case minimizer_scalar_width::u64:
            return (std::numeric_limits<std::uint64_t>::max)();
    }
    return 0u;
}

[[nodiscard]] constexpr std::expected<minimizer_scalar_width,
    minimizer_generation_encoding_error>
select_grouped_soa_hash_width(
        minimizer_scheme_identity scheme,
        std::uint16_t alphabet_code,
        std::uint16_t kmer_size) noexcept {
    if (alphabet_code != k_minimizer_persisted_dna4_alphabet_code
        || kmer_size == 0u) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    if (scheme == k_bounded_nthash_u32_r7_sum_scheme_identity) {
        if (kmer_size != 15u) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        return minimizer_scalar_width::u32;
    }
    if (scheme != k_legacy_minimap_scheme_identity || kmer_size > 28u) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    if (kmer_size <= 8u) return minimizer_scalar_width::u16;
    if (kmer_size <= 16u) return minimizer_scalar_width::u32;
    return minimizer_scalar_width::u64;
}

/** Derive the one admitted grouped-SoA profile. */
[[nodiscard]] constexpr std::expected<grouped_soa_minimizer_profile,
    minimizer_generation_encoding_error>
derive_grouped_soa_minimizer_profile(
        minimizer_scheme_identity scheme,
        std::uint16_t alphabet_code,
        std::uint16_t kmer_size,
        std::uint64_t position_segment_span_bp,
        std::uint64_t max_occ_cap) noexcept {
    const auto hash_width = select_grouped_soa_hash_width(
        scheme, alphabet_code, kmer_size);
    if (!hash_width || position_segment_span_bp == 0u
        || max_occ_cap == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    const minimizer_scalar_width position_width =
        position_segment_span_bp <= (std::uint64_t{1u} << 15u)
        ? minimizer_scalar_width::u16
        : position_segment_span_bp <= (std::uint64_t{1u} << 31u)
            ? minimizer_scalar_width::u32
            : minimizer_scalar_width::u64;
    const minimizer_scalar_width count_width = max_occ_cap <= 65'534u
        ? minimizer_scalar_width::u16
        : max_occ_cap <= static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)()) - 1u
            ? minimizer_scalar_width::u32
            : minimizer_scalar_width::u64;
    const std::uint64_t max_hash =
        scheme == k_bounded_nthash_u32_r7_sum_scheme_identity
        ? static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)())
        : (std::uint64_t{1u} << (2u * kmer_size)) - 1u;
    return grouped_soa_minimizer_profile{
        .layout = minimizer_generation_layout::grouped_soa_segment_local,
        .minimizer_scheme = scheme,
        .alphabet_code = alphabet_code,
        .kmer_size = kmer_size,
        .hash_width = *hash_width,
        .position_width = position_width,
        .count_width = count_width,
        .max_hash = max_hash,
        .max_occ_cap = max_occ_cap,
    };
}

/** Reject profiles that disagree with canonical grouped-SoA derivation. */
[[nodiscard]] constexpr std::expected<void,
    minimizer_generation_encoding_error>
validate_grouped_soa_minimizer_profile(
        const grouped_soa_minimizer_profile& profile) noexcept {
    const std::uint64_t representative_span =
        profile.position_width == minimizer_scalar_width::u16
        ? (std::uint64_t{1u} << 15u)
        : profile.position_width == minimizer_scalar_width::u32
            ? (std::uint64_t{1u} << 31u)
            : (std::uint64_t{1u} << 31u) + 1u;
    const auto expected = derive_grouped_soa_minimizer_profile(
        profile.minimizer_scheme,
        profile.alphabet_code,
        profile.kmer_size,
        representative_span,
        profile.max_occ_cap);
    if (!expected
        || profile.layout != minimizer_generation_layout::grouped_soa_segment_local
        || expected->hash_width != profile.hash_width
        || expected->count_width != profile.count_width
        || expected->position_width != profile.position_width
        || profile.offset_policy
            != grouped_soa_offset_policy::per_extent_narrowest
        || profile.control_layout
            != grouped_soa_control_layout::soa_hash_prefix_offset_count
        || profile.count_semantics
            != grouped_soa_count_semantics::saturating_cap_plus_one
        || profile.position_semantics
            != grouped_soa_position_semantics::all_segment_local_bp_strand
        || profile.max_hash != expected->max_hash
        || profile.payload_layout_abi_revision
            != k_grouped_soa_payload_layout_abi_revision
        || profile.gpu_abi_revision != k_grouped_soa_gpu_abi_revision
        || !profile.segment_local_positions) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    return {};
}

[[nodiscard]] constexpr std::expected<grouped_soa_minimizer_profile,
    minimizer_generation_encoding_error>
derive_grouped_soa_minimizer_profile(
        const minimizer_generation_manifest& manifest) noexcept;

[[nodiscard]] constexpr std::size_t minimizer_manifest_encoded_bytes(
        minimizer_generation_layout layout) noexcept {
    return layout == minimizer_generation_layout::grouped_soa_segment_local
        ? 160u : 0u;
}

[[nodiscard]] constexpr std::size_t minimizer_directory_entry_encoded_bytes(
        const grouped_soa_minimizer_profile& profile) noexcept {
    return profile.layout == minimizer_generation_layout::grouped_soa_segment_local
        ? 128u : 0u;
}

[[nodiscard]] constexpr minimizer_scalar_width
select_grouped_soa_offset_width(std::uint64_t terminal_prefix) noexcept {
    return terminal_prefix <= (std::numeric_limits<std::uint16_t>::max)()
        ? minimizer_scalar_width::u16
        : terminal_prefix <= (std::numeric_limits<std::uint32_t>::max)()
            ? minimizer_scalar_width::u32
            : minimizer_scalar_width::u64;
}

struct grouped_soa_control_plane_offsets {
    std::uint64_t hash_offset = 0;
    std::uint64_t prefix_offset = 0;
    std::uint64_t count_offset = 0;
    std::uint64_t encoded_bytes = 0;

    [[nodiscard]] bool operator==(
        const grouped_soa_control_plane_offsets&) const = default;
};

[[nodiscard]] constexpr std::expected<
    grouped_soa_control_plane_offsets,
    minimizer_generation_encoding_error>
plan_grouped_soa_control_plane(
        std::uint64_t control_count,
        minimizer_scalar_width hash_width,
        minimizer_scalar_width offset_width,
        minimizer_scalar_width count_width) noexcept {
    if (!is_minimizer_scalar_width(hash_width)
        || !is_minimizer_scalar_width(offset_width)
        || !is_minimizer_scalar_width(count_width)) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    const auto checked_mul = [](std::uint64_t lhs, std::uint64_t rhs)
            -> std::optional<std::uint64_t> {
        if (rhs != 0
            && lhs > (std::numeric_limits<std::uint64_t>::max)() / rhs) {
            return std::nullopt;
        }
        return lhs * rhs;
    };
    const auto align_up = [](std::uint64_t value, std::uint64_t alignment)
            -> std::optional<std::uint64_t> {
        const std::uint64_t remainder = value % alignment;
        if (remainder == 0) return value;
        const std::uint64_t add = alignment - remainder;
        if (value > (std::numeric_limits<std::uint64_t>::max)() - add) {
            return std::nullopt;
        }
        return value + add;
    };
    const std::uint64_t h = static_cast<std::uint8_t>(hash_width);
    const std::uint64_t o = static_cast<std::uint8_t>(offset_width);
    const std::uint64_t c = static_cast<std::uint8_t>(count_width);
    const auto hash_end = checked_mul(control_count, h);
    if (!hash_end || control_count
            == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const auto prefix_offset = align_up(
        *hash_end, std::max<std::uint64_t>(4u, o));
    const auto prefix_bytes = checked_mul(control_count + 1u, o);
    if (!prefix_offset || !prefix_bytes
        || *prefix_offset
            > (std::numeric_limits<std::uint64_t>::max)() - *prefix_bytes) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const auto count_offset = align_up(
        *prefix_offset + *prefix_bytes,
        std::max<std::uint64_t>(4u, c));
    const auto count_bytes = checked_mul(control_count, c);
    if (!count_offset || !count_bytes
        || *count_offset
            > (std::numeric_limits<std::uint64_t>::max)() - *count_bytes) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const auto encoded_bytes = align_up(*count_offset + *count_bytes, 8u);
    if (!encoded_bytes) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return grouped_soa_control_plane_offsets{
        .hash_offset = 0,
        .prefix_offset = *prefix_offset,
        .count_offset = *count_offset,
        .encoded_bytes = *encoded_bytes,
    };
}

/// Derive the physical position-extent key from one authoritative logical /
/// control key.  The checked boundary keeps both payload namespaces below the
/// temporary-run reservation without changing the directory wire.
[[nodiscard]] constexpr std::expected<
    index_sub_key_t,
    minimizer_generation_encoding_error>
derive_minimizer_position_extent_sub_key(
        index_sub_key_t control_sub_key) noexcept {
    if (control_sub_key < 0 || control_sub_key >= k_position_extent_sub_key_base) {
        return std::unexpected(minimizer_generation_encoding_error::sub_key);
    }
    return k_position_extent_sub_key_base + control_sub_key;
}

/// Project one physical extent key back into the authoritative logical /
/// control namespace without performing unchecked signed arithmetic.
[[nodiscard]] constexpr std::expected<
    index_sub_key_t,
    minimizer_generation_encoding_error>
project_minimizer_extent_sub_key(
        index_sub_key_t physical_sub_key,
        bool position_extent) noexcept {
    if (position_extent) {
        if (physical_sub_key < k_position_extent_sub_key_base
            || physical_sub_key >= k_temporary_run_sub_key_base) {
            return std::unexpected(minimizer_generation_encoding_error::sub_key);
        }
        return physical_sub_key - k_position_extent_sub_key_base;
    }
    if (physical_sub_key < 0
        || physical_sub_key >= k_position_extent_sub_key_base) {
        return std::unexpected(minimizer_generation_encoding_error::sub_key);
    }
    return physical_sub_key;
}

/// Fixed 64-byte generation publication head stored at k_generation_head_sub_key.
struct minimizer_generation_head {
    static constexpr std::uint32_t magic = 0x42494F48u; // BIOH numeric tag
    static constexpr std::size_t encoded_bytes = 64u;

    minimizer_generation_layout layout =
        minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t compatibility_checksum = 0;
    std::uint64_t manifest_checksum = 0;
    std::uint64_t source_content_checksum = 0;

    [[nodiscard]] bool operator==(const minimizer_generation_head&) const = default;
};

/// Immutable grouped-SoA generation manifest with its persisted shape fields.
struct minimizer_generation_manifest {
    static constexpr std::uint32_t magic = 0x42494F47u; // BIOG numeric tag
    static constexpr std::size_t header_bytes = 128u;
    static constexpr std::size_t encoded_bytes = 160u;
    static constexpr std::uint16_t uniform_segment_geometry_revision =
        k_minimizer_segment_geometry_revision;

    minimizer_generation_layout layout =
        minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t target_length = 0;
    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t payload_checksum = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t directory_checksum = 0;
    std::uint32_t position_segment_count = 0;
    std::uint32_t extent_count = 0;
    std::uint32_t directory_chunk_count = 0;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    std::uint32_t gpu_abi_revision = 0;
    std::uint64_t position_segment_span_bp = 0;
    std::uint16_t segment_geometry_revision = 0;
    std::uint16_t minimizer_scheme_id = k_legacy_minimap_scheme_id;
    std::uint16_t minimizer_scheme_revision = k_legacy_minimap_scheme_revision;
    minimizer_producer_backend producer_backend_id =
        minimizer_producer_backend::invalid;
    std::uint8_t producer_identity_revision = 0u;
    minimizer_scalar_width hash_width = minimizer_scalar_width::u32;
    minimizer_scalar_width position_width = minimizer_scalar_width::u32;
    minimizer_scalar_width count_width = minimizer_scalar_width::u32;
    grouped_soa_offset_policy offset_policy =
        grouped_soa_offset_policy::per_extent_narrowest;
    grouped_soa_control_layout control_layout =
        grouped_soa_control_layout::soa_hash_prefix_offset_count;
    grouped_soa_count_semantics count_semantics =
        grouped_soa_count_semantics::saturating_cap_plus_one;
    grouped_soa_position_semantics position_semantics =
        grouped_soa_position_semantics::all_segment_local_bp_strand;
    std::uint64_t max_hash = 0u;
    std::uint64_t max_occ_cap = 0u;

    [[nodiscard]] bool operator==(const minimizer_generation_manifest&) const = default;
};

/// Caller-owned compatibility contract checked before consuming one generation.
struct expected_minimizer_generation_identity {
    minimizer_generation_layout layout =
        minimizer_generation_layout::invalid;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    std::uint32_t gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
    std::uint16_t minimizer_scheme_id = k_invalid_minimizer_scheme_id;
    std::uint16_t minimizer_scheme_revision = k_invalid_minimizer_scheme_revision;
    minimizer_producer_backend producer_backend_id =
        minimizer_producer_backend::invalid;
    std::uint8_t producer_identity_revision = 0u;
    std::optional<std::uint64_t> max_occ_cap{};

    [[nodiscard]] bool operator==(
        const expected_minimizer_generation_identity&) const = default;
};

/// Construct an independent persisted-read contract from the selected sketch
/// backend. No manifest field participates in this selection.
[[nodiscard]] constexpr std::expected<
    expected_minimizer_generation_identity,
    minimizer_generation_encoding_error>
expected_grouped_soa_generation_identity_for_backend(
        minimizer_generation_layout layout,
        minimizer_sketch_backend backend,
        std::uint16_t alphabet_code,
        std::uint16_t storage_unit_byte_width,
        std::uint16_t kmer_size,
        std::uint16_t window_size,
        std::optional<std::uint64_t> max_occ_cap = std::nullopt) noexcept {
    const minimizer_scheme_identity scheme =
        minimizer_scheme_identity_for_backend(backend);
    const minimizer_producer_identity producer_backend =
        minimizer_producer_backend_identity_for_backend(backend);
    const auto profile = derive_grouped_soa_minimizer_profile(
        scheme,
        alphabet_code,
        kmer_size,
        1u,
        max_occ_cap.value_or(
            (std::numeric_limits<std::uint64_t>::max)()));
    if (!profile || storage_unit_byte_width == 0 || window_size == 0
        || layout != minimizer_generation_layout::grouped_soa_segment_local
        || !max_occ_cap) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    return expected_minimizer_generation_identity{
        .layout = layout,
        .alphabet_code = alphabet_code,
        .storage_unit_byte_width = storage_unit_byte_width,
        .kmer_size = kmer_size,
        .window_size = window_size,
        .gpu_abi_revision = profile->gpu_abi_revision,
        .minimizer_scheme_id = scheme.id,
        .minimizer_scheme_revision = scheme.revision,
        .producer_backend_id = producer_backend.id,
        .producer_identity_revision = producer_backend.revision,
        .max_occ_cap = max_occ_cap,
    };
}

/// Logical current directory entry; persistence always uses the fixed codec.
/// `sub_key` is the logical/control key. Position rows use the checked derived
/// key k_position_extent_sub_key_base + sub_key. Both extent-local chunk sequences start
/// at zero, while the 96-byte directory ABI remains unchanged.
struct grouped_soa_bucket_directory_entry {
    static constexpr std::size_t header_bytes = 96u;
    static constexpr std::size_t encoded_bytes = 128u;

    std::uint32_t partition_id = 0;
    std::int32_t position_segment = 0;
    std::uint64_t lo_hash = 0;
    std::uint64_t hi_hash = 0;
    index_sub_key_t sub_key = 0;
    std::uint32_t control_first_chunk = 0;
    std::uint32_t control_chunk_count = 0;
    std::uint32_t position_first_chunk = 0;
    std::uint32_t position_chunk_count = 0;
    std::uint64_t control_element_count = 0;
    std::uint64_t position_element_count = 0;
    std::uint64_t control_encoded_bytes = 0;
    std::uint64_t position_encoded_bytes = 0;
    std::uint64_t control_checksum = 0;
    std::uint64_t position_checksum = 0;
    minimizer_scalar_width shape_offset_width = minimizer_scalar_width::u32;

    [[nodiscard]] bool operator==(const grouped_soa_bucket_directory_entry&) const = default;
};

/// Variable-size fixed-record wrapper. Its unused suffix is always zero; the
/// byte_count is the only exposed range so stored checksums and golden bytes remain
/// exactly 128/96 bytes.
template <std::size_t MaximumBytes>
struct encoded_minimizer_fixed_record {
    std::array<std::byte, MaximumBytes> storage{};
    std::size_t byte_count = 0;

    [[nodiscard]] constexpr const std::byte* data() const noexcept {
        return storage.data();
    }
    [[nodiscard]] constexpr std::byte* data() noexcept {
        return storage.data();
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return byte_count;
    }
    [[nodiscard]] constexpr const std::byte* begin() const noexcept {
        return storage.data();
    }
    [[nodiscard]] constexpr const std::byte* end() const noexcept {
        return storage.data() + byte_count;
    }
    [[nodiscard]] constexpr std::byte* begin() noexcept {
        return storage.data();
    }
    [[nodiscard]] constexpr std::byte* end() noexcept {
        return storage.data() + byte_count;
    }
    [[nodiscard]] constexpr const std::byte& operator[](
            std::size_t index) const noexcept {
        return storage[index];
    }
    [[nodiscard]] constexpr std::byte& operator[](
            std::size_t index) noexcept {
        return storage[index];
    }

    [[nodiscard]] constexpr bool operator==(
        const encoded_minimizer_fixed_record&) const = default;

    template <std::size_t N>
    [[nodiscard]] friend constexpr bool operator==(
            const encoded_minimizer_fixed_record& lhs,
            const std::array<std::byte, N>& rhs) noexcept {
        if (lhs.byte_count != N) return false;
        for (std::size_t i = 0; i < N; ++i) {
            if (lhs.storage[i] != rhs[i]) return false;
        }
        return true;
    }

    template <std::size_t N>
    [[nodiscard]] friend constexpr bool operator==(
            const std::array<std::byte, N>& lhs,
            const encoded_minimizer_fixed_record& rhs) noexcept {
        return rhs == lhs;
    }
};

using encoded_minimizer_manifest = encoded_minimizer_fixed_record<
    minimizer_generation_manifest::encoded_bytes>;
using encoded_grouped_soa_bucket_directory_entry = encoded_minimizer_fixed_record<
    grouped_soa_bucket_directory_entry::encoded_bytes>;

[[nodiscard]] constexpr std::expected<
    grouped_soa_minimizer_profile,
    minimizer_generation_encoding_error>
derive_grouped_soa_minimizer_profile(
        const minimizer_generation_manifest& manifest) noexcept {
    const minimizer_scheme_identity scheme{
        .id = manifest.minimizer_scheme_id,
        .revision = manifest.minimizer_scheme_revision,
    };
    if (manifest.layout
            != minimizer_generation_layout::grouped_soa_segment_local) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    const auto profile = derive_grouped_soa_minimizer_profile(
        scheme,
        manifest.alphabet_code,
        manifest.kmer_size,
        manifest.position_segment_span_bp == 0
            ? 1u : manifest.position_segment_span_bp,
        manifest.max_occ_cap);
    if (!profile
        || profile->hash_width != manifest.hash_width
        || profile->position_width != manifest.position_width
        || profile->count_width != manifest.count_width
        || profile->offset_policy != manifest.offset_policy
        || profile->control_layout != manifest.control_layout
        || profile->count_semantics != manifest.count_semantics
        || profile->position_semantics != manifest.position_semantics
        || profile->max_hash != manifest.max_hash) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    return profile;
}

/// Half-open ordinal interval covering a requested base-pair range.
struct minimizer_position_segment_range {
    std::uint32_t begin = 0;
    std::uint32_t end = 0; // half-open

    [[nodiscard]] bool operator==(
        const minimizer_position_segment_range&) const = default;
};

} // namespace bioseq::indexing

namespace bioseq::indexing::minimizer_generation_wire_detail {

template <class UInt>
requires (std::is_unsigned_v<UInt>)
constexpr void write_le(
        std::span<std::byte> output,
        std::size_t offset,
        UInt value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(UInt); ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & UInt{0xff});
        value >>= 8u;
    }
}

template <class UInt>
requires (std::is_unsigned_v<UInt>)
[[nodiscard]] constexpr UInt read_le(
        std::span<const std::byte> input,
        std::size_t offset) noexcept {
    UInt value = 0;
    for (std::size_t byte = 0; byte < sizeof(UInt); ++byte) {
        value |= static_cast<UInt>(
            std::to_integer<std::uint8_t>(input[offset + byte])) << (byte * 8u);
    }
    return value;
}

[[nodiscard]] constexpr bool write_shape_scalar(
        std::span<std::byte> output,
        std::size_t offset,
        minimizer_scalar_width width,
        std::uint64_t value) noexcept {
    const std::size_t bytes = static_cast<std::uint8_t>(width);
    if (!is_minimizer_scalar_width(width)
        || offset > output.size() || bytes > output.size() - offset
        || value > minimizer_scalar_width_max(width)) {
        return false;
    }
    for (std::size_t byte = 0; byte < bytes; ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
    return true;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> read_shape_scalar(
        std::span<const std::byte> input,
        std::size_t offset,
        minimizer_scalar_width width) noexcept {
    const std::size_t bytes = static_cast<std::uint8_t>(width);
    if (!is_minimizer_scalar_width(width)
        || offset > input.size() || bytes > input.size() - offset) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < bytes; ++byte) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(input[offset + byte]))
            << (byte * 8u);
    }
    return value;
}

[[nodiscard]] constexpr bool all_zero(
        std::span<const std::byte> input,
        std::size_t begin,
        std::size_t end) noexcept {
    for (std::size_t offset = begin; offset < end; ++offset) {
        if (input[offset] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

// Standalone wire decoding validates only invariants available in the fixed
// manifest itself. Head/manifest identity plus payload/directory byte counts,
// checksums, extents, and compatibility relationships require the referenced
// objects and deliberately belong to generation publication/load validation.
[[nodiscard]] constexpr std::optional<minimizer_generation_encoding_error>
validate_manifest_standalone(const minimizer_generation_manifest& manifest) noexcept {
    const minimizer_producer_identity producer_backend{
        .id = manifest.producer_backend_id,
        .revision = manifest.producer_identity_revision,
    };
    if (!is_supported_minimizer_producer_backend_identity(producer_backend)) {
        return minimizer_generation_encoding_error::compatibility_mismatch;
    }
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    if (!profile) {
        return minimizer_generation_encoding_error::compatibility_mismatch;
    }
    if (minimizer_scheme_identity_for_producer_backend(producer_backend)
            != profile->minimizer_scheme) {
        return minimizer_generation_encoding_error::compatibility_mismatch;
    }
    if (manifest.layout
            == minimizer_generation_layout::
                grouped_soa_segment_local
        && manifest.gpu_abi_revision != profile->gpu_abi_revision) {
        return minimizer_generation_encoding_error::compatibility_mismatch;
    }
    if (manifest.segment_geometry_revision
        != minimizer_generation_manifest::uniform_segment_geometry_revision) {
        return minimizer_generation_encoding_error::segment_geometry_revision;
    }
    if (!sql_integer_can_represent(manifest.position_segment_count)) {
        return minimizer_generation_encoding_error::segment_count;
    }
    if (!sql_integer_can_represent(manifest.directory_chunk_count)) {
        return minimizer_generation_encoding_error::directory_chunk_count;
    }
    if (manifest.target_length == 0) {
        if (manifest.position_segment_count != 0) {
            return minimizer_generation_encoding_error::segment_count;
        }
        return std::nullopt;
    }
    if (manifest.position_segment_span_bp == 0) {
        return minimizer_generation_encoding_error::segment_span;
    }
    const std::uint64_t expected_count =
        manifest.target_length / manifest.position_segment_span_bp
        + (manifest.target_length % manifest.position_segment_span_bp != 0 ? 1u : 0u);
    if (!sql_integer_can_represent(expected_count)
        || expected_count != manifest.position_segment_count) {
        return minimizer_generation_encoding_error::segment_count;
    }
    return std::nullopt;
}

} // namespace minimizer_generation_wire_detail

export namespace bioseq::indexing {

/// Encode a validated generation head to its exact 64-byte little-endian image.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, minimizer_generation_head::encoded_bytes>,
    minimizer_generation_encoding_error>
encode_minimizer_generation_head(const minimizer_generation_head& head) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (!is_supported_minimizer_generation_layout(head.layout)) {
        return std::unexpected(minimizer_generation_encoding_error::format_revision);
    }
    if (head.generation_id == 0 || !sql_bigint_can_represent(head.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    std::array<std::byte, minimizer_generation_head::encoded_bytes> output{};
    std::span<std::byte> bytes{output};
    write_le<std::uint32_t>(bytes, 0, minimizer_generation_head::magic);
    write_le<std::uint16_t>(bytes, 4,
        static_cast<std::uint16_t>(head.layout));
    write_le<std::uint16_t>(bytes, 6,
        static_cast<std::uint16_t>(minimizer_generation_head::encoded_bytes));
    write_le<std::uint64_t>(bytes, 8, head.generation_id);
    write_le<std::uint64_t>(bytes, 16, head.source_identity);
    write_le<std::uint64_t>(bytes, 24, head.compatibility_checksum);
    write_le<std::uint64_t>(bytes, 32, head.manifest_checksum);
    write_le<std::uint64_t>(bytes, 40, head.source_content_checksum);
    return output;
}

/// Decode one exact 64-byte generation head, rejecting malformed or reserved data.
[[nodiscard]] constexpr std::expected<
    minimizer_generation_head,
    minimizer_generation_encoding_error>
decode_minimizer_generation_head(std::span<const std::byte> input) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (input.size() != minimizer_generation_head::encoded_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    if (read_le<std::uint32_t>(input, 0) != minimizer_generation_head::magic) {
        return std::unexpected(minimizer_generation_encoding_error::magic);
    }
    const auto layout = static_cast<minimizer_generation_layout>(
        read_le<std::uint16_t>(input, 4));
    if (!is_supported_minimizer_generation_layout(layout)) {
        return std::unexpected(minimizer_generation_encoding_error::format_revision);
    }
    if (read_le<std::uint16_t>(input, 6) != minimizer_generation_head::encoded_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::declared_size);
    }
    if (!all_zero(input, 48, 64)) {
        return std::unexpected(minimizer_generation_encoding_error::reserved_nonzero);
    }
    minimizer_generation_head head{
        .layout = layout,
        .generation_id = read_le<std::uint64_t>(input, 8),
        .source_identity = read_le<std::uint64_t>(input, 16),
        .compatibility_checksum = read_le<std::uint64_t>(input, 24),
        .manifest_checksum = read_le<std::uint64_t>(input, 32),
        .source_content_checksum = read_le<std::uint64_t>(input, 40),
    };
    if (head.generation_id == 0 || !sql_bigint_can_represent(head.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    return head;
}

/// Encode a standalone-valid grouped-SoA manifest as exactly 160 bytes,
/// including the frozen 32-byte profile extension.
[[nodiscard]] constexpr std::expected<
    encoded_minimizer_manifest,
    minimizer_generation_encoding_error>
encode_minimizer_generation_manifest_record(
        const minimizer_generation_manifest& manifest) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (manifest.generation_id == 0
        || !sql_bigint_can_represent(manifest.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    encoded_minimizer_manifest output{
        .byte_count = minimizer_manifest_encoded_bytes(manifest.layout),
    };
    std::span<std::byte> bytes{output.data(), output.size()};
    write_le<std::uint32_t>(bytes, 0, minimizer_generation_manifest::magic);
    write_le<std::uint16_t>(bytes, 4,
        static_cast<std::uint16_t>(manifest.layout));
    write_le<std::uint16_t>(bytes, 6,
        static_cast<std::uint16_t>(output.size()));
    write_le<std::uint64_t>(bytes, 8, manifest.generation_id);
    write_le<std::uint64_t>(bytes, 16, manifest.source_identity);
    write_le<std::uint64_t>(bytes, 24, manifest.source_content_checksum);
    write_le<std::uint64_t>(bytes, 32, manifest.target_length);
    write_le<std::uint64_t>(bytes, 40, manifest.control_count);
    write_le<std::uint64_t>(bytes, 48, manifest.position_count);
    write_le<std::uint64_t>(bytes, 56, manifest.payload_bytes);
    write_le<std::uint64_t>(bytes, 64, manifest.payload_checksum);
    write_le<std::uint64_t>(bytes, 72, manifest.directory_bytes);
    write_le<std::uint64_t>(bytes, 80, manifest.directory_checksum);
    write_le<std::uint32_t>(bytes, 88, manifest.position_segment_count);
    write_le<std::uint32_t>(bytes, 92, manifest.extent_count);
    write_le<std::uint32_t>(bytes, 96, manifest.directory_chunk_count);
    write_le<std::uint16_t>(bytes, 100, manifest.alphabet_code);
    write_le<std::uint16_t>(bytes, 102, manifest.storage_unit_byte_width);
    write_le<std::uint16_t>(bytes, 104, manifest.kmer_size);
    write_le<std::uint16_t>(bytes, 106, manifest.window_size);
    write_le<std::uint32_t>(bytes, 108, manifest.gpu_abi_revision);
    write_le<std::uint64_t>(bytes, 112, manifest.position_segment_span_bp);
    write_le<std::uint16_t>(bytes, 120, manifest.segment_geometry_revision);
    write_le<std::uint16_t>(bytes, 122, manifest.minimizer_scheme_id);
    write_le<std::uint16_t>(bytes, 124, manifest.minimizer_scheme_revision);
    write_le<std::uint8_t>(bytes, 126,
        static_cast<std::uint8_t>(manifest.producer_backend_id));
    write_le<std::uint8_t>(bytes, 127,
        manifest.producer_identity_revision);
    write_le<std::uint32_t>(bytes, 128, k_grouped_soa_extension_magic);
    write_le<std::uint16_t>(
        bytes, 132, k_grouped_soa_extension_format_revision);
    write_le<std::uint16_t>(bytes, 134, k_grouped_soa_extension_bytes);
    write_le<std::uint8_t>(
        bytes, 136, static_cast<std::uint8_t>(manifest.hash_width));
    write_le<std::uint8_t>(
        bytes, 137, static_cast<std::uint8_t>(manifest.position_width));
    write_le<std::uint8_t>(
        bytes, 138, static_cast<std::uint8_t>(manifest.count_width));
    write_le<std::uint8_t>(
        bytes, 139, static_cast<std::uint8_t>(manifest.offset_policy));
    write_le<std::uint8_t>(
        bytes, 140, static_cast<std::uint8_t>(manifest.control_layout));
    write_le<std::uint8_t>(
        bytes, 141, static_cast<std::uint8_t>(manifest.count_semantics));
    write_le<std::uint8_t>(
        bytes, 142, static_cast<std::uint8_t>(manifest.position_semantics));
    write_le<std::uint64_t>(bytes, 144, manifest.max_hash);
    write_le<std::uint64_t>(bytes, 152, manifest.max_occ_cap);
    return output;
}

[[nodiscard]] constexpr std::expected<
    std::array<std::byte, minimizer_generation_manifest::encoded_bytes>,
    minimizer_generation_encoding_error>
encode_minimizer_generation_manifest(
        const minimizer_generation_manifest& manifest) noexcept {
    if (manifest.layout
            != minimizer_generation_layout::
                grouped_soa_segment_local) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    const auto encoded = encode_minimizer_generation_manifest_record(manifest);
    if (!encoded
        || encoded->size()
            != minimizer_generation_manifest::encoded_bytes) {
        return std::unexpected(encoded
            ? minimizer_generation_encoding_error::encoded_size : encoded.error());
    }
    return encoded->storage;
}

/// Decode the exact 160-byte grouped-SoA manifest.
[[nodiscard]] constexpr std::expected<
    minimizer_generation_manifest,
    minimizer_generation_encoding_error>
decode_minimizer_generation_manifest(std::span<const std::byte> input) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (input.size() != minimizer_generation_manifest::encoded_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    if (read_le<std::uint32_t>(input, 0) != minimizer_generation_manifest::magic) {
        return std::unexpected(minimizer_generation_encoding_error::magic);
    }
    const auto layout = static_cast<minimizer_generation_layout>(
        read_le<std::uint16_t>(input, 4));
    if (!is_supported_minimizer_generation_layout(layout)) {
        return std::unexpected(minimizer_generation_encoding_error::format_revision);
    }
    const std::size_t expected_size = minimizer_manifest_encoded_bytes(
        layout);
    if (input.size() != expected_size
        || read_le<std::uint16_t>(input, 6) != expected_size) {
        return std::unexpected(minimizer_generation_encoding_error::declared_size);
    }
    minimizer_generation_manifest manifest{
        .layout = layout,
        .generation_id = read_le<std::uint64_t>(input, 8),
        .source_identity = read_le<std::uint64_t>(input, 16),
        .source_content_checksum = read_le<std::uint64_t>(input, 24),
        .target_length = read_le<std::uint64_t>(input, 32),
        .control_count = read_le<std::uint64_t>(input, 40),
        .position_count = read_le<std::uint64_t>(input, 48),
        .payload_bytes = read_le<std::uint64_t>(input, 56),
        .payload_checksum = read_le<std::uint64_t>(input, 64),
        .directory_bytes = read_le<std::uint64_t>(input, 72),
        .directory_checksum = read_le<std::uint64_t>(input, 80),
        .position_segment_count = read_le<std::uint32_t>(input, 88),
        .extent_count = read_le<std::uint32_t>(input, 92),
        .directory_chunk_count = read_le<std::uint32_t>(input, 96),
        .alphabet_code = read_le<std::uint16_t>(input, 100),
        .storage_unit_byte_width = read_le<std::uint16_t>(input, 102),
        .kmer_size = read_le<std::uint16_t>(input, 104),
        .window_size = read_le<std::uint16_t>(input, 106),
        .gpu_abi_revision = read_le<std::uint32_t>(input, 108),
        .position_segment_span_bp = read_le<std::uint64_t>(input, 112),
        .segment_geometry_revision = read_le<std::uint16_t>(input, 120),
        .minimizer_scheme_id = read_le<std::uint16_t>(input, 122),
        .minimizer_scheme_revision = read_le<std::uint16_t>(input, 124),
        .producer_backend_id = static_cast<minimizer_producer_backend>(
            read_le<std::uint8_t>(input, 126)),
        .producer_identity_revision =
            read_le<std::uint8_t>(input, 127),
    };
    if (read_le<std::uint32_t>(input, 128)
                != k_grouped_soa_extension_magic
            || read_le<std::uint16_t>(input, 132)
                != k_grouped_soa_extension_format_revision
            || read_le<std::uint16_t>(input, 134)
                != k_grouped_soa_extension_bytes) {
        return std::unexpected(
            minimizer_generation_encoding_error::format_revision);
    }
    if (input[143] != std::byte{0}) {
        return std::unexpected(
            minimizer_generation_encoding_error::reserved_nonzero);
    }
    manifest.hash_width = static_cast<minimizer_scalar_width>(
        read_le<std::uint8_t>(input, 136));
    manifest.position_width = static_cast<minimizer_scalar_width>(
        read_le<std::uint8_t>(input, 137));
    manifest.count_width = static_cast<minimizer_scalar_width>(
        read_le<std::uint8_t>(input, 138));
    manifest.offset_policy = static_cast<grouped_soa_offset_policy>(
        read_le<std::uint8_t>(input, 139));
    manifest.control_layout = static_cast<grouped_soa_control_layout>(
        read_le<std::uint8_t>(input, 140));
    manifest.count_semantics = static_cast<grouped_soa_count_semantics>(
        read_le<std::uint8_t>(input, 141));
    manifest.position_semantics = static_cast<grouped_soa_position_semantics>(
        read_le<std::uint8_t>(input, 142));
    manifest.max_hash = read_le<std::uint64_t>(input, 144);
    manifest.max_occ_cap = read_le<std::uint64_t>(input, 152);
    if (manifest.generation_id == 0
        || !sql_bigint_can_represent(manifest.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    return manifest;
}

inline constexpr std::size_t k_max_generation_head_bytes =
    k_hash_major_minimizer_head_bytes;
inline constexpr std::size_t k_max_manifest_bytes =
    k_hash_major_minimizer_manifest_bytes;
static_assert(minimizer_generation_head::encoded_bytes == 64u);
static_assert(minimizer_generation_manifest::encoded_bytes == 160u);
static_assert(k_max_generation_head_bytes == 224u);
static_assert(k_max_manifest_bytes == 336u);

enum class tagged_minimizer_generation_encoding_error : std::uint8_t {
    encoded_size,
    grouped_soa,
    hash_major,
};

using tagged_minimizer_generation_head = std::variant<
    minimizer_generation_head, hash_major_minimizer_head>;
using tagged_minimizer_generation_manifest = std::variant<
    minimizer_generation_manifest, hash_major_minimizer_manifest>;

/** Exact-length grouped-SoA/hash-major dispatch; older 128-byte manifests are not
 * part of this tagged publication branch and cannot be widened implicitly. */
[[nodiscard]] inline std::expected<
    tagged_minimizer_generation_head, tagged_minimizer_generation_encoding_error>
decode_tagged_minimizer_generation_head(
        std::span<const std::byte> input) noexcept {
    if (input.size() == minimizer_generation_head::encoded_bytes) {
        auto decoded = decode_minimizer_generation_head(input);
        if (!decoded
            || decoded->layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local) {
            return std::unexpected(
                tagged_minimizer_generation_encoding_error::grouped_soa);
        }
        return tagged_minimizer_generation_head{std::in_place_index<0>, *decoded};
    }
    if (input.size() == k_hash_major_minimizer_head_bytes) {
        auto decoded = decode_hash_major_minimizer_head(input);
        if (!decoded) {
            return std::unexpected(
                tagged_minimizer_generation_encoding_error::hash_major);
        }
        return tagged_minimizer_generation_head{std::in_place_index<1>, *decoded};
    }
    return std::unexpected(tagged_minimizer_generation_encoding_error::encoded_size);
}

[[nodiscard]] inline std::expected<
    tagged_minimizer_generation_manifest, tagged_minimizer_generation_encoding_error>
decode_tagged_minimizer_generation_manifest(
        std::span<const std::byte> input) noexcept {
    if (input.size()
        == minimizer_generation_manifest::encoded_bytes) {
        auto decoded = decode_minimizer_generation_manifest(input);
        if (!decoded
            || decoded->layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local) {
            return std::unexpected(
                tagged_minimizer_generation_encoding_error::grouped_soa);
        }
        return tagged_minimizer_generation_manifest{
            std::in_place_index<0>, *decoded};
    }
    if (input.size() == k_hash_major_minimizer_manifest_bytes) {
        auto decoded = decode_hash_major_minimizer_manifest(input);
        if (!decoded) {
            return std::unexpected(
                tagged_minimizer_generation_encoding_error::hash_major);
        }
        return tagged_minimizer_generation_manifest{
            std::in_place_index<1>, *decoded};
    }
    return std::unexpected(tagged_minimizer_generation_encoding_error::encoded_size);
}

/// Translate the frozen runtime producer identity into the exact registered
/// hash-major compatibility profile.  Keeping this bridge beside the existing
/// grouped-SoA producer/scheme registry prevents PostgreSQL publication and
/// reopen code from independently reconstructing (or silently defaulting)
/// persisted biological semantics.
[[nodiscard]] constexpr std::expected<
    hash_major_minimizer_compatibility, hash_major_minimizer_error>
hash_major_minimizer_compatibility_for_backend(
        minimizer_sketch_backend backend,
        std::uint16_t alphabet_code,
        std::uint16_t kmer_size,
        std::uint16_t window_size,
        std::uint64_t target_length) noexcept {
    const minimizer_scheme_identity scheme =
        minimizer_scheme_identity_for_backend(backend);
    const minimizer_producer_identity producer =
        minimizer_producer_backend_identity_for_backend(backend);
    if (!is_supported_minimizer_scheme_identity(
            scheme.id, scheme.revision)
        || !is_supported_minimizer_producer_backend_identity(producer)
        || alphabet_code != 0u
        || kmer_size != 15u
        || window_size != 10u) {
        return std::unexpected(hash_major_minimizer_error::compatibility);
    }
    const hash_major_minimizer_compatibility compatibility{
        .hash_scheme_identity = scheme.id,
        .hash_scheme_revision = scheme.revision,
        .alphabet_code = alphabet_code,
        .kmer_size = kmer_size,
        .window_size = window_size,
        .hash_width_bytes = 4u,
        .stored_posting_width_bytes =
            hash_major_minimizer_uses_u32_postings(target_length) ? 4u : 8u,
        .position_encoding_revision = 1u,
        .hash_semantics_revision = 1u,
        .count_semantics_revision = 1u,
        .producer_backend_identity =
            static_cast<std::uint16_t>(producer.id),
        .producer_backend_revision = producer.revision,
        .controls_per_block = k_hash_major_minimizer_controls_per_block,
        .hash_seed = 0u,
    };
    if (!is_valid_hash_major_minimizer_compatibility(
            compatibility, target_length)) {
        return std::unexpected(hash_major_minimizer_error::compatibility);
    }
    return compatibility;
}

/// Validate invariants intrinsic to one decoded current directory entry.
[[nodiscard]] constexpr std::optional<minimizer_generation_encoding_error>
validate_minimizer_bucket_dir_entry(
        const grouped_soa_bucket_directory_entry& entry,
        const grouped_soa_minimizer_profile& profile) noexcept {
    if (!validate_grouped_soa_minimizer_profile(profile)) {
        return minimizer_generation_encoding_error::compatibility_mismatch;
    }
    if (entry.position_segment < 0) {
        return minimizer_generation_encoding_error::position_segment;
    }
    if (entry.lo_hash > entry.hi_hash || entry.hi_hash > profile.max_hash) {
        return minimizer_generation_encoding_error::hash_range;
    }
    if (!derive_minimizer_position_extent_sub_key(entry.sub_key)) {
        return minimizer_generation_encoding_error::sub_key;
    }
    if (profile.layout
            == minimizer_generation_layout::
                grouped_soa_segment_local
        && entry.sub_key
            != static_cast<index_sub_key_t>(entry.partition_id)) {
        return minimizer_generation_encoding_error::sub_key;
    }
    if (entry.control_element_count == 0
        || entry.position_element_count == 0
        || entry.control_element_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return minimizer_generation_encoding_error::element_count;
    }
    const std::uint64_t position_width =
        static_cast<std::uint8_t>(profile.position_width);
    if (entry.shape_offset_width
                != select_grouped_soa_offset_width(
                    entry.position_element_count)) {
        return minimizer_generation_encoding_error::compatibility_mismatch;
    }
    const auto control_layout = plan_grouped_soa_control_plane(
        entry.control_element_count,
        profile.hash_width,
        entry.shape_offset_width,
        profile.count_width);
    if (!control_layout
        || entry.control_encoded_bytes != control_layout->encoded_bytes
        || entry.position_element_count
            > (std::numeric_limits<std::uint64_t>::max)() / position_width
        || entry.position_encoded_bytes
            != entry.position_element_count * position_width) {
        return minimizer_generation_encoding_error::byte_count;
    }
    if (entry.control_first_chunk != 0 || entry.position_first_chunk != 0
        || entry.control_chunk_count == 0 || entry.position_chunk_count == 0) {
        return minimizer_generation_encoding_error::chunk_range;
    }
    const std::uint64_t control_end =
        static_cast<std::uint64_t>(entry.control_first_chunk)
        + entry.control_chunk_count;
    const std::uint64_t position_end =
        static_cast<std::uint64_t>(entry.position_first_chunk)
        + entry.position_chunk_count;
    if (control_end > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)()) + 1u
        || position_end > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)()) + 1u) {
        return minimizer_generation_encoding_error::chunk_range;
    }
    return std::nullopt;
}

/// Encode one logical directory entry to the exact 96-byte little-endian ABI.
[[nodiscard]] constexpr std::expected<
    encoded_grouped_soa_bucket_directory_entry,
    minimizer_generation_encoding_error>
encode_minimizer_bucket_dir_entry_record(
        const grouped_soa_bucket_directory_entry& entry,
        const grouped_soa_minimizer_profile& profile) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
        return std::unexpected(*error);
    }
    encoded_grouped_soa_bucket_directory_entry output{
        .byte_count = minimizer_directory_entry_encoded_bytes(profile),
    };
    std::span<std::byte> bytes{output.data(), output.size()};
    write_le<std::uint32_t>(bytes, 0, entry.partition_id);
    write_le<std::uint32_t>(bytes, 4,
        std::bit_cast<std::uint32_t>(entry.position_segment));
    write_le<std::uint64_t>(bytes, 8, entry.lo_hash);
    write_le<std::uint64_t>(bytes, 16, entry.hi_hash);
    write_le<std::uint64_t>(bytes, 24,
        std::bit_cast<std::uint64_t>(entry.sub_key));
    write_le<std::uint32_t>(bytes, 32, entry.control_first_chunk);
    write_le<std::uint32_t>(bytes, 36, entry.control_chunk_count);
    write_le<std::uint32_t>(bytes, 40, entry.position_first_chunk);
    write_le<std::uint32_t>(bytes, 44, entry.position_chunk_count);
    write_le<std::uint64_t>(bytes, 48, entry.control_element_count);
    write_le<std::uint64_t>(bytes, 56, entry.position_element_count);
    write_le<std::uint64_t>(bytes, 64, entry.control_encoded_bytes);
    write_le<std::uint64_t>(bytes, 72, entry.position_encoded_bytes);
    write_le<std::uint64_t>(bytes, 80, entry.control_checksum);
    write_le<std::uint64_t>(bytes, 88, entry.position_checksum);
    write_le<std::uint8_t>(bytes, 96,
            static_cast<std::uint8_t>(profile.hash_width));
        write_le<std::uint8_t>(bytes, 97,
            static_cast<std::uint8_t>(entry.shape_offset_width));
        write_le<std::uint8_t>(bytes, 98,
            static_cast<std::uint8_t>(profile.count_width));
        write_le<std::uint8_t>(bytes, 99,
            static_cast<std::uint8_t>(profile.position_width));
        write_le<std::uint8_t>(bytes, 100,
            static_cast<std::uint8_t>(profile.control_layout));
        write_le<std::uint8_t>(bytes, 101,
            static_cast<std::uint8_t>(profile.count_semantics));
        write_le<std::uint8_t>(bytes, 102,
            static_cast<std::uint8_t>(profile.position_semantics));
        write_le<std::uint8_t>(bytes, 103, 1u); // terminal-prefix present
    return output;
}

[[nodiscard]] constexpr std::expected<
    std::array<std::byte, grouped_soa_bucket_directory_entry::encoded_bytes>,
    minimizer_generation_encoding_error>
encode_grouped_soa_bucket_directory_entry(
        const grouped_soa_bucket_directory_entry& entry,
        const grouped_soa_minimizer_profile& profile) noexcept {
    if (profile.layout
            != minimizer_generation_layout::
                grouped_soa_segment_local) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    const auto encoded = encode_minimizer_bucket_dir_entry_record(
        entry, profile);
    if (!encoded
        || encoded->size()
            != grouped_soa_bucket_directory_entry::encoded_bytes) {
        return std::unexpected(encoded
            ? minimizer_generation_encoding_error::encoded_size : encoded.error());
    }
    return encoded->storage;
}

/// Decode and intrinsically validate one exact 96-byte directory entry.
[[nodiscard]] constexpr std::expected<
    grouped_soa_bucket_directory_entry,
    minimizer_generation_encoding_error>
decode_minimizer_bucket_dir_entry(
        std::span<const std::byte> input,
        const grouped_soa_minimizer_profile& profile) noexcept {
    using namespace minimizer_generation_wire_detail;
    const std::size_t expected_size =
        minimizer_directory_entry_encoded_bytes(profile);
    if (input.size() != expected_size) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    grouped_soa_bucket_directory_entry entry{
        .partition_id = read_le<std::uint32_t>(input, 0),
        .position_segment = std::bit_cast<std::int32_t>(
            read_le<std::uint32_t>(input, 4)),
        .lo_hash = read_le<std::uint64_t>(input, 8),
        .hi_hash = read_le<std::uint64_t>(input, 16),
        .sub_key = std::bit_cast<index_sub_key_t>(
            read_le<std::uint64_t>(input, 24)),
        .control_first_chunk = read_le<std::uint32_t>(input, 32),
        .control_chunk_count = read_le<std::uint32_t>(input, 36),
        .position_first_chunk = read_le<std::uint32_t>(input, 40),
        .position_chunk_count = read_le<std::uint32_t>(input, 44),
        .control_element_count = read_le<std::uint64_t>(input, 48),
        .position_element_count = read_le<std::uint64_t>(input, 56),
        .control_encoded_bytes = read_le<std::uint64_t>(input, 64),
        .position_encoded_bytes = read_le<std::uint64_t>(input, 72),
        .control_checksum = read_le<std::uint64_t>(input, 80),
        .position_checksum = read_le<std::uint64_t>(input, 88),
    };
    if (read_le<std::uint8_t>(input, 96)
                != static_cast<std::uint8_t>(profile.hash_width)
            || read_le<std::uint8_t>(input, 98)
                != static_cast<std::uint8_t>(profile.count_width)
            || read_le<std::uint8_t>(input, 99)
                != static_cast<std::uint8_t>(profile.position_width)
            || read_le<std::uint8_t>(input, 100)
                != static_cast<std::uint8_t>(profile.control_layout)
            || read_le<std::uint8_t>(input, 101)
                != static_cast<std::uint8_t>(profile.count_semantics)
            || read_le<std::uint8_t>(input, 102)
                != static_cast<std::uint8_t>(profile.position_semantics)
            || read_le<std::uint8_t>(input, 103) != 1u
            || !all_zero(input, 104, 128)) {
            return std::unexpected(
                !all_zero(input, 104, 128)
                    ? minimizer_generation_encoding_error::reserved_nonzero
                    : minimizer_generation_encoding_error::compatibility_mismatch);
    }
    entry.shape_offset_width = static_cast<minimizer_scalar_width>(
        read_le<std::uint8_t>(input, 97));
    if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
        return std::unexpected(*error);
    }
    return entry;
}

/// Validate stable ordinal assignment and sorted, non-overlapping hash ranges.
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
validate_minimizer_bucket_directory_entries(
        std::span<const grouped_soa_bucket_directory_entry> entries,
        const grouped_soa_minimizer_profile& profile) noexcept {
    if (!validate_grouped_soa_minimizer_profile(profile)) {
        return minimizer_generation_encoding_error::compatibility_mismatch;
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const grouped_soa_bucket_directory_entry& entry = entries[index];
        if (entry.partition_id != index) {
            return minimizer_generation_encoding_error::partition_id;
        }
        if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
            return error;
        }
        if (index == 0) {
            continue;
        }
        const grouped_soa_bucket_directory_entry& previous = entries[index - 1u];
        if (entry.position_segment < previous.position_segment
            || (entry.position_segment == previous.position_segment
                && entry.lo_hash <= previous.hi_hash)) {
            return minimizer_generation_encoding_error::directory_order;
        }
    }
    return std::nullopt;
}

/// Encode the authoritative header-free sequence of fixed directory entries.
[[nodiscard]] inline std::expected<
    std::vector<std::byte>,
    minimizer_generation_encoding_error>
encode_minimizer_bucket_directory(
        std::span<const grouped_soa_bucket_directory_entry> entries,
        const grouped_soa_minimizer_profile& profile) {
    if (auto error = validate_minimizer_bucket_directory_entries(
            entries, profile)) {
        return std::unexpected(*error);
    }
    const std::size_t entry_bytes =
        minimizer_directory_entry_encoded_bytes(profile);
    if (entries.size() > (std::numeric_limits<std::size_t>::max)()
            / entry_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    std::vector<std::byte> output(
        entries.size() * entry_bytes);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto encoded = encode_minimizer_bucket_dir_entry_record(
            entries[index], profile);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        std::copy(encoded->begin(), encoded->end(),
            output.begin() + static_cast<std::ptrdiff_t>(
                index * entry_bytes));
    }
    return output;
}

/// Owning decoded view of the authoritative fixed directory bytes.
class grouped_soa_directory_view {
public:
    grouped_soa_directory_view() = default;

    /// Copy and validate a complete header-free encoded directory.
    [[nodiscard]] static std::expected<grouped_soa_directory_view, minimizer_generation_encoding_error>
    decode(
            std::span<const std::byte> bytes,
            const grouped_soa_minimizer_profile& profile) {
        if (!validate_grouped_soa_minimizer_profile(profile)) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        const std::size_t entry_bytes =
            minimizer_directory_entry_encoded_bytes(profile);
        if (bytes.size() % entry_bytes != 0) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        grouped_soa_directory_view view;
        view.profile_ = profile;
        view.encoded_.assign(bytes.begin(), bytes.end());
        const std::size_t count =
            bytes.size() / entry_bytes;
        view.entries_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const std::span<const std::byte> encoded_entry{
                view.encoded_.data()
                    + index * entry_bytes,
                entry_bytes};
            auto entry = decode_minimizer_bucket_dir_entry(
                encoded_entry, profile);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            view.entries_.push_back(*entry);
        }
        if (auto error = validate_minimizer_bucket_directory_entries(
                view.entries_, profile)) {
            return std::unexpected(*error);
        }
        return view;
    }

    /// Decode under an existing logical reservation. Both vectors reserve
    /// first, then their actual capacities are reconciled before any persisted
    /// byte or decoded entry is written. The caller's permit may include other
    /// simultaneously-live allocations; only the directory capacity delta is
    /// added here.
    [[nodiscard]] static std::expected<grouped_soa_directory_view, minimizer_generation_encoding_error>
    decode_admitted(
            std::span<const std::byte> bytes,
            const grouped_soa_minimizer_profile& profile,
            bioseq::parallelism::memory_permit& permit,
            bioseq::parallelism::memory_admission_failure*
                admission_failure = nullptr) {
        if (!validate_grouped_soa_minimizer_profile(profile)) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        const std::size_t entry_bytes =
            minimizer_directory_entry_encoded_bytes(profile);
        if (bytes.size() % entry_bytes != 0) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        const std::size_t count =
            bytes.size() / entry_bytes;
        const std::uint64_t logical_bytes =
            bioseq::common::saturating_add(
                bytes.size(),
                bioseq::common::saturating_mul(
                    count, sizeof(grouped_soa_bucket_directory_entry)));
        if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)()
            || permit.bytes() < logical_bytes) {
            return std::unexpected(
                logical_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                ? minimizer_generation_encoding_error::arithmetic_overflow
                : minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t non_directory_bytes =
            permit.bytes() - logical_bytes;
        const std::uint64_t allocation_headroom =
            bioseq::common::saturating_add(
                non_directory_bytes,
                bioseq::common::saturating_mul(
                    logical_bytes,
                    bioseq::parallelism::
                        k_vector_preallocation_headroom_factor));
        if (allocation_headroom
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (!permit.grow_to(allocation_headroom, admission_failure)) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        grouped_soa_directory_view view;
        view.profile_ = profile;
        try {
            view.encoded_.reserve(bytes.size());
            view.entries_.reserve(count);
        } catch (const std::bad_alloc&) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t actual_bytes = view.owned_capacity_bytes();
        if (actual_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t reconciled =
            bioseq::common::saturating_add(
                non_directory_bytes, actual_bytes);
        if (reconciled == (std::numeric_limits<std::uint64_t>::max)()
            || reconciled > allocation_headroom) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        permit.shrink_to(reconciled);
        view.encoded_.assign(bytes.begin(), bytes.end());
        for (std::size_t index = 0; index < count; ++index) {
            const auto entry = decode_minimizer_bucket_dir_entry(
                std::span<const std::byte>{view.encoded_}.subspan(
                    index * entry_bytes,
                    entry_bytes),
                profile);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            view.entries_.push_back(*entry);
        }
        if (auto error = validate_minimizer_bucket_directory_entries(
                view.entries_, profile)) {
            return std::unexpected(*error);
        }
        return view;
    }

    /// Return the immutable encoded bytes suitable for direct GPU upload.
    [[nodiscard]] std::span<const std::byte> encoded_bytes() const noexcept {
        return encoded_;
    }

    /// Return decoded entries in their stable directory-ordinal order.
    [[nodiscard]] std::span<const grouped_soa_bucket_directory_entry> entries() const noexcept {
        return entries_;
    }

    /// Return the canonical profile used to qualify every decoded entry.
    [[nodiscard]] const grouped_soa_minimizer_profile& profile() const noexcept {
        return profile_;
    }

    /// Heap bytes owned by this view. Readers use capacities, rather than
    /// logical sizes, when retaining a memory-admission permit.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        return bioseq::common::saturating_add(
            static_cast<std::uint64_t>(encoded_.capacity()),
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(entries_.capacity()),
                sizeof(grouped_soa_bucket_directory_entry)));
    }

    /// Resolve one stable directory ordinal.
    [[nodiscard]] const grouped_soa_bucket_directory_entry* entry_by_ordinal(
            std::uint32_t ordinal) const noexcept {
        return ordinal < entries_.size() ? &entries_[ordinal] : nullptr;
    }

    /// Find the unique hash partition in one fixed persisted position core.
    [[nodiscard]] const grouped_soa_bucket_directory_entry* find(
            std::int32_t position_segment,
            std::uint64_t hash) const noexcept {
        std::size_t lo = 0;
        std::size_t hi = entries_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2u;
            const grouped_soa_bucket_directory_entry& entry = entries_[mid];
            if (entry.position_segment < position_segment
                || (entry.position_segment == position_segment
                    && entry.hi_hash < hash)) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (lo >= entries_.size()) {
            return nullptr;
        }
        const grouped_soa_bucket_directory_entry& entry = entries_[lo];
        return entry.position_segment == position_segment
                && entry.lo_hash <= hash && hash <= entry.hi_hash
            ? &entry
            : nullptr;
    }

private:
    grouped_soa_minimizer_profile profile_{};
    std::vector<std::byte> encoded_;
    std::vector<grouped_soa_bucket_directory_entry> entries_;
};

/// Half-open logical-rank bounds for one persisted position segment.
struct minimizer_position_segment_bounds {
    std::uint64_t begin_bp = 0u;
    std::uint64_t end_bp = 0u;

    [[nodiscard]] bool operator==(
        const minimizer_position_segment_bounds&) const = default;
};

/// Derive the overflow-safe half-open core for one persisted segment ordinal.
[[nodiscard]] constexpr std::expected<
    minimizer_position_segment_bounds,
    minimizer_generation_encoding_error>
derive_minimizer_position_segment_bounds(
        const minimizer_generation_manifest& manifest,
        std::uint32_t ordinal) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    if (ordinal >= manifest.position_segment_count) {
        return std::unexpected(minimizer_generation_encoding_error::segment_ordinal);
    }
    if (manifest.position_segment_span_bp != 0
        && ordinal > (std::numeric_limits<std::uint64_t>::max)()
            / manifest.position_segment_span_bp) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t begin =
        static_cast<std::uint64_t>(ordinal) * manifest.position_segment_span_bp;
    if (begin >= manifest.target_length) {
        return std::unexpected(minimizer_generation_encoding_error::segment_ordinal);
    }
    const std::uint64_t remaining = manifest.target_length - begin;
    const std::uint64_t width =
        std::min(manifest.position_segment_span_bp, remaining);
    return minimizer_position_segment_bounds{begin, begin + width};
}

/// Map a half-open base-pair range to the covering persisted segment ordinals.
[[nodiscard]] constexpr std::expected<
    minimizer_position_segment_range,
    minimizer_generation_encoding_error>
minimizer_position_segments_for_bp_range(
        const minimizer_generation_manifest& manifest,
        std::uint64_t begin_bp,
        std::uint64_t end_bp) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    if (begin_bp > end_bp || end_bp > manifest.target_length) {
        return std::unexpected(minimizer_generation_encoding_error::bp_range);
    }
    if (begin_bp == end_bp) {
        const std::uint32_t ordinal = begin_bp == manifest.target_length
            ? manifest.position_segment_count
            : static_cast<std::uint32_t>(
                begin_bp / manifest.position_segment_span_bp);
        return minimizer_position_segment_range{ordinal, ordinal};
    }
    const std::uint64_t first = begin_bp / manifest.position_segment_span_bp;
    const std::uint64_t last = (end_bp - 1u) / manifest.position_segment_span_bp;
    if (last >= manifest.position_segment_count
        || !sql_integer_can_represent(first)
        || !sql_integer_can_represent(last + 1u)) {
        return std::unexpected(minimizer_generation_encoding_error::segment_ordinal);
    }
    return minimizer_position_segment_range{
        static_cast<std::uint32_t>(first),
        static_cast<std::uint32_t>(last + 1u)};
}

/// Compute the compatibility identity from every named semantic/ABI axis.
[[nodiscard]] constexpr std::uint64_t
minimizer_generation_compatibility_checksum(
        const minimizer_generation_manifest& manifest) noexcept {
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    crc64_ecma182_accumulator crc;
    crc.update_little_endian(manifest.alphabet_code);
    crc.update_little_endian(manifest.storage_unit_byte_width);
    crc.update_little_endian(manifest.kmer_size);
    crc.update_little_endian(manifest.window_size);
    crc.update_little_endian(k_minimizer_hash_semantics_revision);
    crc.update_little_endian(
        k_minimizer_segment_local_position_encoding_revision);
    crc.update_little_endian(static_cast<std::uint32_t>(
        k_grouped_soa_payload_layout_abi_revision));
    crc.update_little_endian(manifest.segment_geometry_revision);
    crc.update_little_endian(manifest.position_segment_span_bp);
    crc.update_little_endian(manifest.position_segment_count);
    crc.update_little_endian(manifest.gpu_abi_revision);
    crc.update_little_endian(
        static_cast<std::uint8_t>(manifest.producer_backend_id));
    crc.update_little_endian(manifest.producer_identity_revision);
    // Named schemes extend the checksum with their frozen wire
    // identity so otherwise identical indexes cannot cross scheme boundaries.
    if (manifest.minimizer_scheme_id != k_legacy_minimap_scheme_id
        || manifest.minimizer_scheme_revision
            != k_legacy_minimap_scheme_revision) {
        crc.update_little_endian(manifest.minimizer_scheme_id);
        crc.update_little_endian(manifest.minimizer_scheme_revision);
    }
    crc.update_little_endian(static_cast<std::uint16_t>(manifest.layout));
    if (profile) {
        crc.update_little_endian(std::uint16_t{0u});
        crc.update_little_endian(static_cast<std::uint16_t>(
            profile->position_width));
        crc.update_little_endian(profile->max_hash);
        crc.update_little_endian(static_cast<std::uint8_t>(
            profile->hash_width));
        crc.update_little_endian(static_cast<std::uint8_t>(
            profile->position_width));
        crc.update_little_endian(static_cast<std::uint8_t>(
            profile->count_width));
        crc.update_little_endian(profile->max_occ_cap);
    }
    return crc.checksum();
}

/// Reject a persisted generation that does not match the caller's biological
/// minimizer scheme or the fixed representation axes used to consume it.
[[nodiscard]] constexpr std::expected<void, minimizer_generation_encoding_error>
validate_minimizer_generation_compatibility(
        const minimizer_generation_manifest& manifest,
        const expected_minimizer_generation_identity& expected) noexcept {
    if (auto error = minimizer_generation_wire_detail::validate_manifest_standalone(
            manifest)) {
        return std::unexpected(*error);
    }
    const auto manifest_profile = derive_grouped_soa_minimizer_profile(manifest);
    const auto expected_profile = manifest_profile;
    const minimizer_producer_identity expected_producer_backend{
        .id = expected.producer_backend_id,
        .revision = expected.producer_identity_revision,
    };
    if (!manifest_profile || !expected_profile
        || !is_supported_minimizer_producer_backend_identity(
            expected_producer_backend)
        || minimizer_scheme_identity_for_producer_backend(
            expected_producer_backend) != expected_profile->minimizer_scheme
        || *manifest_profile != *expected_profile
        || manifest.gpu_abi_revision != manifest_profile->gpu_abi_revision
        || expected.gpu_abi_revision != expected_profile->gpu_abi_revision
        || manifest.layout != expected.layout
        || manifest.alphabet_code != expected.alphabet_code
        || manifest.storage_unit_byte_width
            != expected.storage_unit_byte_width
        || manifest.kmer_size != expected.kmer_size
        || manifest.window_size != expected.window_size
        || manifest.gpu_abi_revision != expected.gpu_abi_revision
        || manifest.minimizer_scheme_id != expected.minimizer_scheme_id
        || manifest.minimizer_scheme_revision
            != expected.minimizer_scheme_revision
        || manifest.producer_backend_id != expected.producer_backend_id
        || manifest.producer_identity_revision
            != expected.producer_identity_revision
        || !expected.max_occ_cap
        || *expected.max_occ_cap != manifest_profile->max_occ_cap) {
        return std::unexpected(minimizer_generation_encoding_error::compatibility_mismatch);
    }
    return {};
}

/// Validate head/manifest/directory equations once all metadata objects exist.
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
validate_minimizer_generation_metadata(
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const grouped_soa_directory_view& directory) noexcept {
    if (auto error = minimizer_generation_wire_detail::validate_manifest_standalone(
            manifest)) {
        return std::unexpected(*error);
    }
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    if (!profile || directory.profile() != *profile) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    if (head.layout != manifest.layout) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    if (head.generation_id != manifest.generation_id
        || head.source_identity != manifest.source_identity
        || head.source_content_checksum != manifest.source_content_checksum) {
        return std::unexpected(minimizer_generation_encoding_error::identity_mismatch);
    }
    const auto manifest_bytes =
        encode_minimizer_generation_manifest_record(manifest);
    if (!manifest_bytes
        || head.manifest_checksum != crc64_ecma182(
            std::span<const std::byte>{*manifest_bytes})) {
        return std::unexpected(minimizer_generation_encoding_error::checksum);
    }
    if (manifest.gpu_abi_revision != profile->gpu_abi_revision
        || head.compatibility_checksum
            != minimizer_generation_compatibility_checksum(manifest)) {
        return std::unexpected(minimizer_generation_encoding_error::compatibility_mismatch);
    }
    if (manifest.directory_bytes != directory.encoded_bytes().size()
        || manifest.directory_checksum
            != crc64_ecma182(directory.encoded_bytes())) {
        return std::unexpected(minimizer_generation_encoding_error::checksum);
    }
    const std::uint32_t extent_payload_bytes =
        minimizer_extent_payload_bytes(manifest.layout);
    if (extent_payload_bytes == 0) {
        return std::unexpected(minimizer_generation_encoding_error::format_revision);
    }
    const std::uint64_t expected_directory_chunks =
        manifest.directory_bytes / extent_payload_bytes
        + (manifest.directory_bytes % extent_payload_bytes != 0 ? 1u : 0u);
    if (expected_directory_chunks != manifest.directory_chunk_count) {
        return std::unexpected(minimizer_generation_encoding_error::chunk_range);
    }

    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    for (const grouped_soa_bucket_directory_entry& entry : directory.entries()) {
        if (static_cast<std::uint64_t>(entry.position_segment)
            >= manifest.position_segment_count) {
            return std::unexpected(minimizer_generation_encoding_error::position_segment);
        }
        const std::uint64_t expected_control_chunks =
            entry.control_encoded_bytes / extent_payload_bytes
            + (entry.control_encoded_bytes % extent_payload_bytes != 0
                ? 1u : 0u);
        const std::uint64_t expected_position_chunks =
            entry.position_encoded_bytes / extent_payload_bytes
            + (entry.position_encoded_bytes % extent_payload_bytes != 0
                ? 1u : 0u);
        if (entry.control_first_chunk != 0
            || entry.control_chunk_count != expected_control_chunks
            || entry.position_first_chunk != 0
            || entry.position_chunk_count != expected_position_chunks) {
            return std::unexpected(minimizer_generation_encoding_error::chunk_range);
        }
        if (control_count > (std::numeric_limits<std::uint64_t>::max)()
                - entry.control_element_count
            || position_count > (std::numeric_limits<std::uint64_t>::max)()
                - entry.position_element_count
            || payload_bytes > (std::numeric_limits<std::uint64_t>::max)()
                - entry.control_encoded_bytes
            || payload_bytes + entry.control_encoded_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - entry.position_encoded_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        control_count += entry.control_element_count;
        position_count += entry.position_element_count;
        payload_bytes += entry.control_encoded_bytes + entry.position_encoded_bytes;
    }
    if (directory.entries().size()
            > (std::numeric_limits<std::uint32_t>::max)() / 2u
        || manifest.extent_count != directory.entries().size() * 2u
        || manifest.control_count != control_count
        || manifest.position_count != position_count
        || manifest.payload_bytes != payload_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::element_count);
    }
    return {};
}

/// Current grouped-SoA implementation.
struct minimizer_hashmap_table_allocation_plan {
    std::uint64_t distinct_upper_bound = 0u;
    std::uint64_t requested_slots = 0u;
    std::uint64_t group_count = 0u;
    std::uint64_t capacity = 0u;
    std::uint64_t allocation_bytes = 0u;
    bool valid = false;
};

[[nodiscard]] constexpr minimizer_hashmap_table_allocation_plan
minimizer_hashmap_table_allocation_plan(
        std::uint64_t possible_distinct) noexcept {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    constexpr std::uint64_t sentinel_margin = 64u;
    constexpr std::uint64_t load_numerator = 7u;
    constexpr std::uint64_t load_denominator = 8u;
    constexpr std::uint64_t slots_per_group = 15u;
    constexpr std::uint64_t bytes_per_group = 256u;
    constexpr std::uint64_t max =
        (std::numeric_limits<std::uint64_t>::max)();

    // An empty sketch builds no table at all.  Keep that real lifetime in the
    // exact plan instead of charging the non-empty sentinel margin and then
    // rejecting the builder's zero retained bytes at handoff validation.
    if (possible_distinct == 0u) {
        return minimizer_hashmap_table_allocation_plan{.valid = true};
    }

    const std::uint64_t bounded_distinct =
        saturating_add(possible_distinct, sentinel_margin);
    if (bounded_distinct == max) return {};
    const std::uint64_t quotient = bounded_distinct / load_numerator;
    const std::uint64_t remainder = bounded_distinct % load_numerator;
    const std::uint64_t requested_slots = saturating_add(
        saturating_mul(quotient, load_denominator),
        (remainder * load_denominator + load_numerator - 1u)
            / load_numerator);
    if (requested_slots == max) return {};
    const std::uint64_t groups_needed =
        requested_slots / slots_per_group + 1u;
    std::uint64_t groups = 2u;
    while (groups < groups_needed) {
        if (groups > max / 2u) return {};
        groups *= 2u;
    }
    const std::uint64_t capacity = saturating_mul(
        groups, slots_per_group);
    const std::uint64_t allocation_bytes = saturating_mul(
        groups, bytes_per_group);
    if (capacity == max || allocation_bytes == max || capacity == 0u) {
        return {};
    }
    return minimizer_hashmap_table_allocation_plan{
        .distinct_upper_bound = bounded_distinct,
        .requested_slots = requested_slots,
        .group_count = groups,
        .capacity = capacity - 1u,
        .allocation_bytes = allocation_bytes,
        .valid = true,
    };
}

// (1) the lookup structure — the old TargetIndex, renamed & relocated, storage-free
struct minimizer_hashmap_fragment_index {
private:
    std::unique_ptr<detail::minimizer_hashmap_fragment_index_table> table_;

    void clear_table();
    void reserve_table(std::size_t bucket_count);
    void reserve_table_for_distinct_upper_bound(std::size_t distinct_count);
    void emplace_table(std::uint64_t hash, std::uint64_t packed);

    template <class p_minimizer_type>
    static void sort_minimizers_in_place_(
            std::vector<p_minimizer_type>& sorted_minimizers) {
        if (sorted_minimizers.empty()) return;
        // `MinimizerRepr::kmer()` deliberately exposes a uint64_t ABI even
        // when the selected representation carries a much narrower hash.  Do
        // not let that ABI choice force radix_sort through known-zero high
        // bytes: XXL_HIFI uses MinimizerRepr<33, 15> with exactly 30 hash bits,
        // so a uint64_t key performs four all-zero radix levels over the whole
        // target before reaching a significant byte.  Returning the smallest
        // unsigned key type that can represent the public kmer() domain
        // preserves the ordering exactly while removing those scans.  The
        // exported 128-bit representation can report a layout hash field
        // wider than 64 bits while kmer() intentionally remains uint64_t; it
        // therefore stays on the uint64_t radix key.
        using minimizer_layout = typename p_minimizer_type::layout_type;
        using public_kmer = decltype(std::declval<const p_minimizer_type&>().kmer());
        static_assert(minimizer_layout::hash_bits > 0u);
        static_assert(std::is_same_v<public_kmer, std::uint64_t>);
        using kmer_radix_key = std::conditional_t<
            (minimizer_layout::hash_bits <= 8u), std::uint8_t,
            std::conditional_t<
                (minimizer_layout::hash_bits <= 16u), std::uint16_t,
                std::conditional_t<
                    (minimizer_layout::hash_bits <= 32u), std::uint32_t,
                    std::uint64_t>>>;
        struct kmer_key {
            [[nodiscard, gnu::always_inline]]
            inline kmer_radix_key operator()(const p_minimizer_type& m) const noexcept {
                return static_cast<kmer_radix_key>(m.kmer());
            }
        };
        bioseq::sorting::radix_sort(
            sorted_minimizers.data(),
            sorted_minimizers.data() + sorted_minimizers.size(),
            kmer_key{});
    }

    template <class p_minimizer_type>
    void build_presorted_in_place_(
            std::vector<p_minimizer_type>& sorted_minimizers,
            sequence_size_t target_length,
            bool reserve_full_table_once) {
        target_len = target_length;
        clear_table();
        positions.clear();

        const std::size_t total = sorted_minimizers.size();
        if (total == 0u) {
            return;
        }

        positions.resize(total);
        if (reserve_full_table_once) {
            // The admitted Vulkan route reserves for the worst-case D=N once.
            // This removes the former half-size allocation/rehash overlap.
            reserve_table_for_distinct_upper_bound(total);
        } else {
            // Preserve the established policy for every unrelated CPU caller.
            reserve_table(total / 2u + 16u);
        }
        std::size_t i = 0;
        while (i < total) {
            const std::uint64_t kmer = sorted_minimizers[i].kmer();
            std::size_t j = i;
            while (j < total && sorted_minimizers[j].kmer() == kmer) {
                positions[j] = sorted_minimizers[j].pos_strand_encoded();
                ++j;
            }
            emplace_table(kmer,
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32)
                | static_cast<std::uint64_t>(static_cast<std::uint32_t>(j - i)));
            i = j;
        }
    }

    template <class p_minimizer_type>
    void build_sorted_in_place(
            std::vector<p_minimizer_type>& sorted_minimizers,
            sequence_size_t target_length,
            bool reserve_full_table_once = false) {
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("target_index_build");
        auto _scope = bioseq::instrumentation::region(_desc);
        sort_minimizers_in_place_(sorted_minimizers);
        build_presorted_in_place_(
            sorted_minimizers, target_length, reserve_full_table_once);
    }

public:
    std::vector<std::uint64_t> positions;        // (last_pos<<1)|strand, full uint64
    sequence_size_t target_len = 0;

    minimizer_hashmap_fragment_index();
    ~minimizer_hashmap_fragment_index();
    minimizer_hashmap_fragment_index(const minimizer_hashmap_fragment_index& other);
    minimizer_hashmap_fragment_index& operator=(const minimizer_hashmap_fragment_index& other);
    minimizer_hashmap_fragment_index(minimizer_hashmap_fragment_index&& other) noexcept;
    minimizer_hashmap_fragment_index& operator=(minimizer_hashmap_fragment_index&& other) noexcept;

    static constexpr std::uint32_t k_encoded_magic   = 0xB1000001u;   // unchanged wire format
    static constexpr std::uint32_t k_encoded_format_revision = 2u;
    static constexpr std::string_view index_name     = k_minimizers_per_fragment_index_name;

    struct encoded_header {
        std::uint32_t magic;
        std::uint32_t revision;
        std::uint64_t target_len;       ///< Full 64-bit target length.
        std::uint32_t bucket_count;
        std::uint32_t reserved;         ///< pad; written as 0
        std::uint64_t total_positions;
    };
    struct encoded_bucket {
        std::uint64_t hash;
        std::uint32_t pos_offset;
        std::uint32_t pos_count;
    };
    static constexpr std::size_t k_encoded_header_bytes = sizeof(encoded_header);
    static constexpr std::size_t k_encoded_bucket_bytes = sizeof(encoded_bucket);
    static_assert(k_encoded_header_bytes == 32, "wire header layout pinned");
    static_assert(k_encoded_bucket_bytes == 16, "wire bucket layout pinned");
    static_assert(std::endian::native == std::endian::little, "wire format requires little-endian host");
    static_assert(sizeof(std::size_t) >= 8, "wire format requires 64-bit size_t");

    [[nodiscard]] std::size_t table_size() const noexcept;
    [[nodiscard]] bool table_empty() const noexcept;
    [[nodiscard]] std::uint64_t table_allocation_bytes() const noexcept;
    [[nodiscard]] std::uint64_t table_peak_allocation_bytes() const noexcept;
    void for_each_table_entry_erased(
        void* ctx,
        void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const;

    template <class F>
    void for_each_table_entry(F&& emit) const {
        using fn = std::remove_reference_t<F>;
        for_each_table_entry_erased(
            &emit,
            [](void* ctx, std::uint64_t hash, std::uint64_t packed) {
                (*static_cast<fn*>(ctx))(hash, packed);
            });
    }

    template <class p_minimizer_type>
    void build(const std::vector<p_minimizer_type>& target_minimizers,
               sequence_size_t target_length) {
        // A call-local copy is intentional.  The old static thread_local
        // scratch retained an XXL target-sized allocation for the lifetime of
        // the worker thread, making unrelated later GPU stages pay its RSS.
        std::vector<p_minimizer_type> sorted_minimizers{
            target_minimizers.begin(), target_minimizers.end()};
        build_sorted_in_place(sorted_minimizers, target_length);
    }

    /// Sort one caller-owned minimizer source by its canonical hash without
    /// constructing the FOA lookup table or a positions vector.  The direct
    /// mapped Vulkan route uses this same production radix implementation,
    /// then streams the sorted source into its final resident allocation.
    template <class p_minimizer_type>
    static void sort_source_by_hash_in_place(
            std::vector<p_minimizer_type>& source) {
        sort_minimizers_in_place_(source);
    }

    /// Build a transient lookup index by consuming caller-owned minimizers.
    /// Sorting happens in the caller's allocation and the vector is
    /// deterministically deallocated before this function returns.
    template <class p_minimizer_type>
    void build_consuming(
            std::vector<p_minimizer_type>&& target_minimizers,
            sequence_size_t target_length) {
        build_sorted_in_place(target_minimizers, target_length);
        std::vector<p_minimizer_type>{}.swap(target_minimizers);
    }

    /// Admitted Vulkan build: reserve one table for worst-case D=N so no FOA
    /// growth allocation can overlap it. The caller must acquire positions +
    /// planned-table admission before entering this method.
    template <class p_minimizer_type>
    void build_consuming_full_table(
            std::vector<p_minimizer_type>&& target_minimizers,
            sequence_size_t target_length) {
        build_sorted_in_place(
            target_minimizers,
            target_length,
            /*reserve_full_table_once=*/true);
        std::vector<p_minimizer_type>{}.swap(target_minimizers);
    }

    /// Deallocate lookup storage after the final seed consumer.  This is for
    /// an operation-owned transient index only; borrowed persisted indexes
    /// must never be passed to the post-seed lifetime coordinator.
    void release_storage() noexcept;

    /// Radix-bin pre-extracted global (kmer, encoded_position) pairs.
    /// `pairs` is MUTABLE and radix-sorted IN PLACE (caller-owned scratch; no copy).
    void build_from_pairs(std::span<std::pair<std::uint64_t,std::uint64_t>> pairs,
                          sequence_size_t target_length);

    [[nodiscard]] std::span<const std::uint64_t> lookup(std::uint64_t kmer_hash) const;

    void add_position_offset(sequence_size_t offset);

    [[nodiscard]] sequence_size_t target_length() const { return target_len; }

    [[nodiscard]] std::size_t serialize_binary_size() const;

    void serialize_to_binary(char* output) const;

    [[nodiscard]] static minimizer_hashmap_fragment_index deserialize_from_binary(
        const char* input, std::size_t input_size);
};
static_assert(minimizer_lookup_index<minimizer_hashmap_fragment_index>,
              "minimizer_hashmap_fragment_index must model minimizer_lookup_index");
static_assert(persistable_minimizer_index<minimizer_hashmap_fragment_index>,
              "minimizer_hashmap_fragment_index must model persistable_minimizer_index");

/// Encode one sorted-dense control without persisting native struct layout.
[[nodiscard]] constexpr std::array<std::byte, 16>
encode_minimizer_serialized_bucket(
        const minimizer_hashmap_fragment_index::encoded_bucket& bucket) noexcept {
    std::array<std::byte, 16> output{};
    std::span<std::byte> bytes{output};
    minimizer_generation_wire_detail::write_le<std::uint64_t>(
        bytes, 0, bucket.hash);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(
        bytes, 8, bucket.pos_offset);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(
        bytes, 12, bucket.pos_count);
    return output;
}

/// Decode one exact sorted-dense control from fixed little-endian bytes.
[[nodiscard]] constexpr std::expected<
    minimizer_hashmap_fragment_index::encoded_bucket,
    minimizer_generation_encoding_error>
decode_minimizer_serialized_bucket(
        std::span<const std::byte> input) noexcept {
    if (input.size() != 16u) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    return minimizer_hashmap_fragment_index::encoded_bucket{
        .hash = minimizer_generation_wire_detail::read_le<std::uint64_t>(
            input, 0),
        .pos_offset = minimizer_generation_wire_detail::read_le<std::uint32_t>(
            input, 8),
        .pos_count = minimizer_generation_wire_detail::read_le<std::uint32_t>(
            input, 12),
    };
}

/// Current grouped-SoA implementation.
struct grouped_soa_decoded_control {
    std::uint64_t hash = 0;
    std::uint64_t position_begin = 0;
    std::uint64_t position_end = 0;
    std::uint64_t saturated_count = 0;

    [[nodiscard]] bool operator==(
        const grouped_soa_decoded_control&) const = default;
};

/// Callback-scoped qualified row emitted directly to PostgreSQL/fake storage.
struct minimizer_generation_row_view {
    index_chunk_address address{};
    std::span<const std::byte> bytes;
    std::uint64_t row_checksum = 0;
};

/// Exact failure site retained alongside the stable wire error
/// `minimizer_generation_encoding_error::admission`.  The numeric wire-error ABI does
/// not change; this stats explains which owner or allocation was refused.
enum class grouped_soa_query_admission_stage : std::uint8_t {
    none,
    identity_and_directory_reservation,
    query_hash_reservation,
    query_hash_allocation,
    query_hash_reconcile,
    query_route_reservation,
    query_route_allocation,
    query_route_reconcile,
    control_payload_reservation,
    control_payload_allocation,
    control_payload_reconcile,
    control_decode_reservation,
    control_decode_allocation,
    control_decode_reconcile,
    position_image_reservation,
    position_image_allocation,
    position_image_reconcile,
};

[[nodiscard]] constexpr std::string_view
grouped_soa_query_admission_substage_name(
        grouped_soa_query_admission_stage substage) noexcept {
    switch (substage) {
        case grouped_soa_query_admission_stage::none:
            return "none";
        case grouped_soa_query_admission_stage::
                identity_and_directory_reservation:
            return "identity_and_directory_reservation";
        case grouped_soa_query_admission_stage::query_hash_reservation:
            return "query_hash_reservation";
        case grouped_soa_query_admission_stage::query_hash_allocation:
            return "query_hash_allocation";
        case grouped_soa_query_admission_stage::query_hash_reconcile:
            return "query_hash_reconcile";
        case grouped_soa_query_admission_stage::query_route_reservation:
            return "query_route_reservation";
        case grouped_soa_query_admission_stage::query_route_allocation:
            return "query_route_allocation";
        case grouped_soa_query_admission_stage::query_route_reconcile:
            return "query_route_reconcile";
        case grouped_soa_query_admission_stage::control_payload_reservation:
            return "control_payload_reservation";
        case grouped_soa_query_admission_stage::control_payload_allocation:
            return "control_payload_allocation";
        case grouped_soa_query_admission_stage::control_payload_reconcile:
            return "control_payload_reconcile";
        case grouped_soa_query_admission_stage::control_decode_reservation:
            return "control_decode_reservation";
        case grouped_soa_query_admission_stage::control_decode_allocation:
            return "control_decode_allocation";
        case grouped_soa_query_admission_stage::control_decode_reconcile:
            return "control_decode_reconcile";
        case grouped_soa_query_admission_stage::position_image_reservation:
            return "position_image_reservation";
        case grouped_soa_query_admission_stage::position_image_allocation:
            return "position_image_allocation";
        case grouped_soa_query_admission_stage::position_image_reconcile:
            return "position_image_reconcile";
    }
    return "unknown";
}

/// Frozen stats copied out before a failed reader releases its admitted
/// storage.  Capacity fields describe actual live owners at the failure site;
/// the controller fields come from the exact rejection snapshot when one
/// exists, otherwise from a read-only post-failure diagnostic snapshot.
struct grouped_soa_query_admission_stats {
    grouped_soa_query_admission_stage substage =
        grouped_soa_query_admission_stage::none;
    bioseq::parallelism::memory_admission_failure_reason controller_reason =
        bioseq::parallelism::memory_admission_failure_reason::none;
    std::uint64_t retained_position_count = 0;
    std::uint64_t retained_position_bytes = 0;
    std::uint64_t allocation_headroom_bytes = 0;
    std::uint64_t projected_bytes = 0;
    std::uint64_t requested_growth_bytes = 0;
    std::uint64_t owned_capacity_bytes = 0;
    std::uint64_t permit_bytes = 0;
    std::uint64_t controller_reserved_bytes = 0;
    std::uint64_t controller_budget_ceiling_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t controller_allocatable_after_reservations_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t process_footprint_bytes = 0;
    std::uint64_t system_available_bytes = 0;
    std::uint64_t system_total_bytes = 0;
    std::uint64_t process_soft_cap_bytes = 0;
    std::uint64_t query_record_count = 0;
    std::uint64_t query_record_capacity = 0;
    std::uint64_t query_hash_count = 0;
    std::uint64_t query_hash_capacity = 0;
    std::uint64_t selected_extent_count = 0;
    std::uint64_t selected_extent_capacity = 0;
    std::uint64_t selected_control_capacity_bytes = 0;
    std::uint64_t control_payload_capacity_bytes = 0;
    std::uint64_t releasable_protocol_query_hash_bytes = 0;
    bool controller_failure_snapshot_present = false;
    bool injected_failure = false;
};

/// Deterministic portable allocation-failure hook.  Production callers leave
/// this at `none`; tests select one exact substage without attempting a huge
/// allocation or depending on an operating-system overcommit policy.
struct grouped_soa_query_reader_options {
    grouped_soa_query_admission_stage fail_admission_substage =
        grouped_soa_query_admission_stage::none;
};

struct grouped_soa_position_image_plan {
    std::uint64_t retained_position_count = 0;
    std::uint64_t retained_position_bytes = 0;
    std::uint64_t allocation_headroom_bytes = 0;
    std::uint64_t owned_capacity_bytes = 0;
    std::uint64_t projected_bytes = 0;
};

/// Exact cached ownership ledger for the trait-driven prepared-reader
/// protocol. This is accounting state for the existing memory_permit, not a
/// second admission system. Every field names one live allocation owner and
/// `total_bytes` is updated in O(1) whenever that owner changes.
struct protocol_owned_bytes_ledger {
    std::uint64_t directory_bytes = 0;
    std::uint64_t query_hash_bytes = 0;
    std::uint64_t query_record_bytes = 0;
    std::uint64_t radix_offset_bytes = 0;
    std::uint64_t global_position_bytes = 0;
    std::uint64_t ordinal_map_bytes = 0;
    std::uint64_t selected_extent_vector_bytes = 0;
    std::uint64_t selected_extent_owner_bytes = 0;
    std::uint64_t query_record_position_bytes = 0;
    std::uint64_t control_scratch_bytes = 0;
    std::uint64_t active_session_bytes = 0;
    std::uint64_t total_bytes = 0;

    protocol_owned_bytes_ledger() = default;
    protocol_owned_bytes_ledger(const protocol_owned_bytes_ledger&) = default;
    protocol_owned_bytes_ledger& operator=(
        const protocol_owned_bytes_ledger&) = default;

    protocol_owned_bytes_ledger(
            protocol_owned_bytes_ledger&& other) noexcept
        : protocol_owned_bytes_ledger(
            static_cast<const protocol_owned_bytes_ledger&>(other)) {
        other.reset();
    }

    protocol_owned_bytes_ledger& operator=(
            protocol_owned_bytes_ledger&& other) noexcept {
        if (this != &other) {
            *this = static_cast<const protocol_owned_bytes_ledger&>(other);
            other.reset();
        }
        return *this;
    }

    /// Replace one already-accounted owner. The caller publishes its named
    /// field only after this checked total update succeeds.
    [[nodiscard]] bool replace_checked(
            std::uint64_t old_bytes,
            std::uint64_t new_bytes) noexcept {
        if (old_bytes > total_bytes) return false;
        const std::uint64_t without_old = total_bytes - old_bytes;
        if (new_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - without_old) {
            return false;
        }
        total_bytes = without_old + new_bytes;
        return true;
    }

    /// Check a temporary coexistence peak before asking the existing permit
    /// to grow. No ownership is published by this helper.
    [[nodiscard]] bool add_temporary_peak_checked(
            std::uint64_t peak_bytes) const noexcept {
        return peak_bytes
            <= (std::numeric_limits<std::uint64_t>::max)() - total_bytes;
    }

    [[nodiscard]] bool field_sum_holds() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t sum = directory_bytes;
        sum = saturating_add(sum, query_hash_bytes);
        sum = saturating_add(sum, query_record_bytes);
        sum = saturating_add(sum, radix_offset_bytes);
        sum = saturating_add(sum, global_position_bytes);
        sum = saturating_add(sum, ordinal_map_bytes);
        sum = saturating_add(sum, selected_extent_vector_bytes);
        sum = saturating_add(sum, selected_extent_owner_bytes);
        sum = saturating_add(sum, query_record_position_bytes);
        sum = saturating_add(sum, control_scratch_bytes);
        sum = saturating_add(sum, active_session_bytes);
        return sum != (std::numeric_limits<std::uint64_t>::max)()
            && sum == total_bytes;
    }

    void reset() noexcept {
        directory_bytes = 0;
        query_hash_bytes = 0;
        query_record_bytes = 0;
        radix_offset_bytes = 0;
        global_position_bytes = 0;
        ordinal_map_bytes = 0;
        selected_extent_vector_bytes = 0;
        selected_extent_owner_bytes = 0;
        query_record_position_bytes = 0;
        control_scratch_bytes = 0;
        active_session_bytes = 0;
        total_bytes = 0;
    }
};

/// Pure checked byte plan used before any position-image reservation or
/// allocation.  Tests can exercise adversarial counts without relying on
/// virtual-memory overcommit or constructing a correspondingly huge payload.
[[nodiscard]] inline std::expected<grouped_soa_position_image_plan,
    minimizer_generation_encoding_error>
plan_grouped_soa_position_image(
        std::uint64_t retained_position_count,
        std::uint64_t owned_capacity_bytes) noexcept {
    const std::uint64_t retained_position_bytes =
        bioseq::common::saturating_mul(
            retained_position_count, sizeof(std::uint64_t));
    // The grouped-SoA image is one exact owned array: unlike a vector reserve,
    // its requested element count and admitted ownership are both exactly P.
    // Keep headroom explicit at zero so SQL stats can distinguish this
    // path from conservative vector preallocation.
    constexpr std::uint64_t allocation_headroom_bytes = 0u;
    const std::uint64_t projected_bytes =
        bioseq::common::saturating_add(
            owned_capacity_bytes, retained_position_bytes);
    if (retained_position_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
        || projected_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return grouped_soa_position_image_plan{
        .retained_position_count = retained_position_count,
        .retained_position_bytes = retained_position_bytes,
        .allocation_headroom_bytes = allocation_headroom_bytes,
        .owned_capacity_bytes = owned_capacity_bytes,
        .projected_bytes = projected_bytes,
    };
}

namespace minimizer_prepared_reader_detail {
template <class traits>
class trait_driven_prepared_query_core;
}

/// Unforgeable stats that the address-bound row checksum was checked before
/// callback-scoped bytes reached a prepared reader. Ordinary importers can
/// obtain one only through validate_minimizer_generation_row_receipt(), which
/// performs that check. PostgreSQL retains that same receipt beside the loaded
/// row, so consumers reuse the single validation pass without special access.

class validated_minimizer_generation_row_receipt {
private:
    index_chunk_address address_{};
    std::span<const std::byte> bytes_{};
    std::uint64_t row_checksum_ = 0;
    std::uint64_t authority_seal_ = 0;
    bool live_ = false;

    [[nodiscard]] static std::uint64_t authority_seal(
            const validated_minimizer_generation_row_receipt* self,
            const index_chunk_address& address,
            std::span<const std::byte> bytes,
            std::uint64_t row_checksum) noexcept {
        std::uint64_t value = 0x9E3779B97F4A7C15ULL;
        const auto mix = [&value](std::uint64_t word) noexcept {
            value ^= word + 0x9E3779B97F4A7C15ULL
                + (value << 6u) + (value >> 2u);
        };
        mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(self)));
        mix(address.generation_id);
        mix(static_cast<std::uint32_t>(address.position_segment));
        mix(static_cast<std::uint64_t>(address.sub_key));
        mix(address.chunk_no);
        mix(static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(bytes.data())));
        mix(bytes.size());
        mix(row_checksum);
        return value;
    }

    validated_minimizer_generation_row_receipt(
            index_chunk_address address,
            std::span<const std::byte> bytes,
            std::uint64_t row_checksum) noexcept
        : address_(address),
          bytes_(bytes),
          row_checksum_(row_checksum),
          authority_seal_(authority_seal(this, address, bytes, row_checksum)),
          live_(true) {}

    void invalidate() noexcept {
        address_ = {};
        bytes_ = {};
        row_checksum_ = 0;
        authority_seal_ = 0;
        live_ = false;
    }

    friend std::expected<
        validated_minimizer_generation_row_receipt,
        minimizer_generation_encoding_error>
    validate_minimizer_generation_row_receipt(
        const minimizer_generation_row_view&) noexcept;
    template <class traits>
    friend class minimizer_prepared_reader_detail::
        trait_driven_prepared_query_core;
    template <class Visitor>
    friend bool visit_validated_minimizer_generation_row(
        const validated_minimizer_generation_row_receipt&,
        Visitor&&) noexcept(std::is_nothrow_invocable_v<
            Visitor&, const index_chunk_address&,
            std::span<const std::byte>, std::uint64_t>);

public:
    validated_minimizer_generation_row_receipt() = delete;
    validated_minimizer_generation_row_receipt(
        const validated_minimizer_generation_row_receipt&) = delete;
    validated_minimizer_generation_row_receipt& operator=(
        const validated_minimizer_generation_row_receipt&) = delete;
    validated_minimizer_generation_row_receipt& operator=(
        validated_minimizer_generation_row_receipt&&) = delete;

    validated_minimizer_generation_row_receipt(
            validated_minimizer_generation_row_receipt&& other) noexcept
        : address_(other.address_),
          bytes_(other.bytes_),
          row_checksum_(other.row_checksum_),
          authority_seal_(authority_seal(
              this, address_, bytes_, row_checksum_)),
          live_(other.live_) {
        other.invalidate();
    }

    ~validated_minimizer_generation_row_receipt() { invalidate(); }

    [[nodiscard]] bool has_valid_authority() const noexcept {
        return live_ && authority_seal_ == authority_seal(
            this, address_, bytes_, row_checksum_);
    }

private:
    [[nodiscard]] constexpr const index_chunk_address& address()
            const noexcept {
        return address_;
    }
    [[nodiscard]] constexpr std::span<const std::byte> bytes()
            const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr std::uint64_t row_checksum() const noexcept {
        return row_checksum_;
    }
};

/// Visit an authenticated row without exporting the receipt's representation.
/// The byte span remains callback-scoped; callers cannot retain authority or
/// mutate the receipt, and an invalid/moved-from receipt is never visited.
template <class Visitor>
[[nodiscard]] bool visit_validated_minimizer_generation_row(
    const validated_minimizer_generation_row_receipt& receipt,
    Visitor&& visitor) noexcept(std::is_nothrow_invocable_v<
        Visitor&, const index_chunk_address&,
        std::span<const std::byte>, std::uint64_t>)
{
    if (!receipt.has_valid_authority()) return false;
    std::forward<Visitor>(visitor)(
        receipt.address_, receipt.bytes_, receipt.row_checksum_);
    return true;
}

/// Authenticate one callback-scoped row before granting the trusted-reader
/// fast path. This is deliberately the only exported receipt construction
/// path, and it never returns authority for an unchecked or mismatched CRC.
[[nodiscard]] inline std::expected<
    validated_minimizer_generation_row_receipt,
    minimizer_generation_encoding_error>
validate_minimizer_generation_row_receipt(
        const minimizer_generation_row_view& row) noexcept {
    if (row.bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t expected_checksum = crc64_index_chunk_row(
        row.address.generation_id,
        row.address.position_segment,
        row.address.sub_key,
        row.address.chunk_no,
        static_cast<std::uint32_t>(row.bytes.size()),
        row.bytes);
    if (row.row_checksum != expected_checksum) {
        return std::unexpected(minimizer_generation_encoding_error::checksum);
    }
    return validated_minimizer_generation_row_receipt{
        row.address, row.bytes, row.row_checksum};
}

static_assert(!std::is_default_constructible_v<
    validated_minimizer_generation_row_receipt>);
static_assert(!std::is_copy_constructible_v<
    validated_minimizer_generation_row_receipt>);
static_assert(!std::is_copy_assignable_v<
    validated_minimizer_generation_row_receipt>);
static_assert(std::is_move_constructible_v<
    validated_minimizer_generation_row_receipt>);
static_assert(!std::is_move_assignable_v<
    validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_copyable_v<
    validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_move_constructible_v<
    validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_destructible_v<
    validated_minimizer_generation_row_receipt>);
static_assert(!std::is_aggregate_v<
    validated_minimizer_generation_row_receipt>);

namespace minimizer_prepared_reader_detail {

template <minimizer_generation_layout p_layout>
struct codec_traits;

template <>
struct codec_traits<
    minimizer_generation_layout::grouped_soa_segment_local> {
    static constexpr minimizer_generation_layout layout =
        minimizer_generation_layout::grouped_soa_segment_local;
    using control_type = grouped_soa_decoded_control;
    using position_type = std::uint64_t;
    using hash_type = std::uint64_t;
    static constexpr bool global_controls_then_positions = true;
};

/// Shared move-only admitted identity/directory owner used by both prepared
/// grouped readers. codec_traits is the format authority; callers cannot decode
/// a directory under a different wire and then relabel the facade.
template <class traits>
class admitted_prepared_reader_core {
protected:
    bioseq::parallelism::memory_permit permit_{};
    grouped_soa_directory_view directory_{};
    minimizer_generation_head head_{};
    minimizer_generation_manifest manifest_{};
    expected_minimizer_generation_identity expected_{};
    grouped_soa_minimizer_profile profile_{};

    admitted_prepared_reader_core() = default;
    admitted_prepared_reader_core(admitted_prepared_reader_core&&) noexcept = default;
    admitted_prepared_reader_core& operator=(
        admitted_prepared_reader_core&&) noexcept = default;
    admitted_prepared_reader_core(const admitted_prepared_reader_core&) = delete;
    admitted_prepared_reader_core& operator=(
        const admitted_prepared_reader_core&) = delete;

    [[nodiscard]] static std::expected<void,
        minimizer_generation_encoding_error>
    begin_admitted_core(
            admitted_prepared_reader_core& destination,
            bioseq::parallelism::memory_admission_controller& admission,
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const expected_minimizer_generation_identity& expected,
            const grouped_soa_minimizer_profile& profile,
            std::span<const std::byte> encoded_directory,
            std::uint64_t additional_logical_bytes = 0,
            bioseq::parallelism::memory_admission_failure*
                admission_failure = nullptr) {
        const bool destination_is_fresh = !destination.permit_
            && destination.directory_.owned_capacity_bytes() == 0u
            && destination.head_.layout
                == minimizer_generation_layout::invalid
            && destination.manifest_.layout
                == minimizer_generation_layout::invalid
            && destination.expected_.layout
                == minimizer_generation_layout::invalid
            && destination.profile_.layout
                == minimizer_generation_layout::invalid;
        DEBUG_ASSERT(destination_is_fresh);
        if (!destination_is_fresh) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (head.layout != traits::layout
            || manifest.layout != traits::layout
            || expected.layout != traits::layout
            || profile.layout != traits::layout
            || !validate_grouped_soa_minimizer_profile(profile)) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        if (auto compatible = validate_minimizer_generation_compatibility(
                manifest, expected); !compatible) {
            return std::unexpected(compatible.error());
        }
        const std::size_t stride =
            minimizer_directory_entry_encoded_bytes(profile);
        if (encoded_directory.size() % stride != 0) {
            return std::unexpected(
                minimizer_generation_encoding_error::directory_size);
        }
        const std::uint64_t entry_count = encoded_directory.size() / stride;
        std::uint64_t logical_bytes = bioseq::common::saturating_add(
            encoded_directory.size(),
            bioseq::common::saturating_mul(
                entry_count, sizeof(grouped_soa_bucket_directory_entry)));
        logical_bytes = bioseq::common::saturating_add(
            logical_bytes, additional_logical_bytes);
        if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        auto permit = admission.try_reserve(
            logical_bytes, admission_failure);
        if (!permit) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        auto directory = grouped_soa_directory_view::decode_admitted(
            encoded_directory, profile, *permit, admission_failure);
        if (!directory) return std::unexpected(directory.error());
        if (auto valid = validate_minimizer_generation_metadata(
                head, manifest, *directory); !valid) {
            return std::unexpected(valid.error());
        }
        // Commit only after every fallible validation has completed. Until
        // this point the local directory is destroyed before the local permit
        // releases, and on success ownership moves exactly once into the
        // already-constructed reader base subobject.
        destination.permit_ = std::move(*permit);
        destination.directory_ = std::move(*directory);
        destination.head_ = head;
        destination.manifest_ = manifest;
        destination.expected_ = expected;
        destination.profile_ = profile;
        return {};
    }

    [[nodiscard]] std::uint64_t core_owned_capacity_bytes() const noexcept {
        return directory_.owned_capacity_bytes();
    }

    void release_core_noexcept() noexcept {
        directory_ = grouped_soa_directory_view{};
        permit_.release();
    }
};

} // namespace minimizer_prepared_reader_detail

/// Decode and validate one complete grouped-SoA control extent. Padding bytes are
/// part of the corruption contract and must remain zero.
[[nodiscard]] inline std::expected<
    std::vector<grouped_soa_decoded_control>,
    minimizer_generation_encoding_error>
decode_grouped_soa_controls(
        std::span<const std::byte> input,
        const grouped_soa_bucket_directory_entry& entry,
        const grouped_soa_minimizer_profile& profile) {
    if (profile.layout
            != minimizer_generation_layout::
                grouped_soa_segment_local
        || entry.control_encoded_bytes != input.size()) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    const auto layout = plan_grouped_soa_control_plane(
        entry.control_element_count,
        profile.hash_width,
        entry.shape_offset_width,
        profile.count_width);
    if (!layout || layout->encoded_bytes != input.size()
        || entry.control_element_count
            > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const std::size_t n = static_cast<std::size_t>(
        entry.control_element_count);
    const std::size_t h = static_cast<std::uint8_t>(
        profile.hash_width);
    const std::size_t o = static_cast<std::uint8_t>(
        entry.shape_offset_width);
    const std::size_t c = static_cast<std::uint8_t>(
        profile.count_width);
    const std::size_t hash_end = n * h;
    const std::size_t prefix_begin = static_cast<std::size_t>(
        layout->prefix_offset);
    const std::size_t prefix_end = prefix_begin + (n + 1u) * o;
    const std::size_t count_begin = static_cast<std::size_t>(
        layout->count_offset);
    const std::size_t count_end = count_begin + n * c;
    if (!minimizer_generation_wire_detail::all_zero(
            input, hash_end, prefix_begin)
        || !minimizer_generation_wire_detail::all_zero(
            input, prefix_end, count_begin)
        || !minimizer_generation_wire_detail::all_zero(
            input, count_end, input.size())) {
        return std::unexpected(
            minimizer_generation_encoding_error::reserved_nonzero);
    }
    std::vector<grouped_soa_decoded_control> controls;
    controls.reserve(n);
    std::uint64_t previous_hash = 0;
    std::uint64_t previous_prefix = 0;
    for (std::size_t i = 0; i <= n; ++i) {
        const auto prefix = minimizer_generation_wire_detail::read_shape_scalar(
            input, prefix_begin + i * o, entry.shape_offset_width);
        if (!prefix || (i == 0 && *prefix != 0)
            || (i != 0 && *prefix <= previous_prefix)
            || *prefix > entry.position_element_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::control_range);
        }
        if (i == n) {
            if (*prefix != entry.position_element_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::control_range);
            }
            break;
        }
        const auto hash = minimizer_generation_wire_detail::read_shape_scalar(
            input, i * h, profile.hash_width);
        const auto next_prefix =
            minimizer_generation_wire_detail::read_shape_scalar(
                input, prefix_begin + (i + 1u) * o,
                entry.shape_offset_width);
        const auto count = minimizer_generation_wire_detail::read_shape_scalar(
            input, count_begin + i * c, profile.count_width);
        if (!hash || *hash > profile.max_hash
            || (i != 0 && *hash <= previous_hash)) {
            return std::unexpected(
                minimizer_generation_encoding_error::control_order);
        }
        if (!next_prefix || !count
            || *next_prefix <= *prefix
            || *next_prefix > entry.position_element_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::control_range);
        }
        const std::uint64_t delta = *next_prefix - *prefix;
        if (*count != std::min(delta, profile.max_occ_cap + 1u)) {
            return std::unexpected(
                minimizer_generation_encoding_error::control_range);
        }
        controls.push_back(grouped_soa_decoded_control{
            .hash = *hash,
            .position_begin = *prefix,
            .position_end = *next_prefix,
            .saturated_count = *count,
        });
        previous_hash = *hash;
        previous_prefix = *prefix;
    }
    if (controls.empty()
        || controls.front().hash != entry.lo_hash
        || controls.back().hash != entry.hi_hash) {
        return std::unexpected(minimizer_generation_encoding_error::hash_range);
    }
    return controls;
}

/// Decode every grouped-SoA position to the ordinary global `(bp<<1)|strand`
/// query ABI and validate strict order independently inside every hash range.
[[nodiscard]] inline std::expected<std::vector<std::uint64_t>,
    minimizer_generation_encoding_error>
decode_grouped_soa_positions(
        std::span<const std::byte> input,
        std::span<const grouped_soa_decoded_control> controls,
        const grouped_soa_bucket_directory_entry& entry,
        const minimizer_position_segment_bounds& core,
        const grouped_soa_minimizer_profile& profile) {
    if (profile.layout
            != minimizer_generation_layout::
                grouped_soa_segment_local
        || input.size() != entry.position_encoded_bytes
        || entry.position_element_count
            > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const std::size_t p = static_cast<std::uint8_t>(
        profile.position_width);
    if (entry.position_element_count
            > (std::numeric_limits<std::size_t>::max)() / p
        || input.size()
            != static_cast<std::size_t>(entry.position_element_count) * p) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    std::vector<std::uint64_t> positions;
    positions.reserve(static_cast<std::size_t>(entry.position_element_count));
    for (std::size_t i = 0; i < entry.position_element_count; ++i) {
        const auto local = minimizer_generation_wire_detail::read_shape_scalar(
            input, i * p, profile.position_width);
        if (!local) {
            return std::unexpected(
                minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t local_bp = *local >> 1u;
        if (local_bp >= core.end_bp - core.begin_bp
            || core.begin_bp
                > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
            return std::unexpected(
                minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t global_bp = core.begin_bp + local_bp;
        if (global_bp
                > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        positions.push_back((global_bp << 1u) | (*local & 1u));
    }
    for (const auto& control : controls) {
        std::optional<std::uint64_t> previous;
        for (std::uint64_t i = control.position_begin;
             i < control.position_end; ++i) {
            const std::uint64_t value = positions[static_cast<std::size_t>(i)];
            if (previous && value <= *previous) {
                return std::unexpected(
                    minimizer_generation_encoding_error::position_range);
            }
            previous = value;
        }
    }
    return positions;
}

namespace minimizer_prepared_reader_detail {

template <class traits>
struct prepared_query_wire_ops;

/// grouped-SoA stores retained positions in one flat image and therefore does
/// not allocate an empty vector per query hash.
template <class Hash, bool FlatPositionImage>
struct prepared_query_record;

template <class Hash>
struct prepared_query_record<Hash, true> {
    std::uint64_t global_occurrence_count = 0;
    std::uint64_t global_position_offset = 0;
    std::uint64_t global_positions_written = 0;
    bool positions_complete = true;
};

template <class Hash>
struct prepared_query_record<Hash, false> {
    Hash hash = 0;
    std::uint64_t global_occurrence_count = 0;
    std::uint64_t global_position_offset = 0;
    std::uint64_t global_positions_written = 0;
    bool positions_complete = true;
    std::vector<std::uint64_t> positions;
};

static_assert(sizeof(prepared_query_record<std::uint64_t, true>) == 32u);
static_assert(sizeof(prepared_query_record<std::uint64_t, false>) == 64u);

/// Exact scalar owner used by the grouped-SoA prepared reader.  Width is chosen
/// from authenticated generation/query bounds; no biological count is ever
/// capped to the runtime max-occ threshold.
enum class prepared_query_scalar_width : std::uint8_t {
    u8 = 1u,
    u16 = 2u,
    u32 = 4u,
    u64 = 8u,
};

[[nodiscard]] constexpr prepared_query_scalar_width
prepared_query_scalar_width_for_max(std::uint64_t maximum) noexcept {
    return maximum <= (std::numeric_limits<std::uint8_t>::max)()
        ? prepared_query_scalar_width::u8
        : maximum <= (std::numeric_limits<std::uint16_t>::max)()
            ? prepared_query_scalar_width::u16
            : maximum <= (std::numeric_limits<std::uint32_t>::max)()
                ? prepared_query_scalar_width::u32
                : prepared_query_scalar_width::u64;
}

[[nodiscard]] constexpr std::uint64_t prepared_query_scalar_max(
        prepared_query_scalar_width width) noexcept {
    switch (width) {
        case prepared_query_scalar_width::u8:
            return (std::numeric_limits<std::uint8_t>::max)();
        case prepared_query_scalar_width::u16:
            return (std::numeric_limits<std::uint16_t>::max)();
        case prepared_query_scalar_width::u32:
            return (std::numeric_limits<std::uint32_t>::max)();
        case prepared_query_scalar_width::u64:
            return (std::numeric_limits<std::uint64_t>::max)();
    }
    return 0u;
}

struct prepared_query_scalar_array {
    std::unique_ptr<std::uint8_t[]> u8;
    std::unique_ptr<std::uint16_t[]> u16;
    std::unique_ptr<std::uint32_t[]> u32;
    std::unique_ptr<std::uint64_t[]> u64;
    std::size_t count = 0u;
    prepared_query_scalar_width width = prepared_query_scalar_width::u8;

    void reset() noexcept {
        u8.reset();
        u16.reset();
        u32.reset();
        u64.reset();
        count = 0u;
        width = prepared_query_scalar_width::u8;
    }

    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept {
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(count),
            static_cast<std::uint8_t>(width));
    }

    void allocate(
            prepared_query_scalar_width wanted_width,
            std::size_t wanted_count,
            bool zero_initialize) {
        reset();
        width = wanted_width;
        count = wanted_count;
        if (wanted_count == 0u) return;
        switch (width) {
            case prepared_query_scalar_width::u8:
                u8 = std::make_unique_for_overwrite<std::uint8_t[]>(
                    wanted_count);
                if (zero_initialize) std::fill_n(u8.get(), wanted_count, 0u);
                return;
            case prepared_query_scalar_width::u16:
                u16 = std::make_unique_for_overwrite<std::uint16_t[]>(
                    wanted_count);
                if (zero_initialize) std::fill_n(u16.get(), wanted_count, 0u);
                return;
            case prepared_query_scalar_width::u32:
                u32 = std::make_unique_for_overwrite<std::uint32_t[]>(
                    wanted_count);
                if (zero_initialize) std::fill_n(u32.get(), wanted_count, 0u);
                return;
            case prepared_query_scalar_width::u64:
                u64 = std::make_unique_for_overwrite<std::uint64_t[]>(
                    wanted_count);
                if (zero_initialize) std::fill_n(u64.get(), wanted_count, 0u);
                return;
        }
    }

    [[nodiscard]] std::uint64_t get(std::size_t index) const noexcept {
        if (index >= count) return 0u;
        switch (width) {
            case prepared_query_scalar_width::u8: return u8[index];
            case prepared_query_scalar_width::u16: return u16[index];
            case prepared_query_scalar_width::u32: return u32[index];
            case prepared_query_scalar_width::u64: return u64[index];
        }
        return 0u;
    }

    [[nodiscard]] bool set(
            std::size_t index,
            std::uint64_t value) noexcept {
        if (index >= count || value > prepared_query_scalar_max(width)) {
            return false;
        }
        switch (width) {
            case prepared_query_scalar_width::u8:
                u8[index] = static_cast<std::uint8_t>(value);
                return true;
            case prepared_query_scalar_width::u16:
                u16[index] = static_cast<std::uint16_t>(value);
                return true;
            case prepared_query_scalar_width::u32:
                u32[index] = static_cast<std::uint32_t>(value);
                return true;
            case prepared_query_scalar_width::u64:
                u64[index] = value;
                return true;
        }
        return false;
    }

    [[nodiscard]] bool add(
            std::size_t index,
            std::uint64_t delta) noexcept {
        if (index >= count) return false;
        const std::uint64_t old_value = get(index);
        const std::uint64_t maximum = prepared_query_scalar_max(width);
        return delta <= maximum - old_value
            && set(index, old_value + delta);
    }

    [[nodiscard]] bool increment(std::size_t index) noexcept {
        return add(index, 1u);
    }
};

template <>
struct prepared_query_wire_ops<codec_traits<
    minimizer_generation_layout::grouped_soa_segment_local>> {
    using traits = codec_traits<
        minimizer_generation_layout::grouped_soa_segment_local>;
    using control_type = traits::control_type;

    [[nodiscard]] static std::expected<std::vector<control_type>,
        minimizer_generation_encoding_error>
    decode_controls(
            std::span<const std::byte> bytes,
            const grouped_soa_bucket_directory_entry& entry,
            const grouped_soa_minimizer_profile& profile) {
        return decode_grouped_soa_controls(bytes, entry, profile);
    }

    [[nodiscard]] static std::uint64_t hash(
            const control_type& control) noexcept {
        return control.hash;
    }
    [[nodiscard]] static std::uint64_t position_begin(
            const control_type& control) noexcept {
        return control.position_begin;
    }
    [[nodiscard]] static std::uint64_t position_end(
            const control_type& control) noexcept {
        return control.position_end;
    }

    [[nodiscard]] static std::expected<std::uint64_t,
        minimizer_generation_encoding_error>
    decode_position(
            std::span<const std::byte> bytes,
            const minimizer_position_segment_bounds& core,
            const grouped_soa_minimizer_profile& profile) noexcept {
        if (bytes.size()
                != static_cast<std::uint8_t>(
                    profile.position_width)) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        const auto local = minimizer_generation_wire_detail::read_shape_scalar(
            bytes, 0u, profile.position_width);
        if (!local) {
            return std::unexpected(
                minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t local_bp = *local >> 1u;
        if (local_bp >= core.end_bp - core.begin_bp
            || core.begin_bp
                > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
            return std::unexpected(
                minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t global_bp = core.begin_bp + local_bp;
        if (global_bp
                > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return (global_bp << 1u) | (*local & 1u);
    }
};

/// The admitted grouped-SoA query protocol owns query routing, physical cursor,
/// authenticated extent session, target-global occurrence policy, retained
/// positions, and the permit covering those allocations.
template <class traits>
class trait_driven_prepared_query_core
    : protected admitted_prepared_reader_core<traits> {
protected:
    using identity_core = admitted_prepared_reader_core<traits>;
    using wire_ops = prepared_query_wire_ops<traits>;
    using hash_type = typename traits::hash_type;

    static constexpr std::size_t no_route =
        (std::numeric_limits<std::size_t>::max)();

    using query_record = prepared_query_record<
        hash_type, traits::global_controls_then_positions>;

    struct dense_selected_control {
        std::size_t query_index = no_route;
        std::uint64_t position_end = 0;
    };

    // Retain every authenticated control boundary but only query routes.
    using selected_control = std::conditional_t<
        traits::global_controls_then_positions,
        std::uint64_t,
        dense_selected_control>;

    struct dense_selected_control_state {
        std::vector<dense_selected_control> controls;
    };
    struct exact_selected_control_state {
        // MPF grouped-SoA extents remain independent.  Retain each fragment's
        // authenticated control boundaries at the narrowest width proven by
        // that fragment's position count instead of globalising every local
        // boundary into a target-wide u64 image.
        prepared_query_scalar_array position_ends;
        std::size_t control_count = 0;
    };
    using selected_control_state = std::conditional_t<
        traits::global_controls_then_positions,
        exact_selected_control_state,
        dense_selected_control_state>;

    static constexpr std::uint32_t no_narrow_route =
        (std::numeric_limits<std::uint32_t>::max)();

    struct sparse_selected_route {
        std::uint32_t control_ordinal = 0;
        std::uint32_t query_index = no_narrow_route;
    };
    static_assert(sizeof(sparse_selected_route) == 8u);

    struct no_selected_routes {};
    struct adaptive_selected_routes {
        std::unique_ptr<std::uint32_t[]> dense_routes;
        std::unique_ptr<sparse_selected_route[]> sparse_routes;
        std::size_t sparse_route_count = 0;
        std::size_t next_sparse_route = 0;
        bool uses_sparse = false;
    };
    using selected_route_state = std::conditional_t<
        traits::global_controls_then_positions,
        adaptive_selected_routes,
        no_selected_routes>;

    struct selected_extent {
        [[no_unique_address]] selected_control_state control_state;
        [[no_unique_address]] selected_route_state route_state;
        std::size_t next_control = 0;
        std::uint64_t next_position = 0;
        // The physical cursor consumes one control at a time.  A prior
        // position therefore belongs to the current control, not every
        // retained control record.  The validity bit resets exactly when the
        // boundary cursor advances and remains live across payload chunks.
        std::uint64_t previous_position = 0;
        bool previous_position_valid = false;
        bool controls_complete = false;
        bool positions_complete = false;
        // Exact heap owners nested below this extent. The outer extent vector
        // is accounted separately, so replacing/releasing one extent remains
        // O(1) without rescanning all selected extents.
        std::uint64_t owned_capacity_bytes = 0;
    };

    struct extent_session {
        std::size_t entry_index = no_route;
        bool positions = false;
        std::uint32_t next_chunk = 0;
        crc64_ecma182_accumulator crc{};
    };

    std::vector<hash_type> protocol_query_hashes_;
    std::vector<query_record> protocol_query_records_;
    // Search exact bucket-implied suffix keys through one admitted high-bit
    // radix directory. The full u64 image is released before payload loading.
    prepared_query_scalar_array protocol_query_suffixes_;
    std::size_t protocol_query_count_ = 0u;
    std::unique_ptr<std::uint32_t[]> protocol_query_radix_offsets_;
    std::size_t protocol_query_radix_offset_count_ = 0u;
    std::uint32_t protocol_query_radix_bits_ = 0u;
    std::uint32_t protocol_query_hash_bits_ = 0u;
    std::uint32_t protocol_query_suffix_bits_ = 0u;
    // grouped-SoA keeps exact target-global counts at the narrowest width proven
    // by the authenticated manifest position_count. Runtime max-occ is not
    // applied until every control extent has contributed. Dense offsets make
    // the hot positive lookup one direct load; UINT64_MAX means pruned.
    prepared_query_scalar_array protocol_query_exact_counts_;
    std::unique_ptr<std::uint64_t[]> protocol_query_position_offsets_;
    std::size_t protocol_query_position_offset_count_ = 0u;
    prepared_query_scalar_array protocol_query_written_;
    // The final retained position count is known before allocation, allowing
    // one exact array instead of a capacity-growing vector.
    std::unique_ptr<std::uint64_t[]> protocol_global_positions_;
    std::size_t protocol_global_position_count_ = 0u;
    std::vector<std::size_t> protocol_ordinal_to_selected_;
    std::vector<selected_extent> protocol_selected_extents_;
    std::vector<std::byte> protocol_control_scratch_;
    std::optional<extent_session> protocol_session_{};
    protocol_owned_bytes_ledger protocol_owned_bytes_{};
    std::uint64_t protocol_max_selected_control_bytes_ = 0u;
    std::uint64_t protocol_max_occ_ = 0;
    std::size_t protocol_entry_index_ = 0;
    std::size_t protocol_segment_begin_ = 0;
    std::size_t protocol_segment_end_ = 0;
    bool protocol_position_stage_ = false;
    bool protocol_global_position_storage_prepared_ = false;
    bool protocol_initialized_ = false;
    bool protocol_ready_ = false;
    bool protocol_failed_ = false;
    minimizer_generation_encoding_error protocol_failure_ =
        minimizer_generation_encoding_error::row_order;
    bioseq::parallelism::memory_admission_controller* protocol_admission_ =
        nullptr;
    grouped_soa_query_reader_options protocol_test_options_{};
    grouped_soa_query_admission_stats
        protocol_admission_failure_stats_{};

    struct protocol_memory_stats {
        std::uint64_t capacity_accounting_passes = 0;
        std::uint64_t global_position_storage_preparations = 0;
        std::uint64_t global_position_storage_allocations = 0;
        std::uint64_t position_append_growth_reservations = 0;
        std::uint64_t decoded_control_records = 0;
        std::uint64_t retained_selected_control_records = 0;
        std::uint64_t retained_selected_control_capacity_bytes = 0;
        std::uint64_t released_query_hash_capacity_bytes = 0;
        std::uint64_t query_route_radix_bucket_count = 0;
        std::uint64_t query_route_radix_nonempty_bucket_count = 0;
        std::uint64_t query_route_radix_max_bucket_size = 0;
        std::uint64_t query_route_control_lookups = 0;
        std::uint64_t query_route_hash_probes = 0;
        std::uint64_t query_route_matches = 0;
        bool saturated = false;
    };
    mutable protocol_memory_stats protocol_memory_stats_{};

    [[nodiscard]] static std::size_t selected_control_count_(
            const selected_extent& extent) noexcept {
        if constexpr (traits::global_controls_then_positions) {
            return extent.control_state.control_count;
        } else {
            return extent.control_state.controls.size();
        }
    }

    [[nodiscard]] static std::uint64_t selected_position_end_(
            const selected_extent& extent,
            std::size_t control_index) noexcept {
        if constexpr (traits::global_controls_then_positions) {
            return extent.control_state.position_ends.get(control_index);
        } else {
            return extent.control_state.controls[control_index].position_end;
        }
    }

    [[nodiscard]] static std::uint64_t
    selected_extent_control_capacity_bytes_(
            const selected_extent& extent) noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = 0u;
        if constexpr (traits::global_controls_then_positions) {
            bytes = extent.control_state.position_ends.capacity_bytes();
            if (extent.route_state.uses_sparse) {
                bytes = saturating_add(bytes, saturating_mul(
                    static_cast<std::uint64_t>(
                        extent.route_state.sparse_route_count),
                    sizeof(sparse_selected_route)));
            } else if (extent.route_state.dense_routes) {
                bytes = saturating_add(bytes, saturating_mul(
                    static_cast<std::uint64_t>(
                        extent.control_state.control_count),
                    sizeof(std::uint32_t)));
            }
        } else {
            bytes = saturating_mul(
                static_cast<std::uint64_t>(
                    extent.control_state.controls.capacity()),
                sizeof(dense_selected_control));
        }
        return bytes;
    }

    [[nodiscard]] std::size_t protocol_query_state_count_() const noexcept {
        if constexpr (traits::global_controls_then_positions) {
            return protocol_query_count_;
        }
        return protocol_query_records_.size();
    }

    [[nodiscard]] std::size_t
    protocol_query_state_capacity_() const noexcept {
        if constexpr (traits::global_controls_then_positions) {
            return protocol_query_exact_counts_.count;
        }
        return protocol_query_records_.capacity();
    }

    [[nodiscard]] std::uint64_t
    protocol_query_state_capacity_bytes_() const noexcept {
        if constexpr (!traits::global_controls_then_positions) {
            return bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(
                    protocol_query_records_.capacity()),
                sizeof(query_record));
        }
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = protocol_query_exact_counts_.capacity_bytes();
        bytes = saturating_add(bytes, saturating_mul(
            static_cast<std::uint64_t>(
                protocol_query_position_offset_count_),
            sizeof(std::uint64_t)));
        return saturating_add(
            bytes, protocol_query_written_.capacity_bytes());
    }

    [[nodiscard]] std::uint64_t
    protocol_query_hash_capacity_bytes_() const noexcept {
        if constexpr (traits::global_controls_then_positions) {
            return protocol_query_suffixes_.capacity_bytes();
        }
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(protocol_query_hashes_.capacity()),
            sizeof(hash_type));
    }

    [[nodiscard]] std::uint64_t protocol_query_exact_count_(
            std::size_t query_index) const noexcept {
        if constexpr (traits::global_controls_then_positions) {
            return protocol_query_exact_counts_.get(query_index);
        }
        return query_index < protocol_query_records_.size()
            ? protocol_query_records_[query_index].global_occurrence_count
            : 0u;
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    add_protocol_query_exact_count_(
            std::size_t query_index,
            std::uint64_t delta) noexcept {
        if constexpr (!traits::global_controls_then_positions) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (delta == 0u
            || !protocol_query_exact_counts_.add(query_index, delta)
            || protocol_query_exact_counts_.get(query_index)
                > this->manifest_.position_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return {};
    }

    [[nodiscard]] bool protocol_query_retained_(
            std::size_t query_index) const noexcept {
        return query_index < protocol_query_position_offset_count_
            && protocol_query_position_offsets_
            && protocol_query_position_offsets_[query_index]
                != (std::numeric_limits<std::uint64_t>::max)();
    }

    trait_driven_prepared_query_core() = default;
    trait_driven_prepared_query_core(
        trait_driven_prepared_query_core&&) noexcept = default;
    trait_driven_prepared_query_core& operator=(
        trait_driven_prepared_query_core&&) noexcept = default;
    trait_driven_prepared_query_core(
        const trait_driven_prepared_query_core&) = delete;
    trait_driven_prepared_query_core& operator=(
        const trait_driven_prepared_query_core&) = delete;

    /// Slow owner walk retained only for a handful of protocol barriers and
    /// focused tests. Hot admission/reconciliation reads the cached ledger.
    [[nodiscard]] std::uint64_t
    protocol_owned_capacity_bytes_oracle_() const noexcept {
        if (protocol_memory_stats_.capacity_accounting_passes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            protocol_memory_stats_.saturated = true;
        } else {
            ++protocol_memory_stats_.capacity_accounting_passes;
        }
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = this->core_owned_capacity_bytes();
        const auto add = [&bytes](std::size_t capacity,
                                  std::size_t width) noexcept {
            bytes = saturating_add(bytes,
                saturating_mul(static_cast<std::uint64_t>(capacity), width));
        };
        if constexpr (traits::global_controls_then_positions) {
            bytes = saturating_add(
                bytes, protocol_query_suffixes_.capacity_bytes());
            bytes = saturating_add(
                bytes, protocol_query_state_capacity_bytes_());
            add(protocol_query_radix_offset_count_, sizeof(std::uint32_t));
            add(protocol_global_position_count_, sizeof(std::uint64_t));
        } else {
            add(protocol_query_hashes_.capacity(), sizeof(hash_type));
            add(protocol_query_records_.capacity(), sizeof(query_record));
            for (const query_record& record : protocol_query_records_) {
                add(record.positions.capacity(), sizeof(std::uint64_t));
            }
        }
        add(protocol_ordinal_to_selected_.capacity(), sizeof(std::size_t));
        add(protocol_selected_extents_.capacity(), sizeof(selected_extent));
        for (const selected_extent& extent : protocol_selected_extents_) {
            bytes = saturating_add(
                bytes, selected_extent_control_capacity_bytes_(extent));
        }
        add(protocol_control_scratch_.capacity(), sizeof(std::byte));
        return bytes;
    }

    [[nodiscard]] std::uint64_t
    protocol_owned_capacity_bytes() const noexcept {
        return protocol_owned_bytes_.total_bytes;
    }

    [[nodiscard]] bool replace_ledger_field_(
            std::uint64_t& field,
            std::uint64_t new_bytes) noexcept {
        const std::uint64_t old_bytes = field;
        if (!protocol_owned_bytes_.replace_checked(old_bytes, new_bytes)) {
            return false;
        }
        field = new_bytes;
        return true;
    }

    [[nodiscard]] bool replace_selected_extent_owner_(
            selected_extent& extent,
            std::uint64_t new_bytes) noexcept {
        if (extent.owned_capacity_bytes
                > protocol_owned_bytes_.selected_extent_owner_bytes) {
            return false;
        }
        const std::uint64_t new_total =
            protocol_owned_bytes_.selected_extent_owner_bytes
            - extent.owned_capacity_bytes;
        if (new_bytes
                > (std::numeric_limits<std::uint64_t>::max)() - new_total) {
            return false;
        }
        if (!replace_ledger_field_(
                protocol_owned_bytes_.selected_extent_owner_bytes,
                new_total + new_bytes)) {
            return false;
        }
        extent.owned_capacity_bytes = new_bytes;
        return true;
    }

    [[nodiscard]] bool protocol_ledger_matches_oracle_() const noexcept {
        return protocol_owned_bytes_.field_sum_holds()
            && protocol_owned_capacity_bytes_oracle_()
                == protocol_owned_capacity_bytes();
    }

    class permit_growth_guard {
    private:
        trait_driven_prepared_query_core* owner_ = nullptr;
        bool committed_ = false;

    public:
        explicit permit_growth_guard(
                trait_driven_prepared_query_core& owner) noexcept
            : owner_(&owner) {}
        permit_growth_guard(const permit_growth_guard&) = delete;
        permit_growth_guard& operator=(const permit_growth_guard&) = delete;
        ~permit_growth_guard() {
            if (!committed_ && owner_ != nullptr && owner_->permit_) {
                // A failed allocation/reconciliation cannot leave newly
                // grown vectors alive under the older, smaller reservation.
                // Every protocol operation is fail-closed, so destroy all
                // covered storage before releasing the permit.
                owner_->protocol_ready_ = false;
                owner_->protocol_release_noexcept();
            }
        }
        [[nodiscard]] bool grow(
                std::uint64_t projected,
                bioseq::parallelism::memory_admission_failure*
                    failure = nullptr) noexcept {
            return projected
                    != (std::numeric_limits<std::uint64_t>::max)()
                && owner_->permit_.grow_to(projected, failure);
        }
        [[nodiscard]] bool reconcile() noexcept {
            const std::uint64_t actual =
                owner_->protocol_owned_capacity_bytes();
            if (actual == (std::numeric_limits<std::uint64_t>::max)()
                || actual > owner_->permit_.bytes()) {
                return false;
            }
            owner_->permit_.shrink_to(actual);
            committed_ = true;
            return true;
        }
    };

    [[nodiscard]] bool release_control_scratch_noexcept_() noexcept {
        {
            std::vector<std::byte> empty;
            protocol_control_scratch_.swap(empty);
        }
        return replace_ledger_field_(
            protocol_owned_bytes_.control_scratch_bytes, 0u);
    }

    void protocol_release_noexcept() noexcept {
        protocol_session_.reset();
        {
            std::vector<std::byte> empty;
            protocol_control_scratch_.swap(empty);
        }
        std::vector<selected_extent>{}.swap(protocol_selected_extents_);
        std::vector<std::size_t>{}.swap(protocol_ordinal_to_selected_);
        protocol_global_positions_.reset();
        protocol_global_position_count_ = 0u;
        protocol_query_suffixes_.reset();
        protocol_query_count_ = 0u;
        protocol_query_radix_offsets_.reset();
        protocol_query_radix_offset_count_ = 0u;
        protocol_query_radix_bits_ = 0u;
        protocol_query_hash_bits_ = 0u;
        protocol_query_suffix_bits_ = 0u;
        protocol_query_exact_counts_.reset();
        protocol_query_position_offsets_.reset();
        protocol_query_position_offset_count_ = 0u;
        protocol_query_written_.reset();
        std::vector<query_record>{}.swap(protocol_query_records_);
        std::vector<hash_type>{}.swap(protocol_query_hashes_);
        protocol_max_selected_control_bytes_ = 0u;
        protocol_global_position_storage_prepared_ = false;
        this->release_core_noexcept();
        protocol_owned_bytes_.reset();
        if (protocol_owned_capacity_bytes_oracle_() != 0u) {
            protocol_memory_stats_.saturated = true;
        }
    }

    template <class T>
    [[nodiscard]] std::expected<T, minimizer_generation_encoding_error>
    protocol_poison(minimizer_generation_encoding_error error) noexcept {
        if (!protocol_failed_) {
            protocol_failed_ = true;
            protocol_ready_ = false;
            protocol_failure_ = error;
            protocol_release_noexcept();
        }
        return std::unexpected(protocol_failure_);
    }

    void mark_protocol_moved_from() noexcept {
        protocol_session_.reset();
        std::vector<std::byte>{}.swap(protocol_control_scratch_);
        protocol_global_positions_.reset();
        protocol_global_position_count_ = 0u;
        protocol_query_suffixes_.reset();
        protocol_query_count_ = 0u;
        protocol_query_radix_offsets_.reset();
        protocol_query_radix_offset_count_ = 0u;
        protocol_query_radix_bits_ = 0u;
        protocol_query_hash_bits_ = 0u;
        protocol_query_suffix_bits_ = 0u;
        protocol_query_exact_counts_.reset();
        protocol_query_position_offsets_.reset();
        protocol_query_position_offset_count_ = 0u;
        protocol_query_written_.reset();
        protocol_max_selected_control_bytes_ = 0u;
        protocol_owned_bytes_.reset();
        protocol_global_position_storage_prepared_ = false;
        protocol_initialized_ = true;
        protocol_ready_ = false;
        protocol_failed_ = true;
        protocol_failure_ = minimizer_generation_encoding_error::row_order;
    }

    void increment_memory_stats_(std::uint64_t& counter) const noexcept {
        if (counter == (std::numeric_limits<std::uint64_t>::max)()) {
            protocol_memory_stats_.saturated = true;
        } else {
            ++counter;
        }
    }

    void add_memory_stats_(
            std::uint64_t& counter,
            std::uint64_t delta) const noexcept {
        if (counter > (std::numeric_limits<std::uint64_t>::max)() - delta) {
            counter = (std::numeric_limits<std::uint64_t>::max)();
            protocol_memory_stats_.saturated = true;
        } else {
            counter += delta;
        }
    }

    [[nodiscard]] bool inject_admission_failure_(
            grouped_soa_query_admission_stage substage) noexcept {
        if constexpr (!traits::global_controls_then_positions) {
            return false;
        }
        if (protocol_test_options_.fail_admission_substage != substage) {
            return false;
        }
        protocol_test_options_.fail_admission_substage =
            grouped_soa_query_admission_stage::none;
        return true;
    }

    void capture_admission_failure_(
            grouped_soa_query_admission_stage substage,
            std::uint64_t projected_bytes,
            std::uint64_t retained_position_count = 0u,
            std::uint64_t retained_position_bytes = 0u,
            std::uint64_t allocation_headroom_bytes = 0u,
            const bioseq::parallelism::memory_admission_failure*
                controller_failure = nullptr,
            bool injected = false) noexcept {
        if constexpr (!traits::global_controls_then_positions) {
            return;
        }
        if (protocol_admission_failure_stats_.substage
                != grouped_soa_query_admission_stage::none) {
            return;
        }
        using bioseq::common::saturating_mul;
        const std::uint64_t owned_capacity_bytes =
            protocol_owned_capacity_bytes();
        const std::uint64_t permit_bytes = this->permit_.bytes();
        grouped_soa_query_admission_stats stats{
            .substage = substage,
            .retained_position_count = retained_position_count,
            .retained_position_bytes = retained_position_bytes,
            .allocation_headroom_bytes = allocation_headroom_bytes,
            .projected_bytes = projected_bytes,
            .requested_growth_bytes = projected_bytes > permit_bytes
                ? projected_bytes - permit_bytes : 0u,
            .owned_capacity_bytes = owned_capacity_bytes,
            .permit_bytes = permit_bytes,
            .query_record_count = static_cast<std::uint64_t>(
                protocol_query_state_count_()),
            .query_record_capacity = static_cast<std::uint64_t>(
                protocol_query_state_capacity_()),
            .query_hash_count = static_cast<std::uint64_t>(
                traits::global_controls_then_positions
                    ? protocol_query_suffixes_.count
                    : protocol_query_hashes_.size()),
            .query_hash_capacity = static_cast<std::uint64_t>(
                traits::global_controls_then_positions
                    ? protocol_query_suffixes_.count
                    : protocol_query_hashes_.capacity()),
            .selected_extent_count = static_cast<std::uint64_t>(
                protocol_selected_extents_.size()),
            .selected_extent_capacity = static_cast<std::uint64_t>(
                protocol_selected_extents_.capacity()),
            .selected_control_capacity_bytes =
                protocol_owned_bytes_.selected_extent_owner_bytes,
            .control_payload_capacity_bytes =
                protocol_owned_bytes_.control_scratch_bytes,
            .releasable_protocol_query_hash_bytes =
                !traits::global_controls_then_positions
                    && protocol_initialized_
                    && protocol_query_records_.size()
                        == protocol_query_hashes_.size()
                ? saturating_mul(
                    static_cast<std::uint64_t>(
                        protocol_query_hashes_.capacity()),
                    sizeof(hash_type))
                : 0u,
            .injected_failure = injected,
        };
        if (controller_failure != nullptr
            && controller_failure->failed()) {
            stats.controller_reason = controller_failure->reason;
            stats.requested_growth_bytes =
                controller_failure->requested_bytes;
            stats.controller_reserved_bytes =
                controller_failure->reserved_bytes;
            stats.controller_budget_ceiling_bytes =
                controller_failure->budget_ceiling_bytes;
            stats.controller_allocatable_after_reservations_bytes =
                controller_failure->allocatable_after_reservations_bytes;
            stats.process_footprint_bytes =
                controller_failure->process_footprint_bytes;
            stats.system_available_bytes =
                controller_failure->system_available_bytes;
            stats.system_total_bytes =
                controller_failure->system_total_bytes;
            stats.process_soft_cap_bytes =
                controller_failure->process_soft_cap_bytes;
            stats.controller_failure_snapshot_present = true;
        } else if (protocol_admission_ != nullptr) {
            const auto snapshot =
                protocol_admission_->diagnostic_snapshot();
            stats.controller_reserved_bytes = snapshot.reserved_bytes;
            stats.controller_budget_ceiling_bytes =
                snapshot.budget_ceiling_bytes;
            stats.controller_allocatable_after_reservations_bytes =
                snapshot.allocatable_after_reservations_bytes;
            stats.process_footprint_bytes =
                snapshot.process_footprint_bytes;
            stats.system_available_bytes =
                snapshot.system_available_bytes;
            stats.system_total_bytes = snapshot.system_total_bytes;
            stats.process_soft_cap_bytes =
                snapshot.process_soft_cap_bytes;
        }
        protocol_admission_failure_stats_ = stats;
    }

    /// grouped-SoA persists all controls before all positions.  At the stage
    /// boundary every selected hash's target-global occurrence count is final,
    /// so admit and allocate the complete retained-position image once.  This
    /// is intentionally one exact flat array: a query union with Q mostly-unique
    /// hashes must not perform Q live Mach/host memory probes, Q heap
    /// allocations, or Q full ownership walks before it can read positions.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    prepare_global_position_storage() {
        if constexpr (!traits::global_controls_then_positions) {
            return {};
        }
        if (protocol_global_position_storage_prepared_
            || !protocol_position_stage_ || protocol_entry_index_ != 0u
            || protocol_session_ || protocol_global_positions_
            || protocol_global_position_count_ != 0u
            || protocol_query_position_offsets_
            || protocol_query_position_offset_count_ != 0u
            || protocol_query_written_.count != 0u) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        increment_memory_stats_(
            protocol_memory_stats_.global_position_storage_preparations);

        std::uint64_t retained_count = 0;
        for (std::size_t query_index = 0u;
             query_index < protocol_query_count_; ++query_index) {
            const std::uint64_t exact_count =
                protocol_query_exact_count_(query_index);
            if (exact_count > this->manifest_.position_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::element_count);
            }
            if (exact_count == 0u || exact_count > protocol_max_occ_) {
                continue;
            }
            if (exact_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - retained_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            retained_count += exact_count;
        }
        if (retained_count
                > (std::numeric_limits<std::size_t>::max)()
                    / sizeof(std::uint64_t)) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        permit_growth_guard guard{*this};
        const auto planned = plan_grouped_soa_position_image(
            retained_count, protocol_owned_capacity_bytes());
        if (!planned) return std::unexpected(planned.error());
        const std::uint64_t retained_bytes =
            planned->retained_position_bytes;
        const std::uint64_t headroom = planned->allocation_headroom_bytes;
        const prepared_query_scalar_width written_width =
            prepared_query_scalar_width_for_max(protocol_max_occ_);
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t compact_final_bytes = saturating_mul(
            static_cast<std::uint64_t>(protocol_query_count_),
            sizeof(std::uint64_t));
        compact_final_bytes = saturating_add(
            compact_final_bytes,
            saturating_mul(
                static_cast<std::uint64_t>(protocol_query_count_),
                static_cast<std::uint8_t>(written_width)));
        const std::uint64_t projected = saturating_add(
            planned->projected_bytes, compact_final_bytes);
        if (compact_final_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
            || projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_reservation,
                projected, retained_count, retained_bytes, headroom,
                nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure position_failure;
        if (!guard.grow(projected, &position_failure)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_reservation,
                projected, retained_count, retained_bytes, headroom,
                &position_failure);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (retained_count != 0u
            && inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_allocation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_allocation,
                projected, retained_count, retained_bytes, headroom,
                nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        try {
            std::unique_ptr<std::uint64_t[]> position_offsets;
            prepared_query_scalar_array written;
            std::unique_ptr<std::uint64_t[]> global_positions;
            if (protocol_query_count_ != 0u) {
                position_offsets =
                    std::make_unique_for_overwrite<std::uint64_t[]>(
                        protocol_query_count_);
                std::fill_n(
                    position_offsets.get(), protocol_query_count_,
                    (std::numeric_limits<std::uint64_t>::max)());
                written.allocate(
                    written_width, protocol_query_count_, true);
            }
            if (retained_count != 0u) {
                global_positions =
                    std::make_unique_for_overwrite<std::uint64_t[]>(
                        static_cast<std::size_t>(retained_count));
            }
            std::uint64_t retained_offset = 0u;
            for (std::size_t query_index = 0u;
                 query_index < protocol_query_count_; ++query_index) {
                const std::uint64_t exact_count =
                    protocol_query_exact_count_(query_index);
                if (exact_count == 0u || exact_count > protocol_max_occ_) {
                    continue;
                }
                position_offsets[query_index] = retained_offset;
                retained_offset += exact_count;
            }
            if (retained_offset != retained_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::element_count);
            }
            protocol_query_position_offsets_ = std::move(position_offsets);
            protocol_query_position_offset_count_ = protocol_query_count_;
            protocol_query_written_ = std::move(written);
            protocol_global_positions_ = std::move(global_positions);
            protocol_global_position_count_ =
                static_cast<std::size_t>(retained_count);
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_allocation,
                projected, retained_count, retained_bytes, headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!replace_ledger_field_(
                protocol_owned_bytes_.global_position_bytes,
                retained_bytes)
            || !replace_ledger_field_(
                protocol_owned_bytes_.query_record_bytes,
                protocol_query_state_capacity_bytes_())) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (retained_count != 0u) {
            increment_memory_stats_(
                protocol_memory_stats_.
                    global_position_storage_allocations);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_reconcile)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_reconcile,
                projected, retained_count, retained_bytes, headroom,
                nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!guard.reconcile()) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    position_image_reconcile,
                projected, retained_count, retained_bytes, headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!protocol_ledger_matches_oracle_()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        protocol_global_position_storage_prepared_ = true;
        return {};
    }

    static constexpr std::size_t query_radix_target_mean_ = 4u;

    struct query_radix_plan {
        std::uint32_t hash_bits = 0u;
        std::uint32_t radix_bits = 0u;
        std::uint32_t suffix_bits = 0u;
        prepared_query_scalar_width suffix_width =
            prepared_query_scalar_width::u8;
        std::size_t bucket_count = 1u;
        std::size_t offset_count = 2u;
    };

    /// Size the high-bit directory for a tiny exact point-lookup bucket. The
    /// high bits are implicit in the bucket and only its low suffix is kept.
    [[nodiscard]] static std::expected<query_radix_plan,
        minimizer_generation_encoding_error>
    plan_query_radix_(
            std::size_t query_count,
            std::uint64_t max_hash) noexcept {
        const std::uint32_t hash_bits = static_cast<std::uint32_t>(
            std::bit_width(max_hash));
        const std::size_t required_buckets =
            query_count / query_radix_target_mean_
            + (query_count % query_radix_target_mean_ != 0u ? 1u : 0u);
        const std::uint32_t requested_bits = required_buckets <= 1u
            ? 0u
            : static_cast<std::uint32_t>(std::bit_width(
                static_cast<std::uint64_t>(required_buckets - 1u)));
        const std::uint32_t radix_bits =
            (std::min)(hash_bits, requested_bits);
        if (radix_bits >= std::numeric_limits<std::size_t>::digits) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::size_t bucket_count = std::size_t{1u} << radix_bits;
        if (bucket_count == (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint32_t suffix_bits = hash_bits - radix_bits;
        const std::uint64_t maximum_suffix = suffix_bits == 0u
            ? 0u
            : suffix_bits >= std::numeric_limits<std::uint64_t>::digits
                ? (std::numeric_limits<std::uint64_t>::max)()
                : (std::uint64_t{1u} << suffix_bits) - 1u;
        return query_radix_plan{
            .hash_bits = hash_bits,
            .radix_bits = radix_bits,
            .suffix_bits = suffix_bits,
            .suffix_width =
                prepared_query_scalar_width_for_max(maximum_suffix),
            .bucket_count = bucket_count,
            .offset_count = bucket_count + 1u,
        };
    }

    [[nodiscard]] static std::size_t query_radix_bucket_(
            hash_type hash,
            std::uint32_t hash_bits,
            std::uint32_t radix_bits) noexcept {
        if (radix_bits == 0u) return 0u;
        return static_cast<std::size_t>(
            static_cast<std::uint64_t>(hash)
                >> (hash_bits - radix_bits));
    }

    [[nodiscard]] static std::uint64_t query_radix_suffix_(
            hash_type hash,
            std::uint32_t suffix_bits) noexcept {
        if (suffix_bits == 0u) return 0u;
        const std::uint64_t value = static_cast<std::uint64_t>(hash);
        if (suffix_bits >= std::numeric_limits<std::uint64_t>::digits) {
            return value;
        }
        return value & ((std::uint64_t{1u} << suffix_bits) - 1u);
    }

    [[nodiscard]] std::size_t query_index_for_hash_impl_(
            std::uint64_t hash,
            std::uint32_t* hash_probes) const noexcept {
        if (hash_probes != nullptr) *hash_probes = 0u;
        if (hash > (std::numeric_limits<hash_type>::max)()
            || hash > this->profile_.max_hash) {
            return no_route;
        }
        const hash_type narrowed = static_cast<hash_type>(hash);
        if constexpr (traits::global_controls_then_positions) {
            if (!protocol_query_radix_offsets_
                || protocol_query_radix_offset_count_ < 2u
                || protocol_query_suffixes_.count != protocol_query_count_
                || protocol_query_exact_counts_.count
                    != protocol_query_count_) {
                return no_route;
            }
            const std::size_t bucket = query_radix_bucket_(
                narrowed, protocol_query_hash_bits_,
                protocol_query_radix_bits_);
            if (bucket + 1u >= protocol_query_radix_offset_count_) {
                return no_route;
            }
            std::size_t first = protocol_query_radix_offsets_[bucket];
            const std::size_t end =
                protocol_query_radix_offsets_[bucket + 1u];
            if (first > end || end > protocol_query_count_) {
                return no_route;
            }
            const std::uint64_t wanted = query_radix_suffix_(
                narrowed, protocol_query_suffix_bits_);
            const auto search = [&](const auto* suffixes) noexcept {
                std::size_t lower = first;
                std::size_t upper = end;
                while (lower < upper) {
                    const std::size_t middle =
                        lower + (upper - lower) / 2u;
                    if (hash_probes != nullptr) ++*hash_probes;
                    if (static_cast<std::uint64_t>(suffixes[middle])
                            < wanted) {
                        lower = middle + 1u;
                    } else {
                        upper = middle;
                    }
                }
                if (lower < end) {
                    if (hash_probes != nullptr) ++*hash_probes;
                    if (static_cast<std::uint64_t>(suffixes[lower])
                            == wanted) {
                        return lower;
                    }
                }
                return no_route;
            };
            switch (protocol_query_suffixes_.width) {
                case prepared_query_scalar_width::u8:
                    return search(protocol_query_suffixes_.u8.get());
                case prepared_query_scalar_width::u16:
                    return search(protocol_query_suffixes_.u16.get());
                case prepared_query_scalar_width::u32:
                    return search(protocol_query_suffixes_.u32.get());
                case prepared_query_scalar_width::u64:
                    return search(protocol_query_suffixes_.u64.get());
            }
            return no_route;
        } else {
            const auto found = std::lower_bound(
                protocol_query_hashes_.begin(),
                protocol_query_hashes_.end(), narrowed);
            return found != protocol_query_hashes_.end() && *found == narrowed
                ? static_cast<std::size_t>(
                    found - protocol_query_hashes_.begin())
                : no_route;
        }
    }

    [[nodiscard]] std::size_t query_index_for_hash(
            std::uint64_t hash) const noexcept {
        return query_index_for_hash_impl_(hash, nullptr);
    }

    [[nodiscard]] std::size_t query_index_for_control_hash(
            std::uint64_t hash,
            std::uint32_t& hash_probes) const noexcept {
        return query_index_for_hash_impl_(hash, &hash_probes);
    }

    [[nodiscard]] bool query_intersects_entry(
            const grouped_soa_bucket_directory_entry& entry) const noexcept {
        const auto found = std::lower_bound(
            protocol_query_hashes_.begin(),
            protocol_query_hashes_.end(),
            static_cast<hash_type>(entry.lo_hash));
        return found != protocol_query_hashes_.end()
            && static_cast<std::uint64_t>(*found) <= entry.hi_hash;
    }

    void initialize_physical_cursor() noexcept {
        protocol_entry_index_ = 0;
        protocol_position_stage_ = false;
        protocol_segment_begin_ = 0;
        protocol_segment_end_ = 0;
        const auto entries = this->directory_.entries();
        if constexpr (!traits::global_controls_then_positions) {
            if (!entries.empty()) {
                protocol_segment_end_ = 1u;
                while (protocol_segment_end_ < entries.size()
                       && entries[protocol_segment_end_].position_segment
                            == entries[0].position_segment) {
                    ++protocol_segment_end_;
                }
            }
        }
    }

    void advance_physical_cursor() noexcept {
        const auto entries = this->directory_.entries();
        ++protocol_entry_index_;
        if constexpr (traits::global_controls_then_positions) {
            if (protocol_entry_index_ == entries.size()
                && !protocol_position_stage_) {
                protocol_position_stage_ = true;
                protocol_entry_index_ = 0;
            } else if (protocol_entry_index_ == entries.size()) {
                protocol_position_stage_ = false;
            }
            return;
        }
        if (protocol_entry_index_ != protocol_segment_end_) return;
        if (!protocol_position_stage_) {
            protocol_position_stage_ = true;
            protocol_entry_index_ = protocol_segment_begin_;
            return;
        }
        protocol_position_stage_ = false;
        protocol_segment_begin_ = protocol_segment_end_;
        protocol_entry_index_ = protocol_segment_begin_;
        if (protocol_segment_begin_ >= entries.size()) return;
        protocol_segment_end_ = protocol_segment_begin_ + 1u;
        while (protocol_segment_end_ < entries.size()
               && entries[protocol_segment_end_].position_segment
                    == entries[protocol_segment_begin_].position_segment) {
            ++protocol_segment_end_;
        }
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    initialize_identity_core(
            bioseq::parallelism::memory_admission_controller& admission,
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const expected_minimizer_generation_identity& expected,
            const grouped_soa_minimizer_profile& profile,
            std::span<const std::byte> encoded_directory,
            std::uint64_t additional_logical_bytes = 0) {
        return identity_core::begin_admitted_core(
            static_cast<identity_core&>(*this),
            admission,
            head,
            manifest,
            expected,
            profile,
            encoded_directory,
            additional_logical_bytes);
    }

    [[nodiscard]] static constexpr std::uint64_t query_hash_value_(
            std::uint64_t hash) noexcept {
        return hash;
    }

    [[nodiscard]] static std::uint64_t query_hash_value_(
            const minimap_sketch_data& minimizer) noexcept {
        return minimizer.kmer();
    }

    template <class QueryValue>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    initialize_query_protocol(
            bioseq::parallelism::memory_admission_controller& admission,
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const expected_minimizer_generation_identity& expected,
            const grouped_soa_minimizer_profile& profile,
            std::span<const std::byte> encoded_directory,
            std::span<const QueryValue> query_hashes,
            std::uint64_t max_occ,
            grouped_soa_query_reader_options test_options = {},
            bool query_hashes_are_sorted_unique = false) {
        protocol_admission_ = &admission;
        if constexpr (traits::global_controls_then_positions) {
            protocol_test_options_ = test_options;
        }
        if (query_hashes.size()
                > (std::numeric_limits<std::size_t>::max)()
                    / sizeof(hash_type)) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        std::uint64_t previous_hash = 0u;
        bool has_previous_hash = false;
        for (const QueryValue& query_value : query_hashes) {
            const std::uint64_t hash = query_hash_value_(query_value);
            if (hash > profile.max_hash
                || hash > (std::numeric_limits<hash_type>::max)()) {
                return std::unexpected(minimizer_generation_encoding_error::hash_range);
            }
            if (query_hashes_are_sorted_unique
                && has_previous_hash && hash <= previous_hash) {
                return std::unexpected(
                    minimizer_generation_encoding_error::row_order);
            }
            previous_hash = hash;
            has_previous_hash = true;
        }
        const std::uint64_t query_bytes =
            bioseq::common::saturating_mul(
                query_hashes.size(), sizeof(hash_type));
        if (query_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    identity_and_directory_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    identity_and_directory_reservation,
                0u, 0u, 0u, 0u, nullptr, true);
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure core_failure;
        if (auto admitted = identity_core::begin_admitted_core(
                static_cast<identity_core&>(*this),
                admission,
                head,
                manifest,
                expected,
                profile,
                encoded_directory,
                query_bytes,
                &core_failure); !admitted) {
            if (admitted.error()
                    == minimizer_generation_encoding_error::admission) {
                capture_admission_failure_(
                    grouped_soa_query_admission_stage::
                        identity_and_directory_reservation,
                    core_failure.requested_bytes,
                    0u, 0u, 0u, &core_failure);
            }
            return std::unexpected(admitted.error());
        }
        if (!replace_ledger_field_(
                protocol_owned_bytes_.directory_bytes,
                this->core_owned_capacity_bytes())) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        protocol_max_occ_ = max_occ;

        permit_growth_guard guard{*this};
        std::uint64_t projected = bioseq::common::saturating_add(
            protocol_owned_capacity_bytes(),
            bioseq::common::saturating_mul(
                query_bytes,
                bioseq::parallelism::k_vector_preallocation_headroom_factor));
        if (projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_hash_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_hash_reservation,
                projected, 0u, 0u, query_bytes, nullptr, true);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure query_hash_failure;
        if (!guard.grow(projected, &query_hash_failure)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_hash_reservation,
                projected, 0u, 0u, query_bytes, &query_hash_failure);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_hash_allocation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::query_hash_allocation,
                projected, 0u, 0u, query_bytes, nullptr, true);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        try {
            protocol_query_hashes_.reserve(query_hashes.size());
            for (const QueryValue& query_value : query_hashes) {
                const std::uint64_t hash = query_hash_value_(query_value);
                protocol_query_hashes_.push_back(
                    static_cast<hash_type>(hash));
            }
            if (!query_hashes_are_sorted_unique) {
                std::ranges::sort(protocol_query_hashes_);
                protocol_query_hashes_.erase(
                    std::unique(protocol_query_hashes_.begin(),
                        protocol_query_hashes_.end()),
                    protocol_query_hashes_.end());
            }
            if constexpr (traits::global_controls_then_positions) {
                // The caller may provide duplicate hashes. Canonicalize into
                // a right-sized temporary image used to build suffixes and
                // select extents; the 2x reservation covers both vectors at
                // this peak, and the full image is released before payload.
                if (!query_hashes_are_sorted_unique) {
                    std::vector<hash_type> canonical;
                    canonical.reserve(protocol_query_hashes_.size());
                    canonical.assign(protocol_query_hashes_.begin(),
                        protocol_query_hashes_.end());
                    protocol_query_hashes_.swap(canonical);
                }
            }
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::query_hash_allocation,
                projected, 0u, 0u, query_bytes);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t actual_query_hash_bytes =
            bioseq::common::saturating_mul(
                protocol_query_hashes_.capacity(), sizeof(hash_type));
        if (actual_query_hash_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || !replace_ledger_field_(
                protocol_owned_bytes_.query_hash_bytes,
                actual_query_hash_bytes)) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_hash_reconcile)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::query_hash_reconcile,
                projected, 0u, 0u, query_bytes, nullptr, true);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        if (!guard.reconcile()) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::query_hash_reconcile,
                projected, 0u, 0u, query_bytes);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        if constexpr (traits::global_controls_then_positions) {
            // UINT32_MAX remains the dense no-route sentinel.  A grouped-SoA
            // query image must therefore leave it unavailable as an index.
            if (protocol_query_hashes_.size() >= no_narrow_route) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }

        query_radix_plan query_radix_plan{};
        prepared_query_scalar_width query_count_width =
            prepared_query_scalar_width::u8;
        if constexpr (traits::global_controls_then_positions) {
            const auto planned = plan_query_radix_(
                protocol_query_hashes_.size(), this->profile_.max_hash);
            if (!planned) return protocol_poison<void>(planned.error());
            query_radix_plan = *planned;
            query_count_width = prepared_query_scalar_width_for_max(
                this->manifest_.position_count);
        }

        const auto entries = this->directory_.entries();
        std::size_t selected_count = 0;
        for (const auto& entry : entries) {
            selected_count += query_intersects_entry(entry) ? 1u : 0u;
        }
        std::uint64_t route_vector_bytes = 0;
        route_vector_bytes = bioseq::common::saturating_add(
            route_vector_bytes,
            bioseq::common::saturating_mul(
                protocol_query_hashes_.size(),
                traits::global_controls_then_positions
                    ? static_cast<std::size_t>(
                        static_cast<std::uint8_t>(
                            query_radix_plan.suffix_width)
                        + static_cast<std::uint8_t>(query_count_width))
                    : sizeof(query_record)));
        route_vector_bytes = bioseq::common::saturating_add(
            route_vector_bytes,
            bioseq::common::saturating_mul(
                entries.size(), sizeof(std::size_t)));
        route_vector_bytes = bioseq::common::saturating_add(
            route_vector_bytes,
            bioseq::common::saturating_mul(
                selected_count, sizeof(selected_extent)));
        const std::uint64_t query_radix_bytes =
            traits::global_controls_then_positions
            ? bioseq::common::saturating_mul(
                query_radix_plan.offset_count, sizeof(std::uint32_t))
            : 0u;
        const std::uint64_t route_bytes =
            bioseq::common::saturating_add(
                route_vector_bytes, query_radix_bytes);
        permit_growth_guard route_guard{*this};
        projected = bioseq::common::saturating_add(
            protocol_owned_capacity_bytes(),
            bioseq::common::saturating_add(
                bioseq::common::saturating_mul(
                    route_vector_bytes,
                    bioseq::parallelism::
                        k_vector_preallocation_headroom_factor),
                query_radix_bytes));
        if (route_bytes == (std::numeric_limits<std::uint64_t>::max)()
            || projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_route_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_route_reservation,
                projected, 0u, 0u, route_bytes, nullptr, true);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure route_failure;
        if (!route_guard.grow(projected, &route_failure)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_route_reservation,
                projected, 0u, 0u, route_bytes, &route_failure);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_route_allocation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::query_route_allocation,
                projected, 0u, 0u, route_bytes, nullptr, true);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        std::uint64_t released_full_query_hash_bytes = 0u;
        try {
            if constexpr (traits::global_controls_then_positions) {
                std::unique_ptr<std::uint32_t[]> radix_offsets;
                prepared_query_scalar_array suffixes;
                prepared_query_scalar_array exact_counts;
                radix_offsets =
                    std::make_unique_for_overwrite<std::uint32_t[]>(
                        query_radix_plan.offset_count);
                suffixes.allocate(
                    query_radix_plan.suffix_width,
                    protocol_query_hashes_.size(), false);
                exact_counts.allocate(
                    query_count_width,
                    protocol_query_hashes_.size(), true);
                std::size_t query_index = 0u;
                std::uint64_t nonempty_buckets = 0u;
                std::uint64_t max_bucket_size = 0u;
                for (std::size_t bucket = 0u;
                     bucket < query_radix_plan.bucket_count; ++bucket) {
                    radix_offsets[bucket] =
                        static_cast<std::uint32_t>(query_index);
                    const std::size_t bucket_begin = query_index;
                    while (query_index < protocol_query_hashes_.size()
                        && query_radix_bucket_(
                            protocol_query_hashes_[query_index],
                            query_radix_plan.hash_bits,
                            query_radix_plan.radix_bits) == bucket) {
                        if (!suffixes.set(
                                query_index,
                                query_radix_suffix_(
                                    protocol_query_hashes_[query_index],
                                    query_radix_plan.suffix_bits))) {
                            return protocol_poison<void>(
                                minimizer_generation_encoding_error::hash_range);
                        }
                        ++query_index;
                    }
                    const std::uint64_t bucket_size =
                        static_cast<std::uint64_t>(
                            query_index - bucket_begin);
                    nonempty_buckets += bucket_size != 0u ? 1u : 0u;
                    max_bucket_size = (std::max)(
                        max_bucket_size, bucket_size);
                }
                if (query_index != protocol_query_hashes_.size()) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::hash_range);
                }
                radix_offsets[query_radix_plan.bucket_count] =
                    static_cast<std::uint32_t>(query_index);
                protocol_query_count_ = protocol_query_hashes_.size();
                protocol_query_suffixes_ = std::move(suffixes);
                protocol_query_exact_counts_ = std::move(exact_counts);
                protocol_query_radix_offsets_ = std::move(radix_offsets);
                protocol_query_radix_offset_count_ =
                    query_radix_plan.offset_count;
                protocol_query_radix_bits_ = query_radix_plan.radix_bits;
                protocol_query_hash_bits_ = query_radix_plan.hash_bits;
                protocol_query_suffix_bits_ = query_radix_plan.suffix_bits;
                protocol_memory_stats_.query_route_radix_bucket_count =
                    query_radix_plan.bucket_count;
                protocol_memory_stats_.
                    query_route_radix_nonempty_bucket_count =
                        nonempty_buckets;
                protocol_memory_stats_.query_route_radix_max_bucket_size =
                    max_bucket_size;
            } else {
                protocol_query_records_.reserve(
                    protocol_query_hashes_.size());
                for (const hash_type hash : protocol_query_hashes_) {
                    protocol_query_records_.push_back(
                        query_record{.hash = hash});
                }
            }
            protocol_ordinal_to_selected_.assign(entries.size(), no_route);
            protocol_selected_extents_.reserve(selected_count);
            for (const auto& entry : entries) {
                if (!query_intersects_entry(entry)) continue;
                protocol_ordinal_to_selected_[entry.partition_id] =
                    protocol_selected_extents_.size();
                protocol_selected_extents_.push_back(selected_extent{});
                protocol_max_selected_control_bytes_ = (std::max)(
                    protocol_max_selected_control_bytes_,
                    entry.control_encoded_bytes);
            }
            if constexpr (traits::global_controls_then_positions) {
                released_full_query_hash_bytes =
                    bioseq::common::saturating_mul(
                        static_cast<std::uint64_t>(
                            protocol_query_hashes_.capacity()),
                        sizeof(hash_type));
                std::vector<hash_type>{}.swap(protocol_query_hashes_);
            }
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::query_route_allocation,
                projected, 0u, 0u, route_bytes);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t query_record_capacity_bytes =
            protocol_query_state_capacity_bytes_();
        const std::uint64_t compact_query_hash_capacity_bytes =
            protocol_query_hash_capacity_bytes_();
        const std::uint64_t radix_offset_capacity_bytes =
            bioseq::common::saturating_mul(
                protocol_query_radix_offset_count_, sizeof(std::uint32_t));
        const std::uint64_t ordinal_map_capacity_bytes =
            bioseq::common::saturating_mul(
                protocol_ordinal_to_selected_.capacity(),
                sizeof(std::size_t));
        const std::uint64_t selected_extent_vector_capacity_bytes =
            bioseq::common::saturating_mul(
                protocol_selected_extents_.capacity(),
                sizeof(selected_extent));
        if (query_record_capacity_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
            || compact_query_hash_capacity_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
            || released_full_query_hash_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
            || radix_offset_capacity_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
            || ordinal_map_capacity_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
            || selected_extent_vector_capacity_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
            || !replace_ledger_field_(
                protocol_owned_bytes_.query_record_bytes,
                query_record_capacity_bytes)
            || !replace_ledger_field_(
                protocol_owned_bytes_.query_hash_bytes,
                compact_query_hash_capacity_bytes)
            || !replace_ledger_field_(
                protocol_owned_bytes_.radix_offset_bytes,
                radix_offset_capacity_bytes)
            || !replace_ledger_field_(
                protocol_owned_bytes_.ordinal_map_bytes,
                ordinal_map_capacity_bytes)
            || !replace_ledger_field_(
                protocol_owned_bytes_.selected_extent_vector_bytes,
                selected_extent_vector_capacity_bytes)) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        add_memory_stats_(
            protocol_memory_stats_.released_query_hash_capacity_bytes,
            released_full_query_hash_bytes);
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    query_route_reconcile)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::query_route_reconcile,
                projected, 0u, 0u, route_bytes, nullptr, true);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        if (!route_guard.reconcile()) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::query_route_reconcile,
                projected, 0u, 0u, route_bytes);
            return protocol_poison<void>(
                minimizer_generation_encoding_error::admission);
        }
        if (!protocol_ledger_matches_oracle_()) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        initialize_physical_cursor();
        protocol_initialized_ = true;
        return {};
    }

    /// Reserve the largest selected control extent once. extent_session owns
    /// only cursor/checksum state and borrows this buffer; no per-extent
    /// reserve/deallocate cycle remains on the cold-load path.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    ensure_control_scratch_() {
        if (!protocol_control_scratch_.empty()) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (protocol_owned_bytes_.control_scratch_bytes != 0u) return {};
        if (protocol_max_selected_control_bytes_
                > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t headroom =
            bioseq::common::saturating_mul(
                protocol_max_selected_control_bytes_,
                bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t projected =
            bioseq::common::saturating_add(
                protocol_owned_capacity_bytes(), headroom);
        if (headroom == (std::numeric_limits<std::uint64_t>::max)()
            || projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        permit_growth_guard guard{*this};
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_reservation,
                projected, 0u, 0u, headroom, nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure payload_failure;
        if (!guard.grow(projected, &payload_failure)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_reservation,
                projected, 0u, 0u, headroom, &payload_failure);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_allocation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_allocation,
                projected, 0u, 0u, headroom, nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        try {
            protocol_control_scratch_.reserve(
                static_cast<std::size_t>(
                    protocol_max_selected_control_bytes_));
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_allocation,
                projected, 0u, 0u, headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t actual_bytes =
            protocol_control_scratch_.capacity();
        if (!replace_ledger_field_(
                protocol_owned_bytes_.control_scratch_bytes,
                actual_bytes)) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_reconcile)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_reconcile,
                projected, 0u, 0u, headroom, nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!guard.reconcile()) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_payload_reconcile,
                projected, 0u, 0u, headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    observe_control_extent(
            const grouped_soa_bucket_directory_entry& entry,
            std::span<const std::byte> bytes) {
        const std::size_t selected_index =
            entry.partition_id < protocol_ordinal_to_selected_.size()
            ? protocol_ordinal_to_selected_[entry.partition_id] : no_route;
        std::uint64_t transient = bioseq::common::saturating_mul(
            entry.control_element_count,
            sizeof(typename wire_ops::control_type));
        // Keep every boundary in an intersected extent for strict per-control
        // position-order validation. grouped-SoA stores query routes sparsely;
        // admit the conservative all-routes peak before the one decode pass.
        const std::uint64_t retained_control_count =
            selected_index == no_route ? 0u : entry.control_element_count;
        const std::uint64_t retained_control_width =
            traits::global_controls_then_positions
            ? static_cast<std::uint8_t>(
                prepared_query_scalar_width_for_max(
                    entry.position_element_count))
            : sizeof(selected_control);
        std::uint64_t retained = bioseq::common::saturating_mul(
            retained_control_count, retained_control_width);
        if constexpr (traits::global_controls_then_positions) {
            // One dense u32 route image is always built.  Sparse conversion
            // happens only below 50% density, so its peak is bounded by one
            // additional dense-image worth of bytes.
            const std::uint64_t dense_route_bytes =
                bioseq::common::saturating_mul(
                    retained_control_count, sizeof(std::uint32_t));
            retained = bioseq::common::saturating_add(
                retained, dense_route_bytes);
            retained = bioseq::common::saturating_add(
                retained, dense_route_bytes);
        }
        const std::uint64_t additional =
            bioseq::common::saturating_add(transient, retained);
        const std::uint64_t transient_headroom =
            bioseq::common::saturating_mul(
                transient,
                bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t retained_headroom = [&]() noexcept {
            if constexpr (traits::global_controls_then_positions) {
                // grouped-SoA ends and route images are exact arrays. Their
                // admitted peak is already represented by retained and must
                // not receive vector growth headroom a second time.
                return retained;
            } else {
                return bioseq::common::saturating_mul(
                    retained,
                    bioseq::parallelism::k_vector_preallocation_headroom_factor);
            }
        }();
        const std::uint64_t headroom =
            bioseq::common::saturating_add(
                transient_headroom, retained_headroom);
        permit_growth_guard guard{*this};
        const std::uint64_t projected = bioseq::common::saturating_add(
            protocol_owned_capacity_bytes(), headroom);
        if (additional == (std::numeric_limits<std::uint64_t>::max)()
            || headroom == (std::numeric_limits<std::uint64_t>::max)()
            || projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_decode_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_decode_reservation,
                projected, 0u, 0u, headroom, nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure control_failure;
        if (!guard.grow(projected, &control_failure)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_decode_reservation,
                projected, 0u, 0u, headroom, &control_failure);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_decode_allocation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_decode_allocation,
                projected, 0u, 0u, headroom, nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        const auto decoded_result = [&]()
                -> std::expected<void, minimizer_generation_encoding_error> {
            try {
                const auto decoded = wire_ops::decode_controls(
                    bytes, entry, this->profile_);
                if (!decoded) return std::unexpected(decoded.error());
                add_memory_stats_(
                    protocol_memory_stats_.decoded_control_records,
                    static_cast<std::uint64_t>(decoded->size()));
                if (selected_index == no_route) return {};
                selected_extent& selected =
                    protocol_selected_extents_[selected_index];
                const std::size_t retained_before =
                    selected_control_count_(selected);
                if (retained_before != 0u || selected.controls_complete) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::row_order);
                }
                if constexpr (traits::global_controls_then_positions) {
                    prepared_query_scalar_array position_ends;
                    std::unique_ptr<std::uint32_t[]> dense_routes;
                    if (decoded->size() != 0u) {
                        position_ends.allocate(
                            prepared_query_scalar_width_for_max(
                                entry.position_element_count),
                            decoded->size(),
                            false);
                        dense_routes =
                            std::make_unique_for_overwrite<std::uint32_t[]>(
                                decoded->size());
                        std::fill_n(
                            dense_routes.get(), decoded->size(),
                            no_narrow_route);
                    }
                    // Publish both exact owners together only after every
                    // allocation succeeds, keeping failure stats exact.
                    selected.control_state.position_ends =
                        std::move(position_ends);
                    selected.route_state.dense_routes =
                        std::move(dense_routes);
                    selected.control_state.control_count = decoded->size();
                } else {
                    selected.control_state.controls.reserve(decoded->size());
                }
                std::uint64_t expected_position_begin = 0u;
                std::size_t control_ordinal = 0u;
                std::size_t routed_control_count = 0u;
                std::uint64_t route_hash_probes = 0u;
                if constexpr (traits::global_controls_then_positions) {
                    constexpr std::uint64_t max_probes_per_lookup =
                        std::numeric_limits<std::uint32_t>::digits + 1u;
                    if (decoded->size()
                            > (std::numeric_limits<std::uint64_t>::max)()
                                / max_probes_per_lookup) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                }
                for (const auto& control : *decoded) {
                    std::uint32_t control_hash_probes = 0u;
                    const std::size_t query_index =
                        query_index_for_control_hash(
                            wire_ops::hash(control), control_hash_probes);
                    route_hash_probes += control_hash_probes;
                    const std::uint64_t begin =
                        wire_ops::position_begin(control);
                    const std::uint64_t end = wire_ops::position_end(control);
                    // Both wire codecs already enforce this partition while
                    // decoding.  Recheck it at the retained-image boundary so
                    // position_begin does not need to survive per control.
                    if (begin != expected_position_begin || end <= begin) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::control_range);
                    }
                    expected_position_begin = end;
                    if (query_index != no_route) {
                        const std::uint64_t count = end - begin;
                        if constexpr (traits::
                                global_controls_then_positions) {
                            if (auto added =
                                    add_protocol_query_exact_count_(
                                        query_index, count); !added) {
                                return added;
                            }
                        } else {
                            query_record& record =
                                protocol_query_records_[query_index];
                            if (record.global_occurrence_count
                                    > (std::numeric_limits<
                                        std::uint64_t>::max)() - count) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::
                                        arithmetic_overflow);
                            }
                            record.global_occurrence_count += count;
                            if (record.global_occurrence_count
                                    > protocol_max_occ_) {
                                const std::uint64_t old_position_bytes =
                                    bioseq::common::saturating_mul(
                                        record.positions.capacity(),
                                        sizeof(std::uint64_t));
                                std::vector<std::uint64_t>{}.swap(
                                    record.positions);
                                if (old_position_bytes
                                        == (std::numeric_limits<
                                            std::uint64_t>::max)()
                                    || old_position_bytes
                                        > protocol_owned_bytes_.
                                            query_record_position_bytes
                                    || !replace_ledger_field_(
                                        protocol_owned_bytes_.
                                            query_record_position_bytes,
                                        protocol_owned_bytes_.
                                            query_record_position_bytes
                                            - old_position_bytes)) {
                                    return std::unexpected(
                                        minimizer_generation_encoding_error::
                                            arithmetic_overflow);
                                }
                                record.positions_complete = false;
                            }
                        }
                    }
                    if constexpr (traits::global_controls_then_positions) {
                        if (!selected.control_state.position_ends.set(
                                control_ordinal, end)) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::control_range);
                        }
                        if (query_index != no_route) {
                            if (query_index >= no_narrow_route) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::
                                        arithmetic_overflow);
                            }
                            selected.route_state.dense_routes[
                                control_ordinal] =
                                    static_cast<std::uint32_t>(query_index);
                            ++routed_control_count;
                        }
                    } else {
                        selected.control_state.controls.push_back(
                            selected_control{
                                .query_index = query_index,
                                .position_end = end,
                            });
                    }
                    ++control_ordinal;
                }
                if (expected_position_begin
                        != entry.position_element_count) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::control_range);
                }
                if constexpr (traits::global_controls_then_positions) {
                    const std::size_t control_count =
                        selected.control_state.control_count;
                    if (control_count <= no_narrow_route
                        && routed_control_count
                            < control_count - routed_control_count) {
                        if (routed_control_count != 0u) {
                            selected.route_state.sparse_routes =
                                std::make_unique_for_overwrite<
                                    sparse_selected_route[]>(
                                        routed_control_count);
                        }
                        std::size_t sparse_index = 0u;
                        for (std::size_t ordinal = 0u;
                             ordinal < control_count; ++ordinal) {
                            const std::uint32_t query_route =
                                selected.route_state.dense_routes[ordinal];
                            if (query_route == no_narrow_route) continue;
                            selected.route_state.sparse_routes[
                                sparse_index++] = sparse_selected_route{
                                    .control_ordinal =
                                        static_cast<std::uint32_t>(ordinal),
                                    .query_index = query_route,
                                };
                        }
                        if (sparse_index != routed_control_count) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::element_count);
                        }
                        selected.route_state.sparse_route_count =
                            routed_control_count;
                        selected.route_state.dense_routes.reset();
                        selected.route_state.uses_sparse = true;
                    }
                    // Update stats once per decoded extent.  A counter write
                    // on every binary-search comparison would itself become
                    // material at hundreds of millions of controls.
                    add_memory_stats_(
                        protocol_memory_stats_.
                            query_route_control_lookups,
                        static_cast<std::uint64_t>(decoded->size()));
                    add_memory_stats_(
                        protocol_memory_stats_.query_route_hash_probes,
                        route_hash_probes);
                    add_memory_stats_(
                        protocol_memory_stats_.query_route_matches,
                        static_cast<std::uint64_t>(routed_control_count));
                }
                add_memory_stats_(
                    protocol_memory_stats_.
                        retained_selected_control_records,
                    static_cast<std::uint64_t>(
                        selected_control_count_(selected)
                            - retained_before));
                add_memory_stats_(
                    protocol_memory_stats_.
                        retained_selected_control_capacity_bytes,
                    selected_extent_control_capacity_bytes_(selected));
                if (!replace_selected_extent_owner_(
                        selected,
                        selected_extent_control_capacity_bytes_(selected))) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                selected.controls_complete = true;
                return {};
            } catch (const std::bad_alloc&) {
                capture_admission_failure_(
                    grouped_soa_query_admission_stage::
                        control_decode_allocation,
                    projected, 0u, 0u, headroom);
                return std::unexpected(
                    minimizer_generation_encoding_error::admission);
            }
        }();
        if (!decoded_result) {
            return std::unexpected(decoded_result.error());
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_decode_reconcile)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_decode_reconcile,
                projected, 0u, 0u, headroom, nullptr, true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!guard.reconcile()) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::
                    control_decode_reconcile,
                projected, 0u, 0u, headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    append_retained_position(
            std::size_t query_index,
            std::uint64_t value) {
        if constexpr (traits::global_controls_then_positions) {
            if (query_index >= protocol_query_count_) {
                return std::unexpected(
                    minimizer_generation_encoding_error::element_count);
            }
            const std::uint64_t exact_count =
                protocol_query_exact_count_(query_index);
            if (exact_count == 0u || exact_count > protocol_max_occ_) {
                return {};
            }
            const std::uint64_t position_offset =
                protocol_query_retained_(query_index)
                ? protocol_query_position_offsets_[query_index]
                : (std::numeric_limits<std::uint64_t>::max)();
            const std::uint64_t positions_written =
                protocol_query_written_.get(query_index);
            if (!protocol_global_position_storage_prepared_
                || !protocol_query_retained_(query_index)
                || positions_written >= exact_count
                || position_offset
                    > protocol_global_position_count_
                || positions_written
                    > protocol_global_position_count_
                        - position_offset) {
                return std::unexpected(
                    minimizer_generation_encoding_error::element_count);
            }
            const std::uint64_t destination =
                position_offset + positions_written;
            if (destination >= protocol_global_position_count_) {
                return std::unexpected(
                    minimizer_generation_encoding_error::element_count);
            }
            if (positions_written != 0u
                && value <= protocol_global_positions_[
                    static_cast<std::size_t>(destination - 1u)]) {
                return std::unexpected(
                    minimizer_generation_encoding_error::position_range);
            }
            protocol_global_positions_[
                static_cast<std::size_t>(destination)] = value;
            if (!protocol_query_written_.increment(query_index)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::element_count);
            }
            return {};
        } else {
            if (query_index >= protocol_query_records_.size()) {
                return std::unexpected(
                    minimizer_generation_encoding_error::element_count);
            }
            query_record& record = protocol_query_records_[query_index];
            if (!record.positions_complete) return {};
            if (!record.positions.empty()
                && value <= record.positions.back()) {
                return std::unexpected(
                    minimizer_generation_encoding_error::position_range);
            }
            if (record.positions.size() == record.positions.capacity()) {
                increment_memory_stats_(
                    protocol_memory_stats_.
                        position_append_growth_reservations);
                const std::size_t maximum = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        protocol_max_occ_, record.positions.max_size()));
                const std::size_t required = record.positions.size() + 1u;
                if (required > maximum) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                std::size_t wanted = record.positions.empty()
                    ? std::min<std::size_t>(maximum, 8u)
                    : record.positions.capacity() > maximum / 2u
                        ? maximum : record.positions.capacity() * 2u;
                wanted = std::max(wanted, required);
                permit_growth_guard guard{*this};
                const std::uint64_t old_position_bytes =
                    bioseq::common::saturating_mul(
                        record.positions.capacity(),
                        sizeof(std::uint64_t));
                const std::uint64_t additional =
                    bioseq::common::saturating_mul(
                        wanted, sizeof(std::uint64_t));
                const std::uint64_t projected =
                    bioseq::common::saturating_add(
                        protocol_owned_capacity_bytes(),
                        bioseq::common::saturating_mul(
                            additional,
                            bioseq::parallelism::
                                k_vector_preallocation_headroom_factor));
                if (!guard.grow(projected)) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                }
                try {
                    record.positions.reserve(wanted);
                } catch (const std::bad_alloc&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                }
                const std::uint64_t new_position_bytes =
                    bioseq::common::saturating_mul(
                        record.positions.capacity(),
                        sizeof(std::uint64_t));
                if (old_position_bytes
                            == (std::numeric_limits<std::uint64_t>::max)()
                    || new_position_bytes
                            == (std::numeric_limits<std::uint64_t>::max)()
                    || old_position_bytes
                        > protocol_owned_bytes_.query_record_position_bytes) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                const std::uint64_t position_owner_without_old =
                    protocol_owned_bytes_.query_record_position_bytes
                    - old_position_bytes;
                if (new_position_bytes
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - position_owner_without_old
                    || !replace_ledger_field_(
                        protocol_owned_bytes_.query_record_position_bytes,
                        position_owner_without_old + new_position_bytes)) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                if (!guard.reconcile()) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                }
            }
            record.positions.push_back(value);
            return {};
        }
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    observe_position_payload(
            const grouped_soa_bucket_directory_entry& entry,
            std::span<const std::byte> payload) {
        const std::size_t width =
            static_cast<std::size_t>(this->profile_.position_width);
        if (width == 0u || payload.size() % width != 0u) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        const std::size_t selected_index =
            entry.partition_id < protocol_ordinal_to_selected_.size()
            ? protocol_ordinal_to_selected_[entry.partition_id] : no_route;
        selected_extent* selected = selected_index == no_route ? nullptr
            : &protocol_selected_extents_[selected_index];
        const auto core = derive_minimizer_position_segment_bounds(
            this->manifest_,
            static_cast<std::uint32_t>(entry.position_segment));
        if (!core) return std::unexpected(core.error());
        for (std::size_t offset = 0; offset < payload.size(); offset += width) {
            const auto decoded = wire_ops::decode_position(
                payload.subspan(offset, width), *core, this->profile_);
            if (!decoded) return std::unexpected(decoded.error());
            if (selected != nullptr) {
                while (selected->next_control
                            < selected_control_count_(*selected)
                       && selected->next_position
                            >= selected_position_end_(
                                *selected, selected->next_control)) {
                    if constexpr (traits::global_controls_then_positions) {
                        auto& routes = selected->route_state;
                        if (routes.uses_sparse
                            && routes.next_sparse_route
                                < routes.sparse_route_count) {
                            const std::uint32_t route_ordinal =
                                routes.sparse_routes[
                                    routes.next_sparse_route]
                                    .control_ordinal;
                            if (route_ordinal < selected->next_control) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::
                                        element_count);
                            }
                            if (route_ordinal == selected->next_control) {
                                ++routes.next_sparse_route;
                            }
                        }
                    }
                    ++selected->next_control;
                    selected->previous_position_valid = false;
                }
                if (selected->next_control
                        < selected_control_count_(*selected)) {
                    std::size_t query_index = no_route;
                    if constexpr (traits::global_controls_then_positions) {
                        const auto& routes = selected->route_state;
                        if (routes.uses_sparse) {
                            if (routes.next_sparse_route
                                    < routes.sparse_route_count) {
                                const sparse_selected_route& route =
                                    routes.sparse_routes[
                                        routes.next_sparse_route];
                                if (route.control_ordinal
                                        < selected->next_control) {
                                    return std::unexpected(
                                        minimizer_generation_encoding_error::
                                            element_count);
                                }
                                if (route.control_ordinal
                                        == selected->next_control) {
                                    query_index = route.query_index;
                                }
                            }
                        } else if (!routes.dense_routes) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::element_count);
                        } else {
                            const std::uint32_t route = routes.dense_routes[
                                selected->next_control];
                            if (route != no_narrow_route) query_index = route;
                        }
                    } else {
                        query_index = selected->control_state.controls[
                            selected->next_control].query_index;
                    }
                    if (selected->previous_position_valid
                        && *decoded <= selected->previous_position) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::position_range);
                    }
                    selected->previous_position = *decoded;
                    selected->previous_position_valid = true;
                    if (query_index != no_route) {
                        if (auto appended = append_retained_position(
                                query_index, *decoded); !appended) {
                            return appended;
                        }
                    }
                }
                ++selected->next_position;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row_impl(
            const minimizer_generation_row_view& row,
            bool row_checksum_prevalidated) {
        const auto entries = this->directory_.entries();
        if (protocol_failed_ || protocol_ready_
            || protocol_entry_index_ >= entries.size()) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::row_order);
        }
        const grouped_soa_bucket_directory_entry& entry =
            entries[protocol_entry_index_];
        const bool positions = protocol_position_stage_;
        const std::size_t selected_index = entry.partition_id
                < protocol_ordinal_to_selected_.size()
            ? protocol_ordinal_to_selected_[entry.partition_id]
            : no_route;
        const bool selected = selected_index != no_route;
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (!position_sub_key) {
            return protocol_poison<void>(position_sub_key.error());
        }
        const std::uint32_t chunk_count = positions
            ? entry.position_chunk_count : entry.control_chunk_count;
        const std::uint64_t encoded_bytes = positions
            ? entry.position_encoded_bytes : entry.control_encoded_bytes;
        if (!protocol_session_) {
            protocol_session_.emplace(extent_session{
                .entry_index = protocol_entry_index_,
                .positions = positions,
            });
            if (!positions && selected) {
                if (encoded_bytes
                        > (std::numeric_limits<std::size_t>::max)()) {
                    protocol_session_.reset();
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                if (auto scratch = ensure_control_scratch_(); !scratch) {
                    protocol_session_.reset();
                    return protocol_poison<void>(scratch.error());
                }
                if (!protocol_control_scratch_.empty()
                    || protocol_control_scratch_.capacity()
                        < static_cast<std::size_t>(encoded_bytes)) {
                    protocol_session_.reset();
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::element_count);
                }
            }
        }
        extent_session& session = *protocol_session_;
        if (session.entry_index != protocol_entry_index_
            || session.positions != positions
            || session.next_chunk >= chunk_count) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::row_order);
        }
        constexpr std::uint32_t extent_payload_bytes =
            minimizer_extent_payload_bytes(traits::layout);
        static_assert(extent_payload_bytes != 0);
        const std::uint64_t byte_offset =
            static_cast<std::uint64_t>(session.next_chunk)
            * extent_payload_bytes;
        if (byte_offset >= encoded_bytes) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::row_order);
        }
        const std::uint64_t expected_size = std::min<std::uint64_t>(
            extent_payload_bytes, encoded_bytes - byte_offset);
        const std::uint32_t first_chunk = positions
            ? entry.position_first_chunk : entry.control_first_chunk;
        const index_sub_key_t expected_sub_key = positions
            ? *position_sub_key : entry.sub_key;
        if (row.address != index_chunk_address{
                .generation_id = this->head_.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = expected_sub_key,
                .chunk_no = first_chunk + session.next_chunk,
            }
            || row.bytes.size() != expected_size) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::row_order);
        }
        if (!row_checksum_prevalidated
            && row.row_checksum != crc64_index_chunk_row(
                row.address.generation_id,
                row.address.position_segment,
                row.address.sub_key,
                row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()),
                row.bytes)) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::checksum);
        }
        // Raw callers reached this point only after their mandatory row CRC
        // traversal above; receipt callers carry the same authenticated
        // address-bound checksum. Compose the extent CRC algebraically in
        // both cases and never reread the payload solely for another CRC.
        session.crc.value = crc64_append_validated_index_chunk_payload(
            session.crc.checksum(), row.address,
            static_cast<std::uint32_t>(row.bytes.size()),
            row.row_checksum);
        if (positions && selected) {
            if (auto observed = observe_position_payload(entry, row.bytes);
                !observed) {
                return protocol_poison<void>(observed.error());
            }
        } else if (!positions && selected) {
            if (protocol_control_scratch_.size()
                    > static_cast<std::size_t>(encoded_bytes)
                || row.bytes.size()
                    > protocol_control_scratch_.capacity()
                        - protocol_control_scratch_.size()
                || row.bytes.size()
                    > static_cast<std::size_t>(encoded_bytes)
                        - protocol_control_scratch_.size()) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::element_count);
            }
            protocol_control_scratch_.insert(
                protocol_control_scratch_.end(),
                row.bytes.begin(), row.bytes.end());
        }
        ++session.next_chunk;
        if (session.next_chunk != chunk_count) return {};
        const std::uint64_t expected_crc = positions
            ? entry.position_checksum : entry.control_checksum;
        if (session.crc.checksum() != expected_crc) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::checksum);
        }
        if (!positions && selected) {
            const auto observed = observe_control_extent(
                entry, protocol_control_scratch_);
            if (!observed) {
                return protocol_poison<void>(observed.error());
            }
            protocol_control_scratch_.clear();
        } else if (positions && selected) {
            selected_extent& extent =
                protocol_selected_extents_[selected_index];
            while (extent.next_control < selected_control_count_(extent)
                   && extent.next_position
                        >= selected_position_end_(
                            extent, extent.next_control)) {
                if constexpr (traits::global_controls_then_positions) {
                    auto& routes = extent.route_state;
                    if (routes.uses_sparse
                        && routes.next_sparse_route
                            < routes.sparse_route_count) {
                        const std::uint32_t route_ordinal =
                            routes.sparse_routes[
                                routes.next_sparse_route].control_ordinal;
                        if (route_ordinal < extent.next_control) {
                            return protocol_poison<void>(
                                minimizer_generation_encoding_error::element_count);
                        }
                        if (route_ordinal == extent.next_control) {
                            ++routes.next_sparse_route;
                        }
                    }
                }
                ++extent.next_control;
                extent.previous_position_valid = false;
            }
            bool routes_complete = true;
            if constexpr (traits::global_controls_then_positions) {
                const auto& routes = extent.route_state;
                routes_complete = routes.uses_sparse
                    ? !routes.dense_routes
                        && routes.next_sparse_route
                            == routes.sparse_route_count
                    : !routes.sparse_routes
                        && (selected_control_count_(extent) == 0u
                            || routes.dense_routes != nullptr);
            }
            if (!extent.controls_complete
                || extent.next_position != entry.position_element_count
                || extent.next_control != selected_control_count_(extent)
                || !routes_complete) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::element_count);
            }
            extent.positions_complete = true;
            if constexpr (traits::global_controls_then_positions) {
                extent.control_state.position_ends.reset();
                extent.control_state.control_count = 0u;
                extent.route_state.dense_routes.reset();
                extent.route_state.sparse_routes.reset();
                extent.route_state.sparse_route_count = 0u;
                extent.route_state.next_sparse_route = 0u;
                extent.route_state.uses_sparse = false;
            } else {
                std::vector<selected_control>{}.swap(
                    extent.control_state.controls);
            }
            if (!replace_selected_extent_owner_(extent, 0u)) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        protocol_session_.reset();
        advance_physical_cursor();
        if constexpr (traits::global_controls_then_positions) {
            if (protocol_position_stage_
                && !protocol_global_position_storage_prepared_) {
                if (!release_control_scratch_noexcept_()) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                this->permit_.shrink_to(protocol_owned_capacity_bytes());
                if (!protocol_ledger_matches_oracle_()) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                if (auto prepared = prepare_global_position_storage();
                    !prepared) {
                    return protocol_poison<void>(prepared.error());
                }
            }
        }
        this->permit_.shrink_to(protocol_owned_capacity_bytes());
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row(const minimizer_generation_row_view& row) {
        return accept_protocol_row_impl(row, false);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row(
            const validated_minimizer_generation_row_receipt& receipt) {
        if (!receipt.has_valid_authority()) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::checksum);
        }
        return accept_protocol_row_impl(minimizer_generation_row_view{
            .address = receipt.address(),
            .bytes = receipt.bytes(),
            .row_checksum = receipt.row_checksum(),
        }, true);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    finish_query_protocol() {
        if (protocol_failed_ || protocol_ready_ || protocol_session_
            || protocol_entry_index_ != this->directory_.entries().size()
            || protocol_position_stage_) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::row_count);
        }
        for (const selected_extent& extent : protocol_selected_extents_) {
            if (!extent.controls_complete || !extent.positions_complete) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::row_count);
            }
        }
        if constexpr (traits::global_controls_then_positions) {
            for (std::size_t query_index = 0u;
                 query_index < protocol_query_count_; ++query_index) {
                const std::uint64_t exact_count =
                    protocol_query_exact_count_(query_index);
                const bool retained = protocol_query_retained_(query_index);
                const std::uint64_t written =
                    protocol_query_written_.get(query_index);
                if (exact_count == 0u || exact_count > protocol_max_occ_) {
                    if (retained || written != 0u) {
                        return protocol_poison<void>(
                            minimizer_generation_encoding_error::element_count);
                    }
                    continue;
                }
                const std::uint64_t position_offset =
                    protocol_query_position_offsets_[query_index];
                if (!retained || written != exact_count
                    || position_offset > protocol_global_position_count_
                    || exact_count > protocol_global_position_count_
                        - position_offset) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::element_count);
                }
            }
            protocol_query_written_.reset();
            if (!replace_ledger_field_(
                    protocol_owned_bytes_.query_record_bytes,
                    protocol_query_state_capacity_bytes_())) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        } else {
            for (const query_record& record : protocol_query_records_) {
                if (record.positions_complete
                    && record.positions.size()
                        != record.global_occurrence_count) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::element_count);
                }
            }
        }
        if (!release_control_scratch_noexcept_()) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        this->permit_.shrink_to(protocol_owned_capacity_bytes());
        if (!protocol_ledger_matches_oracle_()) {
            return protocol_poison<void>(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        protocol_ready_ = true;
        return {};
    }

    struct protocol_lookup {
        std::uint64_t global_occurrence_count = 0;
        bool positions_complete = true;
        std::span<const std::uint64_t> positions{};
    };

    [[nodiscard]] protocol_lookup protocol_lookup(
            std::uint64_t hash) const noexcept {
        if (!protocol_ready_
            || hash > (std::numeric_limits<hash_type>::max)()) return {};
        const std::size_t query_index = query_index_for_hash(hash);
        if (query_index == no_route
            || query_index >= protocol_query_state_count_()) return {};
        std::span<const std::uint64_t> positions;
        if constexpr (traits::global_controls_then_positions) {
            const std::uint64_t exact_count =
                protocol_query_exact_count_(query_index);
            const bool retained = protocol_query_retained_(query_index);
            const bool positions_complete = exact_count == 0u || retained;
            if (retained && exact_count != 0u
                && protocol_query_position_offsets_[query_index]
                    <= protocol_global_position_count_
                && exact_count
                    <= protocol_global_position_count_
                        - protocol_query_position_offsets_[query_index]) {
                positions = std::span<const std::uint64_t>{
                    protocol_global_positions_.get()
                        + static_cast<std::size_t>(
                            protocol_query_position_offsets_[query_index]),
                    static_cast<std::size_t>(exact_count)};
            }
            return protocol_lookup{
                .global_occurrence_count = exact_count,
                .positions_complete = positions_complete,
                .positions = positions,
            };
        } else {
            const query_record& found = protocol_query_records_[query_index];
            positions = found.positions;
            return protocol_lookup{
                .global_occurrence_count = found.global_occurrence_count,
                .positions_complete = found.positions_complete,
                .positions = positions,
            };
        }
    }

    [[nodiscard]] std::size_t
    protocol_retained_position_count() const noexcept {
        if constexpr (traits::global_controls_then_positions) {
            return protocol_global_position_count_;
        } else {
            std::size_t count = 0;
            for (const query_record& record : protocol_query_records_) {
                if (record.positions.size()
                        > (std::numeric_limits<std::size_t>::max)() - count) {
                    return (std::numeric_limits<std::size_t>::max)();
                }
                count += record.positions.size();
            }
            return count;
        }
    }

    [[nodiscard]] const grouped_soa_query_admission_stats&
    protocol_admission_failure_stats() const noexcept {
        return protocol_admission_failure_stats_;
    }
};

} // namespace minimizer_prepared_reader_detail

struct grouped_soa_query_lookup {
    std::uint64_t global_occurrence_count = 0;
    bool positions_complete = true;
    std::span<const std::uint64_t> positions{};
};

/// Reader-owned stats for the grouped-SoA cold-load memory path.  In
/// particular, `position_append_growth_reservations == 0` proves that payload
/// ingestion did not re-run live memory admission (and its OS probes) while
/// appending individual retained positions.
struct grouped_soa_query_reader_memory_stats {
    std::uint64_t capacity_accounting_passes = 0;
    std::uint64_t global_position_storage_preparations = 0;
    std::uint64_t global_position_storage_allocations = 0;
    std::uint64_t position_append_growth_reservations = 0;
    std::uint64_t retained_position_count = 0;
    // Exact allocated array elements. For grouped-SoA this must equal the
    // retained count; the exported capacity name remains stable for E2E SQL.
    std::uint64_t retained_position_capacity = 0;
    std::uint64_t query_record_size_bytes = 0;
    std::uint64_t query_record_count = 0;
    std::uint64_t query_record_capacity = 0;
    std::uint64_t query_record_capacity_bytes = 0;
    std::uint64_t query_record_position_capacity_bytes = 0;
    // Exact bucket-implied query suffixes plus high-bit radix offsets. The
    // aggregate SQL key retains this stable name and exact owner equation.
    std::uint64_t query_hash_capacity_bytes = 0;
    std::uint64_t released_query_hash_capacity_bytes = 0;
    std::uint64_t query_route_radix_bits = 0;
    std::uint64_t query_route_radix_bucket_count = 0;
    std::uint64_t query_route_radix_nonempty_bucket_count = 0;
    std::uint64_t query_route_radix_max_bucket_size = 0;
    std::uint64_t query_route_control_lookups = 0;
    std::uint64_t query_route_hash_probes = 0;
    std::uint64_t query_route_matches = 0;
    std::uint64_t decoded_control_records = 0;
    std::uint64_t retained_selected_control_records = 0;
    std::uint64_t selected_control_size_bytes = 0;
    std::uint64_t retained_selected_control_capacity_bytes = 0;
    std::uint64_t current_selected_control_capacity_bytes = 0;
    std::uint64_t directory_capacity_bytes = 0;
    std::uint64_t ordinal_route_capacity_bytes = 0;
    std::uint64_t selected_extent_capacity_bytes = 0;
    // Exact array bytes, not allocator headroom or vector capacity.
    std::uint64_t global_position_capacity_bytes = 0;
    std::uint64_t control_payload_capacity_bytes = 0;
    std::uint64_t owned_capacity_bytes = 0;
    bool saturated = false;

    [[nodiscard]] bool retained_layout_equation_holds() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t accounted = directory_capacity_bytes;
        accounted = saturating_add(accounted, query_hash_capacity_bytes);
        accounted = saturating_add(accounted, query_record_capacity_bytes);
        accounted = saturating_add(
            accounted, query_record_position_capacity_bytes);
        accounted = saturating_add(accounted, ordinal_route_capacity_bytes);
        accounted = saturating_add(accounted, selected_extent_capacity_bytes);
        accounted = saturating_add(
            accounted, current_selected_control_capacity_bytes);
        accounted = saturating_add(
            accounted, global_position_capacity_bytes);
        accounted = saturating_add(
            accounted, control_payload_capacity_bytes);
        return accounted != (std::numeric_limits<std::uint64_t>::max)()
            && accounted == owned_capacity_bytes;
    }
};

/// Backend-independent scalar grouped-SoA reader. The facade consumes the same
/// grouped_soa_directory_view/address authority as existing prepared readers
/// and accepts only the grouped-SoA layout.
class prepared_grouped_soa_query_reader : private
    minimizer_prepared_reader_detail::trait_driven_prepared_query_core<
        minimizer_prepared_reader_detail::codec_traits<
            minimizer_generation_layout::
                grouped_soa_segment_local>> {
private:
    using protocol_core = minimizer_prepared_reader_detail::
        trait_driven_prepared_query_core<
            minimizer_prepared_reader_detail::codec_traits<
                minimizer_generation_layout::
                    grouped_soa_segment_local>>;

    prepared_grouped_soa_query_reader() = default;

public:
    prepared_grouped_soa_query_reader(
            prepared_grouped_soa_query_reader&& other) noexcept
        : protocol_core(std::move(static_cast<protocol_core&>(other))) {
        other.mark_protocol_moved_from();
    }
    prepared_grouped_soa_query_reader& operator=(
        prepared_grouped_soa_query_reader&&) = delete;
    prepared_grouped_soa_query_reader(
        const prepared_grouped_soa_query_reader&) = delete;
    prepared_grouped_soa_query_reader& operator=(
        const prepared_grouped_soa_query_reader&) = delete;

    [[nodiscard]] static std::expected<prepared_grouped_soa_query_reader,
        minimizer_generation_encoding_error>
    begin(
            bioseq::parallelism::memory_admission_controller& admission,
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const expected_minimizer_generation_identity& expected,
            std::span<const std::byte> encoded_directory,
            std::span<const std::uint64_t> query_hashes,
            std::uint64_t max_occ,
            grouped_soa_query_admission_stats*
                admission_failure_stats = nullptr,
            grouped_soa_query_reader_options test_options = {}) {
        if (admission_failure_stats != nullptr) {
            *admission_failure_stats = {};
        }
        if (manifest.layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local
            || expected.layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || !expected.max_occ_cap
            || *expected.max_occ_cap != profile->max_occ_cap
            || profile->max_occ_cap
                == (std::numeric_limits<std::uint64_t>::max)()
            || max_occ > profile->max_occ_cap) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        prepared_grouped_soa_query_reader reader;
        if (auto initialized = reader.initialize_query_protocol(
                admission,
                head,
                manifest,
                expected,
                *profile,
                encoded_directory,
                query_hashes,
                max_occ,
                test_options); !initialized) {
            if (admission_failure_stats != nullptr) {
                *admission_failure_stats =
                    reader.protocol_admission_failure_stats();
            }
            return std::unexpected(initialized.error());
        }
        return reader;
    }

    /// Direct canonical-query overload used by the scheduler. The input must
    /// already be strictly sorted and unique by kmer(); it is validated while
    /// copying once into the reader-owned compact hash image, avoiding the
    /// scheduler's temporary u64 vector, permit, and a second sort/unique.
    [[nodiscard]] static std::expected<prepared_grouped_soa_query_reader,
        minimizer_generation_encoding_error>
    begin(
            bioseq::parallelism::memory_admission_controller& admission,
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const expected_minimizer_generation_identity& expected,
            std::span<const std::byte> encoded_directory,
            std::span<const minimap_sketch_data> sorted_unique_query_minimizers,
            std::uint64_t max_occ,
            grouped_soa_query_admission_stats*
                admission_failure_stats = nullptr,
            grouped_soa_query_reader_options test_options = {}) {
        if (admission_failure_stats != nullptr) {
            *admission_failure_stats = {};
        }
        if (manifest.layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local
            || expected.layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || !expected.max_occ_cap
            || *expected.max_occ_cap != profile->max_occ_cap
            || max_occ > profile->max_occ_cap) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        prepared_grouped_soa_query_reader reader;
        if (auto initialized = reader.initialize_query_protocol(
                admission,
                head,
                manifest,
                expected,
                *profile,
                encoded_directory,
                sorted_unique_query_minimizers,
                max_occ,
                test_options,
                true); !initialized) {
            if (admission_failure_stats != nullptr) {
                *admission_failure_stats =
                    reader.protocol_admission_failure_stats();
            }
            return std::unexpected(initialized.error());
        }
        return reader;
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const minimizer_generation_row_view& row) {
        return this->accept_protocol_row(row);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const validated_minimizer_generation_row_receipt& receipt) {
        return this->accept_protocol_row(receipt);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    finish() {
        return this->finish_query_protocol();
    }

    [[nodiscard]] grouped_soa_query_lookup lookup(
            std::uint64_t hash) const noexcept {
        const auto found = this->protocol_lookup(hash);
        return grouped_soa_query_lookup{
            .global_occurrence_count = found.global_occurrence_count,
            .positions_complete = found.positions_complete,
            .positions = found.positions,
        };
    }

    [[nodiscard]] const grouped_soa_query_admission_stats&
    admission_failure_stats() const noexcept {
        return this->protocol_admission_failure_stats();
    }

    /// Direct seed-consumer surface. A hash that
    /// was not part of the immutable query union is a caller error rather than
    /// an authenticated absence.  This keeps minimap_port on the native
    /// persisted representation and prevents representation conversion.
    [[nodiscard]] minimizer_lookup_spans lookup_spans(
            std::uint64_t hash) const {
        if (!ready()) {
            throw persisted_minimizer_lookup_error(
                "grouped-SoA persisted lookup is not ready");
        }
        if (hash > this->profile_.max_hash
            || this->query_index_for_hash(hash)
                == (std::numeric_limits<std::size_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "grouped-SoA persisted lookup hash was not prepared");
        }
        const auto found = this->protocol_lookup(hash);
        if (found.global_occurrence_count == 0) {
            return minimizer_lookup_spans::contiguous(
                std::span<const std::uint64_t>{});
        }
        if (!found.positions_complete) {
            return minimizer_lookup_spans::positions_unavailable(
                found.global_occurrence_count);
        }
        if (found.positions.size() != found.global_occurrence_count) {
            throw persisted_minimizer_lookup_error(
                "grouped-SoA persisted lookup retained span is inconsistent");
        }
        return minimizer_lookup_spans::contiguous(found.positions);
    }

    [[nodiscard]] bool failed() const noexcept {
        return this->protocol_failed_;
    }

    [[nodiscard]] bool ready() const noexcept {
        return this->protocol_ready_ && !this->protocol_failed_;
    }

    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return this->manifest_.target_length;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return this->permit_.bytes();
    }

    [[nodiscard]] grouped_soa_query_reader_memory_stats
    memory_stats() const noexcept {
        using bioseq::common::saturating_add;
        const auto& ledger = this->protocol_owned_bytes_;
        const std::uint64_t query_hash_capacity_bytes = saturating_add(
            ledger.query_hash_bytes, ledger.radix_offset_bytes);
        return grouped_soa_query_reader_memory_stats{
            .capacity_accounting_passes =
                this->protocol_memory_stats_.capacity_accounting_passes,
            .global_position_storage_preparations =
                this->protocol_memory_stats_.
                    global_position_storage_preparations,
            .global_position_storage_allocations =
                this->protocol_memory_stats_.
                    global_position_storage_allocations,
            .position_append_growth_reservations =
                this->protocol_memory_stats_.
                    position_append_growth_reservations,
            .retained_position_count =
                static_cast<std::uint64_t>(
                    this->protocol_global_position_count_),
            .retained_position_capacity =
                static_cast<std::uint64_t>(
                    this->protocol_global_position_count_),
            .query_record_size_bytes =
                static_cast<std::uint8_t>(
                    this->protocol_query_exact_counts_.width),
            .query_record_count = static_cast<std::uint64_t>(
                this->protocol_query_state_count_()),
            .query_record_capacity = static_cast<std::uint64_t>(
                this->protocol_query_state_capacity_()),
            .query_record_capacity_bytes = ledger.query_record_bytes,
            .query_record_position_capacity_bytes =
                ledger.query_record_position_bytes,
            .query_hash_capacity_bytes = query_hash_capacity_bytes,
            .released_query_hash_capacity_bytes =
                this->protocol_memory_stats_.
                    released_query_hash_capacity_bytes,
            .query_route_radix_bits =
                this->protocol_query_radix_bits_,
            .query_route_radix_bucket_count =
                this->protocol_memory_stats_.
                    query_route_radix_bucket_count,
            .query_route_radix_nonempty_bucket_count =
                this->protocol_memory_stats_.
                    query_route_radix_nonempty_bucket_count,
            .query_route_radix_max_bucket_size =
                this->protocol_memory_stats_.
                    query_route_radix_max_bucket_size,
            .query_route_control_lookups =
                this->protocol_memory_stats_.
                    query_route_control_lookups,
            .query_route_hash_probes =
                this->protocol_memory_stats_.query_route_hash_probes,
            .query_route_matches =
                this->protocol_memory_stats_.query_route_matches,
            .decoded_control_records =
                this->protocol_memory_stats_.decoded_control_records,
            .retained_selected_control_records =
                this->protocol_memory_stats_.
                    retained_selected_control_records,
            .selected_control_size_bytes =
                sizeof(typename protocol_core::selected_control),
            .retained_selected_control_capacity_bytes =
                this->protocol_memory_stats_.
                    retained_selected_control_capacity_bytes,
            .current_selected_control_capacity_bytes =
                ledger.selected_extent_owner_bytes,
            .directory_capacity_bytes =
                ledger.directory_bytes,
            .ordinal_route_capacity_bytes = ledger.ordinal_map_bytes,
            .selected_extent_capacity_bytes =
                ledger.selected_extent_vector_bytes,
            .global_position_capacity_bytes =
                ledger.global_position_bytes,
            .control_payload_capacity_bytes =
                ledger.control_scratch_bytes,
            .owned_capacity_bytes = ledger.total_bytes,
            .saturated = this->protocol_memory_stats_.saturated,
        };
    }
};
static_assert(minimizer_lookup_index<prepared_grouped_soa_query_reader>);

struct fragment_seed_query {
    std::span<const minimap_sketch_data> minimizers{};
    sequence_size_t query_length = 0u;
    std::int32_t kmer_size = 0;
};

/// Exact query-set identity already produced by the compact-direct occurrence
/// planner. The checksum preserves the historical CPU prepared key-set FNV
/// byte order so deferred publication is numerically identical to the former
/// canonical-u32 owner/sort path.
struct compact_direct_query_set_stats {
    std::uint64_t query_count = 0u;
    std::uint64_t query_occurrence_count = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t group_key_set_checksum = 0u;
    bool present = false;

    [[nodiscard]] constexpr bool operator==(
        const compact_direct_query_set_stats&) const noexcept = default;
};

inline constexpr std::uint64_t k_compact_direct_query_set_fnv_offset =
    UINT64_C(1469598103934665603);
inline constexpr std::uint64_t k_compact_direct_query_set_fnv_prime =
    UINT64_C(1099511628211);

[[nodiscard]] constexpr std::uint64_t compact_direct_query_set_fnv_mix_u64(
        std::uint64_t hash, std::uint64_t value) noexcept {
    for (int byte_index = 0; byte_index < 8; ++byte_index) {
        hash ^= value & UINT64_C(0xff);
        hash *= k_compact_direct_query_set_fnv_prime;
        value >>= 8u;
    }
    return hash;
}

[[nodiscard]] constexpr std::uint64_t
compact_direct_query_set_checksum_prefix(
        std::uint32_t kmer_size,
        std::uint32_t window_size,
        std::uint32_t max_occ) noexcept {
    std::uint64_t checksum = k_compact_direct_query_set_fnv_offset;
    checksum = compact_direct_query_set_fnv_mix_u64(checksum, kmer_size);
    checksum = compact_direct_query_set_fnv_mix_u64(checksum, window_size);
    return compact_direct_query_set_fnv_mix_u64(checksum, max_occ);
}

[[nodiscard]] inline std::uint64_t compact_direct_query_set_checksum(
        std::span<const std::uint32_t> strictly_increasing_unique_hashes,
        std::uint32_t kmer_size,
        std::uint32_t window_size,
        std::uint32_t max_occ) noexcept {
    std::uint64_t checksum = compact_direct_query_set_checksum_prefix(
        kmer_size, window_size, max_occ);
    for (const std::uint32_t hash : strictly_increasing_unique_hashes) {
        checksum = compact_direct_query_set_fnv_mix_u64(checksum, hash);
    }
    return checksum;
}

enum class minimizer_fragment_lookup_route : std::uint8_t {
    grouped_soa_control_scans,
    direct_compact_hash_lookup,
};

/// Borrowed, fail-open bridge from the PostgreSQL-free direct reader to the
/// mapping owner which can issue asynchronous page advice.  Absolute virtual
/// page intervals keep the reader independent of file/mmap offsets; the
/// callback validates and translates them against its stable lease context.
using compact_direct_selected_page_advice_fn = void (*)(
    void* context,
    std::uintptr_t page_begin,
    std::uintptr_t page_end) noexcept;

struct compact_direct_selected_page_adviser {
    void* context = nullptr;
    std::size_t page_bytes = 0u;
    compact_direct_selected_page_advice_fn advise_fn = nullptr;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return context == nullptr && page_bytes == 0u
            && advise_fn == nullptr;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return context != nullptr && page_bytes != 0u
            && advise_fn != nullptr;
    }

    void advise(
            const std::uintptr_t page_begin,
            const std::uintptr_t page_end) const noexcept {
        if (valid() && page_begin < page_end) {
            advise_fn(context, page_begin, page_end);
        }
    }
};
static_assert(std::is_trivially_copyable_v<
    compact_direct_selected_page_adviser>);

struct compact_direct_selected_page_range {
    std::uintptr_t begin = 0u;
    std::uintptr_t end = 0u;

    [[nodiscard]] constexpr bool operator==(
        const compact_direct_selected_page_range&) const noexcept = default;
};

[[nodiscard]] inline std::expected<
    compact_direct_selected_page_range,
    direct_inverted_index_error>
compact_direct_align_selected_page_range(
        const std::uintptr_t raw_begin,
        const std::size_t raw_bytes,
        const std::size_t page_bytes) noexcept {
    if (raw_begin == 0u || raw_bytes == 0u || page_bytes == 0u) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    if (raw_bytes
        > (std::numeric_limits<std::uintptr_t>::max)() - raw_begin) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    const std::uintptr_t raw_end = raw_begin + raw_bytes;
    const std::uintptr_t page_width = page_bytes;
    const std::uintptr_t aligned_begin = raw_begin
        - raw_begin % page_width;
    std::uintptr_t aligned_end = raw_end;
    const std::uintptr_t remainder = aligned_end % page_width;
    if (remainder != 0u) {
        const std::uintptr_t growth = page_width - remainder;
        if (growth
            > (std::numeric_limits<std::uintptr_t>::max)()
                - aligned_end) {
            return std::unexpected(
                direct_inverted_index_error::arithmetic_overflow);
        }
        aligned_end += growth;
    }
    if (aligned_begin >= aligned_end) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return compact_direct_selected_page_range{
        .begin = aligned_begin,
        .end = aligned_end,
    };
}

/// Per-layout-shard, fixed-size coalescer. It merges overlap/touch only and
/// flushes before accepting a genuinely disjoint interval. Callback failure
/// cannot affect control flow because the adviser ABI is noexcept and void.
struct compact_direct_selected_page_advice_accumulator {
    compact_direct_selected_page_range pending_range{};
    bool pending = false;

    [[nodiscard]] bool accumulate(
            const compact_direct_selected_page_adviser adviser,
            const compact_direct_selected_page_range range) noexcept {
        if (range.begin >= range.end) return false;
        if (pending
            && range.begin <= pending_range.end
            && pending_range.begin <= range.end) {
            pending_range.begin = std::min(pending_range.begin, range.begin);
            pending_range.end = std::max(pending_range.end, range.end);
            return true;
        }
        if (pending) flush(adviser);
        pending_range = range;
        pending = true;
        return true;
    }

    void flush(
            const compact_direct_selected_page_adviser adviser) noexcept {
        if (!pending) return;
        adviser.advise(pending_range.begin, pending_range.end);
        pending_range = {};
        pending = false;
    }
};
static_assert(std::is_trivially_copyable_v<
    compact_direct_selected_page_advice_accumulator>);

/// Classify and page-align one span exposed by the authenticated compact
/// traversal. Descriptor-resident P32 singletons intentionally return no
/// range; only spans wholly contained by the authoritative postings section
/// are eligible. This helper performs no allocation and no operating-system
/// call, and keeps all arithmetic checks release-active for focused tests.
[[nodiscard]] inline std::expected<
    std::optional<compact_direct_selected_page_range>,
    direct_inverted_index_error>
compact_direct_selected_page_range(
        const compact_direct_selected_page_adviser adviser,
        const compact_hash32_direct_index_view& direct,
        const std::span<const std::byte> encoded_bytes) noexcept {
    if (adviser.empty()) return std::nullopt;
    if (!adviser.valid() || direct.image.empty()
        || encoded_bytes.empty() || encoded_bytes.data() == nullptr
        || direct.header.image_bytes != direct.image.size()) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    const auto checked_section_end = [&direct](
            const std::uint64_t offset,
            const std::uint64_t bytes)
            -> std::optional<std::uint64_t> {
        if (offset > direct.image.size()
            || bytes > direct.image.size()
                - static_cast<std::size_t>(offset)) {
            return std::nullopt;
        }
        return offset + bytes;
    };
    const auto descriptors_end = checked_section_end(
        direct.header.descriptors_offset,
        direct.header.descriptors_bytes);
    const auto postings_end = checked_section_end(
        direct.header.postings_offset,
        direct.header.postings_bytes);
    if (!descriptors_end || !postings_end
        || *descriptors_end > direct.header.postings_offset) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }

    const std::uintptr_t image_begin =
        reinterpret_cast<std::uintptr_t>(direct.image.data());
    if (direct.image.size()
        > (std::numeric_limits<std::uintptr_t>::max)() - image_begin) {
        return std::unexpected(
            direct_inverted_index_error::arithmetic_overflow);
    }
    const std::uintptr_t image_end = image_begin + direct.image.size();
    const std::uintptr_t raw_begin =
        reinterpret_cast<std::uintptr_t>(encoded_bytes.data());
    const auto aligned = compact_direct_align_selected_page_range(
        raw_begin, encoded_bytes.size(), adviser.page_bytes);
    if (!aligned) return std::unexpected(aligned.error());
    const std::uintptr_t raw_end = raw_begin + encoded_bytes.size();
    if (raw_begin < image_begin || raw_end > image_end) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    const std::uintptr_t descriptors_begin = image_begin
        + static_cast<std::uintptr_t>(direct.header.descriptors_offset);
    const std::uintptr_t descriptors_limit = image_begin
        + static_cast<std::uintptr_t>(*descriptors_end);
    if (raw_begin >= descriptors_begin && raw_end <= descriptors_limit) {
        return std::nullopt;
    }

    const std::uintptr_t postings_begin = image_begin
        + static_cast<std::uintptr_t>(direct.header.postings_offset);
    const std::uintptr_t postings_limit = image_begin
        + static_cast<std::uintptr_t>(*postings_end);
    if (raw_begin < postings_begin || raw_end > postings_limit) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    return std::optional<compact_direct_selected_page_range>{
        *aligned};
}

/// A compact direct view and its exact image token are one contract.  The
/// shared pointer must be an alias whose stored pointer is the first byte of
/// `view.image`; callers owning a larger mmap create the alias at the direct-
/// section offset.  A token without a control block is non-owning, so its
/// caller must retain the external mmap lease for the reader's full lifetime.
/// Keeping this wrapper intact prevents a validated span from being paired
/// with an unrelated owner.
class owned_compact_hash32_direct_index_view {
private:
    std::shared_ptr<const std::byte> image_owner_{};
    compact_hash32_direct_index_view view_{};

    owned_compact_hash32_direct_index_view(
            std::shared_ptr<const std::byte> image_owner,
            compact_hash32_direct_index_view view) noexcept
        : image_owner_(std::move(image_owner)), view_(view) {}

    friend class prepared_minimizer_fragment_reader;

public:
    owned_compact_hash32_direct_index_view() = delete;

    [[nodiscard]] static std::expected<
        owned_compact_hash32_direct_index_view,
        minimizer_generation_encoding_error>
    bind(
            std::shared_ptr<const std::byte> image_owner,
            compact_hash32_direct_index_view view) noexcept {
        if (image_owner == nullptr
            || view.image.empty()
            || image_owner.get() != view.image.data()
            || view.header.image_bytes != view.image.size()) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        return owned_compact_hash32_direct_index_view{
            std::move(image_owner), view};
    }

    [[nodiscard]] const compact_hash32_direct_index_view& view() const noexcept {
        return view_;
    }

    [[nodiscard]] const std::shared_ptr<const std::byte>& owner()
            const noexcept {
        return image_owner_;
    }
};

enum class query_occurrence_route : std::uint8_t {
    packed,
    wide,
};

struct packed_query_occurrence {
    std::uint32_t hash = 0u;
    std::uint32_t query_and_ordinal = 0u;
};
static_assert(sizeof(packed_query_occurrence) == 8u);

/// The compact-direct split keeps smaller occurrence waves on the measured
/// serial/in-place radix baseline so pool dispatch cannot dominate them.
inline constexpr std::size_t
k_compact_direct_parallel_packed_occurrence_radix_min_count = 1u << 16u;

struct wide_query_occurrence {
    std::uint32_t hash = 0u;
    std::uint32_t batch_query_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;
};
static_assert(sizeof(wide_query_occurrence) == 12u);

struct query_occurrence_packing_layout {
    query_occurrence_route route = query_occurrence_route::packed;
    std::uint32_t query_count = 0u;
    std::uint32_t occurrence_count = 0u;
    std::uint32_t query_bits = 0u;
    std::uint32_t ordinal_bits = 32u;
    std::uint64_t ordinal_capacity = std::uint64_t{1} << 32u;
    std::uint32_t ordinal_mask = UINT32_MAX;

    [[nodiscard]] constexpr bool packed() const noexcept {
        return route == query_occurrence_route::packed;
    }

    [[nodiscard]] constexpr bool operator==(
        const query_occurrence_packing_layout&) const noexcept = default;
};

struct query_occurrence_reference {
    std::uint32_t batch_query_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;

    [[nodiscard]] constexpr bool operator==(
        const query_occurrence_reference&) const noexcept = default;
};

[[nodiscard]] constexpr bool valid_query_occurrence_packing_layout(
        const query_occurrence_packing_layout& layout) noexcept {
    if (layout.query_bits > 32u
        || layout.ordinal_bits > 32u
        || layout.query_bits + layout.ordinal_bits != 32u) {
        return false;
    }
    const std::uint32_t expected_query_bits = layout.query_count == 0u
        ? 0u
        : std::bit_width(std::uint32_t(layout.query_count - 1u));
    const std::uint32_t expected_ordinal_bits =
        32u - expected_query_bits;
    const std::uint64_t expected_ordinal_capacity =
        expected_ordinal_bits == 32u
        ? (std::uint64_t{1} << 32u)
        : (std::uint64_t{1} << expected_ordinal_bits);
    const std::uint32_t expected_ordinal_mask =
        expected_ordinal_bits == 32u
        ? UINT32_MAX
        : static_cast<std::uint32_t>(
            (std::uint32_t{1} << expected_ordinal_bits) - 1u);
    return layout.query_bits == expected_query_bits
        && layout.ordinal_bits == expected_ordinal_bits
        && layout.ordinal_capacity == expected_ordinal_capacity
        && layout.ordinal_mask == expected_ordinal_mask;
}

/// Plan the 32-bit packed query/ordinal word without converting a caller's
/// sizes to 32 bits until every existing query-wave bound is proved.
[[nodiscard]] constexpr std::expected<
    query_occurrence_packing_layout,
    minimizer_generation_encoding_error>
plan_query_occurrence_packing(
        std::uint64_t query_count,
        std::uint64_t occurrence_count,
        std::uint64_t largest_query_occurrence_count) noexcept {
    constexpr std::uint64_t u32_limit =
        (std::numeric_limits<std::uint32_t>::max)();
    if (query_count > u32_limit
        || occurrence_count > u32_limit
        || largest_query_occurrence_count > u32_limit
        || largest_query_occurrence_count > occurrence_count
        || (query_count == 0u
            && (occurrence_count != 0u
                || largest_query_occurrence_count != 0u))) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint32_t q32 = static_cast<std::uint32_t>(query_count);
    const std::uint32_t query_bits = q32 == 0u
        ? 0u
        : std::bit_width(std::uint32_t(q32 - 1u));
    const std::uint32_t ordinal_bits = 32u - query_bits;
    const std::uint64_t ordinal_capacity = ordinal_bits == 32u
        ? (std::uint64_t{1} << 32u)
        : (std::uint64_t{1} << ordinal_bits);
    const std::uint32_t ordinal_mask = ordinal_bits == 32u
        ? UINT32_MAX
        : static_cast<std::uint32_t>(
            (std::uint32_t{1} << ordinal_bits) - 1u);
    return query_occurrence_packing_layout{
        .route = largest_query_occurrence_count <= ordinal_capacity
            ? query_occurrence_route::packed
            : query_occurrence_route::wide,
        .query_count = q32,
        .occurrence_count = static_cast<std::uint32_t>(occurrence_count),
        .query_bits = query_bits,
        .ordinal_bits = ordinal_bits,
        .ordinal_capacity = ordinal_capacity,
        .ordinal_mask = ordinal_mask,
    };
}

[[nodiscard]] constexpr std::expected<
    std::uint32_t,
    minimizer_generation_encoding_error>
encode_packed_query_occurrence(
        const query_occurrence_packing_layout& layout,
        std::uint32_t query_ordinal,
        std::uint32_t occurrence_ordinal) noexcept {
    if (!valid_query_occurrence_packing_layout(layout)
        || !layout.packed()
        || query_ordinal >= layout.query_count
        || static_cast<std::uint64_t>(occurrence_ordinal)
            >= layout.ordinal_capacity) {
        return std::unexpected(
            minimizer_generation_encoding_error::partition_id);
    }
    const std::uint64_t encoded =
        (static_cast<std::uint64_t>(query_ordinal)
            << layout.ordinal_bits)
        | occurrence_ordinal;
    if (encoded
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return static_cast<std::uint32_t>(encoded);
}

[[nodiscard, gnu::always_inline]] inline std::uint32_t
encode_packed_query_occurrence_trusted(
        const query_occurrence_packing_layout& layout,
        std::uint32_t query_ordinal,
        std::uint32_t occurrence_ordinal) noexcept {
#ifndef NDEBUG
    DEBUG_ASSERT(
        valid_query_occurrence_packing_layout(layout)
            && layout.packed()
            && query_ordinal < layout.query_count
            && static_cast<std::uint64_t>(occurrence_ordinal)
                < layout.ordinal_capacity,
        "trusted packed query occurrence stays within planned layout");
#endif
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(query_ordinal)
            << layout.ordinal_bits)
        | occurrence_ordinal);
}

[[nodiscard]] constexpr query_occurrence_reference
decode_packed_query_occurrence_trusted(
        const query_occurrence_packing_layout& layout,
        std::uint32_t word) noexcept {
    return query_occurrence_reference{
        .batch_query_ordinal = layout.ordinal_bits == 32u
            ? 0u : word >> layout.ordinal_bits,
        .query_ordinal = layout.ordinal_bits == 32u
            ? word : word & layout.ordinal_mask,
    };
}

struct query_occurrence_capacity_snapshot {
    query_occurrence_route route = query_occurrence_route::packed;
    std::size_t packed_size = 0u;
    std::size_t packed_capacity = 0u;
    std::size_t wide_size = 0u;
    std::size_t wide_capacity = 0u;
    std::size_t query_plan_size = 0u;
    std::size_t query_plan_capacity = 0u;
    std::size_t query_plan_reserve_request = 0u;
    std::size_t query_group_offset_size = 0u;
    std::size_t query_group_offset_capacity = 0u;
    std::uint64_t aggregate_occurrence_capacity_bytes = 0u;
    std::uint64_t query_membership_filter_capacity_bytes = 0u;
    bool occurrence_radix_invoked = false;
    // Route selection only: SINGLE mode or a nested task scope may execute the
    // selected split inline without dispatching work to another pool worker.
    bool parallel_occurrence_radix_split_selected = false;
    bool bucket_direct_matcher_active = false;
};

struct block_query_anchor_layout {
    std::uint64_t forward_begin = 0u;
    std::uint64_t forward_count = 0u;
    std::uint64_t reverse_begin = 0u;
    std::uint64_t reverse_count = 0u;
    std::uint64_t forward_cursor = 0u;
    std::uint64_t reverse_cursor = 0u;
};
static_assert(sizeof(block_query_anchor_layout) == 48u);

struct prepared_query_anchors {
    std::unique_ptr<bioseq::chaining::anchor[]> anchors{};
    std::uint64_t anchor_count = 0u;
    const minimap_sketch_data* minimizer_data = nullptr;
    std::uint64_t minimizer_count = 0u;
    sequence_size_t query_length = 0u;
    std::int32_t kmer_size = 0;
    std::uint64_t sortedness_receipt = 0u;
};

/// Immutable operation stats for one successful fragment-native query wave.
/// Work/timing fields are additive across waves; retained-capacity fields are
/// peaks because one query_wave_scratch owner is reused serially.
struct prepared_fragment_anchor_batch_stats {
    std::uint64_t waves = 0u;
    std::uint64_t queries = 0u;
    std::uint64_t query_occurrences = 0u;
    std::uint64_t unique_query_plans = 0u;
    std::uint64_t packed_occurrence_waves = 0u;
    std::uint64_t wide_occurrence_waves = 0u;
    std::uint64_t occurrence_fill_ns = 0u;
    std::uint64_t occurrence_radix_ns = 0u;
    std::uint64_t unique_plan_build_ns = 0u;
    std::uint64_t query_plan_radix_fill_ns = 0u;
    std::uint64_t control_count_pass_ns = 0u;
    std::uint64_t compact_direct_lookup_ns = 0u;
    std::uint64_t rejected_control_validation_ns = 0u;
    std::uint64_t orientation_layout_ns = 0u;
    std::uint64_t direct_emission_ns = 0u;
    std::uint64_t local_sort_ns = 0u;
    std::uint64_t tie_normalization_ns = 0u;
    std::uint64_t control_count_bytes = 0u;
    std::uint64_t rejected_control_validation_bytes = 0u;
    std::uint64_t pass2_control_bytes = 0u;
    std::uint64_t pass2_accepted_prefix_bytes = 0u;
    std::uint64_t pass2_position_bytes = 0u;
    std::uint64_t pass3_control_bytes = 0u;
    std::uint64_t pass3_accepted_prefix_bytes = 0u;
    std::uint64_t pass3_position_bytes = 0u;
    std::uint64_t anchors_constructed = 0u;
    std::uint64_t local_sort_bytes = 0u;
    std::uint64_t tie_normalization_bytes = 0u;
    std::uint64_t exact_final_bytes = 0u;
    std::uint64_t peak_exact_final_bytes = 0u;
    std::uint64_t global_sort_calls_invoked = 0u;
    std::uint64_t global_sort_calls_skipped = 0u;
    std::uint64_t bucket_direct_matcher_view_receipts = 0u;
    std::uint64_t compositional_sortedness_receipts = 0u;
    std::uint64_t final_sortedness_validation_bytes = 0u;
    std::uint64_t candidate_anchor_bytes = 0u;
    std::uint64_t plan_ordinal_bytes = 0u;
    std::uint64_t compaction_bytes = 0u;
    std::uint64_t final_copy_bytes = 0u;
    std::uint64_t peak_occurrence_capacity_bytes = 0u;
    std::uint64_t peak_query_plan_capacity_bytes = 0u;
    std::uint64_t peak_query_group_offset_capacity_bytes = 0u;
    std::uint64_t peak_query_membership_filter_capacity_bytes = 0u;
    std::uint64_t peak_query_radix_capacity_bytes = 0u;
    std::uint64_t peak_occurrence_count_capacity_bytes = 0u;
    std::uint64_t peak_block_layout_capacity_bytes = 0u;
    std::uint64_t peak_block_stats_capacity_bytes = 0u;
    std::uint64_t compact_direct_waves = 0u;
    std::uint64_t compact_direct_unique_query_groups = 0u;
    std::uint64_t compact_direct_unique_lookups = 0u;
    std::uint64_t compact_direct_selected_hashes = 0u;
    std::uint64_t compact_direct_runtime_rejected_hashes = 0u;
    std::uint64_t compact_direct_runtime_rejected_postings = 0u;
    std::uint64_t compact_direct_selected_postings = 0u;
    std::uint64_t compact_direct_selection_shards = 0u;
    std::uint64_t compact_direct_shard_query_layouts = 0u;
    std::uint64_t compact_direct_layout_shards_completed = 0u;
    std::uint64_t compact_direct_emission_shards_completed = 0u;
    std::uint64_t compact_direct_posting_visit_passes = 0u;
    std::uint64_t compact_direct_layout_postings_visited = 0u;
    std::uint64_t compact_direct_emission_postings_visited = 0u;
    std::uint64_t compact_direct_layout_selection_callbacks = 0u;
    std::uint64_t compact_direct_emission_selection_callbacks = 0u;
    std::uint64_t compact_direct_layout_spans_exposed = 0u;
    std::uint64_t compact_direct_emission_spans_exposed = 0u;
    std::uint64_t compact_direct_layout_descriptors_examined = 0u;
    std::uint64_t compact_direct_emission_descriptors_examined = 0u;
    std::uint64_t compact_direct_traversal_rank_checkpoints_examined = 0u;
    std::uint64_t compact_direct_traversal_membership_words_examined = 0u;
    std::uint64_t compact_direct_rejected_posting_reads = 0u;
    std::uint64_t compact_direct_target_control_scan_bytes = 0u;
    std::uint64_t compact_direct_target_control_scan_passes = 0u;
    std::uint64_t compact_direct_fallbacks = 0u;
    std::uint64_t compact_direct_plan_bytes = 0u;
    std::uint64_t compact_direct_plan_reserve_calls = 0u;
    std::uint64_t compact_direct_plan_reserve_requested_elements = 0u;
    std::uint64_t compact_direct_plan_initial_capacity_elements = 0u;
    std::uint64_t compact_direct_plan_fill_reallocations = 0u;
    std::uint64_t compact_direct_plan_compaction_allocations = 0u;
    std::uint64_t compact_direct_plan_storage_allocations = 0u;
    std::uint64_t compact_direct_plan_retained_capacity_elements = 0u;
    std::uint64_t compact_direct_plan_retained_capacity_bytes = 0u;
    std::uint64_t peak_compact_direct_plan_capacity_bytes = 0u;
    std::uint64_t occurrence_allocations = 0u;
    std::uint64_t query_plan_allocations = 0u;
    std::uint64_t query_group_offset_allocations = 0u;
    std::uint64_t query_membership_filter_allocations = 0u;
    std::uint64_t query_radix_allocations = 0u;
    std::uint64_t occurrence_count_allocations = 0u;
    std::uint64_t block_layout_allocations = 0u;
    std::uint64_t block_stats_allocations = 0u;
    bool saturated = false;

    void merge(const prepared_fragment_anchor_batch_stats& other) noexcept {
        bool overflow = saturated || other.saturated;
        const auto add = [&overflow](
                std::uint64_t& destination,
                std::uint64_t increment) noexcept {
            if (increment
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - destination) {
                destination = (std::numeric_limits<std::uint64_t>::max)();
                overflow = true;
            } else {
                destination += increment;
            }
        };
        add(waves, other.waves);
        add(queries, other.queries);
        add(query_occurrences, other.query_occurrences);
        add(unique_query_plans, other.unique_query_plans);
        add(packed_occurrence_waves, other.packed_occurrence_waves);
        add(wide_occurrence_waves, other.wide_occurrence_waves);
        add(occurrence_fill_ns, other.occurrence_fill_ns);
        add(occurrence_radix_ns, other.occurrence_radix_ns);
        add(unique_plan_build_ns, other.unique_plan_build_ns);
        add(query_plan_radix_fill_ns, other.query_plan_radix_fill_ns);
        add(control_count_pass_ns, other.control_count_pass_ns);
        add(compact_direct_lookup_ns, other.compact_direct_lookup_ns);
        add(rejected_control_validation_ns,
            other.rejected_control_validation_ns);
        add(orientation_layout_ns, other.orientation_layout_ns);
        add(direct_emission_ns, other.direct_emission_ns);
        add(local_sort_ns, other.local_sort_ns);
        add(tie_normalization_ns, other.tie_normalization_ns);
        add(control_count_bytes, other.control_count_bytes);
        add(rejected_control_validation_bytes,
            other.rejected_control_validation_bytes);
        add(pass2_control_bytes, other.pass2_control_bytes);
        add(pass2_accepted_prefix_bytes,
            other.pass2_accepted_prefix_bytes);
        add(pass2_position_bytes, other.pass2_position_bytes);
        add(pass3_control_bytes, other.pass3_control_bytes);
        add(pass3_accepted_prefix_bytes,
            other.pass3_accepted_prefix_bytes);
        add(pass3_position_bytes, other.pass3_position_bytes);
        add(anchors_constructed, other.anchors_constructed);
        add(local_sort_bytes, other.local_sort_bytes);
        add(tie_normalization_bytes, other.tie_normalization_bytes);
        add(exact_final_bytes, other.exact_final_bytes);
        add(global_sort_calls_invoked, other.global_sort_calls_invoked);
        add(global_sort_calls_skipped, other.global_sort_calls_skipped);
        add(bucket_direct_matcher_view_receipts,
            other.bucket_direct_matcher_view_receipts);
        add(compositional_sortedness_receipts,
            other.compositional_sortedness_receipts);
        add(final_sortedness_validation_bytes,
            other.final_sortedness_validation_bytes);
        add(candidate_anchor_bytes, other.candidate_anchor_bytes);
        add(plan_ordinal_bytes, other.plan_ordinal_bytes);
        add(compaction_bytes, other.compaction_bytes);
        add(final_copy_bytes, other.final_copy_bytes);
        add(occurrence_allocations, other.occurrence_allocations);
        add(query_plan_allocations, other.query_plan_allocations);
        add(query_group_offset_allocations,
            other.query_group_offset_allocations);
        add(query_membership_filter_allocations,
            other.query_membership_filter_allocations);
        add(query_radix_allocations, other.query_radix_allocations);
        add(occurrence_count_allocations,
            other.occurrence_count_allocations);
        add(block_layout_allocations, other.block_layout_allocations);
        add(block_stats_allocations, other.block_stats_allocations);
        add(compact_direct_waves, other.compact_direct_waves);
        add(compact_direct_unique_query_groups,
            other.compact_direct_unique_query_groups);
        add(compact_direct_unique_lookups,
            other.compact_direct_unique_lookups);
        add(compact_direct_selected_hashes,
            other.compact_direct_selected_hashes);
        add(compact_direct_runtime_rejected_hashes,
            other.compact_direct_runtime_rejected_hashes);
        add(compact_direct_runtime_rejected_postings,
            other.compact_direct_runtime_rejected_postings);
        add(compact_direct_selected_postings,
            other.compact_direct_selected_postings);
        add(compact_direct_selection_shards,
            other.compact_direct_selection_shards);
        add(compact_direct_shard_query_layouts,
            other.compact_direct_shard_query_layouts);
        add(compact_direct_layout_shards_completed,
            other.compact_direct_layout_shards_completed);
        add(compact_direct_emission_shards_completed,
            other.compact_direct_emission_shards_completed);
        add(compact_direct_posting_visit_passes,
            other.compact_direct_posting_visit_passes);
        add(compact_direct_layout_postings_visited,
            other.compact_direct_layout_postings_visited);
        add(compact_direct_emission_postings_visited,
            other.compact_direct_emission_postings_visited);
        add(compact_direct_layout_selection_callbacks,
            other.compact_direct_layout_selection_callbacks);
        add(compact_direct_emission_selection_callbacks,
            other.compact_direct_emission_selection_callbacks);
        add(compact_direct_layout_spans_exposed,
            other.compact_direct_layout_spans_exposed);
        add(compact_direct_emission_spans_exposed,
            other.compact_direct_emission_spans_exposed);
        add(compact_direct_layout_descriptors_examined,
            other.compact_direct_layout_descriptors_examined);
        add(compact_direct_emission_descriptors_examined,
            other.compact_direct_emission_descriptors_examined);
        add(compact_direct_traversal_rank_checkpoints_examined,
            other.compact_direct_traversal_rank_checkpoints_examined);
        add(compact_direct_traversal_membership_words_examined,
            other.compact_direct_traversal_membership_words_examined);
        add(compact_direct_rejected_posting_reads,
            other.compact_direct_rejected_posting_reads);
        add(compact_direct_target_control_scan_bytes,
            other.compact_direct_target_control_scan_bytes);
        add(compact_direct_target_control_scan_passes,
            other.compact_direct_target_control_scan_passes);
        add(compact_direct_fallbacks, other.compact_direct_fallbacks);
        add(compact_direct_plan_bytes, other.compact_direct_plan_bytes);
        add(compact_direct_plan_reserve_calls,
            other.compact_direct_plan_reserve_calls);
        add(compact_direct_plan_reserve_requested_elements,
            other.compact_direct_plan_reserve_requested_elements);
        add(compact_direct_plan_initial_capacity_elements,
            other.compact_direct_plan_initial_capacity_elements);
        add(compact_direct_plan_fill_reallocations,
            other.compact_direct_plan_fill_reallocations);
        add(compact_direct_plan_compaction_allocations,
            other.compact_direct_plan_compaction_allocations);
        add(compact_direct_plan_storage_allocations,
            other.compact_direct_plan_storage_allocations);
        peak_exact_final_bytes = std::max(
            peak_exact_final_bytes, other.peak_exact_final_bytes);
        peak_occurrence_capacity_bytes = std::max(
            peak_occurrence_capacity_bytes,
            other.peak_occurrence_capacity_bytes);
        peak_query_plan_capacity_bytes = std::max(
            peak_query_plan_capacity_bytes,
            other.peak_query_plan_capacity_bytes);
        peak_query_group_offset_capacity_bytes = std::max(
            peak_query_group_offset_capacity_bytes,
            other.peak_query_group_offset_capacity_bytes);
        peak_query_membership_filter_capacity_bytes = std::max(
            peak_query_membership_filter_capacity_bytes,
            other.peak_query_membership_filter_capacity_bytes);
        peak_query_radix_capacity_bytes = std::max(
            peak_query_radix_capacity_bytes,
            other.peak_query_radix_capacity_bytes);
        peak_occurrence_count_capacity_bytes = std::max(
            peak_occurrence_count_capacity_bytes,
            other.peak_occurrence_count_capacity_bytes);
        peak_block_layout_capacity_bytes = std::max(
            peak_block_layout_capacity_bytes,
            other.peak_block_layout_capacity_bytes);
        peak_block_stats_capacity_bytes = std::max(
            peak_block_stats_capacity_bytes,
            other.peak_block_stats_capacity_bytes);
        peak_compact_direct_plan_capacity_bytes = std::max(
            peak_compact_direct_plan_capacity_bytes,
            other.peak_compact_direct_plan_capacity_bytes);
        compact_direct_plan_retained_capacity_elements = std::max(
            compact_direct_plan_retained_capacity_elements,
            other.compact_direct_plan_retained_capacity_elements);
        compact_direct_plan_retained_capacity_bytes = std::max(
            compact_direct_plan_retained_capacity_bytes,
            other.compact_direct_plan_retained_capacity_bytes);
        saturated = overflow;
    }

    [[nodiscard]] constexpr bool forbidden_buffers_are_zero()
            const noexcept {
        return candidate_anchor_bytes == 0u
            && plan_ordinal_bytes == 0u
            && compaction_bytes == 0u
            && final_copy_bytes == 0u
            && global_sort_calls_invoked == 0u;
    }

    [[nodiscard]] constexpr bool operator==(
        const prepared_fragment_anchor_batch_stats&) const noexcept = default;
};

struct fragment_block_wave_stats {
    std::uint64_t pass2_matched_count_bytes = 0u;
    std::uint64_t pass2_accepted_prefix_bytes = 0u;
    std::uint64_t pass2_rejected_prefix_bytes = 0u;
    std::uint64_t pass2_position_bytes = 0u;
    std::uint64_t pass3_matched_count_bytes = 0u;
    std::uint64_t pass3_accepted_prefix_bytes = 0u;
    std::uint64_t pass3_position_bytes = 0u;
    std::uint64_t anchors_constructed = 0u;
    std::uint64_t local_sort_bytes = 0u;
    std::uint64_t tie_normalization_bytes = 0u;
};
static_assert(sizeof(fragment_block_wave_stats) == 80u);

class prepared_minimizer_fragment_reader;

/// Reusable, separately admitted query-wave planning state. The scratch owns
/// only transient planner/layout capacity and is never referenced by a
/// returned anchor span.
class query_wave_scratch {
private:
    static constexpr std::size_t query_membership_filter_word_bits_ = 21u;
    static constexpr std::size_t query_membership_filter_word_count_ =
        std::size_t{1u} << query_membership_filter_word_bits_;

    enum class wave_state : std::uint8_t {
        idle,
        preparing,
        outstanding,
        cleaning,
    };

    struct query_hash_plan {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(query_hash_plan) == 16u);

    // Declared first so all owned capacities die before their permit.
    bioseq::parallelism::memory_permit permit_{};
    std::vector<packed_query_occurrence> packed_occurrences_{};
    std::vector<wide_query_occurrence> wide_occurrences_{};
    std::vector<query_hash_plan> query_plans_{};
    std::vector<std::uint32_t> query_group_offsets_{};
    std::vector<std::uint32_t> query_plan_radix_{};
    std::unique_ptr<std::uint64_t[]> query_membership_filter_{};
    std::size_t query_membership_filter_word_capacity_ = 0u;
    std::unique_ptr<std::atomic<std::uint32_t>[]>
        occurrence_counts_{};
    std::size_t occurrence_count_capacity_ = 0u;
    std::size_t active_occurrence_count_ = 0u;
    std::vector<block_query_anchor_layout> block_query_layouts_{};
    std::vector<fragment_block_wave_stats> block_stats_{};
    query_occurrence_packing_layout occurrence_layout_{};
    std::size_t query_plan_reserve_request_ = 0u;
    bool occurrence_radix_invoked_ = false;
    bool parallel_occurrence_radix_split_selected_ = false;
    bool bucket_direct_matcher_active_ = false;
    bioseq::parallelism::memory_admission_controller* admission_ = nullptr;
    std::atomic<wave_state> wave_state_{wave_state::idle};

    friend class prepared_minimizer_fragment_reader;

    query_wave_scratch() = default;

    [[nodiscard]] std::uint64_t retained_occurrence_capacity_bytes_()
            const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        return saturating_add(
            saturating_mul(
                packed_occurrences_.capacity(),
                sizeof(packed_query_occurrence)),
            saturating_mul(
                wide_occurrences_.capacity(),
                sizeof(wide_query_occurrence)));
    }

    [[nodiscard]] std::uint64_t retained_capacity_bytes_() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = retained_occurrence_capacity_bytes_();
        bytes = saturating_add(bytes, saturating_mul(
            query_plans_.capacity(), sizeof(query_hash_plan)));
        bytes = saturating_add(bytes, saturating_mul(
            query_group_offsets_.capacity(), sizeof(std::uint32_t)));
        bytes = saturating_add(bytes, saturating_mul(
            query_plan_radix_.capacity(), sizeof(std::uint32_t)));
        bytes = saturating_add(bytes, saturating_mul(
            query_membership_filter_word_capacity_,
            sizeof(std::uint64_t)));
        bytes = saturating_add(bytes, saturating_mul(
            occurrence_count_capacity_,
            sizeof(std::atomic<std::uint32_t>)));
        bytes = saturating_add(bytes, saturating_mul(
            block_query_layouts_.capacity(),
            sizeof(block_query_anchor_layout)));
        bytes = saturating_add(bytes, saturating_mul(
            block_stats_.capacity(),
            sizeof(fragment_block_wave_stats)));
        return bytes;
    }

    void release_all_capacity_noexcept_() noexcept {
        {
            std::vector<packed_query_occurrence> empty_packed_occurrences;
            std::vector<wide_query_occurrence> empty_wide_occurrences;
            std::vector<query_hash_plan> empty_plans;
            std::vector<std::uint32_t> empty_group_offsets;
            std::vector<std::uint32_t> empty_radix;
            std::vector<block_query_anchor_layout> empty_layouts;
            std::vector<fragment_block_wave_stats> empty_block_stats;
            packed_occurrences_.swap(empty_packed_occurrences);
            wide_occurrences_.swap(empty_wide_occurrences);
            query_plans_.swap(empty_plans);
            query_group_offsets_.swap(empty_group_offsets);
            query_plan_radix_.swap(empty_radix);
            block_query_layouts_.swap(empty_layouts);
            block_stats_.swap(empty_block_stats);
            occurrence_counts_.reset();
            query_membership_filter_.reset();
        }
        query_membership_filter_word_capacity_ = 0u;
        occurrence_count_capacity_ = 0u;
        active_occurrence_count_ = 0u;
        occurrence_layout_ = {};
        query_plan_reserve_request_ = 0u;
        occurrence_radix_invoked_ = false;
        parallel_occurrence_radix_split_selected_ = false;
        bucket_direct_matcher_active_ = false;
        permit_.shrink_to(0u);
    }

    void ensure_query_membership_filter_capacity_() {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (query_membership_filter_word_capacity_
                == query_membership_filter_word_count_) {
            std::fill_n(
                query_membership_filter_.get(),
                query_membership_filter_word_capacity_,
                std::uint64_t{0u});
            return;
        }
        const std::uint64_t retained = retained_capacity_bytes_();
        const std::uint64_t allocation = saturating_mul(
            query_membership_filter_word_count_,
            sizeof(std::uint64_t));
        const std::uint64_t peak = saturating_add(retained, allocation);
        if (allocation == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()
            || !permit_.grow_to(peak)) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter admission failed");
        }
        std::unique_ptr<std::uint64_t[]> replacement;
        try {
            replacement = std::make_unique_for_overwrite<
                std::uint64_t[]>(query_membership_filter_word_count_);
        } catch (const std::bad_alloc&) {
            permit_.shrink_to(retained_capacity_bytes_());
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter allocation failed");
        }
        query_membership_filter_ = std::move(replacement);
        query_membership_filter_word_capacity_ =
            query_membership_filter_word_count_;
        std::fill_n(
            query_membership_filter_.get(),
            query_membership_filter_word_capacity_,
            std::uint64_t{0u});
        const std::uint64_t actual = retained_capacity_bytes_();
        if (actual == (std::numeric_limits<std::uint64_t>::max)()
            || actual > permit_.bytes()) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter reconciliation failed");
        }
        permit_.shrink_to(actual);
    }

    template <class T>
    void ensure_vector_capacity_(
            std::vector<T>& values,
            std::size_t required_capacity) {
        if (required_capacity <= values.capacity()) return;
        if (required_capacity > values.max_size()) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch capacity is out of range");
        }
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        const std::uint64_t retained = retained_capacity_bytes_();
        const std::uint64_t requested = saturating_mul(
            required_capacity, sizeof(T));
        const std::uint64_t headroom = saturating_mul(
            requested,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = saturating_add(retained, headroom);
        if (requested == (std::numeric_limits<std::uint64_t>::max)()
            || headroom == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch admission size overflow");
        }
        if (!permit_.grow_to(peak)) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::
                    admission_or_allocation,
                "fragment-native query scratch admission failed");
        }
        try {
            values.reserve(required_capacity);
        } catch (const std::bad_alloc&) {
            permit_.shrink_to(retained_capacity_bytes_());
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::
                    admission_or_allocation,
                "fragment-native query scratch allocation failed");
        }
        const std::uint64_t actual = retained_capacity_bytes_();
        if (actual == (std::numeric_limits<std::uint64_t>::max)()) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch reconciliation overflow");
        }
        if (actual > permit_.bytes() && !permit_.grow_to(actual)) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::
                    admission_or_allocation,
                "fragment-native query scratch reconciliation admission "
                "failed");
        }
        permit_.shrink_to(actual);
    }

    void ensure_occurrence_count_capacity_(std::size_t required_capacity) {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (required_capacity > occurrence_count_capacity_) {
            const std::uint64_t retained = retained_capacity_bytes_();
            const std::uint64_t allocation = saturating_mul(
                required_capacity,
                sizeof(std::atomic<std::uint32_t>));
            const std::uint64_t peak = saturating_add(
                retained, allocation);
            if (allocation
                    == (std::numeric_limits<std::uint64_t>::max)()
                || peak
                    == (std::numeric_limits<std::uint64_t>::max)()
                || !permit_.grow_to(peak)) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count admission failed");
            }
            std::unique_ptr<std::atomic<std::uint32_t>[]> replacement;
            try {
                replacement =
                    std::make_unique<std::atomic<std::uint32_t>[]>(
                        required_capacity);
            } catch (const std::bad_alloc&) {
                permit_.shrink_to(retained_capacity_bytes_());
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count allocation failed");
            }
            occurrence_counts_ = std::move(replacement);
            occurrence_count_capacity_ = required_capacity;
            const std::uint64_t actual = retained_capacity_bytes_();
            if (actual
                    == (std::numeric_limits<std::uint64_t>::max)()
                || actual > permit_.bytes()) {
                release_all_capacity_noexcept_();
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count reconciliation failed");
            }
            permit_.shrink_to(actual);
        }
        active_occurrence_count_ = required_capacity;
        for (std::size_t ordinal = 0u;
             ordinal < required_capacity;
             ++ordinal) {
            occurrence_counts_[ordinal].store(
                0u, std::memory_order_relaxed);
        }
    }

    void clear_wave_storage_noexcept_() noexcept {
        // The next activation zeros its exact U-sized range once. No reader
        // observes inactive counters, so clearing them here would add a second
        // O(U) atomic-store pass to every wave.
        active_occurrence_count_ = 0u;
        packed_occurrences_.clear();
        wide_occurrences_.clear();
        query_plans_.clear();
        query_group_offsets_.clear();
        query_plan_radix_.clear();
        block_query_layouts_.clear();
        block_stats_.clear();
        occurrence_layout_ = {};
        query_plan_reserve_request_ = 0u;
        occurrence_radix_invoked_ = false;
        parallel_occurrence_radix_split_selected_ = false;
        bucket_direct_matcher_active_ = false;
    }

    void reset_failed_wave_() noexcept {
        clear_wave_storage_noexcept_();
        wave_state_.store(wave_state::idle, std::memory_order_release);
    }

public:
    query_wave_scratch(query_wave_scratch&& other) noexcept
        : permit_(std::move(other.permit_)),
          packed_occurrences_(std::move(other.packed_occurrences_)),
          wide_occurrences_(std::move(other.wide_occurrences_)),
          query_plans_(std::move(other.query_plans_)),
          query_group_offsets_(std::move(other.query_group_offsets_)),
          query_plan_radix_(std::move(other.query_plan_radix_)),
          query_membership_filter_(
              std::move(other.query_membership_filter_)),
          query_membership_filter_word_capacity_(
              other.query_membership_filter_word_capacity_),
          occurrence_counts_(std::move(other.occurrence_counts_)),
          occurrence_count_capacity_(
              other.occurrence_count_capacity_),
          active_occurrence_count_(other.active_occurrence_count_),
          block_query_layouts_(
              std::move(other.block_query_layouts_)),
          block_stats_(std::move(other.block_stats_)),
          occurrence_layout_(other.occurrence_layout_),
          query_plan_reserve_request_(other.query_plan_reserve_request_),
          occurrence_radix_invoked_(other.occurrence_radix_invoked_),
          parallel_occurrence_radix_split_selected_(
              other.parallel_occurrence_radix_split_selected_),
          bucket_direct_matcher_active_(
              other.bucket_direct_matcher_active_),
          admission_(other.admission_),
          wave_state_(other.wave_state_.exchange(
              wave_state::idle, std::memory_order_relaxed)) {
        other.occurrence_count_capacity_ = 0u;
        other.active_occurrence_count_ = 0u;
        other.occurrence_layout_ = {};
        other.query_plan_reserve_request_ = 0u;
        other.occurrence_radix_invoked_ = false;
        other.parallel_occurrence_radix_split_selected_ = false;
        other.query_membership_filter_word_capacity_ = 0u;
        other.bucket_direct_matcher_active_ = false;
        other.admission_ = nullptr;
    }

    query_wave_scratch& operator=(query_wave_scratch&&) = delete;
    query_wave_scratch(const query_wave_scratch&) = delete;
    query_wave_scratch& operator=(const query_wave_scratch&) = delete;

    [[nodiscard]] static std::expected<
        query_wave_scratch,
        minimizer_generation_encoding_error>
    create(bioseq::parallelism::memory_admission_controller& admission) {
        auto permit = admission.try_reserve(0u);
        if (!permit) {
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        }
        query_wave_scratch scratch;
        scratch.permit_ = std::move(*permit);
        scratch.admission_ = &admission;
        return scratch;
    }

    void complete_wave_after_join() noexcept {
        wave_state expected = wave_state::outstanding;
        if (wave_state_.compare_exchange_strong(
                expected,
                wave_state::cleaning,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            clear_wave_storage_noexcept_();
            wave_state_.store(wave_state::idle, std::memory_order_release);
        }
    }

    void release_retained_capacity() {
        wave_state expected = wave_state::idle;
        if (!wave_state_.compare_exchange_strong(
                expected,
                wave_state::cleaning,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch is still in use");
        }
        release_all_capacity_noexcept_();
        wave_state_.store(wave_state::idle, std::memory_order_release);
    }

    [[nodiscard]] bool wave_outstanding() const noexcept {
        return wave_state_.load(std::memory_order_acquire)
            != wave_state::idle;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] query_occurrence_capacity_snapshot
    occurrence_capacity_snapshot() const noexcept {
        return query_occurrence_capacity_snapshot{
            .route = occurrence_layout_.route,
            .packed_size = packed_occurrences_.size(),
            .packed_capacity = packed_occurrences_.capacity(),
            .wide_size = wide_occurrences_.size(),
            .wide_capacity = wide_occurrences_.capacity(),
            .query_plan_size = query_plans_.size(),
            .query_plan_capacity = query_plans_.capacity(),
            .query_plan_reserve_request = query_plan_reserve_request_,
            .query_group_offset_size = query_group_offsets_.size(),
            .query_group_offset_capacity = query_group_offsets_.capacity(),
            .aggregate_occurrence_capacity_bytes =
                retained_occurrence_capacity_bytes_(),
            .query_membership_filter_capacity_bytes =
                bioseq::common::saturating_mul(
                    query_membership_filter_word_capacity_,
                    sizeof(std::uint64_t)),
            .occurrence_radix_invoked = occurrence_radix_invoked_,
            .parallel_occurrence_radix_split_selected =
                parallel_occurrence_radix_split_selected_,
            .bucket_direct_matcher_active =
                bucket_direct_matcher_active_,
        };
    }
};

/// Immutable, separately admitted final anchor owner.
class prepared_fragment_anchor_batch {
private:
    static constexpr std::uint64_t sortedness_receipt_ =
        UINT64_C(0x46524147414E4348); // "FRAGANCH"

    // Declared first so exact anchors die before their permit.
    bioseq::parallelism::memory_permit permit_{};
    std::unique_ptr<prepared_query_anchors[]> queries_{};
    std::size_t query_count_ = 0u;
    std::uint64_t target_length_ = 0u;
    std::uint64_t max_occ_ = 0u;
    std::uint64_t owned_bytes_ = 0u;
    prepared_fragment_anchor_batch_stats stats_{};
    compact_direct_query_set_stats compact_direct_query_set_stats_{};
    bool ready_ = false;

    friend class prepared_minimizer_fragment_reader;

    explicit prepared_fragment_anchor_batch(
            bioseq::parallelism::memory_permit&& permit) noexcept
        : permit_(std::move(permit)) {}

public:
    class query_view {
    private:
        const prepared_fragment_anchor_batch* batch_ = nullptr;
        std::size_t query_index_ = 0u;

        friend class prepared_fragment_anchor_batch;

        query_view(
                const prepared_fragment_anchor_batch& batch,
                std::size_t query_index) noexcept
            : batch_(&batch), query_index_(query_index) {}

    public:
        query_view() = default;

        [[nodiscard]] std::uint64_t target_length() const noexcept {
            return batch_ == nullptr ? 0u : batch_->target_length_;
        }

        [[nodiscard]] std::span<const bioseq::chaining::anchor>
        ordered_fragment_seed_anchors(
                std::span<const minimap_sketch_data> minimizers,
                sequence_size_t query_length,
                std::int32_t max_occ,
                std::int32_t kmer_size) const {
            if (batch_ == nullptr || !batch_->ready_
                || query_index_ >= batch_->query_count_
                || max_occ < 0
                || static_cast<std::uint64_t>(max_occ)
                    != batch_->max_occ_) {
                throw persisted_minimizer_lookup_error(
                    "prepared fragment anchor batch is inconsistent");
            }
            const prepared_query_anchors& query =
                batch_->queries_[query_index_];
            if (query.minimizer_data != minimizers.data()
                || query.minimizer_count != minimizers.size()
                || query.query_length != query_length
                || query.kmer_size != kmer_size
                || query.sortedness_receipt != sortedness_receipt_
                || query.anchor_count
                    > (std::numeric_limits<std::size_t>::max)()
                || (query.anchor_count != 0u
                    && query.anchors == nullptr)) {
                throw persisted_minimizer_lookup_error(
                    "prepared fragment anchor query identity is inconsistent");
            }
            return std::span<const bioseq::chaining::anchor>{
                query.anchors.get(),
                static_cast<std::size_t>(query.anchor_count)};
        }
    };

    prepared_fragment_anchor_batch(
            prepared_fragment_anchor_batch&& other) noexcept
        : permit_(std::move(other.permit_)),
          queries_(std::move(other.queries_)),
          query_count_(other.query_count_),
          target_length_(other.target_length_),
          max_occ_(other.max_occ_),
          owned_bytes_(other.owned_bytes_),
          stats_(other.stats_),
          compact_direct_query_set_stats_(
              other.compact_direct_query_set_stats_),
          ready_(other.ready_) {
        other.query_count_ = 0u;
        other.target_length_ = 0u;
        other.max_occ_ = 0u;
        other.owned_bytes_ = 0u;
        other.stats_ = {};
        other.compact_direct_query_set_stats_ = {};
        other.ready_ = false;
    }

    prepared_fragment_anchor_batch& operator=(
            prepared_fragment_anchor_batch&&) = delete;
    prepared_fragment_anchor_batch(
            const prepared_fragment_anchor_batch&) = delete;
    prepared_fragment_anchor_batch& operator=(
            const prepared_fragment_anchor_batch&) = delete;

    [[nodiscard]] query_view query_view(
            std::size_t query_index) const {
        if (!ready_ || query_index >= query_count_) {
            throw persisted_minimizer_lookup_error(
                "prepared fragment anchor query is out of range");
        }
        return query_view{*this, query_index};
    }

    [[nodiscard]] std::size_t query_count() const noexcept {
        return query_count_;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] const prepared_fragment_anchor_batch_stats& stats()
            const noexcept {
        return stats_;
    }

    [[nodiscard]] const compact_direct_query_set_stats&
    compact_direct_query_set_stats() const noexcept {
        return compact_direct_query_set_stats_;
    }
};

/// Fragment-native grouped-SoA reader.  Every physical position segment remains
/// an independently persisted mmap view. Seed collection probes fragment-local
/// controls and packed positions directly. Max-occ reduction is transient
/// query-wave work performed during that same fragment-parallel probe; reader
/// open never scans or combines fragment controls. No posting list,
/// hash-to-fragment span directory, or target-wide occurrence table is
/// materialized, concatenated, merged, or sorted across fragments. The mapped
/// payload is the exact image emitted by PostgreSQL's complete authenticated
/// READY reread, so opening it validates identity and structure without
/// rereading fragment payload.  The compact-direct-only constructor is a
/// separate backing contract: it retains no grouped-SoA directory or payload and
/// authenticates uniform segment geometry without retaining scan-route
/// address plans.
class prepared_minimizer_fragment_reader {
public:
    using fragment_seed_query_type = fragment_seed_query;
    using query_wave_scratch_type = query_wave_scratch;
    using prepared_fragment_anchor_batch_type =
        prepared_fragment_anchor_batch;

private:
    static constexpr std::uint32_t query_radix_bits_ = 20u;
    static constexpr std::uint32_t query_radix_bucket_count_ =
        std::uint32_t{1} << query_radix_bits_;
    static constexpr std::uint32_t query_radix_bucket_mask_ =
        query_radix_bucket_count_ - 1u;
    static constexpr std::uint32_t query_membership_filter_word_mask_ =
        (std::uint32_t{1} << 21u) - 1u;
    static constexpr std::size_t compact_direct_selection_shard_target_ =
        32u;

    struct compact_direct_selection_shard_range final {
        std::size_t begin = 0u;
        std::size_t end = 0u;
    };

    [[nodiscard]] static constexpr compact_direct_selection_shard_range
    compact_direct_selection_shard_range_(
            std::size_t selection_count,
            std::size_t shard_count,
            std::size_t shard_ordinal) noexcept {
        if (shard_count == 0u || shard_ordinal >= shard_count) return {};
        const std::size_t selections_per_shard =
            selection_count / shard_count;
        const std::size_t remainder = selection_count % shard_count;
        const std::size_t begin =
            shard_ordinal * selections_per_shard
            + std::min(shard_ordinal, remainder);
        return compact_direct_selection_shard_range{
            .begin = begin,
            .end = begin + selections_per_shard
                + (shard_ordinal < remainder ? 1u : 0u),
        };
    }

    enum class fragment_encoding_dispatch : std::uint8_t {
        checked_dynamic,
        h32_c16_p16,
    };

    enum class backing_kind : std::uint8_t {
        grouped_soa_fragments,
        compact_direct_only,
    };

    struct entry_plan {
        std::uint64_t control_begin = 0u;
        std::uint64_t control_end = 0u;
        std::uint64_t hash_begin = 0u;
        std::uint64_t hash_end = 0u;
        std::uint64_t count_begin = 0u;
        std::uint64_t count_end = 0u;
        std::uint64_t prefix_begin = 0u;
        std::uint64_t prefix_end = 0u;
        std::uint64_t position_begin = 0u;
        std::uint64_t position_end = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_element_count = 0u;
        std::uint64_t reference_begin = 0u;
        std::uint64_t reference_end = 0u;
        std::uint64_t control_scan_bytes = 0u;
        std::uint32_t position_segment = 0u;
        std::uint8_t hash_byte_width = 0u;
        std::uint8_t count_byte_width = 0u;
        std::uint8_t position_byte_width = 0u;
        std::uint8_t offset_byte_width = 0u;
        minimizer_scalar_width hash_width{};
        minimizer_scalar_width count_width{};
        minimizer_scalar_width position_width{};
        minimizer_scalar_width shape_offset_width{};
    };

    struct fragment_block_plan {
        std::uint32_t entry_begin = 0u;
        std::uint32_t entry_end = 0u;
        std::uint32_t first_position_segment = 0u;
        std::uint32_t last_position_segment = 0u;
        std::uint64_t reference_begin = 0u;
        std::uint64_t reference_end = 0u;
    };

    struct direct_open_contract {
        grouped_soa_minimizer_profile profile{};
        std::uint8_t hash_domain_bits = 0u;
    };

    bioseq::parallelism::memory_permit permit_{};
    grouped_soa_directory_view directory_{};
    minimizer_generation_head head_{};
    minimizer_generation_manifest manifest_{};
    grouped_soa_minimizer_profile profile_{};
    std::shared_ptr<const void> mapped_payload_owner_{};
    std::span<const std::byte> mapped_payload_{};
    std::optional<owned_compact_hash32_direct_index_view> compact_direct_{};
    compact_direct_selected_page_adviser selected_page_adviser_{};
    std::unique_ptr<entry_plan[]> entry_plans_{};
    std::unique_ptr<fragment_block_plan[]> block_plans_{};
    std::size_t entry_count_ = 0;
    // Exact retained block_plans_ element count. Direct-only readers keep
    // this zero and expose manifest segment geometry through the public view.
    std::size_t block_count_ = 0;
    std::uint32_t largest_block_entry_count_ = 0u;
    std::uint32_t oversized_segment_block_count_ = 0u;
    std::uint64_t max_occ_ = 0;
    std::uint64_t total_control_hash_scan_bytes_ = 0u;
    std::uint64_t total_control_count_scan_bytes_ = 0u;
    std::uint64_t owned_bytes_ = 0;
    std::uint8_t query_radix_shift_ = 12u;
    std::uint8_t query_membership_filter_shift_ = 11u;
    std::uint16_t query_membership_filter_mask_id_mask_ = 0x7ffu;
    fragment_encoding_dispatch wire_dispatch_ =
        fragment_encoding_dispatch::checked_dynamic;
    backing_kind backing_kind_ = backing_kind::grouped_soa_fragments;
    bool ready_ = false;

    prepared_minimizer_fragment_reader() = default;

    [[nodiscard]] static constexpr std::uint32_t project_query_bucket_(
            std::uint32_t hash, std::uint8_t shift) noexcept {
        return (hash >> shift) & query_radix_bucket_mask_;
    }

    [[nodiscard]] constexpr std::uint32_t query_bucket_(
            std::uint32_t hash) const noexcept {
        return project_query_bucket_(hash, query_radix_shift_);
    }

    static_assert(((std::uint32_t{1} << 28u) >> 10u)
        == (std::uint32_t{1} << 18u));
    static_assert(((std::uint32_t{1} << 29u) >> 10u)
        == (std::uint32_t{1} << 19u));
    static_assert(((std::uint32_t{3} << 28u) >> 10u)
        == (std::uint32_t{3} << 18u));
    static_assert((UINT32_MAX >> 12u) == query_radix_bucket_mask_);

    [[nodiscard]] static std::uint64_t stage_now_ns_() noexcept {
        const auto now = bioseq::instrumentation::clock::now_ns();
        return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
    }

    [[nodiscard]] static std::uint64_t elapsed_stage_ns_(
            std::uint64_t started_ns) noexcept {
        const std::uint64_t finished_ns = stage_now_ns_();
        const std::uint64_t elapsed = finished_ns >= started_ns
            ? finished_ns - started_ns : 0u;
        return elapsed > 0u ? elapsed : 1u;
    }

    template <minimizer_scalar_width Width>
    using minimizer_wire_uint_t_ = std::conditional_t<
        Width == minimizer_scalar_width::u16,
        std::uint16_t,
        std::conditional_t<
            Width == minimizer_scalar_width::u32,
            std::uint32_t,
            std::uint64_t>>;

    /// Read one scalar after open() has proved the containing component and
    /// extent geometry. The DEBUG_ASSERT documents that stats without
    /// replacing the release-active checks at the plan boundary.
    template <minimizer_scalar_width Width>
    [[nodiscard, gnu::always_inline]]
    static std::uint64_t read_fixed_width_le_(
            std::span<const std::byte> validated_extent,
            std::size_t offset) noexcept {
        static_assert(
            Width == minimizer_scalar_width::u16
            || Width == minimizer_scalar_width::u32
            || Width == minimizer_scalar_width::u64);
        using T = minimizer_wire_uint_t_<Width>;
        DEBUG_ASSERT(
            offset <= validated_extent.size()
                && sizeof(T) <= validated_extent.size() - offset,
            "fixed-width scalar read stays in its prevalidated extent");
        return minimizer_generation_wire_detail::read_le<T>(
            validated_extent, offset);
    }

    /// Concrete position decoder shared by both P3 occurrence routes. Route
    /// selection remains outside this cursor, so packed/wide does not become
    /// a template axis for the wire loop.
    class position_range_cursor {
    private:
        std::span<const std::byte> payload_{};
        grouped_soa_decoded_control control_{};
        minimizer_position_segment_bounds core_{};
        minimizer_scalar_width width_ = minimizer_scalar_width::u16;
        std::size_t position_begin_ = 0u;
        std::size_t position_byte_width_ = 0u;
        std::uint64_t ordinal_ = 0u;
        std::optional<std::uint64_t> previous_{};
        std::optional<minimizer_generation_encoding_error> error_{};

        [[nodiscard]] bool accept_local_(
                std::uint64_t local,
                std::uint64_t& packed_global) noexcept {
            const std::uint64_t local_bp = local >> 1u;
            if (core_.end_bp <= core_.begin_bp
                || local_bp >= core_.end_bp - core_.begin_bp
                || core_.begin_bp
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - local_bp) {
                error_ = minimizer_generation_encoding_error::position_range;
                return false;
            }
            const std::uint64_t global_bp = core_.begin_bp + local_bp;
            if (global_bp
                    > ((std::numeric_limits<std::uint64_t>::max)()
                        >> 1u)) {
                error_ = minimizer_generation_encoding_error::arithmetic_overflow;
                return false;
            }
            packed_global = (global_bp << 1u) | (local & 1u);
            if (previous_ && packed_global <= *previous_) {
                error_ = minimizer_generation_encoding_error::position_range;
                return false;
            }
            previous_ = packed_global;
            ++ordinal_;
            return true;
        }

    public:
        [[nodiscard]] static std::expected<
            position_range_cursor,
            minimizer_generation_encoding_error>
        create(
                std::span<const std::byte> payload,
                const grouped_soa_decoded_control& control,
                const entry_plan& entry) noexcept {
            if (control.position_end <= control.position_begin
                || control.position_end > entry.position_element_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::control_range);
            }
            DEBUG_ASSERT(is_minimizer_scalar_width(entry.position_width)
                    && entry.position_byte_width
                        == static_cast<std::uint8_t>(entry.position_width)
                    && entry.position_begin <= entry.position_end
                    && entry.position_end <= payload.size()
                    && entry.position_end - entry.position_begin
                        == entry.position_element_count
                            * entry.position_byte_width,
                "position cursor consumes open-validated entry geometry");
            position_range_cursor cursor;
            cursor.payload_ = payload;
            cursor.control_ = control;
            cursor.core_ = minimizer_position_segment_bounds{
                .begin_bp = entry.reference_begin,
                .end_bp = entry.reference_end,
            };
            cursor.width_ = entry.position_width;
            cursor.position_begin_ = static_cast<std::size_t>(
                entry.position_begin);
            cursor.position_byte_width_ = entry.position_byte_width;
            cursor.ordinal_ = control.position_begin;
            return cursor;
        }

        [[nodiscard, gnu::always_inline]] bool next_fixed_p16(
                std::uint64_t& packed_global) noexcept {
            if (error_ || ordinal_ >= control_.position_end) return false;
            DEBUG_ASSERT(width_ == minimizer_scalar_width::u16,
                "fixed P16 cursor requires a P16 plan");
            const std::size_t offset = position_begin_
                + static_cast<std::size_t>(ordinal_)
                    * sizeof(std::uint16_t);
            return accept_local_(
                read_fixed_width_le_<minimizer_scalar_width::u16>(
                    payload_, offset),
                packed_global);
        }

        [[nodiscard, gnu::always_inline]] bool next_dynamic(
                std::uint64_t& packed_global) noexcept {
            if (error_ || ordinal_ >= control_.position_end) return false;
            const std::size_t offset = position_begin_
                + static_cast<std::size_t>(ordinal_)
                    * position_byte_width_;
            std::uint64_t local = 0u;
            switch (width_) {
            case minimizer_scalar_width::u16:
                local = minimizer_generation_wire_detail::read_le<
                    std::uint16_t>(payload_, offset);
                break;
            case minimizer_scalar_width::u32:
                local = minimizer_generation_wire_detail::read_le<
                    std::uint32_t>(payload_, offset);
                break;
            case minimizer_scalar_width::u64:
                local = minimizer_generation_wire_detail::read_le<
                    std::uint64_t>(payload_, offset);
                break;
            }
            return accept_local_(local, packed_global);
        }

        [[nodiscard]] std::optional<minimizer_generation_encoding_error>
        error() const noexcept {
            return error_;
        }
    };

    enum class fragment_probe_pass : std::uint8_t {
        control_count,
        rejected_validation,
        orientation,
        emission,
    };

    struct query_occurrence_group final {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t counter_ordinal = 0u;
    };
    static_assert(sizeof(query_occurrence_group) == 16u);

    struct bucket_direct_matcher_view final {
        const std::uint64_t* membership_filter = nullptr;
        const std::uint32_t* radix = nullptr;
        const std::uint32_t* group_offsets = nullptr;
        const packed_query_occurrence* packed_occurrences = nullptr;
        const wide_query_occurrence* wide_occurrences = nullptr;
        std::atomic<std::uint32_t>* occurrence_counts = nullptr;
        std::size_t membership_filter_word_count = 0u;
        std::size_t radix_count = 0u;
        std::size_t group_offset_count = 0u;
        std::size_t packed_occurrence_count = 0u;
        std::size_t wide_occurrence_count = 0u;
        std::size_t occurrence_count = 0u;
        std::size_t counter_count = 0u;
        query_occurrence_packing_layout occurrence_layout{};
    };

    struct fragment_probe_context final {
        fragment_probe_pass pass = fragment_probe_pass::control_count;
        const fragment_block_plan* block = nullptr;
        std::size_t block_ordinal = 0u;
        std::span<const query_wave_scratch::query_hash_plan> plans{};
        std::span<const std::uint32_t> query_plan_radix{};
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        const bucket_direct_matcher_view* bucket_direct_matcher = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::uint32_t max_occ = 0u;
        std::uint64_t* matched_count_bytes = nullptr;
        std::uint64_t* accepted_prefix_bytes = nullptr;
        std::uint64_t* rejected_prefix_bytes = nullptr;
        std::uint64_t* position_bytes = nullptr;
        std::uint64_t* anchors_constructed = nullptr;
    };

    struct alignas(64) CompactDirectLayoutPassContext final {
        const prepared_minimizer_fragment_reader* reader = nullptr;
        const compact_hash32_unique_query_plan* plan = nullptr;
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        std::size_t shard_ordinal = 0u;
        std::uint64_t logical_postings_counted = 0u;
        compact_direct_selected_page_advice_accumulator selected_page_advice{};
        std::optional<minimizer_generation_encoding_error> error{};
        std::optional<direct_inverted_index_error> direct_error{};
        std::optional<compact_hash32_selection_span_visit_stats> visit_stats{};
    };

    struct alignas(64) CompactDirectEmissionPassContext final {
        const prepared_minimizer_fragment_reader* reader = nullptr;
        const compact_hash32_unique_query_plan* plan = nullptr;
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::size_t shard_ordinal = 0u;
        std::uint64_t posting_bytes_read = 0u;
        std::uint64_t postings_visited = 0u;
        std::uint64_t anchors_constructed = 0u;
        std::optional<minimizer_generation_encoding_error> error{};
        std::optional<direct_inverted_index_error> direct_error{};
        std::optional<compact_hash32_selection_span_visit_stats> visit_stats{};
    };

    inline static constexpr std::array<std::uint64_t, 1u << 11u>
        query_membership_filter_masks_ = []() consteval {
            std::array<std::uint64_t, 1u << 11u> masks{};
            for (std::size_t low = 0u; low < masks.size(); ++low) {
                const std::uint64_t mixed =
                    (static_cast<std::uint64_t>(low) + 1u)
                    * UINT64_C(0x9e3779b97f4a7c15);
                masks[low] =
                    (UINT64_C(1) << ((mixed >> 0u) & 63u))
                    | (UINT64_C(1) << ((mixed >> 6u) & 63u))
                    | (UINT64_C(1) << ((mixed >> 12u) & 63u))
                    | (UINT64_C(1) << ((mixed >> 18u) & 63u))
                    | (UINT64_C(1) << ((mixed >> 24u) & 63u))
                    | (UINT64_C(1) << ((mixed >> 30u) & 63u));
            }
            return masks;
        }();

    [[nodiscard, gnu::always_inline]] constexpr std::uint64_t
    query_membership_filter_mask_(std::uint32_t hash) const noexcept {
        // Reader open derives the low-bit mask from the persisted hash domain.
        // Those remaining bits identify a hash inside one filter word; one
        // compile-time LUT preserves the exact six register-blocked positions
        // without repeating their multiplicative diffusion in every probe.
        // A query inserts this exact mask and a probe tests this exact mask,
        // which is the no-false-negative authority; positives still undergo
        // an exact hash comparison inside their packed-occurrence bucket.
        return query_membership_filter_masks_[
            hash & query_membership_filter_mask_id_mask_];
    }

    [[nodiscard]] static constexpr std::size_t
    project_query_membership_filter_word_(
            std::uint32_t hash, std::uint8_t shift) noexcept {
        return static_cast<std::size_t>(
            (hash >> shift) & query_membership_filter_word_mask_);
    }

    [[nodiscard, gnu::always_inline]] constexpr std::size_t
    query_membership_filter_word_(std::uint32_t hash) const noexcept {
        return project_query_membership_filter_word_(
            hash, query_membership_filter_shift_);
    }

    static_assert(((std::uint32_t{1} << 28u) >> 9u)
        == (std::uint32_t{1} << 19u));
    static_assert(((std::uint32_t{1} << 29u) >> 9u)
        == (std::uint32_t{1} << 20u));
    static_assert(((std::uint32_t{3} << 28u) >> 9u)
        == (std::uint32_t{3} << 19u));
    static_assert((UINT32_MAX >> 11u)
        == query_membership_filter_word_mask_);

    [[nodiscard, gnu::always_inline]] static query_occurrence_reference
    query_occurrence_reference_(
            const packed_query_occurrence& occurrence,
            const query_occurrence_packing_layout& layout) noexcept {
        return decode_packed_query_occurrence_trusted(
            layout, occurrence.query_and_ordinal);
    }

    [[nodiscard, gnu::always_inline]] static query_occurrence_reference
    query_occurrence_reference_(
            const wide_query_occurrence& occurrence,
            const query_occurrence_packing_layout&) noexcept {
        return query_occurrence_reference{
            .batch_query_ordinal = occurrence.batch_query_ordinal,
            .query_ordinal = occurrence.query_ordinal,
        };
    }

    /// Select packed or wide storage once for a complete hash plan. The
    /// visitor receives a route-specific span, so its target-position x query-
    /// occurrence loop contains no representation branch.
    template <bool Oracle, class Visitor>
    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    with_query_group_occurrences_(
            const fragment_probe_context& context,
            const query_occurrence_group& group,
            Visitor&& visitor) noexcept {
        const std::size_t begin_size = group.occurrence_ref_begin;
        const std::size_t count_size =
            static_cast<std::size_t>(group.occurrence_ref_count);
        const query_occurrence_packing_layout* occurrence_layout = nullptr;
        const packed_query_occurrence* packed_occurrences = nullptr;
        const wide_query_occurrence* wide_occurrences = nullptr;
        std::size_t packed_occurrence_count = 0u;
        std::size_t wide_occurrence_count = 0u;
        if constexpr (Oracle) {
            DEBUG_ASSERT(context.scratch != nullptr,
                "oracle occurrence view is prevalidated");
            occurrence_layout = &context.scratch->occurrence_layout_;
            packed_occurrences = context.scratch->packed_occurrences_.data();
            wide_occurrences = context.scratch->wide_occurrences_.data();
            packed_occurrence_count =
                context.scratch->packed_occurrences_.size();
            wide_occurrence_count =
                context.scratch->wide_occurrences_.size();
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                "production occurrence view is prevalidated");
            const bucket_direct_matcher_view& view =
                *context.bucket_direct_matcher;
            occurrence_layout = &view.occurrence_layout;
            packed_occurrences = view.packed_occurrences;
            wide_occurrences = view.wide_occurrences;
            packed_occurrence_count = view.packed_occurrence_count;
            wide_occurrence_count = view.wide_occurrence_count;
        }
        switch (occurrence_layout->route) {
        case query_occurrence_route::packed:
            DEBUG_ASSERT(begin_size <= packed_occurrence_count
                    && count_size <= packed_occurrence_count - begin_size,
                "packed group range is prevalidated");
            return std::forward<Visitor>(visitor)(
                std::span<const packed_query_occurrence>{
                    packed_occurrences + begin_size, count_size},
                *occurrence_layout);
        case query_occurrence_route::wide:
            DEBUG_ASSERT(begin_size <= wide_occurrence_count
                    && count_size <= wide_occurrence_count - begin_size,
                "wide group range is prevalidated");
            return std::forward<Visitor>(visitor)(
                std::span<const wide_query_occurrence>{
                    wide_occurrences + begin_size, count_size},
                *occurrence_layout);
        }
        return minimizer_generation_encoding_error::partition_id;
    }

    template <bool Oracle>
    [[nodiscard, gnu::always_inline]] static std::atomic<std::uint32_t>&
    occurrence_counter_(
            const fragment_probe_context& context,
            std::uint32_t counter_ordinal) noexcept {
        if constexpr (Oracle) {
            DEBUG_ASSERT(context.scratch != nullptr
                    && counter_ordinal
                        < context.scratch->active_occurrence_count_,
                "oracle counter ordinal is prevalidated");
            return context.scratch->occurrence_counts_[counter_ordinal];
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr
                    && counter_ordinal
                        < context.bucket_direct_matcher->counter_count,
                "production counter ordinal is prevalidated");
            return context.bucket_direct_matcher->occurrence_counts[
                counter_ordinal];
        }
    }

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    add_expected_anchor_count_(
            query_occurrence_reference occurrence,
            std::span<const fragment_seed_query> queries,
            prepared_fragment_anchor_batch& batch,
            std::uint32_t global_count) noexcept {
        const std::uint32_t query_ordinal =
            occurrence.batch_query_ordinal;
        if (query_ordinal >= queries.size()
            || !add_checked_(
                batch.queries_[query_ordinal].anchor_count,
                global_count)) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return std::nullopt;
    }

    struct orientation_occurrence_context {
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        std::size_t layout_base = 0u;
        std::uint64_t target_strand_zero = 0u;
        std::uint64_t target_strand_one = 0u;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    accumulate_orientation_(
            query_occurrence_reference occurrence,
            const orientation_occurrence_context& context) noexcept {
        if (context.scratch == nullptr
            || occurrence.batch_query_ordinal >= context.queries.size()
            || occurrence.query_ordinal
                >= context.queries[occurrence.batch_query_ordinal].
                    minimizers.size()) {
            return minimizer_generation_encoding_error::partition_id;
        }
        const std::size_t layout_ordinal = context.layout_base
            + occurrence.batch_query_ordinal;
        if (layout_ordinal < context.layout_base
            || layout_ordinal
                >= context.scratch->block_query_layouts_.size()) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        block_query_anchor_layout& layout =
            context.scratch->block_query_layouts_[layout_ordinal];
        const std::uint32_t query_strand = static_cast<std::uint32_t>(
            context.queries[occurrence.batch_query_ordinal].
                minimizers[occurrence.query_ordinal].
                pos_strand_encoded()
            & 1u);
        const std::uint64_t forward = query_strand == 0u
            ? context.target_strand_zero
            : context.target_strand_one;
        const std::uint64_t reverse = query_strand == 0u
            ? context.target_strand_one
            : context.target_strand_zero;
        if (!add_checked_(layout.forward_count, forward)
            || !add_checked_(layout.reverse_count, reverse)) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return std::nullopt;
    }

    struct direct_emission_occurrence_context {
        std::span<const fragment_seed_query> queries{};
        const fragment_block_plan* block = nullptr;
        std::size_t block_ordinal = 0u;
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    emit_fragment_anchor_(
            std::uint64_t packed_target_position,
            query_occurrence_reference occurrence,
            const direct_emission_occurrence_context& context) noexcept {
        if (context.block == nullptr
            || context.scratch == nullptr
            || context.batch == nullptr
            || context.queries.empty()
            || occurrence.batch_query_ordinal >= context.queries.size()
            || occurrence.query_ordinal
                >= context.queries[occurrence.batch_query_ordinal].
                    minimizers.size()) {
            return minimizer_generation_encoding_error::partition_id;
        }
        const std::size_t query_ordinal =
            occurrence.batch_query_ordinal;
        const auto anchor = make_fragment_anchor_(
            packed_target_position,
            context.queries[query_ordinal].
                minimizers[occurrence.query_ordinal],
            context.queries[query_ordinal].query_length,
            static_cast<std::uint8_t>(
                context.queries[query_ordinal].kmer_size));
        if (!anchor
            || anchor->ref_position < context.block->reference_begin
            || anchor->ref_position >= context.block->reference_end) {
            return anchor
                ? minimizer_generation_encoding_error::position_range
                : anchor.error();
        }
        if (context.block_ordinal
                > (std::numeric_limits<std::size_t>::max)()
                    / context.queries.size()) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        const std::size_t layout_base =
            context.block_ordinal * context.queries.size();
        const std::size_t layout_ordinal =
            layout_base + query_ordinal;
        if (layout_ordinal < layout_base
            || layout_ordinal
                >= context.scratch->block_query_layouts_.size()) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        block_query_anchor_layout& layout =
            context.scratch->block_query_layouts_[layout_ordinal];
        std::uint64_t* cursor = anchor->is_reverse != 0u
            ? &layout.reverse_cursor : &layout.forward_cursor;
        const std::uint64_t slice_begin = anchor->is_reverse != 0u
            ? layout.reverse_begin : layout.forward_begin;
        const std::uint64_t slice_count = anchor->is_reverse != 0u
            ? layout.reverse_count : layout.forward_count;
        if (slice_begin
                > (std::numeric_limits<std::uint64_t>::max)()
                    - slice_count
            || *cursor < slice_begin
            || *cursor >= slice_begin + slice_count
            || *cursor
                >= context.batch->queries_[query_ordinal].anchor_count
            || context.batch->queries_[query_ordinal].anchors == nullptr) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        context.batch->queries_[query_ordinal].anchors[
            static_cast<std::size_t>(*cursor)] = *anchor;
        ++*cursor;
        return std::nullopt;
    }

    struct compact_direct_emission_occurrence_context final {
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::size_t layout_base = 0u;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    emit_compact_direct_anchor_(
            std::uint64_t packed_target_position,
            query_occurrence_reference occurrence,
            const compact_direct_emission_occurrence_context& context) noexcept {
        if (context.scratch == nullptr || context.batch == nullptr
            || occurrence.batch_query_ordinal >= context.queries.size()
            || occurrence.query_ordinal
                >= context.queries[occurrence.batch_query_ordinal].
                    minimizers.size()) {
            return minimizer_generation_encoding_error::partition_id;
        }
        const std::size_t query_ordinal = occurrence.batch_query_ordinal;
        const auto anchor = make_fragment_anchor_(
            packed_target_position,
            context.queries[query_ordinal].
                minimizers[occurrence.query_ordinal],
            context.queries[query_ordinal].query_length,
            static_cast<std::uint8_t>(
                context.queries[query_ordinal].kmer_size));
        if (!anchor
            || anchor->ref_position >= context.batch->target_length_) {
            return anchor
                ? minimizer_generation_encoding_error::position_range
                : anchor.error();
        }
        if (context.layout_base
                > context.scratch->block_query_layouts_.size()
            || query_ordinal
                >= context.scratch->block_query_layouts_.size()
                    - context.layout_base
            || query_ordinal >= context.batch->query_count_) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        block_query_anchor_layout& layout =
            context.scratch->block_query_layouts_[
                context.layout_base + query_ordinal];
        std::uint64_t* cursor = anchor->is_reverse != 0u
            ? &layout.reverse_cursor : &layout.forward_cursor;
        const std::uint64_t slice_begin = anchor->is_reverse != 0u
            ? layout.reverse_begin : layout.forward_begin;
        const std::uint64_t slice_count = anchor->is_reverse != 0u
            ? layout.reverse_count : layout.forward_count;
        if (slice_begin
                > (std::numeric_limits<std::uint64_t>::max)()
                    - slice_count
            || *cursor < slice_begin
            || *cursor >= slice_begin + slice_count
            || *cursor
                >= context.batch->queries_[query_ordinal].anchor_count
            || context.batch->queries_[query_ordinal].anchors == nullptr) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        context.batch->queries_[query_ordinal].anchors[
            static_cast<std::size_t>(*cursor)] = *anchor;
        ++*cursor;
        return std::nullopt;
    }

    template <class Context>
    [[nodiscard]] static bool bind_compact_direct_selection_(
            Context& context,
            const compact_hash32_selection_posting_spans& selected,
            query_occurrence_group& group) noexcept {
        if (context.error || context.reader == nullptr
            || context.plan == nullptr || context.scratch == nullptr
            || selected.selection_ordinal
                >= context.plan->selections.size()
            || selected.global_count == 0u
            || selected.strand_zero_count > selected.global_count
            || selected.span_count == 0u
            || selected.span_count > selected.spans.size()) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        const compact_hash32_unique_selection& selection =
            context.plan->selections[selected.selection_ordinal];
        if (selected.dense_ordinal != selection.dense_ordinal
            || selected.query_ordinal != selection.query_ordinal
            || selection.query_ordinal
                >= context.scratch->query_plans_.size()) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }

        std::uint64_t span_postings = 0u;
        std::uint64_t strand_zero_postings = 0u;
        bool have_previous_strand = false;
        bool previous_strand_one = false;
        for (std::uint8_t span_ordinal = 0u;
             span_ordinal < selected.span_count;
             ++span_ordinal) {
            const compact_hash32_posting_span_view& span =
                selected.spans[span_ordinal];
            if (span.count == 0u
                || span.posting_width_bytes
                    != context.plan->posting_width_bytes
                || span.target_length
                    != context.reader->manifest_.target_length
                || (have_previous_strand
                    && previous_strand_one >= span.target_strand_one)
                || !add_checked_(span_postings, span.count)
                || (!span.target_strand_one
                    && !add_checked_(strand_zero_postings, span.count))) {
                context.error = minimizer_generation_encoding_error::partition_id;
                return false;
            }
            previous_strand_one = span.target_strand_one;
            have_previous_strand = true;
        }
        if (span_postings != selected.global_count
            || strand_zero_postings != selected.strand_zero_count) {
            context.error = minimizer_generation_encoding_error::element_count;
            return false;
        }

        // Compact selections deliberately retain only dense and query
        // ordinals. The query-wave owner remains the single hash authority.
        const query_wave_scratch::query_hash_plan& query_plan =
            context.scratch->query_plans_[selection.query_ordinal];
        if (query_plan.occurrence_ref_count == 0u) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        group = query_occurrence_group{
            .hash = query_plan.hash,
            .occurrence_ref_begin = query_plan.occurrence_ref_begin,
            .occurrence_ref_count = query_plan.occurrence_ref_count,
            .counter_ordinal = selection.query_ordinal,
        };
        return true;
    }

    static void flush_compact_direct_selected_page_advice_(
            CompactDirectLayoutPassContext& context) noexcept {
        if (context.reader == nullptr) return;
        context.selected_page_advice.flush(
            context.reader->selected_page_adviser_);
    }

    [[nodiscard]] static bool
    accumulate_compact_direct_selected_page_advice_(
            CompactDirectLayoutPassContext& context,
            const compact_hash32_selection_posting_spans& selected) noexcept {
        if (context.reader == nullptr
            || !context.reader->compact_direct_) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        const compact_direct_selected_page_adviser adviser =
            context.reader->selected_page_adviser_;
        if (adviser.empty()) return true;

        for (std::uint8_t span_ordinal = 0u;
             span_ordinal < selected.span_count;
             ++span_ordinal) {
            const auto page_range = compact_direct_selected_page_range(
                adviser,
                context.reader->compact_direct_->view(),
                selected.spans[span_ordinal].encoded_bytes);
            if (!page_range) {
                context.direct_error = page_range.error();
                return false;
            }
            if (!*page_range) continue;

            if (!context.selected_page_advice.accumulate(
                    adviser, **page_range)) {
                context.direct_error =
                    direct_inverted_index_error::query_plan;
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool visit_compact_direct_layout_selection_(
            CompactDirectLayoutPassContext& context,
            const compact_hash32_selection_posting_spans& selected) noexcept {
        query_occurrence_group group{};
        if (!bind_compact_direct_selection_(context, selected, group)) {
            return false;
        }
        if (!accumulate_compact_direct_selected_page_advice_(
                context, selected)) {
            return false;
        }
        fragment_probe_context occurrence_context{
            .block_ordinal = context.shard_ordinal,
            .queries = context.queries,
            .scratch = context.scratch,
        };
        context.error = accumulate_orientation_for_group_<true>(
            occurrence_context,
            group,
            selected.strand_zero_count,
            selected.global_count - selected.strand_zero_count);
        if (context.error
            || !add_checked_(
                context.logical_postings_counted,
                selected.global_count)) {
            if (!context.error) {
                context.error =
                    minimizer_generation_encoding_error::arithmetic_overflow;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] static bool visit_compact_direct_emission_selection_(
            CompactDirectEmissionPassContext& context,
            const compact_hash32_selection_posting_spans& selected) noexcept {
        if (context.direct_error || context.batch == nullptr) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        query_occurrence_group group{};
        if (!bind_compact_direct_selection_(context, selected, group)) {
            return false;
        }
        const compact_direct_emission_occurrence_context emission_context{
            .queries = context.queries,
            .scratch = context.scratch,
            .batch = context.batch,
            .layout_base = context.shard_ordinal * context.queries.size(),
        };
        const fragment_probe_context occurrence_context{
            .block_ordinal = context.shard_ordinal,
            .queries = context.queries,
            .scratch = context.scratch,
            .batch = context.batch,
        };

        // Bind the packed/wide occurrence representation once per selected
        // hash. The fixed occurrence span then stays in registers while all of
        // that descriptor's postings are decoded and expanded into anchors.
        context.error = with_query_group_occurrences_<true>(
            occurrence_context,
            group,
            [&context, &selected, &emission_context, &group](
                    const auto occurrences,
                    const query_occurrence_packing_layout& packing_layout)
                    noexcept
                    -> std::optional<minimizer_generation_encoding_error> {
                if (occurrences.size() == 1u) {
                    const query_occurrence_reference occurrence =
                        query_occurrence_reference_(
                            occurrences.front(), packing_layout);
                    if (emission_context.scratch == nullptr
                        || emission_context.batch == nullptr
                        || occurrence.batch_query_ordinal
                            >= emission_context.queries.size()
                        || occurrence.query_ordinal
                            >= emission_context.queries[
                                occurrence.batch_query_ordinal].
                                    minimizers.size()) {
                        return minimizer_generation_encoding_error::partition_id;
                    }

                    const std::size_t query_ordinal =
                        occurrence.batch_query_ordinal;
                    if (emission_context.layout_base
                            > emission_context.scratch->
                                block_query_layouts_.size()
                        || query_ordinal
                            >= emission_context.scratch->
                                block_query_layouts_.size()
                                - emission_context.layout_base
                        || query_ordinal
                            >= emission_context.batch->query_count_) {
                        return minimizer_generation_encoding_error::
                            arithmetic_overflow;
                    }
                    const fragment_seed_query& query =
                        emission_context.queries[query_ordinal];
                    const minimap_sketch_data& minimizer =
                        query.minimizers[occurrence.query_ordinal];
                    prepared_query_anchors& output =
                        emission_context.batch->queries_[query_ordinal];
                    block_query_anchor_layout& layout =
                        emission_context.scratch->block_query_layouts_[
                            emission_context.layout_base + query_ordinal];

                    std::uint64_t selection_postings = 0u;
                    for (std::uint8_t span_ordinal = 0u;
                         span_ordinal < selected.span_count;
                         ++span_ordinal) {
                        const compact_hash32_posting_span_view& span =
                            selected.spans[span_ordinal];

                        // Target strand is invariant for one authoritative
                        // span. Build the query-dependent anchor fields once
                        // at reference position zero; the typed posting loop
                        // below supplies only the changing reference position.
                        const auto prepared_anchor = make_fragment_anchor_(
                            static_cast<std::uint64_t>(
                                span.target_strand_one),
                            minimizer,
                            query.query_length,
                            static_cast<std::uint8_t>(query.kmer_size));
                        if (!prepared_anchor) return prepared_anchor.error();
                        const bioseq::chaining::anchor anchor_template =
                            *prepared_anchor;

                        std::uint64_t* const cursor =
                            anchor_template.is_reverse != 0u
                            ? &layout.reverse_cursor
                            : &layout.forward_cursor;
                        const std::uint64_t slice_begin =
                            anchor_template.is_reverse != 0u
                            ? layout.reverse_begin
                            : layout.forward_begin;
                        const std::uint64_t slice_count =
                            anchor_template.is_reverse != 0u
                            ? layout.reverse_count
                            : layout.forward_count;
                        if (slice_begin
                                > (std::numeric_limits<std::uint64_t>::max)()
                                    - slice_count) {
                            return minimizer_generation_encoding_error::
                                arithmetic_overflow;
                        }
                        const std::uint64_t slice_end =
                            slice_begin + slice_count;
                        if (*cursor < slice_begin || *cursor > slice_end
                            || span.count > slice_end - *cursor
                            || *cursor > output.anchor_count
                            || span.count > output.anchor_count - *cursor
                            || output.anchors == nullptr) {
                            return minimizer_generation_encoding_error::
                                arithmetic_overflow;
                        }

                        const std::uint64_t initial_cursor = *cursor;
                        std::uint64_t emitted = 0u;
                        bioseq::chaining::anchor* destination =
                            output.anchors.get()
                            + static_cast<std::size_t>(initial_cursor);
                        bool have_previous_target_bp = false;
                        std::uint64_t previous_target_bp = 0u;
                        std::optional<minimizer_generation_encoding_error>
                            decode_visitor_error{};
                        const auto decoded_positions =
                            span.visit_encoded_positions(
                                [&anchor_template,
                                 &decode_visitor_error,
                                 &destination,
                                 &emission_context,
                                 &emitted,
                                 &have_previous_target_bp,
                                 &previous_target_bp](
                                        const std::uint64_t encoded_position)
                                        noexcept {
                                    const std::uint64_t target_bp =
                                        encoded_position >> 1u;
                                    if (have_previous_target_bp
                                        && target_bp < previous_target_bp) {
                                        decode_visitor_error =
                                            minimizer_generation_encoding_error::
                                                position_range;
                                        return false;
                                    }
                                    previous_target_bp = target_bp;
                                    have_previous_target_bp = true;
                                    if (target_bp
                                        >= emission_context.batch->
                                            target_length_) {
                                        decode_visitor_error =
                                            minimizer_generation_encoding_error::
                                                position_range;
                                        return false;
                                    }

                                    *destination = anchor_template;
                                    destination->ref_position = target_bp;
                                    ++destination;
                                    ++emitted;
                                    return true;
                                });

                        // The per-anchor path advances after every
                        // successful store. Commit the same partial progress
                        // before propagating a later decode/visitor error.
                        *cursor = initial_cursor + emitted;
                        if (!decoded_positions) {
                            if (decode_visitor_error) {
                                return decode_visitor_error;
                            }
                            context.direct_error =
                                decoded_positions.error();
                            return minimizer_generation_encoding_error::
                                position_range;
                        }
                        if (decode_visitor_error) {
                            return decode_visitor_error;
                        }
                        if (*decoded_positions != span.count
                            || emitted != span.count) {
                            return minimizer_generation_encoding_error::
                                element_count;
                        }
                        if (span.count
                                > (std::numeric_limits<std::uint64_t>::max)()
                                    / group.occurrence_ref_count
                            || span.count
                                > (std::numeric_limits<std::uint64_t>::max)()
                                    / span.posting_width_bytes
                            || !add_checked_(
                                context.anchors_constructed,
                                span.count * group.occurrence_ref_count)
                            || !add_checked_(
                                context.posting_bytes_read,
                                span.count * span.posting_width_bytes)
                            || !add_checked_(
                                context.postings_visited, span.count)
                            || !add_checked_(
                                selection_postings, span.count)) {
                            return minimizer_generation_encoding_error::
                                arithmetic_overflow;
                        }
                    }
                    if (selection_postings != selected.global_count) {
                        return std::optional<minimizer_generation_encoding_error>{
                            minimizer_generation_encoding_error::element_count};
                    }
                    return std::optional<minimizer_generation_encoding_error>{};
                }

                std::uint64_t selection_postings = 0u;
                for (std::uint8_t span_ordinal = 0u;
                     span_ordinal < selected.span_count;
                     ++span_ordinal) {
                    const compact_hash32_posting_span_view& span =
                        selected.spans[span_ordinal];
                    bool have_previous_target_bp = false;
                    std::uint64_t previous_target_bp = 0u;
                    std::optional<minimizer_generation_encoding_error>
                        decode_visitor_error{};
                    const auto decoded_positions =
                        span.visit_encoded_positions(
                            [&decode_visitor_error,
                             &have_previous_target_bp,
                             &previous_target_bp,
                             occurrences,
                             &packing_layout,
                             &emission_context](
                                    const std::uint64_t encoded_position)
                                    noexcept {
                                const std::uint64_t target_bp =
                                    encoded_position >> 1u;
                                if (have_previous_target_bp
                                    && target_bp < previous_target_bp) {
                                    decode_visitor_error =
                                        minimizer_generation_encoding_error::
                                            position_range;
                                    return false;
                                }
                                previous_target_bp = target_bp;
                                have_previous_target_bp = true;

                                for (const auto& occurrence : occurrences) {
                                    if (const auto error =
                                            emit_compact_direct_anchor_(
                                                encoded_position,
                                                query_occurrence_reference_(
                                                    occurrence,
                                                    packing_layout),
                                                emission_context)) {
                                        decode_visitor_error = error;
                                        return false;
                                    }
                                }
                                return true;
                            });
                    if (!decoded_positions) {
                        if (decode_visitor_error) {
                            return decode_visitor_error;
                        }
                        context.direct_error = decoded_positions.error();
                        return minimizer_generation_encoding_error::position_range;
                    }
                    if (decode_visitor_error) return decode_visitor_error;
                    if (*decoded_positions != span.count) {
                        return minimizer_generation_encoding_error::element_count;
                    }
                    if (span.count
                            > (std::numeric_limits<std::uint64_t>::max)()
                                / group.occurrence_ref_count
                        || span.count
                            > (std::numeric_limits<std::uint64_t>::max)()
                                / span.posting_width_bytes
                        || !add_checked_(
                            context.anchors_constructed,
                            span.count * group.occurrence_ref_count)
                        || !add_checked_(
                            context.posting_bytes_read,
                            span.count * span.posting_width_bytes)
                        || !add_checked_(
                            context.postings_visited, span.count)
                        || !add_checked_(selection_postings, span.count)) {
                        return minimizer_generation_encoding_error::
                            arithmetic_overflow;
                    }
                }
                if (selection_postings != selected.global_count) {
                    return std::optional<minimizer_generation_encoding_error>{
                        minimizer_generation_encoding_error::element_count};
                }
                return std::optional<minimizer_generation_encoding_error>{};
            });
        return !context.error && !context.direct_error;
    }

    /// Reserve one fragment-local occurrence count in a query-wave counter.
    /// A value of max_occ + 1 is the only rejected state. This bounds decoded
    /// candidates for a globally repetitive hash to at most max_occ while
    /// preserving every hit for hashes whose final target-wide count is
    /// accepted. The counter is query state, never persisted MPF index state.
    [[nodiscard]] static bool reserve_query_occurrences_(
            std::atomic<std::uint32_t>& global_count,
            std::uint64_t local_count,
            std::uint32_t max_occ) noexcept {
        const std::uint32_t rejected = max_occ + 1u;
        std::uint32_t current =
            global_count.load(std::memory_order_relaxed);
        for (;;) {
            if (current > max_occ) return false;
            if (local_count == 0u || local_count > max_occ - current) {
                if (global_count.compare_exchange_weak(
                        current,
                        rejected,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    return false;
                }
                continue;
            }
            if (global_count.compare_exchange_weak(
                    current,
                    current + static_cast<std::uint32_t>(local_count),
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    static void record_error_(
            std::atomic<std::uint32_t>& destination,
            minimizer_generation_encoding_error error) noexcept {
        std::uint32_t expected = 0u;
        const std::uint32_t encoded =
            static_cast<std::uint32_t>(error) + 1u;
        (void)destination.compare_exchange_strong(
            expected, encoded,
            std::memory_order_relaxed,
            std::memory_order_relaxed);
    }

    [[nodiscard]] static bool add_atomic_checked_(
            std::atomic<std::uint64_t>& destination,
            const std::uint64_t increment) noexcept {
        std::uint64_t current = destination.load(
            std::memory_order_relaxed);
        for (;;) {
            if (increment
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - current) {
                return false;
            }
            if (destination.compare_exchange_weak(
                    current,
                    current + increment,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    recorded_error_(
            const std::atomic<std::uint32_t>& source) noexcept {
        const std::uint32_t encoded =
            source.load(std::memory_order_relaxed);
        return encoded == 0u
            ? std::nullopt
            : std::optional<minimizer_generation_encoding_error>{
                static_cast<minimizer_generation_encoding_error>(encoded - 1u)};
    }

    template <class Body>
    static void parallel_entries_(std::size_t count, Body&& body) {
        bioseq::parallelism::parallel_for_pipeline(
            0u,
            count,
            1u,
            bioseq::parallelism::task_parallel_minimums::k_fragment_count,
            bioseq::parallelism::task_parallel_region::fragment_build,
            std::forward<Body>(body));
    }

    /// Execute exactly the first (most-significant) byte pass of the ordinary
    /// minimap-style u32 radix and return its 256 disjoint bucket extents.
    /// The returned state is constant-size stack data; no occurrence owner or
    /// per-query merge is introduced.  Lower-byte sorts may therefore run in
    /// any order without changing the serial radix's byte-for-byte result.
    [[nodiscard]] static std::array<std::size_t, 257u>
    partition_packed_occurrence_top_byte_(
            std::span<packed_query_occurrence> occurrences) noexcept {
        struct bucket {
            packed_query_occurrence* begin = nullptr;
            packed_query_occurrence* end = nullptr;
        };

        DEBUG_ASSERT(!occurrences.empty(),
            "packed occurrence top-byte partition requires payload");
        packed_query_occurrence* const payload_begin = occurrences.data();
        std::array<bucket, 256u> buckets{};
        for (bucket& bucket : buckets) {
            bucket.begin = payload_begin;
            bucket.end = payload_begin;
        }
        for (packed_query_occurrence& occurrence : occurrences) {
            ++buckets[occurrence.hash >> 24u].end;
        }
        for (std::size_t bucket = 1u; bucket < buckets.size(); ++bucket) {
            buckets[bucket].end +=
                buckets[bucket - 1u].end - payload_begin;
            buckets[bucket].begin = buckets[bucket - 1u].end;
        }

        for (std::size_t bucket_ordinal = 0u;
             bucket_ordinal < buckets.size();) {
            bucket& bucket = buckets[bucket_ordinal];
            if (bucket.begin == bucket.end) {
                ++bucket_ordinal;
                continue;
            }
            bucket* destination =
                &buckets[bucket.begin->hash >> 24u];
            if (destination == &bucket) {
                ++bucket.begin;
                continue;
            }
            packed_query_occurrence displaced = *bucket.begin;
            do {
                const packed_query_occurrence placed = displaced;
                displaced = *destination->begin;
                *destination->begin++ = placed;
                destination = &buckets[displaced.hash >> 24u];
            } while (destination != &bucket);
            *bucket.begin++ = displaced;
        }

        std::array<std::size_t, 257u> bounds{};
        for (std::size_t bucket = 0u; bucket < buckets.size(); ++bucket) {
            bounds[bucket + 1u] = static_cast<std::size_t>(
                buckets[bucket].end - payload_begin);
        }
        return bounds;
    }

    /// Count-only control scan over one open-validated entry plan. It
    /// deliberately does not load a prefix scalar or position byte; accepted
    /// plans receive their range validation in passes 2 and 3. All component
    /// offsets, widths, and byte products were proved once by open().
    template <class Visitor>
    [[nodiscard]] std::expected<void,
        minimizer_generation_encoding_error>
    visit_control_counts_planned_(
            std::size_t entry_ordinal,
            Visitor&& visitor) const noexcept {
        DEBUG_ASSERT(entry_ordinal < entry_count_,
            "block plan entry ordinal is open-validated");
        const grouped_soa_bucket_directory_entry& entry =
            directory_.entries()[entry_ordinal];
        const entry_plan& plan = entry_plans_[entry_ordinal];
        DEBUG_ASSERT(plan.control_count == entry.control_element_count
                && plan.position_element_count
                    == entry.position_element_count
                && plan.shape_offset_width == entry.shape_offset_width
                && plan.hash_begin <= plan.hash_end
                && plan.hash_end <= plan.control_end
                && plan.count_begin <= plan.count_end
                && plan.count_end <= plan.control_end
                && plan.control_end <= mapped_payload_.size(),
            "dynamic control scan consumes open-validated geometry");
        const std::size_t count = static_cast<std::size_t>(
            plan.control_count);

        std::uint64_t previous_hash = 0u;
        std::uint64_t first_hash = 0u;
        std::uint64_t last_hash = 0u;
        for (std::size_t index = 0u; index < count; ++index) {
            const std::uint64_t hash = read_shape_scalar_trusted_(
                mapped_payload_,
                static_cast<std::size_t>(plan.hash_begin)
                    + index * plan.hash_byte_width,
                plan.hash_width);
            const std::uint64_t saturated_count =
                read_shape_scalar_trusted_(
                    mapped_payload_,
                    static_cast<std::size_t>(plan.count_begin)
                        + index * plan.count_byte_width,
                    plan.count_width);
            if (hash > profile_.max_hash
                || (index != 0u && hash <= previous_hash)
                || saturated_count == 0u
                || saturated_count
                    > profile_.max_occ_cap + 1u) {
                return std::unexpected(
                    minimizer_generation_encoding_error::control_range);
            }
            if (index == 0u) first_hash = hash;
            last_hash = hash;
            std::invoke(visitor, hash, saturated_count);
            previous_hash = hash;
        }
        if (first_hash != entry.lo_hash
            || last_hash != entry.hi_hash) {
            return std::unexpected(
                minimizer_generation_encoding_error::control_range);
        }
        return {};
    }

    template <class Visitor>
    [[nodiscard]] static std::expected<void,
        minimizer_generation_encoding_error>
    visit_controls_(
            std::span<const std::byte> input,
            const grouped_soa_bucket_directory_entry& entry,
            const grouped_soa_minimizer_profile& profile,
            Visitor&& visitor) {
        using namespace minimizer_generation_wire_detail;
        if (profile.layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local
            || entry.control_encoded_bytes != input.size()
            || entry.control_element_count == 0u
            || entry.control_element_count
                > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::byte_count);
        }
        const auto layout = plan_grouped_soa_control_plane(
            entry.control_element_count,
            profile.hash_width,
            entry.shape_offset_width,
            profile.count_width);
        if (!layout || layout->encoded_bytes != input.size()) {
            return std::unexpected(
                minimizer_generation_encoding_error::byte_count);
        }
        const std::size_t count =
            static_cast<std::size_t>(entry.control_element_count);
        const std::size_t hash_width = static_cast<std::uint8_t>(
            profile.hash_width);
        const std::size_t offset_width = static_cast<std::uint8_t>(
            entry.shape_offset_width);
        const std::size_t count_width = static_cast<std::uint8_t>(
            profile.count_width);
        const std::size_t hash_end = count * hash_width;
        const std::size_t prefix_begin =
            static_cast<std::size_t>(layout->prefix_offset);
        const std::size_t prefix_end =
            prefix_begin + (count + 1u) * offset_width;
        const std::size_t count_begin =
            static_cast<std::size_t>(layout->count_offset);
        const std::size_t count_end =
            count_begin + count * count_width;
        if (!all_zero(input, hash_end, prefix_begin)
            || !all_zero(input, prefix_end, count_begin)
            || !all_zero(input, count_end, input.size())) {
            return std::unexpected(
                minimizer_generation_encoding_error::reserved_nonzero);
        }

        std::uint64_t previous_hash = 0u;
        std::uint64_t previous_prefix = 0u;
        std::uint64_t first_hash = 0u;
        std::uint64_t last_hash = 0u;
        for (std::size_t index = 0u; index < count; ++index) {
            const auto hash = read_shape_scalar(
                input,
                index * hash_width,
                profile.hash_width);
            const auto prefix = read_shape_scalar(
                input,
                prefix_begin + index * offset_width,
                entry.shape_offset_width);
            const auto next_prefix = read_shape_scalar(
                input,
                prefix_begin + (index + 1u) * offset_width,
                entry.shape_offset_width);
            const auto saturated_count = read_shape_scalar(
                input,
                count_begin + index * count_width,
                profile.count_width);
            if (!hash || !prefix || !next_prefix || !saturated_count
                || *hash > profile.max_hash
                || (index != 0u && *hash <= previous_hash)
                || (index == 0u ? *prefix != 0u
                                : *prefix <= previous_prefix)
                || *next_prefix <= *prefix
                || *next_prefix > entry.position_element_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::control_range);
            }
            const std::uint64_t delta = *next_prefix - *prefix;
            if (*saturated_count != std::min(
                    delta, profile.max_occ_cap + 1u)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::control_range);
            }
            if (index == 0u) first_hash = *hash;
            last_hash = *hash;
            std::invoke(
                visitor,
                grouped_soa_decoded_control{
                    .hash = *hash,
                    .position_begin = *prefix,
                    .position_end = *next_prefix,
                    .saturated_count = *saturated_count,
                });
            previous_hash = *hash;
            previous_prefix = *prefix;
        }
        const auto terminal_prefix = read_shape_scalar(
            input,
            prefix_begin + count * offset_width,
            entry.shape_offset_width);
        if (!terminal_prefix
            || *terminal_prefix != entry.position_element_count
            || first_hash != entry.lo_hash
            || last_hash != entry.hi_hash) {
            return std::unexpected(
                minimizer_generation_encoding_error::control_range);
        }
        return {};
    }

    [[nodiscard, gnu::always_inline]]
    static std::uint64_t read_shape_scalar_trusted_(
            std::span<const std::byte> input,
            std::size_t offset,
            minimizer_scalar_width width) noexcept {
        using namespace minimizer_generation_wire_detail;
        switch (width) {
            case minimizer_scalar_width::u16:
                return read_le<std::uint16_t>(input, offset);
            case minimizer_scalar_width::u32:
                return read_le<std::uint32_t>(input, offset);
            case minimizer_scalar_width::u64:
                return read_le<std::uint64_t>(input, offset);
        }
        std::unreachable();
    }

    template <class Visitor>
    [[nodiscard]] static std::expected<void,
        minimizer_generation_encoding_error>
    visit_position_range_(
            std::span<const std::byte> input,
            const grouped_soa_decoded_control& control,
            const grouped_soa_bucket_directory_entry& entry,
            const minimizer_position_segment_bounds& core,
            const grouped_soa_minimizer_profile& profile,
            Visitor&& visitor) noexcept {
        using namespace minimizer_generation_wire_detail;
        if (control.position_end <= control.position_begin
            || control.position_end > entry.position_element_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::control_range);
        }
        const std::size_t width = static_cast<std::uint8_t>(
            profile.position_width);
        if (width == 0u
            || entry.position_element_count
                > (std::numeric_limits<std::size_t>::max)() / width
            || input.size()
                != static_cast<std::size_t>(
                    entry.position_element_count) * width) {
            return std::unexpected(
                minimizer_generation_encoding_error::byte_count);
        }
        std::optional<std::uint64_t> previous;
        for (std::uint64_t ordinal = control.position_begin;
             ordinal < control.position_end;
             ++ordinal) {
            const std::uint64_t local = read_shape_scalar_trusted_(
                input,
                static_cast<std::size_t>(ordinal) * width,
                profile.position_width);
            const std::uint64_t local_bp = local >> 1u;
            if (local_bp >= core.end_bp - core.begin_bp
                || core.begin_bp
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - local_bp) {
                return std::unexpected(
                    minimizer_generation_encoding_error::position_range);
            }
            const std::uint64_t global_bp = core.begin_bp + local_bp;
            if (global_bp
                    > ((std::numeric_limits<std::uint64_t>::max)()
                        >> 1u)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t encoded =
                (global_bp << 1u) | (local & 1u);
            if (previous && encoded <= *previous) {
                return std::unexpected(
                    minimizer_generation_encoding_error::position_range);
            }
            std::invoke(visitor, encoded);
            previous = encoded;
        }
        return {};
    }

    [[nodiscard]] static bool add_checked_(
            std::uint64_t& destination,
            std::uint64_t value) noexcept {
        if (destination
                > (std::numeric_limits<std::uint64_t>::max)()
                    - value) {
            return false;
        }
        destination += value;
        return true;
    }

    /// Delegate both ordinary and fragment-native ordering to the same
    /// duplicate-linear authority and return exact payload-work receipts for
    /// independently timed primary and tie stages.
    [[nodiscard]] static bioseq::chaining::canonical_anchor_sort_stats
    sort_anchor_slice_primary_(
            bioseq::chaining::anchor* anchors,
            std::size_t anchor_count,
            std::uint64_t authenticated_target_length) noexcept {
        return bioseq::chaining::sort_canonical_anchor_primary_bounded(
            std::span<bioseq::chaining::anchor>{anchors, anchor_count},
            authenticated_target_length);
    }

    [[nodiscard]] static bioseq::chaining::canonical_anchor_sort_stats
    normalize_anchor_slice_ties_(
            bioseq::chaining::anchor* anchors,
            std::size_t anchor_count) noexcept {
        return bioseq::chaining::normalize_canonical_anchor_ties(
            std::span<bioseq::chaining::anchor>{anchors, anchor_count});
    }

    [[nodiscard]] static std::expected<
        bioseq::chaining::anchor,
        minimizer_generation_encoding_error>
    make_fragment_anchor_(
            std::uint64_t packed_target,
            const minimap_sketch_data& minimizer,
            sequence_size_t query_length,
            std::uint8_t query_span) noexcept {
        const std::uint64_t ref_position = packed_target >> 1u;
        const std::uint32_t target_strand =
            static_cast<std::uint32_t>(packed_target & 1u);
        const std::uint64_t encoded =
            minimizer.pos_strand_encoded();
        const std::uint32_t query_strand =
            static_cast<std::uint32_t>(encoded & 1u);
        const std::uint64_t forward_last = encoded >> 1u;
        if (ref_position >= (std::uint64_t{1} << 63u)
            || query_span == 0u
            || query_length < query_span
            || forward_last >= query_length
            || forward_last + 1u < query_span) {
            return std::unexpected(
                minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t kmer_start =
            forward_last + 1u - query_span;
        const std::uint64_t reverse_last =
            query_length - 1u - kmer_start;
        const std::uint32_t reverse =
            target_strand ^ query_strand;
        return bioseq::chaining::anchor{
            .ref_position = ref_position,
            .query_position =
                reverse != 0u ? reverse_last : forward_last,
            .is_reverse = reverse,
            .query_span = query_span,
            .flags = 0u,
            .segment_id = 0u,
        };
    }

    template <bool Oracle, class Visitor>
    [[nodiscard]] std::expected<void,
        minimizer_generation_encoding_error>
    visit_matched_control_counts_(
            const fragment_block_plan& block,
            fragment_probe_context& context,
            Visitor&& visitor) const noexcept {
        if constexpr (Oracle) {
            DEBUG_ASSERT(context.query_plan_radix.size()
                    == static_cast<std::size_t>(
                        query_radix_bucket_count_) + 1u,
                "oracle query radix is prevalidated");
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                "production matcher view is prevalidated");
        }
        DEBUG_ASSERT(block.entry_begin < block.entry_end
                && block.entry_end <= entry_count_,
            "dynamic count scan consumes an open-validated block");
        for (std::size_t ordinal = block.entry_begin;
             ordinal < block.entry_end;
            ++ordinal) {
            std::uint32_t active_bucket =
                (std::numeric_limits<std::uint32_t>::max)();
            std::uint32_t plan_cursor = 0u;
            std::uint32_t plan_end = 0u;
            std::optional<minimizer_generation_encoding_error>
                callback_error;
            const auto decoded = visit_control_counts_planned_(
                ordinal,
                [this, &callback_error, &active_bucket, &plan_cursor,
                 &plan_end, &context, &visitor](std::uint64_t hash,
                    std::uint64_t local_count) noexcept {
                    if (callback_error) return;
                    const std::uint32_t hash32 =
                        static_cast<std::uint32_t>(hash);
                    const auto matched_group = match_query_group_<Oracle>(
                        hash32, context, active_bucket,
                        plan_cursor, plan_end);
                    if (!matched_group) return;
                    callback_error = std::invoke(
                        visitor, *matched_group, local_count);
                });
            if (!decoded) return std::unexpected(decoded.error());
            if (callback_error) {
                return std::unexpected(*callback_error);
            }
        }
        return {};
    }

    template <bool Oracle, class Visitor>
    [[nodiscard]] std::expected<void,
        minimizer_generation_encoding_error>
    visit_matched_controls_(
            const fragment_block_plan& block,
            fragment_probe_context& context,
            Visitor&& visitor,
            std::uint64_t* accepted_prefix_bytes = nullptr,
            std::uint64_t* rejected_prefix_bytes = nullptr,
            bool validate_rejected_prefixes = true) const noexcept {
        using namespace minimizer_generation_wire_detail;
        if constexpr (Oracle) {
            DEBUG_ASSERT(context.query_plan_radix.size()
                    == static_cast<std::size_t>(
                        query_radix_bucket_count_) + 1u,
                "oracle query radix is prevalidated");
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                "production matcher view is prevalidated");
        }
        DEBUG_ASSERT(block.entry_begin < block.entry_end
                && block.entry_end <= entry_count_,
            "dynamic matched scan consumes an open-validated block");
        for (std::size_t ordinal = block.entry_begin;
             ordinal < block.entry_end;
            ++ordinal) {
            const entry_plan& entry_plan = entry_plans_[ordinal];
            const std::size_t offset_width =
                entry_plan.offset_byte_width;
            std::uint32_t active_bucket =
                (std::numeric_limits<std::uint32_t>::max)();
            std::uint32_t plan_cursor = 0u;
            std::uint32_t plan_end = 0u;
            std::optional<minimizer_generation_encoding_error>
                callback_error;
            std::optional<std::uint64_t>
                previous_accepted_position_end;
            std::size_t control_ordinal = 0u;
            const auto decoded = visit_control_counts_planned_(
                ordinal,
                [this, &control_ordinal, &callback_error, &active_bucket,
                 &plan_cursor, &plan_end, &context,
                 offset_width, &entry_plan,
                 &previous_accepted_position_end, &visitor,
                 accepted_prefix_bytes, rejected_prefix_bytes,
                 validate_rejected_prefixes,
                 ordinal](std::uint64_t hash,
                    std::uint64_t saturated_count) noexcept {
                    const std::size_t current_ordinal =
                        control_ordinal++;
                    if (callback_error) return;
                    const std::uint32_t hash32 =
                        static_cast<std::uint32_t>(hash);
                    const auto matched_group = match_query_group_<Oracle>(
                        hash32, context, active_bucket,
                        plan_cursor, plan_end);
                    if (!matched_group) return;
                    const std::uint32_t global_count =
                        occurrence_counter_<Oracle>(
                            context,
                            matched_group->counter_ordinal).load(
                            std::memory_order_relaxed);
                    if (global_count == 0u) {
                        callback_error =
                            minimizer_generation_encoding_error::partition_id;
                        return;
                    }
                    // Pass 3 suppresses a rejected control before either O
                    // endpoint is read. Rejected endpoints were authenticated
                    // exactly once by Pass 2 (or the rejected-only rescan).
                    if (global_count > context.max_occ
                        && !validate_rejected_prefixes) {
                        return;
                    }
                    std::uint64_t* const prefix_stats =
                        global_count > context.max_occ
                        ? rejected_prefix_bytes
                        : accepted_prefix_bytes;
                    if (prefix_stats != nullptr
                        && !add_checked_(
                            *prefix_stats,
                            2u * static_cast<std::uint64_t>(
                                offset_width))) {
                        callback_error =
                            minimizer_generation_encoding_error::
                                arithmetic_overflow;
                        return;
                    }
                    const std::size_t prefix_offset =
                        static_cast<std::size_t>(
                            entry_plan.prefix_begin)
                        + current_ordinal * offset_width;
                    const std::uint64_t position_begin =
                        read_shape_scalar_trusted_(
                            mapped_payload_,
                            prefix_offset,
                            entry_plan.shape_offset_width);
                    const std::uint64_t position_end =
                        read_shape_scalar_trusted_(
                            mapped_payload_,
                            prefix_offset + offset_width,
                            entry_plan.shape_offset_width);
                    if (position_end <= position_begin
                        || position_end
                            > entry_plan.position_element_count
                        || (current_ordinal == 0u
                            && position_begin != 0u)
                        || (current_ordinal + 1u
                                == entry_plan.control_count
                            && position_end
                                != entry_plan.position_element_count)
                        || (previous_accepted_position_end
                            && position_begin
                                < *previous_accepted_position_end)) {
                        callback_error =
                            minimizer_generation_encoding_error::
                                control_range;
                        return;
                    }
                    previous_accepted_position_end = position_end;
                    const std::uint64_t delta =
                        position_end - position_begin;
                    if (saturated_count != std::min(
                            delta,
                            profile_.
                                max_occ_cap + 1u)) {
                        callback_error =
                            minimizer_generation_encoding_error::
                                control_range;
                        return;
                    }
                    // A globally rejected query hash still names a selected
                    // control. Authenticate its prefix geometry and saturated
                    // count before suppressing the position walk; otherwise a
                    // corrupt heavy control could hide behind max-occ pruning.
                    if (global_count > context.max_occ) {
                        return;
                    }
                    callback_error = std::invoke(
                        visitor,
                        ordinal,
                        *matched_group,
                        grouped_soa_decoded_control{
                            .hash = hash,
                            .position_begin = position_begin,
                            .position_end = position_end,
                            .saturated_count = saturated_count,
                        });
                });
            if (!decoded) return std::unexpected(decoded.error());
            if (callback_error) {
                return std::unexpected(*callback_error);
            }
        }
        return {};
    }

    template <bool Oracle>
    [[nodiscard, gnu::always_inline]]
    std::optional<query_occurrence_group>
    match_query_group_(
            std::uint32_t hash,
            const fragment_probe_context& context,
            std::uint32_t& active_bucket,
            std::uint32_t& plan_cursor,
            std::uint32_t& plan_end) const noexcept {
        const std::uint32_t bucket = query_bucket_(hash);
        if constexpr (Oracle) {
            DEBUG_ASSERT(context.scratch != nullptr
                    && context.query_plan_radix.size()
                        == static_cast<std::size_t>(
                            query_radix_bucket_count_) + 1u,
                "oracle matcher owners are prevalidated");
            if (bucket != active_bucket) {
                active_bucket = bucket;
                plan_cursor = context.query_plan_radix[bucket];
                plan_end = context.query_plan_radix[bucket + 1u];
            }
            DEBUG_ASSERT(plan_cursor <= plan_end
                    && plan_end <= context.plans.size(),
                "oracle matcher bucket range is prevalidated");
            while (plan_cursor < plan_end
                && context.plans[plan_cursor].hash < hash) {
                ++plan_cursor;
            }
            if (plan_cursor == plan_end
                || context.plans[plan_cursor].hash != hash) {
                return std::nullopt;
            }
            const std::uint32_t matched_plan = plan_cursor++;
            const query_wave_scratch::query_hash_plan& plan =
                context.plans[matched_plan];
            return query_occurrence_group{
                .hash = plan.hash,
                .occurrence_ref_begin = plan.occurrence_ref_begin,
                .occurrence_ref_count = plan.occurrence_ref_count,
                .counter_ordinal = matched_plan,
            };
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                "production matcher view is prevalidated");
            const bucket_direct_matcher_view& view =
                *context.bucket_direct_matcher;
            const std::size_t filter_word =
                query_membership_filter_word_(hash);
            DEBUG_ASSERT(filter_word < view.membership_filter_word_count,
                "production filter projection is prevalidated");
            const std::uint64_t required_mask =
                query_membership_filter_mask_(hash);
            const std::uint64_t present_mask =
                view.membership_filter[filter_word];
            if ((present_mask & required_mask) != required_mask) {
                return std::nullopt;
            }

            DEBUG_ASSERT(static_cast<std::size_t>(bucket) + 1u
                    < view.radix_count,
                "production radix projection is prevalidated");
            const std::uint32_t group_begin = view.radix[bucket];
            const std::uint32_t group_end = view.radix[bucket + 1u];
            DEBUG_ASSERT(group_begin <= group_end
                    && static_cast<std::size_t>(group_end) + 1u
                        <= view.group_offset_count,
                "production group directory is prevalidated");
            const auto exact_group = [&view, hash, group_begin, group_end](
                    const auto* occurrences,
                    std::size_t occurrence_count)
                    noexcept -> std::optional<query_occurrence_group> {
                const std::uint32_t* const first =
                    view.group_offsets + group_begin;
                const std::uint32_t* const last =
                    view.group_offsets + group_end;
                const auto found = std::lower_bound(
                    first, last, hash,
                    [occurrences, occurrence_count](
                        std::uint32_t occurrence_ordinal,
                        std::uint32_t query_hash) noexcept {
                        DEBUG_ASSERT(occurrence_ordinal < occurrence_count,
                            "production group offset is prevalidated");
                        return occurrences[occurrence_ordinal].hash
                            < query_hash;
                    });
                if (found == last || occurrences[*found].hash != hash) {
                    return std::nullopt;
                }
                const std::uint32_t occurrence_begin = *found;
                const std::uint32_t occurrence_end = *(found + 1u);
                DEBUG_ASSERT(occurrence_end > occurrence_begin
                        && occurrence_end <= occurrence_count,
                    "production group extent is prevalidated");
                return query_occurrence_group{
                    .hash = hash,
                    .occurrence_ref_begin = occurrence_begin,
                    .occurrence_ref_count =
                        occurrence_end - occurrence_begin,
                    .counter_ordinal =
                        static_cast<std::uint32_t>(
                            found - view.group_offsets),
                };
            };
            switch (view.occurrence_layout.route) {
            case query_occurrence_route::packed:
                return exact_group(
                    view.packed_occurrences,
                    view.packed_occurrence_count);
            case query_occurrence_route::wide:
                return exact_group(
                    view.wide_occurrences,
                    view.wide_occurrence_count);
            }
            return std::nullopt;
        }
    }

    template <bool Oracle>
    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    accumulate_orientation_for_group_(
            const fragment_probe_context& context,
            const query_occurrence_group& group,
            std::uint64_t target_strand_zero,
            std::uint64_t target_strand_one) noexcept {
        DEBUG_ASSERT(context.scratch != nullptr
                && !context.queries.empty()
                && context.block_ordinal
                    <= (std::numeric_limits<std::size_t>::max)()
                        / context.queries.size(),
            "orientation occurrence context is prevalidated");
        const orientation_occurrence_context occurrence_context{
            .queries = context.queries,
            .scratch = context.scratch,
            .layout_base = context.block_ordinal
                * context.queries.size(),
            .target_strand_zero = target_strand_zero,
            .target_strand_one = target_strand_one,
        };
        return with_query_group_occurrences_<Oracle>(
            context,
            group,
            [&occurrence_context](
                    const auto occurrences,
                    const query_occurrence_packing_layout& packing_layout)
                    noexcept
                    -> std::optional<minimizer_generation_encoding_error> {
                for (const auto& occurrence : occurrences) {
                    if (const auto error = accumulate_orientation_(
                            query_occurrence_reference_(
                                occurrence, packing_layout),
                            occurrence_context)) {
                        return error;
                    }
                }
                return std::nullopt;
            });
    }

    template <bool Oracle>
    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    process_fixed_orientation_control_(
            fragment_probe_context& context,
            const entry_plan& entry,
            const query_occurrence_group& group,
            const grouped_soa_decoded_control& control) const noexcept {
        DEBUG_ASSERT(context.position_bytes != nullptr
                && entry.position_width == minimizer_scalar_width::u16,
            "fixed orientation context is prevalidated");
        auto cursor = position_range_cursor::create(
            mapped_payload_, control, entry);
        if (!cursor) return cursor.error();
        std::uint64_t target_strand_zero = 0u;
        std::uint64_t target_strand_one = 0u;
        std::uint64_t packed = 0u;
        while (cursor->next_fixed_p16(packed)) {
            std::uint64_t& count = (packed & 1u) == 0u
                ? target_strand_zero : target_strand_one;
            if (count == (std::numeric_limits<std::uint64_t>::max)()) {
                return minimizer_generation_encoding_error::arithmetic_overflow;
            }
            ++count;
        }
        if (const auto error = cursor->error()) return error;
        std::uint64_t observed = target_strand_zero;
        if (!add_checked_(observed, target_strand_one)
            || observed != control.position_end - control.position_begin
            || !add_checked_(
                *context.position_bytes,
                observed * sizeof(std::uint16_t))) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return accumulate_orientation_for_group_<Oracle>(
            context,
            group,
            target_strand_zero,
            target_strand_one);
    }

    template <bool Oracle>
    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    process_fixed_emission_control_(
            fragment_probe_context& context,
            const entry_plan& entry,
            const query_occurrence_group& group,
            const grouped_soa_decoded_control& control) const noexcept {
        DEBUG_ASSERT(context.scratch != nullptr
                && context.batch != nullptr
                && context.block != nullptr
                && context.position_bytes != nullptr
                && context.anchors_constructed != nullptr
                && entry.position_width == minimizer_scalar_width::u16,
            "fixed emission context is prevalidated");
        auto cursor = position_range_cursor::create(
            mapped_payload_, control, entry);
        if (!cursor) return cursor.error();
        const direct_emission_occurrence_context occurrence_context{
            .queries = context.queries,
            .block = context.block,
            .block_ordinal = context.block_ordinal,
            .scratch = context.scratch,
            .batch = context.batch,
        };

        const auto occurrence_error = with_query_group_occurrences_<Oracle>(
            context,
            group,
            [this, &cursor, &occurrence_context](
                    const auto occurrences,
                    const query_occurrence_packing_layout& packing_layout)
                    noexcept
                    -> std::optional<minimizer_generation_encoding_error> {
                std::uint64_t packed = 0u;
                while (cursor->next_fixed_p16(packed)) {
                    for (const auto& occurrence : occurrences) {
                        if (const auto error = emit_fragment_anchor_(
                                packed,
                                query_occurrence_reference_(
                                    occurrence, packing_layout),
                                occurrence_context)) {
                            return error;
                        }
                    }
                }
                return cursor->error();
            });
        if (occurrence_error) return occurrence_error;

        const std::uint64_t decoded_positions =
            control.position_end - control.position_begin;
        const std::uint64_t position_bytes =
            bioseq::common::saturating_mul(
                decoded_positions, sizeof(std::uint16_t));
        const std::uint64_t constructed =
            bioseq::common::saturating_mul(
                decoded_positions, group.occurrence_ref_count);
        if (position_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || constructed
                == (std::numeric_limits<std::uint64_t>::max)()
            || !add_checked_(*context.position_bytes, position_bytes)
            || !add_checked_(
                *context.anchors_constructed, constructed)) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return std::nullopt;
    }

    template <bool Oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_count_(
            fragment_probe_context& context,
            std::size_t entry_ordinal) const noexcept {
        DEBUG_ASSERT(context.scratch != nullptr
                && entry_ordinal < entry_count_
                && context.pass == fragment_probe_pass::control_count,
            "fixed count context is prevalidated");
        if constexpr (Oracle) {
            DEBUG_ASSERT(context.query_plan_radix.size()
                    == static_cast<std::size_t>(
                        query_radix_bucket_count_) + 1u,
                "oracle query radix is prevalidated");
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                "production matcher view is prevalidated");
        }
        const grouped_soa_bucket_directory_entry& directory_entry =
            directory_.entries()[entry_ordinal];
        const entry_plan& entry = entry_plans_[entry_ordinal];
        DEBUG_ASSERT(entry.hash_width == minimizer_scalar_width::u32
                && entry.count_width == minimizer_scalar_width::u16
                && entry.position_width == minimizer_scalar_width::u16
                && entry.control_count
                    == directory_entry.control_element_count
                && entry.position_element_count
                    == directory_entry.position_element_count
                && entry.control_begin <= entry.hash_begin
                && entry.hash_begin <= entry.hash_end
                && entry.hash_end <= entry.prefix_begin
                && entry.prefix_begin <= entry.prefix_end
                && entry.prefix_end <= entry.count_begin
                && entry.count_begin <= entry.count_end
                && entry.count_end <= entry.control_end
                && entry.control_end <= mapped_payload_.size(),
            "fixed probe consumes open-validated entry geometry");

        std::uint32_t active_bucket =
            (std::numeric_limits<std::uint32_t>::max)();
        std::uint32_t plan_cursor = 0u;
        std::uint32_t plan_end = 0u;
        std::uint32_t previous_hash = 0u;
        std::uint32_t first_hash = 0u;
        std::uint32_t last_hash = 0u;
        const std::size_t count = static_cast<std::size_t>(
            entry.control_count);
        for (std::size_t control_ordinal = 0u;
             control_ordinal < count;
             ++control_ordinal) {
            const std::uint32_t hash = static_cast<std::uint32_t>(
                read_fixed_width_le_<minimizer_scalar_width::u32>(
                    mapped_payload_,
                    static_cast<std::size_t>(entry.hash_begin)
                        + control_ordinal * sizeof(std::uint32_t)));
            const std::uint64_t local_count =
                read_fixed_width_le_<minimizer_scalar_width::u16>(
                    mapped_payload_,
                    static_cast<std::size_t>(entry.count_begin)
                        + control_ordinal * sizeof(std::uint16_t));
            if (hash > profile_.max_hash
                || (control_ordinal != 0u && hash <= previous_hash)
                || local_count == 0u
                || local_count > profile_.max_occ_cap + 1u) {
                return std::unexpected(
                    minimizer_generation_encoding_error::control_range);
            }
            if (control_ordinal == 0u) first_hash = hash;
            last_hash = hash;
            previous_hash = hash;

            const auto matched_group = match_query_group_<Oracle>(
                hash, context, active_bucket, plan_cursor, plan_end);
            if (!matched_group) continue;
            (void)reserve_query_occurrences_(
                occurrence_counter_<Oracle>(
                    context, matched_group->counter_ordinal),
                local_count,
                context.max_occ);
        }
        if (first_hash != directory_entry.lo_hash
            || last_hash != directory_entry.hi_hash) {
            return std::unexpected(
                minimizer_generation_encoding_error::control_range);
        }
        return {};
    }

    template <bool Oracle, minimizer_scalar_width O>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_p16_matched_(
            fragment_probe_context& context,
            std::size_t entry_ordinal) const noexcept {
        static_assert(
            O == minimizer_scalar_width::u16
            || O == minimizer_scalar_width::u32
            || O == minimizer_scalar_width::u64);
        DEBUG_ASSERT(context.scratch != nullptr
                && entry_ordinal < entry_count_
                && context.pass != fragment_probe_pass::control_count
                && context.matched_count_bytes != nullptr,
            "fixed matched context is prevalidated");
        if constexpr (Oracle) {
            DEBUG_ASSERT(context.query_plan_radix.size()
                    == static_cast<std::size_t>(
                        query_radix_bucket_count_) + 1u,
                "oracle query radix is prevalidated");
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                "production matcher view is prevalidated");
        }
        const entry_plan& entry = entry_plans_[entry_ordinal];
        DEBUG_ASSERT(entry.shape_offset_width == O
                && entry.hash_width == minimizer_scalar_width::u32
                && entry.count_width == minimizer_scalar_width::u16
                && entry.position_width == minimizer_scalar_width::u16
                && entry.control_count
                    == directory_.entries()[entry_ordinal].
                        control_element_count
                && entry.position_element_count
                    == directory_.entries()[entry_ordinal].
                        position_element_count
                && entry.control_begin <= entry.hash_begin
                && entry.hash_begin <= entry.hash_end
                && entry.hash_end <= entry.prefix_begin
                && entry.prefix_begin <= entry.prefix_end
                && entry.prefix_end <= entry.count_begin
                && entry.count_begin <= entry.count_end
                && entry.count_end <= entry.control_end
                && entry.control_end <= mapped_payload_.size(),
            "fixed matched probe consumes Pass1-authenticated controls");

        std::uint32_t active_bucket =
            (std::numeric_limits<std::uint32_t>::max)();
        std::uint32_t plan_cursor = 0u;
        std::uint32_t plan_end = 0u;
        std::optional<std::uint64_t> previous_selected_position_end;
        const std::size_t count = static_cast<std::size_t>(
            entry.control_count);
        for (std::size_t control_ordinal = 0u;
             control_ordinal < count;
             ++control_ordinal) {
            // Pass 1 has already authenticated H ordering/range and every C
            // value under a joined barrier. Later mandatory passes scan H for
            // matching, then touch C only for a control they will consume.
            const std::uint32_t hash = static_cast<std::uint32_t>(
                read_fixed_width_le_<minimizer_scalar_width::u32>(
                    mapped_payload_,
                    static_cast<std::size_t>(entry.hash_begin)
                        + control_ordinal * sizeof(std::uint32_t)));
            const auto matched_group = match_query_group_<Oracle>(
                hash, context, active_bucket, plan_cursor, plan_end);
            if (!matched_group) continue;
            const std::uint32_t global_count =
                occurrence_counter_<Oracle>(
                    context, matched_group->counter_ordinal).load(
                    std::memory_order_relaxed);
            if (global_count == 0u) {
                return std::unexpected(
                    minimizer_generation_encoding_error::partition_id);
            }
            if (global_count > context.max_occ
                && context.pass == fragment_probe_pass::emission) {
                continue;
            }
            if (!add_checked_(
                    *context.matched_count_bytes,
                    sizeof(std::uint16_t))) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t local_count =
                read_fixed_width_le_<minimizer_scalar_width::u16>(
                    mapped_payload_,
                    static_cast<std::size_t>(entry.count_begin)
                        + control_ordinal * sizeof(std::uint16_t));
            std::uint64_t* const prefix_stats =
                global_count > context.max_occ
                ? context.rejected_prefix_bytes
                : context.accepted_prefix_bytes;
            if (prefix_stats != nullptr
                && !add_checked_(
                    *prefix_stats,
                    2u * sizeof(minimizer_wire_uint_t_<O>))) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::size_t prefix_offset =
                static_cast<std::size_t>(entry.prefix_begin)
                + control_ordinal * sizeof(minimizer_wire_uint_t_<O>);
            const std::uint64_t position_begin =
                read_fixed_width_le_<O>(mapped_payload_, prefix_offset);
            const std::uint64_t position_end =
                read_fixed_width_le_<O>(
                    mapped_payload_,
                    prefix_offset + sizeof(minimizer_wire_uint_t_<O>));
            if (position_end <= position_begin
                || position_end > entry.position_element_count
                || (control_ordinal == 0u && position_begin != 0u)
                || (control_ordinal + 1u == count
                    && position_end != entry.position_element_count)
                || (previous_selected_position_end
                    && position_begin < *previous_selected_position_end)
                || local_count != std::min(
                    position_end - position_begin,
                    profile_.max_occ_cap + 1u)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::control_range);
            }
            previous_selected_position_end = position_end;

            if (global_count > context.max_occ) {
                if (context.pass ==
                        fragment_probe_pass::rejected_validation
                    || context.pass == fragment_probe_pass::orientation) {
                    continue;
                }
                return std::unexpected(
                    minimizer_generation_encoding_error::partition_id);
            }
            if (context.pass ==
                    fragment_probe_pass::rejected_validation) {
                return std::unexpected(
                    minimizer_generation_encoding_error::partition_id);
            }
            const grouped_soa_decoded_control control{
                .hash = hash,
                .position_begin = position_begin,
                .position_end = position_end,
                .saturated_count = local_count,
            };
            const auto error = context.pass == fragment_probe_pass::orientation
                ? process_fixed_orientation_control_<Oracle>(
                    context, entry, *matched_group, control)
                : process_fixed_emission_control_<Oracle>(
                    context, entry, *matched_group, control);
            if (error) return std::unexpected(*error);
        }
        return {};
    }

    template <bool Oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_p16_dispatch_(
            fragment_probe_context& context,
            std::size_t entry_ordinal) const noexcept {
        if (context.pass == fragment_probe_pass::control_count) {
            return probe_entry_h32_c16_count_<Oracle>(
                context, entry_ordinal);
        }
        switch (entry_plans_[entry_ordinal].shape_offset_width) {
        case minimizer_scalar_width::u16:
            return probe_entry_h32_c16_p16_matched_<
                Oracle, minimizer_scalar_width::u16>(context, entry_ordinal);
        case minimizer_scalar_width::u32:
            return probe_entry_h32_c16_p16_matched_<
                Oracle, minimizer_scalar_width::u32>(context, entry_ordinal);
        case minimizer_scalar_width::u64:
            return probe_entry_h32_c16_p16_matched_<
                Oracle, minimizer_scalar_width::u64>(context, entry_ordinal);
        }
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }

    template <bool Oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_block_h32_c16_p16_(
            fragment_probe_context& context) const noexcept {
        if (context.block == nullptr
            || context.block->entry_begin >= context.block->entry_end
            || context.block->entry_end > entry_count_) {
            return std::unexpected(
                minimizer_generation_encoding_error::directory_size);
        }
        for (std::size_t entry_ordinal = context.block->entry_begin;
             entry_ordinal < context.block->entry_end;
             ++entry_ordinal) {
            const auto probed = probe_entry_h32_c16_p16_dispatch_<Oracle>(
                context, entry_ordinal);
            if (!probed) return probed;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t final_owned_bytes_() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = directory_.owned_capacity_bytes();
        bytes = saturating_add(bytes, saturating_mul(
            entry_count_, sizeof(entry_plan)));
        bytes = saturating_add(bytes, saturating_mul(
            block_count_, sizeof(fragment_block_plan)));
        return bytes;
    }

    [[nodiscard]] static std::expected<
        direct_open_contract,
        minimizer_generation_encoding_error>
    validate_direct_open_contract_(
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const expected_minimizer_generation_identity& expected,
            std::uint64_t max_occ,
            const owned_compact_hash32_direct_index_view& compact_direct) {
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || manifest.layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local
            || head.layout != manifest.layout
            || expected.layout != manifest.layout
            || !expected.max_occ_cap
            || *expected.max_occ_cap != profile->max_occ_cap
            || max_occ > profile->max_occ_cap
            || profile->max_hash
                > (std::numeric_limits<std::uint32_t>::max)()
            || max_occ > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int32_t>::max)())) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        if (auto compatible = validate_minimizer_generation_compatibility(
                manifest, expected); !compatible) {
            return std::unexpected(compatible.error());
        }

        const std::uint32_t profile_max_hash =
            static_cast<std::uint32_t>(profile->max_hash);
        const std::uint32_t profile_hash_bits = std::bit_width(
            profile_max_hash);
        const std::uint64_t expected_profile_max_hash =
            profile_hash_bits == 0u
            ? 0u
            : (std::uint64_t{1} << profile_hash_bits) - 1u;
        if (profile_hash_bits == 0u
            || profile_hash_bits
                > (std::numeric_limits<std::uint8_t>::max)()
            || profile->max_hash != expected_profile_max_hash) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }

        if (head.generation_id != manifest.generation_id
            || head.source_identity != manifest.source_identity
            || head.source_content_checksum
                != manifest.source_content_checksum) {
            return std::unexpected(
                minimizer_generation_encoding_error::identity_mismatch);
        }
        const auto manifest_bytes =
            encode_minimizer_generation_manifest_record(manifest);
        if (!manifest_bytes) {
            return std::unexpected(manifest_bytes.error());
        }
        if (head.manifest_checksum != crc64_ecma182(
                std::span<const std::byte>{*manifest_bytes})) {
            return std::unexpected(
                minimizer_generation_encoding_error::checksum);
        }
        if (manifest.gpu_abi_revision != profile->gpu_abi_revision
            || head.compatibility_checksum
                != minimizer_generation_compatibility_checksum(manifest)) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }

        const compact_hash32_header& direct_header =
            compact_direct.view_.header;
        const direct_index_identity expected_identity{
            .generation_id = head.generation_id,
            .source_identity = head.source_identity,
            .compatibility_checksum = head.compatibility_checksum,
            .target_length = manifest.target_length,
        };
        if (direct_header.identity != expected_identity
            || direct_header.max_occ_cap
                != profile->max_occ_cap
            || direct_header.geometry.hash_domain_bits
                != profile_hash_bits
            || direct_header.geometry.max_hash != profile_max_hash
            || direct_header.image_bytes
                != compact_direct.view_.image.size()
            || compact_direct.view_.image.empty()
            || compact_direct.image_owner_.get()
                != compact_direct.view_.image.data()) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        return direct_open_contract{
            .profile = *profile,
            .hash_domain_bits = static_cast<std::uint8_t>(
                profile_hash_bits),
        };
    }

    void prepare_compact_direct_anchors_(
            bioseq::parallelism::memory_admission_controller& admission,
            std::span<const fragment_seed_query> queries,
            std::uint32_t runtime_max_occ,
            query_wave_scratch& scratch,
            prepared_fragment_anchor_batch& batch) const {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (!compact_direct_) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 direct query contract is inconsistent");
        }
        const std::size_t direct_query_plan_count =
            scratch.query_plans_.size();

        batch.stats_.compact_direct_waves = 1u;
        batch.stats_.compact_direct_unique_query_groups =
            direct_query_plan_count;
        const std::uint64_t maximum_plan_bytes = saturating_mul(
            saturating_mul(
                direct_query_plan_count,
                sizeof(compact_hash32_unique_selection)),
            2u);
        if (maximum_plan_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 accepted-plan admission size overflow");
        }
        auto direct_plan_permit = admission.try_reserve(
            maximum_plan_bytes);
        if (!direct_plan_permit) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::
                    admission_or_allocation,
                "compact-H32 accepted-plan admission failed");
        }

        const std::uint64_t lookup_started_ns = stage_now_ns_();
        auto queried = query_direct_compact_hash_lookup_index_unique_projected(
            compact_direct_->view_,
            direct_query_plan_count,
            [&query_plans = scratch.query_plans_](
                    std::size_t ordinal) noexcept -> std::uint32_t {
                return query_plans[ordinal].hash;
            },
            runtime_max_occ);
        if (!queried) {
            if (queried.error() == direct_inverted_index_error::admission
                || queried.error()
                    == direct_inverted_index_error::allocation) {
                throw persisted_minimizer_lookup_error(
                    persisted_minimizer_lookup_error::reason::
                        admission_or_allocation,
                    "compact-H32 unique lookup allocation/admission "
                    "failed");
            }
            throw persisted_minimizer_lookup_error(
                "compact-H32 unique lookup failed; direct_error="
                + std::to_string(static_cast<unsigned>(queried.error())));
        }
        compact_hash32_unique_query_plan plan = std::move(*queried);
        const std::uint64_t retained_plan_bytes =
            plan.retained_selection_capacity_bytes();
        if (plan.stats.selection_peak_capacity_bytes
                > direct_plan_permit->bytes()
            || retained_plan_bytes
                != plan.stats.selection_retained_capacity_bytes) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 accepted-plan peak exceeded admission");
        }
        direct_plan_permit->shrink_to(retained_plan_bytes);
        batch.stats_.compact_direct_lookup_ns = elapsed_stage_ns_(
            lookup_started_ns);
        batch.stats_.compact_direct_unique_lookups =
            plan.stats.distinct_query_hash_count;
        batch.stats_.compact_direct_selected_hashes =
            plan.stats.unique_selected_hash_count;
        batch.stats_.compact_direct_runtime_rejected_hashes =
            plan.stats.runtime_rejected_hash_count;
        batch.stats_.compact_direct_runtime_rejected_postings =
            plan.stats.runtime_rejected_posting_count;
        batch.stats_.compact_direct_selected_postings =
            plan.stats.selected_posting_count;
        batch.stats_.compact_direct_rejected_posting_reads =
            plan.stats.rejected_posting_reads;
        batch.stats_.compact_direct_plan_bytes = saturating_mul(
            plan.selections.size(), sizeof(compact_hash32_unique_selection));
        batch.stats_.compact_direct_plan_reserve_calls =
            plan.stats.selection_reserve_call_count;
        batch.stats_.compact_direct_plan_reserve_requested_elements =
            plan.stats.selection_reserve_requested_elements;
        batch.stats_.compact_direct_plan_initial_capacity_elements =
            plan.stats.selection_initial_capacity_elements;
        batch.stats_.compact_direct_plan_fill_reallocations =
            plan.stats.selection_fill_reallocation_count;
        batch.stats_.compact_direct_plan_compaction_allocations =
            plan.stats.selection_compaction_allocation_count;
        batch.stats_.compact_direct_plan_storage_allocations =
            plan.stats.selection_storage_allocation_count;
        batch.stats_.compact_direct_plan_retained_capacity_elements =
            plan.stats.selection_retained_capacity_elements;
        batch.stats_.compact_direct_plan_retained_capacity_bytes =
            plan.stats.selection_retained_capacity_bytes;
        batch.stats_.peak_compact_direct_plan_capacity_bytes =
            plan.stats.selection_peak_capacity_bytes;
        const std::uint64_t expected_storage_allocations =
            direct_query_plan_count == 0u ? 0u
            : plan.selections.empty()
                || plan.selections.size()
                    == direct_query_plan_count
            ? 1u : 2u;
        const std::uint64_t expected_compaction_allocations =
            !plan.selections.empty()
                && plan.selections.size()
                    < direct_query_plan_count
            ? 1u : 0u;
        const std::uint64_t expected_peak_capacity_elements =
            expected_compaction_allocations == 0u
            ? direct_query_plan_count
            : saturating_add(
                direct_query_plan_count,
                plan.selections.size());
        const std::uint64_t expected_peak_capacity_bytes = saturating_mul(
            expected_peak_capacity_elements,
            sizeof(compact_hash32_unique_selection));
        if (plan.stats.query_occurrence_count
                != direct_query_plan_count
            || plan.stats.distinct_query_hash_count
                != direct_query_plan_count
            || plan.stats.unique_selected_hash_count
                != plan.selections.size()
            || plan.stats.rejected_posting_reads != 0u
            || plan.stats.selection_reserve_call_count != 1u
            || plan.stats.selection_reserve_requested_elements
                != direct_query_plan_count
            || plan.stats.selection_initial_capacity_elements
                != direct_query_plan_count
            || plan.stats.selection_fill_reallocation_count != 0u
            || plan.stats.selection_compaction_allocation_count
                != expected_compaction_allocations
            || plan.stats.selection_storage_allocation_count
                != expected_storage_allocations
            || plan.stats.selection_retained_capacity_elements
                != plan.selections.size()
            || plan.stats.selection_retained_capacity_bytes
                != batch.stats_.compact_direct_plan_bytes
            || plan.stats.selection_peak_capacity_bytes
                != expected_peak_capacity_bytes
            || retained_plan_bytes
                != batch.stats_.compact_direct_plan_bytes
            || batch.stats_.compact_direct_plan_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || expected_peak_capacity_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 unique lookup stats disagrees");
        }

        const bool have_selected_postings =
            plan.stats.selected_posting_count != 0u;
        const std::size_t selection_shard_count =
            have_selected_postings
            ? std::min(
                plan.selections.size(),
                compact_direct_selection_shard_target_)
            : 0u;
        if (have_selected_postings
            && (selection_shard_count == 0u || queries.empty()
                || selection_shard_count
                    > (std::numeric_limits<std::size_t>::max)()
                        / queries.size())) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 shard/query layout size is inconsistent");
        }
        const std::size_t shard_query_layout_count =
            selection_shard_count * queries.size();
        batch.stats_.compact_direct_selection_shards =
            selection_shard_count;
        batch.stats_.compact_direct_shard_query_layouts =
            shard_query_layout_count;
        const std::uint64_t orientation_started_ns = stage_now_ns_();
        if (have_selected_postings) {
            const std::size_t old_layout_capacity =
                scratch.block_query_layouts_.capacity();
            scratch.ensure_vector_capacity_(
                scratch.block_query_layouts_, shard_query_layout_count);
            batch.stats_.block_layout_allocations =
                scratch.block_query_layouts_.capacity()
                        > old_layout_capacity
                ? 1u : 0u;
            batch.stats_.peak_block_layout_capacity_bytes =
                saturating_mul(
                    scratch.block_query_layouts_.capacity(),
                    sizeof(block_query_anchor_layout));
            scratch.block_query_layouts_.assign(
                shard_query_layout_count, block_query_anchor_layout{});

            const std::size_t old_block_stats_capacity =
                scratch.block_stats_.capacity();
            scratch.ensure_vector_capacity_(
                scratch.block_stats_, queries.size());
            batch.stats_.block_stats_allocations =
                scratch.block_stats_.capacity()
                        > old_block_stats_capacity
                ? 1u : 0u;
            batch.stats_.peak_block_stats_capacity_bytes =
                saturating_mul(
                    scratch.block_stats_.capacity(),
                    sizeof(fragment_block_wave_stats));
            scratch.block_stats_.assign(
                queries.size(), fragment_block_wave_stats{});
        }

        const auto selection_access =
            bind_direct_compact_hash_lookup_index_selection_spans(
                compact_direct_->view_, plan);
        if (!selection_access
            || selection_access->selection_count()
                != plan.selections.size()) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 selection-range binding failed; direct_error="
                + std::to_string(static_cast<unsigned>(
                    selection_access
                        ? direct_inverted_index_error::query_plan
                        : selection_access.error())));
        }

        std::array<CompactDirectLayoutPassContext,
            compact_direct_selection_shard_target_> layout_contexts{};
        for (std::size_t shard_ordinal = 0u;
             shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            layout_contexts[shard_ordinal] =
                CompactDirectLayoutPassContext{
                    .reader = this,
                    .plan = &plan,
                    .queries = queries,
                    .scratch = &scratch,
                    .shard_ordinal = shard_ordinal,
                };
        }
        if (have_selected_postings) {
            parallel_entries_(
                selection_shard_count,
                [&selection_access, &layout_contexts,
                 selection_count = plan.selections.size(),
                 selection_shard_count](
                        std::size_t shard_begin,
                        std::size_t shard_end) noexcept {
                    for (std::size_t shard_ordinal = shard_begin;
                         shard_ordinal < shard_end;
                         ++shard_ordinal) {
                        CompactDirectLayoutPassContext& context =
                            layout_contexts[shard_ordinal];
                        const compact_direct_selection_shard_range range =
                            compact_direct_selection_shard_range_(
                                selection_count,
                                selection_shard_count,
                                shard_ordinal);
                        auto visited =
                            visit_direct_compact_hash_lookup_index_selection_span_range(
                                *selection_access,
                                range.begin,
                                range.end,
                                [&context](
                                        const compact_hash32_selection_posting_spans&
                                            selected) noexcept {
                                    return
                                        visit_compact_direct_layout_selection_(
                                            context, selected);
                                });
                        flush_compact_direct_selected_page_advice_(context);
                        if (visited) {
                            context.visit_stats = *visited;
                        } else if (!context.error
                            && !context.direct_error) {
                            context.direct_error = visited.error();
                        }
                    }
                });
        }

        // Every shard binds authenticated descriptors and span geometry, but
        // deliberately does not load one encoded posting position.
        batch.stats_.compact_direct_layout_postings_visited = 0u;
        std::uint64_t layout_logical_postings = 0u;
        std::size_t layout_selection_end = 0u;
        for (std::size_t shard_ordinal = 0u;
             shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            const CompactDirectLayoutPassContext& context =
                layout_contexts[shard_ordinal];
            if (context.error) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 descriptor layout shard failed; wire_error="
                    + std::to_string(static_cast<unsigned>(*context.error)));
            }
            if (context.direct_error || !context.visit_stats) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 descriptor layout shard failed; direct_error="
                    + std::to_string(static_cast<unsigned>(
                        context.direct_error
                            ? *context.direct_error
                            : direct_inverted_index_error::query_plan)));
            }
            const compact_direct_selection_shard_range range =
                compact_direct_selection_shard_range_(
                    plan.selections.size(),
                    selection_shard_count,
                    shard_ordinal);
            const compact_hash32_selection_span_visit_stats& visit =
                *context.visit_stats;
            if (range.begin != layout_selection_end
                || range.end <= range.begin
                || range.end > plan.selections.size()
                || visit.selection_callbacks != range.end - range.begin
                || visit.descriptors_examined != range.end - range.begin
                || visit.postings_exposed
                    != context.logical_postings_counted
                || visit.rank_checkpoints_examined != 0u
                || visit.membership_words_examined != 0u
                || !add_checked_(
                    layout_logical_postings,
                    context.logical_postings_counted)
                || !add_checked_(
                    batch.stats_.
                        compact_direct_layout_selection_callbacks,
                    visit.selection_callbacks)
                || !add_checked_(
                    batch.stats_.compact_direct_layout_spans_exposed,
                    visit.spans_exposed)
                || !add_checked_(
                    batch.stats_.
                        compact_direct_layout_descriptors_examined,
                    visit.descriptors_examined)
                || !add_checked_(
                    batch.stats_.
                        compact_direct_traversal_rank_checkpoints_examined,
                    visit.rank_checkpoints_examined)
                || !add_checked_(
                    batch.stats_.
                        compact_direct_traversal_membership_words_examined,
                    visit.membership_words_examined)) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 descriptor layout shard receipt disagrees");
            }
            layout_selection_end = range.end;
            ++batch.stats_.compact_direct_layout_shards_completed;
        }
        if (layout_logical_postings
                != plan.stats.selected_posting_count
            || layout_selection_end != plan.selections.size()
            || batch.stats_.compact_direct_layout_selection_callbacks
                != plan.selections.size()
            || batch.stats_.compact_direct_layout_descriptors_examined
                != plan.selections.size()) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 descriptor layout aggregate receipt disagrees");
        }

        for (std::size_t query_ordinal = 0u;
             query_ordinal < queries.size();
             ++query_ordinal) {
            if (!have_selected_postings) {
                batch.queries_[query_ordinal].anchor_count = 0u;
                continue;
            }
            std::uint64_t forward_cursor = 0u;
            for (std::size_t shard_ordinal = 0u;
                 shard_ordinal < selection_shard_count;
                 ++shard_ordinal) {
                block_query_anchor_layout& layout =
                    scratch.block_query_layouts_[
                        shard_ordinal * queries.size() + query_ordinal];
                layout.forward_begin = forward_cursor;
                layout.forward_cursor = forward_cursor;
                if (!add_checked_(forward_cursor, layout.forward_count)) {
                    throw persisted_minimizer_lookup_error(
                        "compact-H32 forward shard prefix overflow");
                }
            }
            std::uint64_t reverse_cursor = forward_cursor;
            for (std::size_t shard_ordinal = 0u;
                 shard_ordinal < selection_shard_count;
                 ++shard_ordinal) {
                block_query_anchor_layout& layout =
                    scratch.block_query_layouts_[
                        shard_ordinal * queries.size() + query_ordinal];
                layout.reverse_begin = reverse_cursor;
                layout.reverse_cursor = reverse_cursor;
                if (!add_checked_(reverse_cursor, layout.reverse_count)) {
                    throw persisted_minimizer_lookup_error(
                        "compact-H32 reverse shard prefix overflow");
                }
            }
            batch.queries_[query_ordinal].anchor_count = reverse_cursor;
        }

        std::uint64_t final_owned_bytes = batch.owned_bytes_;
        for (std::size_t query_ordinal = 0u;
             query_ordinal < queries.size();
             ++query_ordinal) {
            const prepared_query_anchors& query =
                batch.queries_[query_ordinal];
            const std::uint64_t anchor_bytes = saturating_mul(
                query.anchor_count,
                sizeof(bioseq::chaining::anchor));
            final_owned_bytes = saturating_add(
                final_owned_bytes, anchor_bytes);
            if (anchor_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || final_owned_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || query.anchor_count
                    > (std::numeric_limits<std::size_t>::max)()
                        / sizeof(bioseq::chaining::anchor)) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 exact anchor size overflow");
            }
        }
        if (!batch.permit_.grow_to(final_owned_bytes)) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::
                    admission_or_allocation,
                "compact-H32 exact anchor admission failed");
        }
        try {
            for (std::size_t query_ordinal = 0u;
                 query_ordinal < queries.size();
                 ++query_ordinal) {
                prepared_query_anchors& query =
                    batch.queries_[query_ordinal];
                if (query.anchor_count == 0u) continue;
                query.anchors = std::make_unique_for_overwrite<
                    bioseq::chaining::anchor[]>(
                        static_cast<std::size_t>(query.anchor_count));
            }
        } catch (const std::bad_alloc&) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::
                    admission_or_allocation,
                "compact-H32 exact anchor allocation failed");
        }
        batch.owned_bytes_ = final_owned_bytes;
        batch.permit_.shrink_to(batch.owned_bytes_);
        batch.stats_.orientation_layout_ns = elapsed_stage_ns_(
            orientation_started_ns);

        const std::uint64_t emission_started_ns = stage_now_ns_();
        std::array<CompactDirectEmissionPassContext,
            compact_direct_selection_shard_target_> emission_contexts{};
        for (std::size_t shard_ordinal = 0u;
             shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            emission_contexts[shard_ordinal] =
                CompactDirectEmissionPassContext{
                    .reader = this,
                    .plan = &plan,
                    .queries = queries,
                    .scratch = &scratch,
                    .batch = &batch,
                    .shard_ordinal = shard_ordinal,
                };
        }
        if (have_selected_postings) {
            parallel_entries_(
                selection_shard_count,
                [&selection_access, &emission_contexts,
                 selection_count = plan.selections.size(),
                 selection_shard_count](
                        std::size_t shard_begin,
                        std::size_t shard_end) noexcept {
                    for (std::size_t shard_ordinal = shard_begin;
                         shard_ordinal < shard_end;
                         ++shard_ordinal) {
                        CompactDirectEmissionPassContext& context =
                            emission_contexts[shard_ordinal];
                        const compact_direct_selection_shard_range range =
                            compact_direct_selection_shard_range_(
                                selection_count,
                                selection_shard_count,
                                shard_ordinal);
                        auto visited =
                            visit_direct_compact_hash_lookup_index_selection_span_range(
                                *selection_access,
                                range.begin,
                                range.end,
                                [&context](
                                        const compact_hash32_selection_posting_spans&
                                            selected) noexcept {
                                    return
                                        visit_compact_direct_emission_selection_(
                                            context, selected);
                                });
                        if (visited) {
                            context.visit_stats = *visited;
                        } else if (!context.error
                            && !context.direct_error) {
                            context.direct_error = visited.error();
                        }
                    }
                });
        }
        batch.stats_.compact_direct_posting_visit_passes =
            have_selected_postings ? 1u : 0u;
        std::uint64_t emission_posting_bytes = 0u;
        std::uint64_t emission_postings = 0u;
        std::uint64_t emission_anchors = 0u;
        std::size_t emission_selection_end = 0u;
        for (std::size_t shard_ordinal = 0u;
             shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            const CompactDirectEmissionPassContext& context =
                emission_contexts[shard_ordinal];
            if (context.error) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 emission shard failed; wire_error="
                    + std::to_string(static_cast<unsigned>(
                        *context.error)));
            }
            if (context.direct_error || !context.visit_stats) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 emission shard failed; direct_error="
                    + std::to_string(static_cast<unsigned>(
                        context.direct_error
                            ? *context.direct_error
                            : direct_inverted_index_error::query_plan)));
            }
            const compact_direct_selection_shard_range range =
                compact_direct_selection_shard_range_(
                    plan.selections.size(),
                    selection_shard_count,
                    shard_ordinal);
            const compact_hash32_selection_span_visit_stats& visit =
                *context.visit_stats;
            if (range.begin != emission_selection_end
                || range.end <= range.begin
                || range.end > plan.selections.size()
                || visit.selection_callbacks != range.end - range.begin
                || visit.descriptors_examined != range.end - range.begin
                || visit.postings_exposed != context.postings_visited
                || visit.rank_checkpoints_examined != 0u
                || visit.membership_words_examined != 0u
                || !add_checked_(
                    emission_posting_bytes, context.posting_bytes_read)
                || !add_checked_(
                    emission_postings, context.postings_visited)
                || !add_checked_(
                    emission_anchors, context.anchors_constructed)
                || !add_checked_(
                    batch.stats_.
                        compact_direct_emission_selection_callbacks,
                    visit.selection_callbacks)
                || !add_checked_(
                    batch.stats_.compact_direct_emission_spans_exposed,
                    visit.spans_exposed)
                || !add_checked_(
                    batch.stats_.
                        compact_direct_emission_descriptors_examined,
                    visit.descriptors_examined)
                || !add_checked_(
                    batch.stats_.
                        compact_direct_traversal_rank_checkpoints_examined,
                    visit.rank_checkpoints_examined)
                || !add_checked_(
                    batch.stats_.
                        compact_direct_traversal_membership_words_examined,
                    visit.membership_words_examined)) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 emission shard receipt disagrees");
            }
            emission_selection_end = range.end;
            ++batch.stats_.compact_direct_emission_shards_completed;
        }
        batch.stats_.compact_direct_emission_postings_visited =
            emission_postings;
        batch.stats_.direct_emission_ns = elapsed_stage_ns_(
            emission_started_ns);
        if (emission_postings != plan.stats.selected_posting_count
            || emission_selection_end != plan.selections.size()
            || batch.stats_.compact_direct_emission_selection_callbacks
                != plan.selections.size()
            || batch.stats_.compact_direct_emission_descriptors_examined
                != plan.selections.size()
            || batch.stats_.compact_direct_emission_spans_exposed
                != batch.stats_.compact_direct_layout_spans_exposed) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 emission aggregate receipt disagrees");
        }

        if (have_selected_postings) {
            for (std::size_t shard_ordinal = 0u;
                 shard_ordinal < selection_shard_count;
                 ++shard_ordinal) {
                for (std::size_t query_ordinal = 0u;
                     query_ordinal < queries.size();
                     ++query_ordinal) {
                    const block_query_anchor_layout& layout =
                        scratch.block_query_layouts_[
                            shard_ordinal * queries.size() + query_ordinal];
                    if (layout.forward_begin
                            > (std::numeric_limits<std::uint64_t>::max)()
                                - layout.forward_count
                        || layout.reverse_begin
                            > (std::numeric_limits<std::uint64_t>::max)()
                                - layout.reverse_count
                        || layout.forward_cursor
                            != layout.forward_begin + layout.forward_count
                        || layout.reverse_cursor
                            != layout.reverse_begin + layout.reverse_count) {
                        throw persisted_minimizer_lookup_error(
                            "compact-H32 shard emission cursor receipt "
                            "disagrees");
                    }
                }
            }
        }

        std::atomic<std::uint32_t> first_error{0u};
        const auto run_local_order_stage =
            [this, queries, &batch, &scratch, &first_error](
                    bool normalize_ties) {
                first_error.store(0u, std::memory_order_relaxed);
                parallel_entries_(
                    queries.size(),
                    [queries, &batch, &scratch, &first_error,
                     normalize_ties,
                     authenticated_target_length = manifest_.target_length](
                            std::size_t query_begin,
                            std::size_t query_end) noexcept {
                        for (std::size_t query_ordinal = query_begin;
                             query_ordinal < query_end;
                             ++query_ordinal) {
                            if (first_error.load(
                                    std::memory_order_relaxed) != 0u
                                || query_ordinal
                                    >= scratch.block_stats_.size()) {
                                if (query_ordinal
                                        >= scratch.block_stats_.size()) {
                                    record_error_(
                                        first_error,
                                        minimizer_generation_encoding_error::
                                            arithmetic_overflow);
                                }
                                return;
                            }
                            std::uint64_t query_order_bytes = 0u;
                            const block_query_anchor_layout& first_shard_layout =
                                scratch.block_query_layouts_[query_ordinal];
                            auto& query = batch.queries_[query_ordinal];
                            const std::uint64_t forward_count =
                                first_shard_layout.reverse_begin;
                            if (forward_count > query.anchor_count) {
                                record_error_(
                                    first_error,
                                    minimizer_generation_encoding_error::
                                        arithmetic_overflow);
                                return;
                            }
                            const std::uint64_t reverse_count =
                                query.anchor_count - forward_count;
                            const auto order_slice =
                                [normalize_ties, &query_order_bytes,
                                 authenticated_target_length](
                                        bioseq::chaining::anchor* anchors,
                                        std::uint64_t count) noexcept {
                                    if (count < 2u) return true;
                                    const auto stats = normalize_ties
                                        ? normalize_anchor_slice_ties_(
                                            anchors,
                                            static_cast<std::size_t>(count))
                                        : sort_anchor_slice_primary_(
                                            anchors,
                                            static_cast<std::size_t>(count),
                                            authenticated_target_length);
                                    const std::uint64_t bytes = normalize_ties
                                        ? stats.tie_bytes
                                        : stats.primary_bytes;
                                    return !stats.saturated
                                        && add_checked_(
                                            query_order_bytes, bytes);
                                };
                            if ((forward_count > 1u
                                    && !order_slice(
                                        query.anchors.get(),
                                        forward_count))
                                || (reverse_count > 1u
                                    && !order_slice(
                                        query.anchors.get()
                                            + static_cast<std::size_t>(
                                                forward_count),
                                        reverse_count))) {
                                record_error_(
                                    first_error,
                                    minimizer_generation_encoding_error::
                                        arithmetic_overflow);
                                return;
                            }
                            fragment_block_wave_stats& block_stats =
                                scratch.block_stats_[query_ordinal];
                            if (normalize_ties) {
                                block_stats.tie_normalization_bytes =
                                    query_order_bytes;
                            } else {
                                block_stats.local_sort_bytes =
                                    query_order_bytes;
                            }
                        }
                    });
                if (const auto error = recorded_error_(first_error)) {
                    throw persisted_minimizer_lookup_error(
                        std::string{normalize_ties
                            ? "compact-H32 tie normalization failed; "
                            : "compact-H32 local primary sort failed; "}
                        + "wire_error="
                        + std::to_string(static_cast<unsigned>(*error)));
                }
            };

        const std::uint64_t local_sort_started_ns = stage_now_ns_();
        if (have_selected_postings) run_local_order_stage(false);
        batch.stats_.local_sort_ns = elapsed_stage_ns_(
            local_sort_started_ns);
        const std::uint64_t tie_started_ns = stage_now_ns_();
        if (have_selected_postings) run_local_order_stage(true);
        batch.stats_.tie_normalization_ns = elapsed_stage_ns_(
            tie_started_ns);

        std::uint64_t expected_constructed = 0u;
        for (std::size_t query_ordinal = 0u;
             query_ordinal < queries.size();
             ++query_ordinal) {
            if (!add_checked_(
                    expected_constructed,
                    batch.queries_[query_ordinal].anchor_count)) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 anchor stats overflow");
            }
        }
        for (const fragment_block_wave_stats& block_stats
             : scratch.block_stats_) {
            if (!add_checked_(
                    batch.stats_.local_sort_bytes,
                    block_stats.local_sort_bytes)
                || !add_checked_(
                    batch.stats_.tie_normalization_bytes,
                    block_stats.tie_normalization_bytes)) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 stats reduction overflow");
            }
        }
        batch.stats_.pass2_position_bytes = 0u;
        batch.stats_.pass3_position_bytes =
            emission_posting_bytes;
        batch.stats_.anchors_constructed =
            emission_anchors;
        const std::uint64_t expected_posting_bytes = saturating_mul(
            plan.stats.selected_posting_count,
            plan.posting_width_bytes);
        const std::uint64_t exact_final_bytes = saturating_mul(
            expected_constructed,
            sizeof(bioseq::chaining::anchor));
        if (batch.stats_.compact_direct_unique_lookups
                != batch.stats_.compact_direct_unique_query_groups
            || batch.stats_.compact_direct_selection_shards
                != selection_shard_count
            || batch.stats_.compact_direct_shard_query_layouts
                != shard_query_layout_count
            || batch.stats_.compact_direct_layout_shards_completed
                != selection_shard_count
            || batch.stats_.compact_direct_emission_shards_completed
                != selection_shard_count
            || batch.stats_.compact_direct_posting_visit_passes
                != (have_selected_postings ? 1u : 0u)
            || batch.stats_.compact_direct_layout_postings_visited != 0u
            || batch.stats_.compact_direct_emission_postings_visited
                != plan.stats.selected_posting_count
            || batch.stats_.compact_direct_layout_selection_callbacks
                != plan.selections.size()
            || batch.stats_.compact_direct_emission_selection_callbacks
                != plan.selections.size()
            || (plan.selections.empty()
                ? batch.stats_.compact_direct_layout_spans_exposed != 0u
                : batch.stats_.compact_direct_layout_spans_exposed == 0u)
            || batch.stats_.compact_direct_layout_spans_exposed
                != batch.stats_.
                    compact_direct_emission_spans_exposed
            || batch.stats_.compact_direct_layout_descriptors_examined
                != plan.selections.size()
            || batch.stats_.compact_direct_emission_descriptors_examined
                != plan.selections.size()
            || batch.stats_.
                    compact_direct_traversal_rank_checkpoints_examined
                != 0u
            || batch.stats_.
                    compact_direct_traversal_membership_words_examined
                != 0u
            || batch.stats_.compact_direct_target_control_scan_bytes
                != 0u
            || batch.stats_.compact_direct_target_control_scan_passes
                != 0u
            || batch.stats_.compact_direct_fallbacks != 0u
            || expected_posting_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || exact_final_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || batch.stats_.pass2_position_bytes != 0u
            || batch.stats_.pass3_position_bytes
                != expected_posting_bytes
            || batch.stats_.anchors_constructed
                != expected_constructed
            || emission_anchors
                != expected_constructed) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 direct-route stats equation failed");
        }
        batch.stats_.exact_final_bytes = exact_final_bytes;
        batch.stats_.peak_exact_final_bytes = exact_final_bytes;
    }

public:
    prepared_minimizer_fragment_reader(
            prepared_minimizer_fragment_reader&&) noexcept = default;
    prepared_minimizer_fragment_reader& operator=(
            prepared_minimizer_fragment_reader&&) = delete;
    prepared_minimizer_fragment_reader(
            const prepared_minimizer_fragment_reader&) = delete;
    prepared_minimizer_fragment_reader& operator=(
            const prepared_minimizer_fragment_reader&) = delete;

    /// Open the compact-H32 route without decoding or retaining the grouped-SoA
    /// directory and without owning a grouped-SoA payload mapping.  The manifest
    /// supplies only its authenticated uniform segment geometry; the direct
    /// image remains the sole posting/control authority for this reader.
    [[nodiscard]] static std::expected<
        prepared_minimizer_fragment_reader,
        minimizer_generation_encoding_error>
    open_compact_direct(
            bioseq::parallelism::memory_admission_controller& admission,
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const expected_minimizer_generation_identity& expected,
            std::uint64_t max_occ,
            owned_compact_hash32_direct_index_view compact_direct,
            compact_direct_selected_page_adviser selected_page_adviser = {}) {
        if (!selected_page_adviser.empty()
            && !selected_page_adviser.valid()) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        const auto contract = validate_direct_open_contract_(
            head, manifest, expected, max_occ, compact_direct);
        if (!contract) {
            return std::unexpected(contract.error());
        }

        auto permit = admission.try_reserve(0u);
        if (!permit) {
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        }

        prepared_minimizer_fragment_reader reader;
        reader.permit_ = std::move(*permit);
        reader.head_ = head;
        reader.manifest_ = manifest;
        reader.profile_ = contract->profile;
        reader.max_occ_ = max_occ;
        reader.backing_kind_ = backing_kind::compact_direct_only;
        reader.compact_direct_ = std::move(compact_direct);
        reader.selected_page_adviser_ = selected_page_adviser;
        reader.query_radix_shift_ = static_cast<std::uint8_t>(
            contract->hash_domain_bits > query_radix_bits_
            ? contract->hash_domain_bits - query_radix_bits_ : 0u);
        reader.query_membership_filter_shift_ =
            static_cast<std::uint8_t>(
                contract->hash_domain_bits > 21u
                ? contract->hash_domain_bits - 21u : 0u);
        reader.query_membership_filter_mask_id_mask_ =
            reader.query_membership_filter_shift_ == 0u
            ? 0u
            : static_cast<std::uint16_t>(
                (std::uint32_t{1}
                    << reader.query_membership_filter_shift_) - 1u);
        reader.wire_dispatch_ =
            reader.profile_.hash_width
                    == minimizer_scalar_width::u32
                && reader.profile_.count_width
                    == minimizer_scalar_width::u16
                && reader.profile_.position_width
                    == minimizer_scalar_width::u16
            ? fragment_encoding_dispatch::h32_c16_p16
            : fragment_encoding_dispatch::checked_dynamic;

        // validate_direct_open_contract_ has already authenticated the
        // manifest checksum and its standalone uniform-segment equations.
        // Compact-direct queries address postings through the direct image,
        // so re-deriving every segment core and retaining fragment_block_plan
        // objects would be redundant O(segment_count) cold-open work.

        reader.ready_ = true;
        return reader;
    }

    [[nodiscard]] static std::expected<
        prepared_minimizer_fragment_reader,
        minimizer_generation_encoding_error>
    open(
            bioseq::parallelism::memory_admission_controller& admission,
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const expected_minimizer_generation_identity& expected,
            std::span<const std::byte> encoded_directory,
            std::uint64_t max_occ,
            std::shared_ptr<const void> mapped_payload_owner,
            std::span<const std::byte> mapped_payload,
            std::optional<owned_compact_hash32_direct_index_view> compact_direct =
                std::nullopt) {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || manifest.layout
                != minimizer_generation_layout::
                    grouped_soa_segment_local
            || head.layout != manifest.layout
            || expected.layout != manifest.layout
            || !expected.max_occ_cap
            || *expected.max_occ_cap != profile->max_occ_cap
            || max_occ > profile->max_occ_cap
            || profile->max_hash
                > (std::numeric_limits<std::uint32_t>::max)()
            || max_occ > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int32_t>::max)())
            || mapped_payload_owner == nullptr
            || mapped_payload.size() != manifest.payload_bytes
            || encoded_directory.size() != manifest.directory_bytes) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        if (auto compatible = validate_minimizer_generation_compatibility(
                manifest, expected); !compatible) {
            return std::unexpected(compatible.error());
        }
        const std::uint32_t profile_max_hash =
            static_cast<std::uint32_t>(profile->max_hash);
        const std::uint32_t profile_hash_bits = std::bit_width(
            profile_max_hash);
        const std::uint64_t expected_profile_max_hash =
            profile_hash_bits == 0u
            ? 0u
            : (std::uint64_t{1} << profile_hash_bits) - 1u;
        if (profile_hash_bits == 0u
            || profile->max_hash != expected_profile_max_hash) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        if (compact_direct) {
            const compact_hash32_header& direct_header =
                compact_direct->view_.header;
            const direct_index_identity expected_identity{
                .generation_id = head.generation_id,
                .source_identity = head.source_identity,
                .compatibility_checksum =
                    head.compatibility_checksum,
                .target_length = manifest.target_length,
            };
            if (direct_header.identity != expected_identity
                || direct_header.max_occ_cap
                    != profile->max_occ_cap
                || direct_header.geometry.hash_domain_bits
                    != profile_hash_bits
                || direct_header.geometry.max_hash != profile_max_hash
                || direct_header.image_bytes
                    != compact_direct->view_.image.size()
                || compact_direct->view_.image.empty()
                || compact_direct->image_owner_.get()
                    != compact_direct->view_.image.data()) {
                return std::unexpected(
                    minimizer_generation_encoding_error::compatibility_mismatch);
            }
        }

        const std::size_t directory_stride =
            minimizer_directory_entry_encoded_bytes(*profile);
        if (directory_stride == 0u
            || encoded_directory.size() % directory_stride != 0u) {
            return std::unexpected(
                minimizer_generation_encoding_error::directory_size);
        }
        const std::size_t entry_count =
            encoded_directory.size() / directory_stride;
        if (entry_count
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::directory_size);
        }
        std::uint64_t initial_bytes = saturating_add(
            encoded_directory.size(),
            saturating_mul(
                entry_count, sizeof(grouped_soa_bucket_directory_entry)));
        initial_bytes = saturating_add(initial_bytes, saturating_mul(
            entry_count, sizeof(entry_plan)));
        initial_bytes = saturating_add(initial_bytes, saturating_mul(
            entry_count, sizeof(fragment_block_plan)));
        if (initial_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        auto permit = admission.try_reserve(initial_bytes);
        if (!permit) {
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        }

        prepared_minimizer_fragment_reader reader;
        reader.permit_ = std::move(*permit);
        reader.head_ = head;
        reader.manifest_ = manifest;
        reader.profile_ = *profile;
        reader.query_radix_shift_ = static_cast<std::uint8_t>(
            profile_hash_bits > query_radix_bits_
            ? profile_hash_bits - query_radix_bits_ : 0u);
        reader.query_membership_filter_shift_ =
            static_cast<std::uint8_t>(
                profile_hash_bits > 21u
                ? profile_hash_bits - 21u : 0u);
        reader.query_membership_filter_mask_id_mask_ =
            reader.query_membership_filter_shift_ == 0u
            ? 0u
            : static_cast<std::uint16_t>(
                (std::uint32_t{1}
                    << reader.query_membership_filter_shift_) - 1u);
        reader.mapped_payload_owner_ =
            std::move(mapped_payload_owner);
        reader.mapped_payload_ = mapped_payload;
        reader.compact_direct_ = std::move(compact_direct);
        reader.entry_count_ = entry_count;
        reader.max_occ_ = max_occ;
        try {
            auto directory = grouped_soa_directory_view::decode_admitted(
                encoded_directory, *profile, reader.permit_);
            if (!directory) {
                return std::unexpected(directory.error());
            }
            if (auto metadata = validate_minimizer_generation_metadata(
                    head, manifest, *directory); !metadata) {
                return std::unexpected(metadata.error());
            }
            reader.directory_ = std::move(*directory);
            if (entry_count != 0u) {
                reader.entry_plans_ =
                    std::make_unique<entry_plan[]>(entry_count);
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        }

        std::uint64_t total_control_bytes = 0u;
        std::uint64_t total_control_hash_scan_bytes = 0u;
        std::uint64_t total_control_count_scan_bytes = 0u;
        std::uint64_t total_position_bytes = 0u;
        const auto entries = reader.directory_.entries();
        for (std::size_t ordinal = 0u;
             ordinal < entries.size();
             ++ordinal) {
            const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
            if (entry.position_segment < 0
                || static_cast<std::uint32_t>(entry.position_segment)
                    >= manifest.position_segment_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::position_segment);
            }

            if (!is_minimizer_scalar_width(
                    profile->hash_width)
                || !is_minimizer_scalar_width(
                    profile->count_width)
                || !is_minimizer_scalar_width(
                    profile->position_width)
                || !is_minimizer_scalar_width(
                    entry.shape_offset_width)
                || entry.control_element_count == 0u) {
                return std::unexpected(
                    minimizer_generation_encoding_error::compatibility_mismatch);
            }
            const auto layout = plan_grouped_soa_control_plane(
                entry.control_element_count,
                profile->hash_width,
                entry.shape_offset_width,
                profile->count_width);
            if (!layout
                || layout->encoded_bytes != entry.control_encoded_bytes) {
                return std::unexpected(
                    layout
                        ? minimizer_generation_encoding_error::byte_count
                        : layout.error());
            }

            const std::uint64_t h = static_cast<std::uint8_t>(
                profile->hash_width);
            const std::uint64_t o = static_cast<std::uint8_t>(
                entry.shape_offset_width);
            const std::uint64_t c = static_cast<std::uint8_t>(
                profile->count_width);
            const std::uint64_t p = static_cast<std::uint8_t>(
                profile->position_width);
            const std::uint64_t prefix_element_count = saturating_add(
                entry.control_element_count, 1u);
            const std::uint64_t hash_bytes = saturating_mul(
                entry.control_element_count, h);
            const std::uint64_t prefix_bytes = saturating_mul(
                prefix_element_count, o);
            const std::uint64_t count_bytes = saturating_mul(
                entry.control_element_count, c);
            const std::uint64_t position_bytes = saturating_mul(
                entry.position_element_count, p);
            const std::uint64_t count_scan_bytes = saturating_mul(
                entry.control_element_count, h + c);
            if (prefix_element_count
                    == (std::numeric_limits<std::uint64_t>::max)()
                || hash_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || prefix_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || count_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || position_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || count_scan_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            if (position_bytes != entry.position_encoded_bytes) {
                return std::unexpected(
                    minimizer_generation_encoding_error::byte_count);
            }

            entry_plan& plan = reader.entry_plans_[ordinal];
            plan.control_begin = total_control_bytes;
            plan.control_end = saturating_add(
                plan.control_begin, entry.control_encoded_bytes);
            plan.hash_begin = saturating_add(
                plan.control_begin, layout->hash_offset);
            plan.hash_end = saturating_add(plan.hash_begin, hash_bytes);
            plan.prefix_begin = saturating_add(
                plan.control_begin, layout->prefix_offset);
            plan.prefix_end = saturating_add(
                plan.prefix_begin, prefix_bytes);
            plan.count_begin = saturating_add(
                plan.control_begin, layout->count_offset);
            plan.count_end = saturating_add(
                plan.count_begin, count_bytes);
            if (plan.control_end
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.hash_begin
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.hash_end
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.prefix_begin
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.prefix_end
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.count_begin
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.count_end
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.control_end > manifest.payload_bytes
                || plan.hash_begin != plan.control_begin
                || plan.hash_end > plan.prefix_begin
                || plan.prefix_end > plan.count_begin
                || plan.count_end > plan.control_end) {
                return std::unexpected(
                    minimizer_generation_encoding_error::byte_count);
            }

            const auto core = derive_minimizer_position_segment_bounds(
                manifest,
                static_cast<std::uint32_t>(entry.position_segment));
            if (!core || core->end_bp <= core->begin_bp
                || core->end_bp > manifest.target_length) {
                return std::unexpected(
                    minimizer_generation_encoding_error::bp_range);
            }
            plan.control_count = entry.control_element_count;
            plan.position_element_count = entry.position_element_count;
            plan.reference_begin = core->begin_bp;
            plan.reference_end = core->end_bp;
            plan.control_scan_bytes = count_scan_bytes;
            plan.position_segment = static_cast<std::uint32_t>(
                entry.position_segment);
            plan.hash_byte_width = static_cast<std::uint8_t>(
                profile->hash_width);
            plan.count_byte_width = static_cast<std::uint8_t>(
                profile->count_width);
            plan.position_byte_width = static_cast<std::uint8_t>(
                profile->position_width);
            plan.offset_byte_width = static_cast<std::uint8_t>(
                entry.shape_offset_width);
            plan.hash_width = profile->hash_width;
            plan.count_width = profile->count_width;
            plan.position_width = profile->position_width;
            plan.shape_offset_width = entry.shape_offset_width;

            total_control_bytes = plan.control_end;
            if (!add_checked_(
                    total_control_hash_scan_bytes,
                    hash_bytes)
                || !add_checked_(
                    total_control_count_scan_bytes,
                    count_scan_bytes)
                || !add_checked_(
                    total_position_bytes,
                    entry.position_encoded_bytes)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        if (total_position_bytes > manifest.payload_bytes
            || total_control_bytes
                > manifest.payload_bytes - total_position_bytes
            || total_control_bytes + total_position_bytes
                != manifest.payload_bytes) {
            return std::unexpected(
                minimizer_generation_encoding_error::byte_count);
        }
        reader.total_control_hash_scan_bytes_ =
            total_control_hash_scan_bytes;
        reader.total_control_count_scan_bytes_ =
            total_control_count_scan_bytes;
        std::uint64_t position_cursor = 0u;
        for (std::size_t ordinal = 0u;
             ordinal < entries.size();
             ++ordinal) {
            entry_plan& plan = reader.entry_plans_[ordinal];
            plan.position_begin = saturating_add(
                total_control_bytes, position_cursor);
            plan.position_end = saturating_add(
                plan.position_begin,
                entries[ordinal].position_encoded_bytes);
            if (plan.position_begin
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.position_end
                    == (std::numeric_limits<std::uint64_t>::max)()
                || plan.position_end > manifest.payload_bytes) {
                return std::unexpected(
                    minimizer_generation_encoding_error::byte_count);
            }
            position_cursor = saturating_add(
                position_cursor,
                entries[ordinal].position_encoded_bytes);
            if (position_cursor
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        if (position_cursor != total_position_bytes) {
            return std::unexpected(
                minimizer_generation_encoding_error::byte_count);
        }

        reader.wire_dispatch_ =
            profile->hash_width == minimizer_scalar_width::u32
                && profile->count_width
                    == minimizer_scalar_width::u16
                && profile->position_width
                    == minimizer_scalar_width::u16
            ? fragment_encoding_dispatch::h32_c16_p16
            : fragment_encoding_dispatch::checked_dynamic;

        // Build a compact address plan whose block boundaries can never split
        // an equal-position-segment run. Empty physical segments are permitted
        // gaps and need no synthetic directory entry.
        constexpr std::size_t target_entries_per_block = 128u;
        for (std::size_t ordinal = 1u;
             ordinal < entries.size();
             ++ordinal) {
            if (entries[ordinal].position_segment
                    < entries[ordinal - 1u].position_segment) {
                return std::unexpected(
                    minimizer_generation_encoding_error::directory_order);
            }
        }
        std::size_t block_count = 0u;
        for (std::size_t begin = 0u;
             begin < entries.size();) {
            std::size_t end = std::min(
                entries.size(),
                begin + target_entries_per_block);
            const auto terminal_segment =
                entries[end - 1u].position_segment;
            while (end < entries.size()
                && entries[end].position_segment
                    == terminal_segment) {
                ++end;
            }
            ++block_count;
            begin = end;
        }
        try {
            if (block_count != 0u) {
                reader.block_plans_ =
                    std::make_unique<fragment_block_plan[]>(
                        block_count);
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                minimizer_generation_encoding_error::admission);
        }
        reader.block_count_ = block_count;

        std::uint64_t previous_block_reference_end = 0u;
        bool have_previous_block = false;
        std::size_t block_ordinal = 0u;
        for (std::size_t begin = 0u;
             begin < entries.size();) {
            std::size_t end = std::min(
                entries.size(),
                begin + target_entries_per_block);
            const auto terminal_segment =
                entries[end - 1u].position_segment;
            while (end < entries.size()
                && entries[end].position_segment
                    == terminal_segment) {
                ++end;
            }
            if (begin >= end
                || end > entries.size()
                || end
                    > (std::numeric_limits<std::uint32_t>::max)()) {
                return std::unexpected(
                    minimizer_generation_encoding_error::directory_size);
            }

            fragment_block_plan& block =
                reader.block_plans_[block_ordinal];
            block.entry_begin = static_cast<std::uint32_t>(begin);
            block.entry_end = static_cast<std::uint32_t>(end);
            block.first_position_segment =
                static_cast<std::uint32_t>(
                    entries[begin].position_segment);
            block.last_position_segment =
                static_cast<std::uint32_t>(
                    entries[end - 1u].position_segment);

            bool have_core = false;
            std::uint64_t previous_core_end = 0u;
            std::int32_t previous_segment = -1;
            std::size_t segment_run_begin = begin;
            for (std::size_t ordinal = begin;
                 ordinal < end;
                 ++ordinal) {
                const std::int32_t segment =
                    entries[ordinal].position_segment;
                if (segment == previous_segment) continue;
                if (previous_segment >= 0
                    && ordinal - segment_run_begin
                        > target_entries_per_block) {
                    ++reader.oversized_segment_block_count_;
                }
                segment_run_begin = ordinal;
                const entry_plan& entry_plan =
                    reader.entry_plans_[ordinal];
                if (entry_plan.position_segment
                        != static_cast<std::uint32_t>(segment)
                    || entry_plan.reference_end
                        <= entry_plan.reference_begin
                    || (have_core
                        && previous_core_end
                            > entry_plan.reference_begin)) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::bp_range);
                }
                if (!have_core) {
                    block.reference_begin = entry_plan.reference_begin;
                    have_core = true;
                }
                block.reference_end = entry_plan.reference_end;
                previous_core_end = entry_plan.reference_end;
                previous_segment = segment;
            }
            if (end - segment_run_begin
                    > target_entries_per_block) {
                ++reader.oversized_segment_block_count_;
            }
            if (!have_core
                || block.reference_end <= block.reference_begin
                || (have_previous_block
                    && previous_block_reference_end
                        > block.reference_begin)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::bp_range);
            }
            previous_block_reference_end = block.reference_end;
            have_previous_block = true;
            reader.largest_block_entry_count_ = std::max(
                reader.largest_block_entry_count_,
                static_cast<std::uint32_t>(end - begin));
            ++block_ordinal;
            begin = end;
        }
        if (block_ordinal != block_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::directory_size);
        }

        // The PostgreSQL-owned image is published only after a complete
        // address-bound row and fragment-extent CRC reread, fsync, and durable
        // rename. Its exact generation header was checked before this call.
        // Trust those immutable fragment payloads here. Reader open owns only
        // directory/address plans; max-occ reduction is fused into the later
        // fragment-parallel query wave so MPF has no target-wide open scan.

        reader.owned_bytes_ = reader.final_owned_bytes_();
        if (reader.owned_bytes_
                == (std::numeric_limits<std::uint64_t>::max)()
            || (reader.owned_bytes_ > reader.permit_.bytes()
                && !reader.permit_.grow_to(reader.owned_bytes_))) {
            return std::unexpected(
                reader.owned_bytes_
                        == (std::numeric_limits<std::uint64_t>::max)()
                    ? minimizer_generation_encoding_error::arithmetic_overflow
                    : minimizer_generation_encoding_error::admission);
        }
        reader.permit_.shrink_to(reader.owned_bytes_);
        reader.ready_ = true;
        return reader;
    }

    /// Prepare one immutable, canonically ordered anchor span per query.
    /// grouped-SoA control scans use count/layout/emission passes. The explicit
    /// compact-direct route instead performs one lookup per unique query hash,
    /// derives exact query/orientation counts from accepted descriptors, and
    /// decodes each accepted posting exactly once into its final owner, with no
    /// control-scan fallback.
    [[nodiscard]] prepared_fragment_anchor_batch
    prepare_fragment_anchor_batch(
            bioseq::parallelism::memory_admission_controller& admission,
            std::span<const fragment_seed_query> queries,
            std::int32_t max_occ,
            query_wave_scratch& scratch,
            bool baseline_query_plan_matcher_for_testing = false,
            bool verify_compositional_order_for_testing = false,
            minimizer_fragment_lookup_route lookup_route =
                minimizer_fragment_lookup_route::grouped_soa_control_scans) const {
        if (lookup_route
                == minimizer_fragment_lookup_route::grouped_soa_control_scans
            && backing_kind_ == backing_kind::compact_direct_only) {
            throw persisted_minimizer_lookup_error(
                "grouped-SoA control scans were explicitly selected on a "
                "compact-direct-only reader");
        }
        if (lookup_route == minimizer_fragment_lookup_route::direct_compact_hash_lookup) {
            if (baseline_query_plan_matcher_for_testing) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 direct route cannot use the grouped-SoA "
                    "baseline matcher");
            }
            return prepare_fragment_anchor_batch_impl_<false, true>(
                admission,
                queries,
                max_occ,
                scratch,
                verify_compositional_order_for_testing);
        }
        if (baseline_query_plan_matcher_for_testing) {
            return prepare_fragment_anchor_batch_impl_<true, false>(
                admission,
                queries,
                max_occ,
                scratch,
                verify_compositional_order_for_testing);
        }
        return prepare_fragment_anchor_batch_impl_<false, false>(
            admission,
            queries,
            max_occ,
            scratch,
            verify_compositional_order_for_testing);
    }

    template <bool Oracle, bool CompactDirect>
    [[nodiscard]] prepared_fragment_anchor_batch
    prepare_fragment_anchor_batch_impl_(
            bioseq::parallelism::memory_admission_controller& admission,
            std::span<const fragment_seed_query> queries,
            std::int32_t max_occ,
            query_wave_scratch& scratch,
            bool verify_compositional_order_for_testing) const {
        using query_hash_plan = query_wave_scratch::query_hash_plan;
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;

        if (!ready_) {
            throw persisted_minimizer_lookup_error(
                "fragment-native grouped-SoA reader is not ready");
        }
        if constexpr (CompactDirect) {
            if (!compact_direct_) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 direct route was explicitly selected "
                    "without an attached direct index");
            }
            if (backing_kind_ == backing_kind::compact_direct_only) {
                if (mapped_payload_owner_ != nullptr
                    || !mapped_payload_.empty()
                    || directory_.owned_capacity_bytes() != 0u
                    || entry_count_ != 0u
                    || entry_plans_ != nullptr
                    || block_plans_ != nullptr
                    || block_count_ != 0u
                    || owned_bytes_ != 0u
                    || permit_.bytes() != 0u) {
                    throw persisted_minimizer_lookup_error(
                        "compact-direct-only reader retained forbidden "
                        "grouped-SoA state");
                }
            } else if (mapped_payload_owner_ == nullptr
                || mapped_payload_.size() != manifest_.payload_bytes) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native grouped-SoA reader is not ready");
            }
        } else if (backing_kind_ != backing_kind::grouped_soa_fragments
            || mapped_payload_owner_ == nullptr
            || mapped_payload_.size() != manifest_.payload_bytes) {
            throw persisted_minimizer_lookup_error(
                "fragment-native grouped-SoA reader is not ready");
        }
        if (max_occ < 0
            || static_cast<std::uint64_t>(max_occ) != max_occ_
            || queries.size()
                > (std::numeric_limits<std::uint32_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "fragment-native grouped-SoA query policy is inconsistent");
        }
        // Freeze the manifest-selected wire route once for the complete wave.
        // O remains an entry-local dispatch inside the fixed leaf family.
        [[maybe_unused]] const bool use_fixed_h32_c16_p16 = wire_dispatch_
            == fragment_encoding_dispatch::h32_c16_p16;
        static_assert(!(Oracle && CompactDirect));
        constexpr bool use_bucket_direct_matcher =
            !Oracle && !CompactDirect;

        // Acquire the reusable scratch before reading or mutating any of its
        // non-atomic owners. The busy state remains published through the
        // synchronous joins that borrow the prepared batch and is cleared by
        // complete_wave_after_join. A failed preparation clears it in the
        // catch path below, so there is no reentrant vector/permit race.
        query_wave_scratch::wave_state expected_state =
            query_wave_scratch::wave_state::idle;
        if (!scratch.wave_state_.compare_exchange_strong(
                expected_state,
                query_wave_scratch::wave_state::preparing,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch is already in use");
        }

        try {
            if (!scratch.permit_ || scratch.admission_ != &admission) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native grouped-SoA query scratch identity is inconsistent");
            }
            scratch.clear_wave_storage_noexcept_();
            if constexpr (CompactDirect) {
                // This route never probes grouped-SoA controls.  Retained Bloom,
                // radix, group-directory, and atomic-counter owners from a
                // prior control-scan wave are released before direct planning
                // so they cannot overlap the accepted-only direct plan.
                {
                    std::vector<std::uint32_t> no_group_offsets;
                    std::vector<std::uint32_t> no_radix;
                    scratch.query_group_offsets_.swap(no_group_offsets);
                    scratch.query_plan_radix_.swap(no_radix);
                }
                scratch.query_membership_filter_.reset();
                scratch.query_membership_filter_word_capacity_ = 0u;
                scratch.occurrence_counts_.reset();
                scratch.occurrence_count_capacity_ = 0u;
                scratch.active_occurrence_count_ = 0u;
                scratch.permit_.shrink_to(
                    scratch.retained_capacity_bytes_());
            }
            if (use_bucket_direct_matcher
                && scratch.query_plans_.capacity() != 0u) {
                {
                    std::vector<query_hash_plan> no_baseline_plans;
                    scratch.query_plans_.swap(no_baseline_plans);
                }
                scratch.query_plan_reserve_request_ = 0u;
                scratch.permit_.shrink_to(
                    scratch.retained_capacity_bytes_());
            }
            std::uint64_t total_query_minimizers = 0u;
            std::uint64_t largest_query_minimizer_count = 0u;
            for (const fragment_seed_query& query : queries) {
                if (!bioseq::minimizers::
                        minimizer_query_length_is_representable<
                            minimap_sketch_data>(
                                query.query_length)
                    || query.kmer_size <= 0
                    || query.kmer_size
                        > (std::numeric_limits<std::uint8_t>::max)()
                    || query.kmer_size
                        != static_cast<std::int32_t>(
                            profile_.kmer_size)
                    || query.minimizers.size()
                        > (std::numeric_limits<std::uint32_t>::max)()
                    || total_query_minimizers
                        > (std::numeric_limits<std::uint32_t>::max)()
                            - query.minimizers.size()
                    || (!query.minimizers.empty()
                        && query.query_length
                            < static_cast<std::uint64_t>(
                                query.kmer_size))) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native grouped-SoA query descriptor is invalid");
                }
                for (const minimap_sketch_data& minimizer
                     : query.minimizers) {
                    const std::uint64_t forward_last =
                        minimizer.pos_strand_encoded() >> 1u;
                    if (minimizer.kmer() > profile_.max_hash
                        || minimizer.kmer()
                            > (std::numeric_limits<
                                std::uint32_t>::max)()
                        || forward_last >= query.query_length
                        || forward_last + 1u
                            < static_cast<std::uint64_t>(
                                query.kmer_size)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native grouped-SoA query minimizer is invalid");
                    }
                }
                total_query_minimizers += query.minimizers.size();
                largest_query_minimizer_count = std::max<std::uint64_t>(
                    largest_query_minimizer_count,
                    query.minimizers.size());
            }
            const auto occurrence_layout = plan_query_occurrence_packing(
                queries.size(),
                total_query_minimizers,
                largest_query_minimizer_count);
            if (!occurrence_layout) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native query occurrence layout is invalid");
            }
            scratch.occurrence_layout_ = *occurrence_layout;

            auto batch_permit = admission.try_reserve(0u);
            if (!batch_permit) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native anchor batch admission failed");
            }
            prepared_fragment_anchor_batch batch{
                std::move(*batch_permit)};
            batch.query_count_ = queries.size();
            batch.target_length_ = manifest_.target_length;
            batch.max_occ_ = static_cast<std::uint64_t>(max_occ);
            batch.stats_.waves = 1u;
            batch.stats_.queries = queries.size();
            batch.stats_.query_occurrences = total_query_minimizers;
            batch.stats_.packed_occurrence_waves =
                occurrence_layout->packed() ? 1u : 0u;
            batch.stats_.wide_occurrence_waves =
                occurrence_layout->packed() ? 0u : 1u;
            batch.stats_.global_sort_calls_skipped = queries.size();

            const std::uint64_t metadata_bytes = saturating_mul(
                queries.size(), sizeof(prepared_query_anchors));
            if (queries.size()
                    > (std::numeric_limits<std::size_t>::max)()
                        / sizeof(prepared_query_anchors)
                || metadata_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native anchor metadata size overflow");
            }
            if (!batch.permit_.grow_to(metadata_bytes)) {
                throw persisted_minimizer_lookup_error(
                    persisted_minimizer_lookup_error::reason::
                        admission_or_allocation,
                    "fragment-native anchor metadata admission failed");
            }
            try {
                if (!queries.empty()) {
                    batch.queries_ =
                        std::make_unique<prepared_query_anchors[]>(
                            queries.size());
                }
            } catch (const std::bad_alloc&) {
                batch.permit_.shrink_to(0u);
                throw persisted_minimizer_lookup_error(
                    persisted_minimizer_lookup_error::reason::
                        admission_or_allocation,
                    "fragment-native anchor metadata allocation failed");
            }
            batch.owned_bytes_ = metadata_bytes;
            batch.permit_.shrink_to(batch.owned_bytes_);
            for (std::size_t query_ordinal = 0u;
                 query_ordinal < queries.size();
                 ++query_ordinal) {
                prepared_query_anchors& destination =
                    batch.queries_[query_ordinal];
                const fragment_seed_query& source =
                    queries[query_ordinal];
                destination.minimizer_data =
                    source.minimizers.data();
                destination.minimizer_count =
                    source.minimizers.size();
                destination.query_length = source.query_length;
                destination.kmer_size = source.kmer_size;
            }

            if constexpr (CompactDirect) {
                // Ordinary seed lookup treats max_occ==0 as selecting no
                // postings.  Preserve that policy without calling the compact
                // codec's positive-cap query API and without touching either
                // grouped-SoA payload or direct postings.
                if (max_occ == 0) {
                    batch.stats_.compact_direct_waves = 1u;
                    batch.stats_.compositional_sortedness_receipts =
                        queries.size();
                    batch.stats_.exact_final_bytes = 0u;
                    batch.stats_.peak_exact_final_bytes = 0u;
                    for (std::size_t query_ordinal = 0u;
                         query_ordinal < queries.size();
                         ++query_ordinal) {
                        batch.queries_[query_ordinal].anchor_count = 0u;
                        batch.queries_[query_ordinal].sortedness_receipt =
                            prepared_fragment_anchor_batch::sortedness_receipt_;
                    }
                    if (!batch.stats_.forbidden_buffers_are_zero()
                        || batch.owned_bytes_ > batch.permit_.bytes()) {
                        throw persisted_minimizer_lookup_error(
                            "compact-H32 zero-cap ownership stats failed");
                    }
                    batch.permit_.shrink_to(batch.owned_bytes_);
                    batch.ready_ = true;
                    scratch.wave_state_.store(
                        query_wave_scratch::wave_state::outstanding,
                        std::memory_order_release);
                    return batch;
                }
            }

            const std::size_t occurrence_count =
                static_cast<std::size_t>(
                    occurrence_layout->occurrence_count);
            if (use_bucket_direct_matcher && occurrence_count != 0u) {
                const std::uint64_t query_radix_fill_started_ns =
                    stage_now_ns_();
                const std::size_t radix_size =
                    static_cast<std::size_t>(
                        query_radix_bucket_count_) + 1u;
                const std::size_t old_query_radix_capacity =
                    scratch.query_plan_radix_.capacity();
                scratch.ensure_vector_capacity_(
                    scratch.query_plan_radix_, radix_size);
                batch.stats_.query_radix_allocations =
                    scratch.query_plan_radix_.capacity()
                            > old_query_radix_capacity
                    ? 1u : 0u;
                batch.stats_.peak_query_radix_capacity_bytes =
                    saturating_mul(
                        scratch.query_plan_radix_.capacity(),
                        sizeof(std::uint32_t));
                scratch.query_plan_radix_.assign(radix_size, 0u);
                for (const fragment_seed_query& query : queries) {
                    for (const minimap_sketch_data& minimizer
                         : query.minimizers) {
                        const std::uint32_t bucket = query_bucket_(
                            static_cast<std::uint32_t>(minimizer.kmer()));
                        ++scratch.query_plan_radix_[bucket + 1u];
                    }
                }
                for (std::size_t bucket = 1u;
                     bucket < radix_size;
                     ++bucket) {
                    if (scratch.query_plan_radix_[bucket]
                            > (std::numeric_limits<std::uint32_t>::max)()
                                - scratch.query_plan_radix_[bucket - 1u]) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native occurrence bucket prefix overflow");
                    }
                    scratch.query_plan_radix_[bucket] +=
                        scratch.query_plan_radix_[bucket - 1u];
                }
                if (scratch.query_plan_radix_.back()
                        != occurrence_count) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native occurrence bucket count disagrees");
                }
                batch.stats_.query_plan_radix_fill_ns =
                    elapsed_stage_ns_(query_radix_fill_started_ns);
            }

            const std::uint64_t occurrence_fill_started_ns =
                stage_now_ns_();
            std::size_t occurrence_cursor = 0u;
            if (occurrence_layout->packed()) {
                const std::size_t old_occurrence_capacity =
                    scratch.packed_occurrences_.capacity();
                scratch.ensure_vector_capacity_(
                    scratch.packed_occurrences_, occurrence_count);
                batch.stats_.occurrence_allocations =
                    scratch.packed_occurrences_.capacity()
                            > old_occurrence_capacity
                    ? 1u : 0u;
                scratch.packed_occurrences_.resize(occurrence_count);
                const auto write_packed_occurrence = [&scratch,
                        occurrence_layout](std::size_t destination,
                            std::size_t query_ordinal,
                            std::size_t minimizer_ordinal,
                            const minimap_sketch_data& minimizer) {
                        const std::uint32_t encoded =
                            encode_packed_query_occurrence_trusted(
                            *occurrence_layout,
                            static_cast<std::uint32_t>(query_ordinal),
                            static_cast<std::uint32_t>(minimizer_ordinal));
#ifndef NDEBUG
                        const query_occurrence_reference decoded =
                            decode_packed_query_occurrence_trusted(
                                *occurrence_layout, encoded);
                        DEBUG_ASSERT(
                            decoded.batch_query_ordinal == query_ordinal
                                && decoded.query_ordinal
                                    == minimizer_ordinal,
                            "packed query occurrence round-trip");
#endif
                        scratch.packed_occurrences_[destination] =
                            packed_query_occurrence{
                                .hash = static_cast<std::uint32_t>(
                                    minimizer.kmer()),
                                .query_and_ordinal = encoded,
                            };
                };
                if constexpr (Oracle || CompactDirect) {
                    for (std::size_t query_ordinal = 0u;
                         query_ordinal < queries.size();
                         ++query_ordinal) {
                        const auto minimizers =
                            queries[query_ordinal].minimizers;
                        for (std::size_t minimizer_ordinal = 0u;
                             minimizer_ordinal < minimizers.size();
                             ++minimizer_ordinal) {
                            write_packed_occurrence(
                                occurrence_cursor++,
                                query_ordinal,
                                minimizer_ordinal,
                                minimizers[minimizer_ordinal]);
                        }
                    }
                } else {
                    for (std::size_t query_ordinal = queries.size();
                         query_ordinal-- > 0u;) {
                        const auto minimizers =
                            queries[query_ordinal].minimizers;
                        for (std::size_t minimizer_ordinal = minimizers.size();
                             minimizer_ordinal-- > 0u;) {
                            const std::uint32_t hash =
                                static_cast<std::uint32_t>(
                                    minimizers[minimizer_ordinal].kmer());
                            const std::uint32_t bucket = query_bucket_(hash);
                            const std::size_t destination =
                                --scratch.query_plan_radix_[bucket + 1u];
                            write_packed_occurrence(
                                destination,
                                query_ordinal,
                                minimizer_ordinal,
                                minimizers[minimizer_ordinal]);
                            ++occurrence_cursor;
                        }
                    }
                }
            } else {
                const std::size_t old_occurrence_capacity =
                    scratch.wide_occurrences_.capacity();
                scratch.ensure_vector_capacity_(
                    scratch.wide_occurrences_, occurrence_count);
                batch.stats_.occurrence_allocations =
                    scratch.wide_occurrences_.capacity()
                            > old_occurrence_capacity
                    ? 1u : 0u;
                scratch.wide_occurrences_.resize(occurrence_count);
                const auto write_wide_occurrence = [&scratch](
                        std::size_t destination,
                        std::size_t query_ordinal,
                        std::size_t minimizer_ordinal,
                        const minimap_sketch_data& minimizer) {
                        scratch.wide_occurrences_[destination] =
                            wide_query_occurrence{
                            .hash = static_cast<std::uint32_t>(
                                minimizer.kmer()),
                            .batch_query_ordinal =
                                static_cast<std::uint32_t>(
                                    query_ordinal),
                            .query_ordinal =
                                static_cast<std::uint32_t>(
                                    minimizer_ordinal),
                            };
                };
                if constexpr (Oracle || CompactDirect) {
                    for (std::size_t query_ordinal = 0u;
                         query_ordinal < queries.size();
                         ++query_ordinal) {
                        const auto minimizers =
                            queries[query_ordinal].minimizers;
                        for (std::size_t minimizer_ordinal = 0u;
                             minimizer_ordinal < minimizers.size();
                             ++minimizer_ordinal) {
                            write_wide_occurrence(
                                occurrence_cursor++,
                                query_ordinal,
                                minimizer_ordinal,
                                minimizers[minimizer_ordinal]);
                        }
                    }
                } else {
                    for (std::size_t query_ordinal = queries.size();
                         query_ordinal-- > 0u;) {
                        const auto minimizers =
                            queries[query_ordinal].minimizers;
                        for (std::size_t minimizer_ordinal = minimizers.size();
                             minimizer_ordinal-- > 0u;) {
                            const std::uint32_t hash =
                                static_cast<std::uint32_t>(
                                    minimizers[minimizer_ordinal].kmer());
                            const std::uint32_t bucket = query_bucket_(hash);
                            const std::size_t destination =
                                --scratch.query_plan_radix_[bucket + 1u];
                            write_wide_occurrence(
                                destination,
                                query_ordinal,
                                minimizer_ordinal,
                                minimizers[minimizer_ordinal]);
                            ++occurrence_cursor;
                        }
                    }
                }
            }
            if (occurrence_cursor != occurrence_count) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native query occurrence count disagrees");
            }
            if (use_bucket_direct_matcher && occurrence_count != 0u) {
                for (std::size_t bucket = 0u;
                     bucket < query_radix_bucket_count_;
                     ++bucket) {
                    scratch.query_plan_radix_[bucket] =
                        scratch.query_plan_radix_[bucket + 1u];
                }
                scratch.query_plan_radix_[query_radix_bucket_count_] =
                    static_cast<std::uint32_t>(occurrence_count);
            }
            batch.stats_.peak_occurrence_capacity_bytes =
                scratch.retained_occurrence_capacity_bytes_();
            batch.stats_.occurrence_fill_ns = elapsed_stage_ns_(
                occurrence_fill_started_ns);

            const std::uint64_t occurrence_radix_started_ns =
                stage_now_ns_();
            if constexpr (Oracle || CompactDirect) {
                if (occurrence_count >= 2u) {
                    scratch.occurrence_radix_invoked_ = true;
                    const auto key = [](const auto& occurrence) noexcept
                            -> std::uint32_t {
                        return occurrence.hash;
                    };
                    if constexpr (CompactDirect) {
                        if (occurrence_layout->packed()
                            && occurrence_count >=
                                k_compact_direct_parallel_packed_occurrence_radix_min_count) {
                            const auto bucket_bounds =
                                partition_packed_occurrence_top_byte_(
                                    scratch.packed_occurrences_);
                            packed_query_occurrence* const occurrences =
                                scratch.packed_occurrences_.data();
                            scratch.parallel_occurrence_radix_split_selected_ =
                                true;
                            parallel_entries_(
                                bucket_bounds.size() - 1u,
                                [occurrences, &bucket_bounds, key](
                                        std::size_t bucket_begin,
                                        std::size_t bucket_end) noexcept {
                                    for (std::size_t bucket = bucket_begin;
                                         bucket < bucket_end;
                                         ++bucket) {
                                        const std::size_t begin =
                                            bucket_bounds[bucket];
                                        const std::size_t end =
                                            bucket_bounds[bucket + 1u];
                                        if (end - begin < 2u) continue;
                                        bioseq::sorting::
                                            radix_sort_from_shift<16>(
                                                occurrences + begin,
                                                occurrences + end,
                                                key);
                                    }
                                });
                        } else if (occurrence_layout->packed()) {
                            bioseq::sorting::radix_sort(
                                scratch.packed_occurrences_.data(),
                                scratch.packed_occurrences_.data()
                                    + scratch.packed_occurrences_.size(),
                                key);
                        } else {
                            bioseq::sorting::radix_sort(
                                scratch.wide_occurrences_.data(),
                                scratch.wide_occurrences_.data()
                                    + scratch.wide_occurrences_.size(),
                                key);
                        }
                    } else if (occurrence_layout->packed()) {
                        bioseq::sorting::radix_sort(
                            scratch.packed_occurrences_.data(),
                            scratch.packed_occurrences_.data()
                                + scratch.packed_occurrences_.size(),
                            key);
                    } else {
                        bioseq::sorting::radix_sort(
                            scratch.wide_occurrences_.data(),
                            scratch.wide_occurrences_.data()
                                + scratch.wide_occurrences_.size(),
                            key);
                    }
                }
            } else {
                if (occurrence_count >= 2u) {
                    const auto sort_bucketed_occurrences = [&scratch](
                            auto& occurrences) {
                        const auto hash_less = [](const auto& left,
                                                  const auto& right) noexcept {
                            return left.hash < right.hash;
                        };
                        for (std::size_t bucket = 0u;
                             bucket < query_radix_bucket_count_;
                             ++bucket) {
                            const std::size_t begin =
                                scratch.query_plan_radix_[bucket];
                            const std::size_t end =
                                scratch.query_plan_radix_[bucket + 1u];
                            if (end - begin < 2u) continue;
                            scratch.occurrence_radix_invoked_ = true;
                            if (end - begin < 256u) {
                                std::sort(
                                    occurrences.begin() + begin,
                                    occurrences.begin() + end,
                                    hash_less);
                            } else {
                                const auto key = [](
                                        const auto& occurrence) noexcept
                                        -> std::uint32_t {
                                    return occurrence.hash;
                                };
                                bioseq::sorting::radix_sort(
                                    occurrences.data() + begin,
                                    occurrences.data() + end,
                                    key);
                            }
                        }
                    };
                    if (occurrence_layout->packed()) {
                        sort_bucketed_occurrences(
                            scratch.packed_occurrences_);
                    } else {
                        sort_bucketed_occurrences(
                            scratch.wide_occurrences_);
                    }
                }
            }
            batch.stats_.occurrence_radix_ns = elapsed_stage_ns_(
                occurrence_radix_started_ns);

            const std::uint64_t unique_plan_started_ns = stage_now_ns_();
            const std::size_t old_query_plan_capacity =
                scratch.query_plans_.capacity();
            std::uint64_t compact_direct_group_key_set_checksum =
                compact_direct_query_set_checksum_prefix(
                    static_cast<std::uint32_t>(profile_.kmer_size),
                    static_cast<std::uint32_t>(manifest_.window_size),
                    static_cast<std::uint32_t>(max_occ));
            const auto build_query_plans = [&scratch,
                    &compact_direct_group_key_set_checksum](
                    const auto& occurrences) {
                std::size_t unique_count = 0u;
                for (std::size_t begin = 0u;
                     begin < occurrences.size();) {
                    std::size_t end = begin + 1u;
                    while (end < occurrences.size()
                        && occurrences[end].hash
                            == occurrences[begin].hash) {
                        ++end;
                    }
                    ++unique_count;
                    begin = end;
                }
                if (unique_count
                        > (std::numeric_limits<std::uint32_t>::max)()) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native unique query plan count overflow");
                }
                scratch.query_plan_reserve_request_ = unique_count;
                scratch.ensure_vector_capacity_(
                    scratch.query_plans_, unique_count);
                scratch.query_plans_.clear();
                for (std::size_t begin = 0u;
                     begin < occurrences.size();) {
                    std::size_t end = begin + 1u;
                    while (end < occurrences.size()
                        && occurrences[end].hash
                            == occurrences[begin].hash) {
                        ++end;
                    }
                    const std::uint32_t hash = occurrences[begin].hash;
                    scratch.query_plans_.push_back(query_hash_plan{
                        .hash = hash,
                        .occurrence_ref_begin =
                            static_cast<std::uint32_t>(begin),
                        .occurrence_ref_count =
                            static_cast<std::uint32_t>(end - begin),
                        .global_occurrence_count = 0u,
                    });
                    if constexpr (CompactDirect) {
                        compact_direct_group_key_set_checksum =
                            compact_direct_query_set_fnv_mix_u64(
                                compact_direct_group_key_set_checksum,
                                hash);
                    }
                    begin = end;
                }
            };
            const auto count_unique_occurrence_groups = [](
                    const auto& occurrences) noexcept {
                std::size_t unique_count = 0u;
                for (std::size_t begin = 0u;
                     begin < occurrences.size();) {
                    std::size_t end = begin + 1u;
                    while (end < occurrences.size()
                        && occurrences[end].hash
                            == occurrences[begin].hash) {
                        ++end;
                    }
                    ++unique_count;
                    begin = end;
                }
                return unique_count;
            };
            std::size_t unique_occurrence_group_count = 0u;
            bool production_group_hashes_strictly_monotonic = false;
            const std::size_t old_query_group_offset_capacity =
                scratch.query_group_offsets_.capacity();
            const std::size_t old_query_membership_filter_word_capacity =
                scratch.query_membership_filter_word_capacity_;
            if constexpr (Oracle || CompactDirect) {
                if (occurrence_layout->packed()) {
                    build_query_plans(scratch.packed_occurrences_);
                } else {
                    build_query_plans(scratch.wide_occurrences_);
                }
                unique_occurrence_group_count =
                    scratch.query_plans_.size();
                if constexpr (CompactDirect) {
                    batch.compact_direct_query_set_stats_ =
                        compact_direct_query_set_stats{
                            .query_count = queries.size(),
                            .query_occurrence_count = occurrence_count,
                            .unique_hash_count =
                                unique_occurrence_group_count,
                            .group_key_set_checksum =
                                compact_direct_group_key_set_checksum,
                            .present = true,
                        };
                }
            } else {
                scratch.query_plan_reserve_request_ = 0u;
                unique_occurrence_group_count = occurrence_layout->packed()
                    ? count_unique_occurrence_groups(
                        scratch.packed_occurrences_)
                    : count_unique_occurrence_groups(
                        scratch.wide_occurrences_);
                if (unique_occurrence_group_count
                        > (std::numeric_limits<std::uint32_t>::max)()
                    || unique_occurrence_group_count
                        == (std::numeric_limits<std::size_t>::max)()) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native unique occurrence group count overflow");
                }
                if (occurrence_count != 0u) {
                    scratch.ensure_vector_capacity_(
                        scratch.query_group_offsets_,
                        unique_occurrence_group_count + 1u);
                    scratch.query_group_offsets_.resize(
                        unique_occurrence_group_count + 1u);
                    scratch.ensure_query_membership_filter_capacity_();
                    const auto build_group_index = [this, &scratch,
                            &production_group_hashes_strictly_monotonic](
                            const auto& occurrences) noexcept {
                        std::size_t group_ordinal = 0u;
                        bool have_previous_hash = false;
                        std::uint32_t previous_hash = 0u;
                        bool strictly_monotonic = true;
                        for (std::size_t begin = 0u;
                             begin < occurrences.size();) {
                            std::size_t end = begin + 1u;
                            while (end < occurrences.size()
                                && occurrences[end].hash
                                    == occurrences[begin].hash) {
                                ++end;
                            }
                            scratch.query_group_offsets_[group_ordinal++] =
                                static_cast<std::uint32_t>(begin);
                            const std::uint32_t hash =
                                occurrences[begin].hash;
                            strictly_monotonic = strictly_monotonic
                                && (!have_previous_hash
                                    || previous_hash < hash);
                            previous_hash = hash;
                            have_previous_hash = true;
                            scratch.query_membership_filter_[
                                query_membership_filter_word_(hash)]
                                |= query_membership_filter_mask_(hash);
                            begin = end;
                        }
                        scratch.query_group_offsets_[group_ordinal] =
                            static_cast<std::uint32_t>(occurrences.size());
                        production_group_hashes_strictly_monotonic =
                            strictly_monotonic && have_previous_hash;
                        return group_ordinal;
                    };
                    const std::size_t built_group_count =
                        occurrence_layout->packed()
                        ? build_group_index(scratch.packed_occurrences_)
                        : build_group_index(scratch.wide_occurrences_);
                    if (built_group_count
                            != unique_occurrence_group_count
                        || !production_group_hashes_strictly_monotonic) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native occurrence group receipt disagrees");
                    }
                    const auto group_hash = [&scratch,
                            occurrence_layout](std::size_t group_ordinal)
                            noexcept {
                        const std::uint32_t occurrence_ordinal =
                            scratch.query_group_offsets_[group_ordinal];
                        return occurrence_layout->packed()
                            ? scratch.packed_occurrences_[occurrence_ordinal].hash
                            : scratch.wide_occurrences_[occurrence_ordinal].hash;
                    };
                    std::uint32_t group_cursor = 0u;
                    for (std::uint32_t bucket = 0u;
                         bucket <= query_radix_bucket_count_;
                         ++bucket) {
                        while (group_cursor
                                < unique_occurrence_group_count
                            && query_bucket_(group_hash(group_cursor))
                                < bucket) {
                            ++group_cursor;
                        }
                        scratch.query_plan_radix_[bucket] = group_cursor;
                    }
                }
            }
            batch.stats_.query_plan_allocations =
                ((Oracle || CompactDirect)
                    && scratch.query_plans_.capacity()
                        > old_query_plan_capacity)
                ? 1u : 0u;
            batch.stats_.peak_query_plan_capacity_bytes =
                (Oracle || CompactDirect)
                ? saturating_mul(
                    scratch.query_plans_.capacity(),
                    sizeof(query_hash_plan))
                : 0u;
            batch.stats_.query_group_offset_allocations =
                use_bucket_direct_matcher
                    && scratch.query_group_offsets_.capacity()
                        > old_query_group_offset_capacity
                ? 1u : 0u;
            batch.stats_.peak_query_group_offset_capacity_bytes =
                use_bucket_direct_matcher
                ? saturating_mul(
                    scratch.query_group_offsets_.capacity(),
                    sizeof(std::uint32_t))
                : 0u;
            batch.stats_.query_membership_filter_allocations =
                use_bucket_direct_matcher
                    && scratch.query_membership_filter_word_capacity_
                        > old_query_membership_filter_word_capacity
                ? 1u : 0u;
            batch.stats_.peak_query_membership_filter_capacity_bytes =
                use_bucket_direct_matcher
                ? saturating_mul(
                    scratch.query_membership_filter_word_capacity_,
                    sizeof(std::uint64_t))
                : 0u;
            batch.stats_.unique_query_plans =
                unique_occurrence_group_count;
            batch.stats_.unique_plan_build_ns = elapsed_stage_ns_(
                unique_plan_started_ns);

            if constexpr (CompactDirect) {
                prepare_compact_direct_anchors_(
                    admission,
                    queries,
                    static_cast<std::uint32_t>(max_occ),
                    scratch,
                    batch);
            } else if (occurrence_count != 0u) {
                const std::size_t radix_size =
                    static_cast<std::size_t>(
                        query_radix_bucket_count_) + 1u;
                if constexpr (Oracle) {
                    const std::uint64_t query_radix_fill_started_ns =
                        stage_now_ns_();
                    const std::size_t old_query_radix_capacity =
                        scratch.query_plan_radix_.capacity();
                    scratch.ensure_vector_capacity_(
                        scratch.query_plan_radix_, radix_size);
                    batch.stats_.query_radix_allocations =
                        scratch.query_plan_radix_.capacity()
                                > old_query_radix_capacity
                        ? 1u : 0u;
                    batch.stats_.peak_query_radix_capacity_bytes =
                        saturating_mul(
                            scratch.query_plan_radix_.capacity(),
                            sizeof(std::uint32_t));
                    scratch.query_plan_radix_.resize(radix_size);
                    std::uint32_t plan_cursor = 0u;
                    const std::uint32_t radix_item_count =
                        static_cast<std::uint32_t>(
                            scratch.query_plans_.size());
                    for (std::uint32_t bucket = 0u;
                         bucket <= query_radix_bucket_count_;
                         ++bucket) {
                        while (plan_cursor < radix_item_count
                            && query_bucket_(
                                scratch.query_plans_[plan_cursor].hash)
                                < bucket) {
                            ++plan_cursor;
                        }
                        scratch.query_plan_radix_[bucket] =
                            plan_cursor;
                    }
                    batch.stats_.query_plan_radix_fill_ns =
                        elapsed_stage_ns_(query_radix_fill_started_ns);
                }
                const std::size_t old_occurrence_count_capacity =
                    scratch.occurrence_count_capacity_;
                scratch.ensure_occurrence_count_capacity_(
                    Oracle
                    ? scratch.query_plans_.size()
                    : unique_occurrence_group_count);
                batch.stats_.occurrence_count_allocations =
                    scratch.occurrence_count_capacity_
                            > old_occurrence_count_capacity
                    ? 1u : 0u;
                batch.stats_.peak_occurrence_count_capacity_bytes =
                    saturating_mul(
                        scratch.occurrence_count_capacity_,
                        sizeof(std::atomic<std::uint32_t>));
                scratch.bucket_direct_matcher_active_ =
                    use_bucket_direct_matcher;

                bucket_direct_matcher_view bucket_direct_matcher_storage{};
                const bucket_direct_matcher_view*
                    bucket_direct_matcher = nullptr;
                if constexpr (!Oracle) {
                    const bool route_storage_matches =
                        occurrence_layout->packed()
                        ? scratch.packed_occurrences_.size()
                                == occurrence_count
                            && scratch.wide_occurrences_.empty()
                            && scratch.packed_occurrences_.data() != nullptr
                        : scratch.wide_occurrences_.size()
                                == occurrence_count
                            && scratch.packed_occurrences_.empty()
                            && scratch.wide_occurrences_.data() != nullptr;
                    if (!production_group_hashes_strictly_monotonic
                        || !valid_query_occurrence_packing_layout(
                            scratch.occurrence_layout_)
                        || scratch.occurrence_layout_
                            != *occurrence_layout
                        || !route_storage_matches
                        || scratch.query_membership_filter_ == nullptr
                        || scratch.query_membership_filter_word_capacity_
                            != query_wave_scratch::
                                query_membership_filter_word_count_
                        || scratch.query_plan_radix_.size() != radix_size
                        || scratch.query_plan_radix_.data() == nullptr
                        || scratch.query_plan_radix_.front() != 0u
                        || scratch.query_plan_radix_.back()
                            != unique_occurrence_group_count
                        || scratch.query_group_offsets_.size()
                            != unique_occurrence_group_count + 1u
                        || scratch.query_group_offsets_.data() == nullptr
                        || scratch.query_group_offsets_.front() != 0u
                        || scratch.query_group_offsets_.back()
                            != occurrence_count
                        || scratch.active_occurrence_count_
                            != unique_occurrence_group_count
                        || scratch.occurrence_count_capacity_
                            < unique_occurrence_group_count
                        || scratch.occurrence_counts_ == nullptr) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native production matcher view receipt disagrees");
                    }
                    bucket_direct_matcher_storage = bucket_direct_matcher_view{
                        .membership_filter =
                            scratch.query_membership_filter_.get(),
                        .radix = scratch.query_plan_radix_.data(),
                        .group_offsets =
                            scratch.query_group_offsets_.data(),
                        .packed_occurrences =
                            scratch.packed_occurrences_.data(),
                        .wide_occurrences =
                            scratch.wide_occurrences_.data(),
                        .occurrence_counts =
                            scratch.occurrence_counts_.get(),
                        .membership_filter_word_count =
                            scratch.query_membership_filter_word_capacity_,
                        .radix_count = scratch.query_plan_radix_.size(),
                        .group_offset_count =
                            scratch.query_group_offsets_.size(),
                        .packed_occurrence_count =
                            scratch.packed_occurrences_.size(),
                        .wide_occurrence_count =
                            scratch.wide_occurrences_.size(),
                        .occurrence_count = occurrence_count,
                        .counter_count =
                            scratch.active_occurrence_count_,
                        .occurrence_layout = scratch.occurrence_layout_,
                    };
                    bucket_direct_matcher =
                        &bucket_direct_matcher_storage;
                    batch.stats_.
                        bucket_direct_matcher_view_receipts = 1u;
                }

                const std::uint32_t max_occ_u32 =
                    static_cast<std::uint32_t>(max_occ);
                std::atomic<std::uint32_t> first_error{0u};

                // Pass 1: controls and saturated counts only.
                const std::uint64_t control_count_started_ns =
                    stage_now_ns_();
                parallel_entries_(
                    block_count_,
                    [this, &scratch, &first_error, max_occ_u32,
                     use_fixed_h32_c16_p16,
                     bucket_direct_matcher](
                            std::size_t block_begin,
                            std::size_t block_end) noexcept {
                        const std::span<const query_hash_plan> plans{
                            scratch.query_plans_};
                        const std::span<const std::uint32_t> radix{
                            scratch.query_plan_radix_};
                        for (std::size_t block_ordinal = block_begin;
                             block_ordinal < block_end;
                             ++block_ordinal) {
                            if (first_error.load(
                                    std::memory_order_relaxed) != 0u) {
                                return;
                            }
                            std::expected<void,
                                minimizer_generation_encoding_error> counted{};
                            fragment_probe_context context{
                                .pass = fragment_probe_pass::control_count,
                                .block = &block_plans_[block_ordinal],
                                .block_ordinal = block_ordinal,
                                .plans = plans,
                                .query_plan_radix = radix,
                                .scratch = &scratch,
                                .bucket_direct_matcher =
                                    bucket_direct_matcher,
                                .max_occ = max_occ_u32,
                            };
                            if (use_fixed_h32_c16_p16) {
                                counted = probe_block_h32_c16_p16_<Oracle>(
                                    context);
                            } else {
                                counted =
                                    visit_matched_control_counts_<Oracle>(
                                    block_plans_[block_ordinal],
                                    context,
                                    [&context, max_occ_u32](
                                            const query_occurrence_group& group,
                                            std::uint64_t local_count)
                                            noexcept
                                            -> std::optional<
                                                minimizer_generation_encoding_error> {
                                        (void)reserve_query_occurrences_(
                                            occurrence_counter_<Oracle>(
                                                context,
                                                group.counter_ordinal),
                                            local_count,
                                            max_occ_u32);
                                        return std::nullopt;
                                    });
                            }
                            if (!counted) {
                                record_error_(
                                    first_error, counted.error());
                                return;
                            }
                        }
                    });
                if (const auto error = recorded_error_(first_error)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native control-count pass failed; wire_error="
                        + std::to_string(static_cast<unsigned>(*error)));
                }
                batch.stats_.control_count_pass_ns = elapsed_stage_ns_(
                    control_count_started_ns);
                batch.stats_.control_count_bytes =
                    total_control_count_scan_bytes_;

                bool have_matched_controls = false;
                fragment_probe_context counted_group_context{
                    .scratch = &scratch,
                    .bucket_direct_matcher = bucket_direct_matcher,
                };
                const auto consume_counted_group = [this, &batch,
                        &counted_group_context, queries, max_occ_u32,
                        &have_matched_controls](
                            const query_occurrence_group& group) {
                    const std::uint32_t global_count =
                        occurrence_counter_<Oracle>(
                            counted_group_context,
                            group.counter_ordinal).load(
                                std::memory_order_relaxed);
                    have_matched_controls =
                        have_matched_controls || global_count != 0u;
                    if (global_count == 0u
                        || global_count > max_occ_u32) {
                        return;
                    }
                    const auto occurrence_error =
                        with_query_group_occurrences_<Oracle>(
                            counted_group_context,
                            group,
                            [this, &batch, queries, global_count](
                                    const auto occurrences,
                                    const query_occurrence_packing_layout& layout)
                                    noexcept
                                    -> std::optional<
                                        minimizer_generation_encoding_error> {
                                for (const auto& occurrence : occurrences) {
                                    const query_occurrence_reference item =
                                        query_occurrence_reference_(
                                            occurrence, layout);
                                    if (const auto error =
                                            add_expected_anchor_count_(
                                                item,
                                                queries,
                                                batch,
                                                global_count)) {
                                        return error;
                                    }
                                }
                                return std::nullopt;
                            });
                    if (occurrence_error) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native expected anchor count overflow");
                    }
                };
                if constexpr (Oracle) {
                    for (std::size_t plan_ordinal = 0u;
                         plan_ordinal < scratch.query_plans_.size();
                         ++plan_ordinal) {
                        query_hash_plan& plan =
                            scratch.query_plans_[plan_ordinal];
                        const query_occurrence_group group{
                            .hash = plan.hash,
                            .occurrence_ref_begin =
                                plan.occurrence_ref_begin,
                            .occurrence_ref_count =
                                plan.occurrence_ref_count,
                            .counter_ordinal =
                                static_cast<std::uint32_t>(plan_ordinal),
                        };
                        plan.global_occurrence_count =
                            occurrence_counter_<Oracle>(
                                counted_group_context,
                                static_cast<std::uint32_t>(
                                    plan_ordinal)).load(
                                    std::memory_order_relaxed);
                        consume_counted_group(group);
                    }
                } else {
                    DEBUG_ASSERT(bucket_direct_matcher != nullptr,
                        "production matcher view is prevalidated");
                    const bucket_direct_matcher_view& view =
                        *bucket_direct_matcher;
                    const auto consume_indexed_groups =
                        [&view, &consume_counted_group](
                                const auto* occurrences,
                                std::size_t occurrence_count) {
                            for (std::size_t group_ordinal = 0u;
                                 group_ordinal + 1u
                                    < view.group_offset_count;
                                 ++group_ordinal) {
                                const std::uint32_t begin =
                                    view.group_offsets[group_ordinal];
                                const std::uint32_t end =
                                    view.group_offsets[
                                        group_ordinal + 1u];
                                DEBUG_ASSERT(end > begin
                                        && end <= occurrence_count,
                                    "production group extent is prevalidated");
                                consume_counted_group(query_occurrence_group{
                                    .hash = occurrences[begin].hash,
                                    .occurrence_ref_begin = begin,
                                    .occurrence_ref_count = end - begin,
                                    .counter_ordinal =
                                        static_cast<std::uint32_t>(
                                            group_ordinal),
                                });
                            }
                        };
                    if (view.occurrence_layout.packed()) {
                        consume_indexed_groups(
                            view.packed_occurrences,
                            view.packed_occurrence_count);
                    } else {
                        consume_indexed_groups(
                            view.wide_occurrences,
                            view.wide_occurrence_count);
                    }
                }

                const bool have_accepted_anchors = std::ranges::any_of(
                    std::span<const prepared_query_anchors>{
                        batch.queries_.get(), batch.query_count_},
                    [](const prepared_query_anchors& query) noexcept {
                        return query.anchor_count != 0u;
                    });
                if (!have_accepted_anchors) {
                    // Matched heavy controls still require prefix/count
                    // authentication, but no layout, position walk, final
                    // allocation, emission pass, or sort is needed.
                    if (have_matched_controls) {
                        const std::uint64_t rejected_validation_started_ns =
                            stage_now_ns_();
                        first_error.store(0u, std::memory_order_relaxed);
                        std::atomic<std::uint64_t>
                            rejected_prefix_bytes{0u};
                        std::atomic<std::uint64_t>
                            rejected_matched_count_bytes{0u};
                        parallel_entries_(
                            block_count_,
                            [this, &scratch, &first_error,
                             &rejected_prefix_bytes,
                             &rejected_matched_count_bytes, max_occ_u32,
                             use_fixed_h32_c16_p16,
                             bucket_direct_matcher](
                                std::size_t block_begin,
                                std::size_t block_end) noexcept {
                            const std::span<const query_hash_plan> plans{
                                scratch.query_plans_};
                            const std::span<const std::uint32_t> radix{
                                scratch.query_plan_radix_};
                            for (std::size_t block_ordinal = block_begin;
                                 block_ordinal < block_end;
                                 ++block_ordinal) {
                                if (first_error.load(
                                        std::memory_order_relaxed) != 0u) {
                                    return;
                                }
                                std::uint64_t block_matched_count_bytes = 0u;
                                std::uint64_t block_prefix_bytes = 0u;
                                std::expected<void,
                                    minimizer_generation_encoding_error>
                                    validated{};
                                fragment_probe_context context{
                                    .pass = fragment_probe_pass::
                                        rejected_validation,
                                    .block = &block_plans_[block_ordinal],
                                    .block_ordinal = block_ordinal,
                                    .plans = plans,
                                    .query_plan_radix = radix,
                                    .scratch = &scratch,
                                    .bucket_direct_matcher =
                                        bucket_direct_matcher,
                                    .max_occ = max_occ_u32,
                                    .matched_count_bytes =
                                        &block_matched_count_bytes,
                                    .rejected_prefix_bytes =
                                        &block_prefix_bytes,
                                };
                                if (use_fixed_h32_c16_p16) {
                                    validated =
                                        probe_block_h32_c16_p16_<Oracle>(
                                            context);
                                } else {
                                    validated =
                                        visit_matched_controls_<Oracle>(
                                        block_plans_[block_ordinal],
                                        context,
                                        [](std::size_t,
                                           const query_occurrence_group&,
                                           const grouped_soa_decoded_control&)
                                                noexcept
                                                -> std::optional<
                                                    minimizer_generation_encoding_error> {
                                            return minimizer_generation_encoding_error::
                                                partition_id;
                                        },
                                        nullptr,
                                        &block_prefix_bytes);
                                }
                                if (!validated) {
                                    record_error_(
                                        first_error, validated.error());
                                    return;
                                }
                                if (!add_atomic_checked_(
                                        rejected_matched_count_bytes,
                                        block_matched_count_bytes)
                                    || !add_atomic_checked_(
                                        rejected_prefix_bytes,
                                        block_prefix_bytes)) {
                                    record_error_(
                                        first_error,
                                        minimizer_generation_encoding_error::
                                            arithmetic_overflow);
                                    return;
                                }
                            }
                            });
                        if (const auto error = recorded_error_(first_error)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native rejected-control validation failed; "
                                "wire_error="
                                + std::to_string(
                                    static_cast<unsigned>(*error)));
                        }
                        batch.stats_.rejected_control_validation_ns =
                            elapsed_stage_ns_(rejected_validation_started_ns);
                        batch.stats_.
                            rejected_control_validation_bytes =
                                use_fixed_h32_c16_p16
                                ? total_control_hash_scan_bytes_
                                : total_control_count_scan_bytes_;
                        if (!add_checked_(
                                batch.stats_.
                                    rejected_control_validation_bytes,
                                rejected_matched_count_bytes.load(
                                    std::memory_order_relaxed))
                            || !add_checked_(
                                batch.stats_.
                                    rejected_control_validation_bytes,
                                rejected_prefix_bytes.load(
                                    std::memory_order_relaxed))) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native rejected-control byte "
                                "accounting overflow");
                        }
                    }
                } else {

                // Layout state is activated only after Pass 1 has proved the
                // exact accepted anchor counts. This keeps failed count waves
                // from publishing/zeroing partially active layout owners.
                const std::uint64_t orientation_layout_started_ns =
                    stage_now_ns_();
                if (queries.size() != 0u
                    && block_count_
                        > (std::numeric_limits<std::size_t>::max)()
                            / queries.size()) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native block/query layout overflow");
                }
                const std::size_t layout_count =
                    block_count_ * queries.size();
                const std::size_t old_block_layout_capacity =
                    scratch.block_query_layouts_.capacity();
                scratch.ensure_vector_capacity_(
                    scratch.block_query_layouts_, layout_count);
                batch.stats_.block_layout_allocations =
                    scratch.block_query_layouts_.capacity()
                            > old_block_layout_capacity
                    ? 1u : 0u;
                batch.stats_.peak_block_layout_capacity_bytes =
                    saturating_mul(
                        scratch.block_query_layouts_.capacity(),
                        sizeof(block_query_anchor_layout));
                scratch.block_query_layouts_.resize(layout_count);
                std::fill(
                    scratch.block_query_layouts_.begin(),
                    scratch.block_query_layouts_.end(),
                    block_query_anchor_layout{});
                const std::size_t old_block_stats_capacity =
                    scratch.block_stats_.capacity();
                scratch.ensure_vector_capacity_(
                    scratch.block_stats_, block_count_);
                batch.stats_.block_stats_allocations =
                    scratch.block_stats_.capacity()
                            > old_block_stats_capacity
                    ? 1u : 0u;
                batch.stats_.peak_block_stats_capacity_bytes =
                    saturating_mul(
                        scratch.block_stats_.capacity(),
                        sizeof(fragment_block_wave_stats));
                scratch.block_stats_.resize(block_count_);
                std::fill(
                    scratch.block_stats_.begin(),
                    scratch.block_stats_.end(),
                    fragment_block_wave_stats{});

                // Pass 2: accepted positions only, counting orientation.
                first_error.store(0u, std::memory_order_relaxed);
                parallel_entries_(
                    block_count_,
                    [this, queries, &scratch, &first_error, max_occ_u32,
                     use_fixed_h32_c16_p16,
                     bucket_direct_matcher](
                            std::size_t block_begin,
                            std::size_t block_end) noexcept {
                        const std::span<const query_hash_plan> plans{
                            scratch.query_plans_};
                        const std::span<const std::uint32_t> radix{
                            scratch.query_plan_radix_};
                        for (std::size_t block_ordinal = block_begin;
                             block_ordinal < block_end;
                             ++block_ordinal) {
                            if (first_error.load(
                                    std::memory_order_relaxed) != 0u) {
                                return;
                            }
                            std::uint64_t block_matched_count_bytes = 0u;
                            std::uint64_t block_accepted_prefix_bytes = 0u;
                            std::uint64_t block_rejected_prefix_bytes = 0u;
                            std::uint64_t block_position_bytes = 0u;
                            std::expected<void,
                                minimizer_generation_encoding_error> counted{};
                            fragment_probe_context context{
                                .pass = fragment_probe_pass::orientation,
                                .block = &block_plans_[block_ordinal],
                                .block_ordinal = block_ordinal,
                                .plans = plans,
                                .query_plan_radix = radix,
                                .queries = queries,
                                .scratch = &scratch,
                                .bucket_direct_matcher =
                                    bucket_direct_matcher,
                                .max_occ = max_occ_u32,
                                .matched_count_bytes =
                                    &block_matched_count_bytes,
                                .accepted_prefix_bytes =
                                    &block_accepted_prefix_bytes,
                                .rejected_prefix_bytes =
                                    &block_rejected_prefix_bytes,
                                .position_bytes = &block_position_bytes,
                            };
                            if (use_fixed_h32_c16_p16) {
                                counted = probe_block_h32_c16_p16_<Oracle>(
                                    context);
                            } else {
                                counted = visit_matched_controls_<Oracle>(
                                    block_plans_[block_ordinal],
                                    context,
                                    [this, queries,
                                     &scratch, block_ordinal,
                                     &block_position_bytes, &context](
                                            std::size_t entry_ordinal,
                                            const query_occurrence_group& group,
                                            const grouped_soa_decoded_control&
                                                control) noexcept
                                            -> std::optional<
                                                minimizer_generation_encoding_error> {
                                        const entry_plan& entry_plan =
                                            entry_plans_[entry_ordinal];
                                        auto position_cursor =
                                            position_range_cursor::create(
                                                mapped_payload_,
                                                control,
                                                entry_plan);
                                        if (!position_cursor) {
                                            return position_cursor.error();
                                        }
                                        std::uint64_t target_strand_zero = 0u;
                                        std::uint64_t target_strand_one = 0u;
                                        std::uint64_t packed = 0u;
                                        while (position_cursor->next_dynamic(
                                                packed)) {
                                            std::uint64_t& count =
                                                (packed & 1u) == 0u
                                                ? target_strand_zero
                                                : target_strand_one;
                                            if (count
                                                    == (std::numeric_limits<
                                                        std::uint64_t>::max)()) {
                                                return minimizer_generation_encoding_error::
                                                    arithmetic_overflow;
                                            }
                                            ++count;
                                        }
                                        if (const auto error =
                                                position_cursor->error()) {
                                            return error;
                                        }
                                        std::uint64_t observed = 0u;
                                        if (!add_checked_(
                                                observed,
                                                target_strand_zero)
                                            || !add_checked_(
                                                observed,
                                                target_strand_one)
                                            || observed
                                                != control.position_end
                                                    - control.position_begin) {
                                            return minimizer_generation_encoding_error::
                                                arithmetic_overflow;
                                        }
                                        const std::uint64_t position_bytes =
                                            bioseq::common::saturating_mul(
                                                observed,
                                                entry_plan.
                                                    position_byte_width);
                                        if (position_bytes
                                                == (std::numeric_limits<
                                                    std::uint64_t>::max)()
                                            || !add_checked_(
                                                block_position_bytes,
                                                position_bytes)) {
                                            return minimizer_generation_encoding_error::
                                                arithmetic_overflow;
                                        }

                                        const orientation_occurrence_context
                                            occurrence_context{
                                                .queries = queries,
                                                .scratch = &scratch,
                                                .layout_base = block_ordinal
                                                    * queries.size(),
                                                .target_strand_zero =
                                                    target_strand_zero,
                                                .target_strand_one =
                                                    target_strand_one,
                                            };
                                        return with_query_group_occurrences_<
                                            Oracle>(
                                            context,
                                            group,
                                            [&occurrence_context](
                                                    const auto occurrences,
                                                    const query_occurrence_packing_layout&
                                                        packing_layout) noexcept
                                                    -> std::optional<
                                                        minimizer_generation_encoding_error> {
                                                for (const auto& occurrence
                                                     : occurrences) {
                                                    const query_occurrence_reference
                                                        item =
                                                            query_occurrence_reference_(
                                                                occurrence,
                                                                packing_layout);
                                                    if (const auto error =
                                                            accumulate_orientation_(
                                                                item,
                                                                occurrence_context)) {
                                                        return error;
                                                    }
                                                }
                                                return std::nullopt;
                                            });
                                    },
                                    &block_accepted_prefix_bytes,
                                    &block_rejected_prefix_bytes);
                            }
                            if (!counted) {
                                record_error_(
                                    first_error, counted.error());
                                return;
                            }
                            if (block_ordinal
                                    >= scratch.block_stats_.size()) {
                                record_error_(
                                    first_error,
                                    minimizer_generation_encoding_error::
                                        arithmetic_overflow);
                                return;
                            }
                            scratch.block_stats_[block_ordinal].
                                pass2_matched_count_bytes =
                                    block_matched_count_bytes;
                            scratch.block_stats_[block_ordinal].
                                pass2_accepted_prefix_bytes =
                                    block_accepted_prefix_bytes;
                            scratch.block_stats_[block_ordinal].
                                pass2_rejected_prefix_bytes =
                                    block_rejected_prefix_bytes;
                            scratch.block_stats_[block_ordinal].
                                pass2_position_bytes = block_position_bytes;
                        }
                    });
                if (const auto error = recorded_error_(first_error)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native orientation-layout pass failed; "
                        "wire_error="
                        + std::to_string(static_cast<unsigned>(*error)));
                }

                // Prefix [all forward blocks][all reverse blocks].
                for (std::size_t query_ordinal = 0u;
                     query_ordinal < queries.size();
                     ++query_ordinal) {
                    std::uint64_t cursor = 0u;
                    for (std::size_t block_ordinal = 0u;
                         block_ordinal < block_count_;
                         ++block_ordinal) {
                        block_query_anchor_layout& layout =
                            scratch.block_query_layouts_[
                                block_ordinal * queries.size()
                                + query_ordinal];
                        layout.forward_begin = cursor;
                        layout.forward_cursor = cursor;
                        if (!add_checked_(
                                cursor, layout.forward_count)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native forward anchor prefix overflow");
                        }
                    }
                    for (std::size_t block_ordinal = 0u;
                         block_ordinal < block_count_;
                         ++block_ordinal) {
                        block_query_anchor_layout& layout =
                            scratch.block_query_layouts_[
                                block_ordinal * queries.size()
                                + query_ordinal];
                        layout.reverse_begin = cursor;
                        layout.reverse_cursor = cursor;
                        if (!add_checked_(
                                cursor, layout.reverse_count)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native reverse anchor prefix overflow");
                        }
                    }
                    if (cursor
                            != batch.queries_[query_ordinal].
                                anchor_count
                        || cursor
                            > (std::numeric_limits<std::size_t>::max)()) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native anchor layout count disagrees");
                    }
                }

                std::uint64_t final_owned_bytes = batch.owned_bytes_;
                for (std::size_t query_ordinal = 0u;
                     query_ordinal < queries.size();
                     ++query_ordinal) {
                    const prepared_query_anchors& query =
                        batch.queries_[query_ordinal];
                    const std::uint64_t anchor_bytes = saturating_mul(
                        query.anchor_count,
                        sizeof(bioseq::chaining::anchor));
                    final_owned_bytes = saturating_add(
                        final_owned_bytes, anchor_bytes);
                    if (anchor_bytes
                            == (std::numeric_limits<
                                std::uint64_t>::max)()
                        || final_owned_bytes
                            == (std::numeric_limits<
                                std::uint64_t>::max)()
                        || query.anchor_count
                            > (std::numeric_limits<
                                std::size_t>::max)()
                                / sizeof(
                                    bioseq::chaining::anchor)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native exact anchor size overflow");
                    }
                }
                // Reserve the complete immutable result before allocating its
                // first per-query array. A later query can therefore never
                // discover an admission failure after earlier anchors exist.
                if (!batch.permit_.grow_to(final_owned_bytes)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native exact anchor admission failed");
                }
                try {
                    for (std::size_t query_ordinal = 0u;
                         query_ordinal < queries.size();
                         ++query_ordinal) {
                        prepared_query_anchors& query =
                            batch.queries_[query_ordinal];
                        if (query.anchor_count == 0u) continue;
                        query.anchors =
                            std::make_unique_for_overwrite<
                                bioseq::chaining::anchor[]>(
                                    static_cast<std::size_t>(
                                        query.anchor_count));
                    }
                } catch (const std::bad_alloc&) {
                    // Batch destruction releases every earlier query array
                    // before returning the aggregate permit.
                    throw persisted_minimizer_lookup_error(
                        "fragment-native exact anchor allocation failed");
                }
                batch.owned_bytes_ = final_owned_bytes;
                batch.permit_.shrink_to(batch.owned_bytes_);
                batch.stats_.orientation_layout_ns = elapsed_stage_ns_(
                    orientation_layout_started_ns);

                // Pass 3: direct exact emission. Primary and tie sorting use
                // separate joined fragment barriers below so their wall times
                // remain independently attributable.
                const std::uint64_t direct_emission_started_ns =
                    stage_now_ns_();
                first_error.store(0u, std::memory_order_relaxed);
                parallel_entries_(
                    block_count_,
                    [this, queries, &batch, &scratch, &first_error,
                     max_occ_u32, use_fixed_h32_c16_p16,
                     bucket_direct_matcher](
                            std::size_t block_begin,
                            std::size_t block_end) noexcept {
                        const std::span<const query_hash_plan> plans{
                            scratch.query_plans_};
                        const std::span<const std::uint32_t> radix{
                            scratch.query_plan_radix_};
                        for (std::size_t block_ordinal = block_begin;
                             block_ordinal < block_end;
                             ++block_ordinal) {
                            if (first_error.load(
                                    std::memory_order_relaxed) != 0u) {
                                return;
                            }
                            const fragment_block_plan& block =
                                block_plans_[block_ordinal];
                            std::uint64_t block_matched_count_bytes = 0u;
                            std::uint64_t block_accepted_prefix_bytes = 0u;
                            std::uint64_t block_position_bytes = 0u;
                            std::uint64_t block_anchors_constructed = 0u;
                            std::expected<void,
                                minimizer_generation_encoding_error> emitted{};
                            fragment_probe_context context{
                                .pass = fragment_probe_pass::emission,
                                .block = &block,
                                .block_ordinal = block_ordinal,
                                .plans = plans,
                                .query_plan_radix = radix,
                                .queries = queries,
                                .scratch = &scratch,
                                .bucket_direct_matcher =
                                    bucket_direct_matcher,
                                .batch = &batch,
                                .max_occ = max_occ_u32,
                                .matched_count_bytes =
                                    &block_matched_count_bytes,
                                .accepted_prefix_bytes =
                                    &block_accepted_prefix_bytes,
                                .position_bytes = &block_position_bytes,
                                .anchors_constructed =
                                    &block_anchors_constructed,
                            };
                            if (use_fixed_h32_c16_p16) {
                                emitted = probe_block_h32_c16_p16_<Oracle>(
                                    context);
                            } else {
                                emitted = visit_matched_controls_<Oracle>(
                                    block,
                                    context,
                                    [this, queries, &batch,
                                     &scratch, block_ordinal, &block,
                                     &block_position_bytes,
                                     &block_anchors_constructed, &context](
                                            std::size_t entry_ordinal,
                                            const query_occurrence_group& group,
                                            const grouped_soa_decoded_control&
                                                control) noexcept
                                            -> std::optional<
                                                minimizer_generation_encoding_error> {
                                        const entry_plan& entry_plan =
                                            entry_plans_[entry_ordinal];
                                        auto position_cursor =
                                            position_range_cursor::create(
                                                mapped_payload_,
                                                control,
                                                entry_plan);
                                        if (!position_cursor) {
                                            return position_cursor.error();
                                        }
                                        const direct_emission_occurrence_context
                                            occurrence_context{
                                                .queries = queries,
                                                .block = &block,
                                                .block_ordinal = block_ordinal,
                                                .scratch = &scratch,
                                                .batch = &batch,
                                        };
                                        const auto occurrence_error =
                                            with_query_group_occurrences_<
                                                Oracle>(
                                                context,
                                                group,
                                                [this, &position_cursor,
                                                 &occurrence_context](
                                                        const auto occurrences,
                                                        const query_occurrence_packing_layout&
                                                            packing_layout)
                                                        noexcept
                                                        -> std::optional<
                                                            minimizer_generation_encoding_error> {
                                                    std::optional<
                                                        minimizer_generation_encoding_error>
                                                        emit_error;
                                                    std::uint64_t packed = 0u;
                                                    while (position_cursor->
                                                            next_dynamic(
                                                                packed)) {
                                                        for (const auto&
                                                             occurrence
                                                             : occurrences) {
                                                            const query_occurrence_reference
                                                                item =
                                                                    query_occurrence_reference_(
                                                                        occurrence,
                                                                        packing_layout);
                                                            if (const auto error =
                                                                    emit_fragment_anchor_(
                                                                        packed,
                                                                        item,
                                                                        occurrence_context)) {
                                                                emit_error = error;
                                                                break;
                                                            }
                                                        }
                                                        if (emit_error) break;
                                                    }
                                                    if (const auto error =
                                                            position_cursor->
                                                                error()) {
                                                        return error;
                                                    }
                                                    return emit_error;
                                                });
                                        if (occurrence_error) {
                                            return occurrence_error;
                                        }
                                        const std::uint64_t decoded_positions =
                                            control.position_end
                                            - control.position_begin;
                                        const std::uint64_t position_bytes =
                                            bioseq::common::saturating_mul(
                                                decoded_positions,
                                                entry_plan.
                                                    position_byte_width);
                                        const std::uint64_t constructed =
                                            bioseq::common::saturating_mul(
                                                decoded_positions,
                                                group.occurrence_ref_count);
                                        if (position_bytes
                                                == (std::numeric_limits<
                                                    std::uint64_t>::max)()
                                            || constructed
                                                == (std::numeric_limits<
                                                    std::uint64_t>::max)()
                                            || !add_checked_(
                                                block_position_bytes,
                                                position_bytes)
                                            || !add_checked_(
                                                block_anchors_constructed,
                                                constructed)) {
                                            return minimizer_generation_encoding_error::
                                                arithmetic_overflow;
                                        }
                                        return std::nullopt;
                                    },
                                    &block_accepted_prefix_bytes,
                                    nullptr,
                                    /*validate_rejected_prefixes=*/false);
                            }
                            if (!emitted) {
                                record_error_(
                                    first_error, emitted.error());
                                return;
                            }
                            if (block_ordinal
                                    >= scratch.block_stats_.size()) {
                                record_error_(
                                    first_error,
                                    minimizer_generation_encoding_error::
                                        arithmetic_overflow);
                                return;
                            }
                            scratch.block_stats_[block_ordinal].
                                pass3_matched_count_bytes =
                                    block_matched_count_bytes;
                            scratch.block_stats_[block_ordinal].
                                pass3_accepted_prefix_bytes =
                                    block_accepted_prefix_bytes;
                            scratch.block_stats_[block_ordinal].
                                pass3_position_bytes = block_position_bytes;
                            scratch.block_stats_[block_ordinal].
                                anchors_constructed =
                                    block_anchors_constructed;

                            for (std::size_t query_ordinal = 0u;
                                 query_ordinal < queries.size();
                                 ++query_ordinal) {
                                block_query_anchor_layout& layout =
                                    scratch.block_query_layouts_[
                                        block_ordinal * queries.size()
                                        + query_ordinal];
                                if (layout.forward_begin
                                        > (std::numeric_limits<
                                            std::uint64_t>::max)()
                                            - layout.forward_count
                                    || layout.reverse_begin
                                        > (std::numeric_limits<
                                            std::uint64_t>::max)()
                                            - layout.reverse_count
                                    || layout.forward_cursor
                                        != layout.forward_begin
                                            + layout.forward_count
                                    || layout.reverse_cursor
                                        != layout.reverse_begin
                                            + layout.reverse_count) {
                                    record_error_(
                                        first_error,
                                        minimizer_generation_encoding_error::
                                            arithmetic_overflow);
                                    return;
                                }
                            }
                        }
                    });
                if (const auto error = recorded_error_(first_error)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native direct anchor emission failed; "
                        "wire_error="
                        + std::to_string(static_cast<unsigned>(*error)));
                }
                batch.stats_.direct_emission_ns = elapsed_stage_ns_(
                    direct_emission_started_ns);

                const auto run_local_order_stage =
                    [this, queries, &batch, &scratch, &first_error](
                            bool normalize_ties) {
                        first_error.store(0u, std::memory_order_relaxed);
                        parallel_entries_(
                            block_count_,
                            [queries, &batch, &scratch, &first_error,
                             normalize_ties,
                             authenticated_target_length = manifest_.target_length](
                                    std::size_t block_begin,
                                    std::size_t block_end) noexcept {
                                for (std::size_t block_ordinal = block_begin;
                                     block_ordinal < block_end;
                                     ++block_ordinal) {
                                    if (first_error.load(
                                            std::memory_order_relaxed) != 0u
                                        || block_ordinal
                                            >= scratch.block_stats_.size()) {
                                        if (block_ordinal
                                                >= scratch.block_stats_.
                                                    size()) {
                                            record_error_(
                                                first_error,
                                                minimizer_generation_encoding_error::
                                                    arithmetic_overflow);
                                        }
                                        return;
                                    }
                                    std::uint64_t block_order_bytes = 0u;
                                    for (std::size_t query_ordinal = 0u;
                                         query_ordinal < queries.size();
                                         ++query_ordinal) {
                                        const block_query_anchor_layout& layout =
                                            scratch.block_query_layouts_[
                                                block_ordinal * queries.size()
                                                + query_ordinal];
                                        auto& query =
                                            batch.queries_[query_ordinal];
                                        const auto order_slice =
                                            [normalize_ties,
                                             &block_order_bytes,
                                             authenticated_target_length](
                                                    bioseq::chaining::anchor*
                                                        anchors,
                                                    std::uint64_t count)
                                                    noexcept -> bool {
                                                if (count < 2u) return true;
                                                const auto stats =
                                                    normalize_ties
                                                    ? normalize_anchor_slice_ties_(
                                                        anchors,
                                                        static_cast<std::size_t>(
                                                            count))
                                                    : sort_anchor_slice_primary_(
                                                        anchors,
                                                        static_cast<std::size_t>(
                                                            count),
                                                        authenticated_target_length);
                                                const std::uint64_t bytes =
                                                    normalize_ties
                                                    ? stats.tie_bytes
                                                    : stats.primary_bytes;
                                                return !stats.saturated
                                                    && add_checked_(
                                                        block_order_bytes,
                                                        bytes);
                                            };
                                        if ((layout.forward_count > 1u
                                                && !order_slice(
                                                    query.anchors.get()
                                                        + static_cast<
                                                            std::size_t>(
                                                            layout.
                                                                forward_begin),
                                                    layout.forward_count))
                                            || (layout.reverse_count > 1u
                                                && !order_slice(
                                                    query.anchors.get()
                                                        + static_cast<
                                                            std::size_t>(
                                                            layout.
                                                                reverse_begin),
                                                    layout.reverse_count))) {
                                            record_error_(
                                                first_error,
                                                minimizer_generation_encoding_error::
                                                    arithmetic_overflow);
                                            return;
                                        }
                                    }
                                    fragment_block_wave_stats& block_stats =
                                        scratch.block_stats_[block_ordinal];
                                    if (normalize_ties) {
                                        block_stats.
                                            tie_normalization_bytes =
                                                block_order_bytes;
                                    } else {
                                        block_stats.local_sort_bytes =
                                            block_order_bytes;
                                    }
                                }
                            });
                        if (const auto error = recorded_error_(first_error)) {
                            throw persisted_minimizer_lookup_error(
                                std::string{normalize_ties
                                    ? "fragment-native tie normalization failed; "
                                    : "fragment-native local primary sort failed; "}
                                + "wire_error="
                                + std::to_string(
                                    static_cast<unsigned>(*error)));
                        }
                    };

                const std::uint64_t local_sort_started_ns = stage_now_ns_();
                run_local_order_stage(/*normalize_ties=*/false);
                batch.stats_.local_sort_ns = elapsed_stage_ns_(
                    local_sort_started_ns);
                const std::uint64_t tie_normalization_started_ns =
                    stage_now_ns_();
                run_local_order_stage(/*normalize_ties=*/true);
                batch.stats_.tie_normalization_ns = elapsed_stage_ns_(
                    tie_normalization_started_ns);

                std::uint64_t expected_constructed = 0u;
                for (std::size_t query_ordinal = 0u;
                     query_ordinal < queries.size();
                     ++query_ordinal) {
                    if (!add_checked_(
                            expected_constructed,
                            batch.queries_[query_ordinal].anchor_count)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native stats anchor total overflow");
                    }
                }
                batch.stats_.pass2_control_bytes =
                    use_fixed_h32_c16_p16
                    ? total_control_hash_scan_bytes_
                    : total_control_count_scan_bytes_;
                batch.stats_.pass3_control_bytes =
                    use_fixed_h32_c16_p16
                    ? total_control_hash_scan_bytes_
                    : total_control_count_scan_bytes_;
                for (const fragment_block_wave_stats& block_stats
                     : scratch.block_stats_) {
                    if (!add_checked_(
                            batch.stats_.pass2_control_bytes,
                            block_stats.pass2_matched_count_bytes)
                        || !add_checked_(
                            batch.stats_.pass2_accepted_prefix_bytes,
                            block_stats.pass2_accepted_prefix_bytes)
                        || !add_checked_(
                            batch.stats_.
                                rejected_control_validation_bytes,
                            block_stats.pass2_rejected_prefix_bytes)
                        || !add_checked_(
                            batch.stats_.pass2_position_bytes,
                            block_stats.pass2_position_bytes)
                        || !add_checked_(
                            batch.stats_.pass3_control_bytes,
                            block_stats.pass3_matched_count_bytes)
                        || !add_checked_(
                            batch.stats_.pass3_accepted_prefix_bytes,
                            block_stats.pass3_accepted_prefix_bytes)
                        || !add_checked_(
                            batch.stats_.pass3_position_bytes,
                            block_stats.pass3_position_bytes)
                        || !add_checked_(
                            batch.stats_.anchors_constructed,
                            block_stats.anchors_constructed)
                        || !add_checked_(
                            batch.stats_.local_sort_bytes,
                            block_stats.local_sort_bytes)
                    || !add_checked_(
                            batch.stats_.tie_normalization_bytes,
                            block_stats.tie_normalization_bytes)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native stats reduction overflow");
                    }
                }
                const std::uint64_t exact_final_bytes = saturating_mul(
                    expected_constructed,
                    sizeof(bioseq::chaining::anchor));
                const bool control_byte_equation =
                    use_fixed_h32_c16_p16
                    ? batch.stats_.pass2_control_bytes
                            >= total_control_hash_scan_bytes_
                        && batch.stats_.pass2_control_bytes
                            <= batch.stats_.control_count_bytes
                        && batch.stats_.pass3_control_bytes
                            >= total_control_hash_scan_bytes_
                        && batch.stats_.pass3_control_bytes
                            <= batch.stats_.pass2_control_bytes
                        && (batch.stats_.pass2_control_bytes
                                - total_control_hash_scan_bytes_)
                            % sizeof(std::uint16_t) == 0u
                        && (batch.stats_.pass3_control_bytes
                                - total_control_hash_scan_bytes_)
                            % sizeof(std::uint16_t) == 0u
                    : batch.stats_.pass2_control_bytes
                            == batch.stats_.control_count_bytes
                        && batch.stats_.pass3_control_bytes
                            == batch.stats_.control_count_bytes;
                if (!control_byte_equation
                    || batch.stats_.pass2_accepted_prefix_bytes
                        != batch.stats_.pass3_accepted_prefix_bytes
                    || batch.stats_.pass2_position_bytes
                        != batch.stats_.pass3_position_bytes
                    || batch.stats_.anchors_constructed
                        != expected_constructed
                    || exact_final_bytes
                        == (std::numeric_limits<std::uint64_t>::max)()) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native stats pass equation failed");
                }
                batch.stats_.exact_final_bytes = exact_final_bytes;
                batch.stats_.peak_exact_final_bytes = exact_final_bytes;
                }
            }

            // Compositional sortedness authority: the control-scan route fills
            // forward/reverse slices inside strictly ordered disjoint blocks;
            // the compact-direct route fills one exact forward and one exact
            // reverse slice per query. Both routes apply the same primary and
            // tie stages inside every resulting slice. canonical_anchor_less
            // encodes orientation as the ref_bits MSB, so the forward slices
            // followed by the reverse slices compose without a further O(A)
            // production traversal.
            batch.stats_.compositional_sortedness_receipts =
                queries.size();
#ifndef NDEBUG
            verify_compositional_order_for_testing = true;
#endif
            const auto less = bioseq::chaining::canonical_anchor_less{};
            for (std::size_t query_ordinal = 0u;
                 query_ordinal < queries.size();
                 ++query_ordinal) {
                prepared_query_anchors& query =
                    batch.queries_[query_ordinal];
                if (verify_compositional_order_for_testing) {
                    const std::uint64_t verification_bytes =
                        saturating_mul(
                            query.anchor_count,
                            sizeof(bioseq::chaining::anchor));
                    if (verification_bytes
                            == (std::numeric_limits<
                                std::uint64_t>::max)()
                        || !add_checked_(
                            batch.stats_.
                                final_sortedness_validation_bytes,
                            verification_bytes)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native anchor order verification overflow");
                    }
                    for (std::size_t anchor_ordinal = 1u;
                         anchor_ordinal
                            < static_cast<std::size_t>(
                                query.anchor_count);
                         ++anchor_ordinal) {
                        if (less(
                                query.anchors[anchor_ordinal],
                                query.anchors[anchor_ordinal - 1u])) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native anchor order stats failed");
                        }
                    }
                }
                query.sortedness_receipt =
                    prepared_fragment_anchor_batch::
                        sortedness_receipt_;
            }
            if (batch.owned_bytes_
                    == (std::numeric_limits<std::uint64_t>::max)()
                || batch.owned_bytes_ > batch.permit_.bytes()) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native anchor ownership stats failed");
            }
            if (!batch.stats_.forbidden_buffers_are_zero()
                || batch.stats_.exact_final_bytes
                    > batch.owned_bytes_) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native stats ownership stats failed");
            }
            batch.permit_.shrink_to(batch.owned_bytes_);
            batch.ready_ = true;
            scratch.wave_state_.store(
                query_wave_scratch::wave_state::outstanding,
                std::memory_order_release);
            return batch;
        } catch (...) {
            scratch.reset_failed_wave_();
            throw;
        }
    }

    [[nodiscard]] bool ready() const noexcept {
        return ready_;
    }

    [[nodiscard]] bool compact_direct_ready() const noexcept {
        return ready_ && compact_direct_.has_value();
    }

    [[nodiscard]] bool compact_direct_only() const noexcept {
        return ready_
            && backing_kind_ == backing_kind::compact_direct_only;
    }

    [[nodiscard]] std::size_t mapped_grouped_soa_payload_bytes()
            const noexcept {
        return mapped_payload_.size();
    }

    [[nodiscard]] std::size_t grouped_soa_directory_entry_count()
            const noexcept {
        return entry_count_;
    }

    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return manifest_.target_length;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] std::size_t fragment_block_count() const noexcept {
        // Preserve the direct reader's manifest-geometry observation without
        // representing those segments as retained fragment_block_plan owners.
        return backing_kind_ == backing_kind::compact_direct_only
            ? manifest_.position_segment_count
            : block_count_;
    }

    [[nodiscard]] std::uint32_t largest_fragment_block_entry_count()
            const noexcept {
        return largest_block_entry_count_;
    }

    [[nodiscard]] std::uint32_t oversized_segment_block_count()
            const noexcept {
        return oversized_segment_block_count_;
    }

    [[nodiscard]] grouped_soa_query_reader_memory_stats
    memory_stats() const noexcept {
        const std::uint64_t directory_bytes =
            directory_.owned_capacity_bytes();
        const std::uint64_t entry_plan_bytes =
            bioseq::common::saturating_mul(
            entry_count_, sizeof(entry_plan));
        const std::uint64_t block_plan_bytes =
            bioseq::common::saturating_mul(
                block_count_, sizeof(fragment_block_plan));
        return grouped_soa_query_reader_memory_stats{
            .capacity_accounting_passes = 1u,
            .global_position_storage_preparations = 0u,
            .global_position_storage_allocations = 0u,
            .position_append_growth_reservations = 0u,
            .retained_position_count = 0u,
            .retained_position_capacity = 0u,
            .query_record_size_bytes = 0u,
            .query_record_count = 0u,
            .query_record_capacity = 0u,
            .query_record_capacity_bytes = 0u,
            .query_record_position_capacity_bytes = 0u,
            .query_hash_capacity_bytes = 0u,
            .released_query_hash_capacity_bytes = 0u,
            .query_route_radix_bits = 0u,
            .query_route_radix_bucket_count = 0u,
            .query_route_radix_nonempty_bucket_count = 0u,
            .query_route_radix_max_bucket_size = 0u,
            .query_route_control_lookups = 0u,
            .query_route_hash_probes = 0u,
            .query_route_matches = 0u,
            .decoded_control_records = 0u,
            .retained_selected_control_records = 0u,
            .selected_control_size_bytes = 0u,
            .retained_selected_control_capacity_bytes = 0u,
            .current_selected_control_capacity_bytes = 0u,
            .directory_capacity_bytes = directory_bytes,
            .ordinal_route_capacity_bytes =
                bioseq::common::saturating_add(
                    entry_plan_bytes, block_plan_bytes),
            .selected_extent_capacity_bytes = 0u,
            .global_position_capacity_bytes = 0u,
            .control_payload_capacity_bytes = 0u,
            .owned_capacity_bytes = owned_bytes_,
            .saturated = owned_bytes_
                == (std::numeric_limits<std::uint64_t>::max)(),
        };
    }
};

static_assert(prepared_fragment_anchor_batch_reader<
    prepared_minimizer_fragment_reader>);
static_assert(prepared_fragment_anchor_query_index<
    prepared_fragment_anchor_batch::query_view>);
static_assert(!minimizer_lookup_index<
    prepared_minimizer_fragment_reader>);

/// Grouped-SoA generation controls.
/// Fixed-width limits of current controls and PostgreSQL key axes.
struct grouped_soa_generation_limits {
    std::uint64_t max_segment_control_count =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t max_segment_position_count =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t max_hash_position_count =
        (std::numeric_limits<std::uint32_t>::max)();
};

/// Immutable compatibility and source identity supplied to both producers.
struct grouped_soa_generation_options {
    minimizer_generation_layout layout =
        minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t target_length = 0;
    /// Requested persisted position geometry. Zero keeps the generic
    /// sequence-wide producer default. MPF supplies its physical fragment
    /// span so one persisted segment remains one independently queryable
    /// source-fragment index; MPS keeps the larger merge-oriented default.
    std::uint64_t position_segment_span_bp = 0;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    // Deliberately invalid: every writer must name the producer's scheme.
    std::uint16_t minimizer_scheme_id = k_invalid_minimizer_scheme_id;
    std::uint16_t minimizer_scheme_revision = k_invalid_minimizer_scheme_revision;
    /// Grouped-SoA count saturation threshold.
    std::uint64_t max_occ_cap = k_grouped_soa_default_max_occurrence_cap;
};

/// One canonical event supplied in `(segment,hash,encoded_position)` order.
struct canonical_minimizer_event {
    std::uint64_t hash = 0;
    std::uint64_t encoded_position = 0;

    [[nodiscard]] bool operator==(const canonical_minimizer_event&) const = default;
};

/// Type-erased callback used by independently replayable MPF/MPS producers.
struct canonical_minimizer_event_receiver {
    void* context = nullptr;
    std::expected<void, minimizer_generation_encoding_error> (*emit_fn)(
        void*, const canonical_minimizer_event&) = nullptr;

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    operator()(const canonical_minimizer_event& event) const {
        if (context == nullptr || emit_fn == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        return emit_fn(context, event);
    }
};

/// A producer that can replay a bounded/external sorted stream for each pass.
template <class Producer>
concept grouped_soa_event_producer = requires(
        Producer& producer,
        std::uint64_t position_segment_span_bp,
        canonical_minimizer_event_receiver receiver) {
    { producer.minimizer_scheme_identity() }
        -> std::same_as<minimizer_scheme_identity>;
    { producer.producer_backend_identity() }
        -> std::same_as<minimizer_producer_identity>;
    { producer.scan_sorted_events(position_segment_span_bp, receiver) }
        -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
};

template <class Sink>
concept minimizer_generation_row_sink = requires(
        Sink& sink,
        const minimizer_generation_row_view& row) {
    { sink.write(row) }
        -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
};

/// Immutable byte/layout contract offered before the first generation row is
/// emitted.  It deliberately contains no planner-owned spans: an optional
/// direct sink may retain this value while the common encoder remains the
/// authority for the eventual directory and manifest bytes.
struct grouped_soa_storage_plan_view {
    minimizer_generation_layout layout =
        minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t manifest_bytes = 0;
    std::uint64_t directory_offset = 0;
    std::uint64_t manifest_offset = 0;
    std::uint64_t file_bytes = 0;

    [[nodiscard]] constexpr bool operator==(
        const grouped_soa_storage_plan_view&) const noexcept = default;
};

template <class Sink>
concept planned_grouped_soa_generation_sink = requires(
        Sink& sink,
        const grouped_soa_storage_plan_view& storage_plan) {
    { sink.begin_planned_generation(storage_plan) }
        -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
};

/// grouped-SoA payload handoff before the address-bound row CRC exists. A sink
/// which implements the optional ownership method below becomes the sole CRC
/// calculator; non-PostgreSQL sinks use the checked adapter fallback.
struct unchecked_minimizer_generation_row_view {
    index_chunk_address address{};
    std::span<const std::byte> bytes;
};

/// Authoritative checksum returned by the sink which accepted one grouped-SoA
/// payload row. CRC zero is valid, so success is represented by expected rather
/// than a sentinel value.
struct minimizer_generation_row_write_receipt {
    std::uint64_t row_checksum = 0;
};

template <class Sink>
concept grouped_soa_minimizer_payload_sink = requires(
        Sink& sink,
        const unchecked_minimizer_generation_row_view& row) {
    { sink.write_grouped_soa_payload(row) }
        -> std::same_as<std::expected<
            minimizer_generation_row_write_receipt,
            minimizer_generation_encoding_error>>;
};

struct minimizer_generation_row_sink_ref {
    void* context = nullptr;
    std::expected<void, minimizer_generation_encoding_error> (*write_fn)(
        void*, const minimizer_generation_row_view&) = nullptr;
    std::expected<minimizer_generation_row_write_receipt,
        minimizer_generation_encoding_error> (*write_grouped_soa_payload_fn)(
            void*, const unchecked_minimizer_generation_row_view&) = nullptr;

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    write(const minimizer_generation_row_view& row) const {
        if (context == nullptr || write_fn == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        return write_fn(context, row);
    }

    /// Let an ownership-aware sink compute the grouped-SoA row CRC exactly once.
    /// The fallback preserves existing non-PostgreSQL sinks by computing once
    /// here and forwarding their unchanged checksummed row API.
    [[nodiscard]] std::expected<minimizer_generation_row_write_receipt,
        minimizer_generation_encoding_error>
    write_grouped_soa_payload(
            const unchecked_minimizer_generation_row_view& row) const {
        if (context == nullptr || write_fn == nullptr) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        if (write_grouped_soa_payload_fn != nullptr) {
            return write_grouped_soa_payload_fn(context, row);
        }
        const std::uint64_t row_checksum = crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            static_cast<std::uint32_t>(row.bytes.size()),
            row.bytes);
        if (auto written = write(minimizer_generation_row_view{
                .address = row.address,
                .bytes = row.bytes,
                .row_checksum = row_checksum,
            }); !written) {
            return std::unexpected(written.error());
        }
        return minimizer_generation_row_write_receipt{
            .row_checksum = row_checksum,
        };
    }
};

/// One bounded fixed metadata row (head or manifest chunk zero).
struct encoded_minimizer_fixed_row {
    index_chunk_address address{};
    std::array<std::byte,
        minimizer_generation_manifest::encoded_bytes> storage{};
    std::uint32_t byte_count = 0;
    std::uint64_t row_checksum = 0;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>{storage.data(), byte_count};
    }
};

/// Bounded metadata result; biological payload rows are never retained here.
struct encoded_grouped_soa_generation {
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    /// Publication is deliberately separate: this row is never sent to the
    /// generation sink by `encode_minimizer_generation`.
    encoded_minimizer_fixed_row unpublished_head_row{};
    encoded_minimizer_fixed_row manifest_row{};
    grouped_soa_directory_view directory{};
};

/// Immutable identity of the exact encoded generation whose execution stats
/// may be admitted to READY publication.  Route stats alone is deliberately
/// insufficient: two generations can execute the same backend over equal-sized
/// sources while naming different source epochs or publication rows.
struct minimizer_generation_publication_binding {
    minimizer_generation_layout layout =
        minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t compatibility_checksum = 0;
    std::uint64_t manifest_checksum = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return layout != minimizer_generation_layout::invalid
            && generation_id != 0 && source_identity != 0;
    }

    [[nodiscard]] constexpr bool operator==(
        const minimizer_generation_publication_binding&) const noexcept = default;
};

[[nodiscard]] constexpr minimizer_generation_publication_binding
minimizer_generation_publication_binding(
        const encoded_grouped_soa_generation& generation) noexcept {
    return minimizer_generation_publication_binding{
        .layout = generation.manifest.layout,
        .generation_id = generation.manifest.generation_id,
        .source_identity = generation.manifest.source_identity,
        .source_content_checksum =
            generation.manifest.source_content_checksum,
        .compatibility_checksum =
            minimizer_generation_compatibility_checksum(generation.manifest),
        .manifest_checksum = generation.head.manifest_checksum,
    };
}

struct grouped_soa_generation_plan {
    grouped_soa_minimizer_profile profile{};
    minimizer_producer_identity producer_backend{};
    std::uint64_t position_segment_span_bp = 0;
    std::uint32_t position_segment_count = 0;
    std::uint64_t canonical_event_count = 0;
    std::uint64_t canonical_event_checksum = 0;
    std::vector<grouped_soa_bucket_directory_entry> entries;
};

namespace minimizer_generation_stream_detail {

inline constexpr std::uint64_t k_planned_generation_image_header_bytes = 4096u;
inline constexpr std::uint64_t k_planned_generation_section_alignment = 8u;

[[nodiscard]] constexpr std::expected<
    std::uint64_t,
    minimizer_generation_encoding_error>
planned_storage_checked_add(
        std::uint64_t left,
        std::uint64_t right) noexcept {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return left + right;
}

[[nodiscard]] constexpr std::expected<
    std::uint64_t,
    minimizer_generation_encoding_error>
planned_storage_align_up(std::uint64_t value) noexcept {
    const std::uint64_t remainder =
        value % k_planned_generation_section_alignment;
    return planned_storage_checked_add(
        value,
        remainder == 0u
            ? 0u
            : k_planned_generation_section_alignment - remainder);
}

[[nodiscard]] inline std::expected<
    grouped_soa_storage_plan_view,
    minimizer_generation_encoding_error>
make_minimizer_generation_storage_plan(
        const grouped_soa_generation_options& options,
        const grouped_soa_generation_plan& plan) noexcept {
    if (options.layout == minimizer_generation_layout::invalid
        || plan.profile.layout != options.layout) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }

    std::uint64_t payload_bytes = 0u;
    for (const grouped_soa_bucket_directory_entry& entry : plan.entries) {
        const auto entry_bytes = planned_storage_checked_add(
            entry.control_encoded_bytes,
            entry.position_encoded_bytes);
        if (!entry_bytes) {
            return std::unexpected(entry_bytes.error());
        }
        const auto next_payload_bytes = planned_storage_checked_add(
            payload_bytes, *entry_bytes);
        if (!next_payload_bytes) {
            return std::unexpected(next_payload_bytes.error());
        }
        payload_bytes = *next_payload_bytes;
    }

    const std::size_t directory_entry_bytes =
        minimizer_directory_entry_encoded_bytes(plan.profile);
    const std::size_t manifest_encoded_bytes =
        minimizer_manifest_encoded_bytes(plan.profile.layout);
    if (directory_entry_bytes == 0u || manifest_encoded_bytes == 0u
        || !std::in_range<std::uint64_t>(plan.entries.size())
        || !std::in_range<std::uint64_t>(directory_entry_bytes)
        || !std::in_range<std::uint64_t>(manifest_encoded_bytes)) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t entry_count =
        static_cast<std::uint64_t>(plan.entries.size());
    const std::uint64_t directory_entry_bytes_u64 =
        static_cast<std::uint64_t>(directory_entry_bytes);
    if (entry_count != 0u
        && directory_entry_bytes_u64
            > (std::numeric_limits<std::uint64_t>::max)() / entry_count) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t directory_bytes =
        entry_count * directory_entry_bytes_u64;
    const std::uint64_t manifest_bytes =
        static_cast<std::uint64_t>(manifest_encoded_bytes);

    const auto payload_end = planned_storage_checked_add(
        k_planned_generation_image_header_bytes, payload_bytes);
    if (!payload_end) {
        return std::unexpected(payload_end.error());
    }
    const auto directory_offset = planned_storage_align_up(*payload_end);
    if (!directory_offset) {
        return std::unexpected(directory_offset.error());
    }
    const auto directory_end = planned_storage_checked_add(
        *directory_offset, directory_bytes);
    if (!directory_end) {
        return std::unexpected(directory_end.error());
    }
    const auto manifest_offset = planned_storage_align_up(*directory_end);
    if (!manifest_offset) {
        return std::unexpected(manifest_offset.error());
    }
    const auto file_bytes = planned_storage_checked_add(
        *manifest_offset, manifest_bytes);
    if (!file_bytes) {
        return std::unexpected(file_bytes.error());
    }
    return grouped_soa_storage_plan_view{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .payload_bytes = payload_bytes,
        .directory_bytes = directory_bytes,
        .manifest_bytes = manifest_bytes,
        .directory_offset = *directory_offset,
        .manifest_offset = *manifest_offset,
        .file_bytes = *file_bytes,
    };
}

inline void update_canonical_event_checksum(
        crc64_ecma182_accumulator& checksum,
        const canonical_minimizer_event& event) noexcept {
    std::array<std::byte, 16> encoded{};
    std::span<std::byte> bytes{encoded};
    minimizer_generation_wire_detail::write_le<std::uint64_t>(
        bytes, 0, event.hash);
    minimizer_generation_wire_detail::write_le<std::uint64_t>(
        bytes, 8, event.encoded_position);
    checksum.update(encoded);
}

[[nodiscard]] constexpr bool is_grouped_soa_payload_profile(
        const grouped_soa_minimizer_profile& profile) noexcept {
    return profile.layout
            == minimizer_generation_layout::grouped_soa_segment_local
        && validate_grouped_soa_minimizer_profile(profile).has_value();
}

[[nodiscard]] constexpr std::expected<
    std::uint64_t,
    minimizer_generation_encoding_error>
checked_payload_byte_count(
        std::uint64_t element_count,
        std::uint64_t encoded_byte_width) noexcept {
    if (encoded_byte_width == 0
        || element_count > (std::numeric_limits<std::uint64_t>::max)()
            / encoded_byte_width) {
        return std::unexpected(
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return element_count * encoded_byte_width;
}

[[nodiscard]] constexpr std::uint64_t grouped_soa_payload_chunk_count(
        std::uint64_t encoded_bytes) noexcept {
    constexpr std::uint32_t extent_payload_bytes =
        k_grouped_soa_extent_payload_bytes;
    static_assert(extent_payload_bytes != 0);
    return encoded_bytes / extent_payload_bytes
        + (encoded_bytes % extent_payload_bytes != 0 ? 1u : 0u);
}

/// grouped-SoA planning keeps the terminal prefix authoritative and derives the
/// per-entry O width before committing an extent. A hash group is indivisible:
/// an ordinary entry is flushed before a widening group, while a heavy group
/// owns the resulting wider entry by itself.
struct grouped_soa_plan_pass {
    std::uint64_t span = 0;
    std::uint64_t target_length = 0;
    grouped_soa_minimizer_profile profile{};
    grouped_soa_generation_limits limits{};
    std::vector<grouped_soa_bucket_directory_entry> entries;
    bool retry_with_smaller_span = false;
    bool has_event = false;
    std::uint64_t previous_segment = 0;
    canonical_minimizer_event previous_event{};
    std::uint64_t current_segment = 0;
    std::uint64_t current_hash = 0;
    std::uint64_t group_positions = 0;
    std::uint64_t segment_controls = 0;
    std::uint64_t segment_positions = 0;
    std::uint64_t partition_controls = 0;
    std::uint64_t partition_positions = 0;
    std::uint64_t partition_lo_hash = 0;
    std::uint64_t partition_hi_hash = 0;
    std::uint64_t canonical_event_count = 0;
    crc64_ecma182_accumulator canonical_event_checksum{};

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    flush_partition() {
        if (partition_controls == 0) return {};
        if (!sql_integer_can_represent(current_segment)
            || partition_controls
                > (std::numeric_limits<std::uint32_t>::max)()
            || entries.size()
                >= (std::numeric_limits<std::uint32_t>::max)() / 2u) {
            return std::unexpected(
                minimizer_generation_encoding_error::element_count);
        }
        const minimizer_scalar_width offset_width =
            select_grouped_soa_offset_width(partition_positions);
        const auto layout = plan_grouped_soa_control_plane(
            partition_controls,
            profile.hash_width,
            offset_width,
            profile.count_width);
        const auto position_bytes = checked_payload_byte_count(
            partition_positions,
            static_cast<std::uint8_t>(profile.position_width));
        if (!layout || !position_bytes) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t control_chunks =
            grouped_soa_payload_chunk_count(layout->encoded_bytes);
        const std::uint64_t position_chunks =
            grouped_soa_payload_chunk_count(*position_bytes);
        if (control_chunks
                > (std::numeric_limits<std::uint32_t>::max)()
            || position_chunks
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::chunk_range);
        }
        const std::uint32_t partition_id =
            static_cast<std::uint32_t>(entries.size());
        entries.push_back(grouped_soa_bucket_directory_entry{
            .partition_id = partition_id,
            .position_segment = static_cast<std::int32_t>(current_segment),
            .lo_hash = partition_lo_hash,
            .hi_hash = partition_hi_hash,
            .sub_key = static_cast<index_sub_key_t>(partition_id),
            .control_first_chunk = 0,
            .control_chunk_count = static_cast<std::uint32_t>(control_chunks),
            .position_first_chunk = 0,
            .position_chunk_count = static_cast<std::uint32_t>(position_chunks),
            .control_element_count = partition_controls,
            .position_element_count = partition_positions,
            .control_encoded_bytes = layout->encoded_bytes,
            .position_encoded_bytes = *position_bytes,
            .shape_offset_width = offset_width,
        });
        partition_controls = 0;
        partition_positions = 0;
        partition_lo_hash = 0;
        partition_hi_hash = 0;
        return {};
    }

    [[nodiscard]] std::expected<std::uint64_t,
        minimizer_generation_encoding_error>
    prospective_partition_bytes(
            std::uint64_t controls,
            std::uint64_t positions) const noexcept {
        const auto layout = plan_grouped_soa_control_plane(
            controls,
            profile.hash_width,
            select_grouped_soa_offset_width(positions),
            profile.count_width);
        const auto position_bytes = checked_payload_byte_count(
            positions,
            static_cast<std::uint8_t>(profile.position_width));
        if (!layout || !position_bytes
            || layout->encoded_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - *position_bytes) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return layout->encoded_bytes + *position_bytes;
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    finish_group() {
        if (group_positions == 0) return {};
        if (partition_positions
                > (std::numeric_limits<std::uint64_t>::max)()
                    - group_positions) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const auto prospective = prospective_partition_bytes(
            partition_controls + 1u,
            partition_positions + group_positions);
        if (!prospective) return std::unexpected(prospective.error());
        if (partition_controls != 0
            && (*prospective > k_minimizer_partition_target_bytes
                || select_grouped_soa_offset_width(partition_positions)
                    != select_grouped_soa_offset_width(
                        partition_positions + group_positions))) {
            if (auto flushed = flush_partition(); !flushed) return flushed;
        }
        if (partition_controls == 0) partition_lo_hash = current_hash;
        partition_hi_hash = current_hash;
        ++partition_controls;
        partition_positions += group_positions;
        group_positions = 0;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const canonical_minimizer_event& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        if (span == 0 || position >= target_length
            || event.hash > profile.max_hash) {
            return std::unexpected(event.hash > profile.max_hash
                ? minimizer_generation_encoding_error::hash_range
                : minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t segment = position / span;
        if (!sql_integer_can_represent(segment)) {
            return std::unexpected(
                minimizer_generation_encoding_error::position_range);
        }
        if (has_event) {
            const bool before = segment < previous_segment
                || (segment == previous_segment
                    && (event.hash < previous_event.hash
                        || (event.hash == previous_event.hash
                            && event.encoded_position
                                < previous_event.encoded_position)));
            if (before) {
                return std::unexpected(
                    minimizer_generation_encoding_error::row_order);
            }
            if (segment == previous_segment && event == previous_event) {
                return std::unexpected(
                    minimizer_generation_encoding_error::duplicate_event);
            }
            if (segment != current_segment || event.hash != current_hash) {
                if (auto group = finish_group(); !group) return group;
                if (segment != current_segment) {
                    if (auto flushed = flush_partition(); !flushed) {
                        return flushed;
                    }
                    current_segment = segment;
                    segment_controls = 0;
                    segment_positions = 0;
                }
                current_hash = event.hash;
                ++segment_controls;
            }
        } else {
            has_event = true;
            current_segment = segment;
            current_hash = event.hash;
            segment_controls = 1;
        }
        if (canonical_event_count
                == (std::numeric_limits<std::uint64_t>::max)()
            || segment_positions
                == (std::numeric_limits<std::uint64_t>::max)()
            || group_positions
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        ++canonical_event_count;
        update_canonical_event_checksum(canonical_event_checksum, event);
        ++segment_positions;
        ++group_positions;
        if (segment_controls > limits.max_segment_control_count) {
            retry_with_smaller_span = true;
            return std::unexpected(
                minimizer_generation_encoding_error::element_count);
        }
        previous_segment = segment;
        previous_event = event;
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish() {
        if (auto group = finish_group(); !group) return group;
        return flush_partition();
    }
};

[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
receive_grouped_soa_plan_event(
        void* context,
        const canonical_minimizer_event& event) {
    return static_cast<grouped_soa_plan_pass*>(context)->accept(event);
}

template <grouped_soa_event_producer Producer>
[[nodiscard]] inline bool producer_identity_matches(
        const Producer& producer,
        minimizer_scheme_identity scheme,
        minimizer_producer_identity backend) noexcept {
    return producer.minimizer_scheme_identity() == scheme
        && producer.producer_backend_identity() == backend;
}

struct grouped_soa_emit_pass {
    template <grouped_soa_event_producer Producer>
    [[nodiscard]] static std::expected<std::uint64_t,
        minimizer_generation_encoding_error>
    run(
            Producer& producer,
            const grouped_soa_generation_options& options,
            const grouped_soa_minimizer_profile& profile,
            grouped_soa_generation_plan& plan,
            const minimizer_generation_row_sink_ref& sink) {
        if (plan.profile != profile) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        crc64_ecma182_accumulator payload_row_checksums;
        grouped_soa_control_emit_pass controls{
            .options = options,
            .profile = profile,
            .span = plan.position_segment_span_bp,
            .entries = &plan.entries,
            .sink = sink,
            .payload_row_checksums = &payload_row_checksums,
        };
        if (!producer_identity_matches(
                producer, profile.minimizer_scheme,
                plan.producer_backend)) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        auto replay = producer.scan_sorted_events(
            plan.position_segment_span_bp == 0
                ? 1u : plan.position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = &controls,
                .emit_fn = [](void* context,
                              const canonical_minimizer_event& event) {
                    return static_cast<grouped_soa_control_emit_pass*>(context)
                        ->accept(event);
                },
            });
        if (!producer_identity_matches(
                producer, profile.minimizer_scheme,
                plan.producer_backend)) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        if (!replay) return std::unexpected(replay.error());
        if (auto finished = controls.finish(); !finished) {
            return std::unexpected(finished.error());
        }
        if (controls.canonical_event_count != plan.canonical_event_count
            || controls.canonical_event_checksum.checksum()
                != plan.canonical_event_checksum) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        if (producer.minimizer_scheme_identity() != profile.minimizer_scheme) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }

        grouped_soa_position_emit_pass positions{
            .options = options,
            .profile = profile,
            .span = plan.position_segment_span_bp,
            .entries = &plan.entries,
            .sink = sink,
            .payload_row_checksums = &payload_row_checksums,
        };
        if (!producer_identity_matches(
                producer, profile.minimizer_scheme,
                plan.producer_backend)) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        replay = producer.scan_sorted_events(
            plan.position_segment_span_bp == 0
                ? 1u : plan.position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = &positions,
                .emit_fn = [](void* context,
                              const canonical_minimizer_event& event) {
                    return static_cast<grouped_soa_position_emit_pass*>(context)
                        ->accept(event);
                },
            });
        if (!producer_identity_matches(
                producer, profile.minimizer_scheme,
                plan.producer_backend)) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        if (!replay) return std::unexpected(replay.error());
        if (auto finished = positions.finish(); !finished) {
            return std::unexpected(finished.error());
        }
        if (positions.canonical_event_count != plan.canonical_event_count
            || positions.canonical_event_checksum.checksum()
                != plan.canonical_event_checksum) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        return payload_row_checksums.checksum();
    }
};

template <grouped_soa_event_producer p_producer>
[[nodiscard]] inline std::expected<
    grouped_soa_generation_plan,
    minimizer_generation_encoding_error>
plan_grouped_soa_minimizer_generation(
        p_producer& producer,
        const grouped_soa_generation_options& options,
        const grouped_soa_minimizer_profile& profile,
        minimizer_producer_identity planned_backend,
        const grouped_soa_generation_limits& limits) {
    if (!is_grouped_soa_payload_profile(profile)) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    if (options.target_length == 0) {
        const auto empty_profile = derive_grouped_soa_minimizer_profile(
            profile.minimizer_scheme,
            options.alphabet_code,
            profile.kmer_size,
            1u,
            options.max_occ_cap);
        if (!empty_profile) {
            return std::unexpected(empty_profile.error());
        }
        grouped_soa_plan_pass empty_pass{
            .span = 1,
            .target_length = 0,
            .profile = *empty_profile,
            .limits = limits,
        };
        if (!producer_identity_matches(
                producer, empty_profile->minimizer_scheme,
                planned_backend)) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        const auto scanned = producer.scan_sorted_events(
            1,
            canonical_minimizer_event_receiver{
                .context = &empty_pass,
                .emit_fn = receive_grouped_soa_plan_event,
            });
        if (!producer_identity_matches(
                producer, empty_profile->minimizer_scheme,
                planned_backend)) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        if (!scanned) {
            return std::unexpected(scanned.error());
        }
        if (empty_pass.has_event) {
            return std::unexpected(
                minimizer_generation_encoding_error::position_range);
        }
        return grouped_soa_generation_plan{
            .profile = *empty_profile,
            /* Empty generations have no physical position geometry. Replay
             * uses a non-zero sentinel while the manifest publishes zero. */
            .position_segment_span_bp = 0u,
        };
    }

    std::uint64_t span = options.position_segment_span_bp == 0u
        ? k_minimizer_preferred_segment_span_bp
        : options.position_segment_span_bp;
    for (;;) {
        const std::uint64_t segment_count = options.target_length / span
            + (options.target_length % span != 0 ? 1u : 0u);
        if (!sql_integer_can_represent(segment_count)) {
            return std::unexpected(
                minimizer_generation_encoding_error::segment_count);
        }
        const auto attempt_profile = derive_grouped_soa_minimizer_profile(
            profile.minimizer_scheme,
            options.alphabet_code,
            profile.kmer_size,
            span,
            options.max_occ_cap);
        if (!attempt_profile) {
            return std::unexpected(attempt_profile.error());
        }
        grouped_soa_plan_pass pass{
            .span = span,
            .target_length = options.target_length,
            .profile = *attempt_profile,
            .limits = limits,
        };
        if (!producer_identity_matches(
                producer, attempt_profile->minimizer_scheme,
                planned_backend)) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        auto scanned = producer.scan_sorted_events(
            span,
            canonical_minimizer_event_receiver{
                .context = &pass,
                .emit_fn = receive_grouped_soa_plan_event,
            });
        if (!producer_identity_matches(
                producer, attempt_profile->minimizer_scheme,
                planned_backend)) {
            return std::unexpected(
                minimizer_generation_encoding_error::replay_mismatch);
        }
        if (scanned) {
            scanned = pass.finish();
        }
        if (scanned) {
            return grouped_soa_generation_plan{
                .profile = *attempt_profile,
                .position_segment_span_bp = span,
                .position_segment_count =
                    static_cast<std::uint32_t>(segment_count),
                .canonical_event_count = pass.canonical_event_count,
                .canonical_event_checksum =
                    pass.canonical_event_checksum.checksum(),
                .entries = std::move(pass.entries),
            };
        }
        if (!pass.retry_with_smaller_span || span == 1u) {
            return std::unexpected(scanned.error());
        }
        span /= 2u;
    }
}

template <grouped_soa_event_producer p_producer>
[[nodiscard]] inline std::expected<
    std::uint64_t,
    minimizer_generation_encoding_error>
emit_grouped_soa_minimizer_generation(
        p_producer& producer,
        const grouped_soa_generation_options& options,
        const grouped_soa_minimizer_profile& profile,
        grouped_soa_generation_plan& plan,
        const minimizer_generation_row_sink_ref& sink) {
    if (!is_grouped_soa_payload_profile(profile)
        || plan.profile != profile) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    if (!producer_identity_matches(
            producer, profile.minimizer_scheme,
            plan.producer_backend)) {
        return std::unexpected(
            minimizer_generation_encoding_error::replay_mismatch);
    }
    return grouped_soa_emit_pass::run(
        producer, options, profile, plan, sink);
}
[[nodiscard]] inline encoded_minimizer_fixed_row make_fixed_row(
        const index_chunk_address& address,
        std::span<const std::byte> bytes) {
    encoded_minimizer_fixed_row row{
        .address = address,
        .byte_count = static_cast<std::uint32_t>(bytes.size()),
    };
    std::copy(bytes.begin(), bytes.end(), row.storage.begin());
    row.row_checksum = crc64_index_chunk_row(
        address.generation_id,
        address.position_segment,
        address.sub_key,
        address.chunk_no,
        row.byte_count,
        row.bytes());
    return row;
}

} // namespace minimizer_generation_stream_detail

/// Deterministically plan fixed geometry without retaining biological events.
template <grouped_soa_event_producer p_producer>
[[nodiscard]] inline std::expected<
    grouped_soa_generation_plan,
    minimizer_generation_encoding_error>
plan_minimizer_generation(
        p_producer& producer,
        const grouped_soa_generation_options& options,
        const grouped_soa_generation_limits& limits = {}) {
    const minimizer_scheme_identity producer_scheme =
        producer.minimizer_scheme_identity();
    const minimizer_producer_identity producer_backend =
        producer.producer_backend_identity();
    const auto profile = derive_grouped_soa_minimizer_profile(
        producer_scheme,
        options.alphabet_code,
        options.kmer_size,
        options.target_length == 0
            ? 1u
            : options.position_segment_span_bp == 0u
                ? k_minimizer_preferred_segment_span_bp
                : options.position_segment_span_bp,
        options.max_occ_cap);
    if (options.layout
            != minimizer_generation_layout::grouped_soa_segment_local
        || !profile
        || !is_supported_minimizer_producer_backend_identity(
            producer_backend)
        || minimizer_scheme_identity_for_producer_backend(producer_backend)
            != producer_scheme
        || producer_scheme.id != options.minimizer_scheme_id
        || producer_scheme.revision != options.minimizer_scheme_revision) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    if (options.target_length != 0
        && (limits.max_segment_control_count == 0
            || limits.max_segment_position_count == 0
            || limits.max_hash_position_count == 0)) {
        return std::unexpected(minimizer_generation_encoding_error::element_count);
    }
    auto plan = minimizer_generation_stream_detail::
        plan_grouped_soa_minimizer_generation(
            producer, options, *profile, producer_backend, limits);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (producer.minimizer_scheme_identity() != producer_scheme
        || producer.producer_backend_identity() != producer_backend) {
        return std::unexpected(
            minimizer_generation_encoding_error::replay_mismatch);
    }
    plan->producer_backend = producer_backend;
    return plan;
}

/// Stream one immutable qualified generation into a callback-scoped row sink.
template <grouped_soa_event_producer p_producer,
          minimizer_generation_row_sink p_sink>
[[nodiscard]] inline std::expected<
    encoded_grouped_soa_generation,
    minimizer_generation_encoding_error>
encode_minimizer_generation(
        p_producer& producer,
        p_sink& sink,
        const grouped_soa_generation_options& options,
        const grouped_soa_generation_limits& limits = {}) {
    if (options.generation_id == 0
        || !sql_bigint_can_represent(options.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    const minimizer_scheme_identity requested_scheme{
        .id = options.minimizer_scheme_id,
        .revision = options.minimizer_scheme_revision,
    };
    const auto profile = derive_grouped_soa_minimizer_profile(
        requested_scheme,
        options.alphabet_code,
        options.kmer_size,
        options.target_length == 0
            ? 1u
            : options.position_segment_span_bp == 0u
                ? k_minimizer_preferred_segment_span_bp
                : options.position_segment_span_bp,
        options.max_occ_cap);
    if (options.layout
            != minimizer_generation_layout::grouped_soa_segment_local
        || !profile
        || options.storage_unit_byte_width == 0
        || options.kmer_size == 0 || options.window_size == 0
        || options.target_length > (std::uint64_t{1} << 63u)
        || !is_supported_minimizer_scheme_identity(
            options.minimizer_scheme_id,
            options.minimizer_scheme_revision)) {
        return std::unexpected(minimizer_generation_encoding_error::compatibility_mismatch);
    }
    static_assert(k_grouped_soa_extent_payload_bytes
        % grouped_soa_bucket_directory_entry::encoded_bytes == 0);
    if constexpr (requires {
            producer.bind_generation_layout(options.layout);
        }) {
        /* Stats-bearing producers freeze their successful replay count
         * before the planning scan.  Generic/vector producers need no hook. */
        producer.bind_generation_layout(options.layout);
    }
    auto plan = plan_minimizer_generation(producer, options, limits);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (!is_supported_minimizer_producer_backend_identity(
            plan->producer_backend)
        || producer.producer_backend_identity()
            != plan->producer_backend) {
        return std::unexpected(
            minimizer_generation_encoding_error::compatibility_mismatch);
    }
    const auto storage_plan = minimizer_generation_stream_detail::
        make_minimizer_generation_storage_plan(options, *plan);
    if (!storage_plan) {
        return std::unexpected(storage_plan.error());
    }
    if constexpr (planned_grouped_soa_generation_sink<p_sink>) {
        if (auto begun = sink.begin_planned_generation(*storage_plan);
            !begun) {
            return std::unexpected(begun.error());
        }
    }
    minimizer_generation_row_sink_ref sink_ref{
        .context = &sink,
        .write_fn = [](void* context, const minimizer_generation_row_view& row) {
            return static_cast<p_sink*>(context)->write(row);
        },
    };
    if constexpr (grouped_soa_minimizer_payload_sink<p_sink>) {
        sink_ref.write_grouped_soa_payload_fn = [](
                void* context,
                const unchecked_minimizer_generation_row_view& row) {
            return static_cast<p_sink*>(context)->write_grouped_soa_payload(row);
        };
    }
    auto payload_checksum = minimizer_generation_stream_detail::
        emit_grouped_soa_minimizer_generation(
            producer, options, plan->profile, *plan, sink_ref);
    if (!payload_checksum) {
        return std::unexpected(payload_checksum.error());
    }
    if (producer.producer_backend_identity() != plan->producer_backend) {
        return std::unexpected(
            minimizer_generation_encoding_error::replay_mismatch);
    }

    auto directory_bytes = encode_minimizer_bucket_directory(
        plan->entries, plan->profile);
    if (!directory_bytes) {
        return std::unexpected(directory_bytes.error());
    }
    if (directory_bytes->size() != storage_plan->directory_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::directory_size);
    }
    auto directory = grouped_soa_directory_view::decode(*directory_bytes, plan->profile);
    if (!directory) {
        return std::unexpected(directory.error());
    }
    const std::uint32_t extent_payload_bytes =
        minimizer_extent_payload_bytes(options.layout);
    if (extent_payload_bytes == 0) {
        return std::unexpected(minimizer_generation_encoding_error::format_revision);
    }
    const std::uint64_t directory_chunk_count = directory_bytes->size()
            / extent_payload_bytes
        + (directory_bytes->size() % extent_payload_bytes != 0 ? 1u : 0u);
    if (!sql_integer_can_represent(directory_chunk_count)) {
        return std::unexpected(minimizer_generation_encoding_error::chunk_range);
    }

    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    for (const grouped_soa_bucket_directory_entry& entry : plan->entries) {
        control_count = bioseq::common::saturating_add(
            control_count, entry.control_element_count);
        position_count = bioseq::common::saturating_add(
            position_count, entry.position_element_count);
        payload_bytes = bioseq::common::saturating_add(
            payload_bytes,
            bioseq::common::saturating_add(
                entry.control_encoded_bytes, entry.position_encoded_bytes));
    }
    if (plan->entries.size()
            > (std::numeric_limits<std::uint32_t>::max)() / 2u
        || control_count == (std::numeric_limits<std::uint64_t>::max)()
        || position_count == (std::numeric_limits<std::uint64_t>::max)()
        || payload_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || payload_bytes != storage_plan->payload_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    minimizer_generation_manifest manifest{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .source_identity = options.source_identity,
        .source_content_checksum = options.source_content_checksum,
        .target_length = options.target_length,
        .control_count = control_count,
        .position_count = position_count,
        .payload_bytes = payload_bytes,
        .payload_checksum = *payload_checksum,
        .directory_bytes = directory_bytes->size(),
        .directory_checksum = crc64_ecma182(*directory_bytes),
        .position_segment_count = plan->position_segment_count,
        .extent_count = static_cast<std::uint32_t>(plan->entries.size() * 2u),
        .directory_chunk_count = static_cast<std::uint32_t>(directory_chunk_count),
        .alphabet_code = options.alphabet_code,
        .storage_unit_byte_width = options.storage_unit_byte_width,
        .kmer_size = options.kmer_size,
        .window_size = options.window_size,
        .gpu_abi_revision = plan->profile.gpu_abi_revision,
        .position_segment_span_bp = plan->position_segment_span_bp,
        .segment_geometry_revision = k_minimizer_segment_geometry_revision,
        .minimizer_scheme_id = options.minimizer_scheme_id,
        .minimizer_scheme_revision = options.minimizer_scheme_revision,
        .producer_backend_id = plan->producer_backend.id,
        .producer_identity_revision = plan->producer_backend.revision,
        .hash_width = plan->profile.hash_width,
        .position_width = plan->profile.position_width,
        .count_width = plan->profile.count_width,
        .offset_policy = plan->profile.offset_policy,
        .control_layout = plan->profile.control_layout,
        .count_semantics = plan->profile.count_semantics,
        .position_semantics = plan->profile.position_semantics,
        .max_hash = plan->profile.max_hash,
        .max_occ_cap = plan->profile.max_occ_cap,
    };
    const auto manifest_bytes =
        encode_minimizer_generation_manifest_record(manifest);
    if (!manifest_bytes) {
        return std::unexpected(manifest_bytes.error());
    }
    if (manifest_bytes->size() != storage_plan->manifest_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    minimizer_generation_head head{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .source_identity = options.source_identity,
        .compatibility_checksum =
            minimizer_generation_compatibility_checksum(manifest),
        .manifest_checksum = crc64_ecma182(
            std::span<const std::byte>{*manifest_bytes}),
        .source_content_checksum = options.source_content_checksum,
    };
    const auto head_bytes = encode_minimizer_generation_head(head);
    if (!head_bytes) {
        return std::unexpected(head_bytes.error());
    }

    const encoded_minimizer_fixed_row manifest_row =
        minimizer_generation_stream_detail::make_fixed_row(
            index_chunk_address{
                .generation_id = options.generation_id,
                .position_segment = -1,
                .sub_key = k_directory_sub_key,
                .chunk_no = 0,
            },
            *manifest_bytes);
    const encoded_minimizer_fixed_row unpublished_head_row =
        minimizer_generation_stream_detail::make_fixed_row(
            index_chunk_address{
                .generation_id = 0,
                .position_segment = -1,
                .sub_key = k_generation_head_sub_key,
                .chunk_no = 0,
            },
            *head_bytes);
    if (auto metadata = validate_minimizer_generation_metadata(
            head, manifest, *directory); !metadata) {
        return std::unexpected(metadata.error());
    }

    // Publication boundary: payload was written by emit_pass above; only the
    // directory and READY manifest are appended here. The active head is
    // returned to the caller and must be published only after a storage
    // reread validates this complete generation.
    for (std::size_t offset = 0, chunk = 1;
         offset < directory_bytes->size();
         offset += extent_payload_bytes, ++chunk) {
        const std::size_t count = std::min<std::size_t>(
            extent_payload_bytes, directory_bytes->size() - offset);
        const auto bytes = std::span<const std::byte>{*directory_bytes}
            .subspan(offset, count);
        const index_chunk_address address{
            .generation_id = options.generation_id,
            .position_segment = -1,
            .sub_key = k_directory_sub_key,
            .chunk_no = static_cast<std::uint32_t>(chunk),
        };
        const std::uint64_t checksum = crc64_index_chunk_row(
            address.generation_id, address.position_segment,
            address.sub_key, address.chunk_no,
            static_cast<std::uint32_t>(bytes.size()), bytes);
        if (auto written = sink.write(minimizer_generation_row_view{
                .address = address, .bytes = bytes, .row_checksum = checksum});
            !written) {
            return std::unexpected(written.error());
        }
    }
    if (auto written = sink.write(minimizer_generation_row_view{
            .address = manifest_row.address,
            .bytes = manifest_row.bytes(),
            .row_checksum = manifest_row.row_checksum}); !written) {
        return std::unexpected(written.error());
    }
    return encoded_grouped_soa_generation{
        .head = head,
        .manifest = manifest,
        .unpublished_head_row = unpublished_head_row,
        .manifest_row = manifest_row,
        .directory = std::move(*directory),
    };
}

/// Streaming complete-generation validator for ordered qualified rows.
class minimizer_generation_row_validator {
private:
    struct validated_tag {};

    encoded_grouped_soa_generation generation_{};
    std::size_t entry_index_ = 0;
    std::uint32_t payload_chunk_ = 0;
    std::uint32_t directory_chunk_ = 1;
    crc64_ecma182_accumulator extent_crc_{};
    crc64_ecma182_accumulator payload_row_checksums_{};
    bool position_payload_stage_ = false;
    bool manifest_seen_ = false;
    bool ready_validated_ = false;

    explicit minimizer_generation_row_validator(
            validated_tag,
            const encoded_grouped_soa_generation& generation)
        : generation_(generation) {}

    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    validate_fixed_row(
            const encoded_minimizer_fixed_row& row,
            const index_chunk_address& expected_address,
            std::span<const std::byte> expected_bytes) noexcept {
        if (row.address != expected_address
            || row.byte_count != expected_bytes.size()
            || row.byte_count > row.storage.size()
            || !std::ranges::equal(row.bytes(), expected_bytes)) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        const std::uint64_t expected_checksum = crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            row.byte_count,
            row.bytes());
        if (row.row_checksum != expected_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        return {};
    }

public:
    /// Validate every metadata equation and both fixed rows before any caller
    /// can enter row/subspan arithmetic.
    [[nodiscard]] static std::expected<
        minimizer_generation_row_validator,
        minimizer_generation_encoding_error>
    create(const encoded_grouped_soa_generation& generation) {
        if (auto metadata = validate_minimizer_generation_metadata(
                generation.head,
                generation.manifest,
                generation.directory); !metadata) {
            return std::unexpected(metadata.error());
        }
        const auto manifest_bytes =
            encode_minimizer_generation_manifest_record(generation.manifest);
        const auto head_bytes = encode_minimizer_generation_head(generation.head);
        if (!manifest_bytes || !head_bytes) {
            return std::unexpected(!manifest_bytes
                ? manifest_bytes.error() : head_bytes.error());
        }
        if (auto fixed = validate_fixed_row(
                generation.manifest_row,
                index_chunk_address{
                    .generation_id = generation.manifest.generation_id,
                    .position_segment = -1,
                    .sub_key = k_directory_sub_key,
                    .chunk_no = 0,
                },
                *manifest_bytes); !fixed) {
            return std::unexpected(fixed.error());
        }
        if (auto fixed = validate_fixed_row(
                generation.unpublished_head_row,
                index_chunk_address{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = k_generation_head_sub_key,
                    .chunk_no = 0,
                },
                *head_bytes); !fixed) {
            return std::unexpected(fixed.error());
        }
        for (const grouped_soa_bucket_directory_entry& entry
             : generation.directory.entries()) {
            if (entry.control_chunk_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                        - entry.position_chunk_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        return minimizer_generation_row_validator{validated_tag{}, generation};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const minimizer_generation_row_view& row) noexcept {
        if (ready_validated_ || manifest_seen_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        const std::uint32_t extent_payload_bytes =
            minimizer_extent_payload_bytes(generation_.manifest.layout);
        if (extent_payload_bytes == 0) {
            return std::unexpected(minimizer_generation_encoding_error::format_revision);
        }
        if (entry_index_ < generation_.directory.entries().size()) {
            const grouped_soa_bucket_directory_entry& entry =
                generation_.directory.entries()[entry_index_];
            if (entry.control_chunk_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                        - entry.position_chunk_count) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const bool grouped_soa = generation_.manifest.layout
                == minimizer_generation_layout::
                    grouped_soa_segment_local;
            const bool control = grouped_soa
                ? !position_payload_stage_
                : payload_chunk_ < entry.control_chunk_count;
            const std::uint32_t extent_chunks = control
                ? entry.control_chunk_count : entry.position_chunk_count;
            const std::uint32_t total_chunks = grouped_soa
                ? extent_chunks
                : entry.control_chunk_count + entry.position_chunk_count;
            if (payload_chunk_ >= total_chunks) {
                return std::unexpected(minimizer_generation_encoding_error::row_count);
            }
            const auto position_sub_key =
                derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key) {
                return std::unexpected(position_sub_key.error());
            }
            const std::uint32_t extent_chunk = control
                ? payload_chunk_
                : grouped_soa ? payload_chunk_
                          : payload_chunk_ - entry.control_chunk_count;
            const std::uint64_t extent_bytes = control
                ? entry.control_encoded_bytes : entry.position_encoded_bytes;
            if (extent_chunk
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / extent_payload_bytes) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t offset =
                static_cast<std::uint64_t>(extent_chunk)
                * extent_payload_bytes;
            if (offset >= extent_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::byte_count);
            }
            const std::uint64_t expected_bytes = std::min<std::uint64_t>(
                extent_payload_bytes, extent_bytes - offset);
            if (row.address.generation_id != generation_.manifest.generation_id
                || row.address.position_segment != entry.position_segment
                || row.address.sub_key != (control
                    ? entry.sub_key : *position_sub_key)
                || row.address.chunk_no != extent_chunk
                || row.bytes.size() != expected_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            const std::uint64_t expected_checksum = crc64_index_chunk_row(
                row.address.generation_id, row.address.position_segment,
                row.address.sub_key, row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()), row.bytes);
            if (row.row_checksum != expected_checksum) {
                return std::unexpected(minimizer_generation_encoding_error::checksum);
            }
            extent_crc_.update(row.bytes);
            payload_row_checksums_.update_little_endian(row.row_checksum);
            ++payload_chunk_;
            const bool extent_complete = grouped_soa
                ? payload_chunk_ == extent_chunks
                : payload_chunk_ == (control
                    ? entry.control_chunk_count : total_chunks);
            if (extent_complete && control) {
                if (extent_crc_.checksum() != entry.control_checksum) {
                    return std::unexpected(minimizer_generation_encoding_error::checksum);
                }
                extent_crc_ = {};
            }
            if (extent_complete && !control) {
                if (extent_crc_.checksum() != entry.position_checksum) {
                    return std::unexpected(minimizer_generation_encoding_error::checksum);
                }
                extent_crc_ = {};
                payload_chunk_ = 0;
                ++entry_index_;
            }
            if (grouped_soa && extent_complete && control) {
                payload_chunk_ = 0;
                ++entry_index_;
                if (entry_index_ == generation_.directory.entries().size()) {
                    entry_index_ = 0;
                    position_payload_stage_ = true;
                }
            }
            return {};
        }
        if (directory_chunk_ <= generation_.manifest.directory_chunk_count) {
            const std::uint64_t chunk_index =
                static_cast<std::uint64_t>(directory_chunk_ - 1u);
            if (chunk_index
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / extent_payload_bytes) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t offset64 =
                chunk_index * extent_payload_bytes;
            if (offset64 >= generation_.directory.encoded_bytes().size()
                || offset64
                    > (std::numeric_limits<std::size_t>::max)()) {
                return std::unexpected(minimizer_generation_encoding_error::directory_size);
            }
            const std::size_t offset = static_cast<std::size_t>(offset64);
            const std::size_t count = std::min<std::size_t>(
                extent_payload_bytes,
                generation_.directory.encoded_bytes().size() - offset);
            const auto expected_bytes = generation_.directory.encoded_bytes()
                .subspan(offset, count);
            if (row.address != index_chunk_address{
                    .generation_id = generation_.manifest.generation_id,
                    .position_segment = -1,
                    .sub_key = k_directory_sub_key,
                    .chunk_no = directory_chunk_,
                }
                || !std::ranges::equal(row.bytes, expected_bytes)) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            const std::uint64_t checksum = crc64_index_chunk_row(
                row.address.generation_id, row.address.position_segment,
                row.address.sub_key, row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()), row.bytes);
            if (row.row_checksum != checksum) {
                return std::unexpected(minimizer_generation_encoding_error::checksum);
            }
            ++directory_chunk_;
            return {};
        }
        if (!manifest_seen_) {
            if (row.address != generation_.manifest_row.address
                || row.row_checksum != generation_.manifest_row.row_checksum
                || !std::ranges::equal(row.bytes,
                    generation_.manifest_row.bytes())) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            manifest_seen_ = true;
            return {};
        }
        return std::unexpected(minimizer_generation_encoding_error::row_count);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    finish_ready() noexcept {
        if (ready_validated_) {
            return {};
        }
        if (entry_index_ != generation_.directory.entries().size()
            || payload_chunk_ != 0
            || (generation_.manifest.layout
                    == minimizer_generation_layout::
                        grouped_soa_segment_local
                && !generation_.directory.entries().empty()
                && !position_payload_stage_)
            || directory_chunk_
                != generation_.manifest.directory_chunk_count + 1u
            || !manifest_seen_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        if (payload_row_checksums_.checksum()
            != generation_.manifest.payload_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        ready_validated_ = true;
        return {};
    }

    /// Validate the separately published active-head row only after READY was
    /// completely reread and accepted.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    validate_publication_head(
            const minimizer_generation_row_view& row) const noexcept {
        if (!ready_validated_) {
            return std::unexpected(minimizer_generation_encoding_error::row_count);
        }
        const encoded_minimizer_fixed_row& expected =
            generation_.unpublished_head_row;
        if (row.address != expected.address
            || row.row_checksum != expected.row_checksum
            || !std::ranges::equal(row.bytes, expected.bytes())) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        return {};
    }
};

inline void index_corruption_panic(std::string_view msg) {
    bioseq_log_progress<logging_level::error>(
        "[bioseq index corruption] {}", msg);
    std::abort();
}

/// Caller-owned per-row/per-query assembly scratch (§9.1). Reused across
/// segments within one query, destroyed with its IndexReadContext.
struct minimizer_assembly_scratch {
    std::vector<std::pair<std::uint64_t,std::uint64_t>> pairs;
    std::vector<std::uint64_t> query_kmers;
    std::vector<std::uint64_t> bucket_positions;
    std::vector<bioseq::indexing::index_sub_key_t> wanted_sub_keys;
    void clear_segment() noexcept { pairs.clear(); }
    void clear_query() noexcept { pairs.clear(); query_kmers.clear(); bucket_positions.clear(); wanted_sub_keys.clear(); }
};

// ---- shared wire walkers (§9.4) ------------------------------------------

/// Walk one persisted wire blob and emit each key/position pair.
template <class F>   // F: (uint64 kmer, uint64 encoded_pos) -> void
void wire_for_each_entry(const char* blob, std::size_t blob_bytes, F&& emit) {
    using header = minimizer_hashmap_fragment_index::encoded_header;
    using bucket_record = minimizer_hashmap_fragment_index::encoded_bucket;
    if (blob_bytes < sizeof(header)) index_corruption_panic("index chunk: truncated header");
    header header{}; std::memcpy(&header, blob, sizeof(header));
    if (header.magic != minimizer_hashmap_fragment_index::k_encoded_magic ||
        header.revision != minimizer_hashmap_fragment_index::k_encoded_format_revision) index_corruption_panic("index chunk: magic/revision");
    const std::size_t expect = sizeof(header) + sizeof(bucket_record)*header.bucket_count + sizeof(std::uint64_t)*header.total_positions;
    if (blob_bytes != expect) index_corruption_panic("index chunk: size mismatch (corruption)");
    const char* recs = blob + sizeof(header);
    const char* pos  = recs + sizeof(bucket_record) * header.bucket_count;
    for (std::uint32_t bucket_no = 0; bucket_no < header.bucket_count; ++bucket_no) {
        bucket_record bucket{}; std::memcpy(&bucket, recs + sizeof(bucket_record) * bucket_no, sizeof(bucket_record));
        if ((std::uint64_t)bucket.pos_offset + bucket.pos_count > header.total_positions)
            index_corruption_panic("index chunk: bucket range exceeds total_positions");
        for (std::uint32_t pos_no = 0; pos_no < bucket.pos_count; ++pos_no) {
            std::uint64_t encoded{};
            std::memcpy(&encoded, pos + sizeof(std::uint64_t) * (bucket.pos_offset + pos_no), sizeof(encoded));
            emit(bucket.hash, encoded);
        }
    }
}

/// PER-KMER sibling of wire_for_each_entry: cb(kmer, pos_count, gpos).
template <class F>   // F: (uint64 kmer, uint32 pos_count, const uint64* gpos) -> void
void wire_for_each_bucket(const char* blob, std::size_t blob_bytes,
                          std::vector<std::uint64_t>& pos_scratch, F&& cb) {
    using header = minimizer_hashmap_fragment_index::encoded_header;
    using bucket_record = minimizer_hashmap_fragment_index::encoded_bucket;
    if (blob_bytes < sizeof(header)) index_corruption_panic("index chunk: truncated header");
    header header{}; std::memcpy(&header, blob, sizeof(header));
    if (header.magic != minimizer_hashmap_fragment_index::k_encoded_magic ||
        header.revision != minimizer_hashmap_fragment_index::k_encoded_format_revision) index_corruption_panic("index chunk: magic/revision");
    const std::size_t expect = sizeof(header) + sizeof(bucket_record)*header.bucket_count + sizeof(std::uint64_t)*header.total_positions;
    if (blob_bytes != expect) index_corruption_panic("index chunk: size mismatch (corruption)");
    const char* recs = blob + sizeof(header);
    const char* pos  = recs + sizeof(bucket_record) * header.bucket_count;
    for (std::uint32_t bucket_no = 0; bucket_no < header.bucket_count; ++bucket_no) {
        bucket_record bucket{}; std::memcpy(&bucket, recs + sizeof(bucket_record) * bucket_no, sizeof(bucket_record));
        if ((std::uint64_t)bucket.pos_offset + bucket.pos_count > header.total_positions)
            index_corruption_panic("index chunk: bucket range exceeds total_positions");
        pos_scratch.resize(bucket.pos_count);
        if (bucket.pos_count > 0)
            std::memcpy(pos_scratch.data(), pos + sizeof(std::uint64_t) * bucket.pos_offset, sizeof(std::uint64_t) * bucket.pos_count);
        cb(bucket.hash, bucket.pos_count, pos_scratch.data());
    }
}

// GPU batched minimizer-fragment helpers. A GPU window covers as many whole
// fragments as fit both the run buffer and the 28-bit position field (with halo).
[[nodiscard]] inline std::size_t gpu_mpf_fragments_per_batch(
    std::size_t nfrag,
    std::size_t frag_bp,
    std::size_t halo_bp,
    std::size_t run_buffer_bytes) noexcept
{
    constexpr std::size_t max_bp =
        static_cast<std::size_t>(
            minimap_sketch_data::k_position_field_mask);
    if (nfrag == 0u || frag_bp == 0u) {
        return 1u;
    }
    constexpr std::size_t gpu_segment_local_size = 128u;
    constexpr std::size_t gpu_segment_max_workgroups = 65535u;
    constexpr std::size_t gpu_segment_bytes_per_bp = 96u;
    const std::size_t by_memory = std::max<std::size_t>(
        1u,
        run_buffer_bytes
            / std::max<std::size_t>(
                1u,
                frag_bp * gpu_segment_bytes_per_bp));
    const std::size_t by_pos =
        halo_bp >= max_bp
            ? 1u
            : std::max<std::size_t>(1u, (max_bp - halo_bp) / frag_bp);
    const std::size_t max_dispatch_bp = gpu_segment_local_size * gpu_segment_max_workgroups;
    const std::size_t by_dispatch =
        halo_bp >= max_dispatch_bp
            ? 1u
            : std::max<std::size_t>(1u, (max_dispatch_bp - halo_bp) / frag_bp);
    return std::min({nfrag, by_memory, by_pos, by_dispatch});
}

// Serialize already (kmer, encoded_pos)-sorted pairs (from the GPU sorted-pair
// seam: fused GPU minimizer emit + host pair sort) into the existing mpf
// per-fragment blob format (header + bucket directory + positions). CPU-only
// persistence -- no minimizer collection or sort HERE.
inline void serialize_sorted_pairs_to_fragment_blob(
    std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted_pairs,
    sequence_size_t target_len,
    std::vector<char>& out_blob)
{
    using header = minimizer_hashmap_fragment_index::encoded_header;
    using bucket = minimizer_hashmap_fragment_index::encoded_bucket;

    std::vector<bucket> buckets;
    std::vector<std::uint64_t> positions;
    buckets.reserve(sorted_pairs.size() / 2u + 1u);
    positions.reserve(sorted_pairs.size());

    std::size_t index = 0;
    while (index < sorted_pairs.size()) {
        const std::uint64_t kmer = sorted_pairs[index].first;
        const std::uint32_t offset =
            static_cast<std::uint32_t>(positions.size());
        std::uint32_t count = 0;
        while (index < sorted_pairs.size() && sorted_pairs[index].first == kmer) {
            positions.push_back(sorted_pairs[index].second);
            ++count;
            ++index;
            if (count == UINT32_MAX) {
                index_corruption_panic(
                    "minimizers_per_fragment GPU blob: kmer count exceeds u32");
            }
        }
        buckets.push_back(bucket{kmer, offset, count});
    }

    const header header{
        minimizer_hashmap_fragment_index::k_encoded_magic,
        minimizer_hashmap_fragment_index::k_encoded_format_revision,
        static_cast<std::uint64_t>(target_len),
        static_cast<std::uint32_t>(buckets.size()),
        0u,
        static_cast<std::uint64_t>(positions.size()),
    };
    const std::size_t bytes =
        sizeof(header)
        + buckets.size() * sizeof(bucket)
        + positions.size() * sizeof(std::uint64_t);
    out_blob.resize(bytes);
    char* cursor = out_blob.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    if (!buckets.empty()) {
        std::memcpy(cursor, buckets.data(), buckets.size() * sizeof(bucket));
        cursor += buckets.size() * sizeof(bucket);
    }
    if (!positions.empty()) {
        std::memcpy(
            cursor,
            positions.data(),
            positions.size() * sizeof(std::uint64_t));
    }
}

/// Operation-originated route stats for one complete MPF generation encode.
/// The common encoder replays its producer at least once for planning and once
/// for emission; every biological core records the branch it actually entered.
/// Selected SIMD/GPU routes additionally carry their native checked stats, so a
/// caller cannot accept a backend merely because its enum was requested.
class mpf_generation_execution_receipt {
public:
    minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
    minimizer_scheme_identity minimizer_scheme{};
    std::uint64_t target_length = 0;
    std::uint16_t window_size = 0;
    std::uint16_t kmer_size = 0;
    std::uint64_t producer_scan_count = 0;
    std::uint64_t geometry_retry_count = 0;
    minimizer_generation_layout generation_layout =
        minimizer_generation_layout::invalid;
    std::uint64_t short_core_runs = 0;
    std::uint64_t scalar_core_runs = 0;
    std::uint64_t exact_simd_core_runs = 0;
    std::uint64_t bounded_nthash_core_runs = 0;
    std::uint64_t vulkan_core_runs = 0;
    exact_simd_index_execution_stats exact_simd_stats{};
    bool exact_simd_stats_present = false;
    packed_bounded_nthash_index_status packed_bounded_status =
        packed_bounded_nthash_index_status::invalid_state;
    packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
    bool packed_bounded_stats_present = false;
    bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
    bool backend_bound = false;
    bool saturated = false;

    [[nodiscard]] std::uint64_t
    required_successful_scan_count() const noexcept {
        if (generation_layout
                == minimizer_generation_layout::
                    grouped_soa_segment_local) {
            return 3u;
        }
        /* An unbound producer can still attest two explicit direct replays.
         * Publication additionally requires a concrete matching wire format. */
        return generation_layout == minimizer_generation_layout::invalid
                || is_supported_minimizer_generation_layout(
                    generation_layout)
            ? 2u : 0u;
    }

    [[nodiscard]] bool is_accepted() const noexcept {
        // A fixed-width geometry retry contributes exactly one additional
        // planning scan.  The successful-pass count is bound from the wire
        // format before the first encoder scan.
        const std::uint64_t required_scans =
            required_successful_scan_count();
        if (!common_identity_accepted_() || required_scans == 0u
            || geometry_retry_count
                > (std::numeric_limits<std::uint64_t>::max)()
                    - required_scans
            || producer_scan_count
                != geometry_retry_count + required_scans) {
            return false;
        }
        return selected_route_accepted_(requires_backend_work_());
    }

    /// Accept stats emitted by the direct MPF builder, which is one
    /// biological construction operation rather than an encoder replay and
    /// therefore deliberately has no wire-format/publication binding.
    [[nodiscard]] bool direct_build_accepted() const noexcept {
        return common_identity_accepted_()
            && producer_scan_count == 1u
            && geometry_retry_count == 0u
            && generation_layout
                == minimizer_generation_layout::invalid
            && window_size == 10u
            && kmer_size == 15u
            && !route_snapshot_present_
            && !publication_binding_present_
            && selected_route_accepted_(requires_backend_work_());
    }

    [[nodiscard]] bool publication_accepted(
            const minimizer_generation_publication_binding& expected)
            const noexcept {
        return is_accepted() && route_snapshot_matches_()
            && publication_binding_present_
            && generation_layout != minimizer_generation_layout::invalid
            && generation_layout == expected.layout
            && expected.valid() && publication_binding_ == expected;
    }

private:
    [[nodiscard]] bool common_identity_accepted_() const noexcept {
        return backend_bound && !saturated
            && target_length != 0u
            && window_size != 0u
            && kmer_size != 0u
            && minimizer_scheme
                == minimizer_scheme_identity_for_backend(selected_backend);
    }

    [[nodiscard]] bool requires_backend_work_() const noexcept {
        const std::uint64_t minimum_work_bases =
            static_cast<std::uint64_t>(window_size) + kmer_size - 1u;
        return target_length >= minimum_work_bases;
    }

    [[nodiscard]] bool selected_route_accepted_(
            bool require_backend_work) const noexcept {
        const bool no_exact = exact_simd_core_runs == 0
            && !exact_simd_stats_present;
        const bool no_bounded = bounded_nthash_core_runs == 0
            && !packed_bounded_stats_present;
        const bool no_vulkan = vulkan_core_runs == 0
            && vulkan_stats.completed_roundtrips == 0
            && vulkan_stats.downloaded_minimizers == 0
            && vulkan_stats.gpu_window_splits == 0;

        switch (selected_backend) {
            case minimizer_sketch_backend::scalar:
                return exact_simd_core_runs == 0
                    && bounded_nthash_core_runs == 0
                    && vulkan_core_runs == 0
                    && no_exact && no_bounded && no_vulkan
                    && (!require_backend_work || scalar_core_runs != 0);
            case minimizer_sketch_backend::two_stack_nthash:
                return scalar_core_runs == 0
                    && bounded_nthash_core_runs == 0
                    && vulkan_core_runs == 0
                    && no_bounded && no_vulkan
                    && (exact_simd_core_runs == 0
                        ? (!require_backend_work
                            && !exact_simd_stats_present)
                        : (exact_simd_stats_present
                            && exact_simd_stats.is_internally_consistent(
                                require_backend_work)));
            case minimizer_sketch_backend::bounded_nthash_simd:
                return scalar_core_runs == 0
                    && exact_simd_core_runs == 0
                    && vulkan_core_runs == 0
                    && no_exact && no_vulkan
                    && (bounded_nthash_core_runs == 0
                        ? (!require_backend_work
                            && !packed_bounded_stats_present)
                        : (packed_bounded_stats_present
                            && packed_bounded_status
                                == packed_bounded_nthash_index_status::ok
                            && packed_bounded_stats.is_internally_consistent(
                                require_backend_work)));
            case minimizer_sketch_backend::vulkan_segment:
                return scalar_core_runs == 0
                    && exact_simd_core_runs == 0
                    && bounded_nthash_core_runs == 0
                    && no_exact && no_bounded
                    && (vulkan_core_runs == 0
                        ? (!require_backend_work
                            && vulkan_stats.is_internally_consistent(false))
                        : vulkan_stats.is_internally_consistent(require_backend_work));
        }
        return false;
    }
    template <class>
    friend struct minimizers_per_fragment_index;
    template <sequence_alphabet Alphabet>
    friend class minimizer_fragment_generation_replay_producer;

    struct route_snapshot {
        minimizer_sketch_backend selected_backend =
            minimizer_sketch_backend::scalar;
        minimizer_scheme_identity minimizer_scheme{};
        std::uint64_t target_length = 0;
        std::uint16_t window_size = 0;
        std::uint16_t kmer_size = 0;
        std::uint64_t producer_scan_count = 0;
        std::uint64_t geometry_retry_count = 0;
        minimizer_generation_layout generation_layout =
            minimizer_generation_layout::invalid;
        std::uint64_t short_core_runs = 0;
        std::uint64_t scalar_core_runs = 0;
        std::uint64_t exact_simd_core_runs = 0;
        std::uint64_t bounded_nthash_core_runs = 0;
        std::uint64_t vulkan_core_runs = 0;
        exact_simd_index_execution_stats exact_simd_stats{};
        bool exact_simd_stats_present = false;
        packed_bounded_nthash_index_status packed_bounded_status =
            packed_bounded_nthash_index_status::invalid_state;
        packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
        bool packed_bounded_stats_present = false;
        bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
        bool backend_bound = false;
        bool saturated = false;
    } route_snapshot_{};
    bool route_snapshot_present_ = false;

    minimizer_generation_publication_binding publication_binding_{};
    bool publication_binding_present_ = false;

    [[nodiscard]] static bool same_vulkan_stats_(
            const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& left,
            const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& right)
            noexcept {
        return left.completed_roundtrips == right.completed_roundtrips
            && left.downloaded_minimizers == right.downloaded_minimizers
            && left.gpu_window_splits == right.gpu_window_splits
            && left.saturated == right.saturated;
    }

    void capture_route_snapshot_() noexcept {
        route_snapshot_ = route_snapshot{
            .selected_backend = selected_backend,
            .minimizer_scheme = minimizer_scheme,
            .target_length = target_length,
            .window_size = window_size,
            .kmer_size = kmer_size,
            .producer_scan_count = producer_scan_count,
            .geometry_retry_count = geometry_retry_count,
            .generation_layout = generation_layout,
            .short_core_runs = short_core_runs,
            .scalar_core_runs = scalar_core_runs,
            .exact_simd_core_runs = exact_simd_core_runs,
            .bounded_nthash_core_runs = bounded_nthash_core_runs,
            .vulkan_core_runs = vulkan_core_runs,
            .exact_simd_stats = exact_simd_stats,
            .exact_simd_stats_present = exact_simd_stats_present,
            .packed_bounded_status = packed_bounded_status,
            .packed_bounded_stats = packed_bounded_stats,
            .packed_bounded_stats_present = packed_bounded_stats_present,
            .vulkan_stats = vulkan_stats,
            .backend_bound = backend_bound,
            .saturated = saturated,
        };
        route_snapshot_present_ = true;
    }

    [[nodiscard]] bool route_snapshot_matches_() const noexcept {
        const route_snapshot& value = route_snapshot_;
        return route_snapshot_present_
            && selected_backend == value.selected_backend
            && minimizer_scheme == value.minimizer_scheme
            && target_length == value.target_length
            && window_size == value.window_size
            && kmer_size == value.kmer_size
            && producer_scan_count == value.producer_scan_count
            && geometry_retry_count == value.geometry_retry_count
            && generation_layout == value.generation_layout
            && short_core_runs == value.short_core_runs
            && scalar_core_runs == value.scalar_core_runs
            && exact_simd_core_runs == value.exact_simd_core_runs
            && bounded_nthash_core_runs == value.bounded_nthash_core_runs
            && vulkan_core_runs == value.vulkan_core_runs
            && exact_simd_stats == value.exact_simd_stats
            && exact_simd_stats_present == value.exact_simd_stats_present
            && packed_bounded_status == value.packed_bounded_status
            && packed_bounded_stats == value.packed_bounded_stats
            && packed_bounded_stats_present
                == value.packed_bounded_stats_present
            && same_vulkan_stats_(vulkan_stats, value.vulkan_stats)
            && backend_bound == value.backend_bound
            && saturated == value.saturated;
    }

    void bind_publication_(
            const encoded_grouped_soa_generation& generation) noexcept {
        const minimizer_producer_identity backend =
            minimizer_producer_backend_identity_for_backend(
                selected_backend);
        publication_binding_ =
            minimizer_generation_publication_binding(generation);
        publication_binding_present_ = publication_binding_.valid()
            && route_snapshot_matches_()
            && generation_layout
                == generation.manifest.layout
            && target_length == generation.manifest.target_length
            && window_size == generation.manifest.window_size
            && kmer_size == generation.manifest.kmer_size
            && minimizer_scheme.id
                == generation.manifest.minimizer_scheme_id
            && minimizer_scheme.revision
                == generation.manifest.minimizer_scheme_revision
            && backend.id == generation.manifest.producer_backend_id
            && backend.revision
                == generation.manifest.producer_identity_revision;
    }
};

/// Concrete MPF current producer. Each replay rescans source fragments with
/// the canonical `w+k-2` halo and emits each physical fragment's independently
/// ordered index.  A compatibility fallback can merge multiple source runs
/// only when a caller explicitly requests a persisted span wider than one
/// physical fragment; the production MPF publisher never does so.
template <sequence_alphabet Alphabet>
class minimizer_fragment_generation_replay_producer {
private:
    using sequence_type = bioseq::storage::lazy_sequence_view<Alphabet>;
    using traits = typename sequence_type::traits;

    sequence_type sequence_;
    minimizer_sketch_backend backend_ = minimizer_sketch_backend::scalar;
    const bioseq::minimizers::minimizer_kernels* kernels_ = nullptr;
    std::uint8_t alphabet_code_ =
        bioseq::minimizers::unbound_packed_alphabet_code;
    std::uint16_t window_size_ = 10;
    std::uint16_t kmer_size_ = 15;
    std::uint64_t scan_count_ = 0;
    std::uint64_t geometry_retry_count_ = 0;
    minimizer_generation_layout generation_layout_ =
        minimizer_generation_layout::invalid;
    std::uint64_t last_scan_span_ = 0;
    bool has_last_scan_span_ = false;
    std::uint64_t peak_buffered_events_ = 0;
    std::uint64_t peak_buffered_core_bp_ = 0;
    std::uint64_t peak_active_fragment_runs_ = 0;
    std::uint64_t peak_bounded_materialized_window_bp_ = 0;
    exact_simd_index_execution_stats exact_simd_stats_{};
    bool exact_simd_stats_present_ = false;
    packed_bounded_nthash_index_status packed_bounded_status_ =
        packed_bounded_nthash_index_status::invalid_state;
    packed_bounded_nthash_index_execution_stats packed_bounded_stats_{};
    bool packed_bounded_stats_present_ = false;
    std::uint64_t short_core_runs_ = 0;
    std::uint64_t scalar_core_runs_ = 0;
    std::uint64_t exact_simd_core_runs_ = 0;
    std::uint64_t bounded_nthash_core_runs_ = 0;
    std::uint64_t vulkan_core_runs_ = 0;
    bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats_{};
    bool route_stats_saturated_ = false;

    void increment_route_counter_(std::uint64_t& counter) noexcept {
        if (counter == (std::numeric_limits<std::uint64_t>::max)()) {
            route_stats_saturated_ = true;
            return;
        }
        ++counter;
    }

    void record_packed_bounded_result(
            const packed_bounded_nthash_index_bridge_result& result) noexcept {
        packed_bounded_status_ = result.status;
        packed_bounded_stats_.merge(result.stats);
        packed_bounded_stats_present_ = true;
    }

    [[nodiscard]] std::expected<
        std::vector<canonical_minimizer_event>,
        minimizer_generation_encoding_error>
    collect_core_run(
            std::uint64_t core_lo,
            std::uint64_t core_hi) {
        const std::uint64_t target_length = sequence_.bp_size();
        const std::uint64_t halo = static_cast<std::uint64_t>(window_size_)
            + kmer_size_ - 2u;
        const std::uint64_t collect_lo = core_lo >= halo
            ? core_lo - halo : 0u;
        const std::uint64_t collect_hi = core_hi
                > target_length - std::min(target_length, halo)
            ? target_length : core_hi + halo;
        const std::uint64_t emit_base = collect_lo;
        const std::uint64_t minimum_bases =
            static_cast<std::uint64_t>(window_size_) + kmer_size_ - 1u;
        std::vector<canonical_minimizer_event> run;
        if (collect_hi <= emit_base
            || collect_hi - emit_base < minimum_bases) {
            increment_route_counter_(short_core_runs_);
            return run;
        }

        if (backend_ == minimizer_sketch_backend::vulkan_segment) {
            increment_route_counter_(vulkan_core_runs_);
            bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats core_stats{};
            auto pairs = bioseq::gpu::vulkan::
                collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                    sequence_,
                    static_cast<sequence_size_t>(collect_lo),
                    static_cast<sequence_size_t>(collect_hi),
                    static_cast<sequence_size_t>(emit_base),
                    static_cast<sequence_size_t>(core_lo),
                    static_cast<sequence_size_t>(core_hi),
                    window_size_,
                    kmer_size_,
                    &core_stats);
            vulkan_stats_.merge(core_stats);
            route_stats_saturated_ = route_stats_saturated_
                || vulkan_stats_.saturated;
            run.reserve(pairs.pairs.size());
            for (const auto& pair : pairs.pairs) {
                run.push_back(canonical_minimizer_event{
                    .hash = pair.first,
                    .encoded_position = pair.second,
                });
            }
        } else {
            sketch_collector collector;
            if (backend_ == minimizer_sketch_backend::bounded_nthash_simd) {
                increment_route_counter_(bounded_nthash_core_runs_);
                std::uint64_t materialized_window_bp = 0u;
                const auto bounded_result =
                    collect_packed_bounded_nthash_index_lazy(
                        sequence_,
                        alphabet_code_,
                        static_cast<sequence_size_t>(collect_lo),
                        static_cast<sequence_size_t>(collect_hi),
                        collect_hi - emit_base,
                        window_size_,
                        kmer_size_,
                        kernels_,
                        collector,
                        &materialized_window_bp);
                peak_bounded_materialized_window_bp_ = std::max(
                    peak_bounded_materialized_window_bp_,
                    materialized_window_bp);
                record_packed_bounded_result(bounded_result);
                if (bounded_result.status
                        != packed_bounded_nthash_index_status::ok) {
                    throw packed_bounded_nthash_index_runtime_error{
                        bounded_result.status,
                        "MPF generation replay"};
                }
            } else if (backend_ == minimizer_sketch_backend::two_stack_nthash) {
                increment_route_counter_(exact_simd_core_runs_);
                try {
                    const auto stats = collect_exact_simd_index_range_or_throw(
                        collector,
                        sequence_,
                        alphabet_code_,
                        static_cast<sequence_size_t>(collect_lo),
                        static_cast<sequence_size_t>(collect_hi),
                        window_size_,
                        kmer_size_,
                        0u,
                        kernels_,
                        "MPF generation replay");
                    exact_simd_stats_.merge(stats);
                    exact_simd_stats_present_ = true;
                    route_stats_saturated_ = route_stats_saturated_
                        || exact_simd_stats_.saturated;
                } catch (const exact_simd_index_runtime_error&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::compatibility_mismatch);
                }
            } else if (backend_ == minimizer_sketch_backend::scalar) {
                increment_route_counter_(scalar_core_runs_);
                bioseq::simd::collect_minimizers_segment_scalar<
                    minimap_sketch_data>(
                        collector, sequence_, collect_lo, collect_hi,
                        window_size_, kmer_size_);
            } else {
                return std::unexpected(
                    minimizer_generation_encoding_error::compatibility_mismatch);
            }
            const auto& sketches = collector.extract_sketches_data();
            run.reserve(sketches.size());
            for (const minimap_sketch_data& minimizer : sketches) {
                const std::uint64_t position = emit_base
                    + static_cast<std::uint64_t>(minimizer.last_pos());
                if (position < core_lo || position >= core_hi) {
                    continue;
                }
                run.push_back(canonical_minimizer_event{
                    .hash = minimizer.kmer(),
                    .encoded_position = (position << 1u)
                        | static_cast<std::uint64_t>(minimizer.strand()),
                });
            }
        }
        std::ranges::sort(run, {}, [](const canonical_minimizer_event& event) {
            return std::pair{event.hash, event.encoded_position};
        });
        // Minimizer collectors may report one selected occurrence from more
        // than one adjacent tied window. Persisted occurrence semantics are
        // canonical `(hash, position, strand)` events for every backend, so
        // collapse identical reports inside this backend-owned core run after
        // the mandatory sort. Core ownership is disjoint, therefore a
        // duplicate cannot legitimately need to survive in another run. The
        // lower wire encoder remains fail-closed for arbitrary producers that
        // violate this canonical-stream contract.
        run.erase(std::ranges::unique(run).begin(), run.end());
        return run;
    }

public:
    minimizer_fragment_generation_replay_producer(
            sequence_type sequence,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint8_t alphabet_code,
            std::uint16_t window_size = 10,
            std::uint16_t kmer_size = 15) noexcept
        : sequence_(sequence),
          backend_(backend),
          kernels_(kernels),
          alphabet_code_(alphabet_code),
          window_size_(window_size),
          kmer_size_(kmer_size) {}

    void bind_generation_layout(
            minimizer_generation_layout layout) noexcept {
        const std::uint64_t required = layout
                == minimizer_generation_layout::
                    grouped_soa_segment_local
            ? 3u
            : is_supported_minimizer_generation_layout(layout)
                ? 2u : 0u;
        if (required == 0u || scan_count_ != 0u
            || (generation_layout_
                    != minimizer_generation_layout::invalid
                && generation_layout_ != layout)) {
            route_stats_saturated_ = true;
            return;
        }
        generation_layout_ = layout;
    }

    [[nodiscard]] minimizer_scheme_identity
    minimizer_scheme_identity() const noexcept {
        return minimizer_scheme_identity_for_backend(backend_);
    }

    [[nodiscard]] minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return minimizer_producer_backend_identity_for_backend(backend_);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    scan_sorted_events(
            std::uint64_t persisted_span,
            canonical_minimizer_event_receiver receiver) {
        constexpr std::uint64_t fragment_bp = 32'768u;
        increment_route_counter_(scan_count_);
        if (has_last_scan_span_ && persisted_span != last_scan_span_) {
            increment_route_counter_(geometry_retry_count_);
        }
        last_scan_span_ = persisted_span;
        has_last_scan_span_ = true;
        if (persisted_span == 0
            || fragment_bp == 0 || window_size_ == 0 || kmer_size_ == 0
            || static_cast<std::uint32_t>(window_size_)
                    + static_cast<std::uint32_t>(kmer_size_) < 2u) {
            return std::unexpected(minimizer_generation_encoding_error::segment_span);
        }
        if (backend_ != minimizer_sketch_backend::scalar
            && backend_ != minimizer_sketch_backend::two_stack_nthash
            && backend_ != minimizer_sketch_backend::bounded_nthash_simd
            && backend_ != minimizer_sketch_backend::vulkan_segment) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        if ((backend_ == minimizer_sketch_backend::two_stack_nthash
                && (kernels_ == nullptr
                    || kernels_->two_stack_nthash == nullptr))
            || (backend_ == minimizer_sketch_backend::bounded_nthash_simd
                && (kernels_ == nullptr
                    || kernels_->bounded_nthash == nullptr))) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        const std::uint64_t target_length = sequence_.bp_size();

        std::uint64_t segment_lo = 0;
        while (segment_lo < target_length) {
            const std::uint64_t segment_width = std::min(
                persisted_span, target_length - segment_lo);
            const std::uint64_t segment_hi = segment_lo + segment_width;
            std::vector<std::vector<canonical_minimizer_event>> runs;
            std::uint64_t buffered_events = 0;
            std::uint64_t fragment_lo = (segment_lo / fragment_bp) * fragment_bp;
            while (fragment_lo < segment_hi) {
                const std::uint64_t fragment_hi = std::min(
                    target_length, fragment_lo + fragment_bp);
                const std::uint64_t core_lo = std::max(segment_lo, fragment_lo);
                const std::uint64_t core_hi = std::min(segment_hi, fragment_hi);
                if (core_lo < core_hi) {
                    auto run = collect_core_run(core_lo, core_hi);
                    if (!run) {
                        return std::unexpected(run.error());
                    }
                    if (buffered_events
                            > (std::numeric_limits<std::uint64_t>::max)()
                                - run->size()) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                    buffered_events += run->size();
                    if (!run->empty()) {
                        runs.push_back(std::move(*run));
                    }
                }
                if (fragment_hi <= fragment_lo) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                fragment_lo = fragment_hi;
            }
            peak_buffered_events_ = std::max(
                peak_buffered_events_, buffered_events);
            peak_buffered_core_bp_ = std::max(
                peak_buffered_core_bp_, segment_width);
            peak_active_fragment_runs_ = std::max<std::uint64_t>(
                peak_active_fragment_runs_, runs.size());

            if (runs.size() == 1u) {
                for (const canonical_minimizer_event& event : runs.front()) {
                    if (auto accepted = receiver(event); !accepted) {
                        return accepted;
                    }
                }
                segment_lo = segment_hi;
                continue;
            }

            struct cursor {
                std::size_t run = 0;
                std::size_t index = 0;
            };
            const auto after = [&runs](const cursor& lhs, const cursor& rhs) {
                const auto& left = runs[lhs.run][lhs.index];
                const auto& right = runs[rhs.run][rhs.index];
                return std::pair{left.hash, left.encoded_position}
                    > std::pair{right.hash, right.encoded_position};
            };
            std::priority_queue<
                cursor, std::vector<cursor>, decltype(after)> heap{after};
            for (std::size_t run = 0; run < runs.size(); ++run) {
                heap.push(cursor{.run = run, .index = 0});
            }
            while (!heap.empty()) {
                cursor cursor = heap.top();
                heap.pop();
                if (auto accepted = receiver(
                        runs[cursor.run][cursor.index]); !accepted) {
                    return accepted;
                }
                ++cursor.index;
                if (cursor.index < runs[cursor.run].size()) {
                    heap.push(cursor);
                }
            }
            segment_lo = segment_hi;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t scan_count() const noexcept {
        return scan_count_;
    }
    [[nodiscard]] std::uint64_t peak_buffered_events() const noexcept {
        return peak_buffered_events_;
    }
    [[nodiscard]] std::uint64_t peak_buffered_core_bp() const noexcept {
        return peak_buffered_core_bp_;
    }
    [[nodiscard]] std::uint64_t peak_active_fragment_runs() const noexcept {
        return peak_active_fragment_runs_;
    }
    [[nodiscard]] std::uint64_t
    peak_bounded_materialized_window_bp() const noexcept {
        return peak_bounded_materialized_window_bp_;
    }
    [[nodiscard]] bool exact_simd_stats_present() const noexcept {
        return exact_simd_stats_present_;
    }
    [[nodiscard]] const exact_simd_index_execution_stats&
    exact_simd_stats() const noexcept {
        return exact_simd_stats_;
    }
    [[nodiscard]] bool packed_bounded_stats_present() const noexcept {
        return packed_bounded_stats_present_;
    }
    [[nodiscard]] packed_bounded_nthash_index_status
    packed_bounded_status() const noexcept {
        return packed_bounded_status_;
    }
    [[nodiscard]] const packed_bounded_nthash_index_execution_stats&
    packed_bounded_stats() const noexcept {
        return packed_bounded_stats_;
    }
    [[nodiscard]] mpf_generation_execution_receipt
    execution_stats() const noexcept {
        mpf_generation_execution_receipt stats;
        stats.selected_backend = backend_;
        stats.minimizer_scheme = minimizer_scheme_identity();
        stats.target_length =
            sequence_.bp_size();
        stats.window_size = window_size_;
        stats.kmer_size = kmer_size_;
        stats.producer_scan_count = scan_count_;
        stats.geometry_retry_count = geometry_retry_count_;
        stats.generation_layout = generation_layout_;
        stats.short_core_runs = short_core_runs_;
        stats.scalar_core_runs = scalar_core_runs_;
        stats.exact_simd_core_runs = exact_simd_core_runs_;
        stats.bounded_nthash_core_runs = bounded_nthash_core_runs_;
        stats.vulkan_core_runs = vulkan_core_runs_;
        stats.exact_simd_stats = exact_simd_stats_;
        stats.exact_simd_stats_present = exact_simd_stats_present_;
        stats.packed_bounded_status = packed_bounded_status_;
        stats.packed_bounded_stats = packed_bounded_stats_;
        stats.packed_bounded_stats_present = packed_bounded_stats_present_;
        stats.vulkan_stats = vulkan_stats_;
        stats.backend_bound = true;
        stats.saturated = route_stats_saturated_
            || vulkan_stats_.saturated;
        stats.capture_route_snapshot_();
        return stats;
    }
};

// (2) the storage-bound CHUNKED_PER_FRAGMENT index template
template <class S>
struct minimizers_per_fragment_index {
    static_assert(is_index_chunked_storage_for<S, sequence_index_layout::fragment_chunks>);
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view  index_name = minimizer_hashmap_fragment_index::index_name;
    using storage_type = S;

    template <sequence_alphabet Alphabet>
    [[nodiscard]] static auto make_generation_replay_producer(
            bioseq::storage::lazy_sequence_view<Alphabet> sequence,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint8_t alphabet_code =
                bioseq::minimizers::unbound_packed_alphabet_code) {
        return minimizer_fragment_generation_replay_producer<Alphabet>{
            sequence, backend, kernels, alphabet_code};
    }

    /// Concrete MPF construction seam. Live routing remains unchanged; this
    /// records the bounded producer used by the current fragment image.
    template <sequence_alphabet Alphabet, minimizer_generation_row_sink Sink>
    [[nodiscard]] static std::expected<
        encoded_grouped_soa_generation,
        minimizer_generation_encoding_error>
    encode_persisted_generation(
            bioseq::storage::lazy_sequence_view<Alphabet> sequence,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            Sink& sink,
            const grouped_soa_generation_options& options,
            const grouped_soa_generation_limits& limits = {},
            mpf_generation_execution_receipt* execution_stats_out = nullptr,
            std::uint8_t runtime_alphabet_code =
                bioseq::minimizers::unbound_packed_alphabet_code) {
        if (execution_stats_out != nullptr) {
            *execution_stats_out = {};
        }
        if (options.window_size != 10 || options.kmer_size != 15) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        if ((backend == minimizer_sketch_backend::two_stack_nthash
                && (kernels == nullptr
                    || kernels->two_stack_nthash == nullptr))
            || (backend == minimizer_sketch_backend::bounded_nthash_simd
                && (kernels == nullptr
                    || kernels->bounded_nthash == nullptr))) {
            return std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        auto producer = make_generation_replay_producer(
            sequence, backend, kernels, runtime_alphabet_code);
        auto encoded = encode_minimizer_generation(
            producer, sink, options, limits);
        mpf_generation_execution_receipt stats =
            producer.execution_stats();
        const minimizer_producer_identity stats_backend =
            minimizer_producer_backend_identity_for_backend(
                stats.selected_backend);
        if (encoded
            && (encoded->manifest.producer_backend_id
                    != stats_backend.id
                || encoded->manifest.producer_identity_revision
                    != stats_backend.revision)) {
            encoded = std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        if (encoded) {
            stats.bind_publication_(*encoded);
        }
        if (encoded && !stats.publication_accepted(
                minimizer_generation_publication_binding(*encoded))) {
            encoded = std::unexpected(
                minimizer_generation_encoding_error::compatibility_mismatch);
        }
        if (execution_stats_out != nullptr) {
            *execution_stats_out = stats;
        }
        return encoded;
    }

    template <sequence_alphabet Alphabet>
    static void build(
        S& storage,
        bioseq::storage::lazy_sequence_view<Alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        mpf_generation_execution_receipt* execution_stats_out = nullptr,
        std::uint8_t alphabet_code =
            bioseq::minimizers::unbound_packed_alphabet_code) {
        constexpr std::size_t w = 10, k = 15;                                  // pinned, == sidecar today
        constexpr std::size_t frag_bp    = 32'768u;
        constexpr std::size_t W          = w + k;                              // backward context
        static_assert(frag_bp < (std::size_t{1} << 28),
            "fragment-core-local positions must fit the 28-bit minimap_sketch_data field");
        const sequence_size_t total_bp = seq.bp_size();
        const std::size_t nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if (backend == minimizer_sketch_backend::two_stack_nthash
            && (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_state,
                "minimizers_per_fragment build"};
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd
            && (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw packed_bounded_nthash_index_runtime_error{
                packed_bounded_nthash_index_status::invalid_state,
                "minimizers_per_fragment build"};
        }
        if (execution_stats_out != nullptr) {
            *execution_stats_out = {};
            execution_stats_out->selected_backend = backend;
            execution_stats_out->minimizer_scheme =
                minimizer_scheme_identity_for_backend(backend);
            execution_stats_out->target_length = total_bp;
            execution_stats_out->window_size = w;
            execution_stats_out->kmer_size = k;
            execution_stats_out->producer_scan_count = 1u;
            execution_stats_out->backend_bound = true;
        }

        // Batch canonical 32 KiBP fragments into large GPU collection windows,
        // then scatter the one sorted global pair stream back into persisted
        // fragment-local blobs.  One synchronous submission per fragment is a
        // severe regression and defeats the existing GPU MPF batch geometry.
        if (backend == minimizer_sketch_backend::vulkan_segment) {
            const std::size_t fragments_per_batch =
                gpu_mpf_fragments_per_batch(
                    nfrag, frag_bp, 2u * W, storage.run_buffer_bytes());
            std::vector<std::vector<
                std::pair<std::uint64_t, std::uint64_t>>> per_fragment(
                    fragments_per_batch);
            std::vector<char> blob;
            const auto write_fragment_blob =
                [&storage](std::size_t fragment_no,
                           const std::vector<char>& fragment_blob) {
                    const std::size_t bytes = fragment_blob.size();
                    storage.resize(
                        static_cast<index_sub_key_t>(fragment_no), bytes);
                    storage.template write_to_each<char>(
                        static_cast<index_sub_key_t>(fragment_no),
                        [&fragment_blob, bytes](
                                std::size_t chunk_no,
                                char* buffer,
                                std::size_t capacity_units) {
                            DEBUG_ASSERT(
                                chunk_no == 0u && capacity_units >= bytes,
                                "one fragment index fits one chunk");
                            if (bytes != 0u) {
                                std::memcpy(
                                    buffer, fragment_blob.data(), bytes);
                            }
                            return chunk_no;
                        });
                };

            for (std::size_t batch_lo_frag = 0u;
                 batch_lo_frag < nfrag;
                 batch_lo_frag += fragments_per_batch) {
                const std::size_t batch_hi_frag = std::min(
                    nfrag, batch_lo_frag + fragments_per_batch);
                const sequence_size_t core_lo =
                    static_cast<sequence_size_t>(batch_lo_frag) * frag_bp;
                const sequence_size_t core_hi = std::min(
                    static_cast<sequence_size_t>(batch_hi_frag) * frag_bp,
                    total_bp);
                const sequence_size_t coll_lo =
                    core_lo >= W ? core_lo - W : 0u;
                const sequence_size_t coll_hi = std::min(
                    core_hi + static_cast<sequence_size_t>(W), total_bp);
                bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats stats{};
                auto batch_pairs = bioseq::gpu::vulkan::
                    collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                        seq, coll_lo, coll_hi, coll_lo,
                        core_lo, core_hi, w, k,
                        execution_stats_out != nullptr ? &stats : nullptr);
                if (execution_stats_out != nullptr) {
                    ++execution_stats_out->vulkan_core_runs;
                    execution_stats_out->vulkan_stats.merge(stats);
                    execution_stats_out->saturated =
                        execution_stats_out->saturated
                        || execution_stats_out->vulkan_stats.saturated;
                }
                for (auto& fragment_pairs : per_fragment) {
                    fragment_pairs.clear();
                }
                for (const auto& pair : batch_pairs.pairs) {
                    const sequence_size_t global_pos =
                        static_cast<sequence_size_t>(pair.second >> 1u);
                    const std::size_t fragment_no =
                        static_cast<std::size_t>(global_pos / frag_bp);
                    if (fragment_no < batch_lo_frag
                        || fragment_no >= batch_hi_frag) {
                        continue;
                    }
                    const sequence_size_t fragment_lo =
                        static_cast<sequence_size_t>(fragment_no) * frag_bp;
                    per_fragment[fragment_no - batch_lo_frag].emplace_back(
                        pair.first,
                        (static_cast<std::uint64_t>(
                            global_pos - fragment_lo) << 1u)
                            | (pair.second & 1u));
                }
                for (std::size_t fragment_no = batch_lo_frag;
                     fragment_no < batch_hi_frag;
                     ++fragment_no) {
                    const sequence_size_t fragment_lo =
                        static_cast<sequence_size_t>(fragment_no) * frag_bp;
                    const sequence_size_t fragment_hi = std::min(
                        fragment_lo + static_cast<sequence_size_t>(frag_bp),
                        total_bp);
                    serialize_sorted_pairs_to_fragment_blob(
                        std::span<const std::pair<
                            std::uint64_t, std::uint64_t>>{
                                per_fragment[
                                    fragment_no - batch_lo_frag]},
                        fragment_hi - fragment_lo,
                        blob);
                    write_fragment_blob(fragment_no, blob);
                }
            }
            const std::int64_t manifest = static_cast<std::int64_t>(nfrag);
            storage.resize(k_directory_sub_key, sizeof(std::int64_t));
            storage.template write_to_each<std::int64_t>(
                k_directory_sub_key,
                [manifest](std::size_t chunk_no,
                           std::int64_t* manifest_buffer,
                           std::size_t) {
                    manifest_buffer[0] = manifest;
                    return chunk_no;
                });
            return;
        }

        auto build_fragment_blob =
            [seq, backend, kernels, alphabet_code, total_bp,
             execution_stats_out](std::size_t fragment_no,
                                     sketch_collector& collector,
                                     std::vector<minimap_sketch_data>& core,
                                     std::vector<char>& out_blob) {
            const sequence_size_t core_lo = (sequence_size_t)fragment_no * frag_bp;
            const sequence_size_t core_hi = std::min(core_lo + (sequence_size_t)frag_bp, total_bp);
            const sequence_size_t coll_lo = core_lo >= W ? core_lo - W : 0;
            const sequence_size_t coll_hi = std::min(core_hi + (sequence_size_t)W, total_bp);
            collector.clear();
            const sequence_size_t emit_base = coll_lo;
            const sequence_size_t collect_bp = coll_hi > emit_base ? coll_hi - emit_base : 0;
            core.clear();
            if (collect_bp >= (sequence_size_t)W) {
                if (backend == minimizer_sketch_backend::vulkan_segment) {
                    auto pairs = bioseq::gpu::vulkan::
                        collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                            seq, coll_lo, coll_hi, emit_base,
                            core_lo, core_hi, w, k);
                    core.reserve(pairs.pairs.size());
                    for (const auto& pair : pairs.pairs) {
                        const sequence_size_t global_pos =
                            static_cast<sequence_size_t>(pair.second >> 1u);
                        core.push_back(minimap_sketch_data{
                            pair.first,
                            static_cast<std::uint64_t>(global_pos - core_lo),
                            static_cast<std::uint64_t>(pair.second & 1u)});
                    }
                } else {
                    if (backend == minimizer_sketch_backend::two_stack_nthash) {
                        const auto stats = collect_exact_simd_index_range_or_throw(
                            collector, seq, alphabet_code,
                            coll_lo, coll_hi, w, k, 0u,
                            kernels,
                            "minimizers_per_fragment build");
                        if (execution_stats_out != nullptr) {
                            ++execution_stats_out->exact_simd_core_runs;
                            execution_stats_out->exact_simd_stats.merge(
                                stats);
                            execution_stats_out->exact_simd_stats_present =
                                true;
                            execution_stats_out->saturated =
                                execution_stats_out->saturated
                                || execution_stats_out->exact_simd_stats.
                                    saturated;
                        }
                    } else if (backend
                            == minimizer_sketch_backend::bounded_nthash_simd) {
                        const auto result =
                            collect_packed_bounded_nthash_index_lazy(
                                seq, alphabet_code,
                                coll_lo, coll_hi, collect_bp,
                                w, k, kernels, collector);
                        if (result.status
                                != packed_bounded_nthash_index_status::ok) {
                            throw packed_bounded_nthash_index_runtime_error{
                                result.status,
                                "minimizers_per_fragment build"};
                        }
                        if (execution_stats_out != nullptr) {
                            ++execution_stats_out->bounded_nthash_core_runs;
                            execution_stats_out->packed_bounded_status =
                                result.status;
                            execution_stats_out->packed_bounded_stats.merge(
                                result.stats);
                            execution_stats_out->
                                packed_bounded_stats_present = true;
                            execution_stats_out->saturated =
                                execution_stats_out->saturated
                                || execution_stats_out->packed_bounded_stats.
                                    saturated;
                        }
                    } else if (backend == minimizer_sketch_backend::scalar) {
                        if (execution_stats_out != nullptr) {
                            ++execution_stats_out->scalar_core_runs;
                        }
                        bioseq::simd::
                            collect_minimizers_segment_scalar<minimap_sketch_data>(
                                collector, seq, coll_lo, coll_hi, w, k);
                    } else {
                        index_corruption_panic(
                            "minimizers_per_fragment build: unsupported sketch backend");
                    }
                    for (const minimap_sketch_data& minimizer
                         : collector.extract_sketches_data()) {
                        const sequence_size_t global_pos =
                            static_cast<sequence_size_t>(minimizer.last_pos())
                            + emit_base;
                        if (global_pos < core_lo || global_pos >= core_hi) {
                            continue;
                        }
                        core.push_back(minimap_sketch_data{
                            minimizer.kmer(),
                            static_cast<std::uint64_t>(global_pos - core_lo),
                            minimizer.strand()});
                    }
                }
            } else if (execution_stats_out != nullptr) {
                ++execution_stats_out->short_core_runs;
            }
            minimizer_hashmap_fragment_index frag_idx;
            frag_idx.build(core, core_hi - core_lo);   // fragment-local target_len
            out_blob.resize(frag_idx.serialize_binary_size());
            frag_idx.serialize_to_binary(out_blob.data());
        };

        auto write_fragment_blob =
            [&storage](std::size_t fragment_no, const std::vector<char>& blob) {
            const std::size_t bytes = blob.size();
            storage.resize((index_sub_key_t)fragment_no, bytes);
            storage.template write_to_each<char>((index_sub_key_t)fragment_no,
                [&blob, bytes](std::size_t chunk_no, char* buf, std::size_t cap_units) {
                DEBUG_ASSERT(chunk_no == 0 && cap_units >= bytes, "one fragment index fits one chunk");
                if (bytes > 0) {
                    std::memcpy(buf, blob.data(), bytes);
                }
                return chunk_no;
            });
        };

        auto build_and_write_serial =
            [&build_fragment_blob, &write_fragment_blob](std::size_t fragment_no) {
            sketch_collector collector;
            std::vector<minimap_sketch_data> core;
            std::vector<char> blob;
            build_fragment_blob(fragment_no, collector, core, blob);
            write_fragment_blob(fragment_no, blob);
        };

        auto& pool = bioseq::parallelism::global_pool::instance();
        const bool may_parallel_fragments =
            execution_stats_out == nullptr
            && bioseq::parallelism::has_parallel_tasks(pool.get_mode())
            && pool.maximum_concurrency() > 1
            && bioseq::storage::is_worker_shareable(seq)
            && nfrag
                >= bioseq::parallelism::task_parallel_minimums::k_fragment_count;

        if (!may_parallel_fragments) {
            for (std::size_t fragment_no = 0; fragment_no < nfrag; ++fragment_no) {
                build_and_write_serial(fragment_no);
            }
        } else {
            const std::uint64_t build_peak =
                bioseq::parallelism::minimizer_index_peak_bytes_for_bp(frag_bp);
            const std::uint64_t peak_per_fragment =
                bioseq::common::saturating_mul(2, build_peak);

            for (std::size_t wave_lo = 0; wave_lo < nfrag;) {
                auto wave = bioseq::parallelism::try_reserve_budgeted_wave(
                    pool.maximum_concurrency(), peak_per_fragment);
                if (!wave.admitted) {
                    build_and_write_serial(wave_lo);
                    ++wave_lo;
                    continue;
                }

                const std::size_t wave_hi = std::min(
                    nfrag, wave_lo + static_cast<std::size_t>(wave.fanout));
                std::vector<std::vector<char>> blobs(wave_hi - wave_lo);

                bioseq::parallelism::parallel_for_pipeline(
                    0, blobs.size(), 1,
                    bioseq::parallelism::task_parallel_minimums::k_fragment_count,
                    bioseq::parallelism::task_parallel_region::fragment_build,
                    [&build_fragment_blob, &blobs, wave_lo](
                            std::size_t lo, std::size_t hi) {
                        sketch_collector collector;
                        std::vector<minimap_sketch_data> core;
                        for (std::size_t rel = lo; rel < hi; ++rel) {
                            build_fragment_blob(wave_lo + rel, collector, core, blobs[rel]);
                        }
                    });

                for (std::size_t rel = 0; rel < blobs.size(); ++rel) {
                    write_fragment_blob(wave_lo + rel, blobs[rel]);
                }
                wave_lo = wave_hi;
            }
        }
        // COMPLETENESS MANIFEST (written LAST): a tiny chunk at k_directory_sub_key recording nfrag.
        { const std::int64_t manifest = (std::int64_t)nfrag;
          storage.resize(k_directory_sub_key, sizeof(std::int64_t));
          storage.template write_to_each<std::int64_t>(k_directory_sub_key, [manifest](std::size_t chunk_no, std::int64_t* manifest_buf, std::size_t){ manifest_buf[0]=manifest; return chunk_no; }); }
    }

    template <sequence_alphabet Alphabet>
    static void build(
        S& storage,
        bioseq::storage::lazy_sequence_view<Alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        const std::uint8_t alphabet_code) {
        build(storage, seq, backend, kernels, nullptr, alphabet_code);
    }

    // The scalar-default overload delegates to the explicit backend.
    template <sequence_alphabet Alphabet>
    static void build(
            S& storage,
            bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        build(storage, seq, minimizer_sketch_backend::scalar, nullptr);
    }

    template <sequence_alphabet Alphabet>
    static void rebuild(
        S& storage,
        bioseq::storage::lazy_sequence_view<Alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code =
            bioseq::minimizers::unbound_packed_alphabet_code)
    {
        if (backend == minimizer_sketch_backend::two_stack_nthash
            && (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_state,
                "minimizers_per_fragment rebuild"};
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd
            && (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw packed_bounded_nthash_index_runtime_error{
                packed_bounded_nthash_index_status::invalid_state,
                "minimizers_per_fragment rebuild"};
        }
        storage.clear_entity();
        build(storage, seq, backend, kernels, nullptr, alphabet_code);
    }

    template <sequence_alphabet Alphabet>
    static void rebuild(
            S& storage,
            bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        rebuild(storage, seq, minimizer_sketch_backend::scalar, nullptr);
    }

    /// query-time: materialise the single pipeline IndexT for one target SEGMENT.
    static minimizer_hashmap_fragment_index assemble_segment(
            S& storage, std::size_t frag_lo, std::size_t frag_hi, sequence_size_t frag_bp,
            sequence_size_t total_bp, sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            minimizer_assembly_scratch& scratch) {
        auto& pairs = scratch.pairs;
        scratch.clear_segment();
        const std::size_t expected_nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if (storage.number_of_chunks(k_directory_sub_key) != 1) storage.index_error("minimizers_per_fragment: missing manifest");
        const auto* manifest_p = storage.template read_chunk<std::int64_t>(k_directory_sub_key, 0);
        if (*manifest_p != (std::int64_t)expected_nfrag) storage.index_error("minimizers_per_fragment: manifest nfrag mismatch");
        if (frag_hi > expected_nfrag) storage.index_error("minimizers_per_fragment: requested fragment past manifest nfrag");
        index_sub_key_t expected_f = (index_sub_key_t)frag_lo;
        storage.read_chunks_in_range((index_sub_key_t)frag_lo, (index_sub_key_t)frag_hi,
                [&](index_sub_key_t f, const char* blob, std::size_t blob_bytes){
            if (f != expected_f) storage.index_error("minimizers_per_fragment: missing or duplicate fragment chunk");
            ++expected_f;
            const sequence_size_t off = (sequence_size_t)f * frag_bp;
            storage.validate_minimizer_blob_or_error(blob, blob_bytes);
            wire_for_each_entry(blob, blob_bytes,
                [&](std::uint64_t kmer, std::uint64_t local_encoded){
                    const sequence_size_t gpos = (sequence_size_t)(local_encoded >> 1) + off;
                    if (gpos < clip_lo_bp || gpos >= clip_hi_bp) return;      // CLIP to the segment's collection range
                    pairs.emplace_back(kmer, ((std::uint64_t)gpos << 1) | (local_encoded & 1ULL));
                });
        });
        if (expected_f != (index_sub_key_t)frag_hi)
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        minimizer_hashmap_fragment_index merged;
        merged.build_from_pairs(pairs, total_bp);   // FULL target length
        return merged;
    }

    /// Query-filtered segment assembly for GPU seed lookup. Only query kmers
    /// can be probed by the seed stage, so persisted MPF does not need to
    /// materialise unrelated buckets before uploading the segment index.
    static minimizer_hashmap_fragment_index assemble_segment_for_query(
            S& storage,
            std::span<const minimap_sketch_data> query_minimizers,
            std::size_t frag_lo, std::size_t frag_hi, sequence_size_t frag_bp,
            sequence_size_t total_bp, sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            minimizer_assembly_scratch& scratch) {
        scratch.clear_query();
        auto& pairs = scratch.pairs;
        auto& qkmers = scratch.query_kmers;
        qkmers.reserve(query_minimizers.size());
        for (const minimap_sketch_data& qm : query_minimizers) {
            qkmers.push_back(qm.kmer());
        }
        std::sort(qkmers.begin(), qkmers.end());
        qkmers.erase(std::unique(qkmers.begin(), qkmers.end()), qkmers.end());

        clip_hi_bp = std::min(clip_hi_bp, total_bp);
        if (clip_lo_bp > clip_hi_bp) {
            clip_lo_bp = clip_hi_bp;
        }

        const std::size_t expected_nfrag = (std::size_t)((total_bp + frag_bp - 1) / frag_bp);
        if (storage.number_of_chunks(k_directory_sub_key) != 1) storage.index_error("minimizers_per_fragment: missing manifest");
        const auto* manifest_p = storage.template read_chunk<std::int64_t>(k_directory_sub_key, 0);
        if (*manifest_p != (std::int64_t)expected_nfrag) storage.index_error("minimizers_per_fragment: manifest nfrag mismatch");
        if (frag_hi > expected_nfrag) storage.index_error("minimizers_per_fragment: requested fragment past manifest nfrag");
        index_sub_key_t expected_f = (index_sub_key_t)frag_lo;
        storage.read_chunks_in_range((index_sub_key_t)frag_lo, (index_sub_key_t)frag_hi,
                [&](index_sub_key_t f, const char* blob, std::size_t blob_bytes){
            if (f != expected_f) storage.index_error("minimizers_per_fragment: missing or duplicate fragment chunk");
            ++expected_f;
            const sequence_size_t off = (sequence_size_t)f * frag_bp;
            storage.validate_minimizer_blob_or_error(blob, blob_bytes);
            wire_for_each_entry(blob, blob_bytes,
                [&](std::uint64_t kmer, std::uint64_t local_encoded){
                    if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer)) return;
                    const sequence_size_t gpos = (sequence_size_t)(local_encoded >> 1) + off;
                    if (gpos < clip_lo_bp || gpos >= clip_hi_bp) return;
                    pairs.emplace_back(kmer, ((std::uint64_t)gpos << 1) | (local_encoded & 1ULL));
                });
        });
        if (expected_f != (index_sub_key_t)frag_hi)
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        minimizer_hashmap_fragment_index merged;
        merged.build_from_pairs(pairs, total_bp);
        return merged;
    }
};

}  // namespace bioseq::indexing

export namespace bioseq::indexing {
template <> struct index_descriptor<minimizers_per_fragment_index> {
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view  index_name = minimizer_hashmap_fragment_index::index_name;
    static constexpr bool allows_update_on_insert = true;   // cheap per-fragment build; DEFAULT on-insert
};
static_assert(requires { index_descriptor<minimizers_per_fragment_index>::index_name; },
              "minimizers_per_fragment_index index_descriptor must be exported");
}
