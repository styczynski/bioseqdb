/**
 * @file index_admin_test.cpp
 * @brief Exercises metadata catalog, allocator, and publication contracts.
 */

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
import bioseq.tests.integration.index_admin_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::index_admin_test_support;

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
    const auto build = [&ctx, table, rowno] {
        return query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.build_ready_minimizer_fragment_generation("
            "'{}'::regclass,'minimizers_per_fragment',{},'scalar');",
            table, rowno));
    };
    const auto descriptors =
        [&ctx, table, rowno](const std::string& sidecar) {
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
