// DOC0377 §§17.1-17.2: normative SQL acceptance matrix.

#include "common_tests_integration.h"
#include "headers/pg_build_config.h"

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

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

class database_cleanup_guard {
public:
    database_cleanup_guard(PGconn* connection, const char* drop_sql) noexcept
        : connection_(connection), drop_sql_(drop_sql) {}

    database_cleanup_guard(const database_cleanup_guard&) = delete;
    database_cleanup_guard& operator=(const database_cleanup_guard&) = delete;

    ~database_cleanup_guard() noexcept {
        if (!active_ || connection_ == nullptr || drop_sql_ == nullptr) {
            return;
        }
        if (PGresult* result = PQexec(connection_, drop_sql_)) {
            PQclear(result);
        }
    }

    void release() noexcept { active_ = false; }

private:
    PGconn* connection_ = nullptr;
    const char* drop_sql_ = nullptr;
    bool active_ = true;
};

struct prepared_letter_result {
    ExecStatusType status = PGRES_FATAL_ERROR;
    std::string sqlstate;
    std::optional<std::string> value;
};

[[nodiscard]] prepared_letter_result execute_prepared_letter_parameter(
    PGconn* connection,
    const char* name,
    const char* sql,
    Oid letter_oid,
    const char* value,
    int length,
    int format)
{
    PGresult* prepared = PQprepare(
        connection, name, sql, 1, &letter_oid);
    REQUIRE(prepared != nullptr);
    REQUIRE(PQresultStatus(prepared) == PGRES_COMMAND_OK);
    PQclear(prepared);

    const char* values[]{value};
    const int lengths[]{length};
    const int formats[]{format};
    PGresult* executed = PQexecPrepared(
        connection, name, 1, values, lengths, formats,
        /* text results = */ 0);
    REQUIRE(executed != nullptr);
    prepared_letter_result result{
        .status = PQresultStatus(executed),
    };
    if (const char* state = PQresultErrorField(
            executed, PG_DIAG_SQLSTATE)) {
        result.sqlstate = state;
    }
    if (result.status == PGRES_TUPLES_OK
        && PQntuples(executed) == 1 && PQnfields(executed) >= 1
        && !PQgetisnull(executed, 0, 0)) {
        result.value = std::string(PQgetvalue(executed, 0, 0));
    }
    PQclear(executed);
    return result;
}

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

std::string doc0377_rank_pattern(
    const std::string& alphabet_symbols,
    std::size_t length)
{
    REQUIRE_FALSE(alphabet_symbols.empty());
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        result.push_back(alphabet_symbols[index % alphabet_symbols.size()]);
    }
    return result;
}

void require_doc0377_sql_packing_profile(
    connection_test_context& ctx,
    const char* alphabet,
    std::uint32_t storage_width_bits,
    std::uint32_t bits_per_symbol,
    const char* alphabet_symbols)
{
    REQUIRE(alphabet != nullptr);
    REQUIRE(alphabet_symbols != nullptr);
    REQUIRE(bits_per_symbol > 0u);
    REQUIRE(storage_width_bits % 8u == 0u);

    const auto letters_per_unit = storage_width_bits / bits_per_symbol;
    REQUIRE(letters_per_unit > 0u);

    std::string cases;
    std::size_t case_id = 0;
    for (std::size_t length = 0;
         length <= static_cast<std::size_t>(letters_per_unit) + 1u;
         ++length) {
        const auto whole = doc0377_rank_pattern(alphabet_symbols, length);
        for (std::size_t split = 0; split <= length; ++split) {
            if (!cases.empty()) {
                cases.push_back(',');
            }
            cases += fmt::format("({},'{}',{})", case_id, whole, split);
            ++case_id;
        }
    }

    const auto expected_case_count =
        (static_cast<std::size_t>(letters_per_unit) + 2u)
        * (static_cast<std::size_t>(letters_per_unit) + 3u) / 2u;
    REQUIRE(case_id == expected_case_count);
    CAPTURE(
        alphabet,
        storage_width_bits,
        bits_per_symbol,
        letters_per_unit,
        expected_case_count);

    const auto cte = fmt::format(
        "WITH cases(case_id,whole,split_at) AS (VALUES {}), "
        "eager_values AS MATERIALIZED ("
        " SELECT case_id,whole,split_at,"
        "        whole::bioseq.seq('{}',{}) AS eager_value "
        " FROM cases), "
        "lazy_values AS ("
        " SELECT case_id,whole,eager_value,"
        "        eager_value[:split_at] OPERATOR(bioseq.||) "
        "        eager_value[split_at:] AS lazy_value "
        " FROM eager_values) ",
        cases,
        alphabet,
        storage_width_bits);

    const auto direct = query_one(
        ctx,
        cte
            + "SELECT count(*)::text, "
              "       bool_and(bioseq.to_text(lazy_value)=whole), "
              "       bool_and(bioseq.seq_send(lazy_value)="
              "                bioseq.seq_send(eager_value)), "
              "       bool_and(bioseq.len(lazy_value)="
              "                char_length(whole)) "
              "FROM lazy_values;");
    REQUIRE(direct.size() == 4u);
    REQUIRE(direct[0] == std::to_string(expected_case_count));
    REQUIRE(direct[1] == "t");
    REQUIRE(direct[2] == "t");
    REQUIRE(direct[3] == "t");

    exec_sql(ctx, "TRUNCATE doc0377_packing_flatten;");
    exec_sql(
        ctx,
        cte
            + "INSERT INTO doc0377_packing_flatten "
              " (case_id,expected_text,expected_binary,"
              "  expected_length,value) "
              "SELECT case_id,whole,bioseq.seq_send(eager_value),"
              "       char_length(whole),lazy_value "
              "FROM lazy_values;");

    const auto flattened = query_one(
        ctx,
        "SELECT count(*)::text, "
        "       bool_and(bioseq.to_text(value)=expected_text), "
        "       bool_and(bioseq.seq_send(value)=expected_binary), "
        "       bool_and(bioseq.len(value)=expected_length) "
        "FROM doc0377_packing_flatten;");
    REQUIRE(flattened.size() == 4u);
    REQUIRE(flattened[0] == std::to_string(expected_case_count));
    REQUIRE(flattened[1] == "t");
    REQUIRE(flattened[2] == "t");
    REQUIRE(flattened[3] == "t");
}

[[nodiscard]] std::string doc0377_match_binding_motif() {
    constexpr std::string_view alphabet = "ACGT";
    std::string sequence(128u, 'A');
    std::uint64_t state = 0x243f6a8885a308d3ull;
    for (char& base : sequence) {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        base = alphabet[static_cast<std::size_t>(state & 3u)];
    }
    return sequence;
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

TEST_CASE("DOC0377 sliced mmsearch helpers bind one stable owner",
          "[seq][slice][mmsearch][binding][nested][acceptance]") {
    constexpr std::string_view target_table =
        "doc0377_match_binding_target";
    constexpr std::string_view secondary_table =
        "doc0377_match_binding_secondary";
    auto ctx = fresh_context({target_table, secondary_table});
    const std::string motif = doc0377_match_binding_motif();
    const std::string short_motif = motif.substr(0u, 96u);
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{target_table},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{.pk = 1, .sequences = {motif + short_motif}}});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{secondary_table},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{.pk = 1, .sequences = {short_motif}}});
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE doc0377_match_binding_queries ("
        " id integer PRIMARY KEY, seq bioseq.seq('dna4',32)); "
        "INSERT INTO doc0377_match_binding_queries VALUES "
        " (1, '{}'::bioseq.seq('dna4',32)),"
        " (2, '{}'::bioseq.seq('dna4',32));",
        motif, short_motif));
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather = 0; "
        "SET bioseq.read_sequence_index = off; "
        "SET bioseq.aligner.vulkan_enabled = off; "
        "SET bioseq.aligner.min_chain_score = 10; "
        "SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash'; "
        "SET bioseq.aligner.chaining_algorithm = 'simd_mm2'; "
        "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner'; "
        "SET join_collapse_limit = 1; "
        "SET from_collapse_limit = 1; "
        "SET geqo = off;");

    /* Core does not expose stable SQL knobs for all five wrapper Path types.
     * This revoked seam runs the real production owner proof/COW propagation
     * over their exact planner nodes. The nested SQL below then supplies the
     * genuine finalized-plan/executor proof: helper values come from each
     * MatchResult and never from an executable projection stub. */
    const auto wrapper_proof = query_one(
        ctx,
        "SELECT exact_owner_found::text, clone_applied::text, "
        "owner_cloned::text, projection_cloned::text, "
        "material_cloned::text, memoize_cloned::text, "
        "unique_cloned::text, append_cloned::text, "
        "helper_on_owner::text, helper_on_projection::text, "
        "helper_on_material::text, helper_on_memoize::text, "
        "helper_on_unique::text, helper_on_append::text, "
        "originals_unchanged::text, sibling_unchanged::text, "
        "duplicate_owner_rejected::text, "
        "unknown_wrapper_rejected::text, "
        "group_result_qual_rejected::text "
        "FROM bioseq_internal."
        "match_helper_wrapper_propagation_probe();");
    REQUIRE(wrapper_proof.size() == 19u);
    for (const auto& field : wrapper_proof) {
        REQUIRE(field.has_value());
        REQUIRE(*field == "true");
    }
	const auto executor_translation_proof = query_one(
		ctx,
		"SELECT target_offset_accepted::text, "
		"target_attno_disagreement_rejected::text, "
		"exact_site_offset_accepted::text, "
		"lineage_alias_disagreement_rejected::text, "
		"outer_slot_accepted::text, "
		"outer_wrong_slot_rejected::text, "
		"inner_slot_accepted::text, "
		"inner_wrong_slot_rejected::text, "
		"index_slot_accepted::text, "
		"index_wrong_slot_rejected::text, "
		"special_out_of_range_rejected::text "
		"FROM bioseq_internal."
		"base_match_executor_translation_probe();");
	REQUIRE(executor_translation_proof.size() == 11u);
	for (const auto& field : executor_translation_proof) {
		REQUIRE(field.has_value());
		REQUIRE(*field == "true");
	}

    /* A base BioseqScan is itself a stable match owner.  Its helpers must be
     * finalized against the exact sequence column before executor rewriting. */
    const auto base_owner_rows = query_rows(ctx, fmt::format(
        "SELECT bioseq.query_len(t.seq0), bioseq.target_len(t.seq0), "
        "       bioseq.cigar(t.seq0) "
        "FROM {} AS t "
        "WHERE mmsearch(t.seq0, '{}'::bioseq.seq('dna4',32))",
        target_table, motif));
    REQUIRE(base_owner_rows.size() == 1u);
    REQUIRE(base_owner_rows.front()[0]
        == std::optional<std::string>{"128"});
    REQUIRE(base_owner_rows.front()[1]
        == std::optional<std::string>{"224"});
    REQUIRE(base_owner_rows.front()[2].has_value());
    REQUIRE(*base_owner_rows.front()[2] != "!UNEXPECTED!");

	/* The second materialized CTE is planned after an unrelated query level,
	 * so its BioseqScan Vars receive a nonzero final-rtable offset.  The
	 * planner-scope owner image must remain local for ambiguity resolution,
	 * while the executor image uses the shifted target and helper Vars. */
	const std::string offset_base_owner_sql = fmt::format(
		"WITH pad AS MATERIALIZED ("
		"  SELECT count(*) AS n FROM {}"
		"), owned AS MATERIALIZED ("
		"  SELECT bioseq.query_len(t.seq0) AS query_len, "
		"         bioseq.target_len(t.seq0) AS target_len, "
		"         bioseq.cigar(t.seq0) AS exact_cigar, "
		"         bioseq.query_start(t.pk) AS query_start, "
		"         bioseq.query_end(t.pk) AS query_end, "
		"         bioseq.cigar(t.pk) AS lineage_cigar "
		"  FROM {} AS t "
		"  WHERE mmsearch(t.seq0, '{}'::bioseq.seq('dna4',32))"
		") "
		"SELECT owned.query_len, owned.target_len, owned.exact_cigar, "
		"       owned.query_start, owned.query_end, owned.lineage_cigar "
		"FROM pad CROSS JOIN owned WHERE pad.n >= 0",
		secondary_table, target_table, motif);
	const auto offset_base_owner_plan = explain_text<false>(
		ctx, offset_base_owner_sql);
	REQUIRE(std::any_of(
		offset_base_owner_plan.begin(), offset_base_owner_plan.end(),
		[](const std::string& line) {
			return line.find("CTE Scan on owned") != std::string::npos;
		}));
	REQUIRE(std::any_of(
		offset_base_owner_plan.begin(), offset_base_owner_plan.end(),
		[](const std::string& line) {
			return line.find("Custom Scan (BioseqScan)")
				!= std::string::npos;
		}));
	const auto offset_base_owner_rows = query_rows(
		ctx, offset_base_owner_sql);
	REQUIRE(offset_base_owner_rows.size() == 1u);
	REQUIRE(offset_base_owner_rows.front().size() == 6u);
	REQUIRE(offset_base_owner_rows.front()[0] == base_owner_rows.front()[0]);
	REQUIRE(offset_base_owner_rows.front()[1] == base_owner_rows.front()[1]);
	REQUIRE(offset_base_owner_rows.front()[2] == base_owner_rows.front()[2]);
	REQUIRE(offset_base_owner_rows.front()[3]
		== std::optional<std::string>{"0"});
	REQUIRE(offset_base_owner_rows.front()[4]
		== std::optional<std::string>{"128"});
	REQUIRE(offset_base_owner_rows.front()[5] == base_owner_rows.front()[2]);
	exec_sql(ctx,
		std::string{"PREPARE doc0377_base_match_owner_offset AS "}
			+ offset_base_owner_sql);
	REQUIRE(query_rows(ctx, "EXECUTE doc0377_base_match_owner_offset;")
		== offset_base_owner_rows);
	REQUIRE(query_rows(ctx, "EXECUTE doc0377_base_match_owner_offset;")
		== offset_base_owner_rows);
	exec_sql(ctx, "DEALLOCATE doc0377_base_match_owner_offset;");

    /* The base owner and an outer sliced owner share t.seq0 lineage.  A row
     * marker cannot select between their MatchResults and must fail closed. */
    const auto finalization_scope_reset = query_one(ctx,
        "SELECT scopes_entered::text, scopes_exited::text, "
        "       scopes_active::text "
        "FROM bioseq_internal."
        "match_owner_finalization_scope_diagnostics(true);");
    REQUIRE(finalization_scope_reset.size() == 3u);
    REQUIRE(finalization_scope_reset[0]
        == std::optional<std::string>{"0"});
    REQUIRE(finalization_scope_reset[1]
        == std::optional<std::string>{"0"});
    REQUIRE(finalization_scope_reset[2]
        == std::optional<std::string>{"0"});
    const auto base_outer_ambiguous = sql_error(ctx, fmt::format(
        "SELECT bioseq.cigar(t.pk) "
        "FROM {} AS t "
        "INNER JOIN doc0377_match_binding_queries AS q "
        "ON q.id=2 AND mmsearch(t.seq0[128:224],q.seq) "
        "WHERE mmsearch(t.seq0, '{}'::bioseq.seq('dna4',32))",
        target_table, motif));
    REQUIRE(base_outer_ambiguous.sqlstate == "42702");
    REQUIRE(base_outer_ambiguous.contains(
        "bioseq match-result helper binding is ambiguous"));
    REQUIRE(base_outer_ambiguous.contains("candidate_match_owners=2"));
    const auto finalization_scope_after_error = query_one(ctx,
        "SELECT scopes_entered::text, scopes_exited::text, "
        "       scopes_active::text "
        "FROM bioseq_internal."
        "match_owner_finalization_scope_diagnostics(false);");
    REQUIRE(finalization_scope_after_error.size() == 3u);
    REQUIRE(finalization_scope_after_error[0].has_value());
    REQUIRE(finalization_scope_after_error[1].has_value());
    REQUIRE(finalization_scope_after_error[0]
        == finalization_scope_after_error[1]);
    REQUIRE(finalization_scope_after_error[2]
        == std::optional<std::string>{"0"});

    /* Every result helper names the complete transformed target recipe.  In
     * particular, the coordinate helpers must describe the 128-letter logical
     * slice rather than the 224-letter stored source. */
    const std::string exact_helpers_sql = fmt::format(
        "SELECT bioseq.query_len(t.seq0[0:128]), "
        "       bioseq.query_start(t.seq0[0:128]), "
        "       bioseq.query_end(t.seq0[0:128]), "
        "       bioseq.target_len(t.seq0[0:128]), "
        "       bioseq.target_start(t.seq0[0:128]), "
        "       bioseq.target_end(t.seq0[0:128]), "
        "       bioseq.num_matches(t.seq0[0:128]), "
        "       bioseq.block_length(t.seq0[0:128]), "
        "       bioseq.edit(t.seq0[0:128]), "
        "       bioseq.cigar(t.seq0[0:128]), "
        "       bioseq.has_cigar(t.seq0[0:128]) "
        "FROM {} AS t "
        "INNER JOIN doc0377_match_binding_queries AS q "
        "ON q.id=1 AND mmsearch(t.seq0[0:128],q.seq) "
        "ORDER BY t.pk",
        target_table);
    const std::string ordered_suffix = " ORDER BY t.pk";
    REQUIRE(exact_helpers_sql.ends_with(ordered_suffix));
    const std::string exact_helpers_unordered_sql = exact_helpers_sql.substr(
        0u, exact_helpers_sql.size() - ordered_suffix.size());
    const auto exact_helpers_unordered_plan = explain_text<false>(
        ctx, exact_helpers_unordered_sql);
    REQUIRE(std::count_if(
        exact_helpers_unordered_plan.begin(),
        exact_helpers_unordered_plan.end(),
        [](const std::string& line) {
            return line.find("Custom Scan (BioseqBatchJoin)")
                != std::string::npos;
        }) == 1);
    /* EXPLAIN itself reaches core setrefs.  Keeping pk out of the visible
     * SELECT list makes this a direct resjunk-sort carrier regression for the
     * transformed owner, before any executor behavior can mask the defect. */
    const auto exact_helpers_plan = explain_text<false>(
        ctx, exact_helpers_sql);
    REQUIRE(std::count_if(
        exact_helpers_plan.begin(), exact_helpers_plan.end(),
        [](const std::string& line) {
            return line.find("Custom Scan (BioseqBatchJoin)")
                != std::string::npos;
        }) == 1);
    const auto exact_rows = expect_query_invariant_under_options(
        ctx, exact_helpers_sql,
        {
            {"bioseq.enable_vectorization", {"on", "off"}},
            {"bioseq.enable_column_cache", {"on", "off"}},
        });
    REQUIRE(exact_rows.size() == 1u);
    REQUIRE(exact_rows.front().size() == 11u);
    for (const auto& value : exact_rows.front()) {
        REQUIRE(value.has_value());
    }
    REQUIRE(exact_rows.front()[0] == std::optional<std::string>{"128"});
    REQUIRE(exact_rows.front()[1] == std::optional<std::string>{"0"});
    REQUIRE(exact_rows.front()[2] == std::optional<std::string>{"128"});
    REQUIRE(exact_rows.front()[3] == std::optional<std::string>{"128"});
    REQUIRE(exact_rows.front()[4] == std::optional<std::string>{"0"});
    REQUIRE(exact_rows.front()[5] == std::optional<std::string>{"128"});
    REQUIRE(std::stoll(*exact_rows.front()[6]) > 0);
    REQUIRE(std::stoll(*exact_rows.front()[7]) >= 128);
    REQUIRE(exact_rows.front()[8] == std::optional<std::string>{"0"});
    REQUIRE(*exact_rows.front()[9] != "!UNEXPECTED!");
    REQUIRE(exact_rows.front()[9]->find('M') != std::string::npos);
    REQUIRE(exact_rows.front()[10] == std::optional<std::string>{"t"});

    /* The historical primary-key spelling remains the unique-lineage
     * fallback when there is exactly one owner in the statement. */
    const std::string lineage_fallback_sql = fmt::format(
        "SELECT bioseq.query_start(t.pk), bioseq.query_end(t.pk), "
        "       bioseq.target_start(t.pk), bioseq.target_end(t.pk), "
        "       bioseq.edit(t.pk), bioseq.cigar(t.pk), "
        "       bioseq.has_cigar(t.pk) "
        "FROM {} AS t "
        "INNER JOIN doc0377_match_binding_queries AS q "
        "ON q.id=1 AND mmsearch(t.seq0[0:128],q.seq) "
        "ORDER BY t.pk",
        target_table);
    const auto fallback_rows = query_rows(ctx, lineage_fallback_sql);
    REQUIRE(fallback_rows.size() == 1u);
    REQUIRE(fallback_rows.front().size() == 7u);
    REQUIRE(fallback_rows.front()[0] == std::optional<std::string>{"0"});
    REQUIRE(fallback_rows.front()[1] == std::optional<std::string>{"128"});
    REQUIRE(fallback_rows.front()[2] == std::optional<std::string>{"0"});
    REQUIRE(fallback_rows.front()[3] == std::optional<std::string>{"128"});
    REQUIRE(fallback_rows.front()[4] == std::optional<std::string>{"0"});
    REQUIRE(fallback_rows.front()[5] == exact_rows.front()[9]);
    REQUIRE(fallback_rows.front()[6] == std::optional<std::string>{"t"});

    /* A sequence-valued edit argument is never a legacy lineage marker: for
     * transformed targets it must equal the complete target recipe. */
    const auto mismatched_edit = sql_error(ctx, fmt::format(
        "SELECT bioseq.edit(t.seq0) FROM {} AS t "
        "INNER JOIN doc0377_match_binding_queries AS q "
        "ON q.id=1 AND mmsearch(t.seq0[0:128],q.seq)",
        target_table));
    REQUIRE(mismatched_edit.sqlstate == "0A000");
    REQUIRE(mismatched_edit.contains(
        "bioseq.edit projection must reference the mmsearch target"));

    const auto raw_exact = query_rows(ctx, fmt::format(
        "SELECT bioseq.query_len(t.seq0), bioseq.target_len(t.seq0), "
        "       bioseq.cigar(t.seq0) "
        "FROM {} AS t "
        "INNER JOIN doc0377_match_binding_queries AS q "
        "ON q.id=1 AND mmsearch(t.seq0,q.seq)",
        target_table));
    REQUIRE(raw_exact.size() == 1u);
    REQUIRE(raw_exact.front()[0] == std::optional<std::string>{"128"});
    REQUIRE(raw_exact.front()[1] == std::optional<std::string>{"224"});
    REQUIRE(raw_exact.front()[2].has_value());
    REQUIRE(*raw_exact.front()[2] != "!UNEXPECTED!");
    const auto tail_exact = query_rows(ctx, fmt::format(
        "SELECT bioseq.query_len(t.seq0[128:224]), "
        "       bioseq.target_len(t.seq0[128:224]), "
        "       bioseq.cigar(t.seq0[128:224]) "
        "FROM {} AS t "
        "INNER JOIN doc0377_match_binding_queries AS q "
        "ON q.id=2 AND mmsearch(t.seq0[128:224],q.seq)",
        target_table));
    REQUIRE(tail_exact.size() == 1u);
    REQUIRE(tail_exact.front()[0] == std::optional<std::string>{"96"});
    REQUIRE(tail_exact.front()[1] == std::optional<std::string>{"96"});
    REQUIRE(tail_exact.front()[2].has_value());
    REQUIRE(*tail_exact.front()[2] != "!UNEXPECTED!");
    REQUIRE(raw_exact.front()[2] != tail_exact.front()[2]);

    /* A parenthesized, collapse-disabled join tree creates two owner nodes in
     * one PlannerInfo and therefore one stable-binding scope.  The inner raw
     * owner aligns the 224-letter whole source; the outer transformed owner
     * aligns its 96-letter tail.  Keeping only the outer owner transformed
     * also proves nested ownership without stacking two SliceProject source
     * tags on the same BioseqScan. */
    const std::string nested_owner_sql = fmt::format(
        "SELECT t.pk, q1.id, q2.id, "
        "       bioseq.query_len(t.seq0), "
        "       bioseq.target_len(t.seq0), bioseq.cigar(t.seq0), "
        "       bioseq.query_len(t.seq0[128:224]), "
        "       bioseq.target_len(t.seq0[128:224]), "
        "       bioseq.cigar(t.seq0[128:224]) "
        "FROM ({} AS t "
        " INNER JOIN doc0377_match_binding_queries AS q1 "
        " ON q1.id=1 AND mmsearch(t.seq0,q1.seq)) "
        "INNER JOIN doc0377_match_binding_queries AS q2 "
        " ON q2.id=2 AND mmsearch(t.seq0[128:224],q2.seq) "
        "ORDER BY t.pk",
        target_table);
    const auto exact_plan = explain_text<false>(ctx, nested_owner_sql);
    const auto owner_count = std::count_if(
        exact_plan.begin(), exact_plan.end(),
        [](const std::string& line) {
            return line.find("Custom Scan (BioseqBatchJoin)")
                != std::string::npos;
        });
    REQUIRE(owner_count == 2);
    const auto nested_rows = query_rows(ctx, nested_owner_sql);
    REQUIRE(nested_rows.size() == 1u);
    REQUIRE(nested_rows.front() == std::vector<std::optional<std::string>>{
        std::optional<std::string>{"1"},
        std::optional<std::string>{"1"},
        std::optional<std::string>{"2"},
        raw_exact.front()[0], raw_exact.front()[1], raw_exact.front()[2],
        tail_exact.front()[0], tail_exact.front()[1], tail_exact.front()[2]});
    exec_sql(ctx,
        std::string{"PREPARE doc0377_exact_match_owners AS "}
            + nested_owner_sql);
    const auto prepared_first = query_rows(
        ctx, "EXECUTE doc0377_exact_match_owners;");
    const auto prepared_second = query_rows(
        ctx, "EXECUTE doc0377_exact_match_owners;");
    REQUIRE(prepared_first == nested_rows);
    REQUIRE(prepared_second == nested_rows);
    exec_sql(ctx, "DEALLOCATE doc0377_exact_match_owners;");

    /* Force a safe SortPath on the route from the raw descendant owner to the
     * transformed owner.  The exact inner helper identities must survive each
     * wrapper targetlist edge and remain MatchResult-backed after setrefs. */
    exec_sql(ctx,
        "SET enable_nestloop = off; "
        "SET enable_hashjoin = off; "
        "SET enable_mergejoin = on;");
    const std::string wrapped_owner_sql = fmt::format(
        "SELECT t.pk, q1.id, q2.id, "
        "       bioseq.query_len(t.seq0), "
        "       bioseq.target_len(t.seq0), bioseq.cigar(t.seq0), "
        "       bioseq.query_len(t.seq0[128:224]), "
        "       bioseq.target_len(t.seq0[128:224]), "
        "       bioseq.cigar(t.seq0[128:224]) "
        "FROM ({} AS t "
        " INNER JOIN doc0377_match_binding_queries AS q1 "
        " ON q1.id=1 AND mmsearch(t.seq0,q1.seq)) "
        "INNER JOIN doc0377_match_binding_queries AS q2 "
        " ON q2.id=t.pk+1 "
        "AND mmsearch(t.seq0[128:224],q2.seq) "
        "ORDER BY t.pk",
        target_table);
    const auto wrapped_plan = explain_text<false>(ctx, wrapped_owner_sql);
    REQUIRE(std::any_of(
        wrapped_plan.begin(), wrapped_plan.end(),
        [](const std::string& line) {
            return line.find("Merge Join") != std::string::npos;
        }));
    REQUIRE(std::any_of(
        wrapped_plan.begin(), wrapped_plan.end(),
        [](const std::string& line) {
            return line.find("Sort") != std::string::npos;
        }));
    const auto wrapped_rows = query_rows(ctx, wrapped_owner_sql);
    REQUIRE(wrapped_rows == nested_rows);
    REQUIRE(wrapped_rows.size() == 1u);
    REQUIRE(wrapped_rows.front()[5].has_value());
    REQUIRE(*wrapped_rows.front()[5] != "!UNEXPECTED!");
    REQUIRE(wrapped_rows.front()[8].has_value());
    REQUIRE(*wrapped_rows.front()[8] != "!UNEXPECTED!");
    exec_sql(ctx,
        "RESET enable_nestloop; "
        "RESET enable_hashjoin; "
        "RESET enable_mergejoin;");

    /* A nonsequence helper marker from the second alias of a composite target
     * binds through that exact alias lineage, not merely the first source Var
     * selected for physical ownership. */
    const auto composite_lineage = query_rows(ctx, fmt::format(
        "SELECT bioseq.query_len(b.pk), bioseq.target_len(b.pk), "
        "       bioseq.cigar(b.pk) "
        "FROM ({} AS a INNER JOIN {} AS b ON a.pk=b.pk) "
        "INNER JOIN doc0377_match_binding_queries AS q "
        "ON q.id=1 AND mmsearch("
        " a.seq0 OPERATOR(bioseq.||) b.seq0, q.seq)",
        target_table, secondary_table));
    REQUIRE(composite_lineage.size() == 1u);
    REQUIRE(composite_lineage.front()[0]
        == std::optional<std::string>{"128"});
    REQUIRE(composite_lineage.front()[1]
        == std::optional<std::string>{"320"});
    REQUIRE(composite_lineage.front()[2].has_value());
    REQUIRE(*composite_lineage.front()[2] != "!UNEXPECTED!");

    /* Both owners now inhabit the same finalization scope and descend from
     * the same SQL-visible t.seq0 lineage.  A nonexact row marker therefore
     * cannot select one MatchResult. */
    const std::string ambiguous_shared_lineage_sql = fmt::format(
        "SELECT bioseq.cigar(t.pk) "
        "FROM ({} AS t "
        " INNER JOIN doc0377_match_binding_queries AS q1 "
        " ON q1.id=1 AND mmsearch(t.seq0,q1.seq)) "
        "INNER JOIN doc0377_match_binding_queries AS q2 "
        " ON q2.id=2 AND mmsearch(t.seq0[128:224],q2.seq)",
        target_table);
    const auto ambiguous = sql_error(ctx, ambiguous_shared_lineage_sql);
    REQUIRE(ambiguous.sqlstate == "42702");
    REQUIRE(ambiguous.contains(
        "bioseq match-result helper binding is ambiguous"));
    REQUIRE(ambiguous.contains("candidate_match_owners=2"));
    REQUIRE(ambiguous.contains("exact transformed target recipe"));

    const auto stacked = sql_error(ctx, fmt::format(
        "SELECT bioseq.cigar(t.seq0[0:128]) FROM {} AS t "
        "INNER JOIN doc0377_match_binding_queries AS q "
        "ON mmsearch(t.seq0[0:128],q.seq) "
        "AND mmsearch(t.seq0[128:224],q.seq)",
        target_table));
    REQUIRE(stacked.sqlstate == "0A000");
    REQUIRE(stacked.contains("stacked mmsearch"));
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
    REQUIRE(multidimensional.sqlstate == "2202E");

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
        " (3,'AC'::bioseq.seq('dna4',8) OPERATOR(bioseq.||) "
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
        "SELECT ('A'::bioseq.seq('dna4',8) OPERATOR(bioseq.||) "
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
        "VALUES ('A'::bioseq.seq('dna4',8) OPERATOR(bioseq.||) "
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
        "CREATE TABLE public.doc0377_total_key "
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
        "CREATE TABLE public.doc0377_total_key_hash AS "
        " TABLE doc0377_total_key; "
        "CREATE INDEX doc0377_total_key_hash_idx "
        " ON doc0377_total_key_hash USING hash(value bioseq.seq_hash_ops); "
        "CREATE TABLE public.doc0377_total_key_join AS "
        " TABLE doc0377_total_key; "
        "CREATE TABLE public.doc0377_total_key_source "
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
        "FROM doc0377_total_key_source a "
        "JOIN doc0377_total_key_source b ON a.id=1 AND b.id=2;";
    const auto source_alias_plan = explain_text<false>(
        ctx, source_alias_query);
    REQUIRE(plan_contains(source_alias_plan, "id = 1"));
    REQUIRE(plan_contains(source_alias_plan, "id = 2"));
    const auto layouts = query_one(ctx, source_alias_query);
    for (const auto& value : layouts) {
        REQUIRE(value == "t");
    }
}

TEST_CASE("DOC0377 typmodless letter persistence validates runtime Params",
          "[letter][typmod][param][copy][acceptance]") {
    auto ctx = fresh_context({
        "doc0377_letter_param_target",
        "doc0377_letter_param_typed",
        "doc0377_letter_param_ctas_text",
        "doc0377_letter_param_ctas_binary",
    });
    exec_sql(ctx,
        "CREATE TABLE public.doc0377_letter_param_target "
        " (id int PRIMARY KEY, value bioseq.letter); "
        "CREATE TABLE public.doc0377_letter_param_typed "
        " (id int PRIMARY KEY, value bioseq.letter('dna4'));");

    const Oid letter_oid = static_cast<Oid>(query_scalar_int(
        ctx, "SELECT 'bioseq.letter'::regtype::oid;"));
    constexpr std::array<char, 3> physical_a{char{1}, char{0}, char{0}};
    constexpr std::array<char, 3> physical_c{char{1}, char{0}, char{1}};
    constexpr char text_a[] = "A";

    const auto ordinary_text = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_ordinary_text", "SELECT $1::text;",
        letter_oid, text_a, 1, /* text = */ 0);
    REQUIRE(ordinary_text.status == PGRES_FATAL_ERROR);
    REQUIRE(ordinary_text.sqlstate == "22023");

    const auto ordinary_binary = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_ordinary_binary", "SELECT $1::text;",
        letter_oid, physical_a.data(), physical_a.size(), /* binary = */ 1);
    REQUIRE(ordinary_binary.status == PGRES_TUPLES_OK);
    REQUIRE(ordinary_binary.value == "A");

    const auto typmodless_text_insert = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_typmodless_insert",
        "INSERT INTO public.doc0377_letter_param_target VALUES (1,$1) "
        "RETURNING value::text;",
        letter_oid, text_a, 1, 0);
    REQUIRE(typmodless_text_insert.status == PGRES_FATAL_ERROR);
    REQUIRE(typmodless_text_insert.sqlstate == "22023");
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM public.doc0377_letter_param_target;") == 0);

    const auto typmodless_binary_insert = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_binary_insert",
        "INSERT INTO public.doc0377_letter_param_target VALUES (1,$1) "
        "RETURNING value::text;",
        letter_oid, physical_a.data(), physical_a.size(), 1);
    REQUIRE(typmodless_binary_insert.status == PGRES_TUPLES_OK);
    REQUIRE(typmodless_binary_insert.value == "A");

    const auto typed_text_insert = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_typed_insert",
        "INSERT INTO public.doc0377_letter_param_typed VALUES (1,$1) "
        "RETURNING value::text;",
        letter_oid, text_a, 1, 0);
    REQUIRE(typed_text_insert.status == PGRES_TUPLES_OK);
    REQUIRE(typed_text_insert.value == "A");

    const auto update_text = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_update_text",
        "UPDATE public.doc0377_letter_param_target SET value=$1 "
        "WHERE id=1 RETURNING value::text;",
        letter_oid, text_a, 1, 0);
    REQUIRE(update_text.status == PGRES_FATAL_ERROR);
    REQUIRE(update_text.sqlstate == "22023");

    const auto update_binary = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_update_binary",
        "UPDATE public.doc0377_letter_param_target SET value=$1 "
        "WHERE id=1 RETURNING value::text;",
        letter_oid, physical_c.data(), physical_c.size(), 1);
    REQUIRE(update_binary.status == PGRES_TUPLES_OK);
    REQUIRE(update_binary.value == "C");

    const auto conflict_text = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_conflict_text",
        "INSERT INTO public.doc0377_letter_param_target "
        "VALUES (1,'A'::bioseq.letter('dna4')) "
        "ON CONFLICT (id) DO UPDATE SET value=$1 "
        "RETURNING value::text;",
        letter_oid, text_a, 1, 0);
    REQUIRE(conflict_text.status == PGRES_FATAL_ERROR);
    REQUIRE(conflict_text.sqlstate == "22023");
    REQUIRE(query_scalar(
        ctx,
        "SELECT value::text FROM public.doc0377_letter_param_target "
        "WHERE id=1;") == "C");

    const auto insert_select_text = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_insert_select_text",
        "INSERT INTO public.doc0377_letter_param_target "
        "SELECT 5,$1 RETURNING value::text;",
        letter_oid, text_a, 1, 0);
    REQUIRE(insert_select_text.status == PGRES_FATAL_ERROR);
    REQUIRE(insert_select_text.sqlstate == "22023");

    const auto merge_update_text = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_merge_update_text",
        "MERGE INTO public.doc0377_letter_param_target AS target "
        "USING (VALUES (1)) AS source(id) ON target.id=source.id "
        "WHEN MATCHED THEN UPDATE SET value=$1;",
        letter_oid, text_a, 1, 0);
    REQUIRE(merge_update_text.status == PGRES_FATAL_ERROR);
    REQUIRE(merge_update_text.sqlstate == "22023");

    const auto merge_insert_text = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_merge_insert_text",
        "MERGE INTO public.doc0377_letter_param_target AS target "
        "USING (VALUES (6)) AS source(id) ON target.id=source.id "
        "WHEN NOT MATCHED THEN INSERT (id,value) VALUES (source.id,$1);",
        letter_oid, text_a, 1, 0);
    REQUIRE(merge_insert_text.status == PGRES_FATAL_ERROR);
    REQUIRE(merge_insert_text.sqlstate == "22023");

    const auto ctas_text = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_ctas_text",
        "CREATE TABLE public.doc0377_letter_param_ctas_text AS "
        "SELECT $1 AS value;",
        letter_oid, text_a, 1, 0);
    REQUIRE(ctas_text.status == PGRES_FATAL_ERROR);
    REQUIRE(ctas_text.sqlstate == "22023");
    REQUIRE(query_scalar(
        ctx,
        "SELECT to_regclass('public.doc0377_letter_param_ctas_text') "
        "IS NULL;") == "t");

    const auto ctas_binary = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_ctas_binary",
        "CREATE TABLE public.doc0377_letter_param_ctas_binary AS "
        "SELECT $1 AS value;",
        letter_oid, physical_a.data(), physical_a.size(), 1);
    REQUIRE(ctas_binary.status == PGRES_COMMAND_OK);
    REQUIRE(query_scalar(
        ctx,
        "SELECT value::text FROM "
        "public.doc0377_letter_param_ctas_binary;") == "A");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT atttypmod FROM pg_catalog.pg_attribute "
        "WHERE attrelid='public.doc0377_letter_param_ctas_binary'::regclass "
        "AND attname='value';") == -1);

    const auto explain_insert_text = execute_prepared_letter_parameter(
        ctx.raw(), "doc0377_param_explain_insert_text",
        "EXPLAIN (ANALYZE, COSTS OFF) "
        "INSERT INTO public.doc0377_letter_param_target VALUES (7,$1);",
        letter_oid, text_a, 1, 0);
    REQUIRE(explain_insert_text.status == PGRES_FATAL_ERROR);
    REQUIRE(explain_insert_text.sqlstate == "22023");
}

TEST_CASE("DOC0377 physical DNA4 letters support arrays grouping and indexes",
          "[letter][array][btree][hash][acceptance]") {
    auto ctx = fresh_context({
        "doc0377_letters_a",
        "doc0377_letters_b",
        "doc0377_letters_binary",
        "doc0377_letters_receive",
        "doc0377_letters_text_copy",
        "doc0377_letters_typmodless_copy",
        "doc0377_letters_dna5",
        "doc0377_letters_dna5_b",
        "doc0377_letters_dna5_binary",
        "doc0377_letters_dna5_receive",
        "doc0377_letters_dna5_text",
    });
    exec_sql(ctx,
        "SET search_path TO pg_catalog, bioseq, public; "
        "CREATE TABLE public.doc0377_letters_a "
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
        "CREATE TABLE public.doc0377_letters_b AS "
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
        "CREATE TABLE public.doc0377_letters_binary "
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

    const auto text_copy_path = fmt::format(
        "/tmp/bioseq_doc0377_letters_text_{}.txt",
        query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    const auto id_copy_path = fmt::format(
        "/tmp/bioseq_doc0377_letters_id_{}.txt",
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
        "CREATE TABLE public.doc0377_letters_text_copy "
        " (id int, value bioseq.letter('dna4')); "
        "CREATE TABLE public.doc0377_letters_typmodless_copy "
        " (id int, value bioseq.letter);");
    exec_sql(ctx, fmt::format(
        "COPY public.doc0377_letters_text_copy FROM '{}' (FORMAT text);",
        text_copy_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM public.doc0377_letters_text_copy;") == "ACGT");

    const auto text_copy_rejected = sql_error(
        ctx, fmt::format(
            "COPY public.doc0377_letters_typmodless_copy "
            "FROM '{}' (FORMAT text);",
            text_copy_path));
    REQUIRE(text_copy_rejected.sqlstate == "22023");
    REQUIRE(text_copy_rejected.contains("text COPY requires an alphabet typmod"));
    const auto csv_copy_rejected = sql_error(
        ctx, fmt::format(
            "COPY public.doc0377_letters_typmodless_copy "
            "FROM '{}' (FORMAT csv);",
            text_copy_path));
    REQUIRE(csv_copy_rejected.sqlstate == "22023");
    const auto program_copy_rejected = sql_error(
        ctx,
        "COPY public.doc0377_letters_typmodless_copy "
        "FROM PROGRAM 'true' (FORMAT text);");
    REQUIRE(program_copy_rejected.sqlstate == "22023");

    PGresult* stdin_copy = PQexec(
        ctx.raw(),
        "COPY public.doc0377_letters_typmodless_copy FROM STDIN "
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
        "COPY public.doc0377_letters_typmodless_copy(id) "
        "FROM '{}' (FORMAT text);",
        id_copy_path));
    exec_sql(ctx, fmt::format(
        "COPY public.doc0377_letters_typmodless_copy "
        "FROM '{}' (FORMAT binary);",
        binary_path));
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT count(*) FROM public.doc0377_letters_typmodless_copy "
        "WHERE value IS NOT NULL;") == 4);
    REQUIRE(std::remove(binary_path.c_str()) == 0);
    REQUIRE(std::remove(text_copy_path.c_str()) == 0);
    REQUIRE(std::remove(id_copy_path.c_str()) == 0);

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
        "CREATE TABLE public.doc0377_letters_receive "
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
        "CREATE TABLE public.doc0377_letters_dna5 "
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
        "CREATE TABLE public.doc0377_letters_dna5_b AS "
        " TABLE doc0377_letters_dna5; "
        "CREATE TABLE public.doc0377_letters_dna5_binary "
        " (id int, value bioseq.letter('dna5')); "
        "CREATE TABLE public.doc0377_letters_dna5_receive "
        " (value bioseq.letter('dna5')); "
        "CREATE TABLE public.doc0377_letters_dna5_text "
        " (id int, value bioseq.letter('dna5')); "
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

    const auto dna5_text_path = fmt::format(
        "/tmp/bioseq_doc0377_letters_dna5_{}.txt", backend_pid);
    (void) std::remove(dna5_text_path.c_str());
    {
        std::ofstream output(dna5_text_path, std::ios::binary);
        REQUIRE(output.is_open());
        output << "1\tA\n2\tC\n3\tG\n4\tT\n5\tN\n";
        REQUIRE(output.good());
    }
    exec_sql(ctx, fmt::format(
        "COPY public.doc0377_letters_dna5_text "
        "FROM '{}' (FORMAT text);",
        dna5_text_path));
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(value::text,'' ORDER BY id) "
        "FROM public.doc0377_letters_dna5_text;") == "ACGTN");
    REQUIRE(std::remove(dna5_text_path.c_str()) == 0);

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
    auto ctx = fresh_context({"doc0377_synthetic_length_heap",
                              "doc0377_synthetic_length_index_input",
                              "doc0377_synthetic_length_real_index_input",
                              "doc0377_synthetic_lengths"});
    exec_sql(ctx,
        "CREATE TABLE public.doc0377_synthetic_lengths "
        " (label text PRIMARY KEY, n bigint NOT NULL); "
        "INSERT INTO public.doc0377_synthetic_lengths VALUES "
        " ('above_flat',4294967296), "
        " ('bigint_max',9223372036854775807);");

    const auto above_flat = query_one(
        ctx,
        "SELECT bioseq.len(s), "
        "       bioseq.len(s[4294967294::bigint:4294967296::bigint]), "
        "       s[-1]::text, "
        "       bioseq.len(s OPERATOR(bioseq.||) "
        "          bioseq_internal.synthetic_lazy_sequence(0)) "
        "FROM public.doc0377_synthetic_lengths AS source "
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
        " FROM public.doc0377_synthetic_lengths "
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
        "FROM public.doc0377_synthetic_lengths AS source "
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
        "FROM public.doc0377_synthetic_lengths "
        "WHERE label='bigint_max';",
        "SELECT bioseq_internal.synthetic_lazy_sequence(1) "
        "       OPERATOR(bioseq.||) "
        "       bioseq_internal.synthetic_lazy_sequence(n) "
        "FROM public.doc0377_synthetic_lengths "
        "WHERE label='bigint_max';",
    };
    for (const char* sql : concat_overflow_queries) {
        REQUIRE(sql_error(ctx, sql).sqlstate == "54000");
    }

    const std::array<const char*, 3> materialization_boundaries{
        "SELECT bioseq.to_text("
        " bioseq_internal.synthetic_lazy_sequence(n)) "
        "FROM public.doc0377_synthetic_lengths "
        "WHERE label='above_flat';",
        "SELECT bioseq.seq_send("
        " bioseq_internal.synthetic_lazy_sequence(n)) "
        "FROM public.doc0377_synthetic_lengths "
        "WHERE label='above_flat';",
        "SELECT bioseq.to_array("
        " bioseq_internal.synthetic_lazy_sequence(n)) "
        "FROM public.doc0377_synthetic_lengths "
        "WHERE label='above_flat';",
    };
    for (const char* sql : materialization_boundaries) {
        REQUIRE(sql_error(ctx, sql).sqlstate == "54000");
    }

    exec_sql(ctx,
        "CREATE TABLE public.doc0377_synthetic_length_index_input "
        " (n bigint NOT NULL); "
        "INSERT INTO public.doc0377_synthetic_length_index_input "
        "VALUES (4294967296);");
    const auto synthetic_index_boundary = sql_error(
        ctx,
        "CREATE INDEX doc0377_synthetic_length_expr_idx "
        "ON public.doc0377_synthetic_length_index_input USING btree "
        "((bioseq_internal.synthetic_lazy_sequence(n)));");
    REQUIRE(synthetic_index_boundary.sqlstate == "54000");
    REQUIRE(synthetic_index_boundary.contains("too long to flatten"));

    // Retain a physically stored incompressible value as a separate proof of
    // PostgreSQL's ordinary btree row-size boundary.  It complements rather
    // than substitutes for the synthetic UINT32_MAX+1 expression above.
    exec_sql(ctx,
        "CREATE TABLE public.doc0377_synthetic_length_real_index_input "
        " (id bigint, s bioseq.seq('dna4',8)); "
        "INSERT INTO public.doc0377_synthetic_length_real_index_input "
        "SELECT 1, translate("
        " string_agg(md5(i::text), '' ORDER BY i), "
        " '0123456789abcdef', 'ACGTACGTACGTACGT')"
        " ::bioseq.seq('dna4',8) "
        "FROM generate_series(1,1024) AS g(i);");
    const auto physical_index_boundary = sql_error(
        ctx,
        "CREATE INDEX doc0377_physical_length_expr_idx "
        "ON public.doc0377_synthetic_length_real_index_input USING btree "
        "((s[0:32768]));");
    REQUIRE(physical_index_boundary.sqlstate == "54000");
    REQUIRE(physical_index_boundary.contains("index row"));

    exec_sql(ctx,
        "CREATE TABLE public.doc0377_synthetic_length_heap "
        " (s bioseq.seq('dna4',8));");
    const auto heap_boundary = sql_error(
        ctx,
        "INSERT INTO public.doc0377_synthetic_length_heap "
        "SELECT bioseq_internal.synthetic_lazy_sequence(n) "
        "FROM public.doc0377_synthetic_lengths "
        "WHERE label='above_flat';");
    REQUIRE(heap_boundary.sqlstate == "54000");
}

TEST_CASE("DOC0377 uint64 bigint and MaxAlloc alignment guards are exact",
          "[seq][slice][length][overflow][rope][lifetime][acceptance]") {
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

TEST_CASE("DOC0377 slice and cast builders release pre-error intermediates",
          "[seq][slice][cast][rope][lifetime][error-atomic][acceptance]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.doc0377_runtime_text(value text) "
        "RETURNS text LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "BEGIN RETURN value; END $$; "
        "DO $doc0377$ "
        "DECLARE iteration integer; "
        "BEGIN "
        "  FOR iteration IN 1..32 LOOP "
        "    BEGIN "
        "      PERFORM ("
        "        pg_temp.doc0377_runtime_text(repeat('A',4097))::bioseq.seq "
        "        OPERATOR(bioseq.||) "
        "        pg_temp.doc0377_runtime_text('X')::bioseq.seq"
        "      )::bioseq.seq('dna4',32); "
        "      RAISE EXCEPTION 'expected invalid DNA4 literal'; "
        "    EXCEPTION WHEN invalid_text_representation THEN "
        "      NULL; "
        "    END; "
        "  END LOOP; "
        "END $doc0377$;");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal.piece_tree_self_test();")
        == "t");
}

TEST_CASE("DOC0377 literal coalescing and arena guards are exact",
          "[seq][concat][rope][lifetime][acceptance]") {
    auto ctx = fresh_context();
    exec_sql(ctx,
        "CREATE FUNCTION pg_temp.doc0377_runtime_length(value integer) "
        "RETURNS integer LANGUAGE plpgsql VOLATILE STRICT AS $$ "
        "BEGIN RETURN value; END $$;");

    const auto thresholds = query_one(
        ctx,
        "SELECT bioseq_internal.sequence_piece_count("
        "         repeat('A',pg_temp.doc0377_runtime_length(2047))::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',pg_temp.doc0377_runtime_length(2048))::bioseq.seq), "
        "       bioseq_internal.sequence_piece_count("
        "         repeat('A',pg_temp.doc0377_runtime_length(2048))::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',pg_temp.doc0377_runtime_length(2048))::bioseq.seq), "
        "       bioseq_internal.sequence_piece_count("
        "         repeat('A',pg_temp.doc0377_runtime_length(2048))::bioseq.seq "
        "         OPERATOR(bioseq.||) "
        "         repeat('C',pg_temp.doc0377_runtime_length(2049))::bioseq.seq);");
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
    REQUIRE(expanded[1].has_value());
    REQUIRE(std::stoll(expanded[1].value()) <= 16);
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

    const auto translated_cpp = sql_error(
        ctx,
        "SELECT bioseq_internal.arm_sequence_source_failure("
        " 'diagnostic_std_exception');");
    REQUIRE(translated_cpp.sqlstate == "XX000");
    REQUIRE(translated_cpp.contains(
        "bioseq sequence callback raised a C++ exception"));
    REQUIRE(translated_cpp.contains(
        "injected DOC0377 diagnostic C++ exception"));

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
    auto ctx = fresh_context({
        "doc0377_source_read_faults",
        "doc0377_source_read_heap",
    });
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
    const auto cleanup_sql = fmt::format(
        "DROP DATABASE IF EXISTS {} WITH (FORCE);", database);
    database_cleanup_guard cleanup{admin.raw(), cleanup_sql.c_str()};
    exec_sql(admin, cleanup_sql);
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
    cleanup.release();

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

TEST_CASE("DOC0377 SQL packing is identical across tails and slice splits",
          "[seq][slice][concat][packing][binary][acceptance]") {
    auto ctx = fresh_context();
    exec_sql(
        ctx,
        "CREATE TEMP TABLE doc0377_packing_flatten ("
        " case_id integer PRIMARY KEY,"
        " expected_text text NOT NULL,"
        " expected_binary bytea NOT NULL,"
        " expected_length bigint NOT NULL,"
        " value bioseq.seq NOT NULL);");

    // Level zero deliberately exercises the widest DNA4 packing profile so
    // fast local iteration still crosses the largest tail boundary.
    require_doc0377_sql_packing_profile(ctx, "dna4", 64u, 2u, "ACGT");

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
    require_doc0377_sql_packing_profile(ctx, "dna5", 64u, 3u, "ACGTN");
#endif

#if BIOSEQ_OPTTIMIZATION_LEVEL > 1
    for (const std::uint32_t storage_width_bits : {8u, 16u, 32u}) {
        require_doc0377_sql_packing_profile(
            ctx, "dna4", storage_width_bits, 2u, "ACGT");
        require_doc0377_sql_packing_profile(
            ctx, "dna5", storage_width_bits, 3u, "ACGTN");
    }
#endif
}
