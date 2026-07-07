/**
 * \file
 */
#pragma once

#include <string>
#include <tuple>

#include <iostream>

#include "exception.hpp"

namespace cppgres {

inline void error(pg_exception e);
inline void error(pg_exception e) {
  ::errstart(ERROR, TEXTDOMAIN);
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#endif
  ::errmsg("%s", e.message());
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
  ::errfinish(__FILE__, __LINE__, __func__);
  __builtin_unreachable();
}

template <typename T>
concept error_formattable =
    std::integral<std::decay_t<T>> ||
    (std::is_pointer_v<std::decay_t<T>> &&
     std::same_as<std::remove_cv_t<std::remove_pointer_t<std::decay_t<T>>>, char>) ||
    std::is_pointer_v<std::decay_t<T>>;

template <std::size_t N, error_formattable... Args>
inline void report(int elevel, const char (&fmt)[N], Args... args) {
  ::errstart(elevel, TEXTDOMAIN);
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#endif
  ::errmsg(fmt, args...);
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
  ::errfinish(__FILE__, __LINE__, __func__);
  if (elevel >= ERROR) {
    __builtin_unreachable();
  }
}
} // namespace cppgres
