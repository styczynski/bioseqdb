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

/** @brief Text cells returned for one libpq result row. */
export using row_t = std::vector<std::optional<std::string>>;
/** @brief Ordered rows returned by one libpq query. */
export using rowset_t = std::vector<row_t>;

namespace detail {

/** @brief Owns one libpq result until the current helper scope ends. */
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

/** @return An owned libpq result for one SQL statement. */
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

/** @return Integration credentials resolved from the test environment. */
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

/** @return A connected libpq session or throws with the connection diagnostic. */
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

// Symbol-count fixtures use the real per-rank tally and assert exact goldens
// directly (for example, 'ACGTACGTACGTACGT' yields A=C=G=T=4).

// Wraps a single libpq PGconn for raw SQL against the bioseq Postgres extension.
// Tests build their state from raw SQL because typed SQL wrappers cannot
// describe `bioseq.seq('dna4', 32)` typmods.
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
                // A libpq notice callback must never unwind through C. Tests
                // still retain the server-side copy in the PostgreSQL log if
                // this diagnostic-only allocation is exhausted.
            }
        }
        // Preserve libpq's previous test-runner behaviour: notices remain
        // visible in the builder log while also becoming assertable.
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

// Issues a SQL statement that does not return rows. Aborts the test on failure
// so each call site can stay free of error-handling boilerplate.
/** @brief Executes a statement and fails the current test on server error. */
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

// Returns true iff the given SQL fails server-side. Used by negative-path
// tests that expect Postgres to raise on invalid typmods, mismatched casts
// etc. Does not propagate any error message — callers usually only care that
// the statement raised at all.
/** @return True when the server rejects the supplied statement. */
export [[nodiscard]] inline bool sql_fails(
    const connection_test_context& ctx, std::string_view sql) {
    auto result = detail::exec(ctx.raw(), sql);
    return result.ptr == nullptr ||
           !detail::command_success(PQresultStatus(result.ptr));
}

// Server-side error captured for SQLSTATE-sensitive negative tests. Unlike
// `sql_fails` (bool only), this returns the SQLSTATE and server
// diagnostic and ASSERTS if the statement unexpectedly SUCCEEDED — so a
// silently-accepted bad statement can never pass as "errored as expected".
/** @brief Captures SQLSTATE and server diagnostics for one expected failure. */
export struct sql_error_info {
    std::string sqlstate;
    std::string message;
    [[nodiscard]] bool contains(std::string_view needle) const {
        return message.find(needle) != std::string::npos;
    }
};

/** @return The SQLSTATE and diagnostics from a statement that must fail. */
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

// Same as `exec_sql`, but accepts a printf-style format string so callers can
// inline parameters without manual `fmt::format` plumbing.
/** @brief Formats and executes a statement that must succeed. */
export template <typename... p_args>
inline void exec_sqlf(const connection_test_context& ctx,
                      fmt::format_string<p_args...> fmtstr, p_args&&... args) {
    exec_sql(ctx, fmt::format(fmtstr, std::forward<p_args>(args)...));
}

// Runs a SELECT and returns the entire row set in textual form. Each NULL value
// becomes std::nullopt; everything else is the libpq text representation. Tests
// can then post-process via `parse_int_cell` etc.
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

// Convenience: returns the single row of a one-row query. Asserts otherwise.
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

// Returns the single row of a one-row query, or nullopt for an empty result
// set. Useful when callers want to express "if any row, then ..." without
// hand-writing the Catch2 matchers.
/** @return The only query row or null when the result is empty. */
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

// Convenience: returns the single (row, column) cell of a one-row, one-column
// query as a string. Asserts otherwise (NULLs included).
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

// Same as `query_scalar`, but parses the cell as a long long. Useful for COUNT,
// symbol_count, len, etc.
/** @return The signed integer value from a scalar query. */
export [[nodiscard]] inline long long query_scalar_int(
    const connection_test_context& ctx, std::string_view sql) {
    return std::stoll(query_scalar(ctx, sql));
}

// Convenience: collects a single text column out of a multi-row SELECT.
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

// Runs a SELECT under EXPLAIN with full instrumentation and returns the lines
// of the textual plan. `p_analyze` is a template parameter so the SQL string
// itself is a compile-time choice (matches CODE_GUIDELINES §1 "templates over
// runtime conditionals").
/** @return Text lines from an instrumented or plan-only EXPLAIN. */
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

// Pulls the value of an EXPLAIN property line of the form
// "  Foo Bar: <value>" out of a textual plan. `key` is the literal text to
// the left of the colon; the returned string is the trimmed remainder. Used
// instead of plain substring grep to avoid false positives.
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
        // Trim leading / trailing whitespace.
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

// Runs `EXPLAIN (FORMAT JSON, ANALYZE, VERBOSE, COSTS, BUFFERS) <sql>` and
// returns the JSON string verbatim. Postgres returns the JSON in a single text
// row which we concatenate so callers can pattern-match easily.
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

// True when ANY of the textual EXPLAIN lines contains `needle`. Used for
// brittle-but-readable plan assertions.
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

// RAII GUC scope: SET on construction, RESET on destruction. Lets each test
// flip a single setting without polluting the connection's session state.
/** @brief Sets one session GUC and resets it when the scope ends. */
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
            // Best-effort reset; do not throw from the destructor.
            detail::pg_result reset_result(
                PQexec(_ctx->raw(), fmt::format("RESET {};", _name).c_str()));
        }
    }
};

// Drops the named tables (CASCADE), recreates the bioseq extension, RESETs
// every bioseq.* GUC, and trims residual sidecar rows so the next test starts
// from a baseline regardless of what ran before.
//
// We DROP+CREATE the extension on every reset so a developer who edited
// bioseq--1.0.sql since the previous test run picks up the new definitions
// (otherwise CREATE EXTENSION IF NOT EXISTS is a no-op against the old
// pg_proc rows). The schemas it owns — bioseq, bioseq_internal — are
// rebuilt by the install script and contain only fixture state at the
// boundary of each test, so the cascade is safe.
/** @brief Recreates the extension and removes the supplied fixture tables. */
export inline void reset_extension(const connection_test_context& ctx,
                                   std::span<const std::string_view> tables) {
    for (const auto& table : tables) {
        exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", table);
    }
    // The reset order is fixed: DROP EXTENSION, then DROP the two schemas
    // CASCADE so every lazily-created seqidx_*/seqidxtab_* table and
    // catalog row is gone, then CREATE EXTENSION re-creates the catalogs empty.
    // No trailing TRUNCATE / drop_all_index_tables() reaper is needed.
    exec_sql(ctx, "DROP EXTENSION IF EXISTS bioseq CASCADE;");
    exec_sql(ctx, "DROP SCHEMA IF EXISTS bioseq_internal CASCADE;");
    exec_sql(ctx, "DROP SCHEMA IF EXISTS bioseq CASCADE;");
    // CREATE EXTENSION loads the C library and runs _PG_init. If this backend
    // already touched bioseq before the reset, reloading the library in the same
    // process can trip PostgreSQL's "attempt to redefine parameter" guard for
    // custom GUCs. Do the create in a fresh backend; the extension objects are
    // database-global and immediately visible to the caller's connection.
    {
        connection_test_context create_ctx;
        exec_sql(create_ctx, "CREATE EXTENSION bioseq;");
    }
    // Reset every Bioseq setting registered in this backend. Querying the
    // catalog keeps the fixture complete when the generated GUC profile grows
    // and avoids resetting unrelated PostgreSQL settings.
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

// Same as the span overload, but accepts an initializer list so callers can
// write `reset_extension(ctx, {"hoe", "x"});` directly.
/** @brief Recreates the extension using an initializer-list table set. */
export inline void reset_extension(
    const connection_test_context& ctx,
    std::initializer_list<std::string_view> tables) {
    reset_extension(ctx, std::span<const std::string_view>(tables));
}

// Builds + connects + cleans state in one call. Returns a fresh fixture
// owned by the caller. Promoted from per-file `fresh_context()` helpers.
/** @return A connected integration session with freshly recreated extension state. */
export [[nodiscard]] inline connection_test_context fresh_context(
    std::initializer_list<std::string_view> tables = {}) {
    connection_test_context ctx;
    reset_extension(ctx, tables);
    return ctx;
}

// Libpq wrappers over the current admin/test SQL surface expose shard-backed
// row and sub-key counts without depending on implementation tables.

// Total chunk rows of one index across its derived seqidx_* shards.
/** @return The total stored row count for one logical sequence index. */
export [[nodiscard]] inline long long index_table_row_count(
    const connection_test_context& ctx, std::string_view index_name,
    std::string_view user_table) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.index_table_row_count('{}'::regclass, '{}')",
        user_table, index_name));
}

// Distinct sub_key count for ONE query_key across an index's shards (the
// per-sequence "is this row fully indexed" check).
/** @return The number of distinct stored subkeys for one indexed sequence row. */
export [[nodiscard]] inline long long index_distinct_sub_keys(
    const connection_test_context& ctx, std::string_view index_name,
    std::string_view user_table, long long query_key) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.index_distinct_sub_keys('{}'::regclass, '{}', {})",
        user_table, index_name, query_key));
}

// Resolve a row's BIOSEQ_COLUMN rowno (== index query_key) from its ctid.
/** @return The Bioseq column row number resolved from a heap tuple identifier. */
export [[nodiscard]] inline long long column_row_number_for_ctid(
    const connection_test_context& ctx, std::string_view user_table,
    std::string_view ctid_literal) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, '{}'::tid)",
        user_table, ctid_literal));
}

// Backend-local spilled-run count of the most recent minimizers_per_sequence build.
/** @return The spilled-run count from the latest minimizer-sequence build. */
export [[nodiscard]] inline long long last_index_merge_run_count(
    const connection_test_context& ctx) {
    return query_scalar_int(ctx, "SELECT bioseq.last_index_merge_run_count()");
}

// Encodes the supported (alphabet, width) typmod values.
/** @brief Describes one supported sequence alphabet and storage width. */
export struct seq_typmod {
    std::string_view alphabet;  // "dna4" or "dna5"
    int width;                  // 8 / 16 / 32 / 64

    [[nodiscard]] std::string sql() const {
        return fmt::format("bioseq.seq('{}', {})", alphabet, width);
    }
};

// The integration-test target compiles outside the Bioseq library tree
// (libpq-driven, no PostgreSQL-extension linkage). The
// `dna4_entry` / `dna5_entry` tag brands mirror the production
// `pgseq_supported_alphabets::entry_for<seq::dnaX>` shape but only
// carry the parts the test fixtures consume — the display name string
// view and a tag type discriminating dna4 from dna5 at compile time.
// A `static_assert` in `sequences.cppm` pins these strings against the
// production pack so any drift fails at build time, preserving the
// "pack is the single source of truth" promise indirectly (drift
// detection lives in the production TU).
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

// Build a seq_typmod from a templated alphabet entry so the test fixture pulls
// the name string from the entry, never from a
// parallel literal in the test file.
/** @return A sequence typmod built from an alphabet entry and storage width. */
export template <class p_entry>
[[nodiscard]] inline constexpr seq_typmod make_seq_typmod(int width) noexcept {
    return seq_typmod{
        .alphabet = p_entry::alphabet_name_sv,
        .width    = width,
    };
}

// Builds the SQL literal `'<payload>'::bioseq.seq('<alphabet>', <width>)`
// so test sites stop reaching for ad-hoc fmt::format with the typmod text
// embedded inline.
/** @return A typed SQL sequence literal for the supplied payload. */
export [[nodiscard]] inline std::string format_literal(
    std::string_view payload, const seq_typmod& typmod) {
    return fmt::format("'{}'::{}", payload, typmod.sql());
}

// CIGAR strings are sequences of (digits, op-letter) pairs; ops are drawn
// from MIDNSHP=X. Anything else is malformed. The bioseq custom scan
// returns an empty string when no alignment was attempted; callers can
// branch on `value.empty()` separately.
/** @return True when a nonempty value is a syntactically valid CIGAR string. */
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

// Exact-match CIGAR check: for a fully-matching alignment of length N
// the CIGAR is canonically "{N}M" (one match run of length N). The
// aligner emits this shape both for self-alignment (mmsearch(A, A))
// and for an exact substring match (mmsearch(target, query) where
// query is a contiguous substring of target). This helper is used by
// the substring-property tests to check that the aligner returned
// the property "match length == query length" — see §9 of
// REPORT_POSTGRES_QUERY_BUGFIXING_1.md for the rationale.
/** @return True when the CIGAR is one exact match of the expected length. */
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

// Builds a `CREATE TABLE` clause and inserts the rows verbatim. Each sequence
// literal is emitted as `'<text>'::bioseq.seq('<alphabet>', <width>)` so the
// extension performs the encoding via seq_in. When `using_bioseq=true` the
// table is created with `USING bioseq` (engages the columnar custom scan).
//
// Rows are passed as `vector<pair<int_pk, vector<string>>>`: one entry per
// sequence column. Callers stay free of the column-naming detail because the
// helper synthesises column names `seq0`, `seq1`, ... around the primary key
// (`pk INT PRIMARY KEY`).
/** @brief Describes one sequence fixture table. */
export struct seq_table_spec {
    std::string name;
    seq_typmod typmod;
    int seq_columns = 1;       // number of bioseq.seq columns
    bool using_bioseq = false; // when true, CREATE TABLE ... USING bioseq;
};

/** @brief Stores one primary key and its sequence fixture values. */
export struct seq_row {
    int pk;
    std::vector<std::string> sequences;  // one entry per seq column
};

// Drops (CASCADE), creates and populates a bioseq.seq-bearing table according
// to `spec`. Sequence columns are named seq0, seq1, ... so tests can refer to
// them by index without pre-declaring schemas.
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

    // Build a single multi-row INSERT for clarity; INSERTs of bioseq.seq
    // varlena values cannot use libpq prepared parameters because the type's
    // recv path requires the typmod, which prepared params don't carry.
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

// The canonical five-row `USING bioseq` fixture.
// `seq0` is the long target sequence; `seq1` is a shorter probe used as the
// mmsearch query argument. Centralised here so multiple test files can pin
// the same input data.
/** @return The canonical five-row alignment fixture. */
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

// Drops + creates the named hoe fixture, populates with `hoe_seed_rows()`,
// engages the columnar AM. Returns the typmod so call sites can build their
// query literals via `format_literal`.
/** @return The typmod used after recreating the canonical alignment fixture. */
export inline seq_typmod seed_hoe_fixture(
    const connection_test_context& ctx, std::string_view name) {
    constexpr seq_typmod typmod{.alphabet = "dna4", .width = 32};
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    exec_sqlf(ctx, "SET bioseq.vectorization.enabled = on;");
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

// The probe sequence used throughout the suite as the mmsearch query
// argument and the current minimizer-index fixture contract.
/** @brief Canonical query sequence used by the alignment fixture. */
export constexpr std::string_view k_hoe_probe_sequence =
    "CTAGTCACTGTAGCCTGATCGTAGCTAGT";

// SQL literal of the canonical probe sequence at the dna4/32 typmod used by
// the hoe fixture. Saves every mmsearch test from re-typing the typmod.
/** @return The canonical probe as a DNA4 32-bit SQL literal. */
export [[nodiscard]] inline std::string hoe_probe_literal() {
    return format_literal(k_hoe_probe_sequence,
                          seq_typmod{.alphabet = "dna4", .width = 32});
}

// Seeds a small dna4/32 columnar table with three deterministic 16-bp rows.
// Used by tests that need a `USING bioseq` table whose meta_sidecar is
// guaranteed to be populated but whose mmsearch result set is unconstrained.
/** @brief Recreates a small deterministic DNA4 columnar fixture. */
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

// Comparable list of integer values returned by a one-column SELECT. Builds
// from query_single_column and parses each cell.
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

// Runs the same query under every (vectorization=on/off, column_cache=on/off)
// pair and asserts the row sets match. Returns the rows from the first run so
// callers can build further assertions on the canonical output.
//
// `kind` controls the GUC families exercised:
//   - "vectorization": only bioseq.vectorization.enabled
//   - "column_cache": only bioseq.column_cache.enabled
//   - "vectorization+column_cache": both
//
// The default flips the full vectorization+column_cache matrix, since these
// are the two GUCs whose code paths are most likely to diverge.
/** @return Canonical rows after checking every supplied GUC combination. */
export [[nodiscard]] inline rowset_t expect_query_invariant_under_options(
    const connection_test_context& ctx, std::string_view sql,
    std::span<const std::pair<std::string_view, std::vector<std::string_view>>>
        guc_matrix) {
    // Build the cartesian product of all (guc, value) pairs.
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

        // Advance the cartesian-product odometer.
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

// Convenience wrapper around the span overload accepting initializer lists.
/** @return Canonical rows after checking literal GUC combinations. */
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

// Common-case shorthand: assert that the query returns the same rows whether
// the named GUC is "on" or "off". Returns the canonical row set.
/** @return Canonical rows after checking both values of one Boolean GUC. */
export inline rowset_t expect_invariant_under_guc_toggle(
    const connection_test_context& ctx, std::string_view sql,
    std::string_view guc) {
    return expect_query_invariant_under_options(
        ctx, sql, {{guc, {"on", "off"}}});
}

}  // namespace bioseq_int_tests
