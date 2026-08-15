/**
 * @file two_stack_exact_output_test.cpp
 * @brief Fixed-output checks for resident and fixed-target two-stack kernels.
 */

#include <tests/support/common.hpp>
#include "common/simd/highway_test_probes/contract.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stack_minimizers;

namespace {

using namespace seq::literals;
using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::minimizer_kernel_algorithm;
using bioseq::minimizers::minimizer_kernel_invocation;
using bioseq::minimizers::minimizer_kernel_status;
using bioseq::minimizers::minimizer_sink;
using bioseq::minimizers::minimizer_sink_batch_view;
using bioseq::minimizers::packed_sequence_kernel_view;
using bioseq::minimizers::two_stack_continuation_status;
using bioseq::minimizers::two_stack_route;

class resident_output_sink final {
public:
    static constexpr std::size_t k_batch_capacity = 256u;

    [[nodiscard]] minimizer_sink contract() noexcept {
        return {
            .state = this,
            .acquire = &acquire,
            .commit = &commit,
        };
    }

    std::vector<minimap_sketch_data> records;
    bool protocol_ok = true;

private:
    [[nodiscard]] static minimizer_sink_batch_view acquire(
        void* opaque, const std::size_t minimum)
    {
        auto& self = *static_cast<resident_output_sink*>(opaque);
        if (self.pending_ || minimum > k_batch_capacity) {
            self.protocol_ok = false;
            return {};
        }
        self.pending_ = true;
        return {
            .data = self.storage_.data(),
            .capacity = k_batch_capacity,
            .layout = bioseq::minimizers::
                k_minimizer_output_layout<minimap_sketch_data>,
            .record_size = sizeof(minimap_sketch_data),
            .record_alignment = alignof(minimap_sketch_data),
        };
    }

    static void commit(void* opaque, const std::size_t initialized) noexcept {
        auto& self = *static_cast<resident_output_sink*>(opaque);
        if (!self.pending_ || initialized > k_batch_capacity) {
            self.protocol_ok = false;
            return;
        }
        auto* const first = std::launder(
            reinterpret_cast<minimap_sketch_data*>(self.storage_.data()));
        try {
            self.records.insert(self.records.end(), first, first + initialized);
        } catch (...) {
            self.protocol_ok = false;
        }
        std::destroy_n(first, initialized);
        self.pending_ = false;
    }

    alignas(minimap_sketch_data) std::array<
        std::byte, k_batch_capacity * sizeof(minimap_sketch_data)> storage_{};
    bool pending_ = false;
};

[[nodiscard]] const bioseq::simd_dispatch::highway_kernel_table& fixed_table() {
    return bioseq::simd_dispatch::highway_test_kernel_table();
}

[[nodiscard]] std::uint8_t fixed_dna4_code() {
    const auto* identity = fixed_table().resolve_alphabet_identity<seq::dna4>();
    REQUIRE(identity != nullptr);
    return identity->alphabet_code;
}

template<class p_range>
[[nodiscard]] packed_sequence_kernel_view packed_view(const p_range& range) {
    using traits_type = typename p_range::traits;
    return {
        .storage = reinterpret_cast<const std::byte*>(
            range.read_cptr(range.cbegin())),
        .storage_units = static_cast<std::size_t>(range.size()),
        .begin_symbol = 0u,
        .end_symbol = static_cast<std::size_t>(range.bp_size()),
        .alphabet_code = fixed_dna4_code(),
        .bits_per_symbol =
            static_cast<std::uint8_t>(traits_type::bits_per_char),
        .storage_unit_bytes = static_cast<std::uint8_t>(
            sizeof(typename traits_type::storage_unit)),
        .final_segment = true,
    };
}

[[nodiscard]] std::vector<std::pair<std::size_t, std::uint64_t>>
run_resident_vector_stream_pairs(
    const std::vector<seq::dna4>& data,
    const std::size_t window,
    const std::size_t kmer)
{
    using sequence_configuration =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_configuration> range{
        std::vector<seq::dna4>(data)};
    resident_output_sink sink;
    minimizer_kernel_invocation invocation{
        .sequence = packed_view(range),
        .window = window,
        .kmer = kmer,
        .output_layout = bioseq::minimizers::
            k_minimizer_output_layout<minimap_sketch_data>,
        .output_hash_mask = minimap_sketch_data::k_hash_field_mask,
        .output_position_mask = minimap_sketch_data::k_position_field_mask,
        .two_stack_route = two_stack_route::resident_vector_stream,
        .sink = sink.contract(),
    };
    const auto kernel = fixed_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    REQUIRE(result.status == minimizer_kernel_status::ok);
    REQUIRE(result.family_status == static_cast<std::uint32_t>(
        two_stack_continuation_status::ok));
    REQUIRE(result.stats.algorithm == minimizer_kernel_algorithm::two_stack_nthash);
    REQUIRE(result.stats.target
        == bioseq::simd_dispatch::highway_test_target_binding().target);
    REQUIRE(result.emitted == sink.records.size());
    REQUIRE(sink.protocol_ok);

    std::vector<std::pair<std::size_t, std::uint64_t>> pairs;
    pairs.reserve(sink.records.size());
    for (const auto& record : sink.records) {
        pairs.emplace_back(
            static_cast<std::size_t>(record.last_pos()), record.kmer());
    }
    std::ranges::sort(pairs);
    pairs.erase(std::ranges::unique(pairs).begin(), pairs.end());
    return pairs;
}

template<typename p_storage_unit>
[[nodiscard]] std::vector<std::pair<std::size_t, std::uint64_t>>
run_two_stack_pairs(
    const std::vector<seq::dna4>& data,
    const std::size_t window,
    const std::size_t kmer)
{
    static_assert(std::same_as<p_storage_unit, std::uint32_t>);
    return run_resident_vector_stream_pairs(data, window, kmer);
}

#if defined(BIOSEQ_TEST_INDEXES)
template<>
[[nodiscard]] std::vector<std::pair<std::size_t, std::uint64_t>>
run_two_stack_pairs<std::uint8_t>(
    const std::vector<seq::dna4>& data,
    const std::size_t window,
    const std::size_t kmer)
{
    REQUIRE(data.size()
        <= static_cast<std::size_t>(
            (std::numeric_limits<std::uint64_t>::max)()));
    REQUIRE(window
        <= static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)()));
    REQUIRE(kmer
        <= static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)()));
    std::vector<std::uint8_t> ranks;
    ranks.reserve(data.size());
    for (const auto symbol : data) {
        ranks.push_back(static_cast<std::uint8_t>(symbol.to_rank()));
    }

    bioseq_highway_two_stack_reference_output_receipt receipt{};
    REQUIRE(bioseq_highway_collect_two_stack_reference_output(
        ranks.data(),
        static_cast<std::uint64_t>(ranks.size()),
        sizeof(std::uint8_t),
        static_cast<std::uint32_t>(window),
        static_cast<std::uint32_t>(kmer),
        &receipt));
    REQUIRE(receipt.target
        == bioseq::simd_dispatch::highway_test_target_binding().target);
    REQUIRE(receipt.input_storage_unit_bytes == sizeof(std::uint8_t));
    REQUIRE(receipt.window == window);
    REQUIRE(receipt.kmer == kmer);
    REQUIRE(receipt.pair_count
        <= k_bioseq_highway_two_stack_reference_output_capacity);

    std::vector<std::pair<std::size_t, std::uint64_t>> pairs;
    pairs.reserve(static_cast<std::size_t>(receipt.pair_count));
    for (std::size_t index = 0u;
         index < static_cast<std::size_t>(receipt.pair_count);
         ++index) {
        pairs.emplace_back(
            static_cast<std::size_t>(receipt.pairs[index].last_position),
            receipt.pairs[index].kmer);
    }
    return pairs;
}
#endif

template<std::size_t p_size>
void require_pairs(
    const std::vector<std::pair<std::size_t, std::uint64_t>>& actual,
    const std::array<
        std::pair<std::size_t, std::uint64_t>, p_size>& expected)
{
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        CAPTURE(index);
        REQUIRE(actual[index] == expected[index]);
    }
}

const std::vector<seq::dna4> k_dual_padded{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "CGCAGCACGCAGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGCGAGCCCAGCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "AGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGATCGTAGCTAGTCACTGTAGCT"
    "A"_dna4};

const std::vector<seq::dna4> k_single_no_pad{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"_dna4};

#if defined(BIOSEQ_TEST_INDEXES)
const std::vector<seq::dna4> k_wrap_fixture{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGA"_dna4};
#endif

constexpr std::array<std::pair<std::size_t, std::uint64_t>, 54>
k_dual_padded_expected{{
    {18, 0x3cbd2736ULL}, {24, 0x2bebb78bULL}, {25, 0x2f249414ULL},
    {29, 0x17218675ULL}, {31, 0xcc88f67ULL}, {35, 0x30aaf70bULL},
    {45, 0x254afdc8ULL}, {47, 0x2bebb78bULL}, {48, 0x2f249414ULL},
    {52, 0x17218675ULL}, {54, 0xcc88f67ULL}, {58, 0x30aaf70bULL},
    {68, 0x316abf15ULL}, {76, 0x839bd00ULL}, {80, 0x2504e63fULL},
    {84, 0x3ab189f4ULL}, {94, 0x1587e5b6ULL}, {103, 0x3b958cafULL},
    {105, 0x1783133bULL}, {107, 0x35770597ULL}, {109, 0x12ab46e1ULL},
    {118, 0x2504e63fULL}, {122, 0x3ab189f4ULL}, {128, 0x179de8a1ULL},
    {132, 0x1a3c42d4ULL}, {136, 0x34b1b6afULL}, {146, 0x3cbd2736ULL},
    {152, 0x2bebb78bULL}, {153, 0x2f249414ULL}, {157, 0x17218675ULL},
    {159, 0xcc88f67ULL}, {163, 0x30aaf70bULL}, {173, 0x254afdc8ULL},
    {175, 0x2bebb78bULL}, {176, 0x2f249414ULL}, {180, 0x17218675ULL},
    {182, 0xcc88f67ULL}, {186, 0x30aaf70bULL}, {194, 0x2167ee01ULL},
    {197, 0x2239d2eaULL}, {200, 0x3abd2666ULL}, {201, 0x2c460f69ULL},
    {204, 0x33972e51ULL}, {212, 0x1587e5b6ULL}, {221, 0x3b958cafULL},
    {223, 0x1783133bULL}, {225, 0x35770597ULL}, {235, 0x5eca9e8ULL},
    {241, 0x14ee37cULL}, {242, 0x19295565ULL}, {249, 0x3e052179ULL},
    {253, 0x254afdc8ULL}, {255, 0x2bebb78bULL}, {256, 0xf36ec4bULL},
}};

constexpr std::array<std::pair<std::size_t, std::uint64_t>, 26>
k_single_no_pad_expected{{
    {18, 0x3cbd2736ULL}, {24, 0x2bebb78bULL}, {25, 0x2f249414ULL},
    {29, 0x17218675ULL}, {31, 0xcc88f67ULL}, {35, 0x30aaf70bULL},
    {45, 0x254afdc8ULL}, {47, 0x2bebb78bULL}, {48, 0x2f249414ULL},
    {52, 0x17218675ULL}, {54, 0xcc88f67ULL}, {58, 0x30aaf70bULL},
    {67, 0x34db7681ULL}, {70, 0x1a3433baULL}, {72, 0x1fb17e7aULL},
    {82, 0x3cbd2736ULL}, {88, 0x2bebb78bULL}, {89, 0x2f249414ULL},
    {93, 0x17218675ULL}, {95, 0xcc88f67ULL}, {99, 0x30aaf70bULL},
    {109, 0x254afdc8ULL}, {111, 0x2bebb78bULL}, {112, 0x2f249414ULL},
    {116, 0x17218675ULL}, {118, 0xcc88f67ULL},
}};

#if defined(BIOSEQ_TEST_INDEXES)
constexpr std::array<std::pair<std::size_t, std::uint64_t>, 25>
k_wrap_expected{{
    {14, 0xeeULL}, {17, 0x57ULL}, {18, 0xeeULL}, {21, 0x4fULL},
    {21, 0x7bULL}, {23, 0xeaULL}, {23, 0xf5ULL}, {24, 0x3ULL},
    {25, 0x7bULL}, {35, 0xf5ULL}, {37, 0x7bULL}, {37, 0xeeULL},
    {40, 0x57ULL}, {41, 0x7bULL}, {41, 0xeeULL}, {42, 0xf5ULL},
    {43, 0x22ULL}, {44, 0x7bULL}, {46, 0xf5ULL}, {47, 0x22ULL},
    {47, 0x4bULL}, {48, 0x7bULL}, {48, 0xeaULL}, {60, 0x7bULL},
    {64, 0x7bULL},
}};
#endif

TEST_CASE("two_stack_nthash fixed output matches dual-padded input",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    const auto first = run_two_stack_pairs<std::uint32_t>(
        k_dual_padded, 10u, 15u);
    require_pairs(first, k_dual_padded_expected);
    REQUIRE(first
        == run_two_stack_pairs<std::uint32_t>(
            k_dual_padded, 10u, 15u));
}

TEST_CASE("two_stack_nthash fixed output matches single unpadded input",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    const auto first = run_two_stack_pairs<std::uint32_t>(
        k_single_no_pad, 10u, 15u);
    require_pairs(first, k_single_no_pad_expected);
    REQUIRE(first
        == run_two_stack_pairs<std::uint32_t>(
            k_single_no_pad, 10u, 15u));
}

#if defined(BIOSEQ_TEST_INDEXES)
TEST_CASE("two_stack_nthash fixed output matches wrapping input",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    const auto first = run_two_stack_pairs<std::uint8_t>(
        k_wrap_fixture, 10u, 4u);
    require_pairs(first, k_wrap_expected);
    REQUIRE(first == run_two_stack_pairs<std::uint8_t>(
        k_wrap_fixture, 10u, 4u));
}
#endif

} // namespace
