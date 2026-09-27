/**
 * @file
 * @brief Circular buffer for values kept by streaming algorithms.
 */
module;

#include <boost/serialization/strong_typedef.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

export module bioseq.common.circular_append_buffer;

import bioseq.common.seqan_adapter;

export namespace bioseq::common {

/// @brief Slot index for a @ref circular_append_buffer.
BOOST_STRONG_TYPEDEF(std::size_t, circular_append_buffer_position);

/**
 * @brief Default allocator type for buffers that  store an inline array.
 *
 * It has no allocate function because inline storage needs no allocation.
 */
template <typename p_value_type, typename p_storage_type>
struct unavailable_circular_append_buffer_allocator {};

/**
 * @brief Stores a window of values and overwrites old slots when it wraps.
 *
 * The buffer owns its storage. Callers track how many values have been
 * written and keep slot indices in range, since accessors do no bounds
 * checking and the buffer has no synchronization Runtime capacity sets
 * the wrap point. Fixed capacity rounds the requested size up to a power
 * of two, which allows wrapping with a mask.
 *
 * @tparam p_value_type Stored element type.
 * @tparam p_requested_capacity Requested storage size before rounding.
 * @tparam p_alignment Storage  alignment. Zero adds no alignment requirement.
 * @tparam p_is_fixed_capacity Use the rounded size as the wrap point.
 * @tparam p_storage_type Inline array when void, otherwise an owning indexable type.
 * @tparam p_allocator_template Allocates storage when p_storage_type is non-void.
 */
template <
    typename p_value_type,
    std::size_t p_requested_capacity,
    std::size_t p_alignment = 16u,
    bool p_is_fixed_capacity = false,
    typename p_storage_type = void,
    template <typename p_allocated_value_type, typename p_allocated_storage_type>
    typename p_allocator_template = unavailable_circular_append_buffer_allocator>
class circular_append_buffer {
public:
    static_assert(p_requested_capacity > 0u);

    /// @brief Slot index used by buffer access and traversal.
    using position_type = circular_append_buffer_position;

    /// @brief Requested cmpile-time capacity rounded up to a power of two.
    static constexpr std::size_t k_storage_size =
        std::size_t{1} << seqan3::detail::ceil_log2(p_requested_capacity);

    /// @brief Mask used to wrap fixed-capacity slot positions.
    static constexpr std::size_t k_size_mask = k_storage_size - 1u;

private:
    /**
     * @brief Inline array by, default, or the caller's owning storage type.
     */
    using storage_type = std::conditional_t<
        std::is_same_v<p_storage_type, void>,
        std::array<p_value_type, k_storage_size>,
        p_storage_type>;

    alignas(p_alignment) storage_type buffer_;
    position_type write_position_;
    [[indeterminate]] position_type previous_write_position_;
    [[no_unique_address]] std::conditional_t<
        p_is_fixed_capacity,
        std::monostate,
        position_type>
        capacity_;

public:
    /// @brief Constructs a fixed-capacity buffer with inline storage.
    explicit constexpr circular_append_buffer()
        requires(p_is_fixed_capacity && std::is_same_v<p_storage_type, void>)
        : write_position_(0u) {}

    /**
     * @brief Constructs a runtime-capacity buffer with inline storage.
     *
     * @param[in] capacity Active slot count.
     * @pre `capacity` is in `[1, k_storage_size]`.
     */
    explicit constexpr circular_append_buffer(std::size_t capacity)
        requires(!p_is_fixed_capacity && std::is_same_v<p_storage_type, void>)
        : write_position_(0u), capacity_(capacity) {}

    /// @brief Constructs a fixed-capacity buffer with allocator-provided storage.
    explicit constexpr circular_append_buffer()
        requires(p_is_fixed_capacity && !std::is_same_v<p_storage_type, void>)
        : buffer_(p_allocator_template<p_value_type, p_storage_type>::allocate(
              k_storage_size)),
          write_position_(0u) {}

    /**
     * @brief Constructs a runtime-capacity buffer with allocator-provided storage.
     *
     * @param[in] capacity Active slot count passed to the allocator.
     * @pre `capacity` is in `[1, k_storage_size]`.
     */
    explicit constexpr circular_append_buffer(std::size_t capacity)
        requires(!p_is_fixed_capacity && !std::is_same_v<p_storage_type, void>)
        : buffer_(std::move(
              p_allocator_template<p_value_type, p_storage_type>::allocate(capacity))),
          write_position_(0u),
          capacity_(capacity) {}

    /**
     * @brief Returns the number of slots used before wrapping.
     *
     * This is  the requested runtime capacity or the rounded fixed capacity.
     */
    [[nodiscard, gnu::always_inline]] constexpr std::size_t get_capacity() const {
        if constexpr (p_is_fixed_capacity) {
            return k_storage_size;
        } else {
            return static_cast<std::size_t>(capacity_);
        }
    }

    /**
     * @brief Returns the index of slot zero.
     */
    [[nodiscard, gnu::always_inline]] static constexpr position_type
    get_begin_position() {
        return position_type{0u};
    }

    /**
     * @brief Reads a slot without checking its index.
     * @pre position is below get_capacity() and the slot contains a value.
     */
    [[nodiscard, gnu::always_inline]] constexpr const p_value_type& get_value_at(
        const position_type& position) const {
        return buffer_[position];
    }

    /**
     * @brief Returns a mutable reference to a slot.
     * @pre position is below get_capacity(). No bounds check is performed.
     */
    [[nodiscard, gnu::always_inline]] constexpr p_value_type& get_value_at(
        const position_type& position) {
        return buffer_[position];
    }

    /**
     * @brief Returns a read-only pointer to a slot.
     * @pre position is below get_capacity(). No bounds check is performed.
     */
    [[nodiscard, gnu::always_inline]] constexpr const p_value_type* get_pointer_at(
        const position_type& position) const {
        return &buffer_[position];
    }

    /**
     * @brief Returns a mutable pointer to a slot.
     * @pre position is below get_capacity(). No bounds check is performed.
     */
    [[nodiscard, gnu::always_inline]] constexpr p_value_type* get_pointer_at(
        const position_type& position) {
        return &buffer_[position];
    }

    /**
     * @brief Returns how many writes fit before the next wrap.
     */
    [[nodiscard, gnu::always_inline]] constexpr std::size_t
    get_contiguous_capacity() const {
        return get_capacity() - static_cast<std::size_t>(write_position_);
    }

    /**
     * @brief Advances a slot index, wrapping at the runtime capacity.
     * @param[in,out] position Slot index to advance.
     * @param[in] increment Number of slots to advance
     */
    [[gnu::always_inline]] constexpr void advance_position(
        position_type& position,
        std::size_t increment = 1u)
        requires(!p_is_fixed_capacity) {
        static_assert(std::is_same_v<decltype(capacity_), position_type>);
        position += increment;
        while (position >= capacity_) {
            position -= capacity_;
        }
    }

    /**
     * @brief Advances a slot index, wrapping at the fixed capacity.
     * @param[in,out] position Slot index to advance.
     * @param[in] increment Number of slots to advance.
     */
    [[gnu::always_inline]] constexpr void advance_position(
        position_type& position,
        std::size_t increment = 1u)
        requires(p_is_fixed_capacity) {
        position = (position + increment) & k_size_mask;
    }

    /**
     * @brief Writes a value and moves to  the next slot.
     *
     * Overwrites the old value when the buffer wraps.
     * @param[in] value Value copied into the current slot.
     */
    [[gnu::always_inline]] inline void push_back(p_value_type value)
        requires std::is_trivially_copyable_v<p_value_type> {
        previous_write_position_ = write_position_;
        buffer_[write_position_] = value;
        advance_position(write_position_);
    }

    /**
     * @brief Forwards a value into the next slot, overwriting its old value.
     * @param[in] value Value assigned to the slot.
     */
    template <typename p_input_type>
        requires(!std::is_trivially_copyable_v<p_value_type>) &&
                std::convertible_to<p_input_type, p_value_type>
    [[gnu::always_inline]] inline void push_back(p_input_type&& value) {
        previous_write_position_ = write_position_;
        buffer_[write_position_] = std::forward<p_input_type>(value);
        advance_position(write_position_);
    }

    /**
     * @brief Returns the position written by the most recent append.
     *
     * @return Most recently written slot position.
     * @pre At least one value has been appended.
     */
    [[nodiscard, gnu::always_inline]] position_type get_last_write_position() const {
        return previous_write_position_;
    }

    /**
     * @brief Returns the value written by the most recent append.
     *
     * @return Mutable reference to the most recently written value.
     * @pre At least one value has been appended.
     */
    [[nodiscard, gnu::always_inline]] p_value_type& get_last_value() {
        return buffer_[previous_write_position_];
    }

    /**
     * @brief Visits every slot from oldest to newest in a full runtime buffer.
     *
     * @tparam p_function Callable accepting `(position, value, distance_from_newest)`.
     * @param[in] function Visitor invoked once per active slot.
     * @pre The buffer is full and its write position identifies the oldest slot.
     */
    template <typename p_function>
    [[gnu::always_inline]] inline void for_each_filled_value_unsafe(p_function function)
        requires(!p_is_fixed_capacity) {
        std::size_t distance_from_newest = get_capacity() - 1u;
        [[assume(static_cast<std::size_t>(write_position_) < get_capacity())]];

        for (auto position = write_position_; position < capacity_;
             ++position, --distance_from_newest) {
            function(position, buffer_[position], distance_from_newest);
        }
        for (auto position = get_begin_position(); position < write_position_;
             ++position, --distance_from_newest) {
            function(position, buffer_[position], distance_from_newest);
        }
    }
};

} // namespace bioseq::common
