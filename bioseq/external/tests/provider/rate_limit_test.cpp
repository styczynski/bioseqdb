/**
 * @file external/tests/provider/rate_limit_test.cpp
 * @brief Check request spacing, concurrency limits, and slot release.
 */

// A controllable clock tests request timing without waiting or using the network.

#include <array>
#include <catch2/catch_test_macros.hpp>
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
    prov::external_clock_ref ref() {
        return {this, &mono, &wall};
    }
};

// Request fixture with one rate tier and a configurable active-request cap.
struct policy_fixture {
    std::array<prov::request_rate_tier_view, 1> tiers{};
    prov::policy_request_view request{};
    policy_fixture(std::int64_t spacing_ns, std::uint32_t burst, std::uint32_t max_in_flight) {
        tiers[0] = prov::request_rate_tier_view{.name = "default",
                                                .initial_spacing = ns{spacing_ns},
                                                .minimum_spacing = ns{spacing_ns},
                                                .burst = burst,
                                                .maximum_in_flight = max_in_flight};
        request.profile.rate_tiers =
            std::span<const prov::request_rate_tier_view>(tiers.data(), tiers.size());
        request.rate_tier = "default";
        request.normalized_origin = "https://eutils.ncbi.nlm.nih.gov";
        request.credential_identity = {};
    }
};
} // namespace

TEST_CASE("gcra enforces global spacing",
          "[component:external_api][layer:unit][feature:external_source]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/0, /*max_in_flight=*/0);

    auto r0 = policy.acquire(fx.request, {});
    REQUIRE(r0.has_value());
    REQUIRE(r0->action == prov::request_policy_action::proceed);

    auto r1 = policy.acquire(fx.request, {}); // immediate re-request at t=0
    REQUIRE(r1.has_value());
    REQUIRE(r1->action == prov::request_policy_action::retry_at);
    REQUIRE(r1->retry_at.count() == 1000);

    clock.now = 1000;
    auto r2 = policy.acquire(fx.request, {});
    REQUIRE(r2->action == prov::request_policy_action::proceed);
}

TEST_CASE("gcra allows an initial burst then throttles",
          "[component:external_api][layer:unit][feature:external_source]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/3, /*max_in_flight=*/0);

    int proceeded = 0;
    for (int i = 0; i < 4; ++i) { // One scheduled request  plus three burst requests can start immediately.
        auto r = policy.acquire(fx.request, {});
        if (r->action == prov::request_policy_action::proceed)
            ++proceeded;
    }
    REQUIRE(proceeded == 4);

    auto r = policy.acquire(fx.request, {});
    REQUIRE(r->action == prov::request_policy_action::retry_at);
}

TEST_CASE("gcra caps concurrent in-flight requests",
          "[component:external_api][layer:unit][feature:external_source]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/100, /*max_in_flight=*/2);

    auto a = policy.acquire(fx.request, {});
    auto b = policy.acquire(fx.request, {});
    REQUIRE(a->action == prov::request_policy_action::proceed);
    REQUIRE(b->action == prov::request_policy_action::proceed);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 2);

    auto c = policy.acquire(fx.request, {}); // Both active slots are already occupied.
    REQUIRE(c->action == prov::request_policy_action::retry_at);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 2); // Rejection leaves the active count unchanged.

    prov::policy_response_view resp{};
    resp.outcome = prov::policy_attempt_outcome::response;
    (void)policy.observe(*a->attempt, fx.request, resp); // releases a slot
    REQUIRE(policy.in_flight_for_tests(fx.request) == 1);

    auto d = policy.acquire(fx.request, {});
    REQUIRE(d->action == prov::request_policy_action::proceed);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 2);
}

TEST_CASE("gcra rolls back the in-flight slot when it throttles",
          "[component:external_api][layer:unit][feature:external_source]") {
    fake_clock clock;
    prov::gcra_request_policy policy(clock.ref());
    policy_fixture fx(/*spacing=*/1000, /*burst=*/0, /*max_in_flight=*/2);

    auto a = policy.acquire(fx.request, {}); // First request starts and advances the schedule.
    REQUIRE(a->action == prov::request_policy_action::proceed);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 1);

    // The second request can reserve a slot but is too early to start.
    // Rejecting it must return that slot.
    auto b = policy.acquire(fx.request, {});
    REQUIRE(b->action == prov::request_policy_action::retry_at);
    REQUIRE(b->retry_at.count() == 1000);
    REQUIRE(policy.in_flight_for_tests(fx.request) == 1); // The rejected request rlturned its slot.

    clock.now = 1000; // Advance to the next allowed request time.
    auto c = policy.acquire(fx.request, {});
    REQUIRE(c->action == prov::request_policy_action::proceed);
}

TEST_CASE("gcra applies a Retry-After penalty floor",
          "[component:external_api][layer:unit][feature:external_source]") {
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

    auto b = policy.acquire(fx.request, {}); // Server retry time overrides the burst allowance.
    REQUIRE(b->action == prov::request_policy_action::retry_at);
    REQUIRE(b->retry_at.count() == 50000);

    clock.now = 50000;
    auto c = policy.acquire(fx.request, {});
    REQUIRE(c->action == prov::request_policy_action::proceed);
}

TEST_CASE("gcra is thread-safe and leaks no in-flight slots under contention",
          "[component:external_api][layer:unit][feature:external_source]") {
    struct steady_clock_src {
        static std::chrono::nanoseconds mono(void*) noexcept {
            return std::chrono::steady_clock::now().time_since_epoch();
        }
        static prov::external_timestamp wall(void*) noexcept {
            return prov::external_timestamp{};
        }
    };
    prov::external_clock_ref clock{nullptr, &steady_clock_src::mono, &steady_clock_src::wall};
    prov::gcra_request_policy policy(clock);
    policy_fixture fx(/*spacing=*/1000, /*burst=*/1000000, /*max_in_flight=*/8);

    constexpr int k_thread_count = 8;
    constexpr int k_iteration_count = 2000;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < k_thread_count; ++t) {
            threads.emplace_back([&policy, &fx] {
                for (int i = 0; i < k_iteration_count; ++i) {
                    auto r = policy.acquire(fx.request, {});
                    if (r.has_value() && r->action == prov::request_policy_action::proceed) {
                        (void)policy.observe(*r->attempt, fx.request, prov::policy_response_view{});
                    }
                    // A delayed request has already  returned any reserved slot.
                }
            });
        }
    } // jthreads join on scope exit
    // All started requests were observed and all delayed requests returned
    // their slots. No active requests should remain after the threads join
    REQUIRE(policy.in_flight_for_tests(fx.request) == 0);
}
