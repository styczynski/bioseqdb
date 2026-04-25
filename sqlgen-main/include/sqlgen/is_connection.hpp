#ifndef SQLGEN_ISCONNECTION_HPP_
#define SQLGEN_ISCONNECTION_HPP_

#include <concepts>
#include <optional>
#include <string>
#include <vector>

#include "Range.hpp"
#include "Ref.hpp"
#include "Result.hpp"
#include "dynamic/SelectFrom.hpp"
#include "dynamic/Statement.hpp"
#include "dynamic/Write.hpp"
#include "internal/MockTable.hpp"
#include "internal/iterator_t.hpp"

namespace sqlgen {

/// The concept any connection needs to implements.
template <class ConnType>
concept is_connection = requires(
    ConnType c, std::string _sql, dynamic::Statement _stmt,
    dynamic::SelectFrom _select_from, dynamic::Insert _insert,
    const dynamic::Write& _write, std::vector<internal::MockTable> _data) {
  /// Begins a transaction.
  { c.begin_transaction() } -> std::same_as<Result<Nothing>>;

  /// Commits a transaction.
  { c.commit() } -> std::same_as<Result<Nothing>>;

  /// Executes a statement.
  { c.execute(_sql) } -> std::same_as<Result<Nothing>>;

  /// Inserts data into the database using the INSERT statement.
  /// More minimal approach than write, but can be used inside transactions.
  {
    c.insert(_insert, _data.begin(), _data.end())
  } -> std::same_as<Result<Nothing>>;

  /// Reads the results of a SelectFrom statement.
  {
    c.template read<std::vector<internal::MockTable>>(_select_from)
  } -> std::same_as<Result<std::vector<internal::MockTable>>>;

  /// Reads the results of a SelectFrom statement as a Range.
  {
    c.template read<Range<internal::MockTable>>(_select_from)
  } -> std::same_as<
        Result<Range<internal::iterator_t<internal::MockTable, ConnType>>>>;

  /// Commits a transaction.
  { c.rollback() } -> std::same_as<Result<Nothing>>;

  /// Transpiles a statement to a particular SQL dialect.
  { c.to_sql(_stmt) } -> std::same_as<std::string>;

  /// Starts the write operation.
  { c.start_write(_write) } -> std::same_as<Result<Nothing>>;

  /// Ends the write operation and thus commits the results.
  { c.end_write() } -> std::same_as<Result<Nothing>>;

  /// Writes data into a table.
  { c.write(_data.begin(), _data.end()) } -> std::same_as<Result<Nothing>>;
};

}  // namespace sqlgen

#endif
