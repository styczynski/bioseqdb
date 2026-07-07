#pragma once

#include "imports.h"
#include "name.hpp"
#include "syscache.hpp"
#include "type.hpp"
#include "types.hpp"

#include <algorithm>
#include <memory>
#include <ranges>
#include <vector>

namespace cppgres {

/**
 * @brief Tuple descriptor operator
 *
 * Allows to create new or manipulate existing tuple descriptors
 */
struct tuple_descriptor {

  /**
   * @brief Create a tuple descriptor for a given number of attributes
   *
   * @param nattrs number of attributes
   * @param ctx memory context, current transaction context by default
   */
  tuple_descriptor(int nattrs, memory_context ctx = memory_context())
      : tupdesc(([&]() {
          memory_context_scope<memory_context> scope(ctx);
          auto res = ffi_guard{::CreateTemplateTupleDesc}(nattrs);
          return res;
        }())),
        blessed(false), owned(true) {
    for (int i = 0; i < nattrs; i++) {
      operator[](i).attcollation = InvalidOid;
      operator[](i).attisdropped = false;
    }
  }
  /**
   * @brief Create a tuple descriptor for a given `TupleDesc`
   *
   * @param tupdesc existing attribute
   * @param blessed true if already blessed (default)
   */
  tuple_descriptor(TupleDesc tupdesc, bool blessed = true)
      : tupdesc(tupdesc), blessed(blessed), owned(false) {}

  /**
   * @brief Create a tuple description for a given composite type
   */
  tuple_descriptor(type t)
      : tuple_descriptor([&]() {
          syscache<Form_pg_type, oid> syscache(t.oid);
          if ((*syscache).typtype != TYPTYPE_COMPOSITE) {
            throw std::invalid_argument("not a composite type");
          }
          return ffi_guard{::lookup_rowtype_tupdesc_copy}(t.oid, (*syscache).typtypmod);
        }()) {}

  /**
   * @brief Copy constructor
   *
   * Creates a copy instance of the tuple descriptor in the current memory contet
   */
  tuple_descriptor(tuple_descriptor &other)
      : tupdesc(ffi_guard{::CreateTupleDescCopyConstr}(other.populate_compact_attribute())),
        blessed(other.blessed), owned(other.owned) {}

  /**
   * @brief Move constructor
   */
  tuple_descriptor(tuple_descriptor &&other) noexcept
      : tupdesc(other.tupdesc), blessed(other.blessed), owned(other.owned) {}

  /**
   * @brief Copy assignment
   *
   * Creates a copy instance of the tuple descriptor in the current memory contet
   */
  tuple_descriptor &operator=(const tuple_descriptor &other) {
    tupdesc = ffi_guard{::CreateTupleDescCopyConstr}(other.populate_compact_attribute());
    blessed = other.blessed;
    return *this;
  }

  /**
   * @brief Move assignment
   */
  tuple_descriptor &operator=(tuple_descriptor &&other) noexcept {
    if (this != &other) {
      tupdesc = other.tupdesc;
      blessed = other.blessed;
      owned = other.owned;
    }
    return *this;
  }

  /**
   * @brief Number of attributes
   */
  int attributes() const noexcept { return tupdesc->natts; }

  /**
   * @brief Get a reference to `Form_pg_attribute`
   *
   * @throws std::out_of_range when attribute index is out of range
   */
  ::FormData_pg_attribute &operator[](int n) const {
    check_bounds(n);
    return *TupleDescAttr(tupdesc, n);
  }

  type get_type(int n) const {
    auto &att = operator[](n);
    return {att.atttypid};
  }

  /**
   * @brief Set attribute type
   *
   * @param n Zero-based attribute index
   * @param type new attribute type
   *
   * @throws std::out_of_range when attribute index is out of range
   */
  void set_type(int n, const type &type) {
    check_not_blessed();
    syscache<Form_pg_type, oid> typ(type.oid);
    auto &att = operator[](n);
    att.atttypid = (*typ).oid;
    att.attcollation = (*typ).typcollation;
    att.attlen = (*typ).typlen;
    att.attstorage = (*typ).typstorage;
    att.attalign = (*typ).typalign;
    att.atttypmod = (*typ).typtypmod;
    att.attbyval = (*typ).typbyval;
  }

  std::string_view get_name(int n) {
    auto &att = operator[](n);
    return NameStr(att.attname);
  }

  /**
   * @brief Set attribute name
   *
   * @param n Zero-based attribute index
   * @param name new attribute name
   *
   * @throws std::out_of_range when attribute index is out of range
   */
  void set_name(int n, const name &name) {
    check_not_blessed();
    auto &att = operator[](n);
    att.attname = name;
  }

  /**
   * @brief returns a pointer to `TupleDesc`
   *
   * At this point, it'll be prepared and blessed.
   */
  operator TupleDesc() {
    populate_compact_attribute();
    if (!blessed) {
      tupdesc = ffi_guard{::BlessTupleDesc}(tupdesc);
      blessed = true;
    }
    return tupdesc;
  }

#if PG_MAJORVERSION_NUM > 16
  /**
   * @brief Determines whether two tuple descriptors have equal row types.
   *
   * This is used to check whether two record types are compatible, whether
   * function return row types are the same, and other similar situations.
   */
  bool equal_row_types(const tuple_descriptor &other) {
    return ffi_guard{::equalRowTypes}(tupdesc, other.tupdesc);
  }
#endif

  /**
   * @brief Determines whether two tuple descriptors have equal row types.
   *
   * This is used to check whether two record types are compatible, whether
   * function return row types are the same, and other similar situations.
   */
  bool equal_types(const tuple_descriptor &other) {
    if (tupdesc->natts != other.tupdesc->natts)
      return false;
    if (tupdesc->tdtypeid != other.tupdesc->tdtypeid)
      return false;

    for (int i = 0; i < other.attributes(); i++) {
      FormData_pg_attribute &att1 = operator[](i);
      FormData_pg_attribute &att2 = other[i];

      if (att1.atttypid != att2.atttypid || att1.atttypmod != att2.atttypmod ||
          att1.attcollation != att2.attcollation || att1.attisdropped != att2.attisdropped ||
          att1.attlen != att2.attlen || att1.attalign != att2.attalign) {
        return false;
      }
    }

    return true;
  }

  /**
   * @brief Compare two TupleDesc structures for logical equality
   *
   * @note This includes checking attribute names.
   */
  bool operator==(const tuple_descriptor &other) {
    return ffi_guard{::equalTupleDescs}(tupdesc, other.tupdesc);
  }

  /**
   * @brief Returns true if the tuple descriptor is blessed
   */
  bool is_blessed() const noexcept { return blessed; }

  operator TupleDesc() const { return tupdesc; }

private:
  inline void check_bounds(int n) const {
    if (n + 1 > tupdesc->natts || n < 0) {
      throw std::out_of_range(cppgres::fmt::format(
          "attribute index {} is out of bounds for the tuple descriptor with the size of {}", n,
          tupdesc->natts));
    }
  }
  inline void check_not_blessed() const {
    if (blessed) {
      throw std::runtime_error("tuple_descriptor already blessed");
    }
  }

  TupleDesc populate_compact_attribute() const {
#if PG_MAJORVERSION_NUM >= 18
    for (int i = 0; i < tupdesc->natts; i++) {
      ffi_guard{::populate_compact_attribute}(tupdesc, i);
    }
#endif
    return tupdesc;
  }

  TupleDesc tupdesc;
  bool blessed;
  bool owned;
};

static_assert(std::copy_constructible<tuple_descriptor>);
static_assert(std::move_constructible<tuple_descriptor>);
static_assert(std::is_copy_assignable_v<tuple_descriptor>);

/**
 * @brief Runtime-typed value of `record` type
 *
 * These records don't have a structure known upfront, for example, when a value
 * of this type (`record`) are passed into a function.
 */
struct record {

  friend struct datum_conversion<record>;

  record(HeapTupleHeader heap_tuple, abstract_memory_context &ctx)
      : tupdesc(/* FIXME: can we use the non-copy version with refcounting? */ ffi_guard{
            ::lookup_rowtype_tupdesc_copy}(HeapTupleHeaderGetTypeId(heap_tuple),
                                           HeapTupleHeaderGetTypMod(heap_tuple))),
        tuple(ctx.template alloc<HeapTupleData>()) {
#if PG_MAJORVERSION_NUM < 18
    tuple->t_len = HeapTupleHeaderGetDatumLength(tupdesc.operator TupleDesc());
#else
    tuple->t_len = HeapTupleHeaderGetDatumLength(heap_tuple);
#endif
    tuple->t_data = heap_tuple;
  }
  record(HeapTupleHeader heap_tuple, abstract_memory_context &&ctx) : record(heap_tuple, ctx) {}

  template <std::input_iterator Iter>
  requires convertible_into_nullable_datum<typename std::iterator_traits<Iter>::value_type>
  record(tuple_descriptor &tupdesc, Iter begin, Iter end)
      : tupdesc(tupdesc), tuple([&]() {
          std::vector<::Datum> values;
          std::vector<bool> null_flags;
          for (auto it = begin; it != end; ++it) {
            auto nd = into_nullable_datum(*it);
            values.push_back(nd.is_null() ? ::Datum(0) : nd);
            null_flags.push_back(nd.is_null());
          }
          auto nulls = std::make_unique<bool[]>(null_flags.size());
          std::ranges::copy(null_flags, nulls.get());
          return ffi_guard{::heap_form_tuple}(this->tupdesc, values.data(), nulls.get());
        }()) {}

  template <convertible_into_nullable_datum... D>
  record(tuple_descriptor &tupdesc, D &&...args)
      : tupdesc(tupdesc), tuple([&]() {
          std::array<nullable_datum, sizeof...(D)> datums = {
              cppgres::into_nullable_datum(std::move(args))...};
          std::array<bool, sizeof...(D)> nulls;
          std::ranges::copy(datums | std::views::transform([](auto &v) { return v.is_null(); }),
                            nulls.begin());
          std::array<::Datum, sizeof...(D)> values;
          std::ranges::copy(
              datums | std::views::transform([](auto &v) { return v.is_null() ? ::Datum(0) : v; }),
              values.begin());
          return ffi_guard{::heap_form_tuple}(this->tupdesc, values.data(), nulls.data());
        }()) {}

  /**
   * @brief Number of attributes in the record
   */
  int attributes() const { return tupdesc.operator TupleDesc()->natts; }

  /**
   * @brief Type of attribute using a 0-based index
   *
   * @throws std::out_of_range
   */
  type attribute_type(int n) const {
    check_bounds(n);
    return {.oid = TupleDescAttr(tupdesc.operator TupleDesc(), n)->atttypid};
  }

  /**
   * @brief Name of attribute using a 0-based index
   *
   * @throws std::out_of_range
   */
  std::string_view attribute_name(int n) const {
    check_bounds(n);
    return {NameStr(TupleDescAttr(tupdesc.operator TupleDesc(), n)->attname)};
  }

  /**
   * @brief Get attribute value (datum) using a 0-based index
   *
   * @throws std::out_of_range
   */
  nullable_datum get_attribute(int n) {
    bool isnull;
    check_bounds(n);
    auto _heap_getattr = ffi_guard{
#if PG_MAJORVERSION_NUM < 15
        // Handle the fact that it is a macro
        [](::HeapTuple tup, int attnum, ::TupleDesc tupleDesc, bool *isnull) {
          return heap_getattr(tup, attnum, tupleDesc, isnull);
        }
#else
        ::heap_getattr
#endif
    };
    datum d(_heap_getattr(tuple, n + 1, tupdesc.operator TupleDesc(), &isnull));
    return isnull ? nullable_datum() : nullable_datum(d);
  }

  /**
   * @brief Get attribute by name
   *
   * @throws std::out_of_range
   */
  nullable_datum operator[](std::string_view name) {
    for (int i = 0; i < attributes(); i++) {
      if (attribute_name(i) == name) {
        return get_attribute(i);
      }
    }
    throw std::out_of_range(cppgres::fmt::format("no attribute by the name of {}", name));
  }

  /**
   * @brief Get attribute by 0-based index
   *
   * @throws std::out_of_range
   */
  nullable_datum operator[](int n) { return get_attribute(n); }

  operator HeapTuple() const noexcept { return tuple; }
  operator TupleDesc() const noexcept { return tupdesc; }

  /**
   * @brief Returns tuple descriptor
   */
  tuple_descriptor get_tuple_descriptor() const noexcept { return tupdesc; }

  record(const record &other) : tupdesc(other.tupdesc), tuple(other.tuple) {}
  record(record &&other) noexcept : tupdesc(std::move(other.tupdesc)), tuple(other.tuple) {}
  record &operator=(const record &other) {
    tupdesc = other.tupdesc;
    tuple = other.tuple;
    return *this;
  }

  record &operator=(record &&other) noexcept {
    if (this != &other) {
      tupdesc = std::move(other.tupdesc);
      tuple = other.tuple;
    }
    return *this;
  }

private:
  inline void check_bounds(int n) const {
    if (n + 1 > attributes() || n < 0) {
      throw std::out_of_range(cppgres::fmt::format(
          "attribute index {} is out of bounds for record with the size of {}", n, attributes()));
    }
  }

  tuple_descriptor tupdesc;
  HeapTuple tuple;
};
static_assert(std::copy_constructible<record>);
static_assert(std::move_constructible<record>);
static_assert(std::is_copy_assignable_v<record>);

template <> struct datum_conversion<record> : default_datum_conversion<record> {
  static record from_datum(const datum &d, oid, std::optional<memory_context> ctx) {
    return {reinterpret_cast<HeapTupleHeader>(ffi_guard{::pg_detoast_datum}(
                reinterpret_cast<struct ::varlena *>(d.operator const ::Datum &()))),
            ctx.has_value() ? ctx.value() : memory_context()};
  }

  static datum into_datum(const record &t) { return datum(PointerGetDatum(t.tuple)); }
};

template <> struct type_traits<record> {
  bool is(const type &t) {
    if (t.oid == RECORDOID)
      return true;
    // Check if it is a composite type and therefore can be coerced to a record
    syscache<Form_pg_type, oid> cache(t.oid);
    return (*cache).typtype == 'c';
  }
  constexpr type type_for() { return {.oid = RECORDOID}; }
};

template <typename T>
concept composite_type = requires {
  { T::composite_type() } -> std::same_as<type>;
};

template <composite_type T> struct datum_conversion<T> : default_datum_conversion<T> {
  static T from_datum(const datum &d, oid oid_, std::optional<memory_context> ctx) {
    if (oid_ != T::composite_type().oid) {
      throw std::runtime_error(fmt::format("invalid type: expected composite type {} got {}",
                                           T::composite_type().name(), type{.oid = oid_}.name()));
    }
    auto mctx = ctx.has_value() ? ctx.value() : memory_context();
    record rec{reinterpret_cast<HeapTupleHeader>(ffi_guard{::pg_detoast_datum}(
                   reinterpret_cast<struct ::varlena *>(d.operator const ::Datum &()))),
               mctx};
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      return T{([&] {
        return from_nullable_datum<utils::tuple_element_t<Is, T>>(rec.get_attribute(Is),
                                                                  rec.attribute_type(Is).oid, mctx);
      }())...};
    }(std::make_index_sequence<utils::tuple_size_v<T>>{});
  }

  static datum into_datum(const T &t) {
    auto res = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      tuple_descriptor td(T::composite_type());
      record rec{td, utils::get<Is>(t)...};
      return datum(ffi_guard{::heap_copy_tuple_as_datum}(rec, rec));
    }(std::make_index_sequence<utils::tuple_size_v<T>>{});
    return res;
  }
};
template <composite_type T> struct type_traits<T> {
  bool is(const type &t) {
    if (t == T::composite_type())
      return true;
    // Check if it is a composite type and therefore can be coerced to a record
    syscache<Form_pg_type, oid> cache(t.oid);
    return (*cache).typtype == 'c';
  }
  constexpr type type_for() { return T::composite_type(); }
};

} // namespace cppgres
