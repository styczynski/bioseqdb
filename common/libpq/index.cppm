module;

#include <libpq-fe.h>

#include <poll.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.libpq.client;

// Own libpq connections and copy results into C++ values. Commands use
// nonblocking I/O with deadlines. This module also handles cancellation,
// unread results and notices while keeping raw libpq handles private.
export namespace bioseq::libpq {

/**
 * Control transaction cleanup after normal result processing.
 * require_idle attempts rollback and reports an error if a transaction remains
 * open. allow_open permits a transaction to span commands. A result-limit stop
 * returns early, before this check.
 */
enum class transaction_policy : std::uint8_t {
    /// Check for PQTRANS_IDLE after normal result processing.
    require_idle,
    /// Preserve an intentionally open transaction.
    allow_open,
};

/// Current transaction state reported by libpq.
enum class transaction_state : std::uint8_t {
    /// No transaction is open.
    idle,
    /// A command is currently active.
    active,
    /// A transaction is open and has not failed.
    in_transaction,
    /// A transaction is open and requires rollback.
    failed_transaction,
    /// libpq cannot determine the transaction state.
    unknown,
};

/**
 * A command failure with a message and optional PostgreSQL diagnostics.
 * Server errors supply SQLSTATE and any detail, hint and severity fields.
 * Client errors can also set SQLSTATE, for example 57014 for a deadline.
 */
class command_error final : public std::runtime_error {
public:
    command_error(
        std::string message,
        std::string sql_state,
        std::string detail = {},
        std::string hint = {},
        std::string severity = {})
        : std::runtime_error(std::move(message)),
          sql_state_(std::move(sql_state)),
          detail_(std::move(detail)),
          hint_(std::move(hint)),
          severity_(std::move(severity)) {}

    /// SQLSTATE supplied for the failure, or empty when unavailable.
    [[nodiscard]] std::string_view sql_state() const noexcept {
        return sql_state_;
    }
    /// @brief PostgreSQL detail field, or empty when absent.
    [[nodiscard]] std::string_view detail() const noexcept { return detail_; }
    /// @brief PostgreSQL hint field, or empty when absent.
    [[nodiscard]] std::string_view hint() const noexcept { return hint_; }
    /// PostgreSQL severity, preferring the non-localized field when available.
    [[nodiscard]] std::string_view severity() const noexcept { return severity_; }

private:
    std::string sql_state_;
    std::string detail_;
    std::string hint_;
    std::string severity_;
};

/// @brief One captured server notice/warning during command execution.
struct server_notice {
    /// PostgreSQL severity, preferring the non-localized field.
    std::string severity;
    /// Five-character SQLSTATE when available.
    std::string sql_state;
    /// Primary PostgreSQL notice message.
    std::string message;
};

/**
 * Stop collecting results once a caller's size limit is exceeded.
 * Zero disables a limit. Keep the row or result set that crosses the limit,
 * then request cancellation and return the collected data. The caller checks
 * the returned sizes and reports the limit failure.
 */
struct result_limits {
    /// Row count that triggers stopping when exceeded within one result set.
    std::size_t max_rows{};
    /// Column count that triggers stopping when exceeded.
    std::size_t max_columns{};
    /// Non-null cell size that triggers stopping when exceeded.
    std::size_t max_cell_bytes{};
    /// Total cell bytes that trigger stopping when exceeded across the command.
    std::size_t max_total_bytes{};
    /// Result-set count that triggers stopping when exceeded.
    std::size_t max_result_sets{};
};

/// Distinguish results with row metadata from command-only results.
enum class result_status : std::uint8_t {
    /// A command completed without returning a row set.
    command_complete,
    /// Result with column metadata, including an empty row set.
    rows_complete,
};

class client_connection;

/**
 * Own the copied columns and cells from a PostgreSQL result.
 * The data remains valid after libpq frees its PGresult. Accessors expose
 * text and null flags without exposing libpq handles. If a result limit
 * stopped execution, this object can contain a partial row set.
 */
class command_result final {
public:
    command_result() = default;

    /// @brief Number of materialized rows.
    [[nodiscard]] std::size_t row_count() const noexcept {
        return rows_.size();
    }
    /// @brief Number of columns described by the result.
    [[nodiscard]] std::size_t column_count() const noexcept {
        return columns_.size();
    }

    /// @brief Column name at a valid zero-based index.
    [[nodiscard]] std::string column_name(std::size_t column) const {
        return at_column(column).name;
    }
    /// @brief PostgreSQL type OID at a valid zero-based column index.
    [[nodiscard]] std::uint32_t column_type_oid(
        std::size_t column) const noexcept {
        return at_column(column).type_oid;
    }
    /// @brief Whether a valid zero-based row and column identify SQL NULL.
    [[nodiscard]] bool cell_is_null(
        std::size_t row,
        std::size_t column) const noexcept {
        return rows_[row][column].is_null;
    }
    /**
     * Borrow the text bytes of a non-null cell at valid row and column indexes.
     * The view stays valid while this result's cell storage remains alive.
     */
    [[nodiscard]] std::string_view text_value(
        std::size_t row,
        std::size_t column) const noexcept {
        return rows_[row][column].text;
    }

    /// @brief PostgreSQL command tag such as "SELECT 3" or "CREATE EXTENSION".
    [[nodiscard]] std::string command_tag() const {
        return command_tag_;
    }
    /// Affected-row count read from PQcmdTuples for a command-only result.
    [[nodiscard]] std::optional<std::uint64_t>
    affected_row_count() const noexcept {
        return affected_row_count_;
    }
    /// Category of the stored result.
    [[nodiscard]] result_status status() const noexcept { return status_; }
    /// Whether the result has row metadata.
    [[nodiscard]] bool contains_rows() const noexcept {
        return status_ == result_status::rows_complete;
    }

private:
    friend class client_connection;

    struct column_metadata {
        std::string name;
        std::uint32_t type_oid{};
    };

    struct text_cell {
        bool is_null{};
        std::string text;
    };

    [[nodiscard]] const column_metadata& at_column(
        std::size_t column) const noexcept {
        return columns_[column];
    }

    result_status status_{result_status::command_complete};
    std::vector<column_metadata> columns_;
    std::vector<std::vector<text_cell>> rows_;
    std::string command_tag_;
    std::optional<std::uint64_t> affected_row_count_;
};

/**
 * Observe command outcomes through synchronous callbacks.
 * Connections borrow this object, so it must stay alive while attached.
 * Callbacks must not throw. A successful batch reports its last result,
 * and a limit-stopped command can report a partial result.
 */
class command_observer {
public:
    virtual ~command_observer() = default;

    /// Receive a result before execute returns, including a result stopped by limits.
    virtual void command_succeeded(
        std::string_view context,
        std::string_view sql,
        const command_result& outcome,
        std::chrono::nanoseconds elapsed) noexcept = 0;

    /// @brief Receives one failed command synchronously.
    virtual void command_failed(
        std::string_view context,
        std::string_view sql,
        const command_error& error,
        std::chrono::nanoseconds elapsed) noexcept = 0;
};

/**
 * Own a command result and record when its first row arrived.
 * Single-row mode lets the client measure arrival before the whole result
 * has been read. The first-row time is empty when no row arrives. Elapsed
 * time ends when result processing returns, including any limit cleanup.
 */
struct timed_command_result {
    /// Copied command result, possibly partial when a limit was exceeded.
    command_result outcome;
    /// Time until the first row reached the client, when a row was observed.
    std::optional<std::chrono::nanoseconds> time_to_first_row;
    /// Time from starting the send through result processing and any cleanup.
    std::chrono::nanoseconds elapsed{};
};

/**
 * Keep results from a command string in execution order.
 * A later statement error leaves earlier completed results available.
 * The error field holds server errors and transaction-policy failures.
 * Connection failures, deadlines, unsupported COPY and retrieval-mode errors
 * throw command_error. A result limit can leave a partial final result.
 */
struct command_batch_result {
    /// Collected results in order, including a partial final result after a limit stop.
    std::vector<command_result> completed;
    /// Server error or transaction-policy failure, when present.
    std::optional<command_error> error;
};

/**
 * Own a PostgreSQL connection and run commands with nonblocking libpq I/O.
 * Connection setup uses a ten-second timeout unless the caller supplies one.
 * Destruction closes the connection. Captured notices stay here until drained.
 */
class client_connection final {
public:
    /// @brief Opens a connection under the default ten-second timeout.
    explicit client_connection(std::string_view connection_parameters)
        : handle_(open_nonblocking_connection(
              connection_parameters, default_command_timeout)) {
        PQsetNoticeReceiver(handle_, &client_connection::receive_notice, this);
    }
    /// @brief Opens a connection under a caller-supplied positive timeout.
    client_connection(
        std::string_view connection_parameters,
        std::chrono::milliseconds timeout)
        : handle_(open_nonblocking_connection(
              connection_parameters, timeout)) {
        PQsetNoticeReceiver(handle_, &client_connection::receive_notice, this);
    }
    client_connection(client_connection&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)),
          notices_(std::move(other.notices_)),
          observer_(std::exchange(other.observer_, nullptr)),
          observer_context_(std::move(other.observer_context_)) {
        if (handle_) {
            PQsetNoticeReceiver(
                handle_, &client_connection::receive_notice, this);
        }
    }
    client_connection& operator=(client_connection&&) = delete;
    client_connection(const client_connection&) = delete;
    client_connection& operator=(const client_connection&) = delete;
    ~client_connection() {
        if (handle_) {
            PQfinish(handle_);
        }
    }

    /// @brief Runs a command with the default ten-second client timeout.
    [[nodiscard]] command_result execute(std::string_view sql) {
        return execute_and_notify(
            sql, default_command_timeout, result_limits{},
            transaction_policy::require_idle);
    }

    /// @brief Runs a command with an explicit client timeout.
    [[nodiscard]] command_result execute(
        std::string_view sql,
        std::chrono::milliseconds timeout) {
        return execute_and_notify(
            sql, timeout, result_limits{},
            transaction_policy::require_idle);
    }

    /// Run a command with the chosen transaction policy.
    [[nodiscard]] command_result execute(
        std::string_view sql,
        transaction_policy policy) {
        return execute_and_notify(
            sql, default_command_timeout, result_limits{}, policy);
    }

    /// Run a command with an explicit timeout and transaction policy.
    [[nodiscard]] command_result execute(
        std::string_view sql,
        std::chrono::milliseconds timeout,
        transaction_policy policy) {
        return execute_and_notify(sql, timeout, result_limits{}, policy);
    }

    /// Run a command with a timeout and result-size stopping limits.
    [[nodiscard]] command_result execute_with_limits(
        std::string_view sql,
        std::chrono::milliseconds timeout,
        const result_limits& limits) {
        return execute_and_notify(
            sql, timeout, limits, transaction_policy::require_idle);
    }

    /// Run a command with timeout, result limits and a transaction policy.
    [[nodiscard]] command_result execute_with_limits(
        std::string_view sql,
        std::chrono::milliseconds timeout,
        const result_limits& limits,
        transaction_policy policy) {
        return execute_and_notify(sql, timeout, limits, policy);
    }

    /**
     * Run a query and measure when its first row reaches the client.
     * Use libpq single-row mode to observe that arrival, then copy rows into the
     * returned result. Apply the supplied timeout, size limits and transaction
     * policy.
     */
    [[nodiscard]] timed_command_result execute_with_row_timing(
        std::string_view sql,
        std::chrono::milliseconds timeout,
        result_limits limits = {},
        transaction_policy policy =
            transaction_policy::require_idle) {
        timed_command_result observed;
        observed.outcome = execute_and_notify(
            sql, timeout, limits, policy, &observed);
        return observed;
    }

    /// Run a command string and collect its results under the supplied limits.
    [[nodiscard]] command_batch_result execute_batch_with_limits(
        std::string_view sql,
        std::chrono::milliseconds timeout,
        const result_limits& limits) {
        return execute_batch_and_notify(
            sql, timeout, limits, transaction_policy::require_idle);
    }

    /// Collect command results with an explicit transaction policy.
    [[nodiscard]] command_batch_result execute_batch_with_limits(
        std::string_view sql,
        std::chrono::milliseconds timeout,
        const result_limits& limits,
        transaction_policy policy) {
        return execute_batch_and_notify(sql, timeout, limits, policy);
    }

    /// Attach a borrowed observer, or pass nullptr to detach it.
    void set_observer(
        command_observer* observer,
        std::string context = {}) {
        observer_context_ = observer == nullptr
            ? std::string{}
            : std::move(context);
        observer_ = observer;
    }

    /// @brief Returns the currently attached observer, if any.
    [[nodiscard]] command_observer* observer() const noexcept {
        return observer_;
    }

    /// @brief Drains and clears notices captured since the last drain.
    [[nodiscard]] std::vector<server_notice> drain_notices() {
        return std::exchange(notices_, {});
    }

    /// Quote an SQL identifier with libpq.
    [[nodiscard]] std::string escape_identifier(std::string_view name) const {
        return take_escaped_value(
            PQescapeIdentifier(handle_, name.data(), name.size()),
            "PQescapeIdentifier");
    }

    /// Quote an SQL string literal with libpq, including its surrounding quotes.
    [[nodiscard]] std::string escape_literal(std::string_view value) const {
        return take_escaped_value(
            PQescapeLiteral(handle_, value.data(), value.size()),
            "PQescapeLiteral");
    }

    /**
     * Return whether the connection is usable and has no open transaction.
     * Use this before cleanup that requires an idle connection.
     */
    [[nodiscard]] bool is_transaction_idle() const noexcept {
        return handle_ != nullptr
            && PQstatus(handle_) == CONNECTION_OK
            && PQtransactionStatus(handle_) == PQTRANS_IDLE;
    }

    /// @brief Returns the current transaction state without exposing PGconn.
    [[nodiscard]] transaction_state transaction_status() const noexcept {
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

    /// @brief Returns the server's PostgreSQL major and minor release numbers.
    [[nodiscard]] std::string postgresql_release() const {
        const int release_number = PQserverVersion(handle_);
        if (release_number == 0) {
            return {};
        }
        const int major = release_number / 10000;
        const int minor = release_number % 10000;
        return std::to_string(major) + "." + std::to_string(minor);
    }

private:
    using clock = std::chrono::steady_clock;
    static constexpr clock::duration default_command_timeout =
        std::chrono::seconds(10);
    static constexpr clock::duration recovery_timeout =
        std::chrono::seconds(2);
    static constexpr int row_chunk_size = 32;

    [[nodiscard]] std::string take_escaped_value(
        char* escaped,
        std::string_view operation) const {
        if (escaped == nullptr) {
            throw command_error(
                std::string{operation} + " failed: "
                    + connection_error_message(),
                std::string{});
        }
        const std::unique_ptr<char, decltype(&PQfreemem)> owned{
            escaped, &PQfreemem};
        return std::string{owned.get()};
    }

    [[nodiscard]] command_result execute_and_notify(
        std::string_view sql,
        clock::duration timeout,
        const result_limits& limits,
        transaction_policy policy,
        timed_command_result* timing = nullptr) {
        const auto started = clock::now();
        try {
            command_result outcome = execute_statement(
                sql, timeout, limits, policy, timing);
            notify_success(sql, outcome, started);
            return outcome;
        } catch (const command_error& error) {
            notify_failure(sql, error, started);
            throw;
        }
    }

    [[nodiscard]] command_batch_result execute_batch_and_notify(
        std::string_view sql,
        clock::duration timeout,
        const result_limits& limits,
        transaction_policy policy) {
        const auto started = clock::now();
        try {
            command_batch_result outcome =
                execute_statement_batch(sql, timeout, limits, policy);
            if (outcome.error) {
                notify_failure(sql, *outcome.error, started);
            } else {
                const command_result empty;
                notify_success(
                    sql,
                    outcome.completed.empty()
                        ? empty
                        : outcome.completed.back(),
                    started);
            }
            return outcome;
        } catch (const command_error& error) {
            notify_failure(sql, error, started);
            throw;
        }
    }

    void notify_success(
        std::string_view sql,
        const command_result& outcome,
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
        const command_error& error,
        clock::time_point started) noexcept {
        if (observer_ == nullptr) {
            return;
        }
        observer_->command_failed(
            observer_context_, sql, error,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now() - started));
    }

    [[nodiscard]] static std::optional<std::uint64_t>
    parse_affected_row_count(PGresult* raw_result) noexcept {
        const char* text = PQcmdTuples(raw_result);
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

    // Wait for a socket event, checking the deadline at most one second apart.
    // Return false for a timeout, negative descriptor or poll failure. Socket
    // errors also wake poll, so libpq must check the connection afterward.
    static bool wait_for_descriptor(
        int descriptor,
        bool want_read,
        clock::time_point deadline) {
        if (descriptor < 0) {
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
            pollfd event{
                descriptor,
                static_cast<short>(want_read ? POLLIN : POLLOUT),
                0};
            const int ready = ::poll(&event, 1, slice);
            if (ready > 0) {
                return true;
            }
            if (ready < 0 && errno != EINTR) {
                return false;
            }
        }
    }

    static bool wait_for_connection(
        PGconn* raw_connection,
        bool want_read,
        clock::time_point deadline) {
        return wait_for_descriptor(
            PQsocket(raw_connection), want_read, deadline);
    }

    // Connect with libpq while polling against a fixed deadline. Enable
    // nonblocking mode after connection succeeds. expand_dbname lets the dbname
    // parameter contain a complete libpq connection string.
    static PGconn* open_nonblocking_connection(
        std::string_view connection_parameters,
        clock::duration timeout) {
        if (timeout <= clock::duration::zero()) {
            throw command_error(
                "connection deadline must be positive", std::string{});
        }
        const std::string owned_parameters{connection_parameters};
        const char* keywords[] = {"dbname", nullptr};
        const char* values[] = {owned_parameters.c_str(), nullptr};
        PGconn* raw_connection =
            PQconnectStartParams(keywords, values, /*expand_dbname=*/1);
        if (raw_connection == nullptr) {
            throw command_error(
                "libpq: out of memory starting connection", std::string{});
        }
        std::unique_ptr<PGconn, decltype(&PQfinish)> connection_owner{
            raw_connection, &PQfinish};
        if (PQstatus(raw_connection) == CONNECTION_BAD) {
            std::string message = PQerrorMessage(raw_connection);
            throw command_error("connection failed: " + message, std::string{});
        }
        const auto deadline = clock::now() + timeout;
        PostgresPollingStatusType polling = PGRES_POLLING_WRITING;
        for (;;) {
            const bool want_read = (polling == PGRES_POLLING_READING);
            if (!wait_for_connection(raw_connection, want_read, deadline)) {
                throw command_error(
                    "connection deadline exceeded", std::string{});
            }
            polling = PQconnectPoll(raw_connection);
            if (polling == PGRES_POLLING_OK) {
                break;
            }
            if (polling == PGRES_POLLING_FAILED) {
                std::string message = PQerrorMessage(raw_connection);
                throw command_error(
                    "connection failed: " + message, std::string{});
            }
        }
        if (PQstatus(raw_connection) != CONNECTION_OK
            || PQsetnonblocking(raw_connection, 1) != 0) {
            std::string message = PQerrorMessage(raw_connection);
            throw command_error(
                "connection not usable: " + message, std::string{});
        }
        return connection_owner.release();
    }

    [[nodiscard]] std::string connection_error_message() const {
        const char* message = handle_ ? PQerrorMessage(handle_) : nullptr;
        return message ? std::string(message) : std::string{};
    }

    struct error_fields {
        std::string sql_state;
        std::string primary;
        std::string detail;
        std::string hint;
        std::string severity;
    };

    static std::string result_error_field(
        const PGresult* raw_result,
        int code) {
        const char* value = PQresultErrorField(raw_result, code);
        return value ? std::string(value) : std::string{};
    }

    static error_fields read_error_fields(const PGresult* raw_result) {
        error_fields error;
        error.sql_state = result_error_field(raw_result, PG_DIAG_SQLSTATE);
        error.primary =
            result_error_field(raw_result, PG_DIAG_MESSAGE_PRIMARY);
        error.detail =
            result_error_field(raw_result, PG_DIAG_MESSAGE_DETAIL);
        error.hint = result_error_field(raw_result, PG_DIAG_MESSAGE_HINT);
        error.severity = result_error_field(
            raw_result, PG_DIAG_SEVERITY_NONLOCALIZED);
        if (error.severity.empty()) {
            error.severity =
                result_error_field(raw_result, PG_DIAG_SEVERITY);
        }
        if (error.primary.empty()) {
            const char* whole = PQresultErrorMessage(raw_result);
            if (whole) {
                error.primary = whole;
            }
        }
        return error;
    }

    // Copy columns and requested rows into owned storage. Keep the entire row
    // that exceeds a limit so the caller can inspect its size, then set limited
    // and stop copying. total_bytes counts cell bytes across the whole command.
    static void append_result_fragment(
        command_result& output,
        const PGresult* raw_result,
        bool copy_rows,
        const result_limits& limits,
        std::size_t& total_bytes,
        bool& limited) {
        if (output.columns_.empty()) {
            const int column_count = PQnfields(raw_result);
            output.columns_.reserve(static_cast<std::size_t>(column_count));
            for (int column = 0; column < column_count; ++column) {
                output.columns_.push_back(command_result::column_metadata{
                    .name = PQfname(raw_result, column)
                        ? PQfname(raw_result, column)
                        : std::string{},
                    .type_oid = static_cast<std::uint32_t>(
                        PQftype(raw_result, column))});
            }
            if (limits.max_columns != 0
                && output.columns_.size() > limits.max_columns) {
                limited = true;
            }
        }
        if (!copy_rows) {
            return;
        }
        const int column_count = PQnfields(raw_result);
        const int row_count = PQntuples(raw_result);
        for (int row = 0; row < row_count; ++row) {
            std::vector<command_result::text_cell> cells;
            cells.reserve(static_cast<std::size_t>(column_count));
            for (int column = 0; column < column_count; ++column) {
                const bool null_cell =
                    PQgetisnull(raw_result, row, column) != 0;
                const int length = PQgetlength(raw_result, row, column);
                std::string text = null_cell
                    ? std::string{}
                    : std::string(
                          PQgetvalue(raw_result, row, column),
                          static_cast<std::size_t>(length));
                total_bytes += text.size();
                if ((limits.max_cell_bytes != 0
                        && text.size() > limits.max_cell_bytes)
                    || (limits.max_total_bytes != 0
                        && total_bytes > limits.max_total_bytes)) {
                    limited = true;
                }
                cells.push_back(command_result::text_cell{
                    .is_null = null_cell,
                    .text = std::move(text)});
            }
            output.rows_.push_back(std::move(cells));
            if (limits.max_rows != 0
                && output.rows_.size() > limits.max_rows) {
                limited = true;
            }
            if (limited) {
                return;
            }
        }
    }

    // Request cancellation through libpq and stop polling when the timeout expires.
    void cancel_command(clock::duration timeout) noexcept {
        PGcancelConn* raw_cancel = PQcancelCreate(handle_);
        if (raw_cancel == nullptr) {
            return;
        }
        [[maybe_unused]] const std::unique_ptr<
            PGcancelConn,
            decltype(&PQcancelFinish)>
            cancel_owner{raw_cancel, &PQcancelFinish};
        if (PQcancelStart(raw_cancel) == 0) {
            return;
        }
        const auto deadline = clock::now() + timeout;
        PostgresPollingStatusType polling = PGRES_POLLING_WRITING;
        for (;;) {
            const bool want_read = (polling == PGRES_POLLING_READING);
            if (!wait_for_descriptor(
                    PQcancelSocket(raw_cancel), want_read, deadline)) {
                break;
            }
            polling = PQcancelPoll(raw_cancel);
            if (polling == PGRES_POLLING_OK || polling == PGRES_POLLING_FAILED) {
                break;
            }
        }
    }

    // Drain unsupported COPY output until it ends or the cleanup deadline passes.
    void discard_copy_output(clock::duration timeout) noexcept {
        const auto deadline = clock::now() + timeout;
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
                if (!wait_for_connection(
                        handle_, /*want_read=*/true, deadline)) {
                    return;
                }
                if (PQconsumeInput(handle_) == 0) {
                    return;
                }
            }
        }
    }

    // Try to discard unread results after cancellation or an error. Give up
    // when the connection fails or the cleanup deadline passes.
    void drain_pending_results(clock::duration timeout) noexcept {
        const auto deadline = clock::now() + timeout;
        for (;;) {
            if (clock::now() >= deadline) {
                return;
            }
            if (PQconsumeInput(handle_) == 0) {
                return;
            }
            while (PQisBusy(handle_) == 0) {
                PGresult* raw_result = PQgetResult(handle_);
                if (raw_result == nullptr) {
                    return;
                }
                std::unique_ptr<PGresult, decltype(&PQclear)> result_owner{
                    raw_result, &PQclear};
                if (PQresultStatus(raw_result) == PGRES_COPY_OUT
                    || PQresultStatus(raw_result) == PGRES_COPY_BOTH) {
                    result_owner.reset();
                    discard_copy_output(deadline - clock::now());
                    continue;
                }
            }
            if (!wait_for_connection(
                    handle_, /*want_read=*/true, deadline)) {
                return;
            }
        }
    }

    // Attempt ROLLBACK and drain its results within the cleanup timeout.
    void rollback_transaction(clock::duration timeout) noexcept {
        if (PQsendQuery(handle_, "ROLLBACK") == 0) {
            return;
        }
        const auto deadline = clock::now() + timeout;
        while (PQflush(handle_) == 1) {
            if (!wait_for_connection(
                    handle_, /*want_read=*/false, deadline)) {
                return;
            }
        }
        drain_pending_results(deadline - clock::now());
    }

    // Run the command string and return its last collected result. Turn a
    // returned server or transaction-policy error into a command_error exception.
    [[nodiscard]] command_result execute_statement(
        std::string_view sql,
        clock::duration timeout,
        const result_limits& limits,
        transaction_policy policy,
        timed_command_result* timing = nullptr) {
        command_batch_result outcome = execute_statement_batch(
            sql, timeout, limits, policy, timing);
        if (outcome.error) {
            throw *outcome.error;
        }
        return outcome.completed.empty()
            ? command_result{}
            : std::move(outcome.completed.back());
    }

    // Send SQL and collect results using row chunks or single-row timing.
    // Keep completed statements in order and discard rows from a failed
    // statement. Return its error alongside earlier results.
    //
    // Client failures throw. Size limits request cancellation and return the
    // collected data, including the row or result set that crossed a limit.
    // Apply the transaction policy after normal result processing.
    [[nodiscard]] command_batch_result execute_statement_batch(
        std::string_view sql,
        clock::duration timeout,
        const result_limits& limits,
        transaction_policy policy,
        timed_command_result* timing = nullptr) {
        const auto started = clock::now();
        const auto deadline = started + timeout;
        if (timing != nullptr) {
            timing->time_to_first_row.reset();
            timing->elapsed = std::chrono::nanoseconds::zero();
        }
        const std::string owned(sql);
        if (PQsendQuery(handle_, owned.c_str()) == 0) {
            throw command_error(
                "failed to send command: " + connection_error_message(),
                std::string{});
        }
        const int row_mode_enabled =
            timing != nullptr
                ? PQsetSingleRowMode(handle_)
                : PQsetChunkedRowsMode(handle_, row_chunk_size);
        if (row_mode_enabled == 0) {
            cancel_command(recovery_timeout);
            drain_pending_results(recovery_timeout);
            throw command_error(
                timing != nullptr
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
                throw command_error(
                    "failed to flush command: "
                        + connection_error_message(),
                    std::string{});
            }
            if (!wait_for_connection(
                    handle_, /*want_read=*/false, deadline)) {
                cancel_command(recovery_timeout);
                drain_pending_results(recovery_timeout);
                throw command_error(
                    "client deadline exceeded while sending", "57014");
            }
        }

        command_batch_result outcome;
        std::optional<error_fields> error;
        std::size_t total_bytes = 0;
        bool have_chunk_columns = false;
        bool limited = false;
        command_result provisional;

        // Save the result, then report whether it crossed the result-set limit.
        auto commit = [&outcome, &limits](command_result&& completed) -> bool {
            outcome.completed.push_back(std::move(completed));
            return limits.max_result_sets != 0
                && outcome.completed.size() > limits.max_result_sets;
        };

        for (;;) {
            while (PQisBusy(handle_) != 0) {
                if (!wait_for_connection(
                        handle_, /*want_read=*/true, deadline)) {
                    cancel_command(recovery_timeout);
                    drain_pending_results(recovery_timeout);
                    throw command_error(
                        "client deadline exceeded awaiting result", "57014");
                }
                if (PQconsumeInput(handle_) == 0) {
                    throw command_error(
                        "connection lost while reading result: "
                            + connection_error_message(),
                        std::string{});
                }
            }
            PGresult* raw_result = PQgetResult(handle_);
            if (raw_result == nullptr) {
                break;
            }
            std::unique_ptr<PGresult, decltype(&PQclear)> result_owner{
                raw_result, &PQclear};
            const ExecStatusType raw_status = PQresultStatus(raw_result);
            bool capped = false;
            if (raw_status == PGRES_SINGLE_TUPLE
                || raw_status == PGRES_TUPLES_CHUNK) {
                if (timing != nullptr
                    && !timing->time_to_first_row
                    && PQntuples(raw_result) > 0) {
                    timing->time_to_first_row =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            clock::now() - started);
                }
                append_result_fragment(
                    provisional,
                    raw_result,
                    /*copy_rows=*/true,
                    limits,
                    total_bytes,
                    limited);
                have_chunk_columns = true;
            } else if (raw_status == PGRES_TUPLES_OK) {
                if (timing != nullptr
                    && !timing->time_to_first_row
                    && PQntuples(raw_result) > 0) {
                    timing->time_to_first_row =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            clock::now() - started);
                }
                command_result current;
                current.status_ = result_status::rows_complete;
                if (PQntuples(raw_result) == 0 && have_chunk_columns) {
                    // The final empty result confirms that the accumulated rows are complete.
                    current.columns_ = std::move(provisional.columns_);
                    current.rows_ = std::move(provisional.rows_);
                } else {
                    // Read a result that arrived without chunks, including an empty SELECT.
                    append_result_fragment(
                        current,
                        raw_result,
                        /*copy_rows=*/true,
                        limits,
                        total_bytes,
                        limited);
                }
                current.command_tag_ = PQcmdStatus(raw_result)
                    ? PQcmdStatus(raw_result)
                    : std::string{};
                provisional = command_result{};
                have_chunk_columns = false;
                capped = commit(std::move(current));
            } else if (raw_status == PGRES_COMMAND_OK) {
                command_result current;
                current.status_ = result_status::command_complete;
                current.command_tag_ = PQcmdStatus(raw_result)
                    ? PQcmdStatus(raw_result)
                    : std::string{};
                current.affected_row_count_ =
                    parse_affected_row_count(raw_result);
                provisional = command_result{};
                have_chunk_columns = false;
                capped = commit(std::move(current));
            } else if (raw_status == PGRES_COPY_IN) {
                PQputCopyEnd(
                    handle_, "bioseq libpq client does not accept COPY input");
                result_owner.reset();
                cancel_command(recovery_timeout);
                drain_pending_results(recovery_timeout);
                throw command_error(
                    "COPY FROM STDIN is unsupported by this client", "0A000");
            } else if (raw_status == PGRES_COPY_OUT
                       || raw_status == PGRES_COPY_BOTH) {
                result_owner.reset();
                cancel_command(recovery_timeout);
                discard_copy_output(recovery_timeout);
                drain_pending_results(recovery_timeout);
                throw command_error(
                    "COPY TO STDOUT is unsupported by this client", "0A000");
            } else if (raw_status == PGRES_FATAL_ERROR
                       || raw_status == PGRES_BAD_RESPONSE) {
                // Drop rows from the failed statement and keep earlier completed results.
                provisional = command_result{};
                have_chunk_columns = false;
                error = read_error_fields(raw_result);
            }
            result_owner.reset();
            if (limited || capped) {
                // Keep the data that crossed the limit, cancel further work and let the
                // caller inspect the returned sizes.
                if (limited
                    && (!provisional.columns_.empty() || !provisional.rows_.empty())) {
                    command_result current;
                    current.status_ = result_status::rows_complete;
                    current.columns_ = std::move(provisional.columns_);
                    current.rows_ = std::move(provisional.rows_);
                    outcome.completed.push_back(std::move(current));
                }
                cancel_command(recovery_timeout);
                drain_pending_results(recovery_timeout);
                if (timing != nullptr) {
                    timing->elapsed =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            clock::now() - started);
                }
                return outcome;
            }
        }

        if (policy == transaction_policy::require_idle
            && PQtransactionStatus(handle_) != PQTRANS_IDLE) {
            rollback_transaction(recovery_timeout);
            if (!error) {
                error = error_fields{
                    .sql_state = "25000",
                    .primary = "command left the connection in an open transaction",
                    .detail = {},
                    .hint = {},
                    .severity = "ERROR"};
            }
        }
        if (error) {
            outcome.error = command_error(
                error->primary,
                error->sql_state,
                error->detail,
                error->hint,
                error->severity);
        }
        if (timing != nullptr) {
            timing->elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    clock::now() - started);
        }
        return outcome;
    }

    static void receive_notice(void* self, const PGresult* raw) noexcept {
        auto* owner = static_cast<client_connection*>(self);
        try {
            error_fields fields = read_error_fields(raw);
            owner->notices_.push_back(server_notice{
                .severity = std::move(fields.severity),
                .sql_state = std::move(fields.sql_state),
                .message = std::move(fields.primary)});
        } catch (...) {
            // Discard the notice if copying it fails. Exceptions must not leave this
            // libpq callback.
        }
    }

    PGconn* handle_{};
    std::vector<server_notice> notices_;
    command_observer* observer_{};
    std::string observer_context_;
};

/// Build connection parameters for the given Unix socket directory.
[[nodiscard]] std::string make_unix_socket_conninfo(
    std::string_view socket_directory,
    std::string_view database,
    std::string_view user,
    int port) {
    const auto quote_conninfo_value = [](std::string_view value) {
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
    std::string connection_parameters;
    connection_parameters +=
        "host=" + quote_conninfo_value(socket_directory);
    connection_parameters += " port=" + std::to_string(port);
    connection_parameters += " dbname=" + quote_conninfo_value(database);
    connection_parameters += " user=" + quote_conninfo_value(user);
    connection_parameters += " sslmode=disable";
    return connection_parameters;
}

}  // namespace bioseq::libpq
