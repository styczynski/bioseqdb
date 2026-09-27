/**
 * @file sequence_fragment_storage_test.cpp
 * @brief Checks fragment storage using relation sizes and catalog entries.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "fragment_storage_layout";

/** @brief Returns the aggregate physical size of a parent's sequence shards. */
[[nodiscard]] long long sub_table_relation_size(const connection_test_context& ctx,
                                                std::string_view parent_table) {
    return query_scalar_int(
        ctx,
        fmt::format("SELECT COALESCE(SUM(pg_relation_size(s.sub_table_relid)), 0)::bigint "
                    "  FROM bioseq_internal.seq_sub_table s "
                    " WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}');",
                    parent_table));
}

} // namespace

TEST_CASE("multi-fragment vs single-fragment relation size scales monotonically",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx_small = fresh_context({k_table});
    // Create a short sequence that fits in one fragment.
    exec_sqlf(ctx_small, "SELECT bioseq.create_sample_table('{}', 1, 1024, 'dna4', 42);", k_table);
    const long long size_small = sub_table_relation_size(ctx_small, k_table);
    REQUIRE(size_small > 0);

    auto ctx_large = fresh_context({k_table});
    // A 100,000-base sequence needs more fragment storage.
    exec_sqlf(ctx_large, "SELECT bioseq.create_sample_table('{}', 1, 100000, 'dna4', 7);", k_table);
    const long long size_large = sub_table_relation_size(ctx_large, k_table);
    REQUIRE(size_large > 0);
    REQUIRE(size_large > size_small);
}

TEST_CASE("multiple sequences in the same bucket land in one shard",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    // The four equal-length sequences should share a fragment storage shard.
    exec_sqlf(ctx, "SELECT bioseq.create_sample_table('{}', 4, 65000, 'dna4', 42);", k_table);
    // One catalog row (one shard).
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            k_table)) == 1);
    REQUIRE(sub_table_relation_size(ctx, k_table) > 0);
}

TEST_CASE("non-aligned final-fragment sequences cover the off-by-one boundary",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr long long k_fragment_bp = 32768;
    auto ctx = fresh_context({k_table});
    for (long long base_pair_count :
         {k_fragment_bp - 1, k_fragment_bp, k_fragment_bp + 1, static_cast<long long>(123456ULL)}) {
        exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", k_table);
        exec_sqlf(ctx,
                  "SELECT bioseq.create_sample_table('{}', 1, {}, 'dna4', 42);",
                  k_table,
                  base_pair_count);
        // Each nonempty sequence must allocate fragment storage.
        REQUIRE(sub_table_relation_size(ctx, k_table) > 0);
    }
}
