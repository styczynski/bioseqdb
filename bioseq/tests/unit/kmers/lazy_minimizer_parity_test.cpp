/**
 * @file lazy_minimizer_parity_test.cpp
 * @brief Minimizer parity across resident and fragmented sequence views.
 *
 * Fixtures cover several sequence lengths and fragment geometries.
 */

#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.batch_sort;
import bioseq.storage.size;
import bioseq.sequences.simd_iteration_geometry;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

namespace hn = hwy::HWY_NAMESPACE;
namespace pipe = ::bioseq::simd::pipe;
namespace vk = ::bioseq::gpu::vulkan;

[[nodiscard]] const ::bioseq::simd_dispatch::highway_kernel_table*
fixed_highway_kernels() noexcept {
    return &::bioseq::simd_dispatch::highway_test_kernel_table();
}

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_type  = compact_sequence_range<dna4_u64>;
using facade_type = bioseq::storage::lazy_sequence_view<seq::dna4>;
using sequence_size_type = bioseq::storage::sequence_size_t;

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed) {
    std::vector<seq::dna4> v(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>((s >> 33) & 0x3));
        v[i] = c;
    }
    return v;
}

struct source_piece {
    std::size_t source_index{};
    sequence_size_type from{};
    sequence_size_type to{};
    bool reversed{};
    bool complemented{};
};

struct composite_source_state {
    std::vector<std::vector<std::uint8_t>> sources;
    std::vector<source_piece> pieces;
    sequence_size_type logical_size{};
    mutable std::size_t callback_count{};
    mutable sequence_size_type requested_bp{};
    mutable std::size_t max_request{};
    bool reject_whole_request{true};
};

[[nodiscard]] std::uint8_t composite_rank_at(const composite_source_state& state,
                                             sequence_size_type logical_position) {
    for (const source_piece& piece : state.pieces) {
        const sequence_size_type piece_size = piece.to - piece.from;
        if (logical_position >= piece_size) {
            logical_position -= piece_size;
            continue;
        }
        const sequence_size_type source_position =
            piece.reversed ? piece.to - 1u - logical_position : piece.from + logical_position;
        std::uint8_t rank = state.sources[piece.source_index]
            [static_cast<std::size_t>(source_position)];
        if (piece.complemented) {
            rank = static_cast<std::uint8_t>(3u - rank);
        }
        return rank;
    }
    throw std::out_of_range("composite source logical position");
}

template <bool p_is_reversed, bool p_write_reversed>
[[nodiscard]] const std::uint8_t* composite_read_at(const void* opaque,
                                                    sequence_size_type bp_from,
                                                    std::size_t required_length,
                                                    std::span<std::uint8_t> scratch) {
    const auto& state = *static_cast<const composite_source_state*>(opaque);
    if (state.reject_whole_request
        && required_length == static_cast<std::size_t>(state.logical_size)) {
        throw std::runtime_error(
            "poisoned whole logical source materialization");
    }
    ++state.callback_count;
    state.requested_bp += required_length;
    state.max_request = std::max(state.max_request, required_length);
    if (required_length == 0u) {
        return scratch.data();
    }
    const sequence_size_type forward_from =
        p_is_reversed ? state.logical_size - (bp_from + required_length) : bp_from;
    constexpr bool descending_write =
        (p_is_reversed && !p_write_reversed) || (!p_is_reversed && p_write_reversed);
    for (std::size_t index = 0u; index < required_length; ++index) {
        std::uint8_t rank = composite_rank_at(state, forward_from + index);
        if constexpr (p_is_reversed) {
            rank = static_cast<std::uint8_t>(3u - rank);
        }
        scratch[descending_write ? required_length - 1u - index : index] = rank;
    }
    return scratch.data();
}

[[nodiscard]] sequence_size_type composite_bp_size(const void* opaque) noexcept {
    return static_cast<const composite_source_state*>(opaque)->logical_size;
}

[[nodiscard]] facade_type::resident_packed_result composite_resident_probe(const void*,
                                                                           sequence_size_type,
                                                                           sequence_size_type,
                                                                           std::uint8_t) noexcept {
    return std::nullopt;
}

[[nodiscard]] bool composite_worker_shareable(const void*) noexcept {
    return false;
}

inline const facade_type::backend_ops composite_ops{
    .bp_size = &composite_bp_size,
    .read_at =
        {
            &composite_read_at<false, false>,
            &composite_read_at<false, true>,
            &composite_read_at<true, false>,
            &composite_read_at<true, true>,
        },
    .try_resident_packed = &composite_resident_probe,
    .copy_packed_sequence = nullptr,
    .worker_shareable = &composite_worker_shareable,
};

[[nodiscard]] facade_type make_composite_source_view(const composite_source_state& state) noexcept {
    return facade_type{std::addressof(state), std::addressof(composite_ops)};
}

void reset_source_observation(composite_source_state& state) noexcept {
    state.callback_count = 0u;
    state.requested_bp = 0u;
    state.max_request = 0u;
}

[[nodiscard]] std::vector<seq::dna4> materialize_logical_letters(
        const composite_source_state& state) {
    std::vector<seq::dna4> letters(static_cast<std::size_t>(state.logical_size));
    for (sequence_size_type position = 0u; position < state.logical_size; ++position) {
        letters[static_cast<std::size_t>(position)].assign_rank(
            composite_rank_at(state, position));
    }
    return letters;
}

struct composite_fixture {
    composite_source_state source;
    std::vector<seq::dna4> selected_letters;
    sequence_size_type selected_from{};
    sequence_size_type selected_to{};
};

[[nodiscard]] composite_fixture make_composite_fixture() {
    composite_source_state source;
    source.sources.resize(2u);
    const auto first = make_dna4(12'000u, 0x377001u);
    const auto second = make_dna4(12'000u, 0x377002u);
    source.sources[0].reserve(first.size());
    source.sources[1].reserve(second.size());
    for (const seq::dna4 letter : first) {
        source.sources[0].push_back(
            static_cast<std::uint8_t>(letter.to_rank()));
    }
    for (const seq::dna4 letter : second) {
        source.sources[1].push_back(
            static_cast<std::uint8_t>(letter.to_rank()));
    }
    source.pieces = {
        {0u, 100u, 3'100u, false, false},
        {1u, 400u, 3'400u, true, false},
        {0u, 3'200u, 6'200u, false, true},
        {1u, 3'600u, 6'600u, true, true},
        {0u, 6'500u, 9'500u, true, false},
        {1u, 6'800u, 9'800u, false, false},
    };
    for (const source_piece& piece : source.pieces) {
        source.logical_size += piece.to - piece.from;
    }
    const auto logical = materialize_logical_letters(source);
    constexpr sequence_size_type selected_from = 2'500u;
    constexpr sequence_size_type selected_to = 10'500u;
    std::vector<seq::dna4> selected(
        logical.begin() + static_cast<std::ptrdiff_t>(selected_from),
        logical.begin() + static_cast<std::ptrdiff_t>(selected_to));
    return {
        .source = std::move(source),
        .selected_letters = std::move(selected),
        .selected_from = selected_from,
        .selected_to = selected_to,
    };
}

using minimizer_record =
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

[[nodiscard]] std::vector<minimizer_record> raw_records(
        const sketch_collector& collector) {
    std::vector<minimizer_record> records;
    records.reserve(collector.extract_sketches_data().size());
    for (const minimap_sketch_data& minimizer :
         collector.extract_sketches_data()) {
        records.emplace_back(
            minimizer.last_pos(), minimizer.kmer(), minimizer.strand());
    }
    return records;
}

[[nodiscard]] std::vector<minimizer_record> canonical_records(
        const sketch_collector& collector) {
    auto records = raw_records(collector);
    std::sort(records.begin(), records.end());
    return records;
}

inline constexpr std::array<std::uint32_t, 4u>
bounded_nthash_oracle_forward_table{
    0x95c60474u,
    0x62a02b4cu,
    0x82572324u,
    0x4be24456u,
};

struct bounded_nthash_hash_and_strand {
    std::uint32_t hash{};
    std::uint32_t strand{};
};

template <std::size_t p_kmer_size>
[[nodiscard]] bounded_nthash_hash_and_strand
bounded_nthash_oracle_hash_and_strand(
        std::span<const std::uint32_t> ranks,
        std::size_t start) {
    std::uint32_t forward = 0u;
    std::uint32_t reverse = 0u;
    for (std::size_t offset = 0u; offset < p_kmer_size; ++offset) {
        const std::uint32_t rank = ranks[start + offset];
        if (rank >= 4u) {
            throw std::invalid_argument(
                "bounded ntHash oracle accepts DNA4 ranks only");
        }
        forward ^= std::rotl(
            bounded_nthash_oracle_forward_table[rank],
            static_cast<int>(((p_kmer_size - 1u - offset) * 7u) & 31u));
        reverse ^= std::rotl(
            bounded_nthash_oracle_forward_table[3u - rank],
            static_cast<int>((offset * 7u) & 31u));
    }
    return {
        .hash = static_cast<std::uint32_t>(forward + reverse),
        .strand = reverse < forward ? 1u : 0u,
    };
}

template <std::size_t p_kmer_size, std::size_t p_window_size>
[[nodiscard]] std::vector<minimizer_record> bounded_nthash_scalar_oracle(
        std::span<const std::uint32_t> ranks) {
    if (ranks.size() < p_kmer_size + p_window_size - 1u) {
        return {};
    }
    const std::size_t kmer_count = ranks.size() - p_kmer_size + 1u;
    const std::size_t window_count = kmer_count - p_window_size + 1u;
    std::vector<bounded_nthash_hash_and_strand> kmers(kmer_count);
    for (std::size_t position = 0u; position < kmer_count; ++position) {
        kmers[position] =
            bounded_nthash_oracle_hash_and_strand<p_kmer_size>(
                ranks, position);
    }

    std::vector<minimizer_record> records;
    records.reserve(window_count * 2u);
    for (std::size_t window = 0u; window < window_count; ++window) {
        std::uint32_t minimum_hash =
            (std::numeric_limits<std::uint32_t>::max)();
        std::size_t left_position = window;
        std::size_t right_position = window;
        for (std::size_t offset = 0u; offset < p_window_size; ++offset) {
            const std::size_t position = window + offset;
            if (kmers[position].hash < minimum_hash) {
                minimum_hash = kmers[position].hash;
                left_position = position;
                right_position = position;
            } else if (kmers[position].hash == minimum_hash) {
                right_position = position;
            }
        }
        const auto append =
            [&records, &kmers, minimum_hash](std::size_t position) {
            const minimizer_record record{
                position + p_kmer_size - 1u,
                minimum_hash,
                kmers[position].strand,
            };
            if (records.empty() || records.back() != record) {
                records.push_back(record);
            }
        };
        append(left_position);
        if (right_position != left_position) {
            append(right_position);
        }
    }
    return records;
}

[[nodiscard]] std::vector<std::uint32_t> bounded_nthash_ranks(
        const std::vector<seq::dna4>& data) {
    std::vector<std::uint32_t> ranks;
    ranks.reserve(data.size());
    for (const seq::dna4 symbol : data) {
        ranks.push_back(static_cast<std::uint32_t>(symbol.to_rank()));
    }
    return ranks;
}

[[nodiscard]] std::vector<minimizer_record> sorted_records(
        std::vector<minimizer_record> records) {
    std::ranges::sort(records);
    return records;
}

void require_bounded_nthash_oracle_parity(
        const std::vector<minimizer_record>& actual,
        const std::vector<minimizer_record>& expected) {
    // The bounded ntHash kernel's eight logical streams append in stream
    // order, while the scalar oracle walks windows chronologically. Sort both
    // without uniquing: the comparator normalizes only order and still locks
    // exact multiplicity.
    REQUIRE(actual.size() == expected.size());
    REQUIRE(sorted_records(actual) == sorted_records(expected));
}

[[nodiscard]] std::uint8_t fixed_dna4_alphabet_code() {
    const auto* table = fixed_highway_kernels();
    const auto* identity = table == nullptr || !table->is_complete()
        ? nullptr : table->resolve_alphabet_identity<seq::dna4>();
    if (identity == nullptr
        || table->resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "generated fixed-target table lacks the DNA4 identity"};
    }
    return identity->alphabet_code;
}

template <class p_storage_unit>
using bounded_nthash_sequence = sequence_custom_alphabet<seq::dna4, p_storage_unit>;

template <class p_storage_unit>
using bounded_nthash_range = compact_sequence_range<bounded_nthash_sequence<p_storage_unit>>;

template <class p_storage_unit>
[[nodiscard]] ::bioseq::minimizers::packed_sequence_kernel_view
bounded_nthash_kernel_view(
        bounded_nthash_range<p_storage_unit>& sequence,
        std::size_t begin_unit,
        std::size_t end_unit) {
    if (begin_unit > end_unit
        || end_unit > static_cast<std::size_t>(sequence.size())) {
        throw std::out_of_range{"bounded ntHash leaf unit range"};
    }
    auto begin = sequence.begin();
    std::advance(begin, static_cast<std::ptrdiff_t>(begin_unit));
    using traits = typename bounded_nthash_range<p_storage_unit>::traits;
    constexpr std::size_t letters_per_unit =
        traits::letters_per_unit;
    const std::size_t begin_bp = begin_unit * letters_per_unit;
    const std::size_t end_bp = std::min(
        end_unit * letters_per_unit,
        static_cast<std::size_t>(sequence.bp_size()));
    return {
        .storage = reinterpret_cast<const std::byte*>(
            sequence.read_cptr(begin)),
        .storage_units = end_unit - begin_unit,
        .begin_symbol = 0u,
        .end_symbol = end_bp - begin_bp,
        .alphabet_code = fixed_dna4_alphabet_code(),
        .bits_per_symbol = 2u,
        .storage_unit_bytes = sizeof(p_storage_unit),
        .final_segment = true,
    };
}

template <class p_collector_type>
[[nodiscard]] ::bioseq::minimizers::minimizer_kernel_result invoke_bounded_nthash_leaf(
        p_collector_type& collector,
        ::bioseq::minimizers::packed_sequence_kernel_view sequence) {
    return pipe::invoke_selected_minimizer_kernel<minimap_sketch_data>(
        fixed_highway_kernels(),
        ::bioseq::simd_dispatch::highway_kernel::minimizer_bounded_nthash,
        ::bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash,
        sequence,
        10u,
        15u,
        collector);
}

struct bounded_nthash_leaf_run {
    std::vector<minimizer_record> records;
    ::bioseq::minimizers::minimizer_kernel_result result;
};

template <class p_storage_unit>
[[nodiscard]] bounded_nthash_leaf_run run_bounded_nthash_leaf(
        std::vector<seq::dna4>& data,
        std::size_t begin_unit = 0u,
        std::size_t end_unit = (std::numeric_limits<std::size_t>::max)()) {
    bounded_nthash_range<p_storage_unit> sequence(data);
    if (end_unit == (std::numeric_limits<std::size_t>::max)()) {
        end_unit = static_cast<std::size_t>(sequence.size());
    }
    sketch_collector collector;
    auto result = invoke_bounded_nthash_leaf(
        collector, bounded_nthash_kernel_view(sequence, begin_unit, end_unit));
    return {
        .records = raw_records(collector),
        .result = result,
    };
}

[[nodiscard]] bool bounded_nthash_target_shape_supported() noexcept {
    const std::size_t lanes = hn::Lanes(hn::CappedTag<std::uint32_t, 8>{});
    return lanes == 4u || lanes == 8u;
}

[[nodiscard]] ::bioseq::minimizers::bounded_nthash_status
bounded_nthash_family_status(
        const ::bioseq::minimizers::minimizer_kernel_result& result) noexcept {
    return static_cast<::bioseq::minimizers::bounded_nthash_status>(
        result.family_status);
}

template <class p_collector_type>
[[nodiscard]] std::vector<std::uint64_t> normalized_minimizer_values(
        const p_collector_type& collector) {
    std::vector<std::uint64_t> values;
    values.reserve(collector.extract_sketches_data().size());
    for (const auto& record : collector.extract_sketches_data()) {
        values.push_back(static_cast<std::uint64_t>(record.raw_value()));
    }
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

template <sequence_alphabet p_alphabet, class p_storage_unit>
void require_reference_route_parity() {
    using sequence_type =
        sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<sequence_type>;
    using pipe::minimizer_kernel_kind;

    std::vector<p_alphabet> data(4099u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        std::uint8_t rank = static_cast<std::uint8_t>(index & 3u);
        if constexpr (std::same_as<p_alphabet, seq::dna5>) {
            rank = index % 97u == 41u
                ? 3u
                : std::array<std::uint8_t, 4u>{0u, 1u, 2u, 4u}
                    [index & 3u];
        }
        data[index].assign_rank(rank);
    }
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    constexpr bool scalar_routed = std::same_as<p_alphabet, seq::dna5>;
    const auto* table = scalar_routed ? nullptr : fixed_highway_kernels();

    sketch_collector runtime_collector;
    const auto runtime_stats = pipe::minimizer_dispatch<
        minimizer_kernel_kind::simd, minimap_sketch_data>(
            runtime_collector, view, 10u, 15u, table);
    sketch_collector constexpr_collector;
    const auto constexpr_stats = pipe::minimizer_dispatch_constexpr<
        minimizer_kernel_kind::simd, 10u, 15u, minimap_sketch_data>(
            constexpr_collector, view, 10u, 15u, table);
    sketch_collector scalar_collector;
    (void)pipe::minimizer_dispatch<
        minimizer_kernel_kind::scalar, minimap_sketch_data>(
            scalar_collector, view, 10u, 15u, table);

    CAPTURE(sizeof(p_storage_unit), scalar_routed);
    REQUIRE(runtime_stats == two_stack_execution_stats{});
    REQUIRE(constexpr_stats == two_stack_execution_stats{});
    REQUIRE(normalized_minimizer_values(runtime_collector)
            == normalized_minimizer_values(scalar_collector));
    REQUIRE(normalized_minimizer_values(constexpr_collector)
            == normalized_minimizer_values(scalar_collector));
    if constexpr (scalar_routed) {
        const auto& runtime = runtime_collector.extract_sketches_data();
        const auto& specialized =
            constexpr_collector.extract_sketches_data();
        const auto& scalar = scalar_collector.extract_sketches_data();
        REQUIRE(runtime.size() == scalar.size());
        REQUIRE(specialized.size() == scalar.size());
        for (std::size_t index = 0u; index < scalar.size(); ++index) {
            CAPTURE(index);
            REQUIRE(runtime[index].raw_value() == scalar[index].raw_value());
            REQUIRE(specialized[index].raw_value()
                    == scalar[index].raw_value());
        }
    }
}

struct minimizer_run {
    std::vector<minimizer_record> records;
    pipe::minimizer_range_execution_stats stats;
};

[[nodiscard]] minimizer_run run_dispatch(facade_type view,
                                         sequence_size_type from,
                                         sequence_size_type to,
                                         minimizer_sketch_backend backend) {
    sketch_collector collector;
    auto stats = pipe::minimizer_dispatch_range<minimap_sketch_data>(
        collector, view, from, to, 10u, 15u, backend,
        pipe::exact_simd_range_route::require_continuation,
        fixed_highway_kernels());
    return {
        .records = canonical_records(collector),
        .stats = stats,
    };
}

void require_partial_source_read(const composite_source_state& source,
                                 sequence_size_type selected_length) {
    REQUIRE(source.callback_count > 0u);
    REQUIRE(source.requested_bp >= selected_length);
    REQUIRE(source.requested_bp < source.logical_size);
    REQUIRE(source.max_request <= 256u);
}

// Run the scalar minimizer kernel over an arbitrary sequence view and return
// the sorted-unique (last_pos, kmer) pair set.
template <class p_sequence_view, class p_index>
[[nodiscard]] std::vector<std::pair<std::size_t, std::uint64_t>>
run_scalar(p_sequence_view& seq, p_index begin, p_index end, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        collector, seq, begin, end, w, k);
    auto pairs = collector.extract_pairs();
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

template <std::size_t p_fragment_units>
void check_parity(std::size_t n, std::uint64_t seed, std::size_t w, std::size_t k) {
    using chunked = bioseq::storage::chunked_span_sequence_scanner<std::uint64_t, p_fragment_units>;
    auto data = make_dna4(n, seed);
    range_type range(data);

    const auto baseline = run_scalar(range, range.begin(), range.end(), w, k);

    bioseq::storage::lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto fragmented = run_scalar(lazy, lazy.begin(), lazy.end(), w, k);

    if (baseline != fragmented) {
        bioseq_log<logging_level::debug>(
                "[parity] n={} frag_units={} baseline={} fragmented={}",
                n, p_fragment_units, baseline.size(), fragmented.size());
    }
    REQUIRE(baseline.size() == fragmented.size());
    REQUIRE(baseline == fragmented);
}

}  // namespace

TEST_CASE("fragmented minimizer set equals all-in-RAM baseline (w=10,k=15)",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    for (std::size_t n : {64u, 96u, 200u, 257u, 1024u, 4096u}) {
        check_parity<1>(n, 0x1111 + n, 10, 15);    // 32 letters / fragment (worst case)
        check_parity<2>(n, 0x2222 + n, 10, 15);    // 64 letters / fragment
        check_parity<32>(n, 0x3333 + n, 10, 15);   // 1024 letters / fragment (production)
    }
}

TEST_CASE("fragmented minimizer parity across w/k variations",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    const std::array<std::pair<std::size_t, std::size_t>, 3> wk{{{5, 7}, {10, 15}, {12, 21}}};
    for (auto [w, k] : wk) {
        check_parity<1>(512, 0xAAAA + w * 131 + k, w, k);
        check_parity<32>(2000, 0xBBBB + w * 131 + k, w, k);
    }
}

TEST_CASE(
    "live packed bounded ntHash leaf matches its independent scalar oracle "
    "at every retained tail",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::array<std::size_t, 13u> lengths{
        24u, 25u, 31u, 32u, 33u, 63u, 64u,
        65u, 127u, 128u, 129u, 257u, 521u,
    };
    for (const std::size_t length : lengths) {
        CAPTURE(length);
        auto data = make_dna4(length, 0x399000u + length);
        const auto expected = bounded_nthash_scalar_oracle<15u, 10u>(
            bounded_nthash_ranks(data));
        const auto actual = run_bounded_nthash_leaf<std::uint32_t>(data);
        if (!bounded_nthash_target_shape_supported()) {
            REQUIRE_FALSE(actual.result.is_ok());
            REQUIRE(bounded_nthash_family_status(actual.result)
                    == ::bioseq::minimizers::bounded_nthash_status::
                        unsupported_vector_shape);
            REQUIRE(actual.records.empty());
            REQUIRE(actual.result.stats.bounded_nthash.packed_decode_batches == 0u);
            continue;
        }

        REQUIRE(actual.result.is_ok());
        REQUIRE(actual.result.stats.bounded_nthash.is_internally_consistent(true));
        REQUIRE(actual.result.emitted == actual.records.size());
        require_bounded_nthash_oracle_parity(actual.records, expected);
        REQUIRE(actual.records.size() == expected.size());
        REQUIRE(actual.result.stats.bounded_nthash.scalar_hash_symbols == 0u);
        REQUIRE(actual.result.stats.bounded_nthash.scalar_window_transitions == 0u);
        REQUIRE(actual.result.stats.bounded_nthash.scalar_minimum_decisions == 0u);
        REQUIRE(actual.result.stats.bounded_nthash.fallback_calls == 0u);
        if (length == 521u) {
            REQUIRE(std::ranges::any_of(
                actual.records,
                [](const minimizer_record& record) {
                    return std::get<1>(record) > 0xFFFFu;
                }));
        }
    }
}

TEST_CASE("live packed bounded ntHash reverse complement preserves hashes and flips strand",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t kmer = 15u;
    auto forward_data = make_dna4(377u, 0x399100u);
    std::vector<seq::dna4> reverse_data;
    reverse_data.reserve(forward_data.size());
    for (auto iterator = forward_data.rbegin();
         iterator != forward_data.rend(); ++iterator) {
        seq::dna4 complement{};
        complement.assign_rank(static_cast<std::uint8_t>(
            3u - iterator->to_rank()));
        reverse_data.push_back(complement);
    }
    const auto forward = run_bounded_nthash_leaf<std::uint32_t>(forward_data);
    const auto reverse = run_bounded_nthash_leaf<std::uint32_t>(reverse_data);
    if (!bounded_nthash_target_shape_supported()) {
        REQUIRE(bounded_nthash_family_status(forward.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_vector_shape);
        REQUIRE(bounded_nthash_family_status(reverse.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_vector_shape);
        return;
    }
    REQUIRE(forward.result.stats.bounded_nthash.is_internally_consistent(true));
    REQUIRE(reverse.result.stats.bounded_nthash.is_internally_consistent(true));
    require_bounded_nthash_oracle_parity(
        forward.records,
        bounded_nthash_scalar_oracle<15u, 10u>(bounded_nthash_ranks(forward_data)));
    require_bounded_nthash_oracle_parity(
        reverse.records,
        bounded_nthash_scalar_oracle<15u, 10u>(bounded_nthash_ranks(reverse_data)));

    std::vector<minimizer_record> mapped_reverse;
    mapped_reverse.reserve(reverse.records.size());
    for (const auto& [position, hash, strand] : reverse.records) {
        mapped_reverse.emplace_back(
            forward_data.size() + kmer - 2u - position,
            hash,
            strand ^ 1u);
    }
    REQUIRE(sorted_records(mapped_reverse)
            == sorted_records(forward.records));
}

TEST_CASE(
    "live packed bounded ntHash u64 storage preserves raw u32 order and "
    "local subrange coordinates",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    auto whole_data = make_dna4(521u, 0x399200u);
    const auto whole_u32 = run_bounded_nthash_leaf<std::uint32_t>(whole_data);
    const auto whole_u64 = run_bounded_nthash_leaf<std::uint64_t>(whole_data);
    if (!bounded_nthash_target_shape_supported()) {
        REQUIRE(bounded_nthash_family_status(whole_u32.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_vector_shape);
        REQUIRE(bounded_nthash_family_status(whole_u64.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_vector_shape);
        return;
    }
    REQUIRE(whole_u32.result.stats.bounded_nthash.is_internally_consistent(true));
    REQUIRE(whole_u64.result.stats.bounded_nthash.is_internally_consistent(true));
    REQUIRE(whole_u32.result.stats.bounded_nthash.source_storage_unit_bytes
            == sizeof(std::uint32_t));
    REQUIRE(whole_u64.result.stats.bounded_nthash.source_storage_unit_bytes
            == sizeof(std::uint64_t));
    REQUIRE(whole_u64.records == whole_u32.records);
    require_bounded_nthash_oracle_parity(
        whole_u64.records,
        bounded_nthash_scalar_oracle<15u, 10u>(bounded_nthash_ranks(whole_data)));

    constexpr std::size_t begin_u64 = 2u;
    constexpr std::size_t end_u64 = 20u;
    constexpr std::size_t letters_per_u64 = 32u;
    auto subrange_data = make_dna4(1021u, 0x399201u);
    const auto u64 = run_bounded_nthash_leaf<std::uint64_t>(
        subrange_data, begin_u64, end_u64);
    const auto u32 = run_bounded_nthash_leaf<std::uint32_t>(
        subrange_data, 2u * begin_u64, 2u * end_u64);
    REQUIRE(u64.result.stats.bounded_nthash.is_internally_consistent(true));
    REQUIRE(u32.result.stats.bounded_nthash.is_internally_consistent(true));
    REQUIRE(u64.records == u32.records);
    const auto ranks = bounded_nthash_ranks(subrange_data);
    const std::size_t begin_bp = begin_u64 * letters_per_u64;
    const std::size_t end_bp = end_u64 * letters_per_u64;
    require_bounded_nthash_oracle_parity(
        u64.records,
        bounded_nthash_scalar_oracle<15u, 10u>(std::span<const std::uint32_t>{
            ranks.data() + begin_bp, end_bp - begin_bp}));
    REQUIRE(u64.result.stats.bounded_nthash.positions_are_segment_local);
    REQUIRE(u64.result.stats.bounded_nthash.input_symbols == end_bp - begin_bp);
    REQUIRE(u64.result.stats.bounded_nthash.input_storage_units
            == end_u64 - begin_u64);
    REQUIRE(std::ranges::all_of(
        u64.records,
        [length = end_bp - begin_bp](const minimizer_record& record) {
            return std::get<0>(record) < length;
        }));
}

TEST_CASE("live packed bounded ntHash suppresses persistent minima at all seven stream joins",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    constexpr std::size_t length = 521u;
    constexpr std::size_t kmer = 15u;
    constexpr std::size_t window = 10u;
    std::vector<seq::dna4> data;
    data.reserve(length);
    std::uint32_t state = 1u;
    for (std::size_t index = 0u; index < length; ++index) {
        state = state * 1664525u + 1013904223u;
        seq::dna4 symbol{};
        symbol.assign_rank(static_cast<std::uint8_t>((state >> 28u) & 3u));
        data.push_back(symbol);
    }
    const auto ranks = bounded_nthash_ranks(data);
    std::vector<bounded_nthash_hash_and_strand> kmers(length - kmer + 1u);
    for (std::size_t position = 0u; position < kmers.size(); ++position) {
        kmers[position] =
            bounded_nthash_oracle_hash_and_strand<kmer>(ranks, position);
    }
    const auto unique_minimum =
        [&kmers](std::size_t window_start) {
            std::uint32_t minimum =
                (std::numeric_limits<std::uint32_t>::max)();
            std::size_t position = 0u;
            std::size_t multiplicity = 0u;
            for (std::size_t offset = 0u; offset < window; ++offset) {
                const std::uint32_t hash = kmers[window_start + offset].hash;
                if (hash < minimum) {
                    minimum = hash;
                    position = window_start + offset;
                    multiplicity = 1u;
                } else if (hash == minimum) {
                    ++multiplicity;
                }
            }
            return std::tuple{minimum, position, multiplicity};
        };
    constexpr std::size_t context_units =
        (kmer + window - 1u + 15u) / 16u;
    const std::size_t view_units = (length + 15u) / 16u;
    const auto [units_per_lane, offsets] =
        ::bioseq::simd::iteration_geometry::
            calculate_iteration_offsets<8u>(
                view_units, 8u, context_units);
    (void)units_per_lane;
    for (std::size_t lane = 1u; lane < 8u; ++lane) {
        CAPTURE(lane);
        const std::size_t boundary = offsets[lane] * 16u;
        const auto before = unique_minimum(boundary - 1u);
        const auto after = unique_minimum(boundary);
        REQUIRE(std::get<0>(before) == std::get<0>(after));
        REQUIRE(std::get<1>(before) == std::get<1>(after));
        REQUIRE(std::get<2>(before) == 1u);
        REQUIRE(std::get<2>(after) == 1u);
    }

    const auto actual = run_bounded_nthash_leaf<std::uint32_t>(data);
    if (!bounded_nthash_target_shape_supported()) {
        REQUIRE(bounded_nthash_family_status(actual.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_vector_shape);
        return;
    }
    const auto expected = bounded_nthash_scalar_oracle<kmer, window>(ranks);
    require_bounded_nthash_oracle_parity(actual.records, expected);
    REQUIRE(actual.records.size() == expected.size());
    REQUIRE(actual.result.stats.bounded_nthash.stream_boundary_records_buffered == 7u);
    REQUIRE(actual.result.stats.bounded_nthash.stream_boundary_duplicates_suppressed
            == 7u);
    REQUIRE(actual.result.stats.bounded_nthash.stream_boundary_records_committed == 0u);
}

TEST_CASE("live packed bounded ntHash ties retain endpoints, adjacent dedup, and strand zero",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    std::vector<seq::dna4> tied_a(257u);
    for (seq::dna4& symbol : tied_a) {
        symbol.assign_rank(0u);
    }
    const auto a_result = run_bounded_nthash_leaf<std::uint32_t>(tied_a);

    std::vector<seq::dna4> tied_g(257u);
    for (seq::dna4& symbol : tied_g) {
        symbol.assign_rank(2u);
    }
    const auto g_result = run_bounded_nthash_leaf<std::uint32_t>(tied_g);
    if (!bounded_nthash_target_shape_supported()) {
        REQUIRE(bounded_nthash_family_status(a_result.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_vector_shape);
        REQUIRE(bounded_nthash_family_status(g_result.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_vector_shape);
        return;
    }

    require_bounded_nthash_oracle_parity(
        a_result.records,
        bounded_nthash_scalar_oracle<15u, 10u>(bounded_nthash_ranks(tied_a)));
    REQUIRE(a_result.result.stats.bounded_nthash.tied_endpoint_records_emitted > 0u);
    REQUIRE(a_result.result.stats.bounded_nthash.stream_boundary_records_buffered
            == a_result.result.stats.bounded_nthash.stream_boundary_records_committed
                + a_result.result.stats.bounded_nthash.
                    stream_boundary_duplicates_suppressed);

    require_bounded_nthash_oracle_parity(
        g_result.records,
        bounded_nthash_scalar_oracle<15u, 10u>(bounded_nthash_ranks(tied_g)));
    REQUIRE(g_result.result.stats.bounded_nthash.tied_endpoint_records_emitted > 0u);
    REQUIRE_FALSE(g_result.records.empty());
    REQUIRE(std::ranges::all_of(
        g_result.records,
        [](const minimizer_record& record) {
            return std::get<2>(record) == 0u;
        }));
}

TEST_CASE("live packed bounded ntHash rejects reversed and narrow views before biological work",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    auto data = make_dna4(257u, 0x399300u);
    bounded_nthash_range<std::uint64_t> reversed_sequence(data);
    auto reversed_view = bounded_nthash_kernel_view(
        reversed_sequence,
        0u,
        static_cast<std::size_t>(reversed_sequence.size()));
    reversed_view.begin_symbol = 17u;
    reversed_view.end_symbol = 16u;
    sketch_collector reversed_collector;
    const auto reversed = invoke_bounded_nthash_leaf(
        reversed_collector, reversed_view);
    REQUIRE(reversed.status
            == ::bioseq::minimizers::minimizer_kernel_status::
                invalid_sequence_view);
    REQUIRE(bounded_nthash_family_status(reversed)
            == ::bioseq::minimizers::bounded_nthash_status::invalid_range);
    REQUIRE(reversed.stats.bounded_nthash
            == ::bioseq::minimizers::bounded_nthash_execution_stats{
                .fail_closed_rejections = 1u});
    REQUIRE(reversed_collector.extract_sketches_data().empty());

    const auto require_narrow_rejection = [&data]<class p_storage_unit>() {
        const auto rejected = run_bounded_nthash_leaf<p_storage_unit>(data);
        REQUIRE(rejected.result.status
                == ::bioseq::minimizers::minimizer_kernel_status::
                    invalid_configuration);
        REQUIRE(bounded_nthash_family_status(rejected.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_storage);
        REQUIRE(rejected.result.stats.bounded_nthash
                == ::bioseq::minimizers::bounded_nthash_execution_stats{
                    .fail_closed_rejections = 1u});
        REQUIRE(rejected.records.empty());
    };
    require_narrow_rejection.template operator()<std::uint8_t>();
    require_narrow_rejection.template operator()<std::uint16_t>();
}

TEST_CASE("live packed bounded ntHash stats aggregation requires one exact source width",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    auto data = make_dna4(2048u, 0x399400u);
    const auto first_u32 = run_bounded_nthash_leaf<std::uint32_t>(data, 0u, 16u);
    const auto second_u32 = run_bounded_nthash_leaf<std::uint32_t>(data, 16u, 32u);
    const auto first_u64 = run_bounded_nthash_leaf<std::uint64_t>(data, 0u, 8u);
    if (!bounded_nthash_target_shape_supported()) {
        REQUIRE(bounded_nthash_family_status(first_u32.result)
                == ::bioseq::minimizers::bounded_nthash_status::
                    unsupported_vector_shape);
        return;
    }
    auto same_width = first_u32.result.stats.bounded_nthash;
    same_width.merge(second_u32.result.stats.bounded_nthash);
    REQUIRE_FALSE(same_width.aggregation_mismatch);
    REQUIRE(same_width.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE(same_width.segments_combined
            == first_u32.result.stats.bounded_nthash.segments_combined
                + second_u32.result.stats.bounded_nthash.segments_combined);
    REQUIRE(same_width.is_internally_consistent(true));

    auto mixed_width = first_u32.result.stats.bounded_nthash;
    mixed_width.merge(first_u64.result.stats.bounded_nthash);
    REQUIRE(mixed_width.aggregation_mismatch);
    REQUIRE(mixed_width.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE_FALSE(mixed_width.is_internally_consistent(false));

    auto saturated = first_u32.result.stats.bounded_nthash;
    saturated.input_symbols =
        (std::numeric_limits<std::uint64_t>::max)() - 1u;
    auto overflow = second_u32.result.stats.bounded_nthash;
    overflow.input_symbols = 2u;
    saturated.merge(overflow);
    REQUIRE(saturated.saturated);
    REQUIRE(saturated.input_symbols
            == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE_FALSE(saturated.is_internally_consistent(false));
}

TEST_CASE("live admitted collector owns tie-heavy bounded ntHash output above input bp",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using collector_type = admitted_sketch_collector<
        minimap_sketch_data, bioseq::parallelism::memory_permit>;
    std::vector<seq::dna4> data(96u);
    for (seq::dna4& symbol : data) {
        symbol.assign_rank(0u);
    }
    bounded_nthash_range<std::uint32_t> sequence(data);
    bioseq::parallelism::memory_admission_controller admission;
    {
        auto permit = admission.try_reserve(0u);
        REQUIRE(permit.has_value());
        collector_type collector{
            std::move(*permit),
            bioseq::parallelism::k_vector_preallocation_headroom_factor};
        const auto result = invoke_bounded_nthash_leaf(
            collector,
            bounded_nthash_kernel_view(
                sequence, 0u,
                static_cast<std::size_t>(sequence.size())));
        if (!bounded_nthash_target_shape_supported()) {
            REQUIRE_FALSE(result.is_ok());
            REQUIRE(collector.extract_sketches_data().empty());
        } else {
            REQUIRE(result.is_ok());
            REQUIRE(result.stats.bounded_nthash.is_internally_consistent(true));
            REQUIRE(collector.is_ok());
            REQUIRE(collector.extract_sketches_data().size()
                    == result.stats.bounded_nthash.minimizers_emitted);
            REQUIRE(collector.extract_sketches_data().size() > data.size());
            REQUIRE(collector.admitted_capacity_bytes()
                    == collector.extract_sketches_data().capacity()
                        * sizeof(minimap_sketch_data));
            REQUIRE(admission.reserved_bytes()
                    == collector.admitted_capacity_bytes());
        }
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("Reference SIMD keeps DNA4 topology and scalar-routes DNA5 before lookup",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    require_reference_route_parity<seq::dna4, std::uint32_t>();
    require_reference_route_parity<seq::dna4, std::uint64_t>();
    require_reference_route_parity<seq::dna5, std::uint32_t>();
    require_reference_route_parity<seq::dna5, std::uint64_t>();
}

TEST_CASE("scalar minimizers stream only a sliced composite source range",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    auto fixture = make_composite_fixture();
    using packed_type = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<packed_type> selected_range(fixture.selected_letters);
    auto selected_owner = bioseq::storage::make_resident_lazy_sequence(selected_range);
    const auto selected_view =
        bioseq::storage::make_lazy_sequence_view(selected_owner);
    const auto baseline = run_dispatch(
        selected_view, 0u, selected_view.bp_size(),
        minimizer_sketch_backend::scalar);

    reset_source_observation(fixture.source);
    const auto source_view = make_composite_source_view(fixture.source);
    REQUIRE_FALSE(bioseq::storage::is_worker_shareable(source_view));
    REQUIRE_FALSE(source_view.try_resident_packed(
        fixture.selected_from, fixture.selected_to, sizeof(std::uint32_t)));
    const auto source = run_dispatch(
        source_view, fixture.selected_from, fixture.selected_to,
        minimizer_sketch_backend::scalar);

    REQUIRE(source.records == baseline.records);
    REQUIRE(pipe::minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::scalar, source.stats,
        /*require_nonempty_vector_work=*/false));
    require_partial_source_read(
        fixture.source, fixture.selected_to - fixture.selected_from);
}

TEST_CASE(
    "exact_simd and bounded ntHash SIMD admit only a sliced "
    "composite source range",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    auto fixture = make_composite_fixture();
    using packed_type = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<packed_type> selected_range(fixture.selected_letters);
    auto selected_owner = bioseq::storage::make_resident_lazy_sequence(selected_range);
    const auto selected_view =
        bioseq::storage::make_lazy_sequence_view(selected_owner);
    const auto scalar = run_dispatch(
        selected_view, 0u, selected_view.bp_size(),
        minimizer_sketch_backend::scalar);
    const auto source_view = make_composite_source_view(fixture.source);
    const sequence_size_type selected_length = fixture.selected_to - fixture.selected_from;

    try {
        const auto exact_baseline = run_dispatch(
            selected_view, 0u, selected_view.bp_size(),
            minimizer_sketch_backend::exact_simd);
        reset_source_observation(fixture.source);
        const auto exact_source = run_dispatch(
            source_view, fixture.selected_from, fixture.selected_to,
            minimizer_sketch_backend::exact_simd);
        REQUIRE(exact_source.records == exact_baseline.records);
        REQUIRE(exact_source.records == scalar.records);
        REQUIRE(pipe::minimizer_range_stats_matches_backend(
            minimizer_sketch_backend::exact_simd, exact_source.stats,
            /*require_nonempty_vector_work=*/true,
            pipe::exact_simd_range_route::require_continuation));
        require_partial_source_read(fixture.source, selected_length);
    } catch (const pipe::exact_simd_runtime_error& error) {
        REQUIRE(error.status()
                == two_stack_continuation_status::unsupported_vector_shape);
    }

    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4u && lanes != 8u) {
        reset_source_observation(fixture.source);
        REQUIRE_THROWS_AS(
            run_dispatch(
                source_view, fixture.selected_from, fixture.selected_to,
                minimizer_sketch_backend::bounded_nthash_simd),
            pipe::bounded_nthash_runtime_error);
        require_partial_source_read(fixture.source, selected_length);
        return;
    }

    const auto bounded_nthash_baseline = run_dispatch(
        selected_view, 0u, selected_view.bp_size(),
        minimizer_sketch_backend::bounded_nthash_simd);
    reset_source_observation(fixture.source);
    const auto bounded_nthash_source = run_dispatch(
        source_view, fixture.selected_from, fixture.selected_to,
        minimizer_sketch_backend::bounded_nthash_simd);
    REQUIRE(bounded_nthash_source.records == bounded_nthash_baseline.records);
    REQUIRE(pipe::minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::bounded_nthash_simd, bounded_nthash_source.stats,
        /*require_nonempty_vector_work=*/true));
    require_partial_source_read(fixture.source, selected_length);
}

TEST_CASE("Vulkan staging dispatches a sliced composite source range only",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    auto fixture = make_composite_fixture();
    const auto source_view = make_composite_source_view(fixture.source);
    if (!vk::hardware_device_available(vk::runtime::instance())) {
        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        const std::uint64_t baseline = admission.reserved_bytes();
        vk::vulkan_sketch_decline decline{};
        reset_source_observation(fixture.source);
        const auto rejected =
            vk::build_gpu_segment_index_on_device<minimap_sketch_data>(
                source_view,
                fixture.selected_from,
                fixture.selected_to,
                10u,
                15u,
                {},
                &decline);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(decline.reason
                == vk::vulkan_sketch_decline_reason::hardware_device_unavailable);
        REQUIRE(decline.bp_size
                == fixture.selected_to - fixture.selected_from);
        REQUIRE(fixture.source.requested_bp
                == fixture.selected_to - fixture.selected_from);
        require_partial_source_read(
            fixture.source, fixture.selected_to - fixture.selected_from);
        REQUIRE(admission.reserved_bytes() == baseline);
        return;
    }

    using packed_type = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<packed_type> selected_range(fixture.selected_letters);
    auto selected_owner = bioseq::storage::make_resident_lazy_sequence(selected_range);
    const auto selected_view =
        bioseq::storage::make_lazy_sequence_view(selected_owner);
    const auto scalar = run_dispatch(
        selected_view, 0u, selected_view.bp_size(),
        minimizer_sketch_backend::scalar);

    reset_source_observation(fixture.source);
    sketch_collector gpu_collector;
    vk::collect_minimizers_vulkan_segment_or_throw<minimap_sketch_data>(
        gpu_collector,
        source_view,
        fixture.selected_from,
        fixture.selected_to,
        10u,
        15u);
    REQUIRE(canonical_records(gpu_collector) == scalar.records);
    REQUIRE(fixture.source.requested_bp
            == fixture.selected_to - fixture.selected_from);
    require_partial_source_read(
        fixture.source, fixture.selected_to - fixture.selected_from);
}

TEST_CASE("lazy batch sorting never requests whole composite sources",
          "[component:bioseq][layer:unit][feature:sequence]") {
    auto high = make_composite_fixture();
    auto low = make_composite_fixture();
    auto middle = make_composite_fixture();
    high.source.sources[0][100u] = 3u;
    low.source.sources[0][100u] = 0u;
    middle.source.sources[0][100u] = 1u;

    const std::array<facade_type, 3> views{
        make_composite_source_view(high.source),
        make_composite_source_view(low.source),
        make_composite_source_view(middle.source),
    };
    const std::span<const facade_type> batch{views};
    using processor = bioseq::storage::sequence_batch_sorter<seq::dna4>;

    bioseq::storage::sequence_sort_options cpu{};
    cpu.vulkan_enabled = false;
    REQUIRE((processor::sort(batch, cpu)
             == std::vector<std::size_t>{1u, 2u, 0u}));
    REQUIRE(high.source.callback_count + low.source.callback_count
                + middle.source.callback_count
            > 0u);
    REQUIRE(high.source.max_request < high.source.logical_size);
    REQUIRE(low.source.max_request < low.source.logical_size);
    REQUIRE(middle.source.max_request < middle.source.logical_size);

    reset_source_observation(high.source);
    reset_source_observation(low.source);
    reset_source_observation(middle.source);
    bioseq::storage::sequence_sort_options gpu{};
    gpu.vulkan_enabled = true;
    REQUIRE((processor::sort(batch, gpu)
             == std::vector<std::size_t>{1u, 2u, 0u}));
    REQUIRE(high.source.max_request < high.source.logical_size);
    REQUIRE(low.source.max_request < low.source.logical_size);
    REQUIRE(middle.source.max_request < middle.source.logical_size);
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
