#include "sqlgen/mysql/Iterator.hpp"

namespace sqlgen::mysql {

Iterator::Iterator(const ResPtr& _res, const ConnPtr& _conn)
    : res_(_res), conn_(_conn), end_(false) {}

Iterator::~Iterator() = default;

bool Iterator::end() const { return end_; }

Result<std::vector<std::vector<std::optional<std::string>>>> Iterator::next(
    const size_t _batch_size) {
  std::vector<std::vector<std::optional<std::string>>> vec;

  const unsigned int num_fields = mysql_num_fields(res_.get());

  for (size_t i = 0; i < _batch_size; ++i) {
    MYSQL_ROW row = mysql_fetch_row(res_.get());

    if (!row) {
      const auto err = mysql_error(conn_.get());
      if (*err) {
        return error(err);
      }
      end_ = true;
      return vec;
    }

    std::vector<std::optional<std::string>> res_row(num_fields);

    for (unsigned int j = 0; j < num_fields; ++j) {
      if (row[j]) {
        res_row[j] = std::string(row[j]);
      } else {
        res_row[j] = std::nullopt;
      }
    }

    vec.emplace_back(std::move(res_row));
  }

  return vec;
}

}  // namespace sqlgen::mysql
