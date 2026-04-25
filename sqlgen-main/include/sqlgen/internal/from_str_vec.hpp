#ifndef SQLGEN_INTERNAL_FROM_STR_VEC_HPP_
#define SQLGEN_INTERNAL_FROM_STR_VEC_HPP_

#include <array>
#include <optional>
#include <rfl.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Result.hpp"
#include "../parsing/Parser.hpp"
#include "call_destructors_where_necessary.hpp"

namespace sqlgen::internal {

template <class ViewType, size_t i>
void assign_if_field_is_field_i(
    const std::vector<std::optional<std::string>>& _row, const size_t _i,
    ViewType* _view, std::optional<Error>* _err) noexcept {
  using FieldType = rfl::tuple_element_t<i, typename ViewType::Fields>;
  using T =
      std::remove_cvref_t<std::remove_pointer_t<typename FieldType::Type>>;
  constexpr auto name = FieldType::name();
  if (_i == i) {
    auto res = parsing::Parser<T>::read(_row[i]);
    if (!res) {
      std::stringstream stream;
      stream << "Failed to parse field '" << std::string(name)
             << "': " << res.error().what();
      *_err = Error(stream.str());
      return;
    }
    ::new (rfl::get<i>(*_view)) T(std::move(*res));
  }
}

template <class ViewType, size_t... is>
std::optional<Error> assign_to_field_i(
    const std::vector<std::optional<std::string>>& _row, const size_t _i,
    ViewType* _view, std::integer_sequence<size_t, is...>) noexcept {
  std::optional<Error> err;
  (assign_if_field_is_field_i<ViewType, is>(_row, _i, _view, &err), ...);
  return err;
}

template <class ViewType>
std::pair<std::optional<Error>, size_t> read_into_view(
    const std::vector<std::optional<std::string>>& _row,
    ViewType* _view) noexcept {
  constexpr size_t size = ViewType::size();
  if (_row.size() != size) {
    std::stringstream stream;
    stream << "Expected exactly " << std::to_string(size) << " fields, but got "
           << _row.size() << ".";
    return std::make_pair(Error(stream.str()), 0);
  }
  for (size_t i = 0; i < size; ++i) {
    const auto err = assign_to_field_i(
        _row, i, _view, std::make_integer_sequence<size_t, size>());
    if (err) {
      return std::make_pair(err, i);
    }
  }
  return std::make_pair(std::nullopt, size);
}

template <class T>
Result<T> from_str_vec(
    const std::vector<std::optional<std::string>>& _str_vec) {
  alignas(T) unsigned char buf[sizeof(T)]{};
  auto ptr = rfl::internal::ptr_cast<T*>(&buf);
  auto view = rfl::to_view(*ptr);
  const auto [err, num_fields_assigned] = read_into_view(_str_vec, &view);
  if (err) [[unlikely]] {
    call_destructors_where_necessary(num_fields_assigned, &view);
    return error(err->what());
  }
  auto res = Result<T>(std::move(*ptr));
  call_destructors_where_necessary(num_fields_assigned, &view);
  return res;
}

}  // namespace sqlgen::internal

#endif
