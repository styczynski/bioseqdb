/**
 * @file wire_test.cpp
 * @brief Tests binary sequence transport and rejection of invalid input.
 *
 * COPY BINARY must preserve sequences and their lengths. Failed receive
 * calls must leave the PostgreSQL connection, usble.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

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
    // Check that  the connection remains usable afterwards.
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

TEST_CASE("quality binary parameters preserve scores and reject malformed rank payloads",
          "[component:bioseq][layer:integration][feature:quality][feature:sql_api][resource:postgres]") {
    auto ctx = fresh_context();
    const auto sequence_oid = static_cast<Oid>(query_scalar_int(ctx,
        "SELECT 'bioseq.seq'::regtype::oid"));
    using result_owner = std::unique_ptr<PGresult, decltype(&PQclear)>;
    const auto receive = [&](const std::string& bytes) {
        const char* values[]{bytes.data()};
        const int lengths[]{static_cast<int>(bytes.size())};
        const int formats[]{1};
        return result_owner{PQexecParams(ctx.raw(),
            "SELECT $1::text, bioseq.sum($1)::text", 1, &sequence_oid,
            values, lengths, formats, 0), PQclear};
    };
    const auto send = [&](const std::string& expression) {
        const std::string query = "SELECT " + expression;
        result_owner result{PQexecParams(ctx.raw(), query.c_str(), 0,
            nullptr, nullptr, nullptr, nullptr, 1), PQclear};
        REQUIRE(result != nullptr);
        INFO(PQresultErrorMessage(result.get()));
        REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
        return std::string{PQgetvalue(result.get(), 0, 0),
            static_cast<std::size_t>(PQgetlength(result.get(), 0, 0))};
    };
    const auto reject = [&](const std::string& bytes) {
        auto result = receive(bytes);
        REQUIRE(result != nullptr);
        INFO(PQresultErrorMessage(result.get()));
        REQUIRE(PQresultStatus(result.get()) == PGRES_FATAL_ERROR);
        const auto* state = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
        REQUIRE(state != nullptr);
        // PostgreSQL's message-framing guard may report a protocol violation.
        REQUIRE((std::string_view{state} == "22P03" || std::string_view{state} == "08P01"));
        REQUIRE(query_scalar_int(ctx, "SELECT 1") == 1);
    };
    struct alphabet_case { const char* name; unsigned code, bits, cardinality; char low, high; int minimum; };
    for (const auto alphabet : {
            alphabet_case{"phred42", 7, 6, 42, '!', 'J', 0},
            alphabet_case{"phred63", 8, 6, 63, '!', '_', 0},
            alphabet_case{"phred68-legacy", 9, 7, 68, ';', '~', -5}}) {
        INFO(alphabet.name);
        for (const std::string text : {std::string{}, std::string{alphabet.low},
                std::string{alphabet.high}, std::string(20003, alphabet.low)}) {
            const auto bytes = send(fmt::format("'{}'::bioseq.seq('{}')", text, alphabet.name));
            auto result = receive(bytes);
            REQUIRE(result != nullptr);
            INFO(PQresultErrorMessage(result.get()));
            REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
            REQUIRE(std::string_view{PQgetvalue(result.get(), 0, 0)} == text);
            const auto total = text.empty() ? 0LL
                : static_cast<long long>(text.size()) * (text.front() == alphabet.high
                    ? static_cast<int>(alphabet.cardinality) - 1 + alphabet.minimum : alphabet.minimum);
            REQUIRE(std::string_view{PQgetvalue(result.get(), 0, 1)} == std::to_string(total));
        }
        const auto valid = send(fmt::format("'{}'::bioseq.seq('{}')", alphabet.low, alphabet.name));
        REQUIRE(valid.size() == 20); // 12-byte framing followed by one uint64 word.
        REQUIRE(static_cast<unsigned char>(valid[0]) == alphabet.code);
        REQUIRE(static_cast<unsigned char>(valid[1]) == 3);
        const auto with_word = [&](std::uint64_t word) {
            auto bytes = valid;
            for (unsigned byte = 0; byte < 8; ++byte)
                bytes[12 + byte] = static_cast<char>(word >> ((7 - byte) * 8));
            return bytes;
        };
        reject(with_word(alphabet.cardinality)); // Invalid active rank.
        reject(with_word(std::uint64_t{1} << alphabet.bits)); // Nonzero unused slot.
        reject(with_word(std::uint64_t{1} << 63)); // Permanent high padding bit.
        auto broken = valid;
        broken[3] = static_cast<char>(64 / alphabet.bits); // Impossible padding count.
        reject(broken);
        broken = valid; broken[0] = static_cast<char>(250); reject(broken);
        broken = valid; broken[1] = 4; reject(broken);
        broken = valid; broken.pop_back(); reject(broken);
        broken = valid; broken.push_back('\0'); reject(broken);
    }
}
