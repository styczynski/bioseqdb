// backend/sequences/indexing/merge_runs_multicursor_test.cpp
//
// DOC0279 §16.1 — the external-sort spill/merge surface of FakeFragmentStorage
// (is_external_sort_spill_storage): >=3 spilled runs with interleaved kmers,
// k-way merged, must equal a single-pass sort+unique over all runs. No SIMD here.

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

namespace {

using bioseq::indexing::test::FakeFragmentStorage;
using pair_t = std::pair<std::uint64_t, std::uint64_t>;

// Reference k-way merge: concatenate every run, sort, dedup per kmer.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
reference_merge(const std::vector<std::vector<pair_t>>& runs) {
    std::vector<pair_t> all;
    for (const auto& r : runs) all.insert(all.end(), r.begin(), r.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (const auto& [kmer, enc] : all) out[kmer].push_back(enc);   // already ascending+unique
    return out;
}

}  // namespace

TEST_CASE("DOC0279 §10.4 — merge_runs over >=3 interleaved spilled runs equals sort+unique",
          "[indexing][merge-runs]") {
    // Three runs with interleaved/overlapping kmers and duplicate (kmer,encoded)
    // pairs across runs (the merge must dedup encoded positions per kmer).
    std::vector<std::vector<pair_t>> runs = {
        {{5, 200}, {5, 100}, {1, 10}, {9, 900}, {5, 100}},   // un-sorted, with a dup
        {{1, 12}, {1, 10}, {3, 30}, {9, 901}},
        {{7, 70}, {5, 150}, {3, 30}, {1, 10}, {11, 111}},
    };

    FakeFragmentStorage<> storage;
    for (auto& r : runs) {
        std::sort(r.begin(), r.end());   // the production builder spills SORTED runs
        storage.spill_sorted_run(std::span<const pair_t>(r));
    }

    // Collect the merge output; also assert kmers arrive in ascending order and
    // each cursor's positions are ascending+unique.
    std::map<std::uint64_t, std::vector<std::uint64_t>> got;
    std::uint64_t prev_kmer = 0;
    bool have_prev = false;
    storage.merge_runs([&](std::uint64_t kmer, auto&& positions) {
        if (have_prev) REQUIRE(kmer > prev_kmer);     // strictly ascending distinct kmers
        prev_kmer = kmer; have_prev = true;

        std::vector<std::uint64_t> v;
        positions.for_each_batch([&](std::span<const std::uint64_t> b) { v.insert(v.end(), b.begin(), b.end()); });
        REQUIRE(v.size() == positions.total_count());
        for (std::size_t i = 1; i < v.size(); ++i) REQUIRE(v[i - 1] < v[i]);   // ascending + unique
        got.emplace(kmer, std::move(v));
    });

    REQUIRE(got == reference_merge(runs));

    // release_runs drops the spill; a subsequent merge yields nothing.
    storage.release_runs();
    std::size_t calls = 0;
    storage.merge_runs([&](std::uint64_t, auto&&) { ++calls; });
    REQUIRE(calls == 0u);
}

TEST_CASE("DOC0279 §10.4 — spill_sorted_run returns ascending run ids",
          "[indexing][merge-runs]") {
    FakeFragmentStorage<> storage;
    std::vector<pair_t> a{{1, 1}}, b{{2, 2}}, c{{3, 3}};
    REQUIRE(storage.spill_sorted_run(std::span<const pair_t>(a)) == 0u);
    REQUIRE(storage.spill_sorted_run(std::span<const pair_t>(b)) == 1u);
    REQUIRE(storage.spill_sorted_run(std::span<const pair_t>(c)) == 2u);
}
