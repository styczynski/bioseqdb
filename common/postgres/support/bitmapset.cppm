/**
 * @file common/postgres/support/bitmapset.cppm
 * @brief C++ iteration and updates for PostgreSQL bitmap sets.
 */
module;

extern "C" {
#include "postgres.h"

#include "nodes/bitmapset.h"
}

#include <cstddef>
#include <iterator>
#include <type_traits>

export module bioseq.postgres.support:bitmapset;

export namespace bioseq::postgres::support {

/** Forward iterator over set bits in a PostgreSQL Bitmapset. */
class postgres_bitmapset_iterator final {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = int;

    /** Constructs the end iterator. */
    constexpr postgres_bitmapset_iterator() noexcept = default;

    /** Starts iteration at the first member of @p source_set. */
    explicit postgres_bitmapset_iterator(const Bitmapset* source_set) noexcept
        : set_(source_set), current_bit_(bms_next_member(source_set, -1)) {}

    /** Returns the current set-bit index. */
    [[nodiscard]] int operator*() const noexcept {
        return current_bit_;
    }

    /** Advances to the next set bit. */
    postgres_bitmapset_iterator& operator++() noexcept {
        current_bit_ = bms_next_member(set_, current_bit_);
        return *this;
    }

    /** Advances and returns the iterator's previous position. */
    postgres_bitmapset_iterator operator++(int) noexcept {
        postgres_bitmapset_iterator previous{*this};
        ++*this;
        return previous;
    }

    /** Compares two iterators by their current bit index. */
    [[nodiscard]] bool operator==(const postgres_bitmapset_iterator& other) const noexcept {
        return current_bit_ == other.current_bit_;
    }

    /** Returns whether PostgreSQL's negative end sentinel was reached. */
    [[nodiscard]] bool operator==(const std::default_sentinel_t&) const noexcept {
        return current_bit_ < 0;
    }

  private:
    const Bitmapset* set_ = nullptr;
    int current_bit_ = -2;
};

/**
 * Wrap a PostgreSQL Bitmapset for iteration and updates.
 * Updates may reallocate the set, so the wrapper saves the pointer PostgreSQL
 * returns. Destruction leaves the allocation, alone. Call clear to free  it, or
 * release to hand its pointer to another owner.
 */
class postgres_bitmapset final {
  public:
    /** Constructs an empty bitmap-set view. */
    constexpr postgres_bitmapset() noexcept = default;

    /** Wraps @p source_set without copying or automatic ownership. */
    explicit constexpr postgres_bitmapset(Bitmapset* source_set) noexcept : set_(source_set) {}

    /** Creates a set containing every integer in `[0, member_count)`. */
    [[nodiscard]] static postgres_bitmapset make_member_range(int member_count) {
        return postgres_bitmapset{bms_add_range(nullptr, 0, member_count - 1)};
    }

    /** Returns the wrapped PostgreSQL Bitmapset pointer. */
    [[nodiscard]] Bitmapset* get() const noexcept {
        return set_;
    }

    /** Returns whether the set has no members. */
    [[nodiscard]] bool is_empty() const noexcept {
        return set_ == nullptr;
    }

    /** Returns the number of set bits. */
    [[nodiscard]] int size() const noexcept {
        return bms_num_members(set_);
    }

    /** Returns whether @p member_bit belongs to this set. */
    [[nodiscard]] bool has_member(int member_bit) const noexcept {
        return bms_is_member(member_bit, set_);
    }

    /** Adds @p member_bit and saves the resulting PostgreSQL set pointer. */
    postgres_bitmapset& add(int member_bit) {
        set_ = bms_add_member(set_, member_bit);
        return *this;
    }

    /** Adds every bit in the closed range `[lower_bit, upper_bit]`. */
    postgres_bitmapset& add_range(int lower_bit, int upper_bit) {
        set_ = bms_add_range(set_, lower_bit, upper_bit);
        return *this;
    }

    /** Removes @p member_bit when present. */
    postgres_bitmapset& remove(int member_bit) noexcept {
        set_ = bms_del_member(set_, member_bit);
        return *this;
    }

    /** Frees the backing set and resets this view to empty. */
    void clear() noexcept {
        bms_free(set_);
        set_ = nullptr;
    }

    /** Releases the wrapped pointer and resets this view to empty. */
    [[nodiscard]] Bitmapset* release() noexcept {
        Bitmapset* released = set_;
        set_ = nullptr;
        return released;
    }

    /** Returns a deep copy of this set. */
    [[nodiscard]] postgres_bitmapset clone() const {
        return postgres_bitmapset{bms_copy(set_)};
    }

    /** Returns whether this set has the same members as @p other. */
    [[nodiscard]] bool is_equal_to(const postgres_bitmapset& other) const noexcept {
        return bms_equal(set_, other.set_);
    }

    /** Returns an iterator positioned at the first set bit. */
    [[nodiscard]] postgres_bitmapset_iterator begin() const noexcept {
        return postgres_bitmapset_iterator{set_};
    }

    /** Return the iterator used to mark the end of the set. */
    [[nodiscard]] postgres_bitmapset_iterator end() const noexcept {
        return {};
    }

  private:
    Bitmapset* set_ = nullptr;
};

static_assert(sizeof(postgres_bitmapset) == sizeof(Bitmapset*));
static_assert(alignof(postgres_bitmapset) == alignof(Bitmapset*));
static_assert(std::is_standard_layout_v<postgres_bitmapset>);

} // namespace bioseq::postgres::support
