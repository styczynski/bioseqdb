/**
 * @file bioseq_caching/core/checked_math.cppm
 * @brief Checked arithmetic and little-endian integer encoding for the cache.
 */
module;

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.cache.core:checked_math;

export import bioseq.common.checked_arithmetic;

export namespace bioseq::cache {

namespace detail {

template <class p_integer_type, bool p_is_enum = std::is_enum_v<p_integer_type>>
struct wire_integer_type {
    using type = p_integer_type;
};

template <class p_integer_type> struct wire_integer_type<p_integer_type, true> {
    using type = std::underlying_type_t<p_integer_type>;
};

template <class p_integer_type>
using wire_integer_type_t = typename wire_integer_type<p_integer_type>::type;

} // namespace detail

using math_error = bioseq::common::checked_arithmetic_error;
using bioseq::common::checked_add;
using bioseq::common::checked_align_up;
using bioseq::common::checked_mul;
using bioseq::common::checked_range;

/** Convert an integer only when its value fits the destination type. */
template <class p_destination_type, class p_source_type>
    requires(std::is_integral_v<p_destination_type> && std::is_integral_v<p_source_type>)
[[nodiscard]] constexpr std::optional<p_destination_type>
checked_narrow(p_source_type value) noexcept {
    if (!std::in_range<p_destination_type>(value)) {
        return std::nullopt;
    }
    return static_cast<p_destination_type>(value);
}

/** Reports whether this host stores integers in little-endian order. */
[[nodiscard]] constexpr bool host_is_little_endian() noexcept {
    return std::endian::native == std::endian::little;
}

/** Appends an integer or enum valu in little-endian byte order. */
template <class p_integer_type>
    requires(std::is_integral_v<p_integer_type> || std::is_enum_v<p_integer_type>)
void append_little_endian(std::vector<std::byte>& destination, p_integer_type value) {
    using raw_type = detail::wire_integer_type_t<p_integer_type>;
    using unsigned_type = std::make_unsigned_t<raw_type>;
    const unsigned_type bits = static_cast<unsigned_type>(value);
    for (std::size_t index = 0; index < sizeof(unsigned_type); ++index) {
        destination.push_back(
            static_cast<std::byte>((bits >> (index * 8u)) & static_cast<unsigned_type>(0xffu)));
    }
}

/** Decode one fixed-width little-endian integer and advance the cursor. */
template <class p_integer_type>
    requires(std::is_integral_v<p_integer_type> || std::is_enum_v<p_integer_type>)
[[nodiscard]] std::expected<p_integer_type, math_error>
read_little_endian(std::span<const std::byte> source, std::size_t& cursor) noexcept {
    using raw_type = detail::wire_integer_type_t<p_integer_type>;
    using unsigned_type = std::make_unsigned_t<raw_type>;
    if (!checked_range(static_cast<std::uint64_t>(cursor),
                       static_cast<std::uint64_t>(sizeof(unsigned_type)),
                       static_cast<std::uint64_t>(source.size()))) {
        return std::unexpected(math_error::truncated);
    }
    unsigned_type bits = 0;
    for (std::size_t index = 0; index < sizeof(unsigned_type); ++index) {
        bits |= static_cast<unsigned_type>(std::to_integer<unsigned int>(source[cursor + index]))
                << (index * 8u);
    }
    cursor += sizeof(unsigned_type);
    if constexpr (std::is_enum_v<p_integer_type>) {
        return static_cast<p_integer_type>(static_cast<raw_type>(bits));
    } else {
        return static_cast<p_integer_type>(bits);
    }
}

/** Returns a byte range after checking that it fits inside the source. */
[[nodiscard]] inline std::expected<std::span<const std::byte>, math_error> checked_subspan(
    std::span<const std::byte> source, std::uint64_t offset, std::uint64_t length) noexcept {
    if (!checked_range(offset, length, source.size())) {
        return std::unexpected(math_error::out_of_bounds);
    }
    return source.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
}

} // namespace bioseq::cache
