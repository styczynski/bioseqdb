/**
 * @file compile_time_string.cppm
 * @brief Fixed-size strings that can be passed as template arguments.
 *
 * The character buffer is part of the value, so templates can use the string
 * itself as an argument. The literal constructor is `constexpr` so implicit
 * conversions into template arguments also work outside immediate functions.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

export module bioseq.common.templating.compile_time_string;

export namespace bioseq::common::templating {

/**
 * @brief A string stored in a fixed-size character array.
 * @tparam p_extent Buffer size, including the terminating null byte.
 *
 * The public array allows this type to be used as a template argument.
 * Constructing from a string literal copies its terminating null byte too.
 */
template <std::size_t p_extent>
struct compile_time_string {
    /// Characters including the terminating null byte.
    std::array<char, p_extent> data{};

    /** @brief Creates a buffer filled with null bytes. */
    constexpr compile_time_string() = default;

    /**
     * @brief Copies all characters from a fixed-size array.
     * @param[in] string Source array, normally a string literal.
     *
     * For a string literal, the copied characters include its terminating null byte.
     */
    constexpr compile_time_string(const char (&string)[p_extent]) {
        std::copy_n(string, p_extent, data.begin());
    }

    /**
     * @brief Returns the character count without the terminating null byte.
     */
    [[nodiscard]] consteval std::size_t size() const noexcept {
        return p_extent - 1u;
    }

    /**
     * @brief Returns the buffer size, including the terminating null byte.
     */
    [[nodiscard]] consteval std::size_t buffer_size() const noexcept {
        return p_extent;
    }

    /**
     * @brief Returns a pointer to the character buffer.
     * @return Pointer valid while this string exists.
     */
    [[nodiscard]] consteval const char* c_str() const noexcept {
        return data.data();
    }

    /**
     * @brief Returns a view without the terminating null byte.
     * @return View valid while this string exists.
     */
    [[nodiscard]] consteval std::string_view view() const noexcept {
        return std::string_view{data.data(), p_extent - 1u};
    }

    /**
     * @brief Returns the character at an unchecked buffer index.
     * @param[in] index Index into the complete buffer, including its terminator.
     * @return Character at `index`.
     * @pre `index < buffer_size()`.
     */
    [[nodiscard]] consteval char operator[](std::size_t index) const noexcept {
        return data[index];
    }

    /**
     * @brief Joins two strings, keeping one terminating null byte.
     * @tparam p_other_extent Size of the other buffer, including its terminator.
     * @param[in] other String to append.
     */
    template <std::size_t p_other_extent>
    [[nodiscard]] consteval compile_time_string<p_extent + p_other_extent - 1u>
    operator+(const compile_time_string<p_other_extent>& other) const noexcept
    {
        compile_time_string<p_extent + p_other_extent - 1u> result{};
        std::copy_n(data.begin(), p_extent - 1u, result.data.begin());
        std::copy_n(
            other.data.begin(),
            p_other_extent,
            result.data.begin() + static_cast<std::ptrdiff_t>(p_extent - 1u));
        return result;
    }

    /**
     * @brief Checks whether a C string has the same contents and length.
     * @param runtime_string String to compare, or null.
     * @return False for null or an earlier terminator.
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

/**
 * @brief Infers the buffer size from the array, including its terminator.
 */
template <std::size_t p_extent>
compile_time_string(const char (&)[p_extent]) -> compile_time_string<p_extent>;

/**
 * @brief Compares buffer sizes and all characters, including the terminator.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval bool operator==(
    const compile_time_string<p_left_extent>& left,
    const compile_time_string<p_right_extent>& right) noexcept
{
    if constexpr (p_left_extent != p_right_extent) {
        return false;
    } else {
        return left.data == right.data;
    }
}

/**
 * @brief Compares a string with a character array, including the terminator.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval bool operator==(
    const compile_time_string<p_left_extent>& left,
    const char (&right)[p_right_extent]) noexcept
{
    return left == compile_time_string<p_right_extent>(right);
}

/**
 * @brief Compares a character array with a string, including the terminator.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval bool operator==(
    const char (&left)[p_left_extent],
    const compile_time_string<p_right_extent>& right) noexcept
{
    return compile_time_string<p_left_extent>(left) == right;
}

/**
 * @brief Appends a character array, keeping one terminating null byte.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval auto operator+(
    compile_time_string<p_left_extent> left,
    const char (&right)[p_right_extent])
{
    return left + compile_time_string<p_right_extent>(right);
}

/**
 * @brief Appends a string to a character array, keeping one terminator.
 */
template <std::size_t p_left_extent, std::size_t p_right_extent>
[[nodiscard]] consteval auto operator+(
    const char (&left)[p_left_extent],
    compile_time_string<p_right_extent> right)
{
    return compile_time_string<p_left_extent>(left) + right;
}

} // namespace bioseq::common::templating
