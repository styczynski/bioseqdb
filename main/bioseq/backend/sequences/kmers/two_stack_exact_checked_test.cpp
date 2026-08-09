// Checked neutral-ABI coverage for the generated ExactSimd two-stack kernel.
// Biological comparisons in this file retain raw chronological order,
// strand, and duplicate multiplicity. Only the historic report-76 contract
// explicitly normalizes its target-geometry-dependent overlap output.

#include "common_tests.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <compare>
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
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.mm_sketch_baseline;
import bioseq.sequences.kmers.two_stacks;
import bioseq.storage.lazy;
import bioseq.storage.scanner;

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

struct exact_record final {
    std::uint64_t hash = 0u;
    std::uint64_t position = 0u;
    std::uint64_t strand = 0u;

    [[nodiscard]] constexpr auto operator<=>(
        const exact_record&) const noexcept = default;
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

    [[nodiscard]] std::vector<exact_record> decoded() const {
        std::vector<exact_record> result;
        result.reserve(records.size());
        for (const auto& record : records) {
            result.push_back({
                static_cast<std::uint64_t>(record.kmer()),
                static_cast<std::uint64_t>(record.last_pos()),
                static_cast<std::uint64_t>(record.strand())});
        }
        return result;
    }

    std::vector<MinimizerT> records;
    bool protocol_ok = true;
    bool reject_acquire = false;
    bool acquire_rejected = false;
    std::size_t raw_batches = 0u;

private:
    [[nodiscard]] static MinimizerSinkBatchView acquire(
        void* opaque, const std::size_t minimum)
    {
        auto& self = *static_cast<exact_sink*>(opaque);
        if (self.reject_acquire) {
            self.acquire_rejected = true;
            return {};
        }
        if (self.pending_ || minimum > batch_capacity) {
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
        if (self.reject_acquire) {
            self.acquire_rejected = true;
            return false;
        }
        if (self.pending_
            || layout
                != bioseq::minimizers::minimizer_output_layout_v<MinimizerT>
            || (count != 0u && raw == nullptr)) {
            self.protocol_ok = false;
            return false;
        }
        if constexpr (requires { typename MinimizerT::storage_t; }) {
            if constexpr (std::same_as<typename MinimizerT::storage_t,
                                       std::uint64_t>) {
                try {
                    const auto decoded =
                        std::span<const std::uint64_t>{raw, count}
                        | std::views::transform(
                            [](const std::uint64_t value) {
                                return MinimizerT::from_raw_storage(value);
                            });
                    self.records.insert(
                        self.records.end(), decoded.begin(), decoded.end());
                    ++self.raw_batches;
                    return true;
                } catch (...) {
                    self.protocol_ok = false;
                    return false;
                }
            }
        }
        self.protocol_ok = false;
        return false;
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

[[nodiscard]] PackedSequenceKernelView descriptor_view(
    const bioseq::storage::basic_lazy_sequence_view<seq::dna5>::
        resident_packed_region& descriptor)
{
    REQUIRE(descriptor.leading_symbol_offset == 0u);
    return {
        .storage = reinterpret_cast<const std::byte*>(descriptor.units),
        .storage_units = descriptor.unit_count,
        .begin_symbol = 0u,
        .end_symbol = static_cast<std::size_t>(descriptor.bp_length),
        .alphabet_code = alphabet_code<seq::dna5>(),
        .bits_per_symbol = descriptor.bits_per_symbol,
        .storage_unit_bytes = descriptor.storage_unit_byte_width,
        .final_segment = true,
    };
}

template<class MinimizerT>
struct run_result final {
    std::vector<exact_record> records;
    MinimizerKernelResult kernel{};
    bool sink_ok = false;
    bool acquire_rejected = false;
    std::size_t raw_batches = 0u;
    std::size_t releases = 0u;
};

template<class MinimizerT>
[[nodiscard]] run_result<MinimizerT> invoke(
    PackedSequenceKernelView sequence,
    const PackedSequenceFragmentSource fragments,
    const std::size_t window,
    const std::size_t kmer,
    const std::size_t position_base = 0u,
    const TwoStackRoute route = TwoStackRoute::chronological_continuation,
    const bool reject_sink = false,
    const MinimizerOutputLayout output_layout =
        bioseq::minimizers::minimizer_output_layout_v<MinimizerT>,
    const std::uint64_t hash_mask = static_cast<std::uint64_t>(
        MinimizerT::hash_field_mask),
    const std::uint64_t position_mask = static_cast<std::uint64_t>(
        MinimizerT::pos_field_mask))
{
    exact_sink<MinimizerT> sink;
    sink.reject_acquire = reject_sink;
    MinimizerKernelInvocation invocation{
        .sequence = sequence,
        .fragments = fragments,
        .window = window,
        .kmer = kmer,
        .emitted_position_base = position_base,
        .output_layout = output_layout,
        .output_hash_mask = hash_mask,
        .output_position_mask = position_mask,
        .two_stack_route = route,
        .sink = sink.contract(),
    };
    const auto kernel = fixed_table().minimizers.two_stack_nthash;
    REQUIRE(kernel != nullptr);
    const auto result = kernel(invocation);
    return {
        .records = sink.decoded(),
        .kernel = result,
        .sink_ok = sink.protocol_ok,
        .acquire_rejected = sink.acquire_rejected,
        .raw_batches = sink.raw_batches,
    };
}

template<sequence_alphabet Alphabet, class StorageUnit,
         class MinimizerT = MinimapSketchData>
[[nodiscard]] run_result<MinimizerT> run_resident(
    const std::vector<Alphabet>& data,
    const std::size_t window,
    const std::size_t kmer,
    const std::size_t position_base = 0u,
    const TwoStackRoute route = TwoStackRoute::chronological_continuation,
    const bool reject_sink = false)
{
    using config = sequence_custom_alphabet<Alphabet, StorageUnit>;
    compact_sequence_range<config> packed{std::vector<Alphabet>(data)};
    return invoke<MinimizerT>(
        packed_view(packed), {}, window, kmer, position_base, route,
        reject_sink);
}

template<class MinimizerT = MinimapSketchData>
[[nodiscard]] std::vector<exact_record> dna4_oracle(
    const std::vector<seq::dna4>& data,
    const std::size_t window,
    const std::size_t kmer,
    const std::size_t position_base = 0u)
{
    if (data.empty()) return {};
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<config> packed{std::vector<seq::dna4>(data)};
    bioseq::minimizers::sketch_collector_t<MinimizerT> collector;
    bioseq::simd::collect_minimizers_view<
        bioseq::simd::minimap_int_hasher, MinimizerT>(
            collector, packed, packed.begin(), packed.end(), window, kmer);
    std::vector<exact_record> result;
    for (const auto& record : collector.extract_sketches_data()) {
        result.push_back({
            record.kmer(), record.last_pos() + position_base, record.strand()});
    }
    return result;
}

[[nodiscard]] std::vector<exact_record> dna5_oracle(
    const std::vector<seq::dna5>& data,
    const std::size_t window,
    const std::size_t kmer)
{
    std::string bases;
    bases.reserve(data.size());
    for (const auto symbol : data) bases.push_back(symbol.to_char());
    const auto entries = bioseq::mm_sketch_baseline_ns::mm_sketch(
        bases.data(),
        static_cast<int>(bases.size()),
        static_cast<int>(window),
        static_cast<int>(kmer));
    std::vector<exact_record> result;
    result.reserve(entries.size());
    for (const auto& entry : entries) {
        result.push_back({
            .hash = entry.x >> 8u,
            .position = (entry.y >> 1u) & std::uint64_t{0xFFFFFFFFu},
            .strand = entry.y & 1u,
        });
    }
    return result;
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

[[nodiscard]] std::vector<seq::dna5> make_dna5(
    const std::string_view bases)
{
    std::vector<seq::dna5> result;
    result.reserve(bases.size());
    for (const char base : bases) {
        seq::dna5 symbol{};
        symbol.assign_char(base);
        result.push_back(symbol);
    }
    return result;
}

template<class MinimizerT>
void require_success(
    const run_result<MinimizerT>& result,
    const std::vector<exact_record>& expected,
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
    const auto& proof = result.kernel.proof.two_stack;
    REQUIRE(proof.scalar_backend_branches == 0u);
    REQUIRE(proof.scalar_tail_symbols == 0u);
    REQUIRE(proof.scalar_hash_symbols == 0u);
    REQUIRE(proof.scalar_window_transitions == 0u);
    REQUIRE(proof.scalar_event_records == 0u);
    REQUIRE(proof.scalar_sort_records == 0u);
    REQUIRE(proof.scalar_dedup_records == 0u);
    REQUIRE(proof.fail_closed_rejections == 0u);
    REQUIRE(proof.accepted(require_vector_work));
}

template<sequence_alphabet Alphabet, class StorageUnit>
struct owned_fragment final {
    std::vector<StorageUnit> units;
    std::size_t bp_size = 0u;
    std::size_t fragment_begin = 0u;
    std::size_t consume_begin = 0u;
    std::size_t consume_end = 0u;
};

template<sequence_alphabet Alphabet, class StorageUnit>
struct fragment_source final {
    std::vector<owned_fragment<Alphabet, StorageUnit>> fragments;
    std::size_t releases = 0u;

    [[nodiscard]] static PackedSequenceKernelFragment read(
        void* opaque, const std::size_t index)
    {
        const auto& fragment =
            static_cast<fragment_source*>(opaque)->fragments.at(index);
        return {
            .storage = fragment.units.empty() ? nullptr
                : reinterpret_cast<const std::byte*>(fragment.units.data()),
            .storage_units = fragment.units.size(),
            .fragment_bp_size = fragment.bp_size,
            .fragment_begin_bp = fragment.fragment_begin,
            .consume_begin_bp = fragment.consume_begin,
            .consume_end_bp = fragment.consume_end,
        };
    }

    static void release(void* opaque, std::size_t) noexcept {
        ++static_cast<fragment_source*>(opaque)->releases;
    }
};

template<sequence_alphabet Alphabet, class StorageUnit>
[[nodiscard]] owned_fragment<Alphabet, StorageUnit> make_fragment(
    const std::vector<Alphabet>& data,
    const std::size_t storage_begin,
    const std::size_t storage_end,
    const std::size_t fragment_begin,
    const std::size_t consume_begin,
    const std::size_t consume_end)
{
    REQUIRE(storage_begin <= storage_end);
    REQUIRE(storage_end <= data.size());
    std::vector<Alphabet> slice(
        data.begin() + static_cast<std::ptrdiff_t>(storage_begin),
        data.begin() + static_cast<std::ptrdiff_t>(storage_end));
    using config = sequence_custom_alphabet<Alphabet, StorageUnit>;
    compact_sequence_range<config> packed(slice);
    owned_fragment<Alphabet, StorageUnit> result{
        .bp_size = slice.size(),
        .fragment_begin = fragment_begin,
        .consume_begin = consume_begin,
        .consume_end = consume_end,
    };
    if (packed.size() != 0u) {
        const auto* first = packed.read_cptr(packed.cbegin());
        result.units.assign(first, first + packed.size());
    }
    return result;
}

template<sequence_alphabet Alphabet, class StorageUnit,
         class MinimizerT = MinimapSketchData>
[[nodiscard]] run_result<MinimizerT> run_fragmented(
    const std::vector<Alphabet>& data,
    const std::vector<std::pair<std::size_t, std::size_t>>& ranges,
    const std::size_t window,
    const std::size_t kmer,
    const std::size_t position_base = 0u)
{
    using config = sequence_custom_alphabet<Alphabet, StorageUnit>;
    using traits = compact_sequence_range<config>::traits;
    fragment_source<Alphabet, StorageUnit> source;
    source.fragments.reserve(ranges.size());
    for (const auto [begin, end] : ranges) {
        source.fragments.push_back(make_fragment<Alphabet, StorageUnit>(
            data, begin, end, begin, begin, end));
    }
    PackedSequenceKernelView topology{
        .storage = nullptr,
        .storage_units = (data.size() + traits::letters_per_unit - 1u)
            / traits::letters_per_unit,
        .begin_symbol = 0u,
        .end_symbol = data.size(),
        .alphabet_code = alphabet_code<Alphabet>(),
        .bits_per_symbol = static_cast<std::uint8_t>(traits::bits_per_char),
        .storage_unit_bytes = static_cast<std::uint8_t>(sizeof(StorageUnit)),
        .final_segment = true,
    };
    const PackedSequenceFragmentSource fragments{
        .state = &source,
        .first_fragment = 0u,
        .fragment_end = source.fragments.size(),
        .read = &fragment_source<Alphabet, StorageUnit>::read,
        .release = &fragment_source<Alphabet, StorageUnit>::release,
    };
    auto result = invoke<MinimizerT>(
        topology, fragments, window, kmer, position_base);
    result.releases = source.releases;
    return result;
}

template<class StorageUnit>
void require_storage_width(const std::vector<seq::dna4>& source) {
    using config = sequence_custom_alphabet<seq::dna4, StorageUnit>;
    using range_type = compact_sequence_range<config>;
    constexpr std::size_t letters_per_unit =
        range_type::traits::letters_per_unit;
    const std::size_t bounded_end = std::min(
        source.size(), 12u * letters_per_unit);
    std::vector<seq::dna4> bounded(
        source.begin() + static_cast<std::ptrdiff_t>(letters_per_unit),
        source.begin() + static_cast<std::ptrdiff_t>(bounded_end));
    for (const auto& data : {bounded, source}) {
        CAPTURE(sizeof(StorageUnit), data.size());
        const auto result = run_resident<seq::dna4, StorageUnit>(data, 5u, 7u);
        require_success(result, dna4_oracle(data, 5u, 7u));
        REQUIRE(result.kernel.proof.two_stack.bp_consumed == data.size());
        REQUIRE(result.kernel.proof.two_stack.packed_decode_lanes
            == data.size());
    }
}

TEST_CASE("ExactSimd checked table — DNA4 packed widths preserve raw order",
          "[two_stack][exact][checked][packed_widths]") {
    const auto data = fixture(391u);
    require_storage_width<std::uint8_t>(data);
    require_storage_width<std::uint16_t>(data);
    require_storage_width<std::uint32_t>(data);
    require_storage_width<std::uint64_t>(data);
}

TEST_CASE("ExactSimd checked table — K15/W10 every partial B8 tail",
          "[two_stack][exact][checked][tail][k15_w10_b8]") {
    using representation = MinimizerRepr<33u, 15u>;
    constexpr std::size_t initial_codes = 14u;
    constexpr std::size_t full_block = 8u;
    bool saw_masked_tail = false;
    for (std::size_t tail = 1u; tail < full_block; ++tail) {
        CAPTURE(tail);
        const auto data = fixture(initial_codes + full_block + tail);
        const auto result = run_resident<
            seq::dna4, std::uint32_t, representation>(data, 10u, 15u);
        require_success(result, dna4_oracle<representation>(data, 10u, 15u));
        REQUIRE(result.kernel.proof.two_stack.vector_lanes_visited
            == full_block + tail);
        saw_masked_tail = saw_masked_tail
            || result.kernel.proof.two_stack.masked_tail_lanes > 0u;
    }
    REQUIRE(saw_masked_tail);
}

TEST_CASE("ExactSimd checked table — packed22 adversarial cuts preserve chronology",
          "[two_stack][exact][checked][fragmented][packed22]") {
    using representation = MinimizerRepr<33u, 15u>;
    const auto data = fixture(257u);
    const auto expected = dna4_oracle<representation>(data, 10u, 15u);
    const std::array cut_sets{
        std::vector<std::size_t>{
            0u, 1u, 7u, 13u, 14u, 15u, 16u, 17u, 21u, 22u, 23u,
            29u, 30u, 31u, 32u, 33u, 47u, 64u, 65u, 127u, 128u,
            129u, data.size()},
        std::vector<std::size_t>{
            0u, 3u, 11u, 18u, 19u, 20u, 26u, 27u, 34u, 35u, 41u,
            42u, 49u, 50u, 113u, 181u, data.size()},
    };
    for (const auto& cuts : cut_sets) {
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        for (std::size_t index = 1u; index < cuts.size(); ++index) {
            ranges.emplace_back(cuts[index - 1u], cuts[index]);
        }
        const auto result = run_fragmented<
            seq::dna4, std::uint32_t, representation>(
                data, ranges, 10u, 15u);
        require_success(result, expected);
        REQUIRE(result.releases == ranges.size());
    }

    std::vector<std::pair<std::size_t, std::size_t>> one_base;
    one_base.reserve(data.size());
    for (std::size_t position = 0u; position < data.size(); ++position) {
        one_base.emplace_back(position, position + 1u);
    }
    const auto result = run_fragmented<
        seq::dna4, std::uint32_t, representation>(
            data, one_base, 10u, 15u);
    require_success(result, expected);
}

TEST_CASE("ExactSimd checked table — resident report-76 fingerprint is locked",
          "[two_stack][exact][checked][resident][report76]") {
    const auto data = fixture(4096u);
    auto result = run_resident<seq::dna4, std::uint32_t>(
        data, 10u, 15u, 0u, TwoStackRoute::resident_report76);
    REQUIRE(result.kernel.status == MinimizerKernelStatus::ok);
    REQUIRE(result.kernel.proof.algorithm
        == MinimizerKernelAlgorithm::two_stack_nthash);
    REQUIRE(result.sink_ok);
    REQUIRE(result.kernel.emitted == result.records.size());
    const auto raw_size = result.records.size();
    std::ranges::sort(result.records);
    result.records.erase(
        std::ranges::unique(result.records).begin(), result.records.end());
    std::uint64_t fingerprint = 1469598103934665603ULL;
    for (const auto& record : result.records) {
        const auto raw = MinimapSketchData{
            record.hash, record.position, record.strand}.raw_value();
        fingerprint = (fingerprint ^ raw) * 1099511628211ULL;
    }
    REQUIRE(result.records.size() == 754u);
    REQUIRE(fingerprint == 0x0f4a56dc592b279dULL);
    const auto& proof = result.kernel.proof.two_stack;
    REQUIRE(proof.accepted(true));
    REQUIRE(proof.resident_vector_stream_kernel_calls == 1u);
    REQUIRE(proof.resident_vector_stream_dual_calls == 1u);
    REQUIRE(proof.resident_vector_stream_input_bp == data.size());
    REQUIRE(proof.resident_vector_stream_minimizers_emitted == raw_size);
    REQUIRE(proof.generic_u64_kernel_calls == 0u);
    REQUIRE(proof.k15_w10_b8_u32_hash_kernel_calls == 0u);
    REQUIRE(proof.scalar_backend_branches == 0u);
}

TEST_CASE("ExactSimd checked table — K15/W10 tie chronology is exact",
          "[two_stack][exact][checked][ties]") {
    using representation = MinimizerRepr<33u, 15u>;
    std::vector<seq::dna4> data(160u);
    for (auto& symbol : data) symbol.assign_rank(0u);
    const auto result = run_resident<
        seq::dna4, std::uint32_t, representation>(data, 10u, 15u);
    require_success(result, dna4_oracle<representation>(data, 10u, 15u));
    const auto& proof = result.kernel.proof.two_stack;
    REQUIRE(proof.tie_matrix_event_batches > 0u);
    REQUIRE((proof.first_window_tie_events > 0u
        || proof.expiry_tie_events > 0u));
    REQUIRE(proof.tie_hash_prefilter_batches > 0u);
    REQUIRE(proof.tie_raw_verification_batches > 0u);
}

TEST_CASE("ExactSimd checked table — reachable K15 hash sentinel is not emitted",
          "[two_stack][exact][checked][sentinel]") {
    using representation = MinimizerRepr<33u, 15u>;
    constexpr std::string_view sentinel_kmer = "AACATATAAATTCAA";
    std::uint32_t forward = 0u;
    std::uint32_t reverse = 0u;
    for (std::size_t index = 0u; index < sentinel_kmer.size(); ++index) {
        seq::dna4 symbol{};
        symbol.assign_char(sentinel_kmer[index]);
        const std::uint32_t code = symbol.to_rank();
        forward = ((forward << 2u) | code) & 0x3fffffffu;
        reverse |= (code ^ 3u) << (2u * index);
    }
    const std::uint32_t canonical = std::min(forward, reverse);
    REQUIRE((bioseq::simd::minimap_int_hasher<seq::dna4>::template
        scramble<std::uint32_t>(canonical, std::uint32_t{0x3fffffffu}))
        == 0x3fffffffu);
    const std::string bases =
        std::string{"GTCGAGTACCGTACGATCGTACGATCG"}
        + std::string{sentinel_kmer}
        + std::string{"CTAGGCTACGATTCGACGTAGCTAGCA"};
    std::vector<seq::dna4> data;
    data.reserve(bases.size());
    for (const char base : bases) {
        seq::dna4 symbol{};
        symbol.assign_char(base);
        data.push_back(symbol);
    }
    const auto result = run_resident<
        seq::dna4, std::uint32_t, representation>(data, 10u, 15u);
    require_success(result, dna4_oracle<representation>(data, 10u, 15u));
    REQUIRE(std::ranges::none_of(result.records, [](const exact_record& record) {
        return record.hash == 0x3fffffffull;
    }));
}

TEST_CASE("ExactSimd checked table — 33-bit emitted positions remain exact",
          "[two_stack][exact][checked][position]") {
    using representation = MinimizerRepr<33u, 15u>;
    constexpr std::size_t output_base = static_cast<std::size_t>(
        (std::numeric_limits<std::uint32_t>::max)()) + 8192u;
    const auto data = fixture(257u);
    const auto result = run_resident<
        seq::dna4, std::uint32_t, representation>(
            data, 10u, 15u, output_base);
    require_success(
        result, dna4_oracle<representation>(data, 10u, 15u, output_base));
    REQUIRE(!result.records.empty());
    REQUIRE(std::ranges::all_of(result.records, [](const exact_record& record) {
        return record.position
            > (std::numeric_limits<std::uint32_t>::max)();
    }));
}

TEST_CASE("ExactSimd checked table — unaligned u32 ownership is exact",
          "[two_stack][exact][checked][unaligned]") {
    using representation = MinimizerRepr<33u, 15u>;
    constexpr std::size_t begin = 13u;
    constexpr std::size_t end = 181u;
    const auto data = fixture(224u);
    std::vector<seq::dna4> slice(
        data.begin() + static_cast<std::ptrdiff_t>(begin),
        data.begin() + static_cast<std::ptrdiff_t>(end));
    fragment_source<seq::dna4, std::uint32_t> source;
    source.fragments.push_back(make_fragment<seq::dna4, std::uint32_t>(
        data, 0u, data.size(), 0u, begin, end));
    using traits = compact_sequence_range<
        sequence_custom_alphabet<seq::dna4, std::uint32_t>>::traits;
    const PackedSequenceKernelView topology{
        .storage = nullptr,
        .storage_units = (data.size() + traits::letters_per_unit - 1u)
            / traits::letters_per_unit,
        .begin_symbol = 0u,
        .end_symbol = data.size(),
        .alphabet_code = alphabet_code<seq::dna4>(),
        .bits_per_symbol = static_cast<std::uint8_t>(traits::bits_per_char),
        .storage_unit_bytes = sizeof(std::uint32_t),
        .final_segment = true,
    };
    const PackedSequenceFragmentSource fragments{
        .state = &source,
        .first_fragment = 0u,
        .fragment_end = 1u,
        .read = &fragment_source<seq::dna4, std::uint32_t>::read,
        .release = &fragment_source<seq::dna4, std::uint32_t>::release,
    };
    const auto result = invoke<representation>(
        topology, fragments, 10u, 15u);
    require_success(result, dna4_oracle<representation>(slice, 10u, 15u));
    REQUIRE(result.kernel.proof.two_stack.packed_word_boundary_merges > 0u);
}

TEST_CASE("ExactSimd checked table — DNA5 T normalization and N resets",
          "[two_stack][exact][checked][dna5][reset][scanner]") {
    constexpr std::size_t window = 10u;
    constexpr std::size_t kmer = 15u;
    constexpr std::size_t size = 311u;
    std::string bases(size, 'A');
    constexpr std::string_view motif = "ACGTTGCAGATCCGTA";
    for (std::size_t index = 0u; index < bases.size(); ++index) {
        bases[index] = motif[index % motif.size()];
    }
    constexpr std::array<std::size_t, 9u> n_positions{
        0u, 6u, 41u, 42u, 43u, 127u, 211u, 289u, size - 1u};
    for (const std::size_t position : n_positions) bases[position] = 'N';
    const auto data = make_dna5(bases);
    const auto expected = dna5_oracle(data, window, kmer);

    const auto resident = run_resident<seq::dna5, std::uint8_t>(
        data, window, kmer);
    require_success(resident, expected);
    REQUIRE(resident.kernel.proof.two_stack.ambiguous_symbols
        == n_positions.size());
    REQUIRE(resident.kernel.proof.two_stack.ambiguous_reset_batches > 0u);

    const auto fragmented = run_fragmented<seq::dna5, std::uint64_t>(
        data,
        {{0u, 41u}, {41u, 42u}, {42u, 43u}, {43u, 127u},
         {127u, 211u}, {211u, 289u}, {289u, data.size()}},
        window,
        kmer);
    require_success(fragmented, expected);
    REQUIRE(fragmented.kernel.proof.two_stack.ambiguous_symbols
        == n_positions.size());
    REQUIRE(fragmented.records == resident.records);

    using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using range_type = compact_sequence_range<dna5_u64>;
    using scanner_type =
        bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, 2u>;
    range_type packed{std::vector<seq::dna5>(data)};
    bioseq::storage::lazy_compact_sequence<dna5_u64, scanner_type> scanner{
        scanner_type::from_range(packed)};
    const auto scanner_view = bioseq::storage::as_lazy_sequence_view(scanner);
    const std::uint64_t admitted_bytes =
        bioseq::storage::packed_region_admission_bytes<seq::dna5>(
            scanner_view.bp_size(), 4u);
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(admitted_bytes);
    REQUIRE(permit.has_value());
    auto canonical = bioseq::storage::materialize_packed_region(
        scanner_view, 0u, scanner_view.bp_size(), 4u, std::move(*permit));
    const auto scanner_result = invoke<MinimapSketchData>(
        descriptor_view(canonical.descriptor()), {}, window, kmer);
    require_success(scanner_result, expected);
    REQUIRE(scanner_result.records == resident.records);
}

TEST_CASE("ExactSimd checked table — scanner bounded materialization preserves tails",
          "[two_stack][exact][checked][scanner][bounded][tail]") {
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using scanner_type =
        bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, 2u>;
    const auto data = fixture(391u);
    range_type packed{std::vector<seq::dna4>(data)};
    bioseq::storage::lazy_compact_sequence<sequence_type, scanner_type> scanner{
        scanner_type::from_range(packed)};
    const auto view = bioseq::storage::as_lazy_sequence_view(scanner);
    constexpr std::size_t begin = range_type::traits::letters_per_unit;
    const std::size_t end = static_cast<std::size_t>(view.bp_size());
    const std::uint64_t admitted_bytes =
        bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            end - begin, 4u);
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(admitted_bytes);
    REQUIRE(permit.has_value());
    auto canonical = bioseq::storage::materialize_packed_region(
        view, begin, end, 4u, std::move(*permit));
    const auto& descriptor = canonical.descriptor();
    REQUIRE(descriptor.leading_symbol_offset == 0u);
    const PackedSequenceKernelView kernel_view{
        .storage = reinterpret_cast<const std::byte*>(descriptor.units),
        .storage_units = descriptor.unit_count,
        .begin_symbol = 0u,
        .end_symbol = static_cast<std::size_t>(descriptor.bp_length),
        .alphabet_code = alphabet_code<seq::dna4>(),
        .bits_per_symbol = descriptor.bits_per_symbol,
        .storage_unit_bytes = descriptor.storage_unit_byte_width,
        .final_segment = true,
    };
    std::vector<seq::dna4> slice(
        data.begin() + static_cast<std::ptrdiff_t>(begin), data.end());
    const auto result = invoke<MinimapSketchData>(
        kernel_view, {}, 5u, 7u);
    require_success(result, dna4_oracle(slice, 5u, 7u));
    REQUIRE(result.kernel.proof.two_stack.bp_consumed == slice.size());
    REQUIRE(result.kernel.proof.two_stack.masked_tail_lanes > 0u);
}

TEST_CASE("ExactSimd checked table — invalid shapes fail before core work",
          "[two_stack][exact][checked][rejection]") {
    const auto data = fixture(96u);
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    compact_sequence_range<config> packed{std::vector<seq::dna4>(data)};
    const auto view = packed_view(packed);

    for (const auto [window, kmer] :
         std::array<std::pair<std::size_t, std::size_t>, 4u>{
             std::pair{0u, 7u},
             std::pair{two_stack_max_window + 1u, 7u},
             std::pair{5u, 0u},
             std::pair{5u, two_stack_exact_max_kmer + 1u}}) {
        CAPTURE(window, kmer);
        const auto result = invoke<MinimapSketchData>(
            view, {}, window, kmer);
        REQUIRE(result.kernel.status
            == MinimizerKernelStatus::invalid_configuration);
        REQUIRE(result.kernel.emitted == 0u);
        REQUIRE(result.records.empty());
        REQUIRE(result.kernel.proof.two_stack.vector_batches == 0u);
        REQUIRE(result.kernel.proof.two_stack.scalar_backend_branches == 0u);
    }

    const auto bad_layout = invoke<MinimapSketchData>(
        view, {}, 5u, 7u, 0u, TwoStackRoute::chronological_continuation,
        false, MinimizerOutputLayout::unsupported);
    REQUIRE(bad_layout.kernel.status
        == MinimizerKernelStatus::invalid_configuration);
    REQUIRE(bad_layout.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::unsupported_representation));
    REQUIRE(bad_layout.records.empty());

    const auto bad_mask = invoke<MinimapSketchData>(
        view, {}, 5u, 7u, 0u, TwoStackRoute::chronological_continuation,
        false,
        bioseq::minimizers::minimizer_output_layout_v<MinimapSketchData>,
        MinimapSketchData::hash_field_mask ^ 1u,
        MinimapSketchData::pos_field_mask);
    REQUIRE(bad_mask.kernel.status
        == MinimizerKernelStatus::invalid_configuration);
    REQUIRE(bad_mask.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::unsupported_representation));
    REQUIRE(bad_mask.kernel.proof.two_stack.fail_closed_rejections == 1u);

    using two_word_representation = MinimizerRepr<63u, 15u>;
    const auto two_word = invoke<two_word_representation>(
        view, {}, 10u, 15u);
    REQUIRE(two_word.kernel.status
        == MinimizerKernelStatus::invalid_configuration);
    REQUIRE(two_word.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::unsupported_representation));
    REQUIRE(two_word.kernel.emitted == 0u);
    REQUIRE(two_word.records.empty());
    REQUIRE(two_word.kernel.proof.two_stack.vector_batches == 0u);
    REQUIRE(two_word.kernel.proof.two_stack.scalar_backend_branches == 0u);

    auto unknown_alphabet = view;
    unknown_alphabet.alphabet_code = 0xffu;
    const auto bad_alphabet = invoke<MinimapSketchData>(
        unknown_alphabet, {}, 5u, 7u);
    REQUIRE(bad_alphabet.kernel.status
        == MinimizerKernelStatus::invalid_configuration);
    REQUIRE(bad_alphabet.records.empty());
}

TEST_CASE("ExactSimd checked table — representation position overflow is rejected",
          "[two_stack][exact][checked][overflow][representation]") {
    constexpr std::size_t sequence_size = 32u;
    constexpr std::uint64_t position_mask = MinimapSketchData::pos_field_mask;
    constexpr std::size_t position_base = static_cast<std::size_t>(
        position_mask - (sequence_size - 2u));
    const auto data = fixture(sequence_size);
    const auto result = run_resident<seq::dna4, std::uint32_t>(
        data, 10u, 15u, position_base);
    REQUIRE(result.kernel.status
        == MinimizerKernelStatus::invalid_configuration);
    REQUIRE(result.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::unsupported_representation));
    REQUIRE(result.kernel.emitted == 0u);
    REQUIRE(result.records.empty());
    REQUIRE(result.kernel.proof.two_stack.vector_batches == 0u);
    REQUIRE(result.kernel.proof.two_stack.scalar_backend_branches == 0u);
}

TEST_CASE("ExactSimd checked table — invalid fragment topology fails closed",
          "[two_stack][exact][checked][fragmented][invalid]") {
    const auto data = fixture(32u);
    using config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using traits = compact_sequence_range<config>::traits;
    fragment_source<seq::dna4, std::uint64_t> source;
    source.fragments.push_back(make_fragment<seq::dna4, std::uint64_t>(
        data, 0u, data.size(), 0u, 9u, 8u));
    const PackedSequenceKernelView topology{
        .storage = nullptr,
        .storage_units = (data.size() + traits::letters_per_unit - 1u)
            / traits::letters_per_unit,
        .begin_symbol = 0u,
        .end_symbol = data.size(),
        .alphabet_code = alphabet_code<seq::dna4>(),
        .bits_per_symbol = static_cast<std::uint8_t>(traits::bits_per_char),
        .storage_unit_bytes = sizeof(std::uint64_t),
        .final_segment = true,
    };
    const PackedSequenceFragmentSource reversed{
        .state = &source,
        .first_fragment = 0u,
        .fragment_end = 1u,
        .read = &fragment_source<seq::dna4, std::uint64_t>::read,
        .release = &fragment_source<seq::dna4, std::uint64_t>::release,
    };
    const auto reversed_result = invoke<MinimapSketchData>(
        topology, reversed, 5u, 7u);
    REQUIRE(reversed_result.kernel.status
        == MinimizerKernelStatus::invalid_sequence_view);
    REQUIRE(reversed_result.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::invalid_range));
    REQUIRE(reversed_result.records.empty());
    REQUIRE(source.releases == 1u);

    fragment_source<seq::dna4, std::uint64_t> beyond_source;
    beyond_source.fragments.push_back(
        make_fragment<seq::dna4, std::uint64_t>(
            data, 0u, data.size(), 0u, 0u, data.size() + 1u));
    const PackedSequenceFragmentSource beyond{
        .state = &beyond_source,
        .first_fragment = 0u,
        .fragment_end = 1u,
        .read = &fragment_source<seq::dna4, std::uint64_t>::read,
        .release = &fragment_source<seq::dna4, std::uint64_t>::release,
    };
    const auto beyond_result = invoke<MinimapSketchData>(
        topology, beyond, 5u, 7u);
    REQUIRE(beyond_result.kernel.status
        == MinimizerKernelStatus::invalid_sequence_view);
    REQUIRE(beyond_result.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::invalid_range));
    REQUIRE(beyond_result.records.empty());
    REQUIRE(beyond_source.releases == 1u);

    fragment_source<seq::dna4, std::uint64_t> gapped_source;
    gapped_source.fragments.push_back(
        make_fragment<seq::dna4, std::uint64_t>(
            data, 0u, 3u, 0u, 0u, 3u));
    gapped_source.fragments.push_back(
        make_fragment<seq::dna4, std::uint64_t>(
            data, 4u, 5u, 4u, 4u, 5u));
    const PackedSequenceFragmentSource gapped{
        .state = &gapped_source,
        .first_fragment = 0u,
        .fragment_end = 2u,
        .read = &fragment_source<seq::dna4, std::uint64_t>::read,
        .release = &fragment_source<seq::dna4, std::uint64_t>::release,
    };
    const auto gapped_result = invoke<MinimapSketchData>(
        topology, gapped, 5u, 7u);
    REQUIRE(gapped_result.kernel.status
        == MinimizerKernelStatus::invalid_sequence_view);
    REQUIRE(gapped_result.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::non_contiguous_range));
    REQUIRE(gapped_result.records.empty());
    REQUIRE(gapped_source.releases == 2u);

    const PackedSequenceFragmentSource missing_callbacks{
        .state = &source,
        .first_fragment = 0u,
        .fragment_end = 1u,
        .read = &fragment_source<seq::dna4, std::uint64_t>::read,
        .release = nullptr,
    };
    const auto missing = invoke<MinimapSketchData>(
        topology, missing_callbacks, 5u, 7u);
    REQUIRE(missing.kernel.status
        == MinimizerKernelStatus::invalid_continuation);
    REQUIRE(missing.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::invalid_state));
    REQUIRE(missing.records.empty());
}

TEST_CASE("ExactSimd checked table — sink rejection and calls are isolated",
          "[two_stack][exact][checked][sink][lifecycle]") {
    const auto data = fixture(197u);
    const auto rejected = run_resident<seq::dna4, std::uint64_t>(
        data, 5u, 7u, 0u, TwoStackRoute::chronological_continuation, true);
    REQUIRE(rejected.kernel.status == MinimizerKernelStatus::sink_rejected);
    REQUIRE(rejected.kernel.family_status == static_cast<std::uint32_t>(
        TwoStackContinuationStatus::collector_overflow));
    REQUIRE(rejected.acquire_rejected);
    REQUIRE(rejected.records.empty());
    REQUIRE(rejected.kernel.proof.two_stack.scalar_event_records == 0u);

    const auto first = run_resident<seq::dna4, std::uint64_t>(data, 5u, 7u);
    const auto second = run_resident<seq::dna4, std::uint64_t>(data, 5u, 7u);
    const auto expected = dna4_oracle(data, 5u, 7u);
    require_success(first, expected);
    require_success(second, expected);
    REQUIRE(first.records == second.records);
    REQUIRE(first.kernel.proof.two_stack == second.kernel.proof.two_stack);
}

}  // namespace
