module;
#include "common.h"
#include <cctype>
#include <cstdlib>
#include <libpq-fe.h>
#include <sqlgen/postgres.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module integration_tests.common;

export import testing;
export import mlogging;

namespace bioseq_int_tests {

export using row_t = std::vector<std::optional<std::string>>;
export using rowset_t = std::vector<row_t>;

// POC `count_symbols()` returns 49 for rank-0 letters (A on dna4, A on dna5)
// and 0 for everything else. Exposed here so every test references the same
// magic value; replace this constant when the real counter lands.
export constexpr long long kSymbolCountPocFor_A = 49;
export constexpr long long kSymbolCountPocOther = 0;

// Wraps a single libpq PGconn (via sqlgen's PostgresV2Connection) for raw SQL
// against the bioseq Postgres extension. Tests build their state from raw SQL
// because typed sqlgen reads cannot describe `bioseq.seq('dna4', 32)` typmods.
export class connection_test_context {
private:
    sqlgen::postgres::Credentials _credentials;
    sqlgen::postgres::PostgresV2Connection _raw_conn;

    static std::string env_or(const char* name, std::string_view fallback) {
        const char* value = std::getenv(name);
        return value && *value ? std::string(value) : std::string(fallback);
    }

    // Environment overrides keep CI / multi-developer setups from forcing a
    // single Postgres instance; the suite still defaults to the DSN baked
    // into setup.sql so a developer with a fresh checkout can just run.
    static sqlgen::postgres::Credentials make_default_credentials() {
        const auto user_str = env_or("BIOSEQ_TEST_PGUSER", "postgres");
        const auto pass_str = env_or("BIOSEQ_TEST_PGPASSWORD", "postgres");
        const auto host_str = env_or("BIOSEQ_TEST_PGHOST", "localhost");
        const auto db_str = env_or("BIOSEQ_TEST_PGDATABASE", "bioseq");
        const auto port_str = env_or("BIOSEQ_TEST_PGPORT", "5432");
        return sqlgen::postgres::Credentials{
            .user = user_str,
            .password = pass_str,
            .host = host_str,
            .dbname = db_str,
            .port = std::stoi(port_str),
        };
    }

    static sqlgen::postgres::PostgresV2Connection connect_raw(
        const sqlgen::postgres::Credentials& credentials) {
        auto raw = sqlgen::postgres::PostgresV2Connection::make(
            credentials.to_str());
        if (!raw) {
            throw std::runtime_error(fmt::format(
                "Failed to connect to Postgres at {}:{} (db={}): {}",
                credentials.host, credentials.port, credentials.dbname,
                raw.error().what()));
        }
        return raw.value();
    }

public:
    connection_test_context()
        : connection_test_context(make_default_credentials()) {}

    explicit connection_test_context(sqlgen::postgres::Credentials credentials)
        : _credentials(std::move(credentials)),
          _raw_conn(connect_raw(_credentials)) {}

    [[nodiscard, always_inline]] const sqlgen::postgres::PostgresV2Connection&
    raw() const {
        return _raw_conn;
    }
};

// Issues a SQL statement that does not return rows. Aborts the test on failure
// so each call site can stay free of error-handling boilerplate.
export inline void exec_sql(const connection_test_context& ctx,
                            std::string_view sql) {
    const auto result = sqlgen::postgres::PostgresV2Result::make(
        std::string(sql), ctx.raw());
    if (!result) {
        ASSERT(false,
               fmt::format("exec_sql failed for `{}`: {}", sql,
                           result.error().what()),
               sql);
    }
}

// Returns true iff the given SQL fails server-side. Used by negative-path
// tests that expect Postgres to raise on invalid typmods, mismatched casts
// etc. Does not propagate any error message — callers usually only care that
// the statement raised at all.
export [[nodiscard]] inline bool sql_fails(
    const connection_test_context& ctx, std::string_view sql) {
    const auto result = sqlgen::postgres::PostgresV2Result::make(
        std::string(sql), ctx.raw());
    return !result;
}

// Same as `exec_sql`, but accepts a printf-style format string so callers can
// inline parameters without manual `fmt::format` plumbing.
export template <typename... Args>
inline void exec_sqlf(const connection_test_context& ctx,
                      fmt::format_string<Args...> fmtstr, Args&&... args) {
    exec_sql(ctx, fmt::format(fmtstr, std::forward<Args>(args)...));
}

// Runs a SELECT and returns the entire row set in textual form. Each NULL value
// becomes std::nullopt; everything else is the libpq text representation. Tests
// can then post-process via `parse_int_cell` etc.
export [[nodiscard]] inline rowset_t query_rows(
    const connection_test_context& ctx, std::string_view sql) {
    auto pgres =
        sqlgen::postgres::PostgresV2Result::make(std::string(sql), ctx.raw());
    if (!pgres) {
        ASSERT(false,
               fmt::format("query_rows failed for `{}`: {}", sql,
                           pgres.error().what()),
               sql);
    }
    PGresult* res = pgres.value().ptr();
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
export [[nodiscard]] inline long long query_scalar_int(
    const connection_test_context& ctx, std::string_view sql) {
    return std::stoll(query_scalar(ctx, sql));
}

// Convenience: collects a single text column out of a multi-row SELECT.
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
// of the textual plan. `Analyze` is a template parameter so the SQL string
// itself is a compile-time choice (matches CODE_GUIDELINES §1 "templates over
// runtime conditionals").
export template <bool Analyze = true>
[[nodiscard]] inline std::vector<std::string> explain_text(
    const connection_test_context& ctx, std::string_view sql) {
    std::string explain_sql;
    if constexpr (Analyze) {
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
            (void)sqlgen::postgres::PostgresV2Result::make(
                fmt::format("RESET {};", _name), _ctx->raw());
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
export inline void reset_extension(const connection_test_context& ctx,
                                   std::span<const std::string_view> tables) {
    for (const auto& table : tables) {
        exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", table);
    }
    exec_sql(ctx, "DROP EXTENSION IF EXISTS bioseq CASCADE;");
    exec_sql(ctx, "CREATE EXTENSION bioseq;");
    // GUC reset: a previous test may have left bioseq.enable_vectorization
    // off etc.; the explicit names cover everything reachable from the
    // option matrix tests. RESET ALL would be cheaper but resets system GUCs
    // we may not own (e.g. work_mem) — be specific to our own.
    static constexpr std::array<std::string_view, 5> kGucsToReset{
        "bioseq.enable_vectorization",
        "bioseq.enable_column_cache",
        "bioseq.enable_bioseq_index_scan",
        "bioseq.aligner.enable_profiling",
        "bioseq.update_index_on_insert",
    };
    for (const auto guc : kGucsToReset) {
        // Best-effort: GUCs not yet set by anyone are silently fine.
        (void)sqlgen::postgres::PostgresV2Result::make(
            fmt::format("RESET {};", guc), ctx.raw());
    }
    // Stale sidecar rows from dropped relids leak — the index/meta tables
    // live in the extension and survive DROP TABLE CASCADE on the user's
    // table. Truncate so per-test row counts start at 0. metadata_age_log
    // is in the same boat: a row written by the previous test under a
    // since-recycled OID would skew bioseq.list_metadata() age values.
    exec_sql(ctx,
             "TRUNCATE bioseq_internal.index_sidecar, "
             "        bioseq_internal.meta_sidecar, "
             "        bioseq_internal.metadata_age_log;");
}

// Same as the span overload, but accepts an initializer list so callers can
// write `reset_extension(ctx, {"hoe", "x"});` directly.
export inline void reset_extension(
    const connection_test_context& ctx,
    std::initializer_list<std::string_view> tables) {
    reset_extension(ctx, std::span<const std::string_view>(tables));
}

// Builds + connects + cleans state in one call. Returns a fresh fixture
// owned by the caller. Promoted from per-file `fresh_context()` helpers.
export [[nodiscard]] inline connection_test_context fresh_context(
    std::initializer_list<std::string_view> tables = {}) {
    connection_test_context ctx;
    reset_extension(ctx, tables);
    return ctx;
}

// Counts rows in the named bioseq_internal sidecar table for a given user
// relation. Used by sidecar-population tests.
export [[nodiscard]] inline long long count_sidecar(
    const connection_test_context& ctx, std::string_view sidecar_name,
    std::string_view user_table) {
    return std::stoll(query_scalar(
        ctx,
        fmt::format("SELECT COUNT(*)::bigint "
                    "  FROM bioseq_internal.{} "
                    " WHERE user_relid = '{}'::regclass;",
                    sidecar_name, user_table)));
}

// Encodes the supported (alphabet, width) typmod values.
export struct seq_typmod {
    std::string_view alphabet;  // "dna4" or "dna5"
    int width;                  // 8 / 16 / 32 / 64

    [[nodiscard]] std::string sql() const {
        return fmt::format("bioseq.seq('{}', {})", alphabet, width);
    }
};

// Builds the SQL literal `'<payload>'::bioseq.seq('<alphabet>', <width>)`
// so test sites stop reaching for ad-hoc fmt::format with the typmod text
// embedded inline.
export [[nodiscard]] inline std::string format_literal(
    std::string_view payload, const seq_typmod& typmod) {
    return fmt::format("'{}'::{}", payload, typmod.sql());
}

// CIGAR strings are sequences of (digits, op-letter) pairs; ops are drawn
// from MIDNSHP=X. Anything else is malformed. The bioseq custom scan
// returns an empty string when no alignment was attempted; callers can
// branch on `value.empty()` separately.
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
        constexpr std::string_view kCigarOps = "MIDNSHP=X";
        if (kCigarOps.find(op) == std::string_view::npos) {
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
export struct seq_table_spec {
    std::string name;
    seq_typmod typmod;
    int seq_columns = 1;       // number of bioseq.seq columns
    bool using_bioseq = false; // when true, CREATE TABLE ... USING bioseq;
};

export struct seq_row {
    int pk;
    std::vector<std::string> sequences;  // one entry per seq column
};

// Drops (CASCADE), creates and populates a bioseq.seq-bearing table according
// to `spec`. Sequence columns are named seq0, seq1, ... so tests can refer to
// them by index without pre-declaring schemas.
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
export inline void create_seq_table(
    const connection_test_context& ctx, const seq_table_spec& spec,
    std::initializer_list<seq_row> rows) {
    std::vector<seq_row> owned(rows.begin(), rows.end());
    create_seq_table(ctx, spec, std::span<const seq_row>(owned));
}

// The canonical 5-row `USING bioseq` fixture lifted from lel.sql / setup.sql.
// `seq0` is the long target sequence; `seq1` is a shorter probe used as the
// mmsearch query argument. Centralised here so multiple test files can pin
// the same input data.
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
export inline seq_typmod seed_hoe_fixture(
    const connection_test_context& ctx, std::string_view name) {
    constexpr seq_typmod typmod{.alphabet = "dna4", .width = 32};
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = on;");
    exec_sqlf(ctx, "SET bioseq.enable_vectorization = on;");
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
// argument. Matches lel.sql and setup.sql Test 13.
export constexpr std::string_view kHoeProbeSequence =
    "CTAGTCACTGTAGCCTGATCGTAGCTAGT";

// SQL literal of the canonical probe sequence at the dna4/32 typmod used by
// the hoe fixture. Saves every mmsearch test from re-typing the typmod.
export [[nodiscard]] inline std::string hoe_probe_literal() {
    return format_literal(kHoeProbeSequence,
                          seq_typmod{.alphabet = "dna4", .width = 32});
}

// Seeds a small dna4/32 columnar table with three deterministic 16-bp rows.
// Used by tests that need a `USING bioseq` table whose meta_sidecar is
// guaranteed to be populated but whose mmsearch result set is unconstrained.
export inline void seed_small_columnar(
    const connection_test_context& ctx, std::string_view name) {
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = on;");
    exec_sqlf(ctx, "SET bioseq.enable_vectorization = on;");
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
//   - "vectorization": only bioseq.enable_vectorization
//   - "column_cache": only bioseq.enable_column_cache
//   - "vectorization+column_cache": both
//
// The default flips the full vectorization+column_cache matrix, since these
// are the two GUCs whose code paths are most likely to diverge.
export [[nodiscard]] inline rowset_t expect_query_invariant_under_options(
    const connection_test_context& ctx, std::string_view sql,
    std::span<const std::pair<std::string_view, std::vector<std::string_view>>>
        guc_matrix) {
    // Build the cartesian product of all (guc, value) pairs.
    std::vector<size_t> idx(guc_matrix.size(), 0);

    auto describe_combination = [&]() {
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
export inline rowset_t expect_invariant_under_guc_toggle(
    const connection_test_context& ctx, std::string_view sql,
    std::string_view guc) {
    return expect_query_invariant_under_options(
        ctx, sql, {{guc, {"on", "off"}}});
}

}  // namespace bioseq_int_tests
