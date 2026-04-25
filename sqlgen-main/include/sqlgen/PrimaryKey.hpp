#ifndef SQLGEN_PRIMARY_KEY_HPP_
#define SQLGEN_PRIMARY_KEY_HPP_

#include <type_traits>

#include "transpilation/is_nullable.hpp"

namespace sqlgen {

inline constexpr bool auto_incr = true;

template <class T, bool _auto_incr = false>
struct PrimaryKey {
  using ReflectionType = T;
  static constexpr bool auto_incr = _auto_incr;

  static_assert(
      !transpilation::is_nullable_v<T>,
      "A primary key cannot be nullable. Please use a non-nullable type.");
  static_assert(!_auto_incr || std::is_integral_v<T>,
                "The type of an auto-incrementing primary key must be "
                "integral. Please use an integral type or remove auto_incr.");

  PrimaryKey() : value_(T()) {}

  PrimaryKey(const T& _value) : value_(_value) {}

  PrimaryKey(PrimaryKey&& _other) noexcept = default;

  PrimaryKey(const PrimaryKey& _other) = default;

  template <class U, bool _other_auto_incr>
  PrimaryKey(const PrimaryKey<U, _other_auto_incr>& _other)
      : value_(_other.get()) {}

  template <class U, bool _other_auto_incr>
  PrimaryKey(PrimaryKey<U, _other_auto_incr>&& _other) : value_(_other.get()) {}

  template <class U>
    requires std::is_convertible_v<U, ReflectionType>
  PrimaryKey(const U& _value) : value_(_value) {}

  template <class U>
    requires std::is_convertible_v<U, ReflectionType>
  PrimaryKey(U&& _value) noexcept : value_(std::forward<U>(_value)) {}

  template <class U>
    requires std::is_convertible_v<U, ReflectionType>
  PrimaryKey(const PrimaryKey<U>& _other) : value_(_other.value()) {}

  ~PrimaryKey() = default;

  /// Returns the underlying object.
  ReflectionType& get() { return value_; }

  /// Returns the underlying object.
  const ReflectionType& get() const { return value_; }

  /// Returns the underlying object.
  ReflectionType& operator()() { return value_; }

  /// Returns the underlying object.
  const ReflectionType& operator()() const { return value_; }

  /// Assigns the underlying object.
  auto& operator=(const ReflectionType& _value) {
    value_ = _value;
    return *this;
  }

  /// Assigns the underlying object.
  template <class U>
    requires std::is_convertible_v<U, ReflectionType>
  auto& operator=(const U& _value) {
    value_ = _value;
    return *this;
  }

  /// Assigns the underlying object.
  PrimaryKey& operator=(const PrimaryKey& _other) = default;

  /// Assigns the underlying object.
  PrimaryKey& operator=(PrimaryKey&& _other) = default;

  /// Assigns the underlying object.
  template <class U, bool _other_auto_incr>
  auto& operator=(const PrimaryKey<U, _other_auto_incr>& _other) {
    value_ = _other.get();
    return *this;
  }

  /// Assigns the underlying object.
  template <class U, bool _other_auto_incr>
  auto& operator=(PrimaryKey<U, _other_auto_incr>&& _other) {
    value_ = std::move(_other.value_);
    return *this;
  }

  /// Necessary for the automated transpilation to work.
  const T& reflection() const { return value_; }

  /// Assigns the underlying object.
  void set(const T& _value) { value_ = _value; }

  /// Returns the underlying object.
  T& value() { return value_; }

  /// Returns the underlying object.
  const T& value() const { return value_; }

  /// The underlying value.
  T value_;
};

}  // namespace sqlgen

#endif
