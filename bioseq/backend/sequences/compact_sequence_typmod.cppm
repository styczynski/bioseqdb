/**
 * @file compact_sequence_typmod.cppm
 * @brief Store a sequence's alphabet and packing width in its PostgreSQL type modifier.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.sequences.compact_sequence_typmod;

import std;
import bioseq.common.logging;

/**
 * @brief Alphabet and storage width for a compact sequence.
 *
 * The type modifier keeps the  alphabet code in bits 8-15. The low byte holds
 * 0, 1, 2, or 3 for storage units of 1, 2, 4, or 8 bytes.
 */
export struct compact_sequence_stdc_type_params {
    /** @brief Alphabet number in the caller's supported-alphabet list. */
    std::uint8_t alphabet_code;

    /** @brief Width of one packed storage unit in bytes. */
    std::uint8_t storage_unit_byte_width;

    /**
     * @brief Pack the alphabet and width into a PostgreSQL type modifier.
     * @pre The storage-unit width is 1, 2, 4, or 8 bytes.
     * @return Encoded type modifier.
     */
    [[nodiscard]] constexpr std::int32_t encode() const noexcept {
        const std::uint8_t width_code =
              storage_unit_byte_width == 1 ? 0u
            : storage_unit_byte_width == 2 ? 1u
            : storage_unit_byte_width == 4 ? 2u
            : storage_unit_byte_width == 8 ? 3u
            : 0xFFu;
        DEBUG_ASSERT(width_code != 0xFFu,
                     fmt::format("encode: storage_unit_byte_width {} invalid",
                                 static_cast<unsigned>(storage_unit_byte_width)));
        return (static_cast<std::int32_t>(alphabet_code) << 8) | width_code;
    }

    /**
     * @brief Read the alphabet and width from an encoded type modifier.
     * @param typmod Value produced by encode(), with a width code from 0 to 3.
     * @return Alphabet code and storage-unit width in bytes.
     */
    [[nodiscard]] static constexpr compact_sequence_stdc_type_params
    decode(std::int32_t typmod) noexcept {
        const std::uint8_t width_code =
            static_cast<std::uint8_t>(typmod & 0xFF);
        DEBUG_ASSERT(width_code <= 3u,
                     fmt::format("decode: width_code {} > 3",
                                 static_cast<unsigned>(width_code)));
        return {
            .alphabet_code           = static_cast<std::uint8_t>((typmod >> 8) & 0xFF),
            .storage_unit_byte_width = static_cast<std::uint8_t>(1u << width_code),
        };
    }

    /** @brief Compares alphabet and storage-width parameters. */
    [[nodiscard]] constexpr bool operator==(
        const compact_sequence_stdc_type_params&) const = default;
};
