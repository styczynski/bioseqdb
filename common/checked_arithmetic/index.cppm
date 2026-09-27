/**
 * @file common/checked_arithmetic/index.cppm
 * @brief Unsigned arithmetic with overflow checks and helpers for byte limits.
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
 * Errors shared by arithmetic checks and byte readers.
 *
 * Readers can also report a range outside the buffer or truncated input through
 * this type.
 */
enum class checked_arithmetic_error : std::uint8_t {
    overflow,
    out_of_bounds,
    invalid_alignment,
    truncated,
};

/** Add unsigned integers, returning no value if the sum would overflow. */
template <class p_left_type, class p_right_type>
    requires(
        std::is_unsigned_v<p_left_type>
        && std::is_unsigned_v<p_right_type>)
[[nodiscard]] constexpr
std::optional<std::common_type_t<p_left_type, p_right_type>> checked_add(
    p_left_type left,
    p_right_type right) noexcept {
    using integer_type = std::common_type_t<p_left_type, p_right_type>;
    const integer_type converted_left = static_cast<integer_type>(left);
    const integer_type converted_right = static_cast<integer_type>(right);
    const integer_type maximum = (std::numeric_limits<integer_type>::max)();
    if (converted_left > maximum - converted_right) {
        return std::nullopt;
    }
    return converted_left + converted_right;
}

/** Multiply unsigned integers, returning no value if the product would overflow. */
template <class p_left_type, class p_right_type>
    requires(
        std::is_unsigned_v<p_left_type>
        && std::is_unsigned_v<p_right_type>)
[[nodiscard]] constexpr
std::optional<std::common_type_t<p_left_type, p_right_type>> checked_mul(
    p_left_type left,
    p_right_type right) noexcept {
    using integer_type = std::common_type_t<p_left_type, p_right_type>;
    const integer_type converted_left = static_cast<integer_type>(left);
    const integer_type converted_right = static_cast<integer_type>(right);
    if (converted_left == 0 || converted_right == 0) {
        return integer_type{0};
    }
    const integer_type maximum =
        (std::numeric_limits<integer_type>::max)();
    if (converted_left > maximum / converted_right) {
        return std::nullopt;
    }
    return converted_left * converted_right;
}

/**
 * Divide unsigned integers and round up. A zero divisor returns zero.
 * Using the remainder avoids the overflow that adding divisor - 1 could cause.
 */
template <class p_dividend_type, class p_divisor_type>
    requires(
        std::is_unsigned_v<p_dividend_type>
        && std::is_unsigned_v<p_divisor_type>)
[[nodiscard]] constexpr
std::common_type_t<p_dividend_type, p_divisor_type> ceil_div(
    p_dividend_type dividend,
    p_divisor_type divisor) noexcept {
    using integer_type =
        std::common_type_t<p_dividend_type, p_divisor_type>;
    const integer_type converted_divisor =
        static_cast<integer_type>(divisor);
    if (converted_divisor == 0) {
        return integer_type{0};
    }
    const integer_type converted_dividend =
        static_cast<integer_type>(dividend);
    const integer_type quotient = converted_dividend / converted_divisor;
    const integer_type remainder = converted_dividend % converted_divisor;
    return quotient + static_cast<integer_type>(remainder != 0);
}

/** Round a byte offset up to a power-of-two alignment without wrapping. */
[[nodiscard]] constexpr
std::expected<std::uint64_t, checked_arithmetic_error> checked_align_up(
    std::uint64_t value,
    std::uint64_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return std::unexpected(
            checked_arithmetic_error::invalid_alignment);
    }
    const auto adjusted = checked_add(value, alignment - 1);
    if (!adjusted.has_value()) {
        return std::unexpected(checked_arithmetic_error::overflow);
    }
    return *adjusted & ~(alignment - 1);
}

/** Check that offset and length describe a byte range wholly within total. */
[[nodiscard]] constexpr bool checked_range(
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t total) noexcept {
    const auto end = checked_add(offset, length);
    return end.has_value() && *end <= total;
}

/** Subtract unsigned integers, returning no value if the result would be negative. */
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

/** Allowed decoded-to-encoded size ratio, plus a fixed allowance in bytes. */
struct expansion_ratio_limit {
    std::uint32_t numerator;
    std::uint32_t denominator;
    std::uint64_t grace_bytes;
};

/**
 * Compute the allowed decoded size from the encoded size and expansion ratio.
 * Add grace_bytes after rounding the scaled size down. Return no value for a
 * zero denominator or overflow, including overflow before division.
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
