#pragma once

#include "tests.hpp"

extern "C" {
#include <executor/spi_priv.h>
}

struct spi_counted_int {
  int32_t value;
  static inline int conversions = 0;
};

struct spi_counted_arg {
  int32_t value;
  static inline int conversions = 0;
};

namespace cppgres {
template <> struct datum_conversion<spi_counted_int> : default_datum_conversion<spi_counted_int> {
  static spi_counted_int from_datum(const datum &d, oid oid,
                                    std::optional<memory_context> context) {
    spi_counted_int::conversions++;
    return {from_nullable_datum<int32_t>(nullable_datum(d), oid, context)};
  }
};

template <> struct datum_conversion<spi_counted_arg> : default_datum_conversion<spi_counted_arg> {
  static datum into_datum(const spi_counted_arg &arg) {
    spi_counted_arg::conversions++;
    return datum_conversion<int32_t>::into_datum(arg.value);
  }
};

template <> struct type_traits<spi_counted_int> {
  bool is(const type &t) { return type_traits<int32_t>().is(t); }
  type type_for() { return type_traits<int32_t>().type_for(); }
};

template <> struct type_traits<spi_counted_arg> {
  type_traits() {}
  type_traits(const spi_counted_arg &) {}
  bool is(const type &t) { return type_traits<int32_t>().is(t); }
  type type_for() { return type_traits<int32_t>().type_for(); }
};
} // namespace cppgres

namespace tests {

add_test(spi, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.query<std::tuple<std::optional<int64_t>>>(
               "select $1 + i from generate_series(1,100) i", static_cast<int64_t>(1LL));

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(std::get<0>(re) == i + 1);
           }
           result = result && _assert(std::get<0>(res.begin()[0]) == 2);
           return result;
         }));

add_test(spi_multiparam, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.query<std::tuple<std::optional<int64_t>>>(
               "select $1 + $2 + i from generate_series(1,100) i", static_cast<int64_t>(1LL),
               static_cast<int64_t>(1LL));

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(std::get<0>(re) == i + 2);
           }
           return result;
         }));

add_test(spi_single, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.query<int64_t>("select $1 + i from generate_series(1,100) i",
                                         static_cast<int64_t>(1LL));

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(re == i + 1);
           }
           result = result && _assert(res.begin()[0] == 2);
           return result;
         }));

add_test(spi_result_iterator_sparse_cache, ([](test_case &) {
           bool result = true;
           spi_counted_int::conversions = 0;

           cppgres::spi_executor spi;
           auto res = spi.query<spi_counted_int>("select i::int4 from generate_series(1,3) i");
           auto it = res.begin();

           result = result && _assert(it[2].value == 3);
           result = result && _assert(spi_counted_int::conversions == 1);
           result = result && _assert(it[0].value == 1);
           result = result && _assert(spi_counted_int::conversions == 2);
           result = result && _assert(it[2].value == 3);
           result = result && _assert(spi_counted_int::conversions == 2);

           return result;
         }));

add_test(spi_result_iterator_random_access_operations, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.query<int64_t>("select i from generate_series(1,3) i");
           auto it = res.begin();

           auto old = it++;
           result = result && _assert(*old == 1);
           result = result && _assert(*it == 2);
           auto prev = it--;
           result = result && _assert(*prev == 2);
           result = result && _assert(*it == 1);
           it += 2;
           result = result && _assert(*it == 3);
           --it;
           result = result && _assert(*it == 2);
           it -= 1;
           result = result && _assert(*it == 1);

           return result;
         }));

add_test(spi_args_convert_once, ([](test_case &) {
           bool result = true;
           spi_counted_arg::conversions = 0;

           cppgres::spi_executor spi;
           auto res = spi.query<int32_t>("select $1::int4", spi_counted_arg{42});

           result = result && _assert(res.begin()[0] == 42);
           result = result && _assert(spi_counted_arg::conversions == 1);

           return result;
         }));

add_test(spi_empty, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;

           {
             auto res = spi.query<std::tuple<>>("select from generate_series(1,100) i",
                                                static_cast<int64_t>(1LL));
             result = result && _assert(res.count() == 100);
           }

           {
             struct res_t {};
             auto res = spi.query<res_t>("select from generate_series(1,100) i",
                                         static_cast<int64_t>(1LL));
             result = result && _assert(res.count() == 100);
           }

           {
             auto res = spi.query<std::vector<cppgres::value>>(
                 "select from generate_series(1,100) i", static_cast<int64_t>(1LL));
             result = result && _assert(res.count() == 100);
             result = result && _assert(res.begin()[0].size() == 0);
           }

           return result;
         }));

add_test(spi_plural_tuple, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.query<std::tuple<int64_t, int64_t>>(
               "select $1 + i, i from generate_series(1,100) i", static_cast<int64_t>(1LL));

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(std::get<0>(re) == i + 1);
             result = result && _assert(std::get<1>(re) == i);
           }
           return result;
         }));

add_test(spi_vector, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.query<std::vector<int64_t>>(
               "select $1 + i, i from generate_series(1,100) i", static_cast<int64_t>(1LL));

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(re[0] == i + 1);
             result = result && _assert(re[1] == i);
           }
           return result;
         }));

add_test(spi_vector_mismatch, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;

           bool exception_raised = false;

           try {
             spi.query<std::vector<int64_t>>("select $1 + i, 'test' from generate_series(1,100) i",
                                             static_cast<int64_t>(1LL));
           } catch (std::invalid_argument) {
             exception_raised = true;
           }

           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_pfr, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           struct my_struct {
             int64_t val;
           };
           auto res = spi.query<my_struct>("select $1 + i from generate_series(1,100) i",
                                           static_cast<int64_t>(1LL));

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(re.val == i + 1);
           }
           return result;
         }));

add_test(spi_non_opt, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.query<std::tuple<int64_t>>("select $1 + i from generate_series(1,100) i",
                                                     static_cast<int64_t>(1LL));

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(std::get<0>(re) == i + 1);
           }
           result = result && _assert(std::get<0>(res.begin()[0]) == 2);

           bool exception_raised = false;
           try {
             spi.query<std::tuple<int64_t>>("select null from generate_series(1,100) i",
                                            static_cast<int64_t>(1LL));
           } catch (std::exception &e) {
             exception_raised = true;
           }
           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_argless, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.query<std::tuple<std::optional<int64_t>>>(
               "select i from generate_series(1,100) i");

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(std::get<0>(re) == i);
           }
           result = result && _assert(std::get<0>(res.begin()[0]) == 1);

           auto plan = spi.plan("select i from generate_series(1,100) i");
           auto res1 = spi.query<std::tuple<std::optional<int64_t>>>(plan);
           result = result && _assert(std::get<0>(res1.begin()[1]) == 2);

           return result;
         }));

add_test(spi_type_mismatch, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;

           bool exception_raised = false;
           try {
             spi.query<std::tuple<std::optional<bool>>>(
                 "select $1 + i from generate_series(1,100) i", static_cast<int64_t>(1LL));
           } catch (std::invalid_argument &e) {
             exception_raised = true;
           }

           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_plan, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto plan = spi.plan<int64_t>("select $1 + i from generate_series(1,100) i");
           auto res =
               spi.query<std::tuple<std::optional<int64_t>>>(plan, static_cast<int64_t>(1LL));

           int i = 0;
           for (auto &re : res) {
             i++;
             result = result && _assert(std::get<0>(re) == i + 1);
           }
           result = result && _assert(std::get<0>(res.begin()[0]) == 2);
           return result;
         }));

add_test(spi_plan_mismatch, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto plan = spi.plan<int64_t>("select $1 + i from generate_series(1,100) i");

           bool exception_raised = false;
           try {
             spi.query<std::tuple<std::optional<bool>>>(plan, static_cast<int64_t>(1LL));
           } catch (std::invalid_argument &e) {
             exception_raised = true;
           }

           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_plan_gone, ([](test_case &) {
           bool result = true;

           auto plan = ({
             cppgres::spi_executor spi;
             spi.plan<int64_t>("select $1 + i from generate_series(1,100) i");
           });

           cppgres::spi_executor spi;

           bool exception_raised = false;
           try {
             spi.query<std::tuple<std::optional<int64_t>>>(plan, static_cast<int64_t>(1LL));
           } catch (cppgres::pointer_gone_exception &e) {
             exception_raised = true;
           }

           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_keep_plan, ([](test_case &) {
           bool result = true;

           ::SPIPlanPtr ptr = nullptr;
           auto context = ({
             auto plan = ({
               cppgres::spi_executor spi;
               auto plan = spi.plan<int64_t>("select $1 + i from generate_series(1,100) i");
               plan.keep();
               std::move(plan);
             });

             ptr = plan;

             cppgres::spi_executor spi;
             auto res =
                 spi.query<std::tuple<std::optional<int64_t>>>(plan, static_cast<int64_t>(1LL));

             int i = 0;
             for (auto &re : res) {
               i++;
               result = result && _assert(std::get<0>(re) == i + 1);
             }
             result = result && _assert(std::get<0>(res.begin()[0]) == 2);
             auto p = static_cast<_SPI_plan *>(ptr);
             cppgres::tracking_memory_context(cppgres::memory_context(p->plancxt));
           });

           // Now the plan should be gone – destroyed and inaccessible
           context.resets();
           result = result && _assert(context.resets() > 0);

           return result;
         }));

add_test(spi_interleave, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;

           // Nesting: query by string
           {
             cppgres::spi_executor spi1;

             bool exception_raised = false;
             try {
               spi.query<std::tuple<std::optional<int64_t>>>(
                   "select $1 + i from generate_series(1,100) i", static_cast<int64_t>(1LL));
             } catch (std::runtime_error &e) {
               exception_raised = true;
             }

             result = result && _assert(exception_raised);
           }

           // fine here
           spi.query<std::tuple<std::optional<int64_t>>>(
               "select $1 + i from generate_series(1,100) i", static_cast<int64_t>(1LL));

           // Nesting: plan
           {
             cppgres::spi_executor spi1;

             bool exception_raised = false;
             try {
               auto plan = spi.plan<int64_t>("select $1 + i from generate_series(1,100) i");
             } catch (std::runtime_error &e) {
               exception_raised = true;
             }

             result = result && _assert(exception_raised);
           }

           // fine here
           auto plan = spi.plan<int64_t>("select $1 + i from generate_series(1,100) i");

           // Nesting: query by plan
           {
             cppgres::spi_executor spi1;

             bool exception_raised = false;
             try {
               spi.query<std::tuple<std::optional<int64_t>>>(plan, static_cast<int64_t>(1LL));
             } catch (std::runtime_error &e) {
               exception_raised = true;
             }

             result = result && _assert(exception_raised);
           }

           // fine here
           spi.query<std::tuple<std::optional<int64_t>>>(plan, static_cast<int64_t>(1LL));

           return result;
         }));

add_test(spi_execute, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto res = spi.execute("create table spi_execute_test (v int)");
           result = result && _assert(res == 0);

           auto res1 = spi.execute(
               "insert into spi_execute_test (v) select $1 from generate_series(1,100)", 1);
           result = result && _assert(res1 == 100);

           return result;
         }));

add_test(spi_ptr_gone, ([](test_case &) {
           bool result = true;
           cppgres::text res = []() {
             cppgres::spi_executor spi;
             return spi.query<cppgres::text>("select 'hello'").begin()[0];
           }();
           bool exception_raised = false;
           try {
             res.operator std::string_view();
           } catch (cppgres::pointer_gone_exception &e) {
             exception_raised = true;
           }
           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_value_type, ([](test_case &) {
           bool result = true;

           {
             cppgres::spi_executor spi;
             auto res = spi.query<cppgres::value>("select $1 + i from generate_series(1,100) i",
                                                  static_cast<int64_t>(1LL));

             int i = 0;
             for (auto &re : res) {
               i++;
               result = result && _assert(re.get_type() == cppgres::type{.oid = INT8OID});

               result = result && _assert(cppgres::from_nullable_datum<int64_t>(
                                              re.get_nullable_datum(), re.get_type().oid) == i + 1);
             }
           }

           {
             cppgres::spi_executor spi;
             auto res = spi.query<std::tuple<cppgres::value>>(
                 "select $1 + i from generate_series(1,100) i", static_cast<int64_t>(1LL));

             int i = 0;
             for (auto &re : res) {
               i++;
               result =
                   result && _assert(std::get<0>(re).get_type() == cppgres::type{.oid = INT8OID});

               result = result && _assert(cppgres::from_nullable_datum<int64_t>(
                                              std::get<0>(re).get_nullable_datum(),
                                              std::get<0>(re).get_type().oid) == i + 1);
             }
           }

           {
             cppgres::spi_executor spi;
             auto res = spi.query<std::vector<cppgres::value>>(
                 "select $1 + i from generate_series(1,100) i", static_cast<int64_t>(1LL));

             int i = 0;
             for (auto &re : res) {
               i++;
               result = result && _assert(re[0].get_type() == cppgres::type{.oid = INT8OID});

               result = result &&
                        _assert(cppgres::from_nullable_datum<int64_t>(
                                    re[0].get_nullable_datum(), re[0].get_type().oid) == i + 1);
             }
           }

           return result;
         }));

add_test(spi_tupdesc_access, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           auto res = spi.query<std::tuple<cppgres::value, cppgres::value>>(
               "select 1 as a, 'a'::text as b");

           auto td = res.get_tuple_descriptor();
           result = result && _assert(td.get_name(0) == "a");
           result = result && _assert(td.get_type(0).oid == INT4OID);
           result = result && _assert(td.get_name(1) == "b");
           result = result && _assert(td.get_type(1).oid == TEXTOID);

           return result;
         }));

add_test(spi_readonly, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           spi.execute("create table q (i int)");

           bool exception_raised = false;

           try {
             spi.query<int>("insert into q default values returning i",
                            cppgres::spi_executor::options(true));
           } catch (...) {
             exception_raised = true;
           }

           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_execute_readonly, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;

           bool exception_raised = false;

           try {
             spi.execute("create table q (i int)", cppgres::spi_executor::options(true));
           } catch (...) {
             exception_raised = true;
           }

           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_plan_readonly, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           spi.execute("create table q (i int)");

           bool exception_raised = false;

           auto plan = spi.plan("insert into q default values returning i");

           try {
             spi.query<int>(plan, cppgres::spi_executor::options(true));
           } catch (...) {
             exception_raised = true;
           }

           result = result && _assert(exception_raised);

           return result;
         }));

add_test(spi_count, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           auto res = spi.query<int>("select i from generate_series(1,10) i",
                                     cppgres::spi_executor::options(2));

           result = result && _assert(res.count() == 2);

           return result;
         }));

add_test(spi_plan_count, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           auto plan = spi.plan("select i from generate_series(1,10) i");
           auto res = spi.query<int>(plan, cppgres::spi_executor::options(2));

           result = result && _assert(res.count() == 2);

           return result;
         }));

add_test(spi_string_length, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           std::string query = "select trueGARBAGEHERE";
           auto res =
               spi.query<bool>(std::string_view(query.c_str(), query.find_first_of("GARBAGEHERE")));

           result = result && _assert(res.begin()[0]);

           return result;
         }));

add_test(spi_plan_string_length, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           std::string query = "select trueGARBAGEHERE";
           auto plan =
               spi.plan(std::string_view(query.c_str(), query.find_first_of("GARBAGEHERE")));
           auto res = spi.query<bool>(plan);

           result = result && _assert(res.begin()[0]);

           return result;
         }));

add_test(spi_execute_string_length, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           std::string query = "create table spi_execute_string_length ()GARBAGEHERE";
           bool exception_raised = false;
           try {
             spi.execute(
                 std::string_view(query.c_str(), query.find_first_of("GARBAGEHERE")));
           } catch (std::exception &e) {
             exception_raised = true;
           }
           result = result && _assert(!exception_raised);

           return result;
         }));

add_test(spi_query_dml, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           bool exception_raised = false;
           try {
             spi.query<int>("create table spi_query_dml ()");
           } catch (std::exception &e) {
             exception_raised = true;
           }
           result = result && _assert(exception_raised);
           return result;
         }));

} // namespace tests
