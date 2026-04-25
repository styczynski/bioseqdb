#ifndef SQLGEN_INTERNAL_RANDOM_HPP
#define SQLGEN_INTERNAL_RANDOM_HPP

#include <random>
#include <string>

namespace sqlgen::internal {

/// Generates a random number to be used in the names of iterators and such.
inline std::string random() {
  std::random_device rd;
  std::mt19937 gen(rd());
  return std::to_string(gen());
}

}  // namespace sqlgen::internal

#endif  // SQLGEN_INTERNAL_RANDOM_HPP

