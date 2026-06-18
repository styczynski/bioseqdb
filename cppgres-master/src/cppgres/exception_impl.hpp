/**
* \file
 */
#pragma once

#include "exception.hpp"
#include "guard.hpp"
#include "memory.hpp"

namespace cppgres {

inline pg_exception::pg_exception(::MemoryContext mcxt) : mcxt(mcxt) {
  ::CurrentMemoryContext = error_cxt =
      memory_context(std::move(alloc_set_memory_context(top_memory_context())));
  error = ffi_guard{::CopyErrorData}();
  ::CurrentMemoryContext = mcxt;
  ffi_guard{::FlushErrorState}();
}

inline pg_exception::~pg_exception() { memory_context(error_cxt).delete_context(); }

} // namespace cppgres
