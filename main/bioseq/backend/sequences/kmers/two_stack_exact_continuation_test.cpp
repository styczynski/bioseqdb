// Exact chronological continuation coverage through the generated kernel's
// neutral fragment-source ABI.  No comparison in this file sorts or uniques:
// hash, position, strand, order, and duplicate multiplicity are all observable.

#include "common_tests.h"

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

import bioseq.common.sequan_compat;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.two_stacks;

namespace {

using bioseq::minimizers::MinimapSketchData;
using bioseq::minimizers::MinimizerKernelAlgorithm;
using bioseq::minimizers::MinimizerKernelInvocation;
using bioseq::minimizers::MinimizerKernelResult;
using bioseq::minimizers::MinimizerKernelStatus;
using bioseq::minimizers::MinimizerOutputLayout;
using bioseq::minimizers::MinimizerRepr;
using bioseq::minimizers::MinimizerSink;
using bioseq::minimizers::MinimizerSinkBatchView;
using bioseq::minimizers::PackedSequenceFragmentSource;
using bioseq::minimizers::PackedSequenceKernelFragment;
using bioseq::minimizers::PackedSequenceKernelView;
using bioseq::minimizers::TwoStackContinuationStatus;
using bioseq::minimizers::TwoStackExecutionProof;
using bioseq::minimizers::TwoStackRoute;
using bioseq::minimizers::two_stack_exact_max_kmer;
using bioseq::minimizers::two_stack_max_window;

struct exact_minimizer final {
    std::uint64_t hash = 0u;
    std::uint64_t position = 0u;
    std::uint64_t strand = 0u;
    [[nodiscard]] bool operator==(const exact_minimizer&) const = default;
};

template<class MinimizerT>
class exact_sink final {
public:
    static constexpr std::size_t batch_capacity = 256u;

    [[nodiscard]] MinimizerSink contract() noexcept {
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

    std::vector<MinimizerT> records;
    bool protocol_ok = true;
    bool reject = false;

private:
    [[nodiscard]] static MinimizerSinkBatchView acquire(
        void* opaque, const std::size_t minimum)
    {
        auto& self = *static_cast<exact_sink*>(opaque);
        if (self.reject || self.pending_ || minimum > batch_capacity) {
            self.protocol_ok = false;
            return {};
        }
        self.pending_ = true;
        return {
            .data = self.storage_.data(),
            .capacity = batch_capacity,
            .layout = bioseq::minimizers::minimizer_output_layout_v<MinimizerT>,
            .record_size = sizeof(MinimizerT),
            .record_alignment = alignof(MinimizerT),
        };
    }

    static void commit(void* opaque, const std::size_t initialized) noexcept {
        auto& self = *static_cast<exact_sink*>(opaque);
        if (!self.pending_ || initialized > batch_capacity) {
            self.protocol_ok = false;
            return;
        }
        auto* const first = std::launder(
            reinterpret_cast<MinimizerT*>(self.storage_.data()));
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
        const MinimizerOutputLayout layout,
        const std::uint64_t* raw,
        const std::size_t count) noexcept
    {
        auto& self = *static_cast<exact_sink*>(opaque);
        if (self.reject || self.pending_
            || layout
                != bioseq::minimizers::minimizer_output_layout_v<MinimizerT>
            || (count != 0u && raw == nullptr)) {
            self.protocol_ok = false;
            return false;
        }
        if constexpr (std::same_as<typename MinimizerT::storage_t,
                                   std::uint64_t>) {
            try {
                const auto decoded = std::span<const std::uint64_t>{raw, count}
                    | std::views::transform([](const std::uint64_t value) {
                        return MinimizerT::from_raw_storage(value);
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

    alignas(MinimizerT) std::array<
        std::byte, batch_capacity * sizeof(MinimizerT)> storage_;
    bool pending_ = false;
};

[[nodiscard]] const bioseq::simd_dispatch::HighwayKernelTable& fixed_table() {
    return bioseq::simd_dispatch::highway_test_kernel_table();
}

template<sequence_alphabet Alphabet>
[[nodiscard]] std::uint8_t alphabet_code() {
    const auto* identity = fixed_table().template resolve_alphabet_identity<
        Alphabet>();
    if (identity == nullptr) {
        throw std::logic_error{"fixed target lacks requested alphabet"};
    }
    return identity->alphabet_code;
}

template<class Range>
[[nodiscard]] PackedSequenceKernelView packed_view(const Range& range) {
    using traits = typename Range::traits;
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

template<class MinimizerT = MinimapSketchData>
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
    bioseq::minimizers::sketch_collector_t<MinimizerT> collector;
    bioseq::simd::collect_minimizers_view<
        bioseq::simd::minimap_int_hasher, MinimizerT>(
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

    [[nodiscard]] static PackedSequenceKernelFragment read(
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
    TwoStackExecutionProof proof{};
    MinimizerKernelResult kernel{};
    std::size_t releases = 0u;
    bool sink_ok = false;
};

template<class MinimizerT = MinimapSketchData>
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
    exact_sink<MinimizerT> sink;
    sink.reject = reject_sink;
    MinimizerKernelInvocation invocation{
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
            bioseq::minimizers::minimizer_output_layout_v<MinimizerT>,
        .output_hash_mask = static_cast<std::uint64_t>(
            MinimizerT::hash_field_mask),
        .output_position_mask = static_cast<std::uint64_t>(
            MinimizerT::pos_field_mask),
        .two_stack_route = TwoStackRoute::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto kernel = fixed_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    auto result = kernel(invocation);
    return {
        .records = sink.decoded(),
        .proof = result.proof.two_stack,
        .kernel = result,
        .releases = source.releases,
        .sink_ok = sink.protocol_ok,
    };
}

template<class MinimizerT = MinimapSketchData>
[[nodiscard]] run_result run_resident(
    const std::vector<seq::dna4>& data,
    const std::size_t window,
    const std::size_t kmer,
    const std::size_t position_base = 0u)
{
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<config> packed{std::vector<seq::dna4>(data)};
    exact_sink<MinimizerT> sink;
    MinimizerKernelInvocation invocation{
        .sequence = packed_view(packed),
        .window = window,
        .kmer = kmer,
        .emitted_position_base = position_base,
        .output_layout =
            bioseq::minimizers::minimizer_output_layout_v<MinimizerT>,
        .output_hash_mask = static_cast<std::uint64_t>(
            MinimizerT::hash_field_mask),
        .output_position_mask = static_cast<std::uint64_t>(
            MinimizerT::pos_field_mask),
        .two_stack_route = TwoStackRoute::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto kernel = fixed_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    auto result = kernel(invocation);
    return {
        .records = sink.decoded(),
        .proof = result.proof.two_stack,
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
    REQUIRE(result.kernel.status == MinimizerKernelStatus::ok);
    REQUIRE(result.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::ok));
    REQUIRE(result.kernel.proof.algorithm
        == MinimizerKernelAlgorithm::two_stack_nthash);
    REQUIRE(result.kernel.proof.target
        == bioseq::simd_dispatch::highway_test_target_binding().target);
    REQUIRE(result.sink_ok);
    REQUIRE(result.kernel.emitted == result.records.size());
    REQUIRE(result.records == expected);
    REQUIRE(result.proof.scalar_backend_branches == 0u);
    REQUIRE(result.proof.scalar_tail_symbols == 0u);
    REQUIRE(result.proof.scalar_hash_symbols == 0u);
    REQUIRE(result.proof.scalar_window_transitions == 0u);
    REQUIRE(result.proof.scalar_event_records == 0u);
    REQUIRE(result.proof.scalar_sort_records == 0u);
    REQUIRE(result.proof.scalar_dedup_records == 0u);
    REQUIRE(result.proof.fail_closed_rejections == 0u);
    REQUIRE(result.proof.accepted(require_vector_work));
}

TEST_CASE("two_stack continuation — exact runtime and constexpr contiguous parity",
          "[two_stack][exact][continuation][contiguous]") {
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
    REQUIRE(fragmented.proof.bp_consumed == data.size());
    REQUIRE(fragmented.proof.masked_tail_lanes > 0u);
}

TEST_CASE("two_stack continuation — every early split has exact ownership",
          "[two_stack][exact][continuation][split_sweep]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    const auto data = fixture(113u);
    const auto expected = scalar_oracle(data, w, k);
    for (std::size_t split = 0u; split <= k + w; ++split) {
        CAPTURE(split);
        const auto result = run_fragmented(
            data, adjacent_specs({{0u, split}, {split, data.size()}}), w, k);
        require_success(result, expected);
        REQUIRE(result.proof.bp_consumed == data.size());
        REQUIRE(result.releases == 2u);
    }
}

TEST_CASE("two_stack continuation — empty and one-symbol fragments",
          "[two_stack][exact][continuation][tiny_fragments]") {
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
    REQUIRE(result.proof.bp_consumed == data.size());
    REQUIRE(result.proof.fragment_symbols_staged == data.size());
    REQUIRE(result.releases == ranges.size());
}

TEST_CASE("two_stack continuation — randomized fragmentation exact parity",
          "[two_stack][exact][continuation][random_fragments]") {
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
        REQUIRE(result.proof.bp_consumed == data.size());
    }
}

TEST_CASE("two_stack continuation — bounded range preserves global coordinates",
          "[two_stack][exact][continuation][bounded_range]") {
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
    REQUIRE(whole.proof.bp_consumed == end - begin);

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
          "[two_stack][exact][continuation][ties][ownership]") {
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
    REQUIRE(result.proof.first_window_tie_events > 0u);
}

TEST_CASE("two_stack continuation — every short length and final flush",
          "[two_stack][exact][continuation][short][final_flush]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    for (std::size_t length = 0u; length <= k + w + 8u; ++length) {
        CAPTURE(length);
        const auto data = fixture(length);
        const auto expected = scalar_oracle(data, w, k);
        const auto result = run_fragmented(
            data, adjacent_specs({{0u, length}}), w, k);
        require_success(result, expected, length >= k);
        REQUIRE(result.proof.bp_consumed == length);
        REQUIRE(result.proof.final_flush_records
            == (expected.empty() ? 0u : 1u));
        if (length < k) {
            REQUIRE(result.records.empty());
            REQUIRE(result.proof.vector_batches == 0u);
            REQUIRE(result.proof.minimizers_emitted == 0u);
        }
    }
}

TEST_CASE("two_stack continuation — even-K palindromes consume sentinel slots",
          "[two_stack][exact][continuation][palindrome]") {
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
    REQUIRE(result.proof.palindrome_lanes > 0u);
}

TEST_CASE("two_stack continuation — equal entering multiplicity is preserved",
          "[two_stack][exact][continuation][ties][multiplicity]") {
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
    REQUIRE(result.proof.entering_min_transitions > 0u);
}

TEST_CASE("two_stack continuation — expiry rescans use rightmost minima",
          "[two_stack][exact][continuation][expiry]") {
    constexpr std::size_t w = 5u;
    constexpr std::size_t k = 7u;
    const auto data = fixture(1024u);
    const auto result = run_fragmented(
        data,
        adjacent_specs({{0u, 73u}, {73u, 190u}, {190u, 511u},
                        {511u, data.size()}}),
        w, k);
    require_success(result, scalar_oracle(data, w, k));
    REQUIRE(result.proof.expiry_rescan_transitions > 0u);
}

TEST_CASE("two_stack continuation — W and K supported boundaries",
          "[two_stack][exact][continuation][boundaries]") {
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

    SECTION("maximum supported W and K") {
        constexpr std::size_t w = two_stack_max_window;
        constexpr std::size_t k = two_stack_exact_max_kmer;
        const auto data = fixture(k + w + 41u);
        const auto result = run_fragmented(
            data,
            adjacent_specs({{0u, 37u}, {37u, 181u}, {181u, data.size()}}),
            w, k);
        require_success(result, scalar_oracle(data, w, k));
    }
}

TEST_CASE("two_stack continuation — unaligned packed source range",
          "[two_stack][exact][continuation][unaligned]") {
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
    REQUIRE(result.proof.packed_word_boundary_merges > 0u);
}

TEST_CASE("two_stack continuation — real hash sentinel is never emitted",
          "[two_stack][exact][continuation][sentinel]") {
    using representation = MinimizerRepr<33u, 15u>;
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
        == representation::hash_field_mask);
    const auto expected = scalar_oracle<representation>(data, 1u, 15u);
    const auto result = run_fragmented<representation>(
        data, adjacent_specs({{0u, 3u}, {3u, data.size()}}), 1u, 15u);
    require_success(result, expected);
    REQUIRE(result.records.empty());
    REQUIRE(result.proof.final_flush_records == 0u);
}

TEST_CASE("two_stack continuation — unsupported layout rejects before proof work",
          "[two_stack][exact][continuation][representation][neutral-boundary]") {
    const auto data = fixture(32u);
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<config> packed{std::vector<seq::dna4>(data)};
    exact_sink<MinimapSketchData> sink;
    MinimizerKernelInvocation invocation{
        .sequence = packed_view(packed),
        .window = 5u,
        .kmer = 7u,
        .output_layout = MinimizerOutputLayout::unsupported,
        .output_hash_mask = MinimapSketchData::hash_field_mask,
        .output_position_mask = MinimapSketchData::pos_field_mask,
        .two_stack_route = TwoStackRoute::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto kernel = fixed_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    REQUIRE(result.status == MinimizerKernelStatus::invalid_configuration);
    REQUIRE(result.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::unsupported_representation));
    REQUIRE(result.emitted == 0u);
    REQUIRE(sink.records.empty());
    REQUIRE(result.proof.two_stack == TwoStackExecutionProof{});
}

} // namespace
