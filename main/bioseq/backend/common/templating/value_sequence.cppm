/**
 * @file value_sequence.cppm
 * @brief Unconstrained carrier for a non-type template-argument pack.
 *
 * `std::integer_sequence` accepts integral element types only. This carrier also
 * supports enumeration and structural class values used by runtime dispatch.
 */
module;

#include <cstddef>

export module bioseq.common.templating.value_sequence;

export namespace bioseq::common::templating {

/**
 * @brief Carries a homogeneous pack of structural compile-time values.
 * @tparam element_type Value type.
 * @tparam values Values in the sequence.
 */
template <class element_type, element_type... values>
struct value_sequence {
    /// Type shared by every value in the sequence.
    using value_type = element_type;

    /// Returns the number of compile-time values.
    [[nodiscard]] static constexpr std::size_t size() noexcept {
        return sizeof...(values);
    }
};

} // namespace bioseq::common::templating
