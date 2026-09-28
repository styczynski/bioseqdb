/**
 * @file index.cppm
 * @brief Compares column values with a scalar and combines Boolean result masks.
 *
 * Comparisons use C++ operators and their usual  type conversions. Mask AND and
 * OR use the Highway implementation.
 */
module;

#include <concepts>
#include <cstdint>

export module bioseq.column.vector_compare;

export namespace bioseq::column::vec {

/** @brief Comparison operation applied to a column value and a scalar operand. */
enum class comparison_operation : std::uint8_t {
    equal,          ///< Tests equality.
    not_equal,      ///< Tests inequality.
    greater,        ///< Tests whether the column value is greater.
    less,           ///< Tests whether the column value is less.
    less_equal,     ///< Tests whether the column value is less than or equal to the operand.
    greater_equal,  ///< Tests whether, the column value is greater than or equal to the operand.
};

/**
 * Applies the comparison selected by p_operation.
 *
 * Uses the C++ operator for the supplied types, including its usual arithmtic
 * conversions. Callers must choose types with the comparison semantics they need.
 */
template <
    comparison_operation p_operation,
    class p_left_type,
    class p_right_type>
[[gnu::always_inline]] constexpr bool apply_comparison(
    p_left_type left,
    p_right_type right) noexcept {
    if constexpr (p_operation == comparison_operation::equal) {
        return left == right;
    } else if constexpr (p_operation == comparison_operation::not_equal) {
        return left != right;
    } else if constexpr (p_operation == comparison_operation::greater) {
        return left > right;
    } else if constexpr (p_operation == comparison_operation::less) {
        return left < right;
    } else if constexpr (p_operation == comparison_operation::less_equal) {
        return left <= right;
    } else {
        return left >= right;
    }
}

/**
 * Result sink providing writable Boolean value and null masks.
 *
 * compare_column allocates once  for the batch, then fills both masks through
 * the pointers returned by value_data() and null_data().
 */
template <class p_sink>
concept compare_result_sink = requires(
    p_sink sink,
    std::uint32_t element_count) {
    { sink.allocate(element_count) };
    { sink.value_data() } -> std::same_as<bool*>;
    { sink.null_data() } -> std::same_as<bool*>;
};

/**
 * Compares each non-null column value with the scalar constant.
 *
 * Copies input null flags to the result. Null entries have a false value bit
 * and are skipped by the comparison.
 * @param values Array of element_count column values.
 * @param nulls Matching null flags.
 * @param element_count Batch length.
 * @param constant Scalar comparison operand.
 * @param sink Allocates and receives the result value and null masks.
 */
template <
    comparison_operation p_operation,
    class p_value_type,
    class p_constant_type,
    compare_result_sink p_sink>
void compare_column(
    const p_value_type* values,
    const bool* nulls,
    std::uint32_t element_count,
    p_constant_type constant,
    p_sink& sink) {
    sink.allocate(element_count);
    bool* results = sink.value_data();
    bool* result_nulls = sink.null_data();
    for (std::uint32_t index = 0; index < element_count; ++index) {
        const bool is_null = nulls[index];
        result_nulls[index] = is_null;
        results[index] = is_null
            ? false
            : apply_comparison<p_operation>(values[index], constant);
    }
}

/**
 * @brief Intersects a Boolean mask with another mask in place.
 *
 * @param[in,out] left Left mask and destination.
 * @param[in] right Right mask.
 * @param[in] element_count Number of elements in both masks.
 */
void mask_and(
    bool* left,
    const bool* right,
    std::uint32_t element_count) noexcept;

/**
 * @brief Unions a Boolean mask with another mask in place.
 *
 * @param[in,out] left Left mask and destination.
 * @param[in] right Right mask.
 * @param[in] element_count Number of elements in both masks.
 */
void mask_or(
    bool* left,
    const bool* right,
    std::uint32_t element_count) noexcept;

}  // namespace bioseq::column::vec
