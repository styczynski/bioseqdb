/**
 * @file int_enumeration_map.cppm
 * @brief Compile-time keyed map that assigns consecutive ints to a pack of
 *        structural NTTP keys (CompTimeStr / enum / integral).
 *
 * Mirrors the supported_alphabets<…> / alphabet_entry idiom but keys on *values*
 * (NTTPs) instead of *types*, so lookup delegates to
 * pack_search::index_of_value_pack (which already static_asserts on absence)
 * rather than re-implementing a linear search.
 *
 *   using F = int_enumeration_map<1, 7, 8, 9>;       // integral keys
 *   F::field<8>   == 2 ;  F::max_value == 3 ;  F::count == 3 ;  F::start == 1
 *
 *   using G = template_int_enumeration_map<1, "a", "b", "c">;  // string keys, bare literals
 *   G::field<"b"> == 2 ;  G::max_value == 3
 */
module;

#include <cstddef>

export module bioseq.common.templating.int_enumeration_map;

export import bioseq.common.templating.template_str;   // CompTimeStr (re-exported for use sites)
import bioseq.common.templating.pack_search;           // index_of_value_pack, contains_value_pack, unique_values_pack

export namespace bioseq::common::templating {

/// Generic core: keys are raw NTTPs (CompTimeStr, enum, integral). Accessors
/// are value forms (no parens) so `M::field<KEY>` and `M::max_value` are values.
template <auto StartIndex, auto... Keys>
struct int_enumeration_map {
    static constexpr bool is_int_enumeration_map = true;

    static constexpr std::size_t count = sizeof...(Keys);
    static_assert(count > 0u, "int_enumeration_map: key pack must be non-empty");
    static_assert(unique_values_pack<Keys...>(), "int_enumeration_map: keys must be unique");

    using value_type = decltype(StartIndex);
    static constexpr value_type start = StartIndex;

    /// Assigned int for KEY (StartIndex + position). Hard build error if KEY is
    /// absent — the static_assert lives inside index_of_value_pack.
    template <auto Key>
    static constexpr value_type field =
        static_cast<value_type>(static_cast<std::size_t>(StartIndex)
                                + index_of_value_pack<Key, Keys...>());

    /// Last assigned int (== field of the final key).
    static constexpr value_type max_value =
        static_cast<value_type>(static_cast<std::size_t>(StartIndex) + (count - 1u));

    /// Non-failing membership probe.
    template <auto Key>
    static constexpr bool contains = contains_value_pack<Key, Keys...>();
};

/// String-ergonomic facade: keys declared as bare string literals
/// (`<1, "database_oid", …>`) and looked up as bare literals
/// (`field<"database_oid">`). Pure forwarding to the generic core; the only
/// difference is the parameter *kind* (`CompTimeStr` instead of `auto`), which is
/// what lets a bare literal bind. No logic is duplicated.
template <auto StartIndex, CompTimeStr... Names>
struct template_int_enumeration_map {
    using core = int_enumeration_map<StartIndex, Names...>;

    using value_type = typename core::value_type;
    static constexpr value_type   start     = core::start;
    static constexpr std::size_t  count     = core::count;
    static constexpr value_type   max_value = core::max_value;

    template <CompTimeStr Key>
    static constexpr value_type field = core::template field<Key>;

    template <CompTimeStr Key>
    static constexpr bool contains = core::template contains<Key>;
};

// ---- self-tests (house style: module-scope static_assert) ----
namespace detail_int_enum_map_tests {
    // String keys, bare literals — the seq_sub_table use case.
    using S = template_int_enumeration_map<1, "database_oid", "tablespace_oid", "shard_idx">;
    static_assert(S::start     == 1);
    static_assert(S::count     == 3u);
    static_assert(S::max_value == 3);
    static_assert(S::field<"database_oid">   == 1);
    static_assert(S::field<"tablespace_oid"> == 2);
    static_assert(S::field<"shard_idx">      == 3);
    static_assert(S::field<"shard_idx">      == S::max_value);
    static_assert(S::contains<"shard_idx">);
    static_assert(!S::contains<"nope">);

    // Generic core: integral keys, non-1 start.
    using N = int_enumeration_map<10, 7, 8, 9>;
    static_assert(N::field<7> == 10);
    static_assert(N::field<9> == 12);
    static_assert(N::max_value == 12);

    // Generic core: enum keys.
    enum class Col { db, tbl, rel };
    using E = int_enumeration_map<0, Col::db, Col::tbl, Col::rel>;
    static_assert(E::field<Col::rel> == 2);
}

} // namespace bioseq::common::templating
