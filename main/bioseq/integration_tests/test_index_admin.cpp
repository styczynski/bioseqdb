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
#include <chrono>
#include <libpq-fe.h>
#include <thread>
#include <utility>

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

void require_bioseq_shared_preload(const connection_test_context& ctx) {
    const auto proof = query_one(ctx,
        "SELECT EXISTS ("
        "SELECT 1 FROM unnest(string_to_array("
        "current_setting('shared_preload_libraries', true), ',')) AS entry "
        "WHERE btrim(entry) IN ('bioseq', '$libdir/bioseq'))::text, "
        "supervisor::text FROM bioseq.scheduler_stats();");
    REQUIRE(proof.size() == 2);
    INFO("ordinary bioseq DROP preflight requires bioseq in "
         "shared_preload_libraries and attached scheduler shared state; "
         "observed configured=" << proof[0].value()
         << " supervisor=" << proof[1].value());
    REQUIRE(proof[0].value() == "true");
    REQUIRE(proof[1].value() == "true");
}

[[nodiscard]] std::string drop_preservation_probe_literal() {
    return format_literal(
        kHoeProbeSequence,
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
    // Pin the original relation identity, then prove its READY locator and
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
            == std::to_string(kHoeProbeSequence.size()));
    REQUIRE(source[6].value() == "1");

    const auto& hydrated = evidence.hydrated;
    REQUIRE(hydrated.size() == 1);
    REQUIRE(hydrated[0].size() == 2);
    REQUIRE(hydrated[0][0].value() == "1");
    REQUIRE(hydrated[0][1].value() == kHoeProbeSequence);
}

void require_preserved_ready_bioseq_source(
        const connection_test_context& ctx,
        std::string_view table,
        const long long expected_oid) {
    require_preserved_ready_bioseq_source_evidence(
        capture_preserved_ready_bioseq_source(ctx, table, expected_oid),
        expected_oid);
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

    // This connection starts without prior extension SQL. The proof helper now
    // calls scheduler_stats() deliberately: `supervisor=true` attests the
    // already-attached shared state rather than merely trusting GUC spelling.
    // The supported correctness contract requires that preload state to predate
    // this top-level DROP. A first-command lazy load cannot retroactively wrap
    // an already-active utility command; this is a hard test precondition, not
    // a path to skip or accept with weaker assertions.
    connection_test_context drop_ctx;
    require_bioseq_shared_preload(drop_ctx);
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
        "INSERT INTO {} VALUES (1, {});",
        recreated, drop_preservation_probe_literal());
    const long long recreated_oid = query_scalar_int(drop_ctx, fmt::format(
        "SELECT '{}'::regclass::oid::bigint;", recreated));
    const auto recreated_evidence = capture_preserved_ready_bioseq_source(
        drop_ctx, recreated, recreated_oid);
    exec_sqlf(drop_ctx, "DROP TABLE {};", recreated);

    REQUIRE(removed_objects == 0);
    REQUIRE(extension_after_drop == 0);
    REQUIRE(extension_after_create == 1);
    REQUIRE(teardown_diagnostics.size() == 2);
    const auto fallback_after = std::stoll(teardown_diagnostics[0].value());
    const auto missing_after = std::stoll(teardown_diagnostics[1].value());
    // Preloaded backends enter through our ProcessUtility hook and never need
    // the late extension-lock fallback.
    REQUIRE(fallback_after == 0);
    // Attested extension teardown leaves the complete dependency list to
    // PostgreSQL and must never manufacture stale/missing OID observations.
    REQUIRE(missing_after == 0);
    require_preserved_ready_bioseq_source_evidence(
        recreated_evidence, recreated_oid);
}

TEST_CASE("ordinary drops fail closed on missing or malformed source catalogs",
          "[index_admin][source-version][drop][catalog-contract]") {
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
    // The following missing-catalog probe must still fail closed, proving the
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
          "[index_admin][source-version][drop][catalog-contract][inheritance]") {
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
          "[index_admin][source-version][drop][catalog-contract][partition]") {
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
          "[index_admin][drop][partition][lock-order]") {
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
    const auto inside_rollback_savepoint = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM doc0371_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_speculative'::regclass;");
    REQUIRE(inside_rollback_savepoint[0].value() == "0");
    REQUIRE(inside_rollback_savepoint[1].value() == "3");
    REQUIRE(inside_rollback_savepoint[2].value() == "0");
    REQUIRE(inside_rollback_savepoint[3].value() == "3");
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

    // The symmetric savepoint-release path runs PRE_COMMIT_SUB rather than
    // ABORT_SUB. Both callbacks close transaction-scoped write-side shard
    // caches, so prove that a READY source and its parent row survive release
    // and outer commit without a ResourceOwner mismatch.
    exec_sql(ctx, "BEGIN; SAVEPOINT source_speculative_commit;");
    exec_sql(ctx,
        "INSERT INTO doc0371_source_speculative VALUES "
        "(4, 'TTTT'::bioseq.seq('dna4', 64)) ON CONFLICT (id) DO NOTHING;");
    const auto inside_commit_savepoint = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM doc0371_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_speculative'::regclass;");
    REQUIRE(inside_commit_savepoint[0].value() == "0");
    REQUIRE(inside_commit_savepoint[1].value() == "3");
    REQUIRE(inside_commit_savepoint[2].value() == "0");
    REQUIRE(inside_commit_savepoint[3].value() == "3");
    exec_sql(ctx, "RELEASE SAVEPOINT source_speculative_commit; COMMIT;");
    const auto after_commit_savepoint = query_one(ctx,
        "SELECT count(*) FILTER (WHERE state=0)::text, "
        "count(*) FILTER (WHERE state=1)::text, "
        "count(*) FILTER (WHERE state=2)::text, "
        "(SELECT count(*) FROM doc0371_source_speculative)::text "
        "FROM bioseq_internal.sequence_source_version "
        "WHERE user_relid='doc0371_source_speculative'::regclass;");
    REQUIRE(after_commit_savepoint[0].value() == "0");
    REQUIRE(after_commit_savepoint[1].value() == "3");
    REQUIRE(after_commit_savepoint[2].value() == "0");
    REQUIRE(after_commit_savepoint[3].value() == "3");
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
        "SELECT physical_index_relations::text, persisted_index_rows::text, "
        "generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'doc0371_no_index_absence'::regclass, 'minimizers_per_fragment');");
    REQUIRE(present.size() == 3);
    REQUIRE(std::stoll(present[0].value()) > 0);
    REQUIRE(std::stoll(present[1].value()) > 0);
    // The live writer is still the legacy five-column format. The audit must
    // count its rows without inventing a revision-3 READY head.
    REQUIRE(present[2].value() == "0");

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

TEST_CASE("DOC0371 generation storage rebinds one handle after speculative reset rollback",
          "[index_admin][doc0371][generation-storage][savepoint][oid]") {
    constexpr std::string_view table = "doc0371_generation_savepoint";
    auto ctx = fresh_context({table});
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT', 2048)::bioseq.seq('dna4', 64));",
        table);
    exec_sqlf(ctx,
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');",
        table);
    const auto rowno = only_column_row_number(ctx, table);

    const auto state = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'savepoint_rollback');",
        table, rowno));
    REQUIRE(state.size() == 7);
    REQUIRE(state[0].has_value());
    REQUIRE(state[1].has_value());
    REQUIRE(state[2].has_value());
    const long long legacy_oid = std::stoll(state[0].value());
    const long long speculative_oid = std::stoll(state[1].value());
    const long long restored_oid = std::stoll(state[2].value());
    REQUIRE(legacy_oid > 0);
    REQUIRE(speculative_oid > 0);
    REQUIRE(speculative_oid != legacy_oid);
    REQUIRE(restored_oid == legacy_oid);
    REQUIRE(state[3].value() == "t");
    REQUIRE(state[4].value() == "f");
    REQUIRE_FALSE(state[5].has_value());
    REQUIRE_FALSE(state[6].has_value());

    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT count(*) FROM pg_class WHERE oid={}::oid;",
        speculative_oid)) == 0);
    const auto legacy_shape = query_one(ctx, fmt::format(
        "SELECT string_agg(a.attname || ':' || "
        "format_type(a.atttypid,a.atttypmod) || ':' || "
        "a.attnotnull::int, ',' ORDER BY a.attnum) "
        "FROM pg_attribute a WHERE a.attrelid={}::oid "
        "AND a.attnum>0 AND NOT a.attisdropped;",
        restored_oid));
    REQUIRE(legacy_shape.size() == 1);
    REQUIRE(legacy_shape[0].value() ==
        "query_key:bigint:1,sub_key:bigint:1,chunk_no:integer:1,"
        "chunk_bytes:integer:1,blob:bytea:1");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0371 committed generation reset evicts both residency forms in another backend",
          "[index_admin][doc0371][generation-storage][residency][sinval]") {
    constexpr std::string_view table = "doc0371_generation_residency";
    auto setup = fresh_context({table});
    exec_sqlf(setup,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(setup,
        "INSERT INTO {} VALUES "
        "(1, repeat('TGCA', 2048)::bioseq.seq('dna4', 64));",
        table);
    exec_sqlf(setup,
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_fragment');",
        table);
    const auto rowno = only_column_row_number(setup, table);

    connection_test_context session_a;
    connection_test_context session_b;
    const long long session_a_pid = query_scalar_int(
        session_a, "SELECT pg_backend_pid();");
    const long long session_b_pid = query_scalar_int(
        session_b, "SELECT pg_backend_pid();");
    REQUIRE(session_a_pid != session_b_pid);

    const auto warmed = query_one(session_a, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'warm_legacy_residency');",
        table, rowno));
    REQUIRE(warmed.size() == 7);
    REQUIRE(warmed[0].has_value());
    REQUIRE_FALSE(warmed[1].has_value());
    REQUIRE(warmed[2] == warmed[0]);
    REQUIRE_FALSE(warmed[3].has_value());
    REQUIRE(warmed[4].value() == "f");
    REQUIRE(warmed[5].value() == "t");
    REQUIRE(warmed[6].value() == "t");

    exec_sql(session_b, "BEGIN;");
    const auto ensured = query_one(session_b, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'ensure_generation');",
        table, rowno));
    // Drain the committing command before asserting anything while it owns the
    // old sidecar's AccessExclusiveLock.
    exec_sql(session_b, "COMMIT;");
    REQUIRE(ensured.size() == 7);
    REQUIRE(ensured[0] == warmed[0]);
    REQUIRE(ensured[1].has_value());
    REQUIRE(ensured[2] == ensured[1]);
    REQUIRE(ensured[1] != warmed[0]);
    REQUIRE_FALSE(ensured[3].has_value());
    REQUIRE(ensured[4].value() == "t");
    REQUIRE_FALSE(ensured[5].has_value());
    REQUIRE_FALSE(ensured[6].has_value());

    // No command runs in session A between warming and this probe.  The helper
    // explicitly drains sinval before looking up the exact same two keys.
    const auto probed = query_one(session_a, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'probe_residency');",
        table, rowno));
    REQUIRE(probed.size() == 7);
    REQUIRE(probed[0] == ensured[2]);
    REQUIRE_FALSE(probed[1].has_value());
    REQUIRE(probed[2] == ensured[2]);
    REQUIRE_FALSE(probed[3].has_value());
    REQUIRE(probed[4].value() == "t");
    REQUIRE(probed[5].value() == "f");
    REQUIRE(probed[6].value() == "f");
    REQUIRE(query_scalar_int(session_a, "SELECT pg_backend_pid();") ==
        session_a_pid);
}

TEST_CASE("DOC0371 generation clear savepoint restores relation head and exact age row",
          "[index_admin][doc0371][generation-clear][savepoint][rollback]") {
    constexpr std::string_view table = "doc0371_generation_clear_rollback";
    auto ctx = fresh_context({table});
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
        "bioseq_internal.rev3_test_build_ready_mpf_generation("
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
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
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
    REQUIRE(age_before.size() == 3);

    const auto cleared = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
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

TEST_CASE("DOC0371 committed generation clear drops every sidecar and evicts another backend",
          "[index_admin][doc0371][generation-clear][commit][sinval][idempotent]") {
    constexpr std::string_view table = "doc0371_generation_clear_commit";
    auto setup = fresh_context({table});
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
        "bioseq_internal.rev3_test_build_ready_mpf_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'scalar');",
        table, rowno_a));
    const auto built_b = query_one(setup, fmt::format(
        "SELECT * FROM "
        "bioseq_internal.rev3_test_build_ready_mpf_generation("
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
    REQUIRE(collateral_age_before.size() == 2);

    connection_test_context session_a;
    connection_test_context session_b;
    const long long session_a_pid = query_scalar_int(
        session_a, "SELECT pg_backend_pid();");
    REQUIRE(session_a_pid != query_scalar_int(
        session_b, "SELECT pg_backend_pid();"));
    const auto warmed = query_one(session_a, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
        "'{}'::regclass, 'minimizers_per_fragment', {}, "
        "'warm_generation_residency');",
        table, rowno_a));
    REQUIRE(warmed.size() == 7);
    REQUIRE(warmed[4].value() == "t");
    REQUIRE(warmed[5].value() == "t");
    REQUIRE(warmed[6].value() == "t");

    exec_sql(session_b, "BEGIN;");
    const auto cleared = query_one(session_b, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
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
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
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
        "SELECT * FROM bioseq_internal.rev3_test_generation_storage_state("
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
        table)).size() == 2);
    exec_sqlf(session_b,
        "DROP TABLE bioseq_internal.{};", commit_lookalike);
    REQUIRE(query_scalar_int(session_b, "SELECT 1;") == 1);
}

TEST_CASE("DOC0371 inactive MPF coordinator executes real scalar and SIMD PostgreSQL scanners",
          "[index_admin][doc0371][rev4][coordinator][scalar][simd][route-proof]") {
    constexpr std::string_view table = "doc0371_ready_mpf_coordinator";
    auto ctx = fresh_context({table});
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
        "scalar", "exact_simd", "paper_nthash_simd"};
    std::array<row_t, backends.size()> observations{};
    long long previous_generation = 0;
    for (std::size_t backend_index = 0;
         backend_index < backends.size(); ++backend_index) {
        const std::string_view backend = backends[backend_index];
        row_t& observed = observations[backend_index];
        observed = query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.rev3_test_build_ready_mpf_generation("
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
        REQUIRE(observed[7].value() == "4");
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
            std::stoll(observed[27].value()) + 2);
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
        "bioseq_internal.rev3_test_build_ready_mpf_generation("
        "'{}'::regclass, 'minimizers_per_fragment', {}, 'invalid');",
        table, rowno));
    REQUIRE(fmgr_error.contains("backend is invalid"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0371 inactive MPS coordinator publishes proof-bound PostgreSQL spill generations",
          "[index_admin][doc0371][rev4][coordinator][mps][spill][route-proof]") {
    constexpr std::string_view table = "doc0371_ready_mps_coordinator";
    auto ctx = fresh_context({table});
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
        "scalar", "exact_simd", "paper_nthash_simd"};
    const bool hardware_vulkan_available = query_scalar(
        ctx,
        "SELECT available AND NOT software_device "
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
            "bioseq_internal.rev3_test_build_ready_mps_generation("
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
        REQUIRE(observed[7].value() == "4");
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
        if (backend == "paper_nthash_simd") {
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

        REQUIRE(replay_scans == replay_reroutes + 2);
        REQUIRE(std::stoll(observed[54].value()) == replay_scans);
        REQUIRE(observed[56].value() == "2");
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
        } else if (backend == "paper_nthash_simd") {
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
        "bioseq_internal.rev3_test_build_ready_mps_generation("
        "'{}'::regclass, 'minimizers_per_sequence', {}, 'invalid');",
        table, rowno));
    REQUIRE(fmgr_error.contains("backend is invalid"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0371 bridged PostgreSQL build and publication leaves preserve READY state",
          "[index_admin][doc0371][rev4][coordinator][mps][pg-error][recovery]") {
    constexpr std::string_view table = "doc0371_ready_mps_pg_recovery";
    auto ctx = fresh_context({table});
    exec_sqlf(ctx,
        "CREATE TABLE {} (id int, seq bioseq.seq('dna4', 64)) USING bioseq;",
        table);
    exec_sqlf(ctx,
        "INSERT INTO {} VALUES "
        "(1, (repeat('ACGT', 16391) || 'A')::bioseq.seq('dna4', 64));",
        table);
    const auto rowno = only_column_row_number(ctx, table);

    struct FailureProbe {
        std::string_view backend;
        std::string_view sqlstate;
        std::string_view diagnostic;
    };
    constexpr std::array probes{
        FailureProbe{"scalar_fail_scanner_interrupt_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 1"},
        FailureProbe{"scalar_fail_scanner_interrupt_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 2"},
        FailureProbe{"scalar_fail_scanner_row_read_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 3"},
        FailureProbe{"scalar_fail_scanner_row_read_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 4"},
        FailureProbe{"scalar_fail_scanner_palloc_before", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 5"},
        FailureProbe{"scalar_fail_scanner_palloc_after", "XX000",
            "injected sequence scanner PostgreSQL leaf failure\nDETAIL: failure point 6"},
        FailureProbe{"scalar_fail_sorted_create_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 1"},
        FailureProbe{"scalar_fail_sorted_create_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 2"},
        // The fixture's 65,565 bp row spans three 32-Kbp scanner cores. Its
        // test-only 64-KiB merge-reader budget therefore compacts three input
        // runs into two outputs. Failing output creation #2 proves output #1
        // completed and its input handles closed before the run store became
        // permanently poisoned; the loop below then proves fresh-txn recovery.
        FailureProbe{"scalar_fail_sorted_compact_create_before_nth2", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 15"},
        FailureProbe{"scalar_fail_sorted_write_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 3"},
        FailureProbe{"scalar_fail_sorted_write_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 4"},
        FailureProbe{"scalar_fail_sorted_read_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 5"},
        FailureProbe{"scalar_fail_sorted_read_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 6"},
        FailureProbe{"scalar_fail_sorted_seek_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 7"},
        FailureProbe{"scalar_fail_sorted_seek_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 8"},
        FailureProbe{"scalar_fail_sorted_size_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 9"},
        FailureProbe{"scalar_fail_sorted_size_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 10"},
        FailureProbe{"scalar_fail_sorted_close_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 11"},
        FailureProbe{"scalar_fail_sorted_close_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 12"},
        FailureProbe{"scalar_fail_sorted_interrupt_before", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 13"},
        FailureProbe{"scalar_fail_sorted_interrupt_after", "XX000",
            "injected sorted-run PostgreSQL leaf failure\nDETAIL: failure point 14"},
        FailureProbe{"scalar_fail_pg_spill", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=mps_spill\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_merge", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=mps_merge_callback\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_tts_create", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_tts_create\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_tuple_form", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_tuple_form\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_tts_store", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_tts_store\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_tts_drop", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_tts_drop\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_multi_insert", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_multi_insert\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_reread", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_reread\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_spi_publish", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_spi_publish\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_writer_close", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_writer_close\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_storage_close", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_storage_close\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_post_publish_reread", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_post_publish_reread\nHINT: retry the failed index build in a fresh transaction"},
        FailureProbe{"scalar_fail_pg_reader_close", "58030",
            "injected revision-3 PostgreSQL failure\nDETAIL: failure_point=generation_reader_close\nHINT: retry the failed index build in a fresh transaction"},
    };

    const auto build = [&ctx, rowno](std::string_view backend) {
        return query_one(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.rev3_test_build_ready_mps_generation("
            "'doc0371_ready_mps_pg_recovery'::regclass, "
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

    for (const FailureProbe& probe : probes) {
        const auto error = sql_error(ctx, fmt::format(
            "SELECT * FROM "
            "bioseq_internal.rev3_test_build_ready_mps_generation("
            "'{}'::regclass, 'minimizers_per_sequence', {}, '{}');",
            table, rowno, probe.backend));
        REQUIRE(error.sqlstate == probe.sqlstate);
        REQUIRE(error.message == probe.diagnostic);
        // Recovery is deliberately a fresh implicit transaction. A poisoned
        // scanner, run store, row sink, rereader, or SPI publisher must never
        // be resumed in-place. Exact equality includes the generation-0 head,
        // all immutable generation rows, and every blob byte; it therefore
        // also proves the failed statement left no orphan generation rows.
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        REQUIRE(capture_generation_family(baseline_sidecar)
            == baseline_family);
        REQUIRE(query_scalar_int(ctx, fmt::format(
            "SELECT count(*) FROM bioseq_internal.{} "
            "WHERE query_key={} AND generation_id=0;",
            baseline_sidecar, rowno)) == 1);

        // A successful publication after every individual probe proves the
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

TEST_CASE("DOC0371 live PG fixture compares format-3 and format-4 physical storage",
          "[index_admin][doc0371][rev3][rev4][fixture][writer][reader]") {
    auto ctx = fresh_context({"doc0371_rev3_live"});
    exec_sql(ctx,
        "CREATE TABLE doc0371_rev3_live "
        "(id int, seq bioseq.seq('dna4', 64)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0371_rev3_live VALUES "
        "(1, (repeat('ACGT', 16387) || 'A')::bioseq.seq('dna4', 64));");
    const auto rowno = only_column_row_number(ctx, "doc0371_rev3_live");

    struct FormatObservation {
        row_t build;
        row_t read;
        std::string sidecar;
        long long heap_bytes = 0;
        long long table_bytes = 0;
        long long total_bytes = 0;
        long long payload_bytes = 0;
    };
    std::array<FormatObservation, 2> observed{};
    constexpr std::array<std::int16_t, 2> wire_formats{3, 4};
    constexpr long long position_extent_base = 1LL << 61;
    const long long builder_pid = query_scalar_int(ctx, "SELECT pg_backend_pid();");

    for (std::size_t format_index = 0;
         format_index < wire_formats.size(); ++format_index) {
        const std::int16_t wire_format = wire_formats[format_index];
        FormatObservation& sample = observed[format_index];
        sample.build = query_one(ctx, fmt::format(
            "SELECT * FROM bioseq_internal.rev3_test_build_fixture("
            "'doc0371_rev3_live'::regclass, 'minimizers_per_fragment', "
            "{}, 15::smallint, 10::smallint, {}::smallint);",
            rowno, wire_format));
        REQUIRE(sample.build.size() == 20);
        sample.sidecar = sample.build[0].value();
        REQUIRE(!sample.sidecar.empty());
        REQUIRE(sample.build[1].value() == std::to_string(wire_format));
        REQUIRE(std::stoll(sample.build[2].value()) > 0);
        REQUIRE(sample.build[3]->size() == 16);
        REQUIRE(sample.build[4]->size() == 16);
        REQUIRE(sample.build[5]->size() == 16);
        REQUIRE(sample.build[6].value() == "3");
        REQUIRE(sample.build[7].value() == "65549");
        REQUIRE(std::stoll(sample.build[8].value()) > 0);
        REQUIRE(std::stoll(sample.build[9].value()) >= 4);
        REQUIRE(std::stoll(sample.build[10].value()) >= 2);
        REQUIRE(sample.build[11].value() == "64");
        REQUIRE(sample.build[12].value() == "4096");
        REQUIRE(sample.build[13].value() == "4096");
        REQUIRE(sample.build[14].value() == "1");
        REQUIRE(sample.build[15].value() == "1");
        REQUIRE(sample.build[4].value() == independent_fixed_hex(
            independent_crc64_ecma182(
                nonaligned_three_fragment_dna4_source())));

        const long long expected_control_bytes =
            wire_format == 3 ? 4096LL * 16LL : 4096LL * 12LL;
        const long long expected_position_bytes =
            wire_format == 3 ? 4096LL * 8LL : 4096LL * 4LL;
        REQUIRE(std::stoll(sample.build[16].value()) ==
            expected_control_bytes);
        REQUIRE(std::stoll(sample.build[17].value()) ==
            expected_position_bytes);
        REQUIRE(std::stoll(sample.build[18].value()) > 0);
        REQUIRE(std::stoll(sample.build[19].value()) > 0);

        {
            connection_test_context reader;
            REQUIRE(query_scalar_int(reader, "SELECT pg_backend_pid();") !=
                builder_pid);
            sample.read = query_one(reader, fmt::format(
                "SELECT * FROM bioseq_internal.rev3_test_read_fixture("
                "'doc0371_rev3_live'::regclass, "
                "'minimizers_per_fragment', {}, 15::smallint, "
                "10::smallint, {}::smallint);",
                rowno, wire_format));
        }
        REQUIRE(sample.read.size() == 35);
        for (std::size_t identity_column = 0;
             identity_column <= 8; ++identity_column) {
            REQUIRE(sample.read[identity_column] ==
                sample.build[identity_column]);
        }
        REQUIRE(std::stoll(sample.read[9].value()) ==
            expected_control_bytes);
        REQUIRE(std::stoll(sample.read[10].value()) ==
            expected_position_bytes);
        REQUIRE(sample.read[11]->size() == 16);
        REQUIRE(sample.read[12]->size() == 16);
        REQUIRE(sample.read[13].value() == "1");
        REQUIRE(sample.read[14].value() == "0");
        REQUIRE(sample.read[15].value() == "f");
        REQUIRE(sample.read[16].value() == "t");
        REQUIRE(sample.read[17].value() == "1");
        REQUIRE(sample.read[18].value() == "4");
        REQUIRE(sample.read[19].value() == "1");
        REQUIRE(sample.read[20].value() == "1");
        REQUIRE(sample.read[21].value() == "2");
        REQUIRE(sample.read[22].value() == "2");
        REQUIRE(sample.read[23].value() == "2");
        REQUIRE(sample.read[24].value() == "2");
        REQUIRE(sample.read[25].value() == "2");
        REQUIRE(sample.read[26].value() == "2");
        REQUIRE(sample.read[27].value() ==
            std::to_string(std::stoll(sample.build[18].value())
                + std::stoll(sample.build[19].value())));
        REQUIRE(sample.read[28] == sample.read[27]);
        REQUIRE(std::stoll(sample.read[29].value()) ==
            expected_control_bytes + expected_position_bytes);
        REQUIRE(sample.read[30] == sample.read[28]);
        REQUIRE(sample.read[31].value() == "0");
        REQUIRE(sample.read[32].value() == "0");
        REQUIRE(sample.read[33].value() == "0");
        REQUIRE(sample.read[34].value() == "0");

        const auto schema = query_one(ctx, fmt::format(
            "SELECT string_agg(a.attname || ':' || "
            "format_type(a.atttypid,a.atttypmod) || ':' || "
            "a.attnotnull::int || ':' || a.attstorage::text, ',' "
            "ORDER BY a.attnum) FROM pg_attribute a "
            "WHERE a.attrelid='bioseq_internal.{}'::regclass "
            "AND a.attnum>0 AND NOT a.attisdropped;",
            sample.sidecar));
        REQUIRE(schema[0].value() ==
            "query_key:bigint:1:p,generation_id:bigint:1:p,"
            "position_segment:integer:1:p,sub_key:bigint:1:p,"
            "chunk_no:integer:1:p,chunk_bytes:integer:1:p,"
            "checksum:bigint:1:p,blob:bytea:1:p");
        const auto index_shape = query_one(ctx, fmt::format(
            "SELECT count(*)::text, bool_and(i.indisprimary AND "
            "i.indisunique AND i.indimmediate AND i.indisvalid AND "
            "i.indisready AND i.indislive AND i.indpred IS NULL AND "
            "i.indexprs IS NULL)::text, max(i.indkey::text), "
            "max(am.amname) FROM pg_index i JOIN pg_class ic "
            "ON ic.oid=i.indexrelid JOIN pg_am am ON am.oid=ic.relam "
            "WHERE i.indrelid='bioseq_internal.{}'::regclass;",
            sample.sidecar));
        REQUIRE(index_shape[0].value() == "1");
        REQUIRE(index_shape[1].value() == "true");
        REQUIRE(index_shape[2].value() == "1 2 3 4 5");
        REQUIRE(index_shape[3].value() == "btree");

        const auto physical = query_one(ctx, fmt::format(
            "SELECT pg_relation_size(c.oid)::text, "
            "pg_indexes_size(c.oid)::text, pg_table_size(c.oid)::text, "
            "pg_total_relation_size(c.oid)::text, "
            "CASE WHEN c.reltoastrelid=0 THEN 0 ELSE "
            "pg_total_relation_size(c.reltoastrelid) END::text, "
            "count(*)::text, sum(s.chunk_bytes)::text, "
            "sum(octet_length(s.blob))::text, "
            "count(*) FILTER (WHERE s.generation_id={})::text, "
            "coalesce(sum(s.chunk_bytes) FILTER (WHERE "
            "s.generation_id={} AND s.position_segment>=0 AND "
            "s.sub_key<{}),0)::text, "
            "coalesce(sum(s.chunk_bytes) FILTER (WHERE "
            "s.generation_id={} AND s.position_segment>=0 AND "
            "s.sub_key>={}),0)::text "
            "FROM pg_class c CROSS JOIN bioseq_internal.{} s "
            "WHERE c.oid='bioseq_internal.{}'::regclass "
            "GROUP BY c.oid,c.reltoastrelid;",
            sample.build[2].value(), sample.build[2].value(),
            position_extent_base, sample.build[2].value(),
            position_extent_base, sample.sidecar, sample.sidecar));
        REQUIRE(physical.size() == 11);
        sample.heap_bytes = std::stoll(physical[0].value());
        sample.table_bytes = std::stoll(physical[2].value());
        sample.total_bytes = std::stoll(physical[3].value());
        REQUIRE(sample.heap_bytes > 0);
        REQUIRE(std::stoll(physical[1].value()) > 0);
        REQUIRE(sample.table_bytes >= sample.heap_bytes);
        REQUIRE(sample.total_bytes >= sample.table_bytes);
        REQUIRE(std::stoll(physical[4].value()) >= 0);
        REQUIRE(std::stoll(physical[5].value()) ==
            std::stoll(sample.build[9].value()) + 1);
        REQUIRE(physical[6] == physical[7]);
        REQUIRE(std::stoll(physical[8].value()) ==
            std::stoll(sample.build[9].value()));
        REQUIRE(std::stoll(physical[9].value()) == expected_control_bytes);
        REQUIRE(std::stoll(physical[10].value()) == expected_position_bytes);
        sample.payload_bytes = expected_control_bytes + expected_position_bytes;

        const auto physical_extents = query_one(ctx, fmt::format(
            "SELECT min(sub_key)::text,max(sub_key)::text,"
            "count(DISTINCT sub_key)::text,"
            "count(*) FILTER (WHERE chunk_no=0)::text "
            "FROM bioseq_internal.{} WHERE generation_id={} "
            "AND position_segment>=0;",
            sample.sidecar, sample.build[2].value()));
        REQUIRE(physical_extents.size() == 4);
        REQUIRE(physical_extents[0].value() == "0");
        REQUIRE(std::stoll(physical_extents[1].value()) ==
            position_extent_base);
        REQUIRE(physical_extents[2].value() == "2");
        REQUIRE(physical_extents[3].value() == "2");
    }

    REQUIRE(observed[0].sidecar == observed[1].sidecar);
    REQUIRE(observed[0].build[2] != observed[1].build[2]);
    REQUIRE(observed[0].build[3] == observed[1].build[3]);
    REQUIRE(observed[0].build[4] == observed[1].build[4]);
    REQUIRE(observed[0].build[5] != observed[1].build[5]);
    REQUIRE(observed[1].payload_bytes * 3 == observed[0].payload_bytes * 2);
    REQUIRE(observed[1].heap_bytes < observed[0].heap_bytes);
    REQUIRE(observed[1].table_bytes < observed[0].table_bytes);
    REQUIRE(observed[1].total_bytes < observed[0].total_bytes);

    // Rebuild canonical format 3 after the A/B measurement so the public
    // revision-3 inspector and its source-drift adversary retain their exact
    // production-facing proof without warming either measured read.
    const auto inspection_fixture = query_one(ctx, fmt::format(
        "SELECT * FROM bioseq_internal.rev3_test_build_fixture("
        "'doc0371_rev3_live'::regclass, 'minimizers_per_fragment', "
        "{}, 15::smallint, 10::smallint, 3::smallint);", rowno));
    const auto inspected = query_one(ctx, fmt::format(
        "SELECT generation_id::text, source_identity, "
        "source_content_checksum, compatibility_checksum, "
        "kmer_size::text, window_size::text "
        "FROM bioseq.inspect_ready_minimizer_generation("
        "'doc0371_rev3_live'::regclass, 'minimizers_per_fragment', {});",
        rowno));
    REQUIRE(inspected.size() == 6);
    REQUIRE(inspected[0] == inspection_fixture[2]);
    REQUIRE(inspected[1] == inspection_fixture[3]);
    REQUIRE(inspected[2] == inspection_fixture[4]);
    REQUIRE(inspected[3] == inspection_fixture[5]);
    REQUIRE(inspected[4].value() == "15");
    REQUIRE(inspected[5].value() == "10");

    const auto rev3_absence_audit = query_one(ctx,
        "SELECT physical_index_relations::text, persisted_index_rows::text, "
        "generation_objects::text "
        "FROM bioseq.inspect_cold_no_index_absence("
        "'doc0371_rev3_live'::regclass, 'minimizers_per_fragment');");
    REQUIRE(rev3_absence_audit.size() == 3);
    REQUIRE(std::stoll(rev3_absence_audit[0].value()) > 0);
    REQUIRE(std::stoll(rev3_absence_audit[1].value()) > 0);
    REQUIRE(rev3_absence_audit[2].value() == "1");

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

    exec_sqlf(ctx, "DROP TABLE bioseq_internal.{};",
        inspection_fixture[0].value());
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
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "3::smallint);",
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
        "'minimizers_per_fragment', {}, 15::smallint, 10::smallint, "
        "3::smallint);",
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
        "{}, 15::smallint, 10::smallint, 3::smallint);", rowno));
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
        "{}, 15::smallint, 10::smallint, 3::smallint);", rowno));
    const std::string sidecar = fixture[0].value();

    const std::array<std::pair<std::string_view, std::string_view>, 11> failures{{
        {"repeated_head", "more than once or out of order"},
        {"repeated_metadata", "repeated, early, or changed"},
        {"early_payload", "before metadata"},
        {"permit_denied", "memory admission was denied"},
        {"scheme_mismatch", "minimizer compatibility disagrees"},
        {"wire_format_mismatch", "wire format disagrees with reader"},
        {"position_before_controls", "before control payload completed"},
        {"finish_before_positions", "before position payload completed"},
        {"repeated_control_plan", "more than once"},
        {"control_position_namespace", "outside the control namespace"},
        {"unauthorized_position", "no delivered control extent"},
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
