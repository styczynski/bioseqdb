#ifndef SQLGEN_SQLITE_ITERATOR_HPP_
#define SQLGEN_SQLITE_ITERATOR_HPP_

#include <sqlite3.h>

#include <optional>
#include <string>
#include <vector>

#include "../Ref.hpp"
#include "../Result.hpp"
#include "../sqlgen_api.hpp"

namespace sqlgen::sqlite {

class SQLGEN_API Iterator {
  using ConnPtr = Ref<sqlite3>;
  using StmtPtr = Ref<sqlite3_stmt>;

 public:
  Iterator(const StmtPtr& _stmt, const ConnPtr& _conn);

  ~Iterator();

  /// Whether the end of the available data has been reached.
  bool end() const;

  /// Returns the next batch of rows.
  /// If _batch_size is greater than the number of rows left, returns all
  /// of the rows left.
  Result<std::vector<std::vector<std::optional<std::string>>>> next(
      const size_t _batch_size);

 private:
  void step() { end_ = (sqlite3_step(stmt_.get()) != SQLITE_ROW); }

 private:
  /// Whether the end is reached.
  bool end_;

  /// The current rownumber.
  size_t rownum_;

  /// The number of columns.
  int num_cols_;

  /// The prepared statement. Note that we have
  /// declared it before conn_, meaning it will be destroyed first.
  StmtPtr stmt_;

  /// The underlying sqlite3 connection. We have this in here to prevent its
  /// destruction for the lifetime of the iterator.
  ConnPtr conn_;
};

}  // namespace sqlgen::sqlite

#endif
