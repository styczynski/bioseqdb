/**
 * @file implementation.cppm
 * @brief Implements the in-memory minimizer table and adapters for SIMD sketching.
 */
module;

#include <libassert/assert.hpp>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.indexing.minimizer_hashmap;

import bioseq.common.radix_sort;
import bioseq.common.heap_allocation;
import bioseq.indexing.type;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;

namespace bioseq::indexing::detail {

/** @brief Tracks live and peak bytes allocated by the transient hash table. */
struct minimizer_hashmap_table_allocation_state {
    std::uint64_t live_bytes = 0u;
    std::uint64_t peak_bytes = 0u;
};

/**
 * @brief Tracks memory allocated by the Boost minimizer table.
 * @tparam p_value Allocated value type
 */
template <class p_value>
struct minimizer_hashmap_table_allocator {
    /// Element type allocated for the Boost table.
    using value_type = p_value;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::false_type;

    minimizer_hashmap_table_allocation_state* state = nullptr;

    minimizer_hashmap_table_allocator() noexcept = default;
    /// Track this allocator's memory in the supplied table counters.
    explicit minimizer_hashmap_table_allocator(
            minimizer_hashmap_table_allocation_state* state_) noexcept
        : state(state_) {}

    /// Share the same memory counters when Boost changes the allocation element  type.
    /// @tparam p_other_value Source allocator element type.
    template <class p_other_value>
    minimizer_hashmap_table_allocator(
            const minimizer_hashmap_table_allocator<p_other_value>& other) noexcept
        : state(other.state) {}

    /// Allocate count elements and update the current and peak byte totals.
    /// @throws std::bad_alloc If the counters are missing, the size overflows or allocation fails.
    [[nodiscard]] p_value* allocate(std::size_t count) {
        if (state == nullptr
            || count > (std::numeric_limits<std::uint64_t>::max)()
                    / sizeof(p_value)) {
            throw std::bad_alloc{};
        }
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(count) * sizeof(p_value);
        if (state->live_bytes
            > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
            throw std::bad_alloc{};
        }
        state->live_bytes += bytes;
        state->peak_bytes = std::max(
            state->peak_bytes, state->live_bytes);
        try {
            return std::allocator<p_value>{}.allocate(count);
        } catch (...) {
            state->live_bytes -= bytes;
            throw;
        }
    }

    /// Deallocate `count` elements and subtract their bytes from live accounting.
    void deallocate(p_value* allocation, std::size_t count) noexcept {
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(count) * sizeof(p_value);
        DEBUG_ASSERT(state != nullptr && state->live_bytes >= bytes);
        std::allocator<p_value>{}.deallocate(allocation, count);
        if (state != nullptr && state->live_bytes >= bytes) {
            state->live_bytes -= bytes;
        }
    }

    /// Allow allocator types for different elements to share the same counters.
    /// @tparam p_other_value Other allocator element type.
    template <class p_other_value>
    friend struct minimizer_hashmap_table_allocator;

    template <class p_other_value>
    [[nodiscard]] bool operator==(
            const minimizer_hashmap_table_allocator<p_other_value>& other) const noexcept {
        return state == other.state;
    }
};

/** @brief Owns the in-memory hash table and its memory counters. */
struct minimizer_hashmap_fragment_index_table {
    /// Allocator that counts bytes used by the Boost table.
    using allocator_type = minimizer_hashmap_table_allocator<
        std::pair<const std::uint64_t, std::uint64_t>>;
    /// Maps a minimizer hash to a packed position offset and count.
    using table_type = boost::unordered_flat_map<
        std::uint64_t,
        std::uint64_t,
        boost::hash<std::uint64_t>,
        std::equal_to<std::uint64_t>,
        allocator_type>;

    minimizer_hashmap_table_allocation_state allocation;
    table_type table;

    /// Construct an empty table bound to its own allocation counters.
    minimizer_hashmap_fragment_index_table()
        : allocation{},
          table(
              0u,
              boost::hash<std::uint64_t>{},
              std::equal_to<std::uint64_t>{},
              allocator_type{&allocation}) {}

    /// Copy the table into storage trackeq by a new set of counters.
    minimizer_hashmap_fragment_index_table(
            const minimizer_hashmap_fragment_index_table& other)
        : minimizer_hashmap_fragment_index_table() {
        table = other.table;
    }

    /// Replace  the table contents while keeping this table's memory counters.
    minimizer_hashmap_fragment_index_table& operator=(
            const minimizer_hashmap_fragment_index_table& other) {
        if (this != &other) table = other.table;
        return *this;
    }
};

static_assert(BOOST_VERSION == 109000,
    "Update plan_minimizer_hashmap_table_allocation for a new Boost FOA layout");
static_assert(sizeof(minimizer_hashmap_fragment_index_table::table_type::value_type)
              == 2u * sizeof(std::uint64_t));
using pinned_foa_group = boost::unordered::detail::foa::group15<
    boost::unordered::detail::foa::plain_integral>;
static_assert(pinned_foa_group::N == 15u);
static_assert(sizeof(pinned_foa_group) == 16u);

}  // namespace bioseq::indexing::detail

namespace bioseq::indexing {

namespace {

using exact_status = bioseq::minimizers::two_stack_continuation_status;
using exact_stats = bioseq::minimizers::two_stack_execution_stats;

/** Convert a two-stack, kernel status to the status used by index builders. */
[[nodiscard]] constexpr exact_simd_index_status bridge_status(
        exact_status status) noexcept {
    switch (status) {
        case exact_status::ok: return exact_simd_index_status::ok;
        case exact_status::invalid_window_size:
            return exact_simd_index_status::invalid_window_size;
        case exact_status::invalid_kmer_size:
            return exact_simd_index_status::invalid_kmer_size;
        case exact_status::invalid_static_configuration:
            return exact_simd_index_status::invalid_static_configuration;
        case exact_status::unsupported_alphabet:
            return exact_simd_index_status::unsupported_alphabet;
        case exact_status::unsupported_storage:
            return exact_simd_index_status::unsupported_storage;
        case exact_status::unsupported_representation:
            return exact_simd_index_status::unsupported_representation;
        case exact_status::unsupported_vector_shape:
            return exact_simd_index_status::unsupported_vector_shape;
        case exact_status::not_initialized:
            return exact_simd_index_status::not_initialized;
        case exact_status::already_finalized:
            return exact_simd_index_status::already_finalized;
        case exact_status::invalid_state:
            return exact_simd_index_status::invalid_state;
        case exact_status::invalid_range:
            return exact_simd_index_status::invalid_range;
        case exact_status::non_contiguous_range:
            return exact_simd_index_status::non_contiguous_range;
        case exact_status::arithmetic_overflow:
            return exact_simd_index_status::arithmetic_overflow;
        case exact_status::collector_overflow:
            return exact_simd_index_status::collector_overflow;
    }
    return exact_simd_index_status::invalid_state;
}

/** Copy two-stack execution counts into the index builder's result type. */
[[nodiscard]] constexpr exact_simd_index_execution_stats bridge_exact_stats(
        const exact_stats& stats) noexcept {
    return exact_simd_index_execution_stats{
        .bp_consumed = stats.bp_consumed,
        .minimizers_emitted = stats.minimizers_emitted,
        .vector_batches = stats.vector_batches,
        .masked_tail_lanes = stats.masked_tail_lanes,
        .packed_decode_batches = stats.packed_decode_batches,
        .packed_decode_lanes = stats.packed_decode_lanes,
        .ambiguous_symbols = stats.ambiguous_symbols,
        .ambiguous_reset_batches = stats.ambiguous_reset_batches,
        .scalar_backend_branches = stats.scalar_backend_branches,
        .scalar_tail_symbols = stats.scalar_tail_symbols,
        .scalar_hash_symbols = stats.scalar_hash_symbols,
        .scalar_window_transitions = stats.scalar_window_transitions,
        .scalar_event_records = stats.scalar_event_records,
        .scalar_sort_records = stats.scalar_sort_records,
        .scalar_dedup_records = stats.scalar_dedup_records,
        .fail_closed_rejections = stats.fail_closed_rejections,
        .saturated = stats.saturated,
    };
}

/** Return a failed exact-SIMD result and count one rejected request. */
[[nodiscard]] constexpr exact_simd_index_bridge_result rejected_bridge_result(
        exact_simd_index_status status) noexcept {
    return {
        .status = status,
        .stats = exact_simd_index_execution_stats{
            .fail_closed_rejections = 1},
    };
}

/** Passes kernel output batches to the index builder's collector */
struct kernel_sink_adapter {
    using sketch_type = bioseq::minimizers::minimap_sketch_data;
    static constexpr std::size_t batch_capacity = 256u;
    static constexpr auto output_layout =
        bioseq::minimizers::k_minimizer_output_layout<sketch_type>;
    static_assert(sizeof(sketch_type) == sizeof(std::uint64_t));
    static_assert(std::is_trivially_copyable_v<sketch_type>);
    static_assert(std::is_trivially_destructible_v<sketch_type>);
    static_assert(std::is_standard_layout_v<sketch_type>);
    static_assert(std::same_as<sketch_type::storage_type, std::uint64_t>);

    /// Connect the collector to the adapter's fixed output buffer.
    explicit kernel_sink_adapter(
            void* context,
            exact_simd_index_append_records records_callback,
            exact_simd_index_append_raw raw_callback) noexcept
        : collector_context(context),
          append_records(records_callback),
          append_raw(raw_callback) {}

    void* collector_context = nullptr;
    exact_simd_index_append_records append_records = nullptr;
    exact_simd_index_append_raw append_raw = nullptr;
    alignas(sketch_type) std::array<std::byte, batch_capacity * sizeof(sketch_type)> batch;
    std::exception_ptr exception{};
    std::size_t committed = 0u;
    bool accepted = true;
    bool acquired = false;

    /**
     * Check that every acquired batch was committed and that the output count
     * agrees with the kernel. Rethrow  any exception saved from the collector.
     */
    void validate(
            const bioseq::minimizers::minimizer_kernel_result& result) {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
        if (acquired || result.emitted != committed) {
            accepted = false;
        }
    }

    /**
     * Lend the kernel a buffer for up to 256 records. Reject a second outstanding
     * batch, a larger request or a collector that has already failed.
     */
    [[nodiscard]] static bioseq::minimizers::minimizer_sink_batch_view acquire(
            void* opaque,
            std::size_t minimum) {
        auto& self = *static_cast<kernel_sink_adapter*>(opaque);
        if (!self.accepted || self.acquired || minimum > batch_capacity) {
            self.accepted = false;
            return {};
        }
        self.acquired = true;
        return {
            .data = self.batch.data(),
            .capacity = batch_capacity,
            .layout = output_layout,
            .record_size = sizeof(sketch_type),
            .record_alignment = alignof(sketch_type),
        };
    }

    /**
     * Send the initialized records to the collector and release the batch.
     * Save collector exceptions for validate() to rethrow after the kernel returns.
     */
    static void commit(void* opaque, std::size_t initialized) noexcept {
        auto& self = *static_cast<kernel_sink_adapter*>(opaque);
        if (!self.acquired || initialized > batch_capacity) {
            self.accepted = false;
            return;
        }
        auto* records = std::launder(
            reinterpret_cast<sketch_type*>(self.batch.data()));
        // Destroy the records and make the buffer available for another batch.
        const auto destroy_records =
            [&self, records, initialized]() noexcept {
                std::destroy_n(records, initialized);
                self.acquired = false;
            };
        if (!self.accepted || self.append_records == nullptr) {
            destroy_records();
            self.accepted = false;
            return;
        }
        try {
            self.accepted = self.append_records(
                self.collector_context, records, initialized);
        } catch (...) {
            self.exception = std::current_exception();
            self.accepted = false;
        }
        destroy_records();
        if (self.accepted) {
            self.committed += initialized;
        }
    }

    /**
     * Pass packed 64-bit records directly to the collector. Reject  a mismatched
     * layout or an unfinished typed batch. Return whether the collector accepted them.
     */
    [[nodiscard]] static bool append_raw_u64(
            void* opaque,
            bioseq::minimizers::minimizer_output_layout layout,
            const std::uint64_t* values,
            std::size_t count) noexcept {
        auto& self = *static_cast<kernel_sink_adapter*>(opaque);
        if (!self.accepted || self.acquired
            || self.append_raw == nullptr
            || layout != bioseq::minimizers::minimizer_output_layout::
                sequence_log2_28_kmer_15_uint64
            || (count != 0u && values == nullptr)) {
            self.accepted = false;
            return false;
        }
        try {
            self.accepted = self.append_raw(
                self.collector_context, values, count);
        } catch (...) {
            self.exception = std::current_exception();
            self.accepted = false;
        }
        if (self.accepted) {
            self.committed += count;
        }
        return self.accepted;
    }

    /** Return kernel callbacks that borrow this adapter. */
    [[nodiscard]] bioseq::minimizers::minimizer_sink sink() noexcept {
        return {
            .state = this,
            .acquire = acquire,
            .commit = commit,
            .append_raw_u64 = append_raw_u64,
        };
    }
};

/** Lets the exacx-SIMD kernel read packed sequence fragments on demand. */
struct exact_fragment_source_adapter {
    const exact_simd_index_bridge_request* request = nullptr;
    std::size_t letters_per_unit = 0u;
    std::size_t fragment_bp_capacity = 0u;
    std::size_t consume_begin_bp = 0u;
    std::size_t consume_end_bp = 0u;

    /** Borrow a packed fragment and limit consumption to the requested base range. */
    [[nodiscard]] static bioseq::minimizers::packed_sequence_kernel_fragment read(
            void* opaque,
            std::size_t fragment_no) {
        auto& self = *static_cast<exact_fragment_source_adapter*>(opaque);
        const auto& request = *self.request;
        const exact_simd_index_packed_fragment fragment =
            request.read_fragment(request.sequence_context, fragment_no);
        const std::size_t fragment_begin_bp =
            fragment_no * self.fragment_bp_capacity;
        const std::size_t packed_bp_capacity = fragment.storage_units
                > (std::numeric_limits<std::size_t>::max)()
                    / self.letters_per_unit
            ? (std::numeric_limits<std::size_t>::max)()
            : fragment.storage_units * self.letters_per_unit;
        const std::size_t fragment_bp_size = std::min(
            packed_bp_capacity,
            request.total_bp - std::min(request.total_bp, fragment_begin_bp));
        return {
            .storage = reinterpret_cast<const std::byte*>(fragment.data),
            .storage_units = fragment.storage_units,
            .fragment_bp_size = fragment_bp_size,
            .fragment_begin_bp = fragment_begin_bp,
            .consume_begin_bp = std::max(
                self.consume_begin_bp, fragment_begin_bp),
            .consume_end_bp = std::min(
                self.consume_end_bp, fragment_begin_bp + fragment_bp_size),
        };
    }

    /** Return a borrowed fragment to the sequence source. */
    static void release(void* opaque, std::size_t fragment_no) noexcept {
        const auto& self = *static_cast<exact_fragment_source_adapter*>(opaque);
        self.request->release_fragment(
            self.request->sequence_context, fragment_no);
    }
};

/**
 * Convert the exact kernel result to an index status. Collector, rejection takes
 * priority, followed by the two-stack  error and then the generic kernel error
 */
[[nodiscard]] constexpr exact_simd_index_status exact_status_from_kernel(
        const bioseq::minimizers::minimizer_kernel_result& result,
        bool sink_accepted) noexcept {
    using kernel_status = bioseq::minimizers::minimizer_kernel_status;
    if (!sink_accepted || result.status == kernel_status::sink_rejected) {
        return exact_simd_index_status::collector_overflow;
    }
    if (result.family_status != static_cast<std::uint32_t>(exact_status::ok)) {
        return bridge_status(static_cast<exact_status>(result.family_status));
    }
    switch (result.status) {
        case kernel_status::ok:
            return exact_simd_index_status::ok;
        case kernel_status::invalid_sequence_view:
            return exact_simd_index_status::invalid_range;
        case kernel_status::invalid_configuration:
            return exact_simd_index_status::invalid_static_configuration;
        case kernel_status::invalid_continuation:
            return exact_simd_index_status::invalid_state;
        case kernel_status::sink_rejected:
            return exact_simd_index_status::collector_overflow;
    }
    return exact_simd_index_status::invalid_state;
}

using packed_bounded_status = bioseq::minimizers::bounded_nthash_status;
using packed_bounded_stats = bioseq::minimizers::bounded_nthash_execution_stats;

static_assert(k_packed_bounded_nthash_kernel_scheme_id
    == bioseq::minimizers::k_bounded_nthash_scheme_id);
static_assert(k_packed_bounded_nthash_maximum_window
    == bioseq::minimizers::k_bounded_nthash_max_window);

/** Convert a bounded-ntHash kernel status to the status used by index builders. */
[[nodiscard]] constexpr packed_bounded_nthash_index_status bridge_bounded_status(
        packed_bounded_status status) noexcept {
    switch (status) {
        case packed_bounded_status::ok:
            return packed_bounded_nthash_index_status::ok;
        case packed_bounded_status::invalid_configuration:
            return packed_bounded_nthash_index_status::invalid_configuration;
        case packed_bounded_status::input_too_short:
            return packed_bounded_nthash_index_status::input_too_short;
        case packed_bounded_status::unsupported_alphabet:
            return packed_bounded_nthash_index_status::unsupported_alphabet;
        case packed_bounded_status::unsupported_storage:
            return packed_bounded_nthash_index_status::unsupported_storage;
        case packed_bounded_status::unsupported_representation:
            return packed_bounded_nthash_index_status::unsupported_representation;
        case packed_bounded_status::unsupported_vector_shape:
            return packed_bounded_nthash_index_status::unsupported_vector_shape;
        case packed_bounded_status::invalid_range:
            return packed_bounded_nthash_index_status::invalid_range;
        case packed_bounded_status::input_too_large:
            return packed_bounded_nthash_index_status::input_too_large;
    }
    return packed_bounded_nthash_index_status::invalid_state;
}

/** Copy bounded-ntHash settings and execution counts into the index result. */
[[nodiscard]] constexpr packed_bounded_nthash_index_execution_stats
bridge_bounded_stats(const packed_bounded_stats& stats) noexcept {
    return {
        .scheme_id = stats.scheme_id,
        .kmer_size = stats.kmer_size,
        .window_size = stats.window_size,
        .nthash_rotation = stats.nthash_rotation,
        .source_storage_unit_bytes = stats.source_storage_unit_bytes,
        .input_symbols = stats.input_symbols,
        .input_storage_units = stats.input_storage_units,
        .segments_combined = stats.segments_combined,
        .logical_streams = stats.logical_streams,
        .physical_vector_lanes = stats.physical_vector_lanes,
        .vector_registers_per_step = stats.vector_registers_per_step,
        .packed_decode_batches = stats.packed_decode_batches,
        .packed_decode_lane_visits = stats.packed_decode_lane_visits,
        .hash_initialization_batches = stats.hash_initialization_batches,
        .rolling_hash_batches = stats.rolling_hash_batches,
        .hash_table_lookup_batches = stats.hash_table_lookup_batches,
        .canonical_sum_batches = stats.canonical_sum_batches,
        .window_update_batches = stats.window_update_batches,
        .suffix_rebuild_batches = stats.suffix_rebuild_batches,
        .emission_decision_batches = stats.emission_decision_batches,
        .emission_mask_store_batches = stats.emission_mask_store_batches,
        .minimizers_emitted = stats.minimizers_emitted,
        .tied_endpoint_records_emitted =
            stats.tied_endpoint_records_emitted,
        .stream_boundary_records_buffered =
            stats.stream_boundary_records_buffered,
        .stream_boundary_records_committed =
            stats.stream_boundary_records_committed,
        .stream_boundary_duplicates_suppressed =
            stats.stream_boundary_duplicates_suppressed,
        .scalar_collector_appends = stats.scalar_collector_appends,
        .partial_chunk_callback_batches =
            stats.partial_chunk_callback_batches,
        .masked_tail_lane_visits = stats.masked_tail_lane_visits,
        .fail_closed_rejections = stats.fail_closed_rejections,
        .scalar_hash_symbols = stats.scalar_hash_symbols,
        .scalar_window_transitions = stats.scalar_window_transitions,
        .scalar_minimum_decisions = stats.scalar_minimum_decisions,
        .fallback_calls = stats.fallback_calls,
        .positions_are_segment_local = stats.positions_are_segment_local,
        .aggregation_mismatch = stats.aggregation_mismatch,
        .saturated = stats.saturated,
    };
}

/** Pass packed 64-bit minimizers to the sketch collector and report acceptance. */
[[nodiscard]] bool append_bounded_raw(
        void* opaque,
        const std::uint64_t* raw,
        std::size_t count) {
    return static_cast<bioseq::minimizers::sketch_collector*>(opaque)
        ->append_raw_storage(raw, count);
}

/** Pass typed minimizer records to the sketch collector and report acceptance. */
[[nodiscard]] bool append_bounded_records(
        void* opaque,
        const bioseq::minimizers::minimap_sketch_data* records,
        std::size_t count) {
    return static_cast<bioseq::minimizers::sketch_collector*>(opaque)
        ->append_records(std::span<const bioseq::minimizers::minimap_sketch_data>{
            records, count});
}

/**
 * Convert the bounded kernel result to an index status. Collector  rejection
 * returns invalid_state before the kernel's own status is considered.
 */
[[nodiscard]] constexpr packed_bounded_nthash_index_status bounded_status_from_kernel(
        const bioseq::minimizers::minimizer_kernel_result& result,
        bool sink_accepted) noexcept {
    using kernel_status = bioseq::minimizers::minimizer_kernel_status;
    if (!sink_accepted || result.status == kernel_status::sink_rejected) {
        return packed_bounded_nthash_index_status::invalid_state;
    }
    if (result.family_status
        != static_cast<std::uint32_t>(packed_bounded_status::ok)) {
        return bridge_bounded_status(
            static_cast<packed_bounded_status>(result.family_status));
    }
    switch (result.status) {
        case kernel_status::ok:
            return packed_bounded_nthash_index_status::ok;
        case kernel_status::invalid_sequence_view:
            return packed_bounded_nthash_index_status::invalid_range;
        case kernel_status::invalid_configuration:
            return packed_bounded_nthash_index_status::invalid_configuration;
        case kernel_status::invalid_continuation:
        case kernel_status::sink_rejected:
            return packed_bounded_nthash_index_status::invalid_state;
    }
    return packed_bounded_nthash_index_status::invalid_state;
}

}  // namespace

exact_simd_index_bridge_result collect_exact_simd_index_erased(
        const exact_simd_index_bridge_request& request) {
    // Report unsupported alphabets and storage widths before missing callbacks
    // so input-format errors take priority.
    if (request.alphabet_code > 1u) {
        return rejected_bridge_result(
            exact_simd_index_status::unsupported_alphabet);
    }
    if (request.storage_unit_bytes != 1u
        && request.storage_unit_bytes != 2u
        && request.storage_unit_bytes != 4u
        && request.storage_unit_bytes != 8u) {
        return rejected_bridge_result(
            exact_simd_index_status::unsupported_storage);
    }
    if (request.kernels == nullptr
        || request.kernels->two_stack_nthash == nullptr
        || request.read_fragment == nullptr
        || request.release_fragment == nullptr
        || request.append_records == nullptr
        || request.append_raw == nullptr
        || request.sequence_context == nullptr
        || request.collector_context == nullptr) {
        return rejected_bridge_result(exact_simd_index_status::invalid_state);
    }
    const std::size_t storage_bits = request.storage_unit_bytes * 8u;
    const std::uint8_t expected_bits_per_symbol =
        request.alphabet_code == 0u ? 2u : 3u;
    if (request.bits_per_symbol != expected_bits_per_symbol) {
        return rejected_bridge_result(
            exact_simd_index_status::unsupported_alphabet);
    }
    const std::size_t letters_per_unit =
        storage_bits / request.bits_per_symbol;
    if (letters_per_unit == 0u
        || request.letters_per_unit != letters_per_unit) {
        return rejected_bridge_result(
            exact_simd_index_status::unsupported_storage);
    }
    if (request.fragment_units == 0u
        || request.lo_unit > request.hi_unit
        || request.hi_unit > request.total_units
        || request.lo_unit
            > (std::numeric_limits<std::size_t>::max)()
                / letters_per_unit
        || request.hi_unit
            > (std::numeric_limits<std::size_t>::max)()
                / letters_per_unit
        || request.total_units
            > (std::numeric_limits<std::size_t>::max)()
                / letters_per_unit
        || request.total_bp > request.total_units * letters_per_unit
        || request.fragment_units
            > (std::numeric_limits<std::size_t>::max)()
                / letters_per_unit) {
        return rejected_bridge_result(exact_simd_index_status::invalid_range);
    }

    const std::size_t first_fragment =
        request.lo_unit / request.fragment_units;
    const std::size_t fragment_end = request.hi_unit == 0u
        ? 0u
        : 1u + (request.hi_unit - 1u) / request.fragment_units;
    const std::size_t fragment_bp_capacity =
        request.fragment_units * letters_per_unit;
    if (fragment_end > request.fragment_count) {
        return rejected_bridge_result(exact_simd_index_status::invalid_range);
    }
    if (fragment_end != 0u
        && fragment_end - 1u
            > (std::numeric_limits<std::size_t>::max)()
                / fragment_bp_capacity) {
        return rejected_bridge_result(
            exact_simd_index_status::arithmetic_overflow);
    }

    exact_fragment_source_adapter fragments{
        .request = &request,
        .letters_per_unit = letters_per_unit,
        .fragment_bp_capacity = fragment_bp_capacity,
        .consume_begin_bp = request.lo_unit * letters_per_unit,
        .consume_end_bp = std::min(
            request.hi_unit * letters_per_unit, request.total_bp),
    };
    kernel_sink_adapter sink{
        request.collector_context,
        request.append_records,
        request.append_raw};
    bioseq::minimizers::minimizer_kernel_invocation invocation{
        .sequence = {
            .storage = nullptr,
            .storage_units = request.total_units,
            .begin_symbol = 0u,
            .end_symbol = request.total_bp,
            .alphabet_code = request.alphabet_code,
            .bits_per_symbol = request.bits_per_symbol,
            .storage_unit_bytes = static_cast<std::uint8_t>(
                request.storage_unit_bytes),
            .final_segment = true,
        },
        .fragments = {
            .state = &fragments,
            .first_fragment = first_fragment,
            .fragment_end = fragment_end,
            .read = exact_fragment_source_adapter::read,
            .release = exact_fragment_source_adapter::release,
        },
        .window = request.window_size,
        .kmer = request.kmer_size,
        .emitted_position_base = request.emitted_position_base,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<
                bioseq::minimizers::minimap_sketch_data>,
        .output_hash_mask =
            bioseq::minimizers::minimap_sketch_data::k_hash_field_mask,
        .output_position_mask =
            bioseq::minimizers::minimap_sketch_data::k_position_field_mask,
        .two_stack_route = bioseq::minimizers::two_stack_route::
            chronological_continuation,
        .sink = sink.sink(),
    };
    const bioseq::minimizers::minimizer_kernel_result kernel_result =
        request.kernels->two_stack_nthash(invocation);
    sink.validate(kernel_result);
    if (kernel_result.stats.algorithm
        != bioseq::minimizers::minimizer_kernel_algorithm::two_stack_nthash) {
        return rejected_bridge_result(exact_simd_index_status::invalid_state);
    }
    return {
        .status = exact_status_from_kernel(kernel_result, sink.accepted),
        .stats = bridge_exact_stats(kernel_result.stats.two_stack),
    };
}

packed_bounded_nthash_index_bridge_result
collect_packed_bounded_nthash_index_erased(
        const packed_bounded_nthash_index_bridge_request& request) {
    if (request.kmer_size == 0u ||
        request.kmer_size > bioseq::minimizers::k_max_exact_two_stack_kmer ||
        request.window_size == 0u ||
        request.window_size > bioseq::minimizers::k_bounded_nthash_max_window) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::invalid_configuration);
    }
    if (request.sequence.alphabet_code ==
        bioseq::minimizers::k_unbound_packed_alphabet_code) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::unsupported_alphabet);
    }
    if (request.storage_unit_bytes != sizeof(std::uint32_t)
        && request.storage_unit_bytes != sizeof(std::uint64_t)) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::unsupported_storage);
    }
    if (request.sequence.storage_unit_bytes != request.storage_unit_bytes) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::unsupported_storage);
    }
    // The generated kernel checks which alphabet and symbol width it supports.
    // This adapter checks that the packed storage can hold that symbol width.
    const std::uint8_t bits_per_symbol = request.sequence.bits_per_symbol;
    if (bits_per_symbol == 0u ||
        bits_per_symbol > request.storage_unit_bytes * 8u) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::unsupported_alphabet);
    }
    if (request.letters_per_unit !=
        request.storage_unit_bytes * 8u / bits_per_symbol) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::unsupported_storage);
    }
    const std::size_t letters_per_unit = request.letters_per_unit;
    if (request.storage_units
            > (std::numeric_limits<std::size_t>::max)()
                / letters_per_unit
        || request.local_bp_size
            > request.storage_units * letters_per_unit
        || request.sequence.storage_units != request.storage_units
        || request.sequence.end_symbol != request.local_bp_size
        || request.sequence.begin_symbol != 0u
        || !request.sequence.final_segment) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::invalid_range);
    }
    if (request.local_bp_size
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::input_too_large);
    }
    if (request.local_bp_size == 0u
        || request.local_bp_size - 1u
            > static_cast<std::size_t>(
                bioseq::minimizers::minimap_sketch_data::k_position_field_mask)) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::unsupported_representation);
    }
    if (request.kernels == nullptr
        || request.kernels->bounded_nthash == nullptr
        || request.sequence_context == nullptr
        || request.collector_context == nullptr) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::invalid_state);
    }

    kernel_sink_adapter sink{
        request.collector_context,
        append_bounded_records,
        append_bounded_raw};
    bioseq::minimizers::minimizer_kernel_invocation invocation{
        .sequence = request.sequence,
        .window = request.window_size,
        .kmer = request.kmer_size,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<
                bioseq::minimizers::minimap_sketch_data>,
        .output_hash_mask =
            bioseq::minimizers::minimap_sketch_data::k_hash_field_mask,
        .output_position_mask =
            bioseq::minimizers::minimap_sketch_data::k_position_field_mask,
        .sink = sink.sink(),
    };
    const bioseq::minimizers::minimizer_kernel_result kernel_result =
        request.kernels->bounded_nthash(invocation);
    sink.validate(kernel_result);
    if (kernel_result.stats.algorithm
        != bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash) {
        return rejected_packed_bounded_nthash_index_result(
            packed_bounded_nthash_index_status::invalid_state);
    }
    return {
        .status = bounded_status_from_kernel(kernel_result, sink.accepted),
        .stats = bridge_bounded_stats(kernel_result.stats.bounded_nthash),
    };
}

minimizer_hashmap_fragment_index::minimizer_hashmap_fragment_index()
    : table_(std::make_unique<detail::minimizer_hashmap_fragment_index_table>())
{}

minimizer_hashmap_fragment_index::minimizer_hashmap_fragment_index(
        defer_table_allocation_t) noexcept {}

minimizer_hashmap_fragment_index::~minimizer_hashmap_fragment_index() = default;

minimizer_hashmap_fragment_index::minimizer_hashmap_fragment_index(
        const minimizer_hashmap_fragment_index& other)
    : table_(std::make_unique<detail::minimizer_hashmap_fragment_index_table>()),
      positions(other.positions),
      target_len(other.target_len)
{
    if (other.table_ != nullptr) {
        *table_ = *other.table_;
    }
}

minimizer_hashmap_fragment_index& minimizer_hashmap_fragment_index::operator=(
        const minimizer_hashmap_fragment_index& other) {
    if (this == &other) return *this;
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::minimizer_hashmap_fragment_index_table>();
    }
    if (other.table_ != nullptr) {
        *table_ = *other.table_;
    } else {
        table_->table.clear();
    }
    positions = other.positions;
    target_len = other.target_len;
    return *this;
}

minimizer_hashmap_fragment_index::minimizer_hashmap_fragment_index(
        minimizer_hashmap_fragment_index&& other) noexcept = default;

minimizer_hashmap_fragment_index& minimizer_hashmap_fragment_index::operator=(
        minimizer_hashmap_fragment_index&& other) noexcept = default;

void minimizer_hashmap_fragment_index::clear_table() {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::minimizer_hashmap_fragment_index_table>();
    }
    table_->table.clear();
}

void minimizer_hashmap_fragment_index::reserve_table(std::size_t bucket_count) {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::minimizer_hashmap_fragment_index_table>();
    }
    table_->table.reserve(bucket_count);
}

void minimizer_hashmap_fragment_index::reserve_table_for_distinct_upper_bound(
        std::size_t distinct_count) {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::minimizer_hashmap_fragment_index_table>();
    }
    const minimizer_hashmap_table_allocation_plan plan =
        plan_minimizer_hashmap_table_allocation(
            static_cast<std::uint64_t>(distinct_count));
    if (!plan.valid
        || plan.requested_slots
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        throw std::length_error{
            "minimizer hashmap table allocation plan overflow"};
    }
    if (table_->allocation.live_bytes != 0u) {
        throw std::runtime_error{
            "one-shot minimizer hashmap table was not allocation-empty"};
    }
    table_->table.rehash(
        static_cast<std::size_t>(plan.requested_slots));
    if (table_->table.bucket_count() != plan.capacity
        || table_->table.max_load() < plan.distinct_upper_bound
        || table_->allocation.live_bytes != plan.allocation_bytes
        || table_->allocation.peak_bytes != plan.allocation_bytes) {
        throw std::runtime_error{
            "minimizer hashmap table allocation exceeded planned geometry"};
    }
}

void minimizer_hashmap_fragment_index::emplace_table(
        std::uint64_t hash, std::uint64_t packed) {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::minimizer_hashmap_fragment_index_table>();
    }
    table_->table.emplace(hash, packed);
}

std::size_t minimizer_hashmap_fragment_index::table_size() const noexcept {
    return table_ == nullptr ? 0u : table_->table.size();
}

bool minimizer_hashmap_fragment_index::table_empty() const noexcept {
    return table_size() == 0u;
}

std::uint64_t minimizer_hashmap_fragment_index::table_allocation_bytes() const noexcept {
    return table_ == nullptr ? 0u : table_->allocation.live_bytes;
}

std::uint64_t
minimizer_hashmap_fragment_index::table_owner_allocation_requirement_bytes() {
    return bioseq::common::heap_allocation_detail::plan_heap_allocation(
               sizeof(detail::minimizer_hashmap_fragment_index_table))
        .allocation_requirement_bytes;
}

std::uint64_t
minimizer_hashmap_fragment_index::table_owner_allocation_bytes() const noexcept {
    return static_cast<std::uint64_t>(
        bioseq::common::heap_allocation_detail::allocator_reported_bytes(table_.get()));
}

std::uint64_t minimizer_hashmap_fragment_index::table_peak_allocation_bytes() const noexcept {
    return table_ == nullptr ? 0u : table_->allocation.peak_bytes;
}

void minimizer_hashmap_fragment_index::release_storage() noexcept {
    table_.reset();
    std::vector<std::uint64_t>{}.swap(positions);
}

void minimizer_hashmap_fragment_index::for_each_table_entry_erased(
        void* ctx,
        void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const {
    if (table_ == nullptr) return;
    for (const auto& [hash, packed] : table_->table) {
        emit(ctx, hash, packed);
    }
}

void minimizer_hashmap_fragment_index::build_from_pairs(
        std::span<std::pair<std::uint64_t, std::uint64_t>> pairs,
        sequence_size_t target_length) {
    target_len = target_length;
    clear_table();
    positions.clear();
    const std::size_t n = pairs.size();
    if (n == 0) return;
    if (n > std::numeric_limits<std::uint32_t>::max()) {
        index_corruption_panic(
            "build_from_pairs: >UINT32_MAX positions (pos_offset is u32)");
    }
    bioseq::sorting::radix_sort(
        pairs.data(), pairs.data() + n,
        [](const auto& p) noexcept { return p.first; });
    positions.resize(n);
    reserve_table(n / 2 + 16);
    std::size_t i = 0;
    while (i < n) {
        const std::uint64_t kmer = pairs[i].first;
        std::size_t j = i;
        while (j < n && pairs[j].first == kmer) {
            positions[j] = pairs[j].second;
            ++j;
        }
        if (j - i > std::numeric_limits<std::uint32_t>::max()) {
            index_corruption_panic(
                "build_from_pairs: kmer count > u32 (packed count field)");
        }
        emplace_table(
            kmer,
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32)
                | static_cast<std::uint32_t>(j - i));
        i = j;
    }
}

std::span<const std::uint64_t> minimizer_hashmap_fragment_index::lookup(
        std::uint64_t kmer_hash) const {
    if (table_ == nullptr) return {};
    const auto iter = table_->table.find(kmer_hash);
    if (iter == table_->table.end()) {
        return {};
    }
    const std::uint64_t packed = iter->second;
    const std::uint32_t off = static_cast<std::uint32_t>(packed >> 32);
    const std::uint32_t count = static_cast<std::uint32_t>(packed);
    return std::span<const std::uint64_t>(positions.data() + off, count);
}

void minimizer_hashmap_fragment_index::add_position_offset(sequence_size_t offset) {
    if (offset == 0) return;
    for (std::uint64_t& encoded : positions) {
        const std::uint64_t pos =
            (encoded >> 1) + static_cast<std::uint64_t>(offset);
        const std::uint64_t strand = encoded & 1ULL;
        encoded = (pos << 1) | strand;
    }
}

std::size_t minimizer_hashmap_fragment_index::serialize_binary_size() const {
    return k_encoded_header_bytes
         + k_encoded_bucket_bytes * table_size()
         + 8 * positions.size();
}

void minimizer_hashmap_fragment_index::serialize_to_binary(char* output) const {
    const std::uint32_t bucket_count =
        static_cast<std::uint32_t>(table_size());
    const std::uint64_t total_positions =
        static_cast<std::uint64_t>(positions.size());
    const encoded_header header{
        k_encoded_magic, k_encoded_layout_revision, target_len, bucket_count, 0u,
        total_positions,
    };
    std::memcpy(output, &header, sizeof(header));
    char* bucket_cursor = output + k_encoded_header_bytes;
    char* position_cursor = bucket_cursor + k_encoded_bucket_bytes * bucket_count;
    for_each_table_entry(
        [&bucket_cursor](std::uint64_t hash, std::uint64_t packed) {
            const std::uint32_t pos_off =
                static_cast<std::uint32_t>(packed >> 32);
            const std::uint32_t pos_cnt =
                static_cast<std::uint32_t>(packed);
            const encoded_bucket bucket{hash, pos_off, pos_cnt};
            std::memcpy(bucket_cursor, &bucket, sizeof(bucket));
            bucket_cursor += k_encoded_bucket_bytes;
        });
    if (total_positions > 0) {
        std::memcpy(position_cursor, positions.data(), total_positions * 8);
    }
}

minimizer_hashmap_fragment_index
minimizer_hashmap_fragment_index::deserialize_from_binary(
        const char* input, std::size_t input_size) {
    if (input_size < k_encoded_header_bytes) {
        index_corruption_panic(
            "minimizer_hashmap_fragment_index deserialize: truncated blob");
    }
    encoded_header header{};
    std::memcpy(&header, input, sizeof(header));
    if (header.magic != k_encoded_magic) {
        index_corruption_panic(
            "minimizer_hashmap_fragment_index deserialize: magic mismatch");
    }
    if (header.layout_revision != k_encoded_layout_revision) {
        index_corruption_panic(
            "minimizer_hashmap_fragment_index deserialize: layout revision mismatch");
    }
    const std::size_t expected_size =
        k_encoded_header_bytes
        + k_encoded_bucket_bytes * header.bucket_count
        + 8 * header.total_positions;
    if (input_size != expected_size) {
        index_corruption_panic(
            "minimizer_hashmap_fragment_index deserialize: size mismatch");
    }
    minimizer_hashmap_fragment_index idx;
    idx.target_len = header.target_len;
    idx.reserve_table(header.bucket_count);
    const char* bucket_cursor = input + k_encoded_header_bytes;
    const char* position_cursor =
        bucket_cursor + k_encoded_bucket_bytes * header.bucket_count;
    idx.positions.resize(header.total_positions);
    if (header.total_positions > 0) {
        std::memcpy(
            idx.positions.data(), position_cursor, header.total_positions * 8);
    }
    for (std::uint32_t b = 0; b < header.bucket_count; ++b) {
        encoded_bucket bucket{};
        std::memcpy(&bucket, bucket_cursor, sizeof(bucket));
        bucket_cursor += k_encoded_bucket_bytes;
        if (static_cast<std::uint64_t>(bucket.pos_offset) + bucket.pos_count
            > header.total_positions) {
            index_corruption_panic(
                "minimizer_hashmap_fragment_index deserialize: bucket range exceeds total_positions");
        }
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(bucket.pos_offset) << 32)
            | static_cast<std::uint64_t>(bucket.pos_count);
        idx.emplace_table(bucket.hash, packed);
    }
    return idx;
}

}  // namespace bioseq::indexing
