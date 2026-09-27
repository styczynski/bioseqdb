/**
 * @file lazy_sequence_storage_test.cpp
 * @brief Exercises physical letters, GUC parity, large lengths, and allocation guards.
 */

#include <cstdio>
#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.lazy_sequence_slicing_acceptance_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::lazy_sequence_slicing_acceptance_test_support;

TEST_CASE("physical DNA4 letters support arrays grouping and indexes",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres][resource:filesystem]") {
    auto ctx = fresh_context({
        "sequence_acceptance_letters_a",
        "sequence_acceptance_letters_b",
        "sequence_acceptance_letters_binary",
        "sequence_acceptance_letters_receive",
        "sequence_acceptance_letters_text_copy",
        "sequence_acceptance_letters_typmodless_copy",
        "sequence_acceptance_letters_dna5",
        "sequence_acceptance_letters_dna5_b",
        "sequence_acceptance_letters_dna5_binary",
        "sequence_acceptance_letters_dna5_receive",
        "sequence_acceptance_letters_dna5_text",
    });
    exec_sql(ctx,
        "SET search_path TO pg_catalog, bioseq, public; "
        "CREATE TABLE public.sequence_acceptance_letters_a "
        " (id int, value bioseq.letter('dna4')); "
        "PREPARE sequence_acceptance_insert_letter(text,int) AS "
        " INSERT INTO sequence_acceptance_letters_a "
        " VALUES ($2::int, $1::bioseq.letter('dna4')); "
        "EXECUTE sequence_acceptance_insert_letter('A',1); "
        "EXECUTE sequence_acceptance_insert_letter('C',2); "
        "EXECUTE sequence_acceptance_insert_letter('G',3); "
        "EXECUTE sequence_acceptance_insert_letter('T',4); "
        "DEALLOCATE sequence_acceptance_insert_letter; "
        "CREATE INDEX sequence_acceptance_letters_a_btree "
        " ON sequence_acceptance_letters_a USING btree(value bioseq.letter_btree_ops); "
        "CREATE INDEX sequence_acceptance_letters_a_hash "
        " ON sequence_acceptance_letters_a USING hash(value bioseq.letter_hash_ops); "
        "CREATE TABLE public.sequence_acceptance_letters_b AS "
        " SELECT * FROM sequence_acceptance_letters_a; "
        "CREATE TEMP VIEW sequence_acceptance_letters_view AS "
        " SELECT id,value FROM sequence_acceptance_letters_a;");

    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY value) "
        "FROM sequence_acceptance_letters_a;") == "ACGT");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(DISTINCT value) FROM sequence_acceptance_letters_a;") == 4);
    {
        guc_scope no_seqscan(ctx, "enable_seqscan", "off");
        REQUIRE(query_scalar_int(
            ctx,
            "SELECT id FROM sequence_acceptance_letters_a "
            "WHERE value = 'G'::bioseq.letter('dna4');") == 3);
    }
    exec_sql(ctx,
        "SET enable_hashjoin=on; "
        "SET enable_nestloop=off; "
        "SET enable_mergejoin=off;");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM sequence_acceptance_letters_a a "
        "JOIN sequence_acceptance_letters_b b USING (value);") == 4);
    REQUIRE(plan_contains(
        explain_text<false>(
            ctx,
            "SELECT * FROM sequence_acceptance_letters_a a "
            "JOIN sequence_acceptance_letters_b b USING (value);"),
        "Hash Join"));
    REQUIRE(query_scalar(
        ctx,
        "SELECT array_to_string(array_agg(value ORDER BY value),'') "
        "FROM sequence_acceptance_letters_a;") == "ACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM sequence_acceptance_letters_view;") == "ACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT array_to_string(ARRAY["
        " 'A'::bioseq.letter('dna4'),"
        " 'C'::bioseq.letter('dna4'),"
        " 'G'::bioseq.letter('dna4'),"
        " 'T'::bioseq.letter('dna4')],'');") == "ACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT ('ACGT'::bioseq.seq('dna4',8))[2]::text;") == "G");
    REQUIRE(query_scalar(
        ctx,
        "SELECT encode(bioseq.letter_send(value),'hex') "
        "FROM sequence_acceptance_letters_a WHERE id=1;") == "010000");
    const auto dna4_cast = query_one(
        ctx,
        "SELECT ('A'::bioseq.letter('dna4')"
        "          ::bioseq.letter('dna4'))::text, "
        "       ('A'::bioseq.letter('dna4')"
        "          ::bioseq.letter)::text;");
    REQUIRE(dna4_cast[0] == "A");
    REQUIRE(dna4_cast[1] == "A");

    exec_sql(ctx,
        "PREPARE sequence_acceptance_letter_param(bioseq.letter) AS "
        " SELECT $1::text;");
    for (const char* symbol : {"A", "C", "G", "T"}) {
        CAPTURE(symbol);
        REQUIRE(query_scalar(
            ctx,
            fmt::format(
                "EXECUTE sequence_acceptance_letter_param("
                " '{}'::bioseq.letter('dna4'));",
                symbol)) == symbol);
    }
    exec_sql(ctx, "DEALLOCATE sequence_acceptance_letter_param;");

    for (const char* invalid : {"", "AC", "N"}) {
        CAPTURE(invalid);
        const auto error = sql_error(
            ctx,
            fmt::format(
                "SELECT '{}'::bioseq.letter('dna4');", invalid));
        REQUIRE(error.sqlstate == "22P02");
    }

    const auto binary_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_letters_{}.bin",
        query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    (void) std::remove(binary_path.c_str());
    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_letters_binary "
        " (id int, value bioseq.letter('dna4')); ");
    exec_sql(ctx, fmt::format(
        "COPY sequence_acceptance_letters_a TO '{}' (FORMAT binary);",
        binary_path));
    exec_sql(ctx, fmt::format(
        "COPY sequence_acceptance_letters_binary FROM '{}' (FORMAT binary);",
        binary_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM sequence_acceptance_letters_binary;") == "ACGT");

    const auto text_copy_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_letters_text_{}.txt",
        query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    const auto id_copy_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_letters_id_{}.txt",
        query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    (void) std::remove(text_copy_path.c_str());
    (void) std::remove(id_copy_path.c_str());
    {
        std::ofstream output(text_copy_path, std::ios::binary);
        REQUIRE(output.is_open());
        output << "1\tA\n2\tC\n3\tG\n4\tT\n";
        REQUIRE(output.good());
    }
    {
        std::ofstream output(id_copy_path, std::ios::binary);
        REQUIRE(output.is_open());
        output << "9\n";
        REQUIRE(output.good());
    }
    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_letters_text_copy "
        " (id int, value bioseq.letter('dna4')); "
        "CREATE TABLE public.sequence_acceptance_letters_typmodless_copy "
        " (id int, value bioseq.letter);");
    exec_sql(ctx, fmt::format(
        "COPY public.sequence_acceptance_letters_text_copy FROM '{}' (FORMAT text);",
        text_copy_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM public.sequence_acceptance_letters_text_copy;") == "ACGT");

    const auto text_copy_rejected = sql_error(
        ctx, fmt::format(
            "COPY public.sequence_acceptance_letters_typmodless_copy "
            "FROM '{}' (FORMAT text);",
            text_copy_path));
    REQUIRE(text_copy_rejected.sqlstate == "22023");
    REQUIRE(text_copy_rejected.contains("text COPY requires an alphabet typmod"));
    const auto csv_copy_rejected = sql_error(
        ctx, fmt::format(
            "COPY public.sequence_acceptance_letters_typmodless_copy "
            "FROM '{}' (FORMAT csv);",
            text_copy_path));
    REQUIRE(csv_copy_rejected.sqlstate == "22023");
    const auto program_copy_rejected = sql_error(
        ctx,
        "COPY public.sequence_acceptance_letters_typmodless_copy "
        "FROM PROGRAM 'true' (FORMAT text);");
    REQUIRE(program_copy_rejected.sqlstate == "22023");

    PGresult* stdin_copy = PQexec(
        ctx.raw(),
        "COPY public.sequence_acceptance_letters_typmodless_copy FROM STDIN "
        "(FORMAT text);");
    REQUIRE(stdin_copy != nullptr);
    REQUIRE(PQresultStatus(stdin_copy) == PGRES_FATAL_ERROR);
    const char* stdin_state = PQresultErrorField(
        stdin_copy, PG_DIAG_SQLSTATE);
    REQUIRE(stdin_state != nullptr);
    REQUIRE(std::string_view{stdin_state} == "22023");
    PQclear(stdin_copy);

    // The guard is target-column exact: omitting the typmodless letter column
    // is valid, and binary COPY remains valid because its physical alphabet is
    // checked by letter_recv.
    exec_sql(ctx, fmt::format(
        "COPY public.sequence_acceptance_letters_typmodless_copy(id) "
        "FROM '{}' (FORMAT text);",
        id_copy_path));
    exec_sql(ctx, fmt::format(
        "COPY public.sequence_acceptance_letters_typmodless_copy "
        "FROM '{}' (FORMAT binary);",
        binary_path));
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM public.sequence_acceptance_letters_typmodless_copy "
        "WHERE value IS NOT NULL;") == 4);
    REQUIRE(std::remove(binary_path.c_str()) == 0);
    REQUIRE(std::remove(text_copy_path.c_str()) == 0);
    REQUIRE(std::remove(id_copy_path.c_str()) == 0);

    const auto backend_pid = query_scalar_int(
        ctx, "SELECT pg_backend_pid();");
    const auto valid_letter_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_letter_valid_{}.bin", backend_pid);
    const auto unsupported_revision_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_letter_unsupported_revision_{}.bin", backend_pid);
    const auto bad_rank_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_letter_bad_rank_{}.bin", backend_pid);
    for (const auto* path : {
             &valid_letter_path, &unsupported_revision_path, &bad_rank_path}) {
        (void) std::remove(path->c_str());
    }
    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_letters_receive "
        " (value bioseq.letter('dna4')); ");
    exec_sql(ctx, fmt::format(
        "COPY (SELECT 'A'::bioseq.letter('dna4')) "
        "TO '{}' (FORMAT binary);",
        valid_letter_path));
    write_letter_binary_copy_variant(
        valid_letter_path, unsupported_revision_path, 2u, 0u, 0u);
    write_letter_binary_copy_variant(
        valid_letter_path, bad_rank_path, 1u, 0u, 0xffu);

    const auto unsupported_revision = sql_error(
        ctx,
        fmt::format(
            "COPY sequence_acceptance_letters_receive FROM '{}' (FORMAT binary);",
            unsupported_revision_path));
    REQUIRE(unsupported_revision.sqlstate == "22P03");
    REQUIRE(unsupported_revision.contains("unsupported bioseq.letter wire version"));
    const auto bad_rank = sql_error(
        ctx,
        fmt::format(
            "COPY sequence_acceptance_letters_receive FROM '{}' (FORMAT binary);",
            bad_rank_path));
    REQUIRE(bad_rank.sqlstate == "22P03");
    REQUIRE(bad_rank.contains("invalid physical bioseq.letter wire value"));
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM sequence_acceptance_letters_receive;") == 0);

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_letters_dna5 "
        " (id int, value bioseq.letter('dna5')); "
        "INSERT INTO sequence_acceptance_letters_dna5 VALUES "
        " (1,'A'::bioseq.letter('dna5')), "
        " (2,'C'::bioseq.letter('dna5')), "
        " (3,'G'::bioseq.letter('dna5')), "
        " (4,'T'::bioseq.letter('dna5')), "
        " (5,'N'::bioseq.letter('dna5')); "
        "CREATE INDEX sequence_acceptance_letters_dna5_btree "
        " ON sequence_acceptance_letters_dna5 USING btree(value); "
        "CREATE INDEX sequence_acceptance_letters_dna5_hash "
        " ON sequence_acceptance_letters_dna5 USING hash(value); "
        "CREATE TABLE public.sequence_acceptance_letters_dna5_b AS "
        " TABLE sequence_acceptance_letters_dna5; "
        "CREATE TABLE public.sequence_acceptance_letters_dna5_binary "
        " (id int, value bioseq.letter('dna5')); "
        "CREATE TABLE public.sequence_acceptance_letters_dna5_receive "
        " (value bioseq.letter('dna5')); "
        "CREATE TABLE public.sequence_acceptance_letters_dna5_text "
        " (id int, value bioseq.letter('dna5')); "
        "CREATE TEMP VIEW sequence_acceptance_letters_dna5_view AS "
        " SELECT id,value FROM sequence_acceptance_letters_dna5;");
    // Native DNA5 rank order is A, C, G, N, T; insertion order remains ACGTN.
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY value) "
        "FROM sequence_acceptance_letters_dna5;") == "ACGNT");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(DISTINCT value) FROM sequence_acceptance_letters_dna5;") == 5);
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM ("
        " SELECT value FROM sequence_acceptance_letters_dna5 GROUP BY value"
        ") AS groups;") == 5);
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM sequence_acceptance_letters_dna5_view;") == "ACGTN");
    REQUIRE(query_scalar(
        ctx,
        "SELECT array_to_string(ARRAY["
        " 'A'::bioseq.letter('dna5'),"
        " 'C'::bioseq.letter('dna5'),"
        " 'G'::bioseq.letter('dna5'),"
        " 'T'::bioseq.letter('dna5'),"
        " 'N'::bioseq.letter('dna5')],'');") == "ACGTN");
    REQUIRE(query_scalar(
        ctx,
        "SELECT ('ACGTN'::bioseq.seq('dna5',8))[4]::text;") == "N");
    REQUIRE(query_scalar(
        ctx,
        "SELECT ('N'::bioseq.letter('dna5')"
        "          ::bioseq.letter('dna5'))::text;") == "N");
    REQUIRE(sql_error(
        ctx,
        "SELECT 'A'::bioseq.letter('dna4')"
        "         ::bioseq.letter('dna5');")
        .sqlstate == "42804");

    exec_sql(ctx,
        "PREPARE sequence_acceptance_letter_dna5_param(bioseq.letter) AS "
        " SELECT $1::text;");
    for (const char* symbol : {"A", "C", "G", "T", "N"}) {
        CAPTURE(symbol);
        REQUIRE(query_scalar(
            ctx,
            fmt::format(
                "EXECUTE sequence_acceptance_letter_dna5_param("
                " '{}'::bioseq.letter('dna5'));",
                symbol)) == symbol);
    }
    exec_sql(ctx, "DEALLOCATE sequence_acceptance_letter_dna5_param;");

    for (const char* invalid : {"", "AC", "X"}) {
        CAPTURE(invalid);
        REQUIRE(sql_error(
            ctx,
            fmt::format(
                "SELECT '{}'::bioseq.letter('dna5');", invalid))
            .sqlstate == "22P02");
    }

    {
        guc_scope no_seqscan(ctx, "enable_seqscan", "off");
        REQUIRE(query_scalar_int(
            ctx,
            "SELECT id FROM sequence_acceptance_letters_dna5 "
            "WHERE value = 'N'::bioseq.letter('dna5');") == 5);
    }
    exec_sql(ctx,
        "SET enable_hashjoin=on; "
        "SET enable_nestloop=off; "
        "SET enable_mergejoin=off;");
    const auto dna5_hash_join =
        "SELECT count(*) FROM sequence_acceptance_letters_dna5 a "
        "JOIN sequence_acceptance_letters_dna5_b b USING (value);";
    REQUIRE(query_scalar_int(ctx, dna5_hash_join) == 5);
    REQUIRE(plan_contains(
        explain_text<false>(ctx, dna5_hash_join), "Hash Join"));

    const auto dna5_binary_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_letters_dna5_{}.bin", backend_pid);
    (void) std::remove(dna5_binary_path.c_str());
    exec_sql(ctx, fmt::format(
        "COPY sequence_acceptance_letters_dna5 TO '{}' (FORMAT binary);",
        dna5_binary_path));
    exec_sql(ctx, fmt::format(
        "COPY sequence_acceptance_letters_dna5_binary FROM '{}' (FORMAT binary);",
        dna5_binary_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM sequence_acceptance_letters_dna5_binary;") == "ACGTN");
    REQUIRE(std::remove(dna5_binary_path.c_str()) == 0);

    const auto dna5_text_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_letters_dna5_{}.txt", backend_pid);
    (void) std::remove(dna5_text_path.c_str());
    {
        std::ofstream output(dna5_text_path, std::ios::binary);
        REQUIRE(output.is_open());
        output << "1\tA\n2\tC\n3\tG\n4\tT\n5\tN\n";
        REQUIRE(output.good());
    }
    exec_sql(ctx, fmt::format(
        "COPY public.sequence_acceptance_letters_dna5_text "
        "FROM '{}' (FORMAT text);",
        dna5_text_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM public.sequence_acceptance_letters_dna5_text;") == "ACGTN");
    REQUIRE(std::remove(dna5_text_path.c_str()) == 0);

    const auto typmod_mismatch = sql_error(
        ctx,
        fmt::format(
            "COPY sequence_acceptance_letters_dna5_receive "
            "FROM '{}' (FORMAT binary);",
            valid_letter_path));
    REQUIRE(typmod_mismatch.sqlstate == "42804");
    REQUIRE(typmod_mismatch.contains(
        "wire alphabet does not match destination typmod"));

    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.sequence_acceptance_letter_overload("
        " value bioseq.letter('dna4')) RETURNS int "
        "LANGUAGE sql IMMUTABLE AS 'SELECT 4';");
    REQUIRE(sql_error(
        ctx,
        "CREATE FUNCTION pg_temp.sequence_acceptance_letter_overload("
        " value bioseq.letter('dna5')) RETURNS int "
        "LANGUAGE sql IMMUTABLE AS 'SELECT 5';")
        .sqlstate == "42723");

    const auto mixed_letter = query_one(
        ctx,
        "WITH v(d4,d5) AS (VALUES ("
        " 'A'::bioseq.letter('dna4'), "
        " 'A'::bioseq.letter('dna5'))) "
        "SELECT NOT bioseq.letter_eq(d4,d5), "
        "       bioseq.letter_cmp(d4,d5) < 0, "
        "       bioseq.letter_hash(d4) <> bioseq.letter_hash(d5), "
        "       bioseq.letter_hash_extended(d4,77) <> "
        "         bioseq.letter_hash_extended(d5,77) "
        "FROM v;");
    for (const auto& value : mixed_letter) {
        REQUIRE(value == "t");
    }
#endif

    for (const auto* path : {
             &valid_letter_path, &unsupported_revision_path, &bad_rank_path}) {
        REQUIRE(std::remove(path->c_str()) == 0);
    }
}

TEST_CASE("slice algebra is invariant across the required GUC matrix",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({"sequence_acceptance_slice_guc"});
    exec_sql(ctx,
        "CREATE TABLE sequence_acceptance_slice_guc "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq; "
        "INSERT INTO sequence_acceptance_slice_guc VALUES "
        " (1,repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    constexpr std::array settings{"off", "on"};
    for (const char* vectorization : settings) {
        for (const char* column_cache : settings) {
            exec_sql(ctx, fmt::format(
                "SET bioseq.vectorization.enabled={}; "
                "SET bioseq.column_cache.enabled={};",
                vectorization, column_cache));
            CAPTURE(vectorization, column_cache);
            REQUIRE(query_scalar(
                ctx,
                "SELECT (s[4093:4101] OPERATOR(bioseq.||) "
                "        bioseq.reverse(s[8190:8194]))::text "
                "FROM sequence_acceptance_slice_guc WHERE id=1;") == "CGTACGTACATG");
            const auto plan = explain_text<false>(
                ctx,
                "SELECT s[4093:4101] FROM sequence_acceptance_slice_guc "
                "WHERE id=1;");
            REQUIRE(plan_contains(plan, "BioseqSliceProject"));
        }
    }
}

TEST_CASE("synthetic logical lengths stay lazy through bigint limits",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({"sequence_acceptance_synthetic_length_heap",
                              "sequence_acceptance_synthetic_length_index_input",
                              "sequence_acceptance_synthetic_length_real_index_input",
                              "sequence_acceptance_synthetic_lengths"});
    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_synthetic_lengths "
        " (label text PRIMARY KEY, n bigint NOT NULL); "
        "INSERT INTO public.sequence_acceptance_synthetic_lengths VALUES "
        " ('above_flat',4294967296), "
        " ('bigint_max',9223372036854775807);");

    const auto above_flat = query_one(
        ctx,
        "SELECT bioseq.len(s), "
        "       bioseq.len(s[4294967294::bigint:4294967296::bigint]), "
        "       s[-1]::text, "
        "       bioseq.len(s OPERATOR(bioseq.||) "
        "          bioseq_internal.synthetic_lazy_sequence(0)) "
        "FROM public.sequence_acceptance_synthetic_lengths AS source "
        "CROSS JOIN LATERAL ("
        " SELECT bioseq_internal.synthetic_lazy_sequence(source.n) AS s "
        " OFFSET 0) AS v "
        "WHERE source.label='above_flat';");
    REQUIRE(above_flat[0] == "4294967296");
    REQUIRE(above_flat[1] == "2");
    REQUIRE(above_flat[2] == "A");
    REQUIRE(above_flat[3] == "4294967296");

    // A materialized CTE is an executor-copy boundary: PostgreSQL must ask
    // the expanded value for a flat pgseq representation before tuplestore
    // ownership can begin.  The same logical source that stays lazy above
    // must therefore fail closed at the uint32 flat-format ceiling here.
    const auto materialized_cte_boundary = sql_error(
        ctx,
        "WITH v(s) AS MATERIALIZED ("
        " SELECT bioseq_internal.synthetic_lazy_sequence(n) "
        " FROM public.sequence_acceptance_synthetic_lengths "
        " WHERE label='above_flat') "
        "SELECT bioseq.len(s) FROM v;");
    REQUIRE(materialized_cte_boundary.sqlstate == "54000");
    REQUIRE(materialized_cte_boundary.contains("too long to flatten"));

    const auto bigint_max = query_one(
        ctx,
        "SELECT bioseq.len(s), s[-1]::text, "
        "       bioseq.len(s[9223372036854775805::bigint:"
        "                         9223372036854775807::bigint]), "
        "       bioseq.len(s OPERATOR(bioseq.||) "
        "          bioseq_internal.synthetic_lazy_sequence(0)), "
        "       bioseq.len(bioseq_internal.synthetic_lazy_sequence(0) "
        "          OPERATOR(bioseq.||) s) "
        "FROM public.sequence_acceptance_synthetic_lengths AS source "
        "CROSS JOIN LATERAL ("
        " SELECT bioseq_internal.synthetic_lazy_sequence(source.n) AS s "
        " OFFSET 0) AS v "
        "WHERE source.label='bigint_max';");
    REQUIRE(bigint_max[0] == "9223372036854775807");
    REQUIRE(bigint_max[1] == "A");
    REQUIRE(bigint_max[2] == "2");
    REQUIRE(bigint_max[3] == "9223372036854775807");
    REQUIRE(bigint_max[4] == "9223372036854775807");

    constexpr std::array concat_overflow_queries{
        "SELECT bioseq_internal.synthetic_lazy_sequence(n) "
        "       OPERATOR(bioseq.||) "
        "       bioseq_internal.synthetic_lazy_sequence(1) "
        "FROM public.sequence_acceptance_synthetic_lengths "
        "WHERE label='bigint_max';",
        "SELECT bioseq_internal.synthetic_lazy_sequence(1) "
        "       OPERATOR(bioseq.||) "
        "       bioseq_internal.synthetic_lazy_sequence(n) "
        "FROM public.sequence_acceptance_synthetic_lengths "
        "WHERE label='bigint_max';",
    };
    for (const char* sql : concat_overflow_queries) {
        REQUIRE(sql_error(ctx, sql).sqlstate == "54000");
    }

    const std::array<const char*, 3> materialization_boundaries{
        "SELECT bioseq.to_text("
        " bioseq_internal.synthetic_lazy_sequence(n)) "
        "FROM public.sequence_acceptance_synthetic_lengths "
        "WHERE label='above_flat';",
        "SELECT bioseq.seq_send("
        " bioseq_internal.synthetic_lazy_sequence(n)) "
        "FROM public.sequence_acceptance_synthetic_lengths "
        "WHERE label='above_flat';",
        "SELECT bioseq.to_array("
        " bioseq_internal.synthetic_lazy_sequence(n)) "
        "FROM public.sequence_acceptance_synthetic_lengths "
        "WHERE label='above_flat';",
    };
    for (const char* sql : materialization_boundaries) {
        REQUIRE(sql_error(ctx, sql).sqlstate == "54000");
    }

    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_synthetic_length_index_input "
        " (n bigint NOT NULL); "
        "INSERT INTO public.sequence_acceptance_synthetic_length_index_input "
        "VALUES (4294967296);");
    const auto synthetic_index_boundary = sql_error(
        ctx,
        "CREATE INDEX sequence_acceptance_synthetic_length_expr_idx "
        "ON public.sequence_acceptance_synthetic_length_index_input USING btree "
        "((bioseq_internal.synthetic_lazy_sequence(n)));");
    REQUIRE(synthetic_index_boundary.sqlstate == "54000");
    REQUIRE(synthetic_index_boundary.contains("too long to flatten"));

    // Retain a physically stored incompressible value as separate evidence of
    // PostgreSQL's ordinary btree row-size boundary.  It complements rather
    // than substitutes for the synthetic UINT32_MAX+1 expression above.
    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_synthetic_length_real_index_input "
        " (id bigint, s bioseq.seq('dna4',8)); "
        "INSERT INTO public.sequence_acceptance_synthetic_length_real_index_input "
        "SELECT 1, translate("
        " string_agg(md5(i::text), '' ORDER BY i), "
        " '0123456789abcdef', 'ACGTACGTACGTACGT')"
        " ::bioseq.seq('dna4',8) "
        "FROM generate_series(1,1024) AS g(i);");
    const auto physical_index_boundary = sql_error(
        ctx,
        "CREATE INDEX sequence_acceptance_physical_length_expr_idx "
        "ON public.sequence_acceptance_synthetic_length_real_index_input USING btree "
        "((s[0:32768]));");
    REQUIRE(physical_index_boundary.sqlstate == "54000");
    REQUIRE(physical_index_boundary.contains("index row"));

    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_synthetic_length_heap "
        " (s bioseq.seq('dna4',8));");
    const auto heap_boundary = sql_error(
        ctx,
        "INSERT INTO public.sequence_acceptance_synthetic_length_heap "
        "SELECT bioseq_internal.synthetic_lazy_sequence(n) "
        "FROM public.sequence_acceptance_synthetic_lengths "
        "WHERE label='above_flat';");
    REQUIRE(heap_boundary.sqlstate == "54000");
}

TEST_CASE("uint64 bigint and MaxAlloc alignment guards are exact",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();

    // Directly exercise the exported checked Size/flat-size helpers as well as
    // the independent tree/ownership matrices, without allocating the
    // represented MaxAlloc/UINT64 payloads.
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal.materialization_size_self_test();") == "t");
    REQUIRE(query_scalar(
        ctx, "SELECT bioseq_internal.piece_tree_self_test();") == "t");
    REQUIRE(query_scalar(
        ctx, "SELECT bioseq_internal.expanded_ownership_self_test();") == "t");
}

TEST_CASE("slice and cast builders release pre-error intermediates",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.sequence_acceptance_runtime_text(value text) "
        "RETURNS text LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "BEGIN RETURN value; END $$; "
        "DO $sequence_acceptance$ "
        "DECLARE iteration integer; "
        "BEGIN "
        "  FOR iteration IN 1..32 LOOP "
        "    BEGIN "
        "      PERFORM ("
        "        pg_temp.sequence_acceptance_runtime_text(repeat('A',4097))::bioseq.seq "
        "        OPERATOR(bioseq.||) "
        "        pg_temp.sequence_acceptance_runtime_text('X')::bioseq.seq"
        "      )::bioseq.seq('dna4',32); "
        "      RAISE EXCEPTION 'expected invalid DNA4 literal'; "
        "    EXCEPTION WHEN invalid_text_representation THEN "
        "      NULL; "
        "    END; "
        "  END LOOP; "
        "END $sequence_acceptance$;");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal.piece_tree_self_test();")
        == "t");
}

TEST_CASE("literal coalescing and arena guards are exact",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.sequence_acceptance_runtime_length(value integer) "
        "RETURNS integer LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "BEGIN RETURN value; END $$;");

    const auto thresholds = query_one(
        ctx,
        "SELECT bioseq_internal.sequence_piece_count("
        "         repeat('A',pg_temp.sequence_acceptance_runtime_length(2047))::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',pg_temp.sequence_acceptance_runtime_length(2048))::bioseq.seq), "
        "       bioseq_internal.sequence_piece_count("
        "         repeat('A',pg_temp.sequence_acceptance_runtime_length(2048))::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',pg_temp.sequence_acceptance_runtime_length(2048))::bioseq.seq), "
        "       bioseq_internal.sequence_piece_count("
        "         repeat('A',pg_temp.sequence_acceptance_runtime_length(2048))::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',pg_temp.sequence_acceptance_runtime_length(2049))::bioseq.seq);");
    REQUIRE(thresholds[0] == "1");
    REQUIRE(thresholds[1] == "1");
    REQUIRE(thresholds[2] == "2");
}
