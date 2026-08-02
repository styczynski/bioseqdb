/**
 * @file common/checked_arithmetic/index.cppm
 * @brief Dependency-free canonical checked unsigned arithmetic.
 */
module;

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <type_traits>

export module bioseq.common.checked_arithmetic;

export namespace bioseq::common {

/**
 * Stable error vocabulary shared by checked arithmetic and bounded byte
 * consumers. The final two values let a consumer preserve its public
 * range/decoding diagnostics without defining a second arithmetic error type.
 */
enum class CheckedArithmeticError : std::uint8_t {
    Overflow,
    OutOfBounds,
    InvalidAlignment,
    Truncated,
};

/** Add two unsigned integers without wrapping. */
template <class Left, class Right>
    requires(
        std::is_unsigned_v<Left>
        && std::is_unsigned_v<Right>)
[[nodiscard]] constexpr
std::optional<std::common_type_t<Left, Right>> checked_add(
    Left left,
    Right right) noexcept {
    using Integer = std::common_type_t<Left, Right>;
    const Integer converted_left = static_cast<Integer>(left);
    const Integer converted_right = static_cast<Integer>(right);
    const Integer maximum = (std::numeric_limits<Integer>::max)();
    if (converted_left > maximum - converted_right) {
        return std::nullopt;
    }
    return converted_left + converted_right;
}

/** Multiply two unsigned integers without wrapping. */
template <class Left, class Right>
    requires(
        std::is_unsigned_v<Left>
        && std::is_unsigned_v<Right>)
[[nodiscard]] constexpr
std::optional<std::common_type_t<Left, Right>> checked_mul(
    Left left,
    Right right) noexcept {
    using Integer = std::common_type_t<Left, Right>;
    const Integer converted_left = static_cast<Integer>(left);
    const Integer converted_right = static_cast<Integer>(right);
    if (converted_left == 0 || converted_right == 0) {
        return Integer{0};
    }
    const Integer maximum = (std::numeric_limits<Integer>::max)();
    if (converted_left > maximum / converted_right) {
        return std::nullopt;
    }
    return converted_left * converted_right;
}

/** Round a byte offset up to a power-of-two alignment without wrapping. */
[[nodiscard]] constexpr
std::expected<std::uint64_t, CheckedArithmeticError> checked_align_up(
    std::uint64_t value,
    std::uint64_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return std::unexpected(
            CheckedArithmeticError::InvalidAlignment);
    }
    const auto adjusted = checked_add(value, alignment - 1);
    if (!adjusted.has_value()) {
        return std::unexpected(CheckedArithmeticError::Overflow);
    }
    return *adjusted & ~(alignment - 1);
}

/** Validate a half-open byte range against a containing size. */
[[nodiscard]] constexpr bool checked_range(
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t total) noexcept {
    const auto end = checked_add(offset, length);
    return end.has_value() && *end <= total;
}

/** Subtract unsigned integers without wrapping below zero. */
[[nodiscard]] constexpr std::optional<std::uint64_t> checked_sub(
    std::uint64_t left,
    std::uint64_t right) noexcept {
    if (right > left) {
        return std::nullopt;
    }
    return left - right;
}

/** Add unsigned values and clamp overflow to UINT64_MAX. */
[[nodiscard]] constexpr std::uint64_t saturating_add(
    std::uint64_t left,
    std::uint64_t right) noexcept {
    return checked_add(left, right).value_or(
        (std::numeric_limits<std::uint64_t>::max)());
}

/** Multiply unsigned values and clamp overflow to UINT64_MAX. */
[[nodiscard]] constexpr std::uint64_t saturating_mul(
    std::uint64_t left,
    std::uint64_t right) noexcept {
    return checked_mul(left, right).value_or(
        (std::numeric_limits<std::uint64_t>::max)());
}

/** Integer expansion ratio and fixed grace allowance. */
struct expansion_ratio_limit {
    std::uint32_t numerator;
    std::uint32_t denominator;
    std::uint64_t grace_bytes;
};

/**
 * Compute grace_bytes + floor(encoded * numerator / denominator), rejecting
 * a zero denominator and every intermediate overflow.
 */
[[nodiscard]] constexpr std::optional<std::uint64_t> scaled_limit(
    std::uint64_t encoded_bytes,
    const expansion_ratio_limit& ratio) noexcept {
    if (ratio.denominator == 0) {
        return std::nullopt;
    }
    const auto scaled = checked_mul(
        encoded_bytes, static_cast<std::uint64_t>(ratio.numerator));
    if (!scaled.has_value()) {
        return std::nullopt;
    }
    const std::uint64_t quotient =
        *scaled / static_cast<std::uint64_t>(ratio.denominator);
    return checked_add(ratio.grace_bytes, quotient);
}

}  // namespace bioseq::common
