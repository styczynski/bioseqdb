// backend/sequences/indexing/index_storage_concept_test.cpp
//
// DOC0279 §16.1 — the §6.3 chunked-storage concepts + the §7.3
// supported_indexes<...> registry. This test does NOT touch the SIMD minimizer
// kernels, so it needs no Highway namespace wrapper. It relies on
// index_test_fakes.h (whose four conformance static_asserts already prove the
// fakes model their concepts at header-include time).

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

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
import bioseq_parallelism.memory_admission;
import bioseq_indexing.registry;

#include "index_test_fakes.h"

namespace {

using bioseq::indexing::SequenceIndexType;
using bioseq::indexing::test::FakeFragmentStorage;
using bioseq::indexing::test::FakeSequenceStorage;
using bioseq::indexing::test::FakeTableStorage;

// Re-assert the §6.3 conformance here so the test file itself documents the
// requirement (they also fire inside index_test_fakes.h at include time).
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  FakeFragmentStorage<>, SequenceIndexType::CHUNKED_PER_FRAGMENT>);
static_assert(bioseq::indexing::is_external_sort_spill_storage<FakeFragmentStorage<>>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  FakeSequenceStorage, SequenceIndexType::INLINE_PER_SEQUENCE>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  FakeTableStorage, SequenceIndexType::CHUNKED_PER_TABLE>);

// The §7.3 registry over the fake storages, instantiated with the three shipped
// index templates (the §7.3 static_asserts inside supported_indexes additionally
// prove name/type/policy invariants at this instantiation site).
using reg = bioseq::indexing::supported_indexes<
    FakeFragmentStorage<>, FakeSequenceStorage, FakeTableStorage,
    bioseq::indexing::MinimizersPerFragmentIndex,
    bioseq::indexing::MinimizerSequenceMergedIndex,
    bioseq::indexing::SymbolCountsIndex>;

}  // namespace

TEST_CASE("DOC0279 §6.3 — fake storages model their chunked-storage concepts",
          "[indexing][storage-concept]") {
    // The static_asserts above are the real proof; this case just makes the
    // requirement visible to the test runner.
    SUCCEED("concept conformance holds at compile time");
}

TEST_CASE("DOC0279 §7.3 — supported_indexes registry lists all three indexes with the right layout",
          "[indexing][registry]") {
    constexpr auto types = reg::list_types();
    REQUIRE(types.size() == 3u);

    std::map<std::string_view, SequenceIndexType> by_name;
    for (auto& [name, layout] : types) by_name.emplace(name, layout);

    REQUIRE(by_name.count(bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME) == 1u);
    REQUIRE(by_name.count(bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME) == 1u);
    REQUIRE(by_name.count(bioseq::indexing::SYMBOL_COUNTS_INDEX_NAME) == 1u);

    REQUIRE(by_name.at(bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME) == SequenceIndexType::CHUNKED_PER_FRAGMENT);
    REQUIRE(by_name.at(bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME) == SequenceIndexType::CHUNKED_PER_FRAGMENT);
    REQUIRE(by_name.at(bioseq::indexing::SYMBOL_COUNTS_INDEX_NAME)           == SequenceIndexType::INLINE_PER_SEQUENCE);

    // layout_of resolves the same way the SQL surface routes by name.
    REQUIRE(reg::layout_of(bioseq::indexing::SYMBOL_COUNTS_INDEX_NAME) == SequenceIndexType::INLINE_PER_SEQUENCE);
    REQUIRE(reg::layout_of(bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME) == SequenceIndexType::CHUNKED_PER_FRAGMENT);
}

TEST_CASE("DOC0279 §7.3 — fresh-table defaults set the cheap on-insert indexes",
          "[indexing][registry]") {
    reg r{};
    r.apply_table_defaults();
    constexpr auto types = reg::list_types();
    constexpr auto allows = reg::allows_on_insert_flags();

    for (std::size_t i = 0; i < types.size(); ++i) {
        REQUIRE(r.enabled[i]);   // every index enabled on a fresh table
        const bool is_default_on_insert =
            (types[i].first == bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME
             || types[i].first == bioseq::indexing::SYMBOL_COUNTS_INDEX_NAME);
        if (is_default_on_insert) {
            REQUIRE(allows[i]);
            REQUIRE(r.policy[i] == bioseq::indexing::IndexUpdatePolicy::UPDATE_ON_INSERT);
        } else {
            REQUIRE(r.policy[i] == bioseq::indexing::IndexUpdatePolicy::UPDATE_MANUALLY);
        }
    }

    // The whole-sequence external-sort index is eligible but remains opt-in.
    REQUIRE(r.set_policy(bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME,
                         bioseq::indexing::IndexUpdatePolicy::UPDATE_ON_INSERT)
            == reg::SetPolicyStatus::Ok);
    // An unknown name is reported as such.
    REQUIRE(r.set_policy("does_not_exist", bioseq::indexing::IndexUpdatePolicy::UPDATE_MANUALLY)
            == reg::SetPolicyStatus::Unknown);
}

TEST_CASE("DOC0279 §6.3 — FakeSequenceStorage INLINE resize round-trips a valid plan",
          "[indexing][storage-concept]") {
    // NOTE: resize(MAX_INDEX_CHUNK_SIZE_BYTES + 1) is an index_error => abort()
    // (death/abort test, not wired here). We instead exercise the positive path:
    // a sub-MAX resize must plan exactly one chunk and round-trip its bytes.
    FakeSequenceStorage storage;
    REQUIRE(storage.number_of_chunks() == 0u);          // absent before any write
    REQUIRE(storage.chunk_byte_size() == 0u);

    constexpr std::size_t kBytes = 4 * sizeof(std::int64_t);
    storage.resize(kBytes);
    storage.write_to_each<std::int64_t>([&](std::size_t c, std::int64_t* buf, std::size_t cap) {
        REQUIRE(c == 0u);
        REQUIRE(cap == kBytes / sizeof(std::int64_t));
        for (std::size_t i = 0; i < cap; ++i) buf[i] = (std::int64_t)(100 + i);
        return c;   // exactly one inline chunk
    });

    REQUIRE(storage.number_of_chunks() == 1u);
    REQUIRE(storage.chunk_byte_size() == kBytes);
    const std::int64_t* read = storage.read_chunk<std::int64_t>();
    for (std::size_t i = 0; i < kBytes / sizeof(std::int64_t); ++i)
        REQUIRE(read[i] == (std::int64_t)(100 + i));
}
