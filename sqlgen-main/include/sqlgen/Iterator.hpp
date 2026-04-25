#ifndef SQLGEN_ITERATOR_HPP_
#define SQLGEN_ITERATOR_HPP_

#include <iterator>
#include <memory>
#include <ranges>

#include "Ref.hpp"
#include "Result.hpp"
#include "internal/batch_size.hpp"
#include "internal/collect/vector.hpp"
#include "internal/from_str_vec.hpp"

namespace sqlgen {

/// An input_iterator that returns the underlying type.
template <class T, class UnderlyingIteratorT>
class Iterator {
 public:
  using difference_type = std::ptrdiff_t;
  using value_type = Result<T>;

  struct End {
    bool operator==(const Iterator& _it) const noexcept { return _it == *this; }

    bool operator!=(const Iterator& _it) const noexcept { return _it != *this; }
  };

  Iterator(const Ref<UnderlyingIteratorT>& _it)
      : current_batch_(get_next_batch(_it)), it_(_it), ix_(0) {}

  ~Iterator() = default;

  Result<T>& operator*() const noexcept { return (*current_batch_)[ix_]; }

  Result<T>* operator->() const noexcept { return &(*current_batch_)[ix_]; }

  bool operator==(const End&) const noexcept {
    return ix_ >= current_batch_->size();
  }

  bool operator!=(const End& _end) const noexcept { return !(*this == _end); }

  Iterator& operator++() noexcept {
    ++ix_;
    if (ix_ >= current_batch_->size() && !it_->end()) {
      current_batch_ = get_next_batch(it_);
      ix_ = 0;
    }
    return *this;
  }

  void operator++(int) noexcept { ++*this; }

 private:
  static Ref<std::vector<Result<T>>> get_next_batch(
      const Ref<UnderlyingIteratorT>& _it) noexcept {
    using namespace std::ranges::views;
    return _it->next(SQLGEN_BATCH_SIZE)
        .transform([](auto str_vec) {
          return Ref<std::vector<Result<T>>>::make(internal::collect::vector(
              str_vec | transform(internal::from_str_vec<T>)));
        })
        .value_or(Ref<std::vector<Result<T>>>());
  }

 private:
  /// The current batch of data.
  Ref<std::vector<Result<T>>> current_batch_;

  /// The underlying database iterator.
  Ref<UnderlyingIteratorT> it_;

  /// The current index in the current batch.
  size_t ix_;
};

}  // namespace sqlgen

#endif
