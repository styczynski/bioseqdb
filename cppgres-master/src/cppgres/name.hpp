#pragma once

#include "imports.h"

namespace cppgres {

struct name {

  template <int N> requires(N < NAMEDATALEN)
  name(const char name[N]) : _name({.data = name}) {}

  name(const char *name) : _name{} { strncpy(NameStr(_name), name, NAMEDATALEN - 1); }

  operator NameData &() const { return _name; }

private:
  mutable NameData _name;
};

} // namespace cppgres
