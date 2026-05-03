/**
 * @file simd_kernels.cppm
 * @brief Highway-bound SIMD primitives for the IO reader.
 *
 * Two responsibilities live here:
 *
 *   * **Scan** — `scan_for_first<NeedleN>` and `scan_for_each<NeedleN>`
 *     are the workhorses of every record FSM. The aligned-body loop
 *     is 4×-unrolled on AVX2 / NEON (single mask reduction decides for
 *     128 B per iter) and 2×-unrolled on AVX-512 (a 4× would exhaust
 *     the per-cycle fill buffer count).
 *
 *   * **dna4 encoder** — `encode_body_chunk_dna4_simd` resolves the
 *     ASCII → rank step via a 16-entry low-nibble LUT (spec §11.2)
 *     replicated across all 128-bit blocks via `LoadDup128`. The
 *     packing stage is fully SIMD: per-lane variable shift via `Shl`
 *     places each rank into its 2-bit slot, `SumsOf4` reduces every 4
 *     adjacent shifted bytes into one packed byte (since the shifted
 *     2-bit slots don't overlap, sum == bitwise-OR), and a fixed
 *     multiply + `ReduceSum` collapses 4 packed bytes into one
 *     `uint32_t`. v2 also processes two 16-byte chunks in parallel for
 *     instruction-level parallelism — independent loads / lookups /
 *     packs flow through separate execution ports without WAW stalls.
 *
 * Both halves wrap content in a *single* `HWY_BEFORE_NAMESPACE() …
 * HWY_AFTER_NAMESPACE()` block inside `namespace bioseq::io::simd::
 * HWY_NAMESPACE { … }`. Splitting them into two module interface
 * units triggers a Highway-internal mangled-name collision on
 * AArch64 (NEON `operator==(Vec128<uint8_t, 16>, …)` would be
 * instantiated in both PCMs and emitted twice when a downstream TU
 * imports both). Living in one module sidesteps the issue: the
 * Highway include + the per-target inline definitions appear in the
 * importer chain exactly once.
 *
 * Spec sections: §8.1-§8.5 (scan), §8.6 (LUT), §11.2 (4-bit collapse),
 * §17.2-§17.3 (unroll factor).
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

export module bioseq_io.simd_kernels;

import mlogging;
import sequan_compat;
import bioseq_io.encoder;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::io::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// =====================================================================
// Scan kernels
// =====================================================================

/// Build a Highway mask "byte equals any of `needles`". For NeedleN==1
/// the expression collapses to a single Eq; for N=2,3 the Eq results
/// are OR'd. Lifted out of the scan loops because the same expression
/// is reused at each unroll position.
template <std::size_t NeedleN, class D>
HWY_INLINE hn::Mask<D> any_needle_match(
    D d,
    const std::array<std::uint8_t, NeedleN>& needles,
    hn::Vec<D> chunk_bytes) noexcept {
    static_assert(NeedleN >= 1 && NeedleN <= 3,
                  "scan_bytes supports 1..3 needles");
    if constexpr (NeedleN == 1) {
        return hn::Eq(chunk_bytes, hn::Set(d, needles[0]));
    } else if constexpr (NeedleN == 2) {
        return hn::Or(hn::Eq(chunk_bytes, hn::Set(d, needles[0])),
                      hn::Eq(chunk_bytes, hn::Set(d, needles[1])));
    } else {
        return hn::Or(hn::Or(hn::Eq(chunk_bytes, hn::Set(d, needles[0])),
                             hn::Eq(chunk_bytes, hn::Set(d, needles[1]))),
                      hn::Eq(chunk_bytes, hn::Set(d, needles[2])));
    }
}

/// Per-target unroll factor for the aligned-body scan loop. AVX-512
/// lanes already process 64 B per chunk so a 2× unroll already
/// achieves the 128 B / iter target; AVX2 + NEON benefit from 4×
/// because each chunk is shorter (32 B / 16 B). See spec §17.2 / §17.3.
template <class D>
constexpr std::size_t scan_unroll_factor() noexcept {
    if constexpr (hn::MaxLanes(D{}) >= 64) return 2;
    return 4;
}

/// Find the first byte in `[data, data+size)` matching ANY of `needles`.
/// Returns `size` if absent. Spec §8.1, §8.2, §17.3.
template <std::size_t NeedleN, class D>
[[nodiscard]] HWY_INLINE std::size_t scan_for_first(
    D d,
    const std::array<std::uint8_t, NeedleN>& needles,
    const std::uint8_t* HWY_RESTRICT data,
    std::size_t size) noexcept {
    const std::size_t lanes = hn::Lanes(d);
    constexpr std::size_t unroll = scan_unroll_factor<D>();

    if (size < lanes) {
        for (std::size_t scan_offset = 0; scan_offset < size; ++scan_offset) {
            for (std::size_t needle_index = 0;
                 needle_index < NeedleN; ++needle_index) {
                if (data[scan_offset] == needles[needle_index]) {
                    return scan_offset;
                }
            }
        }
        return size;
    }

    std::size_t scan_cursor = 0;
    const std::size_t unrolled_step = unroll * lanes;

    while (scan_cursor + unrolled_step <= size) {
        if constexpr (unroll == 4) {
            const auto chunk_0 = hn::LoadU(d, data + scan_cursor + 0 * lanes);
            const auto chunk_1 = hn::LoadU(d, data + scan_cursor + 1 * lanes);
            const auto chunk_2 = hn::LoadU(d, data + scan_cursor + 2 * lanes);
            const auto chunk_3 = hn::LoadU(d, data + scan_cursor + 3 * lanes);
            const auto match_0 = any_needle_match<NeedleN>(d, needles, chunk_0);
            const auto match_1 = any_needle_match<NeedleN>(d, needles, chunk_1);
            const auto match_2 = any_needle_match<NeedleN>(d, needles, chunk_2);
            const auto match_3 = any_needle_match<NeedleN>(d, needles, chunk_3);
            const auto match_01 = hn::Or(match_0, match_1);
            const auto match_23 = hn::Or(match_2, match_3);
            const auto match_combined = hn::Or(match_01, match_23);
            if (!hn::AllFalse(d, match_combined)) {
                if (!hn::AllFalse(d, match_0)) {
                    return scan_cursor + 0 * lanes
                         + static_cast<std::size_t>(hn::FindFirstTrue(d, match_0));
                }
                if (!hn::AllFalse(d, match_1)) {
                    return scan_cursor + 1 * lanes
                         + static_cast<std::size_t>(hn::FindFirstTrue(d, match_1));
                }
                if (!hn::AllFalse(d, match_2)) {
                    return scan_cursor + 2 * lanes
                         + static_cast<std::size_t>(hn::FindFirstTrue(d, match_2));
                }
                return scan_cursor + 3 * lanes
                     + static_cast<std::size_t>(hn::FindFirstTrue(d, match_3));
            }
        } else {
            const auto chunk_0 = hn::LoadU(d, data + scan_cursor + 0 * lanes);
            const auto chunk_1 = hn::LoadU(d, data + scan_cursor + 1 * lanes);
            const auto match_0 = any_needle_match<NeedleN>(d, needles, chunk_0);
            const auto match_1 = any_needle_match<NeedleN>(d, needles, chunk_1);
            const auto match_combined = hn::Or(match_0, match_1);
            if (!hn::AllFalse(d, match_combined)) {
                if (!hn::AllFalse(d, match_0)) {
                    return scan_cursor + 0 * lanes
                         + static_cast<std::size_t>(hn::FindFirstTrue(d, match_0));
                }
                return scan_cursor + 1 * lanes
                     + static_cast<std::size_t>(hn::FindFirstTrue(d, match_1));
            }
        }
        scan_cursor += unrolled_step;
    }

    while (scan_cursor + lanes <= size) {
        const auto chunk_bytes = hn::LoadU(d, data + scan_cursor);
        const auto match_mask =
            any_needle_match<NeedleN>(d, needles, chunk_bytes);
        if (!hn::AllFalse(d, match_mask)) {
            return scan_cursor
                 + static_cast<std::size_t>(hn::FindFirstTrue(d, match_mask));
        }
        scan_cursor += lanes;
    }

    if (scan_cursor < size) {
        const std::size_t overlap_start = size - lanes;
        const auto chunk_bytes = hn::LoadU(d, data + overlap_start);
        const auto match_mask =
            any_needle_match<NeedleN>(d, needles, chunk_bytes);
        if (!hn::AllFalse(d, match_mask)) {
            const std::size_t already_searched = scan_cursor - overlap_start;
            const auto suppress_first_n = hn::FirstN(d, already_searched);
            const auto live_match = hn::AndNot(suppress_first_n, match_mask);
            if (!hn::AllFalse(d, live_match)) {
                return overlap_start
                     + static_cast<std::size_t>(hn::FindFirstTrue(d, live_match));
            }
        }
    }
    return size;
}

/// Iterate every offset in `[data, data+size)` whose byte matches any
/// needle, calling `visit(offset)` per match in ascending order.
/// Spec §8.4-§8.5.
template <std::size_t NeedleN, class D, typename Visit>
HWY_INLINE void scan_for_each(
    D d,
    const std::array<std::uint8_t, NeedleN>& needles,
    const std::uint8_t* HWY_RESTRICT data,
    std::size_t size,
    Visit&& visit) noexcept {
    const std::size_t lanes = hn::Lanes(d);
    if (size < lanes) {
        for (std::size_t scan_offset = 0; scan_offset < size; ++scan_offset) {
            for (std::size_t needle_index = 0;
                 needle_index < NeedleN; ++needle_index) {
                if (data[scan_offset] == needles[needle_index]) {
                    visit(scan_offset);
                    break;
                }
            }
        }
        return;
    }
    std::size_t scan_cursor = 0;
    while (scan_cursor + lanes <= size) {
        const auto chunk_bytes = hn::LoadU(d, data + scan_cursor);
        const auto match_mask =
            any_needle_match<NeedleN>(d, needles, chunk_bytes);
        if (!hn::AllFalse(d, match_mask)) {
            const std::uint64_t match_bitmask =
                hn::BitsFromMask(d, match_mask);
            std::uint64_t remaining_bits = match_bitmask;
            while (remaining_bits != 0) {
                const std::size_t lane_index = static_cast<std::size_t>(
                    std::countr_zero(remaining_bits));
                visit(scan_cursor + lane_index);
                remaining_bits &= remaining_bits - 1;
            }
        }
        scan_cursor += lanes;
    }
    for (std::size_t scan_offset = scan_cursor;
         scan_offset < size; ++scan_offset) {
        for (std::size_t needle_index = 0;
             needle_index < NeedleN; ++needle_index) {
            if (data[scan_offset] == needles[needle_index]) {
                visit(scan_offset);
                break;
            }
        }
    }
}

// =====================================================================
// dna4 SIMD encoder primitives
// =====================================================================

/// Vector LUT lookup: read `Lanes(d)` ASCII bytes, mask down to the
/// low nibble, look the rank up via the 16-entry replicated table.
/// Output is `Lanes(d)` ranks, with `0xFF` for newline / CR / unknown.
/// Spec §8.6 / §11.2 / Appendix G.
///
/// The `D` parameter is required for template deduction — Highway's
/// `Vec<D>` does not expose its tag as a deducible outer type, so the
/// caller passes the tag explicitly.
template <class D>
HWY_INLINE hn::Vec<D> dna4_lookup_ranks(
    D /* tag, deduction-only */,
    hn::Vec<D> letter_to_rank_table_vec,
    hn::Vec<D> low_nibble_mask,
    hn::Vec<D> chunk_bytes) noexcept {
    const auto letter_index = hn::And(chunk_bytes, low_nibble_mask);
    return hn::TableLookupBytes(letter_to_rank_table_vec, letter_index);
}

/// Pack 16 dna4 ranks into a 32-bit word, 2 bits per letter, lowest
/// letter at bit 0. The `ranks` vector MUST contain only values in
/// `[0, 3]`; the caller is responsible for screening out the `0xFF`
/// skip-sentinel before calling this. The pack pipeline is:
///
///   1. `Shl(ranks, [0,2,4,6, 0,2,4,6, ...])` — variable per-lane left
///      shift places each rank into its 2-bit slot within a byte. The
///      4 ranks within each 4-byte group land in disjoint slots so the
///      reduction in step 2 can use sum instead of OR (single-cycle on
///      every microarchitecture; OR on u32 lanes is equivalent but
///      `SumsOf4` already exists as a horizontal-reduction primitive).
///   2. `SumsOf4(shifted)` — per-block 4-lane horizontal reduction
///      yields 4 `uint32_t` lanes, each in `[0, 255]` representing one
///      packed byte (since the 4 shifted bytes have non-overlapping
///      bits, sum == bitwise OR).
///   3. `Mul + ReduceSum` with multiplier `[1, 256, 65536, 16777216]`
///      collapses the 4 packed bytes into one `uint32_t` whose layout
///      matches `compact_sequence_range`'s LSB-first invariant.
///
/// Total: 5 SIMD ops + 1 scalar reduction per 16 letters. Compare to
/// the v1 scalar fold, which executed 16 iterations of (load, branch,
/// shift, OR, increment) per chunk.
template <class D>
HWY_INLINE std::uint32_t pack_16_dna4_to_u32(
    D d_byte,
    hn::Vec<D> ranks) noexcept {
    HWY_ALIGN constexpr std::uint8_t shift_pattern[16] = {
        0, 2, 4, 6, 0, 2, 4, 6,
        0, 2, 4, 6, 0, 2, 4, 6};
    const auto shift_pattern_v = hn::LoadU(d_byte, shift_pattern);
    const auto shifted = hn::Shl(ranks, shift_pattern_v);

    using d_u32_t = hn::Repartition<std::uint32_t, D>;
    d_u32_t d_u32;
    const auto sums = hn::SumsOf4(shifted);

    HWY_ALIGN constexpr std::uint32_t multiplier_pattern[4] = {
        1u, 1u << 8, 1u << 16, 1u << 24};
    const auto multipliers = hn::LoadU(d_u32, multiplier_pattern);
    const auto multiplied = hn::Mul(sums, multipliers);
    return hn::ReduceSum(d_u32, multiplied);
}

/// Detect whether any rank in the 16-byte vector equals the skip
/// sentinel (`0xFF`). Returns `true` if the slow scalar fallback path
/// must run for this chunk. Compiles to a single `HWY` `Eq + AllFalse`
/// pair on every target.
template <class D>
HWY_INLINE bool has_skip_sentinel(
    D d_byte,
    hn::Vec<D> ranks) noexcept {
    const auto sentinel_mask = hn::Eq(ranks, hn::Set(d_byte, std::uint8_t{0xFF}));
    return !hn::AllFalse(d_byte, sentinel_mask);
}

/// Merge a freshly-packed 32-bit chunk into the partial-word
/// accumulator. `local_partial_letters` MUST be in `[0, 32)`. After
/// the merge, `local_partial_letters` is updated and a storage unit
/// may be pushed into `*packed_words`.
///
/// The shift arithmetic relies on the invariant that the 32-bit chunk
/// lives at bit positions `[2*partial_letters, 2*partial_letters + 32)`
/// of an imaginary 96-bit accumulator. When the new tail crosses bit
/// 64, the carry slides into the next storage unit.
template <typename storage_unit_t>
HWY_INLINE void merge_packed_chunk_into_partial(
    storage_unit_t& local_partial_word,
    std::size_t& local_partial_letters,
    std::vector<storage_unit_t>* HWY_RESTRICT packed_words,
    std::uint32_t packed_chunk) noexcept {
    const std::size_t shift_amount = 2 * local_partial_letters;
    local_partial_word |= static_cast<storage_unit_t>(packed_chunk) << shift_amount;
    const std::size_t new_letters = local_partial_letters + 16;
    if (new_letters >= 32) {
        packed_words->push_back(local_partial_word);
        // Leftover bits live at positions `[64-shift_amount, 32)` of the
        // 32-bit chunk; right-shift by `64 - shift_amount` brings them to
        // bit 0 of the next storage unit. For `shift_amount == 32` the
        // shift count is 32 — valid because we cast to uint64_t first
        // (uint32_t >> 32 would be UB).
        local_partial_word =
            (shift_amount > 32)
                ? (static_cast<storage_unit_t>(packed_chunk) >> (64 - shift_amount))
                : storage_unit_t{0};
        local_partial_letters = new_letters - 32;
    } else {
        local_partial_letters = new_letters;
    }
}

/// Encode a dna4 body span into the encoder state via the SIMD LUT
/// + SIMD pack pipeline. Stages:
///
///   1. Vector LoadU + low-nibble TableLookupBytes → 16 ranks per chunk.
///      Two chunks are processed per iteration so independent loads /
///      lookups / packs can flow through separate execution ports
///      (instruction-level parallelism). On NEON this pairs the 16-byte
///      pack with another 16-byte pack so the second chunk's load is
///      issued before the first chunk's pack finishes.
///   2. SIMD pack via `pack_16_dna4_to_u32` — 32 bits packed per chunk.
///   3. Merge into the encoder's partial-word accumulator. When the
///      accumulator crosses the 32-letter boundary, a `uint64_t`
///      storage unit is pushed onto `state.output_storage` (the
///      batch's shared sequence buffer).
///
/// The pack stage is fully SIMD; the only scalar work is the
/// partial-word arithmetic in `merge_packed_chunk_into_partial`, which
/// runs at most twice per 32 letters. The fallback path (any chunk
/// containing the `0xFF` skip sentinel from the LUT, e.g. `'N'` in a
/// dna4 stream) reverts to per-byte scalar folding for that one chunk.
///
/// The encoder writes through `state.output_storage`, which the FSM
/// configures to point at the batch's shared sequence vector. There
/// is no per-record allocation: the batch's vector grows
/// geometrically across the whole batch.
/// Spec §8.8 / §11.2 / §11.4 / Appendix G.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void encode_body_chunk_dna4_simd(
    bioseq::io::encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd is specialised for "
                  "dna4-into-uint64 packing (2 bits/letter, 32 letters/word)");

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before encode_body_chunk_dna4_simd");

    static constexpr auto fallback_lut =
        bioseq::io::build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto low_nibble_lut_array =
        bioseq::io::build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec = hn::LoadDup128(
        d_byte, low_nibble_lut_array.data());
    const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});

    // Hoist partial-word state into locals so the inner loop's
    // store-forwarding does not bottleneck on the encoder struct's
    // address. The writes back to `state` happen once at the end.
    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    auto* packed_words = state.output_storage;

    std::size_t read_cursor = 0;

    // Two-chunk unrolled fast path. Each chunk is 16 bytes / 32
    // packed bits; two chunks per iter is 32 bytes / 64 packed bits =
    // exactly one `uint64_t` storage unit when partial_letters == 0,
    // which is the common case after a previous iteration that
    // pushed a word.
    while (read_cursor + 2 * lanes <= body_size) {
        const auto chunk_a = hn::LoadU(d_byte, body_bytes + read_cursor);
        const auto chunk_b = hn::LoadU(d_byte, body_bytes + read_cursor + lanes);

        const auto low_a = hn::And(chunk_a, low_nibble_mask);
        const auto low_b = hn::And(chunk_b, low_nibble_mask);

        const auto ranks_a = hn::TableLookupBytes(letter_to_rank_table_vec, low_a);
        const auto ranks_b = hn::TableLookupBytes(letter_to_rank_table_vec, low_b);

        // Slow path: any chunk with the skip sentinel falls back to
        // per-byte scalar fold. The branch is taken only on `'N'` /
        // unknown bytes, which are absent from canonical dna4 corpora;
        // mispredicting once per sentinel is negligible.
        if (HWY_UNLIKELY(has_skip_sentinel(d_byte, ranks_a) ||
                         has_skip_sentinel(d_byte, ranks_b))) {
            for (std::size_t scalar_offset = 0;
                 scalar_offset < 2 * lanes; ++scalar_offset) {
                const std::uint8_t letter_rank =
                    fallback_lut[body_bytes[read_cursor + scalar_offset]];
                if (letter_rank != 0xFF) {
                    local_partial_word |=
                        static_cast<storage_unit_t>(letter_rank)
                        << (BitsPerChar * local_partial_letters);
                    local_partial_letters += 1;
                    local_total_letters += 1;
                    if (local_partial_letters == LettersPerUnit) {
                        packed_words->push_back(local_partial_word);
                        local_partial_word = 0;
                        local_partial_letters = 0;
                    }
                }
            }
            read_cursor += 2 * lanes;
            continue;
        }

        const std::uint32_t packed_a = pack_16_dna4_to_u32(d_byte, ranks_a);
        const std::uint32_t packed_b = pack_16_dna4_to_u32(d_byte, ranks_b);

        merge_packed_chunk_into_partial(local_partial_word,
                                        local_partial_letters,
                                        packed_words,
                                        packed_a);
        merge_packed_chunk_into_partial(local_partial_word,
                                        local_partial_letters,
                                        packed_words,
                                        packed_b);
        local_total_letters += 2 * lanes;
        read_cursor += 2 * lanes;
    }

    // Single-chunk path for the trailing 16-byte block.
    if (read_cursor + lanes <= body_size) {
        const auto chunk_bytes = hn::LoadU(d_byte, body_bytes + read_cursor);
        const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
        const auto letter_ranks = hn::TableLookupBytes(
            letter_to_rank_table_vec, low_nibbles);

        if (HWY_UNLIKELY(has_skip_sentinel(d_byte, letter_ranks))) {
            for (std::size_t scalar_offset = 0;
                 scalar_offset < lanes; ++scalar_offset) {
                const std::uint8_t letter_rank =
                    fallback_lut[body_bytes[read_cursor + scalar_offset]];
                if (letter_rank != 0xFF) {
                    local_partial_word |=
                        static_cast<storage_unit_t>(letter_rank)
                        << (BitsPerChar * local_partial_letters);
                    local_partial_letters += 1;
                    local_total_letters += 1;
                    if (local_partial_letters == LettersPerUnit) {
                        packed_words->push_back(local_partial_word);
                        local_partial_word = 0;
                        local_partial_letters = 0;
                    }
                }
            }
        } else {
            const std::uint32_t packed = pack_16_dna4_to_u32(d_byte, letter_ranks);
            merge_packed_chunk_into_partial(local_partial_word,
                                            local_partial_letters,
                                            packed_words,
                                            packed);
            local_total_letters += lanes;
        }
        read_cursor += lanes;
    }

    // Scalar tail for the final 0..15 bytes.
    while (read_cursor < body_size) {
        const std::uint8_t letter_rank = fallback_lut[body_bytes[read_cursor]];
        if (letter_rank != 0xFF) {
            local_partial_word |= static_cast<storage_unit_t>(letter_rank)
                                  << (BitsPerChar * local_partial_letters);
            local_partial_letters += 1;
            local_total_letters += 1;
            if (local_partial_letters == LettersPerUnit) {
                packed_words->push_back(local_partial_word);
                local_partial_word = 0;
                local_partial_letters = 0;
            }
        }
        read_cursor += 1;
    }

    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
}

}  // namespace bioseq::io::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

// =====================================================================
// Non-inline wrappers — entry points for non-Highway-aware modules.
// =====================================================================
//
// Modules like `bioseq_io.fasta` / `bioseq_io.fastq` need a "find next
// '\n'" primitive but do not include `<hwy/highway.h>` in their GMF
// (doing so re-emits Highway's per-target inline operator overloads
// into every importer's BMI and trips clang's "definition with same
// mangled name" check on AArch64). The wrappers below take plain
// `std::uint8_t` arguments and have their bodies emitted once in
// `simd_kernels.cppm.o`; importers see only the declaration.
//
// The wrappers are deliberately NOT inline. C++26 module interface
// units define non-inline functions exactly once — perfect for this
// "expose a Highway-backed primitive but hide the templates" use
// case.

namespace bioseq::io::simd {

/// Find the first byte in `[data, data+size)` equal to `needle`, or
/// `size` if absent. Spec §8.2.
[[nodiscard]] std::size_t scan_first_byte(
    std::uint8_t needle,
    const std::uint8_t* data,
    std::size_t size) noexcept;

/// Same as `scan_first_byte` but for two needles. Spec §8.3.
[[nodiscard]] std::size_t scan_first_byte_pair(
    std::uint8_t needle_0,
    std::uint8_t needle_1,
    const std::uint8_t* data,
    std::size_t size) noexcept;

/// dna4 SIMD encoder entry point — packs `body_size` ASCII bytes into
/// the encoder state's 32-letter `uint64_t` accumulator via the
/// vectorised low-nibble LUT. Body MUST NOT contain `\n` — the caller
/// strips line endings before passing the span. Spec §11.2 / Appendix G.
///
/// Specialised for the canonical `(dna4, uint64_t, 2 bits, 32 letters)`
/// shape. Other shapes fall back to the scalar encoder via
/// `bioseq_io.encoder_dispatch::encode_body_chunk`.
void dna4_simd_encode_body_chunk(
    bioseq::io::encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* body_bytes,
    std::size_t body_size) noexcept;

}  // namespace bioseq::io::simd

}  // export

// =====================================================================
// Wrapper definitions — module purview, non-inline. Lives in this TU
// only; no other BMI inherits Highway template instantiations.
// =====================================================================

namespace bioseq::io::simd {

std::size_t scan_first_byte(
    std::uint8_t needle,
    const std::uint8_t* data,
    std::size_t size) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint8_t> byte_tag;
    const std::array<std::uint8_t, 1> needles{needle};
    return HWY_NAMESPACE::scan_for_first<1>(byte_tag, needles, data, size);
}

std::size_t scan_first_byte_pair(
    std::uint8_t needle_0,
    std::uint8_t needle_1,
    const std::uint8_t* data,
    std::size_t size) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint8_t> byte_tag;
    const std::array<std::uint8_t, 2> needles{needle_0, needle_1};
    return HWY_NAMESPACE::scan_for_first<2>(byte_tag, needles, data, size);
}

void dna4_simd_encode_body_chunk(
    bioseq::io::encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* body_bytes,
    std::size_t body_size) noexcept {
    HWY_NAMESPACE::encode_body_chunk_dna4_simd<std::uint64_t, 2, 32>(
        state, body_bytes, body_size);
}

}  // namespace bioseq::io::simd
