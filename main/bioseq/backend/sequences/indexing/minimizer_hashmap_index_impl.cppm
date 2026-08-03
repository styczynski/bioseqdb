/**
 * @file minimizer_hashmap_index_impl.cppm
 * @brief Boost-backed implementation for MinimizerHashmapFragmentIndex.
 */
module;

#include "common.h"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.indexing.minimizer_hashmap;

import bioseq.common.radix_sort;
import bioseq.indexing.type;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_contract;

namespace bioseq::indexing::detail {

struct MinimizerHashmapTableAllocationState {
    std::uint64_t live_bytes = 0u;
    std::uint64_t peak_bytes = 0u;
};

template <class T>
struct MinimizerHashmapTableAllocator {
    using value_type = T;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::false_type;

    MinimizerHashmapTableAllocationState* state = nullptr;

    MinimizerHashmapTableAllocator() noexcept = default;
    explicit MinimizerHashmapTableAllocator(
            MinimizerHashmapTableAllocationState* state_) noexcept
        : state(state_) {}

    template <class U>
    MinimizerHashmapTableAllocator(
            const MinimizerHashmapTableAllocator<U>& other) noexcept
        : state(other.state) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (state == nullptr
            || count > (std::numeric_limits<std::uint64_t>::max)()
                    / sizeof(T)) {
            throw std::bad_alloc{};
        }
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(count) * sizeof(T);
        if (state->live_bytes
            > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
            throw std::bad_alloc{};
        }
        state->live_bytes += bytes;
        state->peak_bytes = std::max(
            state->peak_bytes, state->live_bytes);
        try {
            return std::allocator<T>{}.allocate(count);
        } catch (...) {
            state->live_bytes -= bytes;
            throw;
        }
    }

    void deallocate(T* allocation, std::size_t count) noexcept {
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(count) * sizeof(T);
        DEBUG_ASSERT(state != nullptr && state->live_bytes >= bytes);
        std::allocator<T>{}.deallocate(allocation, count);
        if (state != nullptr && state->live_bytes >= bytes) {
            state->live_bytes -= bytes;
        }
    }

    template <class U>
    friend struct MinimizerHashmapTableAllocator;

    template <class U>
    [[nodiscard]] bool operator==(
            const MinimizerHashmapTableAllocator<U>& other) const noexcept {
        return state == other.state;
    }
};

struct MinimizerHashmapFragmentIndexTable {
    using allocator_type = MinimizerHashmapTableAllocator<
        std::pair<const std::uint64_t, std::uint64_t>>;
    using table_type = boost::unordered_flat_map<
        std::uint64_t,
        std::uint64_t,
        boost::hash<std::uint64_t>,
        std::equal_to<std::uint64_t>,
        allocator_type>;

    MinimizerHashmapTableAllocationState allocation;
    table_type table;

    MinimizerHashmapFragmentIndexTable()
        : allocation{},
          table(
              0u,
              boost::hash<std::uint64_t>{},
              std::equal_to<std::uint64_t>{},
              allocator_type{&allocation}) {}

    MinimizerHashmapFragmentIndexTable(
            const MinimizerHashmapFragmentIndexTable& other)
        : MinimizerHashmapFragmentIndexTable() {
        table = other.table;
    }

    MinimizerHashmapFragmentIndexTable& operator=(
            const MinimizerHashmapFragmentIndexTable& other) {
        if (this != &other) table = other.table;
        return *this;
    }
};

static_assert(BOOST_VERSION == 109000,
    "Update minimizer_hashmap_table_allocation_plan for a new Boost FOA layout");
static_assert(sizeof(MinimizerHashmapFragmentIndexTable::table_type::value_type)
              == 2u * sizeof(std::uint64_t));
using PinnedFoaGroup = boost::unordered::detail::foa::group15<
    boost::unordered::detail::foa::plain_integral>;
static_assert(PinnedFoaGroup::N == 15u);
static_assert(sizeof(PinnedFoaGroup) == 16u);

}  // namespace bioseq::indexing::detail

namespace bioseq::indexing {

namespace {

using ExactStatus = bioseq::minimizers::TwoStackContinuationStatus;
using ExactProof = bioseq::minimizers::TwoStackExecutionProof;

[[nodiscard]] constexpr ExactSimdIndexStatus bridge_status(
        ExactStatus status) noexcept {
    switch (status) {
        case ExactStatus::ok: return ExactSimdIndexStatus::ok;
        case ExactStatus::invalid_window_size:
            return ExactSimdIndexStatus::invalid_window_size;
        case ExactStatus::invalid_kmer_size:
            return ExactSimdIndexStatus::invalid_kmer_size;
        case ExactStatus::invalid_static_configuration:
            return ExactSimdIndexStatus::invalid_static_configuration;
        case ExactStatus::unsupported_alphabet:
            return ExactSimdIndexStatus::unsupported_alphabet;
        case ExactStatus::unsupported_storage:
            return ExactSimdIndexStatus::unsupported_storage;
        case ExactStatus::unsupported_representation:
            return ExactSimdIndexStatus::unsupported_representation;
        case ExactStatus::unsupported_vector_shape:
            return ExactSimdIndexStatus::unsupported_vector_shape;
        case ExactStatus::not_initialized:
            return ExactSimdIndexStatus::not_initialized;
        case ExactStatus::already_finalized:
            return ExactSimdIndexStatus::already_finalized;
        case ExactStatus::invalid_state:
            return ExactSimdIndexStatus::invalid_state;
        case ExactStatus::invalid_range:
            return ExactSimdIndexStatus::invalid_range;
        case ExactStatus::non_contiguous_range:
            return ExactSimdIndexStatus::non_contiguous_range;
        case ExactStatus::arithmetic_overflow:
            return ExactSimdIndexStatus::arithmetic_overflow;
        case ExactStatus::collector_overflow:
            return ExactSimdIndexStatus::collector_overflow;
    }
    return ExactSimdIndexStatus::invalid_state;
}

[[nodiscard]] constexpr ExactSimdIndexExecutionProof bridge_proof(
        const ExactProof& proof) noexcept {
    return ExactSimdIndexExecutionProof{
        .bp_consumed = proof.bp_consumed,
        .minimizers_emitted = proof.minimizers_emitted,
        .vector_batches = proof.vector_batches,
        .masked_tail_lanes = proof.masked_tail_lanes,
        .packed_decode_batches = proof.packed_decode_batches,
        .packed_decode_lanes = proof.packed_decode_lanes,
        .ambiguous_symbols = proof.ambiguous_symbols,
        .ambiguous_reset_batches = proof.ambiguous_reset_batches,
        .scalar_backend_branches = proof.scalar_backend_branches,
        .scalar_tail_symbols = proof.scalar_tail_symbols,
        .scalar_hash_symbols = proof.scalar_hash_symbols,
        .scalar_window_transitions = proof.scalar_window_transitions,
        .scalar_event_records = proof.scalar_event_records,
        .scalar_sort_records = proof.scalar_sort_records,
        .scalar_dedup_records = proof.scalar_dedup_records,
        .fail_closed_rejections = proof.fail_closed_rejections,
        .saturated = proof.saturated,
    };
}

[[nodiscard]] constexpr ExactSimdIndexBridgeResult rejected_bridge_result(
        ExactSimdIndexStatus status) noexcept {
    return {
        .status = status,
        .proof = ExactSimdIndexExecutionProof{.fail_closed_rejections = 1},
    };
}

struct KernelSinkAdapter {
    using sketch_type = bioseq::minimizers::MinimapSketchData;
    static_assert(sizeof(sketch_type) == sizeof(std::uint64_t));
    static_assert(std::is_trivially_copyable_v<sketch_type>);

    void* collector_context = nullptr;
    ExactSimdIndexAppendRawFn append_raw = nullptr;
    std::vector<sketch_type> sketches;
    std::vector<std::uint64_t> raw;
    bool accepted = true;

    [[nodiscard]] static std::span<sketch_type> acquire(
            void* opaque,
            std::size_t minimum) {
        auto& self = *static_cast<KernelSinkAdapter*>(opaque);
        if (!self.accepted) return {};
        try {
            self.sketches.resize(minimum);
            self.raw.resize(minimum);
        } catch (...) {
            self.accepted = false;
            return {};
        }
        return self.sketches;
    }

    static void commit(void* opaque, std::size_t initialized) noexcept {
        auto& self = *static_cast<KernelSinkAdapter*>(opaque);
        if (!self.accepted
            || self.append_raw == nullptr
            || initialized > self.sketches.size()
            || initialized > self.raw.size()) {
            self.accepted = false;
            return;
        }
        if (initialized != 0u) {
            std::memcpy(
                self.raw.data(),
                self.sketches.data(),
                initialized * sizeof(std::uint64_t));
        }
        try {
            self.accepted = self.append_raw(
                self.collector_context, self.raw.data(), initialized);
        } catch (...) {
            self.accepted = false;
        }
    }

    [[nodiscard]] bioseq::minimizers::MinimizerSink sink() noexcept {
        return {
            .state = this,
            .acquire = acquire,
            .commit = commit,
        };
    }
};

struct ExactFragmentSourceAdapter {
    const ExactSimdIndexBridgeRequest* request = nullptr;
    std::size_t letters_per_unit = 0u;
    std::size_t fragment_bp_capacity = 0u;
    std::size_t consume_begin_bp = 0u;
    std::size_t consume_end_bp = 0u;

    [[nodiscard]] static bioseq::minimizers::PackedSequenceKernelFragment read(
            void* opaque,
            std::size_t fragment_no) {
        auto& self = *static_cast<ExactFragmentSourceAdapter*>(opaque);
        const auto& request = *self.request;
        const ExactSimdIndexPackedFragment fragment =
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

    static void release(void* opaque, std::size_t fragment_no) noexcept {
        const auto& self = *static_cast<ExactFragmentSourceAdapter*>(opaque);
        self.request->release_fragment(
            self.request->sequence_context, fragment_no);
    }
};

[[nodiscard]] constexpr ExactSimdIndexStatus exact_status_from_kernel(
        const bioseq::minimizers::MinimizerKernelResult& result,
        bool sink_accepted) noexcept {
    using KernelStatus = bioseq::minimizers::MinimizerKernelStatus;
    if (!sink_accepted || result.status == KernelStatus::sink_rejected) {
        return ExactSimdIndexStatus::collector_overflow;
    }
    if (result.family_status != static_cast<std::uint32_t>(ExactStatus::ok)) {
        return bridge_status(static_cast<ExactStatus>(result.family_status));
    }
    switch (result.status) {
        case KernelStatus::ok:
            return ExactSimdIndexStatus::ok;
        case KernelStatus::invalid_sequence_view:
            return ExactSimdIndexStatus::invalid_range;
        case KernelStatus::invalid_configuration:
            return ExactSimdIndexStatus::invalid_static_configuration;
        case KernelStatus::invalid_continuation:
            return ExactSimdIndexStatus::invalid_state;
        case KernelStatus::sink_rejected:
            return ExactSimdIndexStatus::collector_overflow;
    }
    return ExactSimdIndexStatus::invalid_state;
}

using PackedPaperStatus = bioseq::minimizers::PackedPaperNthashStatus;
using PackedPaperProof = bioseq::minimizers::PackedPaperNthashExecutionProof;

static_assert(kPackedPaperNthashKernelSchemeId
    == bioseq::minimizers::packed_paper_nthash_scheme_id);
static_assert(kPackedPaperNthashMaximumWindow
    == bioseq::minimizers::packed_paper_nthash_max_window);

[[nodiscard]] constexpr PackedPaperNthashIndexStatus bridge_paper_status(
        PackedPaperStatus status) noexcept {
    switch (status) {
        case PackedPaperStatus::ok:
            return PackedPaperNthashIndexStatus::ok;
        case PackedPaperStatus::invalid_configuration:
            return PackedPaperNthashIndexStatus::invalid_configuration;
        case PackedPaperStatus::input_too_short:
            return PackedPaperNthashIndexStatus::input_too_short;
        case PackedPaperStatus::unsupported_alphabet:
            return PackedPaperNthashIndexStatus::unsupported_alphabet;
        case PackedPaperStatus::unsupported_storage:
            return PackedPaperNthashIndexStatus::unsupported_storage;
        case PackedPaperStatus::unsupported_representation:
            return PackedPaperNthashIndexStatus::unsupported_representation;
        case PackedPaperStatus::unsupported_vector_shape:
            return PackedPaperNthashIndexStatus::unsupported_vector_shape;
        case PackedPaperStatus::invalid_range:
            return PackedPaperNthashIndexStatus::invalid_range;
        case PackedPaperStatus::input_too_large:
            return PackedPaperNthashIndexStatus::input_too_large;
    }
    return PackedPaperNthashIndexStatus::invalid_state;
}

[[nodiscard]] constexpr PackedPaperNthashIndexExecutionProof
bridge_paper_proof(const PackedPaperProof& proof) noexcept {
    return {
        .scheme_id = proof.scheme_id,
        .kmer_size = proof.kmer_size,
        .window_size = proof.window_size,
        .nthash_rotation = proof.nthash_rotation,
        .source_storage_unit_bytes = proof.source_storage_unit_bytes,
        .input_symbols = proof.input_symbols,
        .input_storage_units = proof.input_storage_units,
        .segments_combined = proof.segments_combined,
        .logical_streams = proof.logical_streams,
        .physical_vector_lanes = proof.physical_vector_lanes,
        .vector_registers_per_step = proof.vector_registers_per_step,
        .packed_decode_batches = proof.packed_decode_batches,
        .packed_decode_lane_visits = proof.packed_decode_lane_visits,
        .hash_initialization_batches = proof.hash_initialization_batches,
        .rolling_hash_batches = proof.rolling_hash_batches,
        .hash_table_lookup_batches = proof.hash_table_lookup_batches,
        .canonical_sum_batches = proof.canonical_sum_batches,
        .window_update_batches = proof.window_update_batches,
        .suffix_rebuild_batches = proof.suffix_rebuild_batches,
        .emission_decision_batches = proof.emission_decision_batches,
        .emission_mask_store_batches = proof.emission_mask_store_batches,
        .minimizers_emitted = proof.minimizers_emitted,
        .tied_endpoint_records_emitted =
            proof.tied_endpoint_records_emitted,
        .stream_boundary_records_buffered =
            proof.stream_boundary_records_buffered,
        .stream_boundary_records_committed =
            proof.stream_boundary_records_committed,
        .stream_boundary_duplicates_suppressed =
            proof.stream_boundary_duplicates_suppressed,
        .scalar_collector_appends = proof.scalar_collector_appends,
        .partial_chunk_callback_batches =
            proof.partial_chunk_callback_batches,
        .masked_tail_lane_visits = proof.masked_tail_lane_visits,
        .fail_closed_rejections = proof.fail_closed_rejections,
        .scalar_hash_symbols = proof.scalar_hash_symbols,
        .scalar_window_transitions = proof.scalar_window_transitions,
        .scalar_minimum_decisions = proof.scalar_minimum_decisions,
        .fallback_calls = proof.fallback_calls,
        .positions_are_segment_local = proof.positions_are_segment_local,
        .aggregation_mismatch = proof.aggregation_mismatch,
        .saturated = proof.saturated,
    };
}

[[nodiscard]] bool append_paper_raw(
        void* opaque,
        const std::uint64_t* raw,
        std::size_t count) {
    return static_cast<bioseq::minimizers::sketch_collector*>(opaque)
        ->append_raw_storage(raw, count);
}

[[nodiscard]] constexpr PackedPaperNthashIndexStatus paper_status_from_kernel(
        const bioseq::minimizers::MinimizerKernelResult& result,
        bool sink_accepted) noexcept {
    using KernelStatus = bioseq::minimizers::MinimizerKernelStatus;
    if (!sink_accepted || result.status == KernelStatus::sink_rejected) {
        return PackedPaperNthashIndexStatus::invalid_state;
    }
    if (result.family_status
        != static_cast<std::uint32_t>(PackedPaperStatus::ok)) {
        return bridge_paper_status(
            static_cast<PackedPaperStatus>(result.family_status));
    }
    switch (result.status) {
        case KernelStatus::ok:
            return PackedPaperNthashIndexStatus::ok;
        case KernelStatus::invalid_sequence_view:
            return PackedPaperNthashIndexStatus::invalid_range;
        case KernelStatus::invalid_configuration:
            return PackedPaperNthashIndexStatus::invalid_configuration;
        case KernelStatus::invalid_continuation:
        case KernelStatus::sink_rejected:
            return PackedPaperNthashIndexStatus::invalid_state;
    }
    return PackedPaperNthashIndexStatus::invalid_state;
}

}  // namespace

ExactSimdIndexBridgeResult collect_exact_simd_index_erased(
        const ExactSimdIndexBridgeRequest& request) {
    if (request.kernels == nullptr
        || request.kernels->two_stack_nthash == nullptr
        || request.read_fragment == nullptr
        || request.release_fragment == nullptr
        || request.append_raw == nullptr
        || request.sequence_context == nullptr
        || request.collector_context == nullptr) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_state);
    }

    std::uint8_t alphabet_code = 0u;
    std::size_t bits_per_symbol = 0u;
    switch (request.alphabet) {
        case ExactSimdIndexAlphabet::dna4:
            alphabet_code = static_cast<std::uint8_t>(
                bioseq::minimizers::PackedSequenceAlphabet::dna4);
            bits_per_symbol = 2u;
            break;
        case ExactSimdIndexAlphabet::dna5:
            alphabet_code = static_cast<std::uint8_t>(
                bioseq::minimizers::PackedSequenceAlphabet::dna5);
            bits_per_symbol = 3u;
            break;
        default:
            return rejected_bridge_result(
                ExactSimdIndexStatus::unsupported_alphabet);
    }
    if (request.storage_unit_bytes != 1u
        && request.storage_unit_bytes != 2u
        && request.storage_unit_bytes != 4u
        && request.storage_unit_bytes != 8u) {
        return rejected_bridge_result(
            ExactSimdIndexStatus::unsupported_storage);
    }
    const std::size_t storage_bits = request.storage_unit_bytes * 8u;
    const std::size_t letters_per_unit = storage_bits / bits_per_symbol;
    if (letters_per_unit == 0u
        || request.letters_per_unit != letters_per_unit) {
        return rejected_bridge_result(
            ExactSimdIndexStatus::unsupported_storage);
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
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_range);
    }

    const std::size_t first_fragment =
        request.lo_unit / request.fragment_units;
    const std::size_t fragment_end = request.hi_unit == 0u
        ? 0u
        : 1u + (request.hi_unit - 1u) / request.fragment_units;
    const std::size_t fragment_bp_capacity =
        request.fragment_units * letters_per_unit;
    if (fragment_end > request.fragment_count) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_range);
    }
    if (fragment_end != 0u
        && fragment_end - 1u
            > (std::numeric_limits<std::size_t>::max)()
                / fragment_bp_capacity) {
        return rejected_bridge_result(
            ExactSimdIndexStatus::arithmetic_overflow);
    }

    ExactFragmentSourceAdapter fragments{
        .request = &request,
        .letters_per_unit = letters_per_unit,
        .fragment_bp_capacity = fragment_bp_capacity,
        .consume_begin_bp = request.lo_unit * letters_per_unit,
        .consume_end_bp = std::min(
            request.hi_unit * letters_per_unit, request.total_bp),
    };
    KernelSinkAdapter sink{
        .collector_context = request.collector_context,
        .append_raw = request.append_raw,
    };
    bioseq::minimizers::MinimizerKernelInvocation invocation{
        .sequence = {
            .storage = nullptr,
            .storage_units = request.total_units,
            .begin_symbol = 0u,
            .end_symbol = request.total_bp,
            .alphabet_code = alphabet_code,
            .bits_per_symbol = static_cast<std::uint8_t>(bits_per_symbol),
            .storage_unit_bytes = static_cast<std::uint8_t>(
                request.storage_unit_bytes),
            .final_segment = true,
        },
        .fragments = {
            .state = &fragments,
            .first_fragment = first_fragment,
            .fragment_end = fragment_end,
            .read = ExactFragmentSourceAdapter::read,
            .release = ExactFragmentSourceAdapter::release,
        },
        .window = request.window_size,
        .kmer = request.kmer_size,
        .emitted_position_base = request.emitted_position_base,
        .two_stack_route = bioseq::minimizers::TwoStackRoute::
            chronological_continuation,
        .sink = sink.sink(),
    };
    const bioseq::minimizers::MinimizerKernelResult kernel_result =
        request.kernels->two_stack_nthash(invocation);
    if (kernel_result.proof.algorithm
        != bioseq::minimizers::MinimizerKernelAlgorithm::two_stack_nthash) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_state);
    }
    return {
        .status = exact_status_from_kernel(kernel_result, sink.accepted),
        .proof = bridge_proof(kernel_result.proof.two_stack),
    };
}

PackedPaperNthashIndexBridgeResult
collect_packed_paper_nthash_index_erased(
        const PackedPaperNthashIndexBridgeRequest& request) {
    if (request.kernels == nullptr
        || request.kernels->paper_nthash == nullptr
        || request.collector_context == nullptr) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_state);
    }
    if (request.window_size != 10u || request.kmer_size != 15u) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_configuration);
    }
    if (request.alphabet != PackedPaperNthashIndexAlphabet::dna4
        && request.alphabet != PackedPaperNthashIndexAlphabet::dna5) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::unsupported_alphabet);
    }
    if (request.storage_unit_bytes != sizeof(std::uint8_t)
        && request.storage_unit_bytes != sizeof(std::uint16_t)
        && request.storage_unit_bytes != sizeof(std::uint32_t)
        && request.storage_unit_bytes != sizeof(std::uint64_t)) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::unsupported_storage);
    }
    if (request.local_bp_size
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::input_too_large);
    }
    if (request.local_bp_size == 0u
        || request.local_bp_size - 1u
            > static_cast<std::size_t>(
                bioseq::minimizers::MinimapSketchData::pos_field_mask)) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::unsupported_representation);
    }
    const bool dna4 = request.alphabet
        == PackedPaperNthashIndexAlphabet::dna4;
    const std::uint8_t bits_per_symbol = dna4 ? 2u : 3u;
    const std::uint8_t alphabet_code = static_cast<std::uint8_t>(
        dna4
            ? bioseq::minimizers::PackedSequenceAlphabet::dna4
            : bioseq::minimizers::PackedSequenceAlphabet::dna5);
    const std::size_t letters_per_unit =
        request.storage_unit_bytes * 8u / bits_per_symbol;
    if (request.storage_units
            > (std::numeric_limits<std::size_t>::max)()
                / letters_per_unit
        || request.local_bp_size
            > request.storage_units * letters_per_unit) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_range);
    }
    if (request.sequence.storage_units != request.storage_units
        || request.sequence.end_symbol != request.local_bp_size
        || request.sequence.begin_symbol != 0u
        || request.sequence.alphabet_code != alphabet_code
        || request.sequence.bits_per_symbol != bits_per_symbol
        || request.sequence.storage_unit_bytes != request.storage_unit_bytes
        || request.letters_per_unit != letters_per_unit
        || !request.sequence.final_segment) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_range);
    }

    KernelSinkAdapter sink{
        .collector_context = request.collector_context,
        .append_raw = append_paper_raw,
    };
    bioseq::minimizers::MinimizerKernelInvocation invocation{
        .sequence = request.sequence,
        .window = request.window_size,
        .kmer = request.kmer_size,
        .sink = sink.sink(),
    };
    const bioseq::minimizers::MinimizerKernelResult kernel_result =
        request.kernels->paper_nthash(invocation);
    if (kernel_result.proof.algorithm
        != bioseq::minimizers::MinimizerKernelAlgorithm::paper_nthash) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_state);
    }
    return {
        .status = paper_status_from_kernel(kernel_result, sink.accepted),
        .proof = bridge_paper_proof(kernel_result.proof.paper),
    };
}

MinimizerHashmapFragmentIndex::MinimizerHashmapFragmentIndex()
    : table_(std::make_unique<detail::MinimizerHashmapFragmentIndexTable>())
{}

MinimizerHashmapFragmentIndex::~MinimizerHashmapFragmentIndex() = default;

MinimizerHashmapFragmentIndex::MinimizerHashmapFragmentIndex(
        const MinimizerHashmapFragmentIndex& other)
    : table_(std::make_unique<detail::MinimizerHashmapFragmentIndexTable>()),
      positions(other.positions),
      target_len(other.target_len)
{
    if (other.table_ != nullptr) {
        *table_ = *other.table_;
    }
}

MinimizerHashmapFragmentIndex& MinimizerHashmapFragmentIndex::operator=(
        const MinimizerHashmapFragmentIndex& other) {
    if (this == &other) return *this;
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
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

MinimizerHashmapFragmentIndex::MinimizerHashmapFragmentIndex(
        MinimizerHashmapFragmentIndex&& other) noexcept = default;

MinimizerHashmapFragmentIndex& MinimizerHashmapFragmentIndex::operator=(
        MinimizerHashmapFragmentIndex&& other) noexcept = default;

void MinimizerHashmapFragmentIndex::clear_table() {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
    }
    table_->table.clear();
}

void MinimizerHashmapFragmentIndex::reserve_table(std::size_t bucket_count) {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
    }
    table_->table.reserve(bucket_count);
}

void MinimizerHashmapFragmentIndex::reserve_table_for_distinct_upper_bound(
        std::size_t distinct_count) {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
    }
    const MinimizerHashmapTableAllocationPlan plan =
        minimizer_hashmap_table_allocation_plan(
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

void MinimizerHashmapFragmentIndex::emplace_table(
        std::uint64_t hash, std::uint64_t packed) {
    if (table_ == nullptr) {
        table_ = std::make_unique<detail::MinimizerHashmapFragmentIndexTable>();
    }
    table_->table.emplace(hash, packed);
}

std::size_t MinimizerHashmapFragmentIndex::table_size() const noexcept {
    return table_ == nullptr ? 0u : table_->table.size();
}

bool MinimizerHashmapFragmentIndex::table_empty() const noexcept {
    return table_size() == 0u;
}

std::uint64_t MinimizerHashmapFragmentIndex::table_allocation_bytes() const noexcept {
    return table_ == nullptr ? 0u : table_->allocation.live_bytes;
}

std::uint64_t MinimizerHashmapFragmentIndex::table_peak_allocation_bytes() const noexcept {
    return table_ == nullptr ? 0u : table_->allocation.peak_bytes;
}

void MinimizerHashmapFragmentIndex::release_storage() noexcept {
    table_.reset();
    std::vector<std::uint64_t>{}.swap(positions);
}

void MinimizerHashmapFragmentIndex::for_each_table_entry_erased(
        void* ctx,
        void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const {
    if (table_ == nullptr) return;
    for (const auto& [hash, packed] : table_->table) {
        emit(ctx, hash, packed);
    }
}

void MinimizerHashmapFragmentIndex::build_from_pairs(
        std::span<std::pair<std::uint64_t, std::uint64_t>> pairs,
        sequence_size_t target_length) {
    target_len = target_length;
    clear_table();
    positions.clear();
    const std::size_t n = pairs.size();
    if (n == 0) return;
    if (n > UINT32_MAX) {
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
        if (j - i > UINT32_MAX) {
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

std::span<const std::uint64_t> MinimizerHashmapFragmentIndex::lookup(
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

void MinimizerHashmapFragmentIndex::add_position_offset(sequence_size_t offset) {
    if (offset == 0) return;
    for (std::uint64_t& encoded : positions) {
        const std::uint64_t pos =
            (encoded >> 1) + static_cast<std::uint64_t>(offset);
        const std::uint64_t strand = encoded & 1ULL;
        encoded = (pos << 1) | strand;
    }
}

std::size_t MinimizerHashmapFragmentIndex::serialize_binary_size() const {
    return kSerializeHeaderBytes
         + kSerializeBucketBytes * table_size()
         + 8 * positions.size();
}

void MinimizerHashmapFragmentIndex::serialize_to_binary(char* output) const {
    const std::uint32_t bucket_count =
        static_cast<std::uint32_t>(table_size());
    const std::uint64_t total_positions =
        static_cast<std::uint64_t>(positions.size());
    const SerializedHeader header{
        kSerializeMagic, kSerializeVersion, target_len, bucket_count, 0u,
        total_positions,
    };
    std::memcpy(output, &header, sizeof(header));
    char* bucket_cursor = output + kSerializeHeaderBytes;
    char* position_cursor = bucket_cursor + kSerializeBucketBytes * bucket_count;
    for_each_table_entry(
        [&bucket_cursor](std::uint64_t hash, std::uint64_t packed) {
            const std::uint32_t pos_off =
                static_cast<std::uint32_t>(packed >> 32);
            const std::uint32_t pos_cnt =
                static_cast<std::uint32_t>(packed);
            const SerializedBucket bucket{hash, pos_off, pos_cnt};
            std::memcpy(bucket_cursor, &bucket, sizeof(bucket));
            bucket_cursor += kSerializeBucketBytes;
        });
    if (total_positions > 0) {
        std::memcpy(position_cursor, positions.data(), total_positions * 8);
    }
}

MinimizerHashmapFragmentIndex
MinimizerHashmapFragmentIndex::deserialize_from_binary(
        const char* input, std::size_t input_size) {
    if (input_size < kSerializeHeaderBytes) {
        index_corruption_panic(
            "MinimizerHashmapFragmentIndex deserialize: truncated blob");
    }
    SerializedHeader header{};
    std::memcpy(&header, input, sizeof(header));
    if (header.magic != kSerializeMagic) {
        index_corruption_panic(
            "MinimizerHashmapFragmentIndex deserialize: magic mismatch");
    }
    if (header.version != kSerializeVersion) {
        index_corruption_panic(
            "MinimizerHashmapFragmentIndex deserialize: version mismatch");
    }
    const std::size_t expected_size =
        kSerializeHeaderBytes
        + kSerializeBucketBytes * header.bucket_count
        + 8 * header.total_positions;
    if (input_size != expected_size) {
        index_corruption_panic(
            "MinimizerHashmapFragmentIndex deserialize: size mismatch");
    }
    MinimizerHashmapFragmentIndex idx;
    idx.target_len = header.target_len;
    idx.reserve_table(header.bucket_count);
    const char* bucket_cursor = input + kSerializeHeaderBytes;
    const char* position_cursor =
        bucket_cursor + kSerializeBucketBytes * header.bucket_count;
    idx.positions.resize(header.total_positions);
    if (header.total_positions > 0) {
        std::memcpy(
            idx.positions.data(), position_cursor, header.total_positions * 8);
    }
    for (std::uint32_t b = 0; b < header.bucket_count; ++b) {
        SerializedBucket bucket{};
        std::memcpy(&bucket, bucket_cursor, sizeof(bucket));
        bucket_cursor += kSerializeBucketBytes;
        if (static_cast<std::uint64_t>(bucket.pos_offset) + bucket.pos_count
            > header.total_positions) {
            index_corruption_panic(
                "MinimizerHashmapFragmentIndex deserialize: bucket range exceeds total_positions");
        }
        const std::uint64_t packed =
            (static_cast<std::uint64_t>(bucket.pos_offset) << 32)
            | static_cast<std::uint64_t>(bucket.pos_count);
        idx.emplace_table(bucket.hash, packed);
    }
    return idx;
}

}  // namespace bioseq::indexing
