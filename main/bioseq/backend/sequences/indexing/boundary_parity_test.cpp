// backend/sequences/indexing/boundary_parity_test.cpp
//
// DOC0279 §16.1 — boundary parity for the per-fragment minimizer index. For
// several dna4 lengths (0, < k, k..w+k-1, exactly w+k, and a ~40000 bp
// multi-fragment sequence), over BOTH the scanner_none (all-in-RAM) and the
// sequence_scanner_test_chunked<…,1024> back-ends, the (kmer -> positions) SET
// produced by MinimizersPerFragmentIndex::build + assemble_segment must equal the
// whole-sequence collect_minimizers_view set. dna4 (no N) => EXACT set equality.
//
// NOTE: scalar(chunked) == SIMD(all-in-RAM) for the underlying minimizer KERNEL
// is already proven by two_stacks_minimizers_test.cpp + lazy_minimizer_parity_test.cpp;
// this file does NOT re-prove that. It pins the index-assembly layer: that the
// fragment-core partition + backward-context clip reconstructs the whole-sequence
// minimizer set, identically for both scanners.
//
// Reaches collect_minimizers_view / MinimapSketchData, so the body lives inside
// bioseq::simd::HWY_NAMESPACE (symbols reached UNQUALIFIED). <hwy/highway.h> comes
// in via common_simd.h.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
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
import bioseq_indexing.registry;

#include "index_test_fakes.h"

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using bioseq::storage::sequence_scanner_none;
using bioseq::storage::sequence_scanner_test_chunked;
using bioseq::storage::lazy_compact_sequence;
using bioseq::storage::make_lazy_in_memory;
using bioseq::indexing::test::FakeFragmentStorage;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

constexpr std::size_t kW = 10, kK = 15;            // pinned w, k (== the builders)
constexpr std::size_t kFragUnits = 1024;
constexpr std::size_t kFragBp    = kFragUnits * 32;   // 32768

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

// Whole-sequence baseline (kmer -> sorted+unique encoded positions).
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
whole_sequence_map(range_t& range) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        collector, range, range.begin(), range.end(), kW, kK);
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (const MinimapSketchData& m : collector.extract_sketches_data())
        out[m.kmer()].push_back(m.pos_strand_encoded());
    for (auto& [kmer, v] : out) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    return out;
}

// (kmer -> sorted+unique positions) of an assembled index over the kmer universe.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
assembled_map(const bioseq::indexing::MinimizerHashmapFragmentIndex& idx,
              const std::map<std::uint64_t, std::vector<std::uint64_t>>& universe) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (const auto& [kmer, _] : universe) {
        auto span = idx.lookup(kmer);
        std::vector<std::uint64_t> v(span.begin(), span.end());
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        if (!v.empty()) out.emplace(kmer, std::move(v));   // drop empties so the SET compares cleanly
    }
    return out;
}

// Build + assemble over Scanner, then compare to the whole-sequence baseline.
template <class Scanner>
void check_segment_parity(std::size_t n, std::uint64_t seed) {
    auto data = make_dna4(n, seed);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag = (std::size_t)((total_bp + kFragBp - 1) / kFragBp);

    auto baseline = whole_sequence_map(range);
    // strip empty baseline buckets (none expected, but keep the SET canonical)
    for (auto it = baseline.begin(); it != baseline.end();)
        it = it->second.empty() ? baseline.erase(it) : std::next(it);

    FakeFragmentStorage<> storage;
    bioseq::indexing::MinimizerAssemblyScratch scratch;
    bioseq::indexing::MinimizerHashmapFragmentIndex assembled;

    if constexpr (Scanner::supports_fragments()) {
        lazy_compact_sequence<dna4_u64, Scanner> lazy{Scanner::from_range(range)};
        bioseq::indexing::MinimizersPerFragmentIndex<FakeFragmentStorage<>>::build(storage, lazy);
    } else {
        auto lazy = make_lazy_in_memory(range);
        bioseq::indexing::MinimizersPerFragmentIndex<FakeFragmentStorage<>>::build(storage, lazy);
    }
    assembled = bioseq::indexing::MinimizersPerFragmentIndex<FakeFragmentStorage<>>::assemble_segment(
        storage, /*frag_lo*/ 0, /*frag_hi*/ nfrag, /*frag_bp*/ (bioseq::storage::sequence_size_t)kFragBp,
        /*total_bp*/ total_bp, /*clip_lo*/ 0, /*clip_hi*/ total_bp, scratch);

    REQUIRE(assembled.target_length() == total_bp);
    const auto got = assembled_map(assembled, baseline);
    if (got != baseline)
        JustLog("[boundary-parity] n={} nfrag={} baseline_kmers={} got_kmers={}",
                n, nfrag, baseline.size(), got.size());
    REQUIRE(got == baseline);
}

}  // namespace

TEST_CASE("DOC0279 §16.1 — per-fragment assemble_segment parity across boundary lengths (scanner_none)",
          "[indexing][boundary-parity]") {
    using none = sequence_scanner_none<std::uint64_t>;
    // 0, < k, k..(w+k-1), exactly w+k, then a multi-fragment length.
    for (std::size_t n : {std::size_t{0}, std::size_t{8}, std::size_t{15}, std::size_t{20},
                          std::size_t{24}, std::size_t{25}, std::size_t{64}, std::size_t{40000}})
        check_segment_parity<none>(n, 0xD00D00 + n);
}

TEST_CASE("DOC0279 §16.1 — per-fragment assemble_segment parity across boundary lengths (chunked-1024)",
          "[indexing][boundary-parity]") {
    using chunked = sequence_scanner_test_chunked<std::uint64_t, kFragUnits>;
    for (std::size_t n : {std::size_t{0}, std::size_t{8}, std::size_t{15}, std::size_t{20},
                          std::size_t{24}, std::size_t{25}, std::size_t{64}, std::size_t{40000}})
        check_segment_parity<chunked>(n, 0xD00D00 + n);
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
