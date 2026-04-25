#ifndef SQLGEN_POSTGRES_ITERATOR_HPP_
#define SQLGEN_POSTGRES_ITERATOR_HPP_

#include <libpq-fe.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../Ref.hpp"
#include "../Result.hpp"
#include "../internal/random.hpp"
#include "../sqlgen_api.hpp"
#include "PostgresV2Connection.hpp"
#include "PostgresV2Result.hpp"

namespace sqlgen::postgres {

class SQLGEN_API Iterator {
  using Conn = PostgresV2Connection;

 public:
  Iterator(const std::string& _sql, const Conn& _conn);

  Iterator(const Iterator& _other) = delete;

  Iterator(Iterator&& _other) noexcept;

  ~Iterator();

  /// Whether the end of the available data has been reached.
  bool end() const;

  /// Returns the next batch of rows.
  /// If _batch_size is greater than the number of rows left, returns all
  /// of the rows left.
  Result<std::vector<std::vector<std::optional<std::string>>>> next(
      const size_t _batch_size);

  Iterator& operator=(const Iterator& _other) = delete;

  Iterator& operator=(Iterator&& _other) noexcept;

  static rfl::Result<Ref<Iterator>> make(const std::string& _sql,
                                         const Conn& _conn) noexcept {
    try {
      return Ref<Iterator>::make(_sql, _conn);
    } catch (const std::exception& e) {
      return error(e.what());
    }
  }

 private:
  static std::string make_cursor_name() {
    return "sqlgen_cursor_" + internal::random();
  }

  /// Shuts the iterator down.
  void shutdown();

 private:
  /// A unique name to identify the cursor.
  std::string cursor_name_;

  /// The underlying postgres connection. We have this in here to prevent its
  /// destruction for the lifetime of the iterator.
  Conn conn_;

  /// Whether the end is reached.
  bool end_;
};

}  // namespace sqlgen::postgres

#endif
