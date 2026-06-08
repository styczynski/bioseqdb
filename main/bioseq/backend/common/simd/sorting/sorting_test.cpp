#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

import bioseq_simd.sorting;
import testing;

extern "C" void bioseq_sort_pair_run_u128_keys(
    hwy::uint128_t* keys,
    std::size_t key_count,
    int sort_impl) noexcept;

namespace {

[[nodiscard]] hwy::uint128_t make_u128(std::uint64_t hi, std::uint64_t lo) {
    return hwy::uint128_t{.lo = lo, .hi = hi};
}

[[nodiscard]] bool u128_less(const hwy::uint128_t& a, const hwy::uint128_t& b) {
    return (a.hi == b.hi) ? (a.lo < b.lo) : (a.hi < b.hi);
}

[[nodiscard]] std::vector<hwy::uint128_t> random_u128_case(std::size_t n) {
    std::mt19937_64 rng(0x5EED12345678ULL + n);
    std::vector<hwy::uint128_t> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t hi = (i % 3 == 0) ? (rng() % 17) : rng();
        const std::uint64_t lo = (i % 5 == 0) ? (rng() % 23) : rng();
        out.push_back(make_u128(hi, lo));
    }
    return out;
}

[[nodiscard]] std::vector<std::vector<hwy::uint128_t>> u128_sort_cases() {
    std::vector<std::vector<hwy::uint128_t>> cases;
    for (std::size_t n : {0u, 1u, 2u, 3u, 4u, 7u, 8u, 9u, 15u, 31u, 64u, 127u, 257u}) {
        cases.push_back(random_u128_case(n));
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

template <std::size_t StreamWidth>
void check_u128_vec_stream_width() {
    for (const auto& input : u128_sort_cases()) {
        auto expected_asc = input;
        std::sort(expected_asc.begin(), expected_asc.end(), u128_less);

        auto got_asc = input;
        bioseq::simd::sorting::sort_u128_ascending_vec_stream<StreamWidth>(
            got_asc.data(), got_asc.size());
        ASSERT(got_asc == expected_asc);

        auto expected_desc = input;
        std::sort(expected_desc.begin(), expected_desc.end(),
            [](const hwy::uint128_t& a, const hwy::uint128_t& b) {
                return u128_less(b, a);
            });

        auto got_desc = input;
        bioseq::simd::sorting::sort_u128_descending_vec_stream<StreamWidth>(
            got_desc.data(), got_desc.size());
        ASSERT(got_desc == expected_desc);
    }
}

}  // namespace

TEST_CASE("u128 vec_stream sort matches std::sort", "[sorting][u128][vec_stream]") {
    check_u128_vec_stream_width<1>();
    check_u128_vec_stream_width<2>();
    check_u128_vec_stream_width<3>();
    check_u128_vec_stream_width<4>();
}

TEST_CASE("u128 default sort uses vec_stream ordering", "[sorting][u128]") {
    auto input = random_u128_case(257);
    auto expected = input;
    std::sort(expected.begin(), expected.end(), u128_less);

    bioseq::simd::sorting::sort_u128_ascending(input.data(), input.size());
    ASSERT(input == expected);
}

TEST_CASE("per-sequence pair-run sidecar sort matches std::sort",
          "[sorting][u128][pair-run-sidecar]") {
    for (int impl : {1, 2, 3, 4}) {
        auto input = random_u128_case(257);
        auto expected = input;
        std::sort(expected.begin(), expected.end(), u128_less);

        bioseq_sort_pair_run_u128_keys(input.data(), input.size(), impl);
        ASSERT(input == expected);
    }
}
