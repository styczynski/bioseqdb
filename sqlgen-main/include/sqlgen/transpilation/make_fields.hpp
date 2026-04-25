#ifndef SQLGEN_TRANSPILATION_MAKE_FIELDS_HPP_
#define SQLGEN_TRANSPILATION_MAKE_FIELDS_HPP_

#include <rfl.hpp>
#include <string>
#include <type_traits>

#include "../dynamic/SelectFrom.hpp"
#include "make_field.hpp"

namespace sqlgen::transpilation {

template <class TableTupleType, class Fields, int... _is>
std::vector<dynamic::SelectFrom::Field> make_fields(
    const Fields& _fields, std::integer_sequence<int, _is...>) {
  return std::vector<dynamic::SelectFrom::Field>(
      {make_field<TableTupleType>(rfl::get<_is>(_fields))...});
}

}  // namespace sqlgen::transpilation

#endif
