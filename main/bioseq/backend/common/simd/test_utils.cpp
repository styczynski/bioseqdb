#include <hwy/highway.h>
#include <hwy/print-inl.h>
#include <hwy/aligned_allocator.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <libassert/assert-catch2.hpp>

#include "common.h"

#include <ranges>

import bioseq_simd.utils;
import testing;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;


TEMPLATE_TEST_CASE("SIMD matrix is correctly loaded via .fill()", "[simd]", uint16_t, uint32_t, uint64_t) {
    using unit_t = TestType;
    const hn::ScalableTag<unit_t> d;
    using d_t = decltype(d);
    using vec_t = hn::Vec<d_t>;

    auto matrix = vector_increasing_matrix_2d<unit_t>(hn::Lanes(d), hn::Lanes(d), 0);
    auto flat_matrix = std::ranges::to<std::vector>(matrix | flatten);

    HWY_ALIGN vec_t buf[256];
    square_lane_matrix<d_t> simd_matrix(buf);
    simd_matrix.fill(d, 0, flat_matrix.data());

    auto loaded_matrix = simd_matrix.template to_scalar_2d<unit_t>(d);
    ASSERT(loaded_matrix == matrix);
};

};

HWY_AFTER_NAMESPACE();