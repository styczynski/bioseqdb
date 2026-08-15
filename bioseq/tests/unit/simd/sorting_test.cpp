/**
 * @file
 * @brief Compares unsigned 128-bit SIMD sort routes with `std::sort`.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <hwy/highway.h>
#include <random>
#include <tests/support/common.hpp>
#include <vector>

import bioseq.simd.sorting;

/**
 * @brief Sorts one sidecar key run through the selected production route.
 * @param[in,out] keys Contiguous keys to reorder in place.
 * @param[in] key_count Number of keys available at `keys`.
 * @param[in] sort_impl Production route selector exercised by the test.
 */
extern "C" void
bioseq_sort_pair_run_u128_keys(hwy::uint128_t* keys, std::size_t key_count, int sort_impl) noexcept;

namespace {

/**
 * @brief Constructs a Highway 128-bit integer from high and low words.
 * @param[in] high Most-significant 64 bits.
 * @param[in] low Least-significant 64 bits.
 * @return Combined unsigned 128-bit value.
 */
[[nodiscard]] auto make_u128(std::uint64_t high, std::uint64_t low) -> hwy::uint128_t {
    return hwy::uint128_t{.lo = low, .hi = high};
}

/**
 * @brief Orders unsigned 128-bit values lexicographically by high then low word.
 * @param[in] left Left comparison operand.
 * @param[in] right Right comparison operand.
 * @return `true` when `left` precedes `right`.
 */
[[nodiscard]] auto u128_less(const hwy::uint128_t& left, const hwy::uint128_t& right) -> bool {
    return (left.hi == right.hi) ? (left.lo < right.lo) : (left.hi < right.hi);
}

/**
 * @brief Generates a deterministic key vector with repeated high and low words.
 * @param[in] key_count Number of keys to generate.
 * @return Reproducible unsorted key vector.
 */
[[nodiscard]] auto random_u128_case(std::size_t key_count) -> std::vector<hwy::uint128_t> {
    std::mt19937_64 random_engine(0x5EED12345678ULL + key_count);
    std::vector<hwy::uint128_t> keys;
    keys.reserve(key_count);
    for (std::size_t index = 0; index < key_count; ++index) {
        const std::uint64_t high = (index % 3 == 0) ? (random_engine() % 17) : random_engine();
        const std::uint64_t low = (index % 5 == 0) ? (random_engine() % 23) : random_engine();
        keys.push_back(make_u128(high, low));
    }
    return keys;
}

/**
 * @brief Builds boundary, duplicate, sorted, and reversed sorting fixtures.
 * @return Complete set of unsigned 128-bit sorting inputs.
 */
[[nodiscard]] auto u128_sort_cases() -> std::vector<std::vector<hwy::uint128_t>> {
    std::vector<std::vector<hwy::uint128_t>> cases;
    for (const std::size_t key_count :
         {0u, 1u, 2u, 3u, 4u, 7u, 8u, 9u, 15u, 31u, 64u, 127u, 257u}) {
        cases.push_back(random_u128_case(key_count));
    }

    std::vector<hwy::uint128_t> all_same_kmer;
    for (std::uint64_t i = 0; i < 96; ++i) {
        all_same_kmer.push_back(make_u128(42, (i * 37) % 19));
    }
    cases.push_back(all_same_kmer);

    cases.push_back(std::vector<hwy::uint128_t>(129, make_u128(7, 11)));

    std::vector<hwy::uint128_t> clustered;
    for (std::uint64_t hi = 0; hi < 16; ++hi) {
        for (std::uint64_t lo = 0; lo < 8; ++lo) {
            clustered.push_back(make_u128(hi % 4, (7 - lo) % 5));
        }
    }
    cases.push_back(clustered);

    auto sorted = random_u128_case(128);
    std::sort(sorted.begin(), sorted.end(), u128_less);
    cases.push_back(sorted);

    auto reversed = sorted;
    std::reverse(reversed.begin(), reversed.end());
    cases.push_back(reversed);

    return cases;
}

/**
 * @brief Checks ascending and descending parity for one vector-stream width.
 * @tparam p_stream_width Number of independently scheduled SIMD streams.
 */
template <std::size_t p_stream_width> auto check_u128_vector_stream_width() -> void {
    for (const auto& input : u128_sort_cases()) {
        auto expected_asc = input;
        std::sort(expected_asc.begin(), expected_asc.end(), u128_less);

        auto got_asc = input;
        bioseq::simd::sorting::sort_unsigned_128_ascending_vector_stream<p_stream_width>(
            got_asc.data(), got_asc.size());
        ASSERT(got_asc == expected_asc);

        auto expected_desc = input;
        std::sort(expected_desc.begin(),
                  expected_desc.end(),
                  [](const hwy::uint128_t& left, const hwy::uint128_t& right) {
                      return u128_less(right, left);
                  });

        auto got_desc = input;
        bioseq::simd::sorting::sort_unsigned_128_descending_vector_stream<p_stream_width>(
            got_desc.data(), got_desc.size());
        ASSERT(got_desc == expected_desc);
    }
}

} // namespace

TEST_CASE("u128 vector_stream sort matches std::sort",
          "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]") {
    check_u128_vector_stream_width<1>();
    check_u128_vector_stream_width<2>();
    check_u128_vector_stream_width<3>();
    check_u128_vector_stream_width<4>();
}

TEST_CASE("u128 default sort uses vector_stream ordering",
          "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]") {
    auto input = random_u128_case(257);
    auto expected = input;
    std::sort(expected.begin(), expected.end(), u128_less);

    bioseq::simd::sorting::sort_unsigned_128_ascending(input.data(), input.size());
    ASSERT(input == expected);
}

TEST_CASE("per-sequence pair-run sidecar sort matches std::sort",
          "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]") {
    for (int impl : {1, 2, 3, 4}) {
        auto input = random_u128_case(257);
        auto expected = input;
        std::sort(expected.begin(), expected.end(), u128_less);

        bioseq_sort_pair_run_u128_keys(input.data(), input.size(), impl);
        ASSERT(input == expected);
    }
}
