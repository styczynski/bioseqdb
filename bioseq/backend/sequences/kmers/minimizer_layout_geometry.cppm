/**
 * @file minimizer_layout_geometry.cppm
 * @brief Calculates minimizer record widths for runtime selection and code generation.
 */
module;

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

export module bioseq.sequences.kmers.minimizer_layout_geometry;

export namespace bioseq::minimizers {

/** Return the storage width of a record without instantiating its C++ type. */
[[nodiscard]] constexpr std::size_t minimizer_record_storage_bits(
    const std::size_t position_log2,
    const std::size_t physical_kmer_size) noexcept {
    const auto hash_bits = static_cast<int>(
        physical_kmer_size < 28u ? 2u * physical_kmer_size : 56u);
    const auto position_bits = static_cast<int>(position_log2);
    return 16 - 1 - position_bits >= hash_bits ? 16u
         : 32 - 1 - position_bits >= hash_bits ? 32u
         : 64 - 1 - position_bits >= hash_bits ? 64u
                                              : 128u;
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
 * final bucket The smallest bucket starts at physical_kmer_size.
 */
[[nodiscard]] constexpr std::uint8_t minimizer_position_bucket(
    const std::uint8_t required_position_log2,
    const std::size_t physical_kmer_size) {
    if (physical_kmer_size == 0u || physical_kmer_size > 63u ||
        required_position_log2 > 63u) {
        throw std::invalid_argument("invalid minimizer record geometry");
    }
    if (required_position_log2 <= physical_kmer_size) {
        return static_cast<std::uint8_t>(physical_kmer_size);
    }
    for (std::size_t width = physical_kmer_size + 1u; width <= 63u; ++width) {
        if (minimizer_record_storage_bits(width, physical_kmer_size) !=
                minimizer_record_storage_bits(width - 1u, physical_kmer_size) &&
            required_position_log2 <= width - 1u) {
            return static_cast<std::uint8_t>(width - 1u);
        }
    }
    return 63u;
}

/** Choose a 15-, 28- or 63-bit position field while keeping a full 32-bit hash. */
[[nodiscard]] constexpr std::uint8_t minimizer_fixed_u32_position_bucket(
    const std::uint8_t required_position_log2) {
    if (required_position_log2 > 63u) {
        throw std::invalid_argument("invalid fixed-u32 minimizer record geometry");
    }
    return required_position_log2 <= 15u ? 15u
         : required_position_log2 <= 28u ? 28u
                                         : 63u;
}

static_assert(minimizer_record_storage_bits(28u, 15u) == 64u);
static_assert(minimizer_record_storage_bits(23u, 20u) == 64u);
static_assert(minimizer_record_storage_bits(24u, 20u) == 128u);
static_assert(minimizer_position_bucket(21u, 20u) == 23u);
static_assert(minimizer_position_bucket(24u, 20u) == 63u);
static_assert(minimizer_fixed_u32_position_bucket(29u) == 63u);

} // namespace bioseq::minimizers
