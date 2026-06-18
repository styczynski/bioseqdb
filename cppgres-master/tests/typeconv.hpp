#pragma once

#include "tests.hpp"

namespace tests {

add_test(from_float, [](test_case &) {
  bool result = true;
  float val = 131073.0;
  Datum d = Float4GetDatum(val);
  result = result && _assert(cppgres::into_nullable_datum(val) == d);
  return result;
});

add_test(to_float, [](test_case &) {
  bool result = true;
  float val = 131073.0;
  Datum d = Float4GetDatum(val);
  result = result && _assert(cppgres::from_nullable_datum<float>(cppgres::nullable_datum(d), FLOAT4OID) == val);
  return result;
});

add_test(from_double, [](test_case &) {
  bool result = true;
  double val = 8796093022209.0;
  Datum d = Float8GetDatum(val);
  result = result && _assert(cppgres::into_nullable_datum(val) == d);
  return result;
});

add_test(to_double, [](test_case &) {
  bool result = true;
  double val = 8796093022209.0;
  Datum d = Float8GetDatum(val);
  result = result &&
           _assert(cppgres::from_nullable_datum<double>(cppgres::nullable_datum(d), FLOAT8OID) == val);

  return result;
});


} // namespace tests
