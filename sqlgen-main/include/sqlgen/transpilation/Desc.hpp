#ifndef SQLGEN_TRANSPILATION_DESC_HPP_
#define SQLGEN_TRANSPILATION_DESC_HPP_

namespace sqlgen::transpilation {

template <class _C>
struct Desc {
  using ColType = Desc<_C>;
};

}  // namespace sqlgen::transpilation

#endif
