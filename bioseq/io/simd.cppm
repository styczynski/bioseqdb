/**
 * @file simd.cppm
 * @brief Highway-bound IO-only SIMD primitives.
 *
 * Two responsibilities live here:
 *
 *   * **Scan** — `scan_for_first<p_needle_count>` and `scan_for_each<p_needle_count>`
 *     are the workhorses of every record FSM. The aligned-body loop
 *     is 4×-unrolled on AVX2 / NEON (single mask reduction decides for
 *     128 B per iter) and 2×-unrolled on AVX-512 (a 4× would exhaust
 *     the per-cycle fill buffer count).
 *
 *   * **Compression / gzip kernels** — `replace_markers_inplace`,
 *     `narrow_lower_bytes_to_dst`, `huffman_fast_loop*`,
 *     `crc32_iso_hdlc_simd`. Used by the parallel-gzip decoder.
 *
 * Sequence-translation primitives live in `bioseq_sequences.simd`, giving
 * backend SQL types and the IO FSM one definitive implementation.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>


#include <zlib.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

export module bioseq.io.simd;


export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::io::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// =====================================================================
// Scan kernels
// =====================================================================

/// Build a Highway mask "byte equals any of `needles`". For p_needle_count==1
/// the expression collapses to a single Eq; for N=2,3 the Eq results
/// are OR'd. Lifted out of the scan loops because the same expression
/// is reused at each unroll position.
template <std::size_t p_needle_count, class p_descriptor>
HWY_INLINE hn::Mask<p_descriptor> any_needle_match(
    p_descriptor d,
    const std::array<std::uint8_t, p_needle_count>& needles,
    hn::Vec<p_descriptor> chunk_bytes) noexcept {
    static_assert(p_needle_count >= 1 && p_needle_count <= 8,
                  "scan supports 1..8 needles");
    if constexpr (p_needle_count == 1) {
        return hn::Eq(chunk_bytes, hn::Set(d, needles[0]));
    } else if constexpr (p_needle_count == 2) {
        return hn::Or(hn::Eq(chunk_bytes, hn::Set(d, needles[0])),
                      hn::Eq(chunk_bytes, hn::Set(d, needles[1])));
    } else if constexpr (p_needle_count == 3) {
        return hn::Or(hn::Or(hn::Eq(chunk_bytes, hn::Set(d, needles[0])),
                             hn::Eq(chunk_bytes, hn::Set(d, needles[1]))),
                      hn::Eq(chunk_bytes, hn::Set(d, needles[2])));
    } else {  // Keep the common one-, two-, and three-needle expressions fixed.
        hn::Mask<p_descriptor> m = hn::Eq(chunk_bytes, hn::Set(d, needles[0]));
        for (std::size_t i = 1; i < p_needle_count; ++i)
            m = hn::Or(m, hn::Eq(chunk_bytes, hn::Set(d, needles[i])));
        return m;
    }
}

/// Per-target unroll factor for the aligned-body scan loop. AVX-512
/// lanes already process 64 B per chunk so a 2× unroll already
/// achieves the 128 B / iter target; AVX2 + NEON benefit from 4×
/// because each chunk is shorter (32 B / 16 B).
template <class p_descriptor>
constexpr std::size_t scan_unroll_factor() noexcept {
    if constexpr (hn::MaxLanes(p_descriptor{}) >= 64) return 2;
    return 4;
}

/// Find the first byte in `[data, data+size)` matching ANY of `needles`.
/// Returns `size` if absent.
template <std::size_t p_needle_count, class p_descriptor>
[[nodiscard]] HWY_INLINE std::size_t scan_for_first(
    p_descriptor d,
    const std::array<std::uint8_t, p_needle_count>& needles,
    const std::uint8_t* HWY_RESTRICT data,
    std::size_t size) noexcept {
    const std::size_t lanes = hn::Lanes(d);
    constexpr std::size_t unroll = scan_unroll_factor<p_descriptor>();

    if (size < lanes) {
        for (std::size_t scan_offset = 0; scan_offset < size; ++scan_offset) {
            for (std::size_t needle_index = 0;
                 needle_index < p_needle_count; ++needle_index) {
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
            const auto match_0 = any_needle_match<p_needle_count>(d, needles, chunk_0);
            const auto match_1 = any_needle_match<p_needle_count>(d, needles, chunk_1);
            const auto match_2 = any_needle_match<p_needle_count>(d, needles, chunk_2);
            const auto match_3 = any_needle_match<p_needle_count>(d, needles, chunk_3);
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
            const auto match_0 = any_needle_match<p_needle_count>(d, needles, chunk_0);
            const auto match_1 = any_needle_match<p_needle_count>(d, needles, chunk_1);
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
            any_needle_match<p_needle_count>(d, needles, chunk_bytes);
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
            any_needle_match<p_needle_count>(d, needles, chunk_bytes);
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
template <std::size_t p_needle_count, class p_descriptor, typename p_visit>
HWY_INLINE void scan_for_each(
    p_descriptor d,
    const std::array<std::uint8_t, p_needle_count>& needles,
    const std::uint8_t* HWY_RESTRICT data,
    std::size_t size,
    p_visit&& visit) noexcept {
    const std::size_t lanes = hn::Lanes(d);
    if (size < lanes) {
        for (std::size_t scan_offset = 0; scan_offset < size; ++scan_offset) {
            for (std::size_t needle_index = 0;
                 needle_index < p_needle_count; ++needle_index) {
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
            any_needle_match<p_needle_count>(d, needles, chunk_bytes);
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
             needle_index < p_needle_count; ++needle_index) {
            if (data[scan_offset] == needles[needle_index]) {
                visit(scan_offset);
                break;
            }
        }
    }
}


// =====================================================================
// Compression-pipeline kernels — used by parallel_gzip_decoder.
// All inside the existing HWY_BEFORE_NAMESPACE() block; no new HWY-
// wrapped module so every runtime target shares one dispatch owner.
// =====================================================================

/// Marker resolution — for each `uint16_t` value `v` in `buffer`, if
/// `v < 0x8000` keep low byte; else replace low byte with
/// `window[v - 0x8000]`.
template <class p_u16_descriptor>
HWY_INLINE void replace_markers_inplace_one_chunk(
    p_u16_descriptor               d_u16,
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
///
/// `Rebind<u8, p_u16_descriptor>` keeps the same lane count as the u16 tag (so
/// `DemoteTo` produces a vector with the same lane count, just half
/// the byte width), letting `StoreU` write `lanes_u16` contiguous
/// bytes in one op. (`Repartition` would double the lane count to
/// fill the same byte-width register, which doesn't match what
/// `DemoteTo` produces from a single u16 vector.)
template <class p_u16_descriptor>
HWY_INLINE void narrow_lower_bytes_to_dst_inner(
    p_u16_descriptor                              d_u16,
    const std::uint16_t* HWY_RESTRICT  src,
    std::uint8_t*        HWY_RESTRICT  dst,
    std::size_t                        count) noexcept {
    const hn::Rebind<std::uint8_t, p_u16_descriptor> d_u8_half;
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
// Huffman fast loop — decode `huffman_fast_loop_unroll` literal-only
// symbols per call from a shared bit buffer. Caller refills first.
// ---------------------------------------------------------------------

inline constexpr std::size_t huffman_fast_loop_unroll = 3;

/// Literal symbols and bit consumption produced by the inline fast loop.
struct huffman_fast_loop_result_inner {
    std::array<std::uint16_t, huffman_fast_loop_unroll> symbols{};
    std::size_t                                       symbols_emitted = 0;
    std::size_t                                       bits_consumed   = 0;
};

/// Decode consecutive primary-table literals without advancing a bit reader.
[[nodiscard]] HWY_INLINE huffman_fast_loop_result_inner
huffman_fast_loop_inner(
    std::uint64_t        bit_buffer,
    const std::uint16_t* HWY_RESTRICT primary_table /* size = 1<<11 */) noexcept {
    constexpr std::uint64_t primary_mask = (std::uint64_t{1} << 11) - 1;
    huffman_fast_loop_result_inner result{};
    std::size_t consumed = 0;
    for (std::size_t lane = 0; lane < huffman_fast_loop_unroll; ++lane) {
        const std::uint64_t peek  = (bit_buffer >> consumed) & primary_mask;
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

/// Store the literal prefix produced by the inline fast loop.
HWY_INLINE void huffman_fast_loop_store_inner(
    const huffman_fast_loop_result_inner& result,
    std::uint16_t* HWY_RESTRICT           window_cursor) noexcept {
    for (std::size_t lane = 0; lane < result.symbols_emitted; ++lane) {
        window_cursor[lane] = result.symbols[lane];
    }
}

// ---------------------------------------------------------------------
// CRC-32/ISO-HDLC reflected-mode Highway kernel. The `cmul_*` helpers map
// polynomial folds to Highway carry-less multiplication operations.
// ---------------------------------------------------------------------

/// Carry-less multiply of the low polynomial lanes.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE hn::Vec<p_descriptor> cmul_low_low(
    p_descriptor /*d*/, hn::Vec<p_descriptor> a, hn::Vec<p_descriptor> b) noexcept {
    return hn::CLMulLower(a, b);
}
/// Carry-less multiply of the high polynomial lanes.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE hn::Vec<p_descriptor> cmul_high_high(
    p_descriptor /*d*/, hn::Vec<p_descriptor> a, hn::Vec<p_descriptor> b) noexcept {
    return hn::CLMulUpper(a, b);
}
/// Carry-less multiply of the low lane by the high lane.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE hn::Vec<p_descriptor> cmul_low_high(
    p_descriptor /*d*/, hn::Vec<p_descriptor> a, hn::Vec<p_descriptor> b) noexcept {
    return hn::CLMulLower(a, hn::Shuffle01(b));
}
/// Carry-less multiply of the high lane by the low lane.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE hn::Vec<p_descriptor> cmul_high_low(
    p_descriptor /*d*/, hn::Vec<p_descriptor> a, hn::Vec<p_descriptor> b) noexcept {
    return hn::CLMulLower(hn::Shuffle01(a), b);
}

/// Build a coefficient vector. `high_lane` lands in lane 1; `low_lane`
/// in lane 0.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE hn::Vec<p_descriptor> make_clmul_coeff(
    p_descriptor d, std::uint64_t high_lane, std::uint64_t low_lane) noexcept {
    HWY_ALIGN std::array<std::uint64_t, 2> lanes{low_lane, high_lane};
    return hn::Load(d, lanes.data());
}

/// Fold one 16-byte reflected-polynomial state with new input.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE hn::Vec<p_descriptor> crc32_fold_16_reflected(
    p_descriptor d, hn::Vec<p_descriptor> state, hn::Vec<p_descriptor> coeff, hn::Vec<p_descriptor> data_to_xor) noexcept {
    const auto h = cmul_low_high(d, state, coeff);
    const auto l = cmul_high_low(d, state, coeff);
    return hn::Xor(hn::Xor(h, l), data_to_xor);
}

/// Build the coefficient sequence used to collapse eight folded lanes.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE std::array<hn::Vec<p_descriptor>, 7>
crc32_collapse_coefficients(
    p_descriptor d, const std::array<std::uint64_t, 23>& keys) noexcept {
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

/// Reduce a folded 128-bit state to a reflected CRC-32 value.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE std::uint32_t crc32_final_reduction_reflected(
    p_descriptor                                    d,
    hn::Vec<p_descriptor>                           state,
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

/// Fold one or more contiguous 128-byte chunks into the running state.
template <class p_descriptor>
HWY_INLINE void crc32_process_simd_chunks_reflected(
    p_descriptor                                    d,
    hn::Vec<p_descriptor>&                          running_state,
    const std::uint8_t*                  HWY_RESTRICT first_128,
    const std::uint8_t*                  HWY_RESTRICT rest_bytes,
    std::size_t                          rest_chunks_of_128,
    const std::array<std::uint64_t, 23>& keys) noexcept {
    const hn::Repartition<std::uint8_t, p_descriptor> d_b;
    std::array<hn::Vec<p_descriptor>, 8> x;
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

/// Update a reflected CRC using SIMD blocks and a scalar tail.
template <class p_descriptor>
[[nodiscard]] HWY_INLINE std::uint32_t crc32_iso_hdlc_reflected_impl(
    p_descriptor                                    d,
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
// Modules like `bioseq.io.fasta` / `bioseq.io.fastq` need a "find next
// '\n'" primitive but do not include `<hwy/highway.h>` in their GMF
// (doing so re-emits Highway's per-target inline operator overloads
// into every importer's BMI and trips clang's "definition with same
// mangled name" check on AArch64). The wrappers below take plain
// `std::uint8_t` arguments and have their bodies emitted once in
// `simd.cppm.o`; importers see only the declaration.
//
// The wrappers are deliberately NOT inline. C++26 module interface
// units define non-inline functions exactly once — perfect for this
// "expose a Highway-backed primitive but hide the templates" use
// case.

namespace bioseq::io::simd {

/// Find the first byte in `[data, data+size)` equal to `needle`, or
/// `size` if absent.
[[nodiscard]] std::size_t scan_first_byte(
    std::uint8_t needle,
    const std::uint8_t* data,
    std::size_t size) noexcept;

/// Same as `scan_first_byte` but for two needles.
[[nodiscard]] std::size_t scan_first_byte_pair(
    std::uint8_t needle_0,
    std::uint8_t needle_1,
    const std::uint8_t* data,
    std::size_t size) noexcept;

/// Find the first byte in `[data, data+size)` matching ANY of the
/// (up to 3) needles in the array. Returns `size` if no match. The
/// 4-needle pack is exposed via this single-arity wrapper for the
/// FASTA FSM's `\n`-or-`>` body-end scan; it bypasses the per-call
/// arity dispatch in `scan_first_byte_pair` so the body scan loop
/// goes through one inline body.
[[nodiscard]] std::size_t scan_first_byte_dispatch3(
    std::uint8_t needle_0,
    std::uint8_t needle_1,
    std::uint8_t needle_2,
    const std::uint8_t* data,
    std::size_t size) noexcept;

/// Visit every offset whose byte equals one of four delimiters, in ascending
/// order. The erased callback keeps the wrapper non-templated so the header
/// lexer does not import Highway templates.
void scan_for_each_delim4(
    std::uint8_t d0, std::uint8_t d1, std::uint8_t d2, std::uint8_t d3,
    const std::uint8_t* data, std::size_t size,
    void* ctx, void (*visit)(void* ctx, std::size_t offset)) noexcept;

// ---------------------------------------------------------------------
// Compression-pipeline wrappers (declarations).
// ---------------------------------------------------------------------

/// In-place marker resolution. Buffer is `uint16_t[]` (the
/// chunk arena's storage). On entry, lanes with `value < 0x8000` are
/// already-resolved literals; lanes with `value >= 0x8000` reference
/// `window[value - 0x8000]`. After the call, every lane's low byte is
/// the resolved 8-bit byte.
void replace_markers_inplace(std::uint16_t*      buffer,
                             std::size_t         buffer_size,
                             const std::uint8_t* window /* exactly 32 KiB */) noexcept;

/// `dst[i] = static_cast<uint8_t>(src[i])` over `count`
/// elements. 4×-unrolled SIMD demote per iteration; tail handles
/// trailing lanes-1 elements via scalar.
void narrow_lower_bytes_to_dst(const std::uint16_t* src,
                               std::uint8_t*        dst,
                               std::size_t          count) noexcept;

/// Maximum literals decoded by one Huffman fast-loop iteration.
inline constexpr std::size_t huffman_fast_loop_unroll = 3;

/// Result of one Huffman fast-loop iteration. A short result signals
/// that the caller must decode the breaking symbol through the scalar path.
struct huffman_fast_loop_result {
    std::array<std::uint16_t, huffman_fast_loop_unroll> symbols;
    std::size_t                                       symbols_emitted;
    std::size_t                                       bits_consumed;
};

/// Decode up to `huffman_fast_loop_unroll` literal-only symbols from a
/// bit buffer that holds at least `huffman_fast_loop_unroll * 15 = 45`
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

/// Highway CRC-32/ISO-HDLC update with zlib-compatible running-state
/// semantics.
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

void scan_for_each_delim4(std::uint8_t d0,
                          std::uint8_t d1,
                          std::uint8_t d2,
                          std::uint8_t d3,
                          const std::uint8_t* data,
                          std::size_t size,
                          void* ctx,
                          void (*visit)(void* ctx, std::size_t offset)) noexcept {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint8_t> byte_tag;
    const std::array<std::uint8_t, 4> needles{d0, d1, d2, d3};
    HWY_NAMESPACE::scan_for_each<4>(
        byte_tag, needles, data, size, [&ctx, &visit](std::size_t offset) { visit(ctx, offset); });
}

// ---------------------------------------------------------------------
// Compression-pipeline wrapper definitions.
// ---------------------------------------------------------------------

void replace_markers_inplace(std::uint16_t* buffer,
                             std::size_t buffer_size,
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
    static constexpr std::array<std::uint64_t, 23> iso_hdlc_reflected_keys = {
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
        d_u64, running, bytes, size, iso_hdlc_reflected_keys);
}

}  // namespace bioseq::io::simd
