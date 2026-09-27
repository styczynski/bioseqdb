/**
 * @file safety_runtime_harness_core_support.cppm
 * @brief Shared JSON, file, and PostgreSQL helpers for runtime-safety tests.
 *
 * Also compares the server's Bioseq settings with their generated definitions.
 */

module;

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spawn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef BIOSEQ_SAFETY_RUNTIME_BINDINGS_HEADER
#error "BIOSEQ_SAFETY_RUNTIME_BINDINGS_HEADER must name generated bindings"
#endif

#include BIOSEQ_SAFETY_RUNTIME_BINDINGS_HEADER

#if BIOSEQ_SAFETY_RUNTIME_BINDING_VERSION != 2
#error "unsupported generated safety runtime binding version"
#endif

#ifndef BIOSEQ_SAFETY_RUNTIME_PROFILE
#error "BIOSEQ_SAFETY_RUNTIME_PROFILE must be production or test_indexes"
#endif

#ifndef BIOSEQ_SAFETY_RUNTIME_CASE_PLAN
#error "BIOSEQ_SAFETY_RUNTIME_CASE_PLAN must name a checked case plan"
#endif

#ifndef BIOSEQ_SAFETY_RUNTIME_CASE_PLAN_SHA256
#error "BIOSEQ_SAFETY_RUNTIME_CASE_PLAN_SHA256 must seal the case plan"
#endif

#ifndef BIOSEQ_SAFETY_RUNTIME_CONTROL_FILE
#error "BIOSEQ_SAFETY_RUNTIME_CONTROL_FILE must name bioseq.control"
#endif

#ifndef BIOSEQ_SAFETY_RUNTIME_SQL_FILE
#error "BIOSEQ_SAFETY_RUNTIME_SQL_FILE must name the generated install SQL"
#endif

#ifndef BIOSEQ_SAFETY_RUNTIME_SQL_MANIFEST
#error "BIOSEQ_SAFETY_RUNTIME_SQL_MANIFEST must name the generated SQL manifest"
#endif

#ifndef BIOSEQ_SAFETY_RUNTIME_CONTROL_DSO
#error "BIOSEQ_SAFETY_RUNTIME_CONTROL_DSO must name the SQL-control helper DSO"
#endif

export module bioseq.tests.integration.safety_runtime_harness_support:core_support;

export namespace bioseq::tests::integration::safety_runtime_harness {

namespace fs = std::filesystem;
using json = nlohmann::json;

inline constexpr std::string_view k_profile = BIOSEQ_SAFETY_RUNTIME_PROFILE;
inline constexpr std::string_view k_case_plan_path =
    BIOSEQ_SAFETY_RUNTIME_CASE_PLAN;
inline constexpr std::string_view k_case_plan_sha256 =
    BIOSEQ_SAFETY_RUNTIME_CASE_PLAN_SHA256;
inline constexpr std::string_view k_control_file =
    BIOSEQ_SAFETY_RUNTIME_CONTROL_FILE;
inline constexpr std::string_view k_sql_file =
    BIOSEQ_SAFETY_RUNTIME_SQL_FILE;
inline constexpr std::string_view k_sql_manifest =
    BIOSEQ_SAFETY_RUNTIME_SQL_MANIFEST;
inline constexpr std::string_view k_control_dso =
    BIOSEQ_SAFETY_RUNTIME_CONTROL_DSO;
inline constexpr std::size_t k_maximum_sql_statements = 64;
inline constexpr std::size_t k_maximum_sql_bytes = 1024 * 1024;
inline constexpr std::string_view k_authorization_marker =
    "bioseq.postgres_error_safety.authorization_marker\nrevision=2\n";
inline constexpr std::string_view k_backend_scoped_vacuum_fault_key =
    "invoke_pg_as_cpp.vacuum_cleanup_restore";

/// Throw a harness error containing the failed operation and its message.
[[noreturn]] void fail(std::string_view context, std::string_view message) {
    throw std::runtime_error{
        fmt::format("{}: {}", context, message)};
}

/// Read the  whole file, or fail if it cannot be read completely.
[[nodiscard]] std::string read_file(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        fail(path.string(), "cannot open file");
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        fail(path.string(), "cannot determine file size");
    }
    input.seekg(0, std::ios::beg);
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!result.empty()) {
        input.read(result.data(), size);
    }
    if (!input) {
        fail(path.string(), "cannot read complete file");
    }
    return result;
}

/// Encode a byte sequence as lowercase hexadecimal.
[[nodiscard]] std::string hex_digest(
        const unsigned char* bytes,
        std::size_t size) {
    constexpr auto digits = std::to_array("0123456789abcdef");
    std::string result(size * 2, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = digits[bytes[index] >> 4U];
        result[index * 2 + 1] = digits[bytes[index] & 0x0fU];
    }
    return result;
}

/// Hash an in-memory byte sequence with SHA-256.
[[nodiscard]] std::string sha256_bytes(std::string_view bytes) {
    using evp_digest_context =
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    evp_digest_context context{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!context
        || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1
        || EVP_DigestUpdate(
               context.get(), bytes.data(), bytes.size()) != 1) {
        fail("sha256", "OpenSSL digest initialization failed");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(
            context.get(), digest.data(), &digest_size) != 1
        || digest_size != 32) {
        fail("sha256", "OpenSSL digest finalization failed");
    }
    return hex_digest(digest.data(), digest_size);
}

/// Hash a complete file with SHA-256.
[[nodiscard]] std::string sha256_file(const fs::path& path) {
    return sha256_bytes(read_file(path));
}

/// Check for a 64-digit lowercase hexadecimal SHA-256 representation.
[[nodiscard]] bool lowercase_sha256(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

/// Parse arbitrary JSON while rejecting duplicate object keys.
[[nodiscard]] json parse_json_value(
        std::string_view bytes,
        std::string_view context) {
    bool duplicate_key = false;
    std::vector<std::set<std::string, std::less<>>> keys_by_depth;
    const json::parser_callback_t callback =
        [&duplicate_key, &keys_by_depth](
            int depth,
            json::parse_event_t event,
            json& parsed) {
            if (depth < 0) {
                return true;
            }
            const std::size_t current_depth =
                static_cast<std::size_t>(depth);
            if (keys_by_depth.size() <= current_depth) {
                keys_by_depth.resize(current_depth + 1);
            }
            if (event == json::parse_event_t::object_start) {
                keys_by_depth[current_depth].clear();
            } else if (event == json::parse_event_t::key) {
                const std::string key = parsed.get<std::string>();
                if (!keys_by_depth[current_depth].insert(key).second) {
                    duplicate_key = true;
                }
            }
            return true;
        };
    try {
        json result = json::parse(
            bytes.begin(), bytes.end(), callback, true, false);
        if (duplicate_key) {
            fail(context, "contains a duplicate JSON key");
        }
        return result;
    } catch (const json::exception& error) {
        fail(context, error.what());
    }
}

/// Parse a top-level JSON object while rejecting duplicate keys.
[[nodiscard]] json parse_json(
        std::string_view bytes,
        std::string_view context) {
    json result = parse_json_value(bytes, context);
    if (!result.is_object()) {
        fail(context, "top-level JSON value must be an object");
    }
    return result;
}

/// Load and parse a top-level JSON object from disk.
[[nodiscard]] json load_json(
        const fs::path& path,
        std::string_view context) {
    return parse_json(read_file(path), context);
}

/// Serialize cmpact JSON with a final newline for repeatable file hashes.
[[nodiscard]] std::string canonical_json(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::strict)
        + "\n";
}

/// Write through a temporary file beside the destination, then rename it into place.
void write_atomic(const fs::path& path, std::string_view bytes) {
    if (!path.is_absolute()) {
        fail(path.string(), "output path must be absolute");
    }
    fs::create_directories(path.parent_path());
    std::array<unsigned char, 8> random_bytes{};
    if (RAND_bytes(
            random_bytes.data(),
            static_cast<int>(random_bytes.size())) != 1) {
        fail(path.string(), "cannot generate temporary-file nonce");
    }
    const fs::path temporary =
        path.parent_path()
        / (path.filename().string() + ".tmp."
           + hex_digest(random_bytes.data(), random_bytes.size()));
    {
        std::ofstream output{
            temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            fail(temporary.string(), "cannot create temporary output");
        }
        output.write(
            bytes.data(),
            static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            fail(temporary.string(), "cannot publish complete output");
        }
    }
    if (::rename(
            temporary.c_str(), path.c_str()) != 0) {
        const int error = errno;
        std::error_code ignored;
        fs::remove(temporary, ignored);
        fail(
            path.string(),
            fmt::format(
                "atomic rename failed: {}", std::strerror(error)));
    }
}

/// Return a required JSON field, reporting its context if the field is missing.
[[nodiscard]] const json& require_field(
        const json& object,
        std::string_view field,
        std::string_view context) {
    if (!object.is_object()) {
        fail(context, "expected an object");
    }
    const auto found = object.find(std::string{field});
    if (found == object.end()) {
        fail(context, fmt::format("missing field '{}'", field));
    }
    return *found;
}

/// Require an object to contain exactly the declared field set.
void require_exact_fields(
        const json& object,
        std::initializer_list<std::string_view> fields,
        std::string_view context) {
    if (!object.is_object()) {
        fail(context, "expected an object");
    }
    std::set<std::string, std::less<>> expected;
    for (const std::string_view field : fields) {
        expected.emplace(field);
    }
    std::set<std::string, std::less<>> actual;
    for (const auto& [field, value] : object.items()) {
        static_cast<void>(value);
        actual.emplace(field);
    }
    if (actual == expected) {
        return;
    }
    std::vector<std::string> missing;
    std::vector<std::string> unknown;
    std::ranges::set_difference(
        expected,
        actual,
        std::back_inserter(missing));
    std::ranges::set_difference(
        actual,
        expected,
        std::back_inserter(unknown));
    fail(
        context,
        fmt::format(
            "exact fields differ (missing=[{}], unknown=[{}])",
            fmt::join(missing, ","),
            fmt::join(unknown, ",")));
}

/// Reject fields outside the allowed set, while permitting allowed fields to be absent.
void require_known_fields(
        const json& object,
        std::initializer_list<std::string_view> fields,
        std::string_view context) {
    if (!object.is_object()) {
        fail(context, "expected an object");
    }
    std::set<std::string, std::less<>> known;
    for (const std::string_view field : fields) {
        known.emplace(field);
    }
    std::vector<std::string> unknown;
    for (const auto& [field, value] : object.items()) {
        static_cast<void>(value);
        if (!known.contains(field)) {
            unknown.push_back(field);
        }
    }
    if (!unknown.empty()) {
        fail(
            context,
            fmt::format(
                "unknown fields: {}", fmt::join(unknown, ",")));
    }
}

/// Read a required nonempty JSON string.
[[nodiscard]] std::string require_string(
        const json& value,
        std::string_view context) {
    if (!value.is_string()) {
        fail(context, "expected a string");
    }
    const std::string result = value.get<std::string>();
    if (result.empty()) {
        fail(context, "expected a non-empty string");
    }
    return result;
}

/// Read a required nonempty string field from a JSON object.
[[nodiscard]] std::string require_string_field(
        const json& object,
        std::string_view field,
        std::string_view context) {
    return require_string(
        require_field(object, field, context),
        fmt::format("{}.{}", context, field));
}

/// Parse an unsigned decimal integer and reject, invalid digits or  overflow.
[[nodiscard]] std::uint64_t parse_unsigned(
        std::string_view value,
        std::string_view context) {
    if (value.empty()) {
        fail(context, "expected an unsigned integer");
    }
    std::uint64_t result = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            fail(context, "expected an unsigned integer");
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - '0');
        if (result
            > (std::numeric_limits<std::uint64_t>::max() - digit)
                / 10) {
            fail(context, "unsigned integer overflows");
        }
        result = result * 10 + digit;
    }
    return result;
}

/// Generate a random 64-digit lowercase hexadecimal token with OpenSSL.
[[nodiscard]] std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(
            bytes.data(), static_cast<int>(bytes.size())) != 1) {
        fail("runtime token", "OpenSSL random generation failed");
    }
    return hex_digest(bytes.data(), bytes.size());
}

/// Own a libpq result and clear it when its owner is destroyed.
struct pg_result final {
    explicit pg_result(PGresult* value) noexcept
        : value_{value} {}

    pg_result(const pg_result&) = delete;
    pg_result& operator=(const pg_result&) = delete;

    pg_result(pg_result&& other) noexcept
        : value_{std::exchange(other.value_, nullptr)} {}

    pg_result& operator=(pg_result&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) {
                PQclear(value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    ~pg_result() {
        if (value_ != nullptr) {
            PQclear(value_);
        }
    }

    [[nodiscard]] PGresult* get() const noexcept {
        return value_;
    }

private:
    PGresult* value_ = nullptr;
};

/// Hold the connection fields accepted by the private-cluster harness.
struct connection_parameters final {
    std::string host;
    std::string port;
    std::string database;
    std::string user;
    std::string password;
};

/// Read the required nonempty connection fields from the cluster configuration.
[[nodiscard]] connection_parameters parse_connection(
        const json& config) {
    const json& value = require_field(
        config, "connection", "cluster-config");
    return connection_parameters{
        .host = require_string_field(
            value, "host", "cluster-config.connection"),
        .port = require_string_field(
            value, "port", "cluster-config.connection"),
        .database = require_string_field(
            value, "database", "cluster-config.connection"),
        .user = require_string_field(
            value, "user", "cluster-config.connection"),
        .password = require_string_field(
            value, "password", "cluster-config.connection"),
    };
}

/// Own one libpq connection used by the standalone safety harness.
class pg_connection final {
public:
    explicit pg_connection(
            const connection_parameters& parameters) {
        const auto keywords = std::to_array<const char*>({
            "host",
            "port",
            "dbname",
            "user",
            "password",
            "application_name",
            nullptr,
        });
        const auto values = std::to_array<const char*>({
            parameters.host.c_str(),
            parameters.port.c_str(),
            parameters.database.c_str(),
            parameters.user.c_str(),
            parameters.password.c_str(),
            "bioseq-safety-runtime",
            nullptr,
        });
        value_ = PQconnectdbParams(keywords.data(), values.data(), 0);
        if (value_ == nullptr || PQstatus(value_) != CONNECTION_OK) {
            const std::string message =
                value_ == nullptr
                ? "PQconnectdbParams returned null"
                : PQerrorMessage(value_);
            if (value_ != nullptr) {
                PQfinish(value_);
                value_ = nullptr;
            }
            fail("postgres connection", message);
        }
    }

    pg_connection(const pg_connection&) = delete;
    pg_connection& operator=(const pg_connection&) = delete;

    ~pg_connection() {
        if (value_ != nullptr) {
            PQfinish(value_);
        }
    }

    /// Execute SQL and return an owned libpq result.
    [[nodiscard]] pg_result execute(std::string_view sql) const {
        return pg_result{
            PQexec(value_, std::string{sql}.c_str())};
    }

    /// Execute one parameterized SQL statement with text parameters.
    [[nodiscard]] pg_result execute_parameters(
            std::string_view sql,
            std::span<const std::string> parameters) const {
        std::vector<const char*> values;
        values.reserve(parameters.size());
        for (const std::string& parameter : parameters) {
            values.push_back(parameter.c_str());
        }
        return pg_result{PQexecParams(
            value_,
            std::string{sql}.c_str(),
            static_cast<int>(values.size()),
            nullptr,
            values.data(),
            nullptr,
            nullptr,
            0)};
    }

    /// Return the server process identifier for this connection.
    [[nodiscard]] int backend_pid() const noexcept {
        return PQbackendPID(value_);
    }

    /// Return the current libpq connection status.
    [[nodiscard]] ConnStatusType connection_status() const noexcept {
        return value_ == nullptr
            ? CONNECTION_BAD
            : PQstatus(value_);
    }

    /// Return the current libpq transaction status.
    [[nodiscard]] PGTransactionStatusType
    transaction_status() const noexcept {
        return PQtransactionStatus(value_);
    }

private:
    PGconn* value_ = nullptr;
};

/// Return whether libpq reports a command or tuples result.
[[nodiscard]] bool command_success(const pg_result& result) noexcept {
    return result.get() != nullptr
        && (PQresultStatus(result.get()) == PGRES_COMMAND_OK
            || PQresultStatus(result.get()) == PGRES_TUPLES_OK);
}

/// Fail on an unsuccessful result, including the libpq diagnostic in the error
void require_success(
        const pg_result& result,
        std::string_view context) {
    if (!command_success(result)) {
        fail(
            context,
            result.get() == nullptr
            ? "PQexec returned null"
            : PQresultErrorMessage(result.get()));
    }
}

/// Copy all query rows as text, preserving SQL NULL cells.
[[nodiscard]] std::vector<std::vector<std::optional<std::string>>>
rows(const pg_result& result, std::string_view context) {
    if (result.get() == nullptr
        || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        fail(
            context,
            result.get() == nullptr
            ? "PQexec returned null"
            : PQresultErrorMessage(result.get()));
    }
    std::vector<std::vector<std::optional<std::string>>> output;
    const int row_count = PQntuples(result.get());
    const int column_count = PQnfields(result.get());
    output.reserve(static_cast<std::size_t>(row_count));
    for (int row_index = 0;
         row_index < row_count;
         ++row_index) {
        std::vector<std::optional<std::string>> row;
        row.reserve(static_cast<std::size_t>(column_count));
        for (int column_index = 0;
             column_index < column_count;
             ++column_index) {
            if (PQgetisnull(
                    result.get(), row_index, column_index)) {
                row.emplace_back(std::nullopt);
            } else {
                row.emplace_back(
                    PQgetvalue(
                        result.get(), row_index, column_index));
            }
        }
        output.push_back(std::move(row));
    }
    return output;
}

/// Read exactly one result row, preserving SQL NULL cells.
[[nodiscard]] std::vector<std::optional<std::string>> one_row(
        const pg_result& result,
        std::string_view context) {
    auto result_rows = rows(result, context);
    if (result_rows.size() != 1) {
        fail(context, "expected exactly one row");
    }
    return std::move(result_rows.front());
}

/// Return one required nonnull result cell.
[[nodiscard]] std::string nonnull_cell(
        const std::vector<std::optional<std::string>>& row,
        std::size_t index,
        std::string_view context) {
    if (index >= row.size() || !row[index].has_value()) {
        fail(context, "required result cell is null or absent");
    }
    return *row[index];
}

/// Expected total and test-only setting counts for a build profile.
struct guc_profile_counts final {
    std::uint64_t total = 0;
    std::uint64_t test_only = 0;
};

/// Generated definitions of the Bioseq settings for this executable's profile.
struct guc_manifest_contract final {
    std::string profile;
    std::string registry_module;
    std::string registry_content_sha256;
    std::string profile_delta_validation_sha256;
    std::map<std::string, json, std::less<>> owner_facets;
    std::vector<json> registrations;
};

/// Checked server settings, their counts, and  a hash of the recorded properties.
struct installed_guc_profile final {
    guc_profile_counts counts;
    json properties;
    std::string properties_sha256;
};

/// Return the exact GUC counts for one supported build profile.
[[nodiscard]] guc_profile_counts expected_guc_profile(
        std::string_view profile) {
    if (profile == "production") {
        return {
            .total = 73,
            .test_only = 0,
        };
    }
    if (profile == "test_indexes") {
        return {
            .total = 76,
            .test_only = 3,
        };
    }
    fail(
        "installed GUC profile",
        fmt::format("unsupported profile '{}'", profile));
}

/// Map a generated setting type to the name used by pg_settings.
[[nodiscard]] std::string expected_pg_guc_kind(
        std::string_view manifest_kind) {
    if (manifest_kind == "bool") {
        return "bool";
    }
    if (manifest_kind == "integer") {
        return "integer";
    }
    if (manifest_kind == "real") {
        return "real";
    }
    if (manifest_kind == "string") {
        return "string";
    }
    if (manifest_kind == "enum") {
        return "enum";
    }
    fail(
        "GUC manifest",
        fmt::format("unknown GUC kind '{}'", manifest_kind));
}

/// Map a generated setting type to its C++ storage-type label.
[[nodiscard]] std::string expected_guc_storage_kind(
        std::string_view manifest_kind) {
    if (manifest_kind == "bool") {
        return "bool";
    }
    if (manifest_kind == "integer") {
        return "int32";
    }
    if (manifest_kind == "real") {
        return "double";
    }
    if (manifest_kind == "string") {
        return "string";
    }
    if (manifest_kind == "enum") {
        return "int32_enum";
    }
    fail(
        "GUC manifest",
        fmt::format("unknown GUC kind '{}'", manifest_kind));
}

/// Map a generated setting context to the name used by pg_settings.
[[nodiscard]] std::string expected_pg_guc_context(
        std::string_view manifest_context) {
    if (manifest_context == "sighup") {
        return "sighup";
    }
    if (manifest_context == "superuser_set") {
        return "superuser";
    }
    if (manifest_context == "user_set") {
        return "user";
    }
    fail(
        "GUC manifest",
        fmt::format(
            "unknown GUC context '{}'", manifest_context));
}

/// Build the profile metadata stored alongside one owner's setting definitions.
[[nodiscard]] json guc_profile_projection(
        const guc_manifest_contract& manifest,
        const json& owner_facet) {
    return json{
        {"profile", manifest.profile},
        {"registry_module", manifest.registry_module},
        {"registry_content_sha256",
         manifest.registry_content_sha256},
        {"profile_delta_validation_sha256",
         manifest.profile_delta_validation_sha256},
        {"owner_facet", owner_facet}};
}

/// Require the recorded servez properties to match the generated setting definition.
void validate_live_guc_property(
        const json& generated_live_authority,
        const json& live,
        std::string_view context) {
    require_exact_fields(
        live,
        {
            "vartype",
            "context",
            "unit",
            "boot_value",
            "reset_value",
            "current_value",
            "minimum",
            "maximum",
            "visible_enum_values",
            "short_description",
            "extra_description",
            "raw_flags",
            "visible_in_show_all",
            "included_in_sample",
            "hooks",
        },
        context);
    require_exact_fields(
        generated_live_authority,
        {
            "vartype",
            "context",
            "unit",
            "boot_value",
            "reset_value",
            "current_value",
            "minimum",
            "maximum",
            "visible_enum_values",
            "short_description",
            "extra_description",
            "raw_flags",
            "visible_in_show_all",
            "included_in_sample",
            "hooks",
        },
        fmt::format("{}.generated_live_authority", context));
    if (live != generated_live_authority) {
        fail(
            context,
            "live GUC projection differs from generated authority");
    }
}

/// Check that generated server properties agree with the setting's registration.
void validate_generated_live_guc_authority(
        const json& registration,
        const json& generated,
        std::string_view context) {
    validate_live_guc_property(generated, generated, context);
    const std::string kind =
        require_string_field(registration, "kind", context);
    const std::string source_context =
        require_string_field(registration, "context", context);
    const json& registration_unit =
        require_field(registration, "unit", context);
    json expected_unit = nullptr;
    if (!registration_unit.is_null()) {
        const std::string unit =
            require_string(registration_unit, context);
        expected_unit = unit == "mb" ? "MB" : unit;
    }
    if (require_string_field(generated, "vartype", context)
            != expected_pg_guc_kind(kind)
        || require_string_field(generated, "context", context)
            != expected_pg_guc_context(source_context)
        || require_field(generated, "unit", context) != expected_unit
        || require_field(generated, "reset_value", context)
            != require_field(generated, "boot_value", context)
        || require_field(generated, "current_value", context)
            != require_field(generated, "boot_value", context)) {
        fail(context, "generated live scalar authority differs");
    }
    const bool bounded = kind == "integer" || kind == "real";
    const json& minimum = require_field(generated, "minimum", context);
    const json& maximum = require_field(generated, "maximum", context);
    const json& visible =
        require_field(generated, "visible_enum_values", context);
    if ((bounded && (!minimum.is_string() || !maximum.is_string()))
        || (!bounded && (!minimum.is_null() || !maximum.is_null()))
        || !visible.is_array()
        || (kind == "enum"
            && (visible.empty()
                || !std::ranges::contains(
                    visible,
                    require_field(generated, "boot_value", context))))
        || (kind != "enum" && !visible.empty())) {
        fail(context, "generated live bound or enum authority differs");
    }
    std::uint64_t expected_flags = 0;
    const json& flags = require_field(registration, "flags", context);
    for (const json& raw_flag : flags) {
        const std::string flag = require_string(raw_flag, context);
        if (flag == "no_show_all") expected_flags |= 0x000004U;
        else if (flag == "not_in_sample") expected_flags |= 0x000080U;
        else fail(context, "generated live flag is unsupported");
    }
    if (!registration_unit.is_null()) {
        const std::string unit = registration_unit.get<std::string>();
        if (unit == "kb") expected_flags |= 0x01000000U;
        else if (unit == "blocks") expected_flags |= 0x02000000U;
        else if (unit == "mb") expected_flags |= 0x04000000U;
        else if (unit == "ms") expected_flags |= 0x10000000U;
        else if (unit == "s") expected_flags |= 0x20000000U;
        else if (unit == "min") expected_flags |= 0x30000000U;
        else fail(context, "generated live unit flag is unsupported");
    }
    const bool expected_show =
        (expected_flags & 0x000004U) == 0;
    const bool expected_sample =
        (expected_flags & 0x000080U) == 0;
    if (!require_field(generated, "raw_flags", context)
             .is_number_unsigned()
        && !require_field(generated, "raw_flags", context)
                .is_number_integer()) {
        fail(context, "generated live raw flags are malformed");
    }
    if (require_field(generated, "raw_flags", context)
                .get<std::uint64_t>()
            != expected_flags
        || require_field(
               generated, "visible_in_show_all", context)
                != expected_show
        || require_field(
               generated, "included_in_sample", context)
                != expected_sample) {
        fail(context, "generated live flag behavior differs");
    }
    const json& registration_hooks =
        require_field(registration, "hooks", context);
    const json& hooks = require_field(generated, "hooks", context);
    require_exact_fields(
        hooks, {"check", "assign", "show"}, context);
    static constexpr std::array<std::string_view, 3> names{
        "check", "assign", "show"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const json& hook = require_field(hooks, names[index], context);
        require_exact_fields(
            hook, {"symbol", "present", "behavior"}, context);
        const json& symbol = registration_hooks[index];
        const bool present = !symbol.is_null();
        if (require_field(hook, "symbol", context) != symbol
            || require_field(hook, "present", context) != present
            || require_string_field(hook, "behavior", context)
                != (present
                        ? fmt::format("custom_{}", names[index])
                        : "none")) {
            fail(context, "generated live hook behavior differs");
        }
    }
}

/// Surround text with SQL quotes and double embedded single quotes.
[[nodiscard]] std::string sql_literal(std::string_view value);

/// Load the control library and add a temporary SQL function  for inspecting settings.
void create_guc_contract_binding(
        const pg_connection& connection,
        const fs::path& control_dso) {
    if (!control_dso.is_absolute()
        || !fs::is_regular_file(control_dso)
        || fs::is_symlink(control_dso)) {
        fail(
            "safety runtime control DSO",
            "expected an absolute regular non-symlink helper");
    }
    const std::string library =
        sql_literal(fs::canonical(control_dso).string());
    require_success(
        connection.execute("LOAD " + library + ";"),
        "load GUC internal contract DSO");
    require_success(
        connection.execute(
            "CREATE OR REPLACE FUNCTION "
            "pg_temp.bioseq_safety_runtime_guc_internal_contract() "
            "RETURNS text "
            "AS " + library
            + ", 'bioseq_safety_runtime_guc_internal_contract' "
              "LANGUAGE C VOLATILE;"),
        "create GUC internal contract binding");
}

/// Internal flags and hook presence for one registered PostgreSQL setting.
struct internal_guc_property final {
    std::uint64_t raw_flags{};
    bool check_hook_present{};
    bool assign_hook_present{};
    bool show_hook_present{};
};

/// Parse the control library's setting list, rejecting malformed or duplicate rows.
[[nodiscard]] std::map<
    std::string,
    internal_guc_property,
    std::less<>>
parse_internal_guc_contract(std::string_view raw) {
    constexpr std::string_view header =
        "bioseq_guc_internal_contract\nrevision=1\n";
    if (!raw.starts_with(header)
        || raw.size() > 64 * 1024
        || raw.back() != '\n') {
        fail(
            "GUC internal contract",
            "revision, size, or final newline differs");
    }
    raw.remove_prefix(header.size());
    std::map<
        std::string,
        internal_guc_property,
        std::less<>>
        result;
    while (!raw.empty()) {
        const std::size_t newline = raw.find('\n');
        if (newline == std::string_view::npos || newline == 0) {
            fail("GUC internal contract", "row framing differs");
        }
        const std::string_view line = raw.substr(0, newline);
        raw.remove_prefix(newline + 1);
        std::array<std::string_view, 5> cells{};
        std::size_t offset = 0;
        for (std::size_t index = 0; index < cells.size(); ++index) {
            const std::size_t tab = line.find('\t', offset);
            if ((index + 1 == cells.size() && tab != std::string_view::npos)
                || (index + 1 != cells.size()
                    && tab == std::string_view::npos)) {
                fail(
                    "GUC internal contract",
                    "row does not have five exact fields");
            }
            cells[index] = index + 1 == cells.size()
                ? line.substr(offset)
                : line.substr(offset, tab - offset);
            offset = tab == std::string_view::npos
                ? line.size()
                : tab + 1;
        }
        if (!cells[0].starts_with("bioseq.")
            || cells[0].size() <= std::string_view{"bioseq."}.size()
            || cells[1].empty()
            || (cells[2] != "0" && cells[2] != "1")
            || (cells[3] != "0" && cells[3] != "1")
            || (cells[4] != "0" && cells[4] != "1")) {
            fail("GUC internal contract", "row value differs");
        }
        internal_guc_property property{
            .raw_flags = parse_unsigned(
                cells[1], "GUC internal raw flags"),
            .check_hook_present = cells[2] == "1",
            .assign_hook_present = cells[3] == "1",
            .show_hook_present = cells[4] == "1",
        };
        if (!result.emplace(std::string{cells[0]}, property).second
            || result.size() > 256) {
            fail(
                "GUC internal contract",
                "duplicate or excessive GUC row");
        }
    }
    return result;
}

/// Compare registered Bioseq settings with the generated manifest and record the result.
[[nodiscard]] installed_guc_profile verify_installed_guc_profile(
        const pg_connection& connection,
        const guc_manifest_contract& manifest) {
    std::map<std::string, std::size_t, std::less<>>
        manifest_index_by_name;
    std::set<std::string, std::less<>> visible_manifest_names;
    for (const json& registration : manifest.registrations) {
        const std::string name = require_string_field(
            registration,
            "name",
            "GUC manifest registration");
        if (!manifest_index_by_name.emplace(
                name, manifest_index_by_name.size()).second) {
            fail("installed GUC profile", "manifest name is duplicated");
        }
        bool visible = true;
        for (const json& flag : require_field(
                 registration,
                 "flags",
                 "GUC manifest registration")) {
            if (require_string(
                    flag,
                    "GUC manifest registration flag")
                == "no_show_all") {
                visible = false;
            }
        }
        if (visible) {
            visible_manifest_names.insert(name);
        }
    }
    const pg_result result = connection.execute(
        "SELECT settings.name, settings.vartype,"
        "       settings.context, settings.unit, settings.setting,"
        "       settings.boot_val, settings.reset_val,"
        "       settings.min_val, settings.max_val,"
        "        pg_catalog.to_json(settings.enumvals)::text,"
        "        settings.short_desc, settings.extra_desc"
        "  FROM pg_catalog.pg_settings AS settings"
        " WHERE settings.name LIKE 'bioseq.%'"
        " ORDER BY settings.name COLLATE \"C\";");
    const auto property_rows =
        rows(result, "installed GUC profile");
    const auto internal_row = one_row(
        connection.execute(
            "SELECT "
            "pg_temp.bioseq_safety_runtime_guc_internal_contract();"),
        "GUC internal contract");
    const auto internal = parse_internal_guc_contract(
        nonnull_cell(internal_row, 0, "GUC internal contract"));
    const auto show_all_rows =
        rows(connection.execute("SHOW ALL;"), "SHOW ALL");
    std::set<std::string, std::less<>> show_all_names;
    for (const auto& row : show_all_rows) {
        if (row.size() != 3) {
            fail("SHOW ALL", "row width differs");
        }
        const std::string name = nonnull_cell(row, 0, "SHOW ALL");
        if (name.starts_with("bioseq.")
            && !show_all_names.insert(name).second) {
            fail("SHOW ALL", "bioseq name is duplicated");
        }
    }
    std::map<std::string, std::vector<std::optional<std::string>>, std::less<>>
        live_rows_by_name;
    for (const auto& row : property_rows) {
        if (row.size() != 12) {
            fail("installed GUC profile", "property row width differs");
        }
        const std::string name =
            nonnull_cell(row, 0, "installed GUC profile");
        if (!live_rows_by_name.emplace(name, row).second) {
            fail("installed GUC profile", "live name is duplicated");
        }
    }
    const guc_profile_counts observed{
        .total = internal.size(),
        .test_only = static_cast<std::uint64_t>(
            internal.contains(
                "bioseq.testing.chunked_scanner.enabled"))
            + static_cast<std::uint64_t>(
                internal.contains(
                    "bioseq.testing.in_memory_scanner.enabled"))
            + static_cast<std::uint64_t>(
                internal.contains(
                    "bioseq.testing.persisted_index.failure_mode")),
    };
    const guc_profile_counts expected =
        expected_guc_profile(manifest.profile);
    constexpr std::size_t expected_visible_count = 62;
    if (observed.total != expected.total
        || observed.test_only != expected.test_only
        || visible_manifest_names.size() != expected_visible_count
        || live_rows_by_name.size() != visible_manifest_names.size()
        || internal.size() != manifest_index_by_name.size()) {
        fail(
            "installed GUC profile",
            fmt::format(
                "profile '{}' expected total={} test_only={}, "
                "visible={}; observed total={} test_only={}, visible={}",
                manifest.profile,
                expected.total,
                expected.test_only,
                expected_visible_count,
                observed.total,
                observed.test_only,
                live_rows_by_name.size()));
    }
    for (const auto& [name, index] : manifest_index_by_name) {
        static_cast<void>(index);
        if (!internal.contains(name)
            || live_rows_by_name.contains(name)
                != visible_manifest_names.contains(name)) {
            fail(
                "installed GUC profile",
                fmt::format(
                    "manifest GUC '{}' differs between the internal "
                    "inventory and pg_settings visibility",
                    name));
        }
    }
    for (const auto& [name, row] : live_rows_by_name) {
        static_cast<void>(row);
        if (!manifest_index_by_name.contains(name)) {
            fail(
                "installed GUC profile",
                fmt::format(
                    "unexpected live GUC '{}' is outside the generated "
                    "authority",
                    name));
        }
    }
    for (const auto& [name, property] : internal) {
        static_cast<void>(property);
        if (!manifest_index_by_name.contains(name)) {
            fail(
                "installed GUC profile",
                fmt::format(
                    "unexpected internal GUC '{}' is outside the "
                    "generated authority",
                    name));
        }
    }

    json properties = json::array();
    for (std::size_t row_index = 0;
         row_index < manifest.registrations.size();
         ++row_index) {
        const json& registration =
            manifest.registrations[row_index];
        const json& generated_live_authority = require_field(
            registration,
            "generated_live_authority",
            "GUC manifest registration");
        const std::string expected_name =
            require_string_field(
                registration,
                "name",
                "GUC manifest registration");
        const internal_guc_property& internal_property =
            internal.at(expected_name);
        const auto hook =
            [](bool present, std::string_view behavior) {
                return json{
                    {"symbol", nullptr},
                    {"present", present},
                    {"behavior",
                     present ? std::string{behavior} : "none"}};
            };
        const std::uint64_t raw_flags =
            internal_property.raw_flags;
        const json hooks{
            {"check",
             hook(
                 internal_property.check_hook_present,
                 "custom_check")},
            {"assign",
             hook(
                 internal_property.assign_hook_present,
                 "custom_assign")},
            {"show",
             hook(
                 internal_property.show_hook_present,
                 "custom_show")}};
        json live;
        if (visible_manifest_names.contains(expected_name)) {
            const auto& row = live_rows_by_name.at(expected_name);
            json enum_values = nullptr;
            if (row[9].has_value()) {
                enum_values = parse_json_value(
                    *row[9],
                    fmt::format(
                        "installed GUC profile {} enum values",
                        expected_name));
                if (!enum_values.is_array()) {
                    fail(
                        "installed GUC profile",
                        "enum values are not a JSON array");
                }
            }
            const auto property_cell =
                [&row](std::size_t index) -> json {
                    if (!row[index].has_value()) {
                        return nullptr;
                    }
                    return *row[index];
                };
            live = json{
                {"vartype", property_cell(1)},
                {"context", property_cell(2)},
                {"unit", property_cell(3)},
                {"boot_value", property_cell(5)},
                {"reset_value", property_cell(6)},
                {"current_value", property_cell(4)},
                {"minimum", property_cell(7)},
                {"maximum", property_cell(8)},
                {"visible_enum_values",
                 enum_values.is_null()
                     ? json::array()
                     : std::move(enum_values)},
                {"short_description", property_cell(10)},
                {"extra_description", property_cell(11)},
                {"raw_flags", raw_flags},
                {"visible_in_show_all",
                 show_all_names.contains(expected_name)},
                {"included_in_sample",
                 (raw_flags & 0x000080U) == 0},
                {"hooks", hooks}};
        } else {
            // Hidden settings have no pg_settings row. Read their flags and hooks
            // through the control library and their value through current_setting.
            // Other properties come from the generated definition
            const auto current_row = one_row(
                connection.execute(
                    "SELECT pg_catalog.current_setting("
                    + sql_literal(expected_name)
                    + ", true);"),
                fmt::format(
                    "installed hidden GUC {}", expected_name));
            live = generated_live_authority;
            live["current_value"] = nonnull_cell(
                current_row,
                0,
                fmt::format(
                    "installed hidden GUC {}", expected_name));
            live["raw_flags"] = raw_flags;
            live["visible_in_show_all"] =
                show_all_names.contains(expected_name);
            live["included_in_sample"] =
                (raw_flags & 0x000080U) == 0;
            live["hooks"] = hooks;
        }
        validate_live_guc_property(
            generated_live_authority,
            live,
            fmt::format(
                "installed GUC profile {}", expected_name));
        const std::string owner = require_string_field(
            registration,
            "owner",
            "GUC manifest registration");
        const auto owner_facet =
            manifest.owner_facets.find(owner);
        if (owner_facet == manifest.owner_facets.end()) {
            fail(
                "installed GUC profile",
                "registration has no sealed owner facet");
        }
        const std::string registration_sha256 =
            sha256_bytes(canonical_json(registration));
        json property{
            {"registration_ordinal",
             require_field(
                 registration,
                 "registration_ordinal",
                 "GUC manifest registration")},
            {"name", expected_name},
            {"manifest_registration_sha256",
             registration_sha256},
            {"registration", registration},
            {"profile_projection",
             guc_profile_projection(
                 manifest, owner_facet->second)},
            {"pg_settings", std::move(live)}};
        properties.push_back(std::move(property));
    }
    const std::string properties_sha256 =
        sha256_bytes(canonical_json(properties));
    return {
        .counts = observed,
        .properties = std::move(properties),
        .properties_sha256 = properties_sha256,
    };
}

/// Verify a saved settings report against,  its SHA-256 and generated definitions.
void validate_guc_property_receipt(
        const json& properties,
        std::string_view properties_sha256,
        const guc_manifest_contract& manifest,
        std::string_view context) {
    if (!properties.is_array()
        || properties.size() != manifest.registrations.size()
        || !lowercase_sha256(properties_sha256)
        || sha256_bytes(canonical_json(properties))
            != properties_sha256) {
        fail(context, "GUC property array is malformed or unsealed");
    }
    for (std::size_t index = 0; index < properties.size(); ++index) {
        const json& property = properties[index];
        const std::string property_context =
            fmt::format("{}[{}]", context, index);
        require_exact_fields(
            property,
            {
                "registration_ordinal",
                "name",
                "manifest_registration_sha256",
                "registration",
                "profile_projection",
                "pg_settings",
            },
            property_context);
        const json& registration =
            manifest.registrations[index];
        const json& generated_live_authority = require_field(
            registration,
            "generated_live_authority",
            property_context);
        const std::string name = require_string_field(
            registration, "name", property_context);
        const std::string owner = require_string_field(
            registration, "owner", property_context);
        const auto facet = manifest.owner_facets.find(owner);
        if (facet == manifest.owner_facets.end()) {
            fail(property_context, "owner facet is absent");
        }
        if (require_field(
                property,
                "registration_ordinal",
                property_context)
                != require_field(
                    registration,
                    "registration_ordinal",
                    property_context)
            || require_string_field(
                   property, "name", property_context)
                != name
            || require_string_field(
                   property,
                   "manifest_registration_sha256",
                   property_context)
                != sha256_bytes(canonical_json(registration))
            || require_field(
                   property, "registration", property_context)
                != registration
            || require_field(
                   property, "profile_projection", property_context)
                != guc_profile_projection(
                    manifest, facet->second)) {
            fail(
                property_context,
                "GUC property does not exactly close over its manifest row");
        }
        validate_live_guc_property(
            generated_live_authority,
            require_field(
                property, "pg_settings", property_context),
            property_context);
    }
}

/// Surround text with SQL quotes and double embedded single quotes.
[[nodiscard]] std::string sql_literal(std::string_view value) {
    std::string result{"'"};
    result.reserve(value.size() + 2);
    for (const char character : value) {
        result.push_back(character);
        if (character == '\'') {
            result.push_back('\'');
        }
    }
    result.push_back('\'');
    return result;
}

}  // namespace bioseq::tests::integration::safety_runtime_harness
