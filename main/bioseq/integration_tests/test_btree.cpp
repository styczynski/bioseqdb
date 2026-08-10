// Integration scenarios:
//   * The default btree opclass for `bioseq.seq` is functional: a CREATE
//     INDEX succeeds, and selecting on equality yields the expected row.
//   * Forcing `enable_seqscan = off` does not break the index path. (The
//     planner is free to choose; we only pin "the query still returns the
//     right row".)
//   * Index-driven scans on a heap table (no `USING bioseq`) avoid the
//     custom-scan path entirely; the result set is therefore unaffected by
//     `bioseq.enable_bioseq_index_scan`.

#include <tests/support/postgres.hpp>
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_btree_t";

void seed_heap(connection_test_context& ctx) {
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kTable),
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
    exec_sqlf(ctx,
              "CREATE INDEX {0}_seq_idx ON {0} USING btree (seq0);",
              kTable);
}

}  // namespace

TEST_CASE("btree index on bioseq.seq returns the expected row on equality",
          "[btree][equality]") {
    auto ctx = fresh_context({kTable});
    seed_heap(ctx);

    REQUIRE(query_int_column(
                ctx, fmt::format(
                         "SELECT pk FROM {} "
                         "WHERE seq0 OPERATOR(bioseq.=) "
                         "      'TTTTTT'::bioseq.seq('dna4', 64) "
                         "ORDER BY pk;",
                         kTable)) == std::vector<long long>{5});
}

TEST_CASE("btree-indexed range query returns rows in lexicographic order",
          "[btree][range]") {
    auto ctx = fresh_context({kTable});
    seed_heap(ctx);

    REQUIRE(query_int_column(
                ctx, fmt::format(
                         "SELECT pk FROM {} "
                         "WHERE seq0 OPERATOR(bioseq.>=) "
                         "      'C'::bioseq.seq('dna4', 64) "
                         "  AND seq0 OPERATOR(bioseq.<) "
                         "      'T'::bioseq.seq('dna4', 64) "
                         "ORDER BY seq0;",
                         kTable)) ==
            std::vector<long long>{3, 4});
}

TEST_CASE("btree index path is correct when enable_seqscan is forced off",
          "[btree][force]") {
    auto ctx = fresh_context({kTable});
    seed_heap(ctx);

    guc_scope no_seq(ctx, "enable_seqscan", "off");
    REQUIRE(query_int_column(
                ctx, fmt::format(
                         "SELECT pk FROM {} "
                         "WHERE seq0 OPERATOR(bioseq.=) "
                         "      'AAAA'::bioseq.seq('dna4', 64);",
                         kTable)) == std::vector<long long>{1});
}

TEST_CASE("hash opclass lookup agrees with bioseq sequence equality",
          "[btree][hash][opclass]") {
    auto ctx = fresh_context({kTable});
    seed_heap(ctx);
    exec_sqlf(ctx, "DROP INDEX {0}_seq_idx;", kTable);
    exec_sqlf(ctx,
              "CREATE INDEX {0}_hash_idx ON {0} USING hash "
              "(seq0 bioseq.seq_hash_ops);",
              kTable);
    guc_scope no_seq(ctx, "enable_seqscan", "off");
    REQUIRE(query_int_column(
                ctx, fmt::format(
                         "SELECT pk FROM {} "
                         "WHERE seq0 OPERATOR(bioseq.=) "
                         "      'ACGT'::bioseq.seq('dna4',64);",
                         kTable)) == std::vector<long long>{2});
}
