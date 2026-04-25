#ifndef SQLGEN_TRANSPILATION_TABLETUPLET_HPP_
#define SQLGEN_TRANSPILATION_TABLETUPLET_HPP_

#include <rfl.hpp>
#include <type_traits>
#include <utility>

#include "../Literal.hpp"
#include "../Result.hpp"
#include "Join.hpp"
#include "extract_table_t.hpp"

namespace sqlgen::transpilation {

template <class TableOrQueryType, class AliasType, class JoinsType>
struct TableTupleType;

template <class TableOrQueryType, class AliasType>
struct TableTupleType<TableOrQueryType, AliasType, Nothing> {
  using Type = TableOrQueryType;
};

template <class TableOrQueryType, class AliasType, class... JoinTypes>
struct TableTupleType<TableOrQueryType, AliasType, rfl::Tuple<JoinTypes...>> {
  template <class T>
  struct JoinWrapper {};

  template <bool _subsequent_right_or_full_join, class... SubsequentTypes>
  struct TableTupleTypeHelper {
    using Type =
        rfl::Tuple<std::pair<extract_table_t<TableOrQueryType,
                                             _subsequent_right_or_full_join>,
                             AliasType>,
                   SubsequentTypes...>;

    template <class T>
    friend consteval auto operator+(const JoinWrapper<T>&,
                                    const TableTupleTypeHelper&) {
      constexpr bool subsequent_right_or_full_join =
          _subsequent_right_or_full_join || T::how == JoinType::right_join ||
          T::how == JoinType::full_join;

      return TableTupleTypeHelper<
          subsequent_right_or_full_join,
          std::pair<extract_table_t<typename T::TableOrQueryType,
                                    _subsequent_right_or_full_join ||
                                        T::how == JoinType::left_join ||
                                        T::how == JoinType::full_join>,
                    typename T::Alias>,
          SubsequentTypes...>{};
    }
  };

  constexpr static auto helper =
      (JoinWrapper<JoinTypes>{} + ... + TableTupleTypeHelper<false>{});

  using Type = typename decltype(helper)::Type;

  static_assert(
      !rfl::define_literal_t<typename JoinTypes::Alias...>::has_duplicates(),
      "Your SELECT FROM query cannot contain duplicate aliases.");
};

template <class TableOrQueryType, class AliasType, class JoinsType>
using table_tuple_t =
    typename TableTupleType<std::remove_cvref_t<TableOrQueryType>,
                            std::remove_cvref_t<AliasType>,
                            std::remove_cvref_t<JoinsType>>::Type;

}  // namespace sqlgen::transpilation

#endif
