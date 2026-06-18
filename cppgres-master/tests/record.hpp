#pragma once

#include <optional>
#include <ranges>
#include <vector>

#include "tests.hpp"

namespace tests {

postgres_function(record_fun, ([](cppgres::record rec) {
                    return std::views::iota(0, rec.attributes()) |
                           std::views::transform([rec = std::move(rec)](auto n) {
                             return std::tuple<std::string, std::string>(
                                 rec.attribute_name(n), rec.attribute_type(n).name());
                           });
                  }));

add_test(record_fun, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute("create type person as (name text, position text)");
           spi.execute(
               cppgres::fmt::format("create function record_fun(record) returns table(name text, "
                                    "type text) language 'c' as '{}'",
                                   get_library_name()));

           struct tab {
             std::string_view name;
             std::string_view type;
           };

           {
             auto res = spi.query<tab>("select * from record_fun(row(1,'test',3.4))");
             auto it = res.begin();
             result = result && _assert(it[0].type == "integer");
             result = result && _assert(it[0].name == "f1");
             result = result && _assert(it[1].type == "unknown");
             result = result && _assert(it[1].name == "f2");
             result = result && _assert(it[2].type == "numeric");
             result = result && _assert(it[2].name == "f3");
           }

           return result;
         }));

add_test(record_test, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute("create type person as (name text, position text)");

           {
             auto res = spi.query<cppgres::record>("select row('Joe', 'Manager')::person");
             auto it = res.begin();
             result = result && _assert(it[0].attribute_name(0) == "name");
             result = result && _assert(it[0].attribute_name(1) == "position");
             result = result && _assert(cppgres::from_nullable_datum<std::string_view>(
                                            it[0].get_attribute(0), TEXTOID) == "Joe");
             result =
                 result && _assert(cppgres::from_nullable_datum<std::string_view>(
                                            it[0]["name"], TEXTOID) == "Joe");
             result = result && _assert(cppgres::from_nullable_datum<std::string_view>(
                                            it[0][0], TEXTOID) == "Joe");
             result = result && _assert(cppgres::from_nullable_datum<std::string_view>(
                                            it[0].get_attribute(1), TEXTOID) == "Manager");
             result = result && _assert(cppgres::from_nullable_datum<std::string_view>(
                                            it[0]["position"], TEXTOID) == "Manager");
             result = result && _assert(cppgres::from_nullable_datum<std::string_view>(
                                            it[0][1], TEXTOID) == "Manager");
           }

           return result;
         }));

postgres_function(record_defining_fun, ([]() {
                    cppgres::tuple_descriptor td(3);
                    td.set_type(0, cppgres::type{.oid = TEXTOID});
                    td.set_type(1, cppgres::type{.oid = INT8OID});
                    td.set_type(2, cppgres::type{.oid = BOOLOID});

                    cppgres::record rec(td, std::string("hello"), 1, false);
                    return std::array<cppgres::record, 1>{rec};
                  }));

add_test(record_iterator_constructor_nulls, ([](test_case &) {
           bool result = true;
           cppgres::tuple_descriptor td(4);
           td.set_type(0, cppgres::type{.oid = INT8OID});
           td.set_type(1, cppgres::type{.oid = INT8OID});
           td.set_type(2, cppgres::type{.oid = INT8OID});
           td.set_type(3, cppgres::type{.oid = INT8OID});

           std::vector<std::optional<std::int64_t>> values = {1, std::nullopt, 3, std::nullopt};
           cppgres::record rec(td, values.begin(), values.end());

           result = result && _assert(cppgres::from_nullable_datum<std::int64_t>(
                                          rec.get_attribute(0), INT8OID) == 1);
           result = result && _assert(rec.get_attribute(1).is_null());
           result = result && _assert(cppgres::from_nullable_datum<std::int64_t>(
                                          rec.get_attribute(2), INT8OID) == 3);
           result = result && _assert(rec.get_attribute(3).is_null());
           return result;
         }));

add_test(record_defining, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute(cppgres::fmt::format(
               "create function record_defining_fun() returns setof record language 'c' as '{}'",
               get_library_name()));

           {
             struct res {
               std::string s;
               std::int64_t i;
               bool b;
             };
             auto results =
                 spi.query<res>("select * from record_defining_fun() as (a text, b int8, c bool)");

             auto it = results.begin();
             result = result && _assert(it[0].s == "hello");
             result = result && _assert(it[0].i == 1);
             result = result && _assert(it[0].b == false);
           }

           {
             struct res {
               std::string s;
               bool b;
             };
             bool exception_raised = false;
             cppgres::internal_subtransaction xact(false);

             try {
               spi.query<res>("select * from record_defining_fun() as (a text,  c bool)");
             } catch (...) {
               exception_raised = true;
             }

             result = result && _assert(exception_raised);
           }

           {
             struct res {
               std::string s, s1;
               bool b;
             };
             bool exception_raised = false;
             cppgres::internal_subtransaction xact(false);

             try {
               spi.query<res>("select * from record_defining_fun() as (a text, b text, c bool)");
             } catch (...) {
               exception_raised = true;
             }

             result = result && _assert(exception_raised);
           }

           return result;
         }));

} // namespace tests
