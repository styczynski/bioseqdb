// DOC0372 §3348-3369 — process-local GCRA request policy. A controllable clock
// drives deterministic assertions on global spacing, burst tolerance, the
// in-flight cap (with rollback), and the Retry-After penalty floor. No network.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

import bioseq.external.provider.rate_limit;
import bioseq.external.provider.runtime_services;
import bioseq.external.provider.errors;
import bioseq.external.provider.base;

namespace {
namespace prov = bioseq::external::provider;
using ns = std::chrono::nanoseconds;

// A monotonic clock whose "now" the test advances by hand.
struct fake_clock {
    std::int64_t now = 0;
    static std::chrono::nanoseconds mono(void* self) noexcept {
        return std::chrono::nanoseconds{static_cast<fake_clock*>(self)->now};
    }
    static prov::external_timestamp wall(void*) noexcept {
        return prov::external_timestamp{};
    }
    prov::external_clock_ref ref() { return {this, &mono, &wall}; }
};

// One-tier policy request (spacing, burst, max_in_flight).
struct policy_fixture {
    std::array<prov::request_rate_tier_view, 1> tiers{};
    prov::policy_request_view request{};
    policy_fixture(std::int64_t spacing_ns, std::uint32_t burst,
                   std::uint32_t max_in_flight) {
        tiers[0] = prov::request_rate_tier_view{
            .name = "default",
            .initial_spacing = ns{spacing_ns},
            .minimum_spacing = ns{spacing_ns},
            .burst = burst,
            .maximum_in_flight = max_in_flight};
        request.profile.rate_tiers =
            std::span<const prov::request_rate_tier_view>(tiers.data(),
                                                          tiers.size());
        request.rate_tier = "default";
        request.normalized_origin = "https://eutils.ncbi.nlm.nih.gov";
        request.credential_identity = {};
    }
};
}  // namespace

TEST_CASE("gcra enforces global spacing", "[external][provider][unit][rate]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/0, /*max_in_flight=*/0);

    auto r0 = policy.acquire(fx.request, {});
    REQUIRE(r0.has_value());
    REQUIRE(r0->action == prov::request_policy_action::proceed);

    auto r1 = policy.acquire(fx.request, {});  // immediate re-request at t=0
    REQUIRE(r1.has_value());
    REQUIRE(r1->action == prov::request_policy_action::retry_at);
    REQUIRE(r1->retry_at.count() == 1000);

    clock.now = 1000;
    auto r2 = policy.acquire(fx.request, {});
    REQUIRE(r2->action == prov::request_policy_action::proceed);
}

TEST_CASE("gcra allows an initial burst then throttles",
          "[external][provider][unit][rate]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/3, /*max_in_flight=*/0);

    int proceeded = 0;
    for (int i = 0; i < 4; ++i) {  // burst=3 → 1 + 3 immediate at t=0
        auto r = policy.acquire(fx.request, {});
        if (r->action == prov::request_policy_action::proceed) ++proceeded;
    }
    REQUIRE(proceeded == 4);

    auto r = policy.acquire(fx.request, {});
    REQUIRE(r->action == prov::request_policy_action::retry_at);
}

TEST_CASE("gcra caps concurrent in-flight requests",
          "[external][provider][unit][rate]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/100, /*max_in_flight=*/2);

    auto a = policy.acquire(fx.request, {});
    auto b = policy.acquire(fx.request, {});
    REQUIRE(a->action == prov::request_policy_action::proceed);
    REQUIRE(b->action == prov::request_policy_action::proceed);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 2);

    auto c = policy.acquire(fx.request, {});  // cap full → refused before reserving
    REQUIRE(c->action == prov::request_policy_action::retry_at);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 2);  // c leaked nothing

    prov::policy_response_view resp{};
    resp.outcome = prov::policy_attempt_outcome::response;
    (void)policy.observe(*a->attempt, fx.request, resp);  // releases a slot
    REQUIRE(policy.in_flight_for_tests(fx.request) == 1);

    auto d = policy.acquire(fx.request, {});
    REQUIRE(d->action == prov::request_policy_action::proceed);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 2);
}

TEST_CASE("gcra rolls back the in-flight slot when it throttles",
          "[external][provider][unit][rate]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/0, /*max_in_flight=*/2);

    auto a = policy.acquire(fx.request, {});  // proceed → tat=1000, in_flight=1
    REQUIRE(a->action == prov::request_policy_action::proceed);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 1);

    // b reserves a slot (cap has room) but GCRA rejects (now=0 < tat=1000); the
    // reserved slot MUST be rolled back, not left held.
    auto b = policy.acquire(fx.request, {});
    REQUIRE(b->action == prov::request_policy_action::retry_at);
    REQUIRE(b->retry_at.count() == 1000);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 1);  // rolled back, not leaked

    clock.now = 1000;  // a later request still proceeds → cap not wedged by a leak
    auto c = policy.acquire(fx.request, {});
    REQUIRE(c->action == prov::request_policy_action::proceed);
}

TEST_CASE("gcra applies a Retry-After penalty floor",
          "[external][provider][unit][rate]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/100, /*max_in_flight=*/0);

    auto a = policy.acquire(fx.request, {});
    REQUIRE(a->action == prov::request_policy_action::proceed);

    prov::policy_response_view resp{};
    resp.outcome = prov::policy_attempt_outcome::response;
    resp.http_status = 429;
    resp.rate.retry_at = ns{50000};
    (void)policy.observe(*a->attempt, fx.request, resp);

    auto b = policy.acquire(fx.request, {});  // blocked by penalty despite burst
    REQUIRE(b->action == prov::request_policy_action::retry_at);
    REQUIRE(b->retry_at.count() == 50000);

    clock.now = 50000;
    auto c = policy.acquire(fx.request, {});
    REQUIRE(c->action == prov::request_policy_action::proceed);
}

TEST_CASE("gcra is thread-safe and leaks no in-flight slots under contention",
          "[external][provider][unit][rate]") {
    struct steady_clock_src {
        static std::chrono::nanoseconds mono(void*) noexcept {
            return std::chrono::steady_clock::now().time_since_epoch();
        }
        static prov::external_timestamp wall(void*) noexcept {
            return prov::external_timestamp{};
        }
    };
    prov::external_clock_ref clock{nullptr, &steady_clock_src::mono,
                                   &steady_clock_src::wall};
    prov::gcra_request_policy policy(clock);
    policy_fixture fx(/*spacing=*/1000, /*burst=*/1000000, /*max_in_flight=*/8);

    constexpr int kThreads = 8;
    constexpr int kIters = 2000;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < kIters; ++i) {
                    auto r = policy.acquire(fx.request, {});
                    if (r.has_value() &&
                        r->action == prov::request_policy_action::proceed) {
                        (void)policy.observe(*r->attempt, fx.request,
                                             prov::policy_response_view{});
                    }
                    // A throttled request reserves no net slot — nothing to release.
                }
            });
        }
    }  // jthreads join on scope exit
    // Every proceed was balanced by an observe and every reject rolled back, so
    // concurrent CAS on the shared bucket leaves the in-flight count at 0.
    REQUIRE(policy.in_flight_for_tests(fx.request) == 0);
}
