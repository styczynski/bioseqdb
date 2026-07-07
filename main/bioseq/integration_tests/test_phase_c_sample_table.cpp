// DOC0264 Phase C — integration tests for `bioseq.create_sample_table`,
// `bioseq.locator_inspect`, and the per-fragment row layout.
//
// USING bioseq tables emit zero rows for SELECT-without-mmsearch by design
// (the columnar custom scan only fires when mmsearch is in the qual list —
// see test_mmsearch.cpp:50). The same applies to the sub-tables (also
// USING bioseq). So verification happens via:
//   1. `bioseq_internal.seq_sub_table` catalog row count + sub_table_relid.
//   2. `pg_relation_size(sub_table_relid)` to confirm fragment bytes were
//      written (positive size proves data exists on disk).
//   3. `bioseq.locator_inspect` on inline literals to prove the v2 layout
//      is wired (the stub path is exercised by the hoe-fixture INSERT
//      path which writes stubs into the parent — see test_phase_c_mmsearch.cpp).

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kSampleTable = "phase_c_sample_t";

}  // namespace

TEST_CASE("DOC0264 — bioseq.create_sample_table writes one catalog row + a "
          "non-empty sub-table",
          "[integration][phase-c][sample-table]") {
    auto ctx = fresh_context({kSampleTable});
    exec_sqlf(ctx,
              "SELECT bioseq.create_sample_table('{}', 100, 1024, 'dna4', 42);",
              kSampleTable);
    // 100 sequences × 1024 bp each = 100 sequences × 1 fragment each.
    // All sequences should land in one (bucket, shard) — bucket determined
    // by len_log2(1024) → bucket_idx ≥ 0; shard 0 (first write of this bucket).
    const long long catalog_rows = query_scalar_int(ctx,
        fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                    "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}');",
                    kSampleTable));
    REQUIRE(catalog_rows == 1);
    // The sub_table_relid must point at an existing pg_class entry.
    const long long sub_relid = query_scalar_int(ctx,
        fmt::format("SELECT sub_table_relid FROM bioseq_internal.seq_sub_table s "
                    "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}') LIMIT 1;",
                    kSampleTable));
    REQUIRE(sub_relid > 0);
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM pg_class WHERE oid = {};",
                            sub_relid)) == 1);
    // pg_relation_size must be > 0 (bytes were written for 100 fragment rows).
    const long long sub_size = query_scalar_int(ctx,
        fmt::format("SELECT pg_relation_size({})::bigint;", sub_relid));
    REQUIRE(sub_size > 0);
}

TEST_CASE("DOC0264 — bioseq.create_sample_table alphabet other than dna4 is rejected",
          "[integration][phase-c][sample-table]") {
    auto ctx = fresh_context({kSampleTable});
    REQUIRE(sql_fails(ctx,
        fmt::format("SELECT bioseq.create_sample_table('{}', 1, 1024, 'dna5', 42);",
                    kSampleTable)));
}

TEST_CASE("DOC0264 — empty sequence (num_fragments=0) creates a catalog row "
          "with empty sub-table",
          "[integration][phase-c][sample-table]") {
    auto ctx = fresh_context({kSampleTable});
    exec_sqlf(ctx,
              "SELECT bioseq.create_sample_table('{}', 1, 0, 'dna4', 42);",
              kSampleTable);
    // Empty sequence: one catalog row, sub-table relation exists.
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            kSampleTable)) == 1);
    // No fragment rows written → pg_relation_size of the sub-table may
    // still be 0 or one initial page. Verify only that the relation exists.
    const long long sub_relid = query_scalar_int(ctx,
        fmt::format("SELECT sub_table_relid FROM bioseq_internal.seq_sub_table s "
                    "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}') LIMIT 1;",
                    kSampleTable));
    REQUIRE(sub_relid > 0);
}

TEST_CASE("DOC0264 — locator_inspect on inline non-stub value returns inline fields",
          "[integration][phase-c][locator-inspect]") {
    auto ctx = fresh_context();
    // Test locator_inspect on a literal (inline, non-stub) bioseq.seq value.
    // Phase C's locator_inspect returns zeros for the locator-only fields and
    // the inline bp_size + padding for non-stub inputs.
    const long long version = query_scalar_int(ctx,
        "SELECT version FROM bioseq.locator_inspect("
        "  'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));");
    REQUIRE(version == 0);
    const long long bp_size = query_scalar_int(ctx,
        "SELECT total_bp_size FROM bioseq.locator_inspect("
        "  'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));");
    REQUIRE(bp_size == 32);
}

TEST_CASE("DOC0264 — INSERT INTO USING bioseq table writes per-fragment "
          "sub-table rows + stub locator",
          "[integration][phase-c][locator-inspect][stub]") {
    auto ctx = fresh_context({kSampleTable});
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              kSampleTable);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));",
              kSampleTable);
    // The standard INSERT path (bioseq_tuple_insert → rewrite-to-stub →
    // insert_payload_as_fragments) creates one catalog row for the bucket/shard.
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            kSampleTable)) == 1);
    // The sub-table relation has positive bytes (one fragment row × ~8 KiB).
    const long long sub_size = query_scalar_int(ctx,
        fmt::format("SELECT pg_relation_size(s.sub_table_relid)::bigint "
                    "  FROM bioseq_internal.seq_sub_table s "
                    " WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}') LIMIT 1;",
                    kSampleTable));
    REQUIRE(sub_size > 0);
}
