module;

#include <libpq-fe.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
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
export namespace bioseq::libpq {

/// @brief A libpq error carrying the failing SQLSTATE when PostgreSQL reports one.
class query_error : public std::runtime_error {
public:
    query_error(std::string message, std::string sqlstate)
        : std::runtime_error(std::move(message)), sqlstate_(std::move(sqlstate)) {}

    /// @brief Five-character SQLSTATE, or empty for a client-side failure.
    [[nodiscard]] std::string_view sqlstate() const noexcept { return sqlstate_; }

private:
    std::string sqlstate_;
};

/// @brief One captured server notice/warning during command execution.
struct notice {
    std::string severity;
    std::string sqlstate;
    std::string message;
};

/// @brief Owned result set with typed accessors and no libpq exposure.
class result {
public:
    explicit result(PGresult* handle) noexcept : handle_(handle) {}
    result(result&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    result& operator=(result&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    result(const result&) = delete;
    result& operator=(const result&) = delete;
    ~result() { reset(); }

    [[nodiscard]] int rows() const noexcept { return handle_ ? PQntuples(handle_) : 0; }
    [[nodiscard]] int columns() const noexcept { return handle_ ? PQnfields(handle_) : 0; }

    [[nodiscard]] std::string column_name(int column) const {
        const char* name = PQfname(handle_, column);
        return name ? std::string(name) : std::string{};
    }
    [[nodiscard]] unsigned int column_type_oid(int column) const {
        return static_cast<unsigned int>(PQftype(handle_, column));
    }
    [[nodiscard]] bool is_null(int row, int column) const noexcept {
        return PQgetisnull(handle_, row, column) != 0;
    }
    [[nodiscard]] std::string_view value(int row, int column) const {
        const char* text = PQgetvalue(handle_, row, column);
        const int length = PQgetlength(handle_, row, column);
        return text ? std::string_view(text, static_cast<std::size_t>(length))
                    : std::string_view{};
    }

    /// @brief PostgreSQL command tag such as "SELECT 3" or "CREATE EXTENSION".
    [[nodiscard]] std::string command_tag() const {
        const char* tag = PQcmdTuples(handle_);
        const char* status = PQcmdStatus(handle_);
        return status ? std::string(status) : (tag ? std::string(tag) : std::string{});
    }
    [[nodiscard]] ExecStatusType status() const noexcept {
        return handle_ ? PQresultStatus(handle_) : PGRES_FATAL_ERROR;
    }
    [[nodiscard]] bool has_rows() const noexcept { return status() == PGRES_TUPLES_OK; }

    /// @brief The five-character SQLSTATE of a failed result, else empty.
    [[nodiscard]] std::string error_sqlstate() const {
        if (handle_ != nullptr) {
            if (const char* sqlstate =
                    PQresultErrorField(handle_, PG_DIAG_SQLSTATE)) {
                return std::string(sqlstate);
            }
        }
        return {};
    }

private:
    void reset() noexcept {
        if (handle_) {
            PQclear(handle_);
            handle_ = nullptr;
        }
    }
    PGresult* handle_{};
};

/// @brief RAII PostgreSQL connection over a Unix-socket-only conninfo string.
///
/// Constructed from an explicit key=value conninfo so callers never build the
/// string outside their owning policy module; notices are captured into an
/// owned buffer that the caller drains after each command.
class connection {
public:
    explicit connection(const std::string& conninfo)
        : handle_(PQconnectdb(conninfo.c_str())) {
        if (handle_ == nullptr || PQstatus(handle_) != CONNECTION_OK) {
            std::string message = handle_ ? PQerrorMessage(handle_)
                                          : "libpq: out of memory";
            if (handle_) {
                PQfinish(handle_);
                handle_ = nullptr;
            }
            throw query_error("connection failed: " + message, std::string{});
        }
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

    /// @brief Runs a command, throwing query_error with SQLSTATE on failure.
    [[nodiscard]] result exec(std::string_view sql) {
        result outcome(PQexec(handle_, std::string(sql).c_str()));
        const ExecStatusType status = outcome.status();
        if (status == PGRES_FATAL_ERROR || status == PGRES_BAD_RESPONSE) {
            throw query_error(current_error_message(), outcome.error_sqlstate());
        }
        return outcome;
    }

    /// @brief Drains and clears notices captured since the last drain.
    [[nodiscard]] std::vector<notice> take_notices() {
        return std::exchange(notices_, {});
    }

    /// @brief Quotes an SQL identifier via libpq — never string interpolation.
    /// @param name Raw identifier (role/database name) to quote for a DDL command.
    /// @return The escaped, double-quoted identifier safe to concatenate.
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
    [[nodiscard]] std::string current_error_message() const {
        const char* message = PQerrorMessage(handle_);
        return message ? std::string(message) : std::string{};
    }

    static void receive_notice(void* self, const PGresult* raw) {
        auto* owner = static_cast<connection*>(self);
        notice captured;
        if (const char* severity = PQresultErrorField(raw, PG_DIAG_SEVERITY_NONLOCALIZED)) {
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
