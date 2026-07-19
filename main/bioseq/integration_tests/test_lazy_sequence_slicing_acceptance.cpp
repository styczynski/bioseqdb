// DOC0377 §§17.1-17.2: normative SQL acceptance matrix.

#include "common_tests_integration.h"
#include "headers/pg_build_config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

void write_letter_binary_copy_variant(
    const std::string& source_path,
    const std::string& destination_path,
    std::uint8_t version,
    std::uint8_t alphabet_code,
    std::uint8_t rank)
{
    std::ifstream input(source_path, std::ios::binary);
    REQUIRE(input.is_open());
    std::vector<unsigned char> bytes{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};

    constexpr std::array<unsigned char, 11> signature{
        'P', 'G', 'C', 'O', 'P', 'Y', '\n', 0xffu, '\r', '\n', 0u};
    REQUIRE(bytes.size() == 30u);
    REQUIRE(std::equal(signature.begin(), signature.end(), bytes.begin()));
    REQUIRE(bytes[19] == 0u);
    REQUIRE(bytes[20] == 1u);
    REQUIRE(bytes[21] == 0u);
    REQUIRE(bytes[22] == 0u);
    REQUIRE(bytes[23] == 0u);
    REQUIRE(bytes[24] == 3u);
    REQUIRE(bytes[25] == 1u);
    REQUIRE(bytes[26] == 0u);
    REQUIRE(bytes[27] == 0u);
    REQUIRE(bytes[28] == 0xffu);
    REQUIRE(bytes[29] == 0xffu);

    bytes[25] = version;
    bytes[26] = alphabet_code;
    bytes[27] = rank;
    std::ofstream output(
        destination_path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("DOC0377 exact Python slicing examples and prompt algebra",
          "[seq][slice][acceptance]") {
    auto ctx = fresh_context();
    const auto elements = query_one(
        ctx,
        "WITH v(s) AS (VALUES "
        " ('ACGTA'::bioseq.seq('dna4',32))) "
        "SELECT s[0]::text, s[4]::text, s[-1]::text, s[-5]::text, "
        "       s[5] IS NULL, s[-6] IS NULL FROM v;");
    REQUIRE(elements[0] == "A");
    REQUIRE(elements[1] == "A");
    REQUIRE(elements[2] == "A");
    REQUIRE(elements[3] == "A");
    REQUIRE(elements[4] == "t");
    REQUIRE(elements[5] == "t");

    const auto slices = query_one(
        ctx,
        "WITH v(s) AS (VALUES "
        " ('ACGTA'::bioseq.seq('dna4',32))) "
        "SELECT s[:2]::text, s[2:]::text, s[1:4]::text, "
        "       s[1:-1]::text, s[-4:-1]::text, "
        "       s[-100:100]::text, bioseq.len(s[4:2]), "
        "       bioseq.len(s[5:5]) FROM v;");
    REQUIRE(slices[0] == "AC");
    REQUIRE(slices[1] == "GTA");
    REQUIRE(slices[2] == "CGT");
    REQUIRE(slices[3] == "CGT");
    REQUIRE(slices[4] == "CGT");
    REQUIRE(slices[5] == "ACGTA");
    REQUIRE(slices[6] == "0");
    REQUIRE(slices[7] == "0");

    const auto null_bounds = query_one(
        ctx,
        "WITH v(s) AS (VALUES "
        " ('ACGTA'::bioseq.seq('dna4',32))) "
        "SELECT s[NULL::bigint:2] IS NULL, "
        "       s[:NULL::bigint] IS NULL FROM v;");
    REQUIRE(null_bounds[0] == "t");
    REQUIRE(null_bounds[1] == "t");

    REQUIRE(query_scalar(
        ctx,
        "SELECT (('CTAGTCACTGTAGCCTGATCGTAGCTAGT' "
        "         ::bioseq.seq('dna4',32))[4:9])::text;") == "TCACT");

    const auto algebra = query_one(
        ctx,
        "WITH v(a,b) AS (VALUES "
        " ('ACGTACGTACGT'::bioseq.seq('dna4',32), "
        "  'TTGCAAGCTTAA'::bioseq.seq('dna4',32))) "
        "SELECT (((a[:10])[5:] OPERATOR(bioseq.||) b[5:10])"
        "          [1:-1])::text, "
        "       (a[6:10] OPERATOR(bioseq.||) b[5:9])::text, "
        "       (((a[:10])[5:] OPERATOR(bioseq.||) b[5:10])"
        "          [1:-1]) OPERATOR(bioseq.=) "
        "       (a[6:10] OPERATOR(bioseq.||) b[5:9]) "
        "FROM v;");
    REQUIRE(algebra[0] == "GTACAGCT");
    REQUIRE(algebra[1] == "GTACAGCT");
    REQUIRE(algebra[2] == "t");
}

TEST_CASE("DOC0377 COALESCE short circuits without inspecting lazy arms",
          "[seq][slice][coalesce][acceptance]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.doc0377_raise_seq() RETURNS bioseq.seq "
        "LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'DOC0377 unselected arm executed'; END $$;");

    const auto selected = query_one(
        ctx,
        "WITH v(s) AS (VALUES "
        " ('ACGTA'::bioseq.seq('dna4',32))) "
        "SELECT COALESCE(NULL::bioseq.seq('dna4',32), s[1:4]) "
        "         OPERATOR(bioseq.=) s[1:4], "
        "       bioseq.len(COALESCE("
        "         s[:2], pg_temp.doc0377_raise_seq()::"
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
        "                pg_temp.doc0377_raise_seq()::"
        "                  bioseq.seq('dna4',32))::text;")
        == "AC");
    REQUIRE(query_scalar(
        ctx,
        "SELECT COALESCE('AC'::bioseq.seq, "
        "                pg_temp.doc0377_raise_seq())::text;") == "AC");
}

TEST_CASE("DOC0377 COALESCE-selected operands preserve the concat matrix",
          "[seq][concat][coalesce][acceptance]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "SET search_path TO pg_catalog, bioseq, public; "
        "CREATE FUNCTION pg_temp.doc0377_concat_raise_seq() "
        "RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected seq arm executed'; END $$; "
        "CREATE FUNCTION pg_temp.doc0377_concat_raise_text() "
        "RETURNS text LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected text arm executed'; END $$; "
        "CREATE FUNCTION pg_temp.doc0377_concat_raise_varchar() "
        "RETURNS varchar LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected varchar arm executed'; END $$; "
        "CREATE FUNCTION pg_temp.doc0377_concat_raise_text_array() "
        "RETURNS text[] LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected text[] arm executed'; END $$; "
        "CREATE FUNCTION pg_temp.doc0377_concat_raise_letter_array() "
        "RETURNS bioseq.letter[] LANGUAGE plpgsql VOLATILE AS $$ "
        "BEGIN RAISE EXCEPTION 'unselected letter[] arm executed'; END $$;");

    const auto selected = query_one(
        ctx,
        "SELECT (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()) || "
        "        COALESCE('GT'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()))::text, "
        "       (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()) || "
        "        COALESCE(text 'GT', "
        "          pg_temp.doc0377_concat_raise_text()))::text, "
        "       (COALESCE(text 'GT', "
        "          pg_temp.doc0377_concat_raise_text()) || "
        "        COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()))::text, "
        "       (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()) || "
        "        COALESCE(varchar 'GT', "
        "          pg_temp.doc0377_concat_raise_varchar()))::text, "
        "       (COALESCE(varchar 'GT', "
        "          pg_temp.doc0377_concat_raise_varchar()) || "
        "        COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()))::text, "
        "       (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()) || "
        "        COALESCE(ARRAY['G','T']::text[], "
        "          pg_temp.doc0377_concat_raise_text_array()))::text, "
        "       (COALESCE(ARRAY['G','T']::text[], "
        "          pg_temp.doc0377_concat_raise_text_array()) || "
        "        COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()))::text, "
        "       (COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()) || "
        "        COALESCE(ARRAY[]::bioseq.letter[], "
        "          pg_temp.doc0377_concat_raise_letter_array()))::text, "
        "       (COALESCE(ARRAY[]::bioseq.letter[], "
        "          pg_temp.doc0377_concat_raise_letter_array()) || "
        "        COALESCE('AC'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()))::text;");
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
        "   pg_temp.doc0377_concat_raise_seq()) || "
        "   COALESCE('GT'::bioseq.seq, "
        "     pg_temp.doc0377_concat_raise_seq()), "
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.doc0377_concat_raise_seq()) || "
        "   COALESCE(text 'GT', "
        "     pg_temp.doc0377_concat_raise_text()), "
        " COALESCE(text 'GT', "
        "   pg_temp.doc0377_concat_raise_text()) || "
        "   COALESCE('AC'::bioseq.seq, "
        "     pg_temp.doc0377_concat_raise_seq()), "
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.doc0377_concat_raise_seq()) || "
        "   COALESCE(varchar 'GT', "
        "     pg_temp.doc0377_concat_raise_varchar()), "
        " COALESCE(varchar 'GT', "
        "   pg_temp.doc0377_concat_raise_varchar()) || "
        "   COALESCE('AC'::bioseq.seq, "
        "     pg_temp.doc0377_concat_raise_seq()), "
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.doc0377_concat_raise_seq()) || "
        "   COALESCE(ARRAY['G','T']::text[], "
        "     pg_temp.doc0377_concat_raise_text_array()), "
        " COALESCE(ARRAY['G','T']::text[], "
        "   pg_temp.doc0377_concat_raise_text_array()) || "
        "   COALESCE('AC'::bioseq.seq, "
        "     pg_temp.doc0377_concat_raise_seq()), "
        " COALESCE('AC'::bioseq.seq, "
        "   pg_temp.doc0377_concat_raise_seq()) || "
        "   COALESCE(ARRAY[]::bioseq.letter[], "
        "     pg_temp.doc0377_concat_raise_letter_array()), "
        " COALESCE(ARRAY[]::bioseq.letter[], "
        "   pg_temp.doc0377_concat_raise_letter_array()) || "
        "   COALESCE('AC'::bioseq.seq, "
        "     pg_temp.doc0377_concat_raise_seq()))) "
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
        "          pg_temp.doc0377_concat_raise_seq()) || "
        "        COALESCE(text 'GT', "
        "          pg_temp.doc0377_concat_raise_text()))[0];")
        .sqlstate == "42804");

    const auto bound = query_one(
        ctx,
        "SELECT (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE('GT'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()))::text, "
        "       (COALESCE('GT'::bioseq.seq, "
        "          pg_temp.doc0377_concat_raise_seq()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE(text 'GT', "
        "          pg_temp.doc0377_concat_raise_text()))::text, "
        "       (COALESCE(varchar 'GT', "
        "          pg_temp.doc0377_concat_raise_varchar()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE(ARRAY['G','T']::text[], "
        "          pg_temp.doc0377_concat_raise_text_array()))::text, "
        "       (COALESCE(ARRAY["
        "          'G'::bioseq.letter('dna4'),"
        "          'T'::bioseq.letter('dna4')], "
        "          pg_temp.doc0377_concat_raise_letter_array()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE(text 'GT', "
        "          pg_temp.doc0377_concat_raise_text()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE(varchar 'GT', "
        "          pg_temp.doc0377_concat_raise_varchar()))::text, "
        "       (COALESCE(ARRAY['G','T']::text[], "
        "          pg_temp.doc0377_concat_raise_text_array()) || "
        "        COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)))::text, "
        "       (COALESCE('AC'::bioseq.seq('dna4',64), "
        "          pg_temp.doc0377_concat_raise_seq()::"
        "            bioseq.seq('dna4',64)) || "
        "        COALESCE(ARRAY["
        "          'G'::bioseq.letter('dna4'),"
        "          'T'::bioseq.letter('dna4')], "
        "          pg_temp.doc0377_concat_raise_letter_array()))::text;");
    const std::array bound_expected{
        "ACGT", "GTAC", "ACGT", "GTAC", "ACGT", "GTAC",
        "GTAC", "ACGT", "GTAC", "ACGT"};
    REQUIRE(bound.size() == bound_expected.size());
    for (std::size_t index = 0; index < bound_expected.size(); ++index) {
        REQUIRE(bound[index] == bound_expected[index]);
    }
}

TEST_CASE("DOC0377 concat overloads are symmetric strict and typed",
          "[seq][concat][array][acceptance]") {
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
        "       pg_typeof('AC'::bioseq.seq('dna4',8) || 'GT')::text;");
    REQUIRE(symmetric[0] == "GTAC");
    REQUIRE(symmetric[1] == "ACGT");
    REQUIRE(symmetric[2] == "GTAC");
    REQUIRE(symmetric[3] == "ACGT");
    REQUIRE(symmetric[4] == "GTAC");
    REQUIRE(symmetric[5] == "ACGT");
    REQUIRE(symmetric[6] == "bioseq.seq");

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

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
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

TEST_CASE("DOC0377 unbound concat adopts only an explicit physical alphabet",
          "[seq][concat][unbound][acceptance]") {
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

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
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

TEST_CASE("DOC0377 unbound values survive heap copy and binary round trips",
          "[seq][unbound][copy][binary][acceptance]") {
    auto ctx = fresh_context({
        "doc0377_unbound_source",
        "doc0377_unbound_copy",
    });
    exec_sql(ctx,
        "CREATE TABLE doc0377_unbound_source (value bioseq.seq); "
        "CREATE TABLE doc0377_unbound_copy (value bioseq.seq); "
        "INSERT INTO doc0377_unbound_source "
        "VALUES ('ACGT'::bioseq.seq); "
        "INSERT INTO doc0377_unbound_copy "
        "SELECT value FROM doc0377_unbound_source;");

    const auto stored = query_one(
        ctx,
        "SELECT value::text, bioseq.len(value), "
        "       bioseq.seq_send(value) = "
        "         bioseq.seq_send('ACGT'::bioseq.seq), "
        "       bioseq.seq_cmp(value,"
        "         value::bioseq.seq('dna4',64)) < 0 "
        "FROM doc0377_unbound_copy;");
    REQUIRE(stored[0] == "ACGT");
    REQUIRE(stored[1] == "4");
    REQUIRE(stored[2] == "t");
    REQUIRE(stored[3] == "t");

    const auto binary_path = fmt::format(
        "/tmp/bioseq_doc0377_unbound_{}.bin",
        query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    (void) std::remove(binary_path.c_str());
    exec_sql(ctx, "TRUNCATE doc0377_unbound_copy;");
    exec_sql(ctx, fmt::format(
        "COPY doc0377_unbound_source TO '{}' (FORMAT binary);",
        binary_path));
    exec_sql(ctx, fmt::format(
        "COPY doc0377_unbound_copy FROM '{}' (FORMAT binary);",
        binary_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT value::text FROM doc0377_unbound_copy;") == "ACGT");
    REQUIRE(std::remove(binary_path.c_str()) == 0);

    constexpr std::array alphabet_dependent_queries{
        "SELECT value[0] FROM doc0377_unbound_copy;",
        "SELECT bioseq.to_array(value) FROM doc0377_unbound_copy;",
        "SELECT bioseq.reverse_complement(value) "
        "FROM doc0377_unbound_copy;",
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
        "FROM doc0377_unbound_copy;");
    REQUIRE(bound[0] == "A");
    REQUIRE(bound[1] == "ACGT");
    REQUIRE(bound[2] == "ACGT");
}

TEST_CASE("DOC0377 sequence operators never resolve through string casts",
          "[seq][operator][search_path][acceptance]") {
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
        "       bioseq.reverse('AC'::bioseq.seq('dna4',8))::text;");
    REQUIRE(qualified[0] == "bioseq.seq");
    REQUIRE(qualified[1] == "t");
    REQUIRE(qualified[2] == "CA");
}

TEST_CASE("DOC0377 unsupported subscript shapes fail explicitly",
          "[seq][slice][error][acceptance]") {
    auto ctx = fresh_context({"doc0377_subscript_errors"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_subscript_errors "
        " (s bioseq.seq('dna4',8)); "
        "INSERT INTO doc0377_subscript_errors VALUES "
        " ('ACGT'::bioseq.seq('dna4',8));");

    const auto assignment = sql_error(
        ctx,
        "UPDATE doc0377_subscript_errors "
        "SET s[1] = 'T'::bioseq.letter('dna4');");
    REQUIRE(assignment.sqlstate == "0A000");

    const auto multidimensional = sql_error(
        ctx,
        "SELECT s[0:3][0:1] FROM doc0377_subscript_errors;");
    REQUIRE(multidimensional.sqlstate == "0A000");

    const auto stepped = sql_error(
        ctx,
        "SELECT s[0:4:2] FROM doc0377_subscript_errors;");
    REQUIRE((stepped.sqlstate == "0A000" || stepped.sqlstate == "42601"));
}

TEST_CASE("DOC0377 string and anyarray consumers require explicit bridges",
          "[seq][cast][array][search_path][acceptance]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.doc0377_f_text(text) RETURNS text "
        "LANGUAGE sql IMMUTABLE AS 'SELECT $1'; "
        "CREATE FUNCTION pg_temp.doc0377_f_varchar(varchar) "
        "RETURNS text LANGUAGE sql IMMUTABLE AS 'SELECT $1::text'; "
        "CREATE FUNCTION pg_temp.doc0377_f_anyarray(anyarray) "
        "RETURNS int LANGUAGE sql IMMUTABLE "
        "AS 'SELECT cardinality($1)'; "
        "SET search_path TO pg_catalog, public;");

    REQUIRE(sql_error(
        ctx,
        "SELECT pg_temp.doc0377_f_text("
        " 'AC'::bioseq.seq('dna4',8));").sqlstate == "42883");
    REQUIRE(sql_error(
        ctx,
        "SELECT pg_temp.doc0377_f_varchar("
        " 'AC'::bioseq.seq('dna4',8));").sqlstate == "42883");
    REQUIRE(sql_error(
        ctx,
        "SELECT pg_temp.doc0377_f_anyarray("
        " 'AC'::bioseq.seq('dna4',8));").sqlstate == "42883");

    const auto explicit_bridges = query_one(
        ctx,
        "SELECT pg_temp.doc0377_f_text(bioseq.to_text("
        "         'AC'::bioseq.seq('dna4',8))), "
        "       pg_temp.doc0377_f_text("
        "         'AC'::bioseq.seq('dna4',8)::text), "
        "       pg_temp.doc0377_f_varchar("
        "         'AC'::bioseq.seq('dna4',8)::varchar), "
        "       pg_temp.doc0377_f_anyarray(bioseq.to_array("
        "         'AC'::bioseq.seq('dna4',8))), "
        "       pg_catalog.array_length(bioseq.to_array("
        "         'AC'::bioseq.seq('dna4',8)),1);");
    REQUIRE(explicit_bridges[0] == "AC");
    REQUIRE(explicit_bridges[1] == "AC");
    REQUIRE(explicit_bridges[2] == "AC");
    REQUIRE(explicit_bridges[3] == "2");
    REQUIRE(explicit_bridges[4] == "2");
}

TEST_CASE("DOC0377 self casts preserve logical values across widths and layouts",
          "[seq][cast][toast][acceptance]") {
    auto ctx = fresh_context({
        "doc0377_cast_matrix",
        "doc0377_cast_source",
        "doc0377_cast_assign",
        "doc0377_cast_layout_assign",
        "doc0377_cast_dna5",
    });
    exec_sql(ctx,
        "CREATE TABLE doc0377_cast_matrix "
        " (id int, s bioseq.seq('dna4',8)); "
        "ALTER TABLE doc0377_cast_matrix ALTER COLUMN s SET STORAGE EXTERNAL; "
        "INSERT INTO doc0377_cast_matrix VALUES "
        " (1,'ACGT'::bioseq.seq('dna4',8)), "
        " (2,repeat('ACGT',300000)::bioseq.seq('dna4',8)); "
        "CREATE TABLE doc0377_cast_source "
        " (id int, s bioseq.seq('dna4',8)) USING bioseq; "
        "INSERT INTO doc0377_cast_source VALUES "
        " (1,'ACGT'::bioseq.seq('dna4',8)); "
        "CREATE TABLE doc0377_cast_assign "
        " (w8 bioseq.seq('dna4',8), "
        "  w16 bioseq.seq('dna4',16), "
        "  w32 bioseq.seq('dna4',32), "
        "  w64 bioseq.seq('dna4',64)); "
        "INSERT INTO doc0377_cast_assign "
        "SELECT s,s,s,s FROM doc0377_cast_source WHERE id=1; "
        "CREATE TABLE doc0377_cast_layout_assign "
        " (id int, value bioseq.seq('dna4',64)); "
        "INSERT INTO doc0377_cast_layout_assign VALUES "
        " (1,'ACGT'::bioseq.seq('dna4',8)); "
        "INSERT INTO doc0377_cast_layout_assign VALUES "
        " (2,'ACGT'::bioseq.seq); "
        "INSERT INTO doc0377_cast_layout_assign VALUES "
        " (3,'AC'::bioseq.seq('dna4',8) || "
        "    'GT'::bioseq.seq('dna4',8)); "
        "INSERT INTO doc0377_cast_layout_assign "
        "SELECT 4,s FROM doc0377_cast_matrix WHERE id=2; "
        "INSERT INTO doc0377_cast_layout_assign "
        "SELECT 5,s FROM doc0377_cast_source WHERE id=1;");

    const auto flat = query_one(
        ctx,
        "SELECT (s::bioseq.seq('dna4',8))::text, "
        "       (s::bioseq.seq('dna4',16))::text, "
        "       (s::bioseq.seq('dna4',32))::text, "
        "       (s::bioseq.seq('dna4',64))::text, "
        "       (s::bioseq.seq)::text "
        "FROM doc0377_cast_matrix WHERE id=1;");
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
        "FROM doc0377_cast_matrix WHERE id=2;") == 1200000);
    REQUIRE(query_scalar(
        ctx,
        "SELECT (s::bioseq.seq('dna4',64))[1199996:1200000]::text "
        "FROM doc0377_cast_matrix WHERE id=2;") == "ACGT");
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
        "SELECT ((('AC'::bioseq.seq || 'GT'::bioseq.seq)"
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
        "FROM doc0377_cast_source WHERE id=1;");
    REQUIRE(source[0] == "ACGT");
    REQUIRE(source[1] == "ACGT");
    REQUIRE(source[2] == "CG");

    const auto assigned = query_one(
        ctx,
        "SELECT w8::text,w16::text,w32::text,w64::text "
        "FROM doc0377_cast_assign;");
    for (const auto& value : assigned) {
        REQUIRE(value == "ACGT");
    }
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,',' ORDER BY id) "
        "FROM doc0377_cast_layout_assign WHERE id IN (1,2,3,5);")
        == "ACGT,ACGT,ACGT,ACGT");
    const auto assigned_toast = query_one(
        ctx,
        "SELECT bioseq.len(value), value[1199996:1200000]::text "
        "FROM doc0377_cast_layout_assign WHERE id=4;");
    REQUIRE(assigned_toast[0] == "1200000");
    REQUIRE(assigned_toast[1] == "ACGT");

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
    exec_sql(ctx,
        "CREATE TABLE doc0377_cast_dna5 "
        " (value bioseq.seq('dna5',64));");
    constexpr std::array cross_alphabet_explicit{
        "SELECT 'A'::bioseq.seq('dna4',8)"
        "         ::bioseq.seq('dna5',64);",
        "SELECT ('A'::bioseq.seq('dna4',8) || "
        "        'C'::bioseq.seq('dna4',8))"
        "         ::bioseq.seq('dna5',64);",
        "SELECT s::bioseq.seq('dna5',64) "
        "FROM doc0377_cast_source WHERE id=1;",
    };
    for (const char* query : cross_alphabet_explicit) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "42804");
    }

    constexpr std::array cross_alphabet_assignment{
        "INSERT INTO doc0377_cast_dna5 "
        "VALUES ('A'::bioseq.seq('dna4',8));",
        "INSERT INTO doc0377_cast_dna5 "
        "VALUES ('A'::bioseq.seq('dna4',8) || "
        "        'C'::bioseq.seq('dna4',8));",
        "INSERT INTO doc0377_cast_dna5 "
        "SELECT s FROM doc0377_cast_source WHERE id=1;",
    };
    for (const char* query : cross_alphabet_assignment) {
        CAPTURE(query);
        REQUIRE(sql_error(ctx, query).sqlstate == "42804");
    }
#endif
}

TEST_CASE("DOC0377 logical equality order and hashes ignore storage width",
          "[seq][compare][hash][group][acceptance]") {
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

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
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

TEST_CASE("DOC0377 total sequence key agrees across states layouts and indexes",
          "[seq][compare][btree][hash][source][acceptance]") {
    auto ctx = fresh_context({
        "doc0377_total_key",
        "doc0377_total_key_hash",
        "doc0377_total_key_join",
        "doc0377_total_key_source",
    });
    exec_sql(ctx,
        "SET search_path TO pg_catalog, bioseq, public; "
        "CREATE TABLE doc0377_total_key "
        " (label text, value bioseq.seq); "
        "INSERT INTO doc0377_total_key VALUES "
        " ('ua','A'::bioseq.seq); "
        "INSERT INTO doc0377_total_key VALUES "
        " ('uc','C'::bioseq.seq); "
        "INSERT INTO doc0377_total_key VALUES "
        " ('d4a8','A'::bioseq.seq('dna4',8)); "
        "INSERT INTO doc0377_total_key VALUES "
        " ('d4a64','A'::bioseq.seq('dna4',64)); "
        "INSERT INTO doc0377_total_key VALUES "
        " ('d4c','C'::bioseq.seq('dna4',32));");
#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
    exec_sql(ctx,
        "INSERT INTO doc0377_total_key VALUES "
        " ('d5a','A'::bioseq.seq('dna5',16));");
#endif
    exec_sql(ctx,
        "CREATE INDEX doc0377_total_key_btree_idx "
        " ON doc0377_total_key USING btree(value bioseq.seq_ops); "
        "CREATE TABLE doc0377_total_key_hash AS "
        " TABLE doc0377_total_key; "
        "CREATE INDEX doc0377_total_key_hash_idx "
        " ON doc0377_total_key_hash USING hash(value bioseq.seq_hash_ops); "
        "CREATE TABLE doc0377_total_key_join AS "
        " TABLE doc0377_total_key; "
        "CREATE TABLE doc0377_total_key_source "
        " (id int, value bioseq.seq('dna4',8)) USING bioseq; "
        "INSERT INTO doc0377_total_key_source VALUES "
        " (1,'A'::bioseq.seq('dna4',8)), "
        " (2,'C'::bioseq.seq('dna4',8));");

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
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
        "FROM doc0377_total_key;") == expected_order);
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(DISTINCT value) FROM doc0377_total_key;")
        == expected_distinct);
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM ("
        " SELECT value FROM doc0377_total_key GROUP BY value"
        ") AS groups;") == expected_distinct);

    {
        guc_scope no_seqscan(ctx, "enable_seqscan", "off");
        const auto btree_plan = explain_text<false>(
            ctx,
            "SELECT value FROM doc0377_total_key ORDER BY value;");
        REQUIRE(plan_contains(btree_plan, "doc0377_total_key_btree_idx"));
        REQUIRE(query_scalar(
            ctx,
            "SELECT string_agg(label,',' ORDER BY label) "
            "FROM doc0377_total_key "
            "WHERE value = 'A'::bioseq.seq('dna4',16);")
            == "d4a64,d4a8");

        const auto hash_plan = explain_text<false>(
            ctx,
            "SELECT label FROM doc0377_total_key_hash "
            "WHERE value = 'A'::bioseq.seq;");
        REQUIRE(plan_contains(hash_plan, "doc0377_total_key_hash_idx"));
        REQUIRE(query_scalar(
            ctx,
            "SELECT label FROM doc0377_total_key_hash "
            "WHERE value = 'A'::bioseq.seq;") == "ua");
    }

    exec_sql(ctx,
        "SET enable_hashjoin=on; "
        "SET enable_nestloop=off; "
        "SET enable_mergejoin=off;");
    const auto hash_join_query =
        "SELECT count(*) FROM doc0377_total_key a "
        "JOIN doc0377_total_key_join b USING (value);";
    REQUIRE(query_scalar_int(ctx, hash_join_query) == expected_join_rows);
    REQUIRE(plan_contains(
        explain_text<false>(ctx, hash_join_query), "Hash Join"));

    const auto layouts = query_one(
        ctx,
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
        "FROM doc0377_total_key_source a "
        "JOIN doc0377_total_key_source b ON a.id=1 AND b.id=2;");
    for (const auto& value : layouts) {
        REQUIRE(value == "t");
    }
}

TEST_CASE("DOC0377 physical DNA4 letters support arrays grouping and indexes",
          "[letter][array][btree][hash][acceptance]") {
    auto ctx = fresh_context({
        "doc0377_letters_a",
        "doc0377_letters_b",
        "doc0377_letters_binary",
        "doc0377_letters_receive",
        "doc0377_letters_dna5",
        "doc0377_letters_dna5_b",
        "doc0377_letters_dna5_binary",
        "doc0377_letters_dna5_receive",
    });
    exec_sql(ctx,
        "SET search_path TO pg_catalog, bioseq, public; "
        "CREATE TABLE doc0377_letters_a "
        " (id int, value bioseq.letter('dna4')); "
        "PREPARE doc0377_insert_letter(text,int) AS "
        " INSERT INTO doc0377_letters_a "
        " VALUES ($2::int, $1::bioseq.letter('dna4')); "
        "EXECUTE doc0377_insert_letter('A',1); "
        "EXECUTE doc0377_insert_letter('C',2); "
        "EXECUTE doc0377_insert_letter('G',3); "
        "EXECUTE doc0377_insert_letter('T',4); "
        "DEALLOCATE doc0377_insert_letter; "
        "CREATE INDEX doc0377_letters_a_btree "
        " ON doc0377_letters_a USING btree(value bioseq.letter_btree_ops); "
        "CREATE INDEX doc0377_letters_a_hash "
        " ON doc0377_letters_a USING hash(value bioseq.letter_hash_ops); "
        "CREATE TABLE doc0377_letters_b AS "
        " SELECT * FROM doc0377_letters_a; "
        "CREATE TEMP VIEW doc0377_letters_view AS "
        " SELECT id,value FROM doc0377_letters_a;");

    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY value) "
        "FROM doc0377_letters_a;") == "ACGT");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(DISTINCT value) FROM doc0377_letters_a;") == 4);
    {
        guc_scope no_seqscan(ctx, "enable_seqscan", "off");
        REQUIRE(query_scalar_int(
            ctx,
            "SELECT id FROM doc0377_letters_a "
            "WHERE value = 'G'::bioseq.letter('dna4');") == 3);
    }
    exec_sql(ctx,
        "SET enable_hashjoin=on; "
        "SET enable_nestloop=off; "
        "SET enable_mergejoin=off;");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM doc0377_letters_a a "
        "JOIN doc0377_letters_b b USING (value);") == 4);
    REQUIRE(plan_contains(
        explain_text<false>(
            ctx,
            "SELECT * FROM doc0377_letters_a a "
            "JOIN doc0377_letters_b b USING (value);"),
        "Hash Join"));
    REQUIRE(query_scalar(
        ctx,
        "SELECT array_to_string(array_agg(value ORDER BY value),'') "
        "FROM doc0377_letters_a;") == "ACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM doc0377_letters_view;") == "ACGT");
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
        "FROM doc0377_letters_a WHERE id=1;") == "010000");
    const auto dna4_cast = query_one(
        ctx,
        "SELECT ('A'::bioseq.letter('dna4')"
        "          ::bioseq.letter('dna4'))::text, "
        "       ('A'::bioseq.letter('dna4')"
        "          ::bioseq.letter)::text;");
    REQUIRE(dna4_cast[0] == "A");
    REQUIRE(dna4_cast[1] == "A");

    exec_sql(ctx,
        "PREPARE doc0377_letter_param(bioseq.letter) AS "
        " SELECT $1::text;");
    for (const char* symbol : {"A", "C", "G", "T"}) {
        CAPTURE(symbol);
        REQUIRE(query_scalar(
            ctx,
            fmt::format(
                "EXECUTE doc0377_letter_param("
                " '{}'::bioseq.letter('dna4'));",
                symbol)) == symbol);
    }
    exec_sql(ctx, "DEALLOCATE doc0377_letter_param;");

    for (const char* invalid : {"", "AC", "N"}) {
        CAPTURE(invalid);
        const auto error = sql_error(
            ctx,
            fmt::format(
                "SELECT '{}'::bioseq.letter('dna4');", invalid));
        REQUIRE(error.sqlstate == "22P02");
    }

    const auto binary_path = fmt::format(
        "/tmp/bioseq_doc0377_letters_{}.bin",
        query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    (void) std::remove(binary_path.c_str());
    exec_sql(ctx,
        "CREATE TABLE doc0377_letters_binary "
        " (id int, value bioseq.letter('dna4')); ");
    exec_sql(ctx, fmt::format(
        "COPY doc0377_letters_a TO '{}' (FORMAT binary);",
        binary_path));
    exec_sql(ctx, fmt::format(
        "COPY doc0377_letters_binary FROM '{}' (FORMAT binary);",
        binary_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM doc0377_letters_binary;") == "ACGT");
    REQUIRE(std::remove(binary_path.c_str()) == 0);

    const auto backend_pid = query_scalar_int(
        ctx, "SELECT pg_backend_pid();");
    const auto valid_letter_path = fmt::format(
        "/tmp/bioseq_doc0377_letter_valid_{}.bin", backend_pid);
    const auto bad_version_path = fmt::format(
        "/tmp/bioseq_doc0377_letter_bad_version_{}.bin", backend_pid);
    const auto bad_rank_path = fmt::format(
        "/tmp/bioseq_doc0377_letter_bad_rank_{}.bin", backend_pid);
    for (const auto* path : {
             &valid_letter_path, &bad_version_path, &bad_rank_path}) {
        (void) std::remove(path->c_str());
    }
    exec_sql(ctx,
        "CREATE TABLE doc0377_letters_receive "
        " (value bioseq.letter('dna4')); ");
    exec_sql(ctx, fmt::format(
        "COPY (SELECT 'A'::bioseq.letter('dna4')) "
        "TO '{}' (FORMAT binary);",
        valid_letter_path));
    write_letter_binary_copy_variant(
        valid_letter_path, bad_version_path, 2u, 0u, 0u);
    write_letter_binary_copy_variant(
        valid_letter_path, bad_rank_path, 1u, 0u, 0xffu);

    const auto bad_version = sql_error(
        ctx,
        fmt::format(
            "COPY doc0377_letters_receive FROM '{}' (FORMAT binary);",
            bad_version_path));
    REQUIRE(bad_version.sqlstate == "22P03");
    REQUIRE(bad_version.contains("unsupported bioseq.letter wire version"));
    const auto bad_rank = sql_error(
        ctx,
        fmt::format(
            "COPY doc0377_letters_receive FROM '{}' (FORMAT binary);",
            bad_rank_path));
    REQUIRE(bad_rank.sqlstate == "22P03");
    REQUIRE(bad_rank.contains("invalid physical bioseq.letter wire value"));
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM doc0377_letters_receive;") == 0);

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
    exec_sql(ctx,
        "CREATE TABLE doc0377_letters_dna5 "
        " (id int, value bioseq.letter('dna5')); "
        "INSERT INTO doc0377_letters_dna5 VALUES "
        " (1,'A'::bioseq.letter('dna5')), "
        " (2,'C'::bioseq.letter('dna5')), "
        " (3,'G'::bioseq.letter('dna5')), "
        " (4,'T'::bioseq.letter('dna5')), "
        " (5,'N'::bioseq.letter('dna5')); "
        "CREATE INDEX doc0377_letters_dna5_btree "
        " ON doc0377_letters_dna5 USING btree(value); "
        "CREATE INDEX doc0377_letters_dna5_hash "
        " ON doc0377_letters_dna5 USING hash(value); "
        "CREATE TABLE doc0377_letters_dna5_b AS "
        " TABLE doc0377_letters_dna5; "
        "CREATE TABLE doc0377_letters_dna5_binary "
        " (id int, value bioseq.letter('dna5')); "
        "CREATE TABLE doc0377_letters_dna5_receive "
        " (value bioseq.letter('dna5')); "
        "CREATE TEMP VIEW doc0377_letters_dna5_view AS "
        " SELECT id,value FROM doc0377_letters_dna5;");
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY value) "
        "FROM doc0377_letters_dna5;") == "ACGTN");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(DISTINCT value) FROM doc0377_letters_dna5;") == 5);
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM ("
        " SELECT value FROM doc0377_letters_dna5 GROUP BY value"
        ") AS groups;") == 5);
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM doc0377_letters_dna5_view;") == "ACGTN");
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
        "PREPARE doc0377_letter_dna5_param(bioseq.letter) AS "
        " SELECT $1::text;");
    for (const char* symbol : {"A", "C", "G", "T", "N"}) {
        CAPTURE(symbol);
        REQUIRE(query_scalar(
            ctx,
            fmt::format(
                "EXECUTE doc0377_letter_dna5_param("
                " '{}'::bioseq.letter('dna5'));",
                symbol)) == symbol);
    }
    exec_sql(ctx, "DEALLOCATE doc0377_letter_dna5_param;");

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
            "SELECT id FROM doc0377_letters_dna5 "
            "WHERE value = 'N'::bioseq.letter('dna5');") == 5);
    }
    exec_sql(ctx,
        "SET enable_hashjoin=on; "
        "SET enable_nestloop=off; "
        "SET enable_mergejoin=off;");
    const auto dna5_hash_join =
        "SELECT count(*) FROM doc0377_letters_dna5 a "
        "JOIN doc0377_letters_dna5_b b USING (value);";
    REQUIRE(query_scalar_int(ctx, dna5_hash_join) == 5);
    REQUIRE(plan_contains(
        explain_text<false>(ctx, dna5_hash_join), "Hash Join"));

    const auto dna5_binary_path = fmt::format(
        "/tmp/bioseq_doc0377_letters_dna5_{}.bin", backend_pid);
    (void) std::remove(dna5_binary_path.c_str());
    exec_sql(ctx, fmt::format(
        "COPY doc0377_letters_dna5 TO '{}' (FORMAT binary);",
        dna5_binary_path));
    exec_sql(ctx, fmt::format(
        "COPY doc0377_letters_dna5_binary FROM '{}' (FORMAT binary);",
        dna5_binary_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM doc0377_letters_dna5_binary;") == "ACGTN");
    REQUIRE(std::remove(dna5_binary_path.c_str()) == 0);

    const auto typmod_mismatch = sql_error(
        ctx,
        fmt::format(
            "COPY doc0377_letters_dna5_receive "
            "FROM '{}' (FORMAT binary);",
            valid_letter_path));
    REQUIRE(typmod_mismatch.sqlstate == "42804");
    REQUIRE(typmod_mismatch.contains(
        "wire alphabet does not match destination typmod"));

    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.doc0377_letter_overload("
        " value bioseq.letter('dna4')) RETURNS int "
        "LANGUAGE sql IMMUTABLE AS 'SELECT 4';");
    REQUIRE(sql_error(
        ctx,
        "CREATE FUNCTION pg_temp.doc0377_letter_overload("
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
             &valid_letter_path, &bad_version_path, &bad_rank_path}) {
        REQUIRE(std::remove(path->c_str()) == 0);
    }
}

TEST_CASE("DOC0377 slice algebra is invariant across the required GUC matrix",
          "[seq][slice][guc][acceptance]") {
    auto ctx = fresh_context({"doc0377_slice_guc"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_slice_guc "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq; "
        "INSERT INTO doc0377_slice_guc VALUES "
        " (1,repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    constexpr std::array settings{"off", "on"};
    for (const char* vectorization : settings) {
        for (const char* column_cache : settings) {
            exec_sql(ctx, fmt::format(
                "SET bioseq.enable_vectorization={}; "
                "SET bioseq.enable_column_cache={};",
                vectorization, column_cache));
            CAPTURE(vectorization, column_cache);
            REQUIRE(query_scalar(
                ctx,
                "SELECT (s[4093:4101] OPERATOR(bioseq.||) "
                "        bioseq.reverse(s[8190:8194]))::text "
                "FROM doc0377_slice_guc WHERE id=1;") == "CGTACGTACATG");
            const auto plan = explain_text<false>(
                ctx,
                "SELECT s[4093:4101] FROM doc0377_slice_guc "
                "WHERE id=1;");
            REQUIRE(plan_contains(plan, "BioseqSliceProject"));
        }
    }
}

TEST_CASE("DOC0377 synthetic logical lengths stay lazy through bigint limits",
          "[seq][slice][length][overflow][acceptance]") {
    auto ctx = fresh_context({"doc0377_synthetic_length_heap"});

    const auto above_flat = query_one(
        ctx,
        "WITH v(s) AS MATERIALIZED ("
        " SELECT bioseq_internal.synthetic_lazy_sequence(4294967296)) "
        "SELECT bioseq.len(s), "
        "       bioseq.len(s[4294967294::bigint:4294967296::bigint]), "
        "       s[-1]::text, "
        "       bioseq.len(s OPERATOR(bioseq.||) "
        "          bioseq_internal.synthetic_lazy_sequence(0)) "
        "FROM v;");
    REQUIRE(above_flat[0] == "4294967296");
    REQUIRE(above_flat[1] == "2");
    REQUIRE(above_flat[2] == "A");
    REQUIRE(above_flat[3] == "4294967296");

    const auto bigint_max = query_one(
        ctx,
        "WITH v(s) AS MATERIALIZED ("
        " SELECT bioseq_internal.synthetic_lazy_sequence("
        "          9223372036854775807::bigint)) "
        "SELECT bioseq.len(s), s[-1]::text, "
        "       bioseq.len(s[9223372036854775805::bigint:"
        "                         9223372036854775807::bigint]), "
        "       bioseq.len(s OPERATOR(bioseq.||) "
        "          bioseq_internal.synthetic_lazy_sequence(0)) "
        "FROM v;");
    REQUIRE(bigint_max[0] == "9223372036854775807");
    REQUIRE(bigint_max[1] == "A");
    REQUIRE(bigint_max[2] == "2");
    REQUIRE(bigint_max[3] == "9223372036854775807");

    const auto concat_overflow = sql_error(
        ctx,
        "SELECT bioseq_internal.synthetic_lazy_sequence("
        "         9223372036854775807::bigint) "
        "       OPERATOR(bioseq.||) "
        "       bioseq_internal.synthetic_lazy_sequence(1);");
    REQUIRE(concat_overflow.sqlstate == "54000");

    const std::array<const char*, 3> materialization_boundaries{
        "SELECT bioseq.to_text("
        " bioseq_internal.synthetic_lazy_sequence(4294967296));",
        "SELECT bioseq.seq_send("
        " bioseq_internal.synthetic_lazy_sequence(4294967296));",
        "SELECT bioseq.to_array("
        " bioseq_internal.synthetic_lazy_sequence(4294967296));",
    };
    for (const char* sql : materialization_boundaries) {
        REQUIRE(sql_error(ctx, sql).sqlstate == "54000");
    }

    exec_sql(ctx,
        "CREATE TABLE doc0377_synthetic_length_heap "
        " (s bioseq.seq('dna4',8));");
    const auto heap_boundary = sql_error(
        ctx,
        "INSERT INTO doc0377_synthetic_length_heap "
        "VALUES (bioseq_internal.synthetic_lazy_sequence(4294967296));");
    REQUIRE(heap_boundary.sqlstate == "54000");
}

TEST_CASE("DOC0377 literal coalescing and arena guards are exact",
          "[seq][concat][rope][lifetime][acceptance]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar(
        ctx, "SELECT bioseq_internal.piece_tree_self_test();") == "t");

    const auto thresholds = query_one(
        ctx,
        "SELECT bioseq_internal.sequence_piece_count("
        "         repeat('A',2047)::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',2048)::bioseq.seq), "
        "       bioseq_internal.sequence_piece_count("
        "         repeat('A',2048)::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',2048)::bioseq.seq), "
        "       bioseq_internal.sequence_piece_count("
        "         repeat('A',2048)::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',2049)::bioseq.seq);");
    REQUIRE(thresholds[0] == "1");
    REQUIRE(thresholds[1] == "1");
    REQUIRE(thresholds[2] == "2");
}

TEST_CASE("DOC0377 persistent ropes survive PLpgSQL transfer heap flatten and ERROR unwind",
          "[seq][concat][rope][eoh][heap][error][acceptance]") {
    auto ctx = fresh_context({"doc0377_rope_heap"});
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.doc0377_build_rope(n int) "
        "RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE AS $$ "
        "DECLARE value bioseq.seq := ''::bioseq.seq; "
        "BEGIN "
        "  FOR i IN 1..n LOOP "
        "    value := value OPERATOR(bioseq.||) "
        "      substr('ACGT', ((i - 1) % 4) + 1, 1); "
        "  END LOOP; "
        "  RETURN value; "
        "END $$; "
        "CREATE FUNCTION pg_temp.doc0377_build_rope_error(n int) "
        "RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE AS $$ "
        "DECLARE value bioseq.seq; "
        "BEGIN "
        "  value := pg_temp.doc0377_build_rope(n); "
        "  RAISE EXCEPTION 'DOC0377 injected rope failure'; "
        "END $$;");

    const auto expanded = query_one(
        ctx,
        "SELECT bioseq.len(s), "
        "       bioseq_internal.sequence_piece_count(s), "
        "       s[4093:4101]::text, "
        "       bioseq.reverse(bioseq.reverse(s)) "
        "         OPERATOR(bioseq.=) s "
        "FROM LATERAL ("
        " SELECT pg_temp.doc0377_build_rope(8192) AS s OFFSET 0"
        ") AS built;");
    REQUIRE(expanded[0] == "8192");
    REQUIRE(std::stoll(expanded[1]) <= 16);
    REQUIRE(expanded[2] == "CGTACGTA");
    REQUIRE(expanded[3] == "t");

    exec_sql(ctx,
        "PREPARE doc0377_rope_plan(int) AS "
        " SELECT bioseq.len(pg_temp.doc0377_build_rope($1));");
    REQUIRE(query_scalar_int(
        ctx, "EXECUTE doc0377_rope_plan(4097);") == 4097);
    exec_sql(ctx, "DEALLOCATE doc0377_rope_plan;");

    const auto injected = sql_error(
        ctx, "SELECT pg_temp.doc0377_build_rope_error(4097);");
    REQUIRE(injected.sqlstate == "P0001");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT bioseq.len(pg_temp.doc0377_build_rope(4097));") == 4097);

    exec_sql(ctx,
        "CREATE TABLE doc0377_rope_heap (value bioseq.seq); "
        "INSERT INTO doc0377_rope_heap "
        "VALUES (pg_temp.doc0377_build_rope(8192));");
    const auto flattened = query_one(
        ctx,
        "SELECT bioseq.len(value), value[4093:4101]::text "
        "FROM doc0377_rope_heap;");
    REQUIRE(flattened[0] == "8192");
    REQUIRE(flattened[1] == "CGTACGTA");
}

#ifdef BIOSEQ_TEST_INDEXES

TEST_CASE("DOC0377 source-domain faults unwind every partial lifetime stage",
          "[seq][source][fault][lifetime][wrong-thread][acceptance]") {
    auto ctx = fresh_context({"doc0377_source_faults"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_faults "
        " (id int, value bioseq.seq('dna4',32)) USING bioseq; "
        "INSERT INTO doc0377_source_faults VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32));");

    const auto unknown = sql_error(
        ctx,
        "SELECT bioseq_internal.arm_sequence_source_failure("
        " 'not_a_failure_point');");
    REQUIRE(unknown.sqlstate == "22023");

    constexpr std::array failure_points{
        "provider_after_registry_control",
        "provider_after_provider_control",
        "domain_after_control",
        "domain_after_context",
        "domain_after_owner",
        "domain_after_snapshot",
        "domain_after_caches",
        "close_before_caches",
        "close_before_snapshot",
        "close_before_before_locks",
        "close_before_locks",
        "close_before_after_locks",
    };
    for (const char* failure_point : failure_points) {
        CAPTURE(failure_point);
        exec_sql(ctx,
            "SELECT bioseq_internal.reset_sequence_source_counters();");
        exec_sql(ctx, fmt::format(
            "SELECT bioseq_internal.arm_sequence_source_failure('{}');",
            failure_point));
        const auto injected = sql_error(
            ctx,
            "SELECT bioseq.to_text(value[4093:4101]) "
            "FROM doc0377_source_faults WHERE id=1;");
        REQUIRE(injected.sqlstate == "XX000");
        REQUIRE(injected.contains(
            "injected PostgreSQL sequence source-domain failure"));
        exec_sql(ctx,
            "SELECT bioseq_internal.disarm_sequence_source_failure();");
        const auto counters = query_one(
            ctx,
            "SELECT (c->>'fault_injections')::bigint, "
            "       (c->>'live_registries')::bigint "
            "       + (c->>'live_providers')::bigint "
            "       + (c->>'live_domains')::bigint "
            "       + (c->>'live_leases')::bigint "
            "       + (c->>'live_cache_sets')::bigint "
            "       + (c->>'live_snapshots')::bigint "
            "FROM (SELECT "
            " bioseq_internal.sequence_source_counters() AS c) AS q;");
        REQUIRE(counters[0] == "1");
        REQUIRE(counters[1] == "0");
    }

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal.sequence_source_wrong_thread_probe();")
        == "t");
    const auto wrong_thread = query_one(
        ctx,
        "SELECT (c->>'wrong_thread_rejections')::bigint, "
        "       (c->>'fault_injections')::bigint, "
        "       (c->>'live_registries')::bigint "
        "       + (c->>'live_providers')::bigint "
        "       + (c->>'live_domains')::bigint "
        "       + (c->>'live_cache_sets')::bigint "
        "       + (c->>'live_snapshots')::bigint "
        "FROM (SELECT "
        " bioseq_internal.sequence_source_counters() AS c) AS q;");
    REQUIRE(wrong_thread[0] == "1");
    REQUIRE(wrong_thread[1] == "0");
    REQUIRE(wrong_thread[2] == "0");

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal.sequence_source_fault_contract_probe();")
        == "t");
    const auto pinned_faults = query_one(
        ctx,
        "SELECT (c->>'refcount_faults')::bigint, "
        "       (c->>'lease_faults')::bigint "
        "FROM (SELECT "
        " bioseq_internal.sequence_source_counters() AS c) AS q;");
    REQUIRE(pinned_faults[0] == "1");
    REQUIRE(pinned_faults[1] == "1");

    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters(); "
        "DO $$ BEGIN "
        "  PERFORM bioseq_internal.sequence_source_pre_prepare_probe(); "
        "  RAISE EXCEPTION 'DOC0377 PRE_PREPARE probe did not reject'; "
        "EXCEPTION WHEN feature_not_supported THEN "
        "  IF SQLERRM NOT LIKE "
        "    'cannot PREPARE TRANSACTION with live bioseq source domains%' "
        "  THEN RAISE; END IF; "
        "END $$;");
    const auto pre_prepare = query_one(
        ctx,
        "SELECT (c->>'pre_prepare_rejections')::bigint, "
        "       (c->>'live_registries')::bigint "
        "       + (c->>'live_domains')::bigint "
        "       + (c->>'live_cache_sets')::bigint "
        "       + (c->>'live_snapshots')::bigint "
        "FROM (SELECT "
        " bioseq_internal.sequence_source_counters() AS c) AS q;");
    REQUIRE(pre_prepare[0] == "1");
    REQUIRE(pre_prepare[1] == "0");
}

TEST_CASE("DOC0377 source read ERROR crosses every sequence callback boundary",
          "[seq][source][fault][eoh][lifetime][acceptance]") {
    auto ctx = fresh_context({"doc0377_source_read_faults"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_source_read_faults "
        " (id int, value bioseq.seq('dna4',32)) USING bioseq; "
        "INSERT INTO doc0377_source_read_faults VALUES "
        " (1, repeat('ACGT',4096)::bioseq.seq('dna4',32)), "
        " (2, repeat('TGCA',4096)::bioseq.seq('dna4',32)); "
        "CREATE TABLE doc0377_source_read_heap "
        " (value bioseq.seq('dna4',32));");
	exec_sql(ctx,
		"SET bioseq.enable_sequence_sort_scan = on; "
		"SET bioseq.aligner.vulkan_enabled = off;");

    const std::array<const char*, 5> callback_queries{
        /* One canonical-view callback. */
        "SELECT bioseq.to_text(value[4093:4101]) "
        "FROM doc0377_source_read_faults WHERE id=1;",
        /* Two-view compare callback. */
        "SELECT a.value[4093:4101] OPERATOR(bioseq.=) "
        "       b.value[4093:4101] "
        "FROM doc0377_source_read_faults AS a "
        "CROSS JOIN doc0377_source_read_faults AS b "
        "WHERE a.id=1 AND b.id=2;",
        /* One-rank scalar callback. */
        "SELECT value[4097]::text "
        "FROM doc0377_source_read_faults WHERE id=1;",
        /* PostgreSQL heap datumCopy invokes the EOH flatten callback. */
        "INSERT INTO doc0377_source_read_heap "
        "SELECT value[4093:4101] "
        "FROM doc0377_source_read_faults WHERE id=1;",
		/* BatchSort canonical-view admission/comparison callback. */
		"SELECT id FROM doc0377_source_read_faults ORDER BY value;",
    };
    for (const char* query : callback_queries) {
        CAPTURE(query);
        exec_sql(ctx,
            "SELECT bioseq_internal.reset_sequence_source_counters(); "
            "SELECT bioseq_internal.arm_sequence_source_failure("
            " 'read_before_scanner');");
        const auto injected = sql_error(ctx, query);
        REQUIRE(injected.sqlstate == "XX000");
        REQUIRE(injected.contains(
            "injected PostgreSQL sequence source-domain failure"));
        exec_sql(ctx,
            "SELECT bioseq_internal.disarm_sequence_source_failure();");
        const auto counters = query_one(
            ctx,
            "SELECT (c->>'fault_injections')::bigint, "
            "       (c->>'read_fault_injections')::bigint, "
            "       (c->>'live_registries')::bigint "
            "       + (c->>'live_providers')::bigint "
            "       + (c->>'live_domains')::bigint "
            "       + (c->>'live_leases')::bigint "
            "       + (c->>'live_cache_sets')::bigint "
            "       + (c->>'live_snapshots')::bigint "
            "FROM (SELECT "
            " bioseq_internal.sequence_source_counters() AS c) AS q;");
        REQUIRE(counters[0] == "1");
        REQUIRE(counters[1] == "1");
        REQUIRE(counters[2] == "0");
    }
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM doc0377_source_read_heap;") == 0);
}

#endif

TEST_CASE("DOC0377 fresh install catalogs expose exact public objects",
          "[seq][catalog][acceptance]") {
    connection_test_context admin;
    const auto database = fmt::format(
        "bioseq_doc0377_fresh_install_{}",
        query_scalar_int(admin, "SELECT pg_backend_pid();"));
    exec_sql(admin, fmt::format(
        "DROP DATABASE IF EXISTS {} WITH (FORCE);", database));
    exec_sql(admin, fmt::format("CREATE DATABASE {};", database));

    row_t catalog;
    row_t runtime;
    {
        connection_test_context ctx(database);
        exec_sql(ctx, "CREATE EXTENSION bioseq;");
        catalog = query_one(
            ctx,
            "SELECT to_regtype('bioseq.seq') IS NOT NULL, "
            "       to_regtype('bioseq.letter') IS NOT NULL, "
            "       (SELECT typsubscript <> 0 FROM pg_catalog.pg_type "
            "        WHERE oid='bioseq.seq'::regtype), "
            "       (SELECT typlen=2 FROM pg_catalog.pg_type "
            "        WHERE oid='bioseq.letter'::regtype), "
            "       to_regprocedure('bioseq.len(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure('bioseq.reverse(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.reverse_complement(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure('bioseq.to_text(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure('bioseq.to_array(bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.from_array(bioseq.letter[],integer)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(bioseq.seq,bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(bioseq.seq,text)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(text,bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(bioseq.seq,character varying)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(character varying,bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(bioseq.seq,text[])') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.||(text[],bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator("
            "         'bioseq.||(bioseq.seq,bioseq.letter[])') "
            "         IS NOT NULL, "
            "       to_regoperator("
            "         'bioseq.||(bioseq.letter[],bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator('bioseq.=(bioseq.seq,bioseq.seq)') "
            "         IS NOT NULL, "
            "       to_regoperator("
            "         'bioseq.=(bioseq.letter,bioseq.letter)') "
            "         IS NOT NULL, "
            "       (SELECT count(*)=9 FROM pg_catalog.pg_operator o "
            "        JOIN pg_catalog.pg_namespace n ON n.oid=o.oprnamespace "
            "        WHERE n.nspname='bioseq' AND o.oprname='||'), "
            "       (SELECT count(*)=2 FROM pg_catalog.pg_cast c "
            "        WHERE c.castsource='bioseq.seq'::regtype "
            "          AND c.casttarget IN ('text'::regtype,'varchar'::regtype) "
            "          AND c.castcontext='a'), "
            "       (SELECT count(*)=1 FROM pg_catalog.pg_cast c "
            "        WHERE c.castsource='bioseq.seq'::regtype "
            "          AND c.casttarget='bioseq.seq'::regtype "
            "          AND c.castcontext='a'), "
            "       (SELECT count(*)=1 FROM pg_catalog.pg_cast c "
            "        WHERE c.castsource='bioseq.letter'::regtype "
            "          AND c.casttarget='bioseq.letter'::regtype "
            "          AND c.castcontext='a'), "
            "       (SELECT count(*)=1 FROM pg_catalog.pg_cast c "
            "        WHERE c.castsource='bioseq.letter'::regtype "
            "          AND c.casttarget='text'::regtype "
            "          AND c.castcontext='e'), "
            "       (SELECT count(*)=4 FROM pg_catalog.pg_opclass c "
            "        JOIN pg_catalog.pg_namespace n ON n.oid=c.opcnamespace "
            "        WHERE n.nspname='bioseq' AND c.opcname IN "
            "          ('seq_ops','seq_hash_ops',"
            "           'letter_btree_ops','letter_hash_ops')), "
            "       to_regprocedure("
            "         'bioseq.seq_cast(bioseq.seq,integer,boolean)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.letter(bioseq.letter,integer,boolean)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.letter_send(bioseq.letter)') IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.letter_cmp(bioseq.letter,bioseq.letter)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.letter_hash(bioseq.letter)') IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.seq_hash(bioseq.seq)') IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.array_length(bioseq.seq,integer)') "
            "         IS NOT NULL, "
            "       to_regprocedure("
            "         'bioseq.cardinality(bioseq.seq)') IS NOT NULL, "
            "       (SELECT bool_and(p.proisstrict) "
            "        FROM pg_catalog.pg_operator o "
            "        JOIN pg_catalog.pg_namespace n ON n.oid=o.oprnamespace "
            "        JOIN pg_catalog.pg_proc p ON p.oid=o.oprcode "
            "        WHERE n.nspname='bioseq' AND o.oprname='||'), "
            "       pg_typeof('AC'::bioseq.seq('dna4',8)) "
            "         = 'bioseq.seq'::regtype, "
            "       pg_typeof('A'::bioseq.letter('dna4')) "
            "         = 'bioseq.letter'::regtype;");

        exec_sql(ctx, "SET search_path TO pg_catalog, bioseq, public;");
        runtime = query_one(
            ctx,
            "SELECT pg_typeof('AC'::bioseq.seq('dna4',8) || 'GT') "
            "         = 'bioseq.seq'::regtype, "
            "       ('AC'::bioseq.seq('dna4',8) || "
            "         'GT'::bioseq.seq('dna4',64))::text, "
            "       ('AC'::bioseq.seq('dna4',8) || text 'GT')::text, "
            "       (text 'GT' || "
            "         'AC'::bioseq.seq('dna4',8))::text, "
            "       ('AC'::bioseq.seq('dna4',8) || varchar 'GT')::text, "
            "       (varchar 'GT' || "
            "         'AC'::bioseq.seq('dna4',8))::text, "
            "       ('AC'::bioseq.seq('dna4',8) || "
            "         ARRAY['G','T']::text[])::text, "
            "       (ARRAY['G','T']::text[] || "
            "         'AC'::bioseq.seq('dna4',8))::text, "
            "       ('AC'::bioseq.seq('dna4',8) || ARRAY["
            "         'G'::bioseq.letter('dna4'),"
            "         'T'::bioseq.letter('dna4')])::text, "
            "       (ARRAY['G'::bioseq.letter('dna4'),"
            "               'T'::bioseq.letter('dna4')] || "
            "         'AC'::bioseq.seq('dna4',8))::text, "
            "       ('ACGTA'::bioseq.seq('dna4',8))[1:4]::text, "
            "       bioseq.reverse_complement("
            "         'ACGT'::bioseq.seq('dna4',8))::text, "
            "       ('ACGT'::bioseq.seq('dna4',8))[2]::text;");
    }
    exec_sql(admin, fmt::format("DROP DATABASE {};", database));

    for (const auto& value : catalog) {
        REQUIRE(value == "t");
    }
    const std::array runtime_expected{
        "t", "ACGT", "ACGT", "GTAC", "ACGT", "GTAC",
        "ACGT", "GTAC", "ACGT", "GTAC", "CGT", "ACGT", "G"};
    REQUIRE(runtime.size() == runtime_expected.size());
    for (std::size_t index = 0; index < runtime_expected.size(); ++index) {
        REQUIRE(runtime[index] == runtime_expected[index]);
    }
}
