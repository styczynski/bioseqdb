/**
 * @file fragment_cache.cppm
 * @brief Provides a fixed-capacity cache with observable slot generations.
 *
 * Slots live inline, lookup is a bounded linear scan, and eviction invokes a
 * caller-owned cleanup callback before clearing the slot. Each replacement
 * increments a 64-bit generation so cached slot references can detect
 * eviction or reuse. A cache instance is single-threaded.
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
 * @brief Stores a bounded set of movable values in generation-tracked slots.
 *
 * The cache owns all values inline and performs no dynamic allocation. Calls
 * are not synchronized; one thread must own an instance. A slot reference is
 * valid only while `is_valid_at()` accepts its index/generation pair.
 *
 * @tparam p_value_type Movable value stored in each slot.
 * @tparam p_capacity Positive number of inline slots.
 *
 * @code{.cpp}
 * bioseq::common::fragment_cache<int, 4> cache;
 * const auto [slot_index, generation] =
 *     cache.push(42, [](int&) noexcept {});
 * const bool is_resident = cache.is_valid_at(slot_index, generation);
 * @endcode
 */
template <class p_value_type, std::size_t p_capacity>
class fragment_cache {
public:
    /** @brief Unsigned type used to address an inline slot. */
    using slot_index_type = std::size_t;

    /** @brief Unsigned type used for replacement generations. */
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
     * @brief Finds a resident value by a caller-defined key projection.
     *
     * Lookup starts at the previous hit/insert hint and examines at most
     * `k_capacity` slots. Updating the hint does not change cache contents.
     *
     * @tparam p_key_type Lookup key type.
     * @tparam p_key_extractor Callable returning the comparable key for a stored value.
     * @param[in] key Key to locate.
     * @param[in] key_extractor Projection invoked only for occupied slots.
     * @return Slot index and generation on a hit, or `std::nullopt` on a miss.
     * @warning `key_extractor` must not throw because lookup is `noexcept`.
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
     * @brief Inserts a value, evicting the oldest slot when the cache is full.
     *
     * The callback observes the old value before it is replaced. Its reference
     * is valid only for the callback duration.
     *
     * @tparam p_eviction_callback Callable accepting a mutable stored value.
     * @param[in] value Value moved into the selected slot.
     * @param[in] on_evict Callback used only when insertion must replace a value.
     * @return Selected slot index and its new generation.
     * @warning `on_evict` must not throw because insertion is `noexcept`.
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
     * @brief Evicts the oldest occupied slot when one exists.
     * @tparam p_eviction_callback Callable accepting a mutable stored value.
     * @param[in] on_evict Callback invoked before the selected slot is cleared.
     * @warning `on_evict` must not throw and may not retain its value reference.
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
     * @param[in] slot_index Slot to clear; out-of-range and empty slots are ignored.
     * @param[in] on_evict Callback invoked before an occupied slot is cleared.
     * @warning `on_evict` must not throw and may not retain its value reference.
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
     * @brief Tests whether a slot still contains the observed generation.
     * @param[in] slot_index Candidate slot index.
     * @param[in] generation Generation captured by the caller.
     * @return `true` only when the same slot generation remains resident.
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
     * @brief Invokes cleanup and marks a known occupied slot empty.
     * @tparam p_eviction_callback Callable accepting a mutable stored value.
     * @param[in] slot_index In-range occupied slot.
     * @param[in] on_evict Callback invoked before the slot state changes.
     * @pre `slot_index < k_capacity` and the slot is occupied.
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
    std::size_t oldest_index_ = 0; ///< Start position for the next age scan.
    mutable std::size_t hint_index_ = 0; ///< Start position for the next lookup.
    generation_type next_generation_ = 0; ///< Generation assigned by the next insert.
};

} // namespace bioseq::common
