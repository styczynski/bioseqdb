#ifndef SQLGEN_DYNAMIC_CONDITION_HPP_
#define SQLGEN_DYNAMIC_CONDITION_HPP_

#include <rfl.hpp>

#include "../Ref.hpp"
#include "ColumnOrValue.hpp"
#include "Operation.hpp"

namespace sqlgen::dynamic {

struct Condition {
  struct And {
    Ref<Condition> cond1;
    Ref<Condition> cond2;
  };

  struct BooleanColumnOrValue {
    ColumnOrValue col_or_val;
  };

  struct Equal {
    Operation op1;
    Operation op2;
  };

  struct GreaterEqual {
    Operation op1;
    Operation op2;
  };

  struct GreaterThan {
    Operation op1;
    Operation op2;
  };

  struct In {
    Operation op;
    std::vector<dynamic::Value> patterns;
  };

  struct IsNotNull {
    Operation op;
  };

  struct IsNull {
    Operation op;
  };

  struct LesserEqual {
    Operation op1;
    Operation op2;
  };

  struct LesserThan {
    Operation op1;
    Operation op2;
  };

  struct Like {
    Operation op;
    dynamic::Value pattern;
  };

  struct Not {
    Ref<Condition> cond;
  };

  struct NotEqual {
    Operation op1;
    Operation op2;
  };

  struct NotLike {
    Operation op;
    dynamic::Value pattern;
  };

  struct NotIn {
    Operation op;
    std::vector<dynamic::Value> patterns;
  };

  struct Or {
    Ref<Condition> cond1;
    Ref<Condition> cond2;
  };

  using ReflectionType =
      rfl::TaggedUnion<"what", And, BooleanColumnOrValue, Equal, GreaterEqual,
                       GreaterThan, In, IsNull, IsNotNull, LesserEqual,
                       LesserThan, Like, Not, NotEqual, NotIn, NotLike, Or>;

  const ReflectionType& reflection() const { return val; }

  ReflectionType val;
};

}  // namespace sqlgen::dynamic

#endif
