#ifndef SQLGEN_FOREIGN_KEY_HPP_
#define SQLGEN_FOREIGN_KEY_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "Literal.hpp"
#include "transpilation/Col.hpp"
#include "transpilation/all_columns_exist.hpp"
#include "transpilation/is_primary_key.hpp"
#include "transpilation/remove_reflection_t.hpp"
#include "transpilation/underlying_t.hpp"

namespace sqlgen {

template <class T, class _ForeignTableType,
          rfl::internal::StringLiteral _col_name>
struct ForeignKey {
  using ReflectionType = T;
  using ForeignTableType = _ForeignTableType;
  using ColumnType = Literal<_col_name>;

  static_assert(transpilation::column_exists_v<
                    _ForeignTableType,
                    typename rfl::named_tuple_t<_ForeignTableType>::Names,
                    transpilation::Col<_col_name>>,
                "The column referenced to in ForeignKey<...> does not exist on "
                "the referenced table.");
  static_assert(
      std::is_same_v<
          std::remove_cvref_t<transpilation::remove_reflection_t<T>>,
          std::remove_cvref_t<
              transpilation::remove_reflection_t<transpilation::underlying_t<
                  ForeignTableType, transpilation::Col<_col_name>>>>>,
      "The type of the column and the type of the referenced column must be "
      "the same.");
  static_assert(
      transpilation::is_primary_key_v<
          std::remove_cvref_t<rfl::field_type_t<_col_name, ForeignTableType>>>,
      "The referenced column must be a primary key.");

  ForeignKey() : value_(T()) {}

  ForeignKey(const T& _value) : value_(_value) {}

  ForeignKey(ForeignKey&& _other) noexcept = default;

  ForeignKey(const ForeignKey& _other) = default;

  template <class U>
    requires std::is_convertible_v<U, ReflectionType>
  ForeignKey(const U& _value) : value_(_value) {}

  template <class U>
    requires std::is_convertible_v<U, ReflectionType>
  ForeignKey(U&& _value) noexcept : value_(std::forward<U>(_value)) {}

  ~ForeignKey() = default;

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
  ForeignKey& operator=(const ForeignKey& _other) = default;

  /// Assigns the underlying object.
  ForeignKey& operator=(ForeignKey&& _other) = default;

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
