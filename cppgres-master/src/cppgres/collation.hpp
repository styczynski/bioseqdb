#pragma once

#include "datum.hpp"
#include "imports.h"

namespace cppgres {

struct collation {

  collation(oid oid) : oid_(oid) {}

  [[nodiscard]]
  std::string name() const {
    return ffi_guard{::get_collation_name}(oid_);
  }

private:
  oid oid_;
};

} // namespace cppgres
