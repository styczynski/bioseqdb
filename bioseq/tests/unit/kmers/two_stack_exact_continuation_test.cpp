/**
 * @file two_stack_exact_continuation_test.cpp
 * @brief Chronological continuation checks for fragmented two-stack input.
 *
 * Hash, position, strand, order, and duplicate multiplicity remain observable.
 */

#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.two_stack_minimizers;

namespace {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::minimizer_kernel_algorithm;
using bioseq::minimizers::minimizer_kernel_invocation;
using bioseq::minimizers::minimizer_kernel_result;
using bioseq::minimizers::minimizer_kernel_status;
using bioseq::minimizers::minimizer_output_layout;
using bioseq::minimizers::minimizer_representation;
using bioseq::minimizers::minimizer_sink;
using bioseq::minimizers::minimizer_sink_batch_view;
using bioseq::minimizers::packed_sequence_fragment_source;
using bioseq::minimizers::packed_sequence_kernel_fragment;
using bioseq::minimizers::packed_sequence_kernel_view;
using bioseq::minimizers::two_stack_continuation_status;
using bioseq::minimizers::two_stack_execution_stats;
using bioseq::minimizers::two_stack_route;
using bioseq::minimizers::k_max_exact_two_stack_kmer;
using bioseq::minimizers::k_max_two_stack_window;

struct exact_minimizer final {
    std::uint64_t hash = 0u;
    std::uint64_t position = 0u;
    std::uint64_t strand = 0u;
    [[nodiscard]] bool operator==(const exact_minimizer&) const = default;
};

template<class p_minimizer_type>
class exact_sink final {
public:
    static constexpr std::size_t k_batch_capacity = 256u;

    [[nodiscard]] minimizer_sink contract() noexcept {
        return {
            .state = this,
            .acquire = &acquire,
            .commit = &commit,
            .append_raw_u64 = &append_raw_u64,
        };
    }

    [[nodiscard]] std::vector<exact_minimizer> decoded() const {
        std::vector<exact_minimizer> result;
        result.reserve(records.size());
        for (const auto& record : records) {
            result.push_back({record.kmer(), record.last_pos(), record.strand()});
        }
        return result;
    }

    std::vector<p_minimizer_type> records;
    bool protocol_ok = true;
    bool reject = false;

private:
    [[nodiscard]] static minimizer_sink_batch_view acquire(
        void* opaque, const std::size_t minimum)
    {
        auto& self = *static_cast<exact_sink*>(opaque);
        if (self.reject || self.pending_ || minimum > k_batch_capacity) {
            self.protocol_ok = false;
            return {};
        }
        self.pending_ = true;
        return {
            .data = self.storage_.data(),
            .capacity = k_batch_capacity,
            .layout = bioseq::minimizers::k_minimizer_output_layout<p_minimizer_type>,
            .record_size = sizeof(p_minimizer_type),
            .record_alignment = alignof(p_minimizer_type),
        };
    }

    static void commit(void* opaque, const std::size_t initialized) noexcept {
        auto& self = *static_cast<exact_sink*>(opaque);
        if (!self.pending_ || initialized > k_batch_capacity) {
            self.protocol_ok = false;
            return;
        }
        auto* const first = std::launder(
            reinterpret_cast<p_minimizer_type*>(self.storage_.data()));
        try {
            if (self.reject) throw std::bad_alloc{};
            self.records.insert(self.records.end(), first, first + initialized);
        } catch (...) {
            self.protocol_ok = false;
        }
        std::destroy_n(first, initialized);
        self.pending_ = false;
    }

    [[nodiscard]] static bool append_raw_u64(
        void* opaque,
        const minimizer_output_layout layout,
        const std::uint64_t* raw,
        const std::size_t count) noexcept
    {
        auto& self = *static_cast<exact_sink*>(opaque);
        if (self.reject || self.pending_
            || layout
                != bioseq::minimizers::k_minimizer_output_layout<p_minimizer_type>
            || (count != 0u && raw == nullptr)) {
            self.protocol_ok = false;
            return false;
        }
        if constexpr (std::same_as<typename p_minimizer_type::storage_type,
                                   std::uint64_t>) {
            try {
                const auto decoded = std::span<const std::uint64_t>{raw, count}
                    | std::views::transform([](const std::uint64_t value) {
                        return p_minimizer_type::from_raw_storage(value);
                    });
                self.records.insert(
                    self.records.end(), decoded.begin(), decoded.end());
                return true;
            } catch (...) {
                self.protocol_ok = false;
                return false;
            }
        } else {
            self.protocol_ok = false;
            return false;
        }
    }

    alignas(p_minimizer_type) std::array<
        std::byte, k_batch_capacity * sizeof(p_minimizer_type)> storage_;
    bool pending_ = false;
};

[[nodiscard]] const bioseq::simd_dispatch::highway_kernel_table& fixed_table() {
    return bioseq::simd_dispatch::highway_test_kernel_table();
}

template<sequence_alphabet p_alphabet>
[[nodiscard]] std::uint8_t alphabet_code() {
    const auto* identity = fixed_table().template resolve_alphabet_identity<
        p_alphabet>();
    if (identity == nullptr) {
        throw std::logic_error{"fixed target lacks requested alphabet"};
    }
    return identity->alphabet_code;
}

template<class p_range>
[[nodiscard]] packed_sequence_kernel_view packed_view(const p_range& range) {
    using traits = typename p_range::traits;
    return {
        .storage = range.size() == 0u ? nullptr
            : reinterpret_cast<const std::byte*>(
                range.read_cptr(range.cbegin())),
        .storage_units = static_cast<std::size_t>(range.size()),
        .begin_symbol = 0u,
        .end_symbol = static_cast<std::size_t>(range.bp_size()),
        .alphabet_code = alphabet_code<typename traits::alphabet_type>(),
        .bits_per_symbol = static_cast<std::uint8_t>(traits::bits_per_char),
        .storage_unit_bytes = static_cast<std::uint8_t>(
            sizeof(typename traits::storage_unit)),
        .final_segment = true,
    };
}

[[nodiscard]] std::vector<seq::dna4> fixture(const std::size_t size) {
    std::vector<seq::dna4> result(size);
    std::uint32_t state = 0x9E3779B9u;
    for (auto& symbol : result) {
        state = state * 1664525u + 1013904223u;
        symbol.assign_rank(static_cast<std::uint8_t>((state >> 29u) & 3u));
    }
    return result;
}

template<class p_minimizer_type = minimap_sketch_data>
[[nodiscard]] std::vector<exact_minimizer> scalar_oracle(
    const std::vector<seq::dna4>& data,
    const std::size_t window,
    const std::size_t kmer,
    const std::size_t begin = 0u,
    const std::size_t end = (std::numeric_limits<std::size_t>::max)(),
    const std::size_t position_base = 0u)
{
    const std::size_t bounded_end = end == (std::numeric_limits<std::size_t>::max)()
        ? data.size() : end;
    REQUIRE(begin <= bounded_end);
    REQUIRE(bounded_end <= data.size());
    if (begin == bounded_end) return {};
    std::vector<seq::dna4> slice(
        data.begin() + static_cast<std::ptrdiff_t>(begin),
        data.begin() + static_cast<std::ptrdiff_t>(bounded_end));
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<config> range(slice);
    bioseq::minimizers::basic_sketch_collector<p_minimizer_type> collector;
    bioseq::simd::collect_minimizers_view<
        bioseq::simd::minimap_int_hasher, p_minimizer_type>(
            collector, range, range.begin(), range.end(), window, kmer);
    std::vector<exact_minimizer> result;
    for (const auto& record : collector.extract_sketches_data()) {
        result.push_back({
            record.kmer(), record.last_pos() + position_base, record.strand()});
    }
    return result;
}

struct fragment_spec final {
    std::size_t storage_begin = 0u;
    std::size_t storage_end = 0u;
    std::size_t fragment_begin = 0u;
    std::size_t consume_begin = 0u;
    std::size_t consume_end = 0u;
};

struct owned_fragment final {
    std::vector<std::uint64_t> units;
    std::size_t bp_size = 0u;
    std::size_t fragment_begin = 0u;
    std::size_t consume_begin = 0u;
    std::size_t consume_end = 0u;
};

struct fragment_source final {
    std::vector<owned_fragment> fragments;
    std::size_t releases = 0u;

    [[nodiscard]] static packed_sequence_kernel_fragment read(
        void* opaque, const std::size_t index)
    {
        const auto& value = static_cast<fragment_source*>(opaque)->fragments.at(
            index);
        return {
            .storage = value.units.empty() ? nullptr
                : reinterpret_cast<const std::byte*>(value.units.data()),
            .storage_units = value.units.size(),
            .fragment_bp_size = value.bp_size,
            .fragment_begin_bp = value.fragment_begin,
            .consume_begin_bp = value.consume_begin,
            .consume_end_bp = value.consume_end,
        };
    }

    static void release(void* opaque, std::size_t) noexcept {
        ++static_cast<fragment_source*>(opaque)->releases;
    }
};

[[nodiscard]] owned_fragment make_fragment(
    const std::vector<seq::dna4>& data, const fragment_spec spec)
{
    REQUIRE(spec.storage_begin <= spec.storage_end);
    REQUIRE(spec.storage_end <= data.size());
    std::vector<seq::dna4> slice(
        data.begin() + static_cast<std::ptrdiff_t>(spec.storage_begin),
        data.begin() + static_cast<std::ptrdiff_t>(spec.storage_end));
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<config> packed(slice);
    owned_fragment result{
        .bp_size = slice.size(),
        .fragment_begin = spec.fragment_begin,
        .consume_begin = spec.consume_begin,
        .consume_end = spec.consume_end,
    };
    if (packed.size() != 0u) {
        const auto* first = packed.read_cptr(packed.cbegin());
        result.units.assign(first, first + packed.size());
    }
    return result;
}

struct run_result final {
    std::vector<exact_minimizer> records;
    two_stack_execution_stats stats{};
    minimizer_kernel_result kernel{};
    std::size_t releases = 0u;
    bool sink_ok = false;
};

template<class p_minimizer_type = minimap_sketch_data>
[[nodiscard]] run_result run_fragmented(
    const std::vector<seq::dna4>& data,
    const std::vector<fragment_spec>& specs,
    const std::size_t window,
    const std::size_t kmer,
    const std::size_t position_base = 0u,
    const bool reject_sink = false)
{
    fragment_source source;
    source.fragments.reserve(specs.size());
    for (const auto spec : specs) {
        source.fragments.push_back(make_fragment(data, spec));
    }
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using traits = compact_sequence_range<config>::traits;
    exact_sink<p_minimizer_type> sink;
    sink.reject = reject_sink;
    minimizer_kernel_invocation invocation{
        .sequence = {
            .storage = nullptr,
            .storage_units = (data.size() + traits::letters_per_unit - 1u)
                / traits::letters_per_unit,
            .begin_symbol = 0u,
            .end_symbol = data.size(),
            .alphabet_code = alphabet_code<seq::dna4>(),
            .bits_per_symbol = static_cast<std::uint8_t>(traits::bits_per_char),
            .storage_unit_bytes = sizeof(std::uint64_t),
            .final_segment = true,
        },
        .fragments = {
            .state = &source,
            .first_fragment = 0u,
            .fragment_end = source.fragments.size(),
            .read = &fragment_source::read,
            .release = &fragment_source::release,
        },
        .window = window,
        .kmer = kmer,
        .emitted_position_base = position_base,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<p_minimizer_type>,
        .output_hash_mask = static_cast<std::uint64_t>(
            p_minimizer_type::k_hash_field_mask),
        .output_position_mask = static_cast<std::uint64_t>(
            p_minimizer_type::k_position_field_mask),
        .two_stack_route = two_stack_route::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto kernel = fixed_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    auto result = kernel(invocation);
    return {
        .records = sink.decoded(),
        .stats = result.stats.two_stack,
        .kernel = result,
        .releases = source.releases,
        .sink_ok = sink.protocol_ok,
    };
}

template<class p_minimizer_type = minimap_sketch_data>
[[nodiscard]] run_result run_resident(
    const std::vector<seq::dna4>& data,
    const std::size_t window,
    const std::size_t kmer,
    const std::size_t position_base = 0u)
{
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<config> packed{std::vector<seq::dna4>(data)};
    exact_sink<p_minimizer_type> sink;
    minimizer_kernel_invocation invocation{
        .sequence = packed_view(packed),
        .window = window,
        .kmer = kmer,
        .emitted_position_base = position_base,
        .output_layout =
            bioseq::minimizers::k_minimizer_output_layout<p_minimizer_type>,
        .output_hash_mask = static_cast<std::uint64_t>(
            p_minimizer_type::k_hash_field_mask),
        .output_position_mask = static_cast<std::uint64_t>(
            p_minimizer_type::k_position_field_mask),
        .two_stack_route = two_stack_route::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto kernel = fixed_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    auto result = kernel(invocation);
    return {
        .records = sink.decoded(),
        .stats = result.stats.two_stack,
        .kernel = result,
        .sink_ok = sink.protocol_ok,
    };
}

[[nodiscard]] std::vector<fragment_spec> adjacent_specs(
    const std::vector<std::pair<std::size_t, std::size_t>>& ranges)
{
    std::vector<fragment_spec> result;
    result.reserve(ranges.size());
    for (const auto [begin, end] : ranges) {
        result.push_back({begin, end, begin, begin, end});
    }
    return result;
}

void require_success(
    const run_result& result,
    const std::vector<exact_minimizer>& expected,
    const bool require_vector_work = true)
{
    REQUIRE(result.kernel.status == minimizer_kernel_status::ok);
    REQUIRE(result.kernel.family_status == static_cast<std::uint32_t>(
        two_stack_continuation_status::ok));
    REQUIRE(result.kernel.stats.algorithm
        == minimizer_kernel_algorithm::two_stack_nthash);
    REQUIRE(result.kernel.stats.target
        == bioseq::simd_dispatch::highway_test_target_binding().target);
    REQUIRE(result.sink_ok);
    REQUIRE(result.kernel.emitted == result.records.size());
    REQUIRE(result.records == expected);
    REQUIRE(result.stats.scalar_backend_branches == 0u);
    REQUIRE(result.stats.scalar_tail_symbols == 0u);
    REQUIRE(result.stats.scalar_hash_symbols == 0u);
    REQUIRE(result.stats.scalar_window_transitions == 0u);
    REQUIRE(result.stats.scalar_event_records == 0u);
    REQUIRE(result.stats.scalar_sort_records == 0u);
    REQUIRE(result.stats.scalar_dedup_records == 0u);
    REQUIRE(result.stats.fail_closed_rejections == 0u);
    REQUIRE(result.stats.is_internally_consistent(require_vector_work));
}

TEST_CASE("two_stack continuation — exact runtime and constexpr contiguous parity",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    const auto data = fixture(197u);
    const auto expected = scalar_oracle(data, w, k);
    const auto resident = run_resident(data, w, k);
    const auto fragmented = run_fragmented(
        data, adjacent_specs({{0u, data.size()}}), w, k);
    require_success(resident, expected);
    require_success(fragmented, expected);
    REQUIRE(resident.records == fragmented.records);
    REQUIRE(fragmented.stats.bp_consumed == data.size());
    REQUIRE(fragmented.stats.masked_tail_lanes > 0u);
}

TEST_CASE("two_stack continuation — every early split has exact ownership",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    const auto data = fixture(113u);
    const auto expected = scalar_oracle(data, w, k);
    for (std::size_t split = 0u; split <= k + w; ++split) {
        CAPTURE(split);
        const auto result = run_fragmented(
            data, adjacent_specs({{0u, split}, {split, data.size()}}), w, k);
        require_success(result, expected);
        REQUIRE(result.stats.bp_consumed == data.size());
        REQUIRE(result.releases == 2u);
    }
}

TEST_CASE("two_stack continuation — empty and one-symbol fragments",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    const auto data = fixture(79u);
    std::vector<std::pair<std::size_t, std::size_t>> ranges{{0u, 0u}};
    for (std::size_t index = 0u; index < data.size(); ++index) {
        ranges.emplace_back(index, index + 1u);
        if (index % 7u == 0u) ranges.emplace_back(index + 1u, index + 1u);
    }
    const auto result = run_fragmented(data, adjacent_specs(ranges), w, k);
    require_success(result, scalar_oracle(data, w, k));
    REQUIRE(result.stats.bp_consumed == data.size());
    REQUIRE(result.stats.fragment_symbols_staged == data.size());
    REQUIRE(result.releases == ranges.size());
}

TEST_CASE("two_stack continuation — randomized fragmentation exact parity",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    const auto data = fixture(263u);
    const auto expected = scalar_oracle(data, w, k);
    for (std::uint32_t seed = 1u; seed <= 16u; ++seed) {
        CAPTURE(seed);
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        std::size_t begin = 0u;
        std::uint32_t rng = seed * 0x45D9F3Bu;
        while (begin < data.size()) {
            rng = rng * 1664525u + 1013904223u;
            const std::size_t end = std::min(
                data.size(), begin + 1u + ((rng >> 24u) % 31u));
            ranges.emplace_back(begin, end);
            if ((rng & 3u) == 0u) ranges.emplace_back(end, end);
            begin = end;
        }
        const auto result = run_fragmented(data, adjacent_specs(ranges), w, k);
        require_success(result, expected);
        REQUIRE(result.stats.bp_consumed == data.size());
    }
}

TEST_CASE("two_stack continuation — bounded range preserves global coordinates",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    constexpr std::size_t begin = 64u;
    constexpr std::size_t end = 224u;
    constexpr std::size_t base = 9000u;
    const auto data = fixture(288u);
    const auto expected = scalar_oracle(data, w, k, begin, end, base);
    const std::vector<fragment_spec> bounded{{
        .storage_begin = 0u,
        .storage_end = data.size(),
        .fragment_begin = 0u,
        .consume_begin = begin,
        .consume_end = end,
    }};
    const auto whole = run_fragmented(data, bounded, w, k, base);
    require_success(whole, expected);
    REQUIRE(whole.stats.bp_consumed == end - begin);

    std::vector<seq::dna4> slice(
        data.begin() + static_cast<std::ptrdiff_t>(begin),
        data.begin() + static_cast<std::ptrdiff_t>(end));
    const auto split = run_fragmented(
        slice,
        adjacent_specs({{0u, 37u}, {37u, 91u}, {91u, slice.size()}}),
        w, k, base);
    require_success(split, expected);
}

TEST_CASE("two_stack continuation — tie minimum crossing split is owned once",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    std::vector<seq::dna4> data(96u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        const auto rank = index >= 38u && index < 57u
            ? 3u : static_cast<std::uint8_t>((index / 16u) & 1u);
        data[index].assign_rank(rank);
    }
    const auto result = run_fragmented(
        data, adjacent_specs({{0u, 48u}, {48u, data.size()}}), w, k);
    require_success(result, scalar_oracle(data, w, k));
    REQUIRE(result.stats.first_window_tie_events > 0u);
}

TEST_CASE("two_stack continuation — every short length and final flush",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    for (std::size_t length = 0u; length <= k + w + 8u; ++length) {
        CAPTURE(length);
        const auto data = fixture(length);
        const auto expected = scalar_oracle(data, w, k);
        const auto result = run_fragmented(
            data, adjacent_specs({{0u, length}}), w, k);
        require_success(result, expected, length >= k);
        REQUIRE(result.stats.bp_consumed == length);
        REQUIRE(result.stats.final_flush_records
            == (expected.empty() ? 0u : 1u));
        if (length < k) {
            REQUIRE(result.records.empty());
            REQUIRE(result.stats.vector_batches == 0u);
            REQUIRE(result.stats.minimizers_emitted == 0u);
        }
    }
}

TEST_CASE("two_stack continuation — even k-mer palindromes consume sentinel slots",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 3u;
    constexpr std::size_t k = 4u;
    std::vector<seq::dna4> data(96u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index & 3u));
    }
    const auto result = run_fragmented(
        data,
        adjacent_specs({{0u, 1u}, {1u, 9u}, {9u, 10u}, {10u, 47u},
                        {47u, data.size()}}),
        w, k);
    require_success(result, scalar_oracle(data, w, k));
    REQUIRE(result.stats.palindrome_lanes > 0u);
}

TEST_CASE("two_stack continuation — equal entering multiplicity is preserved",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    std::vector<seq::dna4> data(64u);
    for (auto& symbol : data) symbol.assign_rank(0u);
    const auto result = run_fragmented(
        data,
        adjacent_specs({{0u, 11u}, {11u, 12u}, {12u, 31u},
                        {31u, data.size()}}),
        w, k);
    require_success(result, scalar_oracle(data, w, k));
    bool duplicate = false;
    for (std::size_t index = 1u; index < result.records.size(); ++index) {
        duplicate = duplicate || result.records[index] == result.records[index - 1u];
    }
    REQUIRE(duplicate);
    REQUIRE(result.stats.entering_min_transitions > 0u);
}

TEST_CASE("two_stack continuation — expiry rescans use rightmost minima",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    const auto data = fixture(1024u);
    const auto result = run_fragmented(
        data,
        adjacent_specs({{0u, 73u}, {73u, 190u}, {190u, 511u},
                        {511u, data.size()}}),
        w, k);
    require_success(result, scalar_oracle(data, w, k));
    REQUIRE(result.stats.expiry_rescan_transitions > 0u);
}

TEST_CASE("two_stack continuation — window and k-mer supported boundaries",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    SECTION("W1 K1") {
        constexpr std::size_t w = 1u;
        constexpr std::size_t k = 1u;
        const auto data = fixture(97u);
        const auto result = run_fragmented(
            data,
            adjacent_specs({{0u, 13u}, {13u, 64u}, {64u, data.size()}}),
            w, k);
        require_success(result, scalar_oracle(data, w, k));
    }

    SECTION("maximum supported window and k-mer sizes") {
        constexpr std::size_t w = k_max_two_stack_window;
        constexpr std::size_t k = k_max_exact_two_stack_kmer;
        const auto data = fixture(k + w + 41u);
        const auto result = run_fragmented(
            data,
            adjacent_specs({{0u, 37u}, {37u, 181u}, {181u, data.size()}}),
            w, k);
        require_success(result, scalar_oracle(data, w, k));
    }
}

TEST_CASE("two_stack continuation — unaligned packed source range",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    constexpr std::size_t begin = 5u;
    constexpr std::size_t end = 173u;
    constexpr std::size_t base = 7001u;
    const auto data = fixture(211u);
    const std::vector<fragment_spec> specs{{
        .storage_begin = 0u,
        .storage_end = data.size(),
        .fragment_begin = 0u,
        .consume_begin = begin,
        .consume_end = end,
    }};
    const auto result = run_fragmented(data, specs, w, k, base);
    require_success(result, scalar_oracle(data, w, k, begin, end, base));
    REQUIRE(result.stats.packed_word_boundary_merges > 0u);
}

TEST_CASE("two_stack continuation — real hash sentinel is never emitted",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using representation = minimizer_representation<33u, 15u>;
    constexpr std::string_view sentinel_kmer = "AACATATAAATTCAA";
    std::vector<seq::dna4> data;
    data.reserve(sentinel_kmer.size());
    std::uint32_t forward = 0u;
    std::uint32_t reverse = 0u;
    for (std::size_t index = 0u; index < sentinel_kmer.size(); ++index) {
        seq::dna4 symbol{};
        symbol.assign_char(sentinel_kmer[index]);
        data.push_back(symbol);
        const std::uint32_t code = symbol.to_rank();
        forward = ((forward << 2u) | code) & 0x3fffffffu;
        reverse |= (code ^ 3u) << (2u * index);
    }
    const std::uint32_t canonical = std::min(forward, reverse);
    REQUIRE((bioseq::simd::minimap_int_hasher<seq::dna4>::template
        scramble<std::uint32_t>(canonical, std::uint32_t{0x3fffffffu}))
        == representation::k_hash_field_mask);
    const auto expected = scalar_oracle<representation>(data, 1u, 15u);
    const auto result = run_fragmented<representation>(
        data, adjacent_specs({{0u, 3u}, {3u, data.size()}}), 1u, 15u);
    require_success(result, expected);
    REQUIRE(result.records.empty());
    REQUIRE(result.stats.final_flush_records == 0u);
}

TEST_CASE("two_stack continuation — unsupported layout rejects before stats work",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    const auto data = fixture(32u);
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<config> packed{std::vector<seq::dna4>(data)};
    exact_sink<minimap_sketch_data> sink;
    minimizer_kernel_invocation invocation{
        .sequence = packed_view(packed),
        .window = 5u,
        .kmer = 7u,
        .output_layout = minimizer_output_layout::unsupported,
        .output_hash_mask = minimap_sketch_data::k_hash_field_mask,
        .output_position_mask = minimap_sketch_data::k_position_field_mask,
        .two_stack_route = two_stack_route::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto kernel = fixed_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    REQUIRE(result.status == minimizer_kernel_status::invalid_configuration);
    REQUIRE(result.family_status == static_cast<std::uint32_t>(
        two_stack_continuation_status::unsupported_representation));
    REQUIRE(result.emitted == 0u);
    REQUIRE(sink.records.empty());
    REQUIRE(result.stats.two_stack == two_stack_execution_stats{});
}

} // namespace
