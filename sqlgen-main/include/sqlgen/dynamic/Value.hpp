#ifndef SQLGEN_DYNAMIC_VALUE_HPP_
#define SQLGEN_DYNAMIC_VALUE_HPP_

#include <rfl.hpp>
#include <string>

#include "TimeUnit.hpp"

namespace sqlgen::dynamic {

struct Duration {
  TimeUnit unit;
  int64_t val;
};

struct Boolean {
  bool val;
};

struct Float {
  double val;
};

struct Integer {
  int64_t val;
};

struct Null {};

struct String {
  std::string val;
};

struct Timestamp {
  int64_t seconds_since_unix;
};

struct Value {
  using ReflectionType = rfl::TaggedUnion<"type", Duration, Boolean, Float,
                                          Integer, Null, String, Timestamp>;
  const auto& reflection() const { return val; }
  ReflectionType val;
};

}  // namespace sqlgen::dynamic

#endif
