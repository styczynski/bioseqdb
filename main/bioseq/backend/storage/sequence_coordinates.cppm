/*-------------------------------------------------------------------------
 *
 * storage/sequence_coordinates.cppm
 *
 * PostgreSQL-free logical sequence transform and coordinate contracts.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

export module bioseq.storage.coordinates;

export namespace bioseq::storage {

enum class sequence_transform : std::uint8_t {
    identity           = 0,
    reversed           = 1,
    complemented       = 2,
    reverse_complement = 3,
};

[[nodiscard]] constexpr sequence_transform compose(
        sequence_transform outer,
        sequence_transform inner) noexcept {
    const auto bits = std::to_underlying(outer) ^ std::to_underlying(inner);
    return static_cast<sequence_transform>(bits);
}

[[nodiscard]] constexpr bool has_reverse(sequence_transform value) noexcept {
    return (std::to_underlying(value) & 1u) != 0u;
}

[[nodiscard]] constexpr bool has_complement(sequence_transform value) noexcept {
    return (std::to_underlying(value) & 2u) != 0u;
}

/// Precondition: value < 0. Unsigned subtraction avoids `-INT64_MIN`.
[[nodiscard]] constexpr std::uint64_t magnitude_of_negative(
        std::int64_t value) noexcept {
    return std::uint64_t{0} - static_cast<std::uint64_t>(value);
}

[[nodiscard]] constexpr std::uint64_t normalize_slice_bound(
        std::int64_t bound,
        std::uint64_t length) noexcept {
    if (bound >= 0) {
        return std::min(static_cast<std::uint64_t>(bound), length);
    }
    const std::uint64_t magnitude = magnitude_of_negative(bound);
    return magnitude >= length ? 0u : length - magnitude;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> normalize_element_index(
        std::int64_t index,
        std::uint64_t length) noexcept {
    if (index >= 0) {
        const auto value = static_cast<std::uint64_t>(index);
        return value < length ? std::optional<std::uint64_t>{value}
                              : std::nullopt;
    }
    const std::uint64_t magnitude = magnitude_of_negative(index);
    return magnitude <= length
        ? std::optional<std::uint64_t>{length - magnitude}
        : std::nullopt;
}

static_assert(compose(sequence_transform::reversed,
                      sequence_transform::complemented)
              == sequence_transform::reverse_complement);
static_assert(compose(sequence_transform::reverse_complement,
                      sequence_transform::reverse_complement)
              == sequence_transform::identity);

}  // namespace bioseq::storage
