#ifndef SQLGEN_UNITE_HPP_
#define SQLGEN_UNITE_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "Literal.hpp"
#include "Range.hpp"
#include "Ref.hpp"
#include "Result.hpp"
#include "dynamic/Union.hpp"
#include "internal/is_range.hpp"
#include "internal/iterator_t.hpp"
#include "is_connection.hpp"
#include "transpilation/Union.hpp"
#include "transpilation/fields_to_named_tuple_t.hpp"
#include "transpilation/get_table_t.hpp"
#include "transpilation/to_union.hpp"
#include "transpilation/value_t.hpp"
#include "transpilation/wrap_in_optional_t.hpp"

namespace sqlgen {

template <class ContainerType, class Connection, class... SelectTs>
  requires is_connection<Connection>
auto unite_impl(const Ref<Connection>& _conn,
                const rfl::Tuple<SelectTs...>& _stmts, const bool _all) {
  if constexpr (internal::is_range_v<ContainerType>) {
    const auto query = transpilation::to_union<ContainerType>(_stmts, _all);
    return _conn->template read<ContainerType>(query);

  } else {
    const auto to_container = [](auto range) -> Result<ContainerType> {
      using ValueType = transpilation::value_t<ContainerType>;
      ContainerType container;
      for (auto& res : range) {
        if (res) {
          container.emplace_back(
              rfl::from_named_tuple<ValueType>(std::move(*res)));
        } else {
          return error("One of the results in the union was an error.");
        }
      }
      return container;
    };

    using IteratorType =
        internal::iterator_t<transpilation::fields_to_named_tuple_t<
                                 transpilation::Union<SelectTs...>>,
                             decltype(_conn)>;

    using RangeType = Range<IteratorType>;

    return unite_impl<RangeType>(_conn, _stmts, _all).and_then(to_container);
  }
}

template <class _ContainerType, class... SelectTs>
struct Union {
  template <class Connection>
    requires is_connection<Connection>
  auto operator()(const Ref<Connection>& _conn) const {
    using ContainerType = std::conditional_t<
        std::is_same_v<std::remove_cvref_t<_ContainerType>, Nothing>,
        Range<internal::iterator_t<transpilation::fields_to_named_tuple_t<
                                       transpilation::Union<SelectTs...>>,
                                   Connection>>,
        _ContainerType>;

    return unite_impl<ContainerType>(_conn, selects_, all_);
  }

  template <class Connection>
    requires is_connection<Connection>
  auto operator()(const Result<Ref<Connection>>& _res) const {
    return _res.and_then([&](const auto& _conn) { return (*this)(_conn); });
  }

  rfl::Tuple<SelectTs...> selects_;
  bool all_ = false;
};

namespace transpilation {

template <class ContainerType, class... SelectTs>
struct ExtractTable<sqlgen::Union<ContainerType, SelectTs...>, false> {
  using Type = std::conditional_t<
      std::is_same_v<std::remove_cvref_t<ContainerType>, Nothing>,
      fields_to_named_tuple_t<Union<SelectTs...>>, value_t<ContainerType>>;
};

template <class ContainerType, class... SelectTs>
struct ExtractTable<sqlgen::Union<ContainerType, SelectTs...>, true> {
  using Type = wrap_in_optional_t<typename ExtractTable<
      sqlgen::Union<ContainerType, SelectTs...>, false>::Type>;
};

template <class ContainerType, class... SelectTs>
struct ToTableOrQuery<sqlgen::Union<ContainerType, SelectTs...>> {
  dynamic::SelectFrom::TableOrQueryType operator()(const auto& _stmt) {
    const auto query = to_union<ContainerType>(_stmt.selects_, _stmt.all_);
    return Ref<dynamic::Union>::make(query);
  }
};

template <class ContainerType, class... SelectTs, class... FieldTs>
struct FieldsToNamedTupleType<sqlgen::Union<ContainerType, SelectTs...>,
                              FieldTs...> {
  using Type = fields_to_named_tuple_t<Union<SelectTs...>, FieldTs...>;
};

template <class ContainerType, class... SelectTs, class... FieldTs>
struct FieldsToNamedTupleType<sqlgen::Union<ContainerType, SelectTs...>,
                              rfl::Tuple<FieldTs...>> {
  using Type = fields_to_named_tuple_t<Union<SelectTs...>, FieldTs...>;
};

template <rfl::internal::StringLiteral _alias, class ContainerType,
          class... SelectTs>
struct GetTableType<Literal<_alias>,
                    sqlgen::Union<ContainerType, SelectTs...>> {
  using TableType = get_table_t<
      Literal<_alias>,
      rfl::Tuple<std::pair<
          extract_table_t<sqlgen::Union<ContainerType, SelectTs...>, false>,
          Literal<_alias>>>>;
};

template <class ContainerType, class... SelectTs>
struct GetTableType<Literal<"">, sqlgen::Union<ContainerType, SelectTs...>> {
  using TableType = get_table_t<
      Literal<"">,
      extract_table_t<sqlgen::Union<ContainerType, SelectTs...>, false>>;
};

template <size_t _i, class ContainerType, class... SelectTs>
struct GetTableType<std::integral_constant<size_t, _i>,
                    sqlgen::Union<ContainerType, SelectTs...>> {
  using TableType = get_table_t<
      std::integral_constant<size_t, _i>,
      extract_table_t<sqlgen::Union<ContainerType, SelectTs...>, false>>;
};

template <class ContainerType, class... SelectTs, class AliasType>
struct TableTupleType<sqlgen::Union<ContainerType, SelectTs...>, AliasType,
                      Nothing> {
  using Type = rfl::Tuple<std::pair<
      extract_table_t<sqlgen::Union<ContainerType, SelectTs...>, false>,
      AliasType>>;
};

template <class ContainerType, class... SelectTs>
struct TableTupleType<sqlgen::Union<ContainerType, SelectTs...>, Literal<"">,
                      Nothing> {
  using Type =
      extract_table_t<sqlgen::Union<ContainerType, SelectTs...>, false>;
};

}  // namespace transpilation

template <class ContainerType, class... SelectTs>
auto unite(const SelectTs&... _stmts) {
  return Union<ContainerType, SelectTs...>{
      .selects_ = rfl::Tuple<SelectTs...>(_stmts...), .all_ = false};
}

template <class... SelectTs>
auto unite(const SelectTs&... _stmts) {
  return unite<Nothing>(_stmts...);
}

template <class ContainerType, class... SelectTs>
auto unite_all(const SelectTs&... _stmts) {
  return Union<ContainerType, SelectTs...>{
      .selects_ = rfl::Tuple<SelectTs...>(_stmts...), .all_ = true};
}

template <class... SelectTs>
auto unite_all(const SelectTs&... _stmts) {
  return unite_all<Nothing>(_stmts...);
}

}  // namespace sqlgen

#endif
