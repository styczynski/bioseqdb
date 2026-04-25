#ifndef SQLGEN_VARCHAR_HPP_
#define SQLGEN_VARCHAR_HPP_

#include <stdexcept>
#include <string>

#include "Result.hpp"

namespace sqlgen {

template <size_t _size>
class Varchar {
 public:
  using ReflectionType = std::string;

  static constexpr size_t size_ = _size;

  Varchar() : value_("") {}

  Varchar(const std::string& _value) : value_(check_size(_value)) {}

  Varchar(const char* _value) : value_(check_size(_value)) {}

  Varchar(Varchar<_size>&& _other) noexcept = default;

  Varchar(const Varchar<_size>& _other) = default;

  template <size_t _other_size>
  Varchar(const Varchar<_other_size>& _other)
      : value_(check_size(_other.get())) {}

  template <size_t _other_size>
  Varchar(Varchar<_other_size>&& _other) : value_(check_size(_other.get())) {}

  ~Varchar() = default;

  static Result<Varchar<size_>> make(const std::string& _value) noexcept {
    try {
      return Varchar<size_>(_value);
    } catch (std::exception& e) {
      return error(e.what());
    }
  }

  /// Returns the underlying object.
  const ReflectionType& get() const { return value_; }

  /// Returns the underlying object.
  const ReflectionType& operator()() const { return value_; }

  /// Assigns the underlying object.
  auto& operator=(const char* _value) {
    value_ = check_size(_value);
    return *this;
  }

  /// Assigns the underlying object.
  auto& operator=(const std::string& _value) {
    value_ = check_size(_value);
    return *this;
  }

  /// Assigns the underlying object.
  Varchar<_size>& operator=(const Varchar<_size>& _other) = default;

  /// Assigns the underlying object.
  Varchar<_size>& operator=(Varchar<_size>&& _other) = default;

  /// Assigns the underlying object.
  template <size_t _other_size>
  auto& operator=(const Varchar<_other_size>& _other) {
    value_ = check_size(_other.get());
    return *this;
  }

  /// Necessary for the automated transpilation to work.
  const std::string& reflection() const { return value_; }

  static constexpr size_t size() { return size_; }

  /// Returns the underlying object.
  const std::string& value() const { return value_; }

 private:
  static std::string check_size(const std::string& _str) {
    if (_str.size() > size_) {
      throw std::runtime_error(
          "String '" + _str + "' too long: " + std::to_string(_str.size()) +
          " exceeds the maximum length of " + std::to_string(size_) + ".");
    }
    return _str;
  }

 private:
  /// The underlying value.
  std::string value_;
};

}  // namespace sqlgen

#endif
