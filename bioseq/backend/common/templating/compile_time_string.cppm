/**
 * @file compile_time_string.cppm
 * @brief Structural compile-time strings for non-type template parameters.
 *
 * `compile_time_string` owns a fixed-size, null-terminated character buffer.
 * Unlike `std::string_view`, it is a structural type and can therefore be used
 * directly as a non-type template argument. Its literal constructor remains
 * `constexpr` because implicit conversion into a template argument is not
 * necessarily an immediate-function context.
 */
module;

#include <algorithm>
#include <cstddef>
#include <string_view>

export module bioseq.common.templating.compile_time_string;

export namespace bioseq::common::templating {

/**
 * @brief Fixed-size structural string suitable for a non-type template argument.
 * @tparam p_extent Character-buffer extent, including the terminating null byte.
 *
 * The public buffer keeps the type structural while preserving aggregate-style
 * value semantics. Instances created from string literals always include the
 * literal's terminating null byte.
 */
template <std::size_t p_extent>
struct compile_time_string {
    /// Characters including the terminating null byte.
    char data[p_extent] {};

    /// Constructs an empty value-initialized buffer.
    constexpr compile_time_string() = default;

    /**
     * @brief Copies a character array into structural storage.
     * @param string Source array, normally a string literal.
     */
    constexpr compile_time_string(const char (&string)[p_extent]) {
        std::copy_n(string, p_extent, data);
    }

    /// Returns the logical character count, excluding the terminator.
    [[nodiscard]] consteval std::size_t size() const noexcept {
        return p_extent - 1u;
    }

    /// Returns the complete buffer extent, including the terminator.
    [[nodiscard]] consteval std::size_t buffer_size() const noexcept {
        return p_extent;
    }

    /// Returns the null-terminated character buffer.
    [[nodiscard]] consteval const char* c_str() const noexcept {
        return data;
    }

    /// Returns a view excluding the terminating null byte.
    [[nodiscard]] consteval std::string_view view() const noexcept {
        return std::string_view{data, p_extent - 1u};
    }

    /**
     * @brief Returns the character at an unchecked buffer index.
     * @param index Index into the complete buffer, including its terminator.
     */
    [[nodiscard]] consteval char operator[](std::size_t index) const noexcept {
        return data[index];
    }

    /**
     * @brief Concatenates two structural strings.
     * @tparam p_other_extent Right-hand buffer extent.
     * @param other Right-hand structural string.
     * @return A structural string containing both logical strings and one terminator.
     */
    template <std::size_t p_other_extent>
    [[nodiscard]] consteval compile_time_string<p_extent + p_other_extent - 1u>
    operator+(const compile_time_string<p_other_extent>& other) const noexcept
    {
        compile_time_string<p_extent + p_other_extent - 1u> result{};
        std::copy_n(data, p_extent - 1u, result.data);
        std::copy_n(other.data, p_other_extent, result.data + p_extent - 1u);
        return result;
    }

    /**
     * @brief Compares with a runtime null-terminated string.
     * @param runtime_string Runtime string, or null.
     * @return `true` only when both strings have identical contents and length.
     */
    [[nodiscard]] constexpr bool equals_c_string(
        const char* runtime_string) const noexcept
    {
        if (runtime_string == nullptr) return false;
        for (std::size_t index = 0; index < p_extent - 1u; ++index) {
            if (runtime_string[index] != data[index]) return false;
            if (runtime_string[index] == '\0') return false;
        }
        return runtime_string[p_extent - 1u] == '\0';
    }
};

/// Deduces structural-string extent from a character array.
template <std::size_t p_extent>
compile_time_string(const char (&)[p_extent]) -> compile_time_string<p_extent>;

/**
 * @brief Compares two structural strings.
 * @return `true` when their extents and complete buffers are identical.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval bool operator==(
    const compile_time_string<p_left_extent>& left,
    const compile_time_string<p_right_extent>& right) noexcept
{
    if constexpr (p_left_extent != p_right_extent) {
        return false;
    } else {
        return std::equal(left.data, left.data + p_left_extent, right.data);
    }
}

/**
 * @brief Compares a structural string with a character array.
 * @return `true` when both strings have identical contents and extent.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval bool operator==(
    const compile_time_string<p_left_extent>& left,
    const char (&right)[p_right_extent]) noexcept
{
    return left == compile_time_string<p_right_extent>(right);
}

/**
 * @brief Compares a character array with a structural string.
 * @return `true` when both strings have identical contents and extent.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval bool operator==(
    const char (&left)[p_left_extent],
    const compile_time_string<p_right_extent>& right) noexcept
{
    return compile_time_string<p_left_extent>(left) == right;
}

/**
 * @brief Concatenates a structural string and a character array.
 * @return A structural string containing both operands and one terminator.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval auto operator+(
    compile_time_string<p_left_extent> left,
    const char (&right)[p_right_extent])
{
    return left + compile_time_string<p_right_extent>(right);
}

/**
 * @brief Concatenates a character array and a structural string.
 * @return A structural string containing both operands and one terminator.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval auto operator+(
    const char (&left)[p_left_extent],
    compile_time_string<p_right_extent> right)
{
    return compile_time_string<p_left_extent>(left) + right;
}

} // namespace bioseq::common::templating
