#ifndef SQLGEN_TRANSPILATION_SET_HPP_
#define SQLGEN_TRANSPILATION_SET_HPP_

namespace sqlgen::transpilation {

/// Defines the SET clause in an UPDATE statement.
template <class _ColType, class T>
struct Set {
  using ColType = typename _ColType::ColType;
  T to;
};

}  // namespace sqlgen::transpilation

#endif
