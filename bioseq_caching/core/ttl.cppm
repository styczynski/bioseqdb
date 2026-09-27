/**
 * @file bioseq_caching/core/ttl.cppm
 * @brief Cache expiration rules, including the first read by an existing waiter.
 */
module;

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

export module bioseq.cache.core:ttl;
import :checked_math;

export namespace bioseq::cache {

struct cache_timestamps {
    std::uint64_t started_ns = 0;
    std::optional<std::uint64_t> completed_ns;
};

struct ttl_decision {
    bool readable = false;
    bool consume_just_completed_entitlement = false;
};

/** Return the elapsed time since completion, or no value if the clock moved backwards. */
[[nodiscard]] constexpr std::optional<std::uint64_t>
monotonic_age_ns(std::uint64_t completed_ns, std::uint64_t now_ns) noexcept {
    if (now_ns < completed_ns) {
        return std::nullopt;
    }
    return now_ns - completed_ns;
}

/** Convert milliseconds to nanoseconds, using the largest uint64_t value on overflow. */
[[nodiscard]] constexpr std::uint64_t ttl_nanoseconds(std::uint64_t ttl_ms) noexcept {
    const auto value = checked_mul(ttl_ms, std::uint64_t{1'000'000});
    return value.value_or((std::numeric_limits<std::uint64_t>::max)());
}

/** Use the shorter time limit for reusing a result, as set by the producer and reader. */
[[nodiscard]] constexpr std::uint64_t effective_ttl_ms(std::uint64_t producer_ttl_ms,
                                                       std::uint64_t consumer_ttl_ms) noexcept {
    return std::min(producer_ttl_ms, consumer_ttl_ms);
}

/** A caller can wait for an unfinished build regardless of how long its result can be reused. */
[[nodiscard]] constexpr bool eligible_inflight(std::uint64_t) noexcept {
    return true;
}

/** A zero-TTL generation is expired at the instant it completes. */
[[nodiscard]] constexpr bool expired_at_completion(std::uint64_t ttl_ms) noexcept {
    return ttl_ms == 0;
}

/** Check whether a completed entry has expired for an ordinary reader. */
[[nodiscard]] constexpr bool expired(const cache_timestamps& timestamps,
                                     std::uint64_t producer_ttl_ms,
                                     std::uint64_t consumer_ttl_ms,
                                     std::uint64_t now_ns) noexcept {
    if (!timestamps.completed_ns.has_value()) {
        return false;
    }
    const auto age = monotonic_age_ns(*timestamps.completed_ns, now_ns);
    if (!age.has_value()) {
        return true;
    }
    return *age >= ttl_nanoseconds(effective_ttl_ms(producer_ttl_ms, consumer_ttl_ms));
}

/**
 * Check expiration and allow an existing waiter one read when the TTL is zero.
 * The caller must still validate the generation and its dependencies.
 */
[[nodiscard]] constexpr ttl_decision
completed_ttl_decision(const cache_timestamps& timestamps,
                       std::uint64_t producer_ttl_ms,
                       std::uint64_t consumer_ttl_ms,
                       std::uint64_t now_ns,
                       bool registered_inflight_waiter,
                       bool entitlement_already_consumed) noexcept {
    if (!timestamps.completed_ns.has_value()) {
        return ttl_decision{};
    }
    if (!expired(timestamps, producer_ttl_ms, consumer_ttl_ms, now_ns)) {
        return ttl_decision{.readable = true};
    }
    const bool one_time_entitlement = registered_inflight_waiter && !entitlement_already_consumed &&
                                      effective_ttl_ms(producer_ttl_ms, consumer_ttl_ms) == 0 &&
                                      now_ns >= *timestamps.completed_ns;
    return ttl_decision{
        .readable = one_time_entitlement,
        .consume_just_completed_entitlement = one_time_entitlement,
    };
}

} // namespace bioseq::cache
