#pragma once

#include "tests.hpp"

namespace tests {

add_test(syscache, ([](test_case &) {
  bool result = true;

  cppgres::spi_executor spi;

  spi.execute("create function test_function() returns int language sql as $$select 0$$");

  auto func_oid = spi.query<cppgres::oid>("select 'test_function'::regproc::oid");
  cppgres::syscache<Form_pg_proc, cppgres::oid> cache(func_oid.begin()[0]);

  result = result && _assert(NameStr((*cache).proname) == std::string_view("test_function"));

  return result;
}));

add_test(syscache_get_attribute, ([](test_case &) {
  bool result = true;

  cppgres::spi_executor spi;

  spi.execute("create function test_function() returns int language sql as $$select 1$$");

  auto func_oid = spi.query<cppgres::oid>("select 'test_function'::regproc::oid");
  cppgres::syscache<Form_pg_proc, cppgres::oid> cache(func_oid.begin()[0]);

  result = result && _assert(cache.get_attribute<std::string_view>(Anum_pg_proc_prosrc) == "select 1");

  return result;
}));
} // namespace tests
