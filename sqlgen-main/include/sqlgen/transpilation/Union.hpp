#ifndef SQLGEN_TRANSPILATION_UNION_HPP_
#define SQLGEN_TRANSPILATION_UNION_HPP_

#include <string>
#include <type_traits>

namespace sqlgen::transpilation {

template <class... SelectTs>
struct Union {};

}  // namespace sqlgen::transpilation

#endif
