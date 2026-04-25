#ifndef SQLGEN_DUCKDB_MAKECHUNKPTRS_HPP_
#define SQLGEN_DUCKDB_MAKECHUNKPTRS_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <rfl/enums.hpp>
#include <rfl/type_name_t.hpp>
#include <type_traits>
#include <utility>

#include "../Ref.hpp"
#include "ColumnData.hpp"
#include "DuckDBResult.hpp"
#include "cast_duckdb_type.hpp"
#include "check_duckdb_type.hpp"
#include "chunk_ptrs_t.hpp"

namespace sqlgen::duckdb {

template <class T>
struct MakeChunkPtrs;

template <class... Ts, class... ColNames>
struct MakeChunkPtrs<rfl::Tuple<ColumnData<Ts, ColNames>...>> {
  Result<rfl::Tuple<ColumnData<Ts, ColNames>...>> operator()(
      const Ref<DuckDBResult>& _res, duckdb_data_chunk _chunk) {
    try {
      return [&]<int... _is>(std::integer_sequence<int, _is...>) {
        return rfl::Tuple<ColumnData<Ts, ColNames>...>(
            make_column_data<Ts, ColNames, _is>(_res, _chunk)...);
      }(std::make_integer_sequence<int, sizeof...(Ts)>());
    } catch (const std::exception& e) {
      return error(e.what());
    }
  }

  template <class T, class ColName, int _i>
  static auto make_column_data(const Ref<DuckDBResult>& _res,
                               duckdb_data_chunk _chunk) {
    const auto actual_duckdb_type = duckdb_column_type(&_res->res(), _i);

    auto vec = duckdb_data_chunk_get_vector(_chunk, _i);

    if (check_duckdb_type<T>(actual_duckdb_type)) {
      return ColumnData<T, ColName>{
          .vec = vec,
          .data = static_cast<T*>(duckdb_vector_get_data(vec)),
          .validity = duckdb_vector_get_validity(vec)};
    }

    if constexpr (std::is_same_v<T, bool>) {
      throw std::runtime_error("Wrong type in field '" + ColName().str() +
                               "'. " + rfl::enum_to_string(actual_duckdb_type) +
                               " could not be cast to " +
                               rfl::type_name_t<T>().str() + ".");

    } else {
      const auto ptr_res = cast_duckdb_type<T>(
          actual_duckdb_type, duckdb_data_chunk_get_size(_chunk),
          duckdb_vector_get_data(vec));

      if (!ptr_res) {
        throw std::runtime_error(
            "Wrong type in field '" + ColName().str() + "'. " +
            rfl::enum_to_string(actual_duckdb_type) + " could not be cast to " +
            rfl::type_name_t<T>().str() + ".");
      }

      return ColumnData<T, ColName>{.vec = vec,
                                    .data = (*ptr_res)->data(),
                                    .validity = duckdb_vector_get_validity(vec),
                                    .ptr = ptr_res->ptr()};
    }
  }
};

template <class T>
struct MakeChunkPtrs {
  auto operator()(const Ref<DuckDBResult>& _res, duckdb_data_chunk _chunk) {
    return MakeChunkPtrs<chunk_ptrs_t<T>>{}(_res, _chunk);
  }
};

template <class T>
auto make_chunk_ptrs = MakeChunkPtrs<std::remove_cvref_t<T>>{};

}  // namespace sqlgen::duckdb

#endif
