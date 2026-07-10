// Integration scenarios for the DOC0279 metadata-admin SQL surface
// (bioseq.list_metadata + bioseq.update_index + bioseq.set_index_policy):
//   * list_metadata enumerates the three PRODUCTION index kinds
//     (minimizers_per_fragment, symbol_counts, minimizers_per_sequence) for a
//     freshly-created USING bioseq table, NOT gated on data: every index
//     appears with size='0 bytes' and a NULL age, carrying its registered
//     (index_type, update_policy).
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
//
// The legacy index_sidecar / meta_sidecar tables and count_sidecar() helper
// are DELETED; row-count assertions go through the new
// bioseq.index_table_row_count() libpq wrapper (index_table_row_count()).

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_index_admin_t";

[[nodiscard]] std::string nonaligned_three_fragment_dna4_source() {
    std::string source;
    source.reserve(65'549);
    for (std::size_t i = 0; i < 16'387; ++i) {
        source.append("ACGT");
    }
    source.push_back('A');
    return source;
}

// Test-local, bit-at-a-time CRC-64/ECMA-182 implementation.  This deliberately
// does not import or call the production table-driven checksum helper, so the
// fixture's decoded-source checksum has an independent oracle.
[[nodiscard]] std::uint64_t independent_crc64_ecma182(
        std::string_view bytes) noexcept {
    constexpr std::uint64_t polynomial = 0x42F0E1EBA9EA3693ULL;
    std::uint64_t crc = 0;
    for (const unsigned char byte : bytes) {
        crc ^= static_cast<std::uint64_t>(byte) << 56u;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & (std::uint64_t{1} << 63u)) != 0
                ? (crc << 1u) ^ polynomial
                : crc << 1u;
        }
    }
    return crc;
}

[[nodiscard]] std::string independent_fixed_hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[result.size() - i - 1] = digits[value & 0x0fU];
        value >>= 4U;
    }
    return result;
}

[[nodiscard]] long long only_column_row_number(
        const connection_test_context& ctx, std::string_view table) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) FROM {}",
        table, table));
}

}  // namespace

TEST_CASE("list_metadata reports all production index kinds for a fresh bioseq table",
          "[index_admin][list]") {
    auto ctx = fresh_context({kTable});
    // No inserts: list_metadata is NOT gated on data, so every registered
    // index must already appear for the fresh table.
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) "
              "USING bioseq;",
              kTable);

    // Filter out the test-only CHUNKED_PER_TABLE 'test_table_rowcount' index
    // (only present under -DBIOSEQ_TEST_INDEXES) so the assertion pins exactly
    // the three production kinds regardless of how the dylib was built.
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT name, index_type, update_policy, size, (age IS NULL) "
                    "  FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    "   AND name <> 'test_table_rowcount' "
                    " ORDER BY name;",
                    kTable));

    REQUIRE(rows.size() == 3);

    // Sorted by name: minimizers_per_fragment, minimizers_per_sequence,
    // symbol_counts.
    REQUIRE(rows[0].size() == 5);
    REQUIRE(rows[0][0].value() == "minimizers_per_fragment");
    REQUIRE(rows[0][1].value() == "CHUNKED_PER_FRAGMENT");
    REQUIRE(rows[0][2].value() == "UPDATE_ON_INSERT");
    REQUIRE(rows[0][3].value() == "0 bytes");
    REQUIRE(rows[0][4].value() == "t");  // age IS NULL

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

TEST_CASE("DOC0371 READY generation inspection SQL ABI is narrow and administrative",
          "[index_admin][doc0371][ready-inspection][abi]") {
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
    REQUIRE(row[5].value() == "3");       // exact input ABI
    REQUIRE(row[6].value() == "26");      // 3 inputs + 23 named outputs
    REQUIRE(row[7].value() == "1");       // PUBLIC has no EXECUTE grant
}

TEST_CASE("DOC0371 revision-3 generation allocator has the exact owned permanent contract",
          "[index_admin][doc0371][rev3][sequence][catalog]") {
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

TEST_CASE("DOC0371 sequence-source epoch allocator has the exact owned permanent contract",
          "[index_admin][doc0371][source-version][sequence][catalog]") {
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

TEST_CASE("DOC0371 sequence-source version catalog has exact schema indexes and ACL",
          "[index_admin][doc0371][source-version][catalog][acl]") {
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

TEST_CASE("DOC0371 source epochs publish atomically and represent repeated empty sources",
          "[index_admin][doc0371][source-version][empty][mvcc]") {
    auto ctx = fresh_context({"doc0371_source_empty"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_source_empty "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_source_empty VALUES "
        "(1, ''::bioseq.seq('dna4', 64)), "
        "(2, ''::bioseq.seq('dna4', 64));");
    const auto empty_rows = query_one(ctx,
        "SELECT count(*)::text, count(DISTINCT source_epoch)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE num_fragments=0 AND fragment_relid IS NULL "
        "AND fragment_storage_id=0 AND first_sub_row_no=0)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_empty'::regclass;");
    REQUIRE(empty_rows[0].value() == "2");
    REQUIRE(empty_rows[1].value() == "2");
    REQUIRE(empty_rows[2].value() == "2");
    REQUIRE(empty_rows[3].value() == "2");

    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "INSERT INTO doc0371_source_empty VALUES "
        "(3, 'ACGT'::bioseq.seq('dna4', 64));");
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_empty'::regclass;") == 3);
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_empty'::regclass;") == 2);
}

TEST_CASE("fresh backend DROP EXTENSION CASCADE owns generated relation deletion without stale revisits",
          "[index_admin][source-version][drop][cascade][stale-contract]") {
    constexpr std::string_view table = "source_drop_cascade_probe";
    auto setup_ctx = fresh_context({table});
    exec_sqlf(setup_ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(setup_ctx,
        "INSERT INTO {} VALUES (1, 'ACGTACGT'::bioseq.seq('dna4', 64));",
        table);

    // Capture the exact generated fragment, sidecar, and implicit sidecar-PK
    // OIDs. DROP EXTENSION's dependency list must remove each itself; Bioseq's
    // parent hook must not recursively delete them and manufacture stale OIDs.
    const auto generated = query_one(setup_ctx, fmt::format(
        "SELECT s.sub_table_relid::text, s.sidecar_relid::text, "
        "i.indexrelid::text "
        "FROM bioseq_internal.seq_sub_table s "
        "JOIN pg_index i ON i.indrelid=s.sidecar_relid AND i.indisprimary "
        "WHERE s.main_relfilenode=pg_relation_filenode('{}'::regclass) "
        "AND s.column_attno=2;",
        table));
    REQUIRE(generated.size() == 3);
    REQUIRE(query_scalar_int(setup_ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='source_drop_cascade_probe'::regclass "
        "AND state=1;") == 1);

    // Reproduce the immediately preceding revision-3 catalog contract. A new
    // same-version dylib/SQL install cannot migrate an already-installed 1.0
    // extension, but DROP EXTENSION must still be able to remove and recreate
    // that extension without interpreting the stale tuple descriptor.
    exec_sql(setup_ctx,
        "ALTER TABLE bioseq_internal.sequence_source_version "
        "ALTER COLUMN fragment_relid SET NOT NULL;");

    // This connection has executed no prior SQL and therefore has not loaded
    // bioseq.dylib. PostgreSQL may load the table-AM library only after the
    // top-level standard_ProcessUtility cascade is already in progress; the
    // OAT-time extension-lock proof must cover that production path.
    connection_test_context drop_ctx;
    exec_sql(drop_ctx, "DROP EXTENSION bioseq CASCADE;");
    const auto removed_objects = query_scalar_int(drop_ctx, fmt::format(
        "SELECT count(*) FROM pg_class WHERE oid IN ({},{},{});",
        generated[0].value(), generated[1].value(), generated[2].value()));
    const auto extension_after_drop = query_scalar_int(drop_ctx,
        "SELECT count(*) FROM pg_extension WHERE extname='bioseq';");

    // Restore the database-global fixture from a fresh backend, just like the
    // shared reset helper, before making assertions that could abort the test.
    {
        connection_test_context create_ctx;
        exec_sql(create_ctx, "CREATE EXTENSION bioseq;");
    }
    const auto extension_after_create = query_scalar_int(drop_ctx,
        "SELECT count(*) FROM pg_extension WHERE extname='bioseq';");
    const auto teardown_diagnostics = query_one(drop_ctx,
        "SELECT lock_fallback_classifications::text, "
        "missing_relation_observations::text "
        "FROM bioseq_internal.teardown_diagnostics();");

    // Prove that the backend which ran DROP can use the recreated extension,
    // rather than accepting a catalog row that only looks installed.
    constexpr std::string_view recreated = "source_drop_recreated_probe";
    exec_sqlf(drop_ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        recreated);
    exec_sqlf(drop_ctx,
        "INSERT INTO {} VALUES (1, 'TGCATGCA'::bioseq.seq('dna4', 64));",
        recreated);
    const auto recreated_probe = query_one(drop_ctx, fmt::format(
        "SELECT (SELECT count(*) FROM {})::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "min(source_epoch)::text, min(locator_version)::text, "
        "min(num_fragments)::text, min(total_bp_size)::text, "
        "bool_and(fragment_relid IS NOT NULL)::int::text, "
        "(SELECT seq::text FROM {} WHERE id=1) "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='{}'::regclass;",
        recreated, recreated, recreated));
    exec_sqlf(drop_ctx, "DROP TABLE {};", recreated);

    REQUIRE(removed_objects == 0);
    REQUIRE(extension_after_drop == 0);
    REQUIRE(extension_after_create == 1);
    REQUIRE(teardown_diagnostics.size() == 2);
    REQUIRE(std::stoll(teardown_diagnostics[0].value()) > 0);
    REQUIRE(std::stoll(teardown_diagnostics[1].value()) == 0);
    REQUIRE(recreated_probe.size() == 8);
    REQUIRE(recreated_probe[0].value() == "1");  // parent row is visible
    REQUIRE(recreated_probe[1].value() == "1");  // exact READY source
    REQUIRE(std::stoll(recreated_probe[2].value()) > 0);
    REQUIRE(recreated_probe[3].value() == "3");  // locator ABI
    REQUIRE(recreated_probe[4].value() == "1");  // one physical fragment
    REQUIRE(recreated_probe[5].value() == "8");  // exact logical length
    REQUIRE(recreated_probe[6].value() == "1");  // physical relation bound
    REQUIRE(recreated_probe[7].value() == "TGCATGCA"); // hydration/text path
}

TEST_CASE("ordinary drops fail closed on missing or malformed source catalogs",
          "[index_admin][source-version][drop][catalog-contract]") {
    constexpr std::string_view table = "source_drop_contract_probe";
    auto ctx = fresh_context({table});
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES (1, 'ACGTACGT'::bioseq.seq('dna4', 64));",
        table);

    // A rejected RESTRICT drop exercises the ProcessUtility PG_CATCH path.
    // The following missing-catalog probe must still fail closed, proving the
    // teardown context did not leak in this same backend.
    const auto restrict_error = sql_error(ctx, "DROP EXTENSION bioseq;");
    REQUIRE(restrict_error.sqlstate == "2BP01");

    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "ALTER TABLE bioseq_internal.sequence_source_version "
        "RENAME TO sequence_source_version_missing_probe;");
    const auto missing_error = sql_error(ctx, fmt::format(
        "DROP TABLE {};", table));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(missing_error.sqlstate == "55000");
    REQUIRE(missing_error.contains("missing outside extension teardown"));
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM {};", table)) == 1);

    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
        "ALTER TABLE bioseq_internal.seq_sub_table "
        "RENAME COLUMN sub_table_relid TO sub_table_relid_bad;");
    const auto schema_error = sql_error(ctx, fmt::format(
        "DROP TABLE {};", table));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(schema_error.sqlstate == "55000");
    REQUIRE(schema_error.contains("fragment registry catalog contract changed"));
    REQUIRE(query_scalar_int(ctx,
        "SELECT count(*) FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='source_drop_contract_probe'::regclass "
        "AND state=1;") == 1);
}

TEST_CASE("DOC0371 speculative source authority is consumed or never created",
          "[index_admin][doc0371][source-version][speculative]") {
    auto ctx = fresh_context({"doc0371_source_speculative"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_source_speculative "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_source_speculative VALUES "
        "(1, 'ACGT'::bioseq.seq('dna4', 64));");
    exec_sql(ctx,
        "INSERT INTO doc0371_source_speculative VALUES "
        "(1, 'TGCA'::bioseq.seq('dna4', 64)) ON CONFLICT (id) DO NOTHING;");
    const auto states = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM doc0371_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_speculative'::regclass;");
    REQUIRE(states[0].value() == "0");
    REQUIRE(states[1].value() == "1");
    // PostgreSQL's ON CONFLICT pre-check may prove the conflict before calling
    // table_tuple_insert_speculative.  In that valid path no source epoch or
    // publication authority is created, so there is nothing to invalidate.
    REQUIRE(states[2].value() == "0");
    REQUIRE(states[3].value() == "1");

    // No-conflict ON CONFLICT takes the successful speculative-completion path.
    // Its move-only authority must be consumed into READY before PRE_COMMIT.
    exec_sql(ctx,
        "INSERT INTO doc0371_source_speculative VALUES "
        "(2, 'CCCC'::bioseq.seq('dna4', 64)) ON CONFLICT (id) DO NOTHING;");
    const auto succeeded = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM doc0371_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_speculative'::regclass;");
    REQUIRE(succeeded[0].value() == "0");
    REQUIRE(succeeded[1].value() == "2");
    REQUIRE(succeeded[2].value() == "0");
    REQUIRE(succeeded[3].value() == "2");

    // A subtransaction rollback must remove its catalog row and leave no
    // backend-local speculative authority for the outer PRE_COMMIT gate.
    exec_sql(ctx, "BEGIN; SAVEPOINT source_speculative;");
    exec_sql(ctx,
        "INSERT INTO doc0371_source_speculative VALUES "
        "(3, 'GGGG'::bioseq.seq('dna4', 64)) ON CONFLICT (id) DO NOTHING;");
    exec_sql(ctx, "ROLLBACK TO SAVEPOINT source_speculative; COMMIT;");
    const auto after_subtransaction = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM doc0371_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_speculative'::regclass;");
    REQUIRE(after_subtransaction[0].value() == "0");
    REQUIRE(after_subtransaction[1].value() == "2");
    REQUIRE(after_subtransaction[2] == succeeded[2]);
    REQUIRE(after_subtransaction[3].value() == "2");
}

TEST_CASE("cold no-index audit is metadata-only and detects physical minimizer relations",
          "[index_admin][doc0371][no-index][absence][fail-closed]") {
    auto ctx = fresh_context({"doc0371_no_index_absence"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_no_index_absence "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off; "
        "INSERT INTO doc0371_no_index_absence VALUES "
        "(1, repeat('ACGT', 4096)::bioseq.seq('dna4', 64));");

    const auto absent = query_one(ctx,
        "SELECT target_rows::text, target_cardinality_metadata_only::int::text, "
        "source_parent_rows_read::text, source_fragment_rows_read::text, "
        "source_decoded_bp::text, physical_index_relations::text, "
        "persisted_index_rows::text, generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'doc0371_no_index_absence'::regclass, 'minimizers_per_fragment');");
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
        "AND c.relkind='r' AND c.relname LIKE 'seqidx\\_mpf\\_%' "
        "ESCAPE '\\' ORDER BY c.relname;");
    exec_sql(ctx,
        "SELECT bioseq.update_index('doc0371_no_index_absence'::regclass, "
        "'minimizers_per_fragment');");
    const auto present = query_one(ctx,
        "SELECT physical_index_relations::text, persisted_index_rows::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'doc0371_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(std::stoll(present[0].value()) > 0);
    REQUIRE(std::stoll(present[1].value()) > 0);

    const auto after_sidecars = query_rows(ctx,
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n "
        "ON n.oid=c.relnamespace WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' AND c.relname LIKE 'seqidx\\_mpf\\_%' "
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
        "'doc0371_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(std::stoll(empty_but_present[0].value()) > 0);
    REQUIRE(empty_but_present[1].value() == "0");
    REQUIRE(empty_but_present[2].value() == "0");

    const auto unsupported = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_cold_no_index_absence("
        "'doc0371_no_index_absence'::regclass, 'symbol_counts');");
    REQUIRE(unsupported.sqlstate == "55000");
    REQUIRE(unsupported.contains("MPF or MPS"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("sequence-source invalidation is terminal across parent update and delete",
          "[index_admin][doc0371][source-version][terminal][update][delete]") {
    auto ctx = fresh_context({"doc0371_source_terminal"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_source_terminal "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert=off; "
        "INSERT INTO doc0371_source_terminal VALUES "
        "(1, 'ACGTACGT'::bioseq.seq('dna4', 64));");
    const auto first_epoch = query_one(ctx,
        "SELECT source_epoch::text FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_terminal'::regclass AND state=1;");
    REQUIRE(first_epoch.size() == 1);

    exec_sql(ctx,
        "UPDATE doc0371_source_terminal SET "
        "seq='TGCATGCA'::bioseq.seq('dna4', 64) WHERE id=1;");
    const auto after_update = query_one(ctx, fmt::format(
        "SELECT count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "max(state) FILTER (WHERE source_epoch={})::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_terminal'::regclass;",
        first_epoch[0].value()));
    REQUIRE(after_update[0].value() == "1");
    REQUIRE(after_update[1].value() == "1");
    REQUIRE(after_update[2].value() == "2");

    exec_sql(ctx, "DELETE FROM doc0371_source_terminal WHERE id=1;");
    const auto after_delete = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_terminal'::regclass;");
    REQUIRE(after_delete[0].value() == "0");
    REQUIRE(after_delete[1].value() == "2");
}

TEST_CASE("DOC0371 READY generation inspection rejects invalid inputs before access",
          "[index_admin][doc0371][ready-inspection][negative]") {
    auto ctx = fresh_context({"doc0371_inspect_invalid", "doc0371_inspect_multi"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_inspect_invalid "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_inspect_invalid VALUES "
        "(1, repeat('ACGT', 4096)::bioseq.seq('dna4', 64));");

    auto err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_inspect_invalid'::regclass, 'symbol_counts', 0);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("requires a minimizer index"));

    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_inspect_invalid'::regclass, "
        "'minimizers_per_fragment', -1);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("query key is negative"));

    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_inspect_invalid'::regclass, "
        "'minimizers_per_fragment', 99);");
    REQUIRE(err.sqlstate == "P0002");
    REQUIRE(err.contains("source row is not visible"));

    exec_sql(ctx,
        "CREATE TABLE doc0371_inspect_multi "
        "(id int, a bioseq.seq('dna4', 64), b bioseq.seq('dna4', 64)) "
        "USING bioseq;");
    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_inspect_multi'::regclass, "
        "'minimizers_per_fragment', 0);");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("exactly one bioseq.seq column"));

    exec_sql(ctx, "CREATE TABLE doc0371_inspect_heap(id int);");
    err = sql_error(ctx,
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_inspect_heap'::regclass, 'minimizers_per_fragment', 0);");
    REQUIRE(err.sqlstate == "42809");
    REQUIRE(err.contains("requires a USING bioseq table"));
    exec_sql(ctx, "DROP TABLE doc0371_inspect_heap;");

    // Every ERROR path must release its registered snapshot/read state and leave
    // the libpq session usable.
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0371 READY inspection rejects legacy schema in read-only mode and recovers",
          "[index_admin][doc0371][ready-inspection][schema][cleanup]") {
    auto ctx = fresh_context({"doc0371_inspect_legacy"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_inspect_legacy "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_inspect_legacy VALUES "
        "(1, repeat('ACGT', 4096)::bioseq.seq('dna4', 64));");
    exec_sql(ctx,
        "SELECT bioseq.update_index('doc0371_inspect_legacy'::regclass, "
        "'minimizers_per_fragment');");
    const auto rowno = only_column_row_number(ctx, "doc0371_inspect_legacy");

    exec_sql(ctx, "BEGIN READ ONLY;");
    const auto err = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_inspect_legacy'::regclass, "
        "'minimizers_per_fragment', {});", rowno));
    REQUIRE(err.sqlstate == "55000");
    REQUIRE(err.contains("generation relation schema mismatch"));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0371 live revision-3 PG fixture uses writer token publisher and read session",
          "[index_admin][doc0371][rev3][fixture][writer][reader]") {
    auto ctx = fresh_context({"doc0371_rev3_live"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_rev3_live "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_rev3_live VALUES "
        "(1, (repeat('ACGT', 16387) || 'A')::bioseq.seq('dna4', 64));");
    const auto rowno = only_column_row_number(ctx, "doc0371_rev3_live");

    const auto fixture = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_build_fixture("
        "'doc0371_rev3_live'::regclass, 'minimizers_per_fragment', "
        "{}, 15::smallint, 10::smallint);", rowno));
    REQUIRE(fixture.size() == 36);
    const std::string sidecar = fixture[0].value();
    REQUIRE(!sidecar.empty());
    REQUIRE(std::stoll(fixture[1].value()) > 0);       // generation
    REQUIRE(fixture[2]->size() == 16);                // source identity hex
    REQUIRE(fixture[3]->size() == 16);                // source content CRC
    REQUIRE(fixture[4]->size() == 16);                // compatibility CRC
    REQUIRE(std::stoll(fixture[5].value()) >= 4);     // payload+dir+manifest
    REQUIRE(std::stoll(fixture[6].value()) >= 2);     // real bounded multi-insert batches
    REQUIRE(fixture[7].value() == "64");             // exact published/RETURNING head
    REQUIRE(fixture[8].value() == "1");              // relation opens
    REQUIRE(fixture[9].value() == "3");              // head + metadata + payload scans
    REQUIRE(fixture[10].value() == "1");             // one head scan
    REQUIRE(fixture[11].value() == "1");             // one head row
    REQUIRE(fixture[12].value() == "64");            // fixed head wire
    REQUIRE(fixture[13].value() == "1");             // one manifest/directory scan
    REQUIRE(fixture[14].value() == "1");             // one manifest row
    REQUIRE(std::stoll(fixture[15].value()) > 0);     // directory rows
    REQUIRE(std::stoll(fixture[16].value()) ==
        2 + std::stoll(fixture[15].value()));          // metadata row equation
    REQUIRE(std::stoll(fixture[17].value()) >= 192);  // 64B head + 128B manifest
    REQUIRE(fixture[18].value() == "1");             // one payload group planned
    REQUIRE(fixture[19].value() == "1");             // one payload group executed
    REQUIRE(fixture[20].value() == "3");             // exact three physical fragments
    REQUIRE(fixture[21].value() == "65549");         // deliberately non-aligned source
    REQUIRE(std::stoll(fixture[22].value()) > 0);      // final-fragment padding participates
    REQUIRE(fixture[3].value() == independent_fixed_hex(
        independent_crc64_ecma182(nonaligned_three_fragment_dna4_source())));
    REQUIRE(std::stoll(fixture[23].value()) > 0);      // callback control bytes
    REQUIRE(std::stoll(fixture[24].value()) > 0);      // callback position bytes
    REQUIRE(fixture[25]->size() == 16);                // callback control CRC
    REQUIRE(fixture[26]->size() == 16);                // callback position CRC
    REQUIRE(fixture[27].value() == "1");              // one admitted bulk-read plan
    REQUIRE(fixture[28].value() == "1");              // one physical range scan
    REQUIRE(fixture[29].value() == "2");              // control + position wanted
    REQUIRE(fixture[30].value() == "2");              // control + position read
    REQUIRE(std::stoll(fixture[31].value()) > 0);      // wanted chunks
    REQUIRE(fixture[32] == fixture[31]);               // every wanted chunk delivered
    REQUIRE(std::stoll(fixture[33].value()) ==
        std::stoll(fixture[23].value())
        + std::stoll(fixture[24].value()));            // callback byte equation
    REQUIRE(fixture[34] == fixture[32]);               // one heap row per chunk
    REQUIRE(fixture[35].value() == "0");              // exact range, no skipped row

    const auto schema = query_one(ctx, fmt::format(
        "SELECT string_agg(a.attname || ':' || format_type(a.atttypid,a.atttypmod) "
        "|| ':' || a.attnotnull::int || ':' || a.attstorage::text, ',' ORDER BY a.attnum), "
        "count(DISTINCT i.indexrelid) FILTER (WHERE i.indisprimary)::text, "
        "max(i.indisunique::int)::text, max(i.indimmediate::int)::text, "
        "max((i.indpred IS NULL)::int)::text, "
        "max((i.indexprs IS NULL)::int)::text, "
        "max(i.indkey::text) "
        "FROM pg_attribute a "
        "LEFT JOIN pg_index i ON i.indrelid=a.attrelid AND i.indisprimary "
        "WHERE a.attrelid='bioseq_internal.{}'::regclass "
        "AND a.attnum>0 AND NOT a.attisdropped;",
        sidecar));
    REQUIRE(schema.size() == 7);
    REQUIRE(schema[0].value() ==
        "query_key:bigint:1:p,generation_id:bigint:1:p,"
        "position_segment:integer:1:p,sub_key:bigint:1:p,"
        "chunk_no:integer:1:p,chunk_bytes:integer:1:p,"
        "checksum:bigint:1:p,blob:bytea:1:p");
    REQUIRE(schema[1].value() == "1");
    REQUIRE(schema[2].value() == "1");
    REQUIRE(schema[3].value() == "1");
    REQUIRE(schema[4].value() == "1");
    REQUIRE(schema[5].value() == "1");
    REQUIRE(schema[6].value() == "1 2 3 4 5");

    const auto inspected = query_one(ctx, fmt::format(
        "SELECT generation_id::text, source_identity, "
        "source_content_checksum, compatibility_checksum, "
        "kmer_size::text, window_size::text "
        "FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_live'::regclass, 'minimizers_per_fragment', {});",
        rowno));
    REQUIRE(inspected.size() == 6);
    REQUIRE(inspected[0] == fixture[1]);
    REQUIRE(inspected[1] == fixture[2]);
    REQUIRE(inspected[2] == fixture[3]);
    REQUIRE(inspected[3] == fixture[4]);
    REQUIRE(inspected[4].value() == "15");
    REQUIRE(inspected[5].value() == "10");

    // Exercise the supported fragment-mutation path. Registered fragment rows
    // are immutable, so the helper transactionally invalidates the exact
    // source epoch; a timed proof must reject it without scanning fragments.
    exec_sql(ctx, fmt::format(
        "SELECT bioseq_internal.rev3_test_remove_source_fragment("
        "'doc0371_rev3_live'::regclass, 'minimizers_per_fragment', {}, 2);",
        rowno));
    const auto fragment_error = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_live'::regclass, 'minimizers_per_fragment', {});",
        rowno));
    REQUIRE(fragment_error.sqlstate == "55000");
    REQUIRE(fragment_error.contains("catalog"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    exec_sqlf(ctx, "DROP TABLE bioseq_internal.{};", sidecar);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0371 revision-3 inspection rejects schema mutations and non-owner invocation",
          "[index_admin][doc0371][rev3][schema][acl][cleanup]") {
    auto ctx = fresh_context({"doc0371_rev3_adversarial"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_rev3_adversarial "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_rev3_adversarial VALUES "
        "(1, repeat('ACGT', 1024)::bioseq.seq('dna4', 64));");
    const auto rowno = only_column_row_number(ctx, "doc0371_rev3_adversarial");
    const auto fixture = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_build_fixture("
        "'doc0371_rev3_adversarial'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint);",
        rowno));
    const std::string sidecar = fixture[0].value();

    exec_sqlf(ctx,
        "ALTER TABLE bioseq_internal.{} RENAME COLUMN checksum TO checksum_bad;",
        sidecar);
    auto err = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_adversarial'::regclass, "
        "'minimizers_per_fragment', {});", rowno));
    REQUIRE(err.sqlstate == "55000");
    REQUIRE(err.contains("schema mismatch"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    exec_sqlf(ctx,
        "ALTER TABLE bioseq_internal.{} RENAME COLUMN checksum_bad TO checksum;",
        sidecar);

    exec_sqlf(ctx,
        "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob DROP NOT NULL;",
        sidecar);
    err = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_adversarial'::regclass, "
        "'minimizers_per_fragment', {});", rowno));
    REQUIRE(err.sqlstate == "55000");
    REQUIRE(err.contains("schema mismatch"));
    exec_sqlf(ctx,
        "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET NOT NULL;",
        sidecar);

    exec_sqlf(ctx,
        "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
        sidecar);
    err = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_adversarial'::regclass, "
        "'minimizers_per_fragment', {});", rowno));
    REQUIRE(err.sqlstate == "55000");
    REQUIRE(err.contains("schema mismatch"));
    exec_sqlf(ctx,
        "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE PLAIN;",
        sidecar);

    // PostgreSQL cannot create a partial/expression PRIMARY KEY through DDL.
    // Create real donor indexes, transactionally copy their parsed pg_node_tree
    // shape onto the actual primary pg_index row, and prove the PG17 inspector
    // rejects each exact catalog state. ROLLBACK restores the valid catalogue.
    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx,
        "CREATE UNIQUE INDEX doc0371_rev3_partial_donor ON "
        "bioseq_internal.{} "
        "(query_key,generation_id,position_segment,sub_key,chunk_no) "
        "WHERE query_key >= 0;",
        sidecar);
    exec_sqlf(ctx,
        "SELECT bioseq_internal.rev3_test_copy_primary_index_shape("
        "'bioseq_internal.{}'::regclass, "
        "'bioseq_internal.doc0371_rev3_partial_donor'::regclass, 'partial');",
        sidecar);
    err = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_adversarial'::regclass, "
        "'minimizers_per_fragment', {});", rowno));
    REQUIRE(err.sqlstate == "55000");
    REQUIRE(err.contains("partial"));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx,
        "CREATE UNIQUE INDEX doc0371_rev3_expression_donor ON "
        "bioseq_internal.{} "
        "((query_key + 0),generation_id,position_segment,sub_key,chunk_no);",
        sidecar);
    exec_sqlf(ctx,
        "SELECT bioseq_internal.rev3_test_copy_primary_index_shape("
        "'bioseq_internal.{}'::regclass, "
        "'bioseq_internal.doc0371_rev3_expression_donor'::regclass, 'expression');",
        sidecar);
    err = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_adversarial'::regclass, "
        "'minimizers_per_fragment', {});", rowno));
    REQUIRE(err.sqlstate == "55000");
    REQUIRE(err.contains("expression"));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    exec_sql(ctx, "DROP ROLE IF EXISTS doc0371_rev3_nonowner;");
    exec_sql(ctx, "CREATE ROLE doc0371_rev3_nonowner;");
    exec_sql(ctx,
        "GRANT USAGE ON SCHEMA bioseq TO doc0371_rev3_nonowner; "
        "GRANT EXECUTE ON FUNCTION "
        "bioseq.inspect_ready_minimizer_generation(regclass,text,bigint) "
        "TO doc0371_rev3_nonowner;");
    exec_sql(ctx, "SET ROLE doc0371_rev3_nonowner;");
    const auto catalog_tamper = sql_error(ctx,
        "UPDATE bioseq_internal.sequence_source_version "
        "SET state=1 WHERE user_relid="
        "'doc0371_rev3_adversarial'::regclass;");
    REQUIRE(catalog_tamper.sqlstate == "42501");
    err = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_adversarial'::regclass, "
        "'minimizers_per_fragment', {});", rowno));
    REQUIRE(err.sqlstate == "42501");
    exec_sql(ctx, "RESET ROLE;");
    exec_sql(ctx,
        "REVOKE EXECUTE ON FUNCTION "
        "bioseq.inspect_ready_minimizer_generation(regclass,text,bigint) "
        "FROM doc0371_rev3_nonowner; "
        "REVOKE USAGE ON SCHEMA bioseq FROM doc0371_rev3_nonowner;");
    exec_sql(ctx, "DROP ROLE doc0371_rev3_nonowner;");

    exec_sqlf(ctx, "DROP TABLE bioseq_internal.{};", sidecar);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0371 source decode rejects self-consistent head and manifest corruption",
          "[index_admin][doc0371][rev3][source-checksum][corruption]") {
    auto ctx = fresh_context({"doc0371_rev3_source_crc"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_rev3_source_crc "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_rev3_source_crc VALUES "
        "(1, repeat('ACGT', 2048)::bioseq.seq('dna4', 64));");
    const auto rowno = only_column_row_number(ctx, "doc0371_rev3_source_crc");
    const auto fixture = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_build_fixture("
        "'doc0371_rev3_source_crc'::regclass, "
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint);",
        rowno));
    const std::string sidecar = fixture[0].value();

    // Rewrites both fixed wires, the head's manifest checksum, and both row
    // CRCs. Metadata is internally self-consistent; only decoding the visible
    // source bases and comparing the independently derived CRC can reject it.
    exec_sqlf(ctx,
        "SELECT bioseq_internal.rev3_test_rewrite_source_checksum("
        "'bioseq_internal.{}'::regclass, {}, 1);",
        sidecar, rowno);
    const auto err = sql_error(ctx, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_source_crc'::regclass, "
        "'minimizers_per_fragment', {});", rowno));
    REQUIRE(err.contains("source attestation"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    exec_sqlf(ctx, "DROP TABLE bioseq_internal.{};", sidecar);
}

TEST_CASE("DOC0371 source proof shares MVCC visibility and rejects post-snapshot drift",
          "[index_admin][doc0371][rev3][mvcc][source-drift]") {
    auto reader = fresh_context({"doc0371_rev3_mvcc"});
    connection_test_context writer;
    exec_sql(reader,
        "CREATE TABLE doc0371_rev3_mvcc "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(reader,
        "INSERT INTO doc0371_rev3_mvcc VALUES "
        "(1, repeat('ACGT', 2048)::bioseq.seq('dna4', 64));");
    const auto rowno = only_column_row_number(reader, "doc0371_rev3_mvcc");
    const auto fixture = query_one(reader, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_build_fixture("
        "'doc0371_rev3_mvcc'::regclass, 'minimizers_per_fragment', "
        "{}, 15::smallint, 10::smallint);", rowno));
    const std::string sidecar = fixture[0].value();

    exec_sql(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ;");
    const auto before = query_one(reader, fmt::format(
        "SELECT generation_id::text, source_content_checksum "
        "FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_mvcc'::regclass, 'minimizers_per_fragment', {});",
        rowno));
    exec_sql(writer, "SET bioseq.update_index_on_insert = off;");
    exec_sql(writer,
        "UPDATE doc0371_rev3_mvcc SET "
        "seq=repeat('TGCA', 2048)::bioseq.seq('dna4', 64) WHERE id=1;");
    const auto stable_snapshot = query_one(reader, fmt::format(
        "SELECT generation_id::text, source_content_checksum "
        "FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_mvcc'::regclass, 'minimizers_per_fragment', {});",
        rowno));
    REQUIRE(stable_snapshot == before);
    exec_sql(reader, "COMMIT;");

    const auto drift = sql_error(reader, fmt::format(
        "SELECT * FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_mvcc'::regclass, 'minimizers_per_fragment', {});",
        rowno));
    REQUIRE(drift.contains("source row is not visible"));
    REQUIRE(query_scalar_int(reader, "SELECT 1;") == 1);
    exec_sqlf(reader, "DROP TABLE bioseq_internal.{};", sidecar);
}

TEST_CASE("DOC0371 revision-3 read session fails closed and restores backend resources",
          "[index_admin][doc0371][rev3][session][permit][cleanup]") {
    auto ctx = fresh_context({"doc0371_rev3_session"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_rev3_session "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_rev3_session VALUES "
        "(1, repeat('ACGT', 2048)::bioseq.seq('dna4', 64));");
    const auto rowno = only_column_row_number(ctx, "doc0371_rev3_session");
    const auto fixture = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_build_fixture("
        "'doc0371_rev3_session'::regclass, 'minimizers_per_fragment', "
        "{}, 15::smallint, 10::smallint);", rowno));
    const std::string sidecar = fixture[0].value();

    const std::array<std::pair<std::string_view, std::string_view>, 4> failures{{
        {"repeated_head", "more than once or out of order"},
        {"repeated_metadata", "repeated, early, or changed"},
        {"early_payload", "before metadata"},
        {"permit_denied", "memory admission was denied"},
    }};
    for (const auto& [mode, message] : failures) {
        const auto err = sql_error(ctx, fmt::format(
            "SELECT bioseq_internal.rev3_test_session_failure("
            "'doc0371_rev3_session'::regclass, "
            "'minimizers_per_fragment', {}, '{}');",
            rowno, mode));
        REQUIRE(err.contains(message));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    }

    // The permit-denial path restored the backend-global admission cap and all
    // borrowed snapshot/relation state; a full authenticated reopen succeeds.
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_session'::regclass, 'minimizers_per_fragment', {});",
        rowno)) == 1);
    exec_sqlf(ctx, "DROP TABLE bioseq_internal.{};", sidecar);
}

TEST_CASE("update_index('minimizers_per_fragment') builds the per-fragment index",
          "[index_admin][update][backfill]") {
    auto ctx = fresh_context({kTable});
    // seed_small_bioseq_column sets bioseq.update_index_on_insert = on, so the two
    // default UPDATE_ON_INSERT indexes (minimizers_per_fragment, symbol_counts)
    // are populated incrementally at INSERT time.
    seed_small_bioseq_column(ctx, kTable);

    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", kTable) > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", kTable) > 0);

    // Explicit rebuild of the per-fragment index returns the count of
    // sequences (re)indexed — three seeded rows.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);
}

TEST_CASE("update_index rejects an unknown index name",
          "[index_admin][update][error]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, 'NOT_A_KIND');",
                    kTable)));
}

TEST_CASE("ExactSimd update_index accepts DNA5 without scalar fallback",
          "[index_admin][exact-simd][dna5]") {
    constexpr std::string_view table = "test_index_admin_exact_dna5";
    auto ctx = fresh_context({table});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    exec_sqlf(ctx,
        "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna5', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES (1, "
        "(repeat('ACGT', 256) || 'N' || repeat('TGCA', 256) || "
        "'N' || repeat('GATTACA', 128))::bioseq.seq('dna5', 64));",
        table);
    exec_sql(ctx,
        "SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';");
    exec_sql(ctx,
        "SET bioseq.aligner.chaining_algorithm = 'simd_mm2';");
    exec_sql(ctx,
        "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');",
        table)) == 1);

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table)) == 1);
}

TEST_CASE("update_index accepts text and explicit regclass for the table arg",
          "[index_admin][update][regclass]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    // Quoted text form — PG implicitly casts text -> regclass via regclassin.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}', "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);

    // Explicit ::regclass cast form.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);
}

TEST_CASE("update_index rejects non-bioseq-AM tables",
          "[index_admin][update][error][heap]") {
    constexpr std::string_view kHeapTable = "test_index_admin_heap_t";
    auto ctx = fresh_context({kHeapTable});

    // Plain heap-AM table with a bioseq.seq column. The chunked-storage
    // reindex path is only reachable from the bioseq columnar AM, so
    // update_index must refuse a heap relation.
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kHeapTable),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = false,
        },
        {seq_row{.pk = 1, .sequences = {"ACGTACGTACGTACGT"}}});

    REQUIRE(sql_fails(
        ctx,
        fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                    "'minimizers_per_fragment');",
                    kHeapTable)));
}

TEST_CASE("set_index_policy toggles update_policy and supports per-sequence on-insert",
          "[index_admin][policy]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    // Helper: read the current update_policy of one index from list_metadata.
    auto policy_of = [&](std::string_view index_name) {
        return query_scalar(
            ctx,
            fmt::format("SELECT update_policy FROM bioseq.list_metadata() "
                        " WHERE table_name = '{}'::regclass "
                        "   AND name = '{}' LIMIT 1;",
                        kTable, index_name));
    };

    // symbol_counts defaults to UPDATE_ON_INSERT; flip it to UPDATE_MANUALLY.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, 'symbol_counts', "
              "'UPDATE_MANUALLY');",
              kTable);
    REQUIRE(policy_of("symbol_counts") == "UPDATE_MANUALLY");

    // Flip it back to UPDATE_ON_INSERT.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, 'symbol_counts', "
              "'UPDATE_ON_INSERT');",
              kTable);
    REQUIRE(policy_of("symbol_counts") == "UPDATE_ON_INSERT");

    // minimizers_per_sequence is opt-in UPDATE_ON_INSERT. This must be
    // accepted so recompute_async_persist and batch load can honor the table
    // policy instead of hard-wiring minimizers_per_fragment.
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, "
              "'minimizers_per_fragment', 'UPDATE_MANUALLY');",
              kTable);
    exec_sqlf(ctx,
              "SELECT bioseq.set_index_policy('{}'::regclass, "
              "'minimizers_per_sequence', 'UPDATE_ON_INSERT');",
              kTable);
    REQUIRE(policy_of("minimizers_per_fragment") == "UPDATE_MANUALLY");
    REQUIRE(policy_of("minimizers_per_sequence") == "UPDATE_ON_INSERT");
}

TEST_CASE("UPDATE_ON_INSERT can build minimizers_per_sequence without per-fragment fallback",
          "[index_admin][policy][insert_hook]") {
    auto ctx = fresh_context({"test_index_admin_mps"});
    exec_sql(ctx, "CREATE TABLE test_index_admin_mps "
                  "(header text, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx, "SELECT bioseq.set_index_policy("
                  "'test_index_admin_mps'::regclass, "
                  "'minimizers_per_fragment', 'UPDATE_MANUALLY');");
    exec_sql(ctx, "SELECT bioseq.set_index_policy("
                  "'test_index_admin_mps'::regclass, "
                  "'minimizers_per_sequence', 'UPDATE_ON_INSERT');");
    exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    exec_sql(ctx, "INSERT INTO test_index_admin_mps VALUES "
                  "('mps', repeat('ACGT', 2000)::bioseq.seq('dna4', 64));");

    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment",
                                  "test_index_admin_mps") == 0);
    REQUIRE(index_table_row_count(ctx, "minimizers_per_sequence",
                                  "test_index_admin_mps") > 0);
}

TEST_CASE("update_index('minimizers_per_fragment') is idempotent at the content level",
          "[index_admin][update][idempotent]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    // Run the rebuild twice; the per-fragment chunk row count must be stable
    // (the rebuild clears the index then repopulates from identical input).
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);
    const auto after_first =
        index_table_row_count(ctx, "minimizers_per_fragment", kTable);

    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 3);
    const auto after_second =
        index_table_row_count(ctx, "minimizers_per_fragment", kTable);

    REQUIRE(after_first > 0);
    REQUIRE(after_first == after_second);
}

TEST_CASE("update_index on an empty bioseq table returns 0 and list_metadata "
          "still lists kinds",
          "[index_admin][update][empty]") {
    auto ctx = fresh_context({kTable});
    // Create an empty USING bioseq table (no rows).
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) "
              "USING bioseq;",
              kTable);

    // No rows -> no sequences (re)indexed -> 0.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.update_index('{}'::regclass, "
                            "'minimizers_per_fragment');",
                            kTable)) == 0);

    // list_metadata still lists the three production kinds with size '0 bytes'.
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT name, size FROM bioseq.list_metadata() "
                    " WHERE table_name = '{}'::regclass "
                    "   AND name <> 'test_table_rowcount' "
                    " ORDER BY name;",
                    kTable));
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0][0].value() == "minimizers_per_fragment");
    REQUIRE(rows[1][0].value() == "minimizers_per_sequence");
    REQUIRE(rows[2][0].value() == "symbol_counts");
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[1].value() == "0 bytes");
    }
}
