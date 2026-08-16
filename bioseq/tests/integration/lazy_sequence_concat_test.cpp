/**
 * @file lazy_sequence_concat_test.cpp
 * @brief Exercises lazy COALESCE, concatenation, and bridge contracts.
 */

#include <tests/support/postgres.hpp>

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

TEST_CASE("COALESCE short circuits without inspecting lazy arms",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.sequence_acceptance_raise_seq() RETURNS bioseq.seq "
        "LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected arm executed'; END $$;");

    const auto selected = query_one(
        ctx,
        "WITH v(s) AS (VALUES "
        " ('ACGTA'::bioseq.seq('dna4',32))) "
        "SELECT COALESCE(NULL::bioseq.seq('dna4',32), s[1:4]) "
        "         OPERATOR(bioseq.=) s[1:4], "
        "       bioseq.len(COALESCE("
        "         s[:2], pg_temp.sequence_acceptance_raise_seq()::"
        "                   bioseq.seq('dna4',32))), "
        "       pg_typeof(COALESCE(NULL::bioseq.seq('dna4',32), "
        "                          s[1:4]))::text "
        "FROM v;");
    REQUIRE(selected[0] == "t");
    REQUIRE(selected[1] == "2");
    REQUIRE(selected[2] == "bioseq.seq");

    REQUIRE(query_scalar(
        ctx,
        "SELECT COALESCE(NULL::bioseq.seq('dna4',32), "
        "                NULL::bioseq.seq('dna4',32)) IS NULL;") == "t");
    REQUIRE(query_scalar(
        ctx,
        "SELECT COALESCE('AC'::bioseq.seq('dna4',32), "
        "                pg_temp.sequence_acceptance_raise_seq()::"
        "                  bioseq.seq('dna4',32))::text;")
        == "AC");
    REQUIRE(query_scalar(
        ctx,
        "SELECT COALESCE('AC'::bioseq.seq, "
        "                pg_temp.sequence_acceptance_raise_seq())::text;") == "AC");
}

TEST_CASE("COALESCE-selected operands preserve the concat matrix",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "SET search_path TO pg_catalog, bioseq, public; "
        "CREATE FUNCTION pg_temp.sequence_acceptance_concat_raise_seq() "
        "RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected seq arm executed'; END $$; "
        "CREATE FUNCTION pg_temp.sequence_acceptance_concat_raise_text() "
        "RETURNS text LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected text arm executed'; END $$; "
        "CREATE FUNCTION pg_temp.sequence_acceptance_concat_raise_varchar() "
        "RETURNS varchar LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected varchar arm executed'; END $$; "
        "CREATE FUNCTION pg_temp.sequence_acceptance_concat_raise_text_array() "
        "RETURNS text[] LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected text[] arm executed'; END $$; "
        "CREATE FUNCTION pg_temp.sequence_acceptance_concat_raise_letter_array() "
        "RETURNS bioseq.letter[] LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected letter[] arm executed'; END $$;");

    const auto selected = query_one(
        ctx,
        "SELECT (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "        COALESCE('GT'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()))::text, "
        "       (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "        COALESCE(text 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_text()))::text, "
        "       (COALESCE(text 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_text()) || "
        "        COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()))::text, "
        "       (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "        COALESCE(varchar 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_varchar()))::text, "
        "       (COALESCE(varchar 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_varchar()) || "
        "        COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()))::text, "
        "       (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "        COALESCE(ARRAY['G','T']::text[], "
        "          pg_temp.sequence_acceptance_concat_raise_text_array()))::text, "
        "       (COALESCE(ARRAY['G','T']::text[], "
        "          pg_temp.sequence_acceptance_concat_raise_text_array()) || "
        "        COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()))::text, "
        "       (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "        COALESCE(ARRAY[]::bioseq.letter[], "
        "          pg_temp.sequence_acceptance_concat_raise_letter_array()))::text, "
        "       (COALESCE(ARRAY[]::bioseq.letter[], "
        "          pg_temp.sequence_acceptance_concat_raise_letter_array()) || "
        "        COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()))::text;");
    const std::array expected{
        "ACGT", "ACGT", "GTAC", "ACGT", "GTAC",
        "ACGT", "GTAC", "AC", "AC"};
    REQUIRE(selected.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE(selected[index] == expected[index]);
    }

    const auto unbound_states = query_one(
        ctx,
        "WITH v(a,b,c,d,e,f,g,h,i) AS (VALUES ("
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "   COALESCE('GT'::bioseq.seq, "
        "     pg_temp.sequence_acceptance_concat_raise_seq()), "
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "   COALESCE(text 'GT', "
        "     pg_temp.sequence_acceptance_concat_raise_text()), "
        " COALESCE(text 'GT', "
        "   pg_temp.sequence_acceptance_concat_raise_text()) || "
        "   COALESCE('AC'::bioseq.seq, "
        "     pg_temp.sequence_acceptance_concat_raise_seq()), "
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "   COALESCE(varchar 'GT', "
        "     pg_temp.sequence_acceptance_concat_raise_varchar()), "
        " COALESCE(varchar 'GT', "
        "   pg_temp.sequence_acceptance_concat_raise_varchar()) || "
        "   COALESCE('AC'::bioseq.seq, "
        "     pg_temp.sequence_acceptance_concat_raise_seq()), "
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "   COALESCE(ARRAY['G','T']::text[], "
        "     pg_temp.sequence_acceptance_concat_raise_text_array()), "
        " COALESCE(ARRAY['G','T']::text[], "
        "   pg_temp.sequence_acceptance_concat_raise_text_array()) || "
        "   COALESCE('AC'::bioseq.seq, "
        "     pg_temp.sequence_acceptance_concat_raise_seq()), "
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "   COALESCE(ARRAY[]::bioseq.letter[], "
        "     pg_temp.sequence_acceptance_concat_raise_letter_array()), "
        " COALESCE(ARRAY[]::bioseq.letter[], "
        "   pg_temp.sequence_acceptance_concat_raise_letter_array()) || "
        "   COALESCE('AC'::bioseq.seq, "
        "     pg_temp.sequence_acceptance_concat_raise_seq()))) "
        "SELECT bioseq.seq_cmp(a,a::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(b,b::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(c,c::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(d,d::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(e,e::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(f,f::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(g,g::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(h,h::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(i,i::bioseq.seq('dna4',8)) < 0 "
        "FROM v;");
    for (const auto& value : unbound_states) {
        REQUIRE(value == "t");
    }
    REQUIRE(sql_error(
        ctx,
        "SELECT (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "        COALESCE(text 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_text()))[0];")
        .sqlstate == "42804");

    const auto bound = query_one(
        ctx,
        "SELECT (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE('GT'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()))::text, "
        "       (COALESCE('GT'::bioseq.seq, "
        "          pg_temp.sequence_acceptance_concat_raise_seq()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE(text 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_text()))::text, "
        "       (COALESCE(varchar 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_varchar()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE(ARRAY['G','T']::text[], "
        "          pg_temp.sequence_acceptance_concat_raise_text_array()))::text, "
        "       (COALESCE(ARRAY["
        "          'G'::bioseq.letter('dna4'),"
        "          'T'::bioseq.letter('dna4')], "
        "          pg_temp.sequence_acceptance_concat_raise_letter_array()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE(text 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_text()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE(varchar 'GT', "
        "          pg_temp.sequence_acceptance_concat_raise_varchar()))::text, "
        "       (COALESCE(ARRAY['G','T']::text[], "
        "          pg_temp.sequence_acceptance_concat_raise_text_array()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.sequence_acceptance_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE(ARRAY["
        "          'G'::bioseq.letter('dna4'),"
        "          'T'::bioseq.letter('dna4')], "
        "          pg_temp.sequence_acceptance_concat_raise_letter_array()))::text;");
    const std::array bound_expected{
        "ACGT", "GTAC", "ACGT", "GTAC", "ACGT", "GTAC",
        "GTAC", "ACGT", "GTAC", "ACGT"};
    REQUIRE(bound.size() == bound_expected.size());
    for (std::size_t index = 0; index < bound_expected.size(); ++index) {
        REQUIRE(bound[index] == bound_expected[index]);
    }
}

TEST_CASE("concat overloads are symmetric strict and typed",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "SET search_path TO pg_catalog, bioseq, public;");

    const auto examples = query_one(
        ctx,
        "SELECT ('AC'::bioseq.seq('dna4',32) || 'TG')::text, "
        "       (text 'GT' || 'AC'::bioseq.seq('dna4',32))::text, "
        "       (ARRAY['A','CT','G'] || "
        "        'TA'::bioseq.seq('dna4',32))::text, "
        "       ('CG'::bioseq.seq('dna4',32) || "
        "        ARRAY['A'::bioseq.letter('dna4'), "
        "              'T'::bioseq.letter('dna4')])::text;");
    REQUIRE(examples[0] == "ACTG");
    REQUIRE(examples[1] == "GTAC");
    REQUIRE(examples[2] == "ACTGTA");
    REQUIRE(examples[3] == "CGAT");

    const auto symmetric = query_one(
        ctx,
        "SELECT (varchar 'GT' || "
        "        'AC'::bioseq.seq('dna4',8))::text, "
        "       ('AC'::bioseq.seq('dna4',8) || varchar 'GT')::text, "
        "       (ARRAY['G','T']::text[] || "
        "        'AC'::bioseq.seq('dna4',8))::text, "
        "       ('AC'::bioseq.seq('dna4',8) || "
        "        ARRAY['G','T']::text[])::text, "
        "       (ARRAY['G'::bioseq.letter('dna4'), "
        "              'T'::bioseq.letter('dna4')] || "
        "        'AC'::bioseq.seq('dna4',8))::text, "
        "       ('AC'::bioseq.seq('dna4',8) || "
        "        ARRAY['G'::bioseq.letter('dna4'), "
        "              'T'::bioseq.letter('dna4')])::text, "
        "       pg_typeof('AC'::bioseq.seq('dna4',8) || 'GT') = "
        "         'bioseq.seq'::regtype;");
    REQUIRE(symmetric[0] == "GTAC");
    REQUIRE(symmetric[1] == "ACGT");
    REQUIRE(symmetric[2] == "GTAC");
    REQUIRE(symmetric[3] == "ACGT");
    REQUIRE(symmetric[4] == "GTAC");
    REQUIRE(symmetric[5] == "ACGT");
    REQUIRE(symmetric[6] == "t");

    const auto identities = query_one(
        ctx,
        "SELECT ('AC'::bioseq.seq('dna4',8) || "
        "        ARRAY[]::text[])::text, "
        "       (ARRAY[]::text[] || "
        "        'AC'::bioseq.seq('dna4',8))::text, "
        "       ('AC'::bioseq.seq || ''::bioseq.seq)::text, "
        "       (''::bioseq.seq || 'AC'::bioseq.seq)::text;");
    REQUIRE(identities[0] == "AC");
    REQUIRE(identities[1] == "AC");
    REQUIRE(identities[2] == "AC");
    REQUIRE(identities[3] == "AC");

    REQUIRE(query_scalar(
        ctx,
        "SELECT (NULL::bioseq.seq || "
        "        'AC'::bioseq.seq('dna4',8)) IS NULL "
        "   AND ('AC'::bioseq.seq('dna4',8) || "
        "        NULL::bioseq.seq) IS NULL "
        "   AND ('AC'::bioseq.seq('dna4',8) || NULL::text) IS NULL "
        "   AND (NULL::text || 'AC'::bioseq.seq('dna4',8)) IS NULL "
        "   AND ('AC'::bioseq.seq('dna4',8) || NULL::varchar) IS NULL "
        "   AND (NULL::varchar || 'AC'::bioseq.seq('dna4',8)) IS NULL "
        "   AND ('AC'::bioseq.seq('dna4',8) || NULL::text[]) IS NULL "
        "   AND (NULL::text[] || 'AC'::bioseq.seq('dna4',8)) IS NULL "
        "   AND ('AC'::bioseq.seq('dna4',8) || "
        "        NULL::bioseq.letter[]) IS NULL "
        "   AND (NULL::bioseq.letter[] || "
        "        'AC'::bioseq.seq('dna4',8)) IS NULL;")
        == "t");

    constexpr std::array null_element_queries{
        "SELECT 'AC'::bioseq.seq('dna4',8) || "
        "       ARRAY['G'::text,NULL::text];",
        "SELECT ARRAY['G'::text,NULL::text] || "
        "       'AC'::bioseq.seq('dna4',8);",
        "SELECT 'AC'::bioseq.seq('dna4',8) || "
        "       ARRAY['G'::bioseq.letter('dna4'), "
        "             NULL::bioseq.letter];",
        "SELECT ARRAY['G'::bioseq.letter('dna4'), "
        "             NULL::bioseq.letter] || "
        "       'AC'::bioseq.seq('dna4',8);",
    };
    for (const char* query : null_element_queries) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "22004");
    }

    constexpr std::array multidimensional_queries{
        "SELECT 'AC'::bioseq.seq('dna4',8) || "
        "       ARRAY[['G','T'],['A','C']]::text[];",
        "SELECT ARRAY[['G','T'],['A','C']]::text[] || "
        "       'AC'::bioseq.seq('dna4',8);",
        "SELECT 'AC'::bioseq.seq('dna4',8) || "
        "       ARRAY[["
        "         'G'::bioseq.letter('dna4')],["
        "         'T'::bioseq.letter('dna4')]];",
        "SELECT ARRAY[["
        "         'G'::bioseq.letter('dna4')],["
        "         'T'::bioseq.letter('dna4')]] || "
        "       'AC'::bioseq.seq('dna4',8);",
    };
    for (const char* query : multidimensional_queries) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "2202E");
    }

    constexpr std::array invalid_fragment_queries{
        "SELECT 'AC'::bioseq.seq('dna4',8) || text 'N';",
        "SELECT text 'N' || 'AC'::bioseq.seq('dna4',8);",
        "SELECT 'AC'::bioseq.seq('dna4',8) || varchar 'N';",
        "SELECT varchar 'N' || 'AC'::bioseq.seq('dna4',8);",
        "SELECT 'AC'::bioseq.seq('dna4',8) || ARRAY['N']::text[];",
        "SELECT ARRAY['N']::text[] || 'AC'::bioseq.seq('dna4',8);",
    };
    for (const char* query : invalid_fragment_queries) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "22P02");
    }

    REQUIRE(query_scalar_int(
        ctx,
        "SELECT bioseq.len('AC'::bioseq.seq('dna4',8) || "
        "                  repeat('G',100000));") == 100002);

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    constexpr std::array mixed_letter_queries{
        "SELECT 'AC'::bioseq.seq('dna4',8) || "
        "       ARRAY['N'::bioseq.letter('dna5')];",
        "SELECT ARRAY['N'::bioseq.letter('dna5')] || "
        "       'AC'::bioseq.seq('dna4',8);",
    };
    for (const char* query : mixed_letter_queries) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "42804");
    }
#endif
}

TEST_CASE("unbound concat adopts only an explicit physical alphabet",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "SET search_path TO pg_catalog, bioseq, public;");

    const auto row = query_one(
        ctx,
        "SELECT ('AC'::bioseq.seq || 'GT'::bioseq.seq)::text, "
        "       ('AC'::bioseq.seq || "
        "        'GT'::bioseq.seq('dna4',8))::text, "
        "       ('AC'::bioseq.seq('dna4',8) || "
        "        'GT'::bioseq.seq)::text, "
        "       ('AC'::bioseq.seq || text 'GT')::text, "
        "       (text 'GT' || 'AC'::bioseq.seq)::text;");
    REQUIRE(row[0] == "ACGT");
    REQUIRE(row[1] == "ACGT");
    REQUIRE(row[2] == "ACGT");
    REQUIRE(row[3] == "ACGT");
    REQUIRE(row[4] == "GTAC");

    const auto transformed_adoption = query_one(
        ctx,
        "SELECT (bioseq.reverse("
        "          'AC'::bioseq.seq || 'GT'::bioseq.seq) || "
        "        'AA'::bioseq.seq('dna4',8))::text, "
        "       ('AA'::bioseq.seq('dna4',8) || "
        "        bioseq.reverse("
        "          'AC'::bioseq.seq || 'GT'::bioseq.seq))::text;");
    REQUIRE(transformed_adoption[0] == "TGCAAA");
    REQUIRE(transformed_adoption[1] == "AATGCA");

    const auto unbound_values = query_one(
        ctx,
        "SELECT ('AC'::bioseq.seq || 'GT'::bioseq.seq)::text, "
        "       ('AC'::bioseq.seq || text 'GT')::text, "
        "       (text 'GT' || 'AC'::bioseq.seq)::text, "
        "       ('AC'::bioseq.seq || varchar 'GT')::text, "
        "       (varchar 'GT' || 'AC'::bioseq.seq)::text, "
        "       ('AC'::bioseq.seq || ARRAY['G','T']::text[])::text, "
        "       (ARRAY['G','T']::text[] || "
        "        'AC'::bioseq.seq)::text;");
    REQUIRE(unbound_values[0] == "ACGT");
    REQUIRE(unbound_values[1] == "ACGT");
    REQUIRE(unbound_values[2] == "GTAC");
    REQUIRE(unbound_values[3] == "ACGT");
    REQUIRE(unbound_values[4] == "GTAC");
    REQUIRE(unbound_values[5] == "ACGT");
    REQUIRE(unbound_values[6] == "GTAC");

    const auto remains_unbound = query_one(
        ctx,
        "WITH v(a,b,c,d,e,f,g) AS (VALUES ("
        " 'AC'::bioseq.seq || 'GT'::bioseq.seq, "
        " 'AC'::bioseq.seq || text 'GT', "
        " text 'GT' || 'AC'::bioseq.seq, "
        " 'AC'::bioseq.seq || varchar 'GT', "
        " varchar 'GT' || 'AC'::bioseq.seq, "
        " 'AC'::bioseq.seq || ARRAY['G','T']::text[], "
        " ARRAY['G','T']::text[] || 'AC'::bioseq.seq)) "
        "SELECT bioseq.seq_cmp(a,a::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(b,b::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(c,c::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(d,d::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(e,e::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(f,f::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(g,g::bioseq.seq('dna4',8)) < 0 "
        "FROM v;");
    for (const auto& value : remains_unbound) {
        REQUIRE(value == "t");
    }

    const auto bound_adoption = query_one(
        ctx,
        "SELECT bioseq.seq_send('AC'::bioseq.seq || "
        "         'GT'::bioseq.seq('dna4',64)) = "
        "       bioseq.seq_send('ACGT'::bioseq.seq('dna4',64)), "
        "       bioseq.seq_send('AC'::bioseq.seq('dna4',64) || "
        "         'GT'::bioseq.seq) = "
        "       bioseq.seq_send('ACGT'::bioseq.seq('dna4',64)), "
        "       (('AC'::bioseq.seq || "
        "          'GT'::bioseq.seq('dna4',64))[3])::text, "
        "       (('AC'::bioseq.seq('dna4',64) || "
        "          'GT'::bioseq.seq)[0])::text;");
    REQUIRE(bound_adoption[0] == "t");
    REQUIRE(bound_adoption[1] == "t");
    REQUIRE(bound_adoption[2] == "T");
    REQUIRE(bound_adoption[3] == "A");

    const auto empty_letters = query_one(
        ctx,
        "SELECT ('AC'::bioseq.seq || "
        "        ARRAY[]::bioseq.letter[])::text, "
        "       (ARRAY[]::bioseq.letter[] || "
        "        'AC'::bioseq.seq)::text;");
    REQUIRE(empty_letters[0] == "AC");
    REQUIRE(empty_letters[1] == "AC");

    const auto empty_letters_stay_unbound = query_one(
        ctx,
        "WITH v(a,b) AS (VALUES ("
        " 'AC'::bioseq.seq || ARRAY[]::bioseq.letter[], "
        " ARRAY[]::bioseq.letter[] || 'AC'::bioseq.seq)) "
        "SELECT bioseq.seq_cmp(a,a::bioseq.seq('dna4',8)) < 0, "
        "       bioseq.seq_cmp(b,b::bioseq.seq('dna4',8)) < 0 "
        "FROM v;");
    REQUIRE(empty_letters_stay_unbound[0] == "t");
    REQUIRE(empty_letters_stay_unbound[1] == "t");

    constexpr std::array no_width_queries{
        "SELECT 'AC'::bioseq.seq || "
        "       ARRAY['G'::bioseq.letter('dna4')];",
        "SELECT ARRAY['G'::bioseq.letter('dna4')] || "
        "       'AC'::bioseq.seq;",
    };
    for (const char* query : no_width_queries) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "42804");
    }

    constexpr std::array invalid_adoption_queries{
        "SELECT 'AN'::bioseq.seq || "
        "       'C'::bioseq.seq('dna4',8);",
        "SELECT 'C'::bioseq.seq('dna4',8) || "
        "       'AN'::bioseq.seq;",
    };
    for (const char* query : invalid_adoption_queries) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "22P02");
    }

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const auto dna5_adoption = query_one(
        ctx,
        "SELECT ('AN'::bioseq.seq || "
        "        'N'::bioseq.seq('dna5',16))::text, "
        "       ('N'::bioseq.seq('dna5',16) || "
        "        'AN'::bioseq.seq)::text;");
    REQUIRE(dna5_adoption[0] == "ANN");
    REQUIRE(dna5_adoption[1] == "NAN");

    constexpr std::array mixed_sequence_queries{
        "SELECT 'A'::bioseq.seq('dna4',8) || "
        "       'A'::bioseq.seq('dna5',8);",
        "SELECT 'A'::bioseq.seq('dna5',8) || "
        "       'A'::bioseq.seq('dna4',8);",
    };
    for (const char* query : mixed_sequence_queries) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "42804");
    }
#endif
}

TEST_CASE("unbound values survive heap copy and binary round trips",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({
        "sequence_acceptance_unbound_source",
        "sequence_acceptance_unbound_copy",
    });
    exec_sql(ctx,
        "CREATE TABLE sequence_acceptance_unbound_source (value bioseq.seq); "
        "CREATE TABLE sequence_acceptance_unbound_copy (value bioseq.seq); "
        "INSERT INTO sequence_acceptance_unbound_source "
        "VALUES ('ACGT'::bioseq.seq); "
        "INSERT INTO sequence_acceptance_unbound_copy "
        "SELECT value FROM sequence_acceptance_unbound_source;");

    const auto stored = query_one(
        ctx,
        "SELECT value::text, bioseq.len(value), "
        "       bioseq.seq_send(value) = "
        "         bioseq.seq_send('ACGT'::bioseq.seq), "
        "       bioseq.seq_cmp(value,"
        "         value::bioseq.seq('dna4',64)) < 0 "
        "FROM sequence_acceptance_unbound_copy;");
    REQUIRE(stored[0] == "ACGT");
    REQUIRE(stored[1] == "4");
    REQUIRE(stored[2] == "t");
    REQUIRE(stored[3] == "t");

    const auto binary_path = fmt::format(
        "/tmp/bioseq_sequence_acceptance_unbound_{}.bin",
        query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    (void) std::remove(binary_path.c_str());
    exec_sql(ctx, "TRUNCATE sequence_acceptance_unbound_copy;");
    exec_sql(ctx, fmt::format(
        "COPY sequence_acceptance_unbound_source TO '{}' (FORMAT binary);",
        binary_path));
    exec_sql(ctx, fmt::format(
        "COPY sequence_acceptance_unbound_copy FROM '{}' (FORMAT binary);",
        binary_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT value::text FROM sequence_acceptance_unbound_copy;") == "ACGT");
    REQUIRE(std::remove(binary_path.c_str()) == 0);

    constexpr std::array alphabet_dependent_queries{
        "SELECT value[0] FROM sequence_acceptance_unbound_copy;",
        "SELECT bioseq.to_array(value) FROM sequence_acceptance_unbound_copy;",
        "SELECT bioseq.reverse_complement(value) "
        "FROM sequence_acceptance_unbound_copy;",
    };
    for (const char* query : alphabet_dependent_queries) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "42804");
    }

    const auto bound = query_one(
        ctx,
        "SELECT (value::bioseq.seq('dna4',64))[0]::text, "
        "       bioseq.reverse_complement("
        "         value::bioseq.seq('dna4',64))::text, "
        "       array_to_string(bioseq.to_array("
        "         value::bioseq.seq('dna4',64)),'') "
        "FROM sequence_acceptance_unbound_copy;");
    REQUIRE(bound[0] == "A");
    REQUIRE(bound[1] == "ACGT");
    REQUIRE(bound[2] == "ACGT");
}

TEST_CASE("sequence operators never resolve through string casts",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "SET search_path TO pg_catalog, public;");

    REQUIRE(sql_error(
        ctx,
        "SELECT 'AC'::bioseq.seq('dna4',8) || "
        "       'GT'::bioseq.seq('dna4',8);").sqlstate == "42883");
    REQUIRE(sql_error(
        ctx,
        "SELECT 'AC'::bioseq.seq('dna4',8) = "
        "       'AC'::bioseq.seq('dna4',8);").sqlstate == "42883");
    REQUIRE(sql_error(
        ctx,
        "SELECT reverse('AC'::bioseq.seq('dna4',8));").sqlstate == "42883");

    const auto qualified = query_one(
        ctx,
        "SELECT pg_typeof('AC'::bioseq.seq('dna4',8) "
        "                 OPERATOR(bioseq.||) "
        "                 'GT'::bioseq.seq('dna4',8))::text, "
        "       ('AC'::bioseq.seq('dna4',8) "
        "        OPERATOR(bioseq.=) "
        "        'AC'::bioseq.seq('dna4',8)), "
        "       bioseq.reverse('AC'::bioseq.seq('dna4',8))::text, "
        "       (('ACGT'::bioseq.seq('dna4',8))[0:2] "
        "        OPERATOR(bioseq.||) "
        "        ('ACGT'::bioseq.seq('dna4',8))[2:])::text;");
    REQUIRE(qualified[0] == "bioseq.seq");
    REQUIRE(qualified[1] == "t");
    REQUIRE(qualified[2] == "CA");
    REQUIRE(qualified[3] == "ACGT");
}

TEST_CASE("unsupported subscript shapes fail explicitly",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({"sequence_acceptance_subscript_errors"});
    exec_sql(ctx,
        "CREATE TABLE sequence_acceptance_subscript_errors "
        " (s bioseq.seq('dna4',8)); "
        "INSERT INTO sequence_acceptance_subscript_errors VALUES "
        " ('ACGT'::bioseq.seq('dna4',8));");

    const auto assignment = sql_error(
        ctx,
        "UPDATE sequence_acceptance_subscript_errors "
        "SET s[1] = 'T'::bioseq.letter('dna4');");
    REQUIRE(assignment.sqlstate == "0A000");

    const auto multidimensional = sql_error(
        ctx,
        "SELECT s[0:3][0:1] FROM sequence_acceptance_subscript_errors;");
    REQUIRE(multidimensional.sqlstate == "2202E");

    const auto stepped = sql_error(
        ctx,
        "SELECT s[0:4:2] FROM sequence_acceptance_subscript_errors;");
    REQUIRE((stepped.sqlstate == "0A000" || stepped.sqlstate == "42601"));
}

TEST_CASE("string and anyarray consumers require explicit bridges",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.sequence_acceptance_f_text(text) RETURNS text "
        "LANGUAGE sql IMMUTABLE AS 'SELECT $1'; "
        "CREATE FUNCTION pg_temp.sequence_acceptance_f_varchar(varchar) "
        "RETURNS text LANGUAGE sql IMMUTABLE AS 'SELECT $1::text'; "
        "CREATE FUNCTION pg_temp.sequence_acceptance_f_anyarray(anyarray) "
        "RETURNS int LANGUAGE sql IMMUTABLE "
        "AS 'SELECT cardinality($1)'; "
        "SET search_path TO pg_catalog, public;");

    REQUIRE(sql_error(
        ctx,
        "SELECT pg_temp.sequence_acceptance_f_text("
        " 'AC'::bioseq.seq('dna4',8));").sqlstate == "42883");
    REQUIRE(sql_error(
        ctx,
        "SELECT pg_temp.sequence_acceptance_f_varchar("
        " 'AC'::bioseq.seq('dna4',8));").sqlstate == "42883");
    REQUIRE(sql_error(
        ctx,
        "SELECT pg_temp.sequence_acceptance_f_anyarray("
        " 'AC'::bioseq.seq('dna4',8));").sqlstate == "42883");

    const auto explicit_bridges = query_one(
        ctx,
        "SELECT pg_temp.sequence_acceptance_f_text(bioseq.to_text("
        "         'AC'::bioseq.seq('dna4',8))), "
        "       pg_temp.sequence_acceptance_f_text("
        "         'AC'::bioseq.seq('dna4',8)::text), "
        "       pg_temp.sequence_acceptance_f_varchar("
        "         'AC'::bioseq.seq('dna4',8)::varchar), "
        "       pg_temp.sequence_acceptance_f_anyarray(bioseq.to_array("
        "         'AC'::bioseq.seq('dna4',8))), "
        "       pg_catalog.array_length(bioseq.to_array("
        "         'AC'::bioseq.seq('dna4',8)),1);");
    REQUIRE(explicit_bridges[0] == "AC");
    REQUIRE(explicit_bridges[1] == "AC");
    REQUIRE(explicit_bridges[2] == "AC");
    REQUIRE(explicit_bridges[3] == "2");
    REQUIRE(explicit_bridges[4] == "2");
}
