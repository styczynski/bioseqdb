#ifndef SQLGEN_TO_HPP_
#define SQLGEN_TO_HPP_

namespace sqlgen {

template <class NewToType>
struct To {};

template <class NewToType>
const auto to = To<NewToType>{};

}  // namespace sqlgen

#endif
