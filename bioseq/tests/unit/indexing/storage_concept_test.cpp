// bioseq/tests/unit/indexing/storage_concept_test.cpp
//
// Chunked-storage concepts and the supported_indexes<...> registry. This test
// does NOT touch the SIMD minimizer
// kernels, so it needs no Highway namespace wrapper. It relies on
// bioseq.tests.support.indexing_storage_fakes, whose four conformance
// static_asserts already demonstrate that the fakes model their concepts.

#include <tests/support/common.hpp>

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

namespace {

using bioseq::indexing::sequence_index_layout;
using bioseq::indexing::test::fake_fragment_storage;
using bioseq::indexing::test::fake_sequence_storage;
using bioseq::indexing::test::fake_table_storage;

// Re-assert the  conformance here so the test file itself documents the
// requirement (they also fire when the support module is imported).
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  fake_fragment_storage<>, sequence_index_layout::fragment_chunks>);
static_assert(bioseq::indexing::is_external_sort_spill_storage<fake_fragment_storage<>>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  fake_sequence_storage, sequence_index_layout::sequence_inline>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  fake_table_storage, sequence_index_layout::table_chunks>);
static_assert(bioseq::indexing::index_read_requirement_ordinal_valid(0));
static_assert(bioseq::indexing::index_read_requirement_ordinal_valid(1));
static_assert(bioseq::indexing::index_read_requirement_ordinal_valid(2));
static_assert(!bioseq::indexing::index_read_requirement_ordinal_valid(-1));
static_assert(!bioseq::indexing::index_read_requirement_ordinal_valid(3));

// The  registry over the fake storages, instantiated with the three shipped
// index templates (the  static_asserts inside supported_indexes additionally
// check name, type, and policy invariants at this instantiation site).
using reg = bioseq::indexing::supported_indexes<
    fake_fragment_storage<>, fake_sequence_storage, fake_table_storage,
    bioseq::indexing::minimizers_per_fragment_index,
    bioseq::indexing::direct_minimizer_bucket_index,
    bioseq::indexing::symbol_counts_index>;

}  // namespace

TEST_CASE("fake storages model their chunked-storage concepts",
          "[component:bioseq][layer:unit][feature:storage]") {
    // The static_asserts above are the actual checks; this case just makes the
    // requirement visible to the test runner.
    SUCCEED("concept conformance holds at compile time");
}

TEST_CASE("index read requirement ordinals are exact and closed",
          "[component:bioseq][layer:unit][feature:index]") {
    using bioseq::indexing::index_read_requirement;
    REQUIRE(static_cast<std::int32_t>(index_read_requirement::allow) == 0);
    REQUIRE(static_cast<std::int32_t>(index_read_requirement::require_absent) == 1);
    REQUIRE(static_cast<std::int32_t>(
                index_read_requirement::require_compatible_persisted) == 2);
}

TEST_CASE("supported_indexes registry lists all three indexes with the right layout",
          "[component:bioseq][layer:unit][feature:configuration]") {
    constexpr auto types = reg::list_types();
    REQUIRE(types.size() == 3u);

    std::map<std::string_view, sequence_index_layout> by_name;
    for (auto& [name, layout] : types) by_name.emplace(name, layout);

    REQUIRE(by_name.count(bioseq::indexing::k_minimizers_per_fragment_index_name) == 1u);
    REQUIRE(by_name.count(bioseq::indexing::k_minimizers_per_sequence_index_name) == 1u);
    REQUIRE(by_name.count(bioseq::indexing::k_symbol_counts_index_name) == 1u);

    REQUIRE(by_name.at(bioseq::indexing::k_minimizers_per_fragment_index_name) == sequence_index_layout::fragment_chunks);
    REQUIRE(by_name.at(bioseq::indexing::k_minimizers_per_sequence_index_name) == sequence_index_layout::fragment_chunks);
    REQUIRE(by_name.at(bioseq::indexing::k_symbol_counts_index_name)           == sequence_index_layout::sequence_inline);

    // layout_of resolves the same way the SQL surface routes by name.
    REQUIRE(reg::layout_of(bioseq::indexing::k_symbol_counts_index_name) == sequence_index_layout::sequence_inline);
    REQUIRE(reg::layout_of(bioseq::indexing::k_minimizers_per_fragment_index_name) == sequence_index_layout::fragment_chunks);
}

TEST_CASE("fresh-table defaults set the cheap on-insert indexes",
          "[component:bioseq][layer:unit][feature:configuration]") {
    reg r{};
    r.apply_table_defaults();
    constexpr auto types = reg::list_types();
    constexpr auto allows = reg::allows_on_insert_flags();

    for (std::size_t i = 0; i < types.size(); ++i) {
        REQUIRE(r.enabled[i]);   // every index enabled on a fresh table
        const bool is_default_on_insert =
            (types[i].first == bioseq::indexing::k_minimizers_per_fragment_index_name
             || types[i].first == bioseq::indexing::k_symbol_counts_index_name);
        if (is_default_on_insert) {
            REQUIRE(allows[i]);
            REQUIRE(r.policy[i] == bioseq::indexing::index_update_policy::update_on_insert);
        } else {
            REQUIRE(r.policy[i] == bioseq::indexing::index_update_policy::manual);
        }
    }

    // The whole-sequence external-sort index is eligible but remains opt-in.
    REQUIRE(r.set_policy(bioseq::indexing::k_minimizers_per_sequence_index_name,
                         bioseq::indexing::index_update_policy::update_on_insert)
            == reg::set_policy_status::applied);
    // An unknown name is reported as such.
    REQUIRE(r.set_policy("does_not_exist", bioseq::indexing::index_update_policy::manual)
            == reg::set_policy_status::unknown_index);
}

TEST_CASE("fake_sequence_storage INLINE resize round-trips a valid plan",
          "[component:bioseq][layer:unit][feature:storage]") {
    // NOTE: resize(k_max_index_chunk_size_bytes + 1) is an index_error => abort()
    // (death/abort test, not wired here). We instead exercise the positive path:
    // a sub-MAX resize must plan exactly one chunk and round-trip its bytes.
    fake_sequence_storage storage;
    REQUIRE(storage.number_of_chunks() == 0u);          // absent before any write
    REQUIRE(storage.chunk_byte_size() == 0u);

    constexpr std::size_t k_storage_bytes = 4 * sizeof(std::int64_t);
    storage.resize(k_storage_bytes);
    storage.write_to_each<std::int64_t>([&](std::size_t c, std::int64_t* buf, std::size_t cap) {
        REQUIRE(c == 0u);
        REQUIRE(cap == k_storage_bytes / sizeof(std::int64_t));
        for (std::size_t i = 0; i < cap; ++i) buf[i] = (std::int64_t)(100 + i);
        return c;   // exactly one inline chunk
    });

    REQUIRE(storage.number_of_chunks() == 1u);
    REQUIRE(storage.chunk_byte_size() == k_storage_bytes);
    const std::int64_t* read = storage.read_chunk<std::int64_t>();
    for (std::size_t i = 0; i < k_storage_bytes / sizeof(std::int64_t); ++i)
        REQUIRE(read[i] == (std::int64_t)(100 + i));
}
