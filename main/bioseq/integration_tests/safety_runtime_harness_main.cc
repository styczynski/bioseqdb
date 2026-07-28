/*
 * Standalone PostgreSQL runtime-evidence protocol for SQL-boundary safety.
 *
 * This file intentionally uses .cc so the ordinary integration-test source
 * glob does not add a second main() to the Catch2 executable. Dedicated
 * production and test-indexes evidence targets compile it with distinct
 * profile and checked case-plan definitions.
 */

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

extern char** environ;

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::string_view kProfile = BIOSEQ_SAFETY_RUNTIME_PROFILE;
constexpr std::string_view kCasePlanPath =
    BIOSEQ_SAFETY_RUNTIME_CASE_PLAN;
constexpr std::string_view kCasePlanSha256 =
    BIOSEQ_SAFETY_RUNTIME_CASE_PLAN_SHA256;
constexpr std::string_view kControlFile =
    BIOSEQ_SAFETY_RUNTIME_CONTROL_FILE;
constexpr std::string_view kSqlFile =
    BIOSEQ_SAFETY_RUNTIME_SQL_FILE;
constexpr std::string_view kSqlManifest =
    BIOSEQ_SAFETY_RUNTIME_SQL_MANIFEST;
constexpr std::string_view kControlDso =
    BIOSEQ_SAFETY_RUNTIME_CONTROL_DSO;
constexpr std::size_t kMaximumSqlStatements = 64;
constexpr std::size_t kMaximumSqlBytes = 1024 * 1024;
constexpr std::string_view kAuthorizationMarker =
    "bioseq-safety-runtime-v2\n";
constexpr std::string_view kBackendScopedVacuumFaultKey =
    "invoke_pg_as_cpp.vacuum_cleanup_restore";

[[noreturn]] void fail(std::string_view context, std::string_view message) {
    throw std::runtime_error{
        fmt::format("{}: {}", context, message)};
}

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

[[nodiscard]] std::string hex_digest(
        const unsigned char* bytes,
        std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = digits[bytes[index] >> 4U];
        result[index * 2 + 1] = digits[bytes[index] & 0x0fU];
    }
    return result;
}

[[nodiscard]] std::string sha256_bytes(std::string_view bytes) {
    using digest_context =
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    digest_context context{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
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

[[nodiscard]] std::string sha256_file(const fs::path& path) {
    return sha256_bytes(read_file(path));
}

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

[[nodiscard]] json parse_json(
        std::string_view bytes,
        std::string_view context) {
    json result = parse_json_value(bytes, context);
    if (!result.is_object()) {
        fail(context, "top-level JSON value must be an object");
    }
    return result;
}

[[nodiscard]] json load_json(
        const fs::path& path,
        std::string_view context) {
    return parse_json(read_file(path), context);
}

[[nodiscard]] std::string canonical_json(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::strict)
        + "\n";
}

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

[[nodiscard]] std::string require_string_field(
        const json& object,
        std::string_view field,
        std::string_view context) {
    return require_string(
        require_field(object, field, context),
        fmt::format("{}.{}", context, field));
}

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

[[nodiscard]] std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(
            bytes.data(), static_cast<int>(bytes.size())) != 1) {
        fail("runtime token", "OpenSSL random generation failed");
    }
    return hex_digest(bytes.data(), bytes.size());
}

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

struct connection_parameters final {
    std::string host;
    std::string port;
    std::string database;
    std::string user;
    std::string password;
};

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

class pg_connection final {
public:
    explicit pg_connection(
            const connection_parameters& parameters) {
        const char* keywords[] = {
            "host",
            "port",
            "dbname",
            "user",
            "password",
            "application_name",
            nullptr,
        };
        const char* values[] = {
            parameters.host.c_str(),
            parameters.port.c_str(),
            parameters.database.c_str(),
            parameters.user.c_str(),
            parameters.password.c_str(),
            "bioseq-safety-runtime",
            nullptr,
        };
        value_ = PQconnectdbParams(keywords, values, 0);
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

    [[nodiscard]] pg_result execute(std::string_view sql) const {
        return pg_result{
            PQexec(value_, std::string{sql}.c_str())};
    }

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

    [[nodiscard]] int backend_pid() const noexcept {
        return PQbackendPID(value_);
    }

    [[nodiscard]] ConnStatusType connection_status() const noexcept {
        return value_ == nullptr
            ? CONNECTION_BAD
            : PQstatus(value_);
    }

    [[nodiscard]] PGTransactionStatusType
    transaction_status() const noexcept {
        return PQtransactionStatus(value_);
    }

private:
    PGconn* value_ = nullptr;
};

[[nodiscard]] bool command_success(const pg_result& result) noexcept {
    return result.get() != nullptr
        && (PQresultStatus(result.get()) == PGRES_COMMAND_OK
            || PQresultStatus(result.get()) == PGRES_TUPLES_OK);
}

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

[[nodiscard]] std::vector<std::optional<std::string>> one_row(
        const pg_result& result,
        std::string_view context) {
    auto result_rows = rows(result, context);
    if (result_rows.size() != 1) {
        fail(context, "expected exactly one row");
    }
    return std::move(result_rows.front());
}

[[nodiscard]] std::string nonnull_cell(
        const std::vector<std::optional<std::string>>& row,
        std::size_t index,
        std::string_view context) {
    if (index >= row.size() || !row[index].has_value()) {
        fail(context, "required result cell is null or absent");
    }
    return *row[index];
}

struct guc_profile_counts final {
    std::uint64_t total = 0;
    std::uint64_t test_only = 0;
};

struct guc_manifest_contract final {
    std::string profile;
    std::string parity_ledger_sha256;
    std::string registry_module;
    std::string registry_content_sha256;
    std::string profile_delta_proof_sha256;
    std::map<std::string, json, std::less<>> owner_facets;
    std::vector<json> registrations;
};

struct installed_guc_profile final {
    guc_profile_counts counts;
    json properties;
    std::string properties_sha256;
};

[[nodiscard]] guc_profile_counts expected_guc_profile(
        std::string_view profile) {
    if (profile == "production") {
        return {
            .total = 76,
            .test_only = 0,
        };
    }
    if (profile == "test_indexes") {
        return {
            .total = 77,
            .test_only = 1,
        };
    }
    fail(
        "installed GUC profile",
        fmt::format("unsupported profile '{}'", profile));
}

[[nodiscard]] std::string guc_owner_key(
        std::string_view candidate_owner) {
    if (candidate_owner == "bioseq") {
        return "bioseq";
    }
    constexpr std::string_view prefix = "bioseq.";
    if (!candidate_owner.starts_with(prefix)
        || candidate_owner.size() == prefix.size()
        || candidate_owner.substr(prefix.size()).contains('.')) {
        fail(
            "GUC manifest",
            fmt::format(
                "candidate owner '{}' is not a supported owner",
                candidate_owner));
    }
    return std::string{candidate_owner.substr(prefix.size())};
}

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

[[nodiscard]] json guc_profile_projection(
        const guc_manifest_contract& manifest,
        const json& owner_facet) {
    return json{
        {"profile", manifest.profile},
        {"parity_ledger_sha256",
         manifest.parity_ledger_sha256},
        {"registry_module", manifest.registry_module},
        {"registry_content_sha256",
         manifest.registry_content_sha256},
        {"profile_delta_proof_sha256",
         manifest.profile_delta_proof_sha256},
        {"owner_facet", owner_facet}};
}

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

void validate_generated_live_guc_authority(
        const json& authority,
        const json& generated,
        std::string_view context) {
    validate_live_guc_property(generated, generated, context);
    const std::string kind =
        require_string_field(authority, "kind", context);
    const std::string source_context =
        require_string_field(authority, "context", context);
    const json& authority_unit =
        require_field(authority, "unit", context);
    json expected_unit = nullptr;
    if (!authority_unit.is_null()) {
        const std::string unit =
            require_string(authority_unit, context);
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
    const json& flags = require_field(authority, "flags", context);
    for (const json& raw_flag : flags) {
        const std::string flag = require_string(raw_flag, context);
        if (flag == "no_show_all") expected_flags |= 0x000004U;
        else if (flag == "not_in_sample") expected_flags |= 0x000080U;
        else fail(context, "generated live flag is unsupported");
    }
    if (!authority_unit.is_null()) {
        const std::string unit = authority_unit.get<std::string>();
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
    const json& authority_hooks =
        require_field(authority, "hooks", context);
    const json& hooks = require_field(generated, "hooks", context);
    require_exact_fields(
        hooks, {"check", "assign", "show"}, context);
    static constexpr std::array<std::string_view, 3> names{
        "check", "assign", "show"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const json& hook = require_field(hooks, names[index], context);
        require_exact_fields(
            hook, {"symbol", "present", "behavior"}, context);
        const json& symbol = authority_hooks[index];
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

[[nodiscard]] std::string sql_literal(std::string_view value);

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

struct internal_guc_property final {
    std::uint64_t raw_flags{};
    bool check_hook_present{};
    bool assign_hook_present{};
    bool show_hook_present{};
};

[[nodiscard]] std::map<
    std::string,
    internal_guc_property,
    std::less<>>
parse_internal_guc_contract(std::string_view raw) {
    constexpr std::string_view header =
        "bioseq_guc_internal_contract_v1\n";
    if (!raw.starts_with(header)
        || raw.size() > 64 * 1024
        || raw.back() != '\n') {
        fail(
            "GUC internal contract",
            "version, size, or final newline differs");
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

[[nodiscard]] installed_guc_profile verify_installed_guc_profile(
        const pg_connection& connection,
        const guc_manifest_contract& manifest) {
    std::map<std::string, std::size_t, std::less<>>
        manifest_index_by_name;
    for (const json& registration : manifest.registrations) {
        const std::string name = require_string_field(
            require_field(
                registration,
                "authority",
                "GUC manifest registration"),
            "name",
            "GUC manifest registration.authority");
        if (!manifest_index_by_name.emplace(
                name, manifest_index_by_name.size()).second) {
            fail("installed GUC profile", "manifest name is duplicated");
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
        .total = live_rows_by_name.size(),
        .test_only = static_cast<std::uint64_t>(
            live_rows_by_name.contains(
                "bioseq.test_force_paper_index_failure")),
    };
    const guc_profile_counts expected =
        expected_guc_profile(manifest.profile);
    if (observed.total != expected.total
        || observed.test_only != expected.test_only
        || live_rows_by_name.size() != manifest_index_by_name.size()
        || internal.size() != manifest_index_by_name.size()) {
        fail(
            "installed GUC profile",
            fmt::format(
                "profile '{}' expected total={} test_only={}, "
                "observed total={} test_only={}",
                manifest.profile,
                expected.total,
                expected.test_only,
                observed.total,
                observed.test_only));
    }
    for (const auto& [name, index] : manifest_index_by_name) {
        static_cast<void>(index);
        if (!live_rows_by_name.contains(name)
            || !internal.contains(name)) {
            fail(
                "installed GUC profile",
                fmt::format(
                    "manifest GUC '{}' is missing from an unfiltered "
                    "live inventory",
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
        const json& authority = require_field(
            registration,
            "authority",
            "GUC manifest registration");
        const json& candidate = require_field(
            registration,
            "candidate",
            "GUC manifest registration");
        const json& generated_live_authority = require_field(
            registration,
            "generated_live_authority",
            "GUC manifest registration");
        const std::string expected_name =
            require_string_field(
                authority,
                "name",
                "GUC manifest registration.authority");
        const auto& row = live_rows_by_name.at(expected_name);
        const internal_guc_property& internal_property =
            internal.at(expected_name);
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
        json live{
            {"vartype", property_cell(1)},
            {"context", property_cell(2)},
            {"unit", property_cell(3)},
            {"boot_value", property_cell(5)},
            {"reset_value", property_cell(6)},
            {"current_value", property_cell(4)},
            {"minimum", property_cell(7)},
            {"maximum", property_cell(8)},
            {"visible_enum_values",
             enum_values.is_null() ? json::array() : std::move(enum_values)},
            {"short_description", property_cell(10)},
            {"extra_description", property_cell(11)},
            {"raw_flags", raw_flags},
            {"visible_in_show_all",
             show_all_names.contains(expected_name)},
            {"included_in_sample",
             (raw_flags & 0x000080U) == 0},
            {"hooks",
             json{
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
                      "custom_show")}}}};
        validate_live_guc_property(
            generated_live_authority,
            live,
            fmt::format(
                "installed GUC profile {}", expected_name));
        const std::string owner = guc_owner_key(
            require_string_field(
                candidate,
                "owner",
                "GUC manifest registration.candidate"));
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
                 authority,
                 "registration_ordinal",
                 "GUC manifest registration.authority")},
            {"name", expected_name},
            {"manifest_registration_sha256",
             registration_sha256},
            {"authority", authority},
            {"candidate", candidate},
            {"generated_live_authority",
             generated_live_authority},
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
                "authority",
                "candidate",
                "generated_live_authority",
                "profile_projection",
                "pg_settings",
            },
            property_context);
        const json& registration =
            manifest.registrations[index];
        const json& authority = require_field(
            registration, "authority", property_context);
        const json& candidate = require_field(
            registration, "candidate", property_context);
        const json& generated_live_authority = require_field(
            registration,
            "generated_live_authority",
            property_context);
        const std::string name = require_string_field(
            authority, "name", property_context);
        const std::string owner = guc_owner_key(
            require_string_field(
                candidate, "owner", property_context));
        const auto facet = manifest.owner_facets.find(owner);
        if (facet == manifest.owner_facets.end()) {
            fail(property_context, "owner facet is absent");
        }
        if (require_field(
                property,
                "registration_ordinal",
                property_context)
                != require_field(
                    authority,
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
                   property, "authority", property_context)
                != authority
            || require_field(
                   property, "candidate", property_context)
                != candidate
            || require_field(
                   property,
                   "generated_live_authority",
                   property_context)
                != generated_live_authority
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

[[nodiscard]] fs::path require_absolute_path(
        const json& object,
        std::string_view field,
        std::string_view context) {
    fs::path result{
        require_string_field(object, field, context)};
    if (!result.is_absolute()) {
        fail(
            fmt::format("{}.{}", context, field),
            "path must be absolute");
    }
    return result.lexically_normal();
}

[[nodiscard]] bool path_within(
        const fs::path& child,
        const fs::path& root) {
    const fs::path canonical_root =
        fs::canonical(root);
    const fs::path canonical_child =
        fs::weakly_canonical(child);
    const fs::path relative =
        canonical_child.lexically_relative(canonical_root);
    return !relative.empty()
        && relative != "."
        && !relative.is_absolute()
        && *relative.begin() != "..";
}

void require_private_authorization(
        const fs::path& marker,
        const fs::path& expected_data_directory) {
    if (!marker.is_absolute()
        || !fs::is_regular_file(marker)
        || fs::is_symlink(marker)
        || !path_within(marker, expected_data_directory)
        || read_file(marker) != kAuthorizationMarker) {
        fail(
            "cluster-config.authorization_file",
            "private-cluster authorization marker is invalid");
    }
    struct stat metadata {};
    if (::stat(marker.c_str(), &metadata) != 0
        || metadata.st_uid != ::getuid()
        || (metadata.st_mode & 0777) != 0600) {
        fail(
            "cluster-config.authorization_file",
            "authorization marker must be owned by this uid with mode 0600");
    }
}

void validate_cluster_config(
        const json& config,
        std::string_view expected_profile) {
    require_exact_fields(
        config,
        {
            "schema_version",
            "profile",
            "dedicated_safety_cluster",
            "connection",
            "private_stage_root",
            "expected_data_directory",
            "terminal_marker_directory",
            "server_log_path",
            "expected_system_identifier",
            "authorization_file",
            "installed_dso_path",
            "installed_control_dso_path",
            "installed_control_path",
            "installed_sql_path",
            "restart_command",
            "stage_manifest_path",
            "stage_manifest_sha256",
            "tool_signature_suite_sha256",
        },
        "cluster-config");
    if (require_field(
            config, "schema_version", "cluster-config") != 1) {
        fail("cluster-config.schema_version", "expected 1");
    }
    if (require_string_field(
            config, "profile", "cluster-config")
        != expected_profile) {
        fail("cluster-config.profile", "profile differs from executable");
    }
    const json& dedicated = require_field(
        config, "dedicated_safety_cluster", "cluster-config");
    if (!dedicated.is_boolean() || !dedicated.get<bool>()) {
        fail(
            "cluster-config.dedicated_safety_cluster",
            "destructive activation requires true");
    }
    const connection_parameters connection =
        parse_connection(config);
    const fs::path socket_directory{connection.host};
    if (!socket_directory.is_absolute()
        || !fs::is_directory(socket_directory)
        || fs::is_symlink(socket_directory)
        || connection.database != "bioseq_safety_runtime") {
        fail(
            "cluster-config.connection",
            "runtime evidence requires a local Unix socket and dedicated database");
    }
    const fs::path stage_root = require_absolute_path(
        config, "private_stage_root", "cluster-config");
    const fs::path data_directory = require_absolute_path(
        config, "expected_data_directory", "cluster-config");
    const fs::path authorization_file = require_absolute_path(
        config, "authorization_file", "cluster-config");
    const fs::path terminal_marker_directory =
        require_absolute_path(
            config,
            "terminal_marker_directory",
            "cluster-config");
    const fs::path server_log_path = require_absolute_path(
        config, "server_log_path", "cluster-config");
    if (!fs::is_directory(stage_root)
        || fs::is_symlink(stage_root)
        || !fs::is_directory(data_directory)
        || fs::is_symlink(data_directory)
        || !fs::is_directory(terminal_marker_directory)
        || fs::is_symlink(terminal_marker_directory)
        || fs::canonical(terminal_marker_directory)
            != terminal_marker_directory
        || terminal_marker_directory
            != fs::canonical(data_directory)
                / "bioseq-safety-terminal-markers"
        || !fs::is_regular_file(server_log_path)
        || fs::is_symlink(server_log_path)
        || fs::canonical(server_log_path) != server_log_path
        || server_log_path
            != fs::canonical(data_directory).parent_path()
                / "server_startup.log") {
        fail(
            "cluster-config",
            "private roots, terminal marker root, or server log differ");
    }
    struct stat marker_metadata {};
    struct stat log_metadata {};
    if (::lstat(
            terminal_marker_directory.c_str(),
            &marker_metadata) != 0
        || !S_ISDIR(marker_metadata.st_mode)
        || marker_metadata.st_uid != ::getuid()
        || (marker_metadata.st_mode & 0777) != 0700
        || ::lstat(server_log_path.c_str(), &log_metadata) != 0
        || !S_ISREG(log_metadata.st_mode)
        || log_metadata.st_uid != ::getuid()
        || log_metadata.st_nlink != 1) {
        fail(
            "cluster-config",
            "terminal marker root or server log metadata differs");
    }
    require_private_authorization(
        authorization_file, data_directory);
    for (const std::string_view field : {
             "installed_dso_path",
             "installed_control_dso_path",
             "installed_control_path",
             "installed_sql_path"}) {
        const fs::path path =
            require_absolute_path(config, field, "cluster-config");
        if (!path_within(path, stage_root)
            || fs::is_symlink(path.parent_path())) {
            fail(
                fmt::format("cluster-config.{}", field),
                "install destination escapes the private stage");
        }
    }
    const json& command = require_field(
        config, "restart_command", "cluster-config");
    if (!command.is_array()
        || command.empty()
        || command.size() > 32) {
        fail(
            "cluster-config.restart_command",
            "expected 1..32 command arguments");
    }
    for (std::size_t index = 0; index < command.size(); ++index) {
        static_cast<void>(require_string(
            command[index],
            fmt::format(
                "cluster-config.restart_command[{}]", index)));
    }
    const fs::path executable =
        command.front().get<std::string>();
    if (!executable.is_absolute()
        || !fs::is_regular_file(executable)
        || fs::is_symlink(executable)
        || !path_within(executable, stage_root)
        || ::access(executable.c_str(), X_OK) != 0) {
        fail(
            "cluster-config.restart_command[0]",
            "restart executable must be inside the private stage");
    }
    bool names_data_directory = false;
    for (const json& argument : command) {
        if (argument.get<std::string>()
            == data_directory.string()) {
            names_data_directory = true;
        }
    }
    if (!names_data_directory) {
        fail(
            "cluster-config.restart_command",
            "restart command does not name the authorized data directory");
    }
}

void run_command(
        const json& command,
        std::string_view context) {
    std::vector<std::string> owned;
    owned.reserve(command.size());
    for (const json& argument : command) {
        owned.push_back(argument.get<std::string>());
    }
    std::vector<char*> arguments;
    arguments.reserve(owned.size() + 1);
    for (std::string& argument : owned) {
        arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);
    pid_t process = 0;
    const int spawn_status = posix_spawn(
        &process,
        owned.front().c_str(),
        nullptr,
        nullptr,
        arguments.data(),
        environ);
    if (spawn_status != 0) {
        fail(
            context,
            fmt::format(
                "posix_spawn failed: {}",
                std::strerror(spawn_status)));
    }
    int status = 0;
    while (::waitpid(process, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        fail(
            context,
            fmt::format(
                "waitpid failed: {}", std::strerror(errno)));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(
            context,
            WIFEXITED(status)
            ? fmt::format(
                "command exited {}", WEXITSTATUS(status))
            : "command did not exit normally");
    }
}

void install_artifact(
        const fs::path& source,
        const fs::path& destination,
        std::string_view expected_sha256,
        std::string_view nonce) {
    if (!source.is_absolute()
        || !fs::is_regular_file(source)
        || fs::is_symlink(source)) {
        fail(source.string(), "source must be an absolute regular file");
    }
    if (!destination.is_absolute()
        || !fs::is_directory(destination.parent_path())) {
        fail(
            destination.string(),
            "destination parent must be an existing absolute directory");
    }
    if (fs::is_symlink(destination)
        || (fs::exists(destination)
            && !fs::is_regular_file(destination))) {
        fail(
            destination.string(),
            "destination must be absent or a regular non-symlink file");
    }
    const std::string source_sha256 = sha256_file(source);
    if (source_sha256 != expected_sha256) {
        fail(source.string(), "source hash changed before installation");
    }
    std::error_code equivalence_error;
    const bool same_file =
        fs::exists(destination)
        && fs::equivalent(
            source, destination, equivalence_error)
        && !equivalence_error;
    if (!same_file) {
        const fs::path temporary =
            destination.parent_path()
            / (destination.filename().string()
               + ".bioseq-safety." + std::string{nonce});
        if (fs::exists(temporary) || fs::is_symlink(temporary)) {
            fail(
                temporary.string(),
                "refuses to reuse an existing temporary path");
        }
        std::error_code copy_error;
        if (!fs::copy_file(
                source,
                temporary,
                fs::copy_options::none,
                copy_error)) {
            fail(
                temporary.string(),
                fmt::format(
                    "cannot copy artifact: {}",
                    copy_error.message()));
        }
        fs::permissions(
            temporary,
            fs::status(source).permissions());
        if (sha256_file(temporary) != expected_sha256) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            fail(
                temporary.string(),
                "copied artifact hash differs from source");
        }
        if (::rename(
                temporary.c_str(), destination.c_str()) != 0) {
            const int rename_error = errno;
            std::error_code ignored;
            fs::remove(temporary, ignored);
            fail(
                destination.string(),
                fmt::format(
                    "cannot atomically install artifact: {}",
                    std::strerror(rename_error)));
        }
    }
    if (sha256_file(destination) != expected_sha256) {
        fail(
            destination.string(),
            "installed artifact hash differs from requested bytes");
    }
}

[[nodiscard]] std::unique_ptr<pg_connection> connect_with_retry(
        const connection_parameters& parameters) {
    std::string last_error;
    for (int attempt = 0; attempt < 30; ++attempt) {
        try {
            return std::make_unique<pg_connection>(parameters);
        } catch (const std::runtime_error& error) {
            last_error = error.what();
            std::this_thread::sleep_for(
                std::chrono::milliseconds{200});
        }
    }
    fail(
        "postgres activation",
        fmt::format(
            "server did not become ready: {}", last_error));
}

[[nodiscard]] std::vector<std::string> sql_array(
        const json& object,
        std::string_view field,
        std::string_view context,
        bool required) {
    const auto found = object.find(std::string{field});
    if (found == object.end()) {
        if (required) {
            fail(
                context,
                fmt::format("missing field '{}'", field));
        }
        return {};
    }
    if (!found->is_array()
        || found->size() > kMaximumSqlStatements) {
        fail(
            fmt::format("{}.{}", context, field),
            "expected a bounded SQL string array");
    }
    std::vector<std::string> result;
    result.reserve(found->size());
    for (std::size_t index = 0; index < found->size(); ++index) {
        std::string sql = require_string(
            (*found)[index],
            fmt::format(
                "{}.{}[{}]", context, field, index));
        if (sql.size() > kMaximumSqlBytes) {
            fail(
                fmt::format(
                    "{}.{}[{}]", context, field, index),
                "SQL statement exceeds the size bound");
        }
        result.push_back(std::move(sql));
    }
    return result;
}

[[nodiscard]] bool case_id(std::string_view value) {
    static const std::regex pattern{
        "[a-z0-9][a-z0-9_.-]{0,127}"};
    return std::regex_match(
        value.begin(), value.end(), pattern);
}

[[nodiscard]] bool valid_fault_key(
        std::string_view value) noexcept {
    if (value.empty() || value.size() >= 129) {
        return false;
    }
    return std::ranges::all_of(
        value,
        [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '_'
                || character == '.';
        });
}

[[nodiscard]] bool probe_site_id(
        std::string_view value) noexcept {
    constexpr std::string_view prefix = "probe:v2:";
    return value.size() == prefix.size() + 64
        && value.substr(0, prefix.size()) == prefix
        && lowercase_sha256(value.substr(prefix.size()));
}

struct probe_contract_values final {
    std::uint64_t expected_visit_count;
    std::uint64_t expected_trigger_count;
    std::string_view ordering;
};

[[nodiscard]] probe_contract_values contract_for_relation(
        std::string_view relation,
        std::string_view context) {
    if (relation == "natural_at_site") {
        return {
            .expected_visit_count = 1,
            .expected_trigger_count = 0,
            .ordering = "visit_only"};
    }
    if (relation == "injected_routes_to_site") {
        return {
            .expected_visit_count = 1,
            .expected_trigger_count = 1,
            .ordering = "trigger_before_visit"};
    }
    if (relation == "injected_after_site_success") {
        return {
            .expected_visit_count = 1,
            .expected_trigger_count = 1,
            .ordering = "visit_before_trigger"};
    }
    fail(
        context,
        "relation must be natural_at_site, "
        "injected_routes_to_site, or "
        "injected_after_site_success");
}

[[nodiscard]] json probe_contract_for_relation(
        std::string_view relation,
        std::string_view context) {
    const probe_contract_values values =
        contract_for_relation(relation, context);
    return json{
        {"abi_version", std::uint64_t{2}},
        {"expected_visit_count",
         values.expected_visit_count},
        {"expected_trigger_count",
         values.expected_trigger_count},
        {"ordering", std::string{values.ordering}}};
}

void require_probe_contract(
        const json& value,
        std::string_view relation,
        std::string_view context) {
    static const std::set<std::string, std::less<>>
        allowed{
            "abi_version",
            "expected_visit_count",
            "expected_trigger_count",
            "ordering"};
    if (!value.is_object()) {
        fail(context, "probe_contract must be an object");
    }
    for (const auto& [field, field_value] : value.items()) {
        static_cast<void>(field_value);
        if (!allowed.contains(field)) {
            fail(
                context,
                fmt::format(
                    "unknown probe_contract field '{}'",
                    field));
        }
    }
    if (value
        != probe_contract_for_relation(relation, context)) {
        fail(
            context,
            "probe_contract differs from the relation's ABI-v2 contract");
    }
}

[[nodiscard]] json normalized_catalog_entry(
        const json& raw,
        std::string_view context) {
    static const std::set<std::string, std::less<>> allowed{
        "case_id",
        "sql_identity",
        "site_id",
        "ledger_site_sha256",
        "invocation_key",
        "fault_key",
        "relation",
        "probe_contract",
        "postgres_severity",
        "expected"};
    if (!raw.is_object()) {
        fail(context, "catalog entry must be an object");
    }
    for (const auto& [field, value] : raw.items()) {
        static_cast<void>(value);
        if (!allowed.contains(field)) {
            fail(
                context,
                fmt::format(
                    "unknown catalog field '{}'", field));
        }
    }
    const json result = raw;
    const std::string id = require_string_field(
        result, "case_id", context);
    if (!case_id(id)) {
        fail(context, "case_id is not artifact-safe");
    }
    static_cast<void>(
        require_string_field(
            result, "sql_identity", context));
    const std::string site_id = require_string_field(
        result, "site_id", context);
    if (!probe_site_id(site_id)) {
        fail(context, "site_id is not a full ABI-v2 probe ID");
    }
    const std::string ledger_site_sha256 =
        require_string_field(
            result, "ledger_site_sha256", context);
    if (!lowercase_sha256(ledger_site_sha256)) {
        fail(
            context,
            "ledger_site_sha256 is not a lowercase SHA-256");
    }
    static_cast<void>(
        require_string_field(
            result, "invocation_key", context));
    const std::string fault_key_value =
        require_string_field(
            result, "fault_key", context);
    if (!valid_fault_key(fault_key_value)) {
        fail(
            context,
            "fault_key must be 1-128 bytes from [a-z0-9_.]");
    }
    const std::string relation = require_string_field(
        result, "relation", context);
    require_probe_contract(
        require_field(result, "probe_contract", context),
        relation,
        fmt::format("{}.probe_contract", context));
    if (require_string_field(
            result, "postgres_severity", context)
        != "error") {
        fail(context, "terminal severity is forbidden at runtime");
    }
    const json& expected = require_field(
        result, "expected", context);
    static const std::set<std::string, std::less<>>
        expected_allowed{
            "backend_outcome",
            "destructor_sentinels",
            "cleanup_events",
            "postgres_cleanup_events",
            "sqlstate",
            "message",
            "recovery_postcondition"};
    if (!expected.is_object()) {
        fail(
            fmt::format("{}.expected", context),
            "expected an object");
    }
    for (const auto& [field, value] : expected.items()) {
        static_cast<void>(value);
        if (!expected_allowed.contains(field)) {
            fail(
                fmt::format("{}.expected", context),
                fmt::format(
                    "unknown expected field '{}'", field));
        }
    }
    const std::string backend_outcome =
        require_string_field(
            expected,
            "backend_outcome",
            fmt::format("{}.expected", context));
    if (backend_outcome != "recoverable_error"
        && backend_outcome != "backend_exit") {
        fail(
            context,
            "backend_outcome must be recoverable_error or backend_exit");
    }
    const bool backend_exit = backend_outcome == "backend_exit";
    const json& sentinels = require_field(
        expected, "destructor_sentinels",
        fmt::format("{}.expected", context));
    if (!sentinels.is_array()) {
        fail(
            fmt::format(
                "{}.expected.destructor_sentinels", context),
            "expected an array");
    }
    std::set<std::string, std::less<>> unique_sentinels;
    for (std::size_t index = 0;
         index < sentinels.size();
         ++index) {
        const std::string sentinel = require_string(
            sentinels[index],
            fmt::format(
                "{}.expected.destructor_sentinels[{}]",
                context,
                index));
        if (!unique_sentinels.insert(sentinel).second) {
            fail(context, "duplicate destructor sentinel");
        }
    }
    for (const std::string_view event_field : {
             "cleanup_events",
             "postgres_cleanup_events"}) {
        const json& events = require_field(
            expected,
            event_field,
            fmt::format("{}.expected", context));
        if (!events.is_array()) {
            fail(
                fmt::format(
                    "{}.expected.{}", context, event_field),
                "expected an array");
        }
        std::set<std::string, std::less<>> unique_events;
        for (std::size_t index = 0;
             index < events.size();
             ++index) {
            const std::string event_name = require_string(
                    events[index],
                    fmt::format(
                        "{}.expected.{}[{}]",
                        context,
                        event_field,
                        index));
            static const std::regex event_pattern{
                "[a-z0-9][a-z0-9_.]{0,63}"};
            if (!std::regex_match(
                    event_name.begin(),
                    event_name.end(),
                    event_pattern)
                || !unique_events.insert(event_name).second) {
                fail(context, "duplicate expected runtime event");
            }
        }
        if (backend_exit && !unique_events.empty()) {
            fail(
                context,
                "backend-exit cases cannot claim unobservable cleanup");
        }
        if (event_field == "postgres_cleanup_events"
            && !backend_exit) {
            if (fault_key_value
                    == kBackendScopedVacuumFaultKey) {
                if (!unique_events.empty()) {
                    fail(
                        context,
                        "backend-scoped VACUUM must expect no FMGR cleanup");
                }
            } else if (!unique_events.contains(
                           "fmgr_abort_observed")) {
                fail(
                    context,
                    "PostgreSQL cleanup evidence must include FMGR ABORT");
            }
        }
    }
    if (backend_exit && !unique_sentinels.empty()) {
        fail(
            context,
            "backend-exit cases cannot claim unobservable destructors");
    }
    const json& sqlstate_value = require_field(
        expected, "sqlstate",
        fmt::format("{}.expected", context));
    const json& message_value = require_field(
        expected, "message",
        fmt::format("{}.expected", context));
    if (backend_exit) {
        if (!sqlstate_value.is_null()
            || !message_value.is_null()) {
            fail(
                context,
                "backend-exit SQLSTATE/message must be null");
        }
    } else {
        const std::string sqlstate = require_string(
            sqlstate_value,
            fmt::format("{}.expected.sqlstate", context));
        if (sqlstate.size() != 5
            || !std::ranges::all_of(
                sqlstate,
                [](char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'A'
                            && character <= 'Z');
                })) {
            fail(context, "expected SQLSTATE must have five characters");
        }
        static_cast<void>(require_string(
            message_value,
            fmt::format("{}.expected.message", context)));
    }
    static_cast<void>(require_string_field(
        expected, "recovery_postcondition",
        fmt::format("{}.expected", context)));
    return result;
}

struct case_definition final {
    json catalog;
    json execution;
};

struct case_plan final {
    std::map<std::string, case_definition, std::less<>> cases;
};

[[nodiscard]] case_plan load_case_plan() {
    const fs::path path{kCasePlanPath};
    if (!path.is_absolute() || !fs::is_regular_file(path)) {
        fail(
            "case-plan", "compiled case-plan path is not a regular file");
    }
    const std::string raw = read_file(path);
    if (!lowercase_sha256(kCasePlanSha256)
        || sha256_bytes(raw) != kCasePlanSha256) {
        fail("case-plan", "bytes differ from compiled SHA-256");
    }
    const json plan = parse_json(raw, "case-plan");
    if (require_field(
            plan, "schema_version", "case-plan") != 2) {
        fail("case-plan.schema_version", "expected 2");
    }
    if (require_string_field(
            plan, "profile", "case-plan") != kProfile) {
        fail("case-plan.profile", "profile differs from executable");
    }
    const json& raw_cases = require_field(
        plan, "cases", "case-plan");
    if (!raw_cases.is_array() || raw_cases.empty()) {
        fail("case-plan.cases", "expected a non-empty array");
    }
    case_plan result;
    for (std::size_t index = 0;
         index < raw_cases.size();
         ++index) {
        const std::string context =
            fmt::format("case-plan.cases[{}]", index);
        const json& raw_case = raw_cases[index];
        const json catalog = normalized_catalog_entry(
            require_field(raw_case, "catalog", context),
            context + ".catalog");
        const json& execution = require_field(
            raw_case, "execution", context);
        if (!execution.is_object()) {
            fail(context + ".execution", "expected an object");
        }
        static const std::set<std::string, std::less<>>
            allowed_execution_fields{
                "target_regprocedure",
                "setup_sql",
                "invocation_sql",
                "post_error_sql",
                "recovery_sql",
                "recovery_expected_rows",
                "cleanup_sql",
                "pre_fault_control_sql",
                "pre_fault_control_recovery_sql",
                "pre_fault_control_recovery_expected_rows"};
        for (const auto& [field, value] : execution.items()) {
            static_cast<void>(value);
            if (!allowed_execution_fields.contains(field)) {
                fail(
                    context,
                    fmt::format(
                        "unknown execution field '{}'", field));
            }
        }
        const std::array<std::string_view, 3>
            pre_fault_control_fields{
                "pre_fault_control_sql",
                "pre_fault_control_recovery_sql",
                "pre_fault_control_recovery_expected_rows"};
        const std::size_t pre_fault_control_field_count =
            std::ranges::count_if(
                pre_fault_control_fields,
                [&execution](std::string_view field) {
                    return execution.contains(
                        std::string{field});
                });
        if (pre_fault_control_field_count != 0
            && pre_fault_control_field_count
                != pre_fault_control_fields.size()) {
            fail(
                context,
                "pre-fault control fields must be present together");
        }
        const bool backend_scoped_vacuum =
            require_string_field(
                catalog, "fault_key", context + ".catalog")
            == kBackendScopedVacuumFaultKey;
        if (backend_scoped_vacuum
            != (pre_fault_control_field_count
                == pre_fault_control_fields.size())) {
            fail(
                context,
                "backend-scoped VACUUM requires its sealed success control");
        }
        const std::string target = require_string_field(
            execution, "target_regprocedure",
            context + ".execution");
        if (target
            != require_string_field(
                catalog, "sql_identity",
                context + ".catalog")) {
            fail(
                context,
                "target_regprocedure must equal the ledger SQL identity");
        }
        static_cast<void>(sql_array(
            execution, "setup_sql",
            context + ".execution", false));
        const std::string invocation = require_string_field(
            execution, "invocation_sql",
            context + ".execution");
        if (invocation.size() > kMaximumSqlBytes) {
            fail(context, "invocation SQL exceeds the size bound");
        }
        static_cast<void>(sql_array(
            execution, "post_error_sql",
            context + ".execution", false));
        const std::string recovery = require_string_field(
            execution, "recovery_sql",
            context + ".execution");
        if (recovery.size() > kMaximumSqlBytes) {
            fail(context, "recovery SQL exceeds the size bound");
        }
        const auto validate_expected_rows =
            [&context](
                const json& expected_rows,
                std::string_view field) {
                if (!expected_rows.is_array()
                    || expected_rows.empty()
                    || expected_rows.size() > 64) {
                    fail(
                        context,
                        fmt::format(
                            "{} must contain 1..64 rows",
                            field));
                }
                std::optional<std::size_t> expected_columns;
                for (std::size_t row_index = 0;
                     row_index < expected_rows.size();
                     ++row_index) {
                    const json& row = expected_rows[row_index];
                    if (!row.is_array()
                        || row.empty()
                        || row.size() > 64
                        || (expected_columns.has_value()
                            && row.size()
                                != *expected_columns)) {
                        fail(
                            context,
                            fmt::format(
                                "{} must be a bounded rectangle",
                                field));
                    }
                    expected_columns = row.size();
                    for (std::size_t column = 0;
                         column < row.size();
                         ++column) {
                        if (!row[column].is_null()
                            && !row[column].is_string()) {
                            fail(
                                context,
                                fmt::format(
                                    "{} cells must be strings or null",
                                    field));
                        }
                    }
                }
            };
        validate_expected_rows(
            require_field(
                execution,
                "recovery_expected_rows",
                context + ".execution"),
            "recovery_expected_rows");
        if (backend_scoped_vacuum) {
            const std::vector<std::string> control_sql =
                sql_array(
                    execution,
                    "pre_fault_control_sql",
                    context + ".execution",
                    true);
            if (control_sql.empty()) {
                fail(
                    context,
                    "pre_fault_control_sql must not be empty");
            }
            const std::string control_recovery =
                require_string_field(
                    execution,
                    "pre_fault_control_recovery_sql",
                    context + ".execution");
            if (control_recovery.size() > kMaximumSqlBytes) {
                fail(
                    context,
                    "pre-fault control recovery SQL exceeds the size bound");
            }
            validate_expected_rows(
                require_field(
                    execution,
                    "pre_fault_control_recovery_expected_rows",
                    context + ".execution"),
                "pre_fault_control_recovery_expected_rows");
        }
        static_cast<void>(sql_array(
            execution, "cleanup_sql",
            context + ".execution", false));
        const std::string id =
            catalog.at("case_id").get<std::string>();
        if (!result.cases.emplace(
                id,
                case_definition{
                    .catalog = catalog,
                    .execution = execution})
                 .second) {
            fail(context, "duplicate case_id");
        }
    }
    return result;
}

struct command_line final {
    std::string command;
    std::map<std::string, std::string, std::less<>> options;
    fs::path executable;
};

[[nodiscard]] command_line parse_command_line(
        int argc,
        char** argv) {
    if (argc < 2 || argv == nullptr || argv[0] == nullptr) {
        fail("command-line", "missing protocol command");
    }
    const char* protocol =
        std::getenv("BIOSEQ_SAFETY_RUNTIME_PROTOCOL");
    if (protocol == nullptr
        || std::string_view{protocol} != "2") {
        fail(
            "command-line",
            "BIOSEQ_SAFETY_RUNTIME_PROTOCOL=2 is required");
    }
    const std::string command{argv[1]};
    static const std::set<std::string, std::less<>>
        supported{
            "--bioseq-safety-runtime-activate",
            "--bioseq-safety-runtime-describe",
            "--bioseq-safety-runtime-run",
            "--bioseq-safety-runtime-self-test"};
    if (!supported.contains(command)) {
        fail("command-line", "unsupported protocol command");
    }
    if ((argc - 2) % 2 != 0) {
        fail(
            "command-line",
            "options must be --name value pairs");
    }
    std::map<std::string, std::string, std::less<>> options;
    for (int index = 2; index < argc; index += 2) {
        if (argv[index] == nullptr
            || argv[index + 1] == nullptr
            || !std::string_view{argv[index]}.starts_with("--")) {
            fail("command-line", "malformed option pair");
        }
        if (!options.emplace(
                argv[index], argv[index + 1]).second) {
            fail(
                "command-line",
                fmt::format(
                    "duplicate option {}", argv[index]));
        }
    }
    fs::path executable = fs::absolute(argv[0]).lexically_normal();
    if (!fs::is_regular_file(executable)) {
        fail(
            "command-line", "executable path is not a regular file");
    }
    return command_line{
        .command = command,
        .options = std::move(options),
        .executable = std::move(executable)};
}

[[nodiscard]] std::string require_option(
        const command_line& command,
        std::string_view name) {
    const auto found =
        command.options.find(std::string{name});
    if (found == command.options.end() || found->second.empty()) {
        fail(
            "command-line",
            fmt::format("missing option {}", name));
    }
    return found->second;
}

void require_exact_options(
        const command_line& command,
        std::initializer_list<std::string_view> expected) {
    std::set<std::string, std::less<>> exact;
    for (const std::string_view option : expected) {
        exact.insert(std::string{option});
    }
    std::set<std::string, std::less<>> observed;
    for (const auto& [option, value] : command.options) {
        static_cast<void>(value);
        observed.insert(option);
    }
    if (observed != exact) {
        fail(
            "command-line",
            "protocol command has missing or extra options");
    }
}

[[nodiscard]] std::string self_sha256(
        const command_line& command) {
    return sha256_file(command.executable);
}

[[nodiscard]] json query_cluster_identity(
        const pg_connection& connection) {
    const pg_result result = connection.execute(
        "SELECT system_identifier::text, "
        "       current_setting('data_directory'), "
        "       pg_postmaster_start_time()::text, "
        "       current_setting('server_version_num'), "
        "       pg_backend_pid()::text, "
        "       pg_catalog.inet_server_addr()::text, "
        "       pg_catalog.current_database()::text, "
        "       pg_catalog.current_user::text, "
        "       role.rolsuper::text, "
        "       pg_catalog.pg_is_in_recovery()::text "
        "  FROM pg_catalog.pg_control_system() "
        "  JOIN pg_catalog.pg_roles AS role "
        "    ON role.rolname = pg_catalog.current_user;");
    const auto row = one_row(result, "cluster identity");
    if (row.size() != 10) {
        fail("cluster identity", "unexpected result shape");
    }
    const std::string data_directory =
        nonnull_cell(row, 1, "cluster identity");
    if (!fs::path{data_directory}.is_absolute()) {
        fail(
            "cluster identity",
            "PostgreSQL data directory is not absolute");
    }
    const std::uint64_t server_version =
        parse_unsigned(
            nonnull_cell(row, 3, "cluster identity"),
            "cluster identity server version");
    if (server_version / 10000 != 17) {
        fail(
            "cluster identity",
            "runtime evidence requires PostgreSQL 17");
    }
    return json{
        {"cluster_system_identifier",
         nonnull_cell(row, 0, "cluster identity")},
        {"cluster_data_directory",
         fs::path{data_directory}.lexically_normal().string()},
        {"postmaster_start_time",
         nonnull_cell(row, 2, "cluster identity")},
        {"server_version_num", server_version},
        {"backend_pid",
         parse_unsigned(
             nonnull_cell(row, 4, "cluster identity"),
             "cluster identity backend PID")},
        {"server_address",
         row[5].has_value()
             ? json{*row[5]}
             : json{nullptr}},
        {"database",
         nonnull_cell(row, 6, "cluster identity")},
        {"user",
         nonnull_cell(row, 7, "cluster identity")},
        {"superuser",
         nonnull_cell(row, 8, "cluster identity") == "true"},
        {"in_recovery",
         nonnull_cell(row, 9, "cluster identity") == "true"}};
}

void validate_private_cluster_identity(
        const json& cluster,
        const json& config) {
    const fs::path expected_data_directory =
        fs::canonical(require_absolute_path(
            config,
            "expected_data_directory",
            "cluster-config"));
    const fs::path observed_data_directory =
        fs::canonical(fs::path{require_string_field(
            cluster,
            "cluster_data_directory",
            "cluster-identity")});
    if (observed_data_directory != expected_data_directory
        || require_string_field(
               cluster,
               "cluster_system_identifier",
               "cluster-identity")
            != require_string_field(
                config,
                "expected_system_identifier",
                "cluster-config")
        || !require_field(
                cluster, "server_address", "cluster-identity")
                .is_null()
        || require_string_field(
               cluster, "database", "cluster-identity")
            != "bioseq_safety_runtime"
        || require_string_field(
               cluster, "user", "cluster-identity")
            != parse_connection(config).user
        || require_field(
               cluster, "superuser", "cluster-identity")
            != true
        || require_field(
               cluster, "in_recovery", "cluster-identity")
            != false) {
        fail(
            "cluster-identity",
            "connection is not the authorized local private cluster");
    }
    require_private_authorization(
        require_absolute_path(
            config, "authorization_file", "cluster-config"),
        expected_data_directory);
}

struct sql_manifest_contract final {
    std::string sha256;
    std::string script_sha256;
    guc_manifest_contract gucs;
    std::map<std::string, std::string, std::less<>>
        symbol_by_identity;
    std::set<std::string, std::less<>> symbols;
    std::set<std::string, std::less<>> finfo_symbols;
};

[[nodiscard]] guc_manifest_contract load_guc_manifest_contract(
        const json& manifest) {
    constexpr std::string_view context = "SQL manifest.guc_generation";
    const json& generation =
        require_field(manifest, "guc_generation", "SQL manifest");
    require_exact_fields(
        generation,
        {
            "description_authority",
            "owner_facets",
            "parity_ledger",
            "profile",
            "profile_delta_proof",
            "registration_count",
            "registrations",
            "registry_content_sha256",
            "registry_module",
        },
        context);
    const std::string profile =
        require_string_field(generation, "profile", context);
    const guc_profile_counts expected =
        expected_guc_profile(profile);
    if (profile != kProfile
        || require_string_field(
               generation, "description_authority", context)
            != "cleaned_doxygen_only") {
        fail(context, "profile or description authority differs");
    }

    const json& parity =
        require_field(generation, "parity_ledger", context);
    require_exact_fields(
        parity, {"path", "sha256"}, "SQL manifest GUC parity ledger");
    const std::string parity_sha256 = require_string_field(
        parity, "sha256", "SQL manifest GUC parity ledger");
    if (require_string_field(
            parity, "path", "SQL manifest GUC parity ledger")
            != "main/bioseq/testdata/doc0388_guc_parity_ledger.json"
        || !lowercase_sha256(parity_sha256)) {
        fail(
            "SQL manifest GUC parity ledger",
            "path or SHA-256 differs");
    }

    const std::string registry_module = require_string_field(
        generation, "registry_module", context);
    const std::string registry_sha256 = require_string_field(
        generation, "registry_content_sha256", context);
    if (registry_module
            != fmt::format(
                "bioseq.sql.generated.{}.guc_registry", profile)
        || !lowercase_sha256(registry_sha256)) {
        fail(context, "registry module or content SHA-256 differs");
    }

    const json& owner_facets =
        require_field(generation, "owner_facets", context);
    static constexpr std::array<std::string_view, 4> owner_order{
        "bioseq", "scheduler", "storage", "aligner"};
    static constexpr std::array<std::uint64_t, 4> production_counts{
        29, 3, 1, 43};
    if (!owner_facets.is_array()
        || owner_facets.size() != owner_order.size()) {
        fail(context, "owner-facet inventory differs");
    }
    std::map<std::string, json, std::less<>> facets;
    for (std::size_t index = 0; index < owner_facets.size(); ++index) {
        const json& facet = owner_facets[index];
        const std::string facet_context = fmt::format(
            "{}.owner_facets[{}]", context, index);
        require_exact_fields(
            facet,
            {
                "owner",
                "source_module",
                "generated_module",
                "content_sha256",
                "registration_count",
            },
            facet_context);
        const std::string owner =
            require_string_field(facet, "owner", facet_context);
        const std::string expected_source =
            owner == "bioseq"
            ? "bioseq.sql_guc_settings"
            : fmt::format(
                  "bioseq.{}.sql_guc_settings", owner);
        const std::uint64_t count = parse_unsigned(
            require_field(
                facet, "registration_count", facet_context).dump(),
            fmt::format("{}.registration_count", facet_context));
        const std::uint64_t expected_count =
            production_counts[index]
            + ((profile == "test_indexes" && owner == "bioseq")
                   ? 1U
                   : 0U);
        if (owner != owner_order[index]
            || require_string_field(
                   facet, "source_module", facet_context)
                != expected_source
            || require_string_field(
                   facet, "generated_module", facet_context)
                != fmt::format(
                    "bioseq.sql.generated.{}.{}.gucs",
                    profile,
                    owner)
            || !lowercase_sha256(require_string_field(
                facet, "content_sha256", facet_context))
            || count != expected_count
            || !facets.emplace(owner, facet).second) {
            fail(facet_context, "owner-facet authority differs");
        }
    }

    const json& delta =
        require_field(generation, "profile_delta_proof", context);
    require_exact_fields(
        delta,
        {
            "production_registry_content_sha256",
            "test_indexes_registry_content_sha256",
            "added_in_test_indexes",
            "removed_from_test_indexes",
            "changed_in_test_indexes",
            "owner_descriptor_profile_invariant",
            "owner_artifacts",
        },
        "SQL manifest GUC profile delta");
    const json expected_added =
        json::array({"bioseq.test_force_paper_index_failure"});
    if (require_field(
            delta,
            "added_in_test_indexes",
            "SQL manifest GUC profile delta")
            != expected_added
        || require_field(
               delta,
               "removed_from_test_indexes",
               "SQL manifest GUC profile delta")
            != json::array()
        || require_field(
               delta,
               "changed_in_test_indexes",
               "SQL manifest GUC profile delta")
            != json::array()) {
        fail(
            "SQL manifest GUC profile delta",
            "the sole profile delta differs");
    }
    const std::string production_registry_sha =
        require_string_field(
            delta,
            "production_registry_content_sha256",
            "SQL manifest GUC profile delta");
    const std::string test_registry_sha =
        require_string_field(
            delta,
            "test_indexes_registry_content_sha256",
            "SQL manifest GUC profile delta");
    if (!lowercase_sha256(production_registry_sha)
        || !lowercase_sha256(test_registry_sha)
        || registry_sha256
            != (profile == "production"
                    ? production_registry_sha
                    : test_registry_sha)) {
        fail(
            "SQL manifest GUC profile delta",
            "current registry hash is not closed by the profile proof");
    }
    const json& invariants = require_field(
        delta,
        "owner_descriptor_profile_invariant",
        "SQL manifest GUC profile delta");
    require_exact_fields(
        invariants,
        {"bioseq", "scheduler", "storage", "aligner"},
        "SQL manifest GUC owner invariants");
    const json& owner_artifacts = require_field(
        delta,
        "owner_artifacts",
        "SQL manifest GUC profile delta");
    require_exact_fields(
        owner_artifacts,
        {"bioseq", "scheduler", "storage", "aligner"},
        "SQL manifest GUC owner artifacts");
    for (const std::string_view owner : owner_order) {
        const json& invariant =
            require_field(
                invariants, owner, "SQL manifest GUC owner invariants");
        if (!invariant.is_boolean()
            || invariant.get<bool>() != (owner != "bioseq")) {
            fail(
                "SQL manifest GUC owner invariants",
                "owner profile-invariance proof differs");
        }
        const json& artifacts = require_field(
            owner_artifacts,
            owner,
            "SQL manifest GUC owner artifacts");
        require_exact_fields(
            artifacts,
            {
                "production_content_sha256",
                "test_indexes_content_sha256",
            },
            "SQL manifest GUC owner artifact");
        const std::string production_sha = require_string_field(
            artifacts,
            "production_content_sha256",
            "SQL manifest GUC owner artifact");
        const std::string test_sha = require_string_field(
            artifacts,
            "test_indexes_content_sha256",
            "SQL manifest GUC owner artifact");
        if (!lowercase_sha256(production_sha)
            || !lowercase_sha256(test_sha)
            || require_string_field(
                   facets.at(std::string{owner}),
                   "content_sha256",
                   "SQL manifest GUC owner facet")
                != (profile == "production"
                        ? production_sha
                        : test_sha)) {
            fail(
                "SQL manifest GUC owner artifact",
                "owner facet is not closed by the profile proof");
        }
    }

    const json& registrations =
        require_field(generation, "registrations", context);
    const std::uint64_t registration_count = parse_unsigned(
        require_field(
            generation, "registration_count", context).dump(),
        "SQL manifest.guc_generation.registration_count");
    if (!registrations.is_array()
        || registrations.size() != registration_count
        || registration_count != expected.total) {
        fail(context, "registration count differs");
    }
    std::vector<json> sealed_registrations;
    sealed_registrations.reserve(registrations.size());
    std::set<std::string, std::less<>> names;
    std::map<std::string, std::uint64_t, std::less<>> owner_counts;
    std::optional<std::uint64_t> previous_ordinal;
    for (std::size_t index = 0; index < registrations.size(); ++index) {
        const json& registration = registrations[index];
        const std::string registration_context =
            fmt::format("{}.registrations[{}]", context, index);
        require_exact_fields(
            registration,
            {
                "authority",
                "candidate",
                "generated_live_authority",
            },
            registration_context);
        const json& authority = require_field(
            registration, "authority", registration_context);
        const json& candidate = require_field(
            registration, "candidate", registration_context);
        const json& generated_live_authority = require_field(
            registration,
            "generated_live_authority",
            registration_context);
        require_exact_fields(
            authority,
            {
                "registration_ordinal",
                "source_owner",
                "name",
                "group",
                "member",
                "kind",
                "storage_kind",
                "storage",
                "default_expression",
                "minimum_expression",
                "maximum_expression",
                "context",
                "flags",
                "unit",
                "hooks",
                "feature_predicate",
                "profiles",
                "default_semantic",
                "enum_rows_name_wire_hidden_feature_alias_of",
                "enum_omissions_name_wire",
            },
            registration_context);
        require_exact_fields(
            candidate,
            {
                "owner",
                "cpp_storage",
                "storage_kind",
                "source_module",
                "source_sha256",
            },
            registration_context);
        const std::uint64_t ordinal = parse_unsigned(
            require_field(
                authority,
                "registration_ordinal",
                registration_context).dump(),
            fmt::format(
                "{}.registration_ordinal", registration_context));
        const std::string name = require_string_field(
            authority, "name", registration_context);
        const std::string group = require_string_field(
            authority, "group", registration_context);
        const std::string member = require_string_field(
            authority, "member", registration_context);
        const std::string owner = require_string_field(
            candidate, "owner", registration_context);
        const std::string owner_key = guc_owner_key(owner);
        const std::string expected_name =
            group + "." + member;
        const std::string kind = require_string_field(
            authority, "kind", registration_context);
        static_cast<void>(expected_pg_guc_kind(kind));
        static_cast<void>(expected_pg_guc_context(
            require_string_field(
                authority, "context", registration_context)));
        if ((previous_ordinal.has_value()
                && ordinal <= *previous_ordinal)
            || name != expected_name
            || group != owner
            || !names.insert(name).second
            || !facets.contains(owner_key)
            || require_string_field(
                   authority, "storage_kind", registration_context)
                != require_string_field(
                    candidate, "storage_kind", registration_context)
            || require_string_field(
                   candidate, "source_module", registration_context)
                != require_string_field(
                    facets.at(owner_key),
                    "source_module",
                    registration_context)
            || !lowercase_sha256(require_string_field(
                candidate, "source_sha256", registration_context))) {
            fail(
                registration_context,
                "registration identity, order, or ownership differs");
        }
        previous_ordinal = ordinal;
        ++owner_counts[owner_key];
        const json& flags =
            require_field(authority, "flags", registration_context);
        std::set<std::string, std::less<>> unique_flags;
        if (!flags.is_array()) {
            fail(registration_context, "flags are not an array");
        }
        for (const json& flag : flags) {
            const std::string value =
                require_string(flag, registration_context);
            if ((value != "no_show_all"
                 && value != "not_in_sample")
                || !unique_flags.insert(value).second) {
                fail(
                    registration_context,
                    "flags contain an unsupported or duplicate value");
            }
        }
        const json& hooks =
            require_field(authority, "hooks", registration_context);
        if (!hooks.is_array() || hooks.size() != 3) {
            fail(registration_context, "hook tuple differs");
        }
        for (const json& hook : hooks) {
            if (!hook.is_null() && !hook.is_string()) {
                fail(registration_context, "hook value is malformed");
            }
        }
        const bool test_only =
            name == "bioseq.test_force_paper_index_failure";
        const json expected_profiles =
            test_only
            ? json::array({"test_indexes"})
            : json::array({"production", "test_indexes"});
        const std::string feature = require_string_field(
            authority, "feature_predicate", registration_context);
        if (require_field(
                authority, "profiles", registration_context)
                != expected_profiles
            || feature
                != (test_only
                        ? "defined(BIOSEQ_TEST_INDEXES)"
                        : "true")
            || (test_only && profile != "test_indexes")) {
            fail(
                registration_context,
                "registration profile authority differs");
        }
        static_cast<void>(require_string_field(
            authority, "source_owner", registration_context));
        static_cast<void>(require_string_field(
            authority, "storage", registration_context));
        static_cast<void>(require_string_field(
            authority, "default_expression", registration_context));
        static_cast<void>(require_string_field(
            candidate, "cpp_storage", registration_context));
        const json& minimum = require_field(
            authority, "minimum_expression", registration_context);
        const json& maximum = require_field(
            authority, "maximum_expression", registration_context);
        const bool bounded = kind == "integer" || kind == "real";
        if ((bounded
                && (!minimum.is_string() || !maximum.is_string()))
            || (!bounded
                && (!minimum.is_null() || !maximum.is_null()))) {
            fail(
                registration_context,
                "minimum/maximum authority differs for the GUC kind");
        }
        const json& unit =
            require_field(authority, "unit", registration_context);
        if (!unit.is_null()
            && (!unit.is_string()
                || unit.get<std::string>() != "mb"
                || kind != "integer")) {
            fail(registration_context, "unit authority differs");
        }
        const json& enum_rows = require_field(
            authority,
            "enum_rows_name_wire_hidden_feature_alias_of",
            registration_context);
        const json& enum_omissions = require_field(
            authority,
            "enum_omissions_name_wire",
            registration_context);
        const json& default_semantic = require_field(
            authority, "default_semantic", registration_context);
        if (!enum_rows.is_array() || !enum_omissions.is_array()
            || (kind == "enum"
                && (enum_rows.empty()
                    || !default_semantic.is_string()))
            || (kind != "enum"
                && (!enum_rows.empty()
                    || !enum_omissions.empty()
                    || !default_semantic.is_null()))) {
            fail(registration_context, "enum authority differs");
        }
        std::set<std::string, std::less<>> enum_names;
        for (const json& row : enum_rows) {
            if (!row.is_array() || row.size() != 5
                || !row[0].is_string()
                || !row[1].is_string()
                || !row[2].is_number_integer()
                || (row[2].get<int>() != 0
                    && row[2].get<int>() != 1)
                || !row[3].is_string()
                || (!row[4].is_null() && !row[4].is_string())
                || !enum_names.insert(
                        row[0].get<std::string>()).second) {
                fail(registration_context, "enum row is malformed");
            }
        }
        for (const json& omission : enum_omissions) {
            if (!omission.is_array() || omission.size() != 2
                || !omission[0].is_string()
                || !omission[1].is_string()) {
                fail(registration_context, "enum omission is malformed");
            }
        }
        validate_generated_live_guc_authority(
            authority,
            generated_live_authority,
            fmt::format(
                "{}.generated_live_authority",
                registration_context));
        sealed_registrations.push_back(registration);
    }
    for (std::size_t index = 0; index < owner_order.size(); ++index) {
        if (owner_counts[std::string{owner_order[index]}]
            != production_counts[index]
                + ((profile == "test_indexes"
                    && owner_order[index] == "bioseq")
                       ? 1U
                       : 0U)) {
            fail(context, "owner registration counts differ");
        }
    }
    if (names.contains("bioseq.test_force_paper_index_failure")
        != (profile == "test_indexes")) {
        fail(context, "sole test-profile GUC delta differs");
    }
    return guc_manifest_contract{
        .profile = profile,
        .parity_ledger_sha256 = parity_sha256,
        .registry_module = registry_module,
        .registry_content_sha256 = registry_sha256,
        .profile_delta_proof_sha256 =
            sha256_bytes(canonical_json(delta)),
        .owner_facets = std::move(facets),
        .registrations = std::move(sealed_registrations)};
}

[[nodiscard]] std::set<std::string, std::less<>>
manifest_string_set(
        const json& manifest,
        std::string_view field) {
    const json& values =
        require_field(manifest, field, "SQL manifest");
    if (!values.is_array() || values.empty()) {
        fail(
            fmt::format("SQL manifest.{}", field),
            "expected a non-empty array");
    }
    std::set<std::string, std::less<>> result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::string value = require_string(
            values[index],
            fmt::format(
                "SQL manifest.{}[{}]", field, index));
        if (!result.insert(value).second) {
            fail(
                fmt::format("SQL manifest.{}", field),
                "contains a duplicate");
        }
    }
    return result;
}

[[nodiscard]] sql_manifest_contract load_sql_manifest() {
    const fs::path manifest_path{kSqlManifest};
    const fs::path sql_path{kSqlFile};
    if (!manifest_path.is_absolute()
        || !sql_path.is_absolute()
        || !fs::is_regular_file(manifest_path)
        || !fs::is_regular_file(sql_path)
        || fs::is_symlink(manifest_path)
        || fs::is_symlink(sql_path)) {
        fail(
            "SQL manifest",
            "compiled SQL artifact paths are unavailable");
    }
    const std::string raw = read_file(manifest_path);
    const json manifest = parse_json(raw, "SQL manifest");
    if (require_field(
            manifest, "manifest_version", "SQL manifest")
            != 1
        || require_string_field(
               manifest, "authority_mode", "SQL manifest")
            != "annotated_cpp"
        || require_string_field(
               manifest, "compiler_profile", "SQL manifest")
            != kProfile) {
        fail(
            "SQL manifest",
            "version, authority, or profile differs");
    }
    const std::string script_sha256 =
        require_string_field(
            manifest, "script_sha256", "SQL manifest");
    if (!lowercase_sha256(script_sha256)
        || sha256_file(sql_path) != script_sha256) {
        fail(
            "SQL manifest",
            "script_sha256 differs from generated SQL bytes");
    }
    const std::set<std::string, std::less<>> identities =
        manifest_string_set(
            manifest, "c_routine_identities");
    const std::set<std::string, std::less<>> symbols =
        manifest_string_set(manifest, "c_link_symbols");
    const std::set<std::string, std::less<>> finfo =
        manifest_string_set(manifest, "c_finfo_symbols");
    std::set<std::string, std::less<>> expected_finfo;
    for (const std::string& symbol : symbols) {
        expected_finfo.insert("pg_finfo_" + symbol);
    }
    if (finfo != expected_finfo) {
        fail(
            "SQL manifest",
            "finfo inventory differs from the exact C symbol set");
    }
    std::map<std::string, std::string, std::less<>>
        symbol_by_identity;
    const json& owners = require_field(
        manifest, "c_symbol_owners", "SQL manifest");
    if (!owners.is_array()
        || owners.size() != symbols.size()) {
        fail(
            "SQL manifest.c_symbol_owners",
            "owner inventory does not close over symbols");
    }
    std::set<std::string, std::less<>> owner_symbols;
    for (std::size_t index = 0; index < owners.size(); ++index) {
        const json& owner = owners[index];
        const std::string context =
            fmt::format(
                "SQL manifest.c_symbol_owners[{}]", index);
        const std::string symbol =
            require_string_field(owner, "symbol", context);
        if (!owner_symbols.insert(symbol).second
            || !symbols.contains(symbol)
            || require_string_field(
                   owner, "finfo_symbol", context)
                != "pg_finfo_" + symbol
            || require_field(
                   owner, "generated_wrapper", context)
                != true) {
            fail(
                context,
                "symbol owner is duplicate or not generated");
        }
        const json& routine_identities =
            require_field(
                owner, "routine_identities", context);
        if (!routine_identities.is_array()
            || routine_identities.empty()) {
            fail(
                context,
                "routine identity group is empty");
        }
        for (std::size_t identity_index = 0;
             identity_index < routine_identities.size();
             ++identity_index) {
            const std::string identity = require_string(
                routine_identities[identity_index],
                fmt::format(
                    "{}.routine_identities[{}]",
                    context,
                    identity_index));
            if (!identities.contains(identity)
                || !symbol_by_identity.emplace(
                        identity, symbol).second) {
                fail(
                    context,
                    "routine identity owner differs from inventory");
            }
        }
    }
    if (owner_symbols != symbols
        || symbol_by_identity.size() != identities.size()) {
        fail(
            "SQL manifest",
            "exact identity/symbol ownership closure differs");
    }
    guc_manifest_contract gucs =
        load_guc_manifest_contract(manifest);
    return sql_manifest_contract{
        .sha256 = sha256_bytes(raw),
        .script_sha256 = script_sha256,
        .gucs = std::move(gucs),
        .symbol_by_identity = std::move(symbol_by_identity),
        .symbols = symbols,
        .finfo_symbols = finfo};
}

[[nodiscard]] std::uint64_t verify_installed_catalog(
        const pg_connection& connection,
        const sql_manifest_contract& manifest) {
    json identities = json::array();
    for (const auto& [identity, symbol] :
         manifest.symbol_by_identity) {
        static_cast<void>(symbol);
        identities.push_back(identity);
    }
    const std::array<std::string, 1> parameters{
        identities.dump()};
    const pg_result exact =
        connection.execute_parameters(
            "WITH expected AS ("
            " SELECT identity, ordinal"
            " FROM pg_catalog.jsonb_array_elements_text($1::jsonb)"
            "      WITH ORDINALITY AS value(identity, ordinal)"
            "), resolved AS ("
            " SELECT identity, ordinal,"
            "        pg_catalog.to_regprocedure(identity) AS oid"
            " FROM expected"
            ")"
            " SELECT resolved.identity, p.oid::text, p.prosrc,"
            "        p.probin, language.lanname::text,"
            "        extension.oid::text, p.prokind::text"
            " FROM resolved"
            " LEFT JOIN pg_catalog.pg_proc AS p"
            "   ON p.oid = resolved.oid"
            " LEFT JOIN pg_catalog.pg_language AS language"
            "   ON language.oid = p.prolang"
            " LEFT JOIN pg_catalog.pg_depend AS dependency"
            "   ON dependency.classid = 'pg_catalog.pg_proc'::regclass"
            "  AND dependency.objid = p.oid"
            "  AND dependency.objsubid = 0"
            "  AND dependency.refclassid ="
            "      'pg_catalog.pg_extension'::regclass"
            "  AND dependency.deptype = 'e'"
            " LEFT JOIN pg_catalog.pg_extension AS extension"
            "   ON extension.oid = dependency.refobjid"
            "  AND extension.extname = 'bioseq'"
            " ORDER BY resolved.ordinal;",
            parameters);
    const auto catalog_rows =
        rows(exact, "installed C routine catalog");
    if (catalog_rows.size()
        != manifest.symbol_by_identity.size()) {
        fail(
            "installed C routine catalog",
            "resolved row count differs from manifest");
    }
    std::set<std::uint64_t> expected_oids;
    std::optional<std::uint64_t> extension_oid;
    std::size_t row_index = 0;
    for (const auto& [identity, symbol] :
         manifest.symbol_by_identity) {
        const auto& row = catalog_rows[row_index++];
        if (row.size() != 7
            || nonnull_cell(
                   row, 0, "installed C routine catalog")
                != identity
            || nonnull_cell(
                   row, 2, "installed C routine catalog")
                != symbol
            || nonnull_cell(
                   row, 3, "installed C routine catalog")
                != "$libdir/bioseq"
            || nonnull_cell(
                   row, 4, "installed C routine catalog")
                != "c"
            || (nonnull_cell(
                    row, 6, "installed C routine catalog")
                    != "f"
                && nonnull_cell(
                    row, 6, "installed C routine catalog")
                    != "p")) {
            fail(
                "installed C routine catalog",
                "identity, symbol, language, kind, or library differs");
        }
        const std::uint64_t oid = parse_unsigned(
            nonnull_cell(
                row, 1, "installed C routine catalog"),
            "installed C routine OID");
        const std::uint64_t owner_oid = parse_unsigned(
            nonnull_cell(
                row, 5, "installed C routine catalog"),
            "installed extension OID");
        if (!expected_oids.insert(oid).second
            || (extension_oid.has_value()
                && *extension_oid != owner_oid)) {
            fail(
                "installed C routine catalog",
                "duplicate routine OID or extension owner");
        }
        extension_oid = owner_oid;
    }
    const pg_result extension_routines = connection.execute(
        "SELECT p.oid::text"
        " FROM pg_catalog.pg_proc AS p"
        " JOIN pg_catalog.pg_language AS language"
        "   ON language.oid = p.prolang"
        " JOIN pg_catalog.pg_depend AS dependency"
        "   ON dependency.classid = 'pg_catalog.pg_proc'::regclass"
        "  AND dependency.objid = p.oid"
        "  AND dependency.objsubid = 0"
        "  AND dependency.refclassid ="
        "      'pg_catalog.pg_extension'::regclass"
        "  AND dependency.deptype = 'e'"
        " JOIN pg_catalog.pg_extension AS extension"
        "   ON extension.oid = dependency.refobjid"
        " WHERE extension.extname = 'bioseq'"
        "   AND language.lanname = 'c'"
        "   AND p.prokind IN ('f','p')"
        " ORDER BY p.oid;");
    const auto extension_rows =
        rows(extension_routines, "extension C routine OIDs");
    std::set<std::uint64_t> observed_oids;
    for (const auto& row : extension_rows) {
        if (!observed_oids.insert(parse_unsigned(
                nonnull_cell(
                    row, 0, "extension C routine OIDs"),
                "extension C routine OID")).second) {
            fail(
                "extension C routine OIDs",
                "contains a duplicate");
        }
    }
    if (observed_oids != expected_oids
        || !extension_oid.has_value()) {
        fail(
            "installed C routine catalog",
            "extension-owned C OID set differs from manifest");
    }
    const pg_result languages = connection.execute(
        "SELECT count(*) FILTER (WHERE l.lanname = 'plpgsql')::text"
        " FROM pg_catalog.pg_proc AS p"
        " JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang"
        " JOIN pg_catalog.pg_depend AS d"
        "   ON d.classid = 'pg_catalog.pg_proc'::regclass"
        "  AND d.objid = p.oid AND d.objsubid = 0"
        " JOIN pg_catalog.pg_extension AS e"
        "   ON d.refclassid = 'pg_catalog.pg_extension'::regclass"
        "  AND d.refobjid = e.oid AND d.deptype = 'e'"
        " WHERE e.extname = 'bioseq';");
    const auto language_row =
        one_row(languages, "installed language catalog");
    if (nonnull_cell(
            language_row, 0, "installed language catalog") != "0") {
        fail(
            "installed language catalog",
            "extension still owns a PL/pgSQL routine");
    }
    return *extension_oid;
}

[[nodiscard]] int activate(
        const command_line& command) {
    require_exact_options(
        command,
        {"--profile", "--cluster-config", "--dso", "--output"});
    const std::string profile =
        require_option(command, "--profile");
    if (profile != kProfile) {
        fail("activation", "requested profile differs from executable");
    }
    const fs::path config_path =
        fs::absolute(
            require_option(command, "--cluster-config"))
            .lexically_normal();
    const fs::path requested_dso =
        fs::absolute(require_option(command, "--dso"))
            .lexically_normal();
    const fs::path output =
        fs::absolute(require_option(command, "--output"))
            .lexically_normal();
    const std::string config_raw = read_file(config_path);
    const json config =
        parse_json(config_raw, "cluster-config");
    validate_cluster_config(config, profile);
    if (!fs::is_regular_file(requested_dso)
        || fs::is_symlink(requested_dso)) {
        fail(
            "activation", "requested DSO must be a regular non-symlink file");
    }
    const fs::path control_source{kControlFile};
    const fs::path sql_source{kSqlFile};
    const fs::path control_dso_source{kControlDso};
    const sql_manifest_contract manifest =
        load_sql_manifest();
    if (!control_source.is_absolute()
        || !sql_source.is_absolute()
        || !control_dso_source.is_absolute()
        || !fs::is_regular_file(control_source)
        || !fs::is_regular_file(sql_source)
        || !fs::is_regular_file(control_dso_source)
        || fs::is_symlink(control_source)
        || fs::is_symlink(sql_source)
        || fs::is_symlink(control_dso_source)) {
        fail(
            "activation",
            "compiled control, SQL, manifest, or helper inputs are unavailable");
    }
    const fs::path installed_dso = require_absolute_path(
        config, "installed_dso_path", "cluster-config");
    const fs::path installed_control_dso = require_absolute_path(
        config, "installed_control_dso_path", "cluster-config");
    const fs::path installed_control = require_absolute_path(
        config, "installed_control_path", "cluster-config");
    const fs::path installed_sql = require_absolute_path(
        config, "installed_sql_path", "cluster-config");
    const std::string nonce = random_token();
    const std::string dso_sha256 =
        sha256_file(requested_dso);
    const std::string control_sha256 =
        sha256_file(control_source);
    const std::string sql_sha256 =
        sha256_file(sql_source);
    const std::string control_dso_sha256 =
        sha256_file(control_dso_source);
    if (sql_sha256 != manifest.script_sha256) {
        fail(
            "activation",
            "generated SQL differs from its manifest");
    }
    install_artifact(
        requested_dso,
        installed_dso,
        dso_sha256,
        nonce.substr(0, 16));
    install_artifact(
        control_source,
        installed_control,
        control_sha256,
        nonce.substr(16, 16));
    install_artifact(
        sql_source,
        installed_sql,
        sql_sha256,
        nonce.substr(32, 16));
    install_artifact(
        control_dso_source,
        installed_control_dso,
        control_dso_sha256,
        nonce.substr(48, 16));
    run_command(
        require_field(
            config, "restart_command", "cluster-config"),
        "cluster restart");
    const connection_parameters parameters =
        parse_connection(config);
    std::unique_ptr<pg_connection> connection =
        connect_with_retry(parameters);
    const json initial_cluster =
        query_cluster_identity(*connection);
    validate_private_cluster_identity(
        initial_cluster, config);
    require_success(
        connection->execute(
            "DROP EXTENSION IF EXISTS bioseq CASCADE;"),
        "drop extension");
    require_success(
        connection->execute(
            "DROP SCHEMA IF EXISTS bioseq_internal CASCADE;"),
        "drop internal schema");
    require_success(
        connection->execute(
            "DROP SCHEMA IF EXISTS bioseq CASCADE;"),
        "drop public schema");
    require_success(
        connection->execute("CREATE EXTENSION bioseq;"),
        "create extension");
    require_success(
        connection->execute(
            "LOAD " + sql_literal(installed_dso.string()) + ";"),
        "load requested DSO");
    if (sha256_file(installed_dso) != dso_sha256) {
        fail(
            "activation", "active DSO bytes changed after PostgreSQL load");
    }
    const json cluster = query_cluster_identity(*connection);
    validate_private_cluster_identity(cluster, config);
    create_guc_contract_binding(
        *connection, installed_control_dso);
    const installed_guc_profile guc_profile =
        verify_installed_guc_profile(*connection, manifest.gucs);
    const std::uint64_t extension_oid =
        verify_installed_catalog(*connection, manifest);
    const json receipt{
        {"schema_version", 1},
        {"profile", profile},
        {"case_plan_sha256", kCasePlanSha256},
        {"test_executable_sha256", self_sha256(command)},
        {"requested_dso_sha256", dso_sha256},
        {"cluster_config_sha256", sha256_bytes(config_raw)},
        {"active_dso_path",
         fs::absolute(installed_dso).lexically_normal().string()},
        {"active_dso_sha256", dso_sha256},
        {"cluster_system_identifier",
         cluster.at("cluster_system_identifier")},
        {"cluster_data_directory",
         cluster.at("cluster_data_directory")},
        {"postmaster_start_time",
         cluster.at("postmaster_start_time")},
        {"activation_nonce", nonce},
        {"server_version_num",
         cluster.at("server_version_num")},
        {"extension_oid",
         extension_oid},
        {"bioseq_guc_count",
         guc_profile.counts.total},
        {"bioseq_test_only_guc_count",
         guc_profile.counts.test_only},
        {"bioseq_guc_properties",
         guc_profile.properties},
        {"bioseq_guc_properties_sha256",
         guc_profile.properties_sha256},
        {"installed_control_sha256", control_sha256},
        {"installed_sql_sha256", sql_sha256},
        {"sql_manifest_sha256", manifest.sha256},
        {"sql_script_sha256", manifest.script_sha256},
        {"control_dso_path",
         fs::canonical(installed_control_dso).string()},
        {"control_dso_sha256", control_dso_sha256}};
    write_atomic(output, canonical_json(receipt));
    return 0;
}

[[nodiscard]] int describe(
        const command_line& command) {
    require_exact_options(
        command, {"--profile", "--output"});
    const std::string profile =
        require_option(command, "--profile");
    if (profile != kProfile) {
        fail("describe", "requested profile differs from executable");
    }
    const fs::path output =
        fs::absolute(require_option(command, "--output"))
            .lexically_normal();
    const case_plan plan = load_case_plan();
    json cases = json::array();
    for (const auto& [case_id_value, definition] : plan.cases) {
        static_cast<void>(case_id_value);
        cases.push_back(definition.catalog);
    }
    const json catalog{
        {"schema_version", 2},
        {"profile", profile},
        {"case_plan_sha256", kCasePlanSha256},
        {"test_executable_sha256", self_sha256(command)},
        {"cases", std::move(cases)}};
    write_atomic(output, canonical_json(catalog));
    return 0;
}

[[nodiscard]] std::set<std::string, std::less<>>
string_set(
        const json& value,
        std::string_view context) {
    if (!value.is_array()) {
        fail(context, "expected an array");
    }
    std::set<std::string, std::less<>> result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item = require_string(
            value[index],
            fmt::format("{}[{}]", context, index));
        if (!result.insert(item).second) {
            fail(context, "contains a duplicate string");
        }
    }
    return result;
}

[[nodiscard]] json resolve_ledger_site(
        const json& ledger,
        const json& catalog,
        std::string_view requested_site_sha256) {
    if (require_field(
            ledger, "schema_version", "ledger") != 2
        || require_string_field(
               ledger, "profile", "ledger") != kProfile) {
        fail("ledger", "schema or profile differs");
    }
    const std::string sql_identity =
        require_string_field(
            catalog, "sql_identity", "catalog-entry");
    const json& parents =
        require_field(ledger, "parents", "ledger");
    if (!parents.is_array()) {
        fail("ledger.parents", "expected an array");
    }
    const json* matching_parent = nullptr;
    for (const json& parent : parents) {
        if (require_string_field(
                parent, "sql_identity", "ledger-parent")
            == sql_identity) {
            if (matching_parent != nullptr) {
                fail("ledger", "duplicate SQL identity");
            }
            matching_parent = &parent;
        }
    }
    if (matching_parent == nullptr) {
        fail("ledger", "catalog SQL identity is absent");
    }
    const json& sites = require_field(
        *matching_parent, "sites", "ledger-parent");
    if (!sites.is_array()) {
        fail("ledger-parent.sites", "expected an array");
    }
    const json* matching_site = nullptr;
    for (const json& site : sites) {
        if (sha256_bytes(canonical_json(site))
            == requested_site_sha256) {
            if (matching_site != nullptr) {
                fail(
                    "ledger",
                    "duplicate canonical ledger-site SHA-256");
            }
            matching_site = &site;
        }
    }
    if (matching_site == nullptr) {
        fail("ledger", "request ledger-site SHA-256 is absent");
    }
    if (require_string_field(
            catalog,
            "ledger_site_sha256",
            "catalog-entry")
        != requested_site_sha256) {
        fail(
            "ledger",
            "catalog ledger-site SHA-256 differs from request");
    }
    const json& severities = require_field(
        *matching_site, "possible_severities", "ledger-site");
    if (!severities.is_array()
        || severities.size() != 1
        || require_string(
               severities.front(),
               "ledger-site.possible_severities[0]")
            != "error") {
        fail(
            "ledger", "runtime site is not recoverable ERROR-only");
    }
    return *matching_site;
}

struct activation_receipt final {
    json value;
    std::string sha256;
    fs::path control_dso_path;
    std::string control_dso_sha256;
};

[[nodiscard]] activation_receipt load_activation_receipt(
        const json& request,
        const fs::path& active_dso,
        std::string_view dso_sha256,
        std::string_view config_sha256,
        std::string_view executable_sha256,
        const json& config,
        const sql_manifest_contract& manifest) {
    const fs::path path{
        require_string_field(
            request,
            "activation_receipt_path",
            "request")};
    if (!path.is_absolute()
        || !fs::is_regular_file(path)
        || fs::is_symlink(path)) {
        fail(
            "activation-receipt",
            "path must be an existing absolute non-symlink file");
    }
    const std::string raw = read_file(path);
    const std::string actual_sha256 = sha256_bytes(raw);
    if (actual_sha256
        != require_string_field(
            request,
            "activation_receipt_sha256",
            "request")) {
        fail(
            "activation-receipt",
            "hash differs from request");
    }
    const json receipt =
        parse_json(raw, "activation-receipt");
    require_exact_fields(
        receipt,
        {
            "schema_version",
            "profile",
            "case_plan_sha256",
            "test_executable_sha256",
            "requested_dso_sha256",
            "cluster_config_sha256",
            "active_dso_path",
            "active_dso_sha256",
            "cluster_system_identifier",
            "cluster_data_directory",
            "postmaster_start_time",
            "activation_nonce",
            "server_version_num",
            "extension_oid",
            "bioseq_guc_count",
            "bioseq_test_only_guc_count",
            "bioseq_guc_properties",
            "bioseq_guc_properties_sha256",
            "installed_control_sha256",
            "installed_sql_sha256",
            "sql_manifest_sha256",
            "sql_script_sha256",
            "control_dso_path",
            "control_dso_sha256",
        },
        "activation-receipt");
    const guc_profile_counts expected_gucs =
        expected_guc_profile(kProfile);
    const json& guc_properties = require_field(
        receipt,
        "bioseq_guc_properties",
        "activation-receipt");
    const std::string guc_properties_sha256 =
        require_string_field(
            receipt,
            "bioseq_guc_properties_sha256",
            "activation-receipt");
    validate_guc_property_receipt(
        guc_properties,
        guc_properties_sha256,
        manifest.gucs,
        "activation-receipt.bioseq_guc_properties");
    if (require_field(
            receipt, "schema_version", "activation-receipt")
            != 1
        || require_string_field(
               receipt, "profile", "activation-receipt")
            != kProfile
        || fs::path{
               require_string_field(
                   receipt,
                   "active_dso_path",
                   "activation-receipt")}
               .lexically_normal()
            != active_dso.lexically_normal()
        || require_string_field(
               receipt,
               "active_dso_sha256",
               "activation-receipt")
            != dso_sha256
        || require_string_field(
               receipt,
               "requested_dso_sha256",
               "activation-receipt")
            != dso_sha256
        || require_string_field(
               receipt,
               "cluster_config_sha256",
               "activation-receipt")
            != config_sha256
        || require_string_field(
               receipt,
               "test_executable_sha256",
               "activation-receipt")
            != executable_sha256
        || require_string_field(
               receipt,
               "case_plan_sha256",
               "activation-receipt")
            != kCasePlanSha256
        || require_string_field(
               receipt,
               "sql_manifest_sha256",
               "activation-receipt")
            != manifest.sha256
        || require_string_field(
               receipt,
               "sql_script_sha256",
               "activation-receipt")
            != manifest.script_sha256
        || require_string_field(
               receipt,
               "installed_sql_sha256",
               "activation-receipt")
            != manifest.script_sha256
        || require_string_field(
               receipt,
               "installed_control_sha256",
               "activation-receipt")
            != sha256_file(fs::path{kControlFile})
        || parse_unsigned(
               require_field(
                   receipt,
                   "bioseq_guc_count",
                   "activation-receipt")
                   .dump(),
               "activation-receipt.bioseq_guc_count")
            != expected_gucs.total
        || parse_unsigned(
               require_field(
                   receipt,
                   "bioseq_test_only_guc_count",
                   "activation-receipt")
                   .dump(),
               "activation-receipt.bioseq_test_only_guc_count")
            != expected_gucs.test_only) {
        fail(
            "activation-receipt",
            "profile, DSO, SQL, executable, GUC profile, or cluster "
            "binding differs");
    }
    const fs::path control_dso_path{
        require_string_field(
            receipt,
            "control_dso_path",
            "activation-receipt")};
    const std::string control_dso_sha256 =
        require_string_field(
            receipt,
            "control_dso_sha256",
            "activation-receipt");
    const fs::path configured_control_dso =
        require_absolute_path(
            config,
            "installed_control_dso_path",
            "cluster-config");
    if (!control_dso_path.is_absolute()
        || !fs::is_regular_file(control_dso_path)
        || fs::is_symlink(control_dso_path)
        || fs::canonical(control_dso_path)
            != fs::canonical(configured_control_dso)
        || !lowercase_sha256(control_dso_sha256)
        || sha256_file(control_dso_path)
            != control_dso_sha256
        || control_dso_sha256
            != sha256_file(fs::path{kControlDso})
        || fs::canonical(control_dso_path)
            == fs::canonical(active_dso)
        || control_dso_sha256 == dso_sha256
        || parse_unsigned(
               require_field(
                   receipt,
                   "extension_oid",
                   "activation-receipt")
                   .dump(),
               "activation-receipt.extension_oid")
            == 0) {
        fail(
            "activation-receipt",
            "control helper, extension OID, or DSO separation differs");
    }
    return activation_receipt{
        .value = receipt,
        .sha256 = actual_sha256,
        .control_dso_path = fs::canonical(control_dso_path),
        .control_dso_sha256 = control_dso_sha256};
}

struct probe_observation final {
    std::string profile;
    std::uint64_t start_count = 0;
    std::uint64_t end_count = 0;
    std::uint64_t abort_count = 0;
    std::uint64_t site_visit_count = 0;
    std::uint64_t fault_trigger_count = 0;
    std::uint64_t invalid_count = 0;
    std::uint64_t overflow_count = 0;
    bool inside_target = false;
    fs::path mapped_target_path;
    std::string site_id;
    std::string fault_key;
    std::string relation;
    std::uint64_t site_visit_sequence = 0;
    std::uint64_t fault_trigger_sequence = 0;
    std::map<std::string, std::uint64_t, std::less<>>
        destructor_events;
    std::map<std::string, std::uint64_t, std::less<>>
        cleanup_events;
    std::map<std::string, std::uint64_t, std::less<>>
        postgres_cleanup_events;
};

[[nodiscard]] std::vector<std::string_view> split(
        std::string_view value,
        char delimiter) {
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find(delimiter, begin);
        if (end == std::string_view::npos) {
            result.push_back(value.substr(begin));
            break;
        }
        result.push_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    return result;
}

[[nodiscard]] probe_observation parse_probe_observation(
        std::string_view value) {
    const std::vector<std::string_view> lines = split(value, '\n');
    if (lines.empty() || lines.front().empty()) {
        fail("probe observation", "header is absent");
    }
    const std::vector<std::string_view> header =
        split(lines.front(), '\t');
    if (header.size() != 16 || header[0] != "v2") {
        fail("probe observation", "header shape differs");
    }
    const auto parse_boolean =
        [](std::string_view raw, std::string_view context) {
            if (raw == "true") {
                return true;
            }
            if (raw == "false") {
                return false;
            }
            fail(context, "expected true or false");
        };
    const fs::path mapped_target_path{header[10]};
    if (!mapped_target_path.is_absolute()
        || header[10].find_first_of("\t\r\n")
            != std::string_view::npos) {
        fail(
            "probe mapped target path",
            "expected an unambiguous absolute path");
    }
    if (!probe_site_id(header[11])) {
        fail(
            "probe observation",
            "site ID is not a full ABI-v2 probe ID");
    }
    if (!valid_fault_key(header[12])) {
        fail(
            "probe observation",
            "fault key must be 1-128 bytes from [a-z0-9_.]");
    }
    static_cast<void>(
        contract_for_relation(
            header[13], "probe observation"));
    probe_observation result{
        .profile = std::string{header[1]},
        .start_count =
            parse_unsigned(header[2], "probe START count"),
        .end_count =
            parse_unsigned(header[3], "probe END count"),
        .abort_count =
            parse_unsigned(header[4], "probe ABORT count"),
        .site_visit_count =
            parse_unsigned(
                header[5], "probe site-visit count"),
        .fault_trigger_count =
            parse_unsigned(
                header[6], "probe fault-trigger count"),
        .invalid_count =
            parse_unsigned(header[7], "probe invalid count"),
        .overflow_count =
            parse_unsigned(header[8], "probe overflow count"),
        .inside_target =
            parse_boolean(header[9], "probe inside-target state"),
        .mapped_target_path = mapped_target_path,
        .site_id = std::string{header[11]},
        .fault_key = std::string{header[12]},
        .relation = std::string{header[13]},
        .site_visit_sequence =
            parse_unsigned(
                header[14], "probe site-visit sequence"),
        .fault_trigger_sequence =
            parse_unsigned(
                header[15], "probe fault-trigger sequence")};
    for (std::size_t index = 1; index < lines.size(); ++index) {
        if (lines[index].empty()) {
            continue;
        }
        const std::vector<std::string_view> event =
            split(lines[index], '\t');
        if (event.size() != 4
            || event[0] != "event"
            || event[2].empty()) {
            fail("probe observation", "event shape differs");
        }
        std::map<std::string, std::uint64_t, std::less<>>*
            destination = nullptr;
        if (event[1] == "destructor") {
            destination = &result.destructor_events;
        } else if (event[1] == "cleanup") {
            destination = &result.cleanup_events;
        } else if (event[1] == "postgres_cleanup") {
            destination = &result.postgres_cleanup_events;
        } else {
            fail("probe observation", "event kind differs");
        }
        const std::uint64_t count =
            parse_unsigned(event[3], "probe event count");
        if (count == 0
            || !destination->emplace(
                    std::string{event[2]}, count)
                    .second) {
            fail("probe observation", "duplicate event");
        }
    }
    return result;
}

void verify_file_binding(
        const json& request,
        std::string_view path_field,
        std::string_view hash_field,
        std::string_view context) {
    const fs::path path{
        require_string_field(request, path_field, "request")};
    if (!path.is_absolute()
        || !fs::is_regular_file(path)
        || fs::is_symlink(path)) {
        fail(
            context,
            "bound path must be an existing absolute non-symlink file");
    }
    const std::string expected =
        require_string_field(request, hash_field, "request");
    if (!lowercase_sha256(expected)
        || sha256_file(path) != expected) {
        fail(context, "bound file SHA-256 differs");
    }
}

[[nodiscard]] const json& catalog_entry(
        const json& catalog,
        std::string_view case_id_value) {
    if (require_field(
            catalog, "schema_version", "catalog") != 2
        || require_string_field(
               catalog, "profile", "catalog") != kProfile
        || require_string_field(
               catalog,
               "case_plan_sha256",
               "catalog")
            != kCasePlanSha256) {
        fail(
            "catalog",
            "schema, profile, or case-plan binding differs");
    }
    const json& cases = require_field(
        catalog, "cases", "catalog");
    if (!cases.is_array()) {
        fail("catalog.cases", "expected an array");
    }
    const json* result = nullptr;
    for (const json& entry : cases) {
        if (require_string_field(
                entry, "case_id", "catalog-entry")
            == case_id_value) {
            if (result != nullptr) {
                fail("catalog", "duplicate case ID");
            }
            result = &entry;
        }
    }
    if (result == nullptr) {
        fail("catalog", "requested case is absent");
    }
    return *result;
}

void require_equal_field(
        const json& left,
        const json& right,
        std::string_view field,
        std::string_view context) {
    if (require_field(left, field, context)
        != require_field(right, field, context)) {
        fail(
            context,
            fmt::format("field '{}' differs", field));
    }
}

[[nodiscard]] std::uint64_t resolve_target_oid(
        const pg_connection& connection,
        std::string_view regprocedure) {
    const std::array<std::string, 1> parameters{
        std::string{regprocedure}};
    const pg_result result = connection.execute_parameters(
        "SELECT pg_catalog.to_regprocedure($1)::oid::text;",
        parameters);
    const auto row = one_row(result, "target regprocedure");
    return parse_unsigned(
        nonnull_cell(row, 0, "target regprocedure"),
        "target regprocedure OID");
}

void require_idle(
        const pg_connection& connection,
        std::string_view context) {
    if (connection.transaction_status() != PQTRANS_IDLE) {
        fail(
            context,
            "PostgreSQL connection did not recover to PQTRANS_IDLE");
    }
}

void create_probe_bindings(
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
        connection.execute(
            "LOAD " + library + ";"),
        "load safety runtime control DSO");
    require_success(
        connection.execute(
            "CREATE FUNCTION "
            "pg_temp.bioseq_safety_runtime_probe_arm("
            "oid, text, text, text, text, text, text, text) "
            "RETURNS void "
            "AS " + library
            + ", 'bioseq_safety_runtime_probe_arm' "
              "LANGUAGE C STRICT VOLATILE;"),
        "create probe arm binding");
    require_success(
        connection.execute(
            "CREATE FUNCTION "
            "pg_temp.bioseq_safety_runtime_probe_snapshot(text) "
            "RETURNS text "
            "AS " + library
            + ", 'bioseq_safety_runtime_probe_snapshot' "
              "LANGUAGE C STRICT VOLATILE;"),
        "create probe snapshot binding");
    require_success(
        connection.execute(
            "CREATE FUNCTION "
            "pg_temp.bioseq_safety_runtime_probe_disarm(text) "
            "RETURNS void "
            "AS " + library
            + ", 'bioseq_safety_runtime_probe_disarm' "
              "LANGUAGE C STRICT VOLATILE;"),
        "create probe disarm binding");
}

void verify_event_set(
        const std::map<
            std::string,
            std::uint64_t,
            std::less<>>& observed,
        const std::set<std::string, std::less<>>& expected,
        std::string_view context) {
    std::set<std::string, std::less<>> names;
    for (const auto& [name, count] : observed) {
        if (count != 1) {
            fail(
                context,
                fmt::format(
                    "event '{}' completed {} times",
                    name,
                    count));
        }
        names.insert(name);
    }
    if (names != expected) {
        fail(context, "observed event set differs from expectation");
    }
}

void verify_probe_observation(
        const probe_observation& observation,
        std::string_view expected_profile,
        const fs::path& expected_target_path,
        std::string_view expected_site_id,
        std::string_view expected_fault_key,
        std::string_view expected_relation,
        const json& expected_probe_contract,
        const std::set<std::string, std::less<>>&
            expected_destructors,
        const std::set<std::string, std::less<>>&
            expected_cleanup,
        const std::set<std::string, std::less<>>&
            expected_postgres_cleanup) {
    require_probe_contract(
        expected_probe_contract,
        expected_relation,
        "expected probe contract");
    const probe_contract_values contract =
        contract_for_relation(
            expected_relation, "expected probe relation");
    if (observation.profile != expected_profile
        || fs::canonical(observation.mapped_target_path)
            != fs::canonical(expected_target_path)) {
        fail(
            "probe observation",
            "profile or mapped target identity differs");
    }
    const bool backend_scoped_vacuum =
        expected_fault_key == kBackendScopedVacuumFaultKey;
    const std::uint64_t expected_start_count =
        backend_scoped_vacuum ? 0U : 1U;
    const std::uint64_t expected_abort_count =
        backend_scoped_vacuum ? 0U : 1U;
    if (observation.start_count != expected_start_count
        || observation.end_count != 0
        || observation.abort_count != expected_abort_count
        || observation.inside_target) {
        fail(
            "probe observation",
            "FMGR/backend lifecycle differs");
    }
    if (observation.site_id != expected_site_id
        || observation.fault_key != expected_fault_key
        || observation.relation != expected_relation) {
        fail(
            "probe observation",
            "site, fault, or relation echo differs");
    }
    const std::uint64_t expected_visit_sequence =
        contract.ordering == "trigger_before_visit" ? 2U : 1U;
    const std::uint64_t expected_trigger_sequence =
        contract.ordering == "visit_only"
        ? 0U
        : (contract.ordering == "trigger_before_visit"
               ? 1U
               : 2U);
    if (observation.site_visit_count
            != contract.expected_visit_count
        || observation.fault_trigger_count
            != contract.expected_trigger_count
        || observation.site_visit_sequence
            != expected_visit_sequence
        || observation.fault_trigger_sequence
            != expected_trigger_sequence) {
        fail(
            "probe observation",
            "causal visit/trigger contract differs");
    }
    if (observation.invalid_count != 0
        || observation.overflow_count != 0) {
        fail(
            "probe observation",
            "integrity counters differ");
    }
    verify_event_set(
        observation.destructor_events,
        expected_destructors,
        "probe destructor events");
    verify_event_set(
        observation.cleanup_events,
        expected_cleanup,
        "probe cleanup-completion events");
    verify_event_set(
        observation.postgres_cleanup_events,
        expected_postgres_cleanup,
        "probe PostgreSQL-cleanup events");
}

[[nodiscard]] json result_rows_json(
        const std::vector<
            std::vector<std::optional<std::string>>>& value) {
    json result = json::array();
    for (const auto& row : value) {
        json result_row = json::array();
        for (const auto& cell : row) {
            if (cell.has_value()) {
                result_row.push_back(*cell);
            } else {
                result_row.push_back(nullptr);
            }
        }
        result.push_back(std::move(result_row));
    }
    return result;
}

void execute_sql_list(
        const pg_connection& connection,
        const std::vector<std::string>& statements,
        std::string_view context,
        bool allow_open_transaction = false) {
    for (std::size_t index = 0;
         index < statements.size();
         ++index) {
        require_success(
            connection.execute(statements[index]),
            fmt::format("{}[{}]", context, index));
        if (allow_open_transaction) {
            const PGTransactionStatusType status =
                connection.transaction_status();
            if (status != PQTRANS_IDLE
                && status != PQTRANS_INTRANS) {
                fail(
                    fmt::format("{}[{}]", context, index),
                    "PostgreSQL setup transaction is not usable");
            }
        } else {
            require_idle(
                connection,
                fmt::format("{}[{}]", context, index));
        }
    }
}

[[nodiscard]] std::optional<std::string_view>
terminal_guard_kind(std::string_view fault_key) noexcept {
    if (fault_key
        == "scoped_spi_session.emergency_finish_error") {
        return "finish_error";
    }
    if (fault_key
        == "scoped_spi_session.emergency_finish_non_ok") {
        return "finish_non_ok";
    }
    return std::nullopt;
}

struct server_log_checkpoint final {
    std::uint64_t device{};
    std::uint64_t inode{};
    std::uint64_t offset{};
};

[[nodiscard]] server_log_checkpoint capture_server_log(
        const fs::path& path) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_uid != ::getuid()
        || metadata.st_nlink != 1
        || metadata.st_size < 0) {
        fail(
            "server log",
            "cannot capture exact regular private-cluster log");
    }
    return {
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
        .offset = static_cast<std::uint64_t>(metadata.st_size),
    };
}

[[nodiscard]] std::string read_server_log_suffix(
        const fs::path& path,
        const server_log_checkpoint& checkpoint) {
    constexpr std::uint64_t maximum_suffix = 1024 * 1024;
    const int descriptor = ::open(
        path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        fail("server log", "cannot open sealed server log");
    }
    struct descriptor_owner final {
        int value;
        ~descriptor_owner() {
            if (value >= 0) ::close(value);
        }
    } owner{descriptor};
    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_nlink != 1
        || static_cast<std::uint64_t>(metadata.st_dev)
            != checkpoint.device
        || static_cast<std::uint64_t>(metadata.st_ino)
            != checkpoint.inode
        || metadata.st_size < 0
        || static_cast<std::uint64_t>(metadata.st_size)
            < checkpoint.offset
        || static_cast<std::uint64_t>(metadata.st_size)
                - checkpoint.offset
            > maximum_suffix) {
        fail(
            "server log",
            "server log was replaced, truncated, or exceeded its bound");
    }
    const std::size_t size = static_cast<std::size_t>(
        static_cast<std::uint64_t>(metadata.st_size)
        - checkpoint.offset);
    std::string result(size, '\0');
    std::size_t read_offset = 0;
    while (read_offset < result.size()) {
        const ssize_t count = ::pread(
            descriptor,
            result.data() + read_offset,
            result.size() - read_offset,
            static_cast<off_t>(
                checkpoint.offset + read_offset));
        if (count > 0) {
            read_offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        fail("server log", "cannot read exact appended interval");
    }
    return result;
}

[[nodiscard]] json wait_for_terminal_server_log(
        const fs::path& path,
        const server_log_checkpoint& checkpoint,
        std::uint64_t backend_pid) {
    const std::string expected_exit = fmt::format(
        "server process (PID {}) exited with exit code 1",
        backend_pid);
    std::string interval;
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        interval = read_server_log_suffix(path, checkpoint);
        if (interval.contains(expected_exit)) break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds{50});
    }
    std::string lower = interval;
    std::ranges::transform(
        lower,
        lower.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    const bool fatal = interval.contains("FATAL:");
    const bool panic = interval.contains("PANIC:");
    const bool signal =
        lower.contains("terminated by signal")
        || lower.contains("was terminated by signal")
        || lower.contains("segmentation fault");
    const bool core_dump =
        lower.contains("core dump") || lower.contains("dumped core");
    if (!interval.contains(expected_exit)
        || fatal || panic || signal || core_dump
        || interval.find('\0') != std::string::npos) {
        fail(
            "terminal server log",
            "intended exit record is absent or crash substitution appears");
    }
    return json{
        {"path", path.string()},
        {"device", checkpoint.device},
        {"inode", checkpoint.inode},
        {"offset", checkpoint.offset},
        {"interval_size", interval.size()},
        {"interval_sha256", sha256_bytes(interval)},
        {"interval_text", interval},
        {"exit_record", expected_exit},
        {"fatal_present", fatal},
        {"panic_present", panic},
        {"signal_present", signal},
        {"core_dump_present", core_dump}};
}

void require_terminal_marker_absent(const fs::path& path) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) == 0 || errno != ENOENT) {
        fail(
            "terminal marker",
            "token-keyed marker existed before probe arm");
    }
}

[[nodiscard]] json read_terminal_marker(
        const fs::path& path,
        std::string_view token,
        std::string_view site_id,
        std::string_view fault_key,
        std::string_view guard_kind,
        std::uint64_t backend_pid) {
    constexpr std::size_t maximum_marker_size = 2048;
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        struct stat pending {};
        if (::lstat(path.c_str(), &pending) == 0) break;
        if (errno != ENOENT) {
            fail("terminal marker", "cannot inspect marker path");
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds{20});
    }
    const int descriptor = ::open(
        path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        fail("terminal marker", "durable marker is absent");
    }
    struct descriptor_owner final {
        int value;
        ~descriptor_owner() {
            if (value >= 0) ::close(value);
        }
    } owner{descriptor};
    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_uid != ::getuid()
        || (metadata.st_mode & 07777) != 0600
        || metadata.st_nlink != 1
        || metadata.st_size <= 0
        || static_cast<std::uint64_t>(metadata.st_size)
            > maximum_marker_size) {
        fail("terminal marker", "marker metadata differs");
    }
    std::string raw(
        static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const ssize_t count = ::read(
            descriptor,
            raw.data() + offset,
            raw.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        fail("terminal marker", "marker read is incomplete");
    }
    const std::string expected = fmt::format(
        "{{\"schema\":\"bioseq_safety_terminal_marker_v1\","
        "\"token\":\"{}\",\"site_id\":\"{}\","
        "\"fault_key\":\"{}\",\"guard_kind\":\"{}\","
        "\"backend_pid\":{}}}\n",
        token,
        site_id,
        fault_key,
        guard_kind,
        backend_pid);
    if (raw != expected) {
        fail("terminal marker", "marker payload differs");
    }
    return json{
        {"path", path.string()},
        {"sha256", sha256_bytes(raw)},
        {"payload", raw},
        {"schema", "bioseq_safety_terminal_marker_v1"},
        {"token", token},
        {"site_id", site_id},
        {"fault_key", fault_key},
        {"guard_kind", guard_kind},
        {"backend_pid", backend_pid},
        {"mode", 0600},
        {"uid", static_cast<std::uint64_t>(metadata.st_uid)},
        {"link_count",
         static_cast<std::uint64_t>(metadata.st_nlink)}};
}

[[nodiscard]] int run_case(
        const command_line& command) {
    require_exact_options(command, {"--request", "--output"});
    const fs::path request_path =
        fs::absolute(require_option(command, "--request"))
            .lexically_normal();
    const fs::path output =
        fs::absolute(require_option(command, "--output"))
            .lexically_normal();
    const std::string request_raw = read_file(request_path);
    const std::string request_sha256 =
        sha256_bytes(request_raw);
    const json request =
        parse_json(request_raw, "request");
    if (require_field(
            request, "schema_version", "request") != 2
        || require_string_field(
               request, "profile", "request") != kProfile
        || require_string_field(
               request,
               "case_plan_sha256",
               "request")
            != kCasePlanSha256) {
        fail(
            "request",
            "schema, profile, or compiled case-plan binding differs");
    }
    const std::string case_id_value =
        require_string_field(request, "case_id", "request");
    const case_plan plan = load_case_plan();
    const auto plan_case = plan.cases.find(case_id_value);
    if (plan_case == plan.cases.end()) {
        fail("request", "case is absent from compiled plan");
    }
    const json& planned_catalog =
        plan_case->second.catalog;
    const json& execution = plan_case->second.execution;
    require_equal_field(
        request, planned_catalog, "sql_identity", "request");
    require_equal_field(
        request, planned_catalog, "site_id", "request");
    require_equal_field(
        request,
        planned_catalog,
        "ledger_site_sha256",
        "request");
    require_equal_field(
        request, planned_catalog, "invocation_key", "request");
    require_equal_field(
        request, planned_catalog, "fault_key", "request");
    require_equal_field(
        request, planned_catalog, "relation", "request");
    require_equal_field(
        request, planned_catalog, "probe_contract", "request");

    const std::string executable_sha256 =
        self_sha256(command);
    if (require_string_field(
            request,
            "test_executable_sha256",
            "request")
        != executable_sha256) {
        fail(
            "request",
            "test executable hash differs from running bytes");
    }
    verify_file_binding(
        request,
        "case_plan_path",
        "case_plan_sha256",
        "case plan");
    if (fs::canonical(fs::path{
            require_string_field(
                request, "case_plan_path", "request")})
        != fs::canonical(fs::path{kCasePlanPath})) {
        fail(
            "case plan",
            "request does not name the executable's compiled plan");
    }
    verify_file_binding(
        request,
        "producer_executable_path",
        "producer_executable_sha256",
        "runtime producer");
    verify_file_binding(
        request,
        "ledger_path",
        "ledger_sha256",
        "safety ledger");
    verify_file_binding(
        request,
        "sql_manifest_path",
        "sql_manifest_sha256",
        "SQL manifest");
    verify_file_binding(
        request,
        "catalog_path",
        "catalog_sha256",
        "case catalog");
    verify_file_binding(
        request,
        "cluster_config_path",
        "cluster_config_sha256",
        "cluster config");
    const fs::path request_manifest_path{
        require_string_field(
            request, "sql_manifest_path", "request")};
    const sql_manifest_contract manifest =
        load_sql_manifest();
    if (fs::canonical(request_manifest_path)
            != fs::canonical(fs::path{kSqlManifest})
        || require_string_field(
               request,
               "sql_manifest_sha256",
               "request")
            != manifest.sha256) {
        fail(
            "SQL manifest",
            "request is not bound to the executable's compiled manifest");
    }

    const fs::path active_dso{
        require_string_field(
            request, "active_dso_path", "request")};
    const std::string active_dso_sha256 =
        require_string_field(
            request, "active_dso_sha256", "request");
    if (!active_dso.is_absolute()
        || !fs::is_regular_file(active_dso)
        || fs::is_symlink(active_dso)
        || !lowercase_sha256(active_dso_sha256)
        || sha256_file(active_dso) != active_dso_sha256) {
        fail(
            "active DSO",
            "path or independently observed bytes differ");
    }

    const fs::path config_path{
        require_string_field(
            request, "cluster_config_path", "request")};
    const std::string config_raw = read_file(config_path);
    const json config =
        parse_json(config_raw, "cluster-config");
    validate_cluster_config(config, kProfile);
    if (require_absolute_path(
            config, "installed_dso_path", "cluster-config")
            .lexically_normal()
        != active_dso.lexically_normal()) {
        fail(
            "active DSO",
            "cluster configuration names a different installed path");
    }
    const std::string config_sha256 =
        sha256_bytes(config_raw);
    if (config_sha256
        != require_string_field(
            request, "cluster_config_sha256", "request")) {
        fail("cluster config", "bytes changed after request publication");
    }
    const activation_receipt activation =
        load_activation_receipt(
            request,
            active_dso,
            active_dso_sha256,
            config_sha256,
            executable_sha256,
            config,
            manifest);

    const fs::path catalog_path{
        require_string_field(
            request, "catalog_path", "request")};
    const json catalog = load_json(
        catalog_path, "catalog");
    if (require_string_field(
            catalog,
            "test_executable_sha256",
            "catalog")
        != executable_sha256) {
        fail("catalog", "test executable binding differs");
    }
    const json& published_entry =
        catalog_entry(catalog, case_id_value);
    if (published_entry != planned_catalog) {
        fail(
            "catalog",
            "published case differs from compiled case plan");
    }
    const std::string catalog_entry_sha256 =
        sha256_bytes(canonical_json(planned_catalog));
    if (catalog_entry_sha256
        != require_string_field(
            request, "catalog_entry_sha256", "request")) {
        fail("catalog", "catalog-entry SHA-256 differs");
    }
    const json& expected = require_field(
        planned_catalog, "expected", "catalog-entry");
    const std::string expected_backend_outcome =
        require_string_field(
            expected,
            "backend_outcome",
            "catalog-entry.expected");
    const bool backend_exit =
        expected_backend_outcome == "backend_exit";

    const fs::path ledger_path{
        require_string_field(
            request, "ledger_path", "request")};
    const json ledger =
        load_json(ledger_path, "ledger");
    const std::string site_id =
        require_string_field(request, "site_id", "request");
    const std::string ledger_site_sha256 =
        require_string_field(
            request, "ledger_site_sha256", "request");
    static_cast<void>(
        resolve_ledger_site(
            ledger,
            planned_catalog,
            ledger_site_sha256));
    if (require_string_field(
            ledger, "sql_manifest_sha256", "ledger")
        != require_string_field(
            request, "sql_manifest_sha256", "request")) {
        fail(
            "ledger", "SQL manifest binding differs from request");
    }

    const connection_parameters parameters =
        parse_connection(config);
    pg_connection connection{parameters};
    const json cluster = query_cluster_identity(connection);
    validate_private_cluster_identity(cluster, config);
    static const std::array<std::string_view, 4>
        cluster_fields{
            "cluster_system_identifier",
            "cluster_data_directory",
            "postmaster_start_time",
            "server_version_num"};
    for (const std::string_view field : cluster_fields) {
        if (require_field(
                cluster, field, "cluster-identity")
            != require_field(
                activation.value,
                field,
                "activation-receipt")) {
            fail(
                "cluster-identity",
                fmt::format("{} differs from activation", field));
        }
    }
    if (sha256_file(active_dso) != active_dso_sha256) {
        fail(
            "active DSO",
            "bytes changed before case execution");
    }
    const std::uint64_t extension_oid =
        verify_installed_catalog(connection, manifest);
    create_guc_contract_binding(
        connection, activation.control_dso_path);
    const installed_guc_profile guc_profile =
        verify_installed_guc_profile(connection, manifest.gucs);
    if (extension_oid
        != require_field(
               activation.value,
               "extension_oid",
               "activation-receipt")
               .get<std::uint64_t>()) {
        fail(
            "installed C routine catalog",
            "extension OID differs from activation");
    }
    if (guc_profile.counts.total
            != parse_unsigned(
                require_field(
                    activation.value,
                    "bioseq_guc_count",
                    "activation-receipt")
                    .dump(),
                "activation-receipt.bioseq_guc_count")
        || guc_profile.counts.test_only
            != parse_unsigned(
                require_field(
                    activation.value,
                    "bioseq_test_only_guc_count",
                    "activation-receipt")
                    .dump(),
                "activation-receipt.bioseq_test_only_guc_count")
        || guc_profile.properties
            != require_field(
                activation.value,
                "bioseq_guc_properties",
                "activation-receipt")
        || guc_profile.properties_sha256
            != require_string_field(
                activation.value,
                "bioseq_guc_properties_sha256",
                "activation-receipt")) {
        fail(
            "installed GUC profile",
            "observed counts differ from activation");
    }
    if (sha256_file(activation.control_dso_path)
            != activation.control_dso_sha256
        || activation.control_dso_sha256
            != require_string_field(
                activation.value,
                "control_dso_sha256",
                "activation-receipt")) {
        fail(
            "safety runtime control DSO",
            "helper bytes changed before case execution");
    }
    create_probe_bindings(
        connection, activation.control_dso_path);
    require_idle(connection, "probe binding setup");

    const std::vector<std::string> setup_sql =
        sql_array(
            execution, "setup_sql", "case.execution", false);
    const std::vector<std::string> post_error_sql =
        sql_array(
            execution,
            "post_error_sql",
            "case.execution",
            false);
    const std::vector<std::string> cleanup_sql =
        sql_array(
            execution, "cleanup_sql", "case.execution", false);
    execute_sql_list(
        connection,
        setup_sql,
        "case setup",
        backend_exit);
    if (
        backend_exit
        && connection.transaction_status() != PQTRANS_INTRANS) {
        fail(
            "case setup",
            "backend-exit setup must leave one rollback-owned transaction");
    }
    if (verify_installed_catalog(connection, manifest)
        != extension_oid) {
        fail(
            "case setup",
            "setup changed the exact extension catalog binding");
    }
    if (execution.contains("pre_fault_control_sql")) {
        const std::vector<std::string> control_sql =
            sql_array(
                execution,
                "pre_fault_control_sql",
                "case.execution",
                true);
        execute_sql_list(
            connection,
            control_sql,
            "pre-fault success control");
        const pg_result control_recovery_result =
            connection.execute(
                require_string_field(
                    execution,
                    "pre_fault_control_recovery_sql",
                    "case.execution"));
        const json observed_control_rows =
            result_rows_json(
                rows(
                    control_recovery_result,
                    "pre-fault success-control recovery"));
        require_idle(
            connection,
            "pre-fault success-control recovery");
        if (observed_control_rows
            != require_field(
                execution,
                "pre_fault_control_recovery_expected_rows",
                "case.execution")) {
            fail(
                "pre-fault success-control recovery",
                "exact result rows differ from the compiled expectation");
        }
        if (verify_installed_catalog(connection, manifest)
            != extension_oid) {
            fail(
                "pre-fault success control",
                "control changed the exact extension catalog binding");
        }
    }

    const std::uint64_t target_oid =
        resolve_target_oid(
            connection,
            require_string_field(
                execution,
                "target_regprocedure",
                "case.execution"));
    const std::string token = random_token();
    const std::string fault_key =
        require_string_field(
            planned_catalog, "fault_key", "catalog-entry");
    const std::string relation =
        require_string_field(
            planned_catalog, "relation", "catalog-entry");
    const json& probe_contract =
        require_field(
            planned_catalog,
            "probe_contract",
            "catalog-entry");
    const fs::path marker_directory = require_absolute_path(
        config, "terminal_marker_directory", "cluster-config");
    const fs::path server_log_path = require_absolute_path(
        config, "server_log_path", "cluster-config");
    const fs::path marker_path =
        marker_directory
        / fmt::format("terminal-{}.marker", token);
    require_terminal_marker_absent(marker_path);
    const std::optional<std::string_view> expected_guard_kind =
        terminal_guard_kind(fault_key);
    if (backend_exit != expected_guard_kind.has_value()) {
        fail(
            "terminal causality",
            "backend-exit case and terminal guard fault differ");
    }
    const std::optional<server_log_checkpoint> log_checkpoint =
        backend_exit
        ? std::optional<server_log_checkpoint>{
              capture_server_log(server_log_path)}
        : std::nullopt;
    const std::array<std::string, 8> arm_parameters{
        std::to_string(target_oid),
        token,
        site_id,
        fault_key,
        relation,
        std::string{kProfile},
        fs::canonical(active_dso).string(),
        fs::canonical(marker_directory).string()};
    require_success(
        connection.execute_parameters(
            "SELECT pg_temp.bioseq_safety_runtime_probe_arm("
            "$1::oid, $2::text, $3::text, $4::text, "
            "$5::text, $6::text, $7::text, $8::text);",
            arm_parameters),
        "arm safety runtime probe");
    if (backend_exit) {
        if (connection.transaction_status() != PQTRANS_INTRANS) {
            fail(
                "arm safety runtime probe",
                "terminal probe arm escaped its rollback transaction");
        }
    } else {
        require_idle(connection, "arm safety runtime probe");
    }

    const std::string invocation_sql =
        require_string_field(
            execution, "invocation_sql", "case.execution");
    const std::uint64_t initial_backend_pid =
        static_cast<std::uint64_t>(connection.backend_pid());
    if (initial_backend_pid == 0) {
        fail("case invocation", "initial backend PID is invalid");
    }
    const pg_result invocation =
        connection.execute(invocation_sql);
    std::optional<std::string> observed_sqlstate;
    std::optional<std::string> observed_message;
    std::string observed_backend_outcome;
    std::string post_invocation_connection_status;
    std::string invocation_result_status;
    std::optional<json> terminal_marker_evidence;
    std::optional<json> terminal_server_log_evidence;
    if (backend_exit) {
        if (connection.connection_status() != CONNECTION_BAD) {
            fail(
                "case invocation",
                "terminal cleanup guard did not close the backend connection");
        }
        observed_backend_outcome = "backend_exit";
        post_invocation_connection_status = "bad";
        invocation_result_status = "connection_lost";
        terminal_marker_evidence.emplace(
            read_terminal_marker(
                marker_path,
                token,
                site_id,
                fault_key,
                *expected_guard_kind,
                initial_backend_pid));
        terminal_server_log_evidence.emplace(
            wait_for_terminal_server_log(
                server_log_path,
                *log_checkpoint,
                initial_backend_pid));
    } else {
        if (invocation.get() == nullptr
            || PQresultStatus(invocation.get())
                != PGRES_FATAL_ERROR
            || connection.connection_status()
                != CONNECTION_OK) {
            fail(
                "case invocation",
                "expected one recoverable PostgreSQL ERROR");
        }
        const char* severity = PQresultErrorField(
            invocation.get(), PG_DIAG_SEVERITY_NONLOCALIZED);
        const char* sqlstate = PQresultErrorField(
            invocation.get(), PG_DIAG_SQLSTATE);
        const char* message = PQresultErrorField(
            invocation.get(), PG_DIAG_MESSAGE_PRIMARY);
        if (severity == nullptr
            || std::string_view{severity} != "ERROR"
            || sqlstate == nullptr
            || message == nullptr) {
            fail(
                "case invocation",
                "PostgreSQL ERROR fields are incomplete");
        }
        observed_sqlstate.emplace(sqlstate);
        observed_message.emplace(message);
        observed_backend_outcome = "recoverable_error";
        post_invocation_connection_status = "ok";
        invocation_result_status = "postgres_error";
        require_idle(connection, "case invocation recovery");
        if (*observed_sqlstate
                != require_string_field(
                    expected,
                    "sqlstate",
                    "catalog-entry.expected")
            || *observed_message
                != require_string_field(
                    expected,
                    "message",
                    "catalog-entry.expected")) {
            fail(
                "case invocation",
                "SQLSTATE or primary message differs from compiled expectation");
        }
        require_terminal_marker_absent(marker_path);
    }

    std::unique_ptr<pg_connection> replacement_connection;
    const pg_connection* active_connection = &connection;
    json recovery_cluster = cluster;
    std::uint64_t recovery_backend_pid = initial_backend_pid;
    if (backend_exit) {
        replacement_connection =
            std::make_unique<pg_connection>(parameters);
        active_connection = replacement_connection.get();
        recovery_backend_pid = static_cast<std::uint64_t>(
            active_connection->backend_pid());
        if (recovery_backend_pid == 0
            || recovery_backend_pid == initial_backend_pid
            || active_connection->connection_status()
                != CONNECTION_OK) {
            fail(
                "backend-exit recovery",
                "replacement backend identity is not distinct and healthy");
        }
        recovery_cluster =
            query_cluster_identity(*active_connection);
        validate_private_cluster_identity(
            recovery_cluster, config);
        for (const std::string_view field : cluster_fields) {
            if (require_field(
                    recovery_cluster,
                    field,
                    "recovery-cluster-identity")
                != require_field(
                    activation.value,
                    field,
                    "activation-receipt")) {
                fail(
                    "backend-exit recovery",
                    fmt::format(
                        "{} differs from activation",
                        field));
            }
        }
        if (verify_installed_catalog(
                *active_connection, manifest)
            != extension_oid) {
            fail(
                "backend-exit recovery",
                "replacement backend has a different extension catalog");
        }
        create_guc_contract_binding(
            *active_connection,
            activation.control_dso_path);
        const installed_guc_profile recovery_guc_profile =
            verify_installed_guc_profile(
                *active_connection, manifest.gucs);
        if (recovery_guc_profile.counts.total
                != guc_profile.counts.total
            || recovery_guc_profile.counts.test_only
                != guc_profile.counts.test_only
            || recovery_guc_profile.properties
                != guc_profile.properties
            || recovery_guc_profile.properties_sha256
                != guc_profile.properties_sha256) {
            fail(
                "backend-exit recovery",
                "replacement backend has a different GUC profile");
        }
    }

    execute_sql_list(
        *active_connection, post_error_sql, "post-error SQL");
    const std::array<std::string, 1> token_parameter{token};
    std::optional<probe_observation> observation;
    const std::set<std::string, std::less<>>
        expected_sentinels = string_set(
            require_field(
                expected,
                "destructor_sentinels",
                "catalog-entry.expected"),
            "catalog-entry.expected.destructor_sentinels");
    const std::set<std::string, std::less<>>
        expected_cleanup = string_set(
            require_field(
                expected,
                "cleanup_events",
                "catalog-entry.expected"),
            "catalog-entry.expected.cleanup_events");
    const std::set<std::string, std::less<>>
        expected_postgres_cleanup = string_set(
            require_field(
                expected,
                "postgres_cleanup_events",
                "catalog-entry.expected"),
            "catalog-entry.expected.postgres_cleanup_events");
    if (backend_exit) {
        if (!expected_sentinels.empty()
            || !expected_cleanup.empty()
            || !expected_postgres_cleanup.empty()) {
            fail(
                "backend-exit evidence",
                "terminal cases cannot claim backend-local snapshot events");
        }
    } else {
        const pg_result snapshot_result =
            connection.execute_parameters(
                "SELECT pg_temp.bioseq_safety_runtime_probe_snapshot("
                "$1::text);",
                token_parameter);
        const auto snapshot_row =
            one_row(snapshot_result, "probe snapshot");
        observation.emplace(
            parse_probe_observation(
                nonnull_cell(
                    snapshot_row, 0, "probe snapshot")));
        verify_probe_observation(
            *observation,
            kProfile,
            active_dso,
            site_id,
            fault_key,
            relation,
            probe_contract,
            expected_sentinels,
            expected_cleanup,
            expected_postgres_cleanup);
        require_idle(connection, "probe snapshot");
        require_success(
            connection.execute_parameters(
                "SELECT pg_temp.bioseq_safety_runtime_probe_disarm("
                "$1::text);",
                token_parameter),
            "disarm safety runtime probe");
        require_idle(connection, "disarm safety runtime probe");
    }

    const std::string recovery_sql =
        require_string_field(
            execution, "recovery_sql", "case.execution");
    const pg_result recovery_result =
        active_connection->execute(recovery_sql);
    const auto recovery_rows =
        rows(recovery_result, "recovery probe");
    require_idle(*active_connection, "recovery probe");
    const json observed_recovery_rows =
        result_rows_json(recovery_rows);
    const json& expected_recovery_rows =
        require_field(
            execution,
            "recovery_expected_rows",
            "case.execution");
    if (observed_recovery_rows
        != expected_recovery_rows) {
        fail(
            "recovery probe",
            "exact result rows differ from the compiled expectation");
    }
    execute_sql_list(
        *active_connection, cleanup_sql, "case cleanup");
    if (sha256_file(active_dso) != active_dso_sha256) {
        fail(
            "active DSO",
            "bytes changed during case execution");
    }
    if (sha256_file(activation.control_dso_path)
        != activation.control_dso_sha256) {
        fail(
            "safety runtime control DSO",
            "helper bytes changed during case execution");
    }

    json destructor_events = json::array();
    json cleanup_events = json::array();
    json postgres_cleanup_events = json::array();
    if (observation.has_value()) {
        for (const auto& [name, count] :
             observation->destructor_events) {
            destructor_events.push_back(
                json{{"name", name}, {"count", count}});
        }
        for (const auto& [name, count] :
             observation->cleanup_events) {
            cleanup_events.push_back(
                json{{"name", name}, {"count", count}});
        }
        for (const auto& [name, count] :
             observation->postgres_cleanup_events) {
            postgres_cleanup_events.push_back(
                json{{"name", name}, {"count", count}});
        }
    }
    const bool connection_replaced = backend_exit;
    const std::string observation_mode =
        backend_exit ? "backend_exit" : "backend_snapshot";
    json terminal_causality = nullptr;
    bool terminal_guard_observed = false;
    if (backend_exit) {
        if (!terminal_marker_evidence.has_value()
            || !terminal_server_log_evidence.has_value()
            || recovery_cluster.at("postmaster_start_time")
                != cluster.at("postmaster_start_time")
            || recovery_cluster.at("cluster_system_identifier")
                != cluster.at("cluster_system_identifier")) {
            fail(
                "terminal causality",
                "durable marker, server log, or postmaster identity "
                "proof is incomplete");
        }
        terminal_causality = json{
            {"marker", *terminal_marker_evidence},
            {"server_log", *terminal_server_log_evidence},
            {"marker_directory", marker_directory.string()},
            {"server_log_path", server_log_path.string()},
            {"postmaster_identity_unchanged", true},
            {"fatal_panic_signal_core_rejected", true}};
        terminal_guard_observed = true;
    }
    const json result{
        {"schema_version", 2},
        {"probe_abi_version", 2},
        {"profile", kProfile},
        {"case_id", case_id_value},
        {"sql_identity",
         require_field(request, "sql_identity", "request")},
        {"site_id", site_id},
        {"ledger_site_sha256", ledger_site_sha256},
        {"invocation_key",
         require_field(request, "invocation_key", "request")},
        {"fault_key",
         require_field(request, "fault_key", "request")},
        {"relation", relation},
        {"probe_contract", probe_contract},
        {"case_plan_sha256", kCasePlanSha256},
        {"ledger_sha256",
         require_field(request, "ledger_sha256", "request")},
        {"sql_manifest_sha256",
         require_field(
             request, "sql_manifest_sha256", "request")},
        {"active_dso_sha256", active_dso_sha256},
        {"test_executable_sha256", executable_sha256},
        {"producer_executable_sha256",
         require_field(
             request,
             "producer_executable_sha256",
             "request")},
        {"activation_receipt_sha256", activation.sha256},
        {"bioseq_guc_properties",
         guc_profile.properties},
        {"bioseq_guc_properties_sha256",
         guc_profile.properties_sha256},
        {"cluster_config_sha256", config_sha256},
        {"request_sha256", request_sha256},
        {"catalog_sha256",
         require_field(request, "catalog_sha256", "request")},
        {"catalog_entry_sha256", catalog_entry_sha256},
        {"execution_backend", "postgresql"},
        {"exercised_terminal_severity", false},
        {"backend_outcome",
         json{
             {"expected", expected_backend_outcome},
             {"observed", observed_backend_outcome},
             {"initial_backend_pid", initial_backend_pid},
             {"recovery_backend_pid", recovery_backend_pid},
             {"initial_connection_status", "ok"},
             {"post_invocation_connection_status",
              post_invocation_connection_status},
             {"invocation_result_status",
              invocation_result_status},
             {"terminal_guard_observed",
              terminal_guard_observed}}},
        {"terminal_causality", terminal_causality},
        {"destructor_events", std::move(destructor_events)},
        {"cleanup_events", std::move(cleanup_events)},
        {"postgres_cleanup_events",
         std::move(postgres_cleanup_events)},
        {"postgres_error",
         backend_exit
             ? json{
                   {"severity", nullptr},
                   {"sqlstate", nullptr},
                   {"message", nullptr}}
             : json{
                   {"severity", "ERROR"},
                   {"sqlstate", *observed_sqlstate},
                   {"message", *observed_message}}},
        {"fault_evidence",
         json{
             {"observation_mode", observation_mode},
             {"probe_abi_version", 2},
             {"site_id",
              observation.has_value()
                  ? observation->site_id
                  : site_id},
             {"fault_key",
              observation.has_value()
                  ? observation->fault_key
                  : fault_key},
             {"relation",
              observation.has_value()
                  ? observation->relation
                  : relation},
             {"probe_contract", probe_contract},
             {"invocation_count",
              observation.has_value()
                  ? observation->start_count
                  : 0},
             {"fmgr_start_count",
              observation.has_value()
                  ? observation->start_count
                  : 0},
             {"fmgr_end_count",
              observation.has_value()
                  ? observation->end_count
                  : 0},
             {"fmgr_abort_count",
              observation.has_value()
                  ? observation->abort_count
                  : 0},
             {"site_visit_count",
              observation.has_value()
                  ? observation->site_visit_count
                  : 0},
             {"fault_trigger_count",
              observation.has_value()
                  ? observation->fault_trigger_count
                  : 0},
             {"site_visit_sequence",
              observation.has_value()
                  ? observation->site_visit_sequence
                  : 0},
             {"fault_trigger_sequence",
              observation.has_value()
                  ? observation->fault_trigger_sequence
                  : 0},
             {"invalid_count",
              observation.has_value()
                  ? observation->invalid_count
                  : 0},
             {"overflow_count",
              observation.has_value()
                  ? observation->overflow_count
                  : 0},
             {"inside_target",
              observation.has_value()
                  ? observation->inside_target
                  : false}}},
        {"recovery_probe",
         json{
             {"postcondition",
              require_field(
                  expected,
                  "recovery_postcondition",
                  "catalog-entry.expected")},
             {"passed", true},
             {"transaction_status", "idle"},
             {"nonempty_result", !recovery_rows.empty()},
             {"expected_rows", expected_recovery_rows},
             {"observed_rows", observed_recovery_rows},
             {"result_row_count", recovery_rows.size()}}},
        {"cluster_evidence",
         json{
             {"cluster_system_identifier",
              recovery_cluster.at(
                  "cluster_system_identifier")},
             {"cluster_data_directory",
              recovery_cluster.at(
                  "cluster_data_directory")},
             {"postmaster_start_time",
              recovery_cluster.at("postmaster_start_time")},
             {"activation_nonce",
              activation.value.at("activation_nonce")},
             {"server_version_num",
              recovery_cluster.at("server_version_num")},
             {"backend_pid", initial_backend_pid},
             {"initial_backend_pid", initial_backend_pid},
             {"recovery_backend_pid", recovery_backend_pid},
             {"connection_replaced", connection_replaced},
             {"active_dso_path",
              fs::canonical(active_dso).string()},
             {"mapped_target_path",
              observation.has_value()
                  ? fs::canonical(
                        observation->mapped_target_path).string()
                  : fs::canonical(active_dso).string()},
             {"control_dso_path",
              activation.control_dso_path.string()},
             {"control_dso_sha256",
              activation.control_dso_sha256}}}};
    write_atomic(output, canonical_json(result));
    return 0;
}

template <typename Operation>
void expect_failure(
        Operation&& operation,
        std::string_view diagnostic) {
    try {
        std::forward<Operation>(operation)();
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()}.find(diagnostic)
            == std::string_view::npos) {
            fail(
                "self-test",
                fmt::format(
                    "expected diagnostic '{}', observed '{}'",
                    diagnostic,
                    error.what()));
        }
        return;
    }
    fail(
        "self-test",
        fmt::format(
            "operation unexpectedly succeeded; expected '{}'",
            diagnostic));
}

[[nodiscard]] int self_test(
        const command_line& command) {
    require_exact_options(command, {});
    const case_plan plan = load_case_plan();
    if (plan.cases.empty()) {
        fail("self-test", "compiled case plan is empty");
    }
    const sql_manifest_contract manifest =
        load_sql_manifest();
    if (manifest.symbol_by_identity.empty()
        || !fs::is_regular_file(fs::path{kControlDso})
        || fs::is_symlink(fs::path{kControlDso})
        || !fs::is_regular_file(fs::path{kControlFile})
        || fs::is_symlink(fs::path{kControlFile})) {
        fail(
            "self-test",
            "compiled SQL/control/helper artifacts are incomplete");
    }
    const guc_profile_counts expected_gucs =
        expected_guc_profile(kProfile);
    if ((kProfile == "production"
            && (expected_gucs.total != 76
                || expected_gucs.test_only != 0))
        || (kProfile == "test_indexes"
            && (expected_gucs.total != 77
                || expected_gucs.test_only != 1))) {
        fail("self-test", "compiled GUC profile mapping differs");
    }
    expect_failure(
        [] {
            static_cast<void>(
                expected_guc_profile("unsupported"));
        },
        "unsupported profile");

    const std::string site_id =
        "probe:v2:" + std::string(64, 'a');
    const std::string other_site_id =
        "probe:v2:" + std::string(64, 'b');
    constexpr std::string_view fault_key =
        "generated_boundary_tail.std_exception";
    constexpr std::string_view generic_fault_key =
        "invoke_pg_as_cpp.cleanup_restore";
    if (!valid_fault_key(generic_fault_key)
        || !valid_fault_key(kBackendScopedVacuumFaultKey)
        || !valid_fault_key(std::string(128, 'a'))
        || valid_fault_key(std::string_view{})
        || valid_fault_key(std::string(129, 'a'))
        || valid_fault_key("invalid-key")
        || valid_fault_key("Invalid")) {
        fail("self-test", "runtime fault-key grammar differs");
    }
    json generic_catalog =
        plan.cases.begin()->second.catalog;
    generic_catalog["fault_key"] =
        std::string{generic_fault_key};
    const json normalized_generic_catalog =
        normalized_catalog_entry(
            generic_catalog, "self-test generic catalog");
    if (require_string_field(
            normalized_generic_catalog,
            "fault_key",
            "self-test generic catalog")
        != generic_fault_key) {
        fail(
            "self-test",
            "catalog parser rejected a valid generic fault key");
    }
    json backend_catalog =
        plan.cases.begin()->second.catalog;
    backend_catalog["fault_key"] =
        std::string{kBackendScopedVacuumFaultKey};
    backend_catalog["expected"]["postgres_cleanup_events"] =
        json::array();
    const json normalized_backend_catalog =
        normalized_catalog_entry(
            backend_catalog,
            "self-test backend-scoped VACUUM catalog");
    if (require_string_field(
            normalized_backend_catalog,
            "fault_key",
            "self-test backend-scoped VACUUM catalog")
        != kBackendScopedVacuumFaultKey) {
        fail(
            "self-test",
            "catalog parser rejected the backend-scoped VACUUM key");
    }
    json invalid_backend_catalog = backend_catalog;
    invalid_backend_catalog["expected"]
        ["postgres_cleanup_events"] =
            json::array({"fmgr_abort_observed"});
    expect_failure(
        [&invalid_backend_catalog] {
            static_cast<void>(normalized_catalog_entry(
                invalid_backend_catalog,
                "self-test invalid backend-scoped VACUUM catalog"));
        },
        "must expect no FMGR cleanup");
    json invalid_key_catalog =
        plan.cases.begin()->second.catalog;
    invalid_key_catalog["fault_key"] = "invalid-key";
    expect_failure(
        [&invalid_key_catalog] {
            static_cast<void>(normalized_catalog_entry(
                invalid_key_catalog,
                "self-test invalid-key catalog"));
        },
        "fault_key must be 1-128 bytes");

    const auto snapshot =
        [&command, &site_id](
            std::string_view relation,
            std::uint64_t visit_count,
            std::uint64_t trigger_count,
            std::uint64_t visit_sequence,
            std::uint64_t trigger_sequence,
            std::string_view snapshot_fault_key) {
            return fmt::format(
                "v2\t{}\t1\t0\t1\t{}\t{}\t0\t0\tfalse\t"
                "{}\t{}\t{}\t{}\t{}\t{}\n"
                "event\tdestructor\t"
                "generated_boundary_callable\t1\n"
                "event\tcleanup\tmemory_context.restore\t1\n"
                "event\tpostgres_cleanup\t"
                "fmgr_abort_observed\t1\n",
                kProfile,
                visit_count,
                trigger_count,
                fs::canonical(command.executable).string(),
                site_id,
                snapshot_fault_key,
                relation,
                visit_sequence,
                trigger_sequence);
        };
    const auto verify =
        [&command, &site_id, &fault_key](
            const probe_observation& observation,
            std::string_view relation) {
            verify_probe_observation(
                observation,
                kProfile,
                command.executable,
                site_id,
                fault_key,
                relation,
                probe_contract_for_relation(
                    relation, "self-test probe contract"),
                {"generated_boundary_callable"},
                {"memory_context.restore"},
                {"fmgr_abort_observed"});
        };

    const std::string natural_snapshot =
        snapshot(
            "natural_at_site", 1, 0, 1, 0, fault_key);
    probe_observation observation =
        parse_probe_observation(natural_snapshot);
    verify(observation, "natural_at_site");

    const std::string routes_snapshot =
        snapshot(
            "injected_routes_to_site", 1, 1, 2, 1, fault_key);
    const probe_observation routes =
        parse_probe_observation(routes_snapshot);
    verify(routes, "injected_routes_to_site");

    const std::string after_snapshot =
        snapshot(
            "injected_after_site_success",
            1,
            1,
            1,
            2,
            fault_key);
    const probe_observation after =
        parse_probe_observation(after_snapshot);
    verify(after, "injected_after_site_success");

    const probe_observation generic_observation =
        parse_probe_observation(
            snapshot(
                "natural_at_site",
                1,
                0,
                1,
                0,
                generic_fault_key));
    if (generic_observation.fault_key != generic_fault_key) {
        fail(
            "self-test",
            "observation parser rejected a valid generic fault key");
    }
    const std::string backend_snapshot = fmt::format(
        "v2\t{}\t0\t0\t0\t1\t1\t0\t0\tfalse\t"
        "{}\t{}\t{}\tinjected_routes_to_site\t2\t1\n"
        "event\tdestructor\t"
        "generated_boundary_callable\t1\n"
        "event\tcleanup\tmemory_context.restore\t1\n",
        kProfile,
        fs::canonical(command.executable).string(),
        site_id,
        kBackendScopedVacuumFaultKey);
    const probe_observation backend_observation =
        parse_probe_observation(backend_snapshot);
    verify_probe_observation(
        backend_observation,
        kProfile,
        command.executable,
        site_id,
        kBackendScopedVacuumFaultKey,
        "injected_routes_to_site",
        probe_contract_for_relation(
            "injected_routes_to_site",
            "self-test backend-scoped VACUUM probe contract"),
        {"generated_boundary_callable"},
        {"memory_context.restore"},
        {});
    probe_observation fabricated_backend_fmgr =
        backend_observation;
    fabricated_backend_fmgr.start_count = 1;
    fabricated_backend_fmgr.abort_count = 1;
    expect_failure(
        [&fabricated_backend_fmgr,
         &command,
         &site_id] {
            verify_probe_observation(
                fabricated_backend_fmgr,
                kProfile,
                command.executable,
                site_id,
                kBackendScopedVacuumFaultKey,
                "injected_routes_to_site",
                probe_contract_for_relation(
                    "injected_routes_to_site",
                    "self-test backend-scoped VACUUM probe contract"),
                {"generated_boundary_callable"},
                {"memory_context.restore"},
                {});
        },
        "FMGR/backend lifecycle");
    for (const std::string& invalid_fault_key :
         std::array{
             std::string{"invalid-key"},
             std::string{"Invalid"},
             std::string(129, 'a')}) {
        expect_failure(
            [&snapshot, &invalid_fault_key] {
                static_cast<void>(
                    parse_probe_observation(
                        snapshot(
                            "natural_at_site",
                            1,
                            0,
                            1,
                            0,
                            invalid_fault_key)));
            },
            "fault key must be 1-128 bytes");
    }

    probe_observation invalid = observation;
    invalid.invalid_count = 1;
    expect_failure(
        [&invalid, &verify] {
            verify(invalid, "natural_at_site");
        },
        "integrity");
    probe_observation overflow = observation;
    overflow.overflow_count = 1;
    expect_failure(
        [&overflow, &verify] {
            verify(overflow, "natural_at_site");
        },
        "integrity");
    probe_observation missing_abort = observation;
    missing_abort.abort_count = 0;
    expect_failure(
        [&missing_abort, &verify] {
            verify(missing_abort, "natural_at_site");
        },
        "FMGR lifecycle");
    probe_observation wrong_cleanup = observation;
    wrong_cleanup.postgres_cleanup_events.clear();
    expect_failure(
        [&wrong_cleanup, &verify] {
            verify(wrong_cleanup, "natural_at_site");
        },
        "event set differs");

    probe_observation reversed = routes;
    reversed.site_visit_sequence = 1;
    reversed.fault_trigger_sequence = 2;
    expect_failure(
        [&reversed, &verify] {
            verify(reversed, "injected_routes_to_site");
        },
        "causal");
    probe_observation missing_visit = observation;
    missing_visit.site_visit_count = 0;
    missing_visit.site_visit_sequence = 0;
    expect_failure(
        [&missing_visit, &verify] {
            verify(missing_visit, "natural_at_site");
        },
        "causal");
    probe_observation missing_trigger = routes;
    missing_trigger.fault_trigger_count = 0;
    missing_trigger.fault_trigger_sequence = 0;
    expect_failure(
        [&missing_trigger, &verify] {
            verify(
                missing_trigger,
                "injected_routes_to_site");
        },
        "causal");
    probe_observation unexpected_trigger = observation;
    unexpected_trigger.fault_trigger_count = 1;
    unexpected_trigger.fault_trigger_sequence = 2;
    expect_failure(
        [&unexpected_trigger, &verify] {
            verify(unexpected_trigger, "natural_at_site");
        },
        "causal");
    probe_observation wrong_echo = observation;
    wrong_echo.site_id = other_site_id;
    expect_failure(
        [&wrong_echo, &verify] {
            verify(wrong_echo, "natural_at_site");
        },
        "echo");

    expect_failure(
        [&routes_snapshot] {
            static_cast<void>(
                parse_probe_observation(
                    routes_snapshot
                    + "event\tpostgres_cleanup\t"
                      "fmgr_abort_observed\t1\n"));
        },
        "duplicate event");
    expect_failure(
        [&command, &site_id, &fault_key] {
            static_cast<void>(parse_probe_observation(
                fmt::format(
                    "v2\t{}\t1\t0\t1\t1\t0\t0\t0\tmaybe\t"
                    "{}\t{}\t{}\tnatural_at_site\t1\t0\n",
                    kProfile,
                    command.executable.string(),
                    site_id,
                    fault_key)));
        },
        "true or false");
    expect_failure(
        [&command, &site_id, &fault_key] {
            static_cast<void>(parse_probe_observation(
                fmt::format(
                    "v1\t{}\t1\t0\t1\t1\t0\t0\t0\tfalse\t"
                    "{}\t{}\t{}\tnatural_at_site\t1\t0\n",
                    kProfile,
                    command.executable.string(),
                    site_id,
                    fault_key)));
        },
        "header shape");
    json wrong_contract =
        probe_contract_for_relation(
            "natural_at_site", "self-test probe contract");
    wrong_contract["abi_version"] = 1;
    expect_failure(
        [&wrong_contract] {
            require_probe_contract(
                wrong_contract,
                "natural_at_site",
                "self-test probe contract");
        },
        "differs");
    expect_failure(
        [] {
            static_cast<void>(parse_unsigned(
                "18446744073709551616",
                "self-test overflow"));
        },
        "overflows");
    expect_failure(
        [] {
            static_cast<void>(
                parse_json(
                    R"json({"value":1,"value":2})json",
                    "self-test duplicate JSON"));
        },
        "duplicate JSON key");

    const std::vector<
        std::vector<std::optional<std::string>>>
        result_rows{{
            std::optional<std::string>{"value"},
            std::nullopt}};
    if (result_rows_json(result_rows)
        != json::array(
            {json::array({"value", nullptr})})) {
        fail(
            "self-test",
            "exact recovery-row serialization differs");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const command_line command =
            parse_command_line(argc, argv);
        if (command.command
            == "--bioseq-safety-runtime-activate") {
            return activate(command);
        }
        if (command.command
            == "--bioseq-safety-runtime-describe") {
            return describe(command);
        }
        if (command.command
            == "--bioseq-safety-runtime-run") {
            return run_case(command);
        }
        if (command.command
            == "--bioseq-safety-runtime-self-test") {
            return self_test(command);
        }
        fail("command-line", "unsupported protocol command");
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "bioseq safety runtime harness: %s\n",
            error.what());
        return 1;
    } catch (...) {
        std::fprintf(
            stderr,
            "bioseq safety runtime harness: unknown failure\n");
        return 1;
    }
}
