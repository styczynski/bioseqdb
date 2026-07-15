module;

#include <libpq-fe.h>

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.libpq.client;

// Source-neutral RAII/query/notice/error primitives over libpq. This is the
// only module in the tree permitted to include libpq headers (§7); the website
// PostgreSQL policy and the integration tests both consume it without ever
// seeing a raw PGconn/PGresult. It exposes no OIDs or connection strings to the
// rest of website generation beyond the typed helpers below.
//
// Every connection and every command runs through the same nonblocking
// PQconnectStartParams/PQconnectPoll and PQsendQuery/PQflush/PQconsumeInput/
// PQgetResult state machine (§13.5): a fixed connection deadline, chunked row
// retrieval (PQsetChunkedRowsMode), bounded PQcancel + drain on a client
// deadline or a caller-imposed result limit, rejection of the COPY sub-protocols
// no tutorial may enter, and a PQTRANS_IDLE requirement after every phase. No
// blocking PQconnectdb/PQexec shortcut exists anywhere in this module.
export namespace bioseq::libpq {

/// @brief A libpq error carrying the failing SQLSTATE and structured diagnostics.
///
/// what() is the primary server message (or a client-side description); the
/// SQLSTATE, detail, hint, and severity fields carry the remaining §13.5 fields
/// so callers can render or redact them without re-parsing libpq output.
class query_error : public std::runtime_error {
public:
    query_error(std::string message, std::string sqlstate, std::string detail = {},
                std::string hint = {}, std::string severity = {})
        : std::runtime_error(std::move(message)),
          sqlstate_(std::move(sqlstate)),
          detail_(std::move(detail)),
          hint_(std::move(hint)),
          severity_(std::move(severity)) {}

    /// @brief Five-character SQLSTATE, or empty for a client-side failure.
    [[nodiscard]] std::string_view sqlstate() const noexcept { return sqlstate_; }
    [[nodiscard]] std::string_view detail() const noexcept { return detail_; }
    [[nodiscard]] std::string_view hint() const noexcept { return hint_; }
    [[nodiscard]] std::string_view severity() const noexcept { return severity_; }

private:
    std::string sqlstate_;
    std::string detail_;
    std::string hint_;
    std::string severity_;
};

/// @brief One captured server notice/warning during command execution.
struct notice {
    std::string severity;
    std::string sqlstate;
    std::string message;
};

/// @brief Caller-imposed retention bounds applied while chunked rows accumulate.
///
/// A zero field means "unlimited". The state machine retains rows up to and
/// including the first element that crosses a bound, then cancels the running
/// statement and stops accumulating, so the materialised result the caller
/// receives already violates the caller's own limit check (which owns the
/// author-facing message) while generator memory stays bounded (§13.4, §13.5).
struct execution_limits {
    std::size_t max_rows{};
    std::size_t max_columns{};
    std::size_t max_cell_bytes{};
    std::size_t max_total_bytes{};
};

/// @brief Owned, fully materialised result set with typed accessors.
///
/// Chunked retrieval means no single PGresult holds every row, so the state
/// machine copies columns and cells into owned storage here; the accessor
/// surface mirrors the libpq shape callers expect (rows/columns/value/is_null/
/// command tag) without exposing a PGresult.
class result {
public:
    struct column_meta {
        std::string name;
        unsigned int type_oid{};
    };
    struct cell {
        bool is_null{};
        std::string text;
    };

    result() = default;

    [[nodiscard]] int rows() const noexcept { return static_cast<int>(rows_.size()); }
    [[nodiscard]] int columns() const noexcept {
        return static_cast<int>(columns_.size());
    }

    [[nodiscard]] std::string column_name(int column) const {
        return at_column(column).name;
    }
    [[nodiscard]] unsigned int column_type_oid(int column) const {
        return at_column(column).type_oid;
    }
    [[nodiscard]] bool is_null(int row, int column) const noexcept {
        return rows_[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)]
            .is_null;
    }
    [[nodiscard]] std::string_view value(int row, int column) const {
        return rows_[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)]
            .text;
    }

    /// @brief PostgreSQL command tag such as "SELECT 3" or "CREATE EXTENSION".
    [[nodiscard]] std::string command_tag() const { return command_tag_; }
    [[nodiscard]] ExecStatusType status() const noexcept { return status_; }
    [[nodiscard]] bool has_rows() const noexcept { return status_ == PGRES_TUPLES_OK; }
    [[nodiscard]] std::string error_sqlstate() const { return error_sqlstate_; }

    // Populated by connection::run_statement; public so the state machine (a
    // member of connection) can assemble it directly.
    ExecStatusType status_{PGRES_COMMAND_OK};
    std::vector<column_meta> columns_;
    std::vector<std::vector<cell>> rows_;
    std::string command_tag_;
    std::string error_sqlstate_;

private:
    [[nodiscard]] const column_meta& at_column(int column) const {
        return columns_[static_cast<std::size_t>(column)];
    }
};

/// @brief RAII PostgreSQL connection driven entirely by the nonblocking machine.
///
/// Constructed from an explicit Unix-socket conninfo so callers never build the
/// string outside their owning policy module; connection uses
/// PQconnectStartParams/PQconnectPoll with a fixed ten-second deadline and then
/// PQsetnonblocking(1). Notices are captured into an owned buffer the caller
/// drains after each command.
class connection {
public:
    explicit connection(const std::string& conninfo)
        : handle_(nonblocking_connect(conninfo, std::chrono::seconds(10))) {
        PQsetNoticeReceiver(handle_, &connection::receive_notice, this);
    }
    connection(connection&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)),
          notices_(std::move(other.notices_)) {
        if (handle_) {
            PQsetNoticeReceiver(handle_, &connection::receive_notice, this);
        }
    }
    connection& operator=(connection&&) = delete;
    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    ~connection() {
        if (handle_) {
            PQfinish(handle_);
        }
    }

    /// @brief Runs a command with the default ten-second client deadline.
    [[nodiscard]] result exec(std::string_view sql) {
        return run_statement(sql, std::chrono::seconds(10), execution_limits{});
    }

    /// @brief Runs a command with an explicit client deadline (e.g. the fence).
    [[nodiscard]] result exec(std::string_view sql,
                              std::chrono::milliseconds deadline) {
        return run_statement(sql, deadline, execution_limits{});
    }

    /// @brief Runs a command under both a client deadline and retention limits.
    [[nodiscard]] result exec_bounded(std::string_view sql,
                                      std::chrono::milliseconds deadline,
                                      const execution_limits& limits) {
        return run_statement(sql, deadline, limits);
    }

    /// @brief Drains and clears notices captured since the last drain.
    [[nodiscard]] std::vector<notice> take_notices() {
        return std::exchange(notices_, {});
    }

    /// @brief Quotes an SQL identifier via libpq — never string interpolation.
    [[nodiscard]] std::string escape_identifier(std::string_view name) const {
        char* escaped = PQescapeIdentifier(handle_, name.data(), name.size());
        if (escaped == nullptr) {
            throw query_error("PQescapeIdentifier failed: " + current_error_message(),
                              std::string{});
        }
        std::string result(escaped);
        PQfreemem(escaped);
        return result;
    }

    [[nodiscard]] std::string server_version() const {
        const int version = PQserverVersion(handle_);
        if (version == 0) {
            return {};
        }
        const int major = version / 10000;
        const int minor = version % 10000;
        return std::to_string(major) + "." + std::to_string(minor);
    }

private:
    using clock = std::chrono::steady_clock;

    // Waits until a raw socket is readable or writable, or the deadline passes.
    // Returns false on timeout or a poll error; slices the wait at one second so
    // a distant deadline is still re-checked promptly against steady_clock.
    static bool wait_fd(int fd, bool want_read, clock::time_point deadline) {
        if (fd < 0) {
            return false;
        }
        for (;;) {
            const auto now = clock::now();
            if (now >= deadline) {
                return false;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                    .count();
            const int slice = remaining > 1000 ? 1000 : static_cast<int>(remaining);
            pollfd descriptor{fd, static_cast<short>(want_read ? POLLIN : POLLOUT), 0};
            const int ready = ::poll(&descriptor, 1, slice);
            if (ready > 0) {
                return true;
            }
            if (ready < 0 && errno != EINTR) {
                return false;
            }
        }
    }

    static bool wait_socket(PGconn* conn, bool want_read, clock::time_point deadline) {
        return wait_fd(PQsocket(conn), want_read, deadline);
    }

    // Opens a connection with PQconnectStartParams/PQconnectPoll under a fixed
    // deadline, then switches the socket to nonblocking mode (§13.5). expand_dbname
    // lets the single conninfo string flow through the "dbname" keyword unchanged.
    static PGconn* nonblocking_connect(const std::string& conninfo,
                                       clock::duration budget) {
        const char* keywords[] = {"dbname", nullptr};
        const char* values[] = {conninfo.c_str(), nullptr};
        PGconn* conn = PQconnectStartParams(keywords, values, /*expand_dbname=*/1);
        if (conn == nullptr) {
            throw query_error("libpq: out of memory starting connection", std::string{});
        }
        if (PQstatus(conn) == CONNECTION_BAD) {
            std::string message = PQerrorMessage(conn);
            PQfinish(conn);
            throw query_error("connection failed: " + message, std::string{});
        }
        const auto deadline = clock::now() + budget;
        PostgresPollingStatusType polling = PGRES_POLLING_WRITING;
        for (;;) {
            const bool want_read = (polling == PGRES_POLLING_READING);
            if (!wait_socket(conn, want_read, deadline)) {
                PQfinish(conn);
                throw query_error("connection did not complete within 10s",
                                  std::string{});
            }
            polling = PQconnectPoll(conn);
            if (polling == PGRES_POLLING_OK) {
                break;
            }
            if (polling == PGRES_POLLING_FAILED) {
                std::string message = PQerrorMessage(conn);
                PQfinish(conn);
                throw query_error("connection failed: " + message, std::string{});
            }
        }
        if (PQstatus(conn) != CONNECTION_OK || PQsetnonblocking(conn, 1) != 0) {
            std::string message = PQerrorMessage(conn);
            PQfinish(conn);
            throw query_error("connection not usable: " + message, std::string{});
        }
        return conn;
    }

    [[nodiscard]] std::string current_error_message() const {
        const char* message = handle_ ? PQerrorMessage(handle_) : nullptr;
        return message ? std::string(message) : std::string{};
    }

    struct captured_error {
        std::string sqlstate;
        std::string primary;
        std::string detail;
        std::string hint;
        std::string severity;
    };

    static std::string field_or_empty(const PGresult* res, int code) {
        const char* value = PQresultErrorField(res, code);
        return value ? std::string(value) : std::string{};
    }

    static captured_error capture_error(const PGresult* res) {
        captured_error error;
        error.sqlstate = field_or_empty(res, PG_DIAG_SQLSTATE);
        error.primary = field_or_empty(res, PG_DIAG_MESSAGE_PRIMARY);
        error.detail = field_or_empty(res, PG_DIAG_MESSAGE_DETAIL);
        error.hint = field_or_empty(res, PG_DIAG_MESSAGE_HINT);
        error.severity = field_or_empty(res, PG_DIAG_SEVERITY_NONLOCALIZED);
        if (error.severity.empty()) {
            error.severity = field_or_empty(res, PG_DIAG_SEVERITY);
        }
        if (error.primary.empty()) {
            const char* whole = PQresultErrorMessage(res);
            if (whole) {
                error.primary = whole;
            }
        }
        return error;
    }

    // Copies a materialised PGresult's columns and, when requested, its rows into
    // `out`, honouring the retention limits: the boundary-crossing row/cell is
    // retained so the caller's own limit check reproduces its exact message, then
    // `limited` is set so the caller cancels and stops. total_bytes threads the
    // running payload total across a statement's chunks.
    static void absorb(result& out, const PGresult* res, bool copy_rows,
                       const execution_limits& limits, std::size_t& total_bytes,
                       bool& limited) {
        if (out.columns_.empty()) {
            const int ncols = PQnfields(res);
            out.columns_.reserve(static_cast<std::size_t>(ncols));
            for (int column = 0; column < ncols; ++column) {
                out.columns_.push_back(result::column_meta{
                    .name = PQfname(res, column) ? PQfname(res, column) : std::string{},
                    .type_oid = static_cast<unsigned int>(PQftype(res, column))});
            }
        }
        if (!copy_rows) {
            return;
        }
        const int ncols = PQnfields(res);
        const int nrows = PQntuples(res);
        for (int row = 0; row < nrows; ++row) {
            std::vector<result::cell> cells;
            cells.reserve(static_cast<std::size_t>(ncols));
            for (int column = 0; column < ncols; ++column) {
                const bool null_cell = PQgetisnull(res, row, column) != 0;
                const int length = PQgetlength(res, row, column);
                std::string text = null_cell
                    ? std::string{}
                    : std::string(PQgetvalue(res, row, column),
                                  static_cast<std::size_t>(length));
                total_bytes += text.size();
                if ((limits.max_cell_bytes != 0 && text.size() > limits.max_cell_bytes) ||
                    (limits.max_total_bytes != 0 && total_bytes > limits.max_total_bytes)) {
                    limited = true;
                }
                cells.push_back(result::cell{.is_null = null_cell, .text = std::move(text)});
            }
            out.rows_.push_back(std::move(cells));
            if (limits.max_rows != 0 && out.rows_.size() > limits.max_rows) {
                limited = true;
            }
            if (limited) {
                return;
            }
        }
    }

    // Bounded nonblocking cancellation using the PG17 PQcancelConn state machine.
    void cancel_bounded(clock::duration budget) noexcept {
        PGcancelConn* cancel = PQcancelCreate(handle_);
        if (cancel == nullptr) {
            return;
        }
        if (PQcancelStart(cancel) == 0) {
            PQcancelFinish(cancel);
            return;
        }
        const auto deadline = clock::now() + budget;
        PostgresPollingStatusType polling = PGRES_POLLING_WRITING;
        for (;;) {
            const bool want_read = (polling == PGRES_POLLING_READING);
            if (!wait_fd(PQcancelSocket(cancel), want_read, deadline)) {
                break;
            }
            polling = PQcancelPoll(cancel);
            if (polling == PGRES_POLLING_OK || polling == PGRES_POLLING_FAILED) {
                break;
            }
        }
        PQcancelFinish(cancel);
    }

    // Discards a COPY OUT/BOTH stream within a bounded window so an unsupported
    // COPY never turns into a caller-driven loop.
    void copy_discard_bounded(clock::duration budget) noexcept {
        const auto deadline = clock::now() + budget;
        for (;;) {
            if (clock::now() >= deadline) {
                return;
            }
            char* buffer = nullptr;
            const int copied = PQgetCopyData(handle_, &buffer, /*async=*/1);
            if (buffer != nullptr) {
                PQfreemem(buffer);
            }
            if (copied == -1 || copied == -2) {
                return;
            }
            if (copied == 0) {
                if (!wait_socket(handle_, /*want_read=*/true, deadline)) {
                    return;
                }
                if (PQconsumeInput(handle_) == 0) {
                    return;
                }
            }
        }
    }

    // Consumes and discards any remaining results within a bounded window so a
    // cancelled or errored statement leaves no dangling protocol state.
    void drain_results_bounded(clock::duration budget) noexcept {
        const auto deadline = clock::now() + budget;
        for (;;) {
            if (clock::now() >= deadline) {
                return;
            }
            if (PQconsumeInput(handle_) == 0) {
                return;
            }
            while (PQisBusy(handle_) == 0) {
                PGresult* res = PQgetResult(handle_);
                if (res == nullptr) {
                    return;
                }
                if (PQresultStatus(res) == PGRES_COPY_OUT ||
                    PQresultStatus(res) == PGRES_COPY_BOTH) {
                    PQclear(res);
                    copy_discard_bounded(deadline - clock::now());
                    continue;
                }
                PQclear(res);
            }
            if (!wait_socket(handle_, /*want_read=*/true, deadline)) {
                return;
            }
        }
    }

    // Best-effort bounded ROLLBACK to return a non-idle connection toward idle.
    void rollback_bounded(clock::duration budget) noexcept {
        if (PQsendQuery(handle_, "ROLLBACK") == 0) {
            return;
        }
        const auto deadline = clock::now() + budget;
        while (PQflush(handle_) == 1) {
            if (!wait_socket(handle_, /*want_read=*/false, deadline)) {
                return;
            }
        }
        drain_results_bounded(deadline - clock::now());
    }

    // The single §13.5 send/flush/consume state machine. Sends `sql`, enables
    // 32-row chunked retrieval, then polls the socket under `budget` while
    // consuming results — committing chunked rows only on their zero-row
    // PGRES_TUPLES_OK terminator, discarding a failed statement's provisional
    // rows, rejecting COPY, bounding accumulation against `limits`, and requiring
    // a PQTRANS_IDLE connection afterward.
    [[nodiscard]] result run_statement(std::string_view sql,
                                       clock::duration budget,
                                       const execution_limits& limits) {
        const auto deadline = clock::now() + budget;
        const std::string owned(sql);
        if (PQsendQuery(handle_, owned.c_str()) == 0) {
            throw query_error("failed to send command: " + current_error_message(),
                              std::string{});
        }
        if (PQsetChunkedRowsMode(handle_, 32) == 0) {
            cancel_bounded(std::chrono::seconds(2));
            drain_results_bounded(std::chrono::seconds(2));
            throw query_error("failed to enable chunked row retrieval", std::string{});
        }
        for (;;) {
            const int flushed = PQflush(handle_);
            if (flushed == 0) {
                break;
            }
            if (flushed < 0) {
                throw query_error("failed to flush command: " + current_error_message(),
                                  std::string{});
            }
            if (!wait_socket(handle_, /*want_read=*/false, deadline)) {
                cancel_bounded(std::chrono::seconds(2));
                drain_results_bounded(std::chrono::seconds(2));
                throw query_error("client deadline exceeded while sending", "57014");
            }
        }

        result out;
        std::optional<captured_error> error;
        std::size_t total_bytes = 0;
        bool have_chunk_columns = false;
        bool limited = false;
        result provisional;

        for (;;) {
            while (PQisBusy(handle_) != 0) {
                if (!wait_socket(handle_, /*want_read=*/true, deadline)) {
                    cancel_bounded(std::chrono::seconds(2));
                    drain_results_bounded(std::chrono::seconds(2));
                    throw query_error("client deadline exceeded awaiting result", "57014");
                }
                if (PQconsumeInput(handle_) == 0) {
                    throw query_error(
                        "connection lost while reading result: " + current_error_message(),
                        std::string{});
                }
            }
            PGresult* res = PQgetResult(handle_);
            if (res == nullptr) {
                break;
            }
            const ExecStatusType st = PQresultStatus(res);
            if (st == PGRES_TUPLES_CHUNK) {
                absorb(provisional, res, /*copy_rows=*/true, limits, total_bytes, limited);
                have_chunk_columns = true;
            } else if (st == PGRES_TUPLES_OK) {
                if (PQntuples(res) == 0 && have_chunk_columns) {
                    // Zero-row terminator: commit the accumulated chunks.
                    out.status_ = PGRES_TUPLES_OK;
                    out.columns_ = std::move(provisional.columns_);
                    out.rows_ = std::move(provisional.rows_);
                } else {
                    // A whole (unchunked) tuple result — e.g. an empty SELECT.
                    out.status_ = PGRES_TUPLES_OK;
                    out.columns_.clear();
                    out.rows_.clear();
                    absorb(out, res, /*copy_rows=*/true, limits, total_bytes, limited);
                }
                out.command_tag_ = PQcmdStatus(res) ? PQcmdStatus(res) : std::string{};
                provisional = result{};
                have_chunk_columns = false;
            } else if (st == PGRES_COMMAND_OK) {
                out.status_ = PGRES_COMMAND_OK;
                out.command_tag_ = PQcmdStatus(res) ? PQcmdStatus(res) : std::string{};
                provisional = result{};
                have_chunk_columns = false;
            } else if (st == PGRES_COPY_IN) {
                PQputCopyEnd(handle_, "bioseq website: COPY FROM STDIN is unsupported");
                PQclear(res);
                cancel_bounded(std::chrono::seconds(2));
                drain_results_bounded(std::chrono::seconds(2));
                throw query_error("COPY FROM STDIN is not a supported tutorial protocol",
                                  "0A000");
            } else if (st == PGRES_COPY_OUT || st == PGRES_COPY_BOTH) {
                PQclear(res);
                cancel_bounded(std::chrono::seconds(2));
                copy_discard_bounded(std::chrono::seconds(2));
                drain_results_bounded(std::chrono::seconds(2));
                throw query_error("COPY TO STDOUT is not a supported tutorial protocol",
                                  "0A000");
            } else if (st == PGRES_FATAL_ERROR || st == PGRES_BAD_RESPONSE) {
                // Discard the failed statement's provisional chunks (§13.5).
                provisional = result{};
                have_chunk_columns = false;
                error = capture_error(res);
            }
            PQclear(res);
            if (limited) {
                // The result already violates the caller's limit; stop the server
                // and materialise what we have so the caller's check reports it.
                if (out.rows_.empty() && !provisional.rows_.empty()) {
                    out.status_ = PGRES_TUPLES_OK;
                    out.columns_ = std::move(provisional.columns_);
                    out.rows_ = std::move(provisional.rows_);
                }
                cancel_bounded(std::chrono::seconds(2));
                drain_results_bounded(std::chrono::seconds(2));
                return out;
            }
        }

        if (PQtransactionStatus(handle_) != PQTRANS_IDLE) {
            rollback_bounded(std::chrono::seconds(2));
            if (!error) {
                error = captured_error{
                    .sqlstate = "25000",
                    .primary = "phase left the connection in an open transaction",
                    .detail = {},
                    .hint = {},
                    .severity = "ERROR"};
            }
        }
        if (error) {
            out.error_sqlstate_ = error->sqlstate;
            throw query_error(error->primary, error->sqlstate, error->detail,
                              error->hint, error->severity);
        }
        return out;
    }

    static void receive_notice(void* self, const PGresult* raw) noexcept {
        auto* owner = static_cast<connection*>(self);
        try {
            notice captured;
            if (const char* severity =
                    PQresultErrorField(raw, PG_DIAG_SEVERITY_NONLOCALIZED)) {
                captured.severity = severity;
            } else if (const char* localized = PQresultErrorField(raw, PG_DIAG_SEVERITY)) {
                captured.severity = localized;
            }
            if (const char* sqlstate = PQresultErrorField(raw, PG_DIAG_SQLSTATE)) {
                captured.sqlstate = sqlstate;
            }
            if (const char* primary = PQresultErrorField(raw, PG_DIAG_MESSAGE_PRIMARY)) {
                captured.message = primary;
            } else if (const char* text = PQresultErrorMessage(raw)) {
                captured.message = text;
            }
            owner->notices_.push_back(std::move(captured));
        } catch (...) {
            // A libpq notice callback must never unwind through C; a notice lost
            // to an allocation failure still survives in the server log.
        }
    }

    PGconn* handle_{};
    std::vector<notice> notices_;
};

/// @brief Builds a Unix-socket-only conninfo string from explicit parts.
///
/// The website executor is the only caller; keeping construction here means no
/// other module needs to spell libpq connection keywords.
[[nodiscard]] std::string unix_socket_conninfo(
    std::string_view socket_directory,
    std::string_view database,
    std::string_view user,
    int port) {
    auto quote = [](std::string_view value) {
        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('\'');
        for (const char character : value) {
            if (character == '\\' || character == '\'') {
                escaped.push_back('\\');
            }
            escaped.push_back(character);
        }
        escaped.push_back('\'');
        return escaped;
    };
    std::string conninfo;
    conninfo += "host=" + quote(socket_directory);
    conninfo += " port=" + std::to_string(port);
    conninfo += " dbname=" + quote(database);
    conninfo += " user=" + quote(user);
    conninfo += " sslmode=disable";
    return conninfo;
}

}  // namespace bioseq::libpq
