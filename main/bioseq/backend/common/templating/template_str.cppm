/**
 * @file template_str.cppm
 * @brief Compile-time fixed-length character buffer usable as a
 *        non-type template parameter (NTTP).
 *
 * Single source of truth for "string at the type level" across the
 * project. `std::string_view` is not structural and therefore not
 * usable as an NTTP. `CompTimeStr<N>` is standard-layout, trivially
 * copyable, and structural per [temp.param]/8 — so it can appear in
 * `template <CompTimeStr Name>` parameter slots.
 *
 * The ctor is constexpr (NOT consteval): the implicit conversion of a
 * string literal to `CompTimeStr<N>` for an NTTP slot is not always an
 * immediate-function context.
 *
 * Equality is a single free function over (N1, N2) — C++20's
 * rewritten-candidate rule synthesises the symmetric and
 * member-vs-literal overload sets from it.
 */
module;

#include <algorithm>
#include <cstddef>
#include <string_view>

export module bioseq_common.templating.template_str;

export namespace bioseq::common::templating {

template <std::size_t N>
struct CompTimeStr {
    char data[N] {};

    constexpr CompTimeStr() = default;

    constexpr CompTimeStr(const char (&str)[N]) {
        std::copy_n(str, N, data);
    }

    [[nodiscard]] consteval std::size_t size() const noexcept { return N - 1u; }
    [[nodiscard]] consteval std::size_t buffer_size() const noexcept { return N; }
    [[nodiscard]] consteval const char* c_str() const noexcept { return data; }
    [[nodiscard]] consteval std::string_view view() const noexcept {
        return std::string_view{data, N - 1u};
    }

    [[nodiscard]] consteval char operator[](std::size_t i) const noexcept {
        return data[i];
    }

    template <std::size_t N2>
    [[nodiscard]] consteval CompTimeStr<N + N2 - 1u> operator+(
        const CompTimeStr<N2>& other) const noexcept
    {
        CompTimeStr<N + N2 - 1u> out{};
        std::copy_n(data, N - 1u, out.data);
        std::copy_n(other.data, N2, out.data + N - 1u);
        return out;
    }

    // Runtime equality against a NUL-terminated cstring. O(N); the
    // compiler can fold to memcmp for small fixed N. constexpr so it
    // can be evaluated in static_assert contexts as well.
    [[nodiscard]] constexpr bool equals_cstr(const char* runtime_str) const noexcept {
        if (runtime_str == nullptr) return false;
        for (std::size_t i = 0; i < N - 1u; ++i) {
            if (runtime_str[i] != data[i]) return false;
            if (runtime_str[i] == '\0')    return false;  // runtime shorter
        }
        return runtime_str[N - 1u] == '\0';               // same length
    }
};

template <std::size_t N>
CompTimeStr(const char (&)[N]) -> CompTimeStr<N>;

template <std::size_t N1, std::size_t N2>
[[nodiscard]] consteval bool operator==(const CompTimeStr<N1>& a,
                                         const CompTimeStr<N2>& b) noexcept
{
    if constexpr (N1 != N2) {
        return false;
    } else {
        return std::equal(a.data, a.data + N1, b.data);
    }
}

// Mixed-form overloads — template-argument deduction cannot synthesise
// the `const char[N] -> CompTimeStr<N>` implicit conversion required to
// match the homogeneous template above. Provide explicit overloads so
// `CompTimeStr{"foo"} == "foo"` (and the symmetric form) work as
// constant expressions.
template <std::size_t N1, std::size_t N2>
[[nodiscard]] consteval bool operator==(const CompTimeStr<N1>& a,
                                         const char (&b)[N2]) noexcept
{
    return a == CompTimeStr<N2>(b);
}

template <std::size_t N1, std::size_t N2>
[[nodiscard]] consteval bool operator==(const char (&a)[N1],
                                         const CompTimeStr<N2>& b) noexcept
{
    return CompTimeStr<N1>(a) == b;
}

template <std::size_t N1, std::size_t N2>
[[nodiscard]] consteval auto operator+(CompTimeStr<N1> fs, const char (&str)[N2]) {
    return fs + CompTimeStr<N2>(str);
}

template <std::size_t N1, std::size_t N2>
[[nodiscard]] consteval auto operator+(const char (&str)[N2], CompTimeStr<N1> fs) {
    return CompTimeStr<N2>(str) + fs;
}

static_assert(CompTimeStr{"dna4"}.size() == 4u);
static_assert(CompTimeStr{"dna4"} == CompTimeStr{"dna4"});
static_assert(CompTimeStr{"dna4"} == "dna4");
static_assert("dna4" == CompTimeStr{"dna4"});
static_assert(!(CompTimeStr{"dna4"} == CompTimeStr{"dna5"}));
static_assert(!(CompTimeStr{"dna4"} == CompTimeStr{"dna45"}));
static_assert(CompTimeStr{"foo"} + CompTimeStr{"bar"} == CompTimeStr{"foobar"});
static_assert(CompTimeStr{"foo"}.equals_cstr("foo"));
static_assert(!CompTimeStr{"foo"}.equals_cstr("fooo"));
static_assert(!CompTimeStr{"foo"}.equals_cstr("fo"));
static_assert(!CompTimeStr{"foo"}.equals_cstr(""));

} // namespace
