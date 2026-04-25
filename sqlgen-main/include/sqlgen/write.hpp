#ifndef SQLGEN_WRITE_HPP_
#define SQLGEN_WRITE_HPP_

#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <rfl.hpp>
#include <string>
#include <type_traits>
#include <vector>

#include "Ref.hpp"
#include "Result.hpp"
#include "dynamic/Write.hpp"
#include "internal/batch_size.hpp"
#include "internal/to_str_vec.hpp"
#include "is_connection.hpp"
#include "transpilation/to_create_table.hpp"
#include "transpilation/to_insert_or_write.hpp"

namespace sqlgen {

template <class ItBegin, class ItEnd, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> write(const Ref<Connection>& _conn, ItBegin _begin,
                              ItEnd _end) noexcept {
  using T =
      std::remove_cvref_t<typename std::iterator_traits<ItBegin>::value_type>;

  const auto start_write = [&](const auto&) -> Result<Nothing> {
    const auto write_stmt =
        transpilation::to_insert_or_write<T, dynamic::Write>();
    return _conn->start_write(write_stmt);
  };

  const auto write = [&](const auto&) -> Result<Nothing> {
    const auto res = _conn->write(_begin, _end);
    if (!res) {
      _conn->end_write();
    }
    return res;
  };

  const auto end_write = [&](const auto&) -> Result<Nothing> {
    return _conn->end_write();
  };

  const auto create_table_stmt = transpilation::to_create_table<T>();

  return _conn->execute(_conn->to_sql(create_table_stmt))
      .and_then(start_write)
      .and_then(write)
      .and_then(end_write)
      .transform([&](const auto&) { return _conn; });
}

template <class ItBegin, class ItEnd, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> write(const Result<Ref<Connection>>& _res,
                              ItBegin _begin, ItEnd _end) noexcept {
  return _res.and_then(
      [&](const auto& _conn) { return write(_conn, _begin, _end); });
}

template <class ContainerType>
auto write(const auto& _conn, const ContainerType& _container) noexcept {
  if constexpr (std::ranges::input_range<std::remove_cvref_t<ContainerType>>) {
    return write(_conn, _container.begin(), _container.end());
  } else {
    return write(_conn, &_container, &_container + 1);
  }
}

template <class ContainerType>
auto write(const auto& _conn,
           const std::reference_wrapper<ContainerType>& _data) {
  return write(_conn, _data.get());
}

template <class ContainerType>
struct Write {
  auto operator()(const auto& _conn) const { return write(_conn, data_); }

  ContainerType data_;
};

template <class ContainerType>
Write<ContainerType> write(const ContainerType& _data) {
  return Write<ContainerType>{.data_ = _data};
}

}  // namespace sqlgen

#endif
