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
 *     pack stage stays scalar but stays register-resident; the
 *     headline win is the vectorised LUT lookup.
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

/// Encode a dna4 body span into the encoder state via the SIMD LUT
/// + scalar pack. Stages:
///
///   1. Vector LoadU + low-nibble TableLookupBytes → `Lanes(d)` ranks
///      stored to a stack scratch.
///   2. Tight scalar fold inlined here (NOT a call into
///      `append_ranks`) so the compiler can keep `partial_word` /
///      `partial_letters` in registers across the entire body.
///
/// The pack step is scalar but tight — `partial_word`/`partial_letters`
/// stay register-resident across the whole encode call instead of
/// being store-forwarded through the `state` struct on every byte.
/// Pre-reserves the destination vector's capacity to a small constant
/// so the per-record allocator churn is amortised across many records
/// (the matrix runner reports throughput sized as bytes-of-input/sec —
/// allocator dominance was the v1 bottleneck on short-read corpora).
/// Spec §8.8 / §11.4 / Appendix G.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void encode_body_chunk_dna4_simd(
    bioseq::io::encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    const hn::ScalableTag<std::uint8_t> byte_tag;
    static constexpr auto fallback_lut =
        bioseq::io::build_letter_to_rank_lut<seq::dna4>();
    constexpr std::size_t kRankScratchCapacity = hn::MaxLanes(byte_tag);
    HWY_ALIGN std::uint8_t rank_scratch[kRankScratchCapacity];

    static constexpr auto low_nibble_lut_array =
        bioseq::io::build_dna4_low_nibble_lut();
    const auto letter_to_rank_table_vec = hn::LoadDup128(
        byte_tag, low_nibble_lut_array.data());
    const auto low_nibble_mask = hn::Set(byte_tag, std::uint8_t{0x0F});

    // Hoist the partial-word state into locals so the inner loop's
    // store-forwarding does not bottleneck on the encoder struct
    // address. The writes back to `state` happen once at the end.
    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;

    // Pre-reserve a small tail buffer so the very first body line of a
    // record doesn't trigger 3-4 reallocs on its way to the typical
    // short-read length (~7 words for 200 bp). Cost: ~constant on
    // already-warm allocators; benefit: amortises the small-vector
    // malloc churn that dominates the v1 short-read throughput.
    constexpr std::size_t kInitialReserveWords = 8;
    if (state.packed_words.capacity() < kInitialReserveWords) {
        state.packed_words.reserve(kInitialReserveWords);
    }
    // No `HWY_RESTRICT` here — `packed_words` aliases the same vector
    // held by `state`, so promising non-overlap to the optimiser is
    // a lie. The pointer-to-member is just a small CSE hint.
    auto* packed_words = &state.packed_words;

    const std::size_t lanes = hn::Lanes(byte_tag);
    std::size_t read_cursor = 0;
    while (read_cursor + lanes <= body_size) {
        const auto chunk_bytes = hn::LoadU(byte_tag, body_bytes + read_cursor);
        const auto letter_ranks = dna4_lookup_ranks(
            byte_tag, letter_to_rank_table_vec, low_nibble_mask, chunk_bytes);
        hn::Store(letter_ranks, byte_tag, rank_scratch);
        for (std::size_t rank_index = 0; rank_index < lanes; ++rank_index) {
            const std::uint8_t letter_rank = rank_scratch[rank_index];
            if (letter_rank == 0xFF) continue;
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
        read_cursor += lanes;
    }
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
