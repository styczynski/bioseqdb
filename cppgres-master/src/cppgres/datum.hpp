/**
 * \file
 */
#pragma once

#include "utils/utils.hpp"

#include "imports.h"
#include "memory.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace cppgres {

struct oid {
  oid() : oid_(InvalidOid) {}
  oid(::Oid oid) : oid_(oid) {}
  oid(oid &oid) : oid_(oid.oid_) {}
  oid(const oid &oid) : oid_(oid.oid_) {}

  bool operator==(const oid &rhs) const { return oid_ == rhs.oid_; }
  bool operator!=(const oid &rhs) const { return !(rhs == *this); }

  bool operator==(const ::Oid &rhs) const { return oid_ == rhs; }
  bool operator!=(const ::Oid &rhs) const { return oid_ != rhs; }

  operator ::Oid() const { return oid_; }
  operator ::Oid &() { return oid_; }

  bool is_valid() const { return oid_ != InvalidOid; }

private:
  ::Oid oid_;
};
static_assert(sizeof(::Oid) == sizeof(oid));

struct datum {
  template <typename T, typename> friend struct datum_conversion;

  operator const ::Datum &() const { return _datum; }

  operator Pointer() const { return reinterpret_cast<Pointer>(_datum); }

  datum() : _datum(0) {}
  explicit datum(::Datum datum) : _datum(datum) {}

  bool operator==(const datum &other) const { return _datum == other._datum; }

private:
  ::Datum _datum;
  friend struct nullable_datum;
};

class null_datum_exception : public std::exception {
  const char *what() const noexcept override { return "passed datum is null"; }
};

struct nullable_datum {

  bool is_null() const noexcept { return _ndatum.isnull; }

  operator struct datum &() {
    if (_ndatum.isnull) {
      throw null_datum_exception();
    }
    return _datum;
  }

  operator const ::Datum &() {
    if (_ndatum.isnull) {
      throw null_datum_exception();
    }
    return _datum.operator const ::Datum &();
  }

  operator const struct datum &() const {
    if (_ndatum.isnull) {
      throw null_datum_exception();
    }
    return _datum;
  }

  explicit nullable_datum(::NullableDatum d) : _ndatum(d) {}

  explicit nullable_datum() : _ndatum({.isnull = true}) {}

  explicit nullable_datum(::Datum d) : _ndatum({.value = d, .isnull = false}) {}
  explicit nullable_datum(datum d) : _ndatum({.value = d._datum, .isnull = false}) {}

  bool operator==(const nullable_datum &other) const {
    if (is_null()) {
      return other.is_null();
    }
    if (other.is_null()) {
      return is_null();
    }
    return _datum.operator==(other._datum);
  }

private:
  union {
    ::NullableDatum _ndatum;
    datum _datum;
  };
};

/**
 * @brief A trait to convert from and into a @ref cppgres::datum
 *
 * @tparam T C++ type to convert into and from
 */
template <typename T, typename = void> struct datum_conversion {

  /**
   * @brief Convert from a nullable datum
   *
   * Gets an optional @ref cppgres::memory_context when available to be able to determine the source
   * of the (pointer) datum.
   */
  static T from_nullable_datum(const nullable_datum &d, const oid oid,
                               std::optional<memory_context> context = std::nullopt) = delete;

  /**
   * @brief Convert from a datum
   *
   * Gets an optional @ref cppgres::memory_context when available to be able to determine the source
   * of the (pointer) datum.
   */
  static T from_datum(const datum &, const oid,
                      std::optional<memory_context> context = std::nullopt) = delete;
  /**
   * @brief Convert datum into a type
   *
   * Unlike @ref from_datum, gets no memory context.
   */
  static datum into_datum(const T &d) = delete;

  /**
   * @brief Convert into a nullable datum
   */
  static nullable_datum into_nullable_datum(const T &d) = delete;
};

template <typename T, typename R = T> struct default_datum_conversion {
  /**
   * @brief Convert from a nullable datum
   *
   * Gets an optional @ref cppgres::memory_context when available to be able to determine the source
   * of the (pointer) datum.
   */
  static R from_nullable_datum(const nullable_datum &d, const oid oid,
                               std::optional<memory_context> context = std::nullopt) {
    if (d.is_null()) {
      throw std::runtime_error(cppgres::fmt::format("datum is null and can't be coerced into {}",
                                                    utils::type_name<T>()));
    }
    return datum_conversion<T>::from_datum(d, oid, context);
  }

  /**
   * @brief Convert into a nullable datum
   */
  static nullable_datum into_nullable_datum(const T &d) {
    return nullable_datum(datum_conversion<T>::into_datum(d));
  }
};

template <typename T>
concept convertible_into_datum = requires(T t) {
  { datum_conversion<T, void>::into_datum(std::declval<T>()) } -> std::same_as<datum>;
};

template <typename T>
concept convertible_from_datum = requires(datum d, oid oid, std::optional<memory_context> context) {
  { datum_conversion<T, void>::from_datum(d, oid, context) } -> std::same_as<T>;
};

template <typename T> struct unsupported_type {};

template <typename T>
requires convertible_from_datum<std::remove_cv_t<T>>
T from_nullable_datum(const nullable_datum &d, const oid oid,
                      std::optional<memory_context> context = std::nullopt) {
  return datum_conversion<std::remove_cv_t<T>>::from_nullable_datum(d, oid, context);
}

template <typename T> nullable_datum into_nullable_datum(const std::optional<T> &v) {
  if (v.has_value()) {
    return nullable_datum(datum_conversion<T>::into_datum(v.value()));
  } else {
    return nullable_datum();
  }
}

template <typename T> nullable_datum into_nullable_datum(const T &v) {
  if constexpr (std::same_as<nullable_datum, T>) {
    return v;
  }
  return datum_conversion<T>::into_nullable_datum(v);
}

template <typename T>
concept convertible_from_nullable_datum = requires {
  {
    cppgres::from_nullable_datum<T>(std::declval<nullable_datum>(), std::declval<oid>(),
                                    std::declval<std::optional<memory_context>>())
  } -> std::same_as<T>;
};

template <typename T>
concept convertible_into_nullable_datum = requires {
  { cppgres::into_nullable_datum(std::declval<T>()) } -> std::same_as<nullable_datum>;
};

template <typename T> struct all_from_nullable_datum {
private:
  static constexpr std::size_t N = utils::tuple_size_v<T>;

  template <std::size_t... I> static constexpr bool impl(std::index_sequence<I...>) {
    return (
        ... &&
        convertible_from_nullable_datum<utils::remove_optional_t<utils::tuple_element_t<I, T>>>);
  }

public:
  static constexpr bool value = impl(std::make_index_sequence<N>{});
};

} // namespace cppgres
