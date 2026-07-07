/**
 * \file
 */
#pragma once

namespace cppgres {
class pg_exception : public std::exception {
  ::MemoryContext mcxt;
  ::MemoryContext error_cxt;
  ::ErrorData *error;

  pg_exception(::MemoryContext mcxt);

  const char *what() const noexcept override { return error->message; }

  template <typename Func> friend struct ffi_guard;

public:
  const char *message() const noexcept { return error->message; }
  ~pg_exception();
};
} // namespace cppgres
