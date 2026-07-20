/**
 * @file minimizer_hashmap_index.cpp
 * @brief Boost-backed implementation for MinimizerHashmapFragmentIndex.
 */
module;

#include "common.h"

#include <hwy/highway.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/version.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

module bioseq.indexing.minimizer_hashmap;

import bioseq.common.radix_sort;
import bioseq.indexing.type;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.simd;
import bioseq.simd.utils;
import bioseq.storage.lazy;

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

namespace grouped32_hn = hwy::HWY_NAMESPACE;
namespace grouped32_vs = bioseq::simd::HWY_NAMESPACE;

auto PreparedGrouped32QueryReader::validate_non_scatter_local32_w2(
        std::span<const std::byte> encoded_bytes,
        const MinimizerGrouped32Control& control,
        const MinimizerPositionSegmentCore& core) noexcept
    -> NonScatterSimdValidationResult {
    NonScatterSimdValidationResult result{
        .selected_hwy_target = static_cast<std::uint64_t>(HWY_TARGET),
    };
#if HWY_TARGET == HWY_SCALAR
    (void)encoded_bytes;
    (void)control;
    (void)core;
    return result;
#else
    if constexpr (std::endian::native != std::endian::little) {
        return result;
    } else {
        if (static_cast<std::uint64_t>(control.position_offset)
                > (std::numeric_limits<std::size_t>::max)()
            || static_cast<std::uint64_t>(control.position_count)
                > (std::numeric_limits<std::size_t>::max)()) {
            return result;
        }

        using D8 = grouped32_hn::ScalableTag<std::uint8_t>;
        using D32 = grouped32_hn::Repartition<std::uint32_t, D8>;
        constexpr std::size_t stream_width = 2u;
        D8 d8;
        D32 d32;
        const std::size_t lanes = grouped32_hn::Lanes(d32);
        const std::size_t count = static_cast<std::size_t>(
            control.position_count);
        if (count == 0u
            || count - 1u < stream_width * lanes
            || core.begin_bp >= core.end_bp
            || core.end_bp - core.begin_bp
                > static_cast<std::uint64_t>(
                    kMinimizerGrouped32MaxLocalBp) + 1u) {
            return result;
        }

        const std::size_t first_element = static_cast<std::size_t>(
            control.position_offset);
        const std::size_t available_words = encoded_bytes.size()
            / kMinimizerGrouped32PositionEncodedBytes;
        if (first_element > available_words
            || count > available_words - first_element) {
            return result;
        }
        const std::size_t first_byte = first_element
            * kMinimizerGrouped32PositionEncodedBytes;
        const auto segment_width = static_cast<std::uint32_t>(
            core.end_bp - core.begin_bp);
        const std::uint32_t first =
            minimizer_generation_wire_detail::read_le<std::uint32_t>(
                encoded_bytes, first_byte);
        if ((first >> 1u) >= segment_width) {
            result.validation = NonScatterSimdValidation::rejected;
            return result;
        }

        const auto* span_bytes = reinterpret_cast<const std::uint8_t*>(
            encoded_bytes.data() + first_byte);
        std::size_t position_index = 1u;
        auto previous_vector = grouped32_hn::Set(d32, first);
        auto invalid_masks = grouped32_vs::mask_stream<
            D32, stream_width>::splat_false(d32);
        const auto widths = grouped32_vs::vec_stream<
            D32, std::uint32_t, stream_width>::splat(d32, segment_width);
        const std::size_t words_per_iteration = stream_width * lanes;
        result.selected_vector_stream_width =
            static_cast<std::uint8_t>(stream_width);
        do {
            const auto loaded_bytes = grouped32_vs::make_vec_stream<
                D8, std::uint8_t, stream_width>(
                [d8, span_bytes, position_index, lanes](auto slot_constant)
                    [[gnu::always_inline]] {
                    constexpr std::size_t slot =
                        decltype(slot_constant)::value;
                    const std::size_t word_offset = position_index
                        + slot * lanes;
                    return grouped32_hn::LoadU(
                        d8,
                        span_bytes + word_offset
                            * kMinimizerGrouped32PositionEncodedBytes);
                });
            const auto current = grouped32_vs::BitCast(d32, loaded_bytes);
            const auto predecessors = grouped32_vs::make_vec_stream<
                D32, std::uint32_t, stream_width>(
                [d32, &current, previous_vector](auto slot_constant)
                    [[gnu::always_inline]] {
                    constexpr std::size_t slot =
                        decltype(slot_constant)::value;
                    if constexpr (slot == 0u) {
                        return grouped32_vs::lanes_shift_left_1_borrow(
                            d32, current[slot], previous_vector);
                    } else {
                        return grouped32_vs::lanes_shift_left_1_borrow(
                            d32, current[slot], current[slot - 1u]);
                    }
                });
            const auto invalid_order = grouped32_vs::Le(
                current, predecessors);
            const auto invalid_range = grouped32_vs::Ge(
                grouped32_vs::ShiftRight<1>(current), widths);
            invalid_masks = grouped32_vs::Or(
                invalid_masks,
                grouped32_vs::Or(invalid_order, invalid_range));
            previous_vector = current[stream_width - 1u];
            position_index += words_per_iteration;
            result.vectorized_words += words_per_iteration;
        } while (count - position_index >= words_per_iteration);

        const auto all_clear = grouped32_vs::AllFalsePerStream(
            d32, invalid_masks);
        if (!all_clear[0] || !all_clear[1]) {
            result.validation = NonScatterSimdValidation::rejected;
            return result;
        }

        std::uint32_t previous = grouped32_vs::extract_last(
            d32, previous_vector);
        while (position_index < count) {
            const std::uint32_t encoded =
                minimizer_generation_wire_detail::read_le<std::uint32_t>(
                    encoded_bytes,
                    first_byte + position_index
                        * kMinimizerGrouped32PositionEncodedBytes);
            if (encoded <= previous
                || (encoded >> 1u) >= segment_width) {
                result.validation = NonScatterSimdValidation::rejected;
                return result;
            }
            previous = encoded;
            ++position_index;
        }
        result.validation = NonScatterSimdValidation::accepted;
        return result;
    }
#endif
}

namespace {

using ExactStatus = bioseq::simd::HWY_NAMESPACE::TwoStackContinuationStatus;
using ExactProof = bioseq::simd::HWY_NAMESPACE::TwoStackExecutionProof;

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

struct ErasedExactCollector {
    using minimizer_type = bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

    const ExactSimdIndexBridgeRequest* request = nullptr;

    [[nodiscard]] bool append_raw_storage(
            const std::uint64_t* raw,
            std::size_t count) {
        return request->append_raw(
            request->collector_context, raw, count);
    }
};

template<std::size_t W, std::size_t K, class Alphabet, class StorageUnit>
[[nodiscard]] ExactSimdIndexBridgeResult collect_exact_width_impl(
        const ExactSimdIndexBridgeRequest& request) {
    using sequence_config = sequence_custom_alphabet<Alphabet, StorageUnit>;
    using range_type = compact_sequence_range<sequence_config>;
    using traits = typename range_type::traits;
    using State = bioseq::simd::HWY_NAMESPACE::
        TwoStackContinuationState<W, K, traits>;
    const std::size_t lpu = bioseq::storage::lazy_packed_letters_per_unit(
        request.storage_unit_bytes,
        static_cast<std::uint8_t>(
            seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>)));

    if (request.read_fragment == nullptr
        || request.release_fragment == nullptr
        || request.append_raw == nullptr
        || request.sequence_context == nullptr
        || request.collector_context == nullptr) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_state);
    }
    if (lpu == 0u
        || request.storage_unit_bytes != sizeof(StorageUnit)
        || request.letters_per_unit != lpu) {
        return rejected_bridge_result(ExactSimdIndexStatus::unsupported_storage);
    }
    if (request.fragment_units == 0
        || request.lo_unit > request.hi_unit
        || request.hi_unit > request.total_units
        || request.lo_unit
            > (std::numeric_limits<std::size_t>::max)() / lpu
        || request.hi_unit
            > (std::numeric_limits<std::size_t>::max)() / lpu
        || request.total_units
            > (std::numeric_limits<std::size_t>::max)() / lpu
        || request.total_bp > request.total_units * lpu) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_range);
    }

    const std::size_t consume_begin_bp = request.lo_unit * lpu;
    const std::size_t consume_end_bp = std::min(
        request.hi_unit * lpu, request.total_bp);
    State state;
    const ExactStatus reset =
        bioseq::simd::HWY_NAMESPACE::reset_two_stack_nthash(
            state,
            request.window_size,
            request.kmer_size,
            request.emitted_position_base);
    if (reset != ExactStatus::ok) {
        return {
            .status = bridge_status(reset),
            .proof = bridge_proof(state.proof),
        };
    }

    const std::size_t first_fragment =
        request.lo_unit / request.fragment_units;
    const std::size_t fragment_end = request.hi_unit == 0
        ? 0
        : 1 + (request.hi_unit - 1) / request.fragment_units;
    if (fragment_end > request.fragment_count
        || request.fragment_units
            > (std::numeric_limits<std::size_t>::max)() / lpu) {
        return rejected_bridge_result(ExactSimdIndexStatus::invalid_range);
    }
    const std::size_t fragment_bp_capacity = request.fragment_units * lpu;
    ErasedExactCollector collector{.request = &request};
    ExactStatus consume_status = ExactStatus::ok;
    for (std::size_t fragment_no = first_fragment;
         fragment_no < fragment_end;
         ++fragment_no) {
        if (fragment_no
                > (std::numeric_limits<std::size_t>::max)()
                    / fragment_bp_capacity) {
            return rejected_bridge_result(
                ExactSimdIndexStatus::arithmetic_overflow);
        }
        const ExactSimdIndexPackedFragment fragment =
            request.read_fragment(request.sequence_context, fragment_no);
        if (fragment.storage_units != 0 && fragment.data == nullptr) {
            request.release_fragment(request.sequence_context, fragment_no);
            return rejected_bridge_result(ExactSimdIndexStatus::invalid_range);
        }
        const std::size_t fragment_begin_bp =
            fragment_no * fragment_bp_capacity;
        const std::size_t packed_bp_capacity = fragment.storage_units
                > (std::numeric_limits<std::size_t>::max)() / lpu
            ? (std::numeric_limits<std::size_t>::max)()
            : fragment.storage_units * lpu;
        const std::size_t fragment_bp_size = std::min(
            packed_bp_capacity,
            request.total_bp - std::min(request.total_bp, fragment_begin_bp));
        const std::size_t fragment_consume_begin = std::max(
            consume_begin_bp, fragment_begin_bp);
        const std::size_t fragment_consume_end = std::min(
            consume_end_bp, fragment_begin_bp + fragment_bp_size);
        try {
            consume_status = bioseq::simd::HWY_NAMESPACE::
                consume_two_stack_nthash_packed_fragment<
                    W, K,
                    bioseq::simd::HWY_NAMESPACE::MinimapSketchData>(
                        state,
                        collector,
                        std::span<const StorageUnit>{
                            static_cast<const StorageUnit*>(fragment.data),
                            fragment.storage_units},
                        fragment_bp_size,
                        fragment_begin_bp,
                        fragment_consume_begin,
                        fragment_consume_end);
        } catch (...) {
            request.release_fragment(request.sequence_context, fragment_no);
            throw;
        }
        request.release_fragment(request.sequence_context, fragment_no);
        if (consume_status != ExactStatus::ok) break;
    }
    if (consume_status != ExactStatus::ok) {
        return {
            .status = bridge_status(consume_status),
            .proof = bridge_proof(state.proof),
        };
    }
    const auto finalized =
        bioseq::simd::HWY_NAMESPACE::finalize_two_stack_nthash(
            state, collector);
    return {
        .status = bridge_status(finalized.status),
        .proof = bridge_proof(finalized.proof),
    };
}

template<class Alphabet, class StorageUnit>
[[nodiscard]] ExactSimdIndexBridgeResult collect_exact_width(
        const ExactSimdIndexBridgeRequest& request) {
    if (request.window_size == 10 && request.kmer_size == 15) [[likely]] {
        return collect_exact_width_impl<10, 15, Alphabet, StorageUnit>(request);
    }
    return collect_exact_width_impl<0, 0, Alphabet, StorageUnit>(request);
}

template<class Alphabet>
[[nodiscard]] ExactSimdIndexBridgeResult collect_exact_alphabet(
        const ExactSimdIndexBridgeRequest& request) {
    switch (request.storage_unit_bytes) {
        case 1:
            return collect_exact_width<Alphabet, std::uint8_t>(request);
        case 2:
            return collect_exact_width<Alphabet, std::uint16_t>(request);
        case 4:
            return collect_exact_width<Alphabet, std::uint32_t>(request);
        case 8:
            return collect_exact_width<Alphabet, std::uint64_t>(request);
        default:
            return rejected_bridge_result(
                ExactSimdIndexStatus::unsupported_storage);
    }
}

using PackedPaperStatus =
    bioseq::simd::HWY_NAMESPACE::PackedPaperNthashStatus;
using PackedPaperProof =
    bioseq::simd::HWY_NAMESPACE::PackedPaperNthashExecutionProof;

static_assert(kPackedPaperNthashKernelSchemeId
    == bioseq::simd::HWY_NAMESPACE::packed_paper_nthash_scheme_id);
static_assert(kPackedPaperNthashMaximumWindow
    == bioseq::simd::HWY_NAMESPACE::packed_paper_nthash_max_window);

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

template<class StorageUnit>
[[nodiscard]] PackedPaperNthashIndexBridgeResult
collect_paper_storage_resident_impl(
        const PackedPaperNthashIndexBridgeRequest& request) {
    using sequence_config = sequence_custom_alphabet<seq::dna4, StorageUnit>;
    using range_type = compact_sequence_range<sequence_config>;
    const std::size_t letters_per_unit =
        bioseq::storage::lazy_packed_letters_per_unit(
            request.storage_unit_bytes, 2u);

    if (letters_per_unit == 0u
        || request.storage_unit_bytes != sizeof(StorageUnit)
        || request.letters_per_unit != letters_per_unit) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::unsupported_storage);
    }
    if (request.storage_units
            > (std::numeric_limits<std::size_t>::max)()
                / letters_per_unit
        || request.local_bp_size
            > request.storage_units * letters_per_unit) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_range);
    }
    if (request.local_bp_size
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::input_too_large);
    }
    if (request.local_bp_size != 0
        && request.local_bp_size - 1u
            > static_cast<std::size_t>(
                bioseq::simd::HWY_NAMESPACE::
                    MinimapSketchData::pos_field_mask)) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::unsupported_representation);
    }
    if (request.sequence_context == nullptr
        || request.collector_context == nullptr) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_state);
    }

    auto& sequence = *static_cast<range_type*>(request.sequence_context);
    auto& collector = *static_cast<
        bioseq::simd::HWY_NAMESPACE::sketch_collector*>(
            request.collector_context);
    if (static_cast<std::size_t>(sequence.size())
            != request.storage_units
        || static_cast<std::size_t>(sequence.bp_size())
            != request.local_bp_size) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_range);
    }

    const auto result = bioseq::simd::HWY_NAMESPACE::
        collect_minimizers_packed_paper_nthash_simd_checked<
            15, 10,
            bioseq::simd::HWY_NAMESPACE::MinimapSketchData>(
                collector,
                sequence,
                sequence.begin(),
                sequence.end());
    return {
        .status = bridge_paper_status(result.status),
        .proof = bridge_paper_proof(result.proof),
    };
}

}  // namespace

ExactSimdIndexBridgeResult collect_exact_simd_index_erased(
        const ExactSimdIndexBridgeRequest& request) {
    switch (request.alphabet) {
        case ExactSimdIndexAlphabet::dna4:
            return collect_exact_alphabet<seq::dna4>(request);
        case ExactSimdIndexAlphabet::dna5:
            return collect_exact_alphabet<seq::dna5>(request);
    }
    return rejected_bridge_result(ExactSimdIndexStatus::unsupported_alphabet);
}

PackedPaperNthashIndexBridgeResult
collect_packed_paper_nthash_index_erased(
        const PackedPaperNthashIndexBridgeRequest& request) {
    if (request.window_size != 10u || request.kmer_size != 15u) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::invalid_configuration);
    }
    if (request.alphabet != PackedPaperNthashIndexAlphabet::dna4) {
        return rejected_packed_paper_nthash_index_result(
            PackedPaperNthashIndexStatus::unsupported_alphabet);
    }
    switch (request.storage_unit_bytes) {
        case sizeof(std::uint32_t):
            return collect_paper_storage_resident_impl<std::uint32_t>(request);
        case sizeof(std::uint64_t):
            return collect_paper_storage_resident_impl<std::uint64_t>(request);
        default:
            return rejected_packed_paper_nthash_index_result(
                PackedPaperNthashIndexStatus::unsupported_storage);
    }
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
