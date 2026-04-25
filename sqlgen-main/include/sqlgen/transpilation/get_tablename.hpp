#ifndef SQLGEN_TRANSPILATION_GET_TABLENAME_HPP_
#define SQLGEN_TRANSPILATION_GET_TABLENAME_HPP_

#include <rfl.hpp>
#include <type_traits>

namespace sqlgen::transpilation {

namespace internal {

inline std::string remove_namespaces(const std::string& _str) {
  const auto pos = _str.find_last_of(':');
  if (pos == std::string::npos) {
    return _str;
  }
  return _str.substr(pos + 1);
}

}  // namespace internal

template <class T>
std::string get_tablename() noexcept {
  using Type = std::remove_cvref_t<T>;

  constexpr bool has_tablename = requires {
    { Type::tablename } -> std::convertible_to<std::string>;
  };

  constexpr bool has_viewname = requires {
    { Type::viewname } -> std::convertible_to<std::string>;
  };

  if constexpr (has_tablename) {
    static_assert(
        !has_viewname,
        "A struct can either have a tablename or a viewname, but not both.");
    return std::string(Type::tablename);

  } else if constexpr (has_viewname) {
    return std::string(Type::viewname);

  } else {
    return internal::remove_namespaces(rfl::type_name_t<Type>().str());
  }
}

}  // namespace sqlgen::transpilation

#endif
