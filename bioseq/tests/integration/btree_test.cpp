/**
 * @file btree_test.cpp
 * @brief Verifies btree and hash index behavior for Bioseq sequences.
 *
 * The cases cover equality, range ordering, disabled sequential scans, and
 * planner index-scan settings on heap relations.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "btree_sequences";

/** @brief Seeds the heap relation and its default sequence btree index. */
void seed_heap(connection_test_context& ctx) {
    create_seq_table(ctx,
                     seq_table_spec{
                         .name = std::string(k_table),
                         .typmod = {.alphabet = "dna4", .width = 64},
                         .seq_columns = 1,
                     },
                     {
                         seq_row{.pk = 1, .sequences = {"AAAA"}},
                         seq_row{.pk = 2, .sequences = {"ACGT"}},
                         seq_row{.pk = 3, .sequences = {"CCCC"}},
                         seq_row{.pk = 4, .sequences = {"GGGG"}},
                         seq_row{.pk = 5, .sequences = {"TTTTTT"}},
                     });
    exec_sqlf(ctx, "CREATE INDEX {0}_seq_idx ON {0} USING btree (seq0);", k_table);
}

} // namespace

TEST_CASE("btree index on bioseq.seq returns the expected row on equality",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_heap(ctx);

    REQUIRE(query_int_column(ctx,
                             fmt::format("SELECT pk FROM {} "
                                         "WHERE seq0 OPERATOR(bioseq.=) "
                                         "      'TTTTTT'::bioseq.seq('dna4', 64) "
                                         "ORDER BY pk;",
                                         k_table)) == std::vector<long long>{5});
}

TEST_CASE("btree-indexed range query returns rows in lexicographic order",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_heap(ctx);

    REQUIRE(query_int_column(ctx,
                             fmt::format("SELECT pk FROM {} "
                                         "WHERE seq0 OPERATOR(bioseq.>=) "
                                         "      'C'::bioseq.seq('dna4', 64) "
                                         "  AND seq0 OPERATOR(bioseq.<) "
                                         "      'T'::bioseq.seq('dna4', 64) "
                                         "ORDER BY seq0;",
                                         k_table)) == std::vector<long long>{3, 4});
}

TEST_CASE("btree index path is correct when enable_seqscan is forced off",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_heap(ctx);

    guc_scope sequence_scans_disabled(ctx, "enable_seqscan", "off");
    REQUIRE(query_int_column(ctx,
                             fmt::format("SELECT pk FROM {} "
                                         "WHERE seq0 OPERATOR(bioseq.=) "
                                         "      'AAAA'::bioseq.seq('dna4', 64);",
                                         k_table)) == std::vector<long long>{1});
}

TEST_CASE("hash opclass lookup agrees with bioseq sequence equality",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_heap(ctx);
    exec_sqlf(ctx, "DROP INDEX {0}_seq_idx;", k_table);
    exec_sqlf(ctx,
              "CREATE INDEX {0}_hash_idx ON {0} USING hash "
              "(seq0 bioseq.seq_hash_ops);",
              k_table);
    guc_scope sequence_scans_disabled(ctx, "enable_seqscan", "off");
    REQUIRE(query_int_column(ctx,
                             fmt::format("SELECT pk FROM {} "
                                         "WHERE seq0 OPERATOR(bioseq.=) "
                                         "      'ACGT'::bioseq.seq('dna4',64);",
                                         k_table)) == std::vector<long long>{2});
}
