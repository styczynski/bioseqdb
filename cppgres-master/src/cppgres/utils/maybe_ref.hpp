#pragma once
#include <functional>
#include <variant>

namespace cppgres::utils {

template <typename T> struct maybe_ref {
  // In-place constructor: constructs T inside the variant using forwarded arguments.
  template <typename... Args> maybe_ref(Args &&...args) : data(std::forward<Args>(args)...) {}
  maybe_ref(const T &value) : data(value) {}
  maybe_ref(T &ref) : data(std::ref(ref)) {}

  operator T &() {
    if (std::holds_alternative<T>(data))
      return std::get<T>(data);
    else
      return std::get<std::reference_wrapper<T>>(data).get();
  }

  operator const T &() const {
    if (std::holds_alternative<T>(data))
      return std::get<T>(data);
    else
      return std::get<std::reference_wrapper<T>>(data).get();
  }

  T *operator->() { return &operator T &(); }
  const T *operator->() const { return &operator const T &(); }

  bool is_ref() const { return !std::holds_alternative<T>(data); }

private:
  std::variant<T, std::reference_wrapper<T>> data;
};
} // namespace cppgres::utils
