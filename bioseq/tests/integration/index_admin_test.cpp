// Integration scenarios for the metadata-admin SQL surface
// (bioseq.list_metadata + bioseq.update_index + bioseq.set_index_policy):
//   * list_metadata enumerates the three production index kinds
//     (minimizers_per_fragment, symbol_counts, minimizers_per_sequence) for a
//     freshly-created USING bioseq table, NOT gated on data: every index
//     appears with storage_size='0 bytes' and a NULL time_since_update,
//     carrying its registered (index_type, update_policy).
//   * update_index('minimizers_per_fragment') (re)builds the per-fragment
//     chunk index for every bioseq.seq column of every row and returns the
//     count of sequences (re)indexed.
//   * update_index rejects an unknown index name, and rejects non-bioseq-AM
//     (heap) tables.
//   * update_index accepts the table argument as quoted text or as an
//     explicit ::regclass cast.
//   * set_index_policy toggles a kind's update_policy (visible through
//     list_metadata), including opt-in UPDATE_ON_INSERT for the heavier
//     minimizers_per_sequence kind.
//   * update_index is idempotent at the content level (stable chunk row
//     count across repeated calls) and returns 0 on an empty table while
//     list_metadata still lists the kinds.
//   * row-count assertions use bioseq.index_table_row_count() through the
//     integration-test index_table_row_count() wrapper.

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>
#include <algorithm>
#include <chrono>
#include <libpq-fe.h>
#include <thread>
#include <utility>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "index_admin_t";

#ifdef BIOSEQ_TEST_INDEXES
[[nodiscard]] std::int64_t facade_source_counter(connection_test_context &ctx,
                                                 std::string_view name) {
  return query_scalar_int(
      ctx, fmt::format("SELECT (bioseq_internal.sequence_source_counters()"
                       "        ->>'{}')::bigint;",
                       name));
}
#endif

[[nodiscard]] long long only_column_row_number(
        const connection_test_context& ctx, std::string_view table) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) FROM {}",
        table, table));
}

[[nodiscard]] long long require_ready_hash_major_generation(
        const connection_test_context& ctx,
        std::string_view table,
        std::string_view index_name,
        long long sequence_rowno,
        long long target_bp,
        long long expected_scheme_identity,
        long long expected_scheme_revision,
        long long expected_producer_identity) {
    const row_t inspected = query_one(ctx, fmt::format(
        "WITH inspected AS ("
        " SELECT bioseq.inspect_ready_hash_major_minimizer_generation("
        "  '{}'::regclass, '{}', {}) AS j"
        ") "
        "SELECT "
        " j->>'layout',"
        " j->'head'->>'generation_id',"
        " j->'source'->>'sequence_rowno',"
        " j->'source'->>'total_bp_size',"
        " j->'manifest'->'compatibility'->>'hash_scheme_identity',"
        " j->'manifest'->'compatibility'->>'hash_scheme_revision',"
        " j->'manifest'->'compatibility'->>'producer_backend_identity',"
        " j->'manifest'->'compatibility'->>'producer_backend_revision',"
        " CASE WHEN "
        "  j->>'schema'='bioseq.hash_major_minimizer_generation_inspection' "
        "  AND j->>'revision'='1' "
        "  AND (j->>'database_oid')::bigint>0 "
        "  AND (j->>'main_relfilenode')::bigint>0 "
        "  AND (j->>'column_attno')::bigint>0 "
        "  AND j->>'index_name'='{}' "
        "  AND j->>'query_key'=j->'source'->>'sequence_rowno' "
        "  AND (j->'source'->>'locator_revision')::bigint=3 "
        "  AND (j->'source'->>'num_fragments')::bigint>0 "
        "  AND (j->'source'->>'source_epoch')::bigint>0 "
        "  AND (j->'source'->>'alphabet_code')::bigint=0 "
        "  AND (j->'source'->>'storage_unit_byte_width')::bigint=8 "
        "  AND j->'head'->>'query_key'=j->>'query_key' "
        "  AND j->'head'->>'source_epoch'="
        "      j->'source'->>'source_epoch' "
        "  AND j->'manifest'->>'source_epoch'="
        "      j->'source'->>'source_epoch' "
        "  AND (j->'head'->>'generation_id')::bigint>0 "
        "  AND j->'head'->>'generation_id'="
        "      j->'manifest'->>'generation_id' "
        "  AND j->'source'->>'source_identity'="
        "      j->'head'->>'source_identity' "
        "  AND j->'head'->>'source_identity'="
        "      j->'manifest'->>'source_identity' "
        "  AND length(j->'source'->>'source_identity')=32 "
        "  AND j->'source'->>'source_identity'~'^[0-9a-f]+$' "
        "  AND j->'source'->>'source_content_crc64'="
        "      j->'head'->>'source_content_crc64' "
        "  AND j->'head'->>'source_content_crc64'="
        "      j->'manifest'->>'source_content_crc64' "
        "  AND length(j->'source'->>'source_content_crc64')=16 "
        "  AND j->'source'->>'source_content_crc64'~'^[0-9a-f]+$' "
        "  AND j->'head'->>'compatibility_digest'="
        "      j->'manifest'->>'compatibility_digest' "
        "  AND length(j->'head'->>'compatibility_digest')=64 "
        "  AND j->'head'->>'compatibility_digest'~'^[0-9a-f]+$' "
        "  AND length(j->'head'->>'manifest_address_bound_row_crc64')=16 "
        "  AND length(j->'head'->>'manifest_payload_crc64')=16 "
        "  AND length(j->'head'->>'head_crc64')=16 "
        "  AND length(j->'manifest'->>'authenticated_directory_crc64')=16 "
        "  AND length(j->'manifest'->>'generation_crc64')=16 "
        "  AND (j->'head'->>'dense_seal_present')::boolean=false "
        "  AND j->'manifest'->>'target_length'="
        "      j->'source'->>'total_bp_size' "
        "  AND (j->'manifest'->>'unique_hash_count')::bigint>0 "
        "  AND (j->'manifest'->>'total_posting_count')::bigint>0 "
        "  AND (j->'manifest'->>'total_control_bytes')::bigint>0 "
        "  AND (j->'manifest'->>'total_posting_bytes')::bigint>0 "
        "  AND (j->'manifest'->>'directory_entry_count')::bigint>0 "
        "  AND (j->'manifest'->'compatibility'->>'alphabet_code')::bigint=0 "
        "  AND (j->'manifest'->'compatibility'->>'kmer_size')::bigint=15 "
        "  AND (j->'manifest'->'compatibility'->>'window_size')::bigint=10 "
        "  AND (j->'manifest'->'compatibility'->>'hash_width_bytes')::bigint=4 "
        "  AND (j->'manifest'->'compatibility'->>"
        "       'stored_posting_width_bytes')::bigint=4 "
        "  AND (j->'manifest'->'compatibility'->>"
        "       'position_encoding_revision')::bigint=1 "
        "  AND (j->'manifest'->'compatibility'->>"
        "       'hash_semantics_revision')::bigint=1 "
        "  AND (j->'manifest'->'compatibility'->>"
        "       'count_semantics_revision')::bigint=1 "
        "  AND (j->'manifest'->'compatibility'->>"
        "       'producer_backend_revision')::bigint=1 "
        "  AND (j->'manifest'->'compatibility'->>"
        "       'controls_per_block')::bigint=4096 "
        "  AND (j->'manifest'->'compatibility'->>'hash_seed')::bigint=0 "
        "  AND (j->'metadata'->>'head_rows')::bigint=1 "
        "  AND (j->'metadata'->>'head_bytes')::bigint=224 "
        "  AND (j->'metadata'->>'manifest_rows')::bigint=1 "
        "  AND (j->'metadata'->>'manifest_bytes')::bigint=336 "
        "  AND jsonb_array_length(j->'streams')=3 "
        "  AND j->'streams'->0->>'kind'='directory' "
        "  AND j->'streams'->1->>'kind'='controls' "
        "  AND j->'streams'->2->>'kind'='postings' "
        "  AND NOT EXISTS ("
        "   SELECT 1 FROM jsonb_array_elements(j->'streams') AS s(value) "
        "   WHERE (s.value->>'element_count')::bigint<=0 "
        "      OR (s.value->>'logical_bytes')::bigint<=0 "
        "      OR (s.value->>'row_count')::bigint"
        "       <>((s.value->>'logical_bytes')::bigint+8063)/8064 "
        "      OR (s.value->>'row_count')::bigint"
        "       <>(s.value->>'exact_catalog_rows')::bigint "
        "      OR (s.value->>'logical_bytes')::bigint"
        "       <>(s.value->>'exact_catalog_bytes')::bigint "
        "      OR s.value->>'stream_crc64'"
        "       <>s.value->>'exact_catalog_stream_crc64' "
        "      OR length(s.value->>'stream_crc64')<>16 "
        "      OR s.value->>'stream_crc64'!~'^[0-9a-f]+$'"
        "  ) "
        "  AND (j->'streams'->0->>'logical_bytes')::bigint="
        "      (j->'streams'->0->>'element_count')::bigint*80 "
        "  AND j->'streams'->0->>'element_count'="
        "      j->'manifest'->>'directory_entry_count' "
        "  AND j->'streams'->0->>'stream_crc64'="
        "      j->'manifest'->>'authenticated_directory_crc64' "
        "  AND j->'streams'->1->>'element_count'="
        "      j->'manifest'->>'unique_hash_count' "
        "  AND j->'streams'->1->>'logical_bytes'="
        "      j->'manifest'->>'total_control_bytes' "
        "  AND j->'streams'->2->>'element_count'="
        "      j->'manifest'->>'total_posting_count' "
        "  AND j->'streams'->2->>'logical_bytes'="
        "      j->'manifest'->>'total_posting_bytes' "
        "  AND (j->'streams'->2->>'logical_bytes')::bigint="
        "      (j->'streams'->2->>'element_count')::bigint*"
        "      (j->'manifest'->'compatibility'->>"
        "       'stored_posting_width_bytes')::bigint "
        " THEN '1' ELSE '0' END "
        "FROM inspected;",
        table, index_name, sequence_rowno, index_name));
    REQUIRE(inspected.size() == 9);
    for (const auto& field : inspected) {
        REQUIRE(field.has_value());
    }
    REQUIRE(inspected[0].value() == "hash_major");
    const long long generation_id = std::stoll(inspected[1].value());
    REQUIRE(generation_id > 0);
    REQUIRE(inspected[2].value() == std::to_string(sequence_rowno));
    REQUIRE(inspected[3].value() == std::to_string(target_bp));
    REQUIRE(inspected[4].value() == std::to_string(expected_scheme_identity));
    REQUIRE(inspected[5].value()
            == std::to_string(expected_scheme_revision));
    REQUIRE(inspected[6].value() == std::to_string(expected_producer_identity));
    REQUIRE(inspected[7].value() == "1");
    REQUIRE(inspected[8].value() == "1");
    return generation_id;
}

[[nodiscard]] long long require_ready_fragment_minimizer_metadata(
        const connection_test_context& ctx,
        std::string_view table,
        std::string_view index_name,
        long long sequence_rowno,
        long long target_bp,
        long long expected_position_segment_span_bp,
        long long expected_position_segment_count) {
    const row_t inspected = query_one(ctx, fmt::format(
        "SELECT generation_id::text, sequence_rowno::text,"
        "total_bp_size::text, kmer_size::text, window_size::text,"
        "gpu_abi_revision::text, segment_geometry_revision::text,"
        "position_segment_span_bp::text, position_segment_count::text "
        "FROM bioseq.inspect_ready_minimizer_generation("
        "'{}'::regclass, '{}', {});",
        table, index_name, sequence_rowno));
    REQUIRE(inspected.size() == 9u);
    for (const auto& field : inspected) {
        REQUIRE(field.has_value());
    }
    const long long generation_id = std::stoll(inspected[0].value());
    REQUIRE(generation_id > 0);
    REQUIRE(inspected[1].value() == std::to_string(sequence_rowno));
    REQUIRE(inspected[2].value() == std::to_string(target_bp));
    REQUIRE(inspected[3].value() == "15");
    REQUIRE(inspected[4].value() == "10");
    // These storage-profile fields identify the fragment-minimizer image
    // accepted by this inspector.
    REQUIRE(inspected[5].value() == "3");
    REQUIRE(inspected[6].value() == "2");
    REQUIRE(inspected[7].value()
            == std::to_string(expected_position_segment_span_bp));
    REQUIRE(inspected[8].value()
            == std::to_string(expected_position_segment_count));
    return generation_id;
}

void require_bioseq_shared_preload(const connection_test_context& ctx) {
    const auto preload_state = query_one(ctx,
        "SELECT EXISTS ("
        "SELECT 1 FROM unnest(string_to_array("
        "current_setting('shared_preload_libraries', true), ',')) AS entry "
        "WHERE btrim(entry) IN ('bioseq', '$libdir/bioseq'))::text, "
        "is_supervisor::text FROM bioseq.scheduler_stats();");
    REQUIRE(preload_state.size() == 2);
    INFO("ordinary bioseq DROP preflight requires bioseq in "
         "shared_preload_libraries and attached scheduler shared state; "
         "observed configured=" << preload_state[0].value()
         << " supervisor=" << preload_state[1].value());
    REQUIRE(preload_state[0].value() == "true");
    REQUIRE(preload_state[1].value() == "true");
}

[[nodiscard]] std::string drop_preservation_probe_literal() {
    return format_literal(
        k_hoe_probe_sequence,
        seq_typmod{.alphabet = "dna4", .width = 64});
}

struct preserved_ready_bioseq_source_evidence {
    long long observed_oid{};
    long long bioseq_relation_count{};
    row_t source;
    rowset_t hydrated;
};

[[nodiscard]] preserved_ready_bioseq_source_evidence
capture_preserved_ready_bioseq_source(
        const connection_test_context& ctx,
        std::string_view table,
        const long long expected_oid) {
    // A plain scan of a USING bioseq relation is not a row-preservation
    // oracle: this AM intentionally emits tuples only for an mmsearch scan.
    // Pin the original relation identity, then verify its published locator and
    // physical fragment still exist before exercising real AM hydration.
    const long long observed_oid = query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('{}')::oid::bigint;", table));
    const long long bioseq_relation_count = query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_am am ON am.oid=c.relam "
        "WHERE c.oid={}::oid AND am.amname='bioseq';",
        expected_oid));

    auto source = query_one(ctx, fmt::format(
        "SELECT count(*)::text, "
        "count(*) FILTER (WHERE s.state=1)::text, "
        "min(s.source_epoch)::text, min(s.locator_version)::text, "
        "min(s.num_fragments)::text, min(s.total_bp_size)::text, "
        "count(*) FILTER (WHERE s.fragment_relid IS NOT NULL "
        "AND fragment.oid IS NOT NULL)::text "
        "FROM bioseq_internal.sequence_source_version s "
        "LEFT JOIN pg_class fragment ON fragment.oid=s.fragment_relid "
        "WHERE s.user_relid={}::oid;",
        expected_oid));

    auto hydrated = query_rows(ctx, fmt::format(
        "SELECT id::text, seq::text FROM {} "
        "WHERE mmsearch(seq, {}) ORDER BY id;",
        table, drop_preservation_probe_literal()));

    return preserved_ready_bioseq_source_evidence{
        .observed_oid = observed_oid,
        .bioseq_relation_count = bioseq_relation_count,
        .source = std::move(source),
        .hydrated = std::move(hydrated),
    };
}

void require_preserved_ready_bioseq_source_evidence(
        const preserved_ready_bioseq_source_evidence& evidence,
        const long long expected_oid) {
    REQUIRE(evidence.observed_oid == expected_oid);
    REQUIRE(evidence.bioseq_relation_count == 1);

    const auto& source = evidence.source;
    REQUIRE(source.size() == 7);
    REQUIRE(source[0].value() == "1");
    REQUIRE(source[1].value() == "1");
    REQUIRE(std::stoll(source[2].value()) > 0);
    REQUIRE(source[3].value() == "3");
    REQUIRE(source[4].value() == "1");
    REQUIRE(source[5].value()
            == std::to_string(k_hoe_probe_sequence.size()));
    REQUIRE(source[6].value() == "1");

    const auto& hydrated = evidence.hydrated;
    REQUIRE(hydrated.size() == 1);
    REQUIRE(hydrated[0].size() == 2);
    REQUIRE(hydrated[0][0].value() == "1");
    REQUIRE(hydrated[0][1].value() == k_hoe_probe_sequence);
}

void require_preserved_ready_bioseq_source(
        const connection_test_context& ctx,
        std::string_view table,
        const long long expected_oid) {
    require_preserved_ready_bioseq_source_evidence(
        capture_preserved_ready_bioseq_source(ctx, table, expected_oid),
        expected_oid);
}

} // namespace

TEST_CASE("list_metadata reports all production index kinds for a fresh bioseq table",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    // No inserts: list_metadata is NOT gated on data, so every registered
    // index must already appear for the fresh table.
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) "
              "USING bioseq;",
              k_table);

    // Filter out the test-only CHUNKED_PER_TABLE 'test_table_rowcount' index
    // (only present under -DBIOSEQ_TEST_INDEXES) so the assertion pins exactly
    // the three production kinds regardless of how the dylib was built.
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT index_name, index_type, update_policy, "
                    "storage_size, (time_since_update IS NULL) "
                    "  FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    "   AND index_name <> 'test_table_rowcount' "
                    " ORDER BY index_name;",
                    k_table));

    REQUIRE(rows.size() == 3);

    // Sorted by index_name: minimizers_per_fragment, minimizers_per_sequence,
    // symbol_counts.
    REQUIRE(rows[0].size() == 5);
    REQUIRE(rows[0][0].value() == "minimizers_per_fragment");
    REQUIRE(rows[0][1].value() == "CHUNKED_PER_FRAGMENT");
    REQUIRE(rows[0][2].value() == "UPDATE_ON_INSERT");
    REQUIRE(rows[0][3].value() == "0 bytes");
    REQUIRE(rows[0][4].value() == "t");  // time_since_update IS NULL

    REQUIRE(rows[1].size() == 5);
    REQUIRE(rows[1][0].value() == "minimizers_per_sequence");
    REQUIRE(rows[1][1].value() == "CHUNKED_PER_FRAGMENT");
    REQUIRE(rows[1][2].value() == "UPDATE_MANUALLY");
    REQUIRE(rows[1][3].value() == "0 bytes");
    REQUIRE(rows[1][4].value() == "t");

    REQUIRE(rows[2].size() == 5);
    REQUIRE(rows[2][0].value() == "symbol_counts");
    REQUIRE(rows[2][1].value() == "INLINE_PER_SEQUENCE");
    REQUIRE(rows[2][2].value() == "UPDATE_ON_INSERT");
    REQUIRE(rows[2][3].value() == "0 bytes");
    REQUIRE(rows[2][4].value() == "t");
}

TEST_CASE("published generation inspection SQL surface is narrow and administrative",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    connection_test_context ctx;
    const auto row = query_one(ctx,
        "SELECT p.provolatile::text, p.proparallel::text, "
        "p.prosecdef::int::text, p.proisstrict::int::text, "
        "p.proretset::int::text, p.pronargs::text, "
        "cardinality(p.proallargtypes)::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(p.proacl) a "
        " WHERE a.grantee=0 AND a.privilege_type='EXECUTE'))::int::text "
        "FROM pg_proc p WHERE p.oid="
        "'bioseq.inspect_ready_minimizer_generation(regclass,text,bigint)'::regprocedure;");
    REQUIRE(row.size() == 8);
    REQUIRE(row[0].value() == "s");       // STABLE: read-only catalogue contract
    REQUIRE(row[1].value() == "r");       // PARALLEL RESTRICTED
    REQUIRE(row[2].value() == "0");       // SECURITY INVOKER
    REQUIRE(row[3].value() == "1");       // STRICT
    REQUIRE(row[4].value() == "1");       // RETURNS TABLE / SETOF record
    REQUIRE(row[5].value() == "3");       // exact input signature
    REQUIRE(row[6].value() == "26");      // 3 inputs + 23 named outputs
    REQUIRE(row[7].value() == "1");       // PUBLIC has no EXECUTE grant
}

TEST_CASE("generation allocator has the exact owned permanent contract",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    connection_test_context ctx;
    const auto row = query_one(ctx,
        "SELECT n.nspname, c.relname, c.relkind::text, "
        "c.relpersistence::text, format_type(s.seqtypid,NULL), "
        "s.seqincrement::text, s.seqmin::text, s.seqmax::text, "
        "s.seqstart::text, s.seqcache::text, s.seqcycle::int::text, "
        "(EXISTS (SELECT 1 FROM pg_depend d JOIN pg_extension e "
        "ON e.oid=d.refobjid WHERE d.classid='pg_class'::regclass "
        "AND d.objid=c.oid AND d.objsubid=0 "
        "AND d.refclassid='pg_extension'::regclass "
        "AND d.deptype='e' AND e.extname='bioseq' "
        "AND e.extowner=c.relowner))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(c.relacl, "
        "acldefault('S',c.relowner))) a WHERE a.grantee=0))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(c.relacl, "
        "acldefault('S',c.relowner))) a "
        "WHERE a.grantee<>c.relowner))::int::text "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_sequence s ON s.seqrelid=c.oid "
        "WHERE c.oid='bioseq_internal.minimizer_generation_id_seq'::regclass;");
    REQUIRE(row.size() == 14);
    REQUIRE(row[0].value() == "bioseq_internal");
    REQUIRE(row[1].value() == "minimizer_generation_id_seq");
    REQUIRE(row[2].value() == "S");
    REQUIRE(row[3].value() == "p");
    REQUIRE(row[4].value() == "bigint");
    REQUIRE(row[5].value() == "1");
    REQUIRE(row[6].value() == "1");
    REQUIRE(row[7].value() == "9223372036854775807");
    REQUIRE(row[8].value() == "1");
    REQUIRE(row[9].value() == "1");
    REQUIRE(row[10].value() == "0");
    REQUIRE(row[11].value() == "1");
    REQUIRE(row[12].value() == "1");
    REQUIRE(row[13].value() == "1");
}

TEST_CASE("sequence-source epoch allocator has the exact owned permanent contract",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    connection_test_context ctx;
    const auto row = query_one(ctx,
        "SELECT n.nspname, c.relname, c.relkind::text, "
        "c.relpersistence::text, format_type(s.seqtypid,NULL), "
        "s.seqincrement::text, s.seqmin::text, s.seqmax::text, "
        "s.seqstart::text, s.seqcache::text, s.seqcycle::int::text, "
        "(EXISTS (SELECT 1 FROM pg_depend d JOIN pg_extension e "
        "ON e.oid=d.refobjid WHERE d.classid='pg_class'::regclass "
        "AND d.objid=c.oid AND d.objsubid=0 "
        "AND d.refclassid='pg_extension'::regclass "
        "AND d.deptype='e' AND e.extname='bioseq' "
        "AND e.extowner=c.relowner))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(c.relacl, "
        "acldefault('S',c.relowner))) a WHERE a.grantee=0))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(c.relacl, "
        "acldefault('S',c.relowner))) a "
        "WHERE a.grantee<>c.relowner))::int::text "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_sequence s ON s.seqrelid=c.oid "
        "WHERE c.oid='bioseq_internal.sequence_source_epoch_seq'::regclass;");
    REQUIRE(row.size() == 14);
    REQUIRE(row[0].value() == "bioseq_internal");
    REQUIRE(row[1].value() == "sequence_source_epoch_seq");
    REQUIRE(row[2].value() == "S");
    REQUIRE(row[3].value() == "p");
    REQUIRE(row[4].value() == "bigint");
    REQUIRE(row[5].value() == "1");
    REQUIRE(row[6].value() == "1");
    REQUIRE(row[7].value() == "9223372036854775807");
    REQUIRE(row[8].value() == "1");
    REQUIRE(row[9].value() == "1");
    REQUIRE(row[10].value() == "0");
    REQUIRE(row[11].value() == "1");
    REQUIRE(row[12].value() == "1");
    REQUIRE(row[13].value() == "1");
}

TEST_CASE("sequence-source catalog has exact schema indexes and ACL",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    connection_test_context ctx;
    const auto catalog = query_one(ctx,
        "SELECT n.nspname, c.relname, c.relkind::text, "
        "c.relpersistence::text, am.amname, "
        "(SELECT string_agg(a.attname || ':' || "
        "format_type(a.atttypid,a.atttypmod) || ':' || "
        "a.attnotnull::int, ',' ORDER BY a.attnum) "
        "FROM pg_attribute a WHERE a.attrelid=c.oid "
        "AND a.attnum>0 AND NOT a.attisdropped), "
        "(EXISTS (SELECT 1 FROM pg_depend d JOIN pg_extension e "
        "ON e.oid=d.refobjid WHERE d.classid='pg_class'::regclass "
        "AND d.objid=c.oid AND d.objsubid=0 "
        "AND d.refclassid='pg_extension'::regclass "
        "AND d.deptype='e' AND e.extname='bioseq' "
        "AND e.extowner=c.relowner))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(c.relacl, "
        "acldefault('r',c.relowner))) a WHERE a.grantee=0))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(c.relacl, "
        "acldefault('r',c.relowner))) a "
        "WHERE a.grantee<>c.relowner))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(n.nspacl, "
        "acldefault('n',n.nspowner))) a WHERE a.grantee=0 "
        "AND a.privilege_type='USAGE'))::int::text, "
        "(EXISTS (SELECT 1 FROM pg_extension e "
        "CROSS JOIN LATERAL unnest(e.extconfig,e.extcondition) "
        "AS cfg(relid,condition) WHERE e.extname='bioseq' "
        "AND cfg.relid=c.oid AND cfg.condition='WHERE FALSE'))::int::text "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_am am ON am.oid=c.relam "
        "WHERE c.oid='bioseq_internal.sequence_source_version'::regclass;");
    REQUIRE(catalog.size() == 11);
    REQUIRE(catalog[0].value() == "bioseq_internal");
    REQUIRE(catalog[1].value() == "sequence_source_version");
    REQUIRE(catalog[2].value() == "r");
    REQUIRE(catalog[3].value() == "p");
    REQUIRE(catalog[4].value() == "heap");
    REQUIRE(catalog[5].value() ==
        "source_epoch:bigint:1,user_relid:oid:1,database_oid:oid:1,"
        "tablespace_oid:oid:1,main_relfilenode:oid:1,main_storage_id:bigint:1,"
        "column_attno:smallint:1,locator_version:smallint:1,"
        "bucket_idx:smallint:1,shard_idx:integer:1,fragment_relid:oid:0,"
        "fragment_storage_id:bigint:1,first_sub_row_no:bigint:1,"
        "num_fragments:bigint:1,total_bp_size:bigint:1,"
        "alphabet_code:smallint:1,storage_unit_byte_width:smallint:1,"
        "padding_letters:integer:1,source_content_checksum:bigint:1,"
        "state:smallint:1");
    REQUIRE(catalog[6].value() == "1");  // extension member, same owner
    REQUIRE(catalog[7].value() == "1");  // no PUBLIC table privilege
    REQUIRE(catalog[8].value() == "1");  // no non-owner table privilege
    REQUIRE(catalog[9].value() == "1");  // no PUBLIC internal-schema USAGE
    REQUIRE(catalog[10].value() == "1"); // empty config-dump contract

    const auto indexes = query_rows(ctx,
        "SELECT ci.relname, i.indisprimary::int::text, "
        "i.indisunique::int::text, i.indimmediate::int::text, "
        "i.indnkeyatts::text, i.indnatts::text, i.indkey::text, "
        "(i.indpred IS NULL)::int::text, "
        "(i.indexprs IS NULL)::int::text "
        "FROM pg_index i JOIN pg_class ci ON ci.oid=i.indexrelid "
        "WHERE i.indrelid="
        "'bioseq_internal.sequence_source_version'::regclass "
        "ORDER BY ci.relname;");
    REQUIRE(indexes.size() == 3);
    REQUIRE(indexes[0][0].value() ==
        "sequence_source_version_fragment_row_idx");
    REQUIRE(indexes[0][1].value() == "0");
    REQUIRE(indexes[0][2].value() == "0");
    REQUIRE(indexes[0][3].value() == "1");
    REQUIRE(indexes[0][4].value() == "2");
    REQUIRE(indexes[0][5].value() == "2");
    REQUIRE(indexes[0][6].value() == "11 13");
    REQUIRE(indexes[0][7].value() == "1");
    REQUIRE(indexes[0][8].value() == "1");
    REQUIRE(indexes[1][0].value() ==
        "sequence_source_version_locator_key");
    REQUIRE(indexes[1][1].value() == "0");
    REQUIRE(indexes[1][2].value() == "1");
    REQUIRE(indexes[1][3].value() == "1");
    REQUIRE(indexes[1][4].value() == "3");
    REQUIRE(indexes[1][5].value() == "3");
    REQUIRE(indexes[1][6].value() == "11 12 13");
    REQUIRE(indexes[1][7].value() == "1");
    REQUIRE(indexes[1][8].value() == "1");
    REQUIRE(indexes[2][0].value() == "sequence_source_version_pkey");
    REQUIRE(indexes[2][1].value() == "1");
    REQUIRE(indexes[2][2].value() == "1");
    REQUIRE(indexes[2][3].value() == "1");
    REQUIRE(indexes[2][4].value() == "1");
    REQUIRE(indexes[2][5].value() == "1");
    REQUIRE(indexes[2][6].value() == "1");
    REQUIRE(indexes[2][7].value() == "1");
    REQUIRE(indexes[2][8].value() == "1");

    const auto constraints = query_one(ctx,
        "SELECT string_agg(conname || ':' || contype::text, "
        "',' ORDER BY conname) FROM pg_constraint "
        "WHERE conrelid="
        "'bioseq_internal.sequence_source_version'::regclass "
        ";");
    REQUIRE(constraints.size() == 1);
    REQUIRE(constraints[0].value() ==
        "sequence_source_version_alphabet_code_check:c,"
        "sequence_source_version_bucket_idx_check:c,"
        "sequence_source_version_column_attno_check:c,"
        "sequence_source_version_first_sub_row_no_check:c,"
        "sequence_source_version_fragment_shape_check:c,"
        "sequence_source_version_fragment_storage_id_check:c,"
        "sequence_source_version_locator_version_check:c,"
        "sequence_source_version_main_storage_id_check:c,"
        "sequence_source_version_num_fragments_check:c,"
        "sequence_source_version_padding_letters_check:c,"
        "sequence_source_version_pkey:p,"
        "sequence_source_version_shard_idx_check:c,"
        "sequence_source_version_source_epoch_check:c,"
        "sequence_source_version_state_check:c,"
        "sequence_source_version_storage_unit_byte_width_check:c,"
        "sequence_source_version_total_bp_size_check:c");

    const auto dependencies = query_one(ctx,
        "SELECT string_agg(c.relname, ',' ORDER BY c.relname) "
        "FROM pg_depend d JOIN pg_am am ON d.classid='pg_am'::regclass "
        "AND d.objid=am.oid JOIN pg_class c "
        "ON d.refclassid='pg_class'::regclass AND d.refobjid=c.oid "
        "WHERE am.amname='bioseq' AND d.deptype='n' AND c.relname IN "
        "('sequence_source_epoch_seq','sequence_source_version',"
        "'sequence_source_version_fragment_row_idx',"
        "'sequence_source_version_locator_key',"
        "'sequence_source_version_pkey');");
    REQUIRE(dependencies.size() == 1);
    REQUIRE(dependencies[0].value() ==
        "sequence_source_epoch_seq,sequence_source_version,"
        "sequence_source_version_fragment_row_idx,"
        "sequence_source_version_locator_key,sequence_source_version_pkey");
}

TEST_CASE("fragment-minimizer lookup-route GUC has canonical spellings",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    connection_test_context ctx;
    const auto setting = query_one(ctx,
        "SELECT setting, reset_val, boot_val, context, vartype, "
        "array_to_string(enumvals, ',') FROM pg_settings "
        "WHERE name='bioseq.aligner.fragment_minimizer.lookup_route';");
    REQUIRE(setting.size() == 6);
    REQUIRE(setting[0].value() == "control_block_scan");
    REQUIRE(setting[1].value() == "control_block_scan");
    REQUIRE(setting[2].value() == "control_block_scan");
    REQUIRE(setting[3].value() == "user");
    REQUIRE(setting[4].value() == "enum");
    REQUIRE(setting[5].value()
        == "control_block_scan,direct_compact_hash_lookup");

    exec_sql(ctx,
        "SET bioseq.aligner.fragment_minimizer.lookup_route="
        "direct_compact_hash_lookup;");
    REQUIRE(query_scalar(ctx,
        "SHOW bioseq.aligner.fragment_minimizer.lookup_route;")
        == "direct_compact_hash_lookup");
    exec_sql(ctx, "RESET bioseq.aligner.fragment_minimizer.lookup_route;");
    REQUIRE(query_scalar(ctx,
        "SHOW bioseq.aligner.fragment_minimizer.lookup_route;")
        == "control_block_scan");
}

TEST_CASE("generation descriptor and reaper catalogs have exact protected shape",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    connection_test_context ctx;
    const auto catalogs = query_rows(ctx,
        "SELECT c.relname, c.relkind::text, c.relpersistence::text, am.amname, "
        "(SELECT string_agg(a.attname || ':' || "
        "format_type(a.atttypid,a.atttypmod) || ':' || "
        "a.attnotnull::int || ':' || a.atthasdef::int, ',' ORDER BY a.attnum) "
        "FROM pg_attribute a WHERE a.attrelid=c.oid "
        "AND a.attnum>0 AND NOT a.attisdropped), "
        "(EXISTS (SELECT 1 FROM pg_depend d JOIN pg_extension e "
        "ON e.oid=d.refobjid WHERE d.classid='pg_class'::regclass "
        "AND d.objid=c.oid AND d.objsubid=0 "
        "AND d.refclassid='pg_extension'::regclass "
        "AND d.deptype='e' AND e.extname='bioseq' "
        "AND e.extowner=c.relowner))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(c.relacl, "
        "acldefault('r',c.relowner))) a WHERE a.grantee=0))::int::text, "
        "(NOT EXISTS (SELECT 1 FROM aclexplode(COALESCE(c.relacl, "
        "acldefault('r',c.relowner))) a "
        "WHERE a.grantee<>c.relowner))::int::text, "
        "(EXISTS (SELECT 1 FROM pg_extension e "
        "CROSS JOIN LATERAL unnest(e.extconfig,e.extcondition) "
        "AS cfg(relid,condition) WHERE e.extname='bioseq' "
        "AND cfg.relid=c.oid AND cfg.condition='WHERE FALSE'))::int::text "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_am am ON am.oid=c.relam "
        "WHERE n.nspname='bioseq_internal' AND c.relname IN "
        "('minimizer_generation_storage',"
        "'minimizer_generation_reaper_state') ORDER BY c.relname;");
    REQUIRE(catalogs.size() == 2);
    REQUIRE(catalogs[0].size() == 9);
    REQUIRE(catalogs[0][0].value() ==
        "minimizer_generation_reaper_state");
    REQUIRE(catalogs[0][1].value() == "r");
    REQUIRE(catalogs[0][2].value() == "u");
    REQUIRE(catalogs[0][3].value() == "heap");
    REQUIRE(catalogs[0][4].value() ==
        "database_oid:oid:1:0,tablespace_root_ordinal:integer:1:1,"
        "shard_ordinal:smallint:1:1,last_canonical_filename:text:1:1,"
        "pass:bigint:1:1");
    REQUIRE(catalogs[1][0].value() == "minimizer_generation_storage");
    REQUIRE(catalogs[1][1].value() == "r");
    REQUIRE(catalogs[1][2].value() == "u");
    REQUIRE(catalogs[1][3].value() == "heap");
    REQUIRE(catalogs[1][4].value() ==
        "database_oid:oid:1:0,tablespace_oid:oid:1:0,"
        "main_relfilenode:oid:1:0,user_relid:oid:1:0,"
        "column_attno:smallint:1:0,index_name:name:1:0,"
        "query_key:bigint:1:0,generation_id:bigint:1:0,"
        "provider:smallint:1:0,payload_relation_oid:oid:1:0,"
        "payload_database_oid:oid:1:0,payload_tablespace_oid:oid:1:0,"
        "payload_relfilenode:oid:1:0,payload_relation_name:name:1:0,"
        "image_bytes:bigint:1:0,header_checksum:bigint:1:0,"
        "retire_full_xid:xid8:0:0,state:smallint:1:0");
    for (const auto& catalog : catalogs) {
        REQUIRE(catalog[5].value() == "1"); // extension-owned member
        REQUIRE(catalog[6].value() == "1"); // no PUBLIC table privilege
        REQUIRE(catalog[7].value() == "1"); // no non-owner privilege
        REQUIRE(catalog[8].value() == "1"); // empty config-dump contract
    }

    const auto storage_indexes = query_rows(ctx,
        "SELECT ci.relname, i.indisprimary::int::text, "
        "i.indisunique::int::text, i.indimmediate::int::text, "
        "i.indisvalid::int::text, i.indisready::int::text, "
        "i.indislive::int::text, i.indnkeyatts::text, i.indnatts::text, "
        "i.indkey::text, COALESCE(regexp_replace(pg_get_expr(i.indpred, "
        "i.indrelid), '[()[:space:]]', '', 'g'), ''), "
        "(SELECT string_agg(opc.opcname, ',' ORDER BY opclass_key.ord) "
        "FROM unnest(i.indclass::oid[]) WITH ORDINALITY "
        "opclass_key(opcoid,ord) "
        "JOIN pg_opclass opc ON opc.oid=opclass_key.opcoid) "
        "FROM pg_index i JOIN pg_class ci ON ci.oid=i.indexrelid "
        "WHERE i.indrelid="
        "'bioseq_internal.minimizer_generation_storage'::regclass "
        "ORDER BY ci.relname;");
    REQUIRE(storage_indexes.size() == 3);
    REQUIRE(storage_indexes[0][0].value() ==
        "minimizer_generation_storage_one_live");
    REQUIRE(storage_indexes[0][1].value() == "0");
    REQUIRE(storage_indexes[0][2].value() == "1");
    REQUIRE(storage_indexes[0][7].value() == "7");
    REQUIRE(storage_indexes[0][8].value() == "7");
    REQUIRE(storage_indexes[0][9].value() == "1 2 3 4 5 6 7");
    REQUIRE(storage_indexes[0][10].value() == "state='1'::smallint");
    REQUIRE(storage_indexes[0][11].value() ==
        "oid_ops,oid_ops,oid_ops,oid_ops,int2_ops,name_ops,int8_ops");
    REQUIRE(storage_indexes[1][0].value() ==
        "minimizer_generation_storage_pkey");
    REQUIRE(storage_indexes[1][1].value() == "1");
    REQUIRE(storage_indexes[1][2].value() == "1");
    REQUIRE(storage_indexes[1][7].value() == "8");
    REQUIRE(storage_indexes[1][8].value() == "8");
    REQUIRE(storage_indexes[1][9].value() == "1 2 3 4 5 6 7 8");
    REQUIRE(storage_indexes[1][10].value().empty());
    REQUIRE(storage_indexes[1][11].value() ==
        "oid_ops,oid_ops,oid_ops,oid_ops,int2_ops,name_ops,int8_ops,int8_ops");
    REQUIRE(storage_indexes[2][0].value() ==
        "minimizer_generation_storage_retired");
    REQUIRE(storage_indexes[2][1].value() == "0");
    REQUIRE(storage_indexes[2][2].value() == "0");
    REQUIRE(storage_indexes[2][7].value() == "9");
    REQUIRE(storage_indexes[2][8].value() == "9");
    REQUIRE(storage_indexes[2][9].value() == "17 1 2 3 4 5 6 7 8");
    REQUIRE(storage_indexes[2][10].value() == "state='2'::smallint");
    REQUIRE(storage_indexes[2][11].value() ==
        "xid8_ops,oid_ops,oid_ops,oid_ops,oid_ops,int2_ops,name_ops,int8_ops,int8_ops");
    for (const auto& index : storage_indexes) {
        REQUIRE(index[3].value() == "1");
        REQUIRE(index[4].value() == "1");
        REQUIRE(index[5].value() == "1");
        REQUIRE(index[6].value() == "1");
    }

    const auto constraint_receipt = query_one(ctx,
        "SELECT count(*) FILTER (WHERE contype='c')::text, "
        "count(*) FILTER (WHERE contype='p')::text, "
        "bool_and(convalidated)::int::text, "
        "bool_and(CASE WHEN contype='c' THEN NOT connoinherit "
        "WHEN contype='p' THEN connoinherit ELSE false END)::int::text, "
        "bool_and(CASE WHEN contype='c' THEN conbin IS NOT NULL "
        "ELSE conkey=ARRAY[1,2,3,4,5,6,7,8]::smallint[] END)::int::text "
        "FROM pg_constraint WHERE conrelid="
        "'bioseq_internal.minimizer_generation_storage'::regclass;");
    REQUIRE(constraint_receipt[0].value() == "13");
    REQUIRE(constraint_receipt[1].value() == "1");
    REQUIRE(constraint_receipt[2].value() == "1");
    REQUIRE(constraint_receipt[3].value() == "1");
    REQUIRE(constraint_receipt[4].value() == "1");

    const auto reaper_index = query_one(ctx,
        "SELECT ci.relname, i.indisprimary::int::text, "
        "i.indisunique::int::text, i.indisvalid::int::text, "
        "i.indnkeyatts::text, i.indnatts::text, i.indkey::text, "
        "(i.indpred IS NULL)::int::text, "
        "(i.indexprs IS NULL)::int::text "
        "FROM pg_index i JOIN pg_class ci ON ci.oid=i.indexrelid "
        "WHERE i.indrelid="
        "'bioseq_internal.minimizer_generation_reaper_state'::regclass;");
    REQUIRE(reaper_index[0].value() ==
        "minimizer_generation_reaper_state_pkey");
    REQUIRE(reaper_index[1].value() == "1");
    REQUIRE(reaper_index[2].value() == "1");
    REQUIRE(reaper_index[3].value() == "1");
    REQUIRE(reaper_index[4].value() == "1");
    REQUIRE(reaper_index[5].value() == "1");
    REQUIRE(reaper_index[6].value() == "1");
    REQUIRE(reaper_index[7].value() == "1");
    REQUIRE(reaper_index[8].value() == "1");
    const auto reaper_constraints = query_one(ctx,
        "SELECT count(*) FILTER (WHERE contype='c')::text, "
        "count(*) FILTER (WHERE contype='p')::text, "
        "bool_and(convalidated AND CASE WHEN contype='c' "
        "THEN NOT connoinherit WHEN contype='p' THEN connoinherit "
        "ELSE false END)::int::text "
        "FROM pg_constraint WHERE conrelid="
        "'bioseq_internal.minimizer_generation_reaper_state'::regclass;");
    REQUIRE(reaper_constraints[0].value() == "4");
    REQUIRE(reaper_constraints[1].value() == "1");
    REQUIRE(reaper_constraints[2].value() == "1");

    exec_sql(ctx, "BEGIN;");
    const auto defaults = query_one(ctx,
        "INSERT INTO bioseq_internal.minimizer_generation_reaper_state "
        "(database_oid) VALUES ('4294967294'::oid) RETURNING "
        "tablespace_root_ordinal::text, shard_ordinal::text, "
        "last_canonical_filename, pass::text;");
    REQUIRE(defaults[0].value() == "0");
    REQUIRE(defaults[1].value() == "0");
    REQUIRE(defaults[2].value().empty());
    REQUIRE(defaults[3].value() == "0");
    exec_sql(ctx, "ROLLBACK;");

    const auto relation_without_locator = sql_error(ctx,
        "INSERT INTO bioseq_internal.minimizer_generation_storage "
        "(database_oid,tablespace_oid,main_relfilenode,user_relid,"
        "column_attno,index_name,query_key,generation_id,provider,"
        "image_bytes,header_checksum,state) VALUES "
        "(1,1,1,1,1,'minimizers_per_fragment',0,9223372036854775800,"
        "0,4096,0,1);");
    REQUIRE(relation_without_locator.sqlstate == "23502");
    const auto unsupported_provider = sql_error(ctx,
        "INSERT INTO bioseq_internal.minimizer_generation_storage "
        "(database_oid,tablespace_oid,main_relfilenode,user_relid,"
        "column_attno,index_name,query_key,generation_id,provider,"
        "payload_relation_oid,payload_database_oid,payload_tablespace_oid,"
        "payload_relfilenode,payload_relation_name,image_bytes,"
        "header_checksum,state) VALUES "
        "(1,1,1,1,1,'minimizers_per_fragment',0,9223372036854775801,"
        "1,1,1,1,1,'unexpected',4096,0,1);");
    REQUIRE(unsupported_provider.sqlstate == "23514");
    const auto live_with_retire_xid = sql_error(ctx,
        "INSERT INTO bioseq_internal.minimizer_generation_storage "
        "(database_oid,tablespace_oid,main_relfilenode,user_relid,"
        "column_attno,index_name,query_key,generation_id,provider,"
        "payload_relation_oid,payload_database_oid,payload_tablespace_oid,"
        "payload_relfilenode,payload_relation_name,image_bytes,"
        "header_checksum,retire_full_xid,state) VALUES "
        "(1,1,1,1,1,'minimizers_per_fragment',0,9223372036854775802,"
        "0,1,1,1,1,'unexpected',4096,0,'1'::xid8,1);");
    REQUIRE(live_with_retire_xid.sqlstate == "23514");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("relation descriptor publication replaces and rolls back atomically",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_descriptor_publication";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled=off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4',64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1,repeat('ACGT',4099)::bioseq.seq('dna4',64));", table);
    const auto rowno = only_column_row_number(ctx, table);
    const auto build = [&] {
        return query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_fragment_generation("
            "'{}'::regclass,'minimizers_per_fragment',{},'scalar');",
            table, rowno));
    };
    const auto descriptors = [&](const std::string& sidecar) {
        return query_rows(ctx, fmt::format(
            "SELECT generation_id::text,state::text,"
            "(retire_full_xid IS NOT NULL)::int::text,provider::text,"
            "(payload_relation_oid='bioseq_internal.{}'::regclass)::int::text,"
            "(payload_database_oid=(SELECT oid FROM pg_database "
            "WHERE datname=current_database()))::int::text,"
            "(payload_relfilenode=pg_relation_filenode("
            "'bioseq_internal.{}'::regclass))::int::text,"
            "(payload_relation_name='{}'::name)::int::text,"
            "(image_bytes>=4096)::int::text "
            "FROM bioseq_internal.minimizer_generation_storage "
            "WHERE user_relid='{}'::regclass AND query_key={} "
            "AND index_name='minimizers_per_fragment' "
            "ORDER BY generation_id;",
            sidecar, sidecar, sidecar, table, rowno));
    };

    const auto baseline = build();
    REQUIRE(baseline.size() == 45);
    const std::string sidecar = baseline[0].value();
    const std::string baseline_generation = baseline[10].value();
    auto rows = descriptors(sidecar);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][0].value() == baseline_generation);
    REQUIRE(rows[0][1].value() == "1");
    REQUIRE(rows[0][2].value() == "0");
    REQUIRE(rows[0][3].value() == "0");
    for (std::size_t field = 4; field < rows[0].size(); ++field) {
        REQUIRE(rows[0][field].value() == "1");
    }

    exec_sql(ctx, "BEGIN;");
    const auto speculative = build();
    REQUIRE(speculative[0].value() == sidecar);
    rows = descriptors(sidecar);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0][0].value() == baseline_generation);
    REQUIRE(rows[0][1].value() == "2");
    REQUIRE(rows[0][2].value() == "1");
    REQUIRE(rows[1][0].value() == speculative[10].value());
    REQUIRE(rows[1][1].value() == "1");
    REQUIRE(rows[1][2].value() == "0");
    exec_sql(ctx, "ROLLBACK;");
    rows = descriptors(sidecar);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][0].value() == baseline_generation);
    REQUIRE(rows[0][1].value() == "1");
    REQUIRE(rows[0][2].value() == "0");

    const auto replacement = build();
    rows = descriptors(sidecar);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0][0].value() == baseline_generation);
    REQUIRE(rows[0][1].value() == "2");
    REQUIRE(rows[0][2].value() == "1");
    REQUIRE(rows[1][0].value() == replacement[10].value());
    REQUIRE(rows[1][1].value() == "1");
    REQUIRE(rows[1][2].value() == "0");
    REQUIRE(descriptors(sidecar) == rows);
    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx,
        "DELETE FROM bioseq_internal.minimizer_generation_storage "
        "WHERE user_relid='{}'::regclass AND query_key={} AND state=1;",
        table, rowno);
    const auto missing_live = sql_error(ctx, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass,'minimizers_per_fragment',{},'scalar');",
        table, rowno));
    REQUIRE(missing_live.contains(
        "active head and LIVE generation descriptor disagree"));
    exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE("source epochs publish atomically and represent repeated empty sources",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_source_empty"});
    exec_sql(ctx,
        "CREATE TABLE index_admin_source_empty "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO index_admin_source_empty VALUES "
        "(1, ''::bioseq.seq('dna4', 64)), "
        "(2, ''::bioseq.seq('dna4', 64));");
    const auto empty_rows = query_one(ctx,
        "SELECT count(*)::text, count(DISTINCT source_epoch)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE num_fragments=0 AND fragment_relid IS NULL "
        "AND fragment_storage_id=0 AND first_sub_row_no=0)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_empty'::regclass;");
    REQUIRE(empty_rows[0].value() == "2");
    REQUIRE(empty_rows[1].value() == "2");
    REQUIRE(empty_rows[2].value() == "2");
    REQUIRE(empty_rows[3].value() == "2");

    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "INSERT INTO index_admin_source_empty VALUES "
        "(3, 'ACGT'::bioseq.seq('dna4', 64));");
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_empty'::regclass;") == 3);
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_empty'::regclass;") == 2);
}

TEST_CASE("ordinary drops fail closed on missing or malformed source catalogs",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table_a = "source_drop_contract_probe_a";
    constexpr std::string_view table_b = "source_drop_contract_probe_b";
    auto ctx = fresh_context({table_a, table_b});
    require_bioseq_shared_preload(ctx);
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table_a);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES (1, {});",
        table_a, drop_preservation_probe_literal());
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table_b);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES (1, {});",
        table_b, drop_preservation_probe_literal());
    const long long table_a_oid = query_scalar_int(ctx, fmt::format(
        "SELECT '{}'::regclass::oid::bigint;", table_a));
    const long long table_b_oid = query_scalar_int(ctx, fmt::format(
        "SELECT '{}'::regclass::oid::bigint;", table_b));

    // A rejected RESTRICT drop exercises the ProcessUtility PG_CATCH path.
    // The following missing-catalog probe must still fail closed, confirming the
    // teardown context did not leak in this same backend.
    const auto restrict_error = sql_error(ctx, "DROP EXTENSION bioseq;");
    REQUIRE(restrict_error.sqlstate == "2BP01");

    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "ALTER TABLE bioseq_internal.sequence_source_version "
        "RENAME TO sequence_source_version_missing_probe;");
    const auto missing_error = sql_error(ctx, fmt::format(
        "DROP TABLE IF EXISTS source_drop_contract_absent, {}, {};",
        table_a, table_b));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(missing_error.sqlstate == "55000");
    REQUIRE(missing_error.contains("missing outside extension teardown"));
    require_preserved_ready_bioseq_source(ctx, table_a, table_a_oid);
    require_preserved_ready_bioseq_source(ctx, table_b, table_b_oid);

    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "ALTER TABLE bioseq_internal.seq_sub_table "
        "RENAME COLUMN sub_table_relid TO sub_table_relid_bad;");
    const auto schema_error = sql_error(ctx, fmt::format(
        "DROP TABLE {}, {};", table_a, table_b));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(schema_error.sqlstate == "55000");
    REQUIRE(schema_error.contains("fragment registry catalog contract changed"));
    require_preserved_ready_bioseq_source(ctx, table_a, table_a_oid);
    require_preserved_ready_bioseq_source(ctx, table_b, table_b_oid);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid IN ('source_drop_contract_probe_a'::regclass, "
        "'source_drop_contract_probe_b'::regclass) AND state=1;") == 2);
}

TEST_CASE("ordinary DROP preflights an implicit bioseq inheritance child",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view parent = "source_drop_inheritance_parent";
    constexpr std::string_view child = "source_drop_inheritance_child";
    auto ctx = fresh_context({child, parent});
    require_bioseq_shared_preload(ctx);

    // The named parent uses heap; only its unmentioned inheritance child uses
    // bioseq. A name-only preflight therefore misses the catalog dependency.
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64));",
        parent);
    exec_sqlf(ctx,
        "CREATE TABLE {} () INHERITS ({}) USING bioseq;",
        child, parent);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES (1, {});",
        child, drop_preservation_probe_literal());
    const long long parent_oid = query_scalar_int(ctx, fmt::format(
        "SELECT '{}'::regclass::oid::bigint;", parent));
    const long long child_oid = query_scalar_int(ctx, fmt::format(
        "SELECT '{}'::regclass::oid::bigint;", child));

    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "ALTER TABLE bioseq_internal.sequence_source_version "
        "RENAME TO sequence_source_version_inheritance_probe;");
    const auto closure_error = sql_error(ctx, fmt::format(
        "DROP TABLE {} CASCADE;", parent));
    exec_sql(ctx, "ROLLBACK;");

    REQUIRE(closure_error.sqlstate == "55000");
    REQUIRE(closure_error.contains("missing outside extension teardown"));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('{}')::oid::bigint;", parent)) == parent_oid);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('{}')::oid::bigint;", child)) == child_oid);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM pg_inherits WHERE inhparent={}::oid "
        "AND inhrelid={}::oid;", parent_oid, child_oid)) == 1);
    require_preserved_ready_bioseq_source(ctx, child, child_oid);
}

TEST_CASE("ordinary DROP preflights an implicit bioseq partition",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view parent = "source_drop_partition_parent";
    constexpr std::string_view child = "source_drop_partition_child";
    auto ctx = fresh_context({child, parent});
    require_bioseq_shared_preload(ctx);

    // A declarative partition parent has no physical table AM of its own. The
    // unmentioned child is the bioseq relation whose cleanup catalogs must be
    // validated before PostgreSQL enters dependency deletion.
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) "
        "PARTITION BY RANGE (id);", parent);
    exec_sqlf(ctx,
        "CREATE TABLE {} PARTITION OF {} FOR VALUES FROM (0) TO (10) "
        "USING bioseq;", child, parent);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES (1, {});",
        parent, drop_preservation_probe_literal());
    const long long parent_oid = query_scalar_int(ctx, fmt::format(
        "SELECT '{}'::regclass::oid::bigint;", parent));
    const long long child_oid = query_scalar_int(ctx, fmt::format(
        "SELECT '{}'::regclass::oid::bigint;", child));

    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "ALTER TABLE bioseq_internal.sequence_source_version "
        "RENAME TO sequence_source_version_partition_probe;");
    const auto closure_error = sql_error(ctx, fmt::format(
        "DROP TABLE {} CASCADE;", parent));
    exec_sql(ctx, "ROLLBACK;");

    REQUIRE(closure_error.sqlstate == "55000");
    REQUIRE(closure_error.contains("missing outside extension teardown"));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('{}')::oid::bigint;", parent)) == parent_oid);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('{}')::oid::bigint;", child)) == child_oid);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM pg_inherits WHERE inhparent={}::oid "
        "AND inhrelid={}::oid;", parent_oid, child_oid)) == 1);
    require_preserved_ready_bioseq_source(ctx, child, child_oid);
}

TEST_CASE("named partition DROP waits on its parent before locking the child",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view parent = "source_drop_lock_parent";
    constexpr std::string_view child = "source_drop_lock_child";
    auto setup_ctx = fresh_context({child, parent});
    require_bioseq_shared_preload(setup_ctx);
    exec_sqlf(setup_ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) "
        "PARTITION BY RANGE (id);", parent);
    exec_sqlf(setup_ctx,
        "CREATE TABLE {} PARTITION OF {} FOR VALUES FROM (0) TO (10) "
        "USING bioseq;", child, parent);
    exec_sqlf(setup_ctx,
        "INSERT INTO {} VALUES (1, 'ACGTACGT'::bioseq.seq('dna4', 64));",
        child);
    const auto relation_oids = query_one(setup_ctx, fmt::format(
        "SELECT '{}'::regclass::oid::text, '{}'::regclass::oid::text;",
        parent, child));
    REQUIRE(relation_oids.size() == 2);

    connection_test_context blocker_ctx;
    connection_test_context drop_ctx;
    connection_test_context observer_ctx;
    exec_sql(blocker_ctx, "BEGIN;");
    exec_sqlf(blocker_ctx,
        "LOCK TABLE ONLY {} IN ACCESS SHARE MODE;", parent);
    const long long drop_pid = query_scalar_int(drop_ctx,
        "SELECT pg_backend_pid();");
    const std::string drop_sql = fmt::format("DROP TABLE {};", child);
    REQUIRE(PQsendQuery(drop_ctx.raw(), drop_sql.c_str()) == 1);

    bool parent_wait_observed = false;
    bool child_locked_while_parent_waited = false;
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        const auto locks = query_one(observer_ctx, fmt::format(
            "SELECT count(*) FILTER (WHERE relation={}::oid "
            "AND mode='AccessExclusiveLock' AND NOT granted)::text, "
            "count(*) FILTER (WHERE relation={}::oid "
            "AND mode='AccessExclusiveLock' AND granted)::text "
            "FROM pg_locks WHERE pid={};",
            relation_oids[0].value(), relation_oids[1].value(), drop_pid));
        if (std::stoll(locks[0].value()) > 0) {
            parent_wait_observed = true;
            child_locked_while_parent_waited =
                std::stoll(locks[1].value()) > 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Always release the blocker and drain the asynchronous result before any
    // assertion can unwind a connection with a live DROP command.
    exec_sql(blocker_ctx, "ROLLBACK;");
    bool consume_ok = true;
    for (unsigned attempt = 0; attempt < 500 && PQisBusy(drop_ctx.raw());
         ++attempt) {
        if (PQconsumeInput(drop_ctx.raw()) == 0) {
            consume_ok = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool got_result = false;
    const bool result_ready = PQisBusy(drop_ctx.raw()) == 0;
    bool drop_succeeded = consume_ok && result_ready;
    if (result_ready) {
        while (PGresult *result = PQgetResult(drop_ctx.raw())) {
            got_result = true;
            drop_succeeded = drop_succeeded
                && PQresultStatus(result) == PGRES_COMMAND_OK;
            PQclear(result);
        }
    }

    REQUIRE(parent_wait_observed);
    REQUIRE_FALSE(child_locked_while_parent_waited);
    REQUIRE(consume_ok);
    REQUIRE(got_result);
    REQUIRE(drop_succeeded);
    REQUIRE(query_scalar_int(setup_ctx, fmt::format(
        "SELECT count(*) FROM pg_class WHERE oid={}::oid;",
        relation_oids[0].value())) == 1);
    REQUIRE(query_scalar_int(setup_ctx, fmt::format(
        "SELECT count(*) FROM pg_class WHERE oid={}::oid;",
        relation_oids[1].value())) == 0);
}

TEST_CASE("speculative source authority is consumed or never created",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_source_speculative"});
    exec_sql(ctx,
        "CREATE TABLE index_admin_source_speculative "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO index_admin_source_speculative VALUES "
        "(1, 'ACGT'::bioseq.seq('dna4', 64));");
    exec_sql(ctx,
        "INSERT INTO index_admin_source_speculative VALUES "
        "(1, 'TGCA'::bioseq.seq('dna4', 64)) ON CONFLICT (id) DO NOTHING;");
    const auto states = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM index_admin_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_speculative'::regclass;");
    REQUIRE(states[0].value() == "0");
    REQUIRE(states[1].value() == "1");
    // PostgreSQL's ON CONFLICT pre-check may determine the conflict before calling
    // table_tuple_insert_speculative.  In that valid path no source epoch or
    // publication authority is created, so there is nothing to invalidate.
    REQUIRE(states[2].value() == "0");
    REQUIRE(states[3].value() == "1");

    // No-conflict ON CONFLICT takes the successful speculative-completion path.
    // Its move-only authority must be published before PRE_COMMIT.
    exec_sql(ctx,
        "INSERT INTO index_admin_source_speculative VALUES "
        "(2, 'CCCC'::bioseq.seq('dna4', 64)) ON CONFLICT (id) DO NOTHING;");
    const auto succeeded = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM index_admin_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_speculative'::regclass;");
    REQUIRE(succeeded[0].value() == "0");
    REQUIRE(succeeded[1].value() == "2");
    REQUIRE(succeeded[2].value() == "0");
    REQUIRE(succeeded[3].value() == "2");

    // A subtransaction rollback must remove its catalog row and leave no
    // backend-local speculative authority for the outer PRE_COMMIT gate.
    exec_sql(ctx, "BEGIN; SAVEPOINT source_speculative;");
    exec_sql(ctx,
        "INSERT INTO index_admin_source_speculative VALUES "
        "(3, 'GGGG'::bioseq.seq('dna4', 64)) ON CONFLICT (id) DO NOTHING;");
    const auto inside_rollback_savepoint = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM index_admin_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_speculative'::regclass;");
    REQUIRE(inside_rollback_savepoint[0].value() == "0");
    REQUIRE(inside_rollback_savepoint[1].value() == "3");
    REQUIRE(inside_rollback_savepoint[2].value() == "0");
    REQUIRE(inside_rollback_savepoint[3].value() == "3");
    exec_sql(ctx, "ROLLBACK TO SAVEPOINT source_speculative; COMMIT;");
    const auto after_subtransaction = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM index_admin_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_speculative'::regclass;");
    REQUIRE(after_subtransaction[0].value() == "0");
    REQUIRE(after_subtransaction[1].value() == "2");
    REQUIRE(after_subtransaction[2] == succeeded[2]);
    REQUIRE(after_subtransaction[3].value() == "2");

    // The symmetric savepoint-release path runs PRE_COMMIT_SUB rather than
    // ABORT_SUB. Both callbacks close transaction-scoped write-side shard
    // caches, so verify that a published source and its parent row survive release
    // and outer commit without a ResourceOwner mismatch.
    exec_sql(ctx, "BEGIN; SAVEPOINT source_speculative_commit;");
    exec_sql(ctx,
        "INSERT INTO index_admin_source_speculative VALUES "
        "(4, 'TTTT'::bioseq.seq('dna4', 64)) ON CONFLICT (id) DO NOTHING;");
    const auto inside_commit_savepoint = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM index_admin_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_speculative'::regclass;");
    REQUIRE(inside_commit_savepoint[0].value() == "0");
    REQUIRE(inside_commit_savepoint[1].value() == "3");
    REQUIRE(inside_commit_savepoint[2].value() == "0");
    REQUIRE(inside_commit_savepoint[3].value() == "3");
    exec_sql(ctx, "RELEASE SAVEPOINT source_speculative_commit; COMMIT;");
    const auto after_commit_savepoint = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM index_admin_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_speculative'::regclass;");
    REQUIRE(after_commit_savepoint[0].value() == "0");
    REQUIRE(after_commit_savepoint[1].value() == "3");
    REQUIRE(after_commit_savepoint[2].value() == "0");
    REQUIRE(after_commit_savepoint[3].value() == "3");
}

TEST_CASE("cold no-index audit is metadata-only and detects physical minimizer relations",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_no_index_absence"});
    exec_sql(ctx,
        "CREATE TABLE index_admin_no_index_absence "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off; "
        "INSERT INTO index_admin_no_index_absence VALUES "
        "(1, repeat('ACGT', 4096)::bioseq.seq('dna4', 64));");

    const auto absent = query_one(ctx,
        "SELECT target_rows::text, is_target_cardinality_metadata_only::int::text, "
        "source_parent_rows_read::text, source_fragment_rows_read::text, "
        "source_decoded_bp::text, physical_index_relations::text, "
        "persisted_index_rows::text, generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'index_admin_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(absent.size() == 8);
    REQUIRE(absent[0].value() == "1");
    REQUIRE(absent[1].value() == "1");
    REQUIRE(absent[2].value() == "0");
    REQUIRE(absent[3].value() == "0");
    REQUIRE(absent[4].value() == "0");
    REQUIRE(absent[5].value() == "0");
    REQUIRE(absent[6].value() == "0");
    REQUIRE(absent[7].value() == "0");

    const auto before_sidecars = query_rows(ctx,
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' AND c.relname LIKE 'seqidx\\_minimizer\\_fragment\\_%' "
        "ESCAPE '\\' ORDER BY c.relname;");
    exec_sql(ctx,
        "SELECT bioseq.update_index('index_admin_no_index_absence'::regclass, "
        "'minimizers_per_fragment');");
    const auto present = query_one(ctx,
        "SELECT physical_index_relations::text, persisted_index_rows::text, "
        "generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'index_admin_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(present.size() == 3);
    REQUIRE(std::stoll(present[0].value()) > 0);
    REQUIRE(std::stoll(present[1].value()) > 0);
    // The default fragment writer does not publish a generation head;
    // the audit must count its physical rows without inventing one.
    REQUIRE(present[2].value() == "0");

    const auto after_sidecars = query_rows(ctx,
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' AND c.relname LIKE 'seqidx\\_minimizer\\_fragment\\_%' "
        "ESCAPE '\\' ORDER BY c.relname;");
    std::vector<std::string> prior_names;
    for (const auto& row : before_sidecars) {
        prior_names.push_back(row[0].value());
    }
    std::size_t truncated_relations = 0;
    for (const auto& row : after_sidecars) {
        const std::string& name = row[0].value();
        if (std::ranges::find(prior_names, name) != prior_names.end()) continue;
        REQUIRE(std::ranges::all_of(name, [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '_';
        }));
        exec_sqlf(ctx, "TRUNCATE bioseq_internal.{};", name);
        ++truncated_relations;
    }
    REQUIRE(truncated_relations > 0);
    const auto empty_but_present = query_one(ctx,
        "SELECT physical_index_relations::text, persisted_index_rows::text, "
        "generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'index_admin_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(std::stoll(empty_but_present[0].value()) > 0);
    REQUIRE(empty_but_present[1].value() == "0");
    REQUIRE(empty_but_present[2].value() == "0");

    const auto unsupported = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_cold_no_index_absence("
        "'index_admin_no_index_absence'::regclass, 'symbol_counts');");
    REQUIRE(unsupported.sqlstate == "55000");
    REQUIRE(unsupported.contains(
        "minimizers_per_fragment or minimizers_per_sequence"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("sequence-source invalidation is terminal across parent update and delete",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_source_terminal"});
    exec_sql(ctx,
        "CREATE TABLE index_admin_source_terminal "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled=off; "
        "INSERT INTO index_admin_source_terminal VALUES "
        "(1, 'ACGTACGT'::bioseq.seq('dna4', 64));");
    const auto first_epoch = query_one(ctx,
        "SELECT source_epoch::text FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_terminal'::regclass AND state=1;");
    REQUIRE(first_epoch.size() == 1);

    exec_sql(ctx,
        "UPDATE index_admin_source_terminal SET "
        "seq='TGCATGCA'::bioseq.seq('dna4', 64) WHERE id=1;");
    const auto after_update = query_one(ctx, fmt::format(
        "SELECT count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "max(state) FILTER (WHERE source_epoch={})::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_terminal'::regclass;",
        first_epoch[0].value()));
    REQUIRE(after_update[0].value() == "1");
    REQUIRE(after_update[1].value() == "1");
    REQUIRE(after_update[2].value() == "2");

    exec_sql(ctx, "DELETE FROM index_admin_source_terminal WHERE id=1;");
    const auto after_delete = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='index_admin_source_terminal'::regclass;");
    REQUIRE(after_delete[0].value() == "0");
    REQUIRE(after_delete[1].value() == "2");
}

TEST_CASE("published generation inspection rejects invalid inputs before access",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_inspect_invalid", "index_admin_inspect_multi"});
    exec_sql(ctx,
        "CREATE TABLE index_admin_inspect_invalid "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO index_admin_inspect_invalid VALUES "
        "(1, repeat('ACGT', 4096)::bioseq.seq('dna4', 64));");

    auto err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_invalid'::regclass, 'symbol_counts', 0);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("requires a minimizer index"));

    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_invalid'::regclass, "
        "'minimizers_per_fragment', -1);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("query key is negative"));

    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_invalid'::regclass, "
        "'minimizers_per_fragment', 99);");
    REQUIRE(err.sqlstate == "P0002");
    REQUIRE(err.contains("source row is not visible"));

    exec_sql(ctx,
        "CREATE TABLE index_admin_inspect_multi "
        "(id int, a bioseq.seq('dna4', 64), b bioseq.seq('dna4', 64)) "
        "USING bioseq;");
    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_multi'::regclass, "
        "'minimizers_per_fragment', 0);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("exactly one bioseq.seq column"));

    exec_sql(ctx, "CREATE TABLE index_admin_inspect_heap(id int);");
    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'index_admin_inspect_heap'::regclass, 'minimizers_per_fragment', 0);");
    REQUIRE(err.sqlstate == "42809");
    REQUIRE(err.contains("requires a USING bioseq table"));
    exec_sql(ctx, "DROP TABLE index_admin_inspect_heap;");

    // Every ERROR path must release its registered snapshot/read state and leave
    // the libpq session usable.
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("generation clear savepoint restores relation head and exact age row",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_generation_clear_rollback";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    const auto built = query_one(ctx, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'scalar');",
        table, rowno));
    REQUIRE(built.size() == 45);
    const std::string sidecar = built[0].value();
    const std::size_t rollback_shard_separator = sidecar.rfind('_');
    REQUIRE(rollback_shard_separator != std::string::npos);
    const std::size_t rollback_bucket_separator = sidecar.rfind(
        '_', rollback_shard_separator - 1);
    REQUIRE(rollback_bucket_separator != std::string::npos);
    const std::string rollback_prefix = sidecar.substr(
        0, rollback_bucket_separator + 1);
    const std::string rollback_lookalike =
        rollback_prefix + "255_4294967295";
    exec_sqlf(ctx,
        "CREATE TABLE bioseq_internal.{} ("
        "query_key bigint NOT NULL, generation_id bigint NOT NULL, "
        "position_segment integer NOT NULL, sub_key bigint NOT NULL, "
        "chunk_no integer NOT NULL, chunk_bytes integer NOT NULL, "
        "checksum bigint NOT NULL, blob bytea NOT NULL, "
        "PRIMARY KEY "
        "(query_key,generation_id,position_segment,sub_key,chunk_no)) "
        "USING heap;",
        rollback_lookalike);
    exec_sqlf(ctx,
        "ALTER TABLE bioseq_internal.{} "
        "ALTER COLUMN blob SET STORAGE PLAIN;",
        rollback_lookalike);
    const long long rollback_lookalike_oid = query_scalar_int(ctx,
        fmt::format(
            "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
            rollback_lookalike));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' "
        "AND left(c.relname::text, length('{}'))='{}';",
        rollback_prefix, rollback_prefix)) == 2);

    exec_sqlf(ctx,
        "INSERT INTO bioseq_internal.metadata_age_log "
        "(user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "SELECT '{}'::regclass, pg_relation_filenode('{}'::regclass), "
        "(a.attnum + v.attno_delta)::smallint, "
        "v.index_name, v.last_updated "
        "FROM pg_attribute a CROSS JOIN (VALUES "
        "('minimizers_per_fragment'::text, "
        " '2026-07-12 10:11:12.123456+00'::timestamptz, 0::smallint), "
        "('minimizers_per_sequence'::text, "
        " '2026-07-12 10:11:13.123456+00'::timestamptz, 0::smallint), "
        "('minimizers_per_fragment'::text, "
        " '2026-07-12 10:11:14.123456+00'::timestamptz, 1::smallint)) "
        "AS v(index_name,last_updated,attno_delta) "
        "WHERE a.attrelid='{}'::regclass "
        "AND a.attname='seq' AND NOT a.attisdropped "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) "
        "DO UPDATE SET last_updated=EXCLUDED.last_updated;",
        table, table, table);

    const auto warmed = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'warm_generation_residency');",
        table, rowno));
    REQUIRE(warmed.size() == 7);
    REQUIRE(warmed[0].has_value());
    REQUIRE(warmed[2] == warmed[0]);
    REQUIRE(warmed[4].value() == "t");
    REQUIRE(warmed[5].value() == "t");
    REQUIRE(warmed[6].value() == "t");

    const rowset_t head_before = query_rows(ctx, fmt::format(
        "SELECT generation_id::text, position_segment::text, "
        "sub_key::text, chunk_no::text, chunk_bytes::text, "
        "checksum::text, encode(blob, 'hex') "
        "FROM bioseq_internal.{} WHERE query_key={} "
        "AND generation_id=0 ORDER BY position_segment, sub_key, chunk_no;",
        sidecar, rowno));
    REQUIRE(head_before.size() == 1);
    const rowset_t age_before = query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table));
    // The three explicit rows cover the exact per-fragment age plus the
    // per-sequence and hostile adjacent-attno per-fragment collateral. Their rollback image must be preserved
    // byte-for-byte below.
    REQUIRE(age_before.size() == 3);

    const auto cleared = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'clear_generation_savepoint_rollback');",
        table, rowno));
    REQUIRE(cleared.size() == 7);
    REQUIRE(cleared[0] == warmed[0]);
    REQUIRE(cleared[1] == warmed[0]);
    REQUIRE(cleared[2] == warmed[0]);
    REQUIRE(cleared[3].value() == "t");
    REQUIRE(cleared[4].value() == "t");
    REQUIRE(cleared[5].value() == "f");
    REQUIRE(cleared[6].value() == "f");
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;", sidecar)) ==
        std::stoll(warmed[0].value()));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
        rollback_lookalike)) == rollback_lookalike_oid);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' "
        "AND left(c.relname::text, length('{}'))='{}';",
        rollback_prefix, rollback_prefix)) == 2);
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT generation_id::text, position_segment::text, "
        "sub_key::text, chunk_no::text, chunk_bytes::text, "
        "checksum::text, encode(blob, 'hex') "
        "FROM bioseq_internal.{} WHERE query_key={} "
        "AND generation_id=0 ORDER BY position_segment, sub_key, chunk_no;",
        sidecar, rowno)) == head_before);
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table)) == age_before);
    exec_sqlf(ctx, "DROP TABLE bioseq_internal.{};", rollback_lookalike);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("committed generation clear drops every sidecar and evicts another backend",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_generation_clear_commit";
    auto setup = fresh_context({table});
    exec_sql(setup, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(setup,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(setup,
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT', 8198)::bioseq.seq('dna4', 64)), "
        "(2, (repeat('TGCA', 16391) || 'T')::bioseq.seq('dna4', 64));",
        table);
    const auto rows = query_rows(setup, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
        "FROM {} ORDER BY id;", table, table));
    REQUIRE(rows.size() == 2);
    const long long rowno_a = std::stoll(rows[0][0].value());
    const long long rowno_b = std::stoll(rows[1][0].value());
    const auto built_a = query_one(setup, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'scalar');",
        table, rowno_a));
    const auto built_b = query_one(setup, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'scalar');",
        table, rowno_b));
    REQUIRE(built_a.size() == 45);
    REQUIRE(built_b.size() == 45);
    const std::string sidecar_a = built_a[0].value();
    const std::string sidecar_b = built_b[0].value();
    REQUIRE(sidecar_a != sidecar_b);
    const std::size_t shard_separator = sidecar_a.rfind('_');
    REQUIRE(shard_separator != std::string::npos);
    const std::size_t bucket_separator = sidecar_a.rfind(
        '_', shard_separator - 1);
    REQUIRE(bucket_separator != std::string::npos);
    const std::string exact_prefix = sidecar_a.substr(
        0, bucket_separator + 1);
    REQUIRE(sidecar_b.starts_with(exact_prefix));
    const std::string commit_lookalike =
        exact_prefix + "255_4294967295";
    exec_sqlf(setup,
        "CREATE TABLE bioseq_internal.{} ("
        "query_key bigint NOT NULL, generation_id bigint NOT NULL, "
        "position_segment integer NOT NULL, sub_key bigint NOT NULL, "
        "chunk_no integer NOT NULL, chunk_bytes integer NOT NULL, "
        "checksum bigint NOT NULL, blob bytea NOT NULL, "
        "PRIMARY KEY "
        "(query_key,generation_id,position_segment,sub_key,chunk_no)) "
        "USING heap;",
        commit_lookalike);
    exec_sqlf(setup,
        "ALTER TABLE bioseq_internal.{} "
        "ALTER COLUMN blob SET STORAGE PLAIN;",
        commit_lookalike);
    const long long commit_lookalike_oid = query_scalar_int(setup,
        fmt::format(
            "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
            commit_lookalike));
    REQUIRE(query_scalar_int(setup, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind IN ('r','p') "
        "AND left(c.relname::text, length('{}'))='{}';",
        exact_prefix, exact_prefix)) == 3);

    exec_sqlf(setup,
        "INSERT INTO bioseq_internal.metadata_age_log "
        "(user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "SELECT '{}'::regclass, pg_relation_filenode('{}'::regclass), "
        "(a.attnum + v.attno_delta)::smallint, "
        "v.index_name, v.last_updated "
        "FROM pg_attribute a CROSS JOIN (VALUES "
        "('minimizers_per_fragment'::text, "
        " '2026-07-12 11:12:13.654321+00'::timestamptz, 0::smallint), "
        "('minimizers_per_sequence'::text, "
        " '2026-07-12 11:12:14.654321+00'::timestamptz, 0::smallint), "
        "('minimizers_per_fragment'::text, "
        " '2026-07-12 11:12:15.654321+00'::timestamptz, 1::smallint)) "
        "AS v(index_name,last_updated,attno_delta) "
        "WHERE a.attrelid='{}'::regclass "
        "AND a.attname='seq' AND NOT a.attisdropped "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) "
        "DO UPDATE SET last_updated=EXCLUDED.last_updated;",
        table, table, table);

    const rowset_t collateral_age_before = query_rows(setup, fmt::format(
        "SELECT main_relfilenode::text, column_attno::text, "
        "index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log m "
        "WHERE m.user_relid='{}'::regclass AND ("
        "m.index_name<>'minimizers_per_fragment' OR "
        "m.column_attno<>(SELECT attnum FROM pg_attribute "
        "WHERE attrelid='{}'::regclass AND attname='seq' "
        "AND NOT attisdropped)) "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table, table));
    // The exact per-sequence row and hostile adjacent-attno per-fragment row are collateral.
    REQUIRE(collateral_age_before.size() == 2);

    connection_test_context session_a;
    connection_test_context session_b;
    const long long session_a_pid = query_scalar_int(
        session_a, "SELECT pg_backend_pid();");
    REQUIRE(session_a_pid != query_scalar_int(
        session_b, "SELECT pg_backend_pid();"));
    const auto warmed = query_one(session_a, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'warm_generation_residency');",
        table, rowno_a));
    REQUIRE(warmed.size() == 7);
    REQUIRE(warmed[4].value() == "t");
    REQUIRE(warmed[5].value() == "t");
    REQUIRE(warmed[6].value() == "t");

    exec_sql(session_b, "BEGIN;");
    const auto cleared = query_one(session_b, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'clear_generation');",
        table, rowno_a));
    exec_sql(session_b, "COMMIT;");
    REQUIRE(cleared.size() == 7);
    REQUIRE(cleared[0].has_value());
    REQUIRE(cleared[1] == cleared[0]);
    REQUIRE_FALSE(cleared[2].has_value());
    REQUIRE_FALSE(cleared[3].has_value());
    REQUIRE(cleared[4].value() == "f");
    REQUIRE(cleared[5].value() == "f");
    REQUIRE(cleared[6].value() == "f");

    const auto probed = query_one(session_a, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'probe_residency');",
        table, rowno_a));
    REQUIRE(probed.size() == 7);
    REQUIRE_FALSE(probed[0].has_value());
    REQUIRE_FALSE(probed[1].has_value());
    REQUIRE_FALSE(probed[2].has_value());
    REQUIRE_FALSE(probed[3].has_value());
    REQUIRE(probed[4].value() == "f");
    REQUIRE(probed[5].value() == "f");
    REQUIRE(probed[6].value() == "f");
    REQUIRE(query_scalar_int(session_a, "SELECT pg_backend_pid();") ==
        session_a_pid);

    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT count(*) FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind IN ('r','p') "
        "AND left(c.relname::text, length('{}'))='{}';",
        exact_prefix, exact_prefix)) == 1);
    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
        commit_lookalike)) == commit_lookalike_oid);
    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT count(*) FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "AND index_name='minimizers_per_fragment' "
        "AND column_attno=(SELECT attnum FROM pg_attribute "
        "WHERE attrelid='{}'::regclass AND attname='seq' "
        "AND NOT attisdropped);", table, table)) == 0);
    REQUIRE(query_rows(session_b, fmt::format(
        "SELECT main_relfilenode::text, column_attno::text, "
        "index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log m "
        "WHERE m.user_relid='{}'::regclass AND ("
        "m.index_name<>'minimizers_per_fragment' OR "
        "m.column_attno<>(SELECT attnum FROM pg_attribute "
        "WHERE attrelid='{}'::regclass AND attname='seq' "
        "AND NOT attisdropped)) "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table, table)) == collateral_age_before);
    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT count(*) FROM pg_class WHERE oid IN ("
        "to_regclass('bioseq_internal.{}'), "
        "to_regclass('bioseq_internal.{}'));",
        sidecar_a, sidecar_b)) == 0);

    const auto absent = query_one(session_b, fmt::format(
        "SELECT * FROM bioseq_internal.inspect_generation_storage_transition("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'clear_generation_absent');",
        table, rowno_a));
    REQUIRE(absent.size() == 7);
    REQUIRE_FALSE(absent[0].has_value());
    REQUIRE_FALSE(absent[1].has_value());
    REQUIRE_FALSE(absent[2].has_value());
    REQUIRE_FALSE(absent[3].has_value());
    REQUIRE(absent[4].value() == "f");
    REQUIRE(absent[5].value() == "f");
    REQUIRE(absent[6].value() == "f");
    REQUIRE(query_scalar_int(session_b, fmt::format(
        "SELECT to_regclass('bioseq_internal.{}')::oid::bigint;",
        commit_lookalike)) == commit_lookalike_oid);
    REQUIRE(query_rows(session_b, fmt::format(
        "SELECT main_relfilenode::text, column_attno::text, "
        "index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log m "
        "WHERE m.user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table)).size() == 2); // exact-attno per-sequence + adjacent-attno per-fragment
    exec_sqlf(session_b,
        "DROP TABLE bioseq_internal.{};", commit_lookalike);
    REQUIRE(query_scalar_int(session_b, "SELECT 1;") == 1);
}

TEST_CASE("fragment-minimizer coordinator executes scalar and SIMD PostgreSQL scanners",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_fragment_minimizer_coordinator";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    const rowset_t metadata_age_before = query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "AND index_name='minimizers_per_fragment' "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table));

    constexpr std::array<std::string_view, 3> backends{
        "scalar", "exact_simd", "bounded_nthash_simd"};
    std::array<row_t, backends.size()> observations{};
    long long previous_generation = 0;
    for (std::size_t backend_index = 0;
         backend_index < backends.size(); ++backend_index) {
        const std::string_view backend = backends[backend_index];
        row_t& observed = observations[backend_index];
        observed = query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_fragment_generation("
            "'{}'::regclass, 'minimizers_per_fragment', {}, '{}');",
            table, rowno, backend));
        REQUIRE(observed.size() == 45);
        REQUIRE(!observed[0]->empty());
        REQUIRE(observed[1].value() == "2");
        REQUIRE(observed[2].value() == std::to_string(rowno));
        REQUIRE(observed[3]->size() == 16);
        REQUIRE(std::stoll(observed[4].value()) >= 3);
        REQUIRE(observed[5].value() == "65565");
        REQUIRE(observed[6].value() == backend);
        REQUIRE(observed[7].value() == "5");
        REQUIRE(observed[8].value() == "t");
        REQUIRE(observed[9].value() == "t");
        const long long generation = std::stoll(observed[10].value());
        REQUIRE(generation > previous_generation);
        previous_generation = generation;
        REQUIRE(observed[11]->size() == 16);
        REQUIRE(observed[12]->size() == 16);
        REQUIRE(observed[13]->size() == 16);
        REQUIRE(observed[14].value() == observed[5].value());
        REQUIRE(observed[15].value() == "15");
        REQUIRE(observed[16].value() == "10");
        REQUIRE(std::stoll(observed[19].value()) > 0);
        REQUIRE(std::stoll(observed[20].value()) > 0);
        REQUIRE(std::stoll(observed[21].value()) > 0);
        REQUIRE(std::stoll(observed[22].value()) > 0);
        REQUIRE(std::stoll(observed[23].value()) > 0);
        REQUIRE(std::stoll(observed[24].value()) > 0);
        REQUIRE(std::stoll(observed[25].value())
            >= std::stoll(observed[24].value()));
        REQUIRE(std::stoll(observed[26].value()) ==
            std::stoll(observed[27].value()) + 3);
        REQUIRE(observed[28].value() == "0");
        REQUIRE(observed[32].value() == "0");
        REQUIRE(observed[33].value() == "t");
        REQUIRE(observed[43].value() == "0");
        REQUIRE(observed[44].value() == "0");

        if (backend == "scalar") {
            REQUIRE(observed[17].value() == "0");
            REQUIRE(observed[18].value() == "0");
            REQUIRE(std::stoll(observed[29].value()) > 0);
            REQUIRE(observed[30].value() == "0");
            REQUIRE(observed[31].value() == "0");
            REQUIRE(observed[34].value() == "f");
            REQUIRE(observed[35].value() == "f");
            REQUIRE(observed[38].value() == "f");
            REQUIRE(observed[40].value() == "f");
        } else if (backend == "exact_simd") {
            REQUIRE(observed[17].value() == "0");
            REQUIRE(observed[18].value() == "0");
            REQUIRE(observed[29].value() == "0");
            REQUIRE(std::stoll(observed[30].value()) > 0);
            REQUIRE(observed[31].value() == "0");
            REQUIRE(observed[34].value() == "t");
            REQUIRE(observed[35].value() == "t");
            REQUIRE(std::stoll(observed[36].value()) > 0);
            REQUIRE(observed[37].value() == "0");
            REQUIRE(observed[38].value() == "f");
        } else {
            REQUIRE(observed[17].value() == "1");
            REQUIRE(observed[18].value() == "1");
            REQUIRE(observed[29].value() == "0");
            REQUIRE(observed[30].value() == "0");
            REQUIRE(std::stoll(observed[31].value()) > 0);
            REQUIRE(observed[34].value() == "f");
            REQUIRE(observed[38].value() == "t");
            REQUIRE(observed[39].value() == "ok");
            REQUIRE(observed[40].value() == "t");
            REQUIRE(std::stoll(observed[41].value()) > 0);
            REQUIRE(observed[42].value() == "0");
        }

        const auto persisted = query_one(ctx, fmt::format(
            "SELECT count(*) FILTER (WHERE generation_id=0)::text, "
            "count(*) FILTER (WHERE generation_id={})::text "
            "FROM bioseq_internal.{} WHERE query_key={};",
            generation, observed[0].value(), rowno));
        REQUIRE(persisted.size() == 2);
        REQUIRE(persisted[0].value() == "1");
        REQUIRE(std::stoll(persisted[1].value()) > 1);
    }

    for (std::size_t identity_column : {3u, 4u, 5u, 11u, 12u}) {
        REQUIRE(observations[1][identity_column]
            == observations[0][identity_column]);
        REQUIRE(observations[2][identity_column]
            == observations[0][identity_column]);
    }
    REQUIRE(observations[2][17] != observations[0][17]);
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "AND index_name='minimizers_per_fragment' "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table)) == metadata_age_before);
    const auto fmgr_error = sql_error(ctx, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_fragment_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'invalid');",
        table, rowno));
    REQUIRE(fmgr_error.contains("backend is invalid"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("PostgreSQL full-generation reader rejects same-scheme producer relabelling",
		  "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
	constexpr std::string_view table =
		"index_admin_same_scheme_producer_rejection";
	auto ctx = fresh_context({table});
	exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
	exec_sqlf(ctx,
		"CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
		table);
	exec_sqlf(ctx,
		"INSERT INTO {} VALUES "
		"(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
		table);
	const auto rowno = only_column_row_number(ctx, table);
	const auto build = [&](std::string_view backend) {
		return query_one(ctx, fmt::format(
			"SELECT * FROM "
			"bioseq_internal.build_ready_minimizer_fragment_generation("
			"'{}'::regclass, 'minimizers_per_fragment', {}, '{}');",
			table, rowno, backend));
	};
	const auto read = [&](std::string_view backend) {
		return fmt::format(
			"SELECT * FROM "
			"bioseq_internal.scan_grouped_soa_minimizer_generation_test("
			"'{}'::regclass, 'minimizers_per_fragment', {}, "
			"15::smallint, 10::smallint, 5::smallint, '{}');",
			table, rowno, backend);
	};
	const auto expect_producer_rejection = [&](std::string_view backend) {
		const auto error = sql_error(ctx, read(backend));
		REQUIRE(error.contains(
			"full-generation backend/source authorization disagrees"));
		REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
	};

	const row_t scalar = build("scalar");
	REQUIRE(scalar[6].value() == "scalar");
	REQUIRE(query_one(ctx, read("scalar")).size() == 16u);
	// Scalar, two-stack ntHash and Vulkan use the same biological minimizer scheme.
	// Exact producer identity is nevertheless mandatory, so neither request may
	// consume a generation produced by Scalar.
	expect_producer_rejection("exact_simd");
	expect_producer_rejection("vulkan_segment");

	const row_t exact = build("exact_simd");
	REQUIRE(exact[6].value() == "exact_simd");
	REQUIRE(query_one(ctx, read("exact_simd")).size() == 16u);
	expect_producer_rejection("scalar");
	expect_producer_rejection("vulkan_segment");
}

TEST_CASE("sequence-minimizer coordinator publishes PostgreSQL spill generations",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_sequence_minimizer_coordinator";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    const rowset_t metadata_age_before = query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table));

    std::vector<std::string_view> backends{
        "scalar", "exact_simd", "bounded_nthash_simd"};
    const bool hardware_vulkan_available = query_scalar(
        ctx,
        "SELECT is_available AND NOT is_software_device "
        "FROM bioseq.vulkan_runtime_status();") == "t";
    if (hardware_vulkan_available) {
        backends.push_back("vulkan_segment");
    }

    std::vector<row_t> observations;
    observations.reserve(backends.size());
    long long previous_generation = 0;
    for (const std::string_view backend : backends) {
        row_t observed = query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_sequence_generation("
            "'{}'::regclass, 'minimizers_per_sequence', {}, '{}');",
            table, rowno, backend));
        REQUIRE(observed.size() == 72);
        REQUIRE(!observed[0]->empty());
        REQUIRE(observed[1].value() == "2");
        REQUIRE(observed[2].value() == std::to_string(rowno));
        REQUIRE(observed[3]->size() == 16);
        REQUIRE(std::stoll(observed[4].value()) >= 3);
        REQUIRE(observed[5].value() == "65565");
        REQUIRE(observed[6].value() == backend);
        REQUIRE(observed[7].value() == "5");
        REQUIRE(observed[8].value() == "t");
        REQUIRE(observed[9].value() == "t");
        const long long generation = std::stoll(observed[10].value());
        REQUIRE(generation > previous_generation);
        previous_generation = generation;
        REQUIRE(observed[11]->size() == 16);
        REQUIRE(observed[12]->size() == 16);
        REQUIRE(observed[13]->size() == 16);
        REQUIRE(observed[14] == observed[5]);
        REQUIRE(observed[15].value() == "15");
        REQUIRE(observed[16].value() == "10");
        if (backend == "bounded_nthash_simd") {
            REQUIRE(observed[17].value() == "1");
            REQUIRE(observed[18].value() == "1");
        } else {
            REQUIRE(observed[17].value() == "0");
            REQUIRE(observed[18].value() == "0");
        }
        REQUIRE(std::stoll(observed[19].value()) > 0);
        REQUIRE(std::stoll(observed[20].value()) > 0);
        REQUIRE(std::stoll(observed[21].value()) > 0);
        REQUIRE(std::stoll(observed[22].value()) > 0);
        REQUIRE(std::stoll(observed[23].value()) > 0);
        REQUIRE(std::stoll(observed[24].value()) > 0);
        REQUIRE(std::stoll(observed[25].value()) > 0);

        const long long spill_runs = std::stoll(observed[26].value());
        const long long spilled_events = std::stoll(observed[27].value());
        const long long spill_mutation = std::stoll(observed[30].value());
        const long long replay_scans = std::stoll(observed[53].value());
        const long long replay_reroutes = std::stoll(observed[55].value());
        const long long replay_final_mutation =
            std::stoll(observed[64].value());
        REQUIRE(spill_runs > 0);
        REQUIRE(spilled_events > 0);
        REQUIRE(observed[25].value() == observed[27].value());
        REQUIRE(std::stoll(observed[28].value()) > 0);
        REQUIRE(std::stoll(observed[29].value()) > 0);
        REQUIRE(observed[31].value() == "1");
        REQUIRE(observed[32].value() == "1");
        REQUIRE(std::stoll(observed[33].value()) > 0);
        REQUIRE(std::stoll(observed[34].value()) > 0);
        REQUIRE(std::stoll(observed[34].value())
            <= std::stoll(observed[33].value()));
        REQUIRE(std::stoll(observed[35].value()) > 0);
        REQUIRE(observed[36].value() == "0");
        REQUIRE(observed[41].value() == "t");

        REQUIRE(replay_scans == replay_reroutes + 3);
        REQUIRE(std::stoll(observed[54].value()) == replay_scans);
        // fragment-minimizer replays the immutable PostgreSQL spill three times: once
        // for planning, once for control emission, and once for position
        // emission. All three scans must complete over the same payload.
        REQUIRE(observed[56].value() == "3");
        REQUIRE(std::stoll(observed[57].value()) == replay_reroutes);
        REQUIRE(observed[58] == observed[57]);
        REQUIRE(observed[59].value() == "0");
        REQUIRE(observed[60].value() == observed[27].value());
        REQUIRE(observed[61].value() == observed[27].value());
        REQUIRE(observed[62].value() == observed[20].value());
        REQUIRE(observed[63].value() == observed[29].value());
        REQUIRE(replay_final_mutation == spill_mutation + replay_reroutes);
        REQUIRE(std::stoll(observed[65].value()) >= 2 * spilled_events);
        REQUIRE(observed[66].value() == observed[27].value());
        REQUIRE(observed[67].value() == observed[20].value());
        REQUIRE(observed[68].value() == observed[63].value());
        REQUIRE(observed[69].value() == observed[64].value());
        REQUIRE(std::stoll(observed[70].value())
            == std::stoll(observed[69].value()) + 1);
        REQUIRE(observed[71].value() == "t");

        REQUIRE(observed[50].value() == "0");
        if (backend == "scalar") {
            REQUIRE(std::stoll(observed[37].value()) > 0);
            REQUIRE(observed[38].value() == "0");
            REQUIRE(observed[39].value() == "0");
            REQUIRE(observed[40].value() == "0");
            REQUIRE(observed[42].value() == "f");
            REQUIRE(observed[46].value() == "f");
            REQUIRE(observed[51].value() == "0");
            REQUIRE(observed[52].value() == "0");
        } else if (backend == "exact_simd") {
            REQUIRE(observed[37].value() == "0");
            REQUIRE(std::stoll(observed[38].value()) > 0);
            REQUIRE(observed[39].value() == "0");
            REQUIRE(observed[40].value() == "0");
            REQUIRE(observed[42].value() == "t");
            REQUIRE(observed[43].value() == "t");
            REQUIRE(std::stoll(observed[44].value()) > 0);
            REQUIRE(observed[45].value() == "0");
            REQUIRE(observed[46].value() == "f");
        } else if (backend == "bounded_nthash_simd") {
            REQUIRE(observed[37].value() == "0");
            REQUIRE(observed[38].value() == "0");
            REQUIRE(std::stoll(observed[39].value()) > 0);
            REQUIRE(observed[40].value() == "0");
            REQUIRE(observed[42].value() == "f");
            REQUIRE(observed[46].value() == "t");
            REQUIRE(observed[47].value() == "ok");
            REQUIRE(observed[48].value() == "t");
            REQUIRE(std::stoll(observed[49].value()) > 0);
        } else {
            REQUIRE(backend == "vulkan_segment");
            REQUIRE(observed[37].value() == "0");
            REQUIRE(observed[38].value() == "0");
            REQUIRE(observed[39].value() == "0");
            REQUIRE(std::stoll(observed[40].value()) > 0);
            REQUIRE(observed[42].value() == "f");
            REQUIRE(observed[46].value() == "f");
            REQUIRE(std::stoll(observed[51].value()) > 0);
            REQUIRE(std::stoll(observed[52].value()) > 0);
        }

        const auto persisted = query_one(ctx, fmt::format(
            "SELECT count(*) FILTER (WHERE generation_id=0)::text, "
            "count(*) FILTER (WHERE generation_id={})::text "
            "FROM bioseq_internal.{} WHERE query_key={};",
            generation, observed[0].value(), rowno));
        REQUIRE(persisted.size() == 2);
        REQUIRE(persisted[0].value() == "1");
        REQUIRE(std::stoll(persisted[1].value()) > 1);
        observations.emplace_back(std::move(observed));
    }

    REQUIRE(observations.size() >= 3);
    for (std::size_t observation = 1;
         observation < observations.size(); ++observation) {
        for (const std::size_t identity_column : {3u, 4u, 5u, 11u, 12u}) {
            REQUIRE(observations[observation][identity_column]
                == observations[0][identity_column]);
        }
    }
    REQUIRE(query_rows(ctx, fmt::format(
        "SELECT user_relid::text, main_relfilenode::text, "
        "column_attno::text, index_name, last_updated::text "
        "FROM bioseq_internal.metadata_age_log "
        "WHERE user_relid='{}'::regclass "
        "ORDER BY main_relfilenode, column_attno, index_name;",
        table)) == metadata_age_before);
    const auto fmgr_error = sql_error(ctx, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.build_ready_minimizer_sequence_generation("
        "'{}'::regclass, 'minimizers_per_sequence', {}, 'invalid');",
        table, rowno));
    REQUIRE(fmgr_error.contains("backend is invalid"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("bridged PostgreSQL build and publication leaves preserve published state",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_sequence_minimizer_pg_recovery";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);

    struct failure_probe {
        std::string_view backend;
        std::string_view sqlstate;
        std::string_view diagnostic;
    };
    constexpr std::array probes{
        failure_probe{"scalar_fail_scanner_interrupt_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 1"},
        failure_probe{"scalar_fail_scanner_interrupt_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 2"},
        failure_probe{"scalar_fail_scanner_row_read_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 3"},
        failure_probe{"scalar_fail_scanner_row_read_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 4"},
        failure_probe{"scalar_fail_scanner_palloc_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 5"},
        failure_probe{"scalar_fail_scanner_palloc_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 6"},
        failure_probe{"scalar_fail_sorted_create_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 1"},
        failure_probe{"scalar_fail_sorted_create_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 2"},
        // The fixture's 65,565 bp row spans three 32-Kbp scanner cores. Its
        // test-only 64-KiB merge-reader budget therefore compacts three input
        // runs into two outputs. Failing output creation #2 confirms output #1
        // completed and its input handles closed before the run store became
        // permanently poisoned; the loop below then confirms fresh-txn recovery.
        failure_probe{"scalar_fail_sorted_compact_create_before_nth2", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 15"},
        failure_probe{"scalar_fail_sorted_write_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 3"},
        failure_probe{"scalar_fail_sorted_write_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 4"},
        failure_probe{"scalar_fail_sorted_read_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 5"},
        failure_probe{"scalar_fail_sorted_read_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 6"},
        failure_probe{"scalar_fail_sorted_seek_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 7"},
        failure_probe{"scalar_fail_sorted_seek_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 8"},
        failure_probe{"scalar_fail_sorted_size_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 9"},
        failure_probe{"scalar_fail_sorted_size_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 10"},
        failure_probe{"scalar_fail_sorted_close_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 11"},
        failure_probe{"scalar_fail_sorted_close_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 12"},
        failure_probe{"scalar_fail_sorted_interrupt_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 13"},
        failure_probe{"scalar_fail_sorted_interrupt_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 14"},
        failure_probe{"scalar_fail_pg_spill", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=minimizer_sequence_spill\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_merge", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=minimizer_sequence_merge\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_tts_create", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_tts_create\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_tuple_form", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_tuple_form\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_tts_store", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_tts_store\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_tts_drop", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_tts_drop\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_multi_insert", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_multi_insert\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_reread", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_reread\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_spi_publish", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_spi_publish\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_writer_close", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_writer_close\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_storage_close", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_storage_close\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_post_publish_reread", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_post_publish_reread\nHINT: retry the failed index build in a fresh transaction"},
        failure_probe{"scalar_fail_pg_reader_close", "58030",
            "injected PostgreSQL generation failure\nDETAIL: failure_point=generation_reader_close\nHINT: retry the failed index build in a fresh transaction"},
    };

    const auto build = [&ctx, rowno](std::string_view backend) {
        return query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_sequence_generation("
            "'index_admin_sequence_minimizer_pg_recovery'::regclass, "
            "'minimizers_per_sequence', {}, '{}');",
            rowno, backend));
    };
    const auto capture_generation_family = [&ctx, rowno](
            const std::string& sidecar) {
        return query_rows(ctx, fmt::format(
            "SELECT generation_id::text, position_segment::text, "
            "sub_key::text, chunk_no::text, chunk_bytes::text, "
            "checksum::text, encode(blob, 'hex') "
            "FROM bioseq_internal.{} WHERE query_key={} "
            "ORDER BY generation_id, position_segment, sub_key, chunk_no;",
            sidecar, rowno));
    };

    row_t baseline_build = build("scalar");
    REQUIRE(baseline_build.size() == 72);
    REQUIRE(baseline_build[6].value() == "scalar");
    REQUIRE(baseline_build[8].value() == "t");
    REQUIRE(baseline_build[9].value() == "t");
    REQUIRE(baseline_build[41].value() == "t");
    REQUIRE(baseline_build[71].value() == "t");
    std::string baseline_sidecar = baseline_build[0].value();
    long long baseline_generation = std::stoll(
        baseline_build[10].value());
    REQUIRE(baseline_generation > 0);
    rowset_t baseline_family = capture_generation_family(baseline_sidecar);
    REQUIRE(!baseline_family.empty());

    for (const failure_probe& probe : probes) {
        const auto error = sql_error(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_sequence_generation("
            "'{}'::regclass, 'minimizers_per_sequence', {}, '{}');",
            table, rowno, probe.backend));
        REQUIRE(error.sqlstate == probe.sqlstate);
        REQUIRE(error.message == probe.diagnostic);
        // Recovery is deliberately a fresh implicit transaction. A poisoned
        // scanner, run store, row sink, rereader, or SPI publisher must never
        // be resumed in-place. Exact equality includes the generation-0 head,
        // all immutable generation rows, and every blob byte; it therefore
        // also confirms the failed statement left no orphan generation rows.
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        REQUIRE(capture_generation_family(baseline_sidecar)
            == baseline_family);
        REQUIRE(query_scalar_int(ctx, fmt::format(
            "SELECT count(*) FROM bioseq_internal.{} "
            "WHERE query_key={} AND generation_id=0;",
            baseline_sidecar, rowno)) == 1);

        // A successful publication after every individual probe confirms the
        // test-only hook was explicitly disarmed and each failure is isolated.
        row_t recovered = build("scalar");
        REQUIRE(recovered.size() == 72);
        REQUIRE(recovered[0].value() == baseline_sidecar);
        REQUIRE(recovered[6].value() == "scalar");
        REQUIRE(recovered[8].value() == "t");
        REQUIRE(recovered[9].value() == "t");
        REQUIRE(recovered[41].value() == "t");
        REQUIRE(recovered[71].value() == "t");
        const long long recovered_generation = std::stoll(
            recovered[10].value());
        REQUIRE(recovered_generation > baseline_generation);
        baseline_generation = recovered_generation;
        baseline_family = capture_generation_family(baseline_sidecar);
        REQUIRE(!baseline_family.empty());
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }
}

TEST_CASE("ordinary update_index publishes fragment-minimizer metadata and a reopenable hash-major generation",
		  "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 2>
        cases{{
            {"index_admin_update_fragment_minimizer", "minimizers_per_fragment"},
            {"index_admin_update_sequence_minimizer", "minimizers_per_sequence"},
        }};
    auto ctx = fresh_context(
        {cases[0].first, cases[1].first});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'scalar';");

    for (const auto& [table, index_name] : cases) {
        INFO(fmt::format("table={} index={}", table, index_name));
        exec_sqlf(ctx,
            "CREATE TABLE {} "
            "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
            table);
        exec_sqlf(ctx,
            "INSERT INTO {} VALUES "
            "(1, (repeat('ACGT', 16391) || 'A')::"
            "bioseq.seq('dna4', 64));",
            table);
        const auto rowno = only_column_row_number(ctx, table);

        REQUIRE(query_scalar_int(ctx, fmt::format(
            "SELECT bioseq.update_index('{}'::regclass, '{}');",
            table, index_name)) == 1);

        // A distinct backend authenticates the published metadata without
        // inheriting writer memory. Fragment-minimizer live mmap execution is
        // covered above; only the sequence-wide hash-major branch below
        // claims a complete hash-major reopen.
        connection_test_context reopened;
        if (index_name == "minimizers_per_fragment") {
            REQUIRE(require_ready_fragment_minimizer_metadata(
                reopened, table, index_name, rowno, 65'565,
                /*expected_position_segment_span_bp=*/32'768,
                /*expected_position_segment_count=*/3) > 0);
        } else {
            REQUIRE(require_ready_hash_major_generation(
                reopened, table, index_name, rowno, 65'565,
                /*expected_scheme_identity=*/0,
                /*expected_scheme_revision=*/0,
                /*expected_producer_identity=*/1) > 0);
        }
        REQUIRE(index_table_row_count(reopened, index_name, table) > 1);
    }
}

TEST_CASE("hash-major core parallel heap scan matches the serial receipt",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table =
        "hash_major_parallel_scan_probe";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'scalar';");
    exec_sqlf(ctx,
        "CREATE TABLE {} "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::"
        "bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
    REQUIRE(require_ready_hash_major_generation(
        ctx, table, "minimizers_per_sequence", rowno, 65'565,
        /*expected_scheme_identity=*/0,
        /*expected_scheme_revision=*/0,
        /*expected_producer_identity=*/1) > 0);

    const long long max_parallel_workers = query_scalar_int(
        ctx,
        "SELECT current_setting('max_parallel_workers')::int;");
    REQUIRE(max_parallel_workers > 0);
    const long long requested_workers =
        std::min<long long>(2, max_parallel_workers);
    const row_t receipt = query_one(ctx, fmt::format(
        "WITH serial AS MATERIALIZED ("
        " SELECT bioseq_internal.hash_major_minimizer_parallel_scan_probe("
        "  '{}'::regclass,'minimizers_per_sequence',{},0) AS j"
        "), parallel AS MATERIALIZED ("
        " SELECT bioseq_internal.hash_major_minimizer_parallel_scan_probe("
        "  '{}'::regclass,'minimizers_per_sequence',{}, {}) AS j"
        ") "
        "SELECT "
        " serial.j->>'schema',"
        " serial.j->>'requested_workers',"
        " parallel.j->>'requested_workers',"
        " parallel.j->>'launched_workers',"
        " (serial.j->>'generation_id'=parallel.j->>'generation_id' "
        "  AND serial.j->>'query_key'=parallel.j->>'query_key' "
        "  AND serial.j->>'expected_rows'=parallel.j->>'expected_rows' "
        "  AND serial.j->>'expected_bytes'=parallel.j->>'expected_bytes' "
        "  AND serial.j->>'scanned_rows'=serial.j->>'expected_rows' "
        "  AND parallel.j->>'scanned_rows'=parallel.j->>'expected_rows' "
        "  AND serial.j->>'scanned_bytes'=serial.j->>'expected_bytes' "
        "  AND parallel.j->>'scanned_bytes'=parallel.j->>'expected_bytes'"
        " )::text,"
        " (serial.j->>'duplicate_rows'='0' "
        "  AND serial.j->>'missing_rows'='0' "
        "  AND serial.j->>'corrupt_rows'='0')::text,"
        " (parallel.j->>'duplicate_rows'='0' "
        "  AND parallel.j->>'missing_rows'='0' "
        "  AND parallel.j->>'corrupt_rows'='0')::text,"
        " (serial.j->>'checksum_digest'="
        "      parallel.j->>'checksum_digest' "
        "  AND serial.j->>'receipt_digest'="
        "      parallel.j->>'receipt_digest')::text,"
        " ((serial.j->>'has_leader_participation')::boolean "
        "  AND (parallel.j->>'has_leader_participation')::boolean)::text,"
        " ((serial.j->>'elapsed_ns')::bigint>0 "
        "  AND (parallel.j->>'elapsed_ns')::bigint>0)::text,"
        " ((serial.j->>'expected_rows')::bigint>0)::text,"
        " ((serial.j->>'expected_bytes')::bigint>0)::text "
        "FROM serial CROSS JOIN parallel;",
        table, rowno, table, rowno, requested_workers));
    REQUIRE(receipt.size() == 12u);
    for (const auto& value : receipt) REQUIRE(value.has_value());
    REQUIRE(receipt[0].value()
        == "bioseq_internal.hash_major_minimizer_parallel_scan_probe");
    REQUIRE(receipt[1].value() == "0");
    REQUIRE(std::stoll(receipt[2].value()) == requested_workers);
    REQUIRE(std::stoll(receipt[3].value()) >= 1);
    for (std::size_t index = 4; index < receipt.size(); ++index) {
        REQUIRE(receipt[index].value() == "true");
    }
}

TEST_CASE("hash-major replacement reuses its sidecar and retains the replaced generation",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "hash_major_generation_replacement";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'scalar';");
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);

    const auto rebuild = [&] {
        REQUIRE(query_scalar_int(ctx, fmt::format(
            "SELECT bioseq.update_index('{}'::regclass, "
            "'minimizers_per_sequence');",
            table)) == 1);
        return require_ready_hash_major_generation(
            ctx, table, "minimizers_per_sequence", rowno, 65'565,
            /*expected_scheme_identity=*/0,
            /*expected_scheme_revision=*/0,
            /*expected_producer_identity=*/1);
    };
    const auto first_generation = rebuild();
    REQUIRE(first_generation > 0);

    const row_t sidecar_identity = query_one(ctx,
        "SELECT c.relname,c.oid::text,c.relfilenode::text,"
        "i.indexrelid::text FROM pg_class c "
        "JOIN pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_index i ON i.indrelid=c.oid AND i.indisprimary "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' "
        "AND c.relname LIKE 'seqidx_minimizer_sequence_%' "
        "AND EXISTS (SELECT 1 FROM pg_attribute a "
        "WHERE a.attrelid=c.oid AND a.attname='generation_id' "
        "AND a.attnum>0 AND NOT a.attisdropped) "
        "ORDER BY c.oid DESC LIMIT 1;");
    REQUIRE(sidecar_identity.size() == 4u);
    for (const auto& value : sidecar_identity) REQUIRE(value.has_value());
    const std::string sidecar = sidecar_identity[0].value();

    const auto second_generation = rebuild();
    REQUIRE(second_generation > first_generation);
    const row_t identity_after = query_one(ctx, fmt::format(
        "SELECT c.oid::text,c.relfilenode::text,i.indexrelid::text "
        "FROM pg_class c JOIN pg_index i "
        "ON i.indrelid=c.oid AND i.indisprimary "
        "WHERE c.oid='bioseq_internal.{}'::regclass;",
        sidecar));
    REQUIRE(identity_after.size() == 3u);
    REQUIRE(identity_after[0] == sidecar_identity[1]);
    REQUIRE(identity_after[1] == sidecar_identity[2]);
    REQUIRE(identity_after[2] == sidecar_identity[3]);

    const row_t generations = query_one(ctx, fmt::format(
        "SELECT count(DISTINCT generation_id)::text,"
        "count(*) FILTER (WHERE generation_id={})::text,"
        "count(*) FILTER (WHERE generation_id={})::text,"
        "count(*) FILTER (WHERE generation_id=0)::text "
        "FROM bioseq_internal.{};",
        first_generation, second_generation, sidecar));
    REQUIRE(generations.size() == 4u);
    REQUIRE(generations[0].value() == "3");
    REQUIRE(std::stoll(generations[1].value()) > 0);
    REQUIRE(std::stoll(generations[2].value()) > 0);
    REQUIRE(generations[3].value() == "1");
}

TEST_CASE("ordinary locator-backed bounded ntHash update publishes hash-major without fallback",
		  "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_bounded_nthash_publication";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'bounded_nthash_simd';");
    exec_sqlf(ctx,
        "CREATE TABLE {} "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::"
        "bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);

    // The production coordinator rejects the publication unless its returned
    // evidence carries accepted bounded-ntHash-native work and zero Scalar/Exact/Vulkan
    // runs, so SQL success is itself no-fallback route evidence.
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
    REQUIRE(require_ready_hash_major_generation(
        ctx, table, "minimizers_per_sequence", rowno, 65'565,
        /*expected_scheme_identity=*/1,
        /*expected_scheme_revision=*/1,
        /*expected_producer_identity=*/3) > 0);
	exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'scalar';");
	const auto wrong_backend = sql_error(ctx, fmt::format(
		"SELECT bioseq.inspect_ready_hash_major_minimizer_generation("
		"'{}'::regclass,'minimizers_per_sequence',{});",
		table, rowno));
	REQUIRE(wrong_backend.contains(
		"bioseq hash-major inspection source/profile identity disagrees"));
	REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
	exec_sql(ctx,
		"SET bioseq.aligner.minimizer.algorithm = 'bounded_nthash_simd';");
	REQUIRE(query_scalar_int(ctx, fmt::format(
		"SELECT count(bioseq.inspect_ready_hash_major_minimizer_generation("
		"'{}'::regclass,'minimizers_per_sequence',{}));",
		table, rowno)) == 1);
}

TEST_CASE("ordinary hash-major replacement preserves its active generation when a locator source is corrupt",
		  "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_update_source_rollback";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sql(ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::"
        "bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
    REQUIRE(require_ready_hash_major_generation(
        ctx, table, "minimizers_per_sequence", rowno, 65'565,
        /*expected_scheme_identity=*/0,
        /*expected_scheme_revision=*/0,
        /*expected_producer_identity=*/1) > 0);
    const rowset_t sidecars = query_rows(ctx,
        "SELECT c.relname, c.oid::text FROM pg_class c "
        "JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' "
        "AND c.relname LIKE 'seqidx_minimizer_sequence_%' "
        "AND EXISTS (SELECT 1 FROM pg_attribute a "
        "WHERE a.attrelid=c.oid AND a.attname='generation_id' "
        "AND a.attnum>0 AND NOT a.attisdropped) "
        "ORDER BY c.relname;");
    REQUIRE(sidecars.size() == 1);
    const std::string sidecar = sidecars[0][0].value();
    const auto capture_generation_rows = [&ctx, &sidecar] {
        return query_rows(ctx, fmt::format(
            "SELECT query_key::text,generation_id::text,"
            "position_segment::text,sub_key::text,chunk_no::text,"
            "chunk_bytes::text,checksum::text,encode(blob,'hex') "
            "FROM bioseq_internal.{} ORDER BY query_key,generation_id,"
            "position_segment,sub_key,chunk_no;",
            sidecar));
    };
    const auto capture_schema_identity = [&ctx, &sidecar] {
        return query_one(ctx, fmt::format(
            "SELECT c.oid::text,c.relfilenode::text,"
            "(SELECT i.indexrelid::text FROM pg_index i "
            "WHERE i.indrelid=c.oid AND i.indisprimary),"
            "string_agg(a.attname || ':' || format_type(a.atttypid,a.atttypmod) "
            "|| ':' || a.attnotnull::int || ':' || a.attstorage::text,',' "
            "ORDER BY a.attnum) FROM pg_class c JOIN pg_attribute a "
            "ON a.attrelid=c.oid WHERE c.oid='bioseq_internal.{}'::regclass "
            "AND a.attnum>0 AND NOT a.attisdropped "
            "GROUP BY c.oid,c.relfilenode;",
            sidecar));
    };
    const auto capture_metadata_age = [&ctx, table] {
        return query_rows(ctx, fmt::format(
            "SELECT user_relid::text,main_relfilenode::text,"
            "column_attno::text,index_name,last_updated::text "
            "FROM bioseq_internal.metadata_age_log "
            "WHERE user_relid='{}'::regclass "
            "ORDER BY main_relfilenode,column_attno,index_name;",
            table));
    };
    const rowset_t generation_rows_before = capture_generation_rows();
    const row_t schema_identity_before = capture_schema_identity();
    const rowset_t metadata_age_before = capture_metadata_age();
    REQUIRE(generation_rows_before.size() > 1);
    for (const row_t& row : generation_rows_before) {
        REQUIRE(row.size() == 8);
        for (const auto& field : row) {
            REQUIRE(field.has_value());
        }
    }
    REQUIRE(schema_identity_before.size() == 4);
    REQUIRE(schema_identity_before[0].has_value());
    REQUIRE(schema_identity_before[1].has_value());
    REQUIRE(schema_identity_before[2].has_value());
    REQUIRE(schema_identity_before[3].has_value());
    REQUIRE(!metadata_age_before.empty());

    // Corrupt a later source fragment after a valid generation exists. The
    // next ordinary rebuild must fail while validating and streaming the authoritative
    // locator, before publishing a replacement head. The stable sidecar and
    // published generation must remain byte-exact.
    exec_sql(ctx, fmt::format(
        "SELECT bioseq_internal.remove_sequence_fragment("
        "'{}'::regclass, 'minimizers_per_sequence', {}, 2);",
        table, rowno));

    const auto error = sql_error(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table));
    // The immutable-source guard owns this failure before the replacement
    // publisher can enter a generation-catalog operation.
    REQUIRE(error.sqlstate == "55000");
    REQUIRE(error.contains(
        "bioseq locator no longer matches its immutable source"));
    REQUIRE(error.contains("source epoch"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    REQUIRE(capture_generation_rows() == generation_rows_before);
    REQUIRE(capture_schema_identity() == schema_identity_before);
    REQUIRE(capture_metadata_age() == metadata_age_before);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("PG index streaming poisons contiguous access and resident "
          "minimizers_per_sequence admits once",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
  constexpr std::string_view source_table = "index_admin_index_poison_source";
  constexpr std::string_view target_table = "index_admin_index_poison_target";
  auto ctx = fresh_context({source_table, target_table});
  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled=off;"
                "SET bioseq.index.recompute_async_persistence.enabled=off;"
                "SET bioseq.aligner.minimizer.algorithm='scalar';"
                "CREATE TABLE index_admin_index_poison_source ("
                " id int, value bioseq.seq('dna4',64)) USING bioseq;"
                "INSERT INTO index_admin_index_poison_source VALUES ("
                " 1, repeat('ACGT',32775)::bioseq.seq('dna4',64));"
                "CREATE TABLE index_admin_index_poison_target ("
                " id int, value bioseq.seq('dna4',64)) USING bioseq;"
                "INSERT INTO index_admin_index_poison_target "
                "SELECT id, value[3:32771] OPERATOR(bioseq.||) "
                "       bioseq.reverse(value[32771:65539]) "
                "FROM index_admin_index_poison_source;");

  for (const std::string_view index_name : std::array<std::string_view, 2>{
           "minimizers_per_fragment", "symbol_counts"}) {
    exec_sql(ctx, "SELECT bioseq_internal.reset_sequence_source_counters();"
                  "SELECT bioseq_internal.arm_sequence_source_failure("
                  " 'facade_contiguous_access');");
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, '{}');",
                            target_table, index_name)) == 1);
    exec_sql(ctx, "SELECT bioseq_internal.disarm_sequence_source_failure();");
    INFO("streaming index=" << index_name);
    REQUIRE(facade_source_counter(ctx, "facade_source_stream_callbacks") > 0);
    REQUIRE(facade_source_counter(ctx, "facade_source_resident_probes") == 0);
    REQUIRE(facade_source_counter(ctx, "facade_declared_materializations") ==
            0);
    REQUIRE(facade_source_counter(ctx, "facade_completed_materializations") ==
            0);
    REQUIRE(facade_source_counter(ctx, "facade_poison_hits") == 0);
  }

  // max_concurrency is PGC_SIGHUP and cannot be changed by a session.  Use
  // the cluster's effective pool width; the exact resident-probe and
  // materialization counters below confirm that this branch was admitted.
  exec_sql(ctx,
           "SET bioseq.scheduler.pipeline.mode='task_parallel';"
           "SELECT bioseq_internal.reset_sequence_source_counters();");
  const long long sequence_minimizer_rows_before = index_table_row_count(
      ctx, "minimizers_per_sequence", target_table);
  const long long reserved_before = query_scalar_int(
      ctx, "SELECT index_memory_reserved_bytes "
           "FROM bioseq.scheduler_stats();");
  exec_sql(ctx, "SELECT bioseq_internal.arm_sequence_source_failure("
                " 'facade_materialization');");
  const auto materialization_error = sql_error(
      ctx, fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                       " 'minimizers_per_sequence');",
                       target_table));
  exec_sql(ctx, "SELECT bioseq_internal.disarm_sequence_source_failure();");

  REQUIRE(materialization_error.sqlstate == "XX000");
  REQUIRE(materialization_error.contains(
      "poisoned PostgreSQL sequence facade materialization"));
  REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
  REQUIRE(index_table_row_count(
              ctx, "minimizers_per_sequence", target_table) ==
          sequence_minimizer_rows_before);
  REQUIRE(query_scalar_int(
              ctx, "SELECT index_memory_reserved_bytes "
                   "FROM bioseq.scheduler_stats();") == reserved_before);
  REQUIRE(facade_source_counter(ctx, "facade_source_resident_probes") == 1);
  REQUIRE(facade_source_counter(ctx, "facade_declared_materializations") ==
          1);
  REQUIRE(facade_source_counter(ctx, "facade_completed_materializations") ==
          0);
  REQUIRE(facade_source_counter(ctx, "facade_poison_hits") == 1);

  exec_sql(ctx, "SELECT bioseq_internal.reset_sequence_source_counters();");
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                               " 'minimizers_per_sequence');",
                               target_table)) == 1);
  REQUIRE(index_table_row_count(
              ctx, "minimizers_per_sequence", target_table) >
          sequence_minimizer_rows_before);
  REQUIRE(facade_source_counter(ctx, "facade_source_stream_callbacks") > 0);
  REQUIRE(facade_source_counter(ctx, "facade_source_resident_probes") == 1);
  REQUIRE(facade_source_counter(ctx, "facade_declared_materializations") == 1);
  REQUIRE(facade_source_counter(ctx, "facade_completed_materializations") == 1);
  REQUIRE(facade_source_counter(ctx, "facade_poison_hits") == 0);
}

TEST_CASE("checked bounded ntHash failures unwind grouped fragment and sequence minimizer publishers",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
  struct failure_route {
    std::string_view table;
    std::string_view index_name;
    std::string_view operation;
  };
  constexpr std::array<failure_route, 2> routes{{
      {"index_admin_bounded_nthash_forced_fragment", "minimizers_per_fragment",
       "minimizer fragment generation replay"},
      {"index_admin_bounded_nthash_forced_sequence", "minimizers_per_sequence",
       "minimizers_per_sequence build"},
  }};

  for (const failure_route& route : routes) {
    DYNAMIC_SECTION(route.index_name) {
      auto ctx = fresh_context({route.table});
      exec_sql(ctx,
               "SET bioseq.index.insert_maintenance.enabled=off;"
               "SET bioseq.index.async_persistence.enabled=off;"
               "SET bioseq.index.recompute_async_persistence.enabled=off;"
               "SET bioseq.aligner.grouped_soa_generation.enabled=on;"
               "SET bioseq.aligner.minimizer.algorithm='bounded_nthash_simd';");
      exec_sqlf(ctx,
                "CREATE TABLE {} "
                "(id int, seq bioseq.seq('dna4',64)) USING bioseq;"
                "INSERT INTO {} VALUES "
                "(1,(repeat('ACGT',16391)||'A')::"
                "bioseq.seq('dna4',64));",
                route.table, route.table);
      REQUIRE(only_column_row_number(ctx, route.table) > 0);
      const long long backend_pid =
          query_scalar_int(ctx, "SELECT pg_backend_pid();");
      const long long admission_before = query_scalar_int(
          ctx, "SELECT index_memory_reserved_bytes "
               "FROM bioseq.scheduler_stats();");

      exec_sql(ctx,
               "SET bioseq.testing.persisted_index.failure_mode="
               "'unsupported_vector_shape';");
      const auto rejected = sql_error(ctx, fmt::format(
          "SELECT bioseq.update_index('{}'::regclass,'{}');",
          route.table, route.index_name));
      exec_sql(ctx,
               "SET bioseq.testing.persisted_index.failure_mode='none';");

      REQUIRE(rejected.sqlstate == "0A000");
      REQUIRE(rejected.contains("status=unsupported_vector_shape"));
      REQUIRE(rejected.contains(route.operation));
      REQUIRE_FALSE(rejected.sqlstate == "XX000");
      REQUIRE_FALSE(rejected.sqlstate == "XX001");
      REQUIRE(query_scalar_int(ctx, "SELECT pg_backend_pid();") ==
              backend_pid);
      REQUIRE(index_table_row_count(ctx, route.index_name, route.table) == 0);
      REQUIRE(query_scalar_int(ctx, fmt::format(
          "SELECT count(*) FROM bioseq_internal.metadata_age_log "
          "WHERE user_relid='{}'::regclass AND index_name='{}';",
          route.table, route.index_name)) == 0);
      REQUIRE(query_scalar_int(ctx, fmt::format(
          "SELECT count(*) FROM "
          "bioseq_internal.minimizer_generation_storage "
          "WHERE user_relid='{}'::regclass AND index_name='{}';",
          route.table, route.index_name)) == 0);
      REQUIRE(query_scalar_int(
          ctx, "SELECT index_memory_reserved_bytes "
               "FROM bioseq.scheduler_stats();") == admission_before);
      REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

      // The same bounded ntHash backend succeeds through the same publisher immediately,
      // confirming the one-shot failure arm, writer, spill, and source owner were
      // all cleared by the rejected statement.
      exec_sql(ctx,
               "SET bioseq.aligner.minimizer.algorithm='bounded_nthash_simd';");
      REQUIRE(query_scalar_int(ctx, fmt::format(
          "SELECT bioseq.update_index('{}'::regclass,'{}');",
          route.table, route.index_name)) == 1);
      REQUIRE(index_table_row_count(ctx, route.index_name, route.table) > 0);
      REQUIRE(query_scalar_int(
          ctx, "SELECT index_memory_reserved_bytes "
               "FROM bioseq.scheduler_stats();") == admission_before);
    }
  }
}
#endif

TEST_CASE(
    "update_index('minimizers_per_fragment') builds the per-fragment index",
    "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  // seed_small_bioseq_column sets bioseq.index.insert_maintenance.enabled = on, so the
  // two default UPDATE_ON_INSERT indexes (minimizers_per_fragment,
  // symbol_counts) are populated incrementally at INSERT time.
  seed_small_bioseq_column(ctx, k_table);

  REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", k_table) > 0);
  REQUIRE(index_table_row_count(ctx, "symbol_counts", k_table) > 0);

  // Explicit rebuild of the per-fragment index returns the count of
  // sequences (re)indexed — three seeded rows.
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                               "'minimizers_per_fragment');",
                               k_table)) == 3);
}

TEST_CASE("update_index rejects an unknown index name",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'NOT_A_KIND');",
                    k_table)));
}

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
TEST_CASE("two-stack ntHash update_index accepts DNA5 without scalar fallback",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "index_admin_exact_dna5";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna5', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES (1, "
        "(repeat('ACGT', 256) || 'N' || repeat('TGCA', 256) || "
        "'N' || repeat('GATTACA', 128))::bioseq.seq('dna5', 64));",
        table);
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';");
    exec_sql(ctx,
        "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';");
    exec_sql(ctx,
        "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');",
        table)) == 1);

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
}

TEST_CASE("bounded ntHash update_index maps checked DNA5 rejection to 0A000",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    struct failure_route {
        std::string_view table;
        std::string_view index_name;
        std::string_view operation;
    };
    constexpr std::array<failure_route, 2> routes{{
        {"index_admin_bounded_nthash_grouped_fragment", "minimizers_per_fragment",
         "grouped minimizer generation preflight"},
        {"index_admin_bounded_nthash_grouped_sequence", "minimizers_per_sequence",
         "grouped minimizer generation preflight"},
    }};

    for (const failure_route& route : routes) {
        DYNAMIC_SECTION("grouped " << route.index_name) {
            auto ctx = fresh_context({route.table});
            exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
            exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
            exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
            exec_sql(ctx,
                "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
            exec_sql(ctx,
                "SET bioseq.aligner.minimizer.algorithm = 'bounded_nthash_simd';");
            exec_sqlf(ctx,
                "CREATE TABLE {} "
                "(id int, seq bioseq.seq('dna5', 64)) USING bioseq;",
                route.table);
            exec_sqlf(ctx,
                "INSERT INTO {} VALUES "
                "(1, (repeat('ACGTN', 13113) || 'A')::"
                "bioseq.seq('dna5', 64));",
                route.table);

            // This source is locator-backed and long enough to enter the
            // selected bounded ntHash producer. The grouped sequence publisher
            // must preserve unsupported_alphabet rather than mapping it to
            // storage_contract_mismatch or XX001 in the publication encoder.
            REQUIRE(only_column_row_number(ctx, route.table) > 0);
            const auto rejected = sql_error(ctx, fmt::format(
                "SELECT bioseq.update_index('{}'::regclass, '{}');",
                route.table, route.index_name));
            REQUIRE(rejected.sqlstate == "0A000");
            REQUIRE(rejected.contains("bounded ntHash SIMD"));
            REQUIRE(rejected.contains("status=unsupported_alphabet"));
            REQUIRE(rejected.contains(route.operation));
            REQUIRE(index_table_row_count(
                ctx, route.index_name, route.table) == 0);
            REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        }
    }
}
#endif

TEST_CASE("update_index accepts text and explicit regclass for the table arg",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    // Quoted text form — PG implicitly casts text -> regclass via regclassin.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}', "
                            "'minimizers_per_fragment');",
                            k_table)) == 3);

    // Explicit ::regclass cast form.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            k_table)) == 3);
}

TEST_CASE("update_index rejects non-bioseq-AM tables",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view k_heap_table = "index_admin_heap_t";
    auto ctx = fresh_context({k_heap_table});

    // Plain heap-AM table with a bioseq.seq column. The chunked-storage
    // reindex path is only reachable from the bioseq columnar AM, so
    // update_index must refuse a heap relation.
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_heap_table),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = false,
        },
        {seq_row{.pk = 1, .sequences = {"ACGTACGTACGTACGT"}}});

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                    "'minimizers_per_fragment');",
                    k_heap_table)));
}

TEST_CASE("set_index_policy toggles update_policy and supports per-sequence on-insert",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    // Helper: read the current update_policy of one index from list_metadata.
    auto policy_of = [&](std::string_view index_name) {
        return query_scalar(
            ctx,
            fmt::format("SELECT update_policy FROM bioseq.list_metadata() "
                        " WHERE table_name = '{}'::regclass "
                        "   AND index_name = '{}' LIMIT 1;",
                        k_table, index_name));
    };

    // symbol_counts defaults to UPDATE_ON_INSERT; flip it to UPDATE_MANUALLY.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, 'symbol_counts', "
              "'UPDATE_MANUALLY');",
              k_table);
    REQUIRE(policy_of("symbol_counts") == "UPDATE_MANUALLY");

    // Flip it back to UPDATE_ON_INSERT.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, 'symbol_counts', "
              "'UPDATE_ON_INSERT');",
              k_table);
    REQUIRE(policy_of("symbol_counts") == "UPDATE_ON_INSERT");

    // minimizers_per_sequence is opt-in UPDATE_ON_INSERT. This must be
    // accepted so asynchronous recompute persistence and batch load can honor
    // the table policy instead of hard-wiring minimizers_per_fragment.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, "
              "'minimizers_per_fragment', 'UPDATE_MANUALLY');",
              k_table);
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, "
              "'minimizers_per_sequence', 'UPDATE_ON_INSERT');",
              k_table);
    REQUIRE(policy_of("minimizers_per_fragment") == "UPDATE_MANUALLY");
    REQUIRE(policy_of("minimizers_per_sequence") == "UPDATE_ON_INSERT");
}

TEST_CASE("UPDATE_ON_INSERT can build minimizers_per_sequence without per-fragment fallback",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"index_admin_sequence_minimizer_policy"});
    exec_sql(ctx, "CREATE TABLE index_admin_sequence_minimizer_policy "
                  "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx, "SELECT bioseq.set_index_policy("
                  "'index_admin_sequence_minimizer_policy'::regclass, "
                  "'minimizers_per_fragment', 'UPDATE_MANUALLY');");
    exec_sql(ctx, "SELECT bioseq.set_index_policy("
                  "'index_admin_sequence_minimizer_policy'::regclass, "
                  "'minimizers_per_sequence', 'UPDATE_ON_INSERT');");
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sql(ctx, "INSERT INTO index_admin_sequence_minimizer_policy VALUES "
                  "('minimizers_per_sequence', "
                  "repeat('ACGT', 2000)::bioseq.seq('dna4', 64));");

    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment",
                                  "index_admin_sequence_minimizer_policy") == 0);
    REQUIRE(index_table_row_count(ctx, "minimizers_per_sequence",
                                  "index_admin_sequence_minimizer_policy") > 0);
}

TEST_CASE("update_index('minimizers_per_fragment') is idempotent at the content level",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    // Run the rebuild twice; the per-fragment chunk row count must be stable
    // (the rebuild clears the index then repopulates from identical input).
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            k_table)) == 3);
    const auto after_first =
        index_table_row_count(ctx, "minimizers_per_fragment", k_table);

    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            k_table)) == 3);
    const auto after_second =
        index_table_row_count(ctx, "minimizers_per_fragment", k_table);

    REQUIRE(after_first > 0);
    REQUIRE(after_first == after_second);
}

TEST_CASE("update_index on an empty bioseq table returns 0 and list_metadata "
          "still lists kinds",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    // Create an empty USING bioseq table (no rows).
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) "
              "USING bioseq;",
              k_table);

    // No rows -> no sequences (re)indexed -> 0.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            k_table)) == 0);

    // list_metadata still lists the three production kinds with
    // storage_size '0 bytes'.
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT index_name, storage_size "
                    "FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    "   AND index_name <> 'test_table_rowcount' "
                    " ORDER BY index_name;",
                    k_table));
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0][0].value() == "minimizers_per_fragment");
    REQUIRE(rows[1][0].value() == "minimizers_per_sequence");
    REQUIRE(rows[2][0].value() == "symbol_counts");
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[1].value() == "0 bytes");
    }
}

TEST_CASE("drop_all_index_tables applies the exact table selector and fires "
          "drop events",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "CREATE TABLE public.native_drop_event_log ("
        "object_type text, schema_name text, object_name text);");
    exec_sql(ctx,
        "CREATE FUNCTION public.capture_native_index_drops() "
        "RETURNS event_trigger LANGUAGE plpgsql AS $capture$ "
        "BEGIN "
        "  INSERT INTO public.native_drop_event_log "
        "      (object_type, schema_name, object_name) "
        "  SELECT object_type, schema_name, object_name "
        "  FROM pg_event_trigger_dropped_objects(); "
        "END "
        "$capture$;");
    exec_sql(ctx,
        "CREATE EVENT TRIGGER capture_native_index_drops "
        "ON sql_drop WHEN TAG IN ('DROP TABLE') "
        "EXECUTE FUNCTION public.capture_native_index_drops();");

    exec_sql(ctx,
        "CREATE TABLE bioseq_internal.seqidx_native_plain (value int);"
        "CREATE TABLE bioseq_internal.seqidxtab_native_plain (value int);"
        "CREATE TABLE bioseq_internal.\"seqidx_native Ω\"\"quoted\" "
        "    (value int);"
        "CREATE VIEW public.native_drop_dependency AS "
        "    SELECT value FROM bioseq_internal.seqidx_native_plain;"
        "CREATE VIEW bioseq_internal.seqidx_native_view_survives AS "
        "    SELECT 1 AS value;"
        "CREATE TABLE bioseq_internal.seqidx (value int);"
        "CREATE TABLE bioseq_internal.seqidxnear (value int);"
        "CREATE TABLE bioseq_internal.seqidxtab (value int);"
        "CREATE TABLE bioseq_internal.seqidxtabnear (value int);"
        "CREATE TABLE bioseq_internal.seqid (value int);"
        "CREATE TABLE bioseq_internal.seqid_ (value int);"
        "CREATE TABLE bioseq_internal.seqid_native_near (value int);");

    exec_sql(ctx, "SELECT bioseq.drop_all_index_tables();");
    exec_sql(ctx, "SELECT bioseq.drop_all_index_tables();");

    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relname::text IN ("
        "  'seqidx_native_plain',"
        "  'seqidxtab_native_plain',"
        "  'seqidx_native Ω\"quoted'"
        ");") == 0);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relname::text IN ("
        "  'seqidx', 'seqidxnear', 'seqidxtab', 'seqidxtabnear',"
        "  'seqid', 'seqid_', 'seqid_native_near'"
        ");") == 7);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relname = 'seqidx_native_view_survives' "
        "AND c.relkind = 'v';") == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (to_regclass('public.native_drop_dependency') IS NULL)::int;"
        ) == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM public.native_drop_event_log "
        "WHERE object_type = 'table' "
        "AND schema_name = 'bioseq_internal' "
        "AND object_name IN ("
        "  'seqidx_native_plain',"
        "  'seqidxtab_native_plain',"
        "  'seqidx_native Ω\"quoted'"
        ");") == 3);
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM public.native_drop_event_log "
        "WHERE object_type = 'view' "
        "AND schema_name = 'public' "
        "AND object_name = 'native_drop_dependency';") == 1);
    exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE("prefix index clearing selects the exact prefix and preserves "
          "DROP TABLE CASCADE behavior",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "native_prefix_selector_source";
    auto ctx = fresh_context({table});
    seed_small_bioseq_column(ctx, table);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');", table)) == 3);

    const std::string sidecar_name = query_scalar(ctx,
        "SELECT c.relname "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relkind = 'r' "
        "AND starts_with(c.relname::text, 'seqidx_minimizer_fragment_') "
        "ORDER BY c.oid LIMIT 1;");
    constexpr std::string_view physical_name_stem = "seqidx_minimizer_fragment_";
    constexpr std::size_t identity_hash_hex_size = 16;
    const std::size_t clear_prefix_size =
        physical_name_stem.size() + identity_hash_hex_size + 1;
    REQUIRE(sidecar_name.size() > clear_prefix_size);
    const std::string selected_prefix =
        sidecar_name.substr(0, clear_prefix_size);
    REQUIRE(selected_prefix.back() == '_');
    const std::string selected_name =
        fmt::format("{}native Ω\"quoted", selected_prefix);
    const std::string selected_identifier =
        fmt::format("\"{}native Ω\"\"quoted\"", selected_prefix);
    const std::string near_miss_name =
        fmt::format("{}x_native_near", selected_prefix.substr(
            0, selected_prefix.size() - 1));

    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx,
        "CREATE TABLE bioseq_internal.{} (value int);"
        "CREATE VIEW public.native_prefix_dependency AS "
        "SELECT value FROM bioseq_internal.{};"
        "CREATE TABLE bioseq_internal.{} (value int);",
        selected_identifier,
        selected_identifier,
        near_miss_name);

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');", table)) == 3);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT (to_regclass(format("
        "'bioseq_internal.%I', '{}')) IS NULL)::int;",
        selected_name)) == 1);
    REQUIRE(query_scalar_int(ctx,
        "SELECT (to_regclass('public.native_prefix_dependency') "
        "IS NULL)::int;") == 1);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT (to_regclass('bioseq_internal.{}') "
        "IS NOT NULL)::int;",
        near_miss_name)) == 1);
    exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE("drop_all_index_tables preserves PostgreSQL ownership checks",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "CREATE ROLE native_drop_owner NOLOGIN;"
        "CREATE ROLE native_drop_caller NOLOGIN;"
        "CREATE TABLE bioseq_internal.seqidx_native_owned (value int);"
        "ALTER TABLE bioseq_internal.seqidx_native_owned "
        "    OWNER TO native_drop_owner;"
        "GRANT USAGE ON SCHEMA bioseq, bioseq_internal "
        "    TO native_drop_caller;"
        "GRANT EXECUTE ON FUNCTION bioseq.drop_all_index_tables() "
        "    TO native_drop_caller;"
        "SET LOCAL ROLE native_drop_caller;");
    const auto denied =
        sql_error(ctx, "SELECT bioseq.drop_all_index_tables();");
    exec_sql(ctx, "ROLLBACK;");

    REQUIRE(denied.sqlstate == "42501");
    REQUIRE(denied.contains("must be owner"));
}

TEST_CASE("drop_all_index_tables rolls back an earlier drop when the second "
          "selected drop fails",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "CREATE TABLE public.native_second_drop_counter (call_no int NOT NULL);"
        "INSERT INTO public.native_second_drop_counter VALUES (0);"
        "CREATE TABLE bioseq_internal.seqidx_native_atomic_a (value int);"
        "CREATE TABLE bioseq_internal.seqidx_native_atomic_b (value int);");
    exec_sql(ctx,
        "CREATE FUNCTION public.fail_second_native_index_drop() "
        "RETURNS event_trigger LANGUAGE plpgsql AS $failure$ "
        "DECLARE next_call int; "
        "BEGIN "
        "  IF EXISTS ("
        "    SELECT 1 FROM pg_event_trigger_dropped_objects() "
        "    WHERE original AND object_type = 'table' "
        "    AND schema_name = 'bioseq_internal' "
        "    AND object_name IN ("
        "      'seqidx_native_atomic_a', 'seqidx_native_atomic_b')) "
        "  THEN "
        "    UPDATE public.native_second_drop_counter "
        "    SET call_no = call_no + 1 "
        "    RETURNING call_no INTO next_call; "
        "    IF next_call = 2 THEN "
        "      RAISE EXCEPTION 'forced second selected drop failure' "
        "      USING ERRCODE = 'XX000'; "
        "    END IF; "
        "  END IF; "
        "END "
        "$failure$;");
    exec_sql(ctx,
        "CREATE EVENT TRIGGER fail_second_native_index_drop "
        "ON sql_drop WHEN TAG IN ('DROP TABLE') "
        "EXECUTE FUNCTION public.fail_second_native_index_drop();");

    const long long first_oid = query_scalar_int(ctx,
        "SELECT 'bioseq_internal.seqidx_native_atomic_a'::regclass"
        "::oid::bigint;");
    const long long second_oid = query_scalar_int(ctx,
        "SELECT 'bioseq_internal.seqidx_native_atomic_b'::regclass"
        "::oid::bigint;");
    exec_sql(ctx, "SAVEPOINT before_native_index_drop;");
    const auto forced_failure =
        sql_error(ctx, "SELECT bioseq.drop_all_index_tables();");
    exec_sql(ctx, "ROLLBACK TO SAVEPOINT before_native_index_drop;");

    REQUIRE(forced_failure.sqlstate == "XX000");
    REQUIRE(forced_failure.contains("forced second selected drop failure"));
    REQUIRE(query_scalar_int(ctx,
        "SELECT 'bioseq_internal.seqidx_native_atomic_a'::regclass"
        "::oid::bigint;") == first_oid);
    REQUIRE(query_scalar_int(ctx,
        "SELECT 'bioseq_internal.seqidx_native_atomic_b'::regclass"
        "::oid::bigint;") == second_oid);
    REQUIRE(query_scalar_int(ctx,
        "SELECT call_no FROM public.native_second_drop_counter;") == 0);
    exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE("prefix index clearing rejects a wrong relation kind and rolls back "
          "prior drops",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    constexpr std::string_view table = "native_prefix_rollback_source";
    auto ctx = fresh_context({table});
    seed_small_bioseq_column(ctx, table);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');", table)) == 3);

    const std::string sidecar_name = query_scalar(ctx,
        "SELECT c.relname "
        "FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'bioseq_internal' "
        "AND c.relkind = 'r' "
        "AND starts_with(c.relname::text, 'seqidx_minimizer_fragment_') "
        "ORDER BY c.oid LIMIT 1;");
    const long long original_oid = query_scalar_int(ctx, fmt::format(
        "SELECT 'bioseq_internal.{}'::regclass::oid::bigint;",
        sidecar_name));
    const long long original_rows = query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM bioseq_internal.{};", sidecar_name));
    REQUIRE(original_rows > 0);
    const std::string wrong_kind_name =
        fmt::format("{}_wrong_relkind", sidecar_name);

    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx,
        "CREATE VIEW bioseq_internal.{} AS SELECT 1 AS value;",
        wrong_kind_name);
    const auto wrong_kind = sql_error(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');", table));
    exec_sql(ctx, "ROLLBACK;");

    REQUIRE(wrong_kind.sqlstate == "42809");
    REQUIRE(wrong_kind.contains("is not a table"));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT 'bioseq_internal.{}'::regclass::oid::bigint;",
        sidecar_name)) == original_oid);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM bioseq_internal.{};",
        sidecar_name)) == original_rows);
}
