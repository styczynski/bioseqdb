module;

#include <libpq-fe.h>

#include <poll.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
// retrieval (PQsetChunkedRowsMode) for ordinary calls, an opt-in single-row
// observer for true first-tuple timing, bounded PQcancel + drain on a client
// deadline or a caller-imposed result limit, rejection of the COPY sub-protocols
// no tutorial may enter, and a PQTRANS_IDLE requirement by default. Explicit
// PostgreSQL isolation fixtures may select allow_open so transactions span
// independently bounded commands. No blocking PQconnectdb/PQexec shortcut
// exists anywhere in this module.
export namespace bioseq::libpq {

/// @brief Controls whether a command must leave its connection transaction-idle.
///
/// Existing website and one-shot test callers use `require_idle`, which retains
/// the historical fail-safe rollback after an accidental BEGIN. PostgreSQL
/// isolation fixtures opt into `allow_open` so a transaction can intentionally
/// span several independently bounded commands.
enum class transaction_policy : std::uint8_t {
    require_idle,
    allow_open,
};

/// @brief Source-neutral view of libpq's current transaction state.
enum class transaction_state : std::uint8_t {
    idle,
    active,
    in_transaction,
    failed_transaction,
    unknown,
};

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

/// @brief Caller-imposed retention bounds applied while streamed rows accumulate.
///
/// A zero field means "unlimited". The state machine retains rows up to and
/// including the first element that crosses a bound, then cancels the running
/// statement and stops accumulating, so the materialised result the caller
/// receives already violates the caller's own limit check (which owns the
/// author-facing message) while generator memory stays bounded (§13.4, §13.5).
/// max_result_sets bounds a multi-statement phase the same way: the set that
/// first crosses the cap is retained so the caller's own count check reports it.
struct execution_limits {
    std::size_t max_rows{};
    std::size_t max_columns{};
    std::size_t max_cell_bytes{};
    std::size_t max_total_bytes{};
    std::size_t max_result_sets{};
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
    /// @brief Tuple count reported by PQcmdTuples, when the command has one.
    [[nodiscard]] std::optional<std::uint64_t> affected_rows() const noexcept {
        return affected_rows_;
    }
    [[nodiscard]] ExecStatusType status() const noexcept { return status_; }
    [[nodiscard]] bool has_rows() const noexcept { return status_ == PGRES_TUPLES_OK; }
    [[nodiscard]] std::string error_sqlstate() const { return error_sqlstate_; }

    // Populated by connection::run_statement; public so the state machine (a
    // member of connection) can assemble it directly.
    ExecStatusType status_{PGRES_COMMAND_OK};
    std::vector<column_meta> columns_;
    std::vector<std::vector<cell>> rows_;
    std::string command_tag_;
    std::optional<std::uint64_t> affected_rows_;
    std::string error_sqlstate_;

private:
    [[nodiscard]] const column_meta& at_column(int column) const {
        return columns_[static_cast<std::size_t>(column)];
    }
};

/// @brief Optional, non-owning observation sink for completed client commands.
///
/// Connections do not install an observer by default. Test fixtures may attach
/// one whose lifetime exceeds the connection; callbacks are synchronous,
/// nonthrowing, and receive owned results before they are returned to callers.
class execution_observer {
public:
    virtual ~execution_observer() = default;

    virtual void command_succeeded(
        std::string_view context,
        std::string_view sql,
        const result& outcome,
        std::chrono::nanoseconds elapsed) noexcept = 0;

    virtual void command_failed(
        std::string_view context,
        std::string_view sql,
        const query_error& error,
        std::chrono::nanoseconds elapsed) noexcept = 0;
};

/// @brief Materialised result plus wire-observed first-row and completion time.
///
/// `time_to_first_row` is empty for a successful command or empty tuple result.
/// Tuple queries use libpq single-row mode, and the timestamp is captured when
/// the first `PGRES_SINGLE_TUPLE` reaches the client state machine. `elapsed`
/// covers send, flush, every row, and the terminal result.
struct single_row_observed_result {
    result rows;
    std::optional<std::chrono::nanoseconds> time_to_first_row;
    std::chrono::nanoseconds elapsed{};
};

/// @brief Every result set a (possibly multi-statement) phase produced, in order.
///
/// A multi-statement visible phase such as `SELECT 1; SELECT 'a','b'` yields one
/// entry per statement (§13.4, §13.6). When a statement raises, the fully
/// completed results before it are retained and `error` carries the SQLSTATE and
/// message so an expect=error example can render them followed by the failure
/// (§10.7); a success example turns the same `error` into a build diagnostic.
/// A client-side failure (deadline, lost connection, COPY, chunk-mode) still
/// throws rather than landing here.
struct multi_result {
    std::vector<result> results;
    std::optional<query_error> error;
};

/// @brief RAII PostgreSQL connection driven entirely by the nonblocking machine.
///
/// Constructed from an explicit Unix-socket conninfo so callers never build the
/// string outside their owning policy module; connection uses
/// PQconnectStartParams/PQconnectPoll with either the default ten-second or a
/// caller-supplied deadline and then PQsetnonblocking(1). Notices are captured
/// into an owned buffer the caller drains after each command.
class connection {
public:
    explicit connection(const std::string& conninfo)
        : handle_(nonblocking_connect(conninfo, std::chrono::seconds(10))) {
        PQsetNoticeReceiver(handle_, &connection::receive_notice, this);
    }
    /// @brief Opens a connection under a caller-supplied positive deadline.
    connection(const std::string& conninfo, std::chrono::milliseconds deadline)
        : handle_(nonblocking_connect(conninfo, deadline)) {
        PQsetNoticeReceiver(handle_, &connection::receive_notice, this);
    }
    connection(connection&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)),
          notices_(std::move(other.notices_)),
          observer_(std::exchange(other.observer_, nullptr)),
          observer_context_(std::move(other.observer_context_)) {
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
        return execute_observed(
            sql, std::chrono::seconds(10), execution_limits{},
            transaction_policy::require_idle);
    }

    /// @brief Runs a command with an explicit client deadline (e.g. the fence).
    [[nodiscard]] result exec(std::string_view sql,
                              std::chrono::milliseconds deadline) {
        return execute_observed(
            sql, deadline, execution_limits{},
            transaction_policy::require_idle);
    }

    /// @brief Runs a command while applying the selected transaction-idle policy.
    [[nodiscard]] result exec(std::string_view sql, transaction_policy policy) {
        return execute_observed(
            sql, std::chrono::seconds(10), execution_limits{}, policy);
    }

    /// @brief Runs a bounded command that may intentionally leave a transaction open.
    [[nodiscard]] result exec(std::string_view sql,
                              std::chrono::milliseconds deadline,
                              transaction_policy policy) {
        return execute_observed(
            sql, deadline, execution_limits{}, policy);
    }

    /// @brief Runs a command under both a client deadline and retention limits.
    [[nodiscard]] result exec_bounded(std::string_view sql,
                                      std::chrono::milliseconds deadline,
                                      const execution_limits& limits) {
        return execute_observed(
            sql, deadline, limits, transaction_policy::require_idle);
    }

    /// @brief Runs a retention-bounded command under an explicit transaction policy.
    [[nodiscard]] result exec_bounded(std::string_view sql,
                                      std::chrono::milliseconds deadline,
                                      const execution_limits& limits,
                                      transaction_policy policy) {
        return execute_observed(sql, deadline, limits, policy);
    }

    /// @brief Executes one bounded query and observes its first wire-level row.
    ///
    /// The returned rows have the same owned representation and transaction
    /// policy as `exec`; only libpq's retrieval mode changes from 32-row chunks
    /// to `PQsetSingleRowMode`. Default `exec` behavior remains unchanged.
    [[nodiscard]] single_row_observed_result exec_single_row_observed(
        std::string_view sql,
        std::chrono::milliseconds deadline,
        execution_limits limits = {},
        transaction_policy policy =
            transaction_policy::require_idle) {
        single_row_observed_result observed;
        observed.rows = execute_observed(
            sql, deadline, limits, policy, &observed);
        return observed;
    }

    /// @brief Runs a (possibly multi-statement) phase capturing every result set
    /// in execution order under a client deadline and retention limits (§13.4).
    ///
    /// Unlike exec/exec_bounded a failed statement does not throw: the results
    /// completed before PostgreSQL stopped are returned alongside the terminating
    /// error so an expect=error phase can render them in order (§10.7). Client-side
    /// failures (deadline, lost connection, COPY, chunk-mode) still throw.
    [[nodiscard]] multi_result exec_bounded_multi(std::string_view sql,
                                                  std::chrono::milliseconds deadline,
                                                  const execution_limits& limits) {
        return execute_multi_observed(
            sql, deadline, limits, transaction_policy::require_idle);
    }

    /// @brief Runs a multi-result command under an explicit transaction policy.
    [[nodiscard]] multi_result exec_bounded_multi(
        std::string_view sql,
        std::chrono::milliseconds deadline,
        const execution_limits& limits,
        transaction_policy policy) {
        return execute_multi_observed(sql, deadline, limits, policy);
    }

    /// @brief Attaches or clears a non-owning synchronous execution observer.
    void set_execution_observer(
        execution_observer* observer,
        std::string context = {}) {
        observer_context_ = observer == nullptr
            ? std::string{}
            : std::move(context);
        observer_ = observer;
    }

    /// @brief Returns the currently attached observer, if any.
    [[nodiscard]] execution_observer* execution_observer_ptr() const noexcept {
        return observer_;
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

    /// @brief Quotes an SQL string literal via libpq (quotes included) so a
    /// generator-owned catalog value can be compared without string interpolation.
    [[nodiscard]] std::string escape_literal(std::string_view value) const {
        char* escaped = PQescapeLiteral(handle_, value.data(), value.size());
        if (escaped == nullptr) {
            throw query_error("PQescapeLiteral failed: " + current_error_message(),
                              std::string{});
        }
        std::string result(escaped);
        PQfreemem(escaped);
        return result;
    }

    /// @brief True when the connection is open and idle (no in-flight transaction).
    ///
    /// The isolation teardown path uses this to decide whether the original
    /// example connection can run teardown or the clone/role must be restored and
    /// reconnected first (§13.5).
    [[nodiscard]] bool is_idle() const noexcept {
        return handle_ != nullptr
            && PQstatus(handle_) == CONNECTION_OK
            && PQtransactionStatus(handle_) == PQTRANS_IDLE;
    }

    /// @brief Returns the current libpq transaction state without exposing PGconn.
    [[nodiscard]] transaction_state current_transaction_state() const noexcept {
        if (handle_ == nullptr || PQstatus(handle_) != CONNECTION_OK) {
            return transaction_state::unknown;
        }
        switch (PQtransactionStatus(handle_)) {
        case PQTRANS_IDLE:
            return transaction_state::idle;
        case PQTRANS_ACTIVE:
            return transaction_state::active;
        case PQTRANS_INTRANS:
            return transaction_state::in_transaction;
        case PQTRANS_INERROR:
            return transaction_state::failed_transaction;
        case PQTRANS_UNKNOWN:
            return transaction_state::unknown;
        }
        return transaction_state::unknown;
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

    [[nodiscard]] result execute_observed(
        std::string_view sql,
        clock::duration budget,
        const execution_limits& limits,
        transaction_policy policy,
        single_row_observed_result* observation = nullptr) {
        const auto started = clock::now();
        try {
            result outcome =
                run_statement(sql, budget, limits, policy, observation);
            notify_success(sql, outcome, started);
            return outcome;
        } catch (const query_error& error) {
            notify_failure(sql, error, started);
            throw;
        }
    }

    [[nodiscard]] multi_result execute_multi_observed(
        std::string_view sql,
        clock::duration budget,
        const execution_limits& limits,
        transaction_policy policy) {
        const auto started = clock::now();
        try {
            multi_result outcome =
                run_statement_multi(sql, budget, limits, policy);
            if (outcome.error) {
                notify_failure(sql, *outcome.error, started);
            } else {
                const result empty;
                notify_success(
                    sql,
                    outcome.results.empty() ? empty : outcome.results.back(),
                    started);
            }
            return outcome;
        } catch (const query_error& error) {
            notify_failure(sql, error, started);
            throw;
        }
    }

    void notify_success(
        std::string_view sql,
        const result& outcome,
        clock::time_point started) noexcept {
        if (observer_ == nullptr) {
            return;
        }
        observer_->command_succeeded(
            observer_context_, sql, outcome,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now() - started));
    }

    void notify_failure(
        std::string_view sql,
        const query_error& error,
        clock::time_point started) noexcept {
        if (observer_ == nullptr) {
            return;
        }
        observer_->command_failed(
            observer_context_, sql, error,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now() - started));
    }

    [[nodiscard]] static std::optional<std::uint64_t> command_affected_rows(
        PGresult* raw) noexcept {
        const char* text = PQcmdTuples(raw);
        if (text == nullptr || *text == '\0') {
            return std::nullopt;
        }
        const std::string_view value{text};
        std::uint64_t count{};
        const auto [end, error] = std::from_chars(
            value.data(), value.data() + value.size(), count);
        if (error != std::errc{} || end != value.data() + value.size()) {
            return std::nullopt;
        }
        return count;
    }

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
        if (budget <= clock::duration::zero()) {
            throw query_error("connection deadline must be positive", std::string{});
        }
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
                throw query_error("connection deadline exceeded",
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
            if (limits.max_columns != 0
                && out.columns_.size() > limits.max_columns) {
                limited = true;
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

    // Runs a single logical command and returns its final result set, throwing
    // query_error on any server or client-side failure. A thin wrapper over the
    // multi-result state machine: single-statement callers (session settings,
    // setup/teardown, admin, catalog) keep the last result and see failures as
    // exceptions exactly as before.
    [[nodiscard]] result run_statement(std::string_view sql,
                                       clock::duration budget,
                                       const execution_limits& limits,
                                       transaction_policy policy,
                                       single_row_observed_result*
                                           observation = nullptr) {
        multi_result outcome = run_statement_multi(
            sql, budget, limits, policy, observation);
        if (outcome.error) {
            throw *outcome.error;
        }
        return outcome.results.empty() ? result{} : std::move(outcome.results.back());
    }

    // The single §13.5 send/flush/consume state machine. Sends `sql`, enables
    // ordinary 32-row chunks or opt-in single-row observation, then polls the
    // socket under `budget` while consuming results — committing streamed rows
    // only on their zero-row
    // PGRES_TUPLES_OK terminator, discarding a failed statement's provisional
    // rows, rejecting COPY, bounding accumulation against `limits`, and requiring
    // a PQTRANS_IDLE connection afterward. Every fully completed result set is
    // retained in execution order (§13.4, §13.6); when a statement fails, the
    // earlier results are kept and the terminating error is returned in
    // multi_result::error rather than thrown, so an expect=error phase can render
    // the successful results before it (§10.7). Client-side failures still throw.
    [[nodiscard]] multi_result run_statement_multi(std::string_view sql,
                                                   clock::duration budget,
                                                   const execution_limits& limits,
                                                   transaction_policy policy,
                                                   single_row_observed_result*
                                                       observation = nullptr) {
        const auto started = clock::now();
        const auto deadline = started + budget;
        if (observation != nullptr) {
            observation->time_to_first_row.reset();
            observation->elapsed = std::chrono::nanoseconds::zero();
        }
        const std::string owned(sql);
        if (PQsendQuery(handle_, owned.c_str()) == 0) {
            throw query_error("failed to send command: " + current_error_message(),
                              std::string{});
        }
        const int row_mode_enabled =
            observation != nullptr
                ? PQsetSingleRowMode(handle_)
                : PQsetChunkedRowsMode(handle_, 32);
        if (row_mode_enabled == 0) {
            cancel_bounded(std::chrono::seconds(2));
            drain_results_bounded(std::chrono::seconds(2));
            throw query_error(
                observation != nullptr
                    ? "failed to enable single-row retrieval"
                    : "failed to enable chunked row retrieval",
                std::string{});
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

        multi_result outcome;
        std::optional<captured_error> error;
        std::size_t total_bytes = 0;
        bool have_chunk_columns = false;
        bool limited = false;
        result provisional;

        // Appends a fully completed result set in execution order, reporting when
        // it is the one that first crosses the result-set cap so accumulation
        // stops (mirroring the per-row retention pattern in absorb).
        auto commit = [&outcome, &limits](result&& completed) -> bool {
            outcome.results.push_back(std::move(completed));
            return limits.max_result_sets != 0
                && outcome.results.size() > limits.max_result_sets;
        };

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
            bool capped = false;
            if (st == PGRES_SINGLE_TUPLE || st == PGRES_TUPLES_CHUNK) {
                if (observation != nullptr
                    && !observation->time_to_first_row
                    && PQntuples(res) > 0) {
                    observation->time_to_first_row =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            clock::now() - started);
                }
                absorb(provisional, res, /*copy_rows=*/true, limits, total_bytes, limited);
                have_chunk_columns = true;
            } else if (st == PGRES_TUPLES_OK) {
                if (observation != nullptr
                    && !observation->time_to_first_row
                    && PQntuples(res) > 0) {
                    observation->time_to_first_row =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            clock::now() - started);
                }
                result current;
                current.status_ = PGRES_TUPLES_OK;
                if (PQntuples(res) == 0 && have_chunk_columns) {
                    // Zero-row terminator: commit the accumulated chunks.
                    current.columns_ = std::move(provisional.columns_);
                    current.rows_ = std::move(provisional.rows_);
                } else {
                    // A whole (unchunked) tuple result — e.g. an empty SELECT.
                    absorb(current, res, /*copy_rows=*/true, limits, total_bytes, limited);
                }
                current.command_tag_ = PQcmdStatus(res) ? PQcmdStatus(res) : std::string{};
                provisional = result{};
                have_chunk_columns = false;
                capped = commit(std::move(current));
            } else if (st == PGRES_COMMAND_OK) {
                result current;
                current.status_ = PGRES_COMMAND_OK;
                current.command_tag_ = PQcmdStatus(res) ? PQcmdStatus(res) : std::string{};
                current.affected_rows_ = command_affected_rows(res);
                provisional = result{};
                have_chunk_columns = false;
                capped = commit(std::move(current));
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
                // Discard the failed statement's provisional chunks (§13.5); the
                // earlier completed results are retained.
                provisional = result{};
                have_chunk_columns = false;
                error = capture_error(res);
            }
            PQclear(res);
            if (limited || capped) {
                // The retained results already cross a caller-owned bound; stop the
                // server and return what we have so the caller's checks report it.
                if (limited
                    && (!provisional.columns_.empty() || !provisional.rows_.empty())) {
                    result current;
                    current.status_ = PGRES_TUPLES_OK;
                    current.columns_ = std::move(provisional.columns_);
                    current.rows_ = std::move(provisional.rows_);
                    outcome.results.push_back(std::move(current));
                }
                cancel_bounded(std::chrono::seconds(2));
                drain_results_bounded(std::chrono::seconds(2));
                if (observation != nullptr) {
                    observation->elapsed =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            clock::now() - started);
                }
                return outcome;
            }
        }

        if (policy == transaction_policy::require_idle
            && PQtransactionStatus(handle_) != PQTRANS_IDLE) {
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
            outcome.error = query_error(error->primary, error->sqlstate, error->detail,
                                        error->hint, error->severity);
        }
        if (observation != nullptr) {
            observation->elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    clock::now() - started);
        }
        return outcome;
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
    execution_observer* observer_{};
    std::string observer_context_;
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
