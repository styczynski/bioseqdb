/**
 * @file external/provider/rate_limit.cppm
 * @brief Process-local request pacing keyed by provider policy identity.
 */
module;

/// Process-local GCRA request policy with fixed-capacity atomic buckets.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>

export module bioseq.external.provider.rate_limit;

import bioseq.external.provider.errors;
import bioseq.external.provider.runtime_services;

export namespace bioseq::external::provider {

/// Process-local GCRA request pacing and retry policy.
class gcra_request_policy {
  public:
    static constexpr std::size_t k_bucket_count = 1024;

    /// Constructs a request policy using the injected monotonic clock.
    explicit gcra_request_policy(external_clock_ref clock) noexcept : clock_(clock) {}

    /// Acquires pacing and in-flight admission for one request attempt.
    std::expected<request_policy_result, external_error> acquire(const policy_request_view& request,
                                                                 std::stop_token) {
        const request_rate_tier_view* tier = find_tier(request);
        if (tier == nullptr) {
            // No rate tier constrains this request — proceed with no slot.
            return request_policy_result{
                request_policy_action::proceed, std::chrono::nanoseconds{0}, std::nullopt};
        }
        const std::int64_t now = now_ns();
        const std::int64_t spacing = effective_spacing(*tier);
        const std::int64_t burst_tolerance =
            saturating_mul(spacing, static_cast<std::int64_t>(tier->burst));
        const std::size_t index = bucket_index(request);
        bucket& b = buckets_[index];

        // 1) Reserve an in-flight slot on every proceeding request. The cap is
        // enforced only when maximum_in_flight > 0, but the counter is always
        // tracked so observe/abandon release symmetrically by token — independent
        // of what the tier reads at release time (no leak if the profile mutates).
        {
            std::uint32_t cur = b.in_flight.load(std::memory_order_acquire);
            for (;;) {
                if (tier->maximum_in_flight > 0 && cur >= tier->maximum_in_flight) {
                    return retry(now + spacing);
                }
                if (b.in_flight.compare_exchange_weak(
                        cur, cur + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
            }
        }

        // 2) GCRA: eligible when now >= max(tat - burst_tolerance, penalty).
        std::int64_t tat = b.tat_ns.load(std::memory_order_acquire);
        for (;;) {
            const std::int64_t penalty = b.penalty_until_ns.load(std::memory_order_acquire);
            const std::int64_t earliest = std::max(tat - burst_tolerance, penalty);
            if (now < earliest) {
                release_slot(b); // roll back the reserved slot
                return retry(earliest);
            }
            const std::int64_t new_tat = (tat > now ? tat : now) + spacing;
            if (b.tat_ns.compare_exchange_weak(
                    tat, new_tat, std::memory_order_acq_rel, std::memory_order_acquire)) {
                // A monotonic per-bucket admission tag carried in the token for
                // telemetry / attempt correlation. It is NOT an ABA guard: release
                // (observe/abandon) is a fungible in-flight decrement keyed only by
                // slot, so the generation is not re-checked and double-release is a
                // caller-contract violation, not something this counter prevents.
                const std::uint64_t gen = b.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                return request_policy_result{
                    request_policy_action::proceed,
                    std::chrono::nanoseconds{0},
                    policy_attempt_token{static_cast<std::uint64_t>(index), gen}};
            }
            // CAS lost: `tat` reloaded with the current value — re-evaluate.
        }
    }

    /// Applies response pacing observations and releases attempt admission.
    std::expected<request_policy_result, external_error>
    observe(policy_attempt_token token,
            const policy_request_view& request,
            const policy_response_view& response) {
        bucket& b = buckets_[token.slot % k_bucket_count];
        // A 429/Retry-After (or a server-advertised spacing) pushes the penalty
        // floor forward so no backend proceeds before it, clamped to the profile's
        // maximum_retry_after so a hostile header cannot wedge the bucket.
        const std::int64_t now = now_ns();
        std::int64_t penalty_target = 0;
        if (response.rate.retry_at.has_value()) {
            penalty_target = response.rate.retry_at->count();
        } else if (response.rate.advertised_spacing.has_value()) {
            penalty_target = now + response.rate.advertised_spacing->count();
        }
        const std::int64_t cap = request.profile.maximum_retry_after.count();
        if (cap > 0 && penalty_target > now + cap) {
            penalty_target = now + cap;
        }
        if (penalty_target > 0) {
            std::int64_t cur = b.penalty_until_ns.load(std::memory_order_acquire);
            while (penalty_target > cur &&
                   !b.penalty_until_ns.compare_exchange_weak(
                       cur, penalty_target, std::memory_order_acq_rel, std::memory_order_acquire)) {
            }
        }
        release_slot(b);
        return request_policy_result{
            request_policy_action::proceed, std::chrono::nanoseconds{0}, std::nullopt};
    }

    /// Releases admission for an attempt without a response observation.
    void abandon(policy_attempt_token token, const policy_request_view& /*request*/) noexcept {
        release_slot(buckets_[token.slot % k_bucket_count]);
    }

    /// Clears all request-policy buckets for isolated tests.
    void reset_for_tests() noexcept {
        for (bucket& b : buckets_) {
            b.tat_ns.store(0, std::memory_order_relaxed);
            b.penalty_until_ns.store(0, std::memory_order_relaxed);
            b.in_flight.store(0, std::memory_order_relaxed);
            b.generation.store(0, std::memory_order_relaxed);
        }
    }

    /// Returns a non-owning erased request-policy reference.
    [[nodiscard]] request_policy_ref ref() noexcept {
        return request_policy_ref{
            this, &acquire_thunk, &observe_thunk, &abandon_thunk, &reset_thunk};
    }

    /// Returns the in-flight count for the request's test bucket.
    [[nodiscard]] std::uint32_t
    in_flight_for_tests(const policy_request_view& request) const noexcept {
        return buckets_[bucket_index(request)].in_flight.load(std::memory_order_acquire);
    }

  private:
    /// Atomic state retained for one hashed request-policy identity.
    struct bucket {
        std::atomic<std::int64_t> tat_ns{0};
        std::atomic<std::int64_t> penalty_until_ns{0};
        std::atomic<std::uint32_t> in_flight{0};
        std::atomic<std::uint64_t> generation{0};
    };
    static_assert(std::atomic<std::int64_t>::is_always_lock_free,
                  "GCRA TAT word must be lock-free");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "GCRA in-flight counter must be lock-free");

    /// Returns the injected monotonic time as a nanosecond count.
    [[nodiscard]] std::int64_t now_ns() const noexcept {
        return clock_.monotonic_now_fn(clock_.object).count();
    }

    /// Returns the active spacing floor for a request tier.
    [[nodiscard]] static std::int64_t
    effective_spacing(const request_rate_tier_view& tier) noexcept {
        const std::int64_t minimum = tier.minimum_spacing.count();
        return minimum > 0 ? minimum : tier.initial_spacing.count();
    }

    /// Multiplies a non-negative burst window and clamps signed overflow.
    [[nodiscard]] static std::int64_t saturating_mul(std::int64_t lhs, std::int64_t rhs) noexcept {
        if (lhs <= 0 || rhs <= 0) {
            return 0;
        }
        constexpr std::int64_t k_maximum = std::numeric_limits<std::int64_t>::max();
        return lhs > k_maximum / rhs ? k_maximum : lhs * rhs;
    }

    /// Finds the named rate tier, or returns null when none applies.
    [[nodiscard]] const request_rate_tier_view*
    find_tier(const policy_request_view& request) const noexcept {
        for (const request_rate_tier_view& tier : request.profile.rate_tiers) {
            if (tier.name == request.rate_tier) {
                return &tier;
            }
        }
        return nullptr;
    }

    /// Hashes origin, credential identity, and tier into a conservative shared bucket.
    [[nodiscard]] std::size_t bucket_index(const policy_request_view& request) const noexcept {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](unsigned char byte) noexcept {
            h ^= byte;
            h *= 1099511628211ull;
        };
        for (const char c : request.normalized_origin) {
            mix(static_cast<unsigned char>(c));
        }
        for (const std::byte c : request.credential_identity) {
            mix(static_cast<unsigned char>(c));
        }
        for (const char c : request.rate_tier) {
            mix(static_cast<unsigned char>(c));
        }
        return static_cast<std::size_t>(h % k_bucket_count);
    }

    /// Releases one reserved in-flight slot and remains idempotent at zero.
    static void release_slot(bucket& b) noexcept {
        std::uint32_t cur = b.in_flight.load(std::memory_order_acquire);
        while (cur > 0 && !b.in_flight.compare_exchange_weak(
                              cur, cur - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    }

    /// Returns a retry decision for an absolute monotonic deadline.
    [[nodiscard]] static request_policy_result retry(std::int64_t at_ns) noexcept {
        return request_policy_result{
            request_policy_action::retry_at, std::chrono::nanoseconds{at_ns}, std::nullopt};
    }

    /// Adapts erased acquisition to this policy instance.
    static std::expected<request_policy_result, external_error>
    acquire_thunk(void* self, const policy_request_view& request, std::stop_token stop) {
        return static_cast<gcra_request_policy*>(self)->acquire(request, std::move(stop));
    }
    /// Adapts erased response observation to this policy instance.
    static std::expected<request_policy_result, external_error>
    observe_thunk(void* self,
                  policy_attempt_token token,
                  const policy_request_view& request,
                  const policy_response_view& response) {
        return static_cast<gcra_request_policy*>(self)->observe(token, request, response);
    }
    /// Adapts erased attempt abandonment to this policy instance.
    static void abandon_thunk(void* self,
                              policy_attempt_token token,
                              const policy_request_view& request) noexcept {
        static_cast<gcra_request_policy*>(self)->abandon(token, request);
    }
    /// Adapts erased test reset to this policy instance.
    static void reset_thunk(void* self) noexcept {
        static_cast<gcra_request_policy*>(self)->reset_for_tests();
    }

    external_clock_ref clock_;
    std::array<bucket, k_bucket_count> buckets_{};
};

} // namespace bioseq::external::provider
