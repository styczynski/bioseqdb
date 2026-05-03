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

#include <zlib.h>

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

// =====================================================================
// Multi-line dna4 encoder — newline-skip via SIMD compaction
// =====================================================================
//
// The single-line `encode_body_chunk_dna4_simd` is the hot path the
// FSM calls per body line: every chunk it sees is guaranteed
// newline-free, so the LUT never produces a skip-sentinel and the
// fallback path is dead code in steady state.
//
// For mmap-direct FASTA the FSM can do better: scan the body region
// for the next `>` once and pass the whole span (newlines included)
// to the encoder. This collapses per-line FSM overhead — for the
// 234-MiB human-genome corpus that's ~3 M iterations of state-machine
// dispatch + scan + per-line encode collapsed to 24 record-level
// dispatches.
//
// The challenge: the body span has `\n` and (rarely) `\r`, which the
// dna4 LUT maps to 0xFF skip-sentinels. The single-line encoder
// reverts to scalar fallback on any sentinel — fast for canonical
// dna4 (no sentinels in the input the FSM gives it) but ~20× slower
// per byte than SIMD when sentinels appear.
//
// The multi-line encoder keeps the SIMD path even when sentinels
// appear by branchless lane-compaction:
//
//   1. Look up ranks via the low-nibble LUT (newlines → 0xFF).
//   2. Compute the valid-lane mask (rank ≠ 0xFF).
//   3. Compact valid ranks to the front of the vector via a
//      precomputed shuffle table indexed by the 8-bit half-mask.
//   4. Pack the compacted ranks via the same `Shl + SumsOf4 + Mul +
//      ReduceSum` pipeline as the single-line path.
//   5. Update partial-word bookkeeping by `popcount(mask)` (the actual
//      letter count) instead of a fixed 16.
//
// The compaction tables (256 × 16 bytes each, two halves) are 8 KiB
// total — fits comfortably in M1 Pro's 192 KiB L1d. The fast path
// (chunk has no sentinels) is preserved via an `AllFalse` early-exit
// branch so a typical 80-char-wrap line that's wholly inside one
// chunk never pays the compaction cost.

/// Build the 256-entry low-half compaction table. Each entry is the
/// 16-byte shuffle pattern that takes a 16-byte input vector with
/// valid bytes at lanes [0..7] (per the mask bit positions) and
/// places them contiguously at output lanes [0..popcount-1]. Output
/// lanes [popcount..15] are filled with 0xFF, which `vqtbl1q_u8` /
/// `TableLookupBytes` resolves to 0 — exactly the rank that pack
/// expects for "no letter contributed here".
[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 256>
build_compact_low_table() {
    std::array<std::array<std::uint8_t, 16>, 256> table{};
    for (int mask = 0; mask < 256; ++mask) {
        std::array<std::uint8_t, 16> pattern{};
        std::size_t out_index = 0;
        for (int input_index = 0; input_index < 8; ++input_index) {
            if ((mask >> input_index) & 1) {
                pattern[out_index] = static_cast<std::uint8_t>(input_index);
                ++out_index;
            }
        }
        for (std::size_t fill = out_index; fill < 16; ++fill) {
            pattern[fill] = std::uint8_t{0xFF};
        }
        table[static_cast<std::size_t>(mask)] = pattern;
    }
    return table;
}

/// Build the 256-entry high-half compaction table. Same idea as the
/// low table but the input lanes are 8..15 (so the index pattern
/// references those positions). The output is still 16 bytes with
/// the popcount-prefix-valid layout.
[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 256>
build_compact_high_table() {
    std::array<std::array<std::uint8_t, 16>, 256> table{};
    for (int mask = 0; mask < 256; ++mask) {
        std::array<std::uint8_t, 16> pattern{};
        std::size_t out_index = 0;
        for (int input_index = 0; input_index < 8; ++input_index) {
            if ((mask >> input_index) & 1) {
                pattern[out_index] =
                    static_cast<std::uint8_t>(input_index + 8);
                ++out_index;
            }
        }
        for (std::size_t fill = out_index; fill < 16; ++fill) {
            pattern[fill] = std::uint8_t{0xFF};
        }
        table[static_cast<std::size_t>(mask)] = pattern;
    }
    return table;
}

/// Build the 9-entry "shift-left by N bytes" pattern table.
/// Entry N maps lane i to lane i-N if i >= N, else 0xFF
/// (out-of-bounds → 0). Used to align the compacted-high half so its
/// first byte sits at offset `popcount(low_mask)` of the result.
[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 9>
build_shift_left_byte_table() {
    std::array<std::array<std::uint8_t, 16>, 9> table{};
    for (std::size_t shift = 0; shift < 9; ++shift) {
        std::array<std::uint8_t, 16> pattern{};
        for (std::size_t lane = 0; lane < 16; ++lane) {
            pattern[lane] = (lane >= shift)
                ? static_cast<std::uint8_t>(lane - shift)
                : std::uint8_t{0xFF};
        }
        table[shift] = pattern;
    }
    return table;
}

inline constexpr auto kCompactLowTable = build_compact_low_table();
inline constexpr auto kCompactHighTable = build_compact_high_table();
inline constexpr auto kShiftLeftByteTable = build_shift_left_byte_table();

/// Variable-count merge of a packed chunk into the partial-word
/// accumulator. `count` is the number of valid 2-bit letter slots in
/// `packed_chunk` (the rest of `packed_chunk` is 0). Used by the
/// multi-line encoder where each chunk produces 0..16 valid letters.
template <typename storage_unit_t>
HWY_INLINE void merge_variable_chunk_into_partial(
    storage_unit_t& local_partial_word,
    std::size_t& local_partial_letters,
    std::vector<storage_unit_t>* HWY_RESTRICT packed_words,
    std::uint32_t packed_chunk,
    std::size_t count) noexcept {
    if (count == 0) return;
    const std::size_t shift_amount = 2 * local_partial_letters;
    local_partial_word |=
        static_cast<storage_unit_t>(packed_chunk) << shift_amount;
    const std::size_t new_total = local_partial_letters + count;
    if (new_total >= 32) {
        packed_words->push_back(local_partial_word);
        if (new_total > 32) {
            // Bits past position 64 of the imaginary 96-bit accumulator
            // become the next storage unit's low bits. Right-shift the
            // packed chunk by `64 - shift_amount` (which is in
            // [32, 62] when this branch is taken — `shift_amount > 0`
            // because partial_letters > 0 is required for overflow at
            // count <= 16).
            local_partial_word =
                static_cast<storage_unit_t>(packed_chunk) >>
                (64 - shift_amount);
            local_partial_letters = new_total - 32;
        } else {
            local_partial_word = 0;
            local_partial_letters = 0;
        }
    } else {
        local_partial_letters = new_total;
    }
}

/// Multi-line dna4 SIMD encoder. Encodes a body span that may contain
/// `\n` and `\r` — the LUT maps these to 0xFF and the SIMD compaction
/// branch strips them lane-wise. The encoder writes through
/// `state.output_storage` exactly like the single-line variant; only
/// the per-chunk processing differs.
///
/// Spec section §11.3 (SWAR pair-merge / multi-line); IO_V3 design.

/// Variant of `encode_body_chunk_dna4_simd_multiline` that ALSO
/// detects an arbitrary record-end sigil byte and stops encoding
/// at that position. Returns the number of bytes consumed — the FSM
/// uses this to advance its cursor and transition to the next state
/// when `consumed < body_size`. Folds what was previously a separate
/// `simd::scan_first_byte(boundary, ...)` pass + an
/// `encode_body_chunk_*` pass into a single body walk. Per chunk
/// this adds a single `Eq(chunk, boundary)` + `AllFalse` (~2 cycles);
/// the saved pass is a full memory walk (~14 ms on the 234 MiB
/// HUMAN cell). Net win on every cell that uses the multiline path.
/// Spec §11.3 / IO_V3.
///
/// Used for two boundary characters:
///   * `>` — FASTA record start
///   * `+` — FASTQ separator line start (within a record's seq region;
///           `+` is not a valid dna4 / dna5 letter so any `+` in the
///           seq region is unambiguously the separator)
///
/// `newline_count_out` (if non-null) accumulates the count of `\n`
/// and `\r` bytes encountered. Used by the FASTQ FSM to derive
/// `sequence_byte_count = consumed - newline_count` for qual length
/// validation; FASTA callers pass nullptr.
template <std::uint8_t BoundaryChar,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
[[nodiscard]] HWY_INLINE std::size_t
encode_body_chunk_dna4_simd_multiline_until_byte(
    bioseq::io::encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size,
    std::size_t* HWY_RESTRICT newline_count_out) {
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline_until_byte is "
                  "specialised for dna4-into-uint64 packing");

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    static constexpr auto fallback_lut =
        bioseq::io::build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto low_nibble_lut_array =
        bioseq::io::build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec =
        hn::LoadDup128(d_byte, low_nibble_lut_array.data());
    const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});
    const auto sentinel_marker = hn::Set(d_byte, std::uint8_t{0xFF});
    const auto boundary_marker = hn::Set(d_byte, BoundaryChar);
    const auto lf_marker = hn::Set(d_byte, std::uint8_t{'\n'});
    const auto cr_marker = hn::Set(d_byte, std::uint8_t{'\r'});

    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    std::size_t local_newline_count = 0;
    auto* packed_words = state.output_storage;
    constexpr bool track_newlines = true;  // always cheap; caller ignores via nullptr

    std::size_t read_cursor = 0;

    auto flush_state_and_return = [&](std::size_t consumed) {
        state.partial_word = local_partial_word;
        state.partial_letters = local_partial_letters;
        state.total_letters = local_total_letters;
        if (newline_count_out != nullptr) {
            *newline_count_out += local_newline_count;
        }
        return consumed;
    };

    while (read_cursor + lanes <= body_size) {
        const auto chunk_bytes =
            hn::LoadU(d_byte, body_bytes + read_cursor);

        // Per-chunk record-boundary detection. The `Eq(chunk, B)`
        // is a single SIMD compare; AllFalse is a single addv on
        // NEON. Together they add ~2 cycles per chunk vs the
        // sentinel-only multiline encoder.
        const auto boundary_eq = hn::Eq(chunk_bytes, boundary_marker);
        if (HWY_UNLIKELY(!hn::AllFalse(d_byte, boundary_eq))) {
            // Boundary byte found in this chunk — encode bytes before
            // it scalarly (rare path; only fires once per record),
            // then return.
            const std::size_t boundary_position = static_cast<std::size_t>(
                hn::FindFirstTrue(d_byte, boundary_eq));
            const std::size_t scalar_end =
                read_cursor + boundary_position;
            for (std::size_t scalar_offset = read_cursor;
                 scalar_offset < scalar_end; ++scalar_offset) {
                const std::uint8_t b = body_bytes[scalar_offset];
                if (b == '\n' || b == '\r') {
                    local_newline_count += 1;
                    continue;
                }
                const std::uint8_t rank = fallback_lut[b];
                if (rank != 0xFF) {
                    local_partial_word |=
                        static_cast<storage_unit_t>(rank)
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
            return flush_state_and_return(scalar_end);
        }

        const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
        const auto ranks =
            hn::TableLookupBytes(letter_to_rank_table_vec, low_nibbles);
        const auto sentinel_eq = hn::Eq(ranks, sentinel_marker);

        if (HWY_LIKELY(hn::AllFalse(d_byte, sentinel_eq))) {
            // Pure body chunk — same as single-line encoder fast path.
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(d_byte, ranks);
            merge_packed_chunk_into_partial(
                local_partial_word, local_partial_letters, packed_words,
                packed_chunk);
            local_total_letters += lanes;
            read_cursor += lanes;
            continue;
        }

        // Newline / CR present — compact valid lanes. Also count
        // newlines for the FASTQ FSM's byte-count derivation.
        const std::uint64_t bits =
            hn::BitsFromMask(d_byte, hn::Not(sentinel_eq));
        const std::uint8_t low_mask =
            static_cast<std::uint8_t>(bits & 0xFFu);
        const std::uint8_t high_mask =
            static_cast<std::uint8_t>((bits >> 8) & 0xFFu);
        const std::size_t low_count =
            static_cast<std::size_t>(std::popcount(low_mask));
        const std::size_t total_count = low_count +
            static_cast<std::size_t>(std::popcount(high_mask));
        if constexpr (track_newlines) {
            const auto lf_eq = hn::Eq(chunk_bytes, lf_marker);
            const auto cr_eq = hn::Eq(chunk_bytes, cr_marker);
            const std::uint64_t newline_bits =
                hn::BitsFromMask(d_byte, hn::Or(lf_eq, cr_eq));
            local_newline_count += static_cast<std::size_t>(
                std::popcount(newline_bits & 0xFFFFu));
        }

        if (total_count == 0) {
            read_cursor += lanes;
            continue;
        }

        const auto low_indices = hn::LoadU(
            d_byte, kCompactLowTable[low_mask].data());
        const auto high_indices = hn::LoadU(
            d_byte, kCompactHighTable[high_mask].data());
        const auto low_compacted =
            hn::TableLookupBytes(ranks, low_indices);
        const auto high_compacted =
            hn::TableLookupBytes(ranks, high_indices);
        const auto shift_indices = hn::LoadU(
            d_byte, kShiftLeftByteTable[low_count].data());
        const auto high_shifted =
            hn::TableLookupBytes(high_compacted, shift_indices);
        const auto compacted = hn::Or(low_compacted, high_shifted);

        const std::uint32_t packed_chunk =
            pack_16_dna4_to_u32(d_byte, compacted);
        merge_variable_chunk_into_partial(
            local_partial_word, local_partial_letters, packed_words,
            packed_chunk, total_count);
        local_total_letters += total_count;
        read_cursor += lanes;
    }

    // Scalar tail: process remaining 0..15 bytes byte-by-byte. If
    // we hit the boundary byte, stop and return its position.
    while (read_cursor < body_size) {
        const std::uint8_t current_byte = body_bytes[read_cursor];
        if (current_byte == BoundaryChar) {
            return flush_state_and_return(read_cursor);
        }
        if (current_byte == '\n' || current_byte == '\r') {
            local_newline_count += 1;
            read_cursor += 1;
            continue;
        }
        const std::uint8_t rank = fallback_lut[current_byte];
        if (rank != 0xFF) {
            local_partial_word |=
                static_cast<storage_unit_t>(rank)
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

    return flush_state_and_return(read_cursor);
}
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void encode_body_chunk_dna4_simd_multiline(
    bioseq::io::encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline is specialised for "
                  "dna4-into-uint64 packing (2 bits/letter, 32 letters/word)");

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    static constexpr auto fallback_lut =
        bioseq::io::build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto low_nibble_lut_array =
        bioseq::io::build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec =
        hn::LoadDup128(d_byte, low_nibble_lut_array.data());
    const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});
    const auto sentinel_marker = hn::Set(d_byte, std::uint8_t{0xFF});

    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    auto* packed_words = state.output_storage;

    // Single-chunk loop. Empirically the 2-chunk unrolled variant of
    // this loop regressed the canonical 80-char-wrap FASTA cells by
    // 8-13% — clang generated extra spills around the 2× compaction
    // bookkeeping vars in the slow path even with no lambda. The
    // single-chunk shape lets the compiler keep the partial-word
    // accumulator pinned to a register across iterations.
    std::size_t read_cursor = 0;
    while (read_cursor + lanes <= body_size) {
        const auto chunk_bytes =
            hn::LoadU(d_byte, body_bytes + read_cursor);
        const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
        const auto ranks =
            hn::TableLookupBytes(letter_to_rank_table_vec, low_nibbles);
        const auto sentinel_eq = hn::Eq(ranks, sentinel_marker);

        if (HWY_LIKELY(hn::AllFalse(d_byte, sentinel_eq))) {
            // Fast path: chunk is wholly inside a body line — no
            // newlines, no CR. Identical to the single-line encoder
            // hot path: pack 16 ranks → uint32, merge into partial.
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(d_byte, ranks);
            merge_packed_chunk_into_partial(
                local_partial_word, local_partial_letters, packed_words,
                packed_chunk);
            local_total_letters += lanes;
            read_cursor += lanes;
            continue;
        }

        // Slow path: chunk straddles a line boundary. Compact the
        // valid lanes via the precomputed shuffle tables, then pack.
        const std::uint64_t bits =
            hn::BitsFromMask(d_byte, hn::Not(sentinel_eq));
        const std::uint8_t low_mask =
            static_cast<std::uint8_t>(bits & 0xFFu);
        const std::uint8_t high_mask =
            static_cast<std::uint8_t>((bits >> 8) & 0xFFu);
        const std::size_t low_count =
            static_cast<std::size_t>(std::popcount(low_mask));
        const std::size_t high_count =
            static_cast<std::size_t>(std::popcount(high_mask));
        const std::size_t total_count = low_count + high_count;

        if (total_count == 0) {
            read_cursor += lanes;
            continue;
        }

        const auto low_indices = hn::LoadU(
            d_byte, kCompactLowTable[low_mask].data());
        const auto high_indices = hn::LoadU(
            d_byte, kCompactHighTable[high_mask].data());
        const auto low_compacted =
            hn::TableLookupBytes(ranks, low_indices);
        const auto high_compacted =
            hn::TableLookupBytes(ranks, high_indices);
        const auto shift_indices = hn::LoadU(
            d_byte, kShiftLeftByteTable[low_count].data());
        const auto high_shifted =
            hn::TableLookupBytes(high_compacted, shift_indices);
        const auto compacted = hn::Or(low_compacted, high_shifted);

        const std::uint32_t packed_chunk =
            pack_16_dna4_to_u32(d_byte, compacted);
        merge_variable_chunk_into_partial(
            local_partial_word, local_partial_letters, packed_words,
            packed_chunk, total_count);
        local_total_letters += total_count;
        read_cursor += lanes;
    }

    // Scalar tail for the trailing 0..15 bytes. Reuses the fallback
    // LUT so we treat newlines / CR as skip-sentinels here too.
    while (read_cursor < body_size) {
        const std::uint8_t letter_rank =
            fallback_lut[body_bytes[read_cursor]];
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
        read_cursor += 1;
    }

    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
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

// =====================================================================
// dna4 SIMD decoder — packed uint64 → ASCII
// =====================================================================
//
// Inverse of `pack_16_dna4_to_u32`: takes packed-rank words (2 bits per
// letter, 32 letters per uint64) and emits ASCII bytes (`A`/`C`/`G`/`T`).
// Used by the FASTA / FASTQ writer to materialise the on-disk body.
//
// Per word the decoder needs to:
//   1. Spread the 32 ranks (2 bits each) across 32 byte lanes — i.e.
//      take 8 source bytes and emit 32 destination bytes, where output
//      byte 4*b + r holds (input_byte[b] >> (2*r)) & 0x03.
//   2. Map each rank in [0..3] to its ASCII letter via a 4-entry LUT
//      replicated to fill the 16-byte tag (ranks 4..15 are unreachable
//      so the table's tail bytes are don't-care).
//
// The first stage is the interesting one. We use four `Shr` + `And`
// operations per 16-byte half (= 64 letters per call), then interleave
// four 16-byte vectors into 64 contiguous bytes via two
// `InterleaveLower`/`InterleaveUpper` rounds. On NEON (16-byte tag) the
// pipeline is ~10 SIMD ops per 64 ASCII bytes — bandwidth-bound on the
// dst stream rather than compute-bound.
//
// The decoder is specialised for the canonical (dna4, uint64_t, 2 bits,
// 32 letters/word) shape that the reader produces. Other shapes go
// through the scalar path.

/// Build the 16-entry rank → ASCII table for dna4 — indexed by the
/// 4 ranks defined in `seqan3::dna4` (A=0, C=1, G=2, T=3); the trailing
/// 12 entries are 0 (unreachable when input ranks are well-formed).
[[nodiscard]] consteval std::array<std::uint8_t, 16>
build_dna4_rank_to_ascii_lut() {
    std::array<std::uint8_t, 16> lut{};
    lut[0] = static_cast<std::uint8_t>('A');
    lut[1] = static_cast<std::uint8_t>('C');
    lut[2] = static_cast<std::uint8_t>('G');
    lut[3] = static_cast<std::uint8_t>('T');
    return lut;
}

/// Decode 16 packed dna4 source bytes (= 2 packed words = 64 letters)
/// into 64 ASCII bytes via SIMD spread + LUT.
///
/// Strategy: for each input byte b, we emit four output bytes at
/// indices 4b, 4b+1, 4b+2, 4b+3 holding ranks
///   (b >> 0) & 3, (b >> 2) & 3, (b >> 4) & 3, (b >> 6) & 3.
///
/// We compute four 16-byte vectors `r0` (low bits), `r1`, `r2`, `r3`
/// (high bits) by per-lane right-shift + mask. Interleaving them
/// pairwise with `InterleaveLower`/`InterleaveUpper` weaves the bytes
/// into the desired output order (output byte 4b+0 in lane 0 of low
/// half, output byte 4b+1 in lane 1, etc.).
///
/// Then a single `TableLookupBytes` against the broadcast 16-entry
/// rank-to-ASCII LUT converts the 16 ranks per chunk into ASCII.
template <class D>
HWY_INLINE void unpack_16_bytes_to_64_ascii(
    D d_byte,
    hn::Vec<D> packed_bytes,
    hn::Vec<D> rank_to_ascii_lut,
    std::uint8_t* HWY_RESTRICT dst) noexcept {
    const auto mask_low_2 = hn::Set(d_byte, std::uint8_t{0x03});

    // r0..r3: per-lane ranks at positions 0, 1, 2, 3 within each input
    // byte. Output byte 4b+r holds rank r of input byte b.
    const auto r0 = hn::And(packed_bytes, mask_low_2);
    const auto r1 = hn::And(hn::ShiftRight<2>(packed_bytes), mask_low_2);
    const auto r2 = hn::And(hn::ShiftRight<4>(packed_bytes), mask_low_2);
    const auto r3 = hn::And(hn::ShiftRight<6>(packed_bytes), mask_low_2);

    // Interleave (r0, r1) lane-wise and (r2, r3) lane-wise:
    //   InterleaveLower(r0, r1) = [r0[0], r1[0], r0[1], r1[1], ..., r0[7], r1[7]]
    // Then interleave the (r0,r1) and (r2,r3) results pairwise on the
    // 16-bit-grouped view to produce
    //   [r0[0], r1[0], r2[0], r3[0], r0[1], r1[1], r2[1], r3[1], ...]
    using d_u16_t = hn::Repartition<std::uint16_t, D>;
    d_u16_t d_u16;

    const auto r01_lo = hn::InterleaveLower(d_byte, r0, r1);
    const auto r01_hi = hn::InterleaveUpper(d_byte, r0, r1);
    const auto r23_lo = hn::InterleaveLower(d_byte, r2, r3);
    const auto r23_hi = hn::InterleaveUpper(d_byte, r2, r3);

    const auto chunk0 = hn::BitCast(
        d_byte,
        hn::InterleaveLower(d_u16,
                             hn::BitCast(d_u16, r01_lo),
                             hn::BitCast(d_u16, r23_lo)));
    const auto chunk1 = hn::BitCast(
        d_byte,
        hn::InterleaveUpper(d_u16,
                             hn::BitCast(d_u16, r01_lo),
                             hn::BitCast(d_u16, r23_lo)));
    const auto chunk2 = hn::BitCast(
        d_byte,
        hn::InterleaveLower(d_u16,
                             hn::BitCast(d_u16, r01_hi),
                             hn::BitCast(d_u16, r23_hi)));
    const auto chunk3 = hn::BitCast(
        d_byte,
        hn::InterleaveUpper(d_u16,
                             hn::BitCast(d_u16, r01_hi),
                             hn::BitCast(d_u16, r23_hi)));

    const auto ascii0 = hn::TableLookupBytes(rank_to_ascii_lut, chunk0);
    const auto ascii1 = hn::TableLookupBytes(rank_to_ascii_lut, chunk1);
    const auto ascii2 = hn::TableLookupBytes(rank_to_ascii_lut, chunk2);
    const auto ascii3 = hn::TableLookupBytes(rank_to_ascii_lut, chunk3);

    constexpr std::size_t lanes_per_chunk = 16;
    hn::StoreU(ascii0, d_byte, dst + 0 * lanes_per_chunk);
    hn::StoreU(ascii1, d_byte, dst + 1 * lanes_per_chunk);
    hn::StoreU(ascii2, d_byte, dst + 2 * lanes_per_chunk);
    hn::StoreU(ascii3, d_byte, dst + 3 * lanes_per_chunk);
}

/// Decode `total_letters` letters from packed dna4 words into `dst`.
/// `words` points to ⌈total_letters/32⌉ uint64 packed words; `dst`
/// MUST have at least `total_letters` byte capacity.
///
/// The function processes whole words (16 packed bytes = 64 letters)
/// per SIMD iteration; the trailing partial word is handled scalarly.
HWY_INLINE void decode_dna4_words_to_ascii(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;

    static constexpr auto rank_to_ascii_lut_array =
        build_dna4_rank_to_ascii_lut();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        d_byte, rank_to_ascii_lut_array.data());

    // Process 64 letters (= 2 packed words = 16 packed bytes) per iter.
    constexpr std::size_t letters_per_iter = 64;
    constexpr std::size_t bytes_per_iter = 16;

    std::size_t letters_emitted = 0;
    std::size_t word_cursor = 0;
    const auto* packed_bytes_base =
        reinterpret_cast<const std::uint8_t*>(words);

    while (letters_emitted + letters_per_iter <= total_letters) {
        const auto packed_bytes = hn::LoadU(
            d_byte, packed_bytes_base + word_cursor * 8);
        unpack_16_bytes_to_64_ascii(d_byte, packed_bytes,
                                     rank_to_ascii_lut, dst + letters_emitted);
        letters_emitted += letters_per_iter;
        word_cursor += 2;
        (void)bytes_per_iter;  // unused except for documentation
    }

    // Trailing partial: 0..63 letters remain. Process scalar per byte
    // — this is the rare path (at most ~63 letters per record).
    while (letters_emitted < total_letters) {
        const std::size_t word_index = letters_emitted / 32;
        const std::size_t letter_in_word = letters_emitted % 32;
        const std::uint64_t word = words[word_index];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (2 * letter_in_word)) & 0x3u);
        dst[letters_emitted] = rank_to_ascii_lut_array[rank];
        ++letters_emitted;
    }
}

/// Variant of `decode_dna4_words_to_ascii` that injects newline bytes
/// every `wrap_width` letters. The decoded body is laid out as
///   <wrap_width letters> '\n' <wrap_width letters> '\n' ... <tail letters>
/// with no trailing newline (the caller adds the terminating `\n`).
///
/// `letters_in_current_line` is an in/out cursor: on entry, the count
/// of letters already emitted into the current line (so callers can
/// resume mid-line across multiple decode batches); on exit, the new
/// count (modulo wrap_width).
///
/// Strategy: decode into a thread-local scratch (the caller's `scratch`
/// buffer of size 64 + wrap_width to handle stride underflow), then
/// memcpy from scratch to `dst` in `wrap_width`-sized strides separated
/// by `\n`. Two-pass over the body bytes is unavoidable when wrap_width
/// is not a SIMD-friendly multiple; we accept the ~2× memory traffic
/// because the destination cache line is hot anyway (consecutive
/// bytes), and the L2 hit rate is high.
HWY_INLINE std::size_t decode_dna4_words_to_ascii_wrapped(
    const std::uint64_t* HWY_RESTRICT words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* HWY_RESTRICT scratch,
    std::uint8_t* HWY_RESTRICT dst) noexcept {
    if (total_letters == 0) return letters_in_current_line;
    decode_dna4_words_to_ascii(words, scratch, total_letters);
    std::size_t in_cursor = 0;
    std::size_t out_cursor = 0;
    std::size_t line_position = letters_in_current_line;
    while (in_cursor < total_letters) {
        const std::size_t room_in_line = wrap_width - line_position;
        const std::size_t this_run = std::min(room_in_line,
                                                total_letters - in_cursor);
        std::memcpy(dst + out_cursor, scratch + in_cursor, this_run);
        out_cursor += this_run;
        in_cursor += this_run;
        line_position += this_run;
        if (line_position == wrap_width) {
            dst[out_cursor++] = static_cast<std::uint8_t>('\n');
            line_position = 0;
        }
    }
    return line_position;
}

// =====================================================================
// Compression-pipeline kernels — used by parallel_gzip_decoder.
// All inside the existing HWY_BEFORE_NAMESPACE() block; no new HWY-
// wrapped module per CODE_STYLE.md.
// =====================================================================

/// Marker resolution — for each `uint16_t` value `v` in `buffer`, if
/// `v < 0x8000` keep low byte; else replace low byte with
/// `window[v - 0x8000]`. Spec §5.1.
template <class D_U16>
HWY_INLINE void replace_markers_inplace_one_chunk(
    D_U16               d_u16,
    std::uint16_t*      HWY_RESTRICT buffer,
    std::size_t                      buffer_size,
    const std::uint8_t* HWY_RESTRICT window /* exactly 32 KiB */) noexcept {
    const std::size_t lanes              = hn::Lanes(d_u16);
    const auto        v_marker_threshold = hn::Set(d_u16,
                                                    std::uint16_t{0x8000});

    std::size_t cursor = 0;
    for (; cursor + lanes <= buffer_size; cursor += lanes) {
        const auto v_input     = hn::LoadU(d_u16, buffer + cursor);
        const auto v_is_marker = hn::Ge(v_input, v_marker_threshold);
        if (HWY_LIKELY(hn::AllFalse(d_u16, v_is_marker))) continue;

        HWY_ALIGN std::array<std::uint16_t, hn::MaxLanes(d_u16)> inputs{};
        HWY_ALIGN std::array<std::uint16_t, hn::MaxLanes(d_u16)> offsets{};
        hn::StoreU(v_input,                              d_u16, inputs.data());
        hn::StoreU(hn::Sub(v_input, v_marker_threshold), d_u16, offsets.data());

        for (std::size_t lane = 0; lane < lanes; ++lane) {
            const std::uint16_t value = inputs[lane];
            if (value >= 0x8000) {
                buffer[cursor + lane] = static_cast<std::uint16_t>(
                    window[offsets[lane]]);
            }
        }
    }
    for (; cursor < buffer_size; ++cursor) {
        const std::uint16_t value = buffer[cursor];
        if (value >= 0x8000) {
            buffer[cursor] = static_cast<std::uint16_t>(
                window[value - 0x8000]);
        }
    }
}

/// `dst[i] = static_cast<uint8_t>(src[i])` — drop the high byte of
/// each `uint16_t`. 4×-unrolled `DemoteTo` per main-loop iteration.
/// Spec §5.2.
///
/// `Rebind<u8, D_U16>` keeps the same lane count as the u16 tag (so
/// `DemoteTo` produces a vector with the same lane count, just half
/// the byte width), letting `StoreU` write `lanes_u16` contiguous
/// bytes in one op. (`Repartition` would double the lane count to
/// fill the same byte-width register, which doesn't match what
/// `DemoteTo` produces from a single u16 vector.)
template <class D_U16>
HWY_INLINE void narrow_lower_bytes_to_dst_inner(
    D_U16                              d_u16,
    const std::uint16_t* HWY_RESTRICT  src,
    std::uint8_t*        HWY_RESTRICT  dst,
    std::size_t                        count) noexcept {
    const hn::Rebind<std::uint8_t, D_U16> d_u8_half;
    const std::size_t lanes_u16 = hn::Lanes(d_u16);

    std::size_t cursor = 0;
    for (; cursor + 4 * lanes_u16 <= count; cursor += 4 * lanes_u16) {
        const auto v0 = hn::LoadU(d_u16, src + cursor + 0 * lanes_u16);
        const auto v1 = hn::LoadU(d_u16, src + cursor + 1 * lanes_u16);
        const auto v2 = hn::LoadU(d_u16, src + cursor + 2 * lanes_u16);
        const auto v3 = hn::LoadU(d_u16, src + cursor + 3 * lanes_u16);
        hn::StoreU(hn::DemoteTo(d_u8_half, v0), d_u8_half,
                   dst + cursor + 0 * lanes_u16);
        hn::StoreU(hn::DemoteTo(d_u8_half, v1), d_u8_half,
                   dst + cursor + 1 * lanes_u16);
        hn::StoreU(hn::DemoteTo(d_u8_half, v2), d_u8_half,
                   dst + cursor + 2 * lanes_u16);
        hn::StoreU(hn::DemoteTo(d_u8_half, v3), d_u8_half,
                   dst + cursor + 3 * lanes_u16);
    }
    for (; cursor + lanes_u16 <= count; cursor += lanes_u16) {
        const auto v_chunk = hn::LoadU(d_u16, src + cursor);
        hn::StoreU(hn::DemoteTo(d_u8_half, v_chunk), d_u8_half,
                   dst + cursor);
    }
    for (; cursor < count; ++cursor) {
        dst[cursor] = static_cast<std::uint8_t>(src[cursor]);
    }
}

// ---------------------------------------------------------------------
// Huffman fast loop — decode `kHuffmanFastLoopUnroll` literal-only
// symbols per call from a shared bit buffer. Caller refills first.
// Spec §5.3.
// ---------------------------------------------------------------------

inline constexpr std::size_t kHuffmanFastLoopUnroll = 3;

struct huffman_fast_loop_result_inner {
    std::array<std::uint16_t, kHuffmanFastLoopUnroll> symbols{};
    std::size_t                                       symbols_emitted = 0;
    std::size_t                                       bits_consumed   = 0;
};

[[nodiscard]] HWY_INLINE huffman_fast_loop_result_inner
huffman_fast_loop_inner(
    std::uint64_t        bit_buffer,
    const std::uint16_t* HWY_RESTRICT primary_table /* size = 1<<11 */) noexcept {
    constexpr std::uint64_t kPrimaryMask = (std::uint64_t{1} << 11) - 1;
    huffman_fast_loop_result_inner result{};
    std::size_t consumed = 0;
    for (std::size_t lane = 0; lane < kHuffmanFastLoopUnroll; ++lane) {
        const std::uint64_t peek  = (bit_buffer >> consumed) & kPrimaryMask;
        const std::uint16_t entry = primary_table[peek];
        const std::uint8_t  code_length =
            static_cast<std::uint8_t>(entry & 0x0Fu);
        const std::uint16_t symbol      =
            static_cast<std::uint16_t>(entry >> 4);
        if (code_length == 0 || symbol >= 256) [[unlikely]] break;
        result.symbols[lane] = symbol;
        consumed            += code_length;
        ++result.symbols_emitted;
    }
    result.bits_consumed = consumed;
    return result;
}

HWY_INLINE void huffman_fast_loop_store_inner(
    const huffman_fast_loop_result_inner& result,
    std::uint16_t* HWY_RESTRICT           window_cursor) noexcept {
    for (std::size_t lane = 0; lane < result.symbols_emitted; ++lane) {
        window_cursor[lane] = result.symbols[lane];
    }
}

// ---------------------------------------------------------------------
// CRC-32/ISO-HDLC reflected-mode Highway kernel. Spec §5.4.
//
// Implementation ported from `crc-fast-rust-main/src/algorithm.rs:200-274`
// + `src/crc32/width32_ops.rs:48-184`. `cmul_*` helpers map upstream's
// `cmul_00 / cmul_11 / cmul_10 / cmul_01` to Highway's `CLMulLower /
// CLMulUpper / CLMulLower(Shuffle01) / CLMulLower(Shuffle01)`.
// ---------------------------------------------------------------------

template <class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> cmul_low_low(
    D /*d*/, hn::Vec<D> a, hn::Vec<D> b) noexcept {
    return hn::CLMulLower(a, b);
}
template <class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> cmul_high_high(
    D /*d*/, hn::Vec<D> a, hn::Vec<D> b) noexcept {
    return hn::CLMulUpper(a, b);
}
template <class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> cmul_low_high(
    D /*d*/, hn::Vec<D> a, hn::Vec<D> b) noexcept {
    return hn::CLMulLower(a, hn::Shuffle01(b));
}
template <class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> cmul_high_low(
    D /*d*/, hn::Vec<D> a, hn::Vec<D> b) noexcept {
    return hn::CLMulLower(hn::Shuffle01(a), b);
}

/// Build a coefficient vector. `high_lane` lands in lane 1; `low_lane`
/// in lane 0.
template <class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> make_clmul_coeff(
    D d, std::uint64_t high_lane, std::uint64_t low_lane) noexcept {
    HWY_ALIGN std::array<std::uint64_t, 2> lanes{low_lane, high_lane};
    return hn::Load(d, lanes.data());
}

/// Reflected fold-16. Mirrors `width32_ops.rs:48-69`.
template <class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> crc32_fold_16_reflected(
    D d, hn::Vec<D> state, hn::Vec<D> coeff, hn::Vec<D> data_to_xor) noexcept {
    const auto h = cmul_low_high(d, state, coeff);
    const auto l = cmul_high_low(d, state, coeff);
    return hn::Xor(hn::Xor(h, l), data_to_xor);
}

template <class D>
[[nodiscard]] HWY_INLINE std::array<hn::Vec<D>, 7>
crc32_collapse_coefficients(
    D d, const std::array<std::uint64_t, 23>& keys) noexcept {
    return {
        make_clmul_coeff(d, keys[10], keys[9]),
        make_clmul_coeff(d, keys[12], keys[11]),
        make_clmul_coeff(d, keys[14], keys[13]),
        make_clmul_coeff(d, keys[16], keys[15]),
        make_clmul_coeff(d, keys[18], keys[17]),
        make_clmul_coeff(d, keys[20], keys[19]),
        make_clmul_coeff(d, keys[2],  keys[1]),
    };
}

template <class D>
[[nodiscard]] HWY_INLINE std::uint32_t crc32_final_reduction_reflected(
    D                                    d,
    hn::Vec<D>                           state,
    const std::array<std::uint64_t, 23>& keys) noexcept {
    const auto coeff_low  = make_clmul_coeff(d, 0,       keys[5]);
    const auto coeff_high = make_clmul_coeff(d, keys[6], 0);
    state = hn::Xor(cmul_low_low(d, state, coeff_low),
                    hn::ShiftRightBytes<8>(d, state));

    HWY_ALIGN std::array<std::uint64_t, 2> mask2_lanes{
        0xFFFFFFFF00000000ULL, 0xFFFFFFFFFFFFFFFFULL};
    const auto mask2   = hn::Load(d, mask2_lanes.data());
    const auto masked  = hn::And(state, mask2);
    const auto shifted = hn::ShiftLeftBytes<12>(d, state);
    const auto folded  = cmul_high_high(d, shifted, coeff_high);
    state = hn::Xor(folded, masked);

    const auto mu_poly  = make_clmul_coeff(d, keys[8], keys[7]);
    const auto br_step1 = cmul_low_low (d, state,    mu_poly);
    const auto br_step2 = cmul_low_high(d, br_step1, mu_poly);
    const auto reduced  = hn::Xor(state, br_step2);

    HWY_ALIGN std::array<std::uint64_t, 2> out_lanes{};
    hn::Store(reduced, d, out_lanes.data());
    return static_cast<std::uint32_t>(out_lanes[1]);
}

/// 128-byte SIMD-aligned chunk processor. Mirrors `algorithm.rs:200-274`.
template <class D>
HWY_INLINE void crc32_process_simd_chunks_reflected(
    D                                    d,
    hn::Vec<D>&                          running_state,
    const std::uint8_t*                  HWY_RESTRICT first_128,
    const std::uint8_t*                  HWY_RESTRICT rest_bytes,
    std::size_t                          rest_chunks_of_128,
    const std::array<std::uint64_t, 23>& keys) noexcept {
    const hn::Repartition<std::uint8_t, D> d_b;
    std::array<hn::Vec<D>, 8> x;
    for (std::size_t i = 0; i < 8; ++i) {
        x[i] = hn::BitCast(d, hn::LoadU(d_b, first_128 + i * 16));
    }
    x[0] = hn::Xor(x[0], running_state);

    const auto coeff_4_3 = make_clmul_coeff(d, keys[4], keys[3]);
    for (std::size_t chunk = 0; chunk < rest_chunks_of_128; ++chunk) {
        const std::uint8_t* HWY_RESTRICT chunk_base = rest_bytes + chunk * 128;
        for (std::size_t i = 0; i < 8; ++i) {
            const auto y_i = hn::BitCast(d, hn::LoadU(d_b, chunk_base + i * 16));
            x[i] = crc32_fold_16_reflected(d, x[i], coeff_4_3, y_i);
        }
    }

    auto res = x[7];
    const auto collapse_coeffs = crc32_collapse_coefficients(d, keys);
    for (std::size_t i = 0; i < 7; ++i) {
        res = crc32_fold_16_reflected(d, x[i], collapse_coeffs[i], res);
    }
    running_state = res;
}

/// Local helper: forward to zlib's `crc32_z` for the tail / fallback.
[[nodiscard]] HWY_INLINE std::uint32_t crc32_z_scalar(
    std::uint32_t running, const std::uint8_t* data, std::size_t size) noexcept {
    return static_cast<std::uint32_t>(::crc32_z(
        static_cast<uLong>(running),
        reinterpret_cast<const Bytef*>(data),
        static_cast<z_size_t>(size)));
}

template <class D>
[[nodiscard]] HWY_INLINE std::uint32_t crc32_iso_hdlc_reflected_impl(
    D                                    d,
    std::uint32_t                        running,
    const std::uint8_t*                  HWY_RESTRICT data,
    std::size_t                          size,
    const std::array<std::uint64_t, 23>& keys) noexcept {
    if (size < 128) {
        return crc32_z_scalar(running, data, size);
    }
    HWY_ALIGN std::array<std::uint64_t, 2> state_lanes{
        static_cast<std::uint64_t>(running), std::uint64_t{0}};
    auto running_state = hn::Load(d, state_lanes.data());

    const std::size_t aligned_chunks = size / 128;
    crc32_process_simd_chunks_reflected(
        d, running_state, data, data + 128,
        aligned_chunks > 0 ? aligned_chunks - 1 : 0, keys);

    std::uint32_t crc = crc32_final_reduction_reflected(d, running_state, keys);

    const std::size_t tail_offset = aligned_chunks * 128;
    if (tail_offset < size) {
        crc = crc32_z_scalar(crc, data + tail_offset, size - tail_offset);
    }
    return crc;
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

/// Multi-line variant: same encoding contract as
/// `dna4_simd_encode_body_chunk`, but `body_bytes` MAY contain `\n`
/// and `\r` (the LUT maps these to skip-sentinels and the SIMD
/// compaction path strips them lane-wise without falling back to
/// scalar). Wins on mmap-direct FASTA where the FSM scans for the
/// next `>` once and feeds the entire multi-line body to the encoder
/// in one call, eliminating per-line FSM dispatch overhead. Spec §11.3.
void dna4_simd_encode_body_chunk_multiline(
    bioseq::io::encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* body_bytes,
    std::size_t body_size) noexcept;

/// Fused multiline encode + record-end detection. Encodes from
/// `body_bytes` up to (but not including) the first `>` byte found,
/// returning the number of bytes consumed. If no `>` appears in
/// `[body_bytes, body_bytes + body_size)`, encodes the whole span and
/// returns `body_size`. Replaces a separate `scan_first_byte('>')`
/// pass + an `encode_body_chunk_multiline()` call with a single
/// SIMD walk over the body bytes — half the memory traffic, no
/// dispatch overhead between the two passes. Spec §11.3 / IO_V3.
[[nodiscard]] std::size_t dna4_simd_encode_body_chunk_multiline_until_gt(
    bioseq::io::encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* body_bytes,
    std::size_t body_size) noexcept;

/// Same fused-pass encoder but stops at `+` instead of `>`. Used by
/// the FASTQ FSM to encode the seq region (which terminates at the
/// `+` separator-line sigil) in a single SIMD walk. `+` is not a
/// valid dna4/dna5 letter so any `+` in a seq region is unambiguously
/// the separator. `newline_count_out` accumulates the count of `\n`
/// and `\r` bytes encountered (used by the FASTQ FSM to derive
/// `sequence_byte_count = consumed - newline_count` for qual length
/// validation). Spec §11.3 / IO_V3.
[[nodiscard]] std::size_t dna4_simd_encode_body_chunk_multiline_until_plus(
    bioseq::io::encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* body_bytes,
    std::size_t body_size,
    std::size_t* newline_count_out) noexcept;

/// Find the first byte in `[data, data+size)` matching ANY of the
/// (up to 3) needles in the array. Returns `size` if no match. The
/// 4-needle pack is exposed via this single-arity wrapper for the
/// FASTA FSM's `\n`-or-`>` body-end scan; it bypasses the per-call
/// arity dispatch in `scan_first_byte_pair` so the body scan loop
/// goes through one inline body. Spec §8.3.
[[nodiscard]] std::size_t scan_first_byte_dispatch3(
    std::uint8_t needle_0,
    std::uint8_t needle_1,
    std::uint8_t needle_2,
    const std::uint8_t* data,
    std::size_t size) noexcept;

/// dna4 SIMD decoder entry point — unpacks `total_letters` packed
/// 2-bit-rank letters (stored 32 letters per uint64) into ASCII bytes
/// at `dst`. The output buffer MUST have at least `total_letters`
/// bytes of capacity; the function emits exactly that many bytes (no
/// trailing newline). Used by the writer to materialise on-disk
/// FASTA / FASTQ bodies. Inverse of `dna4_simd_encode_body_chunk`.
void dna4_simd_decode_words_to_ascii(
    const std::uint64_t* words,
    std::uint8_t* dst,
    std::size_t total_letters) noexcept;

/// Same as `dna4_simd_decode_words_to_ascii` but injects `\n` every
/// `wrap_width` letters (no trailing newline). `letters_in_current_line`
/// is the number of letters already emitted on the current line on
/// entry; the function returns the new line-position counter so callers
/// can resume mid-line across multiple decode batches. `scratch` MUST
/// have at least `total_letters` bytes of capacity (used as an
/// intermediate buffer; the SIMD decoder writes there first, then the
/// memcpy step injects newlines into `dst`).
[[nodiscard]] std::size_t dna4_simd_decode_words_to_ascii_wrapped(
    const std::uint64_t* words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* scratch,
    std::uint8_t* dst) noexcept;

// ---------------------------------------------------------------------
// Compression-pipeline wrappers (declarations).
// ---------------------------------------------------------------------

/// Spec §5.1. In-place marker resolution. Buffer is `uint16_t[]` (the
/// chunk arena's storage). On entry, lanes with `value < 0x8000` are
/// already-resolved literals; lanes with `value >= 0x8000` reference
/// `window[value - 0x8000]`. After the call, every lane's low byte is
/// the resolved 8-bit byte.
void replace_markers_inplace(std::uint16_t*      buffer,
                             std::size_t         buffer_size,
                             const std::uint8_t* window /* exactly 32 KiB */) noexcept;

/// Spec §5.2. `dst[i] = static_cast<uint8_t>(src[i])` over `count`
/// elements. 4×-unrolled SIMD demote per iteration; tail handles
/// trailing lanes-1 elements via scalar.
void narrow_lower_bytes_to_dst(const std::uint16_t* src,
                               std::uint8_t*        dst,
                               std::size_t          count) noexcept;

/// Spec §5.3. Result of one Huffman fast-loop iteration. The `symbols`
/// array holds up to 3 decoded literal-only symbols;
/// `symbols_emitted < 3` signals the caller should fall back to the
/// scalar path for the breaking symbol.
inline constexpr std::size_t kHuffmanFastLoopUnroll = 3;

struct huffman_fast_loop_result {
    std::array<std::uint16_t, kHuffmanFastLoopUnroll> symbols;
    std::size_t                                       symbols_emitted;
    std::size_t                                       bits_consumed;
};

/// Decode up to `kHuffmanFastLoopUnroll` literal-only symbols from a
/// bit buffer that holds at least `kHuffmanFastLoopUnroll * 15 = 45`
/// bits. Caller is responsible for refilling before invocation.
[[nodiscard]] huffman_fast_loop_result huffman_fast_loop(
    std::uint64_t        bit_buffer,
    const std::uint16_t* primary_table) noexcept;

/// Drain the fast-loop result into the `_window16[]` ring. Scalar
/// store; vectorising over `symbols_emitted` would clobber later lanes
/// on the partial-emit path.
void huffman_fast_loop_store(
    const huffman_fast_loop_result& result,
    std::uint16_t*                  window_cursor) noexcept;

/// Spec §5.4. Highway CRC-32/ISO-HDLC entry point. Implemented but
/// **not wired** into the chunk pipeline by default — the orchestrator
/// keeps calling zlib's `crc32_z`. Same return semantics as
/// `crc32_update` in `compression/crc32.cppm` (they are bit-exact).
[[nodiscard]] std::uint32_t crc32_iso_hdlc_simd(
    std::uint32_t        running,
    const std::uint8_t*  bytes,
    std::size_t          size) noexcept;

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

void dna4_simd_encode_body_chunk_multiline(
    bioseq::io::encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* body_bytes,
    std::size_t body_size) noexcept {
    HWY_NAMESPACE::encode_body_chunk_dna4_simd_multiline<
        std::uint64_t, 2, 32>(state, body_bytes, body_size);
}

std::size_t dna4_simd_encode_body_chunk_multiline_until_gt(
    bioseq::io::encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* body_bytes,
    std::size_t body_size) noexcept {
    return HWY_NAMESPACE::encode_body_chunk_dna4_simd_multiline_until_byte<
        std::uint8_t{'>'}, std::uint64_t, 2, 32>(
            state, body_bytes, body_size, /*newline_count_out=*/nullptr);
}

std::size_t dna4_simd_encode_body_chunk_multiline_until_plus(
    bioseq::io::encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* body_bytes,
    std::size_t body_size,
    std::size_t* newline_count_out) noexcept {
    return HWY_NAMESPACE::encode_body_chunk_dna4_simd_multiline_until_byte<
        std::uint8_t{'+'}, std::uint64_t, 2, 32>(
            state, body_bytes, body_size, newline_count_out);
}

std::size_t scan_first_byte_dispatch3(
    std::uint8_t needle_0,
    std::uint8_t needle_1,
    std::uint8_t needle_2,
    const std::uint8_t* data,
    std::size_t size) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint8_t> byte_tag;
    const std::array<std::uint8_t, 3> needles{needle_0, needle_1, needle_2};
    return HWY_NAMESPACE::scan_for_first<3>(byte_tag, needles, data, size);
}

void dna4_simd_decode_words_to_ascii(
    const std::uint64_t* words,
    std::uint8_t* dst,
    std::size_t total_letters) noexcept {
    HWY_NAMESPACE::decode_dna4_words_to_ascii(words, dst, total_letters);
}

std::size_t dna4_simd_decode_words_to_ascii_wrapped(
    const std::uint64_t* words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* scratch,
    std::uint8_t* dst) noexcept {
    return HWY_NAMESPACE::decode_dna4_words_to_ascii_wrapped(
        words, total_letters, wrap_width,
        letters_in_current_line, scratch, dst);
}

// ---------------------------------------------------------------------
// Compression-pipeline wrappers (definitions). Spec §5.
// ---------------------------------------------------------------------

void replace_markers_inplace(
    std::uint16_t*      buffer,
    std::size_t         buffer_size,
    const std::uint8_t* window) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint16_t> d_u16;
    HWY_NAMESPACE::replace_markers_inplace_one_chunk(
        d_u16, buffer, buffer_size, window);
}

void narrow_lower_bytes_to_dst(
    const std::uint16_t* src,
    std::uint8_t*        dst,
    std::size_t          count) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint16_t> d_u16;
    HWY_NAMESPACE::narrow_lower_bytes_to_dst_inner(d_u16, src, dst, count);
}

huffman_fast_loop_result huffman_fast_loop(
    std::uint64_t        bit_buffer,
    const std::uint16_t* primary_table) noexcept {
    const auto inner = HWY_NAMESPACE::huffman_fast_loop_inner(
        bit_buffer, primary_table);
    huffman_fast_loop_result result{};
    result.symbols         = inner.symbols;
    result.symbols_emitted = inner.symbols_emitted;
    result.bits_consumed   = inner.bits_consumed;
    return result;
}

void huffman_fast_loop_store(
    const huffman_fast_loop_result& result,
    std::uint16_t*                  window_cursor) noexcept {
    HWY_NAMESPACE::huffman_fast_loop_result_inner inner{};
    inner.symbols         = result.symbols;
    inner.symbols_emitted = result.symbols_emitted;
    inner.bits_consumed   = result.bits_consumed;
    HWY_NAMESPACE::huffman_fast_loop_store_inner(inner, window_cursor);
}

std::uint32_t crc32_iso_hdlc_simd(
    std::uint32_t        running,
    const std::uint8_t*  bytes,
    std::size_t          size) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::Full128<std::uint64_t> d_u64;
    // Inline the constant key-table here so simd_kernels doesn't have
    // to import the compression module (avoids circular module deps).
    static constexpr std::array<std::uint64_t, 23> kIsoHdlcReflectedKeys = {
        0x0000000000000000ULL,
        0x00000000ccaa009eULL, 0x00000001751997d0ULL, 0x000000014a7fe880ULL,
        0x00000001e88ef372ULL, 0x00000000ccaa009eULL, 0x0000000163cd6124ULL,
        0x00000001f7011641ULL, 0x00000001db710641ULL, 0x00000001d7cfc6acULL,
        0x00000001ea89367eULL, 0x000000018cb44e58ULL, 0x00000000df068dc2ULL,
        0x00000000ae0b5394ULL, 0x00000001c7569e54ULL, 0x00000001c6e41596ULL,
        0x0000000154442bd4ULL, 0x0000000174359406ULL, 0x000000003db1ecdcULL,
        0x000000015a546366ULL, 0x00000000f1da05aaULL, 0x00000001322d1430ULL,
        0x000000011542778aULL,
    };
    return HWY_NAMESPACE::crc32_iso_hdlc_reflected_impl(
        d_u64, running, bytes, size, kIsoHdlcReflectedKeys);
}

}  // namespace bioseq::io::simd
