/**
 * @file
 * @brief Defines fmt adapters for reflected enums and diagnostic string values.
 */
module;

#include <rfl/thirdparty/enchantum/bitflags.hpp>
#include <rfl/thirdparty/enchantum/enchantum.hpp>

export module bioseq.common.logging:formatters;

import :fmtlog_backend;
import std;

namespace bioseq::common::formatting_detail {

/**
 * @brief Produces a stable textual representation of an enum value.
 *
 * @tparam p_enum_type Reflected enum type.
 * @param[in] value Enum value to format.
 * @return Reflected name, reflected bit-flag expression, or the underlying integer.
 */
template<enchantum::Enum p_enum_type>
std::string enum_to_string(p_enum_type value)
{
    if constexpr (enchantum::is_bitflag<p_enum_type>) {
        auto name = enchantum::to_string_bitflag(value);
        if (!name.empty()) {
            return name;
        }
    } else {
        auto name = enchantum::to_string(value);
        if (!name.empty()) {
            return std::string{name.data(), name.size()};
        }
    }
    return std::to_string(+enchantum::to_underlying(value));
}

} // namespace bioseq::common::formatting_detail

export {
namespace fmt {

/**
 * @brief Formats reflected enum values by name and falls back to their integer value.
 *
 * @tparam p_value_type Reflected enum type.
 */
template<typename p_value_type>
    requires enchantum::Enum<p_value_type>
struct formatter<p_value_type> : formatter<std::string> {
    /**
     * @brief Writes the reflected enum representation to a fmt context.
     *
     * @tparam p_format_context Destination fmt context type.
     * @param[in] value Enum value to format.
     * @param[in,out] context Destination formatting context.
     * @return Iterator immediately after the formatted value.
     */
    template<typename p_format_context>
    auto format(const p_value_type& value, p_format_context& context) const
    {
        return formatter<std::string>::format(
            bioseq::common::formatting_detail::enum_to_string(value),
            context);
    }
};

/**
 * @brief Formats values that expose a string-valued diagnostic representation.
 *
 * @tparam p_value_type Type exposing to_debug_string().
 */
template<typename p_value_type>
    requires requires(const p_value_type& value) {
        { value.to_debug_string() } -> std::convertible_to<std::string>;
    }
struct formatter<p_value_type> : formatter<std::string> {
    /**
     * @brief Writes the value's diagnostic representation to a fmt context.
     *
     * @tparam p_format_context Destination fmt context type.
     * @param[in] value Value to format.
     * @param[in,out] context Destination formatting context.
     * @return Iterator immediately after the formatted value.
     */
    template<typename p_format_context>
    auto format(const p_value_type& value, p_format_context& context) const
    {
        return formatter<std::string>::format(value.to_debug_string(), context);
    }
};

} // namespace fmt
}
