#pragma once

#include "tests.hpp"

namespace tests {

// Test signature compliance
postgres_function(_sig1, ([] { return true; }));
postgres_function(_sig2, ([](std::optional<bool> v) { return v; }));
postgres_function(_sig3, ([](bool v) { return v; }));
postgres_function(_sig4, ([](bool v, std::optional<int64_t> n) { return v && n.has_value(); }));
postgres_function(_void_sig_fun, ([] {}));
postgres_function(strict_add_c, ([](int32_t a, int32_t b) { return a + b; }));

postgres_function(cstring_fun, ([](const char *s) { return std::string_view(s); }));
postgres_function(to_cstring_fun, ([](std::string_view s) { return s.data(); }));

add_test(cstring_fun_test, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           spi.execute(
               cppgres::fmt::format("create function cstring_fun(cstring) returns text language c as '{}'",
                           get_library_name()));
           spi.execute(cppgres::fmt::format(
               "create function to_cstring_fun(text) returns cstring language c as '{}'",
               get_library_name()));

           {
             auto res = spi.query<cppgres::text>("select cstring_fun($1)", "hello");
             result = result && _assert(res.begin()[0].operator std::string_view() == "hello");
           }

           {
             auto res =
                 spi.query<const char *>("select to_cstring_fun($1)", std::string_view("hello"));
             result = result && _assert(strncmp(res.begin()[0], "hello", 5) == 0);
           }

           return result;
         }));

postgres_function(_byte_array_sig, ([](const cppgres::byte_array) { return true; }));

postgres_function(infer, ([](std::string_view s) { return s; }));

add_test(syscache_type_inference, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute(cppgres::fmt::format("create function infer(text) returns text language c as '{}'",
                                   get_library_name()));
           auto func_oid = spi.query<cppgres::oid>("select 'infer'::regproc::oid").begin()[0];
           auto v = cppgres::datum_conversion<std::string_view>::from_datum(
               cppgres::datum(cppgres::ffi_guard{::OidFunctionCall1Coll}(
                   func_oid, 0, PointerGetDatum(::cstring_to_text("test")))),
               TEXTOID, cppgres::memory_context());
           result = result && _assert(v == "test");
           return result;
         }));

add_test(syscache_type_inference_priority, ([](test_case &) {
           // This one ensures that we don't impose the number of args called but the number
           // of args receivable when there's lack of typing information.
           //
           // This is the case with things like "input functions" – regardless of the signature,
           // `InputFunctionCall` will supply three argument and there are no types given.
           //
           // We are effectively simulating this here.
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute(cppgres::fmt::format("create function infer(text) returns text language c as '{}'",
                                   get_library_name()));
           auto func_oid = spi.query<cppgres::oid>("select 'infer'::regproc::oid").begin()[0];
           auto v = cppgres::datum_conversion<std::string_view>::from_datum(
               cppgres::datum(cppgres::ffi_guard{::OidFunctionCall2Coll}(
                   func_oid, 0, PointerGetDatum(::cstring_to_text("test")), cppgres::datum(0))),
               TEXTOID, cppgres::memory_context());
           result = result && _assert(v == "test");
           return result;
         }));

add_test(enforce_return_type, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;

           bool exception_raised = false;
           spi.execute(cppgres::fmt::format("create function _sig1() returns int language c as '{}'",
                                   get_library_name()));
           {
             cppgres::internal_subtransaction xact(false);
             try {
               spi.query<int32_t>("select _sig1()");
             } catch (cppgres::pg_exception &e) {
               exception_raised =
                   true && _assert(std::string_view(e.message()) ==
                                   "unexpected return type, can't convert `integer` into `bool`");
             }
           }

           result = result && _assert(exception_raised);

           return result;
         }));

add_test(current_postgres_function, ([](test_case &) {
           bool result = true;

           auto ci = cppgres::current_postgres_function::call_info();
           result = result && _assert(ci.has_value());
           cppgres::syscache<Form_pg_proc, cppgres::oid> proc((*ci).called_function_oid());
           result = result && _assert(std::string_view(NameStr((*proc).proname)) == "cppgres_test");

           // args
           result = result && _assert((*ci).nargs() == 1);
           result = result && _assert((*ci).arg_types()[0] == cppgres::type{.oid = TEXTOID});
           result =
               result && _assert((*ci).arg_values()[0].get_type() == cppgres::type{.oid = TEXTOID});

           // return type
           result = result && _assert((*ci).return_type() == cppgres::type{.oid = BOOLOID});

           // collation
           result = result && _assert(cppgres::collation((*ci).collation()).name() == "default");

           return result;
         }));

add_test(function_call, ([](test_case &) {
           bool result = true;

           {
             cppgres::function<int32_t, std::string> f("length");
             result = result && _assert(f("test") == 4);
           }

           {
             cppgres::function<std::optional<int32_t>, std::optional<std::string>> f("length");
             result = result && _assert(f("test").value() == 4);
             result = result && _assert(!f(std::nullopt).has_value());
           }

           {
             cppgres::function<int32_t, std::string> f("pg_catalog", "length");
             result = result && _assert(f("test") == 4);
           }

           /// Erroneous cases:

           {
             // Wrong arg
             cppgres::internal_subtransaction sub;
             bool exception_raised = false;
             try {
               cppgres::function<int32_t, std::int32_t> f("length");
             } catch (std::exception &e) {
               result =
                   result &&
                   _assert(std::string_view("function length(integer) does not exist") == e.what());
               exception_raised = true;
             }
             result = result && _assert(exception_raised);
           }

           {
             // Wrong arg count
             cppgres::internal_subtransaction sub;
             bool exception_raised = false;
             try {
               cppgres::function<int32_t, std::string, std::string> f("length");
             } catch (std::exception &e) {
               result = result &&
                        _assert(std::string_view("function length(text, text) does not exist") ==
                                e.what());
               exception_raised = true;
             }
             result = result && _assert(exception_raised);
           }

           {
             // Wrong return type
             cppgres::internal_subtransaction sub;
             bool exception_raised = false;
             try {
               cppgres::function<std::string, std::string> f("length");
             } catch (std::exception &e) {
               result =
                   result &&
                   _assert(std::string_view("expected return type text, got integer") == e.what());
               exception_raised = true;
             }
             result = result && _assert(exception_raised);
           }

           {
             // Wrong name
             cppgres::internal_subtransaction sub;
             bool exception_raised = false;
             try {
               cppgres::function<int32_t, std::string> f("lengt");
             } catch (std::exception &e) {
               result = result && _assert(std::string_view("function lengt(text) does not exist") ==
                                          e.what());
               exception_raised = true;
             }
             result = result && _assert(exception_raised);
           }

           return result;
         }));

add_test(function_oid_rejects_extra_args, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           auto func_oid =
               spi.query<cppgres::oid>("select 'pg_catalog.length(text)'::regprocedure::oid")
                   .begin()[0];
           bool exception_raised = false;
           try {
             cppgres::function<int32_t, std::string, std::string> f(func_oid);
           } catch (std::exception &e) {
             exception_raised = true;
           }
           result = result && _assert(exception_raised);
           return result;
         }));

add_test(function_strict_null_short_circuit, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute(cppgres::fmt::format("create function strict_add_c(int, int) returns int "
                                            "language c strict as '{}'",
                                            get_library_name()));
           cppgres::function<std::optional<int32_t>, std::optional<int32_t>,
                             std::optional<int32_t>>
               f("strict_add_c");
           {
             cppgres::internal_subtransaction tx(false);
             bool exception_raised = false;
             try {
               result =
                   result && _assert(!f(std::nullopt, std::optional<int32_t>(1)).has_value());
             } catch (std::exception &e) {
               exception_raised = true;
             }
             result = result && _assert(!exception_raised);
           }
           return result;
         }));

add_test(postgres_function_rejects_too_few_fmgr_args, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute(cppgres::fmt::format("create function strict_add_c_arity(int, int) "
                                            "returns int language c strict as '{}', "
                                            "'strict_add_c'",
                                            get_library_name()));
           auto func_oid =
               spi.query<cppgres::oid>("select 'strict_add_c_arity(int, int)'::regprocedure::oid")
                   .begin()[0];

           bool expected_exception_raised = false;
           {
             cppgres::internal_subtransaction tx(false);
             try {
               cppgres::ffi_guard{::OidFunctionCall1Coll}(func_oid, InvalidOid,
                                                           Int32GetDatum(1));
             } catch (cppgres::pg_exception &e) {
               expected_exception_raised =
                   std::string_view(e.message()).find("expected 2 arguments") !=
                   std::string_view::npos;
             } catch (std::exception &e) {
             }
           }

           result = result && _assert(expected_exception_raised);
           return result;
         }));

// Function that takes a function
postgres_function(function_arg, ([](cppgres::function<std::int32_t, std::string_view> f,
                                    std::string_view s) { return f(s); }));

add_test(function_takes_a_function, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute(cppgres::fmt::format(
               "create function function_arg(regprocedure, text) returns int language c as '{}'",
               get_library_name()));
           auto val = spi.query<std::int32_t>("select function_arg('length(text)', 'hello')");
           result = result && _assert(val.begin()[0] == std::string("hello").length());

           {
             // Wrong return type
             cppgres::internal_subtransaction tx(false);
             bool exception_raised = false;
             try {
               spi.query<std::int32_t>("select function_arg('lower(text)', 'hello')");
             } catch (std::exception &e) {
               exception_raised =
                   _assert(std::string_view("exception: expected return type integer, got text") ==
                           e.what());
             }
             result = result && _assert(exception_raised);
           }

           {
             // Wrong argument type
             cppgres::internal_subtransaction tx(false);
             bool exception_raised = false;
             try {
               spi.query<std::int32_t>("select function_arg('abs(integer)', 'hello')");
             } catch (std::exception &e) {
               exception_raised = _assert(
                   std::string_view("exception: expected type text for argument 0, got integer") ==
                   e.what());
             }
             result = result && _assert(exception_raised);
           }

           return result;
         }));

// Function that takes a function with "any"
postgres_function(function_arg_value, ([](cppgres::function<cppgres::value, cppgres::value> f,
                                          cppgres::value v) { return f(v); }));

add_test(function_takes_a_function_with_value, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute(cppgres::fmt::format("create function function_arg_value(regprocedure, "
                                            "anyelement) returns anyelement language c as '{}'",
                                            get_library_name()));
           auto val = spi.query<std::int32_t>("select function_arg_value('abs(int)', -3)");
           result = result && _assert(val.begin()[0] == 3);

           return result;
         }));

// Function that returns a function
postgres_function(function_ret,
                  ([](std::string s) { return cppgres::function<int32_t, int32_t>(s); }));

add_test(function_returns_function, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute(cppgres::fmt::format(
               "create function function_ret(text) returns regprocedure language c as '{}'",
               get_library_name()));
           auto val = spi.query<bool>("select function_ret('abs') = 'abs(int)'::regprocedure");
           result = result && _assert(val.begin()[0]);

           {
             // Missing function
             cppgres::internal_subtransaction tx(false);
             bool exception_raised = false;
             try {
               spi.query<bool>("select function_ret('absolute') = 'abs(int)'::regprocedure");
             } catch (std::exception &e) {
               exception_raised = true;
             }
             result = result && _assert(exception_raised);
           }

           {
             // Wrong args function
             cppgres::internal_subtransaction tx(false);
             bool exception_raised = false;
             try {
               spi.query<bool>(
                   "select function_ret('length(text)') = 'length(text)'::regprocedure");
             } catch (std::exception &e) {
               exception_raised = true;
             }
             result = result && _assert(exception_raised);
           }
           return result;
         }));

} // namespace tests
