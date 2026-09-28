/**
 * @file bioseq_caching/core/state_machine.cppm
 * @brief Allowed cache states and the reade and waiter counts that control eviction.
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

struct state_transition {
    cache_entry_state from = cache_entry_state::absent;
    cache_entry_state to = cache_entry_state::absent;

    [[nodiscard]] bool operator==(const state_transition&) const noexcept = default;
};

inline constexpr std::array<state_transition, 11> k_legal_state_transitions{{
    {cache_entry_state::absent, cache_entry_state::claimed},
    {cache_entry_state::claimed, cache_entry_state::building},
    {cache_entry_state::claimed, cache_entry_state::failed},
    {cache_entry_state::building, cache_entry_state::complete},
    {cache_entry_state::building, cache_entry_state::failed},
    {cache_entry_state::building, cache_entry_state::invalidated},
    {cache_entry_state::complete, cache_entry_state::invalidated},
    {cache_entry_state::complete, cache_entry_state::evicting},
    {cache_entry_state::failed, cache_entry_state::evicting},
    {cache_entry_state::invalidated, cache_entry_state::evicting},
    {cache_entry_state::evicting, cache_entry_state::absent},
}};

/** Check whether the cache allows this state change */
[[nodiscard]] constexpr bool legal_state_transition(cache_entry_state from,
                                                    cache_entry_state to) noexcept {
    for (const state_transition transition : k_legal_state_transitions) {
        if (transition.from == from && transition.to == to) {
            return true;
        }
    }
    return false;
}

/** Tracks one cache generation, its state and its reader and waiter references. */
class generation_state_machine {
  public:
    /** Reserve an empty,  slot using nonzero generation and claim identifiers. */
    [[nodiscard]] bool claim(std::uint64_t generation, std::uint64_t claim_sequence) noexcept {
        if (state_ != cache_entry_state::absent || generation == 0 || claim_sequence == 0) {
            return false;
        }
        generation_ = generation;
        claim_sequence_ = claim_sequence;
        failure_ = cache_failure_code::none;
        return transition(cache_entry_state::claimed);
    }

    /** Mark the matching claim as building. */
    [[nodiscard]] bool begin_build(std::uint64_t generation,
                                   std::uint64_t claim_sequence) noexcept {
        return identity_matches(generation, claim_sequence) &&
               transition(cache_entry_state::building);
    }

    /** Mark the matching building generation as complete. */
    [[nodiscard]] bool publish_complete(std::uint64_t generation,
                                        std::uint64_t claim_sequence) noexcept {
        return identity_matches(generation, claim_sequence) &&
               transition(cache_entry_state::complete);
    }

    /** Record why the matching claim or build failed. */
    [[nodiscard]] bool
    publish_failed(std::uint64_t generation,
                   std::uint64_t claim_sequence,
                   cache_failure_code failure = cache_failure_code::producer_error) noexcept {
        if (!identity_matches(generation, claim_sequence) || failure == cache_failure_code::none ||
            !transition(cache_entry_state::failed)) {
            return false;
        }
        failure_ = failure;
        return true;
    }

    /** Invalidate the matching generation while it is building or complete. */
    [[nodiscard]] bool invalidate(std::uint64_t generation, std::uint64_t claim_sequence) noexcept {
        return identity_matches(generation, claim_sequence) &&
               transition(cache_entry_state::invalidated);
    }

    /** Pin a matching complete generation for one reader, unless the count is full. */
    [[nodiscard]] bool pin_reader(std::uint64_t generation, std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence) ||
            state_ != cache_entry_state::complete ||
            reader_pins_ == (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        ++reader_pins_;
        return true;
    }

    /** Drop exactly one reader pin from a matching generation. */
    [[nodiscard]] bool unpin_reader(std::uint64_t generation,
                                    std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence) || reader_pins_ == 0) {
            return false;
        }
        --reader_pins_;
        return true;
    }

    /** Register a waiter while the matching generation  is claimed or building. */
    [[nodiscard]] bool register_waiter(std::uint64_t generation,
                                       std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence) ||
            (state_ != cache_entry_state::claimed && state_ != cache_entry_state::building) ||
            waiter_references_ == (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        ++waiter_references_;
        return true;
    }

    /** Cancel one matching waiter reference. */
    [[nodiscard]] bool unregister_waiter(std::uint64_t generation,
                                         std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence) || waiter_references_ == 0) {
            return false;
        }
        --waiter_references_;
        return true;
    }

    /** Replace one waiter reference with a reader pin once the matching generation completes. */
    [[nodiscard]] bool convert_waiter_to_reader(std::uint64_t generation,
                                                std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence) ||
            state_ != cache_entry_state::complete || waiter_references_ == 0 ||
            reader_pins_ == (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        --waiter_references_;
        ++reader_pins_;
        return true;
    }

    /** Start evicting a complete, failed or invalidated generation after all references are released. */
    [[nodiscard]] bool begin_evict(std::uint64_t generation,
                                   std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence) || reader_pins_ != 0 ||
            waiter_references_ != 0) {
            return false;
        }
        return transition(cache_entry_state::evicting);
    }

    /** Clear the matching slot after evction. The caller assigns the next generation identifier. */
    [[nodiscard]] bool finish_evict(std::uint64_t generation,
                                    std::uint64_t claim_sequence) noexcept {
        if (!identity_matches(generation, claim_sequence) ||
            !transition(cache_entry_state::absent)) {
            return false;
        }
        generation_ = 0;
        claim_sequence_ = 0;
        failure_ = cache_failure_code::none;
        return true;
    }

    [[nodiscard]] cache_entry_state value() const noexcept {
        return state_;
    }

    [[nodiscard]] cache_failure_code failure() const noexcept {
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
    /** Check both identifiers so an old claim cannot act on a replacement generation. */
    [[nodiscard]] bool identity_matches(std::uint64_t generation,
                                        std::uint64_t claim_sequence) const noexcept {
        return generation_ == generation && claim_sequence_ == claim_sequence;
    }

    /** Change state only when the transition is allowed. */
    [[nodiscard]] bool transition(cache_entry_state destination) noexcept {
        if (!legal_state_transition(state_, destination)) {
            return false;
        }
        state_ = destination;
        return true;
    }

    cache_entry_state state_ = cache_entry_state::absent;
    cache_failure_code failure_ = cache_failure_code::none;
    std::uint64_t generation_ = 0;
    std::uint64_t claim_sequence_ = 0;
    std::uint32_t reader_pins_ = 0;
    std::uint32_t waiter_references_ = 0;
};

/**
 * Allow waiting on a nonzero claim only if every owned claim  is newer.
 * Descending claim numbers prevent cycles between waiting producers.
 */
[[nodiscard]] inline bool
claim_sequence_wait_allowed(std::uint64_t target_claim_sequence,
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

/** Allow a replacement producer after cancellation or producer exit */
[[nodiscard]] constexpr bool failure_allows_takeover(cache_failure_code failure) noexcept {
    return failure == cache_failure_code::producer_cancelled ||
           failure == cache_failure_code::producer_exited;
}

} // namespace bioseq::cache
