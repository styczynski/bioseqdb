/**
 * @file storage_concept_test.cpp
 * @brief Checks test storage interfaces, index registration, and default policies.
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

using bioseq::indexing::sequence_index_layout;
using bioseq::indexing::test::fake_fragment_storage;
using bioseq::indexing::test::fake_sequence_storage;
using bioseq::indexing::test::fake_table_storage;

// Check that each test storage supports its required index layout.
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

// Register the three index types with test storage. Instantiation also
// checks their names, layouts, and update policies at compile time.
using reg = bioseq::indexing::supported_indexes<
    fake_fragment_storage<>, fake_sequence_storage, fake_table_storage,
    bioseq::indexing::minimizers_per_fragment_index,
    bioseq::indexing::direct_minimizer_bucket_index,
    bioseq::indexing::symbol_counts_index>;

}  // namespace

TEST_CASE("fake storages model their chunked-storage concepts",
          "[component:bioseq][layer:unit][feature:storage]") {
    // Report the compile-time checks above in the test runner.
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

    // Resolve index layouts by the names used in SQL.
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
        REQUIRE(r.enabled[i]);   // All index types are enabled by default.
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

    // Updating the sequence index on insert must be enabled explicitly.
    REQUIRE(r.set_policy(bioseq::indexing::k_minimizers_per_sequence_index_name,
                         bioseq::indexing::index_update_policy::update_on_insert)
            == reg::set_policy_status::applied);
    
    REQUIRE(r.set_policy("does_not_exist", bioseq::indexing::index_update_policy::manual)
            == reg::set_policy_status::unknown_index);
}

TEST_CASE("fake_sequence_storage INLINE resize round-trips a valid plan",
          "[component:bioseq][layer:unit][feature:storage]") {
    // A valid inline allocation should use one chunk and preserve its bytes.
    fake_sequence_storage storage;
    REQUIRE(storage.number_of_chunks() == 0u);          // No chunk exists before the first allocation.
    REQUIRE(storage.chunk_byte_size() == 0u);

    constexpr std::size_t k_storage_bytes = 4 * sizeof(std::int64_t);
    storage.resize(k_storage_bytes);
    storage.write_to_each<std::int64_t>([](std::size_t c, std::int64_t* buf, std::size_t cap) {
        REQUIRE(c == 0u);
        REQUIRE(cap == k_storage_bytes / sizeof(std::int64_t));
        for (std::size_t i = 0; i < cap; ++i) {
            buf[i] = static_cast<std::int64_t>(100u + i);
        }
        return c;   
    });

    REQUIRE(storage.number_of_chunks() == 1u);
    REQUIRE(storage.chunk_byte_size() == k_storage_bytes);
    const std::int64_t* read = storage.read_chunk<std::int64_t>();
    for (std::size_t i = 0; i < k_storage_bytes / sizeof(std::int64_t); ++i)
        REQUIRE(read[i] == static_cast<std::int64_t>(100u + i));
}
