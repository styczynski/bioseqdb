/**
 * @file minimizer_layout_geometry.cppm
 * @brief Calculates minimizer record widths for runtime selection and code generation.
 */
module;

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <initializer_list>

export module bioseq.sequences.kmers.minimizer_layout_geometry;


export namespace bioseq::minimizers {

/**
 * Return the first position band in which a compact capacity is preferred.
 * Full-range hash families start at zero. A partial exact capacity follows the
 * reserved under-filled records of its rounded carrier, preserving their typed
 * routes and physical-maximum policy. Shared by codegen and generic dispatch.
 */
template<class p_record>
    requires requires(const p_record& record) {
        record.position_log2;
        record.hash_bits;
        record.storage_bits;
    }
[[nodiscard]] constexpr std::uint8_t minimizer_capacity_minimum_position(
    const std::span<const p_record> records, const p_record& record) {
    constexpr auto maximum_position_bits = std::numeric_limits<std::uint64_t>::digits - 1u;
    for (const auto& candidate : records) {
        if (candidate.hash_bits == record.hash_bits
            && candidate.position_log2 == maximum_position_bits) return 0u;
    }
    const auto carrier_bits = std::bit_ceil(static_cast<unsigned>(record.hash_bits));
    std::uint8_t minimum_position{};
    for (const auto& candidate : records) {
        if (candidate.hash_bits == carrier_bits && candidate.storage_bits == record.storage_bits
            && 1u + candidate.position_log2 + carrier_bits < candidate.storage_bits) {
            minimum_position = std::max(minimum_position,
                static_cast<std::uint8_t>(candidate.position_log2 + 1u));
        }
    }
    return minimum_position;
}

/** Return the storage width of a record without instantiating its C++ type. */
[[nodiscard]] constexpr std::size_t minimizer_record_storage_bits(
    const std::size_t position_log2,
    const std::size_t hash_bits) noexcept {
    const std::size_t required_bits = 1u + position_log2 + hash_bits;
    return required_bits <= 16u ? 16u
         : required_bits <= 32u ? 32u
         : required_bits <= 64u ? 64u : 128u;
}

/**
 * Return  the bits needed for positions in a sequnce of up to maximum_sequence_bp symbols.
 * Uses at least one bit and throws std::length_error above 2^63 symbols.
 */
[[nodiscard]] constexpr std::uint8_t minimizer_position_requirement(
    const std::uint64_t maximum_sequence_bp) {
    if (maximum_sequence_bp > (std::uint64_t{1u} << 63u)) {
        throw std::length_error("minimizer positions exceed the 63-bit record limit");
    }
    return static_cast<std::uint8_t>(std::bit_width(
        maximum_sequence_bp > 1u ? maximum_sequence_bp - 1u : 1u));
}

/**
 * Choose a supported position-field size that covers the required bits.
 * Buckets end where the record needs a wider integer, with 63 bits as the
 * final bucket Buckets use the position capacity left by the requested hash width.
 */
[[nodiscard]] constexpr std::uint8_t minimizer_position_bucket(
    const std::uint8_t required_position_log2,
    const std::size_t hash_bits) {
    if (hash_bits == 0u || hash_bits > 64u || required_position_log2 > 63u) {
        throw std::invalid_argument("invalid minimizer record geometry");
    }
    for (const std::size_t storage_bits : {16u, 32u, 64u}) {
        if (storage_bits > hash_bits + 1u) {
            const auto capacity = storage_bits - hash_bits - 1u;
            if (required_position_log2 <= capacity) {
                return static_cast<std::uint8_t>(capacity);
            }
        }
    }
    return 63u;
}

/** Choose the smallest physical position capacity holding a full 32-bit hash. */
[[nodiscard]] constexpr std::uint8_t minimizer_fixed_u32_position_bucket(
    const std::uint8_t required_position_log2) {
    return minimizer_position_bucket(required_position_log2, 32u);
}

static_assert(minimizer_record_storage_bits(28u, 32u) == 64u);
static_assert(minimizer_record_storage_bits(23u, 40u) == 64u);
static_assert(minimizer_record_storage_bits(24u, 40u) == 128u);
static_assert(minimizer_record_storage_bits(63u, 64u) == 128u);
static_assert(minimizer_position_bucket(21u, 40u) == 23u);
static_assert(minimizer_position_bucket(24u, 40u) == 63u);
static_assert(minimizer_fixed_u32_position_bucket(29u) == 31u);

} // namespace bioseq::minimizers
