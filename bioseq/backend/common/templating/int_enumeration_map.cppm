/**
 * @file int_enumeration_map.cppm
 * @brief Assigns consecutive numbers to compile-time keys.
 *
 * Pack-search helpers check for duplicate keys and report missing keys.
 */
module;

#include <cstddef>

export module bioseq.common.templating.int_enumeration_map;

export import bioseq.common.templating.compile_time_string;
import bioseq.common.templating.pack_search;

export namespace bioseq::common::templating {

/**
 * @brief Assigns consecutive numbers to a non-empty list of unique keys.
 * @tparam p_start_index Number assigned to the first key.
 * @tparam p_keys Keys in enumeration order.
 */
template <auto p_start_index, auto... p_keys> struct int_enumeration_map {
    /// Identifies this type as an integer-enumeration map.
    static constexpr bool is_int_enumeration_map = true;

    /// Number of keys in the map.
    static constexpr std::size_t count = sizeof...(p_keys);
    static_assert(count > 0u, "int_enumeration_map: key pack must be non-empty");
    static_assert(unique_values_pack<p_keys...>(), "int_enumeration_map: keys must be unique");

    /// Integer type used for assigned values.
    using value_type = decltype(p_start_index);

    /// Value assigned to the first key.
    static constexpr value_type start = p_start_index;

    /**
     * @brief Number assigned to a key.
     * @tparam p_key Key to find. A missing key is a compile-time error.
     */
    template <auto p_key>
    static constexpr value_type field = static_cast<value_type>(
        static_cast<std::size_t>(p_start_index) + index_of_value_pack<p_key, p_keys...>());

    /// Value assigned to the final key.
    static constexpr value_type max_value =
        static_cast<value_type>(static_cast<std::size_t>(p_start_index) + (count - 1u));

    /**
     * @brief Checks whether the map contains a key.
     */
    template <auto p_key> static constexpr bool contains = contains_value_pack<p_key, p_keys...>();
};

/**
 * @brief Assigns consecutive numbers to string keys.
 * @tparam p_start_index Number assigned to the first string.
 * @tparam p_names Unique strings in enumeration order.
 *
 * String literals can be used directly when declaring the map or looking up
 * a key. `compile_time_string` stores their characters in template arguments.
 */
template <auto p_start_index, compile_time_string... p_names> struct string_enumeration_map {
    /// Map that assigns numbers to these strings.
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
     * @tparam p_key String key to find. Absence is a compile-time error.
     */
    template <compile_time_string p_key>
    static constexpr value_type field = core_type::template field<p_key>;

    /**
     * @brief Checks whether the map contains a string key.
     */
    template <compile_time_string p_key>
    static constexpr bool contains = core_type::template contains<p_key>;
};

} // namespace bioseq::common::templating
