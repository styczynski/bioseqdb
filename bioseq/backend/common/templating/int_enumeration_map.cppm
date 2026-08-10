/**
 * @file int_enumeration_map.cppm
 * @brief Compile-time maps from structural keys to consecutive integer values.
 *
 * Lookup delegates to the shared value-pack search primitives, keeping key
 * validation and absence diagnostics in one owner.
 */
module;

#include <cstddef>

export module bioseq.common.templating.int_enumeration_map;

export import bioseq.common.templating.compile_time_string;
import bioseq.common.templating.pack_search;

export namespace bioseq::common::templating {

/**
 * @brief Assigns consecutive values to a non-empty pack of structural keys.
 * @tparam p_start_index Value assigned to the first key.
 * @tparam p_keys Unique structural keys in enumeration order.
 */
template <auto p_start_index, auto... p_keys>
struct int_enumeration_map {
    /// Identifies this type as an integer-enumeration map.
    static constexpr bool is_int_enumeration_map = true;

    /// Number of keys in the map.
    static constexpr std::size_t count = sizeof...(p_keys);
    static_assert(count > 0u, "int_enumeration_map: key pack must be non-empty");
    static_assert(
        unique_values_pack<p_keys...>(),
        "int_enumeration_map: keys must be unique");

    /// Integer type used for assigned values.
    using value_type = decltype(p_start_index);

    /// Value assigned to the first key.
    static constexpr value_type start = p_start_index;

    /**
     * @brief Value assigned to a key.
     * @tparam p_key Structural key to find; absence is a compile-time error.
     */
    template <auto p_key>
    static constexpr value_type field = static_cast<value_type>(
        static_cast<std::size_t>(p_start_index)
        + index_of_value_pack<p_key, p_keys...>());

    /// Value assigned to the final key.
    static constexpr value_type max_value = static_cast<value_type>(
        static_cast<std::size_t>(p_start_index) + (count - 1u));

    /**
     * @brief Reports whether a key belongs to the map.
     * @tparam p_key Structural key to probe.
     */
    template <auto p_key>
    static constexpr bool contains = contains_value_pack<p_key, p_keys...>();
};

/**
 * @brief String-literal facade over `int_enumeration_map`.
 * @tparam p_start_index Value assigned to the first string key.
 * @tparam p_names Unique string keys in enumeration order.
 *
 * Constraining the key pack to `compile_time_string` permits bare string
 * literals in both the map declaration and its lookup expressions.
 */
template <auto p_start_index, compile_time_string... p_names>
struct string_enumeration_map {
    /// Underlying generic map implementation.
    using core_type = int_enumeration_map<p_start_index, p_names...>;

    /// Integer type used for assigned values.
    using value_type = typename core_type::value_type;

    /// Value assigned to the first key.
    static constexpr value_type start = core_type::start;

    /// Number of keys in the map.
    static constexpr std::size_t count = core_type::count;

    /// Value assigned to the final key.
    static constexpr value_type max_value = core_type::max_value;

    /**
     * @brief Value assigned to a string key.
     * @tparam p_key String key to find; absence is a compile-time error.
     */
    template <compile_time_string p_key>
    static constexpr value_type field = core_type::template field<p_key>;

    /**
     * @brief Reports whether a string key belongs to the map.
     * @tparam p_key String key to probe.
     */
    template <compile_time_string p_key>
    static constexpr bool contains = core_type::template contains<p_key>;
};

} // namespace bioseq::common::templating
