// bioseq/tests/unit/indexing/boundary_parity_test.cpp
//
// Boundary parity for the per-fragment minimizer index. For
// several dna4 lengths (0, < k, k..w+k-1, exactly w+k, and a ~40000 bp
// multi-fragment sequence), over BOTH the scanner_none (all-in-RAM) and the
// chunked_span_sequence_scanner<…,1024> back-ends, the (kmer -> positions) SET
// produced by minimizers_per_fragment_index::build + assemble_segment must equal the
// whole-sequence collect_minimizers_view set. dna4 (no N) => EXACT set equality.
//
// NOTE: scalar(chunked) == SIMD(all-in-RAM) for the underlying minimizer KERNEL
// is already checked by two_stack_minimizers_test.cpp and lazy_minimizer_parity_test.cpp;
// this file does not repeat that coverage. It checks the index-assembly layer:
// fragment-core partition + backward-context clip reconstructs the whole-sequence
// minimizer set, identically for both scanners.
//
// Reaches collect_minimizers_view / minimap_sketch_data, so the body lives inside
// bioseq::simd::HWY_NAMESPACE (symbols reached UNQUALIFIED), with Highway
// included directly for its namespace macros.

#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
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

import bioseq.tests.support.indexing_storage_fakes;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using bioseq::storage::resident_sequence_scanner;
using bioseq::storage::chunked_span_sequence_scanner;
using bioseq::storage::lazy_sequence;
using bioseq::storage::make_resident_lazy_sequence;
using bioseq::indexing::test::fake_fragment_storage;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

constexpr std::size_t k_window_size = 10, k_kmer_size = 15;            // pinned w, k (== the builders)
constexpr std::size_t k_fragment_units = 1024;
constexpr std::size_t k_fragment_bp    = k_fragment_units * 32;   // 32768

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
    if (range.bp_size() < k_window_size + k_kmer_size) {
        return {};
    }
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        collector, range, range.begin(), range.end(), k_window_size, k_kmer_size);
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (const minimap_sketch_data& m : collector.extract_sketches_data())
        out[m.kmer()].push_back(m.pos_strand_encoded());
    for (auto& [kmer, v] : out) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    return out;
}

// (kmer -> sorted+unique positions) of an assembled index over the kmer universe.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
assembled_map(const bioseq::indexing::minimizer_hashmap_fragment_index& idx,
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

// Build + assemble over p_scanner, then compare to the whole-sequence baseline.
template <class p_scanner>
void check_segment_parity(std::size_t n, std::uint64_t seed) {
    auto data = make_dna4(n, seed);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag = (std::size_t)((total_bp + k_fragment_bp - 1) / k_fragment_bp);

    auto baseline = whole_sequence_map(range);
    // strip empty baseline buckets (none expected, but keep the SET canonical)
    for (auto it = baseline.begin(); it != baseline.end();)
        it = it->second.empty() ? baseline.erase(it) : std::next(it);

    fake_fragment_storage<> storage;
    bioseq::indexing::minimizer_assembly_scratch scratch;
    bioseq::indexing::minimizer_hashmap_fragment_index assembled;

    if constexpr (p_scanner::is_fragmented()) {
        lazy_sequence<dna4_u64, p_scanner> lazy{p_scanner::from_range(range)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        bioseq::indexing::minimizers_per_fragment_index<
            fake_fragment_storage<>>::build(storage, fragmented_view);
    } else {
        auto lazy = make_resident_lazy_sequence(range);
        const auto in_memory_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        bioseq::indexing::minimizers_per_fragment_index<
            fake_fragment_storage<>>::build(storage, in_memory_view);
    }
    assembled = bioseq::indexing::minimizers_per_fragment_index<fake_fragment_storage<>>::assemble_segment(
        storage, /*frag_lo*/ 0, /*frag_hi*/ nfrag, /*frag_bp*/ (bioseq::storage::sequence_size_t)k_fragment_bp,
        /*total_bp*/ total_bp, /*clip_lo*/ 0, /*clip_hi*/ total_bp, scratch);

    REQUIRE(assembled.target_length() == total_bp);
    const auto got = assembled_map(assembled, baseline);
    if (got != baseline)
        bioseq_log<logging_level::debug>(
                "[boundary-parity] n={} nfrag={} baseline_kmers={} got_kmers={}",
                n, nfrag, baseline.size(), got.size());
    REQUIRE(got == baseline);
}

}  // namespace

TEST_CASE("per-fragment assemble_segment parity across boundary lengths (scanner_none)",
          "[component:bioseq][layer:unit][feature:index]") {
    using none = resident_sequence_scanner<std::uint64_t>;
    // 0, < k, k..(w+k-1), exactly w+k, then a multi-fragment length.
    for (std::size_t n : {std::size_t{0}, std::size_t{8}, std::size_t{15}, std::size_t{20},
                          std::size_t{24}, std::size_t{25}, std::size_t{64}, std::size_t{40000}}) {
        DYNAMIC_SECTION("scanner_none n=" << n) {
            check_segment_parity<none>(n, 0xD00D00 + n);
        }
    }
}

TEST_CASE("per-fragment assemble_segment parity across boundary lengths (chunked-1024)",
          "[component:bioseq][layer:unit][feature:index]") {
    using chunked = chunked_span_sequence_scanner<std::uint64_t, k_fragment_units>;
    for (std::size_t n : {std::size_t{0}, std::size_t{8}, std::size_t{15}, std::size_t{20},
                          std::size_t{24}, std::size_t{25}, std::size_t{64}, std::size_t{40000}}) {
        DYNAMIC_SECTION("chunked-1024 n=" << n) {
            check_segment_parity<chunked>(n, 0xD00D00 + n);
        }
    }
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
