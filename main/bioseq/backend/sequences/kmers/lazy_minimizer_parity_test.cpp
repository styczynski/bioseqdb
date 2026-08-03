// main/bioseq/backend/sequences/kmers/lazy_minimizer_parity_test.cpp
//
// DOC0263 §8.4 / §11.2.1 — minimizer parity under fragmentation.
//
// The fragmented minimizer path routes through the SCALAR `collect_minimizers`
// kernel over a lazy_compact_sequence's fragment-streaming BP iterator (rather
// than the SIMD lane-replay the spec's §8 sketches, whose boundary emit-gate is
// documented as fragile in §11.2.2). The scalar kernel is kept bit-identical to
// the SIMD two-stack kernel by the lock-step contract in
// two_stacks_minimizers_test.cpp, so:
//
//   scalar(all-in-RAM range)  ==  scalar(chunked lazy)  ==  SIMD(all-in-RAM)
//
// This test locks the first equality (the Phase-B claim): the minimizer set a
// fragmented scanner produces is bit-identical to the all-in-RAM baseline, for
// several sequence lengths and the three fragment sizes from §11.1 (32, 64, 1
// storage-unit fragments → 1024/2048/32 letters per fragment on dna4/uint64).

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.kmers.vulkan;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.storage.batch_processor;
import bioseq.storage.size;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

namespace hn = hwy::HWY_NAMESPACE;
namespace pipe = bioseq::simd::HWY_NAMESPACE::pipe;
namespace vk = ::bioseq::gpu::vulkan;

[[nodiscard]] const ::bioseq::simd_dispatch::HighwayKernelTable*
fixed_highway_kernels() noexcept {
    return &::bioseq::simd_dispatch::highway_test_kernel_table();
}

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;
using facade_t = bioseq::storage::basic_lazy_sequence_view<seq::dna4>;
using sequence_size_t = bioseq::storage::sequence_size_t;

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
    sequence_size_t from{};
    sequence_size_t to{};
    bool reversed{};
    bool complemented{};
};

struct composite_source_state {
    std::vector<std::vector<std::uint8_t>> sources;
    std::vector<source_piece> pieces;
    sequence_size_t logical_size{};
    mutable std::size_t callback_count{};
    mutable sequence_size_t requested_bp{};
    mutable std::size_t max_request{};
    bool reject_whole_request{true};
};

[[nodiscard]] std::uint8_t composite_rank_at(
        const composite_source_state& state,
        sequence_size_t logical_position) {
    for (const source_piece& piece : state.pieces) {
        const sequence_size_t piece_size = piece.to - piece.from;
        if (logical_position >= piece_size) {
            logical_position -= piece_size;
            continue;
        }
        const sequence_size_t source_position = piece.reversed
            ? piece.to - 1u - logical_position
            : piece.from + logical_position;
        std::uint8_t rank = state.sources[piece.source_index]
            [static_cast<std::size_t>(source_position)];
        if (piece.complemented) {
            rank = static_cast<std::uint8_t>(3u - rank);
        }
        return rank;
    }
    throw std::out_of_range("composite source logical position");
}

template <bool IsReversed, bool WriteReversed>
[[nodiscard]] const std::uint8_t* composite_ptr_at(
        const void* opaque,
        sequence_size_t bp_from,
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
    const sequence_size_t forward_from = IsReversed
        ? state.logical_size - (bp_from + required_length)
        : bp_from;
    constexpr bool descending_write =
        (IsReversed && !WriteReversed) || (!IsReversed && WriteReversed);
    for (std::size_t index = 0u; index < required_length; ++index) {
        std::uint8_t rank = composite_rank_at(state, forward_from + index);
        if constexpr (IsReversed) {
            rank = static_cast<std::uint8_t>(3u - rank);
        }
        scratch[descending_write ? required_length - 1u - index : index] = rank;
    }
    return scratch.data();
}

[[nodiscard]] sequence_size_t composite_bp_size(const void* opaque) noexcept {
    return static_cast<const composite_source_state*>(opaque)->logical_size;
}

[[nodiscard]] facade_t::resident_probe_result composite_resident_probe(
        const void*, sequence_size_t, sequence_size_t, std::uint8_t) noexcept {
    return std::nullopt;
}

[[nodiscard]] bool composite_worker_shareable(const void*) noexcept {
    return false;
}

inline const facade_t::backend_ops composite_ops{
    .bp_size = &composite_bp_size,
    .ptr_at = {
        &composite_ptr_at<false, false>,
        &composite_ptr_at<false, true>,
        &composite_ptr_at<true, false>,
        &composite_ptr_at<true, true>,
    },
    .try_resident_packed = &composite_resident_probe,
    .copy_full_packed = nullptr,
    .worker_shareable = &composite_worker_shareable,
};

[[nodiscard]] facade_t make_composite_source_view(
        const composite_source_state& state) noexcept {
    return facade_t{std::addressof(state), std::addressof(composite_ops)};
}

void reset_source_observation(composite_source_state& state) noexcept {
    state.callback_count = 0u;
    state.requested_bp = 0u;
    state.max_request = 0u;
}

[[nodiscard]] std::vector<seq::dna4> materialize_logical_letters(
        const composite_source_state& state) {
    std::vector<seq::dna4> letters(static_cast<std::size_t>(state.logical_size));
    for (sequence_size_t position = 0u; position < state.logical_size;
         ++position) {
        letters[static_cast<std::size_t>(position)].assign_rank(
            composite_rank_at(state, position));
    }
    return letters;
}

struct composite_fixture {
    composite_source_state source;
    std::vector<seq::dna4> selected_letters;
    sequence_size_t selected_from{};
    sequence_size_t selected_to{};
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
    constexpr sequence_size_t selected_from = 2'500u;
    constexpr sequence_size_t selected_to = 10'500u;
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

[[nodiscard]] std::vector<minimizer_record> canonical_records(
        const sketch_collector& collector) {
    std::vector<minimizer_record> records;
    records.reserve(collector.extract_sketches_data().size());
    for (const MinimapSketchData& minimizer :
         collector.extract_sketches_data()) {
        records.emplace_back(
            minimizer.last_pos(), minimizer.kmer(), minimizer.strand());
    }
    std::sort(records.begin(), records.end());
    return records;
}

struct minimizer_run {
    std::vector<minimizer_record> records;
    pipe::MinimizerRangeExecutionProof proof;
};

[[nodiscard]] minimizer_run run_dispatch(
        facade_t view,
        sequence_size_t from,
        sequence_size_t to,
        MinimizerSketchBackend backend) {
    sketch_collector collector;
    auto proof = pipe::minimizer_dispatch_range<MinimapSketchData>(
        collector, view, from, to, 10u, 15u, backend,
        pipe::ExactSimdRangeRoute::require_continuation,
        fixed_highway_kernels());
    return {
        .records = canonical_records(collector),
        .proof = proof,
    };
}

void require_partial_source_read(
        const composite_source_state& source,
        sequence_size_t selected_length) {
    REQUIRE(source.callback_count > 0u);
    REQUIRE(source.requested_bp >= selected_length);
    REQUIRE(source.requested_bp < source.logical_size);
    REQUIRE(source.max_request <= 256u);
}

// Run the scalar minimizer kernel over an arbitrary sequence view and return
// the sorted-unique (last_pos, kmer) pair set.
template <class SeqView, class I>
[[nodiscard]] std::vector<std::pair<std::size_t, std::uint64_t>>
run_scalar(SeqView& seq, I begin, I end, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        collector, seq, begin, end, w, k);
    auto pairs = collector.extract_pairs();
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

template <std::size_t FragUnits>
void check_parity(std::size_t n, std::uint64_t seed, std::size_t w, std::size_t k) {
    using chunked = bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, FragUnits>;
    auto data = make_dna4(n, seed);
    range_t range(data);

    const auto baseline = run_scalar(range, range.begin(), range.end(), w, k);

    bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto fragmented = run_scalar(lazy, lazy.begin(), lazy.end(), w, k);

    if (baseline != fragmented) {
        bioseq_log<DBG>("[parity] n={} frag_units={} baseline={} fragmented={}",
                n, FragUnits, baseline.size(), fragmented.size());
    }
    REQUIRE(baseline.size() == fragmented.size());
    REQUIRE(baseline == fragmented);
}

}  // namespace

TEST_CASE("DOC0263 §11.2.1 — fragmented minimizer set equals all-in-RAM baseline (w=10,k=15)",
          "[lazy-fragmented][minimizer-parity]") {
    for (std::size_t n : {64u, 96u, 200u, 257u, 1024u, 4096u}) {
        check_parity<1>(n, 0x1111 + n, 10, 15);    // 32 letters / fragment (worst case)
        check_parity<2>(n, 0x2222 + n, 10, 15);    // 64 letters / fragment
        check_parity<32>(n, 0x3333 + n, 10, 15);   // 1024 letters / fragment (production)
    }
}

TEST_CASE("DOC0263 §11.2.1 — fragmented minimizer parity across w/k variations",
          "[lazy-fragmented][minimizer-parity]") {
    const std::array<std::pair<std::size_t, std::size_t>, 3> wk{{{5, 7}, {10, 15}, {12, 21}}};
    for (auto [w, k] : wk) {
        check_parity<1>(512, 0xAAAA + w * 131 + k, w, k);
        check_parity<32>(2000, 0xBBBB + w * 131 + k, w, k);
    }
}

TEST_CASE("DOC0377 — scalar minimizers stream only a sliced composite source range",
          "[lazy-view][slice][concat][reverse][source-backed][scalar][minimizer-parity]") {
    auto fixture = make_composite_fixture();
    using packed_t = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<packed_t> selected_range(fixture.selected_letters);
    auto selected_owner = bioseq::storage::make_lazy_in_memory(selected_range);
    const auto selected_view =
        bioseq::storage::as_lazy_sequence_view(selected_owner);
    const auto baseline = run_dispatch(
        selected_view, 0u, selected_view.bp_size(),
        MinimizerSketchBackend::Scalar);

    reset_source_observation(fixture.source);
    const auto source_view = make_composite_source_view(fixture.source);
    REQUIRE_FALSE(lazy_is_worker_shareable(source_view));
    REQUIRE_FALSE(source_view.try_resident_packed(
        fixture.selected_from, fixture.selected_to, sizeof(std::uint32_t)));
    const auto source = run_dispatch(
        source_view, fixture.selected_from, fixture.selected_to,
        MinimizerSketchBackend::Scalar);

    REQUIRE(source.records == baseline.records);
    REQUIRE(pipe::minimizer_range_proof_matches_backend(
        MinimizerSketchBackend::Scalar, source.proof,
        /*require_nonempty_vector_work=*/false));
    require_partial_source_read(
        fixture.source, fixture.selected_to - fixture.selected_from);
}

TEST_CASE("DOC0377 — ExactSimd and Paper SIMD admit only a sliced composite source range",
          "[lazy-view][slice][concat][reverse][source-backed][simd][minimizer-parity]") {
    auto fixture = make_composite_fixture();
    using packed_t = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<packed_t> selected_range(fixture.selected_letters);
    auto selected_owner = bioseq::storage::make_lazy_in_memory(selected_range);
    const auto selected_view =
        bioseq::storage::as_lazy_sequence_view(selected_owner);
    const auto scalar = run_dispatch(
        selected_view, 0u, selected_view.bp_size(),
        MinimizerSketchBackend::Scalar);
    const auto source_view = make_composite_source_view(fixture.source);
    const sequence_size_t selected_length =
        fixture.selected_to - fixture.selected_from;

    try {
        const auto exact_baseline = run_dispatch(
            selected_view, 0u, selected_view.bp_size(),
            MinimizerSketchBackend::ExactSimd);
        reset_source_observation(fixture.source);
        const auto exact_source = run_dispatch(
            source_view, fixture.selected_from, fixture.selected_to,
            MinimizerSketchBackend::ExactSimd);
        REQUIRE(exact_source.records == exact_baseline.records);
        REQUIRE(exact_source.records == scalar.records);
        REQUIRE(pipe::minimizer_range_proof_matches_backend(
            MinimizerSketchBackend::ExactSimd, exact_source.proof,
            /*require_nonempty_vector_work=*/true,
            pipe::ExactSimdRangeRoute::require_continuation));
        require_partial_source_read(fixture.source, selected_length);
    } catch (const pipe::ExactSimdRuntimeError& error) {
        REQUIRE(error.status()
                == TwoStackContinuationStatus::unsupported_vector_shape);
    }

    const std::size_t lanes = hn::Lanes(hn::ScalableTag<std::uint32_t>{});
    if (lanes != 4u && lanes != 8u) {
        reset_source_observation(fixture.source);
        REQUIRE_THROWS_AS(
            run_dispatch(
                source_view, fixture.selected_from, fixture.selected_to,
                MinimizerSketchBackend::PaperNtHashSimd),
            pipe::PaperNtHashRuntimeError);
        require_partial_source_read(fixture.source, selected_length);
        return;
    }

    const auto paper_baseline = run_dispatch(
        selected_view, 0u, selected_view.bp_size(),
        MinimizerSketchBackend::PaperNtHashSimd);
    reset_source_observation(fixture.source);
    const auto paper_source = run_dispatch(
        source_view, fixture.selected_from, fixture.selected_to,
        MinimizerSketchBackend::PaperNtHashSimd);
    REQUIRE(paper_source.records == paper_baseline.records);
    REQUIRE(pipe::minimizer_range_proof_matches_backend(
        MinimizerSketchBackend::PaperNtHashSimd, paper_source.proof,
        /*require_nonempty_vector_work=*/true));
    require_partial_source_read(fixture.source, selected_length);
}

TEST_CASE("DOC0377 — Vulkan staging dispatches a sliced composite source range only",
          "[lazy-view][slice][concat][reverse][source-backed][vulkan][minimizer-parity][memory-admission]") {
    auto fixture = make_composite_fixture();
    const auto source_view = make_composite_source_view(fixture.source);
    if (!vk::hardware_device_available(vk::Runtime::instance())) {
        auto& admission =
            bioseq::parallelism::MemoryAdmissionController::instance();
        const std::uint64_t baseline = admission.reserved_bytes();
        vk::VulkanSketchDecline decline{};
        reset_source_observation(fixture.source);
        const auto rejected =
            vk::build_gpu_segment_index_on_device<MinimapSketchData>(
                source_view,
                fixture.selected_from,
                fixture.selected_to,
                10u,
                15u,
                {},
                &decline);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(decline.reason
                == vk::VulkanSketchDeclineReason::hardware_device_unavailable);
        REQUIRE(decline.bp_size
                == fixture.selected_to - fixture.selected_from);
        REQUIRE(fixture.source.requested_bp
                == fixture.selected_to - fixture.selected_from);
        require_partial_source_read(
            fixture.source, fixture.selected_to - fixture.selected_from);
        REQUIRE(admission.reserved_bytes() == baseline);
        return;
    }

    using packed_t = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    compact_sequence_range<packed_t> selected_range(fixture.selected_letters);
    auto selected_owner = bioseq::storage::make_lazy_in_memory(selected_range);
    const auto selected_view =
        bioseq::storage::as_lazy_sequence_view(selected_owner);
    const auto scalar = run_dispatch(
        selected_view, 0u, selected_view.bp_size(),
        MinimizerSketchBackend::Scalar);

    reset_source_observation(fixture.source);
    sketch_collector gpu_collector;
    vk::collect_minimizers_vulkan_segment_or_throw<MinimapSketchData>(
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

TEST_CASE("DOC0377 — lazy batch sorting never requests whole composite sources",
          "[lazy-view][source-backed][poison][batch-sort][vulkan]") {
    auto high = make_composite_fixture();
    auto low = make_composite_fixture();
    auto middle = make_composite_fixture();
    high.source.sources[0][100u] = 3u;
    low.source.sources[0][100u] = 0u;
    middle.source.sources[0][100u] = 1u;

    const std::array<facade_t, 3> views{
        make_composite_source_view(high.source),
        make_composite_source_view(low.source),
        make_composite_source_view(middle.source),
    };
    const std::span<const facade_t> batch{views};
    using processor =
        bioseq::storage::lazy_compact_sequence_batch_processor<seq::dna4>;

    bioseq::storage::batch_sort_spec cpu{};
    cpu.vulkan_enabled = false;
    REQUIRE((processor::lazy_compact_sequence_batch_sort(batch, cpu)
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
    bioseq::storage::batch_sort_spec gpu{};
    gpu.vulkan_enabled = true;
    REQUIRE((processor::lazy_compact_sequence_batch_sort(batch, gpu)
             == std::vector<std::size_t>{1u, 2u, 0u}));
    REQUIRE(high.source.max_request < high.source.logical_size);
    REQUIRE(low.source.max_request < low.source.logical_size);
    REQUIRE(middle.source.max_request < middle.source.logical_size);
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
