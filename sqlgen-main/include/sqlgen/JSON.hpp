#ifndef SQLGEN_JSON_HPP_
#define SQLGEN_JSON_HPP_

#include <rfl/json.hpp>
#include <type_traits>

#include "transpilation/is_nullable.hpp"

namespace sqlgen {

template <class T>
class JSON {
 public:
  using ReflectionType = T;

  JSON() : value_(T()) {}

  JSON(const T& _value) : value_(_value) {}

  JSON(JSON&& _other) noexcept = default;

  JSON(const JSON& _other) = default;

  template <class U>
  JSON(const JSON<U>& _other) : value_(_other.get()) {}

  template <class U>
  JSON(JSON<U>&& _other) : value_(_other.get()) {}

  template <class U>
    requires std::is_convertible_v<U, T>
  JSON(const U& _value) : value_(_value) {}

  template <class U>
    requires std::is_convertible_v<U, T>
  JSON(U&& _value) noexcept : value_(std::forward<U>(_value)) {}

  template <class U>
    requires std::is_convertible_v<U, T>
  JSON(const JSON<U>& _other) : value_(_other.value()) {}

  ~JSON() = default;

  /// Returns the underlying object.
  T& get() { return value_; }

  /// Returns the underlying object.
  const T& get() const { return value_; }

  /// Returns the underlying object.
  T& operator()() { return value_; }

  /// Returns the underlying object.
  const T& operator()() const { return value_; }

  /// Assigns the underlying object.
  auto& operator=(const T& _value) {
    value_ = _value;
    return *this;
  }

  /// Assigns the underlying object.
  template <class U>
    requires std::is_convertible_v<U, T>
  auto& operator=(const U& _value) {
    value_ = _value;
    return *this;
  }

  /// Assigns the underlying object.
  JSON& operator=(const JSON& _other) = default;

  /// Assigns the underlying object.
  JSON& operator=(JSON&& _other) = default;

  /// Assigns the underlying object.
  template <class U>
  auto& operator=(const JSON<U>& _other) {
    value_ = _other.get();
    return *this;
  }

  /// Assigns the underlying object.
  template <class U>
  auto& operator=(JSON<U>&& _other) {
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

 private:
  /// The underlying value.
  T value_;
};

}  // namespace sqlgen

#endif
