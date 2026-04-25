#ifndef SQLGEN_TRANSPILATION_ALLCOLUMNSEXIST_HPP_
#define SQLGEN_TRANSPILATION_ALLCOLUMNSEXIST_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "../Literal.hpp"
#include "Col.hpp"
#include "get_table_t.hpp"

namespace sqlgen::transpilation {

template <class TableType, class Names, class ColType>
struct ColumnExists;

template <class TableType, rfl::internal::StringLiteral _col_name,
          rfl::internal::StringLiteral _alias,
          rfl::internal::StringLiteral... _field_names>
struct ColumnExists<TableType, Literal<_field_names...>,
                    transpilation::Col<_col_name, _alias>> {
  static constexpr bool value = (false || ... || (_col_name == _field_names));
  static_assert(value, "Column does not exist.");
};

template <class TableType, class Names, class ColType>
constexpr bool column_exists_v = ColumnExists<TableType, Names, ColType>::value;

template <class TableTupleType, class... ColTypes>
consteval bool all_columns_exist() {
  return (true && ... &&
          column_exists_v<get_table_t<typename ColTypes::Alias, TableTupleType>,
                          typename rfl::named_tuple_t<get_table_t<
                              typename ColTypes::Alias, TableTupleType>>::Names,
                          ColTypes>);
}

}  // namespace sqlgen::transpilation

#endif
