module;
#include "common.h"
#include <numeric>
#include <ranges>
#include <iostream>
#include <algorithm>
#include <set>
#include <vector>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>


export module testing;
export import mlogging;

export {

template<
	std::ranges::contiguous_range R,
	typename T = std::remove_reference_t<std::ranges::range_reference_t<R>>
>
requires (
	std::ranges::sized_range<R>
)
bool assert_range_set_equality(const R& expected, const R& actual) {
    std::set<T> expected_set = std::ranges::to<std::set>(expected); //(expected.begin(), expected.end());
    std::set<T> actual_set = std::ranges::to<std::set>(actual);

    std::vector<T> els_not_present;
    std::vector<T> els_extra;
    std::vector<T> els_common;

    std::ranges::set_difference(expected_set, actual_set, std::back_inserter(els_not_present));
    std::ranges::set_difference(actual_set, expected_set, std::back_inserter(els_extra));
    std::ranges::set_intersection(actual_set, expected_set, std::back_inserter(els_common));

    const size_t expected_size = expected_set.size();
    const size_t actual_size = actual_set.size();

    ASSERT(
        els_not_present.size() == 0 && els_extra.size() == 0 && els_common.size() == expected_set.size(),
        fmt::format(
            "assert_range_set_equality: Expect ranges to be equal (as sets). Two sets of size (expected) {} and (actual) {} have {} common element, while there are {} missing and {} extra. ",
            expected_size, actual_size,
            els_common.size(), els_not_present.size(), els_extra.size()
        ),
        els_not_present,
        els_extra,
        expected_set,
        actual_set
    );

    return true;
}

template<typename T>
std::vector<T> increasing_vector(size_t length, T start = 0) {
    std::vector<T> v(length);
    std::iota(v.begin(), v.end(), start);
    return v; 
}

template<typename T, size_t L>
std::vector<T> increasing_array(T start = 0) {
    std::array<T, L> v(L);
    std::iota(v.begin(), v.end(), start);
    return v; 
}

template<typename T>
std::vector<std::vector<T>> vector_increasing_matrix_2d(size_t dim_rows, size_t dim_cols, T start = 0) {
    std::vector<std::vector<T>> v(dim_cols);
    T val = start;
    for(size_t row_no=0;row_no<dim_rows;++row_no) {
        v[row_no].resize(dim_cols);
        for(size_t col_no=0;col_no<dim_cols;++col_no) {
            v[row_no][col_no] = val++;
        }
    }
    return v;
}

template<typename R>
concept nested_range = std::ranges::input_range<R> && std::ranges::range<std::ranges::range_reference_t<R>>;

struct flatten_t {
    template<nested_range R>
    auto operator()(R && r) const
    {
        return std::forward<R>(r) | std::ranges::views::transform(*this) | std::ranges::views::join;
    }
    
    template<typename T>
    auto operator()(T && t) const
    {
        return std::forward<T>(t);
    }
};

template<typename T>
auto operator |(T && t, flatten_t f) {
    return f(std::forward<T>(t));
};

constexpr flatten_t flatten;

class testRunListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        log_configure(true);
        libassert::set_diff_highlighting(true);
    }
};

};

CATCH_REGISTER_LISTENER(testRunListener);