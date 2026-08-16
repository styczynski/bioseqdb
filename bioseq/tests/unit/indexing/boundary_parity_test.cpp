/**
 * @file boundary_parity_test.cpp
 * @brief Tests fragment-index parity at k-mer, window, and fragment boundaries.
 *
 * The tests compare resident and chunked scanners with whole-sequence
 * minimizer collection. They run inside `bioseq::simd::HWY_NAMESPACE` to call
 * the selected collectors directly.
 */

#include <hwy/highway.h>
#include <catch2/catch_test_macros.hpp>

import std;
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

/// Packed DNA4 alphabet used by parity fixtures.
using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
/// Resident DNA4 range used by parity fixtures.
using range_t = compact_sequence_range<dna4_u64>;

/// Window length shared with the fragment builder.
constexpr std::size_t k_window_size = 10;
/// K-mer length shared with the fragment builder.
constexpr std::size_t k_kmer_size = 15;
/// Scanner storage units per source fragment.
constexpr std::size_t k_fragment_units = 1024;
/// Logical base-pair span of one persisted fragment.
constexpr std::size_t k_fragment_bp = k_fragment_units * 32;

/// Generate deterministic DNA4 symbols from a seeded linear recurrence.
[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t symbol_count,
                                               std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::vector<seq::dna4> symbols(symbol_count);
    std::uint64_t state = seed;
    for (std::size_t index = 0; index < symbol_count; ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 symbol;
        symbol.assign_rank(static_cast<decltype(symbol.to_rank())>((state >> 33) & 0x3));
        symbols[index] = symbol;
    }
    return symbols;
}

/// Collect the whole-sequence hash-to-position oracle.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
whole_sequence_map(range_t& range) {
    if (range.bp_size() < k_window_size + k_kmer_size) {
        return {};
    }
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        collector, range, range.begin(), range.end(), k_window_size, k_kmer_size);
    std::map<std::uint64_t, std::vector<std::uint64_t>> output;
    for (const minimap_sketch_data& minimizer : collector.extract_sketches_data()) {
        output[minimizer.kmer()].push_back(minimizer.pos_strand_encoded());
    }
    for (auto& [kmer, positions] : output) {
        (void)kmer;
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    }
    return output;
}

/// Collect sorted positions from an assembled index over the oracle hashes.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
assembled_map(const bioseq::indexing::minimizer_hashmap_fragment_index& idx,
              const std::map<std::uint64_t, std::vector<std::uint64_t>>& universe) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> output;
    for (const auto& [kmer, expected_positions] : universe) {
        (void)expected_positions;
        auto span = idx.lookup(kmer);
        std::vector<std::uint64_t> positions(span.begin(), span.end());
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
        if (!positions.empty()) {
            output.emplace(kmer, std::move(positions));
        }
    }
    return output;
}

/// Build through one scanner type and compare with whole-sequence collection.
template <class p_scanner> void check_segment_parity(std::size_t symbol_count, std::uint64_t seed) {
    auto data = make_dna4(symbol_count, seed);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag =
        static_cast<std::size_t>((total_bp + k_fragment_bp - 1) / k_fragment_bp);

    auto baseline = whole_sequence_map(range);
    // Remove empty oracle buckets before comparing the maps.
    for (auto it = baseline.begin(); it != baseline.end();) {
        it = it->second.empty() ? baseline.erase(it) : std::next(it);
    }

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
    assembled =
        bioseq::indexing::minimizers_per_fragment_index<fake_fragment_storage<>>::assemble_segment(
            storage,
            0,
            nfrag,
            static_cast<bioseq::storage::sequence_size_t>(k_fragment_bp),
            total_bp,
            0,
            total_bp,
            scratch);

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
