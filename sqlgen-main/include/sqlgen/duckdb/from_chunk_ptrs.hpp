#ifndef SQLGEN_DUCKDB_FROMCHUNKPTRS_HPP_
#define SQLGEN_DUCKDB_FROMCHUNKPTRS_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <rfl/NamedTuple.hpp>
#include <rfl/from_named_tuple.hpp>
#include <type_traits>
#include <utility>

#include "../Result.hpp"
#include "./parsing/Parser_default.hpp"
#include "ColumnData.hpp"
#include "chunk_ptrs_t.hpp"

namespace sqlgen::duckdb {

template <class T, class NamedTupeT, class ChunkPtrsT>
struct FromChunkPtrs;

template <class T, class... FieldTs, class... Ts, class... ColNames>
struct FromChunkPtrs<T, rfl::NamedTuple<FieldTs...>,
                     rfl::Tuple<ColumnData<Ts, ColNames>...>> {
  Result<T> operator()(
      const rfl::Tuple<ColumnData<Ts, ColNames>...>& _chunk_ptrs,
      idx_t _i) noexcept {
    return [&]<int... _is>(std::integer_sequence<int, _is...>) -> Result<T> {
      try {
        return rfl::from_named_tuple<T>(rfl::named_tuple_t<T>(
            duckdb::parsing::Parser<typename FieldTs::Type>::read(
                rfl::get<_is>(_chunk_ptrs).is_not_null(_i)
                    ? rfl::get<_is>(_chunk_ptrs).data + _i
                    : nullptr)
                .value()...));
      } catch (const std::exception& e) {
        return error(e.what());
      }
    }(std::make_integer_sequence<int, sizeof...(Ts)>());
  }
};

template <class T>
auto from_chunk_ptrs = FromChunkPtrs<std::remove_cvref_t<T>,
                                     rfl::named_tuple_t<T>, chunk_ptrs_t<T>>{};

}  // namespace sqlgen::duckdb

#endif
