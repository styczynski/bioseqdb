#pragma once

#include "datum.hpp"
#include "guard.hpp"
#include "heap_tuple.hpp"
#include "imports.h"
#include "xact.hpp"

namespace cppgres {

template <typename T> struct syscache_traits {};

template <> struct syscache_traits<Form_pg_type> {
  static constexpr ::SysCacheIdentifier cache_id = TYPEOID;
};

template <> struct syscache_traits<Form_pg_proc> {
  static constexpr ::SysCacheIdentifier cache_id = PROCOID;
};

template <typename T>
concept syscached = requires(T t) {
  { *t };
  { syscache_traits<T>::cache_id } -> std::same_as<const ::SysCacheIdentifier &>;
};

template <syscached T, convertible_into_datum... D> struct syscache {
  syscache(const D &...key) : syscache(syscache_traits<T>::cache_id, key...) {}
  syscache(::SysCacheIdentifier cache_id, const D &...key)
      requires(sizeof...(key) > 0 && sizeof...(key) < 5)
      : cache_id(cache_id), tuple([&]() {
          datum keys[4] = {datum_conversion<D>::into_datum(key)...};
          return ffi_guard{::SearchSysCache}(cache_id, keys[0], keys[1], keys[2], keys[3]);
        }()) {
    if (!HeapTupleIsValid(tuple)) {
      throw std::runtime_error("invalid tuple");
    }
  }

  ~syscache() { ReleaseSysCache(tuple); }

  decltype(*std::declval<T>()) &operator*() {
    return *reinterpret_cast<T>(GETSTRUCT(tuple.operator HeapTuple()));
  }
  const decltype(*std::declval<T>()) &operator*() const {
    return *reinterpret_cast<T>(GETSTRUCT(tuple.operator HeapTuple()));
  }

  /**
   * @brief Get an attribute by index
   *
   * @tparam V type to convert to
   * @param attr attribute index
   * @return
   */
  template <convertible_from_datum V> std::optional<V> get_attribute(int attr) {
    bool isnull;
    Datum ret = ffi_guard{::SysCacheGetAttr}(cache_id, tuple, attr, &isnull);
    if (isnull) {
      return std::nullopt;
    }
    return from_nullable_datum<V>(nullable_datum(ret), oid(/*FIXME*/ InvalidOid));
  }

  operator const heap_tuple &() const { return tuple; }

private:
  ::SysCacheIdentifier cache_id;
  heap_tuple tuple;
};

} // namespace cppgres
