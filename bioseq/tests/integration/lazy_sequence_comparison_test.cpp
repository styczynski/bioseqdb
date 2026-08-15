/**
 * @file lazy_sequence_comparison_test.cpp
 * @brief Exercises sequence casts, equality, ordering, hashes, and letter persistence.
 */

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <libpq-fe.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

import bioseq.tests.integration.support;
import bioseq.tests.integration.lazy_sequence_slicing_acceptance_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::lazy_sequence_slicing_acceptance_test_support;

TEST_CASE("self casts preserve logical values across widths and layouts",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({
        "sequence_acceptance_cast_matrix",
        "sequence_acceptance_cast_source",
        "sequence_acceptance_cast_assign",
        "sequence_acceptance_cast_layout_assign",
        "sequence_acceptance_cast_dna5",
    });
    exec_sql(ctx,
        "CREATE TABLE sequence_acceptance_cast_matrix "
        " (id int, s bioseq.seq('dna4',8)); "
        "ALTER TABLE sequence_acceptance_cast_matrix ALTER COLUMN s SET STORAGE EXTERNAL; "
        "INSERT INTO sequence_acceptance_cast_matrix VALUES "
        " (1,'ACGT'::bioseq.seq('dna4',8)), "
        " (2,repeat('ACGT',300000)::bioseq.seq('dna4',8)); "
        "CREATE TABLE sequence_acceptance_cast_source "
        " (id int, s bioseq.seq('dna4',8)) USING bioseq; "
        "INSERT INTO sequence_acceptance_cast_source VALUES "
        " (1,'ACGT'::bioseq.seq('dna4',8)); "
        "CREATE TABLE sequence_acceptance_cast_assign "
        " (w8 bioseq.seq('dna4',8), "
        "  w16 bioseq.seq('dna4',16), "
        "  w32 bioseq.seq('dna4',32), "
        "  w64 bioseq.seq('dna4',64)); "
        "INSERT INTO sequence_acceptance_cast_assign "
        "SELECT s,s,s,s FROM sequence_acceptance_cast_source WHERE id=1; "
        "CREATE TABLE sequence_acceptance_cast_layout_assign "
        " (id int, value bioseq.seq('dna4',64)); "
        "INSERT INTO sequence_acceptance_cast_layout_assign VALUES "
        " (1,'ACGT'::bioseq.seq('dna4',8)); "
        "INSERT INTO sequence_acceptance_cast_layout_assign VALUES "
        " (2,'ACGT'::bioseq.seq); "
        "INSERT INTO sequence_acceptance_cast_layout_assign VALUES "
        " (3,'AC'::bioseq.seq('dna4',8) OPERATOR(bioseq.||) "
        "    'GT'::bioseq.seq('dna4',8)); "
        "INSERT INTO sequence_acceptance_cast_layout_assign "
        "SELECT 4,s FROM sequence_acceptance_cast_matrix WHERE id=2; "
        "INSERT INTO sequence_acceptance_cast_layout_assign "
        "SELECT 5,s FROM sequence_acceptance_cast_source WHERE id=1;");

    const auto flat = query_one(
        ctx,
        "SELECT (s::bioseq.seq('dna4',8))::text, "
        "       (s::bioseq.seq('dna4',16))::text, "
        "       (s::bioseq.seq('dna4',32))::text, "
        "       (s::bioseq.seq('dna4',64))::text, "
        "       (s::bioseq.seq)::text "
        "FROM sequence_acceptance_cast_matrix WHERE id=1;");
    for (const auto& value : flat) {
        REQUIRE(value == "ACGT");
    }

    const auto typmodless = query_one(
        ctx,
        "SELECT bioseq.seq_send("
        "         'ACGT'::bioseq.seq('dna4',8)::bioseq.seq) = "
        "       bioseq.seq_send('ACGT'::bioseq.seq('dna4',8)), "
        "       bioseq.seq_cmp("
        "         'ACGT'::bioseq.seq::bioseq.seq, "
        "         'ACGT'::bioseq.seq::bioseq.seq('dna4',8)) < 0;");
    REQUIRE(typmodless[0] == "t");
    REQUIRE(typmodless[1] == "t");

    const auto unbound_bind = query_one(
        ctx,
        "SELECT ('ACGT'::bioseq.seq::bioseq.seq('dna4',8))::text, "
        "       ('ACGT'::bioseq.seq::bioseq.seq('dna4',16))::text, "
        "       ('ACGT'::bioseq.seq::bioseq.seq('dna4',32))::text, "
        "       ('ACGT'::bioseq.seq::bioseq.seq('dna4',64))::text;");
    for (const auto& value : unbound_bind) {
        REQUIRE(value == "ACGT");
    }
    const auto invalid_unbound_bind = sql_error(
        ctx,
        "SELECT 'ACGN'::bioseq.seq::bioseq.seq('dna4',8);");
    REQUIRE(invalid_unbound_bind.sqlstate == "22P02");

    REQUIRE(query_scalar_int(
        ctx,
        "SELECT bioseq.len(s::bioseq.seq('dna4',64)) "
        "FROM sequence_acceptance_cast_matrix WHERE id=2;") == 1200000);
    REQUIRE(query_scalar(
        ctx,
        "SELECT (s::bioseq.seq('dna4',64))[1199996:1200000]::text "
        "FROM sequence_acceptance_cast_matrix WHERE id=2;") == "ACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT (('ACGT'::bioseq.seq('dna4',8))[1:3]"
        "          ::bioseq.seq('dna4',64))::text;") == "CG");
    REQUIRE(query_scalar(
        ctx,
        "SELECT ('ACGT'::bioseq.seq::bioseq.seq('dna4',32))::text;")
        == "ACGT");

    const auto expanded = query_one(
        ctx,
        "SELECT ((('AC'::bioseq.seq OPERATOR(bioseq.||) "
        "          'GT'::bioseq.seq)"
        "          ::bioseq.seq('dna4',64)))::text, "
        "       (bioseq.reverse("
        "          'ACGT'::bioseq.seq('dna4',8))"
        "          ::bioseq.seq('dna4',16))::text, "
        "       bioseq.seq_cmp("
        "         bioseq.reverse('ACGT'::bioseq.seq)::bioseq.seq, "
        "         bioseq.reverse('ACGT'::bioseq.seq)"
        "           ::bioseq.seq('dna4',8)) < 0;");
    REQUIRE(expanded[0] == "ACGT");
    REQUIRE(expanded[1] == "TGCA");
    REQUIRE(expanded[2] == "t");

    const auto source = query_one(
        ctx,
        "SELECT (s::bioseq.seq('dna4',64))::text, "
        "       (s::bioseq.seq)::text, "
        "       (s::bioseq.seq('dna4',64))[1:3]::text "
        "FROM sequence_acceptance_cast_source WHERE id=1;");
    REQUIRE(source[0] == "ACGT");
    REQUIRE(source[1] == "ACGT");
    REQUIRE(source[2] == "CG");

    const auto assigned = query_one(
        ctx,
        "SELECT w8::text,w16::text,w32::text,w64::text "
        "FROM sequence_acceptance_cast_assign;");
    for (const auto& value : assigned) {
        REQUIRE(value == "ACGT");
    }
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,',' ORDER BY id) "
        "FROM sequence_acceptance_cast_layout_assign WHERE id IN (1,2,3,5);")
        == "ACGT,ACGT,ACGT,ACGT");
    const auto assigned_toast = query_one(
        ctx,
        "SELECT bioseq.len(value), value[1199996:1200000]::text "
        "FROM sequence_acceptance_cast_layout_assign WHERE id=4;");
    REQUIRE(assigned_toast[0] == "1200000");
    REQUIRE(assigned_toast[1] == "ACGT");

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    exec_sql(ctx,
        "CREATE TABLE sequence_acceptance_cast_dna5 "
        " (value bioseq.seq('dna5',64));");
    constexpr std::array cross_alphabet_explicit{
        "SELECT 'A'::bioseq.seq('dna4',8)"
        "         ::bioseq.seq('dna5',64);",
        "SELECT ('A'::bioseq.seq('dna4',8) OPERATOR(bioseq.||) "
        "        'C'::bioseq.seq('dna4',8))"
        "         ::bioseq.seq('dna5',64);",
        "SELECT s::bioseq.seq('dna5',64) "
        "FROM sequence_acceptance_cast_source WHERE id=1;",
    };
    for (const char* query : cross_alphabet_explicit) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "42804");
    }

    constexpr std::array cross_alphabet_assignment{
        "INSERT INTO sequence_acceptance_cast_dna5 "
        "VALUES ('A'::bioseq.seq('dna4',8));",
        "INSERT INTO sequence_acceptance_cast_dna5 "
        "VALUES ('A'::bioseq.seq('dna4',8) OPERATOR(bioseq.||) "
        "        'C'::bioseq.seq('dna4',8));",
        "INSERT INTO sequence_acceptance_cast_dna5 "
        "SELECT s FROM sequence_acceptance_cast_source WHERE id=1;",
    };
    for (const char* query : cross_alphabet_assignment) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "42804");
    }
#endif
}

TEST_CASE("logical equality order and hashes ignore storage width",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        "WITH v(a,b,c,d) AS (VALUES ("
        " 'ACGT'::bioseq.seq('dna4',8), "
        " 'ACGT'::bioseq.seq('dna4',16), "
        " 'ACGT'::bioseq.seq('dna4',32), "
        " 'ACGT'::bioseq.seq('dna4',64))) "
        "SELECT bioseq.seq_eq(a,b), bioseq.seq_eq(b,c), "
        "       bioseq.seq_eq(c,d), "
        "       bioseq.seq_cmp(a,d), "
        "       bioseq.seq_hash(a)=bioseq.seq_hash(b) "
        "         AND bioseq.seq_hash(b)=bioseq.seq_hash(c) "
        "         AND bioseq.seq_hash(c)=bioseq.seq_hash(d), "
        "       bioseq.seq_hash_extended(a,77)="
        "         bioseq.seq_hash_extended(d,77) "
        "FROM v;");
    REQUIRE(row[0] == "t");
    REQUIRE(row[1] == "t");
    REQUIRE(row[2] == "t");
    REQUIRE(row[3] == "0");
    REQUIRE(row[4] == "t");
    REQUIRE(row[5] == "t");

    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM ("
        " SELECT value FROM (VALUES "
        "  ('ACGT'::bioseq.seq('dna4',8)), "
        "  ('ACGT'::bioseq.seq('dna4',64)), "
        "  ('TGCA'::bioseq.seq('dna4',32))) AS x(value) "
        " GROUP BY value) AS grouped;") == 2);
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(DISTINCT value) FROM (VALUES "
        " ('ACGT'::bioseq.seq('dna4',8)), "
        " ('ACGT'::bioseq.seq('dna4',64)), "
        " ('TGCA'::bioseq.seq('dna4',32))) AS x(value);") == 2);

    const auto tagged = query_one(
        ctx,
        "WITH v(ua,uc,ba,bc) AS (VALUES ("
        " 'A'::bioseq.seq, 'C'::bioseq.seq, "
        " 'A'::bioseq.seq('dna4',8), "
        " 'C'::bioseq.seq('dna4',64))) "
        "SELECT bioseq.seq_cmp(ua,uc) < 0, "
        "       bioseq.seq_cmp(ba,bc) < 0, "
        "       bioseq.seq_cmp(ua,ba) < 0, "
        "       NOT bioseq.seq_eq(ua,ba), "
        "       bioseq.seq_hash(ua) <> bioseq.seq_hash(ba), "
        "       bioseq.seq_hash_extended(ua,77) <> "
        "         bioseq.seq_hash_extended(ba,77) "
        "FROM v;");
    for (const auto& value : tagged) {
        REQUIRE(value == "t");
    }

    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(label, ',' ORDER BY value) FROM (VALUES "
        " ('ua','A'::bioseq.seq), "
        " ('uc','C'::bioseq.seq), "
        " ('ba','A'::bioseq.seq('dna4',8)), "
        " ('bc','C'::bioseq.seq('dna4',64))) AS v(label,value);")
        == "ua,uc,ba,bc");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(DISTINCT value) FROM (VALUES "
        " ('A'::bioseq.seq), "
        " ('A'::bioseq.seq('dna4',8)), "
        " ('A'::bioseq.seq('dna4',64)), "
        " ('C'::bioseq.seq)) AS v(value);") == 3);

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const auto mixed_alphabet = query_one(
        ctx,
        "WITH v(d4,d5) AS (VALUES ("
        " 'A'::bioseq.seq('dna4',8), "
        " 'A'::bioseq.seq('dna5',64))) "
        "SELECT NOT bioseq.seq_eq(d4,d5), "
        "       bioseq.seq_cmp(d4,d5) < 0, "
        "       bioseq.seq_hash(d4) <> bioseq.seq_hash(d5), "
        "       bioseq.seq_hash_extended(d4,77) <> "
        "         bioseq.seq_hash_extended(d5,77) "
        "FROM v;");
    for (const auto& value : mixed_alphabet) {
        REQUIRE(value == "t");
    }
#endif
}

TEST_CASE("total sequence key agrees across states layouts and indexes",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({
        "sequence_acceptance_total_key",
        "sequence_acceptance_total_key_hash",
        "sequence_acceptance_total_key_join",
        "sequence_acceptance_total_key_source",
    });
    exec_sql(ctx,
        "SET search_path TO pg_catalog, bioseq, public; "
        "CREATE TABLE public.sequence_acceptance_total_key "
        " (label text, value bioseq.seq); "
        "INSERT INTO sequence_acceptance_total_key VALUES "
        " ('ua','A'::bioseq.seq); "
        "INSERT INTO sequence_acceptance_total_key VALUES "
        " ('uc','C'::bioseq.seq); "
        "INSERT INTO sequence_acceptance_total_key VALUES "
        " ('d4a8','A'::bioseq.seq('dna4',8)); "
        "INSERT INTO sequence_acceptance_total_key VALUES "
        " ('d4a64','A'::bioseq.seq('dna4',64)); "
        "INSERT INTO sequence_acceptance_total_key VALUES "
        " ('d4c','C'::bioseq.seq('dna4',32));");
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    exec_sql(ctx,
        "INSERT INTO sequence_acceptance_total_key VALUES "
        " ('d5a','A'::bioseq.seq('dna5',16));");
#endif
    exec_sql(ctx,
        "CREATE INDEX sequence_acceptance_total_key_btree_idx "
        " ON sequence_acceptance_total_key USING btree(value bioseq.seq_ops); "
        "CREATE TABLE public.sequence_acceptance_total_key_hash AS "
        " TABLE sequence_acceptance_total_key; "
        "CREATE INDEX sequence_acceptance_total_key_hash_idx "
        " ON sequence_acceptance_total_key_hash USING hash(value bioseq.seq_hash_ops); "
        "CREATE TABLE public.sequence_acceptance_total_key_join AS "
        " TABLE sequence_acceptance_total_key; "
        "CREATE TABLE public.sequence_acceptance_total_key_source "
        " (id int, value bioseq.seq('dna4',8)) USING bioseq; "
        "INSERT INTO sequence_acceptance_total_key_source VALUES "
        " (1,'A'::bioseq.seq('dna4',8)), "
        " (2,'C'::bioseq.seq('dna4',8)); "
        "CREATE INDEX sequence_acceptance_total_key_source_length_idx "
        " ON sequence_acceptance_total_key_source ((bioseq.len(value)));");

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    constexpr auto expected_order = "ua,uc,d4a64,d4a8,d4c,d5a";
    constexpr std::int64_t expected_distinct = 5;
    constexpr std::int64_t expected_join_rows = 8;
#else
    constexpr auto expected_order = "ua,uc,d4a64,d4a8,d4c";
    constexpr std::int64_t expected_distinct = 4;
    constexpr std::int64_t expected_join_rows = 7;
#endif
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(label,',' ORDER BY value,label) "
        "FROM sequence_acceptance_total_key;") == expected_order);
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(DISTINCT value) FROM sequence_acceptance_total_key;")
        == expected_distinct);
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM ("
        " SELECT value FROM sequence_acceptance_total_key GROUP BY value"
        ") AS groups;") == expected_distinct);

    {
        guc_scope no_seqscan(ctx, "enable_seqscan", "off");
        const auto btree_plan = explain_text<false>(
            ctx,
            "SELECT value FROM sequence_acceptance_total_key ORDER BY value;");
        REQUIRE(plan_contains(btree_plan, "sequence_acceptance_total_key_btree_idx"));
        REQUIRE(query_scalar(
            ctx,
            "SELECT string_agg(label,',' ORDER BY label) "
            "FROM sequence_acceptance_total_key "
            "WHERE value = 'A'::bioseq.seq('dna4',16);")
            == "d4a64,d4a8");

        const auto hash_plan = explain_text<false>(
            ctx,
            "SELECT label FROM sequence_acceptance_total_key_hash "
            "WHERE value = 'A'::bioseq.seq;");
        REQUIRE(plan_contains(hash_plan, "sequence_acceptance_total_key_hash_idx"));
        REQUIRE(query_scalar(
            ctx,
            "SELECT label FROM sequence_acceptance_total_key_hash "
            "WHERE value = 'A'::bioseq.seq;") == "ua");

        // Building this expression index reads locator stubs through the
        // standalone provider and promotion boundary.  A usable plan confirms
        // that the post-catch error tail did not alter the success path.
        const auto source_expression_plan = explain_text<false>(
            ctx,
            "SELECT id FROM sequence_acceptance_total_key_source "
            "WHERE bioseq.len(value)=1;");
        REQUIRE(plan_contains(
            source_expression_plan,
            "sequence_acceptance_total_key_source_length_idx"));
        REQUIRE(query_scalar_int(
            ctx,
            "SELECT count(*) FROM sequence_acceptance_total_key_source "
            "WHERE bioseq.len(value)=1;") == 2);
    }

    exec_sql(ctx,
        "SET enable_hashjoin=on; "
        "SET enable_nestloop=off; "
        "SET enable_mergejoin=off;");
    const auto hash_join_query =
        "SELECT count(*) FROM sequence_acceptance_total_key a "
        "JOIN sequence_acceptance_total_key_join b USING (value);";
    REQUIRE(query_scalar_int(ctx, hash_join_query) == expected_join_rows);
    REQUIRE(plan_contains(
        explain_text<false>(ctx, hash_join_query), "Hash Join"));

    /* The two aliases below scan one physical relation through independent
     * providers.  The probe also attests that relation-only lookup rejects
     * this ambiguity while each provider generation remains exact. */
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal.sequence_source_route_alias_probe();")
        == "t");
    constexpr auto source_alias_query =
        "SELECT bioseq.seq_eq(a.value,"
        "         'A'::bioseq.seq('dna4',64)), "
        "       bioseq.seq_hash(a.value) = bioseq.seq_hash("
        "         'A'::bioseq.seq('dna4',64)), "
        "       bioseq.seq_hash_extended(a.value,77) = "
        "         bioseq.seq_hash_extended("
        "           'A'::bioseq.seq('dna4',64),77), "
        "       bioseq.seq_eq(a.value,a.value[0:]), "
        "       bioseq.seq_hash(a.value) = "
        "         bioseq.seq_hash(a.value[0:]), "
        "       NOT bioseq.seq_eq(a.value,b.value), "
        "       bioseq.seq_cmp(a.value,b.value) < 0, "
        "       bioseq.seq_hash(a.value) <> bioseq.seq_hash(b.value) "
        "FROM sequence_acceptance_total_key_source a "
        "JOIN sequence_acceptance_total_key_source b ON a.id=1 AND b.id=2;";
    const auto source_alias_plan = explain_text<false>(
        ctx, source_alias_query);
    REQUIRE(plan_contains(source_alias_plan, "id = 1"));
    REQUIRE(plan_contains(source_alias_plan, "id = 2"));
    const auto layouts = query_one(ctx, source_alias_query);
    for (const auto& value : layouts) {
        REQUIRE(value == "t");
    }
}

TEST_CASE("typmodless letter persistence validates runtime Params",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    auto ctx = fresh_context({
        "sequence_acceptance_letter_param_target",
        "sequence_acceptance_letter_param_typed",
        "sequence_acceptance_letter_param_ctas_text",
        "sequence_acceptance_letter_param_ctas_binary",
    });
    exec_sql(ctx,
        "CREATE TABLE public.sequence_acceptance_letter_param_target "
        " (id int PRIMARY KEY, value bioseq.letter); "
        "CREATE TABLE public.sequence_acceptance_letter_param_typed "
        " (id int PRIMARY KEY, value bioseq.letter('dna4'));");

    const Oid letter_oid = static_cast<Oid>(query_scalar_int(
        ctx, "SELECT 'bioseq.letter'::regtype::oid;"));
    constexpr std::array<char, 3> physical_a{char{1}, char{0}, char{0}};
    constexpr std::array<char, 3> physical_c{char{1}, char{0}, char{1}};
    constexpr auto text_a = std::to_array("A");

    const auto ordinary_text = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_ordinary_text", "SELECT $1::text;",
        letter_oid, text_a.data(), 1, /* text = */ 0);
    REQUIRE(ordinary_text.status == PGRES_FATAL_ERROR);
    REQUIRE(ordinary_text.sqlstate == "22023");

    const auto ordinary_binary = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_ordinary_binary", "SELECT $1::text;",
        letter_oid, physical_a.data(), physical_a.size(), /* binary = */ 1);
    REQUIRE(ordinary_binary.status == PGRES_TUPLES_OK);
    REQUIRE(ordinary_binary.value == "A");

    const auto typmodless_text_insert = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_typmodless_insert",
        "INSERT INTO public.sequence_acceptance_letter_param_target VALUES (1,$1) "
        "RETURNING value::text;",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(typmodless_text_insert.status == PGRES_FATAL_ERROR);
    REQUIRE(typmodless_text_insert.sqlstate == "22023");
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM public.sequence_acceptance_letter_param_target;") == 0);

    const auto typmodless_binary_insert = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_binary_insert",
        "INSERT INTO public.sequence_acceptance_letter_param_target VALUES (1,$1) "
        "RETURNING value::text;",
        letter_oid, physical_a.data(), physical_a.size(), 1);
    REQUIRE(typmodless_binary_insert.status == PGRES_TUPLES_OK);
    REQUIRE(typmodless_binary_insert.value == "A");

    const auto typed_text_insert = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_typed_insert",
        "INSERT INTO public.sequence_acceptance_letter_param_typed VALUES (1,$1) "
        "RETURNING value::text;",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(typed_text_insert.status == PGRES_TUPLES_OK);
    REQUIRE(typed_text_insert.value == "A");

    const auto update_text = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_update_text",
        "UPDATE public.sequence_acceptance_letter_param_target SET value=$1 "
        "WHERE id=1 RETURNING value::text;",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(update_text.status == PGRES_FATAL_ERROR);
    REQUIRE(update_text.sqlstate == "22023");

    const auto update_binary = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_update_binary",
        "UPDATE public.sequence_acceptance_letter_param_target SET value=$1 "
        "WHERE id=1 RETURNING value::text;",
        letter_oid, physical_c.data(), physical_c.size(), 1);
    REQUIRE(update_binary.status == PGRES_TUPLES_OK);
    REQUIRE(update_binary.value == "C");

    const auto conflict_text = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_conflict_text",
        "INSERT INTO public.sequence_acceptance_letter_param_target "
        "VALUES (1,'A'::bioseq.letter('dna4')) "
        "ON CONFLICT (id) DO UPDATE SET value=$1 "
        "RETURNING value::text;",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(conflict_text.status == PGRES_FATAL_ERROR);
    REQUIRE(conflict_text.sqlstate == "22023");
    REQUIRE(query_scalar(
        ctx,
        "SELECT value::text FROM public.sequence_acceptance_letter_param_target "
        "WHERE id=1;") == "C");

    const auto insert_select_text = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_insert_select_text",
        "INSERT INTO public.sequence_acceptance_letter_param_target "
        "SELECT 5,$1 RETURNING value::text;",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(insert_select_text.status == PGRES_FATAL_ERROR);
    REQUIRE(insert_select_text.sqlstate == "22023");

    const auto merge_update_text = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_merge_update_text",
        "MERGE INTO public.sequence_acceptance_letter_param_target AS target "
        "USING (VALUES (1)) AS source(id) ON target.id=source.id "
        "WHEN MATCHED THEN UPDATE SET value=$1;",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(merge_update_text.status == PGRES_FATAL_ERROR);
    REQUIRE(merge_update_text.sqlstate == "22023");

    const auto merge_insert_text = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_merge_insert_text",
        "MERGE INTO public.sequence_acceptance_letter_param_target AS target "
        "USING (VALUES (6)) AS source(id) ON target.id=source.id "
        "WHEN NOT MATCHED THEN INSERT (id,value) VALUES (source.id,$1);",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(merge_insert_text.status == PGRES_FATAL_ERROR);
    REQUIRE(merge_insert_text.sqlstate == "22023");

    const auto ctas_text = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_ctas_text",
        "CREATE TABLE public.sequence_acceptance_letter_param_ctas_text AS "
        "SELECT $1 AS value;",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(ctas_text.status == PGRES_FATAL_ERROR);
    REQUIRE(ctas_text.sqlstate == "22023");
    REQUIRE(query_scalar(
        ctx,
        "SELECT to_regclass('public.sequence_acceptance_letter_param_ctas_text') "
        "IS NULL;") == "t");

    const auto ctas_binary = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_ctas_binary",
        "CREATE TABLE public.sequence_acceptance_letter_param_ctas_binary AS "
        "SELECT $1 AS value;",
        letter_oid, physical_a.data(), physical_a.size(), 1);
    REQUIRE(ctas_binary.status == PGRES_COMMAND_OK);
    REQUIRE(query_scalar(
        ctx,
        "SELECT value::text FROM "
        "public.sequence_acceptance_letter_param_ctas_binary;") == "A");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT atttypmod FROM pg_catalog.pg_attribute "
        "WHERE attrelid='public.sequence_acceptance_letter_param_ctas_binary'::regclass "
        "AND attname='value';") == -1);

    const auto explain_insert_text = execute_prepared_letter_parameter(
        ctx.raw(), "sequence_acceptance_param_explain_insert_text",
        "EXPLAIN (ANALYZE, COSTS OFF) "
        "INSERT INTO public.sequence_acceptance_letter_param_target VALUES (7,$1);",
        letter_oid, text_a.data(), 1, 0);
    REQUIRE(explain_insert_text.status == PGRES_FATAL_ERROR);
    REQUIRE(explain_insert_text.sqlstate == "22023");
}
