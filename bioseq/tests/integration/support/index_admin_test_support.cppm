/**
 * @file
 * @brief Shared fixtures for index administration tests.
 */
module;
#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.integration.index_admin_test_support;

import std;
import bioseq.tests.integration.support;

export namespace bioseq_int_tests::index_admin_test_support {

/** Table used by metadata administration cases. */
inline constexpr std::string_view k_table = "index_admin_t";

/** Return the column row number from a table containing exactly one row. */
[[nodiscard]] long long only_column_row_number(
        const connection_test_context& ctx, std::string_view table) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) FROM {}",
        table, table));
}

/** Check a ready hash-major generation and return its generation ID. */
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

/** Check a ready fragment-minimizer generation and return its generation ID. */
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
    // Check the format revisions expected by this fragment-index inspector.
    REQUIRE(inspected[5].value() == "3");
    REQUIRE(inspected[6].value() == "2");
    REQUIRE(inspected[7].value()
            == std::to_string(expected_position_segment_span_bp));
    REQUIRE(inspected[8].value()
            == std::to_string(expected_position_segment_count));
    return generation_id;
}

/** Require Bioseq preloading and an attached scheduler supervisor before DROP tests. */
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

/** Format the shared probe as DNA4/64 for data-preservation checks. */
[[nodiscard]] std::string drop_preservation_probe_literal() {
    return format_literal(
        k_hoe_probe_sequence,
        seq_typmod{.alphabet = "dna4", .width = 64});
}

/** Catalog metadata and sequence rows read after a DROP attempt. */
struct preserved_ready_bioseq_source_evidence {
    long long observed_oid{};
    long long bioseq_relation_count{};
    row_t source;
    rowset_t hydrated;
};

/** Read the table identity, source metadata, and matching sequence rows. */
[[nodiscard]] preserved_ready_bioseq_source_evidence
capture_preserved_ready_bioseq_source(
        const connection_test_context& ctx,
        std::string_view table,
        const long long expected_oid) {
    // Check the original table identity and its published fragment metadata.
    // Then use mmsearch to read the stored sequence bytes.
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

    rowset_t hydrated;
    {
        // Allow a short probe with one seed to match during this check.
        // These settings are RESET when the scopes end.
        guc_scope minimum_score{
            ctx, "bioseq.aligner.chaining.minimum_score", "10"};
        guc_scope minimum_seed_count{
            ctx, "bioseq.aligner.chaining.minimum_seed_count", "1"};
        hydrated = query_rows(ctx, fmt::format(
            "SELECT id::text, seq::text FROM {} "
            "WHERE mmsearch(seq, {}) ORDER BY id;",
            table, drop_preservation_probe_literal()));
    }

    return preserved_ready_bioseq_source_evidence{
        .observed_oid = observed_oid,
        .bioseq_relation_count = bioseq_relation_count,
        .source = std::move(source),
        .hydrated = std::move(hydrated),
    };
}

/** Check that the original table, ready source, and expected sequence row survived. */
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

/** Read and check that the Bioseq table and its stored sequence survived. */
void require_preserved_ready_bioseq_source(
        const connection_test_context& ctx,
        std::string_view table,
        const long long expected_oid) {
    require_preserved_ready_bioseq_source_evidence(
        capture_preserved_ready_bioseq_source(ctx, table, expected_oid),
        expected_oid);
}

}  // namespace bioseq_int_tests::index_admin_test_support
