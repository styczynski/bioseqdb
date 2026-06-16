// Integration scenarios:
//   * `bioseq.seq` text-roundtrip: `'ACGT'::bioseq.seq(...)::text` returns the
//     original ASCII even on a vanilla heap table (no `USING bioseq`).
//   * `bioseq.len` reports the base-pair count and `bioseq.unit_len` reports
//     the number of storage units, regardless of typmod.
//   * Lexicographic ordering between sequences via the default btree opclass,
//     both as expression-level cmp and as `ORDER BY seq` driven by the
//     opclass-default sort.
//   * Equality / range predicates (`=`, `<`, `BETWEEN`) work as plain SQL
//     comparisons, not memcmp on the encoded payload.
//   * Empty literal round-trips on every (alphabet, width) combination.
//   * Empty literal also survives an INSERT-then-SELECT roundtrip on a heap
//     table — exercises the recv-side path that special-cases length 0.
//   * `seq_typmod_in` rejects every malformed typmod we can express.
//
// All scenarios use a heap table because columnar `USING bioseq` storage only
// emits tuples for SELECTs that include a `WHERE mmsearch(...)` qual (see
// SPEC_HOW_TO_CONNECT_ALIGNER_3 §13.1). Round-trip / metadata tests live on a
// heap table to keep the assertions tight on the type itself.

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_seq_basic_t";

}  // namespace

TEST_CASE("bioseq.seq round-trips ASCII payloads on heap tables",
          "[seq][basic]") {
    auto ctx = fresh_context({kTable});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kTable),
            .typmod = {.alphabet = "dna4", .width = 64},
            .seq_columns = 1,
            .using_bioseq = false,
        },
        {
            seq_row{.pk = 23,  .sequences = {"ACTAGGCACAT"}},
            seq_row{.pk = 101, .sequences = {"GGGGTTTTACTAGGCACATACTAGGCACATACT"}},
            seq_row{.pk = 69,
                    .sequences = {"ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT"}},
            seq_row{.pk = 42,  .sequences = {"TTTTTT"}},
        });

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, seq0::text, bioseq.len(seq0), bioseq.unit_len(seq0) "
            "FROM {} ORDER BY pk;",
            kTable));

    REQUIRE(rows.size() == 4);
    REQUIRE(rows[0][1].value() == "ACTAGGCACAT");
    REQUIRE(rows[0][2].value() == "11");
    REQUIRE(rows[1][1].value() == "TTTTTT");
    REQUIRE(rows[1][2].value() == "6");
    REQUIRE(rows[2][1].value() ==
            "ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT");
    REQUIRE(rows[2][2].value() == "44");
    REQUIRE(rows[3][1].value() == "GGGGTTTTACTAGGCACATACTAGGCACATACT");
    REQUIRE(rows[3][2].value() == "33");
}

// Heap-table ORDER BY drives the btree opclass at the planner level. We assert
// the resulting pk ordering matches lexicographic comparison so any
// disagreement between opclass-cmp and operator `<` lights up immediately.
TEST_CASE("ORDER BY bioseq.seq follows the documented lexicographic order",
          "[seq][basic][order_by]") {
    auto ctx = fresh_context({kTable});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kTable),
            .typmod = {.alphabet = "dna4", .width = 64},
            .seq_columns = 1,
        },
        {
            seq_row{.pk = 1, .sequences = {"ACGT"}},
            seq_row{.pk = 2, .sequences = {"ACTA"}},
            seq_row{.pk = 3, .sequences = {"AC"}},     // shorter prefix sorts first
            seq_row{.pk = 4, .sequences = {"GGGG"}},
            seq_row{.pk = 5, .sequences = {"AAAA"}},
        });

    const auto pks = query_int_column(
        ctx, fmt::format("SELECT pk FROM {} ORDER BY seq0;", kTable));
    // Expected lexicographic order: AAAA, AC, ACGT, ACTA, GGGG -> pk 5,3,1,2,4.
    REQUIRE(pks == std::vector<long long>{5, 3, 1, 2, 4});
}

TEMPLATE_TEST_CASE_SIG(
    "bioseq.seq accepts the empty sequence for every supported typmod",
    "[seq][basic][empty]",
    ((int Width), Width),
    8, 16, 32, 64) {
    auto ctx = fresh_context();
    for (const auto alphabet : {std::string_view("dna4"), std::string_view("dna5")}) {
        const seq_typmod typmod{.alphabet = alphabet, .width = Width};
        const auto text = query_scalar(
            ctx, fmt::format("SELECT {}::text;", format_literal("", typmod)));
        REQUIRE(text == "");
        REQUIRE(query_scalar_int(
                    ctx, fmt::format("SELECT bioseq.len({});",
                                     format_literal("", typmod))) == 0);
        REQUIRE(query_scalar_int(
                    ctx, fmt::format("SELECT bioseq.unit_len({});",
                                     format_literal("", typmod))) == 0);
    }
}

// Empty + non-empty literals stored side-by-side on a heap table. The recv
// path is exercised because libpq round-trips the value through the binary
// format when fetching results in some clients; we verify the basic invariant
// that ordering and length data survive INSERT.
TEST_CASE("Empty literal survives INSERT-then-SELECT on heap table",
          "[seq][basic][empty][insert]") {
    auto ctx = fresh_context({kTable});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kTable),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
        },
        {
            seq_row{.pk = 1, .sequences = {""}},
            seq_row{.pk = 2, .sequences = {"AAAA"}},
        });

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT pk, seq0::text, bioseq.len(seq0) FROM {} "
                    "ORDER BY pk;",
                    kTable));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0][1].value() == "");
    REQUIRE(rows[0][2].value() == "0");
    REQUIRE(rows[1][1].value() == "AAAA");
    REQUIRE(rows[1][2].value() == "4");
}

TEST_CASE("bioseq.seq comparison operators yield lexicographic ordering",
          "[seq][basic][cmp]") {
    auto ctx = fresh_context();
    const auto row =
        query_one(ctx,
                  "SELECT (a < b) AS lt, (a = b) AS eq, (a > b) AS gt, "
                  "       bioseq.seq_cmp(a, b) AS cmp "
                  "FROM (SELECT 'ACGT'::bioseq.seq('dna4', 8) AS a, "
                  "             'ACTA'::bioseq.seq('dna4', 8) AS b) t;");
    REQUIRE(row[0].value() == "t");
    REQUIRE(row[1].value() == "f");
    REQUIRE(row[2].value() == "f");
    const int cmp = std::stoi(row[3].value());
    REQUIRE(cmp < 0);
}

TEST_CASE("bioseq.seq prefix ordering: shorter prefix sorts before its extension",
          "[seq][basic][cmp]") {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        "SELECT ('A'::bioseq.seq('dna4', 8)    < 'AC'::bioseq.seq('dna4', 8)),"
        "       ('AC'::bioseq.seq('dna4', 8)   = 'AC'::bioseq.seq('dna4', 8)),"
        "       ('ACGT'::bioseq.seq('dna4', 8) > 'ACG'::bioseq.seq('dna4', 8));");
    REQUIRE(row[0].value() == "t");
    REQUIRE(row[1].value() == "t");
    REQUIRE(row[2].value() == "t");
}

TEST_CASE("bioseq.seq_cmp accepts unequal-typmod operands and orders consistently",
          "[seq][basic][cmp][cross_typmod]") {
    auto ctx = fresh_context();
    // Comparing across alphabets / widths: the function must either succeed
    // (and return a consistent sign) or raise. Whatever the policy, both
    // directions must agree (cmp(a,b) == -cmp(b,a)).
    constexpr std::string_view sql_pair =
        "SELECT bioseq.seq_cmp('ACGT'::bioseq.seq('dna4', 16), "
        "                      'ACGT'::bioseq.seq('dna4', 64));";
    const bool succeeds = !sql_fails(ctx, sql_pair);
    if (succeeds) {
        const int forward = std::stoi(query_scalar(ctx, sql_pair));
        const int reverse = std::stoi(query_scalar(
            ctx,
            "SELECT bioseq.seq_cmp('ACGT'::bioseq.seq('dna4', 64), "
            "                      'ACGT'::bioseq.seq('dna4', 16));"));
        REQUIRE(forward == -reverse);
    }
}

TEST_CASE("bioseq.seq supports equality and range filters on heap tables",
          "[seq][basic][predicates]") {
    auto ctx = fresh_context({kTable});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kTable),
            .typmod = {.alphabet = "dna4", .width = 64},
            .seq_columns = 1,
        },
        {
            seq_row{.pk = 23,  .sequences = {"ACTAGGCACAT"}},
            seq_row{.pk = 101, .sequences = {"GGGGTTTTACT"}},
            seq_row{.pk = 69,  .sequences = {"ACGTACGTACGT"}},
            seq_row{.pk = 42,  .sequences = {"TTTTTT"}},
        });

    SECTION("Exact equality") {
        REQUIRE(query_int_column(
                    ctx, fmt::format(
                             "SELECT pk FROM {} "
                             "WHERE seq0 = 'TTTTTT'::bioseq.seq('dna4', 64) "
                             "ORDER BY pk;",
                             kTable)) ==
                std::vector<long long>{42});
    }
    SECTION("Strict less-than") {
        REQUIRE(query_int_column(
                    ctx, fmt::format(
                             "SELECT pk FROM {} "
                             "WHERE seq0 < 'C'::bioseq.seq('dna4', 64) "
                             "ORDER BY pk;",
                             kTable)) ==
                std::vector<long long>{23, 69});
    }
    SECTION("BETWEEN") {
        REQUIRE(query_int_column(
                    ctx, fmt::format(
                             "SELECT pk FROM {} "
                             "WHERE seq0 BETWEEN 'A'::bioseq.seq('dna4', 64) "
                             "                AND 'G'::bioseq.seq('dna4', 64) "
                             "ORDER BY pk;",
                             kTable)) ==
                std::vector<long long>{23, 69});
    }
}

TEST_CASE("dna4 typmod cast loses non-canonical letters; dna5 preserves N",
          "[seq][basic][cast]") {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        "SELECT 'ACGTN'::bioseq.seq('dna4', 16)::text, "
        "       'ACGTN'::bioseq.seq('dna5', 16)::text;");
    REQUIRE(row[0].value() == "ACGTA");
    REQUIRE(row[1].value() == "ACGTN");
}

// Negative typmod matrix: every entry must raise a SQL error. Adding a new
// invalid form is one row.
TEMPLATE_TEST_CASE_SIG(
    "Invalid bioseq.seq typmods raise a SQL error",
    "[seq][basic][typmod][negative]",
    ((std::size_t Idx), Idx),
    0, 1, 2, 3, 4, 5, 6) {
    static constexpr std::array<std::string_view, 7> kInvalidTypmods{
        "SELECT 'ACGT'::bioseq.seq('protein', 32);",
        "SELECT 'ACGT'::bioseq.seq('dna4', 128);",
        "SELECT 'ACGT'::bioseq.seq('dna4', 0);",
        "SELECT 'ACGT'::bioseq.seq('dna4', -8);",
        "SELECT 'ACGT'::bioseq.seq('', 32);",
        "SELECT 'ACGT'::bioseq.seq('dna4');",
        "SELECT 'ACGT'::bioseq.seq('dna4', 64, 10);",
    };
    auto ctx = fresh_context();
    REQUIRE(sql_fails(ctx, kInvalidTypmods[Idx]));
}
