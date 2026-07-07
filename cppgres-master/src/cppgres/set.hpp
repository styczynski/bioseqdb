/**
* \file
 */
#pragma once

#include <iterator>

#include "datum.hpp"
#include "imports.h"
#include "types.hpp"

namespace cppgres {

template <typename I>
concept datumable_iterator =
    requires(I i) {
      { std::begin(i) } -> std::input_iterator;
      { std::end(i) } -> std::sentinel_for<decltype(std::begin(i))>;
    } &&
    all_from_nullable_datum<typename std::iterator_traits<decltype(std::begin(
        std::declval<I &>()))>::value_type>::value;

template <datumable_iterator I> struct set_iterator_traits {
  using value_type = std::iterator_traits<decltype(std::begin(std::declval<I &>()))>::value_type;
};

template <typename I> requires datumable_iterator<I>
struct type_traits<I> {
  bool is(type &t) { return t.oid == RECORDOID; }
};

} // namespace cppgres
