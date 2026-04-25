#ifndef SQLGEN_TRANSPILATION_TABLEWRAPPER_HPP_
#define SQLGEN_TRANSPILATION_TABLEWRAPPER_HPP_

namespace sqlgen::transpilation {

template <class _TableType>
struct TableWrapper {
  using TableType = _TableType;
};

}  // namespace sqlgen::transpilation

#endif
