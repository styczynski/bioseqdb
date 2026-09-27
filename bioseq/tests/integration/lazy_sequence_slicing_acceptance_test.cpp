/**
 * @file lazy_sequence_slicing_acceptance_test.cpp
 * @brief Exercises SQL slicing, sequence algebra, and stable mmsearch binding.
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

TEST_CASE("exact Python slicing examples and sequence algebra",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
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

TEST_CASE("sliced mmsearch helpers bind one stable owner",
          "[component:bioseq][layer:integration][feature:prefilter][resource:postgres]") {
    constexpr std::string_view target_table =
        "sequence_acceptance_match_binding_target";
    constexpr std::string_view secondary_table =
        "sequence_acceptance_match_binding_secondary";
    auto ctx = fresh_context({target_table, secondary_table});
    const std::string motif = sequence_acceptance_match_binding_motif();
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
        "CREATE TEMP TABLE sequence_acceptance_match_binding_queries ("
        " id integer PRIMARY KEY, seq bioseq.seq('dna4',32)); "
        "INSERT INTO sequence_acceptance_match_binding_queries VALUES "
        " (1, '{}'::bioseq.seq('dna4',32)),"
        " (2, '{}'::bioseq.seq('dna4',32)),"
        " (3, '{}'::bioseq.seq('dna4',32));",
        motif, short_motif, motif));
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather = 0; "
        "SET bioseq.index.sequence_read.enabled = off; "
        "SET bioseq.aligner.vulkan.enabled = off; "
        "SET bioseq.aligner.chaining.minimum_score = 10; "
        "SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash'; "
        "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2'; "
        "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner'; "
        "SET join_collapse_limit = 1; "
        "SET from_collapse_limit = 1; "
        "SET geqo = off;");

    /* This diagnostic runs owner propagation over all five wrapper Path types.
     * The nested SQL below supplies the finalized-plan executor evidence:
     * helper values come from each MatchResult and never from a projection stub. */
    const auto wrapper_receipt = query_one(
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
    REQUIRE(wrapper_receipt.size() == 19u);
    for (const auto& field : wrapper_receipt) {
        REQUIRE(field.has_value());
        REQUIRE(*field == "true");
    }
    const auto executor_translation_receipt = query_one(
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
    REQUIRE(executor_translation_receipt.size() == 11u);
    for (const auto& field : executor_translation_receipt) {
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
        std::string{"PREPARE sequence_acceptance_base_match_owner_offset AS "}
            + offset_base_owner_sql);
    REQUIRE(query_rows(ctx, "EXECUTE sequence_acceptance_base_match_owner_offset;")
        == offset_base_owner_rows);
    REQUIRE(query_rows(ctx, "EXECUTE sequence_acceptance_base_match_owner_offset;")
        == offset_base_owner_rows);
    exec_sql(ctx, "DEALLOCATE sequence_acceptance_base_match_owner_offset;");

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
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
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
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
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
            {"bioseq.vectorization.enabled", {"on", "off"}},
            {"bioseq.column_cache.enabled", {"on", "off"}},
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

    /* The primary-key spelling provides the unique-lineage fallback when
     * there is exactly one owner in the statement. */
    const std::string lineage_fallback_sql = fmt::format(
        "SELECT bioseq.query_start(t.pk), bioseq.query_end(t.pk), "
        "       bioseq.target_start(t.pk), bioseq.target_end(t.pk), "
        "       bioseq.edit(t.pk), bioseq.cigar(t.pk), "
        "       bioseq.has_cigar(t.pk) "
        "FROM {} AS t "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
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

    /* A sequence-valued edit argument for a transformed target must equal the
     * complete target recipe. */
    const auto mismatched_edit = sql_error(ctx, fmt::format(
        "SELECT bioseq.edit(t.seq0) FROM {} AS t "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
        "ON q.id=1 AND mmsearch(t.seq0[0:128],q.seq)",
        target_table));
    REQUIRE(mismatched_edit.sqlstate == "0A000");
    REQUIRE(mismatched_edit.contains(
        "bioseq.edit projection must reference the mmsearch target"));

    const auto raw_exact = query_rows(ctx, fmt::format(
        "SELECT bioseq.query_len(t.seq0), bioseq.target_len(t.seq0), "
        "       bioseq.cigar(t.seq0) "
        "FROM {} AS t "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
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
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
        "ON q.id=2 AND mmsearch(t.seq0[128:224],q.seq)",
        target_table));
    REQUIRE(tail_exact.size() == 1u);
    REQUIRE(tail_exact.front()[0] == std::optional<std::string>{"96"});
    REQUIRE(tail_exact.front()[1] == std::optional<std::string>{"96"});
    REQUIRE(tail_exact.front()[2].has_value());
    REQUIRE(*tail_exact.front()[2] != "!UNEXPECTED!");
    REQUIRE(raw_exact.front()[2] != tail_exact.front()[2]);

    /* A parameterized baserel candidate sees both the base PARAM_EXTERN owner
     * and the higher sliced join owner through ParamPathInfo.  Their distinct
     * RestrictInfo frontiers must be decided before query operand shape; each
     * exact helper then binds to its own MatchResult. */
    exec_sql(ctx,
        "SET plan_cache_mode = force_generic_plan; "
        "PREPARE sequence_acceptance_base_param_sliced_join(bioseq.seq) AS "
        "SELECT t.pk, q.id, "
        "       bioseq.query_len(t.seq0), "
        "       bioseq.target_len(t.seq0), bioseq.cigar(t.seq0), "
        "       bioseq.query_len(t.seq0[128:224]), "
        "       bioseq.target_len(t.seq0[128:224]), "
        "       bioseq.cigar(t.seq0[128:224]) "
        "FROM sequence_acceptance_match_binding_target AS t "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
        "ON q.id=2 AND mmsearch(t.seq0[128:224], q.seq[0:]) "
        "WHERE mmsearch(t.seq0, $1) "
        "ORDER BY t.pk;");
    const std::string base_param_execution = fmt::format(
        "EXECUTE sequence_acceptance_base_param_sliced_join({});",
        format_literal(
            motif, seq_typmod{.alphabet = "dna4", .width = 32}));
    const auto base_param_plan = explain_text<false>(
        ctx, base_param_execution);
    REQUIRE(std::count_if(
        base_param_plan.begin(), base_param_plan.end(),
        [](const std::string& line) {
            return line.find("Custom Scan (BioseqBatchJoin)")
                != std::string::npos;
        }) == 1);
    REQUIRE(std::count_if(
        base_param_plan.begin(), base_param_plan.end(),
        [](const std::string& line) {
            return line.find("Custom Scan (BioseqScan)")
                != std::string::npos;
        }) == 1);
    const auto base_param_rows = query_rows(ctx, base_param_execution);
    REQUIRE(base_param_rows.size() == 1u);
    REQUIRE(base_param_rows.front()
        == std::vector<std::optional<std::string>>{
            std::optional<std::string>{"1"},
            std::optional<std::string>{"2"},
            raw_exact.front()[0], raw_exact.front()[1], raw_exact.front()[2],
            tail_exact.front()[0], tail_exact.front()[1],
            tail_exact.front()[2]});
    exec_sql(ctx,
        "DEALLOCATE sequence_acceptance_base_param_sliced_join; "
        "RESET plan_cache_mode;");

    /* The same physical/recipe target can belong to two frontiers when the
     * base restriction has a Const query and the higher join evaluates a
     * sliced query recipe. The plan must contain one owner at each frontier
     * and no executable SQL mmsearch filter. */
    const std::string base_const_same_target_sql = fmt::format(
        "SELECT t.pk, q.id "
        "FROM {} AS t "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
        "ON q.id=1 AND mmsearch(t.seq0, q.seq[0:]) "
        "WHERE mmsearch(t.seq0, {}) "
        "ORDER BY t.pk, q.id",
        target_table,
        format_literal(
            motif, seq_typmod{.alphabet = "dna4", .width = 32}));
    const auto base_const_same_target_plan = explain_text<false>(
        ctx, base_const_same_target_sql);
    REQUIRE(std::count_if(
        base_const_same_target_plan.begin(),
        base_const_same_target_plan.end(),
        [](const std::string& line) {
            return line.find("Custom Scan (BioseqBatchJoin)")
                != std::string::npos;
        }) == 1);
    REQUIRE(std::count_if(
        base_const_same_target_plan.begin(),
        base_const_same_target_plan.end(),
        [](const std::string& line) {
            return line.find("Custom Scan (BioseqScan)")
                != std::string::npos;
        }) == 1);
    REQUIRE_FALSE(plan_contains(
        base_const_same_target_plan, "Filter: mmsearch"));
    REQUIRE_FALSE(plan_contains(
        base_const_same_target_plan, "Join Filter: mmsearch"));
    /* Without a match-result helper both owners retain the full hit and the
     * strong chain-only candidate.  Independent owner frontiers therefore
     * compose as the Cartesian MatchResult multiset, rather than executing
     * either RestrictInfo twice. */
    const auto base_const_rows = query_rows(ctx, fmt::format(
        "SELECT t.pk FROM {} AS t WHERE mmsearch(t.seq0, {}) "
        "ORDER BY t.pk",
        target_table,
        format_literal(
            motif, seq_typmod{.alphabet = "dna4", .width = 32})));
    const auto sliced_query_rows = query_rows(ctx, fmt::format(
        "SELECT t.pk, q.id FROM {} AS t "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
        "ON q.id=1 AND mmsearch(t.seq0, q.seq[0:]) "
        "ORDER BY t.pk, q.id",
        target_table));
    REQUIRE(base_const_rows.size() == 2u);
    REQUIRE(sliced_query_rows.size() == 2u);
    const auto base_const_same_target_rows = query_rows(
        ctx, base_const_same_target_sql);
    REQUIRE(base_const_same_target_rows.size()
        == base_const_rows.size() * sliced_query_rows.size());
    REQUIRE(std::all_of(
        base_const_same_target_rows.begin(),
        base_const_same_target_rows.end(),
        [](const auto& row) {
            return row == std::vector<std::optional<std::string>>{
                std::optional<std::string>{"1"},
                std::optional<std::string>{"1"}};
        }));

    /* A parenthesized, collapse-disabled join tree creates two owner nodes in
     * one PlannerInfo and therefore one stable-binding scope.  The inner raw
     * owner aligns the 224-letter whole source; the outer transformed owner
     * aligns its 96-letter tail.  Keeping only the outer owner transformed
     * also confirms nested ownership without stacking two SliceProject source
     * tags on the same BioseqScan. */
    const std::string nested_owner_sql = fmt::format(
        "SELECT t.pk, q1.id, q2.id, "
        "       bioseq.query_len(t.seq0), "
        "       bioseq.target_len(t.seq0), bioseq.cigar(t.seq0), "
        "       bioseq.query_len(t.seq0[128:224]), "
        "       bioseq.target_len(t.seq0[128:224]), "
        "       bioseq.cigar(t.seq0[128:224]) "
        "FROM ({} AS t "
        " INNER JOIN sequence_acceptance_match_binding_queries AS q1 "
        " ON q1.id=1 AND mmsearch(t.seq0,q1.seq)) "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q2 "
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
        std::string{"PREPARE sequence_acceptance_exact_match_owners AS "}
            + nested_owner_sql);
    const auto prepared_first = query_rows(
        ctx, "EXECUTE sequence_acceptance_exact_match_owners;");
    const auto prepared_second = query_rows(
        ctx, "EXECUTE sequence_acceptance_exact_match_owners;");
    REQUIRE(prepared_first == nested_rows);
    REQUIRE(prepared_second == nested_rows);
    exec_sql(ctx, "DEALLOCATE sequence_acceptance_exact_match_owners;");

    /* Three pairwise-distinct PPI required-relids sets are three owning
     * frontiers, not a three-call stack in the deepest baserel candidate. */
    const std::string three_owner_sql = fmt::format(
        "SELECT t.pk, q1.id, q2.id, q3.id, "
        "       bioseq.query_len(t.seq0), "
        "       bioseq.target_len(t.seq0), bioseq.cigar(t.seq0), "
        "       bioseq.query_len(t.seq0[128:224]), "
        "       bioseq.target_len(t.seq0[128:224]), "
        "       bioseq.cigar(t.seq0[128:224]), "
        "       bioseq.query_len(t.seq0[0:128]), "
        "       bioseq.target_len(t.seq0[0:128]), "
        "       bioseq.cigar(t.seq0[0:128]) "
        "FROM (({} AS t "
        " INNER JOIN sequence_acceptance_match_binding_queries AS q1 "
        " ON q1.id=1 AND mmsearch(t.seq0,q1.seq)) "
        " INNER JOIN sequence_acceptance_match_binding_queries AS q2 "
        " ON q2.id=2 AND mmsearch(t.seq0[128:224],q2.seq)) "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q3 "
        "ON q3.id=3 AND mmsearch(t.seq0[0:128],q3.seq) "
        "ORDER BY t.pk",
        target_table);
    const auto three_owner_plan = explain_text<false>(ctx, three_owner_sql);
    REQUIRE(std::count_if(
        three_owner_plan.begin(), three_owner_plan.end(),
        [](const std::string& line) {
            return line.find("Custom Scan (BioseqBatchJoin)")
                != std::string::npos;
        }) == 3);
    const auto three_owner_rows = query_rows(ctx, three_owner_sql);
    REQUIRE(three_owner_rows.size() == 1u);
    REQUIRE(three_owner_rows.front()
        == std::vector<std::optional<std::string>>{
            std::optional<std::string>{"1"},
            std::optional<std::string>{"1"},
            std::optional<std::string>{"2"},
            std::optional<std::string>{"3"},
            raw_exact.front()[0], raw_exact.front()[1], raw_exact.front()[2],
            tail_exact.front()[0], tail_exact.front()[1], tail_exact.front()[2],
            exact_rows.front()[0], exact_rows.front()[3],
            exact_rows.front()[9]});

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
        " INNER JOIN sequence_acceptance_match_binding_queries AS q1 "
        " ON q1.id=1 AND mmsearch(t.seq0,q1.seq)) "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q2 "
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
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
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
        " INNER JOIN sequence_acceptance_match_binding_queries AS q1 "
        " ON q1.id=1 AND mmsearch(t.seq0,q1.seq)) "
        "INNER JOIN sequence_acceptance_match_binding_queries AS q2 "
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
        "INNER JOIN sequence_acceptance_match_binding_queries AS q "
        "ON mmsearch(t.seq0[0:128],q.seq) "
        "AND mmsearch(t.seq0[128:224],q.seq)",
        target_table));
    REQUIRE(stacked.sqlstate == "0A000");
    REQUIRE(stacked.contains("stacked mmsearch"));
}
