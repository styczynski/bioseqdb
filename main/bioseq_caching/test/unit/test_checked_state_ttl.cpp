#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

TEST_CASE("checked cache arithmetic rejects every boundary overflow") {
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
    REQUIRE_FALSE(
        checked_narrow<std::uint8_t>(std::uint16_t{256}).has_value());
}

TEST_CASE("little-endian wire helpers are canonical and bounded") {
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
    REQUIRE(
        read_little_endian<std::uint32_t>(bytes, cursor).value()
        == 0x89abcdef);
    REQUIRE_FALSE(
        read_little_endian<std::uint8_t>(bytes, cursor).has_value());
    REQUIRE_FALSE(checked_subspan(bytes, 5, 2).has_value());
}

TEST_CASE("the normative cache state graph rejects every omitted edge") {
    constexpr std::array states{
        CacheEntryState::Absent,
        CacheEntryState::Claimed,
        CacheEntryState::Building,
        CacheEntryState::Complete,
        CacheEntryState::Failed,
        CacheEntryState::Invalidated,
        CacheEntryState::Evicting,
    };

    std::size_t legal_count = 0;
    for (const auto from : states) {
        for (const auto to : states) {
            if (legal_state_transition(from, to)) {
                ++legal_count;
            }
        }
    }
    REQUIRE(legal_count == kLegalStateTransitions.size());
    for (const auto transition : kLegalStateTransitions) {
        REQUIRE(legal_state_transition(transition.from, transition.to));
    }
    REQUIRE_FALSE(legal_state_transition(
        CacheEntryState::Claimed, CacheEntryState::Complete));
    REQUIRE_FALSE(legal_state_transition(
        CacheEntryState::Invalidated, CacheEntryState::Complete));
}

TEST_CASE("generation identity prevents ABA publication and premature eviction") {
    GenerationStateMachine state;
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
    REQUIRE_FALSE(state.begin_evict(7));
    REQUIRE(state.unpin_reader(7));
    REQUIRE(state.begin_evict(7));
    REQUIRE_FALSE(state.finish_evict(8));
    REQUIRE(state.finish_evict(7));
    REQUIRE(state.value() == CacheEntryState::Absent);

    REQUIRE(state.claim(8, 12));
    REQUIRE_FALSE(state.publish_failed(
        7, 11, CacheFailureCode::ProducerExited));
    REQUIRE(state.publish_failed(
        8, 12, CacheFailureCode::ProducerExited));
    REQUIRE(state.failure() == CacheFailureCode::ProducerExited);
}

TEST_CASE("wait dependencies form a strictly descending claim DAG") {
    const std::array<std::uint64_t, 3> owned{10, 20, 30};
    REQUIRE(claim_sequence_wait_allowed(9, owned));
    REQUIRE_FALSE(claim_sequence_wait_allowed(10, owned));
    REQUIRE_FALSE(claim_sequence_wait_allowed(21, owned));
    REQUIRE_FALSE(claim_sequence_wait_allowed(0, owned));

    const std::array<std::uint64_t, 1> invalid_owned{0};
    REQUIRE_FALSE(claim_sequence_wait_allowed(1, invalid_owned));
    REQUIRE(failure_allows_takeover(CacheFailureCode::ProducerCancelled));
    REQUIRE(failure_allows_takeover(CacheFailureCode::StorageFailure));
    REQUIRE_FALSE(failure_allows_takeover(CacheFailureCode::CorruptResult));
}

TEST_CASE("TTL starts at completion and uses the stricter consumer policy") {
    CacheTimestamps timestamps{.started_ns = 10, .completed_ns = 1'000};

    REQUIRE(eligible_inflight(0));
    REQUIRE_FALSE(expired(timestamps, 10, 20, 10'000'999));
    REQUIRE(expired(timestamps, 10, 20, 10'001'000));
    REQUIRE(expired(timestamps, 20, 10, 10'001'000));
    REQUIRE(expired(timestamps, 10, 10, 999));
    REQUIRE(effective_ttl_ms(8, 3) == 3);
    REQUIRE(ttl_nanoseconds(
        (std::numeric_limits<std::uint64_t>::max)())
        == (std::numeric_limits<std::uint64_t>::max)());
}

TEST_CASE("zero TTL grants one same-generation waiter read exactly once") {
    const CacheTimestamps timestamps{
        .started_ns = 1,
        .completed_ns = 500,
    };
    REQUIRE(expired_at_completion(0));

    const auto entitled = completed_ttl_decision(
        timestamps, 0, 0, 500, true, false);
    REQUIRE(entitled.readable);
    REQUIRE(entitled.consume_just_completed_entitlement);

    REQUIRE_FALSE(completed_ttl_decision(
        timestamps, 0, 0, 500, true, true).readable);
    REQUIRE_FALSE(completed_ttl_decision(
        timestamps, 0, 0, 501, true, false).readable);
    REQUIRE_FALSE(completed_ttl_decision(
        timestamps, 0, 0, 500, false, false).readable);
}
