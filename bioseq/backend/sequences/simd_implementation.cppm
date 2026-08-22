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

/// @brief Appends one valid rank to local packing state and grows its output.
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
HWY_INLINE void append_valid_rank_to_locals(
        p_storage_unit& partial_word,
        std::size_t& partial_letters,
        std::size_t& total_letters,
        std::vector<p_storage_unit>* HWY_RESTRICT packed_words,
        std::uint8_t rank) {
    auto write_unit = [packed_words](p_storage_unit value) {
        packed_words->push_back(value);
        return true;
    };
    const bool appended =
        bioseq::sequences::rank_pack_detail::append_valid_rank_lsb<
            p_storage_unit, p_bits_per_character, p_letters_per_unit>(
                partial_word,
                partial_letters,
                total_letters,
                rank,
                write_unit);
    DEBUG_ASSERT(appended,
                 "growing SIMD rank pack unexpectedly overflowed");
}

/// @brief Encodes dna4 text with the selected unknown-byte policy.
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit,
          unknown_byte_policy p_unknown_byte_policy = unknown_byte_policy::drop>
HWY_INLINE void encode_body_chunk_dna4_simd_impl(
    encode_state<p_storage_unit,
                 p_bits_per_character,
                 p_letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(p_bits_per_character == 2 && p_letters_per_unit == 32 &&
                  std::is_same_v<p_storage_unit, std::uint64_t>,
                  "encode_body_chunk_dna4_simd is specialised for "
                  "(dna4, uint64_t, 2, 32)");

    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;
    constexpr std::size_t k_lanes = 16;

    DEBUG_ASSERT(
        state.output_storage != nullptr,
        "encode_state output storage must be configured before dna4 encoding");

    static constexpr auto k_fallback_lut =
        build_letter_to_rank_lut<seq::dna4, p_unknown_byte_policy>();

    p_storage_unit local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    auto* packed_words = state.output_storage;

    if constexpr (p_unknown_byte_policy == unknown_byte_policy::drop) {
        // 16-byte low-nibble LUT — `'A' & 0x0F = 1`, etc. Sentinel
        // (0xFF) appears for newlines, CR, and any unknown byte;
        // chunks containing one fall back to scalar.
        static constexpr auto k_low_nibble_lut =
            build_dna4_low_nibble_lut();
        const auto letter_to_rank_table_vec = hn::LoadDup128(
            simd_tag, k_low_nibble_lut.data());
        const auto low_nibble_mask = hn::Set(simd_tag, std::uint8_t{0x0F});

        std::size_t read_cursor = 0;

        // Two-chunk unrolled fast path (32 bytes / 64 packed bits =
        // exactly one uint64 storage unit when partial_letters == 0).
        while (read_cursor + 2 * k_lanes <= body_size) {
            const auto first_chunk = hn::LoadU(
                simd_tag, body_bytes + read_cursor);
            const auto second_chunk = hn::LoadU(
                simd_tag, body_bytes + read_cursor + k_lanes);

            const auto first_low_nibbles = hn::And(
                first_chunk, low_nibble_mask);
            const auto second_low_nibbles = hn::And(
                second_chunk, low_nibble_mask);

            const auto first_ranks = hn::TableLookupBytes(
                letter_to_rank_table_vec, first_low_nibbles);
            const auto second_ranks = hn::TableLookupBytes(
                letter_to_rank_table_vec, second_low_nibbles);

            if (HWY_UNLIKELY(has_skip_sentinel(simd_tag, first_ranks) ||
                             has_skip_sentinel(simd_tag, second_ranks))) {
                for (std::size_t scalar_offset = 0;
                     scalar_offset < 2 * k_lanes; ++scalar_offset) {
                    const std::uint8_t letter_rank =
                        k_fallback_lut[
                            body_bytes[read_cursor + scalar_offset]];
                    if (letter_rank != 0xFF) {
                        append_valid_rank_to_locals<
                            p_storage_unit,
                            p_bits_per_character,
                            p_letters_per_unit>(
                                local_partial_word,
                                local_partial_letters,
                                local_total_letters,
                                packed_words,
                                letter_rank);
                    }
                }
                read_cursor += 2 * k_lanes;
                continue;
            }

            const std::uint32_t first_packed_chunk =
                pack_16_dna4_to_u32(simd_tag, first_ranks);
            const std::uint32_t second_packed_chunk =
                pack_16_dna4_to_u32(simd_tag, second_ranks);

            merge_packed_chunk_into_partial(local_partial_word,
                                            local_partial_letters,
                                            packed_words,
                                            first_packed_chunk);
            merge_packed_chunk_into_partial(local_partial_word,
                                            local_partial_letters,
                                            packed_words,
                                            second_packed_chunk);
            local_total_letters += 2 * k_lanes;
            read_cursor += 2 * k_lanes;
        }

        // Single-chunk path for the trailing 16-byte block.
        if (read_cursor + k_lanes <= body_size) {
            const auto chunk_bytes = hn::LoadU(
                simd_tag, body_bytes + read_cursor);
            const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
            const auto letter_ranks = hn::TableLookupBytes(
                letter_to_rank_table_vec, low_nibbles);

            if (HWY_UNLIKELY(has_skip_sentinel(simd_tag, letter_ranks))) {
                for (std::size_t scalar_offset = 0;
                     scalar_offset < k_lanes; ++scalar_offset) {
                    const std::uint8_t letter_rank =
                        k_fallback_lut[
                            body_bytes[read_cursor + scalar_offset]];
                    if (letter_rank != 0xFF) {
                        append_valid_rank_to_locals<
                            p_storage_unit,
                            p_bits_per_character,
                            p_letters_per_unit>(
                                local_partial_word,
                                local_partial_letters,
                                local_total_letters,
                                packed_words,
                                letter_rank);
                    }
                }
            } else {
                const std::uint32_t packed_chunk =
                    pack_16_dna4_to_u32(simd_tag, letter_ranks);
                merge_packed_chunk_into_partial(local_partial_word,
                                                local_partial_letters,
                                                packed_words,
                                                packed_chunk);
                local_total_letters += k_lanes;
            }
            read_cursor += k_lanes;
        }

        // Scalar tail.
        while (read_cursor < body_size) {
            const std::uint8_t letter_rank =
                k_fallback_lut[body_bytes[read_cursor]];
            if (letter_rank != 0xFF) {
                append_valid_rank_to_locals<
                    p_storage_unit,
                    p_bits_per_character,
                    p_letters_per_unit>(
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
        static constexpr auto k_letter_to_rank_lut_32 =
            build_letter_to_rank_lut_32<
                seq::dna4, p_unknown_byte_policy>();
        const auto lower_lut = hn::LoadDup128(
            simd_tag, k_letter_to_rank_lut_32[0].data());
        const auto upper_lut = hn::LoadDup128(
            simd_tag, k_letter_to_rank_lut_32[1].data());

        std::size_t read_cursor = 0;
        while (read_cursor + k_lanes <= body_size) {
            const auto chunk_bytes = hn::LoadU(
                simd_tag, body_bytes + read_cursor);
            const auto ranks = lookup_ranks_via_32byte_lut(
                simd_tag, lower_lut, upper_lut, chunk_bytes);
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(simd_tag, ranks);
            merge_packed_chunk_into_partial(local_partial_word,
                                            local_partial_letters,
                                            packed_words,
                                            packed_chunk);
            local_total_letters += k_lanes;
            read_cursor += k_lanes;
        }
        // Scalar tail (always-valid rank under map-to-default semantics).
        while (read_cursor < body_size) {
            const std::uint8_t letter_rank =
                k_fallback_lut[body_bytes[read_cursor]];
            append_valid_rank_to_locals<
                p_storage_unit,
                p_bits_per_character,
                p_letters_per_unit>(
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

/// @brief Encodes dna4 text while dropping unknown input bytes.
void encode_body_chunk_dna4_simd_drop_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna4_simd_impl<std::uint64_t, 2, 32,
        unknown_byte_policy::drop>(state, body_bytes, body_size);
}

/// @brief Encodes dna4 text while mapping unknown bytes to the default rank.
void encode_body_chunk_dna4_simd_map_to_default_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna4_simd_impl<std::uint64_t, 2, 32,
        unknown_byte_policy::map_to_default>(state, body_bytes, body_size);
}

/// @brief Encodes multiline dna4 text while ignoring line separators.
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
HWY_INLINE void encode_body_chunk_dna4_simd_multiline_impl(
    encode_state<p_storage_unit,
                 p_bits_per_character,
                 p_letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(p_bits_per_character == 2 && p_letters_per_unit == 32 &&
                  std::is_same_v<p_storage_unit, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline is specialised for "
                  "(dna4, uint64_t, 2, 32)");

    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;
    constexpr std::size_t k_lanes = 16;

    static constexpr auto k_fallback_lut =
        build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto k_low_nibble_lut =
        build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec =
        hn::LoadDup128(simd_tag, k_low_nibble_lut.data());
    const auto low_nibble_mask = hn::Set(simd_tag, std::uint8_t{0x0F});
    const auto sentinel_marker = hn::Set(simd_tag, std::uint8_t{0xFF});

    p_storage_unit local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    auto* packed_words = state.output_storage;

    std::size_t read_cursor = 0;
    while (read_cursor + k_lanes <= body_size) {
        const auto chunk_bytes =
            hn::LoadU(simd_tag, body_bytes + read_cursor);
        const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
        const auto ranks =
            hn::TableLookupBytes(letter_to_rank_table_vec, low_nibbles);
        const auto sentinel_eq = hn::Eq(ranks, sentinel_marker);

        if (HWY_LIKELY(hn::AllFalse(simd_tag, sentinel_eq))) {
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(simd_tag, ranks);
            merge_packed_chunk_into_partial(
                local_partial_word, local_partial_letters, packed_words,
                packed_chunk);
            local_total_letters += k_lanes;
            read_cursor += k_lanes;
            continue;
        }

        // Slow path: chunk contains newline/unknown sentinels. Keep this
        // target-independent; mask bit ordering differs across Highway
        // backends, while this path only runs at multiline boundaries.
        for (std::size_t lane = 0; lane < k_lanes; ++lane) {
            const std::uint8_t letter_rank =
                k_fallback_lut[body_bytes[read_cursor + lane]];
            if (letter_rank == 0xFF) continue;
            append_valid_rank_to_locals<
                p_storage_unit,
                p_bits_per_character,
                p_letters_per_unit>(
                    local_partial_word,
                    local_partial_letters,
                    local_total_letters,
                    packed_words,
                    letter_rank);
        }
        read_cursor += k_lanes;
    }

    while (read_cursor < body_size) {
        const std::uint8_t letter_rank =
            k_fallback_lut[body_bytes[read_cursor]];
        if (letter_rank != 0xFF) {
            append_valid_rank_to_locals<
                p_storage_unit,
                p_bits_per_character,
                p_letters_per_unit>(
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

/// @brief Dispatch leaf for multiline dna4 encoding.
void encode_body_chunk_dna4_simd_multiline_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna4_simd_multiline_impl<std::uint64_t, 2, 32>(
        state, body_bytes, body_size);
}

/// @brief Encodes multiline dna4 input until a boundary byte is encountered.
[[nodiscard]] HWY_INLINE std::size_t
encode_body_chunk_dna4_simd_multiline_until_byte_runtime_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size,
    std::size_t* HWY_RESTRICT newline_count_out,
    std::uint8_t boundary_char) {
    using storage_unit_type = std::uint64_t;
    constexpr std::size_t k_bits_per_character = 2;
    constexpr std::size_t k_letters_per_unit = 32;
    static_assert(k_bits_per_character == 2 && k_letters_per_unit == 32 &&
                  std::is_same_v<storage_unit_type, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline_until_byte is "
                  "specialised for (dna4, uint64_t, 2, 32)");

    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;
    constexpr std::size_t k_lanes = 16;

    static constexpr auto k_fallback_lut =
        build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto k_low_nibble_lut =
        build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec =
        hn::LoadDup128(simd_tag, k_low_nibble_lut.data());
    const auto low_nibble_mask = hn::Set(simd_tag, std::uint8_t{0x0F});
    const auto sentinel_marker = hn::Set(simd_tag, std::uint8_t{0xFF});
    const auto boundary_marker = hn::Set(simd_tag, boundary_char);
    const auto line_feed_marker = hn::Set(
        simd_tag, std::uint8_t{'\n'});
    const auto carriage_return_marker = hn::Set(
        simd_tag, std::uint8_t{'\r'});

    storage_unit_type local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    std::size_t local_newline_count = 0;
    auto* packed_words = state.output_storage;

    std::size_t read_cursor = 0;

    auto flush_state_and_return = [&state,
                                   &local_partial_word,
                                   &local_partial_letters,
                                   &local_total_letters,
                                   &newline_count_out,
                                   &local_newline_count](std::size_t consumed) {
        state.partial_word = local_partial_word;
        state.partial_letters = local_partial_letters;
        state.total_letters = local_total_letters;
        if (newline_count_out != nullptr) {
            *newline_count_out += local_newline_count;
        }
        return consumed;
    };

    while (read_cursor + k_lanes <= body_size) {
        const auto chunk_bytes =
            hn::LoadU(simd_tag, body_bytes + read_cursor);

        const auto boundary_eq = hn::Eq(chunk_bytes, boundary_marker);
        if (HWY_UNLIKELY(!hn::AllFalse(simd_tag, boundary_eq))) {
            const std::size_t boundary_position = static_cast<std::size_t>(
                hn::FindFirstTrue(simd_tag, boundary_eq));
            const std::size_t scalar_end =
                read_cursor + boundary_position;
            for (std::size_t scalar_offset = read_cursor;
                 scalar_offset < scalar_end; ++scalar_offset) {
                const std::uint8_t current_byte = body_bytes[scalar_offset];
                if (current_byte == '\n' || current_byte == '\r') {
                    local_newline_count += 1;
                    continue;
                }
                const std::uint8_t rank = k_fallback_lut[current_byte];
                if (rank != 0xFF) {
                    append_valid_rank_to_locals<
                        storage_unit_type,
                        k_bits_per_character,
                        k_letters_per_unit>(
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

        if (HWY_LIKELY(hn::AllFalse(simd_tag, sentinel_eq))) {
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(simd_tag, ranks);
            merge_packed_chunk_into_partial(
                local_partial_word, local_partial_letters, packed_words,
                packed_chunk);
            local_total_letters += k_lanes;
            read_cursor += k_lanes;
            continue;
        }

        const auto line_feed_matches = hn::Eq(
            chunk_bytes, line_feed_marker);
        const auto carriage_return_matches = hn::Eq(
            chunk_bytes, carriage_return_marker);
        const std::uint64_t newline_bits =
            hn::BitsFromMask(
                simd_tag,
                hn::Or(line_feed_matches, carriage_return_matches));
        local_newline_count += static_cast<std::size_t>(
            std::popcount(newline_bits & 0xFFFFu));

        // Boundary was checked above; this chunk only needs newline/unknown
        // filtering. Keep the rare multiline-boundary path scalar so it is
        // independent of target-specific mask bit ordering.
        for (std::size_t lane = 0; lane < k_lanes; ++lane) {
            const std::uint8_t current_byte = body_bytes[read_cursor + lane];
            if (current_byte == '\n' || current_byte == '\r') continue;
            const std::uint8_t rank = k_fallback_lut[current_byte];
            if (rank == 0xFF) continue;
            append_valid_rank_to_locals<
                storage_unit_type,
                k_bits_per_character,
                k_letters_per_unit>(
                    local_partial_word,
                    local_partial_letters,
                    local_total_letters,
                    packed_words,
                    rank);
        }
        read_cursor += k_lanes;
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
        const std::uint8_t rank = k_fallback_lut[current_byte];
        if (rank != 0xFF) {
            append_valid_rank_to_locals<
                storage_unit_type,
                k_bits_per_character,
                k_letters_per_unit>(
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

/// @brief Dispatch leaf for boundary-limited multiline dna4 encoding.
[[nodiscard]] std::size_t encode_body_chunk_dna4_simd_multiline_until_byte_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size,
    std::size_t* HWY_RESTRICT newline_count_out,
    std::uint8_t boundary_char) {
    return encode_body_chunk_dna4_simd_multiline_until_byte_runtime_impl(
        state, body_bytes, body_size, newline_count_out, boundary_char);
}

/// @brief Expands sixteen packed dna4 bytes into sixty-four ASCII symbols.
template <class p_simd_tag>
HWY_INLINE void unpack_16_bytes_to_64_ascii(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> packed_bytes,
    hn::Vec<p_simd_tag> rank_to_ascii_lut,
    std::uint8_t* HWY_RESTRICT destination) noexcept {
    const auto low_two_bit_mask = hn::Set(
        simd_tag, std::uint8_t{0x03});

    const auto first_rank = hn::And(packed_bytes, low_two_bit_mask);
    const auto second_rank = hn::And(
        hn::ShiftRight<2>(packed_bytes), low_two_bit_mask);
    const auto third_rank = hn::And(
        hn::ShiftRight<4>(packed_bytes), low_two_bit_mask);
    const auto fourth_rank = hn::And(
        hn::ShiftRight<6>(packed_bytes), low_two_bit_mask);

    using uint16_simd_tag_type = hn::Repartition<std::uint16_t, p_simd_tag>;
    uint16_simd_tag_type uint16_simd_tag;

    const auto first_pair_lower = hn::InterleaveLower(
        simd_tag, first_rank, second_rank);
    const auto first_pair_upper = hn::InterleaveUpper(
        simd_tag, first_rank, second_rank);
    const auto second_pair_lower = hn::InterleaveLower(
        simd_tag, third_rank, fourth_rank);
    const auto second_pair_upper = hn::InterleaveUpper(
        simd_tag, third_rank, fourth_rank);

    const auto first_rank_chunk = hn::BitCast(
        simd_tag,
        hn::InterleaveLower(
            uint16_simd_tag,
            hn::BitCast(uint16_simd_tag, first_pair_lower),
            hn::BitCast(uint16_simd_tag, second_pair_lower)));
    const auto second_rank_chunk = hn::BitCast(
        simd_tag,
        hn::InterleaveUpper(
            uint16_simd_tag,
            hn::BitCast(uint16_simd_tag, first_pair_lower),
            hn::BitCast(uint16_simd_tag, second_pair_lower)));
    const auto third_rank_chunk = hn::BitCast(
        simd_tag,
        hn::InterleaveLower(
            uint16_simd_tag,
            hn::BitCast(uint16_simd_tag, first_pair_upper),
            hn::BitCast(uint16_simd_tag, second_pair_upper)));
    const auto fourth_rank_chunk = hn::BitCast(
        simd_tag,
        hn::InterleaveUpper(
            uint16_simd_tag,
            hn::BitCast(uint16_simd_tag, first_pair_upper),
            hn::BitCast(uint16_simd_tag, second_pair_upper)));

    const auto first_ascii_chunk = hn::TableLookupBytes(
        rank_to_ascii_lut, first_rank_chunk);
    const auto second_ascii_chunk = hn::TableLookupBytes(
        rank_to_ascii_lut, second_rank_chunk);
    const auto third_ascii_chunk = hn::TableLookupBytes(
        rank_to_ascii_lut, third_rank_chunk);
    const auto fourth_ascii_chunk = hn::TableLookupBytes(
        rank_to_ascii_lut, fourth_rank_chunk);

    constexpr std::size_t k_lanes_per_chunk = 16;
    hn::StoreU(first_ascii_chunk, simd_tag, destination);
    hn::StoreU(
        second_ascii_chunk,
        simd_tag,
        destination + k_lanes_per_chunk);
    hn::StoreU(
        third_ascii_chunk,
        simd_tag,
        destination + 2 * k_lanes_per_chunk);
    hn::StoreU(
        fourth_ascii_chunk,
        simd_tag,
        destination + 3 * k_lanes_per_chunk);
}

/// @brief Decodes packed dna4 words into ASCII without reading past the tail.
void decode_dna4_words_to_ascii_impl(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t total_letters) noexcept {
    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;

    static constexpr auto k_rank_to_ascii_lut =
        build_dna4_rank_to_ascii_lut();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        simd_tag, k_rank_to_ascii_lut.data());

    constexpr std::size_t k_letters_per_iteration = 64;

    std::size_t letters_emitted = 0;
    std::size_t word_cursor = 0;
    const auto* packed_bytes_base =
        reinterpret_cast<const std::uint8_t*>(words);

    while (letters_emitted + k_letters_per_iteration <= total_letters) {
        const auto packed_bytes = hn::LoadU(
            simd_tag, packed_bytes_base + word_cursor * 8);
        unpack_16_bytes_to_64_ascii(
            simd_tag,
            packed_bytes,
            rank_to_ascii_lut,
            destination + letters_emitted);
        letters_emitted += k_letters_per_iteration;
        word_cursor += 2;
    }

    while (letters_emitted < total_letters) {
        const std::size_t word_index = letters_emitted / 32;
        const std::size_t letter_in_word = letters_emitted % 32;
        const std::uint64_t word = words[word_index];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (2 * letter_in_word)) & 0x3u);
        destination[letters_emitted] = k_rank_to_ascii_lut[rank];
        ++letters_emitted;
    }
}

/// @brief Encodes two-ranks-per-byte dna5 input with a selected unknown policy.
template <unknown_byte_policy p_unknown_byte_policy>
HWY_INLINE void encode_body_chunk_dna5_uint8_simd_impl(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;
    constexpr std::size_t k_lanes = 16;

    DEBUG_ASSERT(
        state.output_storage != nullptr,
        "encode_state output storage must be configured before dna5 encoding");

    static constexpr auto k_letter_to_rank_lut_32 =
        build_letter_to_rank_lut_32<seq::dna5, p_unknown_byte_policy>();
    static constexpr auto k_letter_to_rank_lut =
        build_letter_to_rank_lut<seq::dna5, p_unknown_byte_policy>();
    const auto lower_lut = hn::LoadDup128(
        simd_tag, k_letter_to_rank_lut_32[0].data());
    const auto upper_lut = hn::LoadDup128(
        simd_tag, k_letter_to_rank_lut_32[1].data());

    auto* packed_words = state.output_storage;
    std::uint8_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;

    auto append_one_rank = [&local_partial_word,
                            &local_partial_letters,
                            &local_total_letters,
                            &packed_words](std::uint8_t rank) {
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
    HWY_ALIGN constexpr std::uint8_t k_even_indices_pattern[16] = {
        0, 2, 4, 6, 8, 10, 12, 14,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    HWY_ALIGN constexpr std::uint8_t k_odd_indices_pattern[16] = {
        1, 3, 5, 7, 9, 11, 13, 15,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const auto even_indices = hn::LoadU(
        simd_tag, k_even_indices_pattern);
    const auto odd_indices = hn::LoadU(
        simd_tag, k_odd_indices_pattern);
#endif
    HWY_ALIGN std::uint8_t packed_bytes[16];

    while (cursor + k_lanes <= body_size) {
        const auto chunk = hn::LoadU(simd_tag, body_bytes + cursor);
        const auto ranks = lookup_ranks_via_32byte_lut(
            simd_tag, lower_lut, upper_lut, chunk);

        if constexpr (p_unknown_byte_policy == unknown_byte_policy::drop) {
            const auto sentinel_matches = hn::Eq(
                ranks, hn::Set(simd_tag, std::uint8_t{0xFF}));
            if (HWY_UNLIKELY(
                    !hn::AllFalse(simd_tag, sentinel_matches))) {
                HWY_ALIGN std::uint8_t rank_scratch[16];
                hn::Store(ranks, simd_tag, rank_scratch);
                for (std::size_t rank_index = 0;
                     rank_index < k_lanes;
                     ++rank_index) {
                    if (rank_scratch[rank_index] == 0xFF) continue;
                    append_one_rank(rank_scratch[rank_index]);
                }
                cursor += k_lanes;
                continue;
            }
        }

        // Fast path: 16 valid ranks. If the partial state is empty,
        // the SIMD pack writes 8 contiguous bytes. If partial_letters
        // == 1, fall back to scalar to keep state coherent (rare).
        if (local_partial_letters == 0) {
#if BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128
            HWY_ALIGN std::uint8_t rank_scratch[16];
            hn::Store(ranks, simd_tag, rank_scratch);
            for (std::size_t rank_index = 0;
                 rank_index < k_lanes;
                 rank_index += 2) {
                packed_bytes[rank_index / 2] = static_cast<std::uint8_t>(
                    (rank_scratch[rank_index] & 0x07u)
                    | ((rank_scratch[rank_index + 1] & 0x07u) << 3));
            }
#else
            const auto even_ranks = hn::TableLookupBytes(
                ranks, even_indices);
            const auto odd_ranks = hn::TableLookupBytes(
                ranks, odd_indices);
            const auto packed_ranks = hn::Or(
                even_ranks, hn::ShiftLeft<3>(odd_ranks));
            hn::Store(packed_ranks, simd_tag, packed_bytes);
#endif
            packed_words->insert(packed_words->end(),
                                  packed_bytes, packed_bytes + 8);
            local_total_letters += k_lanes;
        } else {
            HWY_ALIGN std::uint8_t rank_scratch[16];
            hn::Store(ranks, simd_tag, rank_scratch);
            for (std::size_t rank_index = 0;
                 rank_index < k_lanes;
                 ++rank_index) {
                append_one_rank(rank_scratch[rank_index]);
            }
        }
        cursor += k_lanes;
    }

    // Scalar tail.
    while (cursor < body_size) {
        const std::uint8_t rank =
            k_letter_to_rank_lut[body_bytes[cursor]];
        if constexpr (p_unknown_byte_policy == unknown_byte_policy::drop) {
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

/// @brief Encodes dna5 bytes while dropping unknown symbols.
void encode_body_chunk_dna5_uint8_simd_drop_impl(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna5_uint8_simd_impl<unknown_byte_policy::drop>(
        state, body_bytes, body_size);
}

/// @brief Encodes dna5 bytes while mapping unknown symbols to the default rank.
void encode_body_chunk_dna5_uint8_simd_map_to_default_impl(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    encode_body_chunk_dna5_uint8_simd_impl<unknown_byte_policy::map_to_default>(
        state, body_bytes, body_size);
}

/// @brief Decodes two-ranks-per-byte dna5 storage into ASCII.
void decode_dna5_uint8_words_to_ascii_impl(
    const std::uint8_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t total_letters) noexcept {
    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;

    static constexpr auto k_rank_to_ascii_lut =
        build_dna5_rank_to_ascii_lut();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        simd_tag, k_rank_to_ascii_lut.data());

    constexpr std::size_t k_letters_per_iteration = 32;
    constexpr std::size_t k_bytes_per_iteration = 16;

    std::size_t letters_emitted = 0;
    std::size_t bytes_consumed = 0;
    while (letters_emitted + k_letters_per_iteration <= total_letters) {
        const auto packed = hn::LoadU(
            simd_tag, words + bytes_consumed);
        const auto low_three_bit_mask = hn::Set(
            simd_tag, std::uint8_t{0x07});
        const auto first_rank = hn::And(packed, low_three_bit_mask);
        const auto second_rank = hn::And(
            hn::ShiftRight<3>(packed), low_three_bit_mask);

        const auto interleaved_lo =
            hn::InterleaveLower(simd_tag, first_rank, second_rank);
        const auto interleaved_hi =
            hn::InterleaveUpper(simd_tag, first_rank, second_rank);
        const auto ascii_lo =
            hn::TableLookupBytes(rank_to_ascii_lut, interleaved_lo);
        const auto ascii_hi =
            hn::TableLookupBytes(rank_to_ascii_lut, interleaved_hi);
        hn::StoreU(ascii_lo, simd_tag, destination + letters_emitted);
        hn::StoreU(
            ascii_hi,
            simd_tag,
            destination + letters_emitted + 16);
        letters_emitted += k_letters_per_iteration;
        bytes_consumed += k_bytes_per_iteration;
    }

    while (letters_emitted < total_letters) {
        const std::size_t byte_index = letters_emitted / 2;
        const std::size_t letter_in_byte = letters_emitted % 2;
        const std::uint8_t byte = words[byte_index];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (byte >> (3 * letter_in_byte)) & 0x07u);
        destination[letters_emitted] = k_rank_to_ascii_lut[rank];
        ++letters_emitted;
    }
}

/// @brief Maps ASCII bytes to ranks through a fixed-target lookup table.
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_unknown_byte_policy>
HWY_INLINE void ascii_to_rank_lut_simd_impl(
    const std::uint8_t* HWY_RESTRICT source,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t size) noexcept {
    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;
    constexpr std::size_t k_lanes = 16;

    static constexpr auto k_letter_to_rank_lut_32 =
        build_letter_to_rank_lut_32<
            p_alphabet, p_unknown_byte_policy>();
    static constexpr auto k_letter_to_rank_lut =
        build_letter_to_rank_lut<p_alphabet, p_unknown_byte_policy>();
    const auto lower_lut = hn::LoadDup128(
        simd_tag, k_letter_to_rank_lut_32[0].data());
    const auto upper_lut = hn::LoadDup128(
        simd_tag, k_letter_to_rank_lut_32[1].data());

    std::size_t cursor = 0;
    while (cursor + k_lanes <= size) {
        const auto chunk = hn::LoadU(simd_tag, source + cursor);
        const auto ranks = lookup_ranks_via_32byte_lut(
            simd_tag, lower_lut, upper_lut, chunk);
        hn::Store(ranks, simd_tag, destination + cursor);
        cursor += k_lanes;
    }
    while (cursor < size) {
        destination[cursor] = k_letter_to_rank_lut[source[cursor]];
        ++cursor;
    }
}

/// @brief Maps dna4 ASCII bytes and preserves the unknown sentinel.
void ascii_to_rank_lut_simd_dna4_drop_impl(
    const std::uint8_t* HWY_RESTRICT source,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t size) noexcept {
    ascii_to_rank_lut_simd_impl<seq::dna4, unknown_byte_policy::drop>(
        source, destination, size);
}

/// @brief Maps dna4 ASCII bytes and replaces unknown bytes with rank zero.
void ascii_to_rank_lut_simd_dna4_map_to_default_impl(
    const std::uint8_t* HWY_RESTRICT source,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t size) noexcept {
    ascii_to_rank_lut_simd_impl<
        seq::dna4, unknown_byte_policy::map_to_default>(
            source, destination, size);
}

/// @brief Maps dna5 ASCII bytes and preserves the unknown sentinel.
void ascii_to_rank_lut_simd_dna5_drop_impl(
    const std::uint8_t* HWY_RESTRICT source,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t size) noexcept {
    ascii_to_rank_lut_simd_impl<seq::dna5, unknown_byte_policy::drop>(
        source, destination, size);
}

/// @brief Maps dna5 ASCII bytes and replaces unknown bytes with rank zero.
void ascii_to_rank_lut_simd_dna5_map_to_default_impl(
    const std::uint8_t* HWY_RESTRICT source,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t size) noexcept {
    ascii_to_rank_lut_simd_impl<
        seq::dna5, unknown_byte_policy::map_to_default>(
            source, destination, size);
}

}  // namespace detail

#undef BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
