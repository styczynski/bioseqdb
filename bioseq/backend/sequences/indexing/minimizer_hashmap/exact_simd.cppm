/**
 * @file minimizer_hashmap/exact_simd.cppm
 * @brief Target-neutral exact SIMD and bounded ntHash indexing bridges.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:exact_simd;

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export namespace bioseq::indexing {

/// Packed minimizer record shared with the selected sketch kernels.
using bioseq::minimizers::minimap_sketch_data;
/// Runtime selector for the available minimizer sketch backends.
using bioseq::minimizers::minimizer_sketch_backend;
/// Collector receiving packed minimizer records from sketch kernels.
using bioseq::minimizers::sketch_collector;
/// SIMD view that emits minimizers from packed sequence storage.
using bioseq::simd::collect_minimizers_view;
/// Unsigned coordinate type used for sequence lengths and base-pair positions.
using bioseq::storage::sequence_size_t;

/// Target-neutral status returned by the selected exact SIMD minimizer table.
/// The implementation unit maps target-family failures onto these stable values.
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

/// Return the stable diagnostic name for an exact SIMD indexing status.
[[nodiscard]] constexpr std::string_view
exact_simd_index_status_name(exact_simd_index_status status) noexcept {
    switch (status) {
    case exact_simd_index_status::ok:
        return "ok";
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

/// Reports a fail-closed exact SIMD indexing rejection.
class exact_simd_index_runtime_error final : public std::runtime_error {
  public:
    /// Construct an error for one rejected indexing operation.
    exact_simd_index_runtime_error(exact_simd_index_status status, std::string_view operation)
        : std::runtime_error(std::string{operation} +
                             ": exact_simd minimizer execution rejected: " +
                             std::string{exact_simd_index_status_name(status)}),
          status_(status) {}

    /// Return the exact rejection status.
    [[nodiscard]] exact_simd_index_status status() const noexcept {
        return status_;
    }

  private:
    exact_simd_index_status status_;
};

/// Describes one packed fragment borrowed for a synchronous bridge callback.
struct exact_simd_index_packed_fragment {
    const void* data = nullptr;
    std::size_t storage_units = 0;
};

/// Reads one packed fragment by ordinal from an erased sequence context.
using exact_simd_index_read_fragment = exact_simd_index_packed_fragment (*)(void*, std::size_t);
/// Release a fragment returned by an erased read callback.
using exact_simd_index_release_fragment = void (*)(void*, std::size_t) noexcept;
/// Appends typed minimizer records to an erased collector.
using exact_simd_index_append_records = bool (*)(void*, const minimap_sketch_data*, std::size_t);
/// Appends raw minimizer words to an erased collector.
using exact_simd_index_append_raw = bool (*)(void*, const std::uint64_t*, std::size_t);

/// Supplies packed sequence geometry and callbacks to the exact SIMD bridge.
struct exact_simd_index_bridge_request {
    const bioseq::minimizers::minimizer_kernels* kernels = nullptr;
    void* sequence_context = nullptr;
    exact_simd_index_read_fragment read_fragment = nullptr;
    exact_simd_index_release_fragment release_fragment = nullptr;
    void* collector_context = nullptr;
    exact_simd_index_append_records append_records = nullptr;
    exact_simd_index_append_raw append_raw = nullptr;
    std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code;
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

/// Counts exact SIMD work and any forbidden scalar or fallback activity.
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

    [[nodiscard]] constexpr bool
    operator==(const exact_simd_index_execution_stats&) const noexcept = default;

    /// Saturating-add one counter and record overflow.
    static void
    add(std::uint64_t& destination, std::uint64_t increment, bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)() - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    /// Merge counters from another checked execution.
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

    /// Check that the receipt proves the requested exact vector route.
    [[nodiscard]] bool
    is_internally_consistent(bool require_nonempty_vector_work = false) const noexcept {
        const bool exact = !saturated && fail_closed_rejections == 0 &&
                           scalar_backend_branches == 0 && scalar_tail_symbols == 0 &&
                           scalar_hash_symbols == 0 && scalar_window_transitions == 0 &&
                           scalar_event_records == 0 && scalar_sort_records == 0 &&
                           scalar_dedup_records == 0 && packed_decode_lanes == bp_consumed;
        return exact && (!require_nonempty_vector_work ||
                         (bp_consumed > 0 && vector_batches > 0 && packed_decode_batches > 0));
    }
};

/// Returns the exact bridge status together with operation-originated counters.
struct exact_simd_index_bridge_result {
    exact_simd_index_status status = exact_simd_index_status::ok;
    exact_simd_index_execution_stats stats{};
};

/// Execute an erased exact SIMD bridge request.
[[nodiscard]] exact_simd_index_bridge_result
collect_exact_simd_index_erased(const exact_simd_index_bridge_request& request);

/// Execute one lazy-sequence interval or throw its exact rejection status.
///
/// @tparam p_collector Collector accepting typed records, raw words, or individual records.
/// @tparam p_alphabet Packed sequence alphabet, restricted at runtime to DNA4 or DNA5.
template <class p_collector, sequence_alphabet p_alphabet>
[[nodiscard]] inline exact_simd_index_execution_stats
collect_exact_simd_index_range_or_throw(p_collector& collector,
                                        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                        std::uint8_t alphabet_code,
                                        sequence_size_t lo_bp,
                                        sequence_size_t hi_bp,
                                        std::size_t runtime_w,
                                        std::size_t runtime_k,
                                        std::size_t emitted_position_base,
                                        const bioseq::minimizers::minimizer_kernels* kernels,
                                        std::string_view operation) {
    static_assert(std::same_as<typename minimap_sketch_data::storage_type, std::uint64_t>);

    if constexpr (!std::same_as<p_alphabet, seq::dna4> && !std::same_as<p_alphabet, seq::dna5>) {
        throw exact_simd_index_runtime_error{exact_simd_index_status::unsupported_alphabet,
                                             operation};
    } else {
        if (lo_bp > hi_bp || hi_bp > sequence.bp_size() ||
            hi_bp - lo_bp >
                static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
            throw exact_simd_index_runtime_error{exact_simd_index_status::invalid_range, operation};
        }
        if (kernels == nullptr || kernels->two_stack_nthash == nullptr) {
            throw exact_simd_index_runtime_error{exact_simd_index_status::invalid_state, operation};
        }
        constexpr std::uint8_t packed_width = sizeof(std::uint32_t);
        const auto access = bioseq::storage::plan_sequence_access(
            sequence,
            bioseq::storage::sequence_access_requirement::contiguous_resident,
            lo_bp,
            hi_bp,
            packed_width);
        std::optional<bioseq::storage::admitted_packed_sequence<p_alphabet>> owner;
        typename bioseq::storage::lazy_sequence_view<p_alphabet>::resident_packed_sequence
            descriptor{};
        if (access.route == bioseq::storage::sequence_access_route::borrow_resident &&
            access.resident.has_value()) {
            descriptor = *access.resident;
        } else {
            auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                access.admission_bytes);
            if (!permit) {
                throw exact_simd_index_runtime_error{exact_simd_index_status::non_contiguous_range,
                                                     operation};
            }
            owner.emplace(bioseq::storage::materialize_packed_sequence(
                sequence, lo_bp, hi_bp, packed_width, std::move(*permit)));
            descriptor = owner->descriptor();
        }
        if (descriptor.logical_length != static_cast<std::size_t>(hi_bp - lo_bp) ||
            descriptor.storage_unit_width != packed_width ||
            descriptor.bits_per_symbol !=
                bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol ||
            descriptor.leading_symbol_offset != 0u ||
            descriptor.bit_order !=
                bioseq::storage::lazy_sequence_view<
                    p_alphabet>::packed_symbol_order::least_significant_symbol_first ||
            (descriptor.logical_length != 0u && descriptor.units == nullptr)) {
            throw exact_simd_index_runtime_error{exact_simd_index_status::invalid_range, operation};
        }

        /// Borrowed contiguous packed storage exposed through the erased bridge callbacks.
        struct sequence_context {
            const void* data{};
            std::size_t unit_count{};
        } sequence_state{descriptor.units, descriptor.unit_count};

        // Expose the one admitted contiguous packed fragment to the erased bridge.
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
        // Release is a no-op because the admitted or borrowed owner spans this synchronous call.
        const auto release_fragment = +[](void*, std::size_t) noexcept {};
        // Forward typed bridge records to the strongest collector interface available.
        const auto append_records =
            +[](void* opaque, const minimap_sketch_data* records, std::size_t count) {
                auto& collector = *static_cast<p_collector*>(opaque);
                if constexpr (requires {
                                  {
                                      collector.append_records(
                                          std::span<const minimap_sketch_data>{records, count})
                                  } -> std::convertible_to<bool>;
                              }) {
                    return static_cast<bool>(collector.append_records(
                        std::span<const minimap_sketch_data>{records, count}));
                } else if constexpr (requires(const minimap_sketch_data& record) {
                                         collector.collect(record);
                                     }) {
                    for (std::size_t index = 0u; index < count; ++index) {
                        collector.collect(records[index]);
                    }
                    if constexpr (requires {
                                      {
                                          std::as_const(collector).is_ok()
                                      } -> std::convertible_to<bool>;
                                  }) {
                        return static_cast<bool>(std::as_const(collector).is_ok());
                    }
                    return true;
                } else {
                    return false;
                }
            };
        // Forward raw bridge words or reconstruct typed records for scalar collector interfaces.
        const auto append_raw = +[](void* opaque, const std::uint64_t* raw, std::size_t count) {
            auto& collector = *static_cast<p_collector*>(opaque);
            if constexpr (requires {
                              {
                                  collector.append_raw_storage(raw, count)
                              } -> std::convertible_to<bool>;
                          }) {
                return static_cast<bool>(collector.append_raw_storage(raw, count));
            } else if constexpr (requires(const minimap_sketch_data& record) {
                                     collector.collect(record);
                                 }) {
                for (std::size_t index = 0u; index < count; ++index) {
                    collector.collect(minimap_sketch_data::from_raw_storage(raw[index]));
                }
                if constexpr (requires {
                                  { std::as_const(collector).is_ok() } -> std::convertible_to<bool>;
                              }) {
                    return static_cast<bool>(std::as_const(collector).is_ok());
                }
                return true;
            } else {
                return false;
            }
        };

        const exact_simd_index_bridge_result result =
            collect_exact_simd_index_erased(exact_simd_index_bridge_request{
                .kernels = kernels,
                .sequence_context = &sequence_state,
                .read_fragment = read_fragment,
                .release_fragment = release_fragment,
                .collector_context = &collector,
                .append_records = append_records,
                .append_raw = append_raw,
                .alphabet_code = alphabet_code,
                .bits_per_symbol = descriptor.bits_per_symbol,
                .storage_unit_bytes = packed_width,
                .letters_per_unit =
                    packed_width * 8u /
                    bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol,
                .total_units = descriptor.unit_count,
                .total_bp = static_cast<std::size_t>(hi_bp - lo_bp),
                .fragment_units = std::max<std::size_t>(descriptor.unit_count, 1u),
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

/// Target-neutral status for the checked packed bounded ntHash route.
/// The implementation unit maps the selected target status and admission result
/// without instantiating the algorithm in this interface.
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

/// Return the stable diagnostic name for a bounded ntHash indexing status.
[[nodiscard]] constexpr std::string_view
packed_bounded_nthash_index_status_name(packed_bounded_nthash_index_status status) noexcept {
    switch (status) {
    case packed_bounded_nthash_index_status::ok:
        return "ok";
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

/// Reports a fail-closed checked bounded ntHash rejection.
class packed_bounded_nthash_index_runtime_error final : public std::runtime_error {
  public:
    /// Construct an error for one rejected indexing operation.
    packed_bounded_nthash_index_runtime_error(packed_bounded_nthash_index_status status,
                                              std::string_view operation)
        : std::runtime_error(std::string(operation) + ": checked bounded ntHash route failed (" +
                             std::string(packed_bounded_nthash_index_status_name(status)) + ")"),
          status_(status) {}

    /// Return the exact rejection status.
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
/// Holds the next integration-only bounded ntHash status override.
inline std::atomic<std::uint8_t> g_forced_packed_bounded_index_status{
    static_cast<std::uint8_t>(packed_bounded_nthash_index_status::ok)};
#endif

/// Arm the integration-only one-shot rejection hook.
inline void arm_forced_packed_bounded_index_status_for_test(
    packed_bounded_nthash_index_status status) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    g_forced_packed_bounded_index_status.store(static_cast<std::uint8_t>(status),
                                               std::memory_order_release);
#else
    (void)status;
#endif
}

/// Clear the integration-only one-shot rejection hook.
inline void disarm_forced_packed_bounded_index_status_for_test() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    g_forced_packed_bounded_index_status.store(
        static_cast<std::uint8_t>(packed_bounded_nthash_index_status::ok),
        std::memory_order_release);
#endif
}

/// Consume and clear the integration-only forced status when one is armed.
[[nodiscard]] inline std::optional<packed_bounded_nthash_index_status>
consume_forced_packed_bounded_index_status_for_test() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    const auto status = static_cast<packed_bounded_nthash_index_status>(
        g_forced_packed_bounded_index_status.exchange(
            static_cast<std::uint8_t>(packed_bounded_nthash_index_status::ok),
            std::memory_order_acq_rel));
    if (status != packed_bounded_nthash_index_status::ok)
        return status;
#endif
    return std::nullopt;
}

/// Persisted scheme identifier produced by the bounded ntHash kernel.
inline constexpr std::uint32_t k_packed_bounded_nthash_kernel_scheme_id = 0x504E5431u;
/// Largest window accepted by the checked bounded ntHash kernel.
inline constexpr std::size_t k_packed_bounded_nthash_maximum_window = 64u;

/** Complete non-Highway mirror of the checked kernel execution stats. */
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

    [[nodiscard]] constexpr bool
    operator==(const packed_bounded_nthash_index_execution_stats&) const noexcept = default;

    /// Saturating-add one counter and record overflow.
    static void
    add(std::uint64_t& destination, std::uint64_t increment, bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)() - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    /** Combine independently checked bounded windows without losing stats. */
    void merge(const packed_bounded_nthash_index_execution_stats& other) noexcept {
        if (other == packed_bounded_nthash_index_execution_stats{})
            return;
        if (*this == packed_bounded_nthash_index_execution_stats{}) {
            *this = other;
            return;
        }
        const bool same_identity = segments_combined > 0 && other.segments_combined > 0 &&
                                   scheme_id == other.scheme_id && kmer_size == other.kmer_size &&
                                   window_size == other.window_size &&
                                   nthash_rotation == other.nthash_rotation &&
                                   source_storage_unit_bytes == other.source_storage_unit_bytes &&
                                   logical_streams == other.logical_streams &&
                                   physical_vector_lanes == other.physical_vector_lanes &&
                                   vector_registers_per_step == other.vector_registers_per_step &&
                                   positions_are_segment_local == other.positions_are_segment_local;
        aggregation_mismatch = aggregation_mismatch || other.aggregation_mismatch || !same_identity;
        saturated = saturated || other.saturated;
        add(input_symbols, other.input_symbols, saturated);
        add(input_storage_units, other.input_storage_units, saturated);
        add(segments_combined, other.segments_combined, saturated);
        add(packed_decode_batches, other.packed_decode_batches, saturated);
        add(packed_decode_lane_visits, other.packed_decode_lane_visits, saturated);
        add(hash_initialization_batches, other.hash_initialization_batches, saturated);
        add(rolling_hash_batches, other.rolling_hash_batches, saturated);
        add(hash_table_lookup_batches, other.hash_table_lookup_batches, saturated);
        add(canonical_sum_batches, other.canonical_sum_batches, saturated);
        add(window_update_batches, other.window_update_batches, saturated);
        add(suffix_rebuild_batches, other.suffix_rebuild_batches, saturated);
        add(emission_decision_batches, other.emission_decision_batches, saturated);
        add(emission_mask_store_batches, other.emission_mask_store_batches, saturated);
        add(minimizers_emitted, other.minimizers_emitted, saturated);
        add(tied_endpoint_records_emitted, other.tied_endpoint_records_emitted, saturated);
        add(stream_boundary_records_buffered, other.stream_boundary_records_buffered, saturated);
        add(stream_boundary_records_committed, other.stream_boundary_records_committed, saturated);
        add(stream_boundary_duplicates_suppressed,
            other.stream_boundary_duplicates_suppressed,
            saturated);
        add(scalar_collector_appends, other.scalar_collector_appends, saturated);
        add(partial_chunk_callback_batches, other.partial_chunk_callback_batches, saturated);
        add(masked_tail_lane_visits, other.masked_tail_lane_visits, saturated);
        add(fail_closed_rejections, other.fail_closed_rejections, saturated);
        add(scalar_hash_symbols, other.scalar_hash_symbols, saturated);
        add(scalar_window_transitions, other.scalar_window_transitions, saturated);
        add(scalar_minimum_decisions, other.scalar_minimum_decisions, saturated);
        add(fallback_calls, other.fallback_calls, saturated);
    }

    /// Check that the receipt proves the requested bounded vector route.
    [[nodiscard]] constexpr bool
    is_internally_consistent(bool require_nonempty_vector_work = true) const noexcept {
        const bool vector_work_ok =
            !require_nonempty_vector_work ||
            (packed_decode_batches > 0 && packed_decode_lane_visits > 0 &&
             hash_initialization_batches > 0 && hash_table_lookup_batches > 0 &&
             canonical_sum_batches > 0 && window_update_batches > 0 &&
             emission_decision_batches > 0 && emission_mask_store_batches > 0);
        return !saturated && !aggregation_mismatch &&
               scheme_id == k_packed_bounded_nthash_kernel_scheme_id && kmer_size > 0 &&
               window_size > 0 && window_size <= k_packed_bounded_nthash_maximum_window &&
               nthash_rotation == 7 && source_storage_unit_bytes >= sizeof(std::uint32_t) &&
               logical_streams == 8 && (physical_vector_lanes == 4 || physical_vector_lanes == 8) &&
               vector_registers_per_step * physical_vector_lanes == 8 && segments_combined > 0 &&
               positions_are_segment_local && vector_work_ok &&
               canonical_sum_batches == window_update_batches &&
               minimizers_emitted == scalar_collector_appends &&
               stream_boundary_records_buffered ==
                   stream_boundary_records_committed + stream_boundary_duplicates_suppressed &&
               fail_closed_rejections == 0 && scalar_hash_symbols == 0 &&
               scalar_window_transitions == 0 && scalar_minimum_decisions == 0 &&
               fallback_calls == 0;
    }
};

/// Supplies one packed logical interval to the erased bounded ntHash bridge.
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

/// Returns the bounded bridge status and its operation-originated counters.
struct packed_bounded_nthash_index_bridge_result {
    packed_bounded_nthash_index_status status = packed_bounded_nthash_index_status::ok;
    packed_bounded_nthash_index_execution_stats stats{};
};

/// Build the canonical one-rejection receipt for a fail-closed status.
[[nodiscard]] constexpr packed_bounded_nthash_index_bridge_result
rejected_packed_bounded_nthash_index_result(packed_bounded_nthash_index_status status) noexcept {
    return {
        .status = status,
        .stats =
            packed_bounded_nthash_index_execution_stats{
                .fail_closed_rejections = 1,
            },
    };
}

/// Execute an erased checked bounded ntHash bridge request.
[[nodiscard]] packed_bounded_nthash_index_bridge_result collect_packed_bounded_nthash_index_erased(
    const packed_bounded_nthash_index_bridge_request& request);

/// Execute one bounded ntHash window through a canonical admitted u32
/// descriptor. Only the selected logical-rank interval is materialized when the source
/// cannot expose stable resident u32 storage. A stable resident interval is
/// borrowed synchronously without allocating or copying it.
///
/// @tparam p_alphabet Packed source alphabet, currently restricted to DNA4.
template <sequence_alphabet p_alphabet>
[[nodiscard]] packed_bounded_nthash_index_bridge_result
collect_packed_bounded_nthash_index_lazy(bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
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
    if constexpr (!std::same_as<p_alphabet, seq::dna4>) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::unsupported_alphabet);
    } else {
        if (lo_bp > hi_bp || hi_bp > sequence.bp_size() ||
            local_bp_size != static_cast<std::uint64_t>(hi_bp - lo_bp)) {
            return rejected_packed_bounded_nthash_index_result(
                packed_bounded_nthash_index_status::invalid_range);
        }
        if (local_bp_size == 0u ||
            local_bp_size - 1u >
                static_cast<std::uint64_t>(minimap_sketch_data::k_position_field_mask)) {
            return rejected_packed_bounded_nthash_index_result(
                packed_bounded_nthash_index_status::unsupported_representation);
        }
        if (local_bp_size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
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
            bioseq::storage::sequence_access_requirement::contiguous_resident,
            lo_bp,
            hi_bp,
            packed_width);
        std::optional<bioseq::storage::admitted_packed_sequence<p_alphabet>> owner;
        typename bioseq::storage::lazy_sequence_view<p_alphabet>::resident_packed_sequence
            descriptor{};
        if (access.route == bioseq::storage::sequence_access_route::borrow_resident &&
            access.resident.has_value()) {
            descriptor = *access.resident;
        } else {
            auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                access.admission_bytes);
            if (!permit) {
                return rejected_packed_bounded_nthash_index_result(
                    packed_bounded_nthash_index_status::admission_denied);
            }
            owner.emplace(bioseq::storage::materialize_packed_sequence(
                sequence, lo_bp, hi_bp, packed_width, std::move(*permit)));
            descriptor = owner->descriptor();
        }
        if (descriptor.logical_length != local_bp_size ||
            descriptor.storage_unit_width != packed_width ||
            descriptor.bits_per_symbol !=
                bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol ||
            descriptor.leading_symbol_offset != 0u ||
            descriptor.bit_order !=
                bioseq::storage::lazy_sequence_view<
                    p_alphabet>::packed_symbol_order::least_significant_symbol_first ||
            (descriptor.logical_length != 0u && descriptor.units == nullptr)) {
            return rejected_packed_bounded_nthash_index_result(
                packed_bounded_nthash_index_status::invalid_range);
        }
        if (materialized_window_bp_out != nullptr) {
            *materialized_window_bp_out = owner.has_value() ? local_bp_size : 0u;
            DEBUG_ASSERT((*materialized_window_bp_out != 0u) == owner.has_value(),
                         "bounded materialization receipt must name only owned storage");
        }
        if (const auto forced = consume_forced_packed_bounded_index_status_for_test()) {
            return rejected_packed_bounded_nthash_index_result(*forced);
        }
        return collect_packed_bounded_nthash_index_erased(
            packed_bounded_nthash_index_bridge_request{
                .kernels = kernels,
                .sequence =
                    bioseq::minimizers::packed_sequence_kernel_view{
                        .storage = reinterpret_cast<const std::byte*>(descriptor.units),
                        .storage_units = descriptor.unit_count,
                        .begin_symbol = 0u,
                        .end_symbol = static_cast<std::size_t>(local_bp_size),
                        .alphabet_code = alphabet_code,
                        .bits_per_symbol = descriptor.bits_per_symbol,
                        .storage_unit_bytes = descriptor.storage_unit_width,
                        .final_segment = true,
                    },
                .sequence_context = std::addressof(descriptor),
                .collector_context = std::addressof(collector),
                .storage_unit_bytes = packed_width,
                .letters_per_unit =
                    descriptor.storage_unit_width * 8u /
                    bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol,
                .storage_units = descriptor.unit_count,
                .local_bp_size = static_cast<std::size_t>(local_bp_size),
                .window_size = window_size,
                .kmer_size = kmer_size,
            });
    }
}

} // namespace bioseq::indexing
