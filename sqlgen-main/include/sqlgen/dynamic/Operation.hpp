#ifndef SQLGEN_DYNAMIC_OPERATION_HPP_
#define SQLGEN_DYNAMIC_OPERATION_HPP_

#include <optional>
#include <rfl.hpp>
#include <string>
#include <vector>

#include "../Ref.hpp"
#include "Column.hpp"
#include "ColumnOrValue.hpp"
#include "Type.hpp"
#include "Value.hpp"

namespace sqlgen::dynamic {

struct Operation {
  struct Abs {
    Ref<Operation> op1;
  };

  struct Aggregation {
    struct Avg {
      Ref<Operation> val;
    };

    struct Count {
      std::optional<Column> val;
      bool distinct = false;
    };

    struct Max {
      Ref<Operation> val;
    };

    struct Min {
      Ref<Operation> val;
    };

    struct Sum {
      Ref<Operation> val;
    };

    using ReflectionType = rfl::TaggedUnion<"what", Avg, Count, Max, Min, Sum>;

    const ReflectionType& reflection() const { return val; }

    ReflectionType val;
  };

  struct Cast {
    Ref<Operation> op1;
    Type target_type;
  };

  struct Ceil {
    Ref<Operation> op1;
  };

  struct Coalesce {
    std::vector<Ref<Operation>> ops;
  };

  struct Concat {
    std::vector<Ref<Operation>> ops;
  };

  struct Cos {
    Ref<Operation> op1;
  };

  struct DatePlusDuration {
    Ref<Operation> date;
    std::vector<Duration> durations;
  };

  struct Day {
    Ref<Operation> op1;
  };

  struct DaysBetween {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Divides {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Exp {
    Ref<Operation> op1;
  };

  struct Floor {
    Ref<Operation> op1;
  };

  struct Hour {
    Ref<Operation> op1;
  };

  struct Length {
    Ref<Operation> op1;
  };

  struct Ln {
    Ref<Operation> op1;
  };

  struct Lower {
    Ref<Operation> op1;
  };

  struct LTrim {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Log2 {
    Ref<Operation> op1;
  };

  struct Minus {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Minute {
    Ref<Operation> op1;
  };

  struct Mod {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Month {
    Ref<Operation> op1;
  };

  struct Multiplies {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Plus {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Replace {
    Ref<Operation> op1;
    Ref<Operation> op2;
    Ref<Operation> op3;
  };

  struct Round {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct RTrim {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Second {
    Ref<Operation> op1;
  };

  struct Sin {
    Ref<Operation> op1;
  };

  struct Sqrt {
    Ref<Operation> op1;
  };

  struct Tan {
    Ref<Operation> op1;
  };

  struct Trim {
    Ref<Operation> op1;
    Ref<Operation> op2;
  };

  struct Unixepoch {
    Ref<Operation> op1;
  };

  struct Upper {
    Ref<Operation> op1;
  };

  struct Weekday {
    Ref<Operation> op1;
  };

  struct Year {
    Ref<Operation> op1;
  };

  using ReflectionType =
      rfl::TaggedUnion<"what", Abs, Aggregation, Cast, Ceil, Column, Coalesce,
                       Concat, Cos, DatePlusDuration, Day, DaysBetween, Divides,
                       Exp, Floor, Hour, Length, Ln, Log2, Lower, LTrim, Month,
                       Minus, Minute, Mod, Multiplies, Plus, Replace, Round,
                       RTrim, Second, Sin, Sqrt, Tan, Trim, Unixepoch, Upper,
                       Value, Weekday, Year>;

  const ReflectionType& reflection() const { return val; }

  ReflectionType val;
};

}  // namespace sqlgen::dynamic

#endif
