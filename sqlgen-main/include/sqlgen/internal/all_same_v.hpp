#include <rfl.hpp>
#include <tuple>
#include <type_traits>

namespace sqlgen::internal {

template <class TupleT>
struct AllSame;

template <class Head, class... Tail>
struct AllSame<std::tuple<Head, Tail...>> {
  static constexpr bool value = std::conjunction_v<std::is_same<Head, Tail>...>;
};

template <class Head, class... Tail>
struct AllSame<rfl::Tuple<Head, Tail...>> {
  static constexpr bool value = std::conjunction_v<std::is_same<Head, Tail>...>;
};

template <class TupleT>
constexpr bool all_same_v = AllSame<std::remove_cvref_t<TupleT>>::value;

}  // namespace sqlgen::internal
