/** Canonical generator input for the exact packed paper-ntHash kernel. */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.minimizer_paper;

import std;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.instr;
import bioseq.sequences.kmers.highway_contract;
import bioseq.highway.sources.sequence_ops;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.vector_stream;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using ::bioseq::minimizers::MinimapSketchData;
using ::bioseq::minimizers::minimizer_sink_collector;
using ::bioseq::minimizers::two_stack_max_window;
using ::bioseq::minimizers::two_stack_exact_max_kmer;
using ::bioseq::minimizers::PackedPaperNthashStatus;
using ::bioseq::minimizers::packed_paper_nthash_scheme_id;
using ::bioseq::minimizers::packed_paper_nthash_max_window;
using ::bioseq::minimizers::PackedPaperNthashExecutionProof;
using ::bioseq::minimizers::PackedPaperNthashResult;
using ::bioseq::minimizers::MinimizerKernelAlgorithm;
using ::bioseq::minimizers::MinimizerKernelInvocation;
using ::bioseq::minimizers::MinimizerKernelResult;
using ::bioseq::minimizers::MinimizerKernelStatus;

namespace paper_nthash_internal {

inline constexpr std::array<std::uint32_t, 4> forward_table{
    0x95c60474u,
    0x62a02b4cu,
    0x82572324u,
    0x4be24456u,
};

/** Look up ACTG ntHash constants in one native u32 register. */
template<class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> lookup_hash_table(
    D d,
    hn::Vec<D> table,
    hn::Vec<D> ranks) noexcept
{
    return hn::TableLookupLanes(
        table, hn::IndicesFromVec(d, ranks));
}

} // namespace paper_nthash_internal

// =============================================================================
// Packed paper-ntHash SIMD kernel.
//
// This is deliberately a fork of the report-76 packed/two-stack architecture,
// not an extension of the O(N) semantic model above.  It consumes resident
// compact dna4 words through iterate_sequence_bp_simd_v2, keeps only bounded
// K-symbol and W-minimum state, and emits directly into the caller's collector.
// The generated fixed-target registry routes production calls to this body.
// =============================================================================

namespace packed_paper_nthash_internal {

/**
 * Zero-copy u32 interpretation of one base-storage-unit-aligned dna4 range.
 * Unlike bit_casted_view itself, size/bp_size/padding describe only the
 * selected [begin,end) interval, so iterator offset zero maps to local
 * position zero and every emitted position is segment-local.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN bounded_u32_dna4_view
struct bounded_u32_dna4_view {
    using output_sequence = sequence_custom_alphabet<
        seq::dna4, std::uint32_t, 2>;
    using output_range = compact_sequence_range<output_sequence>;
    using traits = typename output_range::traits;
    using iterator = const std::uint32_t*;

    std::size_t local_bp_size = 0;
    std::size_t local_view_units = 0;

    bounded_u32_dna4_view(
        std::size_t bp_size,
        std::size_t view_units)
        : local_bp_size(bp_size),
          local_view_units(view_units) {}

    [[nodiscard]] HWY_INLINE std::size_t bp_size() const noexcept {
        return local_bp_size;
    }
    [[nodiscard]] HWY_INLINE std::size_t byte_size() const noexcept {
        return local_view_units * sizeof(std::uint32_t);
    }
    [[nodiscard]] HWY_INLINE std::size_t size() const noexcept {
        return local_view_units;
    }
    [[nodiscard]] HWY_INLINE std::size_t padding() const noexcept {
        return local_view_units * traits::letters_per_unit - local_bp_size;
    }
    [[nodiscard]] HWY_INLINE const std::uint32_t* read_cptr(
        const iterator& position) const noexcept {
        return position;
    }
    HWY_INLINE static void advance_iter(
        iterator& position,
        std::size_t view_units) {
        position += view_units;
    }
    [[nodiscard]] HWY_INLINE static std::size_t view_distance(
        const iterator& first,
        const iterator& last) {
        return static_cast<std::size_t>(last - first);
    }
};
// BIOSEQ_PRIVATE_PACKED_ACCESS_END bounded_u32_dna4_view

template<class D, std::size_t NumStreams>
[[nodiscard]] HWY_INLINE auto lookup_table_stream(
    D d,
    hn::Vec<D> table,
    const vec_stream<D, std::uint32_t, NumStreams>& ranks) noexcept
{
    return make_vec_stream<D, std::uint32_t, NumStreams>(
        [&](auto stream_constant) [[gnu::always_inline]] {
            constexpr std::size_t stream =
                decltype(stream_constant)::value;
            return paper_nthash_internal::lookup_hash_table(
                d, table, ranks[stream]);
        });
}

template<int Rotation, class D, std::size_t NumStreams>
[[nodiscard]] HWY_INLINE auto rotate_left_stream(
    const vec_stream<D, std::uint32_t, NumStreams>& value) noexcept
{
    if constexpr ((Rotation & 31) == 0) {
        return value;
    } else {
        return RotateLeft<(Rotation & 31)>(value);
    }
}

template<class D, std::size_t NumStreams>
[[nodiscard]] HWY_INLINE auto rotate_left_stream_runtime(
    const vec_stream<D, std::uint32_t, NumStreams>& value,
    int rotation) noexcept
{
    rotation &= 31;
    if (rotation == 0) return value;
    return Or(
        ShiftLeftSame(value, rotation),
        ShiftRightSame(value, 32 - rotation));
}

template<int Rotation, class D, std::size_t NumStreams>
[[nodiscard]] HWY_INLINE auto rotate_right_stream(
    const vec_stream<D, std::uint32_t, NumStreams>& value) noexcept
{
    if constexpr ((Rotation & 31) == 0) {
        return value;
    } else {
        return RotateRight<(Rotation & 31)>(value);
    }
}

/** One full-u32 minimum plus RC strand at its two extreme positions. */
template<class D, std::size_t NumStreams>
struct minimum_stream {
    using vec_stream_t = vec_stream<D, std::uint32_t, NumStreams>;
    using mask_stream_t = mask_stream<D, NumStreams>;

    vec_stream_t hash;
    vec_stream_t left_position;
    vec_stream_t right_position;
    vec_stream_t left_strand;
    vec_stream_t right_strand;
    mask_stream_t valid;

    // This template is instantiated by an ordinary per-target owner unit.
    // Keep every special member explicit so the renderer can attach HWY_ATTR;
    // otherwise Clang synthesizes targetless members around BF16 streams.
    HWY_INLINE minimum_stream() noexcept
        : hash{},
          left_position{},
          right_position{},
          left_strand{},
          right_strand{},
          valid{} {}

    HWY_INLINE minimum_stream(
        vec_stream_t hash_value,
        vec_stream_t left_position_value,
        vec_stream_t right_position_value,
        vec_stream_t left_strand_value,
        vec_stream_t right_strand_value,
        mask_stream_t valid_value) noexcept
        : hash(std::move(hash_value)),
          left_position(std::move(left_position_value)),
          right_position(std::move(right_position_value)),
          left_strand(std::move(left_strand_value)),
          right_strand(std::move(right_strand_value)),
          valid(std::move(valid_value)) {}

    HWY_INLINE minimum_stream(const minimum_stream& other) noexcept
        : hash(other.hash),
          left_position(other.left_position),
          right_position(other.right_position),
          left_strand(other.left_strand),
          right_strand(other.right_strand),
          valid(other.valid) {}

    HWY_INLINE minimum_stream(minimum_stream&& other) noexcept
        : hash(std::move(other.hash)),
          left_position(std::move(other.left_position)),
          right_position(std::move(other.right_position)),
          left_strand(std::move(other.left_strand)),
          right_strand(std::move(other.right_strand)),
          valid(std::move(other.valid)) {}

    HWY_INLINE minimum_stream& operator=(
        const minimum_stream& other) noexcept
    {
        hash = other.hash;
        left_position = other.left_position;
        right_position = other.right_position;
        left_strand = other.left_strand;
        right_strand = other.right_strand;
        valid = other.valid;
        return *this;
    }

    HWY_INLINE minimum_stream& operator=(minimum_stream&& other) noexcept
    {
        hash = std::move(other.hash);
        left_position = std::move(other.left_position);
        right_position = std::move(other.right_position);
        left_strand = std::move(other.left_strand);
        right_strand = std::move(other.right_strand);
        valid = std::move(other.valid);
        return *this;
    }
};

/**
 * Data-independent lane-wise minimum combine. Equal hashes preserve only the
 * leftmost and rightmost positions, which is the explicit RC-symmetric policy
 * for the even default W=10 window.
 */
template<class D, std::size_t NumStreams>
[[nodiscard]] HWY_INLINE minimum_stream<D, NumStreams> combine_minima(
    D,
    const minimum_stream<D, NumStreams>& first,
    const minimum_stream<D, NumStreams>& second) noexcept
{
    const auto both_valid = And(first.valid, second.valid);
    const auto second_smaller = And(
        second.valid,
        Or(AndNot(first.valid, second.valid), Lt(second.hash, first.hash)));
    const auto hashes_equal = And(both_valid, Eq(first.hash, second.hash));
    const auto second_leftmost = And(
        hashes_equal, Lt(second.left_position, first.left_position));
    const auto second_rightmost = And(
        hashes_equal, Gt(second.right_position, first.right_position));

    return minimum_stream<D, NumStreams>{
        IfThenElse(second_smaller, second.hash, first.hash),
        IfThenElse(
            second_smaller,
            second.left_position,
            IfThenElse(
                second_leftmost,
                second.left_position,
                first.left_position)),
        IfThenElse(
            second_smaller,
            second.right_position,
            IfThenElse(
                second_rightmost,
                second.right_position,
                first.right_position)),
        IfThenElse(
            second_smaller,
            second.left_strand,
            IfThenElse(
                second_leftmost,
                second.left_strand,
                first.left_strand)),
        IfThenElse(
            second_smaller,
            second.right_strand,
            IfThenElse(
                second_rightmost,
                second.right_strand,
                first.right_strand)),
        Or(first.valid, second.valid),
    };
}

/**
 * v2 supplies one tail mask for the whole vec_stream. Only the final logical
 * stream actually reaches sequence padding; applying that mask to every
 * stream drops valid bases from all earlier streams. This adapter is the
 * production fork's explicit tail fix.
 */
template<bool IsFull, class D, std::size_t NumStreams>
[[nodiscard]] HWY_INLINE auto corrected_live_stream(
    D d,
    hn::Mask<D> iterator_mask) noexcept
{
    const auto full = hn::FirstN(d, hn::Lanes(d));
    return [&]<std::size_t... Streams>(
               std::index_sequence<Streams...>) [[gnu::always_inline]] {
        return mask_stream<D, NumStreams>::pack(
            ((IsFull || Streams + 1 < NumStreams)
                ? full
                : iterator_mask)...);
    }(std::make_index_sequence<NumStreams>{});
}

template<class D, std::size_t NumStreams>
[[nodiscard]] HWY_INLINE auto load_stream_metadata(
    D d,
    const std::array<std::uint32_t, 8>& values) noexcept
{
    const std::size_t lanes = hn::Lanes(d);
    return make_vec_stream<D, std::uint32_t, NumStreams>(
        [&](auto stream_constant) [[gnu::always_inline]] {
            constexpr std::size_t stream =
                decltype(stream_constant)::value;
            return hn::LoadU(d, values.data() + stream * lanes);
        });
}

template<
    std::size_t K,
    std::size_t W,
    std::size_t NumStreams,
    class MinimizerT,
    class Collector>
struct packed_iteration_state {
    using d_t = hn::CappedTag<std::uint32_t, 8>;
    using vec_t = hn::Vec<d_t>;
    using mask_t = hn::Mask<d_t>;
    using vec_stream_t = vec_stream<d_t, std::uint32_t, NumStreams>;
    using mask_stream_t = mask_stream<d_t, NumStreams>;
    using minimum_t = minimum_stream<d_t, NumStreams>;

    struct buffered_boundary_event {
        std::uint32_t hash = 0;
        std::uint64_t absolute_last_position = 0;
        std::uint32_t strand = 0;
        bool valid = false;
    };

    struct runtime_evidence {
        std::uint64_t emission_mask_store_batches = 0;
        std::uint64_t minimizers_emitted = 0;
        std::uint64_t tied_endpoint_records_emitted = 0;
        std::uint64_t stream_boundary_records_buffered = 0;
        std::uint64_t stream_boundary_records_committed = 0;
        std::uint64_t stream_boundary_duplicates_suppressed = 0;
        std::uint64_t scalar_collector_appends = 0;
    };

    vec_stream_t forward_hash;
    vec_stream_t reverse_hash;
    std::array<vec_stream_t, K> symbol_ring;
    std::size_t symbols_seen = 0;
    std::size_t next_symbol_slot = 0;

    minimum_t prefix_minimum;
    std::array<minimum_t, W> suffix_ring;
    std::size_t next_window_slot = 0;

    vec_stream_t window_limits;
    vec_stream_t last_emit_hash;
    vec_stream_t last_emit_position;
    vec_stream_t last_emit_strand;
    mask_stream_t last_emit_valid;
    std::array<bool, 8> logical_lane_has_event{};
    std::array<buffered_boundary_event, 8> first_boundary_event{};

    vec_t forward_table;
    vec_t complement_table;
    Collector& collector;
    runtime_evidence evidence{};

    template<bool IsFull, class Offsets>
    [[gnu::always_inline]] [[gnu::flatten]] HWY_INLINE void step(
        d_t d,
        const Offsets& offsets,
        mask_t iterator_mask,
        vec_stream_t incoming_rank)
    {
        const auto live = corrected_live_stream<IsFull, d_t, NumStreams>(
            d, iterator_mask);

        if (symbols_seen < K) {
            symbol_ring[symbols_seen] = incoming_rank;
            const auto forward_value = lookup_table_stream(
                d, forward_table, incoming_rank);
            const auto reverse_value = lookup_table_stream(
                d, complement_table, incoming_rank);
            const int forward_rotation = static_cast<int>(
                ((K - 1 - symbols_seen) * 7) & 31u);
            const int reverse_rotation = static_cast<int>(
                (symbols_seen * 7) & 31u);
            // Initialization runs only K times. The rolling steady state below
            // uses compile-time rotations; this bounded startup uses the
            // target's vector shift-by-scalar lowering.
            forward_hash = Xor(
                forward_hash,
                rotate_left_stream_runtime(
                    forward_value, forward_rotation));
            reverse_hash = Xor(
                reverse_hash,
                rotate_left_stream_runtime(
                    reverse_value, reverse_rotation));
            ++symbols_seen;
            if (symbols_seen < K) return;
        } else {
            const vec_stream_t outgoing_rank =
                symbol_ring[next_symbol_slot];
            symbol_ring[next_symbol_slot] = incoming_rank;
            if (++next_symbol_slot == K) next_symbol_slot = 0;

            const auto forward_out = lookup_table_stream(
                d, forward_table, outgoing_rank);
            const auto forward_in = lookup_table_stream(
                d, forward_table, incoming_rank);
            const auto reverse_out = lookup_table_stream(
                d, complement_table, outgoing_rank);
            const auto reverse_in = lookup_table_stream(
                d, complement_table, incoming_rank);
            forward_hash = Xor(
                Xor(
                    rotate_left_stream<7>(forward_hash),
                    rotate_left_stream<static_cast<int>((K * 7) & 31u)>(
                        forward_out)),
                forward_in);
            reverse_hash = Xor(
                Xor(
                    rotate_right_stream<7>(reverse_hash),
                    rotate_right_stream<7>(reverse_out)),
                rotate_left_stream<
                    static_cast<int>(((K - 1) * 7) & 31u)>(reverse_in));
            ++symbols_seen;
        }

        const std::uint32_t local_kmer_position =
            static_cast<std::uint32_t>(symbols_seen - K);
        const vec_stream_t position = vec_stream_t::splat(
            d, local_kmer_position);
        const vec_stream_t canonical_hash = Add(
            forward_hash, reverse_hash);
        const mask_stream_t reverse_strand = Lt(
            reverse_hash, forward_hash);
        const vec_stream_t strand = IfThenElse(
            reverse_strand,
            vec_stream_t::splat(d, 1u),
            vec_stream_t::splat_zero(d));
        const minimum_t entering{
            canonical_hash,
            position,
            position,
            strand,
            strand,
            live,
        };

        const std::size_t written_slot = next_window_slot;
        if (++next_window_slot == W) next_window_slot = 0;
        suffix_ring[written_slot] = entering;
        prefix_minimum = combine_minima(d, prefix_minimum, entering);
        const bool rebuild_suffixes = next_window_slot == 0;
        if (rebuild_suffixes) {
            minimum_t suffix = suffix_ring[W - 1];
            for (std::size_t slot = W - 1; slot > 0; --slot) {
                suffix = combine_minima(
                    d, suffix_ring[slot - 1], suffix);
                suffix_ring[slot - 1] = suffix;
            }
            prefix_minimum = entering;
        }
        const minimum_t window_minimum = combine_minima(
            d, prefix_minimum, suffix_ring[next_window_slot]);

        const std::size_t candidate_index = local_kmer_position;
        if (candidate_index + 1 < W) return;
        const std::uint32_t window_index = static_cast<std::uint32_t>(
            candidate_index + 1 - W);
        const vec_stream_t window_index_v = vec_stream_t::splat(
            d, window_index);
        const mask_stream_t owned_window = And(
            window_minimum.valid,
            Lt(window_index_v, window_limits));
        const mask_stream_t tied = And(
            owned_window,
            Ne(window_minimum.left_position,
               window_minimum.right_position));

        emit_selected(
            d,
            offsets,
            owned_window,
            window_minimum.hash,
            window_minimum.left_position,
            window_minimum.left_strand,
            false);
        const auto no_ties_per_stream = AllFalsePerStream(d, tied);
        bool any_tie = false;
        for (const bool no_tie : no_ties_per_stream) {
            any_tie = any_tie || !no_tie;
        }
        if (any_tie) [[unlikely]] {
            emit_selected(
                d,
                offsets,
                tied,
                window_minimum.hash,
                window_minimum.right_position,
                window_minimum.right_strand,
                true);
        }
    }

private:
    template<class Offsets>
    [[gnu::always_inline]] HWY_INLINE void emit_selected(
        d_t d,
        const Offsets& offsets,
        mask_stream_t selected,
        const vec_stream_t& hash,
        const vec_stream_t& position,
        const vec_stream_t& strand,
        bool tie_endpoint)
    {
        const auto same_identity = And(
            last_emit_valid,
            And(Eq(hash, last_emit_hash),
                And(Eq(position, last_emit_position),
                    Eq(strand, last_emit_strand))));
        const auto emit = AndNot(same_identity, selected);
        const auto no_emit_per_stream = AllFalsePerStream(d, emit);
        bool any_emit = false;
        for (const bool no_emit : no_emit_per_stream) {
            any_emit = any_emit || !no_emit;
        }
        if (!any_emit) return;

        constexpr std::size_t max_lanes = hn::MaxLanes(d_t{});
        HWY_ALIGN std::array<std::array<std::uint32_t, max_lanes>,
            NumStreams> hash_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, max_lanes>,
            NumStreams> position_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, max_lanes>,
            NumStreams> strand_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, max_lanes>,
            NumStreams> emit_lanes;
        for_each_stream<NumStreams>([&](auto stream_constant) {
            constexpr std::size_t stream =
                decltype(stream_constant)::value;
            if (no_emit_per_stream[stream]) return;
            hn::StoreU(hash[stream], d, hash_lanes[stream].data());
            hn::StoreU(
                position[stream], d, position_lanes[stream].data());
            hn::StoreU(strand[stream], d, strand_lanes[stream].data());
            hn::StoreU(
                hn::VecFromMask(d, emit[stream]),
                d,
                emit_lanes[stream].data());
        });
        ++evidence.emission_mask_store_batches;

        const std::size_t lanes = hn::Lanes(d);
        for (std::size_t stream = 0; stream < NumStreams; ++stream) {
            if (no_emit_per_stream[stream]) continue;
            for (std::size_t lane = 0; lane < lanes; ++lane) {
                if (emit_lanes[stream][lane] == 0) continue;
                const std::size_t logical_lane = stream * lanes + lane;
                const std::uint64_t absolute_last_position =
                    static_cast<std::uint64_t>(offsets[logical_lane]) * 16u
                    + position_lanes[stream][lane]
                    + (K - 1);
                if (!logical_lane_has_event[logical_lane]) {
                    logical_lane_has_event[logical_lane] = true;
                    if (logical_lane != 0) {
                        first_boundary_event[logical_lane] = {
                            .hash = hash_lanes[stream][lane],
                            .absolute_last_position = absolute_last_position,
                            .strand = strand_lanes[stream][lane],
                            .valid = true,
                        };
                        ++evidence.stream_boundary_records_buffered;
                        continue;
                    }
                }
                append_collector_event(
                    hash_lanes[stream][lane],
                    absolute_last_position,
                    strand_lanes[stream][lane],
                    tie_endpoint);
            }
        }

        last_emit_hash = IfThenElse(emit, hash, last_emit_hash);
        last_emit_position = IfThenElse(
            emit, position, last_emit_position);
        last_emit_strand = IfThenElse(emit, strand, last_emit_strand);
        last_emit_valid = Or(last_emit_valid, emit);
    }

public:
    /**
     * Complete adjacent dedup across the seven logical-stream joins using
     * bounded metadata only. The first event of lanes 1..7 was withheld; it
     * is appended here unless it equals the nearest preceding non-empty
     * lane's final event. Collector append order is intentionally unspecified,
     * as it already is for eight lock-step SIMD streams; record identity is
     * globally adjacent-deduplicated.
     */
    template<class Offsets>
    HWY_INLINE void finalize_stream_boundaries(
        d_t d,
        const Offsets& offsets) {
        constexpr std::size_t max_lanes = hn::MaxLanes(d_t{});
        HWY_ALIGN std::array<std::array<std::uint32_t, max_lanes>,
            NumStreams> hash_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, max_lanes>,
            NumStreams> position_lanes;
        HWY_ALIGN std::array<std::array<std::uint32_t, max_lanes>,
            NumStreams> strand_lanes;
        for_each_stream<NumStreams>([&](auto stream_constant) {
            constexpr std::size_t stream =
                decltype(stream_constant)::value;
            hn::StoreU(
                last_emit_hash[stream], d, hash_lanes[stream].data());
            hn::StoreU(
                last_emit_position[stream],
                d,
                position_lanes[stream].data());
            hn::StoreU(
                last_emit_strand[stream],
                d,
                strand_lanes[stream].data());
        });

        bool previous_valid = false;
        std::uint32_t previous_hash = 0;
        std::uint64_t previous_position = 0;
        std::uint32_t previous_strand = 0;
        const std::size_t lanes = hn::Lanes(d);
        for (std::size_t logical_lane = 0;
             logical_lane < 8;
             ++logical_lane) {
            if (!logical_lane_has_event[logical_lane]) continue;
            buffered_boundary_event& pending =
                first_boundary_event[logical_lane];
            if (pending.valid) {
                const bool duplicate = previous_valid
                    && pending.hash == previous_hash
                    && pending.absolute_last_position == previous_position
                    && pending.strand == previous_strand;
                if (duplicate) {
                    ++evidence.stream_boundary_duplicates_suppressed;
                } else {
                    append_collector_event(
                        pending.hash,
                        pending.absolute_last_position,
                        pending.strand,
                        false);
                    ++evidence.stream_boundary_records_committed;
                }
            }

            const std::size_t stream = logical_lane / lanes;
            const std::size_t lane = logical_lane % lanes;
            previous_hash = hash_lanes[stream][lane];
            previous_position =
                static_cast<std::uint64_t>(offsets[logical_lane]) * 16u
                + position_lanes[stream][lane]
                + (K - 1);
            previous_strand = strand_lanes[stream][lane];
            previous_valid = true;
        }
    }

private:
    HWY_INLINE void append_collector_event(
        std::uint32_t hash,
        std::uint64_t absolute_last_position,
        std::uint32_t strand,
        bool tie_endpoint)
    {
        collector.collect(MinimizerT(
            static_cast<std::uint64_t>(hash),
            absolute_last_position,
            static_cast<std::uint64_t>(strand)));
        ++evidence.minimizers_emitted;
        ++evidence.scalar_collector_appends;
        if (tie_endpoint) ++evidence.tied_endpoint_records_emitted;
    }
};

template<
    std::size_t K,
    std::size_t W,
    std::size_t NumStreams,
    class MinimizerT,
    class Collector,
    supports_compact_sequence_bitcast_view View,
    class Iterator>
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN run_packed_kernel
[[gnu::always_inline]] [[gnu::flatten]] inline void run_packed_kernel(
    Collector& collector,
    const View& view,
    const Iterator& begin,
    const Iterator& end,
    PackedPaperNthashExecutionProof& proof)
{
    using d_t = hn::CappedTag<std::uint32_t, 8>;
    using vec_stream_t = vec_stream<d_t, std::uint32_t, NumStreams>;
    using mask_stream_t = mask_stream<d_t, NumStreams>;
    using state_t = packed_iteration_state<
        K, W, NumStreams, MinimizerT, Collector>;
    using minimum_t = typename state_t::minimum_t;
    const d_t d;
    const std::size_t lanes = hn::Lanes(d);
    constexpr std::size_t logical_streams = 8;
    constexpr std::size_t letters_per_unit = 16;
    constexpr std::size_t context_bp = K + W - 1;
    constexpr std::size_t context_units =
        (context_bp + letters_per_unit - 1) / letters_per_unit;
    const auto [units_per_lane, offsets] =
        calculate_iteration_offsets<logical_streams>(
            view.size(), logical_streams, context_units);
    const std::size_t total_windows =
        view.bp_size() - K - W + 2;

    std::array<std::uint32_t, logical_streams> window_limits{};
    for (std::size_t lane = 0; lane < logical_streams; ++lane) {
        const std::size_t start_bp = offsets[lane] * letters_per_unit;
        const std::size_t unclamped_end_bp = lane + 1 < logical_streams
            ? offsets[lane + 1] * letters_per_unit
            : total_windows;
        const std::size_t end_bp = std::min(
            unclamped_end_bp, total_windows);
        const std::size_t owned = start_bp < total_windows
                && end_bp > start_bp
            ? end_bp - start_bp
            : 0;
        window_limits[lane] = static_cast<std::uint32_t>(owned);
    }

    constexpr std::size_t max_lanes = hn::MaxLanes(d_t{});
    HWY_ALIGN std::array<std::uint32_t, max_lanes> table_values{};
    HWY_ALIGN std::array<std::uint32_t, max_lanes> complement_values{};
    for (std::size_t lane = 0; lane < max_lanes; ++lane) {
        const std::size_t rank = lane & 3u;
        table_values[lane] = paper_nthash_internal::forward_table[rank];
        complement_values[lane] =
            paper_nthash_internal::forward_table[3u - rank];
    }
    const auto zero = vec_stream_t::splat_zero(d);
    const auto false_masks = mask_stream_t::splat_false(d);
    const minimum_t empty_minimum{
        zero,
        zero,
        zero,
        zero,
        zero,
        false_masks,
    };
    state_t state{
        .forward_hash = zero,
        .reverse_hash = zero,
        .symbol_ring = {},
        .symbols_seen = 0,
        .next_symbol_slot = 0,
        .prefix_minimum = empty_minimum,
        .suffix_ring = {},
        .next_window_slot = 0,
        .window_limits = load_stream_metadata<d_t, NumStreams>(
            d, window_limits),
        .last_emit_hash = zero,
        .last_emit_position = zero,
        .last_emit_strand = zero,
        .last_emit_valid = false_masks,
        .forward_table = hn::LoadU(d, table_values.data()),
        .complement_table = hn::LoadU(d, complement_values.data()),
        .collector = collector,
        .evidence = {},
    };
    for (std::size_t slot = 0; slot < W; ++slot) {
        state.suffix_ring[slot] = empty_minimum;
    }

    iterate_sequence_bp_simd_v2<NumStreams, d_t>(
        view,
        begin,
        end,
        context_bp,
        [&]<bool IsFull>(
            std::bool_constant<IsFull>,
            const auto& iterator_offsets,
            const auto& d_in,
            hn::Mask<d_t> iterator_mask,
            vec_stream_t ranks) [[gnu::always_inline]] [[gnu::flatten]] {
            state.template step<IsFull>(
                d_in, iterator_offsets, iterator_mask, ranks);
        },
        [](const auto&, const auto&, auto&) {});
    state.finalize_stream_boundaries(d, offsets);

    // All per-base proof values are deterministic from admitted geometry.
    // Populate them once after the kernel so telemetry adds no memory traffic
    // to the biological loop.
    const std::uint64_t callback_steps =
        static_cast<std::uint64_t>(context_units + units_per_lane)
        * letters_per_unit;
    const std::uint64_t initialization_batches =
        std::min<std::uint64_t>(callback_steps, K);
    const std::uint64_t rolling_batches = callback_steps > K
        ? callback_steps - K
        : 0;
    const std::uint64_t candidate_batches = callback_steps >= K
        ? callback_steps - K + 1
        : 0;
    const std::uint64_t decision_batches = candidate_batches >= W
        ? candidate_batches - W + 1
        : 0;
    const std::uint64_t bps_per_matrix =
        static_cast<std::uint64_t>(lanes) * letters_per_unit;
    std::uint64_t partial_callbacks = callback_steps % bps_per_matrix;
    if (partial_callbacks == 0 && callback_steps != 0) {
        partial_callbacks = bps_per_matrix;
    }
    const std::uint64_t masked_tail_lanes =
        static_cast<std::uint64_t>(view.padding())
        * (offsets.front() == offsets.back() ? lanes : 1u);

    proof.packed_decode_batches = callback_steps;
    proof.packed_decode_lane_visits = callback_steps * logical_streams
        - masked_tail_lanes;
    proof.hash_initialization_batches = initialization_batches;
    proof.rolling_hash_batches = rolling_batches;
    proof.hash_table_lookup_batches =
        2 * initialization_batches + 4 * rolling_batches;
    proof.canonical_sum_batches = candidate_batches;
    proof.window_update_batches = candidate_batches;
    proof.suffix_rebuild_batches = candidate_batches / W;
    proof.emission_decision_batches = decision_batches;
    proof.partial_chunk_callback_batches = partial_callbacks;
    proof.masked_tail_lane_visits = masked_tail_lanes;
    proof.emission_mask_store_batches =
        state.evidence.emission_mask_store_batches;
    proof.minimizers_emitted = state.evidence.minimizers_emitted;
    proof.tied_endpoint_records_emitted =
        state.evidence.tied_endpoint_records_emitted;
    proof.stream_boundary_records_buffered =
        state.evidence.stream_boundary_records_buffered;
    proof.stream_boundary_records_committed =
        state.evidence.stream_boundary_records_committed;
    proof.stream_boundary_duplicates_suppressed =
        state.evidence.stream_boundary_duplicates_suppressed;
    proof.scalar_collector_appends =
        state.evidence.scalar_collector_appends;
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END run_packed_kernel

} // namespace packed_paper_nthash_internal

/**
 * Checked resident entry point for the production-oriented packed paper-ntHash
 * kernel. [begin,end) must be ordered and aligned to the resident range's
 * storage units. The zero-copy bounded u32 view emits segment-local positions;
 * segmented callers add their known global base offset after collection.
 */
template<
    std::size_t K = 15,
    std::size_t W = 10,
    class MinimizerT = MinimapSketchData,
    supports_compact_sequence_range R,
    class Iterator = decltype(compact_sequence_range<R>().begin()),
    class Collector = minimizer_sink_collector<MinimizerT>>
[[nodiscard]] inline PackedPaperNthashResult
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN collect_minimizers_packed_paper_nthash_simd_checked
collect_minimizers_packed_paper_nthash_simd_checked(
    Collector& collector,
    compact_sequence_range<R>& sequence,
    const Iterator& begin,
    const Iterator& end)
{
    using range_t = compact_sequence_range<R>;
    using traits = typename range_t::traits;
    PackedPaperNthashResult result;
    auto reject = [&](PackedPaperNthashStatus status) {
        result.status = status;
        result.proof.fail_closed_rejections = 1;
        return result;
    };

    if constexpr (K == 0 || K > two_stack_exact_max_kmer
                  || W == 0 || W > packed_paper_nthash_max_window) {
        return reject(PackedPaperNthashStatus::invalid_configuration);
    } else if constexpr (!std::same_as<
            typename traits::alphabet_type, seq::dna4>) {
        return reject(PackedPaperNthashStatus::unsupported_alphabet);
    } else if constexpr (!is_sequence_bit_castable_to<R, std::uint32_t>) {
        return reject(PackedPaperNthashStatus::unsupported_storage);
    } else if constexpr (sizeof(typename traits::storage_unit)
            < sizeof(std::uint32_t)
        || sizeof(typename traits::storage_unit)
            % sizeof(std::uint32_t) != 0) {
        return reject(PackedPaperNthashStatus::unsupported_storage);
    } else if constexpr (!std::constructible_from<
            MinimizerT, std::uint64_t, std::uint64_t, std::uint64_t>) {
        return reject(PackedPaperNthashStatus::unsupported_representation);
    } else if constexpr (!requires {
            MinimizerT::hash_field_mask;
            MinimizerT::pos_field_mask;
        }) {
        return reject(PackedPaperNthashStatus::unsupported_representation);
    } else if constexpr (static_cast<std::uint64_t>(
            MinimizerT::hash_field_mask) < 0xFFFFFFFFull) {
        return reject(PackedPaperNthashStatus::unsupported_representation);
    } else {
        const auto whole_begin = sequence.begin();
        const auto whole_end = sequence.end();
        if (begin < whole_begin || end < begin || whole_end < end) {
            return reject(PackedPaperNthashStatus::invalid_range);
        }
        constexpr std::size_t base_letters_per_unit =
            traits::letters_per_unit;
        const std::size_t begin_units = static_cast<std::size_t>(
            begin - whole_begin);
        const std::size_t end_units = static_cast<std::size_t>(
            end - whole_begin);
        const std::size_t local_begin_bp =
            begin_units * base_letters_per_unit;
        const std::size_t local_nominal_end_bp =
            end_units * base_letters_per_unit;
        const std::size_t local_end_bp = std::min(
            local_nominal_end_bp,
            static_cast<std::size_t>(sequence.bp_size()));
        const std::size_t local_bp_size = local_end_bp >= local_begin_bp
            ? local_end_bp - local_begin_bp
            : 0;
        if (local_bp_size < K + W - 1) {
            return reject(PackedPaperNthashStatus::input_too_short);
        }
        if (local_bp_size
            > static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return reject(PackedPaperNthashStatus::input_too_large);
        }
        if (local_bp_size - 1
            > static_cast<std::size_t>(MinimizerT::pos_field_mask)) {
            return reject(PackedPaperNthashStatus::unsupported_representation);
        }
        constexpr std::size_t u32_letters_per_unit = 16;
        const std::size_t local_view_units =
            (local_bp_size + u32_letters_per_unit - 1)
            / u32_letters_per_unit;
        const auto* local_words = reinterpret_cast<const std::uint32_t*>(
            sequence.read_cptr(begin));
        const auto* local_words_end = local_words + local_view_units;

        const packed_paper_nthash_internal::bounded_u32_dna4_view view(
            local_bp_size, local_view_units);
        using d_t = hn::CappedTag<std::uint32_t, 8>;
        const std::size_t lanes = hn::Lanes(d_t{});
        if (lanes != 4 && lanes != 8) {
            return reject(PackedPaperNthashStatus::unsupported_vector_shape);
        }

        result.proof.logical_streams = 8;
        result.proof.physical_vector_lanes = lanes;
        result.proof.vector_registers_per_step = 8 / lanes;
        result.proof.input_symbols = local_bp_size;
        result.proof.input_storage_units = end_units - begin_units;
        result.proof.segments_combined = 1;
        result.proof.positions_are_segment_local = true;
        result.proof.scheme_id = packed_paper_nthash_scheme_id;
        result.proof.kmer_size = static_cast<std::uint32_t>(K);
        result.proof.window_size = static_cast<std::uint32_t>(W);
        result.proof.nthash_rotation = 7;
        result.proof.source_storage_unit_bytes =
            sizeof(typename traits::storage_unit);
        static const auto* region_description =
            bioseq::instr::register_region_static(
                "mm_sketch_packed_paper_nthash_simd");
        auto region_scope = bioseq::instr::region(region_description);
        if (lanes == 4) {
            packed_paper_nthash_internal::run_packed_kernel<
                K, W, 2, MinimizerT>(
                    collector,
                    view,
                    local_words,
                    local_words_end,
                    result.proof);
        } else {
            packed_paper_nthash_internal::run_packed_kernel<
                K, W, 1, MinimizerT>(
                    collector,
                    view,
                    local_words,
                    local_words_end,
                    result.proof);
        }
        return result;
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END collect_minimizers_packed_paper_nthash_simd_checked

/** Whole-resident convenience overload; still performs the checked gate. */
template<
    std::size_t K = 15,
    std::size_t W = 10,
    class MinimizerT = MinimapSketchData,
    supports_compact_sequence_range R,
    class Collector = minimizer_sink_collector<MinimizerT>>
[[nodiscard]] inline PackedPaperNthashResult
collect_minimizers_packed_paper_nthash_simd(
    Collector& collector,
    compact_sequence_range<R>& sequence)
{
    return collect_minimizers_packed_paper_nthash_simd_checked<
        K, W, MinimizerT>(
            collector, sequence, sequence.begin(), sequence.end());
}

namespace coarse_paper_detail {

[[nodiscard]] constexpr MinimizerKernelStatus transport_status_for(
    const PackedPaperNthashStatus status) noexcept
{
    switch (status) {
        case PackedPaperNthashStatus::ok:
            return MinimizerKernelStatus::ok;
        case PackedPaperNthashStatus::invalid_range:
            return MinimizerKernelStatus::invalid_sequence_view;
        default:
            return MinimizerKernelStatus::invalid_configuration;
    }
}

template<class StorageUnit>
[[nodiscard]] bool run(
    MinimizerKernelInvocation& invocation,
    MinimizerKernelResult& result)
{
    return ::bioseq::minimizers::kernel_detail::
        with_borrowed_compact_range<seq::dna4, StorageUnit>(
            invocation.sequence,
            [&invocation, &result]<class Range>(Range& range) {
                minimizer_sink_collector<MinimapSketchData> collector{
                    invocation.sink};
                const PackedPaperNthashResult native =
                    collect_minimizers_packed_paper_nthash_simd_checked<
                        15u, 10u, MinimapSketchData>(
                            collector,
                            range,
                            range.begin(),
                            range.end());
                result.proof.paper = native.proof;
                result.family_status =
                    static_cast<std::uint32_t>(native.status);
                result.status = transport_status_for(native.status);

                const bool flushed = collector.flush();
                result.emitted = collector.committed_size();
                if (!flushed || !collector.ok()) {
                    result.status = MinimizerKernelStatus::sink_rejected;
                }
            });
}

} // namespace coarse_paper_detail

/** Source-topology-owned exact paper-ntHash case. */
template<sequence_alphabet Alphabet, class StorageUnit>
requires (
    (std::same_as<Alphabet, seq::dna4>
        || std::same_as<Alphabet, seq::dna5>)
    && (std::same_as<StorageUnit, std::uint8_t>
        || std::same_as<StorageUnit, std::uint16_t>
        || std::same_as<StorageUnit, std::uint32_t>
        || std::same_as<StorageUnit, std::uint64_t>))
[[nodiscard]] inline MinimizerKernelResult paper_nthash_case(
    MinimizerKernelInvocation& invocation)
{
    MinimizerKernelResult result{};
    result.proof.algorithm = MinimizerKernelAlgorithm::paper_nthash;
    result.proof.target = static_cast<std::int64_t>(HWY_TARGET);

    if constexpr (!std::same_as<Alphabet, seq::dna4>
                  || (!std::same_as<StorageUnit, std::uint32_t>
                      && !std::same_as<StorageUnit, std::uint64_t>)) {
        result.status = MinimizerKernelStatus::invalid_configuration;
        result.family_status = static_cast<std::uint32_t>(
            PackedPaperNthashStatus::invalid_configuration);
        return result;
    } else {
        if (!invocation.sequence.final_segment
            || invocation.fragments.present()
            || invocation.emitted_position_base != 0u
            || invocation.sequence.begin_symbol != 0u
            || invocation.window != 10u
            || invocation.kmer != 15u
            || invocation.sequence.end_symbol
                > static_cast<std::size_t>(
                    MinimapSketchData::pos_field_mask) + 1u) {
            result.status = MinimizerKernelStatus::invalid_configuration;
            result.family_status = static_cast<std::uint32_t>(
                PackedPaperNthashStatus::invalid_configuration);
            return result;
        }
        if (invocation.sequence.end_symbol
            < invocation.kmer + invocation.window - 1u) {
            result.status = MinimizerKernelStatus::invalid_configuration;
            result.family_status = static_cast<std::uint32_t>(
                PackedPaperNthashStatus::input_too_short);
            return result;
        }

        const bool accepted = coarse_paper_detail::run<StorageUnit>(
            invocation, result);
        if (!accepted && result.status == MinimizerKernelStatus::ok) {
            result.status = MinimizerKernelStatus::invalid_sequence_view;
            result.family_status = static_cast<std::uint32_t>(
                PackedPaperNthashStatus::invalid_range);
        }
        return result;
    }
}

} // namespace target
} // namespace bioseq::simd
