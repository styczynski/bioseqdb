#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

namespace {

inline constexpr std::uint64_t k_test_generation = 7;
inline constexpr std::uint64_t k_test_claim_sequence = 11;

struct generation_snapshot {
    cache_entry_state state = cache_entry_state::absent;
    cache_failure_code failure = cache_failure_code::none;
    std::uint64_t generation = 0;
    std::uint64_t claim_sequence = 0;
    std::uint32_t reader_pins = 0;
    std::uint32_t waiter_references = 0;

    [[nodiscard]] bool operator==(const generation_snapshot&) const noexcept = default;
};

[[nodiscard]] generation_snapshot snapshot(const generation_state_machine& state) {
    return generation_snapshot{
        .state = state.value(),
        .failure = state.failure(),
        .generation = state.generation(),
        .claim_sequence = state.claim_sequence(),
        .reader_pins = state.reader_pins(),
        .waiter_references = state.waiter_references(),
    };
}

[[nodiscard]] generation_state_machine machine_in_state(cache_entry_state requested) {
    generation_state_machine state;
    if (requested == cache_entry_state::absent) {
        return state;
    }
    (void)state.claim(k_test_generation, k_test_claim_sequence);
    switch (requested) {
    case cache_entry_state::absent:
    case cache_entry_state::claimed:
        break;
    case cache_entry_state::building:
        (void)state.begin_build(k_test_generation, k_test_claim_sequence);
        break;
    case cache_entry_state::complete:
        (void)state.begin_build(k_test_generation, k_test_claim_sequence);
        (void)state.publish_complete(k_test_generation, k_test_claim_sequence);
        break;
    case cache_entry_state::failed:
        (void)state.publish_failed(
            k_test_generation, k_test_claim_sequence, cache_failure_code::producer_error);
        break;
    case cache_entry_state::invalidated:
        (void)state.begin_build(k_test_generation, k_test_claim_sequence);
        (void)state.invalidate(k_test_generation, k_test_claim_sequence);
        break;
    case cache_entry_state::evicting:
        (void)state.publish_failed(
            k_test_generation, k_test_claim_sequence, cache_failure_code::producer_error);
        (void)state.begin_evict(k_test_generation, k_test_claim_sequence);
        break;
    }
    return state;
}

struct state_operation_expectations {
    cache_entry_state state = cache_entry_state::absent;
    bool claim = false;
    bool begin_build = false;
    bool publish_complete = false;
    bool publish_failed = false;
    bool invalidate = false;
    bool pin_reader = false;
    bool register_waiter = false;
    bool begin_evict = false;
    bool finish_evict = false;
};

} // namespace

TEST_CASE("checked cache arithmetic rejects every boundary overflow",
          "[component:cache][layer:unit][feature:result_cache]") {
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();

    REQUIRE(checked_add(std::uint64_t{4}, std::uint64_t{7}) == 11);
    REQUIRE_FALSE(checked_add(maximum, std::uint64_t{1}).has_value());
    REQUIRE(checked_mul(std::uint64_t{0}, maximum) == 0);
    REQUIRE(checked_mul(std::uint64_t{9}, std::uint64_t{7}) == 63);
    REQUIRE_FALSE(checked_mul(maximum, std::uint64_t{2}).has_value());
    REQUIRE(checked_range(8, 4, 12));
    REQUIRE_FALSE(checked_range(maximum, 2, maximum));

    REQUIRE(checked_align_up(9, 8).value() == 16);
    REQUIRE_FALSE(checked_align_up(9, 3).has_value());
    REQUIRE_FALSE(checked_align_up(maximum, 8).has_value());
    REQUIRE(checked_narrow<std::uint8_t>(std::uint16_t{255}) == 255);
    REQUIRE_FALSE(checked_narrow<std::uint8_t>(std::uint16_t{256}).has_value());
}

TEST_CASE("little-endian wire helpers are canonical and bounded",
          "[component:cache][layer:unit][feature:result_cache]") {
    std::vector<std::byte> bytes;
    append_little_endian(bytes, std::uint16_t{0x1234});
    append_little_endian(bytes, std::uint32_t{0x89abcdef});

    REQUIRE(bytes.size() == 6);
    REQUIRE(bytes[0] == std::byte{0x34});
    REQUIRE(bytes[1] == std::byte{0x12});
    REQUIRE(bytes[2] == std::byte{0xef});
    REQUIRE(bytes[5] == std::byte{0x89});

    std::size_t cursor = 0;
    REQUIRE(read_little_endian<std::uint16_t>(bytes, cursor).value() == 0x1234);
    REQUIRE(read_little_endian<std::uint32_t>(bytes, cursor).value() == 0x89abcdef);
    REQUIRE_FALSE(read_little_endian<std::uint8_t>(bytes, cursor).has_value());
    REQUIRE_FALSE(checked_subspan(bytes, 5, 2).has_value());
}

TEST_CASE("the normative cache state graph rejects every omitted edge",
          "[component:cache][layer:unit][feature:result_cache]") {
    constexpr std::array states{
        cache_entry_state::absent,
        cache_entry_state::claimed,
        cache_entry_state::building,
        cache_entry_state::complete,
        cache_entry_state::failed,
        cache_entry_state::invalidated,
        cache_entry_state::evicting,
    };

    std::size_t legal_count = 0;
    for (const auto from : states) {
        for (const auto to : states) {
            if (legal_state_transition(from, to)) {
                ++legal_count;
            }
        }
    }
    REQUIRE(legal_count == k_legal_state_transitions.size());
    for (const auto transition : k_legal_state_transitions) {
        REQUIRE(legal_state_transition(transition.from, transition.to));
    }
    REQUIRE_FALSE(legal_state_transition(cache_entry_state::claimed, cache_entry_state::complete));
    REQUIRE_FALSE(
        legal_state_transition(cache_entry_state::invalidated, cache_entry_state::complete));
}

TEST_CASE("generation operations exhaust every state and preserve rejections",
          "[component:cache][layer:unit][feature:result_cache]") {
    constexpr std::array expectations{
        state_operation_expectations{
            .state = cache_entry_state::absent,
            .claim = true,
        },
        state_operation_expectations{
            .state = cache_entry_state::claimed,
            .begin_build = true,
            .publish_failed = true,
            .register_waiter = true,
        },
        state_operation_expectations{
            .state = cache_entry_state::building,
            .publish_complete = true,
            .publish_failed = true,
            .invalidate = true,
            .register_waiter = true,
        },
        state_operation_expectations{
            .state = cache_entry_state::complete,
            .invalidate = true,
            .pin_reader = true,
            .begin_evict = true,
        },
        state_operation_expectations{
            .state = cache_entry_state::failed,
            .begin_evict = true,
        },
        state_operation_expectations{
            .state = cache_entry_state::invalidated,
            .begin_evict = true,
        },
        state_operation_expectations{
            .state = cache_entry_state::evicting,
            .finish_evict = true,
        },
    };

    for (const state_operation_expectations expected : expectations) {
        const auto check = [expected](bool allowed, auto operation) {
            generation_state_machine state = machine_in_state(expected.state);
            const generation_snapshot before = snapshot(state);
            REQUIRE(operation(state) == allowed);
            if (!allowed) {
                REQUIRE(snapshot(state) == before);
            }
        };

        check(expected.claim, [](generation_state_machine& state) {
            return state.claim(k_test_generation, k_test_claim_sequence);
        });
        check(expected.begin_build, [](generation_state_machine& state) {
            return state.begin_build(k_test_generation, k_test_claim_sequence);
        });
        check(expected.publish_complete, [](generation_state_machine& state) {
            return state.publish_complete(k_test_generation, k_test_claim_sequence);
        });
        check(expected.publish_failed, [](generation_state_machine& state) {
            return state.publish_failed(
                k_test_generation, k_test_claim_sequence, cache_failure_code::producer_error);
        });
        check(false, [](generation_state_machine& state) {
            return state.publish_failed(
                k_test_generation, k_test_claim_sequence, cache_failure_code::none);
        });
        check(expected.invalidate, [](generation_state_machine& state) {
            return state.invalidate(k_test_generation, k_test_claim_sequence);
        });
        check(expected.pin_reader, [](generation_state_machine& state) {
            return state.pin_reader(k_test_generation, k_test_claim_sequence);
        });
        check(false, [](generation_state_machine& state) {
            return state.unpin_reader(k_test_generation, k_test_claim_sequence);
        });
        check(expected.register_waiter, [](generation_state_machine& state) {
            return state.register_waiter(k_test_generation, k_test_claim_sequence);
        });
        check(false, [](generation_state_machine& state) {
            return state.unregister_waiter(k_test_generation, k_test_claim_sequence);
        });
        check(false, [](generation_state_machine& state) {
            return state.convert_waiter_to_reader(k_test_generation, k_test_claim_sequence);
        });
        check(expected.begin_evict, [](generation_state_machine& state) {
            return state.begin_evict(k_test_generation, k_test_claim_sequence);
        });
        check(expected.finish_evict, [](generation_state_machine& state) {
            return state.finish_evict(k_test_generation, k_test_claim_sequence);
        });
    }
}

TEST_CASE("generation identity prevents ABA publication and premature eviction",
          "[component:cache][layer:unit][feature:result_cache]") {
    generation_state_machine state;
    REQUIRE_FALSE(state.claim(0, 1));
    REQUIRE(state.claim(7, 11));
    REQUIRE_FALSE(state.claim(8, 12));
    REQUIRE_FALSE(state.begin_build(7, 10));
    REQUIRE(state.register_waiter(7, 11));
    REQUIRE(state.begin_build(7, 11));
    REQUIRE_FALSE(state.publish_complete(8, 11));
    REQUIRE(state.publish_complete(7, 11));
    REQUIRE(state.convert_waiter_to_reader(7, 11));
    REQUIRE(state.reader_pins() == 1);
    REQUIRE(state.waiter_references() == 0);
    REQUIRE_FALSE(state.begin_evict(7, 11));
    REQUIRE_FALSE(state.unpin_reader(7, 10));
    REQUIRE(state.unpin_reader(7, 11));
    REQUIRE_FALSE(state.begin_evict(7, 10));
    REQUIRE(state.begin_evict(7, 11));
    REQUIRE_FALSE(state.finish_evict(8, 11));
    REQUIRE_FALSE(state.finish_evict(7, 10));
    REQUIRE(state.finish_evict(7, 11));
    REQUIRE(state.value() == cache_entry_state::absent);

    REQUIRE(state.claim(8, 12));
    REQUIRE_FALSE(state.publish_failed(7, 11, cache_failure_code::producer_exited));
    REQUIRE(state.publish_failed(8, 12, cache_failure_code::producer_exited));
    REQUIRE(state.failure() == cache_failure_code::producer_exited);
}

TEST_CASE("invalidation preserves old pins but rejects every new reader",
          "[component:cache][layer:unit][feature:result_cache]") {
    generation_state_machine state;
    REQUIRE(state.claim(3, 4));
    REQUIRE(state.begin_build(3, 4));
    REQUIRE(state.publish_complete(3, 4));
    REQUIRE(state.pin_reader(3, 4));
    REQUIRE_FALSE(state.invalidate(3, 5));
    REQUIRE(state.invalidate(3, 4));
    REQUIRE_FALSE(state.pin_reader(3, 4));
    REQUIRE_FALSE(state.begin_evict(3, 4));
    REQUIRE(state.unpin_reader(3, 4));
    REQUIRE(state.begin_evict(3, 4));
}

TEST_CASE("waiter and reader references drain exactly across terminal states",
          "[component:cache][layer:unit][feature:result_cache]") {
    generation_state_machine complete;
    REQUIRE(complete.claim(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.register_waiter(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.register_waiter(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.begin_build(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.publish_complete(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.convert_waiter_to_reader(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.convert_waiter_to_reader(k_test_generation, k_test_claim_sequence));
    REQUIRE_FALSE(complete.convert_waiter_to_reader(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.pin_reader(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.reader_pins() == 3);
    REQUIRE(complete.waiter_references() == 0);
    REQUIRE(complete.invalidate(k_test_generation, k_test_claim_sequence));
    REQUIRE_FALSE(complete.pin_reader(k_test_generation, k_test_claim_sequence));
    for (std::size_t count = 0; count < 3; ++count) {
        REQUIRE(complete.unpin_reader(k_test_generation, k_test_claim_sequence));
    }
    REQUIRE_FALSE(complete.unpin_reader(k_test_generation, k_test_claim_sequence));
    REQUIRE(complete.begin_evict(k_test_generation, k_test_claim_sequence));

    generation_state_machine failed;
    REQUIRE(failed.claim(8, 12));
    REQUIRE(failed.register_waiter(8, 12));
    REQUIRE(failed.publish_failed(8, 12, cache_failure_code::producer_exited));
    REQUIRE_FALSE(failed.begin_evict(8, 12));
    REQUIRE(failed.unregister_waiter(8, 12));
    REQUIRE_FALSE(failed.unregister_waiter(8, 12));
    REQUIRE(failed.begin_evict(8, 12));
}

TEST_CASE("wait dependencies form a strictly descending claim DAG",
          "[component:cache][layer:unit][feature:result_cache]") {
    const std::array<std::uint64_t, 3> owned{10, 20, 30};
    REQUIRE(claim_sequence_wait_allowed(9, owned));
    REQUIRE_FALSE(claim_sequence_wait_allowed(10, owned));
    REQUIRE_FALSE(claim_sequence_wait_allowed(21, owned));
    REQUIRE_FALSE(claim_sequence_wait_allowed(0, owned));

    const std::array<std::uint64_t, 1> invalid_owned{0};
    REQUIRE_FALSE(claim_sequence_wait_allowed(1, invalid_owned));
    REQUIRE(failure_allows_takeover(cache_failure_code::producer_cancelled));
    REQUIRE(failure_allows_takeover(cache_failure_code::producer_exited));
    REQUIRE_FALSE(failure_allows_takeover(cache_failure_code::storage_failure));
    REQUIRE_FALSE(failure_allows_takeover(cache_failure_code::admission_denied));
    REQUIRE_FALSE(failure_allows_takeover(cache_failure_code::corrupt_result));
}

TEST_CASE("TTL starts at completion and uses the stricter consumer policy",
          "[component:cache][layer:unit][feature:result_cache]") {
    cache_timestamps timestamps{.started_ns = 10, .completed_ns = 1'000};

    REQUIRE(eligible_inflight(0));
    REQUIRE_FALSE(expired(timestamps, 10, 20, 10'000'999));
    REQUIRE(expired(timestamps, 10, 20, 10'001'000));
    REQUIRE(expired(timestamps, 20, 10, 10'001'000));
    REQUIRE(expired(timestamps, 10, 10, 999));
    REQUIRE(effective_ttl_ms(8, 3) == 3);
    REQUIRE(ttl_nanoseconds((std::numeric_limits<std::uint64_t>::max)()) ==
            (std::numeric_limits<std::uint64_t>::max)());
}

TEST_CASE("monotonic TTL boundaries reject wrap instead of inferring age",
          "[component:cache][layer:unit][feature:result_cache]") {
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::uint64_t nanos_per_millisecond = 1'000'000;
    constexpr std::uint64_t largest_exact_ttl_ms = maximum / nanos_per_millisecond;

    REQUIRE(monotonic_age_ns(0, 0) == 0);
    REQUIRE(monotonic_age_ns(0, maximum) == maximum);
    REQUIRE(monotonic_age_ns(maximum, maximum) == 0);
    REQUIRE_FALSE(monotonic_age_ns(maximum, 0).has_value());
    REQUIRE_FALSE(monotonic_age_ns(maximum, maximum - 1).has_value());

    REQUIRE(ttl_nanoseconds(0) == 0);
    REQUIRE(ttl_nanoseconds(largest_exact_ttl_ms) == largest_exact_ttl_ms * nanos_per_millisecond);
    REQUIRE(ttl_nanoseconds(largest_exact_ttl_ms + 1) == maximum);

    const cache_timestamps ordinary{
        .started_ns = 10,
        .completed_ns = 20,
    };
    REQUIRE_FALSE(expired(ordinary, 1, 1, 1'000'019));
    REQUIRE(expired(ordinary, 1, 1, 1'000'020));

    const cache_timestamps saturated{
        .started_ns = 0,
        .completed_ns = 0,
    };
    REQUIRE_FALSE(expired(saturated, maximum, maximum, maximum - 1));
    REQUIRE(expired(saturated, maximum, maximum, maximum));

    const cache_timestamps wrapped_sample{
        .started_ns = maximum - 1,
        .completed_ns = maximum,
    };
    REQUIRE(expired(wrapped_sample, maximum, maximum, 0));
    REQUIRE_FALSE(completed_ttl_decision(wrapped_sample, 0, 0, 0, true, false).readable);

    const cache_timestamps incomplete{
        .started_ns = maximum,
        .completed_ns = std::nullopt,
    };
    REQUIRE_FALSE(expired(incomplete, 0, 0, maximum));
}

TEST_CASE("zero TTL ABA recheck preserves only one waiter entitlement",
          "[component:cache][layer:unit][feature:result_cache]") {
    const cache_timestamps timestamps{
        .started_ns = 1,
        .completed_ns = 500,
    };
    REQUIRE(expired_at_completion(0));

    const auto entitled = completed_ttl_decision(timestamps, 0, 0, 500, true, false);
    REQUIRE(entitled.readable);
    REQUIRE(entitled.consume_just_completed_entitlement);

    REQUIRE_FALSE(completed_ttl_decision(timestamps, 0, 0, 500, true, true).readable);
    const auto rebound_before_commit = completed_ttl_decision(timestamps, 0, 0, 501, true, false);
    REQUIRE(rebound_before_commit.readable);
    REQUIRE(rebound_before_commit.consume_just_completed_entitlement);
    REQUIRE_FALSE(completed_ttl_decision(timestamps, 0, 0, 499, true, false).readable);
    // The waiter has already used its one read of this TTL-zero generation.
    REQUIRE_FALSE(completed_ttl_decision(timestamps, 0, 0, 501, true, true).readable);
    // This reader did not register while the generation was being built.
    REQUIRE_FALSE(completed_ttl_decision(timestamps, 0, 0, 500, false, false).readable);
}
