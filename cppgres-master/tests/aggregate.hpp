#pragma once

#include <cstring>

#include "tests.hpp"

namespace tests {

struct aggregate_test {
  int64_t x;
  aggregate_test() : x(0) {}
  aggregate_test(int64_t x_) : x(x_) {}

  void update(int64_t v) { x += v; }
  void update(int64_t v, int64_t v1) { x += v + v1; }

  int64_t finalize() const { return x; }

  // Serialization

  cppgres::bytea serialize() const {
    std::array<std::byte, 8> bytes;
    std::memcpy(bytes.data(), &x, sizeof(x));
    return {bytes, cppgres::memory_context()};
  };
  aggregate_test(cppgres::bytea &a) {
    std::memcpy(&x, a.operator cppgres::byte_array().data(), sizeof(x));
  }

  // Combination
  aggregate_test(aggregate_test &self, aggregate_test &other) : x(self.x + other.x) {
  }
};

declare_aggregate(aggregate, aggregate_test, int64_t);
declare_aggregate(aggregate2, aggregate_test, int64_t, int64_t);

struct aggregate_destructor_test {
  static inline int destructions = 0;
  int64_t x = 0;

  void update(int64_t v) { x += v; }
  int64_t finalize() const { return x; }

  ~aggregate_destructor_test() { destructions++; }
};

declare_aggregate(aggregate_destructor, aggregate_destructor_test, int64_t);

struct aggregate_convertible_test {
  int64_t x;
  aggregate_convertible_test() : x(0) {}
  aggregate_convertible_test(int64_t x_) : x(x_) {}
  void update(int64_t v) { x += v; }
  aggregate_convertible_test(aggregate_convertible_test &self,
                             aggregate_convertible_test &other)
      : x(self.x + other.x) {}
};

add_test(aggregate_simple, [](test_case &) {
  bool result = true;
  cppgres::spi_executor spi;
  spi.execute(cppgres::fmt::format("create or replace function aggregate_sfunc(internal, int8) "
                                   "returns internal language c as '{}'",
                                   get_library_name()));
  spi.execute(cppgres::fmt::format(
      "create or replace function aggregate_ffunc(internal) returns int8 language c as '{}'",
      get_library_name()));
  spi.execute(
      "create aggregate agg (int8) (sfunc = aggregate_sfunc, finalfunc = aggregate_ffunc, stype "
      "= internal)");
  auto res = spi.query<int64_t>("select agg(v) from (values (1), (2), (3)) as t(v)");
  result = result && _assert(res.begin()[0] == 6);
  return result;
});

add_test(aggregate_destructor, [](test_case &) {
  bool result = true;
  cppgres::spi_executor spi;
  spi.execute(
      cppgres::fmt::format("create or replace function aggregate_destructor_sfunc(internal, int8) "
                           "returns internal language c as '{}'",
                           get_library_name()));
  spi.execute(cppgres::fmt::format(
      "create or replace function aggregate_destructor_ffunc(internal) returns int8 language c "
      "as '{}'",
      get_library_name()));
  spi.execute("create aggregate agg_destructor (int8) (sfunc = aggregate_destructor_sfunc, "
              "finalfunc = aggregate_destructor_ffunc, stype = internal)");

  auto before = aggregate_destructor_test::destructions;
  auto res = spi.query<int64_t>("select agg_destructor(v) from (values (1), (2), (3)) as t(v)");
  result = result && _assert(res.begin()[0] == 6);
  result = result && _assert(aggregate_destructor_test::destructions > before);
  return result;
});

add_test(aggregate_simple_empty, [](test_case &) {
  bool result = true;
  cppgres::spi_executor spi;
  spi.execute(cppgres::fmt::format("create or replace function aggregate_sfunc(internal, int8) "
                                   "returns internal language c as '{}'",
                                   get_library_name()));
  spi.execute(cppgres::fmt::format(
      "create or replace function aggregate_ffunc(internal) returns int8 language c as '{}'",
      get_library_name()));
  spi.execute(
      "create aggregate agg (int8) (sfunc = aggregate_sfunc, finalfunc = aggregate_ffunc, stype "
      "= internal)");
  auto res = spi.query<int64_t>("select agg(v) from (select 1 limit 0) as t(v)");
  result = result && _assert(res.begin()[0] == 0);
  return result;
});

add_test(aggregate_simple_2arg, [](test_case &) {
  bool result = true;
  cppgres::spi_executor spi;
  spi.execute(
      cppgres::fmt::format("create or replace function aggregate2_sfunc(internal, int8, int8) "
                           "returns internal language c as '{}'",
                           get_library_name()));
  spi.execute(cppgres::fmt::format(
      "create or replace function aggregate2_ffunc(internal) returns int8 language c as '{}'",
      get_library_name()));
  spi.execute("create aggregate agg (int8, int8) (sfunc = aggregate2_sfunc, finalfunc = "
              "aggregate2_ffunc, stype "
              "= internal)");
  auto res = spi.query<int64_t>("select agg(v,v) from (values (1), (2), (3)) as t(v)");
  result = result && _assert(res.begin()[0] == 12);
  return result;
});

add_test(aggregate_simple_serial, [](test_case &) {
  bool result = true;
  cppgres::spi_executor spi;
  spi.execute(cppgres::fmt::format("create or replace function aggregate_sfunc(internal, int8) "
                                   "returns internal language c as '{}'",
                                   get_library_name()));
  spi.execute(cppgres::fmt::format(
      "create or replace function aggregate_ffunc(internal) returns int8 language c as '{}'",
      get_library_name()));
  spi.execute(cppgres::fmt::format(
      "create or replace function aggregate_serial(internal) returns bytea language c as '{}'",
      get_library_name()));
  spi.execute(cppgres::fmt::format("create or replace function aggregate_deserial(bytea, internal) "
                                   "returns internal language c as '{}'",
                                   get_library_name()));
  spi.execute(
      cppgres::fmt::format("create or replace function aggregate_combine(internal, internal) "
                           "returns internal language c as '{}'",
                           get_library_name()));
  spi.execute(
      "create aggregate agg (int8) (sfunc = aggregate_sfunc, finalfunc = aggregate_ffunc, stype "
      "= internal, serialfunc = aggregate_serial, deserialfunc = aggregate_deserial, parallel = "
      "safe, combinefunc = aggregate_combine)");

  spi.execute("set max_parallel_workers_per_gather = 4");
  spi.execute("set parallel_setup_cost = 1");
  spi.execute("set parallel_tuple_cost = 0.01");

  spi.execute("create table values as select v from generate_series(1, 1000000) v");

  {
    auto res0 = spi.query<std::string>("explain select agg(v) from values");

    std::ostringstream oss;
    for (auto s : res0) {
      oss << s << "\n";
    }
    auto plan = oss.str();

    result = result && _assert(plan.find("Partial Aggregate") != std::string::npos);
  }

  auto res = spi.query<int64_t>("select agg(v) from values");
  result = result && _assert(res.begin()[0] == 500000500000);
  return result;
});

add_test(aggregate_convertible, [](test_case &) {
  bool result = true;
  cppgres::spi_executor spi;
  spi.execute(
      cppgres::fmt::format("create or replace function aggregate_convertible_sfunc(int8, int8) "
                           "returns int8 language c as '{}'",
                           get_library_name()));
  spi.execute("create aggregate agg (int8) (sfunc = aggregate_convertible_sfunc, stype = int8)");
  auto res = spi.query<int64_t>("select agg(v) from (values (1), (2), (3)) as t(v)");
  result = result && _assert(res.begin()[0] == 6);
  return result;
});

add_test(aggregate_convertible_combine, [](test_case &) {
  bool result = true;
  cppgres::spi_executor spi;
  spi.execute(
      cppgres::fmt::format("create or replace function aggregate_convertible_sfunc(int8, int8) "
                           "returns int8 language c as '{}'",
                           get_library_name()));
  spi.execute(
      cppgres::fmt::format("create or replace function aggregate_convertible_combine(int8, int8) "
                           "returns int8 language c as '{}'",
                           get_library_name()));
  spi.execute("create aggregate agg (int8) (sfunc = aggregate_convertible_sfunc, "
              "stype = int8, combinefunc = aggregate_convertible_combine, parallel = safe)");

  spi.execute("set max_parallel_workers_per_gather = 4");
  spi.execute("set min_parallel_table_scan_size = 0");
  spi.execute("set parallel_setup_cost = 0");
  spi.execute("set parallel_tuple_cost = 0");
  spi.execute("create table aggregate_convertible_combine_values as "
              "select v::int8 from generate_series(1, 1000000) v");
  spi.execute("analyze aggregate_convertible_combine_values");

  {
    auto res0 = spi.query<std::string>(
        "explain select agg(v) from aggregate_convertible_combine_values");

    std::ostringstream oss;
    for (auto s : res0) {
      oss << s << "\n";
    }
    auto plan = oss.str();

    result = result && _assert(plan.find("Partial Aggregate") != std::string::npos);
  }

  auto res = spi.query<int64_t>("select agg(v) from aggregate_convertible_combine_values");
  result = result && _assert(res.begin()[0] == 500000500000);
  return result;
});

}; // namespace tests

namespace cppgres {
template <> struct datum_conversion<tests::aggregate_convertible_test> {
  static tests::aggregate_convertible_test
  from_nullable_datum(const nullable_datum &d, const oid oid,
                      std::optional<memory_context> context = std::nullopt) {
    if (d.is_null()) {
      return {};
    } else {
      return {datum_conversion<int64_t>::from_datum(d, oid, context)};
    }
  }

  static tests::aggregate_convertible_test
  from_datum(const datum &d, const oid oid, std::optional<memory_context> context = std::nullopt) {
    return {datum_conversion<int64_t>::from_datum(d, oid, context)};
  }
  static datum into_datum(const tests::aggregate_convertible_test &d) {
    return datum_conversion<int64_t>::into_datum(d.x);
  }
};
} // namespace cppgres

declare_aggregate(aggregate_convertible, tests::aggregate_convertible_test, int64_t);
