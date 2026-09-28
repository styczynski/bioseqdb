/**
 * @file fragment_cache.cppm
 * @brief Small fixed-size cache whose slot generations detect stale references.
 *
 * Lookup scans the slots. Eviction calls a cleanup callback before marking
 * a slot empty, and insertion assigns a new generation. Use each cache
 * from one thread.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

export module bioseq.common.fragment_cache;

static_assert(__cplusplus >= 202302L, "fragment_cache requires C++23 or later");

export namespace bioseq::common {

/**
 * @brief Keeps values in reusable slots with a  generation for each insertion.
 *
 * The slot array is stored inside the cache. Check the saved index and
 * generation, with is_valid_at() before reusing a reference, since eviction
 * or replacement invalidates it. Calls are unshnchronized.
 *
 * @tparam p_value_type Value type that can be default-constructed and move-assigned
 * @tparam p_capacity Positive number of slots.
 */
template <class p_value_type, std::size_t p_capacity>
class fragment_cache {
public:
    /** @brief Slot index type. */
    using slot_index_type = std::size_t;

    /** @brief Counter type used to distinguish successive slot contents. */
    using generation_type = std::uint64_t;

    /** @brief Number of slots owned by each cache instance. */
    static constexpr std::size_t k_capacity = p_capacity;

    static_assert(p_capacity >= 1, "fragment_cache requires at least one slot");

    /** @brief Constructs a cache with every slot empty. */
    fragment_cache() noexcept = default;

    fragment_cache(const fragment_cache&) = delete;
    fragment_cache& operator=(const fragment_cache&) = delete;
    fragment_cache(fragment_cache&&) = delete;
    fragment_cache& operator=(fragment_cache&&) = delete;
    ~fragment_cache() = default;

    /**
     * @brief Reports the number of occupied slots.
     * @return Value in the inclusive range `[0, k_capacity]`.
     */
    [[nodiscard]] [[gnu::always_inline]] std::size_t valid_count() const noexcept {
        return valid_count_;
    }

    /**
     * @brief Finds a stored value by comparing its extracted key.
     *
     * Lookup starts at  the last hit or insertion and scans at most k_capacity
     * slots. The extractor is called only for occupied slots.
     *
     * @param[in] key Key to find.
     * @param[in] key_extractor Returns a stored value's key and must not throw.
     * @return Slot index and generation, or std::nullopt when absent.
     */
    template <class p_key_type, class p_key_extractor>
    [[nodiscard]] [[gnu::always_inline]]
    std::optional<std::pair<slot_index_type, generation_type>>
    find(p_key_type key, p_key_extractor&& key_extractor) const noexcept {
        std::size_t slot_index = hint_index_;
        for (std::size_t slots_checked = 0; slots_checked < k_capacity; ++slots_checked) {
            if (filled_[slot_index] && key_extractor(slots_[slot_index]) == key) {
                hint_index_ = slot_index;
                return std::pair<slot_index_type, generation_type>{
                    slot_index, generations_[slot_index]};
            }
            slot_index = (slot_index + 1) % k_capacity;
        }
        return std::nullopt;
    }

    /**
     * @brief Inserts into an empty slot, or replaces the next slot in eviction order.
     *
     * When full, the cache cycles through slots. Explicit evictions can leave
     * holes, which later insertions fill before replacing occupied slots.
     *
     * @param[in] value Value moved into the selected slot.
     * @param[in] on_evict Cleans up a replaced value before assignment. It must
     * not throw or keep a reference  to that value.
     * @return Slot index and new generation.
     * @warning Moving the value into its slot must not throw.
     */
    template <class p_eviction_callback>
    [[gnu::always_inline]] std::pair<slot_index_type, generation_type>
    push(p_value_type value, p_eviction_callback&& on_evict) noexcept {
        slot_index_type slot_index = 0;
        if (valid_count_ < k_capacity) {
            for (slot_index = 0; slot_index < k_capacity; ++slot_index) {
                if (!filled_[slot_index]) {
                    break;
                }
            }
        } else {
            slot_index = oldest_index_;
            evict_slot_unchecked(
                slot_index, std::forward<p_eviction_callback>(on_evict));
            oldest_index_ = (oldest_index_ + 1) % k_capacity;
        }

        slots_[slot_index] = std::move(value);
        filled_[slot_index] = true;
        generations_[slot_index] = ++next_generation_;
        ++valid_count_;
        hint_index_ = slot_index;
        return {slot_index, generations_[slot_index]};
    }

    /**
     * @brief Evicts the next occupied slot in the circular eviction order.
     *
     * Does nothing when empty.
     * @param[in] on_evict Cleans up the value before its slot is marked empty.
     * The callback must not throw or keep a reference to the value.
     */
    template <class p_eviction_callback>
    [[gnu::always_inline]] void evict_oldest(p_eviction_callback&& on_evict) noexcept {
        if (valid_count_ == 0) {
            return;
        }

        slot_index_type slot_index = oldest_index_;
        for (std::size_t slots_checked = 0; slots_checked < k_capacity; ++slots_checked) {
            if (filled_[slot_index]) {
                break;
            }
            slot_index = (slot_index + 1) % k_capacity;
        }
        evict_slot_unchecked(slot_index, std::forward<p_eviction_callback>(on_evict));
        oldest_index_ = (slot_index + 1) % k_capacity;
    }

    /**
     * @brief Evicts a specified occupied slot.
     *
     * @tparam p_eviction_callback Callable accepting a mutable stored value.
     * @param[in] slot_index Slot, to plear Out-of-range and empty slots are ignored.
     * @param[in] on_evict Callback invoked before an occupied slot is cleared.
     * @warning `on_evict` must not throw or keep its value reference after returning.
     */
    template <class p_eviction_callback>
    [[gnu::always_inline]] void evict_at(
        slot_index_type slot_index,
        p_eviction_callback&& on_evict) noexcept {
        if (slot_index >= k_capacity || !filled_[slot_index]) {
            return;
        }
        evict_slot_unchecked(slot_index, std::forward<p_eviction_callback>(on_evict));
    }

    /**
     * @brief Checks that a slot still holds the saved generation.
     *
     * Returns false for  an invalid index, an empty slot or a replaced value.
     */
    [[nodiscard]] [[gnu::always_inline]] bool
    is_valid_at(slot_index_type slot_index, generation_type generation) const noexcept {
        return slot_index < k_capacity && filled_[slot_index]
            && generations_[slot_index] == generation;
    }

    /**
     * @brief Accesses an occupied slot as a const reference.
     * @param[in] slot_index Valid occupied slot index.
     * @return Stored value reference, valid until the slot is evicted or replaced.
     * @pre `slot_index < k_capacity` and the slot is occupied.
     */
    [[nodiscard]] [[gnu::always_inline]]
    const p_value_type& slot_at(slot_index_type slot_index) const noexcept {
        return slots_[slot_index];
    }

    /**
     * @brief Accesses an occupied slot as a mutable reference.
     * @param[in] slot_index Valid occupied slot index.
     * @return Stored value reference, valid until the slot is evicted or replaced.
     * @pre `slot_index < k_capacity` and the slot is occupied.
     */
    [[nodiscard]] [[gnu::always_inline]]
    p_value_type& slot_at(slot_index_type slot_index) noexcept {
        return slots_[slot_index];
    }

private:
    /**
     * @brief Calls cleanup and marks an occupied slot empty.
     * @pre slot_index is in range and occupied. The callback must not throw.
     */
    template <class p_eviction_callback>
    [[gnu::always_inline]] void evict_slot_unchecked(
        slot_index_type slot_index,
        p_eviction_callback&& on_evict) noexcept {
        std::forward<p_eviction_callback>(on_evict)(slots_[slot_index]);
        filled_[slot_index] = false;
        --valid_count_;
    }

    std::array<p_value_type, k_capacity> slots_{}; ///< Inline value storage.
    std::array<bool, k_capacity> filled_{}; ///< Occupancy flag for each slot.
    std::array<generation_type, k_capacity> generations_{}; ///< Generation per slot.
    std::size_t valid_count_ = 0; ///< Number of occupied slots.
    std::size_t oldest_index_ = 0; ///< Start slot for the next circular eviction scan.
    mutable std::size_t hint_index_ = 0; ///< Start position for the next lookup.
    generation_type next_generation_ = 0; ///< Last assigned generation, incremented before the next insertion.
};

} // namespace bioseq::common
