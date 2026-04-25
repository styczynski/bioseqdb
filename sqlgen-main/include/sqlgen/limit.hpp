#ifndef SQLGEN_LIMIT_HPP_
#define SQLGEN_LIMIT_HPP_

#include "transpilation/Limit.hpp"

namespace sqlgen {

using Limit = transpilation::Limit;

inline auto limit(const size_t _val) { return Limit{_val}; };

}  // namespace sqlgen

#endif
