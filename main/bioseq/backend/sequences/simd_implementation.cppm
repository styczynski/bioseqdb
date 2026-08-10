/**
 * @file simd_implementation.cppm
 * @brief Same-module implementation unit for Highway-heavy sequence SIMD kernels.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

module bioseq.sequences.simd;

import bioseq.sequences.rank_pack_core;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

#if HWY_TARGET == HWY_SSE2 || HWY_TARGET == HWY_SSSE3 || \
    HWY_TARGET == HWY_SSE4 || HWY_TARGET == HWY_AVX2
#define BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128 1
#else
#define BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128 0
#endif

namespace detail {

template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void append_valid_rank_to_locals(
        storage_unit_t& partial_word,
        std::size_t& partial_letters,
        std::size_t& total_letters,
        std::vector<storage_unit_t>* HWY_RESTRICT packed_words,
        std::uint8_t rank) {
    auto write_unit = [packed_words](storage_unit_t value) {
        packed_words->push_back(value);
        return true;
    };
    const bool appended =
        bioseq::sequences::rank_pack_detail::append_valid_rank_lsb<
            storage_unit_t, BitsPerChar, LettersPerUnit>(
                partial_word,
                partial_letters,
                total_letters,
                rank,
                write_unit);
    DEBUG_ASSERT(appended,
                 "growing SIMD rank pack unexpectedly overflowed");
}

template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit,
          unknown_byte_policy Policy = unknown_byte_policy::drop>
HWY_INLINE void encode_body_chunk_dna4_simd_impl(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd is specialised for "
                  "(dna4, uint64_t, 2, 32)");

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before encode_body_chunk_dna4_simd");

    static constexpr auto fallback_lut =
        build_letter_to_rank_lut<seq::dna4, Policy>();

    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    auto* packed_words = state.output_storage;

    if constexpr (Policy == unknown_byte_policy::drop) {
        // 16-byte low-nibble LUT — `'A' & 0x0F = 1`, etc. Sentinel
        // (0xFF) appears for newlines, CR, and any unknown byte;
        // chunks containing one fall back to scalar.
        static constexpr auto low_nibble_lut_array =
            build_dna4_low_nibble_lut();
        const auto letter_to_rank_table_vec = hn::LoadDup128(
            d_byte, low_nibble_lut_array.data());
        const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});

        std::size_t read_cursor = 0;

        // Two-chunk unrolled fast path (32 bytes / 64 packed bits =
        // exactly one uint64 storage unit when partial_letters == 0).
        while (read_cursor + 2 * lanes <= body_size) {
            const auto chunk_a = hn::LoadU(d_byte, body_bytes + read_cursor);
            const auto chunk_b = hn::LoadU(d_byte, body_bytes + read_cursor + lanes);

            const auto low_a = hn::And(chunk_a, low_nibble_mask);
            const auto low_b = hn::And(chunk_b, low_nibble_mask);

            const auto ranks_a = hn::TableLookupBytes(letter_to_rank_table_vec, low_a);
            const auto ranks_b = hn::TableLookupBytes(letter_to_rank_table_vec, low_b);

            if (HWY_UNLIKELY(has_skip_sentinel(d_byte, ranks_a) ||
                             has_skip_sentinel(d_byte, ranks_b))) {
                for (std::size_t scalar_offset = 0;
                     scalar_offset < 2 * lanes; ++scalar_offset) {
                    const std::uint8_t letter_rank =
                        fallback_lut[body_bytes[read_cursor + scalar_offset]];
                    if (letter_rank != 0xFF) {
                        append_valid_rank_to_locals<
                            storage_unit_t, BitsPerChar, LettersPerUnit>(
                                local_partial_word,
                                local_partial_letters,
                                local_total_letters,
                                packed_words,
                                letter_rank);
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
                        append_valid_rank_to_locals<
                            storage_unit_t, BitsPerChar, LettersPerUnit>(
                                local_partial_word,
                                local_partial_letters,
                                local_total_letters,
                                packed_words,
                                letter_rank);
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

        // Scalar tail.
        while (read_cursor < body_size) {
            const std::uint8_t letter_rank = fallback_lut[body_bytes[read_cursor]];
            if (letter_rank != 0xFF) {
                append_valid_rank_to_locals<
                    storage_unit_t, BitsPerChar, LettersPerUnit>(
                        local_partial_word,
                        local_partial_letters,
                        local_total_letters,
                        packed_words,
                        letter_rank);
            }
            read_cursor += 1;
        }
    } else {
        // Map-to-default policy: use the 32-byte two-half LUT. No sentinels,
        // no fallback path — every input byte produces a valid rank.
        static constexpr auto lut32 =
            build_letter_to_rank_lut_32<seq::dna4, Policy>();
        const auto lut_lo = hn::LoadDup128(d_byte, lut32[0].data());
        const auto lut_hi = hn::LoadDup128(d_byte, lut32[1].data());

        std::size_t read_cursor = 0;
        while (read_cursor + lanes <= body_size) {
            const auto chunk_bytes = hn::LoadU(d_byte, body_bytes + read_cursor);
            const auto ranks = lookup_ranks_via_32byte_lut(
                d_byte, lut_lo, lut_hi, chunk_bytes);
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(d_byte, ranks);
            merge_packed_chunk_into_partial(local_partial_word,
                                            local_partial_letters,
                                            packed_words,
                                            packed_chunk);
            local_total_letters += lanes;
            read_cursor += lanes;
        }
        // Scalar tail (always-valid rank under map-to-default semantics).
        while (read_cursor < body_size) {
            const std::uint8_t letter_rank = fallback_lut[body_bytes[read_cursor]];
            append_valid_rank_to_locals<
                storage_unit_t, BitsPerChar, LettersPerUnit>(
                    local_partial_word,
                    local_partial_letters,
                    local_total_letters,
                    packed_words,
                    letter_rank);
            read_cursor += 1;
        }
    }

    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
}



void encode_body_chunk_dna4_simd_drop_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna4_simd_impl<std::uint64_t, 2, 32,
        unknown_byte_policy::drop>(state, body_bytes, body_size);
}

void encode_body_chunk_dna4_simd_map_to_default_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna4_simd_impl<std::uint64_t, 2, 32,
        unknown_byte_policy::map_to_default>(state, body_bytes, body_size);
}

template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void encode_body_chunk_dna4_simd_multiline_impl(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline is specialised for "
                  "(dna4, uint64_t, 2, 32)");

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    static constexpr auto fallback_lut =
        build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto low_nibble_lut_array =
        build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec =
        hn::LoadDup128(d_byte, low_nibble_lut_array.data());
    const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});
    const auto sentinel_marker = hn::Set(d_byte, std::uint8_t{0xFF});

    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    auto* packed_words = state.output_storage;

    std::size_t read_cursor = 0;
    while (read_cursor + lanes <= body_size) {
        const auto chunk_bytes =
            hn::LoadU(d_byte, body_bytes + read_cursor);
        const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
        const auto ranks =
            hn::TableLookupBytes(letter_to_rank_table_vec, low_nibbles);
        const auto sentinel_eq = hn::Eq(ranks, sentinel_marker);

        if (HWY_LIKELY(hn::AllFalse(d_byte, sentinel_eq))) {
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(d_byte, ranks);
            merge_packed_chunk_into_partial(
                local_partial_word, local_partial_letters, packed_words,
                packed_chunk);
            local_total_letters += lanes;
            read_cursor += lanes;
            continue;
        }

        // Slow path: chunk contains newline/unknown sentinels. Keep this
        // target-independent; mask bit ordering differs across Highway
        // backends, while this path only runs at multiline boundaries.
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            const std::uint8_t letter_rank =
                fallback_lut[body_bytes[read_cursor + lane]];
            if (letter_rank == 0xFF) continue;
            append_valid_rank_to_locals<
                storage_unit_t, BitsPerChar, LettersPerUnit>(
                    local_partial_word,
                    local_partial_letters,
                    local_total_letters,
                    packed_words,
                    letter_rank);
        }
        read_cursor += lanes;
    }

    while (read_cursor < body_size) {
        const std::uint8_t letter_rank =
            fallback_lut[body_bytes[read_cursor]];
        if (letter_rank != 0xFF) {
            append_valid_rank_to_locals<
                storage_unit_t, BitsPerChar, LettersPerUnit>(
                    local_partial_word,
                    local_partial_letters,
                    local_total_letters,
                    packed_words,
                    letter_rank);
        }
        read_cursor += 1;
    }

    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
}



void encode_body_chunk_dna4_simd_multiline_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna4_simd_multiline_impl<std::uint64_t, 2, 32>(
        state, body_bytes, body_size);
}

[[nodiscard]] HWY_INLINE std::size_t
encode_body_chunk_dna4_simd_multiline_until_byte_runtime_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size,
    std::size_t* HWY_RESTRICT newline_count_out,
    std::uint8_t boundary_char) {
    using storage_unit_t = std::uint64_t;
    constexpr std::size_t BitsPerChar = 2;
    constexpr std::size_t LettersPerUnit = 32;
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline_until_byte is "
                  "specialised for (dna4, uint64_t, 2, 32)");

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    static constexpr auto fallback_lut =
        build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto low_nibble_lut_array =
        build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec =
        hn::LoadDup128(d_byte, low_nibble_lut_array.data());
    const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});
    const auto sentinel_marker = hn::Set(d_byte, std::uint8_t{0xFF});
    const auto boundary_marker = hn::Set(d_byte, boundary_char);
    const auto lf_marker = hn::Set(d_byte, std::uint8_t{'\n'});
    const auto cr_marker = hn::Set(d_byte, std::uint8_t{'\r'});

    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    std::size_t local_newline_count = 0;
    auto* packed_words = state.output_storage;

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

        const auto boundary_eq = hn::Eq(chunk_bytes, boundary_marker);
        if (HWY_UNLIKELY(!hn::AllFalse(d_byte, boundary_eq))) {
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
                    append_valid_rank_to_locals<
                        storage_unit_t, BitsPerChar, LettersPerUnit>(
                            local_partial_word,
                            local_partial_letters,
                            local_total_letters,
                            packed_words,
                            rank);
                }
            }
            return flush_state_and_return(scalar_end);
        }

        const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
        const auto ranks =
            hn::TableLookupBytes(letter_to_rank_table_vec, low_nibbles);
        const auto sentinel_eq = hn::Eq(ranks, sentinel_marker);

        if (HWY_LIKELY(hn::AllFalse(d_byte, sentinel_eq))) {
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(d_byte, ranks);
            merge_packed_chunk_into_partial(
                local_partial_word, local_partial_letters, packed_words,
                packed_chunk);
            local_total_letters += lanes;
            read_cursor += lanes;
            continue;
        }

        const auto lf_eq = hn::Eq(chunk_bytes, lf_marker);
        const auto cr_eq = hn::Eq(chunk_bytes, cr_marker);
        const std::uint64_t newline_bits =
            hn::BitsFromMask(d_byte, hn::Or(lf_eq, cr_eq));
        local_newline_count += static_cast<std::size_t>(
            std::popcount(newline_bits & 0xFFFFu));

        // Boundary was checked above; this chunk only needs newline/unknown
        // filtering. Keep the rare multiline-boundary path scalar so it is
        // independent of target-specific mask bit ordering.
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            const std::uint8_t current_byte = body_bytes[read_cursor + lane];
            if (current_byte == '\n' || current_byte == '\r') continue;
            const std::uint8_t rank = fallback_lut[current_byte];
            if (rank == 0xFF) continue;
            append_valid_rank_to_locals<
                storage_unit_t, BitsPerChar, LettersPerUnit>(
                    local_partial_word,
                    local_partial_letters,
                    local_total_letters,
                    packed_words,
                    rank);
        }
        read_cursor += lanes;
    }

    while (read_cursor < body_size) {
        const std::uint8_t current_byte = body_bytes[read_cursor];
        if (current_byte == boundary_char) {
            return flush_state_and_return(read_cursor);
        }
        if (current_byte == '\n' || current_byte == '\r') {
            local_newline_count += 1;
            read_cursor += 1;
            continue;
        }
        const std::uint8_t rank = fallback_lut[current_byte];
        if (rank != 0xFF) {
            append_valid_rank_to_locals<
                storage_unit_t, BitsPerChar, LettersPerUnit>(
                    local_partial_word,
                    local_partial_letters,
                    local_total_letters,
                    packed_words,
                    rank);
        }
        read_cursor += 1;
    }

    return flush_state_and_return(read_cursor);
}



[[nodiscard]] std::size_t encode_body_chunk_dna4_simd_multiline_until_byte_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size,
    std::size_t* HWY_RESTRICT newline_count_out,
    std::uint8_t boundary_char) {
    return encode_body_chunk_dna4_simd_multiline_until_byte_runtime_impl(
        state, body_bytes, body_size, newline_count_out, boundary_char);
}

template <class D>
HWY_INLINE void unpack_16_bytes_to_64_ascii(
    D d_byte,
    hn::Vec<D> packed_bytes,
    hn::Vec<D> rank_to_ascii_lut,
    std::uint8_t* HWY_RESTRICT dst) noexcept {
    const auto mask_low_2 = hn::Set(d_byte, std::uint8_t{0x03});

    const auto r0 = hn::And(packed_bytes, mask_low_2);
    const auto r1 = hn::And(hn::ShiftRight<2>(packed_bytes), mask_low_2);
    const auto r2 = hn::And(hn::ShiftRight<4>(packed_bytes), mask_low_2);
    const auto r3 = hn::And(hn::ShiftRight<6>(packed_bytes), mask_low_2);

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


void decode_dna4_words_to_ascii_impl(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;

    static constexpr auto rank_to_ascii_lut_array =
        build_dna4_rank_to_ascii_lut();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        d_byte, rank_to_ascii_lut_array.data());

    constexpr std::size_t letters_per_iter = 64;

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
    }

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


template <unknown_byte_policy Policy>
HWY_INLINE void encode_body_chunk_dna5_uint8_simd_impl(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before encode_body_chunk_dna5_uint8_simd");

    static constexpr auto lut32 =
        build_letter_to_rank_lut_32<seq::dna5, Policy>();
    static constexpr auto lut256 =
        build_letter_to_rank_lut<seq::dna5, Policy>();
    const auto lut_lo = hn::LoadDup128(d_byte, lut32[0].data());
    const auto lut_hi = hn::LoadDup128(d_byte, lut32[1].data());

    auto* packed_words = state.output_storage;
    std::uint8_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;

    auto append_one_rank = [&](std::uint8_t rank) {
        append_valid_rank_to_locals<std::uint8_t, 3u, 2u>(
            local_partial_word,
            local_partial_letters,
            local_total_letters,
            packed_words,
            rank);
    };

    std::size_t cursor = 0;

    // Compaction shuffles for the SIMD pack fast path.
#if !BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128
    HWY_ALIGN constexpr std::uint8_t even_indices_pattern[16] = {
        0, 2, 4, 6, 8, 10, 12, 14,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    HWY_ALIGN constexpr std::uint8_t odd_indices_pattern[16] = {
        1, 3, 5, 7, 9, 11, 13, 15,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const auto even_indices = hn::LoadU(d_byte, even_indices_pattern);
    const auto odd_indices = hn::LoadU(d_byte, odd_indices_pattern);
#endif
    HWY_ALIGN std::uint8_t store_buf[16];

    while (cursor + lanes <= body_size) {
        const auto chunk = hn::LoadU(d_byte, body_bytes + cursor);
        const auto ranks = lookup_ranks_via_32byte_lut(
            d_byte, lut_lo, lut_hi, chunk);

        if constexpr (Policy == unknown_byte_policy::drop) {
            const auto sentinel_eq = hn::Eq(ranks, hn::Set(d_byte, std::uint8_t{0xFF}));
            if (HWY_UNLIKELY(!hn::AllFalse(d_byte, sentinel_eq))) {
                HWY_ALIGN std::uint8_t scratch[16];
                hn::Store(ranks, d_byte, scratch);
                for (std::size_t i = 0; i < lanes; ++i) {
                    if (scratch[i] == 0xFF) continue;
                    append_one_rank(scratch[i]);
                }
                cursor += lanes;
                continue;
            }
        }

        // Fast path: 16 valid ranks. If the partial state is empty,
        // the SIMD pack writes 8 contiguous bytes. If partial_letters
        // == 1, fall back to scalar to keep state coherent (rare).
        if (local_partial_letters == 0) {
#if BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128
            HWY_ALIGN std::uint8_t scratch[16];
            hn::Store(ranks, d_byte, scratch);
            for (std::size_t i = 0; i < lanes; i += 2) {
                store_buf[i / 2] = static_cast<std::uint8_t>(
                    (scratch[i] & 0x07u) | ((scratch[i + 1] & 0x07u) << 3));
            }
#else
            const auto evens = hn::TableLookupBytes(ranks, even_indices);
            const auto odds = hn::TableLookupBytes(ranks, odd_indices);
            const auto packed = hn::Or(evens, hn::ShiftLeft<3>(odds));
            hn::Store(packed, d_byte, store_buf);
#endif
            packed_words->insert(packed_words->end(),
                                  store_buf, store_buf + 8);
            local_total_letters += lanes;
        } else {
            HWY_ALIGN std::uint8_t scratch[16];
            hn::Store(ranks, d_byte, scratch);
            for (std::size_t i = 0; i < lanes; ++i) {
                append_one_rank(scratch[i]);
            }
        }
        cursor += lanes;
    }

    // Scalar tail.
    while (cursor < body_size) {
        const std::uint8_t rank = lut256[body_bytes[cursor]];
        if constexpr (Policy == unknown_byte_policy::drop) {
            if (rank == 0xFF) {
                ++cursor;
                continue;
            }
        }
        append_one_rank(rank);
        ++cursor;
    }

    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
}



void encode_body_chunk_dna5_uint8_simd_drop_impl(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna5_uint8_simd_impl<unknown_byte_policy::drop>(
        state, body_bytes, body_size);
}

void encode_body_chunk_dna5_uint8_simd_map_to_default_impl(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna5_uint8_simd_impl<unknown_byte_policy::map_to_default>(
        state, body_bytes, body_size);
}

void decode_dna5_uint8_words_to_ascii_impl(
    const std::uint8_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;

    static constexpr auto rank_to_ascii_lut_array =
        build_dna5_rank_to_ascii_lut();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        d_byte, rank_to_ascii_lut_array.data());

    constexpr std::size_t letters_per_iter = 32;
    constexpr std::size_t bytes_per_iter = 16;

    std::size_t letters_emitted = 0;
    std::size_t bytes_consumed = 0;
    while (letters_emitted + letters_per_iter <= total_letters) {
        const auto packed = hn::LoadU(d_byte, words + bytes_consumed);
        const auto mask3 = hn::Set(d_byte, std::uint8_t{0x07});
        const auto rank_a = hn::And(packed, mask3);
        const auto rank_b = hn::And(hn::ShiftRight<3>(packed), mask3);

        const auto interleaved_lo =
            hn::InterleaveLower(d_byte, rank_a, rank_b);
        const auto interleaved_hi =
            hn::InterleaveUpper(d_byte, rank_a, rank_b);
        const auto ascii_lo =
            hn::TableLookupBytes(rank_to_ascii_lut, interleaved_lo);
        const auto ascii_hi =
            hn::TableLookupBytes(rank_to_ascii_lut, interleaved_hi);
        hn::StoreU(ascii_lo, d_byte, dst + letters_emitted);
        hn::StoreU(ascii_hi, d_byte, dst + letters_emitted + 16);
        letters_emitted += letters_per_iter;
        bytes_consumed += bytes_per_iter;
    }

    while (letters_emitted < total_letters) {
        const std::size_t byte_idx = letters_emitted / 2;
        const std::size_t letter_in_byte = letters_emitted % 2;
        const std::uint8_t byte = words[byte_idx];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (byte >> (3 * letter_in_byte)) & 0x07u);
        dst[letters_emitted] = rank_to_ascii_lut_array[rank];
        ++letters_emitted;
    }
}



template <sequence_alphabet alphabet_t, unknown_byte_policy Policy>
HWY_INLINE void ascii_to_rank_lutsimd_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept {
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    static constexpr auto lut32 =
        build_letter_to_rank_lut_32<alphabet_t, Policy>();
    static constexpr auto lut256 =
        build_letter_to_rank_lut<alphabet_t, Policy>();
    const auto lut_lo = hn::LoadDup128(d_byte, lut32[0].data());
    const auto lut_hi = hn::LoadDup128(d_byte, lut32[1].data());

    std::size_t cursor = 0;
    while (cursor + lanes <= size) {
        const auto chunk = hn::LoadU(d_byte, src + cursor);
        const auto ranks = lookup_ranks_via_32byte_lut(
            d_byte, lut_lo, lut_hi, chunk);
        hn::Store(ranks, d_byte, dst + cursor);
        cursor += lanes;
    }
    while (cursor < size) {
        dst[cursor] = lut256[src[cursor]];
        ++cursor;
    }
}

void ascii_to_rank_lutsimd_dna4_drop_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept {
    ascii_to_rank_lutsimd_impl<seq::dna4, unknown_byte_policy::drop>(
        src, dst, size);
}

void ascii_to_rank_lutsimd_dna4_map_to_default_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept {
    ascii_to_rank_lutsimd_impl<seq::dna4, unknown_byte_policy::map_to_default>(
        src, dst, size);
}

void ascii_to_rank_lutsimd_dna5_drop_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept {
    ascii_to_rank_lutsimd_impl<seq::dna5, unknown_byte_policy::drop>(
        src, dst, size);
}

void ascii_to_rank_lutsimd_dna5_map_to_default_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept {
    ascii_to_rank_lutsimd_impl<seq::dna5, unknown_byte_policy::map_to_default>(
        src, dst, size);
}

}  // namespace detail

#undef BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
