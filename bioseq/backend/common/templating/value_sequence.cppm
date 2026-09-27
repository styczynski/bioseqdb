/**
 * @file value_sequence.cppm
 * @brief Groups compile-time values in a type for runtime dispatch.
 *
 * The values may be integers, enums, or class values allowed as  template
 * arguments.
 */
module;

#include <cstddef>

export module bioseq.common.templating.value_sequence;

export namespace bioseq::common::templating {

/**
 * @brief A list of compile-time values that all have the same type.
 * @tparam p_element_type Value type.
 * @tparam p_values Values in the sequence.
 */
template <class p_element_type, p_element_type... p_values>
struct value_sequence {
    /// Type shared by every value in the sequence.
    using value_type = p_element_type;

    /**
     * @brief Returns the number of values in the sequence.
     */
    [[nodiscard]] static constexpr std::size_t size() noexcept {
        return sizeof...(p_values);
    }
};

} // namespace bioseq::common::templating
