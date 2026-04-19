/*-------------------------------------------------------------------------
 *
 * pg_utils/bitmapset.cppm
 *
 * Standard-layout wrapper around PostgreSQL's Bitmapset*. Exposes the
 * bitmap-set C API (bms_add_member, bms_is_member, bms_next_member,
 * bms_num_members, ...) as named methods plus a forward iterator so
 * callers can write range-based for loops and chained mutations instead
 * of the raw `bms_*` calls.
 *
 * Layout contract: the class holds exactly one Bitmapset* pointer (same
 * footprint as the raw field it replaces) and is verified as
 * std::is_standard_layout so it stays ABI-compatible with a `Bitmapset*`
 * slot inside a struct shared with PostgreSQL's C code. See
 * static_assert block at the bottom of this file for the invariants.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <iterator>
#include <type_traits>

export module pg_utils.bitmapset;

/*
 * Forward iterator over the members of a PostgreSQL Bitmapset. Yields the
 * next set bit on each dereference by calling bms_next_member. Compares
 * against std::default_sentinel_t so range-based for loops terminate when
 * the underlying bms_next_member hits the -2 sentinel.
 */
export class p_bitmapset_iterator
{
private:
    const Bitmapset* set;
    int current_bit;

public:
    using difference_type = std::ptrdiff_t;
    using value_type = int;

    p_bitmapset_iterator() : set(nullptr), current_bit(-2) {}
    explicit p_bitmapset_iterator(const Bitmapset* source_set)
        : set(source_set), current_bit(bms_next_member(source_set, -1)) {}

    [[nodiscard]] int operator*() const { return current_bit; }

    p_bitmapset_iterator& operator++()
    {
        current_bit = bms_next_member(set, current_bit);
        return *this;
    }

    p_bitmapset_iterator operator++(int)
    {
        p_bitmapset_iterator snapshot{ *this };
        ++*this;
        return snapshot;
    }

    [[nodiscard]] bool operator==(const p_bitmapset_iterator& other) const
    {
        return other.current_bit == current_bit;
    }

    [[nodiscard]] bool operator==(const std::default_sentinel_t&) const
    {
        return current_bit < 0;
    }
};

/*
 * PBitmapset wraps a raw Bitmapset* so struct fields can hold the set
 * through a named type while keeping the same memory layout as the C
 * pointer. The empty set is represented by a null pointer, matching the
 * convention used by the underlying bms_* API (see bitmapset.h).
 *
 * Mutating methods follow the PG idiom where each operation returns a
 * possibly-new set pointer; the wrapper reassigns the internal pointer
 * from the returned value so the caller does not have to re-bind.
 */
export class PBitmapset
{
private:
    Bitmapset* set;

public:
    PBitmapset() : set(nullptr) {}
    /*
     * Implicit construction from a raw Bitmapset* keeps existing call
     * sites that still hand us a raw pointer working without a static
     * cast. Use ptr()/release() for the reverse direction.
     */
    PBitmapset(Bitmapset* source_set) : set(source_set) {}

    /*
     * Returns a PBitmapset covering the integer range [0, natts). Used
     * by every callsite that wants "all columns of this relation" as
     * the attr_needed set; consolidates the recurring
     * `bms_add_range(NULL, 0, natts - 1)` idiom.
     */
    [[nodiscard]] static PBitmapset full_range(int element_count)
    {
        return PBitmapset{ bms_add_range(nullptr, 0, element_count - 1) };
    }

    /* Returns the raw Bitmapset* for passing to external C APIs. */
    [[nodiscard]] Bitmapset* ptr() const { return set; }

    /*
     * Implicit conversion to Bitmapset* so legacy call sites that still
     * hand off to bms_* / bioseq_* functions taking a raw pointer keep
     * compiling. All such functions are NULL-safe, matching the empty-set
     * convention already baked into the PG API.
     */
    [[nodiscard]] operator Bitmapset*() const { return set; }

    /* True when the set contains no members (empty set == NULL). */
    [[nodiscard]] bool empty() const { return set == nullptr; }

    /* Number of members currently in the set. */
    [[nodiscard]] int size() const { return bms_num_members(set); }

    /* True when the given bit is a member of the set. */
    [[nodiscard]] bool contains(int member_bit) const
    {
        return bms_is_member(member_bit, set);
    }

    /*
     * Adds the given bit to the set; updates the stored pointer in case
     * bms_add_member reallocated the backing storage. Returns *this to
     * allow chaining.
     */
    PBitmapset& add(int member_bit)
    {
        set = bms_add_member(set, member_bit);
        return *this;
    }

    /*
     * Adds every integer in the closed range [lower_bit, upper_bit] to
     * the set. Mirrors bms_add_range semantics.
     */
    PBitmapset& add_range(int lower_bit, int upper_bit)
    {
        set = bms_add_range(set, lower_bit, upper_bit);
        return *this;
    }

    /* Removes the given bit from the set if present. */
    PBitmapset& remove(int member_bit)
    {
        set = bms_del_member(set, member_bit);
        return *this;
    }

    /*
     * Releases the backing storage with bms_free and resets the pointer
     * to null so the wrapper once again represents the empty set.
     */
    void free()
    {
        bms_free(set);
        set = nullptr;
    }

    /*
     * Releases ownership of the underlying pointer and resets to empty.
     * Useful when handing the pointer to an external consumer that will
     * manage the lifetime.
     */
    [[nodiscard]] Bitmapset* release()
    {
        Bitmapset* released = set;
        set = nullptr;
        return released;
    }

    /* Deep copy the underlying set using bms_copy. */
    [[nodiscard]] PBitmapset copy() const { return PBitmapset{ bms_copy(set) }; }

    [[nodiscard]] bool equals(const PBitmapset& other) const
    {
        return bms_equal(set, other.set);
    }

    [[nodiscard]] p_bitmapset_iterator begin() const
    {
        return p_bitmapset_iterator(set);
    }
    [[nodiscard]] p_bitmapset_iterator end() const { return p_bitmapset_iterator(); }
};

/*
 * Layout invariants: PBitmapset is embedded into structs that are also
 * accessed from PostgreSQL's C code (e.g., BioseqScanState), where the
 * original field was a plain Bitmapset*. The wrapper must therefore be
 * the same size / alignment as a raw Bitmapset* and must remain
 * standard-layout so any reinterpret_cast round-trip on the containing
 * struct stays well-defined.
 */
static_assert(sizeof(PBitmapset) == sizeof(Bitmapset*),
              "PBitmapset must match the memory footprint of Bitmapset*");
static_assert(alignof(PBitmapset) == alignof(Bitmapset*),
              "PBitmapset must match the alignment of Bitmapset*");
static_assert(std::is_standard_layout_v<PBitmapset>,
              "PBitmapset must remain standard-layout so embedding inside ABI-shared structs stays safe");
