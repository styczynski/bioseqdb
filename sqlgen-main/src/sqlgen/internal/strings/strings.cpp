#include "sqlgen/internal/strings/strings.hpp"

#include <algorithm>

namespace sqlgen::internal::strings {

char to_lower(const char ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return ch + ('a' - 'A');
  } else {
    return ch;
  }
}

std::string to_lower(const std::string& _str) {
  auto str = _str;
  for (char& ch : str) {
    ch = to_lower(ch);
  }
  return str;
}

char to_upper(const char ch) {
  if (ch >= 'a' && ch <= 'z') {
    return ch + ('A' - 'a');
  } else {
    return ch;
  }
}

std::string to_upper(const std::string& _str) {
  auto str = _str;
  for (char& ch : str) {
    ch = to_upper(ch);
  }
  return str;
}

std::string join(const std::string& _delimiter,
                 const std::vector<std::string>& _strings) {
  if (_strings.size() == 0) {
    return "";
  }
  auto res = _strings[0];
  for (size_t i = 1; i < _strings.size(); ++i) {
    res += _delimiter + _strings[i];
  }
  return res;
}

std::string replace_all(const std::string& _str, const std::string& _from,
                        const std::string& _to) {
  auto str = _str;

  size_t pos = 0;
  while ((pos = str.find(_from, pos)) != std::string::npos) {
    str.replace(pos, _from.length(), _to);
    pos += _to.length();
  }
  return str;
}

std::vector<std::string> split(const std::string& _str,
                               const std::string& _delimiter) {
  auto str = _str;
  size_t pos = 0;
  std::vector<std::string> result;
  while ((pos = str.find(_delimiter)) != std::string::npos) {
    result.emplace_back(str.substr(0, pos));
    str.erase(0, pos + _delimiter.length());
  }
  result.emplace_back(std::move(str));
  return result;
}

std::string ltrim(const std::string& _str, const std::string& _chars) {
  auto str = _str;
  str.erase(str.begin(), std::find_if(str.begin(), str.end(), [&](char ch1) {
              return std::none_of(_chars.begin(), _chars.end(),
                                  [&](char ch2) { return ch1 == ch2; });
            }));
  return str;
}

std::string rtrim(const std::string& _str, const std::string& _chars) {
  auto str = _str;
  str.erase(std::find_if(str.rbegin(), str.rend(),
                         [&](char ch1) {
                           return std::none_of(
                               _chars.begin(), _chars.end(),
                               [&](char ch2) { return ch1 == ch2; });
                         })
                .base(),
            str.end());
  return str;
}

}  // namespace sqlgen::internal::strings
