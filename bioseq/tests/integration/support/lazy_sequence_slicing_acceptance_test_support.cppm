/**
 * @file
 * @brief Provides sequence-slicing acceptance fixtures and lifetime guards.
 */
module;
#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>

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

export module bioseq.tests.integration.lazy_sequence_slicing_acceptance_test_support;

import bioseq.tests.integration.support;

export namespace bioseq_int_tests::lazy_sequence_slicing_acceptance_test_support {

/// Best-effort cleanup for a database created by a raw libpq acceptance case.
class database_cleanup_guard {
public:
    /// Bind cleanup to an existing connection and DROP statement.
    database_cleanup_guard(PGconn* connection, const char* drop_sql) noexcept
        : connection_(connection), drop_sql_(drop_sql) {}

    database_cleanup_guard(const database_cleanup_guard&) = delete;
    database_cleanup_guard& operator=(const database_cleanup_guard&) = delete;

    /// Execute the armed DROP statement during exceptional scope exit.
    ~database_cleanup_guard() noexcept {
        if (!active_ || connection_ == nullptr || drop_sql_ == nullptr) {
            return;
        }
        if (PGresult* result = PQexec(connection_, drop_sql_)) {
            PQclear(result);
        }
    }

    /// Disarm cleanup after the owning case removes the database explicitly.
    void release() noexcept { active_ = false; }

private:
    PGconn* connection_ = nullptr;
    const char* drop_sql_ = nullptr;
    bool active_ = true;
};

/// Capture the status, SQLSTATE, and optional value of a prepared letter call.
struct prepared_letter_result {
    ExecStatusType status = PGRES_FATAL_ERROR;
    std::string sqlstate;
    std::optional<std::string> value;
};

/// Execute one prepared letter parameter with caller-selected wire format.
[[nodiscard]] prepared_letter_result execute_prepared_letter_parameter(
    PGconn* connection,
    const char* name,
    const char* sql,
    Oid letter_oid,
    const char* value,
    int length,
    int format) {
    PGresult* prepared = PQprepare(
        connection, name, sql, 1, &letter_oid);
    REQUIRE(prepared != nullptr);
    REQUIRE(PQresultStatus(prepared) == PGRES_COMMAND_OK);
    PQclear(prepared);

    const std::array values{value};
    const std::array lengths{length};
    const std::array formats{format};
    PGresult* executed = PQexecPrepared(
        connection,
        name,
        1,
        values.data(),
        lengths.data(),
        formats.data(),
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

/// Rewrite the three-byte payload in a single-row binary COPY fixture.
void write_letter_binary_copy_variant(
    const std::string& source_path,
    const std::string& destination_path,
    std::uint8_t wire_revision,
    std::uint8_t alphabet_code,
    std::uint8_t rank) {
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

    bytes[25] = wire_revision;
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

/// Repeat the supplied alphabet ranks to an exact sequence length.
[[nodiscard]] std::string sequence_acceptance_rank_pattern(
    const std::string& alphabet_symbols,
    std::size_t length) {
    REQUIRE_FALSE(alphabet_symbols.empty());
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        result.push_back(alphabet_symbols[index % alphabet_symbols.size()]);
    }
    return result;
}

/// Verify lazy concatenation packing for every boundary in one profile.
void require_sequence_acceptance_sql_packing_profile(
    connection_test_context& ctx,
    const char* alphabet,
    std::uint32_t storage_width_bits,
    std::uint32_t bits_per_symbol,
    const char* alphabet_symbols) {
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
        const auto whole = sequence_acceptance_rank_pattern(alphabet_symbols, length);
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

    exec_sql(ctx, "TRUNCATE sequence_acceptance_packing_flatten;");
    exec_sql(
        ctx,
        cte
            + "INSERT INTO sequence_acceptance_packing_flatten "
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
        "FROM sequence_acceptance_packing_flatten;");
    REQUIRE(flattened.size() == 4u);
    REQUIRE(flattened[0] == std::to_string(expected_case_count));
    REQUIRE(flattened[1] == "t");
    REQUIRE(flattened[2] == "t");
    REQUIRE(flattened[3] == "t");
}

/// Build the deterministic high-entropy motif used by binding tests.
[[nodiscard]] std::string sequence_acceptance_match_binding_motif() {
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

}  // namespace bioseq_int_tests::lazy_sequence_slicing_acceptance_test_support
