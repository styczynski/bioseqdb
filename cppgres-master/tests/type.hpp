#pragma once

#include <cstring>
#include <new>

#include "tests.hpp"

struct my_custom_type {
  std::string s;
  int reserved;
  static cppgres::type composite_type() { return cppgres::named_type("custom_type"); }
};

namespace tests {

add_test(type_name_smoke, [](test_case &) {
  auto ty = cppgres::type_traits<std::string_view>().type_for();
  return _assert(ty.name() == "text") && _assert(ty.name(true) == "pg_catalog.text");
});

add_test(named_type, [](test_case &) {
  bool result = true;
  auto ty1 = cppgres::named_type("text");
  result = result && _assert(ty1.name(true) == "pg_catalog.text");
  auto ty2 = cppgres::named_type("pg_catalog", "text");
  result = result && _assert(ty2.name(true) == "pg_catalog.text");
  return result;
});

add_test(named_type_string_view_bounds, [](test_case &) {
  bool result = true;
  std::string storage = "text_suffix";
  bool exception_raised = false;
  try {
    auto ty = cppgres::named_type(std::string_view(storage.data(), 4));
    result = result && _assert(ty.name(true) == "pg_catalog.text");
  } catch (std::exception &e) {
    exception_raised = true;
  }
  result = result && _assert(!exception_raised);
  return result;
});

add_test(name_long_input_is_terminated, [](test_case &) {
  bool result = true;

  alignas(cppgres::name) std::byte storage[sizeof(cppgres::name)];
  std::memset(storage, 'x', sizeof(storage));
  std::string long_name(NAMEDATALEN * 2, 'a');
  auto *name = new (storage) cppgres::name(long_name.c_str());
  NameData &raw = *name;

  result = result && _assert(NameStr(raw)[NAMEDATALEN - 1] == '\0');

  name->~name();
  return result;
});

postgres_function(custom_type_fun, ([](my_custom_type t) {
                    my_custom_type t1 = t;
                    std::reverse(t1.s.begin(), t1.s.end());
                    return t1;
                  }));

add_test(
    custom_type, ([](test_case &) {
      bool result = true;

      cppgres::spi_executor spi;
      spi.execute("create type custom_type as (s text, i int);");
      spi.execute(cppgres::fmt::format(
          "create function custom_type_fun(custom_type) returns custom_type language c as '{}'",
          get_library_name()));

      {
        // Ensure the type comes returns well
        auto val = spi.query<my_custom_type>("select row('test',1)::custom_type");
        result = result && _assert(val.begin()[0].s == "test");
      }
      {
        // Ensure the type is passed to the executor well
        my_custom_type v{.s = "test"};
        auto val = spi.query<my_custom_type>("select $1", v);
        result = result && _assert(val.begin()[0].s == "test");
      }
      {
        // After processing
        auto val = spi.query<my_custom_type>("select custom_type_fun(row('test',1))");
        result = result && _assert(val.begin()[0].s == "tset");
      }

      {
        auto val = spi.query<std::tuple<my_custom_type, my_custom_type>>(
            "select custom_type_fun(row('test', 1)), custom_type_fun(row('hi', 1))");
        result = result && _assert(std::get<0>(val.begin()[0]).s == "tset");
        result = result && _assert(std::get<1>(val.begin()[0]).s == "ih");
      }
      return result;
    }));

add_test(tuples_are_records, ([](test_case &) {
           bool result = true;

           using t = std::tuple<std::string, std::string>;
           result =
               result && _assert(cppgres::type_traits<t>().is(cppgres::type{.oid = RECORDOID}));

           return result;
         }));

add_test(value_output, ([](test_case &) {
           bool result = true;
           result = result &&
                    _assert(std::string(cppgres::output_function<std::int64_t>()(123)) == "123");
           {
             // Value
             cppgres::spi_executor spi;
             auto results = spi.query<cppgres::value>("select true");
             auto res = results.begin()[0];
             result = result && _assert(std::string(cppgres::output_function(res)(res)) == "t");
           }
           {
             // By type
             cppgres::spi_executor spi;
             auto results = spi.query<cppgres::value>("select 'test'");
             auto f = cppgres::output_function(results.get_tuple_descriptor().get_type(0));
             result = result && _assert(std::string(f(results.begin()[0])) == "test");
           }
           return result;
         }));

} // namespace tests
