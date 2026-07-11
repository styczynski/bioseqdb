// backend/sequences/indexing/minimizer_sequence_index_test.cpp
//
// DOC0279 §16.1 — minimizers_per_sequence: the merged, globally-deduped,
// kmer-range-partitioned per-sequence index. build() spills sorted runs through
// the storage (run_buffer_bytes_ forced TINY so >=2 runs spill), then
// assemble_for_query() loads only the buckets touched by the query kmers.
//
// Reaches collect_minimizers_view / MinimapSketchData, so the body lives inside
// bioseq::simd::HWY_NAMESPACE (symbols reached UNQUALIFIED). bioseq::indexing::*
// types are named fully-qualified.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.gpu.vulkan.runtime;   // DOC0345 §16.3 — hardware Vulkan skip guard
import bioseq.gpu.vulkan.phase_c_abi;

#include "index_test_fakes.h"

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using bioseq::storage::sequence_scanner_test_chunked;
using bioseq::storage::lazy_compact_sequence;
using bioseq::indexing::test::FakeFragmentStorage;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
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

template<class Range>
[[nodiscard]] std::vector<MinimapSketchData>
whole_sequence_minimizers(Range& range, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        collector, range, range.begin(), range.end(), w, k);
    return collector.extract_sketches_data();
}

// DOC0345 §16.3 — generic deterministic range builder + whole-target query
// minimizers (pinned w=10,k=15 to match the persisted mpf/mps build).
using TestFragmentStorage = FakeFragmentStorage<>;

template <class Config>
[[nodiscard]] compact_sequence_range<Config>
make_deterministic_test_range(std::size_t n) {
    // DOC0351 — draw ranks from the NON-AMBIGUOUS letters only (ACGT, ranks
    // 0..3) so persisted-index coverage checks focus on Phase-C routing and
    // storage assembly rather than N-reset density. A letters_per_unit-multiple
    // length also avoids scalar/GPU disagreement over trailing padding slots.
    constexpr std::uint8_t kNonAmbiguous =
        compact_sequence_range<Config>::traits::alphabet_size < 4u
            ? static_cast<std::uint8_t>(compact_sequence_range<Config>::traits::alphabet_size)
            : 4u;
    // Round down to a whole number of storage units so the trailing unit has
    // ZERO padding — otherwise the scalar unit-iterator sub-range over the final
    // fragment processes the last unit's padding slots (decoded as A) as
    // spurious minimizers that the GPU (which honours tail_padding) omits,
    // breaking exact parity for dna5 (letters_per_unit=21 ∤ 8Mi).
    constexpr std::size_t kLettersPerUnit =
        compact_sequence_range<Config>::traits::letters_per_unit;
    n -= n % kLettersPerUnit;
    std::vector<typename Config::alphabet_type> letters(n);
    std::uint64_t s = 0x9e3779b97f4a7c15ULL;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        typename Config::alphabet_type c{};
        c.assign_rank(static_cast<std::uint8_t>((s >> 33) % kNonAmbiguous));
        letters[i] = c;
    }
    return compact_sequence_range<Config>(std::move(letters));
}

template <class Config>
[[nodiscard]] std::vector<MinimapSketchData>
make_deterministic_query_minimizers(compact_sequence_range<Config>& range) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        collector, range, range.begin(), range.end(), 10, 15);
    return collector.extract_sketches_data();
}

// (kmer -> sorted+unique encoded positions). Dedups to match the global dedup the
// minimizers_per_sequence merge performs.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
lookup_map(const bioseq::indexing::MinimizerHashmapFragmentIndex& idx,
           const std::vector<std::uint64_t>& kmers) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (std::uint64_t kmer : kmers) {
        if (out.count(kmer)) continue;
        auto span = idx.lookup(kmer);
        std::vector<std::uint64_t> v(span.begin(), span.end());
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        out.emplace(kmer, std::move(v));
    }
    return out;
}

// Whole-sequence baseline (kmer -> sorted+unique positions), restricted to a
// query kmer set, with the same max_occ global-count drop assemble_for_query uses.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
baseline_for_query(const std::vector<MinimapSketchData>& whole,
                   const std::vector<std::uint64_t>& query_kmers,
                   std::size_t max_occ) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> per_kmer;
    for (const auto& m : whole) per_kmer[m.kmer()].push_back(m.pos_strand_encoded());
    for (auto& [kmer, v] : per_kmer) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (std::uint64_t kmer : query_kmers) {
        if (out.count(kmer)) continue;
        auto it = per_kmer.find(kmer);
        std::vector<std::uint64_t> v = (it == per_kmer.end()) ? std::vector<std::uint64_t>{} : it->second;
        if (v.size() > max_occ) v.clear();   // over-frequent kmer dropped
        out.emplace(kmer, std::move(v));
    }
    return out;
}

void require_lookup_maps_equal(
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& actual,
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& expected) {
    INFO("actual_kmers=" << actual.size() << " expected_kmers=" << expected.size());
    REQUIRE(actual.size() == expected.size());
    auto actual_it = actual.begin();
    auto expected_it = expected.begin();
    for (; actual_it != actual.end() && expected_it != expected.end(); ++actual_it, ++expected_it) {
        INFO("actual_kmer=" << actual_it->first << " expected_kmer=" << expected_it->first
             << " actual_positions=" << actual_it->second.size()
             << " expected_positions=" << expected_it->second.size());
        REQUIRE(actual_it->first == expected_it->first);
        REQUIRE(actual_it->second == expected_it->second);
    }
}

void require_lookup_maps_cover_scalar(
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& actual,
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& expected) {
    INFO("actual_kmers=" << actual.size() << " expected_kmers=" << expected.size());
    REQUIRE(actual.size() == expected.size());
    auto actual_it = actual.begin();
    auto expected_it = expected.begin();
    for (; actual_it != actual.end() && expected_it != expected.end(); ++actual_it, ++expected_it) {
        INFO("actual_kmer=" << actual_it->first << " expected_kmer=" << expected_it->first
             << " actual_positions=" << actual_it->second.size()
             << " expected_positions=" << expected_it->second.size());
        REQUIRE(actual_it->first == expected_it->first);
        REQUIRE(std::includes(
            actual_it->second.begin(),
            actual_it->second.end(),
            expected_it->second.begin(),
            expected_it->second.end()));
    }
}

void require_lookup_maps_cover_scalar_with_tile_extras(
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& actual,
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& expected,
        std::uint64_t total_bp,
        std::uint64_t fragment_bp,
        std::uint32_t k,
        std::uint32_t window,
        std::uint32_t letters_per_unit) {
    (void)fragment_bp;
    INFO("actual_kmers=" << actual.size() << " expected_kmers=" << expected.size());
    REQUIRE(actual.size() == expected.size());
    std::uint64_t total_extras = 0;
    auto actual_it = actual.begin();
    auto expected_it = expected.begin();
    for (; actual_it != actual.end() && expected_it != expected.end(); ++actual_it, ++expected_it) {
        INFO("actual_kmer=" << actual_it->first << " expected_kmer=" << expected_it->first
             << " actual_positions=" << actual_it->second.size()
             << " expected_positions=" << expected_it->second.size());
        REQUIRE(actual_it->first == expected_it->first);
        REQUIRE(std::includes(
            actual_it->second.begin(),
            actual_it->second.end(),
            expected_it->second.begin(),
            expected_it->second.end()));

        std::vector<std::uint64_t> extras;
        std::set_difference(
            actual_it->second.begin(),
            actual_it->second.end(),
            expected_it->second.begin(),
            expected_it->second.end(),
            std::back_inserter(extras));
        total_extras += extras.size();
    }
    const auto whole_plan = bioseq::gpu::vulkan::phase_c_tile_plan(
        total_bp, k, window, letters_per_unit);
    const std::uint64_t radius =
        static_cast<std::uint64_t>(k) + static_cast<std::uint64_t>(window) + 1u;
    const std::uint64_t max_extras =
        static_cast<std::uint64_t>(whole_plan.tile_count) * 2u * radius;
    INFO("total_extras=" << total_extras << " max_extras=" << max_extras);
    REQUIRE(total_extras <= max_extras);
}

}  // namespace

TEST_CASE("DOC0279 §10 — assemble_for_query equals whole-sequence build restricted to query kmers",
          "[indexing][minimizer-sequence][assemble]") {
    constexpr std::size_t FragUnits = 1024;
    using chunked = sequence_scanner_test_chunked<std::uint64_t, FragUnits>;

    auto data = make_dna4(40000, 0xBEEF01);
    range_t range(data);
    const auto total_bp = range.bp_size();

    FakeFragmentStorage<> storage;
    FakeFragmentStorage<> exact_storage;
    storage.run_buffer_bytes_ = 4096;   // TINY -> forces multiple spilled runs
    exact_storage.run_buffer_bytes_ = 4096;
    {
        lazy_compact_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>::build(storage, lazy);
    }
    {
        lazy_compact_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>::build(
            exact_storage, lazy, MinimizerSketchBackend::ExactSimd);
    }
    REQUIRE(storage.runs_.empty());   // build() calls release_runs() at the end
    REQUIRE(exact_storage.runs_.empty());

    // Query = the whole sequence's own minimizers; max_occ wide-open.
    auto query = whole_sequence_minimizers(range, 10, 15);
    bioseq::indexing::MinimizerAssemblyScratch scratch;
    auto idx = bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>::assemble_for_query(
        storage, std::span<const MinimapSketchData>(query), total_bp, /*max_occ*/ INT32_MAX, scratch);
    bioseq::indexing::MinimizerAssemblyScratch exact_scratch;
    auto exact_idx = bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>::assemble_for_query(
        exact_storage,
        std::span<const MinimapSketchData>(query),
        total_bp,
        /*max_occ*/ INT32_MAX,
        exact_scratch);

    REQUIRE(idx.target_length() == total_bp);

    std::vector<std::uint64_t> kmers;
    for (const auto& m : query) kmers.push_back(m.kmer());

    require_lookup_maps_equal(
        lookup_map(idx, kmers),
        baseline_for_query(query, kmers, (std::size_t)INT32_MAX));
    require_lookup_maps_equal(
        lookup_map(exact_idx, kmers),
        lookup_map(idx, kmers));
}

TEST_CASE("ExactSimd MPS collection accepts fragmented DNA5 with N resets",
          "[indexing][minimizer-sequence][exact-simd][dna5]") {
    using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using dna5_range = compact_sequence_range<dna5_u64>;
    using chunked = sequence_scanner_test_chunked<std::uint64_t, 2>;
    using MpsIndex =
        bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>;

    std::vector<seq::dna5> data(257);
    constexpr std::array<std::uint8_t, 4> acgt_ranks{0, 1, 2, 4};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(acgt_ranks[(index * 3u + index / 7u) % 4u]);
    }
    for (const std::size_t position : {41u, 42u, 43u, 127u, 211u}) {
        data[position].assign_rank(3); // SeqAn dna5 N rank
    }
    dna5_range range(data);
    lazy_compact_sequence<dna5_u64, chunked> lazy{
        chunked::from_range(range)};
    FakeFragmentStorage<> storage;

    REQUIRE_NOTHROW(MpsIndex::build(
        storage, lazy, MinimizerSketchBackend::ExactSimd));
    REQUIRE(storage.runs_.empty());
}

TEST_CASE("DOC0279 §10.5 — assemble_for_query drops an over-frequent kmer under a tiny max_occ",
          "[indexing][minimizer-sequence][max-occ]") {
    constexpr std::size_t FragUnits = 1024;
    using chunked = sequence_scanner_test_chunked<std::uint64_t, FragUnits>;

    std::vector<seq::dna4> data;
    data.reserve(40000);
    const std::string motif = "ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGT";
    for (std::size_t i = 0; i < 40000; ++i) {
        seq::dna4 c;
        c.assign_char(motif[i % motif.size()]);
        data.push_back(c);
    }
    range_t range(data);
    const auto total_bp = range.bp_size();

    FakeFragmentStorage<> storage;
    storage.run_buffer_bytes_ = 4096;
    {
        lazy_compact_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>::build(storage, lazy);
    }

    auto query = whole_sequence_minimizers(range, 10, 15);

    // Find a kmer whose global DEDUPED count is > 1 (an over-frequent kmer).
    std::map<std::uint64_t, std::vector<std::uint64_t>> per_kmer;
    for (const auto& m : query) per_kmer[m.kmer()].push_back(m.pos_strand_encoded());
    std::uint64_t over_freq_kmer = 0;
    bool found = false;
    for (auto& [kmer, v] : per_kmer) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        if (v.size() > 1) { over_freq_kmer = kmer; found = true; break; }
    }
    REQUIRE(found);   // a 40 kbp random dna4 sequence has repeated minimizers

    bioseq::indexing::MinimizerAssemblyScratch scratch;
    auto idx = bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>::assemble_for_query(
        storage, std::span<const MinimapSketchData>(query), total_bp, /*max_occ*/ 1, scratch);

    // Under max_occ=1 the over-frequent kmer is dropped entirely.
    REQUIRE(idx.lookup(over_freq_kmer).empty());
}

// DOC0351 — persisted mpf + mps built with Vulkan must cover scalar lookups.
TEST_CASE("persisted minimizer indexes built with Vulkan cover scalar lookups",
          "[indexing][vulkan][minimizer]") {
    if (!bioseq::gpu::vulkan::hardware_device_available(
            bioseq::gpu::vulkan::Runtime::instance())) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    using backend_t = MinimizerSketchBackend;
    using config_t = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using cfg_range_t = compact_sequence_range<config_t>;
    using storage_unit = typename cfg_range_t::traits::storage_unit;
    using scanner_t =
        sequence_scanner_test_chunked<
            storage_unit,
            bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
    using lazy_t = lazy_compact_sequence<config_t, scanner_t>;
    constexpr std::size_t frag_bp =
        scanner_t::fragment_storage_units * cfg_range_t::traits::letters_per_unit;

    cfg_range_t target = make_deterministic_test_range<config_t>(8u << 20);
    lazy_t lazy_scalar{scanner_t::from_range(target)};
    lazy_t lazy_gpu{scanner_t::from_range(target)};

    TestFragmentStorage scalar_mpf;
    TestFragmentStorage gpu_mpf;
    TestFragmentStorage scalar_mps;
    TestFragmentStorage gpu_mps;

    bioseq::indexing::MinimizersPerFragmentIndex<TestFragmentStorage>::build(
        scalar_mpf, lazy_scalar, backend_t::Scalar);
    bioseq::indexing::MinimizersPerFragmentIndex<TestFragmentStorage>::build(
        gpu_mpf, lazy_gpu, backend_t::VulkanSegment);

    bioseq::indexing::MinimizerSequenceMergedIndex<TestFragmentStorage>::build(
        scalar_mps, lazy_scalar, backend_t::Scalar);
    bioseq::indexing::MinimizerSequenceMergedIndex<TestFragmentStorage>::build(
        gpu_mps, lazy_gpu, backend_t::VulkanSegment);

    bioseq::indexing::MinimizerAssemblyScratch scratch_scalar;
    bioseq::indexing::MinimizerAssemblyScratch scratch_gpu;
    bioseq::indexing::MinimizerAssemblyScratch mpf_scratch_scalar;
    bioseq::indexing::MinimizerAssemblyScratch mpf_scratch_gpu;
    std::vector<MinimapSketchData> query_minimizers =
        make_deterministic_query_minimizers(target);
    std::vector<std::uint64_t> kmers;
    for (const auto& m : query_minimizers) {
        kmers.push_back(m.kmer());
    }

    const auto total_bp = target.bp_size();
    const std::size_t nfrag =
        static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);

    auto scalar_mpf_index =
        bioseq::indexing::MinimizersPerFragmentIndex<
            TestFragmentStorage>::assemble_segment(
                scalar_mpf,
                0,
                nfrag,
                frag_bp,
                total_bp,
                0,
                total_bp,
                mpf_scratch_scalar);
    auto gpu_mpf_index =
        bioseq::indexing::MinimizersPerFragmentIndex<
            TestFragmentStorage>::assemble_segment(
                gpu_mpf,
                0,
                nfrag,
                frag_bp,
                total_bp,
                0,
                total_bp,
                mpf_scratch_gpu);
    require_lookup_maps_cover_scalar_with_tile_extras(
        lookup_map(gpu_mpf_index, kmers),
        lookup_map(scalar_mpf_index, kmers),
        total_bp,
        frag_bp,
        15,
        10,
        cfg_range_t::traits::letters_per_unit);

    auto scalar_index =
        bioseq::indexing::MinimizerSequenceMergedIndex<
            TestFragmentStorage>::assemble_for_query(
                scalar_mps,
                query_minimizers,
                total_bp,
                (std::numeric_limits<int>::max)(),
                scratch_scalar);
    auto gpu_index =
        bioseq::indexing::MinimizerSequenceMergedIndex<
            TestFragmentStorage>::assemble_for_query(
                gpu_mps,
                query_minimizers,
                total_bp,
                (std::numeric_limits<int>::max)(),
                scratch_gpu);

    require_lookup_maps_cover_scalar_with_tile_extras(
        lookup_map(gpu_index, kmers),
        lookup_map(scalar_index, kmers),
        total_bp,
        frag_bp,
        15,
        10,
        cfg_range_t::traits::letters_per_unit);

    bioseq::indexing::MinimizerAssemblyScratch scratch_scalar_max_occ;
    bioseq::indexing::MinimizerAssemblyScratch scratch_gpu_max_occ;
    auto scalar_max_occ_index =
        bioseq::indexing::MinimizerSequenceMergedIndex<
            TestFragmentStorage>::assemble_for_query(
                scalar_mps,
                query_minimizers,
                total_bp,
                1,
                scratch_scalar_max_occ);
    auto gpu_max_occ_index =
        bioseq::indexing::MinimizerSequenceMergedIndex<
            TestFragmentStorage>::assemble_for_query(
                gpu_mps,
                query_minimizers,
                total_bp,
                1,
                scratch_gpu_max_occ);
    const auto gpu_max_occ_lookup = lookup_map(gpu_max_occ_index, kmers);
    const auto scalar_max_occ_lookup = lookup_map(scalar_max_occ_index, kmers);
    require_lookup_maps_cover_scalar(gpu_max_occ_lookup, scalar_max_occ_lookup);
    REQUIRE(bioseq::indexing::detail::effective_max_occ_threshold(1, false) == 1u);
    REQUIRE(bioseq::indexing::detail::effective_max_occ_threshold(1, true) == 2u);
    std::size_t scalar_useful_kmers_at_threshold = 0;
    for (const auto& [kmer, scalar_positions] : scalar_max_occ_lookup) {
        if (scalar_positions.empty()) {
            continue;
        }
        ++scalar_useful_kmers_at_threshold;
        const auto gpu_it = gpu_max_occ_lookup.find(kmer);
        REQUIRE(gpu_it != gpu_max_occ_lookup.end());
        REQUIRE(std::includes(
            gpu_it->second.begin(),
            gpu_it->second.end(),
            scalar_positions.begin(),
            scalar_positions.end()));
    }
    REQUIRE(scalar_useful_kmers_at_threshold > 0);

    bioseq::indexing::MinimizerAssemblyScratch scratch_gpu_zero_occ;
    auto gpu_zero_occ_index =
        bioseq::indexing::MinimizerSequenceMergedIndex<
            TestFragmentStorage>::assemble_for_query(
                gpu_mps,
                query_minimizers,
                total_bp,
                0,
                scratch_gpu_zero_occ);
    const auto gpu_unlimited_lookup = lookup_map(gpu_index, kmers);
    const auto gpu_zero_occ_lookup = lookup_map(gpu_zero_occ_index, kmers);
    std::size_t gpu_nonzero_kmers_filtered = 0;
    for (const auto& [kmer, gpu_positions] : gpu_unlimited_lookup) {
        if (gpu_positions.empty()) {
            continue;
        }
        ++gpu_nonzero_kmers_filtered;
        const auto filtered_it = gpu_zero_occ_lookup.find(kmer);
        REQUIRE(filtered_it != gpu_zero_occ_lookup.end());
        REQUIRE(filtered_it->second.empty());
        break;
    }
    REQUIRE(gpu_nonzero_kmers_filtered > 0);
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
