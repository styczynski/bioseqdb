#ifndef SQLGEN_OFFSET_HPP_
#define SQLGEN_OFFSET_HPP_

#include "transpilation/Offset.hpp"

namespace sqlgen {

using Offset = transpilation::Offset;

inline auto offset(const size_t _val) { return Offset{_val}; };

}  // namespace sqlgen

#endif
