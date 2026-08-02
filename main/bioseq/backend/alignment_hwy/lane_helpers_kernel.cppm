/**
 * @file lane_helpers.cppm
 * @brief Per-lane gather/load helpers for the inter-region batched KSW2
 *        kernel (DOC0254 §11.7).
 *
 * The batched KSW2 inner loop packs one fill per SIMD lane.  Each lane
 * therefore needs to load its own per-lane query/target byte from the
 * (sequence range, per-lane offset) tuple — a per-lane gather.  On
 * NEON 128-bit each gather expands to `Lanes × vld1_lane_s8`; on
 * AVX-512BW the same surface compiles to `_mm512_i32gather_epi8`.
 *
 * The helpers also include `lane_load_u32`, which loads a per-lane
 * 32-bit field out of the FillDescriptor array, and `lane_gather_score`,
 * which performs the per-lane scoring-matrix lookup.
 */
module;

#include <hwy/highway.h>

#include <cstdint>
#include <array>
#include <cstring>

export module bioseq.highway.sources.alignment_lanes;

import std;
import bioseq.sequences;
import bioseq.alignment.contracts;
import bioseq.highway.sources.alignment_utils;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * @brief Per-lane 32-bit gather helper.
 *
 * Loads one `int32_t` from each fill descriptor at the given member
 * offset.  Used to materialise the per-lane query/target lengths into
 * vector registers so the inner DP can mask-disable lanes past their
 * sequence end.
 *
 * The implementation walks `n_fills` per-lane elements via
 * `hn::InsertLane`, leaving the trailing `Lanes - n_fills` slots zeroed
 * so they remain inert (their per-lane length is 0 → vAlive mask is
 * false).
 *
 * @tparam D            Highway tag type for `int32_t` lanes.
 * @tparam FillT        FillDescriptor type (the kernel's per-fill POD).
 * @param  fills        Pointer to the per-fill descriptor array.
 * @param  offset_bytes Byte offset of the int32_t field within FillT.
 * @param  n_fills      Number of live lanes (≤ Lanes(d)).
 * @param  d            Highway tag instance.
 */
template <class D, class FillT>
[[gnu::always_inline]] inline hn::Vec<D> lane_load_int32(
    const FillT* fills,
    std::size_t  offset_bytes,
    std::size_t  n_fills,
    D            d)
{
    HWY_ALIGN std::int32_t scratch[hn::MaxLanes(D{})];
    const std::size_t lanes = hn::Lanes(d);
    for (std::size_t l = 0; l < lanes; ++l) {
        if (l < n_fills) {
            const auto* base = reinterpret_cast<const std::uint8_t*>(fills + l);
            std::int32_t value;
            std::memcpy(&value, base + offset_bytes, sizeof(std::int32_t));
            scratch[l] = value;
        } else {
            scratch[l] = 0;
        }
    }
    return hn::Load(d, scratch);
}

/**
 * @brief Per-lane gather of an int16_t field (e.g. bandwidth).
 *
 * Same shape as @ref lane_load_int32 but narrowed to int16_t lanes so
 * it can feed the 16-bit DP state without an explicit downcast.
 */
template <class D, class FillT>
[[gnu::always_inline]] inline hn::Vec<D> lane_load_int16(
    const FillT* fills,
    std::size_t  offset_bytes,
    std::size_t  n_fills,
    D            d)
{
    HWY_ALIGN std::int16_t scratch[hn::MaxLanes(D{})];
    const std::size_t lanes = hn::Lanes(d);
    for (std::size_t l = 0; l < lanes; ++l) {
        if (l < n_fills) {
            const auto* base = reinterpret_cast<const std::uint8_t*>(fills + l);
            std::int32_t value;
            std::memcpy(&value, base + offset_bytes, sizeof(std::int32_t));
            scratch[l] = static_cast<std::int16_t>(value);
        } else {
            scratch[l] = 0;
        }
    }
    return hn::Load(d, scratch);
}

/**
 * @brief Per-lane sequence byte gather.
 *
 * Walks each fill's `(offset_field, step)` tuple, samples the
 * sequence byte at `offset_field + step`, and packs the bytes into
 * an int16_t-lane vector.  Lanes past `n_fills` receive zero (a benign
 * value — the surrounding code already masks them off via the
 * per-lane alive predicate).
 *
 * The `Reverse` template parameter distinguishes forward- and
 * reverse-strand fills; for `Reverse == true` the kernel substitutes
 * `(target_len - 1 - step)` for the per-step index so the same
 * compact_sequence_range can serve both strand orientations without
 * the runtime branch shipped in today's `ksw2_align_pair<reverse>`.
 *
 * @tparam D        Highway tag (int16_t lanes).
 * @tparam Reverse  Strand orientation.
 * @tparam Range    `compact_sequence_range<R>` query/target type.
 * @tparam FillT    FillDescriptor type.
 */
template <class D, bool Reverse, class FillT, class Range>
[[gnu::always_inline]] inline hn::Vec<D> lane_gather_seq_byte(
    const FillT*                         fills,
    std::size_t                          offset_bytes,
    std::int32_t                         step,
    std::size_t                          n_fills,
    const compact_sequence_range<Range>& seq,
    D                                    d)
{
    HWY_ALIGN std::int16_t scratch[hn::MaxLanes(D{})];
    const std::size_t lanes = hn::Lanes(d);
    const std::size_t seq_len = seq.bp_size();
    for (std::size_t l = 0; l < lanes; ++l) {
        if (l < n_fills) {
            const auto* base = reinterpret_cast<const std::uint8_t*>(fills + l);
            std::int32_t fill_offset;
            std::memcpy(&fill_offset, base + offset_bytes, sizeof(std::int32_t));
            std::size_t pos;
            if constexpr (Reverse) {
                const std::int64_t reversed =
                    static_cast<std::int64_t>(seq_len) - 1 -
                    static_cast<std::int64_t>(fill_offset + step);
                pos = reversed >= 0
                    ? static_cast<std::size_t>(reversed)
                    : std::size_t{0};
            } else {
                pos = static_cast<std::size_t>(fill_offset + step);
            }
            if (pos < seq_len) {
                // Use the compact range's iter<BP>() to extract a single
                // sequence byte at the given position.
                auto it = seq.template iter<BP>(seq.begin());
                for (std::size_t k = 0; k < pos; ++k, ++it) {}
                scratch[l] = static_cast<std::int16_t>(*it);
            } else {
                scratch[l] = 0;
            }
        } else {
            scratch[l] = 0;
        }
    }
    return hn::Load(d, scratch);
}

/**
 * @brief Per-lane scoring-matrix lookup.
 *
 * For each lane `l`, computes `sm.score(vq[l], vt[l])` and packs the
 * results into an int16_t-lane vector.  The matrix is small (≤ 5×5
 * for dna4/dna5/dna4_with_n), so the per-lane scalar lookup is the
 * portable code path; on AVX-512BW the inner loop unrolls and packs
 * into the same int16_t vector lane width as the DP state.
 */
template <class D, sequence_alphabet Alphabet>
[[gnu::always_inline]] inline hn::Vec<D> lane_gather_score(
    const ScoringMatrix<Alphabet>& sm,
    hn::Vec<D>                  vq,
    hn::Vec<D>                  vt,
    D                           d)
{
    HWY_ALIGN std::int16_t q_lanes[hn::MaxLanes(D{})];
    HWY_ALIGN std::int16_t t_lanes[hn::MaxLanes(D{})];
    HWY_ALIGN std::int16_t s_lanes[hn::MaxLanes(D{})];
    hn::Store(vq, d, q_lanes);
    hn::Store(vt, d, t_lanes);

    constexpr std::size_t dim = ScoringMatrix<Alphabet>::alphabet_dim;
    const std::size_t lanes = hn::Lanes(d);
    for (std::size_t l = 0; l < lanes; ++l) {
        const auto qi = static_cast<int>(q_lanes[l]);
        const auto ti = static_cast<int>(t_lanes[l]);
        if (qi >= 0 && ti >= 0
            && static_cast<std::size_t>(qi) < dim
            && static_cast<std::size_t>(ti) < dim)
        {
            s_lanes[l] = static_cast<std::int16_t>(sm.at(qi, ti));
        } else {
            s_lanes[l] = 0;
        }
    }
    return hn::Load(d, s_lanes);
}

}  // namespace target
}  // namespace bioseq::simd
