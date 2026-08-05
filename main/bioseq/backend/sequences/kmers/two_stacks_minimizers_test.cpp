#include "common_tests.h"

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
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
using bioseq::simd::PackedPaperNthashExecutionProof;
using bioseq::simd::TwoStackContinuationStatus;
using bioseq::simd::exact_simd_minimizer_representation;
using bioseq::simd::packed_paper_nthash_scheme_id;
using bioseq::simd::two_stack_status_name;

struct unsupported_exact_representation {};

static_assert(exact_simd_minimizer_representation<MinimapSketchData>);
static_assert(
    !exact_simd_minimizer_representation<unsupported_exact_representation>);
static_assert(
    two_stack_status_name(TwoStackContinuationStatus::unsupported_vector_shape)
    == std::string_view{"unsupported_vector_shape"});

using bioseq::minimizers::MinimizerKernelAlgorithm;
using bioseq::minimizers::MinimizerKernelInvocation;
using bioseq::minimizers::MinimizerKernelStatus;
using bioseq::minimizers::MinimizerSink;
using bioseq::minimizers::PackedPaperNthashStatus;
using bioseq::minimizers::PackedSequenceKernelView;
using bioseq::minimizers::TwoStackRoute;

struct minimizer_record {
    std::uint64_t hash = 0u;
    std::uint64_t last_position = 0u;
    std::uint64_t strand = 0u;

    [[nodiscard]] constexpr auto operator<=>(
        const minimizer_record&) const noexcept = default;
};

struct collecting_minimizer_sink {
    std::vector<MinimapSketchData> records;
    std::size_t pending_begin = 0u;
    std::size_t pending_capacity = 0u;
    bool protocol_ok = true;

    [[nodiscard]] static std::span<MinimapSketchData> acquire(
        void* opaque,
        std::size_t minimum)
    {
        auto& self = *static_cast<collecting_minimizer_sink*>(opaque);
        if (self.pending_capacity != 0u) {
            throw std::logic_error{"nested minimizer sink acquire"};
        }
        self.pending_begin = self.records.size();
        self.pending_capacity = minimum;
        self.records.resize(self.pending_begin + minimum);
        return std::span<MinimapSketchData>{self.records}.subspan(
            self.pending_begin, minimum);
    }

    static void commit(void* opaque, std::size_t initialized) noexcept
    {
        auto& self = *static_cast<collecting_minimizer_sink*>(opaque);
        if (initialized > self.pending_capacity) {
            self.protocol_ok = false;
            initialized = self.pending_capacity;
        }
        self.records.resize(self.pending_begin + initialized);
        self.pending_capacity = 0u;
    }

    [[nodiscard]] MinimizerSink contract() noexcept
    {
        return {
            .state = this,
            .acquire = &collecting_minimizer_sink::acquire,
            .commit = &collecting_minimizer_sink::commit,
        };
    }
};

[[nodiscard]] const ::bioseq::simd_dispatch::HighwayKernelTable&
fixed_highway_table()
{
    return ::bioseq::simd_dispatch::highway_test_kernel_table();
}

template<sequence_alphabet Alphabet>
[[nodiscard]] std::uint8_t fixed_alphabet_code()
{
    const auto& table = fixed_highway_table();
    const auto* identity = table.complete()
        ? table.template resolve_alphabet_identity<Alphabet>() : nullptr;
    if (identity == nullptr
        || table.resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "generated fixed-target table lacks the requested alphabet"};
    }
    return identity->alphabet_code;
}

template<class Range>
[[nodiscard]] PackedSequenceKernelView fixed_packed_view(const Range& range)
{
    using traits = typename Range::traits;
    return {
        .storage = reinterpret_cast<const std::byte*>(
            range.read_cptr(range.cbegin())),
        .storage_units = static_cast<std::size_t>(range.size()),
        .begin_symbol = 0u,
        .end_symbol = static_cast<std::size_t>(range.bp_size()),
        .alphabet_code = fixed_alphabet_code<
            typename traits::alphabet_type>(),
        .bits_per_symbol = static_cast<std::uint8_t>(
            traits::bits_per_char),
        .storage_unit_bytes = static_cast<std::uint8_t>(
            sizeof(typename traits::storage_unit)),
        .final_segment = true,
    };
}

template<class Records>
[[nodiscard]] std::vector<minimizer_record> decoded_records(
    const Records& records)
{
    std::vector<minimizer_record> decoded;
    decoded.reserve(records.size());
    for (const MinimapSketchData& record : records) {
        decoded.push_back({
            .hash = record.kmer(),
            .last_position = record.last_pos(),
            .strand = record.strand(),
        });
    }
    return decoded;
}

[[nodiscard]] std::vector<seq::dna4> deterministic_dna4(
    std::size_t size)
{
    std::vector<seq::dna4> result(size);
    std::uint32_t state = 0x9E3779B9u;
    for (seq::dna4& symbol : result) {
        state = state * 1664525u + 1013904223u;
        symbol.assign_rank(static_cast<std::uint8_t>((state >> 29) & 3u));
    }
    return result;
}

template<class Range>
[[nodiscard]] std::vector<minimizer_record> scalar_exact_oracle(
    Range& range,
    std::size_t window,
    std::size_t kmer)
{
    ::bioseq::simd::sketch_collector collector;
    ::bioseq::simd::collect_minimizers_view<
        ::bioseq::simd::minimap_int_hasher,
        MinimapSketchData>(
            collector,
            range,
            range.begin(),
            range.end(),
            window,
            kmer);
    return decoded_records(collector.extract_sketches_data());
}

template<class StorageUnit>
void require_fixed_exact_parity(
    const std::vector<seq::dna4>& source,
    std::size_t begin_bp,
    std::size_t end_bp)
{
    CAPTURE(sizeof(StorageUnit), begin_bp, end_bp, source.size());
    REQUIRE(begin_bp < end_bp);
    REQUIRE(end_bp <= source.size());

    std::vector<seq::dna4> slice(
        source.begin() + static_cast<std::ptrdiff_t>(begin_bp),
        source.begin() + static_cast<std::ptrdiff_t>(end_bp));
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, StorageUnit>;
    compact_sequence_range<sequence_type> range(slice);
    const auto expected = scalar_exact_oracle(range, 5u, 7u);

    collecting_minimizer_sink sink;
    MinimizerKernelInvocation invocation{
        .sequence = fixed_packed_view(range),
        .fragments = {},
        .window = 5u,
        .kmer = 7u,
        .emitted_position_base = 0u,
        .two_stack_route = TwoStackRoute::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto kernel = fixed_highway_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    const auto actual = decoded_records(sink.records);

    REQUIRE(result.status == MinimizerKernelStatus::ok);
    REQUIRE(result.proof.algorithm
        == MinimizerKernelAlgorithm::two_stack_nthash);
    REQUIRE(result.proof.target == binding.target);
    REQUIRE(sink.protocol_ok);
    REQUIRE(result.emitted == actual.size());
    REQUIRE(actual == expected);

    const auto& proof = result.proof.two_stack;
    REQUIRE(proof.accepted(true));
    REQUIRE(proof.bp_consumed == slice.size());
    REQUIRE(proof.minimizers_emitted == actual.size());
    REQUIRE(proof.vector_batches > 0u);
    REQUIRE(proof.packed_decode_lanes == proof.bp_consumed);
    REQUIRE(proof.scalar_backend_branches == 0u);
    REQUIRE(proof.scalar_tail_symbols == 0u);
    REQUIRE(proof.scalar_hash_symbols == 0u);
    REQUIRE(proof.scalar_window_transitions == 0u);
    REQUIRE(proof.scalar_event_records == 0u);
    REQUIRE(proof.scalable_storage_fallback_calls == 0u);
}

template<class StorageUnit>
void require_fixed_exact_width(
    const std::vector<seq::dna4>& source,
    std::size_t bounded_end_unit)
{
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, StorageUnit>;
    using range_type = compact_sequence_range<sequence_type>;
    constexpr std::size_t letters_per_unit =
        range_type::traits::letters_per_unit;

    require_fixed_exact_parity<StorageUnit>(
        source,
        letters_per_unit,
        std::min(source.size(), bounded_end_unit * letters_per_unit));
    require_fixed_exact_parity<StorageUnit>(source, 0u, source.size());
}

void require_fixed_resident_report76_parity(
    const std::vector<seq::dna4>& source)
{
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(source);
    auto expected = scalar_exact_oracle(range, 10u, 15u);

    collecting_minimizer_sink sink;
    MinimizerKernelInvocation invocation{
        .sequence = fixed_packed_view(range),
        .fragments = {},
        .window = 10u,
        .kmer = 15u,
        .emitted_position_base = 0u,
        .two_stack_route = TwoStackRoute::resident_report76,
        .sink = sink.contract(),
    };
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto kernel = fixed_highway_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    auto actual = decoded_records(sink.records);

    REQUIRE(result.status == MinimizerKernelStatus::ok);
    REQUIRE(result.proof.algorithm
        == MinimizerKernelAlgorithm::two_stack_nthash);
    REQUIRE(result.proof.target == binding.target);
    REQUIRE(sink.protocol_ok);
    REQUIRE(result.emitted == actual.size());
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    REQUIRE(actual == expected);

    const auto& proof = result.proof.two_stack;
    REQUIRE(proof.accepted(true));
    REQUIRE(proof.resident_vector_stream_kernel_calls == 1u);
    REQUIRE(proof.resident_vector_stream_single_calls == 0u);
    REQUIRE(proof.resident_vector_stream_dual_calls == 1u);
    REQUIRE(proof.resident_vector_stream_nonempty_work_calls == 1u);
    REQUIRE(proof.resident_vector_stream_input_bp == source.size());
    REQUIRE(proof.resident_vector_stream_minimizers_emitted
        == actual.size());
    REQUIRE(proof.generic_u64_kernel_calls == 0u);
    REQUIRE(proof.k15_w10_b8_u32_hash_kernel_calls == 0u);
    REQUIRE(proof.scalar_backend_branches == 0u);
    REQUIRE(proof.scalar_hash_symbols == 0u);
    REQUIRE(proof.scalable_storage_fallback_calls == 0u);
}

inline constexpr std::array<std::uint32_t, 4u>
    paper_oracle_forward_table{
        0x95c60474u,
        0x62a02b4cu,
        0x82572324u,
        0x4be24456u,
    };

struct paper_hash_and_strand {
    std::uint32_t hash = 0u;
    std::uint32_t strand = 0u;
};

template<std::size_t K>
[[nodiscard]] paper_hash_and_strand paper_oracle_hash(
    std::span<const std::uint32_t> ranks,
    std::size_t start)
{
    std::uint32_t forward = 0u;
    std::uint32_t reverse = 0u;
    for (std::size_t offset = 0u; offset < K; ++offset) {
        const std::uint32_t rank = ranks[start + offset];
        REQUIRE(rank < paper_oracle_forward_table.size());
        forward ^= std::rotl(
            paper_oracle_forward_table[rank],
            static_cast<int>(((K - 1u - offset) * 7u) & 31u));
        reverse ^= std::rotl(
            paper_oracle_forward_table[3u - rank],
            static_cast<int>((offset * 7u) & 31u));
    }
    return {
        .hash = static_cast<std::uint32_t>(forward + reverse),
        .strand = reverse < forward ? 1u : 0u,
    };
}

template<std::size_t K, std::size_t W>
[[nodiscard]] std::vector<minimizer_record> paper_scalar_oracle(
    std::span<const std::uint32_t> ranks)
{
    REQUIRE(ranks.size() >= K + W - 1u);
    const std::size_t kmer_count = ranks.size() - K + 1u;
    const std::size_t window_count = kmer_count - W + 1u;
    std::vector<paper_hash_and_strand> kmers(kmer_count);
    for (std::size_t position = 0u; position < kmer_count; ++position) {
        kmers[position] = paper_oracle_hash<K>(ranks, position);
    }

    std::vector<minimizer_record> records;
    records.reserve(window_count * 2u);
    for (std::size_t window = 0u; window < window_count; ++window) {
        std::uint32_t minimum_hash =
            (std::numeric_limits<std::uint32_t>::max)();
        std::size_t left_position = window;
        std::size_t right_position = window;
        for (std::size_t offset = 0u; offset < W; ++offset) {
            const std::size_t position = window + offset;
            if (kmers[position].hash < minimum_hash) {
                minimum_hash = kmers[position].hash;
                left_position = position;
                right_position = position;
            } else if (kmers[position].hash == minimum_hash) {
                right_position = position;
            }
        }
        const auto append = [&](std::size_t position) {
            const minimizer_record record{
                .hash = minimum_hash,
                .last_position = position + K - 1u,
                .strand = kmers[position].strand,
            };
            if (records.empty() || records.back() != record) {
                records.push_back(record);
            }
        };
        append(left_position);
        if (right_position != left_position) append(right_position);
    }
    return records;
}

template<class Range>
[[nodiscard]] std::vector<std::uint32_t> compact_ranks(Range& range)
{
    std::vector<std::uint32_t> ranks;
    ranks.reserve(range.bp_size());
    for (const auto rank : range.template iterable<BP>()) {
        ranks.push_back(static_cast<std::uint32_t>(rank));
    }
    return ranks;
}

struct fixed_paper_run {
    std::vector<minimizer_record> records;
    PackedPaperNthashExecutionProof proof{};
    std::size_t emitted = 0u;
};

template<class StorageUnit>
[[nodiscard]] fixed_paper_run run_fixed_paper(
    const std::vector<seq::dna4>& data,
    const std::size_t window = 10u,
    const std::size_t kmer = 15u)
{
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, StorageUnit>;
    compact_sequence_range<sequence_type> range(data);
    collecting_minimizer_sink sink;
    MinimizerKernelInvocation invocation{
        .sequence = fixed_packed_view(range),
        .fragments = {},
        .window = window,
        .kmer = kmer,
        .emitted_position_base = 0u,
        .two_stack_route = TwoStackRoute::chronological_continuation,
        .sink = sink.contract(),
    };
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto kernel = fixed_highway_table().minimizers.paper_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    auto records = decoded_records(sink.records);

    REQUIRE(result.status == MinimizerKernelStatus::ok);
    REQUIRE(result.family_status
        == static_cast<std::uint32_t>(PackedPaperNthashStatus::ok));
    REQUIRE(result.proof.algorithm
        == MinimizerKernelAlgorithm::paper_nthash);
    REQUIRE(result.proof.target == binding.target);
    REQUIRE(sink.protocol_ok);
    REQUIRE(result.emitted == records.size());
    REQUIRE(result.proof.paper.accepted(true));
    REQUIRE(result.proof.paper.source_storage_unit_bytes
        == sizeof(StorageUnit));
    REQUIRE(result.proof.paper.input_symbols == data.size());
    return {
        .records = std::move(records),
        .proof = result.proof.paper,
        .emitted = result.emitted,
    };
}

TEST_CASE(
    "generated fixed table ExactSimd preserves DNA4 packed widths and tails",
    "[two_stack][exact_simd][fixed_table][packed_widths][tail][highway]")
{
    const auto data = deterministic_dna4(391u);
    require_fixed_exact_width<std::uint8_t>(data, 96u);
    require_fixed_exact_width<std::uint16_t>(data, 48u);
    require_fixed_exact_width<std::uint32_t>(data, 24u);
    require_fixed_exact_width<std::uint64_t>(data, 12u);
}

TEST_CASE(
    "generated fixed table ExactSimd preserves the resident report-76 hot path",
    "[two_stack][exact_simd][resident][vector_stream][highway]")
{
    require_fixed_resident_report76_parity(deterministic_dna4(4096u));
}

TEST_CASE(
    "generated fixed table Paper K15 W10 resident u32 matches independent compact oracle",
    "[two_stack][packed_paper_nthash][fixed_table][oracle][highway]")
{
    constexpr std::size_t kmer = 15u;
    constexpr std::size_t window = 10u;
    auto data = deterministic_dna4(521u);
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(data);
    const auto ranks = compact_ranks(range);
    auto expected = paper_scalar_oracle<kmer, window>(ranks);

    auto run = run_fixed_paper<std::uint32_t>(data, window, kmer);

    std::ranges::sort(run.records);
    std::ranges::sort(expected);
    REQUIRE(run.records == expected);

    const auto& proof = run.proof;
    REQUIRE(proof.scheme_id == packed_paper_nthash_scheme_id);
    REQUIRE(proof.kmer_size == kmer);
    REQUIRE(proof.window_size == window);
    REQUIRE(proof.nthash_rotation == 7u);
    REQUIRE(proof.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE(proof.input_symbols == data.size());
    REQUIRE(proof.logical_streams == 8u);
    REQUIRE(proof.minimizers_emitted == run.records.size());
    REQUIRE(proof.scalar_hash_symbols == 0u);
    REQUIRE(proof.scalar_window_transitions == 0u);
    REQUIRE(proof.scalar_minimum_decisions == 0u);
    REQUIRE(proof.fallback_calls == 0u);
    REQUIRE(proof.scalable_storage_fallback_calls == 0u);
}

TEST_CASE(
    "generated fixed table Paper preserves u64 packed storage through the u32 SIMD view",
    "[two_stack][packed_paper_nthash][u64][oracle][highway]")
{
    auto data = deterministic_dna4(521u);
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(data);
    const auto ranks = compact_ranks(range);
    auto expected = paper_scalar_oracle<15u, 10u>(ranks);

    auto u32 = run_fixed_paper<std::uint32_t>(data);
    auto u64 = run_fixed_paper<std::uint64_t>(data);
    REQUIRE(u64.records == u32.records);
    std::ranges::sort(u64.records);
    std::ranges::sort(expected);
    REQUIRE(u64.records == expected);
    REQUIRE(u64.proof.minimizers_emitted == u64.emitted);
}

TEST_CASE(
    "generated fixed table Paper retains tied endpoints and adjacent deduplication",
    "[two_stack][packed_paper_nthash][ties][dedup][highway]")
{
    std::vector<seq::dna4> data(257u);
    for (seq::dna4& symbol : data) symbol.assign_rank(0u);
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<sequence_type> range(data);
    const auto ranks = compact_ranks(range);
    auto expected = paper_scalar_oracle<15u, 10u>(ranks);
    auto run = run_fixed_paper<std::uint32_t>(data);

    std::ranges::sort(run.records);
    std::ranges::sort(expected);
    REQUIRE(run.records == expected);
    REQUIRE(run.proof.tied_endpoint_records_emitted > 0u);
    REQUIRE(run.proof.stream_boundary_records_buffered
        == run.proof.stream_boundary_records_committed
            + run.proof.stream_boundary_duplicates_suppressed);
}

[[nodiscard]] constexpr PackedPaperNthashExecutionProof
valid_paper_proof(const std::uint64_t input_symbols) noexcept {
    PackedPaperNthashExecutionProof proof{};
    proof.scheme_id = packed_paper_nthash_scheme_id;
    proof.kmer_size = 15u;
    proof.window_size = 10u;
    proof.nthash_rotation = 7u;
    proof.source_storage_unit_bytes = sizeof(std::uint32_t);
    proof.input_symbols = input_symbols;
    proof.input_storage_units = input_symbols / 16u;
    proof.segments_combined = 1u;
    proof.logical_streams = 8u;
    proof.physical_vector_lanes = 4u;
    proof.vector_registers_per_step = 2u;
    proof.packed_decode_batches = 1u;
    proof.packed_decode_lane_visits = 8u;
    proof.hash_initialization_batches = 1u;
    proof.rolling_hash_batches = 1u;
    proof.hash_table_lookup_batches = 1u;
    proof.canonical_sum_batches = 1u;
    proof.window_update_batches = 1u;
    proof.emission_decision_batches = 1u;
    proof.emission_mask_store_batches = 1u;
    proof.minimizers_emitted = 2u;
    proof.scalar_collector_appends = 2u;
    proof.positions_are_segment_local = true;
    return proof;
}

TEST_CASE("packed paper proof aggregation is identity checked and saturating",
          "[two_stack][contract][packed_paper_nthash][proof]") {
    const auto first = valid_paper_proof(128u);
    const auto second = valid_paper_proof(256u);
    REQUIRE(first.accepted());
    REQUIRE(second.accepted());

    auto combined = first;
    combined.combine(second);
    REQUIRE(combined.segments_combined == 2u);
    REQUIRE(combined.input_symbols
        == first.input_symbols + second.input_symbols);
    REQUIRE(combined.accepted());

    auto unchanged = first;
    unchanged.combine(PackedPaperNthashExecutionProof{});
    REQUIRE(unchanged == first);

    PackedPaperNthashExecutionProof adopted{};
    adopted.combine(first);
    REQUIRE(adopted == first);

    auto incompatible = second;
    ++incompatible.kmer_size;
    auto mismatch = first;
    mismatch.combine(incompatible);
    REQUIRE(mismatch.aggregation_mismatch);
    REQUIRE_FALSE(mismatch.accepted());

    auto saturated = first;
    saturated.input_symbols =
        (std::numeric_limits<std::uint64_t>::max)();
    saturated.combine(second);
    REQUIRE(saturated.input_symbols
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(saturated.saturated);
    REQUIRE_FALSE(saturated.accepted());
}

} // namespace
