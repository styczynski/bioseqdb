#ifndef SQLGEN_DUCKDB_CHUNKPTRST_HPP_
#define SQLGEN_DUCKDB_CHUNKPTRST_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "./parsing/Parser.hpp"
#include "ColumnData.hpp"

namespace sqlgen::duckdb {

template <class T>
struct ChunkPtrsType;

template <class... FieldTs>
struct ChunkPtrsType<rfl::NamedTuple<FieldTs...>> {
  using Type = rfl::Tuple<ColumnData<
      typename duckdb::parsing::Parser<typename FieldTs::Type>::ResultingType,
      typename FieldTs::Name>...>;
};

template <class T>
struct ChunkPtrsType {
  using Type = typename ChunkPtrsType<rfl::named_tuple_t<T>>::Type;
};

template <class T>
using chunk_ptrs_t = typename ChunkPtrsType<std::remove_cvref_t<T>>::Type;

}  // namespace sqlgen::duckdb

#endif
