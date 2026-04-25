#ifndef SQLGEN_RESULT_HPP_
#define SQLGEN_RESULT_HPP_

#include <rfl.hpp>
#include <string>

namespace sqlgen {

using Error = rfl::Error;

using Nothing = rfl::Nothing;

template <class T>
using Result = rfl::Result<T>;

inline auto error(const std::string& _msg) { return rfl::error(_msg); }

};  // namespace sqlgen

#endif

