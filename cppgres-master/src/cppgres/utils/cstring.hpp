#include <string>
#include <string_view>

namespace cppgres::utils {

template <typename T>
concept is_cstring = std::same_as<T, char *> || std::same_as<T, const char *>;

template <typename T>
concept c_str_available = requires(T t) {
  { t.c_str() } -> std::same_as<const char *>;
};

template <typename T>
concept data_length_available = requires(T t) {
  { t.data() } -> std::same_as<const char *>;
  { t.length() } -> std::same_as<std::size_t>;
} && !c_str_available<T>;

template <typename T>
concept convertible_to_cstring = is_cstring<T> || c_str_available<T> || data_length_available<T>;

template <is_cstring S> const char *to_cstring(S string) {
  return const_cast<const char *>(string);
}

template <c_str_available S> const char *to_cstring(S &&string) { return string.c_str(); }

struct owned_cstring {
  owned_cstring(std::string s) : str_(s) {}

  operator const char *() const { return str_.c_str(); }

private:
  std::string str_;
};

template <data_length_available S> owned_cstring to_cstring(S &&string) {
  return std::string(string.data(), string.length());
}

} // namespace cppgres::utils
