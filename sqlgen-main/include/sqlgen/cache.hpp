#ifndef SQLGEN_CACHE_HPP_
#define SQLGEN_CACHE_HPP_

#include <functional>
#include <future>
#include <limits>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "Ref.hpp"
#include "Result.hpp"
#include "internal/query_value_t.hpp"
#include "is_connection.hpp"
#include "transpilation/to_sql.hpp"

namespace sqlgen {

template <class QueryT, class Connection, size_t _max_size>
  requires is_connection<Connection>
class CacheImpl {
  static constexpr size_t max_size_ =
      _max_size == 0 ? std::numeric_limits<size_t>::max() : _max_size;

 public:
  using ValueType = internal::query_value_t<QueryT, Ref<Connection>>;

  static Result<ValueType> fetch(const QueryT& _query,
                                 const Ref<Connection>& _conn) {
    const auto sql = _conn->to_sql(transpilation::to_sql(_query));

    const auto try_read_from_cache =
        [&]() -> Result<std::shared_future<Result<ValueType>>> {
      std::shared_lock read_lock(mtx_);
      const auto it = cache_.find(sql);
      if (it != cache_.end()) {
        return it->second.first;
      }
      return error("Could not find the result for the query in the cache.");
    };

    const auto write_to_cache =
        [&](const std::shared_future<Result<ValueType>>& _future) {
          std::unique_lock write_lock(mtx_);
          cache_[sql] = std::make_pair(_future, counter_++);
          if (cache_.size() > max_size_) {
            const auto it =
                std::min_element(cache_.begin(), cache_.end(),
                                 [](const auto& _p1, const auto& _p2) {
                                   return _p1.second.second < _p2.second.second;
                                 });
            cache_.erase(it);
          }
        };

    return try_read_from_cache()
        .and_then([](auto _future) { return _future.get(); })
        .or_else([&](auto&&) -> Result<ValueType> {
          const std::shared_future<Result<ValueType>> future =
              std::async(std::launch::async, [&]() { return _query(_conn); });
          write_to_cache(future);
          return future.get();
        });
  }

  static void clear() {
    std::unique_lock write_lock(mtx_);
    cache_.clear();
  }

  static const auto& cache() { return cache_; }

 private:
  inline static size_t counter_ = 0;

  inline static std::unordered_map<
      std::string, std::pair<std::shared_future<Result<ValueType>>, size_t>>
      cache_;

  inline static std::shared_mutex mtx_;
};

template <class QueryT, size_t _max_size>
struct Cache {
  template <class Connection>
    requires is_connection<Connection>
  auto operator()(const Ref<Connection>& _conn) const {
    return CacheImpl<QueryT, std::remove_cvref_t<Connection>, _max_size>::fetch(
        query_, _conn);
  }

  template <class Connection>
    requires is_connection<Connection>
  auto operator()(const Result<Ref<Connection>>& _res) const {
    return _res.and_then(
        [&](const Ref<Connection>& _conn) { return (*this)(_conn); });
  }

  template <class Connection>
    requires is_connection<Connection>
  static const auto& cache(const Ref<Connection>& _conn) {
    return CacheImpl<QueryT, std::remove_cvref_t<Connection>,
                     _max_size>::cache();
  }

  template <class Connection>
    requires is_connection<Connection>
  static const auto& cache(const Result<Ref<Connection>>& _res) {
    return CacheImpl<QueryT, std::remove_cvref_t<Connection>,
                     _max_size>::cache();
  }

  template <class Connection>
    requires is_connection<Connection>
  static void clear(const Result<Ref<Connection>>&) {
    CacheImpl<QueryT, std::remove_cvref_t<Connection>, _max_size>::clear();
  }

  QueryT query_;
};

template <size_t _max_size = 2056, class QueryT>
auto cache(const QueryT& _query) {
  return Cache<std::remove_cvref_t<QueryT>, _max_size>{.query_ = _query};
}

}  // namespace sqlgen

#endif
