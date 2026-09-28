/**
 * @file merge_runs_multicursor_test.cpp
 * @brief Checks tlat merging fake spill runs sorts hashes and removes repeated positions.
 */

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

namespace {

using bioseq::indexing::test::fake_fragment_storage;
/// A minimizer hash paired with its encoded position
using pair_t = std::pair<std::uint64_t, std::uint64_t>;

/// Builds a reference map by sorting all pairs and removing duplicates.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
reference_merge(const std::vector<std::vector<pair_t>>& runs) {
    std::vector<pair_t> pairs;
    for (const auto& run : runs) {
        pairs.insert(pairs.end(), run.begin(), run.end());
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    std::map<std::uint64_t, std::vector<std::uint64_t>> output;
    for (const auto& [kmer, encoded_position] : pairs) {
        output[kmer].push_back(encoded_position);
    }
    return output;
}

}  // namespace

TEST_CASE("merge_runs over >=3 interleaved spilled runs equals sort+unique",
          "[component:bioseq][layer:unit][feature:storage]") {
    // Hashes and duplicate positions are spread across three runs.
    // The merge must collect each hash's distinct positions together.
    std::vector<std::vector<pair_t>> runs = {
        {{5, 200}, {5, 100}, {1, 10}, {9, 900}, {5, 100}},   // Unsorted input with a repeated pair.
        {{1, 12}, {1, 10}, {3, 30}, {9, 901}},
        {{7, 70}, {5, 150}, {3, 30}, {1, 10}, {11, 111}},
    };

    fake_fragment_storage<> storage;
    for (auto& r : runs) {
        std::sort(r.begin(), r.end());   // Spill input must already be  sorted.
        storage.spill_sorted_run(std::span<const pair_t>(r));
    }

    // Collect the result while checking hash order and sorted, distinct positions.
    std::map<std::uint64_t, std::vector<std::uint64_t>> got;
    std::uint64_t prev_kmer = 0;
    bool have_prev = false;
    storage.merge_runs([&got, &prev_kmer, &have_prev](std::uint64_t kmer, auto&& positions) {
        if (have_prev)
            REQUIRE(kmer > prev_kmer);
        prev_kmer = kmer;
        have_prev = true;

        std::vector<std::uint64_t> v;
        positions.for_each_batch([&v](std::span<const std::uint64_t> batch) {
            v.insert(v.end(), batch.begin(), batch.end());
        });
        REQUIRE(v.size() == positions.total_count());
        for (std::size_t i = 1; i < v.size(); ++i)
            REQUIRE(v[i - 1] < v[i]);
        got.emplace(kmer, std::move(v));
    });

    REQUIRE(got == reference_merge(runs));

    // Clearing the spill runs leaves nothing for the next merge to emit.
    storage.release_runs();
    std::size_t calls = 0;
    storage.merge_runs([&calls](std::uint64_t, auto&&) { ++calls; });
    REQUIRE(calls == 0u);
}

TEST_CASE("spill_sorted_run returns ascending run ids",
          "[component:bioseq][layer:unit][feature:storage]") {
    fake_fragment_storage<> storage;
    std::vector<pair_t> a{{1, 1}}, b{{2, 2}}, c{{3, 3}};
    REQUIRE(storage.spill_sorted_run(std::span<const pair_t>(a)) == 0u);
    REQUIRE(storage.spill_sorted_run(std::span<const pair_t>(b)) == 1u);
    REQUIRE(storage.spill_sorted_run(std::span<const pair_t>(c)) == 2u);
}
