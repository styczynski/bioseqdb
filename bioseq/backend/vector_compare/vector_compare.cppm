/**
 * @file vector_compare.cppm
 * @brief Type-generic comparison and Boolean-mask operations for column vectors.
 *
 * The comparison kernel relies on the language's usual arithmetic conversions
 * so mixed-width integer comparisons retain PostgreSQL-compatible semantics.
 * Mask composition is implemented in the target-specific implementation unit.
 */
module;

#include <concepts>
#include <cstdint>

export module bioseq.column.vector_compare;

export namespace bioseq::column::vec {

/** Comparison operation applied to a column value and a scalar operand. */
enum class comparison_operation : std::uint8_t {
    equal,
    not_equal,
    greater,
    less,
    less_equal,
    greater_equal,
};

/**
 * Apply a comparison after the usual arithmetic conversions.
 *
 * @tparam p_operation Comparison selected at compile time.
 * @tparam p_left_type Column element type.
 * @tparam p_right_type Scalar operand type.
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
 * Writable result sink accepted by the comparison kernel.
 *
 * Plain pointers intentionally cross the module boundary: allocation is
 * performed once per batch and the kernel writes both value and null masks.
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
 * Compare each non-null column value with a scalar operand.
 *
 * @tparam p_operation Comparison selected at compile time.
 * @tparam p_value_type Column element type.
 * @tparam p_constant_type Scalar operand type.
 * @tparam p_sink Writable mask sink.
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

/** Intersect a Boolean mask with another mask in place. */
void mask_and(
    bool* left,
    const bool* right,
    std::uint32_t element_count) noexcept;

/** Union a Boolean mask with another mask in place. */
void mask_or(
    bool* left,
    const bool* right,
    std::uint32_t element_count) noexcept;

}  // namespace bioseq::column::vec
