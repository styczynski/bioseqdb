#ifndef SQLGEN_TRANSPILATION_TO_TABLE_OR_QUERY_HPP_
#define SQLGEN_TRANSPILATION_TO_TABLE_OR_QUERY_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "../dynamic/SelectFrom.hpp"
#include "../dynamic/Table.hpp"
#include "TableWrapper.hpp"
#include "get_schema.hpp"
#include "get_tablename.hpp"

namespace sqlgen::transpilation {

template <class T>
struct ToTableOrQuery;

template <class TableType>
struct ToTableOrQuery<TableWrapper<TableType>> {
  dynamic::SelectFrom::TableOrQueryType operator()(const auto&) {
    using T = std::remove_cvref_t<TableType>;
    return dynamic::Table{.alias = std::nullopt,
                          .name = get_tablename<T>(),
                          .schema = get_schema<T>()};
  }
};

template <class T>
dynamic::SelectFrom::TableOrQueryType to_table_or_query(const T& _t) {
  return ToTableOrQuery<T>{}(_t);
}

}  // namespace sqlgen::transpilation

#endif
