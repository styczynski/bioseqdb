#ifndef SQLGEN_PARSING_PARSER_DEFAULT_HPP_
#define SQLGEN_PARSING_PARSER_DEFAULT_HPP_

#include <ranges>
#include <rfl.hpp>
#include <string>
#include <type_traits>

#include "../Result.hpp"
#include "../dynamic/Type.hpp"
#include "../dynamic/types.hpp"
#include "../transpilation/has_reflection_method.hpp"
#include "Parser_base.hpp"

namespace sqlgen::parsing {

template <class T>
struct Parser {
  using Type = std::remove_cvref_t<T>;

  static Result<T> read(const std::optional<std::string>& _str) noexcept {
    if constexpr (transpilation::has_reflection_method<Type>) {
      return Parser<std::remove_cvref_t<typename Type::ReflectionType>>::read(
                 _str)
          .transform([](auto&& _t) { return Type(std::move(_t)); });

    } else {
      if (!_str) {
        return error("NULL value encounted: Numeric value cannot be NULL.");
      }

      try {
        if constexpr (std::is_floating_point_v<Type>) {
          return static_cast<Type>(std::stod(*_str));

        } else if constexpr (std::is_same_v<Type, bool>) {
          if (*_str == "t" || *_str == "T" || *_str == "true" ||
              *_str == "TRUE") {
            return true;
          }
          if (*_str == "f" || *_str == "F" || *_str == "false" ||
              *_str == "FALSE") {
            return false;
          }
          return std::stoi(*_str) != 0;

        } else if constexpr (std::is_integral_v<Type>) {
          return static_cast<Type>(std::stoll(*_str));

        } else if constexpr (std::is_enum_v<Type>) {
          if (auto res = rfl::string_to_enum<Type>(*_str)) {
            return Type{*res};
          } else {
            return error(res.error());
          }

        } else {
          static_assert(rfl::always_false_v<Type>, "Unsupported type");
        }

      } catch (std::exception& e) {
        return error(e.what());
      }
    }
  }

  static std::optional<std::string> write(const T& _t) noexcept {
    if constexpr (transpilation::has_reflection_method<Type>) {
      return Parser<std::remove_cvref_t<typename Type::ReflectionType>>::write(
          _t.reflection());
    } else if constexpr (std::is_enum_v<Type>) {
      return rfl::enum_to_string(_t);
    } else {
      return std::to_string(_t);
    }
  }

  static dynamic::Type to_type() noexcept {
    if constexpr (transpilation::has_reflection_method<Type>) {
      return Parser<
          std::remove_cvref_t<typename Type::ReflectionType>>::to_type();

    } else if constexpr (std::is_same_v<T, bool>) {
      return dynamic::types::Boolean{};

    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
      if constexpr (sizeof(T) == 1) {
        return dynamic::types::Int8{};

      } else if constexpr (sizeof(T) == 2) {
        return dynamic::types::Int16{};

      } else if constexpr (sizeof(T) == 4) {
        return dynamic::types::Int32{};

      } else if constexpr (sizeof(T) == 8) {
        return dynamic::types::Int64{};

      } else {
        static_assert(rfl::always_false_v<T>, "Unsupported signed integer.");
      }

    } else if constexpr (std::is_integral_v<T> && !std::is_signed_v<T>) {
      if constexpr (sizeof(T) == 1) {
        return dynamic::types::UInt8{};

      } else if constexpr (sizeof(T) == 2) {
        return dynamic::types::UInt16{};

      } else if constexpr (sizeof(T) == 4) {
        return dynamic::types::UInt32{};

      } else if constexpr (sizeof(T) == 8) {
        return dynamic::types::UInt64{};

      } else {
        static_assert(rfl::always_false_v<T>, "Unsupported unsigned integer.");
      }

    } else if constexpr (std::is_floating_point_v<T>) {
      if constexpr (sizeof(T) == 4) {
        return dynamic::types::Float32{};

      } else if constexpr (sizeof(T) == 8) {
        return dynamic::types::Float64{};

      } else {
        static_assert(rfl::always_false_v<T>,
                      "Unsupported floating point value.");
      }

    } else if constexpr (std::is_enum_v<T>) {
      constexpr auto values = rfl::get_enumerator_array<T>();
      std::array<std::string_view, std::size(values)> enum_names{};
      for (std::size_t i = 0; i < std::size(values); ++i) {
        enum_names[i] = values[i].first;
      }
      constexpr auto org_name = rfl::internal::get_type_name_str_view<T>();
      static_assert(org_name.size() < 64,
                    "Enum type exceeds type level. If it's defined in a nested "
                    "namespace, consider moving it up to a higher level.");
      // Transform '::' to '__' to avoid postgres limitations
      std::string trf_name(org_name);
      std::ranges::replace(trf_name, ':', '_');
      return sqlgen::dynamic::types::Enum{
          std::move(trf_name),
          std::vector<std::string>(enum_names.begin(), enum_names.end())};
    } else {
      static_assert(rfl::always_false_v<T>, "Unsupported type.");
    }
  }
};

}  // namespace sqlgen::parsing

#endif
