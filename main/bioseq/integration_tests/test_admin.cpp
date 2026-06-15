// Integration scenarios:
//   * The metadata views (`bioseq.storage`, `bioseq.options`, `bioseq.stripe`,
//     `bioseq.chunk_group`, `bioseq.chunk`) are queryable post-INSERT and
//     have the expected row counts on a freshly-populated columnar table.
//   * The defaults written into `bioseq.options` at table creation time are
//     non-NULL and within sensible ranges (chunk_group_row_limit > 0 etc.).
//
// We do not currently exercise `pg_catalog.alter_bioseq_table_set` /
// `alter_bioseq_table_reset` because the underlying `ALTER TABLE ... SET
// (bioseq.<opt>)` statement raises "unrecognized parameter namespace" on this
// branch. Once the AM registers its storage parameters, expand this file
// with set/reset coverage.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_admin_t";

long long row_count(const connection_test_context& ctx,
                    std::string_view view_or_table,
                    std::string_view filter) {
    return query_scalar_int(
        ctx, fmt::format("SELECT COUNT(*)::bigint FROM {} WHERE {};",
                         view_or_table, filter));
}

}  // namespace

TEST_CASE("bioseq.options carries default settings for a new columnar table",
          "[admin][options]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto row = query_one(
        ctx,
        fmt::format("SELECT chunk_group_row_limit, stripe_row_limit, "
                    "       compression "
                    "  FROM bioseq.options "
                    " WHERE relation = '{}'::regclass;",
                    kTable));
    REQUIRE(row.size() == 3);
    REQUIRE(std::stoll(row[0].value()) > 0);   // chunk_group_row_limit
    REQUIRE(std::stoll(row[1].value()) > 0);   // stripe_row_limit
    REQUIRE_FALSE(row[2].value().empty());     // compression name
}

TEST_CASE("bioseq.storage exposes one row per columnar relation",
          "[admin][views]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    REQUIRE(row_count(ctx, "bioseq.storage",
                      fmt::format("relation = '{}'::regclass", kTable)) == 1);
}

TEST_CASE("bioseq.stripe / chunk_group / chunk surface stripe metadata",
          "[admin][views]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    REQUIRE(row_count(ctx, "bioseq.stripe",
                      fmt::format("relation = '{}'::regclass", kTable)) >= 1);
    REQUIRE(row_count(ctx, "bioseq.chunk_group",
                      fmt::format("relation = '{}'::regclass", kTable)) >= 1);
    REQUIRE(row_count(ctx, "bioseq.chunk",
                      fmt::format("relation = '{}'::regclass", kTable)) >= 1);
}
