#pragma once

#include "tests.hpp"

namespace tests {

add_test(nullable_datum_enforcement, [](test_case &) {
  cppgres::nullable_datum d;
  assert(d.is_null());
  try {
    [[maybe_unused]] auto val = DatumGetDateADT(d);
    _assert(false);
  } catch (cppgres::null_datum_exception &e) {
    return true;
  }
  return false;
});

add_test(nullable_type_to_non_optional, ([](test_case &) {
           bool result = true;
           bool exception_raised = false;

           try {
             auto nd = cppgres::nullable_datum();
             cppgres::from_nullable_datum<int64_t>(nd, INT8OID);
           } catch (std::runtime_error &e) {
             exception_raised = true;
           }
           result = result && _assert(exception_raised);

           return result;
         }));

add_test(varlena_text, [](test_case &) {
  bool result = true;
  auto nd = cppgres::nullable_datum(PointerGetDatum(::cstring_to_text("test")));
  auto s = cppgres::from_nullable_datum<cppgres::text>(nd, TEXTOID);
  std::string_view str = s;
  result = result && _assert(s.is_detoasted());
  result = result && _assert(str == "test");

  // Try memory context being gone
  {
    auto ctx = cppgres::memory_context(std::move(cppgres::alloc_set_memory_context()));
    auto p = ::CurrentMemoryContext;
    _assert(p != ctx);
    ::CurrentMemoryContext = ctx;
    auto nd1 = cppgres::nullable_datum(PointerGetDatum(::cstring_to_text("test1")));
    auto s1 = cppgres::from_nullable_datum<cppgres::text>(nd1, TEXTOID, ctx);
    _assert(s1.get_memory_context() == ctx);
    ::CurrentMemoryContext = p;
    ctx.reset();

    bool exception_raised = false;
    try {
      s1.operator std::string_view();
    } catch (cppgres::pointer_gone_exception &e) {
      exception_raised = true;
    }
    result = result && _assert(exception_raised);
  }

  return result;
});

add_test(varlena_bytea, ([](test_case &) {
           bool result = true;
           auto nd = cppgres::nullable_datum(PointerGetDatum(::cstring_to_text("test")));
           auto s = cppgres::from_nullable_datum<cppgres::bytea>(nd, BYTEAOID);
           cppgres::byte_array ba = s;
           result = result && _assert(ba[0] == std::byte('t'));
           result = result && _assert(ba[1] == std::byte('e'));
           result = result && _assert(ba[2] == std::byte('s'));
           result = result && _assert(ba[3] == std::byte('t'));
           return result;
         }));

add_test(varlena_byte_array, ([](test_case &) {
           bool result = true;
           auto nd = cppgres::nullable_datum(PointerGetDatum(::cstring_to_text("test")));
           auto s = cppgres::from_nullable_datum<cppgres::bytea>(nd, BYTEAOID);
           cppgres::byte_array ba = s;
           const auto d = cppgres::datum_conversion<cppgres::byte_array>::into_datum(ba);
           // NB: below we endure a copy; can we do any better?
           auto ba1 = cppgres::datum_conversion<cppgres::byte_array>::from_datum(d, BYTEAOID,
                                                                                 std::nullopt);

           result = result && _assert(ba1[0] == std::byte('t'));
           result = result && _assert(ba1[1] == std::byte('e'));
           result = result && _assert(ba1[2] == std::byte('s'));
           result = result && _assert(ba1[3] == std::byte('t'));
           return result;
         }));

add_test(varlena_text_into_strings, ([](test_case &) {
           bool result = true;
           auto nd = cppgres::nullable_datum(PointerGetDatum(::cstring_to_text("test")));
           {
             auto str = cppgres::from_nullable_datum<std::string_view>(nd, TEXTOID);
             result = result && _assert(str == "test");
           }
           {
             auto str = cppgres::from_nullable_datum<std::string>(nd, TEXTOID);
             result = result && _assert(str == "test");
           }

           return result;
         }));

add_test(varlena_text_from_strings, ([](test_case &) {
           bool result = true;

           {
             auto d =
                 cppgres::datum_conversion<std::string_view>::into_datum(std::string_view("test"));
             auto str =
                 cppgres::datum_conversion<std::string_view>::from_datum(d, TEXTOID, std::nullopt);
             result = result && _assert(str == "test");
           }

           {
             auto d = cppgres::datum_conversion<std::string>::into_datum(std::string("test"));
             auto str =
                 cppgres::datum_conversion<std::string_view>::from_datum(d, TEXTOID, std::nullopt);
             result = result && _assert(str == "test");
           }

           return result;
         }));

add_test(cstring_into_datum_copies_input, ([](test_case &) {
           char storage[] = "first";
           auto d = cppgres::datum_conversion<const char *>::into_datum(
               static_cast<const char *>(storage));

           storage[0] = 'x';

           auto str =
               cppgres::datum_conversion<const char *>::from_datum(d, CSTRINGOID, std::nullopt);
           return _assert(std::string_view(str) == "first");
         }));

// Ensure arbitrary types are not automatically deducted
// to be convertible from into datums.
struct some_type {};
static_assert(!cppgres::convertible_from_datum<some_type>);
static_assert(!cppgres::convertible_into_datum<some_type>);

add_test(lazy_detoast, ([](test_case &) {
           bool result = true;
           cppgres::spi_executor spi;
           spi.execute("create table a (a text)");
           spi.execute("insert into a values ('hello world')");
           auto res = spi.query<cppgres::text>("select a from a");
           // here we never actually detoast it
           for (auto r : res) {
             result = result && _assert(!r.is_detoasted());
             spi.execute("insert into a values ($1)", r);
           }
           for (auto re : spi.query<std::string_view>("select a from a")) {
             result = result && _assert(re == "hello world");
           }
           return result;
         }));

add_test(detoasted_varlena_tracks_detoast_context, ([](test_case &) {
           bool result = true;
           bool saw_row = false;
           cppgres::spi_executor spi;

           spi.execute("create table detoast_context_test (v text)");
           spi.execute("alter table detoast_context_test alter column v set storage external");
           spi.execute("insert into detoast_context_test "
                       "select string_agg(md5(i::text), '') from generate_series(1, 10000) i");

           for (auto text_value : spi.query<cppgres::text>("select v from detoast_context_test")) {
             saw_row = true;
             result = result && _assert(VARATT_IS_EXTENDED(reinterpret_cast<::varlena *>(
                                    DatumGetPointer(text_value.get_datum()))));

             auto temp = cppgres::memory_context(std::move(cppgres::alloc_set_memory_context()));
             {
               cppgres::memory_context_scope scope(temp);
               std::string_view view = text_value;
               result = result && _assert(text_value.is_detoasted());
               result = result && _assert(view.size() > 100000);
             }

             temp.reset();

             bool exception_raised = false;
             try {
               [[maybe_unused]] std::string_view view = text_value;
             } catch (cppgres::pointer_gone_exception &) {
               exception_raised = true;
             }
             result = result && _assert(exception_raised);
           }

           return result && _assert(saw_row);
         }));

add_test(eoh_smoke, ([](test_case &e) {
           bool result = true;

           struct my_eoh {
             int a = 0;
             int b;
             int *c = nullptr;
             std::size_t flat_size() { return sizeof(a) * 100; }

             my_eoh() : b(100) {}
             my_eoh(int _a) : a(_a) {}

             void flatten_into(std::span<std::byte> buffer) {
               cppgres::report(NOTICE, "flatten_into");
               if (buffer.size_bytes() == flat_size()) {
                 int *to_ptr = reinterpret_cast<int *>(buffer.data());
                 std::span tbuffer(to_ptr, 1);
                 tbuffer[0] = a;
               } else {
                 cppgres::report(ERROR, "wrong buffer size");
               }
             }

             static cppgres::type type() { return {.oid = BYTEAOID}; }

             static my_eoh restore_from(std::span<std::byte> buffer) {
               int *to_ptr = reinterpret_cast<int *>(buffer.data());
               std::span tbuffer(to_ptr, 1);
               auto res = my_eoh(tbuffer[0]);
               if (res.flat_size() != buffer.size_bytes()) {
                 throw std::runtime_error("mismatch in size");
               }
               return res;
             }

             ~my_eoh() {
               if (c != nullptr) {
                 *c = a;
               }
             }
           };

           static_assert(cppgres::flattenable<my_eoh>);

           int val = 0;

           {
             // Create a new one
             auto d = cppgres::expanded_varlena<my_eoh>();
             my_eoh &dv = d;
             // Ensure the constructor is called
             result = result && _assert(dv.b == 100);
             dv.a = 650;
             dv.c = &val;

             // Flatten it. We are hijacking `bytea` varlena type to pass it through
             // to avoid having to define a new type (FIXME)
             cppgres::spi_executor spi;
             spi.execute("create table eoh_smoke_test (v bytea)");
             auto d2 = spi.query<cppgres::expanded_varlena<my_eoh>>(
                 "insert into eoh_smoke_test values ($1) returning v", d);
             result = result && _assert(d2.begin()[0].operator my_eoh &().a == dv.a);

             // Ensure we're reusing detoasted copy correctly
             auto d2v = d2.begin()[0];
             my_eoh &dv0 = d2v;
             result = result && _assert(d2v.is_detoasted());
             result = result && _assert(dv0.a == dv.a);
             // if we get it again, we get the same copy
             my_eoh &dv1 = d2v;
             result = result && _assert(dv1.a == dv0.a);

             // Let's insert it again
             auto d3 = spi.query<cppgres::expanded_varlena<my_eoh>>(
                 "insert into eoh_smoke_test values ($1) returning v", d2v);
             result = result && _assert(d3.begin()[0].operator my_eoh &().a == dv.a);

             // `d` is not destructed yet
             result = result && _assert(val == 0);
           }

           // `d` is still not destructed
           result = result && _assert(val == 0);

           // Reset parent memory context
           cppgres::memory_context().reset();

           // Ensure destructor gets called on `d`
           result = result && _assert(val == 650);

           // Also, try to pass args into it
           // (we just check this compiles)
           auto d = cppgres::expanded_varlena<my_eoh>(1);
           my_eoh &dv = d;
           // and the value gets passed
           result = result && _assert(dv.a == 1);

           return result;
         }));

struct flattening_tracker {
  int flattened;
  char hello[sizeof("this is so much fun")] = "this is so much fun";
  std::size_t flat_size() { return sizeof(int); }

  flattening_tracker() : flattened(false) {}
  flattening_tracker(int f) : flattened(f) {}

  void flatten_into(std::span<std::byte> buffer) {
    flattened = 1;
    if (buffer.size_bytes() == flat_size()) {
      int *to_ptr = reinterpret_cast<int *>(buffer.data());
      std::span tbuffer(to_ptr, 1);
      tbuffer[0] = flattened;
    } else {
      cppgres::report(ERROR, "wrong buffer size");
    }
  }

  static cppgres::type type() { return cppgres::named_type("flattening_tracker"); }

  static flattening_tracker restore_from(std::span<std::byte> buffer) {
    int *to_ptr = reinterpret_cast<int *>(buffer.data());
    std::span tbuffer(to_ptr, 1);
    auto res = flattening_tracker(tbuffer[0]);
    if (res.flat_size() != buffer.size_bytes()) {
      throw std::runtime_error("mismatch in size");
    }
    return res;
  }
};

postgres_function(flattening_tracker_new,
                  ([]() { return cppgres::expanded_varlena<flattening_tracker>(); }));

postgres_function(flattening_tracker_fun, ([](cppgres::expanded_varlena<flattening_tracker> f) {
                    return (f.operator flattening_tracker &().flattened) != 0;
                  }));

postgres_function(flattening_tracker_out,
                  ([](cppgres::expanded_varlena<flattening_tracker> f) -> const char * {
                    return ((f.operator flattening_tracker &().flattened) == 1 ? "f" : "n");
                  }));

postgres_function(flattening_tracker_in, ([](const char *v) {
                    return cppgres::expanded_varlena<flattening_tracker>(v[0] == 'f' ? 1 : 0);
                  }));

add_test(a_eoh_flattening, ([](test_case &e) {
           bool result = true;

           static_assert(cppgres::flattenable<flattening_tracker>);

           { // Establish the type
             cppgres::spi_executor spi;
             spi.execute("create type flattening_tracker");
             spi.execute(cppgres::fmt::format("    create function flattening_tracker_in(cstring)\n"
                                              "        returns flattening_tracker\n"
                                              "    as\n"
                                              "    '{}',\n"
                                              "    'flattening_tracker_in'\n"
                                              "        language c immutable\n"
                                              "                   strict\n",
                                              get_library_name()));
             spi.execute(cppgres::fmt::format(
                 "    create function flattening_tracker_out(flattening_tracker)\n"
                 "        returns cstring\n"
                 "    as\n"
                 "    '{}',\n"
                 "    'flattening_tracker_out'\n"
                 "        language c immutable\n"
                 "                   strict\n",
                 get_library_name()));
             spi.execute(cppgres::fmt::format("    create type flattening_tracker\n"
                                              "    (\n"
                                              "        input = flattening_tracker_in,\n"
                                              "        output = flattening_tracker_out,\n"
                                              "        alignment = int4,\n"
                                              "        storage = 'extended',\n"
                                              "        internallength = variable\n"
                                              "    )"));
             spi.execute(cppgres::fmt::format("    create function flattening_tracker_new()\n"
                                              "        returns flattening_tracker\n"
                                              "    as\n"
                                              "    '{}'\n"
                                              "        language c\n",
                                              get_library_name()));
             spi.execute(cppgres::fmt::format(
                 "    create function flattening_tracker_fun(flattening_tracker)\n"
                 "        returns bool\n"
                 "    as\n"
                 "    '{}'\n"
                 "        language c\n",
                 get_library_name()));
           }

           {
             cppgres::spi_executor spi;
             auto res = spi.query<bool>("select flattening_tracker_fun(flattening_tracker_new())");
             auto flattened = res.begin()[0];
             result = result && _assert(!flattened);
           }

           return result;
         }));

static_assert(cppgres::convertible_from_nullable_datum<cppgres::datum>);
static_assert(cppgres::convertible_from_nullable_datum<cppgres::nullable_datum>);
static_assert(cppgres::convertible_into_nullable_datum<cppgres::datum>);
static_assert(cppgres::convertible_into_nullable_datum<cppgres::nullable_datum>);

add_test(converting_null_nullable_datum_into_datum, ([](test_case &) {
           bool result = true;
           result = result && _assert(cppgres::into_nullable_datum(cppgres::nullable_datum()) ==
                                      cppgres::nullable_datum());
           result =
               result && _assert(cppgres::into_nullable_datum(cppgres::nullable_datum()).is_null());
           return result;
         }));
} // namespace tests

struct myopt {
  bool present;
  int value;
};

namespace cppgres {
template <> struct datum_conversion<myopt> {

  static myopt from_nullable_datum(const nullable_datum &d, const oid oid,
                                   std::optional<memory_context> context = std::nullopt) {
    if (d.is_null()) {
      return {false};
    }
    return from_datum(d, oid, context);
  }

  static myopt from_datum(const datum &d, oid oid, std::optional<memory_context> context) {
    return {true, cppgres::from_nullable_datum<int>(nullable_datum(d), oid, context)};
  }

  static datum into_datum(const myopt &t) { return t.present ? datum(0) : datum(t.value); }

  static nullable_datum into_nullable_datum(const myopt &t) {
    std::cout << t.present << std::endl;
    return !t.present ? nullable_datum() : nullable_datum(t.value);
  }
};
} // namespace cppgres

namespace tests {

add_test(converting_into_nullable_datum, ([](test_case &) {
           bool result = true;
           result = result && _assert(cppgres::into_nullable_datum(myopt{false}).is_null());
           result = result && _assert(!cppgres::into_nullable_datum(myopt{true}).is_null());
           return result;
         }));

} // namespace tests
