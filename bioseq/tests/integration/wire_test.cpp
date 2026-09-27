/**
 * @file wire_test.cpp
 * @brief Tests binary sequence transport and rejection of invalid input.
 *
 * COPY BINARY must preserve sequences and their lengths. Failed receive
 * calls must leave the PostgreSQL connection usable.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;


using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_source_table = "wire_source";
constexpr std::string_view k_destination_table = "wire_destination";
constexpr std::string_view k_binary_path = "/tmp/bioseq_seq_wire.bin";

/** @brief Seeds the source relation used by the binary COPY round trip. */
void seed_pair(connection_test_context& ctx) {
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_source_table),
            .typmod = {.alphabet = "dna4", .width = 64},
            .seq_columns = 1,
        },
        {
            seq_row{.pk = 23, .sequences = {"ACTAGGCACAT"}},
            seq_row{.pk = 101, .sequences = {"GGGGTTTTACTAGGCACATACTAGGCACATACT"}},
            seq_row{.pk = 69, .sequences = {"ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT"}},
            seq_row{.pk = 42, .sequences = {"TTTTTT"}},
        });
}

} // namespace

TEST_CASE("COPY BINARY round-trip preserves bioseq.seq payloads",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
    auto ctx = fresh_context({k_source_table, k_destination_table});
    seed_pair(ctx);

    // Try to remove a file left by an earlier COPY test.
    std::error_code cleanup_error;
    std::filesystem::remove(std::filesystem::path(k_binary_path), cleanup_error);

    exec_sqlf(ctx, "COPY {} TO '{}' (FORMAT binary);", k_source_table, k_binary_path);

    exec_sqlf(ctx, "CREATE TABLE {} (LIKE {} INCLUDING ALL);", k_destination_table, k_source_table);
    exec_sqlf(ctx, "COPY {} FROM '{}' (FORMAT binary);", k_destination_table, k_binary_path);

    const auto source_rows =
        query_rows(ctx,
                   fmt::format("SELECT pk, seq0::text, bioseq.len(seq0), bioseq.unit_len(seq0) "
                               "FROM {} ORDER BY pk;",
                               k_source_table));
    const auto destination_rows =
        query_rows(ctx,
                   fmt::format("SELECT pk, seq0::text, bioseq.len(seq0), bioseq.unit_len(seq0) "
                               "FROM {} ORDER BY pk;",
                               k_destination_table));

    REQUIRE(source_rows == destination_rows);
    REQUIRE(source_rows.size() == 4);
}

TEST_CASE("seq_recv rejects malformed wire payloads (or refuses to be invoked)",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
    auto ctx = fresh_context();
    // This attempted seq_recv call must fail. PostgreSQL may reject the call
    // before decoding because its first argument is an internal value.
    // Check that the connection remains usable afterwards.
    REQUIRE(sql_fails(ctx,
                      "SELECT bioseq.seq_recv("
                      "    e'\\\\x0000000000000000630000000000'::bytea, 0::oid, -1);"));

    // Verify that another statement succeeds after the failed call.
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("letter_recv rejects unknown versions, bad ranks, and typmod mismatch",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar(ctx, "SELECT bioseq_internal.letter_wire_self_test();") == "t");
}
