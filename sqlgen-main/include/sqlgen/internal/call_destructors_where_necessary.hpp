#ifndef SQLGEN_INTERNAL_CALL_DESTRUCTORS_WHERE_NECESSARY_HPP_
#define SQLGEN_INTERNAL_CALL_DESTRUCTORS_WHERE_NECESSARY_HPP_

#include <rfl.hpp>
#include <type_traits>

namespace sqlgen::internal {

/// Because of the way we have allocated the fields, we need to manually
/// trigger the destructors.
template <class ViewType, size_t _size, size_t _i>
void call_destructor_on_one_if_necessary(const size_t _num_fields_assigned,
                                         ViewType* _view) {
  using FieldType = rfl::tuple_element_t<_i, typename ViewType::Fields>;
  using OriginalType = std::remove_cv_t<typename FieldType::Type>;
  using ValueType =
      std::remove_cvref_t<std::remove_pointer_t<typename FieldType::Type>>;
  if constexpr (std::is_pointer_v<OriginalType> &&
                std::is_destructible_v<ValueType>) {
    if (_i < _num_fields_assigned) {
      rfl::get<_i>(*_view)->~ValueType();
    }
  }
}

template <class ViewType>
void call_destructors_where_necessary(const size_t _num_fields_assigned,
                                      ViewType* _view) {
  constexpr size_t size = ViewType::size();
  [&]<size_t... is>(std::integer_sequence<size_t, is...>) {
    (call_destructor_on_one_if_necessary<ViewType, size, is>(
         _num_fields_assigned, _view),
     ...);
  }(std::make_integer_sequence<size_t, size>());
}

}  // namespace sqlgen::internal

#endif
