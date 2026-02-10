#include <hwy/highway.h>
#include <hwy/print-inl.h>
#include <hwy/aligned_allocator.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <libassert/assert-catch2.hpp>

#include "common.h"

#include <ranges>

import bioseq_simd.utils;
import testing;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;


TEMPLATE_TEST_CASE("SIMD matrix is correctly loaded via .fill() context=0", "[simd]", uint8_t, uint16_t, uint32_t, uint64_t) {
    using unit_t = TestType;
    const hn::ScalableTag<unit_t> d;
    using d_t = decltype(d);
    using vec_t = hn::Vec<d_t>;

    auto matrix = vector_increasing_matrix_2d<unit_t>(hn::Lanes(d), hn::Lanes(d), unit_t{0});
    auto flat_matrix = std::ranges::to<std::vector>(matrix | flatten);

    HWY_ALIGN vec_t buf[hn::MaxLanes(d) * hn::MaxLanes(d)];
    square_lane_matrix<d_t> simd_matrix(buf);
    simd_matrix.fill(d, 0, flat_matrix.data());

    auto loaded_matrix = simd_matrix.template to_scalar_2d<unit_t>(d);
    ASSERT(loaded_matrix == matrix);
}

TEMPLATE_TEST_CASE("SIMD matrix transpose() produces transposed matrix", "[simd]", uint8_t, uint16_t, uint32_t, uint64_t) {
    using unit_t = TestType;
    const hn::ScalableTag<unit_t> d;
    using d_t = decltype(d);
    using vec_t = hn::Vec<d_t>;

    const size_t lanes = hn::Lanes(d);

    auto matrix = vector_increasing_matrix_2d<unit_t>(lanes, lanes, unit_t{0});
    auto flat_matrix = std::ranges::to<std::vector>(matrix | flatten);

    HWY_ALIGN vec_t buf[hn::MaxLanes(d) * hn::MaxLanes(d)];
    square_lane_matrix<d_t> simd_matrix(buf);
    simd_matrix.fill(d, 0, flat_matrix.data());

    JustLog("mat = {}", hwy_str(d, simd_matrix));
    simd_matrix.transpose(d);
    JustLog("mat = {}", hwy_str(d, simd_matrix));
    auto transposed = simd_matrix.template to_scalar_2d<unit_t>(d);

    std::vector<std::vector<unit_t>> expected_transposed;
    expected_transposed.resize(lanes);
    for (size_t i = 0; i < lanes; ++i) {
        expected_transposed[i].resize(lanes);
        for (size_t j = 0; j < lanes; ++j) {
            expected_transposed[i][j] = matrix[j][i];
        }
    }

    ASSERT(expected_transposed == transposed);
}

TEMPLATE_TEST_CASE("SIMD matrix partial_fill()/fill() with context limits loaded elements and zeros the rest", "[simd]", uint8_t, uint16_t, uint32_t, uint64_t) {
    const size_t context = GENERATE(0, 1, 3);
    const bool is_full = GENERATE(false, true);
    
    using unit_t = TestType;
    const hn::ScalableTag<unit_t> d;
    using d_t = decltype(d);
    using vec_t = hn::Vec<d_t>;

    const size_t lanes = hn::Lanes(d);
    if (context > lanes) {
        // Skip test for context too big
        return;
    }
    const size_t row_offset = lanes - context;

    // Backing storage for input data (longer than data_available to keep loads in-bounds)
    const size_t storage_len = lanes * lanes * 4;
    auto data = increasing_vector<unit_t>(storage_len, unit_t{1});

    // Choose data_available so that we have a partially filled matrix
    const size_t data_available = std::min(storage_len, lanes + (lanes-1) * (row_offset));

    HWY_ALIGN vec_t buf[256];
    square_lane_matrix<d_t> simd_matrix(buf);
    if (!is_full) {
        simd_matrix.partial_fill(d, data_available, context, data.data());
    } else {
        simd_matrix.fill(d, context, data.data());
    }

    auto loaded = simd_matrix.template to_scalar_2d<unit_t>(d);

    std::vector<std::vector<unit_t>> expected_matrix;
    expected_matrix.resize(lanes);
    for (size_t i = 0; i < lanes; ++i) {
        expected_matrix[i].resize(lanes);
        for (size_t j = 0; j < lanes; ++j) {
            const size_t src_idx = i * row_offset + j;
            const unit_t expected = (src_idx < data_available || is_full) ? data[src_idx] : unit_t{0};
            expected_matrix[i][j] = expected;
        }
    }

    ASSERT(loaded == expected_matrix);
}

TEMPLATE_TEST_CASE("SIMD matrix double transpose()", "[simd]", uint8_t, uint16_t, uint32_t, uint64_t) {
    using unit_t = TestType;
    const hn::ScalableTag<unit_t> d;
    using d_t = decltype(d);
    using vec_t = hn::Vec<d_t>;

    auto matrix = vector_increasing_matrix_2d<unit_t>(hn::Lanes(d), hn::Lanes(d), unit_t{0});
    auto flat_matrix = std::ranges::to<std::vector>(matrix | flatten);

    HWY_ALIGN vec_t buf[256];
    square_lane_matrix<d_t> simd_matrix(buf);
    simd_matrix.fill(d, 0, flat_matrix.data());
    simd_matrix.transpose(d);
    simd_matrix.transpose(d);

    auto loaded_matrix = simd_matrix.template to_scalar_2d<unit_t>(d);
    ASSERT(loaded_matrix == matrix);
}

};

HWY_AFTER_NAMESPACE();