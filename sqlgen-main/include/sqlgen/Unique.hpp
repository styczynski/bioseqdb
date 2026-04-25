#ifndef SQLGEN_UNIQUE_HPP_
#define SQLGEN_UNIQUE_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "Literal.hpp"
#include "transpilation/Col.hpp"
#include "transpilation/all_columns_exist.hpp"
#include "transpilation/is_primary_key.hpp"
#include "transpilation/remove_reflection_t.hpp"
#include "transpilation/underlying_t.hpp"

namespace sqlgen {

template <class T>
struct Unique {
  using ReflectionType = T;

  Unique() : value_(T()) {}

  Unique(const T& _value) : value_(_value) {}

  Unique(Unique&& _other) noexcept = default;

  Unique(const Unique& _other) = default;

  template <class U>
    requires std::is_convertible_v<U, ReflectionType>
  Unique(const U& _value) : value_(_value) {}

  template <class U>
    requires std::is_convertible_v<U, ReflectionType>
  Unique(U&& _value) noexcept : value_(std::forward<U>(_value)) {}

  ~Unique() = default;

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
  Unique& operator=(const Unique& _other) = default;

  /// Assigns the underlying object.
  Unique& operator=(Unique&& _other) = default;

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
