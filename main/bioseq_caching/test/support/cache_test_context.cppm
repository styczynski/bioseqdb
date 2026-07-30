module;

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.cache.test_context;

import bioseq.test.postgres;

export namespace bioseq::cache_test {

using namespace std::chrono_literals;

/// @brief Build artifacts and watchdogs supplied by the native Catch2 runner.
struct runner_configuration {
    std::filesystem::path postgres_bin_dir;
    std::filesystem::path cache_control;
    std::filesystem::path cache_install_sql;
    std::filesystem::path cache_dso;
    std::optional<std::filesystem::path> bioseq_control;
    std::optional<std::filesystem::path> bioseq_install_sql;
    std::optional<std::filesystem::path> bioseq_dso;
    std::optional<std::filesystem::path> provider_peer_test_dso;
    std::filesystem::path report;
    std::chrono::seconds case_timeout{480};
};

/// @brief Supported sibling-DSO preload orders for coexistence tests.
enum class preload_order {
    none,
    cache_only,
    cache_then_bioseq,
    bioseq_then_cache,
};

/// @brief Public cache diagnostics sampled without affecting a data session.
struct cache_stats {
    std::int64_t produced{};
    std::int64_t completed_hits{};
    std::int64_t inflight_waits{};
    std::int64_t reuse_hits{};
    std::int64_t fallbacks{};
    std::int64_t invalidations{};
    std::int64_t evictions{};
    std::int64_t logical_bytes{};
    std::int64_t stored_bytes{};

    friend bool operator==(const cache_stats&, const cache_stats&) = default;
};

}  // namespace bioseq::cache_test

namespace bioseq::cache_test::detail {

/// @brief Returns the process-owned command-line configuration.
[[nodiscard]] inline runner_configuration& configuration_storage() {
    static runner_configuration value;
    return value;
}

/// @brief Parses a signed PostgreSQL text integer without locale dependence.
[[nodiscard]] inline std::int64_t parse_i64(std::string_view text) {
    std::int64_t value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("invalid PostgreSQL bigint text: "
                                 + std::string(text));
    }
    return value;
}

/// @brief Validates one required artifact path from the runner.
inline void require_artifact(const std::filesystem::path& path,
                             std::string_view label) {
    if (path.empty()) {
        throw std::invalid_argument(std::string(label) + " was not supplied");
    }
}

}  // namespace bioseq::cache_test::detail

export namespace bioseq::cache_test {

/// @brief Installs the immutable runner configuration before Catch starts.
inline void set_runner_configuration(runner_configuration configuration) {
    detail::require_artifact(configuration.postgres_bin_dir,
                             "PostgreSQL bin directory");
    detail::require_artifact(configuration.cache_control,
                             "cache control file");
    detail::require_artifact(configuration.cache_install_sql,
                             "cache install SQL");
    detail::require_artifact(configuration.cache_dso, "cache DSO");
    if (configuration.case_timeout <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("case timeout must be positive");
    }
    detail::configuration_storage() = std::move(configuration);
}

/// @brief Returns the runner configuration for the active executable.
[[nodiscard]] inline const runner_configuration& configuration() {
    return detail::configuration_storage();
}

/// @brief Returns true when the runner supplied a complete Bioseq artifact.
[[nodiscard]] inline bool has_bioseq_artifact() {
    const runner_configuration& value = configuration();
    return value.bioseq_control.has_value()
        && value.bioseq_install_sql.has_value()
        && value.bioseq_dso.has_value();
}

/// @brief Constructs a private-cluster configuration for one scenario.
[[nodiscard]] inline postgres_test::cluster_options cluster_options(
    bool include_bioseq,
    preload_order order = preload_order::cache_only,
    std::vector<std::pair<std::string, std::string>> startup_gucs = {},
    bool create_bioseq_first = false) {
    const runner_configuration& value = configuration();
    const postgres_test::extension_artifact cache_artifact{
        .name = "bioseq_cache",
        .control_file = value.cache_control,
        .install_sql = value.cache_install_sql,
        .dso = value.cache_dso};
    std::vector<postgres_test::extension_artifact> artifacts;
    if (include_bioseq) {
        if (!has_bioseq_artifact()) {
            throw std::logic_error(
                "combined test requires complete Bioseq artifacts");
        }
        const postgres_test::extension_artifact bioseq_artifact{
            .name = "bioseq",
            .control_file = *value.bioseq_control,
            .install_sql = *value.bioseq_install_sql,
            .dso = *value.bioseq_dso};
        if (create_bioseq_first) {
            artifacts = {bioseq_artifact, cache_artifact};
        } else {
            artifacts = {cache_artifact, bioseq_artifact};
        }
    } else {
        artifacts.push_back(cache_artifact);
    }

    std::vector<std::string> preload_libraries;
    switch (order) {
    case preload_order::none:
        break;
    case preload_order::cache_only:
        preload_libraries.emplace_back("bioseq_cache");
        break;
    case preload_order::cache_then_bioseq:
        if (!include_bioseq) {
            throw std::invalid_argument(
                "Bioseq preload order requires Bioseq artifacts");
        }
        preload_libraries = {"bioseq_cache", "bioseq"};
        break;
    case preload_order::bioseq_then_cache:
        if (!include_bioseq) {
            throw std::invalid_argument(
                "Bioseq preload order requires Bioseq artifacts");
        }
        preload_libraries = {"bioseq", "bioseq_cache"};
        break;
    }

    return postgres_test::cluster_options{
        .postgres_bin_dir = value.postgres_bin_dir,
        .extensions = std::move(artifacts),
        .preload_libraries = std::move(preload_libraries),
        .startup_gucs = std::move(startup_gucs),
        .process_environment = {},
        .pre_initdb = {},
        .startup_timeout = 60s,
        .stop_timeout = 30s};
}

/// @brief Executes a command and intentionally discards its command result.
inline void exec(libpq::connection& connection,
                 std::string_view sql,
                 std::chrono::milliseconds deadline = 10s,
                 libpq::transaction_policy policy =
                     libpq::transaction_policy::require_idle) {
    static_cast<void>(connection.exec(sql, deadline, policy));
}

/// @brief Returns one required, non-null scalar text value.
[[nodiscard]] inline std::string scalar(
    libpq::connection& connection,
    std::string_view sql,
    std::chrono::milliseconds deadline = 10s,
    libpq::transaction_policy policy =
        libpq::transaction_policy::require_idle) {
    const libpq::result result = connection.exec(sql, deadline, policy);
    if (result.rows() != 1 || result.columns() != 1 || result.is_null(0, 0)) {
        throw std::runtime_error(
            "expected exactly one non-null SQL scalar result");
    }
    return std::string(result.value(0, 0));
}

/// @brief Returns one required PostgreSQL Boolean scalar.
[[nodiscard]] inline bool boolean(
    libpq::connection& connection,
    std::string_view sql,
    std::chrono::milliseconds deadline = 10s,
    libpq::transaction_policy policy =
        libpq::transaction_policy::require_idle) {
    const std::string value = scalar(connection, sql, deadline, policy);
    if (value == "t") {
        return true;
    }
    if (value == "f") {
        return false;
    }
    throw std::runtime_error("invalid PostgreSQL Boolean text: " + value);
}

/// @brief Returns one required PostgreSQL signed-integer scalar.
[[nodiscard]] inline std::int64_t integer(
    libpq::connection& connection,
    std::string_view sql,
    std::chrono::milliseconds deadline = 10s,
    libpq::transaction_policy policy =
        libpq::transaction_policy::require_idle) {
    return detail::parse_i64(scalar(connection, sql, deadline, policy));
}

/// @brief Samples all public cache counters through a diagnostics session.
[[nodiscard]] inline cache_stats read_cache_stats(
    libpq::connection& admin,
    std::chrono::milliseconds deadline = 10s) {
    const libpq::result result = admin.exec(
        "SELECT produced, completed_hits, inflight_waits, reuse_hits, "
        "fallbacks, invalidations, evictions, logical_bytes, stored_bytes "
        "FROM bioseq_cache.stats()",
        deadline);
    if (result.rows() != 1 || result.columns() != 9) {
        throw std::runtime_error("bioseq_cache.stats() returned wrong shape");
    }
    for (int column = 0; column < result.columns(); ++column) {
        if (result.is_null(0, column)) {
            throw std::runtime_error("bioseq_cache.stats() returned NULL");
        }
    }
    return cache_stats{
        .produced = detail::parse_i64(result.value(0, 0)),
        .completed_hits = detail::parse_i64(result.value(0, 1)),
        .inflight_waits = detail::parse_i64(result.value(0, 2)),
        .reuse_hits = detail::parse_i64(result.value(0, 3)),
        .fallbacks = detail::parse_i64(result.value(0, 4)),
        .invalidations = detail::parse_i64(result.value(0, 5)),
        .evictions = detail::parse_i64(result.value(0, 6)),
        .logical_bytes = detail::parse_i64(result.value(0, 7)),
        .stored_bytes = detail::parse_i64(result.value(0, 8))};
}

/// @brief Computes a nonnegative counter delta and rejects reset/wrap races.
[[nodiscard]] inline cache_stats operator-(const cache_stats& after,
                                           const cache_stats& before) {
    const auto delta = [](std::int64_t newer, std::int64_t older,
                          std::string_view name) {
        if (newer < older) {
            throw std::runtime_error("cache counter moved backwards: "
                                     + std::string(name));
        }
        return newer - older;
    };
    return cache_stats{
        .produced = delta(after.produced, before.produced, "produced"),
        .completed_hits =
            delta(after.completed_hits, before.completed_hits,
                  "completed_hits"),
        .inflight_waits =
            delta(after.inflight_waits, before.inflight_waits,
                  "inflight_waits"),
        .reuse_hits = delta(after.reuse_hits, before.reuse_hits, "reuse_hits"),
        .fallbacks = delta(after.fallbacks, before.fallbacks, "fallbacks"),
        .invalidations =
            delta(after.invalidations, before.invalidations, "invalidations"),
        .evictions = delta(after.evictions, before.evictions, "evictions"),
        .logical_bytes =
            delta(after.logical_bytes, before.logical_bytes, "logical_bytes"),
        .stored_bytes =
            delta(after.stored_bytes, before.stored_bytes, "stored_bytes")};
}

}  // namespace bioseq::cache_test
