#ifndef SQLGEN_RANGE_HPP_
#define SQLGEN_RANGE_HPP_

#include <memory>
#include <ranges>

#include "Result.hpp"

namespace sqlgen {

template <class T>
class Range;

/// This exists mainly for user-friendliness: We want people to be able
/// to pass Range<T> to read without having to worry about connector-specific
/// iterators.
template <class T>
struct Range {
  using value_type = Result<T>;
};

/// This class is meant to provide a way to iterate through the data in the
/// database efficiently that is compatible with std::ranges.
template <class IteratorType>
  requires std::input_iterator<IteratorType>
class Range<IteratorType> {
 public:
  using value_type = typename IteratorType::value_type;

  struct End {};

  Range(const IteratorType& _it) : it_(_it) {}

  ~Range() = default;

  auto begin() const { return it_; }

  auto end() const { return typename IteratorType::End{}; }

 private:
  /// The underlying database iterator.
  IteratorType it_;
};

}  // namespace sqlgen

#endif
