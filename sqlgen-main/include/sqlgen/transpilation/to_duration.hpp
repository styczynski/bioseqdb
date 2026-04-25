#ifndef SQLGEN_TRANSPILATION_TO_DURATION_HPP_
#define SQLGEN_TRANSPILATION_TO_DURATION_HPP_

#include <chrono>
#include <rfl.hpp>
#include <type_traits>

#include "../dynamic/TimeUnit.hpp"
#include "../dynamic/Value.hpp"

namespace sqlgen::transpilation {

template <class DurationType>
struct ToDuration {
  dynamic::Duration operator()(const DurationType& _t) {
    if constexpr (std::is_same_v<DurationType, std::chrono::milliseconds>) {
      return dynamic::Duration{.unit = dynamic::TimeUnit::milliseconds,
                               .val = static_cast<int64_t>(_t.count())};

    } else if constexpr (std::is_same_v<DurationType, std::chrono::seconds>) {
      return dynamic::Duration{.unit = dynamic::TimeUnit::seconds,
                               .val = static_cast<int64_t>(_t.count())};

    } else if constexpr (std::is_same_v<DurationType, std::chrono::minutes>) {
      return dynamic::Duration{.unit = dynamic::TimeUnit::minutes,
                               .val = static_cast<int64_t>(_t.count())};

    } else if constexpr (std::is_same_v<DurationType, std::chrono::hours>) {
      return dynamic::Duration{.unit = dynamic::TimeUnit::hours,
                               .val = static_cast<int64_t>(_t.count())};

    } else if constexpr (std::is_same_v<DurationType, std::chrono::days>) {
      return dynamic::Duration{.unit = dynamic::TimeUnit::days,
                               .val = static_cast<int64_t>(_t.count())};

    } else if constexpr (std::is_same_v<DurationType, std::chrono::weeks>) {
      return dynamic::Duration{.unit = dynamic::TimeUnit::weeks,
                               .val = static_cast<int64_t>(_t.count())};

    } else if constexpr (std::is_same_v<DurationType, std::chrono::months>) {
      return dynamic::Duration{.unit = dynamic::TimeUnit::months,
                               .val = static_cast<int64_t>(_t.count())};

    } else if constexpr (std::is_same_v<DurationType, std::chrono::years>) {
      return dynamic::Duration{.unit = dynamic::TimeUnit::years,
                               .val = static_cast<int64_t>(_t.count())};

    } else {
      static_assert(rfl::always_false_v<DurationType>, "Unsupported type.");
    }
  }
};

template <class T>
auto to_duration(const T& _t) {
  return ToDuration<std::remove_cvref_t<T>>{}(_t);
}

}  // namespace sqlgen::transpilation

#endif
