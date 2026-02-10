module;
#include "common.h"
#include <numeric>
#include <ranges>
#include <iostream>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>


export module testing;
export import mlogging;

export {

template<typename T>
std::vector<T> increasing_vector(size_t length, T start = 0) {
    std::vector<T> v(length);
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
}

constexpr flatten_t flatten;

class testRunListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        log_configure(true);
    }
};

};

CATCH_REGISTER_LISTENER(testRunListener);