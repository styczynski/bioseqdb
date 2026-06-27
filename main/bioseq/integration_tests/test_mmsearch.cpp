// Integration scenarios:
//   * `mmsearch(seq, query)` runs cleanly on a `USING bioseq` columnar table.
//     The exact match set depends on the alignment pipeline that is still
//     under development on this branch; we constrain only the output domain.
//   * The result set is invariant under the GUC matrix:
//        bioseq.enable_vectorization           on / off
//        bioseq.enable_column_cache            on / off
//        bioseq.enable_bioseq_index_scan       on / off
//        bioseq.aligner.enable_profiling       on / off
//   * `aligner_options()` with all-NULL fields produces the same rows as the
//     plain two-arg `mmsearch`; partial overrides (e.g. setting only
//     `kmer_size`) execute without erroring out.
//   * Per-row projection helpers (`bioseq.cigar`, `bioseq.target_start`, …)
//     produce well-shaped values: integer ranges in order, valid CIGAR text.
//   * SELECTs without an mmsearch qual on a `USING bioseq` table emit zero
//     rows. This is the documented "tuples only flow under mmsearch"
//     constraint from SPEC_HOW_TO_CONNECT_ALIGNER_3 §13.1.

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

#include <set>

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kHoe = "hoe_int";

}  // namespace

TEST_CASE("mmsearch on columnar table runs without errors and stays in domain",
          "[mmsearch][columnar]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string base_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());

    const auto pks = query_int_column(ctx, base_sql);
    INFO("mmsearch returned " << pks.size() << " pks");
    const std::set<long long> seeded{1, 2, 3, 4, 5};
    for (const auto pk : pks) {
        REQUIRE(seeded.contains(pk));
    }
}

TEST_CASE("USING bioseq table without mmsearch qual emits zero rows",
          "[mmsearch][columnar][no_qual]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    // Pinned per SPEC_HOW_TO_CONNECT_ALIGNER_3 §13.1: the custom scan only
    // buffers tuples when SeqMmsearchOpExpr is part of the scan qual list.
    REQUIRE(query_scalar_int(
                ctx, fmt::format("SELECT COUNT(*)::bigint FROM {};", kHoe)) ==
            0);
}

TEST_CASE(
    "mmsearch result set is byte-identical across the full GUC parity matrix",
    "[mmsearch][parity]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string sql = fmt::format(
        "SELECT pk, "
        "       bioseq.query_start(pk)  AS qs, "
        "       bioseq.query_end(pk)    AS qe, "
        "       bioseq.target_start(pk) AS ts, "
        "       bioseq.target_end(pk)   AS te, "
        "       bioseq.cigar(pk)        AS cg "
        "FROM {} "
        "WHERE mmsearch(seq0, {}) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal());

    const auto reference = expect_query_invariant_under_options(
        ctx, sql,
        {
            {"bioseq.enable_vectorization",     {"on", "off"}},
            {"bioseq.enable_column_cache",      {"on", "off"}},
            {"bioseq.enable_bioseq_index_scan", {"on", "off"}},
            {"bioseq.aligner.enable_profiling", {"on", "off"}},
        });

    // CIGAR strings on materialised rows must be either empty (no alignment)
    // or syntactically valid.
    for (const auto& row : reference) {
        REQUIRE(row.size() == 6);
        if (row[5].has_value() && !row[5].value().empty()) {
            REQUIRE(is_valid_cigar(row[5].value()));
        }
    }
}

namespace {
void check_single_guc_parity(std::string_view guc) {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());
    expect_invariant_under_guc_toggle(ctx, sql, guc);
}
}  // namespace

TEST_CASE("mmsearch parity: bioseq.enable_vectorization on/off agree",
          "[mmsearch][parity][toggle]") {
    check_single_guc_parity("bioseq.enable_vectorization");
}

TEST_CASE("mmsearch parity: bioseq.enable_column_cache on/off agree",
          "[mmsearch][parity][toggle]") {
    check_single_guc_parity("bioseq.enable_column_cache");
}

TEST_CASE("mmsearch parity: bioseq.enable_bioseq_index_scan on/off agree",
          "[mmsearch][parity][toggle]") {
    check_single_guc_parity("bioseq.enable_bioseq_index_scan");
}

TEST_CASE("mmsearch parity: bioseq.aligner.enable_profiling on/off agree",
          "[mmsearch][parity][toggle]") {
    check_single_guc_parity("bioseq.aligner.enable_profiling");
}

TEST_CASE("mmsearch with all-NULL aligner_options matches the GUC defaults",
          "[mmsearch][aligner_options]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string base_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());

    const std::string overridden_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, aligner_options()) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal());

    REQUIRE(query_rows(ctx, base_sql) == query_rows(ctx, overridden_sql));
}

// Each named override field is a separate TEST_CASE; the helper keeps the body
// of each call site to a single line.
namespace {
void check_aligner_options_partial_override(std::string_view named_arg_sql) {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string overridden_sql = fmt::format(
        "SELECT pk FROM {} "
        "WHERE mmsearch(seq0, {}, aligner_options({})) "
        "ORDER BY pk;",
        kHoe, hoe_probe_literal(), named_arg_sql);

    // We assert the query executes and stays inside the seeded pk domain;
    // the override may shift the match set but must not raise.
    const auto pks = query_int_column(ctx, overridden_sql);
    const std::set<long long> seeded{1, 2, 3, 4, 5};
    for (const auto pk : pks) {
        REQUIRE(seeded.contains(pk));
    }
}
}  // namespace

TEST_CASE("aligner_options(kmer_size => 11) executes without errors",
          "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("kmer_size => 11");
}

// Strengthens item 3 of the round-2 review: a per-call override on
// `kmer_size` must produce the same row set as setting the corresponding
// `bioseq.aligner.kmer_size` GUC ahead of a plain two-arg mmsearch. If they
// diverge, the override is plumbed differently and the suite catches it.
TEST_CASE("aligner_options(kmer_size => K) matches SET bioseq.aligner.kmer_size = K",
          "[mmsearch][aligner_options][parity]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    constexpr int kKmerSize = 11;

    rowset_t via_options;
    rowset_t via_guc;
    {
        // Per-call override leaves session GUCs untouched.
        const std::string sql = fmt::format(
            "SELECT pk FROM {} "
            "WHERE mmsearch(seq0, {}, aligner_options(kmer_size => {})) "
            "ORDER BY pk;",
            kHoe, hoe_probe_literal(), kKmerSize);
        via_options = query_rows(ctx, sql);
    }
    {
        guc_scope guc(ctx, "bioseq.aligner.kmer_size",
                      std::to_string(kKmerSize));
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
            kHoe, hoe_probe_literal());
        via_guc = query_rows(ctx, sql);
    }

    REQUIRE(via_options == via_guc);
}

TEST_CASE("aligner_options(window_size => 5) executes without errors",
          "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("window_size => 5");
}

TEST_CASE("aligner_options(max_occ => 69) executes without errors",
          "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("max_occ => 69");
}

TEST_CASE("aligner_options(chaining_algorithm => 0) executes without errors",
          "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("chaining_algorithm => 0");
}

TEST_CASE("selected Vulkan minimizer reports a Postgres error when Vulkan is disabled",
          "[mmsearch][vulkan][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = off;");

    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "XX000");
    REQUIRE(err.contains("Vulkan aligner execution failed"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("selected Vulkan batch rejects CIGAR projection instead of CPU fallback",
          "[mmsearch][vulkan][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    exec_sql(ctx, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'vulkan_tiled_dp';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_seed_hits_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_index_cache_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_fused_frontend_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_gpu_index_build_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_extend_enabled = off;");

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.cigar(pk) "
        "FROM {} "
        "WHERE mmsearch(seq0, {}) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal());
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "XX000");
    REQUIRE(err.contains("Vulkan aligner execution failed"));
    REQUIRE(err.contains("GPU CIGAR/extension is not implemented"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE(
    "aligner_options(base_alignment_algorithm => 0) executes without errors",
    "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("base_alignment_algorithm => 0");
}

TEST_CASE("mmsearch projection helpers run cleanly for the produced row set",
          "[mmsearch][projections]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const auto rows = query_rows(
        ctx, fmt::format(
                 "SELECT pk, "
                 "       bioseq.query_start(pk)  AS qs, "
                 "       bioseq.query_end(pk)    AS qe, "
                 "       bioseq.target_start(pk) AS ts, "
                 "       bioseq.target_end(pk)   AS te, "
                 "       bioseq.cigar(pk)        AS cigar, "
                 "       bioseq.has_cigar(pk)    AS has_cigar, "
                 "       bioseq.is_reverse(pk)   AS is_reverse, "
                 "       bioseq.is_primary(pk)   AS is_primary "
                 "FROM {} "
                 "WHERE mmsearch(seq0, {}) "
                 "ORDER BY pk;",
                 kHoe, hoe_probe_literal()));

    for (const auto& row : rows) {
        REQUIRE(row.size() == 9);
        if (row[1].has_value() && row[2].has_value()) {
            REQUIRE(std::stoll(row[1].value()) <= std::stoll(row[2].value()));
        }
        if (row[3].has_value() && row[4].has_value()) {
            REQUIRE(std::stoll(row[3].value()) <= std::stoll(row[4].value()));
        }
        if (row[5].has_value() && row[6].has_value()) {
            const bool has_text = !row[5].value().empty();
            REQUIRE(row[6].value() == (has_text ? "t" : "f"));
            if (has_text) {
                REQUIRE(is_valid_cigar(row[5].value()));
            }
        }
        if (row[7].has_value()) {
            REQUIRE((row[7].value() == "t" || row[7].value() == "f"));
        }
        if (row[8].has_value()) {
            REQUIRE((row[8].value() == "t" || row[8].value() == "f"));
        }
    }
}

// All remaining MatchResult helpers exercised in one parameterised pass.
// Each row of `kProjectionHelpers` is a (sql_function, expected_pg_type)
// pair; for any rows the alignment pipeline materialises we assert the
// helper either returns NULL or a value parseable by the named type. Adding
// a new helper takes one line.
namespace {
struct projection_probe {
    std::string_view sql_call;     // e.g. "bioseq.score(pk)"
    std::string_view pg_typename;  // "int4", "bool", "float4", "text"
};

constexpr std::array<projection_probe, 13> kProjectionHelpers = {{
    {"bioseq.score(pk)",                  "int4"},
    {"bioseq.dp_score(pk)",               "int4"},
    {"bioseq.dp_max_score(pk)",           "int4"},
    {"bioseq.chain_score(pk)",            "int4"},
    {"bioseq.secondary_chain_score(pk)",  "int4"},
    {"bioseq.seed_count(pk)",             "int4"},
    // DOC0274 §1.8 — these four are sequence_size_t (uint64) in MatchResult,
    // so the projections RETURN BIGINT (int8), not int4.
    {"bioseq.num_matches(pk)",            "int8"},
    {"bioseq.block_length(pk)",           "int8"},
    {"bioseq.mapq(pk)",                   "int4"},
    {"bioseq.target_len(pk)",             "int8"},
    {"bioseq.query_len(pk)",              "int8"},
    {"bioseq.divergence(pk)",             "float4"},
    {"bioseq.cigar(pk)",                  "text"},
}};

void check_projection_helper(const projection_probe& probe) {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    // The cast forces Postgres to type-check the helper's return value at
    // query time — a misdeclared return type lights up here as a server
    // error rather than as a silent text-is-text comparison.
    const std::string sql = fmt::format(
        "SELECT pk, ({})::{} "
        "  FROM {} "
        " WHERE mmsearch(seq0, {}) "
        " ORDER BY pk;",
        probe.sql_call, probe.pg_typename, kHoe,
        hoe_probe_literal());

    const auto rows = query_rows(ctx, sql);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        // The pk column must come back as an integer.
        REQUIRE(row[0].has_value());
        (void)std::stoll(row[0].value());
        // Cast itself succeeded: any value, even NULL, is acceptable here.
    }
}
}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "Every MatchResult projection helper executes and casts to its declared type",
    "[mmsearch][projections][matrix]",
    ((size_t Idx), Idx),
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12) {
    static_assert(Idx < kProjectionHelpers.size());
    check_projection_helper(kProjectionHelpers[Idx]);
}
