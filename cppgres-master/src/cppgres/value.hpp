#pragma once

#include "datum.hpp"
#include "type.hpp"

namespace cppgres {

struct value {

  value(nullable_datum &&datum, type &&type) : datum_(datum), type_(type) {}

  const type &get_type() const { return type_; }

  const nullable_datum &get_nullable_datum() const { return datum_; };

private:
  nullable_datum datum_;
  type type_;
};

template <> struct datum_conversion<value> {

  static value from_nullable_datum(const nullable_datum &d, oid oid,
                                   std::optional<memory_context> = std::nullopt) {
    return {nullable_datum(d), type{.oid = oid}};
  }

  static value from_datum(const datum &d, oid oid, std::optional<memory_context>) {
    return {nullable_datum(d), type{.oid = oid}};
  }

  static datum into_datum(const value &t) { return t.get_nullable_datum(); }

  static nullable_datum into_nullable_datum(const value &t) { return t.get_nullable_datum(); }
};

template <> struct type_traits<value> {
  type_traits() : value_(std::nullopt) {}
  type_traits(value &value) : value_(std::optional(std::ref(value))) {}
  bool is(const type &t) { return !value_.has_value() || (*value_).get().get_type() == t; }
  constexpr type type_for() {
    if (value_.has_value()) {
      return (*value_).get().get_type();
    }
    return {UNKNOWNOID};
  }

private:
  std::optional<std::reference_wrapper<value>> value_;
};

} // namespace cppgres
