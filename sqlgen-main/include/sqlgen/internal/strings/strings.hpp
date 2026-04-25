#ifndef SQLGEN_INTERNAL_STRINGS_STRINGS_HPP_
#define SQLGEN_INTERNAL_STRINGS_STRINGS_HPP_

#include <string>
#include <vector>

#include "../../sqlgen_api.hpp"

namespace sqlgen::internal::strings {

SQLGEN_API char to_lower(const char ch);

SQLGEN_API std::string to_lower(const std::string& _str);

SQLGEN_API char to_upper(const char ch);

SQLGEN_API std::string to_upper(const std::string& _str);

SQLGEN_API std::string join(const std::string& _delimiter,
                            const std::vector<std::string>& _strings);

SQLGEN_API std::string replace_all(const std::string& _str,
                                   const std::string& _from,
                                   const std::string& _to);

SQLGEN_API std::vector<std::string> split(const std::string& _str,
                                          const std::string& _delimiter);

SQLGEN_API std::string ltrim(const std::string& _str,
                             const std::string& _chars = " ");

SQLGEN_API std::string rtrim(const std::string& _str,
                             const std::string& _chars = " ");

inline std::string trim(const std::string& _str,
                        const std::string& _chars = " ") {
  return ltrim(rtrim(_str, _chars), _chars);
}

}  // namespace sqlgen::internal::strings

#endif
