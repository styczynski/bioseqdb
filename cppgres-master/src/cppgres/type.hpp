/**
* \file
 */
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include "datum.hpp"
#include "guard.hpp"
#include "imports.h"
#include "utils/utils.hpp"

namespace cppgres {

/**
 * @brief Postgres type
 */
struct type {
  ::Oid oid;

  /**
   * @brief Type name as defined in Postgres
   *
   * @param qualified if set to true (false by default), it will always include the schema name.
   *                  Otherwise, if the schema is in the `search_path`, the schema will not be
   *                  included.
   */
  std::string_view name(bool qualified = false) {
    if (!OidIsValid(oid)) {
      throw std::runtime_error("invalid type");
    }
    return (qualified ? ffi_guard{::format_type_be_qualified}
                      : ffi_guard{::format_type_be})(oid);
  }

  bool operator==(const type &other) const { return oid == other.oid; }
};

template <typename T, typename = void> struct type_traits {
  type_traits() {}
  type_traits(T &) {}
  bool is(const type &t) { return false; }
  type type_for() = delete;
};

template <typename T>
concept has_type_traits = requires(const type &t) {
  { type_traits<T>().type_for() } -> std::same_as<type>;
  { type_traits<T>().is(t) } -> std::same_as<bool>;
};

template <typename T> requires std::is_reference_v<T>
struct type_traits<T> {
  type_traits() {}
  type_traits(const T &) {}
  constexpr type type_for() { return type_traits<std::remove_reference_t<T>>().type_for(); }
};

template <typename T> struct type_traits<std::optional<T>> {
  type_traits() {}
  type_traits(const std::optional<T> &) {}
  bool is(const type &t) { return type_traits<T>().is(t); }
  constexpr type type_for() { return type_traits<T>().type_for(); }
};

struct non_by_value_type {
  friend struct datum;

  non_by_value_type(std::pair<const struct datum &, std::optional<memory_context>> init)
      : non_by_value_type(init.first, init.second) {}
  non_by_value_type(const struct datum &datum, std::optional<memory_context> ctx)
      : value_datum(datum),
        ctx(tracking_memory_context(ctx.has_value() ? *ctx : top_memory_context())) {}

  non_by_value_type(const non_by_value_type &other)
      : value_datum(other.value_datum), ctx(other.ctx) {}
  non_by_value_type(non_by_value_type &&other) noexcept
      : value_datum(std::move(other.value_datum)), ctx(std::move(other.ctx)) {}
  non_by_value_type &operator=(non_by_value_type &&other) noexcept {
    value_datum = std::move(other.value_datum);
    ctx = std::move(other.ctx);
    return *this;
  }

  memory_context &get_memory_context() { return ctx.get_memory_context(); }

  datum get_datum() const { return value_datum; }

protected:
  datum value_datum;
  tracking_memory_context<memory_context> ctx;
  void *ptr(bool tracked = true) const {
    if (tracked && ctx.resets() > 0) {
      throw pointer_gone_exception();
    }
    return reinterpret_cast<void *>(value_datum.operator const ::Datum &());
  }
};

static_assert(std::copy_constructible<non_by_value_type>);

struct varlena : public non_by_value_type {
  using non_by_value_type::non_by_value_type;

  operator void *() { return VARDATA_ANY(detoasted_ptr()); }

  datum get_datum() const { return value_datum; }

  bool is_detoasted() const { return detoasted != nullptr; }

protected:
  void *detoasted = nullptr;
  std::optional<tracking_memory_context<memory_context>> detoasted_ctx;
  void *detoasted_ptr() {
    if (detoasted != nullptr) {
      if (detoasted_ctx.has_value() && detoasted_ctx->resets() > 0) {
        throw pointer_gone_exception();
      }
      return detoasted;
    }
    auto *source = reinterpret_cast<::varlena *>(ptr());
    detoasted = ffi_guard{::pg_detoast_datum}(source);
    if (detoasted == source) {
      detoasted_ctx = ctx;
    } else {
      detoasted_ctx.emplace(memory_context());
    }
    return detoasted;
  }
};

struct text : public varlena {
  using varlena::varlena;

  operator std::string_view() {
    return {static_cast<char *>(this->operator void *()), VARSIZE_ANY_EXHDR(this->detoasted_ptr())};
  }
};

using byte_array = std::span<const std::byte>;

struct bytea : public varlena {
  using varlena::varlena;

  bytea(const byte_array &ba, memory_context ctx)
      : varlena(([&]() {
                  auto alloc = ctx.alloc<std::byte>(VARHDRSZ + ba.size_bytes());
                  SET_VARSIZE(alloc, VARHDRSZ + ba.size_bytes());
                  auto ptr = VARDATA_ANY(alloc);
                  std::copy(ba.begin(), ba.end(), reinterpret_cast<std::byte *>(ptr));
                  return datum(PointerGetDatum(alloc));
                })(),
                ctx) {}

  operator byte_array() {
    return {reinterpret_cast<std::byte *>(this->operator void *()),
            VARSIZE_ANY_EXHDR(this->detoasted_ptr())};
  }
};

template <typename T>
concept flattenable = requires(T t, std::span<std::byte> span) {
  { T::type() } -> std::same_as<type>;
  { t.flat_size() } -> std::same_as<std::size_t>;
  { t.flatten_into(span) };
  { T::restore_from(span) } -> std::same_as<T>;
};

template <flattenable T> struct expanded_varlena : public varlena {
  using flattenable_type = T;
  using varlena::varlena;

  expanded_varlena()
      : varlena(allocate_expanded()),
        detoasted_value(reinterpret_cast<expanded *>(DatumGetPointer(value_datum))) {}

  template <typename... Args>
  explicit expanded_varlena(Args &&...args)
      // avoid being copy/move constructor
      requires(sizeof...(Args) > 0 &&
               !(sizeof...(Args) == 1 &&
                 std::is_same_v<std::decay_t<std::tuple_element_t<0, std::tuple<Args...>>>,
                                expanded_varlena>))
      : varlena(allocate_expanded(args...)),
        detoasted_value(reinterpret_cast<expanded *>(DatumGetPointer(value_datum))) {}

  operator T &() {
    if (detoasted_value.has_value()) {
      return detoasted_value.value()->inner;
    } else {
      if (VARATT_IS_EXTERNAL_EXPANDED(ptr())) {
        detoasted_value = reinterpret_cast<expanded *>(DatumGetEOHP(value_datum));
        return detoasted_value.value()->inner;
      }
      auto *ptr1 = reinterpret_cast<std::byte *>(varlena::operator void *());
      auto ctx = memory_context(std::move(alloc_set_memory_context()));
      auto *value = new (ctx.alloc<expanded>())
          expanded(T::restore_from(std::span(ptr1, VARSIZE_ANY_EXHDR(detoasted_ptr()))));
      ctx.register_reset_callback(
          [](void *arg) {
            auto v = reinterpret_cast<expanded *>(arg);
            v->inner.~T();
          },
          value);
      init(&value->hdr, ctx);
      detoasted_value = value;
      return value->inner;
    }
  }

  datum get_expanded_datum() const {
    if (!detoasted_value.has_value()) {
      throw std::runtime_error("hasn't been expanded yet");
    }
    return datum(EOHPGetRWDatum(&detoasted_value.value()->hdr));
  }

private:
  struct expanded {
    expanded(auto &&...args) : inner(std::forward<decltype(args)...>(args)...) {}
    ::ExpandedObjectHeader hdr;
    T inner;
  };

  template <typename... Args> static auto allocate_expanded(Args &&...args) {
    auto ctx = memory_context(std::move(alloc_set_memory_context()));
    return ctx([&]() {
      auto *e = ctx.alloc<expanded>();
      std::construct_at(e, args...);
      init(&e->hdr, ctx);
      ctx.register_reset_callback(
          [](void *arg) {
            auto v = reinterpret_cast<expanded *>(arg);
            v->inner.~T();
          },
          e);
      return std::make_pair(datum(PointerGetDatum(e)), ctx);
    });
  }

  std::optional<expanded *> detoasted_value = std::nullopt;

  static void init(ExpandedObjectHeader *hdr, memory_context &ctx) {
    using header = int32_t;

    static const ::ExpandedObjectMethods eom = {
        .get_flat_size =
            [](ExpandedObjectHeader *eohptr) {
              auto *e = reinterpret_cast<expanded *>(eohptr);
              T *inner = &e->inner;
              return inner->flat_size() + sizeof(header);
            },
        .flatten_into =
            [](ExpandedObjectHeader *eohptr, void *result, size_t allocated_size) {
              auto *e = reinterpret_cast<expanded *>(eohptr);
              T *inner = &e->inner;
              SET_VARSIZE(reinterpret_cast<header *>(result), allocated_size);
              auto bytes = reinterpret_cast<std::byte *>(result) + sizeof(header);
              std::span buffer(bytes, allocated_size - sizeof(header));
              inner->flatten_into(buffer);
            }};

    ffi_guard{::EOH_init_header}(hdr, &eom, ctx);
  }
};

template <typename T>
concept expanded_varlena_type = requires { typename T::flattenable_type; };

template <typename T>
concept has_a_type = requires(type_traits<T> t) {
  { t.type_for() } -> std::same_as<type>;
};

} // namespace cppgres
