/**
 * @file
 * @brief Fixed-storage circular buffer for delayed streaming algorithm values.
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

/// @brief Strongly typed slot position within a @ref circular_append_buffer.
BOOST_STRONG_TYPEDEF(std::size_t, circular_append_buffer_position);

/**
 * @brief Placeholder allocator for buffers whose storage is an inline array.
 *
 * @tparam p_value_type Element type carried by the owning buffer.
 * @tparam p_storage_type Storage type carried by the owning buffer.
 */
template <typename p_value_type, typename p_storage_type>
struct unavailable_circular_append_buffer_allocator {};

/**
 * @brief Circular write buffer backed by inline or allocator-provided storage.
 *
 * The buffer owns its storage and is not synchronized. Dynamic-capacity
 * instances wrap at the constructor-supplied capacity. Fixed-capacity
 * instances use the next power-of-two storage bound and a mask. Accessors do
 * not validate positions because this type serves latency-sensitive streaming
 * loops whose bounds are established by the owning algorithm.
 *
 * @tparam p_value_type Stored element type.
 * @tparam p_requested_capacity Compile-time upper bound for allocated slots.
 * @tparam p_alignment Alignment of the storage field. Zero requests no additional alignment.
 * @tparam p_is_fixed_capacity Whether wrapping uses the compile-time power-of-two capacity.
 * @tparam p_storage_type Inline storage when `void`, otherwise an indexable owning storage type.
 * @tparam p_allocator_template Allocator used only for non-void storage types.
 *
 * @code{.cpp}
 * bioseq::common::circular_append_buffer<int, 8> values{8};
 * values.push_back(3);
 * const int latest = values.get_last_value();
 * @endcode
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

    /// @brief Strong slot-position type used by traversal methods.
    using position_type = circular_append_buffer_position;

    /// @brief Number of physically available slots, rounded up to a power of two.
    static constexpr std::size_t k_storage_size =
        std::size_t{1} << seqan3::detail::ceil_log2(p_requested_capacity);

    /// @brief Bit mask used by fixed-capacity position advancement.
    static constexpr std::size_t k_size_mask = k_storage_size - 1u;

private:
    /**
     * @brief Concrete inline-array or allocator-owned storage representation.
     *
     * `std::array` preserves contiguous inline storage without allocation while
     * making the fixed extent part of the storage type.
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
     * @brief Returns the active number of slots.
     *
     * @return Runtime capacity or the fixed power-of-two storage capacity.
     */
    [[nodiscard, gnu::always_inline]] constexpr std::size_t get_capacity() const {
        if constexpr (p_is_fixed_capacity) {
            return k_storage_size;
        } else {
            return static_cast<std::size_t>(capacity_);
        }
    }

    /**
     * @brief Returns the first slot position.
     *
     * @return Position representing slot zero.
     */
    [[nodiscard, gnu::always_inline]] static constexpr position_type
    get_begin_position() {
        return position_type{0u};
    }

    /**
     * @brief Returns a read-only value at a trusted slot position.
     *
     * @param[in] position Position known to be below @ref get_capacity.
     * @return Reference to the stored value.
     */
    [[nodiscard, gnu::always_inline]] constexpr const p_value_type& get_value_at(
        const position_type& position) const {
        return buffer_[position];
    }

    /**
     * @brief Returns a mutable value at a trusted slot position.
     *
     * @param[in] position Position known to be below @ref get_capacity.
     * @return Mutable reference to the stored value.
     */
    [[nodiscard, gnu::always_inline]] constexpr p_value_type& get_value_at(
        const position_type& position) {
        return buffer_[position];
    }

    /**
     * @brief Returns a read-only pointer at a trusted slot position.
     *
     * @param[in] position Position known to be below @ref get_capacity.
     * @return Pointer into this buffer's storage.
     */
    [[nodiscard, gnu::always_inline]] constexpr const p_value_type* get_pointer_at(
        const position_type& position) const {
        return &buffer_[position];
    }

    /**
     * @brief Returns a mutable pointer at a trusted slot position.
     *
     * @param[in] position Position known to be below @ref get_capacity.
     * @return Mutable pointer into this buffer's storage.
     */
    [[nodiscard, gnu::always_inline]] constexpr p_value_type* get_pointer_at(
        const position_type& position) {
        return &buffer_[position];
    }

    /**
     * @brief Returns the slots between the write position and the physical end.
     *
     * @return Contiguous slot count available before wrapping.
     */
    [[nodiscard, gnu::always_inline]] constexpr std::size_t
    get_contiguous_capacity() const {
        return get_capacity() - static_cast<std::size_t>(write_position_);
    }

    /**
     * @brief Advances and wraps a position for a runtime-capacity buffer.
     *
     * @param[in,out] position Position to advance.
     * @param[in] increment Number of slots to advance.
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
     * @brief Advances and masks a position for a fixed-capacity buffer.
     *
     * @param[in,out] position Position to advance.
     * @param[in] increment Number of slots to advance.
     */
    [[gnu::always_inline]] constexpr void advance_position(
        position_type& position,
        std::size_t increment = 1u)
        requires(p_is_fixed_capacity) {
        position = (position + increment) & k_size_mask;
    }

    /**
     * @brief Appends a trivially copyable value and advances the write position.
     *
     * @param[in] value Value copied into the current slot.
     */
    [[gnu::always_inline]] inline void push_back(p_value_type value)
        requires std::is_trivially_copyable_v<p_value_type> {
        previous_write_position_ = write_position_;
        buffer_[write_position_] = value;
        advance_position(write_position_);
    }

    /**
     * @brief Appends a convertible nontrivial value and advances the write position.
     *
     * @tparam p_input_type Source value type.
     * @param[in] value Value forwarded into the current slot.
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
