#ifndef SQLGEN_TRANSPILATION_IS_DURATION_HPP_
#define SQLGEN_TRANSPILATION_IS_DURATION_HPP_

#include <chrono>

namespace sqlgen::transpilation {

template <class T>
class is_duration;

template <class T>
class is_duration : public std::false_type {};

template <class Rep, class Period>
class is_duration<std::chrono::duration<Rep, Period>> : public std::true_type {
};

template <class T>
constexpr bool is_duration_v = is_duration<std::remove_cvref_t<T>>();

}  // namespace sqlgen::transpilation

#endif
