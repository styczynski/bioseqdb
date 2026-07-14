module;

#include <cstdint>
#include <optional>

export module bioseq.common.checked_arithmetic;

// DOC0372: lightweight, dependency-free owner of the checked/saturating/scaled
// arithmetic helpers and the expansion-ratio POD. Both the existing memory
// admission controller and the external provider budget factory import this leaf
// instead of re-deriving overflow-safe math. It links the standard library only:
// no runtime, TBB, logging, external, IO, or PostgreSQL edge.
export namespace bioseq::common {

// Saturating unsigned add/multiply: clamp to UINT64_MAX instead of wrapping.
[[nodiscard]] constexpr std::uint64_t saturating_add(
    std::uint64_t lhs, std::uint64_t rhs) noexcept {
    const std::uint64_t sum = lhs + rhs;
    if (sum < lhs) {
        return UINT64_MAX;
    }
    return sum;
}

[[nodiscard]] constexpr std::uint64_t saturating_mul(
    std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    if (lhs > UINT64_MAX / rhs) {
        return UINT64_MAX;
    }
    return lhs * rhs;
}

// Checked unsigned add/multiply: disengaged optional signals overflow so the
// caller fails an admission/limit computation instead of silently wrapping.
[[nodiscard]] constexpr std::optional<std::uint64_t> checked_add(
    std::uint64_t lhs, std::uint64_t rhs) noexcept {
    const std::uint64_t sum = lhs + rhs;
    if (sum < lhs) {
        return std::nullopt;
    }
    return sum;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_mul(
    std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (lhs == 0 || rhs == 0) {
        return std::uint64_t{0};
    }
    if (lhs > UINT64_MAX / rhs) {
        return std::nullopt;
    }
    return lhs * rhs;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_sub(
    std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (rhs > lhs) {
        return std::nullopt;
    }
    return lhs - rhs;
}

// Expansion-ratio POD shared by provider budgets and gzip decode limits. The
// denominator is required to be non-zero by every consumer.
struct expansion_ratio_limit {
    std::uint32_t numerator;
    std::uint32_t denominator;  // Non-zero.
    std::uint64_t grace_bytes;
};

// Checked scaled limit: grace_bytes + floor(encoded * numerator / denominator).
// No floating point is used; overflow anywhere disengages the optional. This is
// the exact decoded-bytes ceiling used by gzip decode and archive-member checks.
[[nodiscard]] constexpr std::optional<std::uint64_t> scaled_limit(
    std::uint64_t encoded_bytes,
    const expansion_ratio_limit& ratio) noexcept {
    if (ratio.denominator == 0) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> scaled =
        checked_mul(encoded_bytes, static_cast<std::uint64_t>(ratio.numerator));
    if (!scaled.has_value()) {
        return std::nullopt;
    }
    const std::uint64_t quotient =
        *scaled / static_cast<std::uint64_t>(ratio.denominator);
    return checked_add(ratio.grace_bytes, quotient);
}

}  // namespace bioseq::common
