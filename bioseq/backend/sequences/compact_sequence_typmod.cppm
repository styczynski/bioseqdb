/**
 * @file compact_sequence_typmod.cppm
 * @brief Compact-sequence PostgreSQL typmod encoding shared across front ends.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.sequences.compact_sequence_typmod;

import std;
import bioseq.common.logging;

/**
 * @brief Runtime type parameters for a standard-layout compact sequence.
 *
 * The encoded representation stores the alphabet code in the next byte and
 * the base-two logarithm of the storage-unit byte width in the low byte.
 */
export struct compact_sequence_stdc_type_params {
    /** @brief Index of the alphabet in the owning supported-alphabet set. */
    std::uint8_t alphabet_code;

    /** @brief Width of one packed storage unit in bytes. */
    std::uint8_t storage_unit_byte_width;

    /**
     * @brief Encode the parameters as a PostgreSQL type modifier.
     * @return The compact signed 32-bit type modifier.
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
     * @brief Decode a PostgreSQL type modifier.
     * @param typmod Encoded type modifier produced by encode().
     * @return Decoded alphabet and storage-width parameters.
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
