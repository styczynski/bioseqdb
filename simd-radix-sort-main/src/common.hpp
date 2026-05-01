#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <typeinfo>

namespace simd_sort {

template <typename>
inline constexpr bool always_false_v = false;

using SortIndex = ssize_t;

template <typename T>
inline constexpr const char *type_name = typeid(T).name();
template <>
inline constexpr const char *type_name<bool> = "bool";
template <>
inline constexpr const char *type_name<uint8_t> = "uint8";
template <>
inline constexpr const char *type_name<uint16_t> = "uint16";
template <>
inline constexpr const char *type_name<uint32_t> = "uint32";
template <>
inline constexpr const char *type_name<uint64_t> = "uint64";
template <>
inline constexpr const char *type_name<int8_t> = "int8";
template <>
inline constexpr const char *type_name<int16_t> = "int16";
template <>
inline constexpr const char *type_name<int32_t> = "int32";
template <>
inline constexpr const char *type_name<int64_t> = "int64";
template <>
inline constexpr const char *type_name<float> = "float";
template <>
inline constexpr const char *type_name<double> = "double";

template <std::size_t Bytes>
struct LargeUInt {
  static_assert(Bytes > 8, "Bytes must be larger than 8");
  static_assert((Bytes & (Bytes - 1)) == 0, "Bytes must be a power of 2");
  uint64_t data[Bytes / 8];

  LargeUInt() = default;
  LargeUInt(const LargeUInt &) = default;
  LargeUInt &operator=(const LargeUInt &) = default;

  LargeUInt(const uint64_t x) {
    data[0] = x;
    for (std::size_t i = 1; i < Bytes / 8; i++) {
      data[i] = 0;
    }
  }
};

namespace internal {

template <std::size_t Bytes>
struct UInt;

template <>
struct UInt<1> {
  using type = uint8_t;
};

template <>
struct UInt<2> {
  using type = uint16_t;
};

template <>
struct UInt<4> {
  using type = uint32_t;
};

template <>
struct UInt<8> {
  using type = uint64_t;
};

template <std::size_t Bytes>
struct UInt {
  using type = LargeUInt<Bytes>;
};

}  // namespace internal

template <std::size_t Bytes>
using UInt = typename internal::UInt<Bytes>::type;

template <std::size_t Bytes>
UInt<Bytes> setBit(const std::size_t n) {
  if constexpr (Bytes <= 8) {
    return UInt<Bytes>(1ULL << n);
  } else {
    UInt<Bytes> x;
    x.data[n / 64] = 1ULL << (n % 64);
    return x;
  }
}

template <std::size_t X>
inline constexpr bool is_power_of_two = X > 0 && (X & (X - 1)) == 0;

}  // namespace simd_sort
