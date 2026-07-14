/**
 * @file bioseq_caching/core/state_machine.cppm
 * @brief Single normative cache-generation transition graph and counters.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

export module bioseq.cache.core:state_machine;
import :wire;

export namespace bioseq::cache {

struct StateTransition {
    CacheEntryState from = CacheEntryState::Absent;
    CacheEntryState to = CacheEntryState::Absent;

    [[nodiscard]] bool operator==(const StateTransition&) const noexcept = default;
};

inline constexpr std::array<StateTransition, 11> kLegalStateTransitions{{
    {CacheEntryState::Absent, CacheEntryState::Claimed},
    {CacheEntryState::Claimed, CacheEntryState::Building},
    {CacheEntryState::Claimed, CacheEntryState::Failed},
    {CacheEntryState::Building, CacheEntryState::Complete},
    {CacheEntryState::Building, CacheEntryState::Failed},
    {CacheEntryState::Building, CacheEntryState::Invalidated},
    {CacheEntryState::Complete, CacheEntryState::Invalidated},
    {CacheEntryState::Complete, CacheEntryState::Evicting},
    {CacheEntryState::Failed, CacheEntryState::Evicting},
    {CacheEntryState::Invalidated, CacheEntryState::Evicting},
    {CacheEntryState::Evicting, CacheEntryState::Absent},
}};

/** Return whether one explicit state transition belongs to the v1 graph. */
[[nodiscard]] constexpr bool legal_state_transition(
    CacheEntryState from,
    CacheEntryState to) noexcept {
    for (const StateTransition transition : kLegalStateTransitions) {
        if (transition.from == from && transition.to == to) {
            return true;
        }
    }
    return false;
}

/** Pure production/test model enforcing generation identity, pins, and waits. */
class GenerationStateMachine {
public:
    /** Claim an absent slot with a nonzero ABA generation and claim sequence. */
    [[nodiscard]] bool claim(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        if (state_ != CacheEntryState::Absent
            || generation == 0
            || claim_sequence == 0) {
            return false;
        }
        generation_ = generation;
        claim_sequence_ = claim_sequence;
        failure_ = CacheFailureCode::None;
        return transition(CacheEntryState::Claimed);
    }

    /** Move the matching initialized producer from CLAIMED to BUILDING. */
    [[nodiscard]] bool begin_build(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        return identity_matches(generation, claim_sequence)
            && transition(CacheEntryState::Building);
    }

    /** Publish a fully sealed matching generation. */
    [[nodiscard]] bool publish_complete(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        return identity_matches(generation, claim_sequence)
            && transition(CacheEntryState::Complete);
    }

    /** Publish a bounded failure for a matching in-flight generation. */
    [[nodiscard]] bool publish_failed(
        std::uint64_t generation,
        std::uint64_t claim_sequence,
        CacheFailureCode failure = CacheFailureCode::ProducerError) noexcept {
        if (!identity_matches(generation, claim_sequence)
            || failure == CacheFailureCode::None
            || !transition(CacheEntryState::Failed)) {
            return false;
        }
        failure_ = failure;
        return true;
    }

    /** Invalidate a matching complete/building generation. */
    [[nodiscard]] bool invalidate(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        return identity_matches(generation, claim_sequence)
            && transition(CacheEntryState::Invalidated);
    }

    /** Register one reader pin unless eviction/absence has begun. */
    [[nodiscard]] bool pin_reader(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence)
            || state_ != CacheEntryState::Complete
            || reader_pins_ == (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        ++reader_pins_;
        return true;
    }

    /** Drop exactly one reader pin from a matching generation. */
    [[nodiscard]] bool unpin_reader(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence)
            || reader_pins_ == 0) {
            return false;
        }
        --reader_pins_;
        return true;
    }

    /** Register one waiter reference on CLAIMED or BUILDING. */
    [[nodiscard]] bool register_waiter(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence)
            || (state_ != CacheEntryState::Claimed
                && state_ != CacheEntryState::Building)
            || waiter_references_
                == (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        ++waiter_references_;
        return true;
    }

    /** Cancel one matching waiter reference. */
    [[nodiscard]] bool unregister_waiter(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence)
            || waiter_references_ == 0) {
            return false;
        }
        --waiter_references_;
        return true;
    }

    /** Atomically model waiter-to-reader conversion after full validation. */
    [[nodiscard]] bool convert_waiter_to_reader(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence)
            || state_ != CacheEntryState::Complete
            || waiter_references_ == 0
            || reader_pins_ == (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        --waiter_references_;
        ++reader_pins_;
        return true;
    }

    /** Begin physical eviction only after every reference drains. */
    [[nodiscard]] bool begin_evict(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence)
            || reader_pins_ != 0
            || waiter_references_ != 0) {
            return false;
        }
        return transition(CacheEntryState::Evicting);
    }

    /** Finish eviction and clear the slot while preserving ABA monotonicity externally. */
    [[nodiscard]] bool finish_evict(
        std::uint64_t generation,
        std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence)
            || !transition(CacheEntryState::Absent)) {
            return false;
        }
        generation_ = 0;
        claim_sequence_ = 0;
        failure_ = CacheFailureCode::None;
        return true;
    }

    [[nodiscard]] CacheEntryState value() const noexcept {
        return state_;
    }

    [[nodiscard]] CacheFailureCode failure() const noexcept {
        return failure_;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] std::uint64_t claim_sequence() const noexcept {
        return claim_sequence_;
    }

    [[nodiscard]] std::uint32_t reader_pins() const noexcept {
        return reader_pins_;
    }

    [[nodiscard]] std::uint32_t waiter_references() const noexcept {
        return waiter_references_;
    }

private:
    /** Match both ABA components required for producer/waiter operations. */
    [[nodiscard]] bool identity_matches(
        std::uint64_t generation,
        std::uint64_t claim_sequence) const noexcept {
        return generation_ == generation
            && claim_sequence_ == claim_sequence;
    }

    /** Apply a transition only through the one normative graph. */
    [[nodiscard]] bool transition(CacheEntryState destination) noexcept {
        if (!legal_state_transition(state_, destination)) {
            return false;
        }
        state_ = destination;
        return true;
    }

    CacheEntryState state_ = CacheEntryState::Absent;
    CacheFailureCode failure_ = CacheFailureCode::None;
    std::uint64_t generation_ = 0;
    std::uint64_t claim_sequence_ = 0;
    std::uint32_t reader_pins_ = 0;
    std::uint32_t waiter_references_ = 0;
};

/** Return true only for the strictly descending claim-sequence wait DAG. */
[[nodiscard]] inline bool claim_sequence_wait_allowed(
    std::uint64_t target_claim_sequence,
    std::span<const std::uint64_t> owned_claim_sequences) noexcept {
    if (target_claim_sequence == 0) {
        return false;
    }
    for (const std::uint64_t owned : owned_claim_sequences) {
        if (owned == 0 || target_claim_sequence >= owned) {
            return false;
        }
    }
    return true;
}

/** Identify transient failures for which one new generation may take over. */
[[nodiscard]] constexpr bool failure_allows_takeover(
    CacheFailureCode failure) noexcept {
    return failure == CacheFailureCode::ProducerCancelled
        || failure == CacheFailureCode::ProducerExited;
}

} // namespace bioseq::cache
