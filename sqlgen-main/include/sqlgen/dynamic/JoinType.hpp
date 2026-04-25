#ifndef SQLGEN_DYNAMIC_JOINTYPE_HPP_
#define SQLGEN_DYNAMIC_JOINTYPE_HPP_

namespace sqlgen::dynamic {

enum class JoinType { inner_join, left_join, right_join, full_join };

}  // namespace sqlgen::dynamic

#endif
