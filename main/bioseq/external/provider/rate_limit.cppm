module;

// DOC0372 §3348-3369 — the standalone (process-local) GCRA request policy. The
// erased `request_policy_ref` interface already exists in runtime_services; this
// is the concrete algorithm behind it. Fixed-capacity buckets keyed by
// tier/origin/credential; each bucket is a lock-free GCRA cell — one
// theoretical-arrival-time (TAT) word, a response-penalty floor, and an
// in-flight count. `acquire` first reserves an in-flight slot, then advances the
// TAT only when eligible *now*, rolling the slot back on ineligibility and
// returning the absolute retry time. `observe` pushes a penalty on 429/Retry-After
// and always releases the slot. The cluster-wide PostgreSQL variant (§3352-3379)
// places these same atomic words in supervisor_pg shared memory; the arithmetic
// is identical and is what that wraps — it is deferred here (needs the shared-mem
// hook + a multiprocess backend harness).

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

// A process-local GCRA request policy satisfying `request_policy_ref`. Lock-free;
// no PostgreSQL API. Trivially placeable in shared memory (all state is atomics
// in a fixed array) for the cluster-wide extension.
class gcra_request_policy {
public:
    static constexpr std::size_t kBucketCount = 1024;

    explicit gcra_request_policy(external_clock_ref clock) noexcept
        : clock_(clock) {}

    std::expected<request_policy_result, external_error> acquire(
        const policy_request_view& request, std::stop_token) {
        const request_rate_tier_view* tier = find_tier(request);
        if (tier == nullptr) {
            // No rate tier constrains this request — proceed with no slot.
            return request_policy_result{request_policy_action::proceed,
                                         std::chrono::nanoseconds{0},
                                         std::nullopt};
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
                if (tier->maximum_in_flight > 0 &&
                    cur >= tier->maximum_in_flight) {
                    return retry(now + spacing);
                }
                if (b.in_flight.compare_exchange_weak(
                        cur, cur + 1, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    break;
                }
            }
        }

        // 2) GCRA: eligible when now >= max(tat - burst_tolerance, penalty).
        std::int64_t tat = b.tat_ns.load(std::memory_order_acquire);
        for (;;) {
            const std::int64_t penalty =
                b.penalty_until_ns.load(std::memory_order_acquire);
            const std::int64_t earliest =
                std::max(tat - burst_tolerance, penalty);
            if (now < earliest) {
                release_slot(b);  // roll back the reserved slot
                return retry(earliest);
            }
            const std::int64_t new_tat = (tat > now ? tat : now) + spacing;
            if (b.tat_ns.compare_exchange_weak(tat, new_tat,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                // A monotonic per-bucket admission tag carried in the token for
                // telemetry / attempt correlation. It is NOT an ABA guard: release
                // (observe/abandon) is a fungible in-flight decrement keyed only by
                // slot, so the generation is not re-checked and double-release is a
                // caller-contract violation, not something this counter prevents.
                const std::uint64_t gen =
                    b.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                return request_policy_result{
                    request_policy_action::proceed, std::chrono::nanoseconds{0},
                    policy_attempt_token{static_cast<std::uint64_t>(index), gen}};
            }
            // CAS lost: `tat` reloaded with the current value — re-evaluate.
        }
    }

    std::expected<request_policy_result, external_error> observe(
        policy_attempt_token token, const policy_request_view& request,
        const policy_response_view& response) {
        bucket& b = buckets_[token.slot % kBucketCount];
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
                       cur, penalty_target, std::memory_order_acq_rel,
                       std::memory_order_acquire)) {
            }
        }
        release_slot(b);
        return request_policy_result{request_policy_action::proceed,
                                     std::chrono::nanoseconds{0}, std::nullopt};
    }

    void abandon(policy_attempt_token token,
                 const policy_request_view& /*request*/) noexcept {
        release_slot(buckets_[token.slot % kBucketCount]);
    }

    void reset_for_tests() noexcept {
        for (bucket& b : buckets_) {
            b.tat_ns.store(0, std::memory_order_relaxed);
            b.penalty_until_ns.store(0, std::memory_order_relaxed);
            b.in_flight.store(0, std::memory_order_relaxed);
            b.generation.store(0, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] request_policy_ref ref() noexcept {
        return request_policy_ref{this, &acquire_thunk, &observe_thunk,
                                  &abandon_thunk, &reset_thunk};
    }

    // Test-only: the live in-flight count for a request's bucket. After every
    // acquire is balanced by an observe/abandon this returns to 0 (leak check).
    [[nodiscard]] std::uint32_t in_flight_for_tests(
        const policy_request_view& request) const noexcept {
        return buckets_[bucket_index(request)].in_flight.load(
            std::memory_order_acquire);
    }

private:
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

    [[nodiscard]] std::int64_t now_ns() const noexcept {
        return clock_.monotonic_now_fn(clock_.object).count();
    }

    [[nodiscard]] static std::int64_t effective_spacing(
        const request_rate_tier_view& tier) noexcept {
        const std::int64_t minimum = tier.minimum_spacing.count();
        return minimum > 0 ? minimum : tier.initial_spacing.count();
    }

    // Saturating multiply for the burst window (spacing × burst). Both operands
    // are non-negative here (spacing is a ns count; burst a uint32); a pathological
    // tier can still overflow int64, so clamp instead of wrapping to a negative
    // tolerance that would make every request appear eligible.
    [[nodiscard]] static std::int64_t saturating_mul(std::int64_t lhs,
                                                      std::int64_t rhs) noexcept {
        if (lhs <= 0 || rhs <= 0) {
            return 0;
        }
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        return lhs > kMax / rhs ? kMax : lhs * rhs;
    }

    [[nodiscard]] const request_rate_tier_view* find_tier(
        const policy_request_view& request) const noexcept {
        for (const request_rate_tier_view& tier : request.profile.rate_tiers) {
            if (tier.name == request.rate_tier) {
                return &tier;
            }
        }
        return nullptr;
    }

    // FNV-1a over origin + credential digest + tier name — the quota identity
    // (spec's policy/tier/origin/digest key). Collisions share the stricter
    // bucket, which is conservative.
    [[nodiscard]] std::size_t bucket_index(
        const policy_request_view& request) const noexcept {
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
        return static_cast<std::size_t>(h % kBucketCount);
    }

    // Release one in-flight slot (idempotent at 0). Not tier-gated: a slot was
    // reserved on every proceed, so release must always decrement.
    static void release_slot(bucket& b) noexcept {
        std::uint32_t cur = b.in_flight.load(std::memory_order_acquire);
        while (cur > 0 &&
               !b.in_flight.compare_exchange_weak(
                   cur, cur - 1, std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
        }
    }

    [[nodiscard]] static request_policy_result retry(std::int64_t at_ns) noexcept {
        return request_policy_result{request_policy_action::retry_at,
                                     std::chrono::nanoseconds{at_ns},
                                     std::nullopt};
    }

    // Erased thunks for request_policy_ref.
    static std::expected<request_policy_result, external_error> acquire_thunk(
        void* self, const policy_request_view& request, std::stop_token stop) {
        return static_cast<gcra_request_policy*>(self)->acquire(request,
                                                                std::move(stop));
    }
    static std::expected<request_policy_result, external_error> observe_thunk(
        void* self, policy_attempt_token token, const policy_request_view& request,
        const policy_response_view& response) {
        return static_cast<gcra_request_policy*>(self)->observe(token, request,
                                                                response);
    }
    static void abandon_thunk(void* self, policy_attempt_token token,
                              const policy_request_view& request) noexcept {
        static_cast<gcra_request_policy*>(self)->abandon(token, request);
    }
    static void reset_thunk(void* self) noexcept {
        static_cast<gcra_request_policy*>(self)->reset_for_tests();
    }

    external_clock_ref clock_;
    std::array<bucket, kBucketCount> buckets_{};
};

}  // namespace bioseq::external::provider
