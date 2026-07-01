#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

import bioseq.simd.utils;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

TEMPLATE_TEST_CASE("Correct upscaling of the vector", "[simd]", uint8_t, uint16_t, uint32_t, uint64_t) {
    using unit_t = TestType;
    using output_t = uint64_t;

    const hn::ScalableTag<unit_t> d_in;
	using D_IN = decltype(d_in);

	const hn::ScalableTag<output_t> d_out;
	using D_OUT = decltype(d_out);

    using vec_in_t = hn::Vec<D_IN>;
	using vec_out_t = hn::Vec<D_OUT>;

	auto data = increasing_vector<unit_t>(hn::Lanes(d_in), unit_t{1});
	vec_in_t v = hn::LoadU(d_in, data.data());

    const size_t output_lanes = hn::Lanes(d_out);
    size_t offset = 0;

	typename vector_upscaler<D_IN>::template rescaled_t<D_OUT> out_vectors = vector_upscaler<D_IN>::upscale(d_in, d_out, v);
	for (auto& single_vec : out_vectors) {
        std::vector<output_t> vec_data = hwy_to_vector<D_OUT, output_t, vec_out_t>(d_out, single_vec);
        std::vector<output_t> vec_input(data.begin() + offset, data.begin() + offset + output_lanes);
		ASSERT(vec_input == vec_data );
        offset += output_lanes;
	}
};

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

    simd_matrix.transpose(d);
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

TEMPLATE_TEST_CASE("SIMD matrix is correctly loaded via .fill_with_offsets() when simulating .fill() with context=0", "[simd]", uint8_t, uint16_t, uint32_t, uint64_t) {
    using unit_t = TestType;
    const hn::ScalableTag<unit_t> d;
    using d_t = decltype(d);
    using vec_t = hn::Vec<d_t>;

    auto matrix = vector_increasing_matrix_2d<unit_t>(hn::Lanes(d), hn::Lanes(d), unit_t{0});
    auto flat_matrix = std::ranges::to<std::vector>(matrix | flatten);

    HWY_ALIGN vec_t buf[hn::MaxLanes(d) * hn::MaxLanes(d)];
    square_lane_matrix<d_t> simd_matrix(buf);
    std::array<size_t, 256> rows_offsets = {};
    const size_t lanes = hn::Lanes(d);
    for(size_t i=0; i<lanes; ++i) {
        rows_offsets[i] = i*lanes;
    }
    simd_matrix.fill_with_offsets(d, rows_offsets, flat_matrix.data());

    auto loaded_matrix = simd_matrix.template to_scalar_2d<unit_t>(d);
    ASSERT(loaded_matrix == matrix);
}

TEMPLATE_TEST_CASE("SIMD matrix partial_fill_with_offsets()/fill_with_offsets() with context limits loaded elements and zeros the rest", "[simd]", uint8_t) { //, uint16_t, uint32_t, uint64_t) {
    // This is similar to the test case without _offsets().
    // We just "simulate" context with offsets
    // Negative number means the function should behave exactly like a call to fill with context=1 (loading mvoed to the left)
    // Positive number (or zero) means we shift data loaded by one to the right (skip one element)
    const int32_t fake_context = GENERATE(0, -1, -3, 1, 3, 5);
    const bool is_full = GENERATE(false, true);
    
    using unit_t = TestType;
    const hn::ScalableTag<unit_t> d;
    using d_t = decltype(d);
    using vec_t = hn::Vec<d_t>;

    const size_t lanes = hn::Lanes(d);
    const size_t simulated_equal_offset = static_cast<int32_t>(lanes) + fake_context;
    std::array<size_t, 256> row_offsets = {};
    for(size_t i=0; i<lanes; ++i) {
        row_offsets[i] = simulated_equal_offset * i;
    }

    // Backing storage for input data (longer than data_available to keep loads in-bounds)
    const size_t storage_len = lanes * lanes * 4;
    auto data = increasing_vector<unit_t>(storage_len, unit_t{1});

    // Choose data_available so that we have a partially filled matrix
    const size_t data_available = std::min(storage_len, lanes + (lanes-1) * (simulated_equal_offset));

    HWY_ALIGN vec_t buf[256];
    square_lane_matrix<d_t> simd_matrix(buf);

    size_t loaded_full_rows = lanes;
    size_t loaded_partial_filled_rows = 0;
    if (!is_full) {
        std::tie(loaded_full_rows, loaded_partial_filled_rows) = simd_matrix.partial_fill_with_offsets(d, data_available, row_offsets, data.data());
    } else {
        size_t loaded_offset = simd_matrix.fill_with_offsets(d, row_offsets, data.data());
        ASSERT(loaded_offset == lanes);
    }

    auto loaded = simd_matrix.template to_scalar_2d<unit_t>(d);

    std::vector<std::vector<unit_t>> expected_matrix;
    expected_matrix.resize(lanes);

    size_t expected_full_rows = 0;
    size_t expected_partial_filled_rows = 0;
    size_t partial_rows_counter = 0;
    for (size_t i = 0; i < lanes; ++i) {
        expected_matrix[i].resize(lanes);
        for (size_t j = 0; j < lanes; ++j) {
            const size_t src_idx = i * simulated_equal_offset + j;
            const unit_t expected = (src_idx < data_available || is_full) ? data[src_idx] : unit_t{0};
            if (!(src_idx < data_available || is_full)) {
                partial_rows_counter++;
            }
            expected_matrix[i][j] = expected;
        }
        if (partial_rows_counter == 0) {
            expected_full_rows++;
        } else if (partial_rows_counter == lanes) {
            // no-op
        } else {
            expected_partial_filled_rows = partial_rows_counter;
        }
        partial_rows_counter = 0;
    }

    if (!is_full) {
        // partial fill counts lanes by number of offsets
        expected_full_rows += (256 - lanes);
    }

    ASSERT(loaded == expected_matrix);
    ASSERT(loaded_full_rows == expected_full_rows, fmt::format("Correct count of loaded full rows fake_context={} is_full={} lanes={}", fake_context, is_full, lanes));
    ASSERT(loaded_partial_filled_rows == expected_partial_filled_rows, fmt::format("Correct count of loaded partial rows fake_context={} is_full={}", fake_context, is_full));
    
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