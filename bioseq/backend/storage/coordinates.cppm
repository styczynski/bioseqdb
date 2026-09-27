/**
 * @file
 * @brief Sequence orientation and slice-coordinate helpers.
 */
module;

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

export module bioseq.storage.coordinates;

export namespace bioseq::storage {

/** Logical orientation applied to a sequence view. */
enum class sequence_transform : std::uint8_t {
    identity = 0u,
    reversed = 1u,
    complemented = 2u,
    reverse_complement = 3u,
};

/** Compose two orientation transforms without inspecting sequence contents. */
[[nodiscard]] constexpr sequence_transform compose_sequence_transforms(
    sequence_transform outer,
    sequence_transform inner) noexcept
{
    const auto bits = std::to_underlying(outer) ^ std::to_underlying(inner);
    return static_cast<sequence_transform>(bits);
}

/** @brief Return whether a transform reverses logical coordinate order. */
[[nodiscard]] constexpr bool is_reversed(
    sequence_transform transform) noexcept
{
    return (std::to_underlying(transform) & 1u) != 0u;
}

/** @brief Return whether a transform complements alphabet ranks. */
[[nodiscard]] constexpr bool is_complemented(
    sequence_transform transform) noexcept
{
    return (std::to_underlying(transform) & 2u) != 0u;
}

/** Return a negative value's unsigned magnitude, including `INT64_MIN`. */
[[nodiscard]] constexpr std::uint64_t negative_magnitude(
    std::int64_t value) noexcept
{
    return std::uint64_t{0} - static_cast<std::uint64_t>(value);
}

/** Resolves a zero-based slice bound, counting negative bounds from the end and clamping to [0, length]. */
[[nodiscard]] constexpr std::uint64_t normalize_slice_bound(
    std::int64_t bound,
    std::uint64_t length) noexcept
{
    if (bound >= 0) {
        return std::min(static_cast<std::uint64_t>(bound), length);
    }
    const std::uint64_t magnitude = negative_magnitude(bound);
    return magnitude >= length ? 0u : length - magnitude;
}

/** Resolves a zero-based index. Negative indices count from the end, and invalid indices return nullopt. */
[[nodiscard]] constexpr std::optional<std::uint64_t> normalize_element_index(
    std::int64_t index,
    std::uint64_t length) noexcept
{
    if (index >= 0) {
        const auto value = static_cast<std::uint64_t>(index);
        return value < length ? std::optional<std::uint64_t>{value}
                              : std::nullopt;
    }
    const std::uint64_t magnitude = negative_magnitude(index);
    return magnitude <= length
        ? std::optional<std::uint64_t>{length - magnitude}
        : std::nullopt;
}

static_assert(compose_sequence_transforms(
                  sequence_transform::reversed,
                  sequence_transform::complemented)
              == sequence_transform::reverse_complement);
static_assert(compose_sequence_transforms(
                  sequence_transform::reverse_complement,
                  sequence_transform::reverse_complement)
              == sequence_transform::identity);

}  // namespace bioseq::storage
