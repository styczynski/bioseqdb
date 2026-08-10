/**
 * @file bioseq_caching/core/checked_math.cppm
 * @brief Overflow-safe arithmetic and endian-stable integer primitives.
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

template <class Integer, bool = std::is_enum_v<Integer>>
struct wire_integer_type {
    using type = Integer;
};

template <class Integer>
struct wire_integer_type<Integer, true> {
    using type = std::underlying_type_t<Integer>;
};

template <class Integer>
using wire_integer_type_t = typename wire_integer_type<Integer>::type;

} // namespace detail

using MathError = bioseq::common::CheckedArithmeticError;
using bioseq::common::checked_add;
using bioseq::common::checked_align_up;
using bioseq::common::checked_mul;
using bioseq::common::checked_range;

/** Convert an integer only when its value fits the destination type. */
template <class Destination, class Source>
    requires(std::is_integral_v<Destination> && std::is_integral_v<Source>)
[[nodiscard]] constexpr std::optional<Destination> checked_narrow(
    Source value) noexcept {
    if (!std::in_range<Destination>(value)) {
        return std::nullopt;
    }
    return static_cast<Destination>(value);
}

/** Return the wire-format endianness of the current host. */
[[nodiscard]] constexpr bool host_is_little_endian() noexcept {
    return std::endian::native == std::endian::little;
}

/** Append one fixed-width integer in canonical little-endian order. */
template <class Integer>
    requires(std::is_integral_v<Integer> || std::is_enum_v<Integer>)
void append_little_endian(std::vector<std::byte>& destination, Integer value) {
    using Raw = detail::wire_integer_type_t<Integer>;
    using Unsigned = std::make_unsigned_t<Raw>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        destination.push_back(static_cast<std::byte>(
            (bits >> (index * 8u)) & static_cast<Unsigned>(0xffu)));
    }
}

/** Decode one fixed-width little-endian integer and advance the cursor. */
template <class Integer>
    requires(std::is_integral_v<Integer> || std::is_enum_v<Integer>)
[[nodiscard]] std::expected<Integer, MathError> read_little_endian(
    std::span<const std::byte> source,
    std::size_t& cursor) noexcept {
    using Raw = detail::wire_integer_type_t<Integer>;
    using Unsigned = std::make_unsigned_t<Raw>;
    if (!checked_range(
            static_cast<std::uint64_t>(cursor),
            static_cast<std::uint64_t>(sizeof(Unsigned)),
            static_cast<std::uint64_t>(source.size()))) {
        return std::unexpected(MathError::Truncated);
    }
    Unsigned bits = 0;
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        bits |= static_cast<Unsigned>(std::to_integer<unsigned int>(
                    source[cursor + index]))
            << (index * 8u);
    }
    cursor += sizeof(Unsigned);
    if constexpr (std::is_enum_v<Integer>) {
        return static_cast<Integer>(static_cast<Raw>(bits));
    } else {
        return static_cast<Integer>(bits);
    }
}

/** Return a checked subspan without pointer arithmetic before validation. */
[[nodiscard]] inline std::expected<std::span<const std::byte>, MathError>
checked_subspan(
    std::span<const std::byte> source,
    std::uint64_t offset,
    std::uint64_t length) noexcept {
    if (!checked_range(offset, length, source.size())) {
        return std::unexpected(MathError::OutOfBounds);
    }
    return source.subspan(
        static_cast<std::size_t>(offset),
        static_cast<std::size_t>(length));
}

} // namespace bioseq::cache
