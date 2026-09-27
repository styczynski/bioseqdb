/**
 * @file index.cppm
 * @brief Provides libpq sessions, SQL assertions, and fixtures for integration tests.
 */
module;
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <libassert/assert.hpp>
#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.tests.integration.support;

namespace bioseq_int_tests {

/** @brief One query row, with SQL NULL represented by std::nullopt. */
export using row_t = std::vector<std::optional<std::string>>;
/** @brief Query rows in the order returned by PostgreSQL. */
export using rowset_t = std::vector<row_t>;

namespace detail {

/** @brief Owns a libpq result and clears it on destruction. */
struct pg_result {
    PGresult* ptr = nullptr;

    explicit pg_result(PGresult* value) noexcept : ptr(value) {}
    pg_result(const pg_result&) = delete;
    pg_result& operator=(const pg_result&) = delete;
    pg_result(pg_result&& other) noexcept : ptr(std::exchange(other.ptr, nullptr)) {}
    pg_result& operator=(pg_result&& other) noexcept {
        if (this != &other) {
            if (ptr != nullptr) {
                PQclear(ptr);
            }
            ptr = std::exchange(other.ptr, nullptr);
        }
        return *this;
    }
    ~pg_result() {
        if (ptr != nullptr) {
            PQclear(ptr);
        }
    }
};

/** @brief Stores connection parameters resolved for an integration session. */
struct pg_credentials {
    std::string user;
    std::string password;
    std::string host;
    std::string dbname;
    std::string port;
};

/** @return True when libpq reports a successful command or tuple result. */
inline bool command_success(ExecStatusType status) noexcept {
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK ||
           status == PGRES_SINGLE_TUPLE;
}

/** @return An owned result from executing the supplied SQL. */
inline pg_result exec(PGconn* conn, std::string_view sql) {
    return pg_result(PQexec(conn, std::string(sql).c_str()));
}

/** @return The current connection diagnostic or an empty string. */
inline std::string connection_error(PGconn* conn) {
    const char* message = conn == nullptr ? nullptr : PQerrorMessage(conn);
    return message == nullptr ? std::string{} : std::string(message);
}

/** @return A nonempty environment value or the supplied fallback. */
inline std::string env_or(const char* name, std::string_view fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : std::string(fallback);
}

/** @return Connection settings from BIOSEQ_TEST_PG* variables, with local test defaults. */
inline pg_credentials make_default_credentials() {
    const auto user_str = env_or("BIOSEQ_TEST_PGUSER", "postgres");
    const auto pass_str = env_or("BIOSEQ_TEST_PGPASSWORD", "postgres");
    const auto host_str = env_or("BIOSEQ_TEST_PGHOST", "localhost");
    const auto db_str = env_or("BIOSEQ_TEST_PGDATABASE", "bioseq");
    const auto port_str = env_or("BIOSEQ_TEST_PGPORT", "5432");
    return pg_credentials{
        .user = user_str,
        .password = pass_str,
        .host = host_str,
        .dbname = db_str,
        .port = port_str,
    };
}

/** Open a libpq connection, or throw with its diagnostic if connection fails. */
inline PGconn* connect_raw(const pg_credentials& credentials) {
    const char* keywords[] = {
        "user", "password", "host", "dbname", "port", nullptr,
    };
    const char* values[] = {
        credentials.user.c_str(),
        credentials.password.c_str(),
        credentials.host.c_str(),
        credentials.dbname.c_str(),
        credentials.port.c_str(),
        nullptr,
    };
    PGconn* raw = PQconnectdbParams(keywords, values, 0);
    if (raw == nullptr || PQstatus(raw) != CONNECTION_OK) {
        const std::string message = connection_error(raw);
        if (raw != nullptr) {
            PQfinish(raw);
        }
        throw std::runtime_error(fmt::format(
            "Failed to connect to Postgres at {}:{} (db={}): {}",
            credentials.host, credentials.port, credentials.dbname, message));
    }
    return raw;
}

}  // namespace detail

// Symbol-count tests compare exact base counts, such as four of each base
// in ACGTACGTACGTACGT.

// SQL fixtures spell out sequence type parameters such as bioseq.seq('dna4', 32).
/** @brief Owns one libpq session and captures its asynchronous notices. */
export class connection_test_context {
private:
    PGconn* _raw_conn = nullptr;
    std::vector<std::string> _notices;

    static void capture_notice(void* arg, const char* message) noexcept {
        auto* self = static_cast<connection_test_context*>(arg);
        if (self != nullptr) {
            try {
                self->_notices.emplace_back(message == nullptr ? "" : message);
            } catch (...) {
                // Keep allocation failures inside this C callback. The message is still
                // written to stderr below.
            }
        }
        // Also write notices to stderr so failed tests keep their diagnostics.
        if (message != nullptr) {
            std::fputs(message, stderr);
        }
    }

    void install_notice_capture() noexcept {
        if (_raw_conn != nullptr) {
            (void)PQsetNoticeProcessor(
                _raw_conn, &connection_test_context::capture_notice, this);
        }
    }

public:
    connection_test_context()
        : _raw_conn(detail::connect_raw(detail::make_default_credentials())) {
        install_notice_capture();
    }

    explicit connection_test_context(std::string_view database_name) {
        auto credentials = detail::make_default_credentials();
        credentials.dbname = std::string(database_name);
        _raw_conn = detail::connect_raw(credentials);
        install_notice_capture();
    }

    connection_test_context(const connection_test_context&) = delete;
    connection_test_context& operator=(const connection_test_context&) = delete;

    connection_test_context(connection_test_context&& other) noexcept
        : _raw_conn(std::exchange(other._raw_conn, nullptr)),
          _notices(std::move(other._notices)) {
        install_notice_capture();
    }

    connection_test_context& operator=(connection_test_context&& other) noexcept {
        if (this != &other) {
            if (_raw_conn != nullptr) {
                PQfinish(_raw_conn);
            }
            _raw_conn = std::exchange(other._raw_conn, nullptr);
            _notices = std::move(other._notices);
            install_notice_capture();
        }
        return *this;
    }

    ~connection_test_context() {
        if (_raw_conn != nullptr) {
            PQfinish(_raw_conn);
        }
    }

    [[nodiscard, always_inline]] PGconn* raw() const noexcept {
        return _raw_conn;
    }

    void clear_notices() noexcept {
        _notices.clear();
    }

    [[nodiscard]] bool notice_contains(std::string_view needle) const noexcept {
        return std::any_of(
            _notices.begin(), _notices.end(),
            [needle](const std::string& notice) {
                return notice.find(needle) != std::string::npos;
            });
    }
};


/** @brief Run SQL and fail the test if execution does not succeed. */
export inline void exec_sql(const connection_test_context& ctx,
                            std::string_view sql) {
    auto result = detail::exec(ctx.raw(), sql);
    if (result.ptr == nullptr ||
        !detail::command_success(PQresultStatus(result.ptr))) {
        ASSERT(false,
               fmt::format("exec_sql failed for `{}`: {}", sql,
                           result.ptr == nullptr
                               ? detail::connection_error(ctx.raw())
                               : PQresultErrorMessage(result.ptr)),
               sql);
    }
}

// Use this for tests that only need to know whether SQL execution failed.
/** @return True when SQL execution fails or libpq returns no result. */
export [[nodiscard]] inline bool sql_fails(
    const connection_test_context& ctx, std::string_view sql) {
    auto result = detail::exec(ctx.raw(), sql);
    return result.ptr == nullptr ||
           !detail::command_success(PQresultStatus(result.ptr));
}


/** @brief SQLSTATE and message captured from a failed statement. */
export struct sql_error_info {
    std::string sqlstate;
    std::string message;
    [[nodiscard]] bool contains(std::string_view needle) const {
        return message.find(needle) != std::string::npos;
    }
};

/** Run SQL that must fail and return its SQLSTATE and diagnostics. */
export [[nodiscard]] inline sql_error_info sql_error(
    const connection_test_context& ctx, std::string_view sql) {
    const std::string sql_owned(sql);
    auto result = detail::pg_result(PQexec(ctx.raw(), sql_owned.c_str()));
    if (result.ptr == nullptr) {
        return sql_error_info{
            "",
            fmt::format("PQexec returned null: {}",
                        detail::connection_error(ctx.raw()))};
    }
    PGresult* res = result.ptr;
    const ExecStatusType status = PQresultStatus(res);
    if (detail::command_success(status)) {
        ASSERT(false,
               fmt::format("sql_error: statement unexpectedly SUCCEEDED "
                           "(expected a server error): `{}`", sql_owned),
               sql_owned);
    }
    const char* state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    const char* primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
    const char* detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
    const char* hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
    const char* full = PQresultErrorMessage(res);
    std::string message;
    if (primary != nullptr && primary[0] != '\0') {
        message += primary;
    }
    if (detail != nullptr && detail[0] != '\0') {
        if (!message.empty()) {
            message += '\n';
        }
        message += "DETAIL: ";
        message += detail;
    }
    if (hint != nullptr && hint[0] != '\0') {
        if (!message.empty()) {
            message += '\n';
        }
        message += "HINT: ";
        message += hint;
    }
    if (message.empty() && full != nullptr) {
        message = full;
    }
    return sql_error_info{
        state == nullptr ? std::string{} : std::string(state),
        std::move(message)};
}

// Use fmt placeholders to insert fixture values into SQL.
/** @brief Formats and executes a statement that must succeed. */
export template <typename... p_args>
inline void exec_sqlf(const connection_test_context& ctx,
                      fmt::format_string<p_args...> fmtstr, p_args&&... args) {
    exec_sql(ctx, fmt::format(fmtstr, std::forward<p_args>(args)...));
}

// SQL NULL becomes std::nullopt. Other cells keep libpq's text representation.
/** @return Every query row as optional libpq text cells. */
export [[nodiscard]] inline rowset_t query_rows(
    const connection_test_context& ctx, std::string_view sql) {
    auto pgres = detail::exec(ctx.raw(), sql);
    if (pgres.ptr == nullptr || PQresultStatus(pgres.ptr) != PGRES_TUPLES_OK) {
        ASSERT(false,
               fmt::format("query_rows failed for `{}`: {}", sql,
                           pgres.ptr == nullptr
                               ? detail::connection_error(ctx.raw())
                               : PQresultErrorMessage(pgres.ptr)),
               sql);
    }
    PGresult* res = pgres.ptr;
    const int n_rows = PQntuples(res);
    const int n_cols = PQnfields(res);
    rowset_t out;
    out.reserve(static_cast<size_t>(n_rows));
    for (int row = 0; row < n_rows; ++row) {
        row_t cells;
        cells.reserve(static_cast<size_t>(n_cols));
        for (int col = 0; col < n_cols; ++col) {
            if (PQgetisnull(res, row, col)) {
                cells.emplace_back(std::nullopt);
            } else {
                cells.emplace_back(std::string(PQgetvalue(res, row, col)));
            }
        }
        out.push_back(std::move(cells));
    }
    return out;
}


/** @return The only row from a query that must return exactly one row. */
export [[nodiscard]] inline row_t query_one(
    const connection_test_context& ctx, std::string_view sql) {
    auto rows = query_rows(ctx, sql);
    ASSERT(rows.size() == 1,
           fmt::format("query_one expected exactly 1 row from `{}`, got {}",
                       sql, rows.size()),
           sql, rows.size());
    return std::move(rows.front());
}

// More than one row fails the test.
/** @return The only query row, or std::nullopt when there are no rows. */
export [[nodiscard]] inline std::optional<row_t> query_one_or_none(
    const connection_test_context& ctx, std::string_view sql) {
    auto rows = query_rows(ctx, sql);
    if (rows.empty()) {
        return std::nullopt;
    }
    ASSERT(rows.size() == 1,
           fmt::format("query_one_or_none expected at most 1 row from `{}`, "
                       "got {}",
                       sql, rows.size()),
           sql, rows.size());
    return std::move(rows.front());
}

// Wrong row or column counts and NULL values fail the test.
/** @return The non-null text value from a one-row one-column query. */
export [[nodiscard]] inline std::string query_scalar(
    const connection_test_context& ctx, std::string_view sql) {
    auto row = query_one(ctx, sql);
    ASSERT(row.size() == 1,
           fmt::format("query_scalar expected exactly 1 column from `{}`, got "
                       "{}",
                       sql, row.size()),
           sql, row.size());
    ASSERT(row.front().has_value(),
           fmt::format("query_scalar got NULL value for `{}`", sql),
           sql);
    return *row.front();
}

// Parse the cell as long long, for counts and sequence lengths.
/** @return The signed integer value from a scalar query. */
export [[nodiscard]] inline long long query_scalar_int(
    const connection_test_context& ctx, std::string_view sql) {
    return std::stoll(query_scalar(ctx, sql));
}

// Require one column in every returned row.
/** @return One optional text cell for every query row. */
export [[nodiscard]] inline std::vector<std::optional<std::string>>
query_single_column(const connection_test_context& ctx,
                    std::string_view sql) {
    auto rows = query_rows(ctx, sql);
    std::vector<std::optional<std::string>> out;
    out.reserve(rows.size());
    for (auto& row : rows) {
        ASSERT(row.size() == 1,
               fmt::format("query_single_column expected one column from `{}`,"
                           " got {}",
                           sql, row.size()),
               sql, row.size());
        out.push_back(std::move(row.front()));
    }
    return out;
}

// Set p_analyze=false to inspect the plan without running the query.
/** @return EXPLAIN text, including execution statistics when p_analyze is true. */
export template <bool p_analyze = true>
[[nodiscard]] inline std::vector<std::string> explain_text(
    const connection_test_context& ctx, std::string_view sql) {
    std::string explain_sql;
    if constexpr (p_analyze) {
        explain_sql = fmt::format(
            "EXPLAIN (ANALYZE, VERBOSE, COSTS, BUFFERS) {}", sql);
    } else {
        explain_sql = fmt::format("EXPLAIN (VERBOSE, COSTS off) {}", sql);
    }
    auto rows = query_rows(ctx, explain_sql);
    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (auto& row : rows) {
        ASSERT(row.size() == 1 && row.front().has_value(),
               fmt::format("explain_text: unexpected EXPLAIN row shape for "
                           "`{}`",
                           sql),
               sql);
        lines.push_back(std::move(*row.front()));
    }
    return lines;
}

// For a line containing "Foo Bar: value", return value with outer whitespace
// removed. The first matching property wins.
/** @return A trimmed EXPLAIN property value when the key is present. */
export [[nodiscard]] inline std::optional<std::string> find_explain_property(
    std::span<const std::string> lines, std::string_view key) {
    const std::string needle = std::string(key) + ":";
    for (const auto& line : lines) {
        const auto pos = line.find(needle);
        if (pos == std::string::npos) {
            continue;
        }
        std::string value = line.substr(pos + needle.size());
        // Remove surrounding whitespace from the property value.
        size_t lo = 0;
        while (lo < value.size() && std::isspace(
                                        static_cast<unsigned char>(value[lo]))) {
            ++lo;
        }
        size_t hi = value.size();
        while (hi > lo && std::isspace(
                              static_cast<unsigned char>(value[hi - 1]))) {
            --hi;
        }
        return value.substr(lo, hi - lo);
    }
    return std::nullopt;
}

// Execute EXPLAIN ANALYZE with JSON output, verbose details, costs, and buffers.
/** @return The JSON EXPLAIN document for an instrumented query. */
export [[nodiscard]] inline std::string explain_json(
    const connection_test_context& ctx, std::string_view sql) {
    const std::string explain_sql = fmt::format(
        "EXPLAIN (FORMAT JSON, ANALYZE, VERBOSE, COSTS, BUFFERS) {}", sql);
    auto rows = query_rows(ctx, explain_sql);
    std::string out;
    for (auto& row : rows) {
        if (row.size() == 1 && row.front()) {
            out += *row.front();
            out += '\n';
        }
    }
    return out;
}


/** @return True when any textual EXPLAIN line contains the requested text. */
export [[nodiscard]] inline bool plan_contains(
    std::span<const std::string> lines, std::string_view needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// RESET returns the setting to its session default when this scope ends.
/** @brief Set one PostgreSQL setting and RESET it on destruction. */
export class guc_scope {
private:
    const connection_test_context* _ctx;
    std::string _name;
    bool _active = true;

public:
    guc_scope(const connection_test_context& ctx, std::string_view name,
              std::string_view value)
        : _ctx(&ctx), _name(name) {
        exec_sqlf(*_ctx, "SET {} TO {};", _name, value);
    }

    guc_scope(const guc_scope&) = delete;
    guc_scope& operator=(const guc_scope&) = delete;
    guc_scope(guc_scope&& other) noexcept
        : _ctx(other._ctx),
          _name(std::move(other._name)),
          _active(other._active) {
        other._active = false;
    }
    guc_scope& operator=(guc_scope&&) = delete;

    ~guc_scope() {
        if (_active && _ctx != nullptr) {
            // Ignore a failed RESET during cleanup.
            detail::pg_result reset_result(
                PQexec(_ctx->raw(), fmt::format("RESET {};", _name).c_str()));
        }
    }
};

// Reinstall the extension for each fixture so tests use the current SQL
// definitions and start with empty Bioseq schemas. Reset visible, mutable
// bioseq.* settings on the caller's connection as well.
/** @brief Recreates the extension and removes the supplied fixture tables. */
export inline void reset_extension(const connection_test_context& ctx,
                                   std::span<const std::string_view> tables) {
    for (const auto& table : tables) {
        exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", table);
    }
    // Drop the schemas as well as the extension to remove index tables created
    // during earlier tests. CREATE EXTENSION then rebuilds the catalogs.
    exec_sql(ctx, "DROP EXTENSION IF EXISTS bioseq CASCADE;");
    exec_sql(ctx, "DROP SCHEMA IF EXISTS bioseq_internal CASCADE;");
    exec_sql(ctx, "DROP SCHEMA IF EXISTS bioseq CASCADE;");
    // Install through a fresh connection. Loading the library again in a backend
    // that already used Bioseq can attempt to register its settings twice.
    // The installed objects are then available to the caller's connection.
    {
        connection_test_context create_ctx;
        exec_sql(create_ctx, "CREATE EXTENSION bioseq;");
    }
    // Reset the visible Bioseq settings that this session can change. Reading
    // pg_settings includes new public settings as they are added.
    const auto bioseq_gucs = query_single_column(
        ctx,
        "SELECT quote_ident(name) FROM pg_settings "
        "WHERE name LIKE 'bioseq.%' "
        "AND context IN ('user','superuser') ORDER BY name;");
    for (const auto& guc : bioseq_gucs) {
        if (!guc.has_value())
            continue;
        detail::pg_result reset_result(
            PQexec(
                ctx.raw(),
                fmt::format("RESET {};", *guc).c_str()));
    }
}


/** @brief Recreates the extension using an initializer-list table set. */
export inline void reset_extension(
    const connection_test_context& ctx,
    std::initializer_list<std::string_view> tables) {
    reset_extension(ctx, std::span<const std::string_view>(tables));
}

// Open a connection and clear the requested fixture tables and extension state.
/** @return A connected integration session with freshly recreated extension state. */
export [[nodiscard]] inline connection_test_context fresh_context(
    std::initializer_list<std::string_view> tables = {}) {
    connection_test_context ctx;
    reset_extension(ctx, tables);
    return ctx;
}

// Read index counts through the SQL functions used by the tests.

// Total chunk rows of one index across its derived seqidx_* shards.
/** @return The total stored row count for one logical sequence index. */
export [[nodiscard]] inline long long index_table_row_count(
    const connection_test_context& ctx, std::string_view index_name,
    std::string_view user_table) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.index_table_row_count('{}'::regclass, '{}')",
        user_table, index_name));
}

// Count distinct subkeys for one sequence row across the index's shards.
/** @return The number of distinct stored subkeys for one indexed sequence row. */
export [[nodiscard]] inline long long index_distinct_sub_keys(
    const connection_test_context& ctx, std::string_view index_name,
    std::string_view user_table, long long query_key) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.index_distinct_sub_keys('{}'::regclass, '{}', {})",
        user_table, index_name, query_key));
}

// Convert ctid to the column row number used as the index query_key.
/** @return The Bioseq column row number for a tuple identifier. */
export [[nodiscard]] inline long long column_row_number_for_ctid(
    const connection_test_context& ctx, std::string_view user_table,
    std::string_view ctid_literal) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, '{}'::tid)",
        user_table, ctid_literal));
}

// Backend-local sorted-run count from the latest index build.
/** @return The latest index build's sorted-run count, including runs kept in memory. */
export [[nodiscard]] inline long long last_index_merge_run_count(
    const connection_test_context& ctx) {
    return query_scalar_int(ctx, "SELECT bioseq.last_index_merge_run_count()");
}

// Alphabet name and packed storage width used in SQL sequence types.
/** @brief Sequence type parameters used by fixtures. */
export struct seq_typmod {
    std::string_view alphabet;  // "dna4" or "dna5"
    int width;                  // 8 / 16 / 32 / 64

    [[nodiscard]] std::string sql() const {
        return fmt::format("bioseq.seq('{}', {})", alphabet, width);
    }
};

// These libpq tests do not link the extension. Keep lightweight alphabet
// tags here so fixture templates can select DNA4 or DNA5 and obtain its SQL name.
/** @brief Tag types distinguishing supported fixture alphabets. */
export namespace seq_entry_tag {
    struct dna4 {};
    struct dna5 {};
}

/** @brief Fixture entry describing the DNA4 alphabet. */
export struct dna4_entry {
    using alphabet_type = seq_entry_tag::dna4;
    static constexpr std::string_view alphabet_name_sv = "dna4";
};

/** @brief Fixture entry describing the DNA5 alphabet. */
export struct dna5_entry {
    using alphabet_type = seq_entry_tag::dna5;
    static constexpr std::string_view alphabet_name_sv = "dna5";
};

// Take the SQL alphabet name from the fixture entry.
/** @return A sequence typmod built from an alphabet entry and storage width. */
export template <class p_entry>
[[nodiscard]] inline constexpr seq_typmod make_seq_typmod(int width) noexcept {
    return seq_typmod{
        .alphabet = p_entry::alphabet_name_sv,
        .width    = width,
    };
}

// Add an explicit sequence type to the fixture literal. Payloads must already
// be safe to place inside SQL quotes.
/** @return A typed SQL sequence literal for the supplied payload. */
export [[nodiscard]] inline std::string format_literal(
    std::string_view payload, const seq_typmod& typmod) {
    return fmt::format("'{}'::{}", payload, typmod.sql());
}

// Accept digit/operator pairs using the CIGAR operators MIDNSHP=X.
// Empty strings are rejected, and run lengths are not checked.
/** @return True when a nonempty string has CIGAR digit/operator syntax. */
export [[nodiscard]] inline bool is_valid_cigar(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    size_t i = 0;
    while (i < value.size()) {
        size_t digits_start = i;
        while (i < value.size() &&
               std::isdigit(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        if (i == digits_start) {
            return false;
        }
        if (i >= value.size()) {
            return false;
        }
        const char op = value[i];
        constexpr std::string_view k_cigar_operations = "MIDNSHP=X";
        if (k_cigar_operations.find(op) == std::string_view::npos) {
            return false;
        }
        ++i;
    }
    return true;
}

// Substring and self-alignment tests expect one M run covering the query.
// This helper checks that CIGAR shape and length.
/** @return True for one M run of the expected positive length. */
export [[nodiscard]] inline bool is_exact_match_cigar(std::string_view value,
                                                      long long expected_len) {
    if (value.empty() || expected_len <= 0) {
        return false;
    }
    // Parse leading integer.
    size_t pos = 0;
    long long parsed = 0;
    while (pos < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[pos]))) {
        parsed = parsed * 10 + (value[pos] - '0');
        ++pos;
    }
    if (pos == 0) {
        return false;
    }
    // Must be followed by exactly one 'M' and end of string.
    if (pos + 1 != value.size()) {
        return false;
    }
    if (value[pos] != 'M') {
        return false;
    }
    return parsed == expected_len;
}

// Sequence fixture tables have an integer primary key and columns named
// seq0, seq1, and so on. Each inserted sequence has an explicit SQL type cast.
/** @brief Describes one sequence fixture table. */
export struct seq_table_spec {
    std::string name;
    seq_typmod typmod;
    int seq_columns = 1;       // number of bioseq.seq columns
    bool using_bioseq = false; // Use the Bioseq table access method.
};

/** @brief Stores one primary key and its sequence fixture values. */
export struct seq_row {
    int pk;
    std::vector<std::string> sequences;  // one entry per seq column
};

// Drop and recreate the table, then insert rows into seq0, seq1, and so on.
/** @brief Recreates and populates a sequence fixture table. */
export inline void create_seq_table(
    const connection_test_context& ctx, const seq_table_spec& spec,
    std::span<const seq_row> rows = {}) {
    exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", spec.name);

    std::string ddl = fmt::format("CREATE TABLE {} (\n  pk INT PRIMARY KEY",
                                  spec.name);
    for (int i = 0; i < spec.seq_columns; ++i) {
        ddl += fmt::format(",\n  seq{} {}", i, spec.typmod.sql());
    }
    ddl += "\n)";
    if (spec.using_bioseq) {
        ddl += " USING bioseq";
    }
    ddl += ";";
    exec_sql(ctx, ddl);

    if (rows.empty()) {
        return;
    }

    // Use typed text literals so PostgreSQL receives the alphabet and width
    // when encoding each sequence.
    std::string insert =
        fmt::format("INSERT INTO {} VALUES\n  ", spec.name);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        ASSERT(row.sequences.size() ==
                   static_cast<size_t>(spec.seq_columns),
               fmt::format("create_seq_table: row {} has {} sequences but "
                           "table {} expects {}",
                           i, row.sequences.size(), spec.name,
                           spec.seq_columns),
               row.sequences.size(), spec.seq_columns);

        if (i > 0) {
            insert += ",\n  ";
        }
        insert += fmt::format("({}", row.pk);
        for (const auto& seq : row.sequences) {
            insert += fmt::format(", '{}'::{}", seq, spec.typmod.sql());
        }
        insert += ")";
    }
    insert += ";";
    exec_sql(ctx, insert);
}

// Initializer-list overload so call sites can write the rows literally.
/** @brief Recreates a sequence fixture table from literal rows. */
export inline void create_seq_table(
    const connection_test_context& ctx, const seq_table_spec& spec,
    std::initializer_list<seq_row> rows) {
    std::vector<seq_row> owned(rows.begin(), rows.end());
    create_seq_table(ctx, spec, std::span<const seq_row>(owned));
}

// Shared five-row alignment data. seq0 holds targets and seq1 holds shorter
// sequences used as query operands in tests.
/** @return The five rows used by the shared alignment fixture. */
export inline std::vector<seq_row> hoe_seed_rows() {
    return {
        seq_row{
            .pk = 1,
            .sequences = {
                "TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA",
                "CTAGTCACTGTAGCCTGATCGTAGCTAGT"}},
        seq_row{
            .pk = 2,
            .sequences = {
                "CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA",
                "TCGTAGCTAGGA"}},
        seq_row{
            .pk = 3,
            .sequences = {
                "GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA",
                "TCATCATCTACTCCTA"}},
        seq_row{
            .pk = 4,
            .sequences = {
                "TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT",
                "TGTAGCCTGATCGTA"}},
        seq_row{
            .pk = 5,
            .sequences = {
                "ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA",
                "CACTGTAGCCTGATCGTA"}},
    };
}

// Create the alignment fixture with the Bioseq table access method. Return
// its type parameters for formatting query literals.
/** @return The sequence type parameters used for the recreated alignment fixture. */
export inline seq_typmod seed_hoe_fixture(
    const connection_test_context& ctx, std::string_view name) {
    constexpr seq_typmod typmod{.alphabet = "dna4", .width = 32};
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sqlf(ctx, "SET bioseq.vectorization.enabled = on;");
    // A short probe may have only one seed. Allow that chain by lowering the
    // minimum score and seed count used by this fixture.
    exec_sqlf(ctx,
        "SET bioseq.aligner.chaining.minimum_score = 10;"
        "SET bioseq.aligner.chaining.minimum_seed_count = 1;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(name),
            .typmod = typmod,
            .seq_columns = 2,
            .using_bioseq = true,
        },
        hoe_seed_rows());
    return typmod;
}

// Shared mmsearch probe for alignment and minimizer-index tests.
/** @brief Query sequence used by the shared alignment fixture. */
export constexpr std::string_view k_hoe_probe_sequence =
    "CTAGTCACTGTAGCCTGATCGTAGCTAGT";

// Format the probe with the DNA4/32 type used by its target table.
/** @return The shared probe as a DNA4 32-bit SQL literal. */
export [[nodiscard]] inline std::string hoe_probe_literal() {
    return format_literal(k_hoe_probe_sequence,
                          seq_typmod{.alphabet = "dna4", .width = 32});
}

// Populate a Bioseq table and its indexes with three fixed 16 bp sequences.
// Tests use this fixture when they need stored data without a specific match set.
/** @brief Recreate a small DNA4 fixture using the Bioseq table access method. */
export inline void seed_small_bioseq_column(
    const connection_test_context& ctx, std::string_view name) {
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sqlf(ctx, "SET bioseq.vectorization.enabled = on;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(name),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {"ACGTACGTACGTACGT"}},
            seq_row{.pk = 2, .sequences = {"TTTTAAAAGGGGCCCC"}},
            seq_row{.pk = 3, .sequences = {"AAAAAAAAAAAAAAAA"}},
        });
}

// Require non-NULL cells and parse the single result column as integers.
/** @return One parsed integer for every row of a single-column query. */
export [[nodiscard]] inline std::vector<long long> query_int_column(
    const connection_test_context& ctx, std::string_view sql) {
    auto col = query_single_column(ctx, sql);
    std::vector<long long> out;
    out.reserve(col.size());
    for (auto& cell : col) {
        ASSERT(cell.has_value(),
               fmt::format("query_int_column: NULL value for `{}`", sql), sql);
        out.push_back(std::stoll(*cell));
    }
    return out;
}

// Run the query for every combination of the supplied setting values and
// compare each result with the first. Each setting needs at least one value.
// Settings are RESET after each run.
/** @return The first result after checking all supplied setting combinations. */
export [[nodiscard]] inline rowset_t expect_query_invariant_under_options(
    const connection_test_context& ctx, std::string_view sql,
    std::span<const std::pair<std::string_view, std::vector<std::string_view>>>
        guc_matrix) {
    // Track the selected value for each setting.
    std::vector<size_t> idx(guc_matrix.size(), 0);

    auto describe_combination = [&guc_matrix, &idx]() {
        std::string description;
        for (size_t i = 0; i < guc_matrix.size(); ++i) {
            if (i > 0) description += ", ";
            description += fmt::format("{}={}", guc_matrix[i].first,
                                       guc_matrix[i].second[idx[i]]);
        }
        return description;
    };

    rowset_t reference;
    bool first_run = true;

    while (true) {
        // Apply the current option combination.
        std::vector<guc_scope> scopes;
        scopes.reserve(guc_matrix.size());
        for (size_t i = 0; i < guc_matrix.size(); ++i) {
            scopes.emplace_back(ctx, guc_matrix[i].first,
                                guc_matrix[i].second[idx[i]]);
        }

        const auto rows = query_rows(ctx, sql);

        if (first_run) {
            reference = rows;
            first_run = false;
        } else {
            ASSERT(rows == reference,
                   fmt::format("expect_query_invariant_under_options: "
                               "results differ for combination {}; sql=`{}`",
                               describe_combination(), sql),
                   sql, describe_combination());
        }

        // Move to the next combination, carrying into the previous setting as needed.
        size_t carry = guc_matrix.size();
        while (carry > 0) {
            const size_t i = carry - 1;
            ++idx[i];
            if (idx[i] < guc_matrix[i].second.size()) {
                break;
            }
            idx[i] = 0;
            --carry;
        }
        if (carry == 0) {
            break;
        }
    }

    return reference;
}


/** @return The first result after checking the supplied setting combinations. */
export inline rowset_t expect_query_invariant_under_options(
    const connection_test_context& ctx, std::string_view sql,
    std::initializer_list<
        std::pair<std::string_view, std::vector<std::string_view>>>
        guc_matrix) {
    std::vector<std::pair<std::string_view, std::vector<std::string_view>>>
        owned(guc_matrix.begin(), guc_matrix.end());
    return expect_query_invariant_under_options(
        ctx, sql,
        std::span<const std::pair<std::string_view,
                                  std::vector<std::string_view>>>(owned));
}

// Run with the setting on and off, requiring identical rows.
/** @return The rows from the run with this setting on. */
export inline rowset_t expect_invariant_under_guc_toggle(
    const connection_test_context& ctx, std::string_view sql,
    std::string_view guc) {
    return expect_query_invariant_under_options(
        ctx, sql, {{guc, {"on", "off"}}});
}

}  // namespace bioseq_int_tests
