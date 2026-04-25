#include "sqlgen/sqlite/Iterator.hpp"

#include <ranges>
#include <rfl.hpp>
#include <sstream>

#include "sqlgen/internal/collect/vector.hpp"
#include "sqlgen/internal/strings/strings.hpp"
#include "sqlgen/sqlite/Iterator.hpp"

namespace sqlgen::sqlite {

Iterator::Iterator(const StmtPtr& _stmt, const ConnPtr& _conn)
    : end_(false),
      rownum_(0),
      num_cols_(sqlite3_column_count(_stmt.get())),
      stmt_(_stmt),
      conn_(_conn) {
  step();
}

Iterator::~Iterator() = default;

bool Iterator::end() const { return end_; }

Result<std::vector<std::vector<std::optional<std::string>>>> Iterator::next(
    const size_t _batch_size) {
  if (end()) {
    return error("End is reached.");
  }

  std::vector<std::vector<std::optional<std::string>>> batch;

  for (size_t i = 0; i < _batch_size; ++i) {
    std::vector<std::optional<std::string>> new_row;

    for (int j = 0; j < num_cols_; ++j) {
      auto ptr = sqlite3_column_text(stmt_.get(), j);
      if (ptr) {
        new_row.emplace_back(
            std::string(std::launder(reinterpret_cast<const char*>(ptr))));
      } else {
        new_row.emplace_back(std::nullopt);
      }
    }

    batch.emplace_back(std::move(new_row));

    step();

    if (end()) {
      return batch;
    }
  }

  return batch;
}

}  // namespace sqlgen::sqlite
