#pragma once

#include <cppgres.hpp>
#include <map>

struct test_case {
  inline static std::map<std::string_view, test_case *> test_cases =
      std::map<std::string_view, test_case *>{};
  test_case(std::string_view name, bool (*function)(test_case &c), bool is_atomic = true);
  bool operator()();

  bool is_atomic() const { return atomic; }

protected:
  bool (*function)(test_case &c);
  bool atomic;
};

#define add_test(name, code) static test_case t__##name(std::string_view(#name), code);
#define add_non_atomic_test(name, code)                                                            \
  static test_case t__##name(std::string_view(#name), code, false);

#define _assert(expr)                                                                              \
  ({                                                                                               \
    auto value = (expr);                                                                           \
    if (!value) {                                                                                  \
      cppgres::report(NOTICE, "assertion failure %s:%d (`%s`): %s", __FILE_NAME__, __LINE__,       \
                      __func__, #expr);                                                            \
    }                                                                                              \
    value;                                                                                         \
  })

static const char *get_library_name();
