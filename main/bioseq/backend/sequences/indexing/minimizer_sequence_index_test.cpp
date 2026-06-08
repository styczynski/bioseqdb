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
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

import testing;
import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_storage_scanner;
import bioseq_storage_lazy;
import bioseq_storage_size;
import bioseq_indexing.type;
import bioseq_indexing.storage;
import bioseq_indexing.traits;
import bioseq_indexing.minimizer_index;
import bioseq_indexing.minimizer_hashmap;
import bioseq_indexing.minimizer_sequence;
import bioseq_indexing.symbol_counts;
import bioseq_parallelism.memory_admission;
import bioseq_indexing.registry;

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

[[nodiscard]] std::vector<MinimapSketchData>
whole_sequence_minimizers(range_t& range, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        collector, range, range.begin(), range.end(), w, k);
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

}  // namespace

TEST_CASE("DOC0279 §10 — assemble_for_query equals whole-sequence build restricted to query kmers",
          "[indexing][minimizer-sequence][assemble]") {
    constexpr std::size_t FragUnits = 1024;
    using chunked = sequence_scanner_test_chunked<std::uint64_t, FragUnits>;

    auto data = make_dna4(40000, 0xBEEF01);
    range_t range(data);
    const auto total_bp = range.bp_size();

    FakeFragmentStorage<> storage;
    storage.run_buffer_bytes_ = 4096;   // TINY -> forces multiple spilled runs
    {
        lazy_compact_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>::build(storage, lazy);
    }
    REQUIRE(storage.runs_.empty());   // build() calls release_runs() at the end

    // Query = the whole sequence's own minimizers; max_occ wide-open.
    auto query = whole_sequence_minimizers(range, 10, 15);
    bioseq::indexing::MinimizerAssemblyScratch scratch;
    auto idx = bioseq::indexing::MinimizerSequenceMergedIndex<FakeFragmentStorage<>>::assemble_for_query(
        storage, std::span<const MinimapSketchData>(query), total_bp, /*max_occ*/ INT32_MAX, scratch);

    REQUIRE(idx.target_length() == total_bp);

    std::vector<std::uint64_t> kmers;
    for (const auto& m : query) kmers.push_back(m.kmer());

    require_lookup_maps_equal(
        lookup_map(idx, kmers),
        baseline_for_query(query, kmers, (std::size_t)INT32_MAX));
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

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
