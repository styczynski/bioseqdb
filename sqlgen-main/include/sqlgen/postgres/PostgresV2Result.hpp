#ifndef SQLGEN_POSTGRES_POSTGRESV2RESULT_HPP_
#define SQLGEN_POSTGRES_POSTGRESV2RESULT_HPP_

#include <libpq-fe.h>

#include <memory>
#include <rfl.hpp>
#include <stdexcept>

#include "../Ref.hpp"
#include "../Result.hpp"
#include "../sqlgen_api.hpp"
#include "PostgresV2Connection.hpp"

namespace sqlgen::postgres {

class SQLGEN_API PostgresV2Result {
 public:
  PostgresV2Result(PGresult* _ptr)
      : ptr_(Ref<PGresult>::make(std::shared_ptr<PGresult>(_ptr, &PQclear))
                 .value()) {}

  ~PostgresV2Result() = default;

  static rfl::Result<PostgresV2Result> make(
      const std::string& _query, const PostgresV2Connection& _conn) noexcept;

  static rfl::Result<PostgresV2Result> make(
      const std::string& _query, const PostgresV2Connection& _conn,
      const std::vector<std::optional<std::string>>& _params) noexcept;

  static rfl::Result<PostgresV2Result> make(PGresult* _ptr) noexcept {
    try {
      return PostgresV2Result(_ptr);
    } catch (const std::exception& e) {
      return rfl::error("Failed to retrieve result from postgres: " +
                        std::string(e.what()));
    }
  }

  PGresult* ptr() const { return ptr_.get(); }

 private:
  Ref<PGresult> ptr_;
};

}  // namespace sqlgen::postgres

#endif
