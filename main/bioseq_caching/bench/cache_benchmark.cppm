module;

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/utsname.h>
#include <unistd.h>

#include "../test/support/legacy_migration_ledger.hpp"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

export module bioseq.cache.benchmark;

import bioseq.common.durable_file_io;
import bioseq.test.postgres;

export namespace bioseq::cache_benchmark {

/// @brief Private-cluster workload owned by one benchmark executable.
enum class scenario {
    standalone,
    bioseq_2gbp,
};

}  // namespace bioseq::cache_benchmark

namespace bioseq::cache_benchmark {

using namespace std::chrono_literals;

constexpr std::uint64_t total_input_bases = 2'000'000'000ULL;
constexpr std::uint64_t query_bases = 5'000'000ULL;
constexpr std::uint64_t target_bases =
    total_input_bases - query_bases;
static_assert(query_bases > 0);
static_assert(query_bases + target_bases == total_input_bases);
constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t minimum_data_space =
    32ULL * gibibyte > 12ULL * total_input_bases
    ? 32ULL * gibibyte
    : 12ULL * total_input_bases;
constexpr std::uint64_t io_probe_bytes = gibibyte;
constexpr double minimum_io_mib_per_second = 150.0;
constexpr std::uint64_t standalone_fixture_rows = 20'000ULL;
constexpr std::string_view report_schema{"bioseq-cache-benchmark-v2"};
constexpr std::chrono::milliseconds rss_sample_interval{50};
constexpr std::chrono::milliseconds temp_sample_interval{1000};
constexpr std::string_view standalone_measurement_cohort{
    "standalone-sql"};
constexpr std::string_view combined_measurement_cohort{
    "bioseq-cpu-column-cache-on"};
constexpr std::string_view standalone_query{
    "SELECT id, group_id, optional_value, payload "
    "FROM cache_benchmark_rows "
    "ORDER BY group_id, id, optional_value, payload"};
constexpr std::string_view bioseq_query{
    "SELECT target_sample.id, "
    "bioseq.target_start(target_sample.id), "
    "bioseq.target_end(target_sample.id), "
    "bioseq.target_len(target_sample.id), "
    "bioseq.query_start(target_sample.id), "
    "bioseq.query_end(target_sample.id), "
    "bioseq.query_len(target_sample.id), "
    "bioseq.is_reverse(target_sample.id) "
    "FROM cache_bioseq_2gbp AS target_sample "
    "JOIN cache_bioseq_2gbp AS query_sample "
    "ON target_sample.id = query_sample.id "
    "AND mmsearch(target_sample.target, query_sample.query, "
    "aligner_options(index_read_requirement => 2)) "
    "ORDER BY target_sample.id, "
    "bioseq.target_start(target_sample.id), "
    "bioseq.target_end(target_sample.id), "
    "bioseq.target_len(target_sample.id), "
    "bioseq.query_start(target_sample.id), "
    "bioseq.query_end(target_sample.id), "
    "bioseq.query_len(target_sample.id), "
    "bioseq.is_reverse(target_sample.id)"};

struct benchmark_options {
    scenario selected_scenario{scenario::standalone};
    std::filesystem::path postgres_bin_dir;
    std::filesystem::path postgres_stage_root;
    std::filesystem::path postgres_stage_manifest;
    std::filesystem::path cache_control;
    std::filesystem::path cache_install_sql;
    std::filesystem::path cache_dso;
    std::optional<std::filesystem::path> bioseq_control;
    std::optional<std::filesystem::path> bioseq_install_sql;
    std::optional<std::filesystem::path> bioseq_dso;
    std::filesystem::path migration_ledger;
    std::filesystem::path report;
    std::uint32_t samples{7};
    std::uint32_t warmup_samples{1};
};

struct cache_counters {
    std::int64_t produced{};
    std::int64_t completed_hits{};
    std::int64_t inflight_waits{};
    std::int64_t reuse_hits{};
    std::int64_t fallbacks{};
    std::int64_t invalidations{};
    std::int64_t evictions{};
    std::int64_t logical_bytes{};
    std::int64_t stored_bytes{};
};

struct benchmark_sample {
    std::string phase;
    std::string measurement_cohort;
    std::uint32_t ordinal{};
    std::uint64_t wall_time_ns{};
    std::uint64_t time_to_first_row_ns{};
    std::uint64_t rows{};
    std::string correctness_digest;
    cache_counters counter_delta;
};

struct fixture_evidence {
    std::string relation;
    std::uint64_t source_rows{};
    std::uint64_t query_base_count{};
    std::uint64_t target_base_count{};
    std::uint64_t aggregate_input_base_count{};
    std::string query;
};

struct build_identity_evidence {
    std::filesystem::path executable_path;
    std::string executable_sha256;
    std::string compiler;
    std::string compiler_version;
    std::uint64_t cxx_standard{};
    std::string build_mode;
    std::string identity_sha256;
};

struct host_identity_evidence {
    std::string system_name;
    std::string node_name;
    std::string release;
    std::string version;
    std::string machine;
    std::uint32_t logical_cpu_count{};
    std::uint64_t page_size_bytes{};
    std::string identity_sha256;
};

struct benchmark_resource_report {
    std::uint64_t peak_postgres_rss_bytes{};
    std::uint64_t peak_temp_bytes{};
    std::uint64_t rss_samples{};
    std::uint64_t temp_samples{};
    std::uint64_t rss_sampling_failures{};
    std::uint64_t temp_sampling_failures{};
};

struct exact_2gbp_host_qualification_evidence {
    bool required{};
    bool performed{};
    bool same_execution_host{};
    std::string status{"not-required"};
    std::string evidence_source{"not-required"};
    std::string detail{"not required for standalone scenario"};
    std::string host_identity_sha256;
    std::uint64_t effective_total_ram_bytes{};
    std::uint64_t effective_available_ram_bytes{};
    std::uint64_t required_total_ram_bytes{};
    std::uint64_t required_available_ram_bytes{};
    std::uint64_t data_filesystem_capacity_bytes{};
    std::uint64_t data_filesystem_available_bytes{};
    std::uint64_t required_data_filesystem_available_bytes{};
    double effective_cpu_cores{};
    std::uint32_t required_logical_cores{};
    std::uint64_t io_probe_bytes{};
    double io_write_mib_per_second{};
    double io_read_mib_per_second{};
    double required_io_mib_per_second{};
};

struct benchmark_cleanup_evidence {
    bool attempted{};
    bool succeeded{};
    bool private_root_removed{};
    std::filesystem::path private_root;
    std::string detail{"cleanup has not run"};
    std::string verdict_source{"private_cluster::cleanup"};
    std::string external_watchdog_scope{
        "not claimed by the native report; the MCP harness must verify "
        "retained processes, roots, and supervision state out of process"};
};

struct benchmark_migration_probe {
    std::string owner;
    std::string role;
    std::string stable_id;
    std::string origin_kind{"benchmark-phase"};
    std::string origin;
    std::string permutation;
    std::uint64_t ordinal{};
    bool passed{};
};

struct benchmark_report {
    std::string status{"passed"};
    std::string error;
    scenario selected_scenario{scenario::standalone};
    std::string executable;
    build_identity_evidence build_identity;
    host_identity_evidence host_identity;
    std::string postgres_server_version;
    std::uint32_t configured_samples{};
    std::uint32_t configured_warmup_samples{};
    std::string measurement_cohort;
    exact_2gbp_host_qualification_evidence
        exact_2gbp_host_qualification;
    fixture_evidence fixture;
    std::string correctness_digest;
    std::optional<benchmark_sample> cold_producer;
    std::vector<benchmark_sample> samples;
    cache_counters counter_deltas;
    std::uint64_t logical_bytes{};
    std::uint64_t stored_bytes{};
    benchmark_resource_report resources;
    std::vector<postgres_test::artifact_report> artifacts;
    std::string migration_evidence_harness;
    std::string migration_report_role;
    std::string migration_coverage{"full-role"};
    std::filesystem::path migration_ledger;
    std::string migration_ledger_sha256;
    std::string evidence_contract_sha256;
    std::string migration_phase{"not-sealed"};
    std::uint64_t migration_mapped_rows{};
    std::uint64_t migration_passing_rows{};
    std::uint64_t migration_retired_with_proof_rows{};
    std::vector<benchmark_migration_probe> migration_probes;
    benchmark_cleanup_evidence cleanup;
};

struct sealed_migration_contract {
    std::filesystem::path path;
    std::string raw_document;
    bioseq::cache_test::migration::ledger parsed;
    std::string raw_sha256;
    std::string evidence_contract_sha256;
    std::string phase;
};

struct measured_result {
    libpq::result rows;
    std::uint64_t wall_time_ns{};
    std::uint64_t time_to_first_row_ns{};
};

[[nodiscard]] std::filesystem::path current_executable_path(
    std::string_view argument_zero) {
    std::filesystem::path executable;
#if defined(__linux__)
    std::error_code link_error;
    executable =
        std::filesystem::read_symlink("/proc/self/exe", link_error);
    if (link_error) {
        executable.clear();
    }
#elif defined(__APPLE__)
    std::uint32_t capacity = 1024;
    std::vector<char> buffer(capacity);
    if (::_NSGetExecutablePath(buffer.data(), &capacity) != 0) {
        buffer.resize(capacity);
        if (::_NSGetExecutablePath(buffer.data(), &capacity) != 0) {
            buffer.clear();
        }
    }
    if (!buffer.empty()) {
        executable = buffer.data();
    }
#endif
    if (executable.empty()) {
        if (argument_zero.empty()) {
            throw std::runtime_error(
                "cannot identify the benchmark executable");
        }
        executable = std::filesystem::absolute(argument_zero);
    }
    std::error_code canonical_error;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(executable, canonical_error);
    return canonical_error ? std::filesystem::absolute(executable)
                           : canonical;
}

[[nodiscard]] std::string compiler_name() {
#if defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string compiler_version() {
#if defined(__clang__)
    return __clang_version__;
#elif defined(__GNUC__)
    return __VERSION__;
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string build_mode() {
#if defined(NDEBUG)
    return "ndebug";
#else
    return "assertions-enabled";
#endif
}

[[nodiscard]] build_identity_evidence build_identity(
    std::string_view argument_zero) {
    const std::filesystem::path executable =
        current_executable_path(argument_zero);
    return build_identity_evidence{
        .executable_path = executable,
        .executable_sha256 =
            postgres_test::sha256_file(executable).hex(),
        .compiler = compiler_name(),
        .compiler_version = compiler_version(),
        .cxx_standard = static_cast<std::uint64_t>(__cplusplus),
        .build_mode = build_mode()};
}

[[nodiscard]] host_identity_evidence host_identity() {
    utsname information{};
    if (::uname(&information) != 0) {
        throw std::runtime_error(
            "uname failed while capturing benchmark host identity");
    }
    const long page_size = ::sysconf(_SC_PAGESIZE);
    host_identity_evidence evidence{
        .system_name = information.sysname,
        .node_name = information.nodename,
        .release = information.release,
        .version = information.version,
        .machine = information.machine,
        .logical_cpu_count = std::thread::hardware_concurrency(),
        .page_size_bytes =
            page_size > 0 ? static_cast<std::uint64_t>(page_size) : 0};
    std::string material = evidence.system_name;
    material.append("|");
    material.append(evidence.node_name);
    material.append("|");
    material.append(evidence.release);
    material.append("|");
    material.append(evidence.version);
    material.append("|");
    material.append(evidence.machine);
    material.append("|");
    material.append(std::to_string(evidence.logical_cpu_count));
    material.append("|");
    material.append(std::to_string(evidence.page_size_bytes));
    evidence.identity_sha256 = postgres_test::sha256(material).hex();
    return evidence;
}

void finalize_build_identity(benchmark_report& report) {
    std::string material = report.build_identity.executable_sha256;
    material.append("|");
    material.append(report.build_identity.compiler);
    material.append("|");
    material.append(report.build_identity.compiler_version);
    material.append("|");
    material.append(std::to_string(report.build_identity.cxx_standard));
    material.append("|");
    material.append(report.build_identity.build_mode);
    for (const postgres_test::artifact_report& artifact : report.artifacts) {
        material.append("|");
        material.append(artifact.name);
        material.append("|");
        material.append(artifact.stage_manifest_sha256);
        material.append("|");
        material.append(artifact.control_sha256);
        material.append("|");
        material.append(artifact.install_sql_sha256);
        material.append("|");
        material.append(artifact.dso_sha256);
        material.append("|");
        material.append(artifact.staged_control_sha256);
    }
    report.build_identity.identity_sha256 =
        postgres_test::sha256(material).hex();
}

[[nodiscard]] bool resource_sampling_contract_satisfied(
    const benchmark_resource_report& report) noexcept {
    return report.rss_samples >= 2
        && report.peak_postgres_rss_bytes != 0
        && report.rss_sampling_failures == 0
        && report.temp_samples >= 2
        && report.peak_temp_bytes != 0
        && report.temp_sampling_failures == 0;
}

[[nodiscard]] benchmark_resource_report adapt_resource_report(
    const postgres_test::resource_sampling_report& source) noexcept {
    return benchmark_resource_report{
        .peak_postgres_rss_bytes =
            source.peak_postmaster_process_tree_rss_bytes,
        .peak_temp_bytes = source.peak_owned_root_bytes,
        .rss_samples = source.rss_samples,
        .temp_samples = source.owned_root_samples,
        .rss_sampling_failures = source.rss_sampling_failures,
        .temp_sampling_failures =
            source.owned_root_sampling_failures};
}

/// @brief Adapts the shared sampler to stable benchmark-v2 field names.
class benchmark_resource_tracker {
public:
    benchmark_resource_tracker(
        std::filesystem::path cluster_root,
        const std::filesystem::path& data_directory,
        benchmark_resource_report& destination)
        : sampler_(
              std::move(cluster_root), data_directory,
              rss_sample_interval, temp_sample_interval),
          destination_(&destination) {}

    benchmark_resource_tracker(const benchmark_resource_tracker&) = delete;
    benchmark_resource_tracker& operator=(
        const benchmark_resource_tracker&) = delete;

    ~benchmark_resource_tracker() noexcept {
        if (!finished_) {
            try {
                static_cast<void>(finish());
            } catch (...) {
                // A failing benchmark retains partial resource evidence;
                // the original workload error remains authoritative.
            }
        }
    }

    void observe_phase_boundary(std::string_view phase) noexcept {
        sampler_.observe_phase_boundary(phase);
    }

    [[nodiscard]] benchmark_resource_report finish() {
        const benchmark_resource_report report =
            adapt_resource_report(sampler_.finish());
        *destination_ = report;
        finished_ = true;
        if (!resource_sampling_contract_satisfied(report)) {
            throw std::runtime_error(
                "shared PostgreSQL RSS/root resource sampling was "
                "incomplete");
        }
        return report;
    }

private:
    postgres_test::resource_sampler sampler_;
    benchmark_resource_report* destination_{};
    bool finished_{};
};

[[nodiscard]] std::string_view scenario_name(scenario value) noexcept {
    switch (value) {
    case scenario::standalone:
        return "standalone";
    case scenario::bioseq_2gbp:
        return "bioseq-2gbp";
    }
    return "unknown";
}

[[nodiscard]] scenario parse_scenario(std::string_view value) {
    if (value == "standalone") {
        return scenario::standalone;
    }
    if (value == "bioseq-2gbp") {
        return scenario::bioseq_2gbp;
    }
    throw std::invalid_argument(
        "scenario must be standalone or bioseq-2gbp");
}

template <class Integer>
[[nodiscard]] Integer parse_integer(
    std::string_view value, std::string_view option, bool allow_zero) {
    Integer parsed{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()
        || (!allow_zero && parsed == 0)) {
        throw std::invalid_argument(
            "invalid numeric value for " + std::string(option));
    }
    return parsed;
}

[[nodiscard]] std::string_view required_option_value(
    int argc, char** argv, int& index, std::string_view option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(
            "missing value for " + std::string(option));
    }
    return argv[++index];
}

[[nodiscard]] benchmark_options parse_options(
    int argc, char** argv, scenario required_scenario) {
    benchmark_options options;
    options.selected_scenario = required_scenario;
    options.samples =
        required_scenario == scenario::bioseq_2gbp ? 3U : 7U;
    bool scenario_seen = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto assign_path =
            [argc, argv, &index, argument](
                std::filesystem::path& destination,
                std::string_view option) {
                destination = required_option_value(
                    argc, argv, index, option);
            };
        if (argument == "--scenario") {
            options.selected_scenario = parse_scenario(
                required_option_value(argc, argv, index, argument));
            scenario_seen = true;
        } else if (argument.starts_with("--scenario=")) {
            options.selected_scenario =
                parse_scenario(argument.substr(11));
            scenario_seen = true;
        } else if (argument == "--postgres-bin-dir") {
            assign_path(options.postgres_bin_dir, argument);
        } else if (argument.starts_with("--postgres-bin-dir=")) {
            options.postgres_bin_dir = argument.substr(19);
        } else if (argument == "--postgres-stage-root") {
            assign_path(options.postgres_stage_root, argument);
        } else if (argument.starts_with("--postgres-stage-root=")) {
            options.postgres_stage_root = argument.substr(22);
        } else if (argument == "--postgres-stage-manifest") {
            assign_path(options.postgres_stage_manifest, argument);
        } else if (argument.starts_with("--postgres-stage-manifest=")) {
            options.postgres_stage_manifest = argument.substr(26);
        } else if (argument == "--cache-control") {
            assign_path(options.cache_control, argument);
        } else if (argument.starts_with("--cache-control=")) {
            options.cache_control = argument.substr(16);
        } else if (argument == "--cache-install-sql") {
            assign_path(options.cache_install_sql, argument);
        } else if (argument.starts_with("--cache-install-sql=")) {
            options.cache_install_sql = argument.substr(20);
        } else if (argument == "--cache-dso") {
            assign_path(options.cache_dso, argument);
        } else if (argument.starts_with("--cache-dso=")) {
            options.cache_dso = argument.substr(12);
        } else if (argument == "--bioseq-control") {
            std::filesystem::path path;
            assign_path(path, argument);
            options.bioseq_control = std::move(path);
        } else if (argument.starts_with("--bioseq-control=")) {
            options.bioseq_control =
                std::filesystem::path(argument.substr(17));
        } else if (argument == "--bioseq-install-sql") {
            std::filesystem::path path;
            assign_path(path, argument);
            options.bioseq_install_sql = std::move(path);
        } else if (argument.starts_with("--bioseq-install-sql=")) {
            options.bioseq_install_sql =
                std::filesystem::path(argument.substr(21));
        } else if (argument == "--bioseq-dso") {
            std::filesystem::path path;
            assign_path(path, argument);
            options.bioseq_dso = std::move(path);
        } else if (argument.starts_with("--bioseq-dso=")) {
            options.bioseq_dso =
                std::filesystem::path(argument.substr(13));
        } else if (argument == "--migration-ledger") {
            assign_path(options.migration_ledger, argument);
        } else if (argument.starts_with("--migration-ledger=")) {
            options.migration_ledger = argument.substr(19);
        } else if (argument == "--report") {
            assign_path(options.report, argument);
        } else if (argument.starts_with("--report=")) {
            options.report = argument.substr(9);
        } else if (argument == "--samples") {
            options.samples = parse_integer<std::uint32_t>(
                required_option_value(argc, argv, index, argument),
                argument, false);
        } else if (argument.starts_with("--samples=")) {
            options.samples = parse_integer<std::uint32_t>(
                argument.substr(10), "--samples", false);
        } else if (argument == "--warmup-samples") {
            options.warmup_samples = parse_integer<std::uint32_t>(
                required_option_value(argc, argv, index, argument),
                argument, true);
        } else if (argument.starts_with("--warmup-samples=")) {
            options.warmup_samples = parse_integer<std::uint32_t>(
                argument.substr(17), "--warmup-samples", true);
        } else {
            throw std::invalid_argument(
                "unknown benchmark argument: " + std::string(argument));
        }
    }

    if (!scenario_seen || options.selected_scenario != required_scenario) {
        throw std::invalid_argument(
            "benchmark executable was invoked with the wrong scenario");
    }
    for (const auto& [path, name] :
         std::vector<std::pair<std::filesystem::path, std::string_view>>{
             {options.postgres_bin_dir, "--postgres-bin-dir"},
             {options.postgres_stage_root, "--postgres-stage-root"},
             {options.postgres_stage_manifest,
              "--postgres-stage-manifest"},
             {options.cache_control, "--cache-control"},
             {options.cache_install_sql, "--cache-install-sql"},
             {options.cache_dso, "--cache-dso"},
             {options.migration_ledger, "--migration-ledger"},
             {options.report, "--report"}}) {
        if (path.empty()) {
            throw std::invalid_argument(
                "required benchmark option is missing: "
                + std::string(name));
        }
    }
    if (options.samples < 3) {
        throw std::invalid_argument(
            "native cache benchmarks require at least three measured "
            "warm samples");
    }
    if (required_scenario == scenario::bioseq_2gbp) {
        if (!options.bioseq_control || !options.bioseq_install_sql
            || !options.bioseq_dso) {
            throw std::invalid_argument(
                "bioseq-2gbp requires all Bioseq artifact paths");
        }
    }
    return options;
}

void validate_native_parser_contract() {
    const auto require_rejection =
        [](std::vector<std::string> arguments,
           scenario required_scenario,
           std::string_view expected_message) {
            std::vector<char*> argv;
            argv.reserve(arguments.size());
            for (std::string& argument : arguments) {
                argv.push_back(argument.data());
            }
            try {
                static_cast<void>(parse_options(
                    static_cast<int>(argv.size()), argv.data(),
                    required_scenario));
            } catch (const std::invalid_argument& error) {
                if (std::string_view(error.what()).find(expected_message)
                    != std::string_view::npos) {
                    return;
                }
                throw std::runtime_error(
                    "native parser contract rejected for the wrong "
                    "reason: " + std::string(error.what()));
            }
            throw std::runtime_error(
                "native parser contract accepted invalid arguments");
        };
    require_rejection(
        {"contract-probe", "--scenario", "standalone"},
        scenario::standalone, "required benchmark option is missing");
    require_rejection(
        {"contract-probe", "--scenario", "standalone", "--samples=0"},
        scenario::standalone, "invalid numeric value for --samples");
    require_rejection(
        {
            "contract-probe",
            "--scenario=standalone",
            "--postgres-bin-dir=/contract/bin",
            "--postgres-stage-root=/contract/stage",
            "--postgres-stage-manifest=/contract/stage.json",
            "--cache-control=/contract/cache.control",
            "--cache-install-sql=/contract/cache.sql",
            "--cache-dso=/contract/cache.so",
            "--migration-ledger=/contract/ledger.tsv",
            "--report=/contract/report.json",
            "--samples=2",
        },
        scenario::standalone, "at least three measured warm samples");
    require_rejection(
        {"contract-probe", "--scenario", "bioseq-2gbp"},
        scenario::standalone,
        "benchmark executable was invoked with the wrong scenario");
    require_rejection(
        {"contract-probe", "--scenario", "standalone", "--unknown"},
        scenario::standalone, "unknown benchmark argument");
}

[[nodiscard]] std::optional<std::filesystem::path> report_path_hint(
    int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--report" && index + 1 < argc) {
            return std::filesystem::path(argv[index + 1]);
        }
        if (argument.starts_with("--report=")) {
            return std::filesystem::path(argument.substr(9));
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::int64_t parse_i64(std::string_view value) {
    std::int64_t parsed{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("PostgreSQL counter is not an int64");
    }
    return parsed;
}

[[nodiscard]] cache_counters read_cache_counters(
    libpq::connection& connection,
    std::chrono::milliseconds timeout = 30s) {
    const libpq::result result = connection.exec(
        "SELECT produced, completed_hits, inflight_waits, reuse_hits, "
        "fallbacks, invalidations, evictions, logical_bytes, stored_bytes "
        "FROM bioseq_cache.stats()",
        timeout);
    if (result.rows() != 1 || result.columns() != 9) {
        throw std::runtime_error(
            "bioseq_cache.stats() returned an unexpected shape");
    }
    for (int column = 0; column < result.columns(); ++column) {
        if (result.is_null(0, column)) {
            throw std::runtime_error(
                "bioseq_cache.stats() returned a null counter");
        }
    }
    return cache_counters{
        .produced = parse_i64(result.value(0, 0)),
        .completed_hits = parse_i64(result.value(0, 1)),
        .inflight_waits = parse_i64(result.value(0, 2)),
        .reuse_hits = parse_i64(result.value(0, 3)),
        .fallbacks = parse_i64(result.value(0, 4)),
        .invalidations = parse_i64(result.value(0, 5)),
        .evictions = parse_i64(result.value(0, 6)),
        .logical_bytes = parse_i64(result.value(0, 7)),
        .stored_bytes = parse_i64(result.value(0, 8))};
}

[[nodiscard]] cache_counters subtract_counters(
    const cache_counters& after, const cache_counters& before) {
    const auto delta =
        [](std::int64_t newer, std::int64_t older,
           std::string_view name) -> std::int64_t {
            if (newer < older) {
                throw std::runtime_error(
                    "cache counter moved backwards: " + std::string(name));
            }
            return newer - older;
        };
    return cache_counters{
        .produced = delta(after.produced, before.produced, "produced"),
        .completed_hits =
            delta(after.completed_hits, before.completed_hits,
                  "completed_hits"),
        .inflight_waits =
            delta(after.inflight_waits, before.inflight_waits,
                  "inflight_waits"),
        .reuse_hits =
            delta(after.reuse_hits, before.reuse_hits, "reuse_hits"),
        .fallbacks = delta(after.fallbacks, before.fallbacks, "fallbacks"),
        .invalidations =
            delta(after.invalidations, before.invalidations, "invalidations"),
        .evictions =
            delta(after.evictions, before.evictions, "evictions"),
        .logical_bytes =
            delta(after.logical_bytes, before.logical_bytes, "logical_bytes"),
        .stored_bytes =
            delta(after.stored_bytes, before.stored_bytes, "stored_bytes")};
}

void execute(
    libpq::connection& connection, std::string_view sql,
    std::chrono::milliseconds timeout) {
    static_cast<void>(connection.exec(sql, timeout));
}

[[nodiscard]] measured_result measure(
    libpq::connection& connection, std::string_view sql,
    std::chrono::milliseconds timeout) {
    libpq::single_row_observed_result observed =
        connection.exec_single_row_observed(
            sql,
            timeout,
            libpq::execution_limits{
                .max_rows = 1'000'000,
                .max_columns = 32,
                .max_cell_bytes = 64ULL * 1024ULL * 1024ULL,
                .max_total_bytes = 1ULL * 1024ULL * 1024ULL * 1024ULL,
                .max_result_sets = 1});
    if (observed.rows.rows() > 0 && !observed.time_to_first_row) {
        throw std::runtime_error(
            "libpq returned rows without a first-row observation");
    }
    if (observed.time_to_first_row
        && *observed.time_to_first_row > observed.elapsed) {
        throw std::runtime_error(
            "libpq first-row observation exceeds completion time");
    }
    return measured_result{
        .rows = std::move(observed.rows),
        .wall_time_ns = static_cast<std::uint64_t>(
            observed.elapsed.count()),
        .time_to_first_row_ns =
            observed.time_to_first_row
                ? static_cast<std::uint64_t>(
                      observed.time_to_first_row->count())
                : 0};
}

void configure_cpu_path(libpq::connection& connection) {
    execute(
        connection,
        "SET bioseq.aligner.vulkan_enabled = off; "
        "SET bioseq.aligner.vulkan_seed_hits_enabled = off; "
        "SET bioseq.aligner.vulkan_index_cache_enabled = off; "
        "SET bioseq.aligner.vulkan_fused_frontend_enabled = off; "
        "SET bioseq.aligner.vulkan_gpu_index_build_enabled = off; "
        "SET bioseq.aligner.vulkan_extend_enabled = off; "
        "SET bioseq.enable_custom_scan = on; "
        "SET bioseq.enable_column_cache = on; "
        "SET bioseq.read_sequence_index = on",
        30s);
}

void create_standalone_fixture(libpq::connection& connection) {
    execute(
        connection,
        "CREATE TABLE cache_benchmark_rows("
        "id bigint PRIMARY KEY, "
        "group_id integer NOT NULL, "
        "optional_value integer, "
        "payload text NOT NULL); "
        "INSERT INTO cache_benchmark_rows "
        "SELECT id, id % 101, "
        "CASE WHEN id % 3 = 0 THEN NULL ELSE id % 1000 END, "
        "repeat(md5(id::text), 4) "
        "FROM generate_series(1, 20000) AS generated(id); "
        "ANALYZE cache_benchmark_rows",
        2min);
}

[[nodiscard]] fixture_evidence create_bioseq_fixture(
    libpq::connection& connection) {
    configure_cpu_path(connection);
    execute(
        connection,
        "SET bioseq.update_index_on_insert = off; "
        "SELECT bioseq.create_query_target_sample_table("
        "'cache_bioseq_2gbp', 1, 5000000, 0.0, 1995000000, "
        "0.001, 0.0, 0.0, 1.0, 1.0, 0.5, 3219876)",
        40min);
    const libpq::result measured_input = connection.exec(
        "SELECT count(*)::bigint, "
        "sum(query_locator.total_bp_size)::bigint, "
        "sum(target_locator.total_bp_size)::bigint, "
        "sum(query_locator.total_bp_size "
        "    + target_locator.total_bp_size)::bigint "
        "FROM cache_bioseq_2gbp AS sample "
        "CROSS JOIN LATERAL "
        "bioseq.inspect_locator(sample.query) AS query_locator "
        "CROSS JOIN LATERAL "
        "bioseq.inspect_locator(sample.target) AS target_locator",
        5min);
    if (measured_input.rows() != 1
        || measured_input.columns() != 4) {
        throw std::runtime_error(
            "bioseq-2gbp stored-input measurement has wrong shape");
    }
    for (int column = 0; column < measured_input.columns(); ++column) {
        if (measured_input.is_null(0, column)) {
            throw std::runtime_error(
                "bioseq-2gbp stored-input measurement is null");
        }
    }
    const std::int64_t measured_source_rows =
        parse_i64(measured_input.value(0, 0));
    const std::int64_t measured_query_bases =
        parse_i64(measured_input.value(0, 1));
    const std::int64_t measured_target_bases =
        parse_i64(measured_input.value(0, 2));
    const std::int64_t measured_total_input_bases =
        parse_i64(measured_input.value(0, 3));
    if (measured_source_rows != 1
        || measured_query_bases
            != static_cast<std::int64_t>(query_bases)
        || measured_query_bases <= 0
        || measured_target_bases
            != static_cast<std::int64_t>(target_bases)
        || measured_total_input_bases
            != static_cast<std::int64_t>(total_input_bases)
        || measured_query_bases + measured_target_bases
            != measured_total_input_bases) {
        throw std::runtime_error(
            "bioseq-2gbp stored input is not exactly "
            "5,000,000 query bases plus 1,995,000,000 target bases");
    }
    execute(
        connection,
        "SELECT bioseq.update_index("
        "'cache_bioseq_2gbp'::regclass, 'minimizers_per_fragment'); "
        "SELECT bioseq.set_active_read_index("
        "'cache_bioseq_2gbp'::regclass, 'minimizers_per_fragment')",
        40min);
    return fixture_evidence{
        .relation = "cache_bioseq_2gbp",
        .source_rows =
            static_cast<std::uint64_t>(measured_source_rows),
        .query_base_count =
            static_cast<std::uint64_t>(measured_query_bases),
        .target_base_count =
            static_cast<std::uint64_t>(measured_target_bases),
        .aggregate_input_base_count =
            static_cast<std::uint64_t>(measured_total_input_bases),
        .query = std::string(bioseq_query)};
}

[[nodiscard]] std::vector<postgres_test::extension_artifact>
extension_artifacts(const benchmark_options& options) {
    std::vector<postgres_test::extension_artifact> artifacts{
        postgres_test::extension_artifact{
            .name = "bioseq_cache",
            .control_file = options.cache_control,
            .install_sql = options.cache_install_sql,
            .dso = options.cache_dso}};
    if (options.selected_scenario == scenario::bioseq_2gbp) {
        artifacts.push_back(postgres_test::extension_artifact{
            .name = "bioseq",
            .control_file = *options.bioseq_control,
            .install_sql = *options.bioseq_install_sql,
            .dso = *options.bioseq_dso});
    }
    return artifacts;
}

[[nodiscard]] postgres_test::cluster_options cluster_options(
    const benchmark_options& options) {
    std::vector<std::string> preloads{"bioseq_cache"};
    if (options.selected_scenario == scenario::bioseq_2gbp) {
        preloads.emplace_back("bioseq");
    }
    postgres_test::cluster_options result{
        .postgres_bin_dir = options.postgres_bin_dir,
        .preinstalled_stage_root = options.postgres_stage_root,
        .preinstalled_stage_manifest = options.postgres_stage_manifest,
        .extensions = extension_artifacts(options),
        .allowed_runtime_dso_overrides = {},
        .preload_libraries = std::move(preloads),
        .startup_gucs = {
            {"max_parallel_workers", "0"},
            {"max_parallel_workers_per_gather", "0"}},
        .startup_timeout = 90s,
        .stop_timeout = 30s};
    if (options.selected_scenario == scenario::bioseq_2gbp) {
        result.pre_initdb =
            [](const postgres_test::pre_initdb_context& context) {
                static_cast<void>(
                    postgres_test::qualify_host_before_initdb(
                        context.data_directory,
                        postgres_test::host_qualification_requirements{
                            .test_case =
                                "bioseq cache 2 Gbp native benchmark",
                            // RAM is telemetry: ticketed allocations, bounded
                            // chunk caches, and spillable runs enforce the
                            // workload's actual memory envelope.
                            .minimum_total_ram_bytes = 0,
                            .minimum_available_ram_bytes = 0,
                            .minimum_data_available_bytes =
                                minimum_data_space,
                            .minimum_logical_cores = 8,
                            .io_probe_bytes = io_probe_bytes,
                            .minimum_io_mib_per_second =
                                minimum_io_mib_per_second}));
            };
    }
    return result;
}

[[nodiscard]] exact_2gbp_host_qualification_evidence
qualification_evidence(
    const postgres_test::host_qualification_report& source,
    const host_identity_evidence& host) {
    exact_2gbp_host_qualification_evidence evidence{
        .required = true,
        .performed = true,
        .same_execution_host = true,
        .status =
            source.status
                    == postgres_test::host_qualification_status::passed
                ? "passed"
                : "infrastructure-inconclusive",
        .evidence_source =
            "same benchmark process before initdb",
        .detail = source.detail,
        .host_identity_sha256 = host.identity_sha256,
        .effective_total_ram_bytes =
            source.effective_total_ram_bytes,
        .effective_available_ram_bytes =
            source.effective_available_ram_bytes,
        .required_total_ram_bytes =
            source.required_total_ram_bytes,
        .required_available_ram_bytes =
            source.required_available_ram_bytes,
        .effective_cpu_cores = source.effective_cpu_cores,
        .required_logical_cores = source.required_logical_cores,
        .io_probe_bytes = source.io.bytes,
        .io_write_mib_per_second =
            source.io.write_mib_per_second,
        .io_read_mib_per_second =
            source.io.read_mib_per_second,
        .required_io_mib_per_second =
            source.required_io_mib_per_second};
    const auto data_filesystem = std::find_if(
        source.filesystems.begin(), source.filesystems.end(),
        [](const postgres_test::filesystem_qualification_report&
               filesystem) {
            return filesystem.role == "data";
        });
    if (data_filesystem != source.filesystems.end()) {
        evidence.data_filesystem_capacity_bytes =
            data_filesystem->capacity_bytes;
        evidence.data_filesystem_available_bytes =
            data_filesystem->available_bytes;
        evidence.required_data_filesystem_available_bytes =
            data_filesystem->required_available_bytes;
    }
    return evidence;
}

void capture_qualification_evidence_if_available(
    benchmark_report& report) {
    const std::vector<postgres_test::host_qualification_report>
        qualifications = postgres_test::host_qualification_reports();
    if (qualifications.size() == 1) {
        report.exact_2gbp_host_qualification =
            qualification_evidence(
                qualifications.front(), report.host_identity);
    }
}

[[nodiscard]] benchmark_sample run_sample(
    libpq::connection& data, libpq::connection& admin,
    std::string_view query, std::chrono::milliseconds timeout,
    std::string phase, std::string_view measurement_cohort,
    std::uint32_t ordinal,
    const postgres_test::sha256_digest& expected_digest,
    std::uint64_t expected_rows) {
    const cache_counters before = read_cache_counters(admin, timeout);
    measured_result measured = measure(data, query, timeout);
    const cache_counters after = read_cache_counters(admin, timeout);
    const auto digest = postgres_test::exact_row_hash(measured.rows);
    if (digest != expected_digest
        || static_cast<std::uint64_t>(measured.rows.rows()) != expected_rows) {
        throw std::runtime_error(
            "benchmark result differs from the disabled oracle");
    }
    return benchmark_sample{
        .phase = std::move(phase),
        .measurement_cohort = std::string(measurement_cohort),
        .ordinal = ordinal,
        .wall_time_ns = measured.wall_time_ns,
        .time_to_first_row_ns = measured.time_to_first_row_ns,
        .rows = expected_rows,
        .correctness_digest = digest.hex(),
        .counter_delta = subtract_counters(after, before)};
}

void require_producer(const benchmark_sample& sample) {
    if (sample.counter_delta.produced != 1
        || sample.counter_delta.completed_hits != 0
        || sample.counter_delta.fallbacks != 0) {
        throw std::runtime_error(
            "cold sample did not produce exactly one cache entry");
    }
}

void require_hit(const benchmark_sample& sample) {
    if (sample.counter_delta.produced != 0
        || sample.counter_delta.completed_hits != 1
        || sample.counter_delta.fallbacks != 0) {
        throw std::runtime_error(
            "warm sample was not exactly one completed cache hit");
    }
}

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> entry_sizes(
    libpq::connection& connection, std::chrono::milliseconds timeout) {
    const libpq::result result = connection.exec(
        "SELECT COALESCE(sum(logical_bytes), 0)::bigint, "
        "COALESCE(sum(stored_bytes), 0)::bigint "
        "FROM bioseq_cache.entries() "
        "WHERE state = 'COMPLETE'",
        timeout);
    if (result.rows() != 1 || result.columns() != 2
        || result.is_null(0, 0) || result.is_null(0, 1)) {
        throw std::runtime_error(
            "bioseq_cache.entries() returned an unexpected size shape");
    }
    const std::int64_t logical = parse_i64(result.value(0, 0));
    const std::int64_t stored = parse_i64(result.value(0, 1));
    if (logical <= 0 || stored <= 0) {
        throw std::runtime_error(
            "completed cache entry has no measured storage");
    }
    return {
        static_cast<std::uint64_t>(logical),
        static_cast<std::uint64_t>(stored)};
}

class benchmark_cleanup_guard {
public:
    benchmark_cleanup_guard(
        postgres_test::postgres_fixture& fixture,
        benchmark_report& report) noexcept
        : fixture_(&fixture),
          report_(&report),
          private_root_(fixture.cluster().root_directory()) {}

    benchmark_cleanup_guard(const benchmark_cleanup_guard&) = delete;
    benchmark_cleanup_guard& operator=(
        const benchmark_cleanup_guard&) = delete;

    ~benchmark_cleanup_guard() noexcept {
        if (!captured_) {
            capture();
        }
    }

    void finish() {
        capture();
        if (!report_->cleanup.succeeded) {
            throw std::runtime_error(
                "private-cluster cleanup was incomplete: "
                + report_->cleanup.detail);
        }
    }

private:
    void capture() noexcept {
        if (captured_) {
            return;
        }
        captured_ = true;
        benchmark_cleanup_evidence evidence{
            .attempted = true,
            .private_root = private_root_};
        try {
            const postgres_test::cluster_cleanup_result& cleanup =
                fixture_->cluster().cleanup();
            std::error_code exists_error;
            const bool root_exists =
                std::filesystem::exists(private_root_, exists_error);
            evidence.private_root_removed =
                !root_exists && !exists_error;
            evidence.succeeded =
                cleanup.succeeded && evidence.private_root_removed;
            evidence.detail = cleanup.detail;
            if (exists_error) {
                evidence.detail +=
                    (evidence.detail.empty() ? "" : "; ")
                    + std::string("cannot verify private root removal: ")
                    + exists_error.message();
            } else if (root_exists) {
                evidence.detail +=
                    (evidence.detail.empty() ? "" : "; ")
                    + std::string("private root remains");
            }
        } catch (const std::exception& error) {
            evidence.detail = error.what();
        } catch (...) {
            evidence.detail =
                "unknown native private-cluster cleanup failure";
        }
        report_->cleanup = std::move(evidence);
    }

    postgres_test::postgres_fixture* fixture_{};
    benchmark_report* report_{};
    std::filesystem::path private_root_;
    bool captured_{};
};

[[nodiscard]] benchmark_report make_base_report(
    scenario selected_scenario, std::string executable,
    std::string_view argument_zero) {
    benchmark_report report{
        .selected_scenario = selected_scenario,
        .executable = std::move(executable),
        .build_identity = build_identity(argument_zero),
        .host_identity = host_identity()};
    report.measurement_cohort = std::string(
        selected_scenario == scenario::bioseq_2gbp
            ? combined_measurement_cohort
            : standalone_measurement_cohort);
    report.exact_2gbp_host_qualification.required =
        selected_scenario == scenario::bioseq_2gbp;
    report.exact_2gbp_host_qualification.status =
        selected_scenario == scenario::bioseq_2gbp
        ? "not-performed"
        : "not-required";
    report.exact_2gbp_host_qualification.evidence_source =
        selected_scenario == scenario::bioseq_2gbp
        ? "not yet performed"
        : "not-required";
    report.exact_2gbp_host_qualification.detail =
        selected_scenario == scenario::bioseq_2gbp
        ? "pre-initdb host qualification has not run"
        : "not required for standalone scenario";
    report.exact_2gbp_host_qualification.host_identity_sha256 =
        report.host_identity.identity_sha256;
    report.migration_evidence_harness =
        selected_scenario == scenario::bioseq_2gbp
        ? "cache-bioseq-bench"
        : "cache-bench";
    report.migration_report_role =
        report.migration_evidence_harness;
    return report;
}

void record_migration_probe(
    benchmark_report& report, std::string_view stable_id,
    std::string_view origin) {
    report.migration_probes.push_back(benchmark_migration_probe{
        .owner = report.migration_evidence_harness,
        .role = report.migration_report_role,
        .stable_id = std::string(stable_id),
        .origin = std::string(origin),
        .permutation =
            std::string(scenario_name(report.selected_scenario)),
        .ordinal = static_cast<std::uint64_t>(
            report.migration_probes.size()),
        .passed = true});
}

[[nodiscard]] sealed_migration_contract seal_migration_contract(
    const std::filesystem::path& path) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(path);
    std::ifstream input(absolute, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot read migration ledger: " + absolute.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad() || !buffer) {
        throw std::runtime_error(
            "failed while sealing migration ledger: "
            + absolute.string());
    }
    std::string raw_document = buffer.str();
    std::istringstream parser{raw_document};
    bioseq::cache_test::migration::ledger parsed =
        bioseq::cache_test::migration::parse_ledger(parser);
    const std::string raw_sha256 =
        postgres_test::sha256(raw_document).hex();
    const std::string evidence_contract_sha256 =
        postgres_test::sha256(
            bioseq::cache_test::migration::
                canonical_evidence_contract_material(parsed))
            .hex();
    const std::string phase{
        bioseq::cache_test::migration::ledger_phase_name(parsed)};
    return sealed_migration_contract{
        .path = absolute,
        .raw_document = std::move(raw_document),
        .parsed = std::move(parsed),
        .raw_sha256 = raw_sha256,
        .evidence_contract_sha256 = evidence_contract_sha256,
        .phase = phase};
}

void attach_migration_contract(
    const sealed_migration_contract& contract,
    benchmark_report& report) {
    namespace migration = bioseq::cache_test::migration;
    report.migration_ledger = contract.path;
    report.migration_ledger_sha256 = contract.raw_sha256;
    report.evidence_contract_sha256 =
        contract.evidence_contract_sha256;
    report.migration_phase = contract.phase;
    for (const migration::ledger_row& row : contract.parsed.rows) {
        switch (row.migration_status) {
        case migration::status::mapped:
            ++report.migration_mapped_rows;
            break;
        case migration::status::passing:
            ++report.migration_passing_rows;
            break;
        case migration::status::retired_with_proof:
            ++report.migration_retired_with_proof_rows;
            break;
        }
    }
}

void validate_migration_evidence(
    const sealed_migration_contract& contract,
    const benchmark_report& report) {
    namespace migration = bioseq::cache_test::migration;
    const migration::evidence_harness owner =
        migration::parse_evidence_harness(
            report.migration_evidence_harness);
    const migration::evidence_role role =
        migration::parse_evidence_role(
            report.migration_report_role);
    if (migration::role_harness(role) != owner) {
        throw std::runtime_error(
            "benchmark migration role does not belong to its harness");
    }

    std::vector<migration::probe_observation> observations;
    observations.reserve(report.migration_probes.size());
    for (std::size_t index = 0;
         index < report.migration_probes.size(); ++index) {
        const benchmark_migration_probe& probe =
            report.migration_probes[index];
        if (probe.owner != report.migration_evidence_harness
            || probe.role != report.migration_report_role) {
            throw std::runtime_error(
                "benchmark migration probe has the wrong harness or role: "
                + probe.stable_id);
        }
        if (!probe.passed || probe.origin_kind != "benchmark-phase"
            || probe.origin.empty() || probe.permutation.empty()
            || probe.ordinal
                != static_cast<std::uint64_t>(index)) {
            throw std::runtime_error(
                "benchmark migration probe has invalid runtime evidence: "
                + probe.stable_id);
        }
        observations.push_back(migration::probe_observation{
            .owner = owner,
            .role = role,
            .stable_id = probe.stable_id,
            .test_case = {},
            .section = {},
            .permutation = probe.permutation,
            .origin_kind = probe.origin_kind,
            .origin = probe.origin,
            .passed = probe.passed});
    }
    migration::validate_report_evidence(
        contract.parsed, owner, role, observations);
}

void require_migration_contract_unchanged(
    const benchmark_report& report) {
    const std::string current_sha256 =
        postgres_test::sha256_file(report.migration_ledger).hex();
    if (current_sha256 != report.migration_ledger_sha256) {
        throw std::runtime_error(
            "migration ledger changed during the native benchmark");
    }
}

void execute_benchmark(
    const benchmark_options& options, benchmark_report& report) {
    report.configured_samples = options.samples;
    report.configured_warmup_samples = options.warmup_samples;
    const sealed_migration_contract migration_contract =
        seal_migration_contract(options.migration_ledger);
    attach_migration_contract(migration_contract, report);
    const bool combined =
        options.selected_scenario == scenario::bioseq_2gbp;
    if (!combined) {
        validate_native_parser_contract();
        record_migration_probe(
            report, "python.benchmark.input_validation",
            "native-parser-rejection-contract");
    }

    postgres_test::clear_host_qualification_reports();
    postgres_test::postgres_fixture fixture{cluster_options(options)};
    benchmark_cleanup_guard cleanup_guard{fixture, report};
    if (combined) {
        capture_qualification_evidence_if_available(report);
        if (!report.exact_2gbp_host_qualification.performed
            || report.exact_2gbp_host_qualification.status != "passed") {
            throw std::runtime_error(
                "bioseq-2gbp benchmark has no passing same-host "
                "pre-initdb qualification");
        }
    }
    report.artifacts = postgres_test::artifact_reports(
        fixture.cluster().staged_artifacts());
    finalize_build_identity(report);
    auto& admin = fixture.admin();
    auto data = fixture.connect(90s);
    report.postgres_server_version = data.server_version();
    benchmark_resource_tracker resource_tracker{
        fixture.cluster().root_directory(),
        fixture.cluster().data_directory(),
        report.resources};
    const std::chrono::milliseconds query_timeout =
        combined ? std::chrono::duration_cast<std::chrono::milliseconds>(30min)
                 : std::chrono::duration_cast<std::chrono::milliseconds>(2min);
    const std::string_view query =
        combined ? bioseq_query : standalone_query;

    if (combined) {
        report.fixture = create_bioseq_fixture(admin);
        configure_cpu_path(data);
    } else {
        create_standalone_fixture(admin);
        report.fixture = fixture_evidence{
            .relation = "cache_benchmark_rows",
            .source_rows = standalone_fixture_rows,
            .query_base_count = 0,
            .target_base_count = 0,
            .aggregate_input_base_count = 0,
            .query = std::string(query)};
    }
    resource_tracker.observe_phase_boundary("fixture-ready");

    execute(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'",
        30s);
    measured_result oracle = measure(data, query, query_timeout);
    if (oracle.rows.rows() <= 0) {
        throw std::runtime_error(
            "disabled benchmark oracle returned no rows");
    }
    const postgres_test::sha256_digest oracle_digest =
        postgres_test::exact_row_hash(oracle.rows);
    const std::uint64_t oracle_rows =
        static_cast<std::uint64_t>(oracle.rows.rows());
    report.correctness_digest = oracle_digest.hex();
    resource_tracker.observe_phase_boundary("disabled-oracle");

    execute(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()",
        30s);
    execute(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "SET bioseq.query_cache_ttl = 3600000",
        30s);
    const cache_counters before_all = read_cache_counters(admin, 30s);

    benchmark_sample producer = run_sample(
        data, admin, query, query_timeout, "producer",
        report.measurement_cohort, 0,
        oracle_digest, oracle_rows);
    require_producer(producer);
    report.cold_producer = std::move(producer);
    resource_tracker.observe_phase_boundary("cold-producer");

    for (std::uint32_t warmup = 0;
         warmup < options.warmup_samples; ++warmup) {
        benchmark_sample discarded = run_sample(
            data, admin, query, query_timeout, "warmup",
            report.measurement_cohort, warmup,
            oracle_digest, oracle_rows);
        require_hit(discarded);
        resource_tracker.observe_phase_boundary("warmup-cache-hit");
    }

    for (std::uint32_t sample = 0; sample < options.samples; ++sample) {
        benchmark_sample warm = run_sample(
            data, admin, query, query_timeout, "warm",
            report.measurement_cohort, sample,
            oracle_digest, oracle_rows);
        require_hit(warm);
        report.samples.push_back(std::move(warm));
        resource_tracker.observe_phase_boundary(
            "measured-warm-cache-hit");
    }
    if (!combined) {
        record_migration_probe(
            report, "python.benchmark.sample_contract",
            "measured-warm-cache-hit-cohort");
        record_migration_probe(
            report, "python.benchmark.correctness_digest",
            "disabled-producer-warm-digest-equivalence");
    }

    const cache_counters after_all = read_cache_counters(admin, 30s);
    report.counter_deltas = subtract_counters(after_all, before_all);
    if (report.counter_deltas.produced != 1
        || report.counter_deltas.completed_hits
            != static_cast<std::int64_t>(
                options.warmup_samples + options.samples)
        || report.counter_deltas.fallbacks != 0) {
        throw std::runtime_error(
            "aggregate cache counters do not match benchmark phases");
    }
    if (!combined) {
        record_migration_probe(
            report, "python.benchmark.counter_deltas",
            "aggregate-cache-counter-deltas");
    }
    const auto [logical_bytes, stored_bytes] =
        entry_sizes(admin, query_timeout);
    report.logical_bytes = logical_bytes;
    report.stored_bytes = stored_bytes;
    report.resources = resource_tracker.finish();
    if (!combined) {
        record_migration_probe(
            report, "python.benchmark.resource_sampling",
            "terminal-zero-failure-resource-sampling");
    }
    validate_migration_evidence(migration_contract, report);
    cleanup_guard.finish();
}

void validate_passed_report(const benchmark_report& report) {
    if (report.status != "passed") {
        return;
    }
    if (!resource_sampling_contract_satisfied(report.resources)) {
        throw std::runtime_error(
            "passed benchmark report has incomplete resource sampling");
    }
    const std::string_view expected_cohort =
        report.selected_scenario == scenario::bioseq_2gbp
        ? combined_measurement_cohort
        : standalone_measurement_cohort;
    if (report.measurement_cohort != expected_cohort) {
        throw std::runtime_error(
            "passed benchmark report has the wrong measurement cohort");
    }
    if (!report.cold_producer
        || report.cold_producer->phase != "producer"
        || report.cold_producer->measurement_cohort
            != report.measurement_cohort
        || report.cold_producer->correctness_digest
            != report.correctness_digest) {
        throw std::runtime_error(
            "cold producer does not match the report contract");
    }
    if (report.configured_samples < 3
        || report.samples.size() != report.configured_samples) {
        throw std::runtime_error(
            "warm percentile population violates its sample contract");
    }
    for (std::size_t index = 0;
         index < report.samples.size(); ++index) {
        const benchmark_sample& sample = report.samples[index];
        if (sample.phase != "warm"
            || sample.ordinal
                != static_cast<std::uint32_t>(index)
            || sample.measurement_cohort
                != report.measurement_cohort
            || sample.correctness_digest
                != report.correctness_digest
            || sample.rows != report.cold_producer->rows) {
            throw std::runtime_error(
                "warm percentile population violates its sample "
                "contract");
        }
    }
    if (!report.cleanup.attempted || !report.cleanup.succeeded
        || !report.cleanup.private_root_removed) {
        throw std::runtime_error(
            "passed benchmark report has no successful native cleanup "
            "verdict");
    }
    const std::string_view expected_owner =
        report.selected_scenario == scenario::bioseq_2gbp
        ? "cache-bioseq-bench"
        : "cache-bench";
    if (report.migration_evidence_harness != expected_owner
        || report.migration_report_role != expected_owner
        || report.migration_coverage != "full-role"
        || report.migration_ledger.empty()
        || report.migration_ledger_sha256.size() != 64
        || report.evidence_contract_sha256.size() != 64
        || (report.migration_phase != "mapped"
            && report.migration_phase != "passing"
            && report.migration_phase != "retired_with_proof"
            && report.migration_phase != "mixed")
        || report.migration_mapped_rows
                + report.migration_passing_rows
                + report.migration_retired_with_proof_rows
            == 0
        || std::any_of(
            report.migration_probes.begin(),
            report.migration_probes.end(),
            [&report](const benchmark_migration_probe& probe) {
                return probe.owner
                        != report.migration_evidence_harness
                    || probe.role != report.migration_report_role
                    || probe.stable_id.empty()
                    || probe.origin_kind != "benchmark-phase"
                    || probe.origin.empty()
                    || probe.permutation.empty()
                    || !probe.passed;
            })) {
        throw std::runtime_error(
            "passed benchmark report has invalid migration evidence");
    }
    if (report.selected_scenario != scenario::bioseq_2gbp) {
        return;
    }
    const exact_2gbp_host_qualification_evidence& qualification =
        report.exact_2gbp_host_qualification;
    if (!qualification.required || !qualification.performed
        || !qualification.same_execution_host
        || qualification.status != "passed"
        || qualification.host_identity_sha256
            != report.host_identity.identity_sha256
        || qualification.required_total_ram_bytes != 0
        || qualification.required_available_ram_bytes != 0
        || qualification.effective_total_ram_bytes == 0
        || qualification.effective_available_ram_bytes == 0
        || qualification.data_filesystem_available_bytes
            < qualification.required_data_filesystem_available_bytes
        || qualification.required_data_filesystem_available_bytes
            < minimum_data_space
        || qualification.data_filesystem_capacity_bytes
            < qualification.data_filesystem_available_bytes
        || !std::isfinite(qualification.effective_cpu_cores)
        || qualification.required_logical_cores < 8
        || qualification.effective_cpu_cores
            < static_cast<double>(
                qualification.required_logical_cores)
        || qualification.io_probe_bytes < io_probe_bytes
        || !std::isfinite(
            qualification.io_write_mib_per_second)
        || qualification.io_write_mib_per_second
            < qualification.required_io_mib_per_second
        || !std::isfinite(
            qualification.io_read_mib_per_second)
        || !std::isfinite(
            qualification.required_io_mib_per_second)
        || qualification.required_io_mib_per_second
            < minimum_io_mib_per_second
        || qualification.io_read_mib_per_second
            < qualification.required_io_mib_per_second) {
        throw std::runtime_error(
            "passed bioseq-2gbp benchmark report has no valid same-host "
            "RAM telemetry plus disk, CPU, and I/O qualification");
    }
    if (report.measurement_cohort != combined_measurement_cohort
        || report.fixture.source_rows != 1
        || report.fixture.query_base_count != query_bases
        || report.fixture.target_base_count != target_bases
        || report.fixture.aggregate_input_base_count
            != total_input_bases) {
        throw std::runtime_error(
            "passed bioseq-2gbp benchmark report has the wrong fixture "
            "or measurement cohort");
    }
}

namespace json {

void string(std::ostream& output, std::string_view value) {
    constexpr std::string_view digits{"0123456789abcdef"};
    output.put('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u00"
                       << digits[(character >> 4U) & 0x0fU]
                       << digits[character & 0x0fU];
            } else {
                output.put(static_cast<char>(character));
            }
        }
    }
    output.put('"');
}

void key(std::ostream& output, std::string_view value) {
    string(output, value);
    output.put(':');
}

void counters(std::ostream& output, const cache_counters& value) {
    output << "{\"produced\":" << value.produced
           << ",\"completed_hits\":" << value.completed_hits
           << ",\"inflight_waits\":" << value.inflight_waits
           << ",\"reuse_hits\":" << value.reuse_hits
           << ",\"fallbacks\":" << value.fallbacks
           << ",\"invalidations\":" << value.invalidations
           << ",\"evictions\":" << value.evictions
           << ",\"logical_bytes\":" << value.logical_bytes
           << ",\"stored_bytes\":" << value.stored_bytes << '}';
}

void sample(std::ostream& output, const benchmark_sample& value) {
    output.put('{');
    key(output, "phase");
    string(output, value.phase);
    output.put(',');
    key(output, "measurement_cohort");
    string(output, value.measurement_cohort);
    output.put(',');
    key(output, "ordinal");
    output << value.ordinal;
    output.put(',');
    key(output, "wall_time_ns");
    output << value.wall_time_ns;
    output.put(',');
    key(output, "time_to_first_row_ns");
    output << value.time_to_first_row_ns;
    output.put(',');
    key(output, "rows");
    output << value.rows;
    output.put(',');
    key(output, "correctness_digest");
    string(output, value.correctness_digest);
    output.put(',');
    key(output, "cache_counter_delta");
    counters(output, value.counter_delta);
    output.put('}');
}

[[nodiscard]] std::uint64_t percentile(
    std::vector<std::uint64_t> values,
    std::uint64_t numerator, std::uint64_t denominator) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::uint64_t rank =
        (numerator * values.size() + denominator - 1U) / denominator;
    return values[static_cast<std::size_t>(
        std::max<std::uint64_t>(rank, 1U) - 1U)];
}

void write(std::ostream& output, const benchmark_report& report) {
    validate_passed_report(report);
    output.put('{');
    key(output, "schema_version");
    string(output, report_schema);
    output.put(',');
    key(output, "metadata");
    output.put('{');
    key(output, "status");
    string(output, report.status);
    output.put(',');
    key(output, "error");
    string(output, report.error);
    output.put(',');
    key(output, "executable");
    string(output, report.executable);
    output.put(',');
    key(output, "build_identity");
    output.put('{');
    key(output, "executable_path");
    string(output, report.build_identity.executable_path.string());
    output.put(',');
    key(output, "executable_sha256");
    string(output, report.build_identity.executable_sha256);
    output.put(',');
    key(output, "compiler");
    string(output, report.build_identity.compiler);
    output.put(',');
    key(output, "compiler_version");
    string(output, report.build_identity.compiler_version);
    output.put(',');
    key(output, "cxx_standard");
    output << report.build_identity.cxx_standard;
    output.put(',');
    key(output, "build_mode");
    string(output, report.build_identity.build_mode);
    output.put(',');
    key(output, "identity_sha256");
    string(output, report.build_identity.identity_sha256);
    output.put('}');
    output.put(',');
    key(output, "host_identity");
    output.put('{');
    key(output, "system_name");
    string(output, report.host_identity.system_name);
    output.put(',');
    key(output, "node_name");
    string(output, report.host_identity.node_name);
    output.put(',');
    key(output, "release");
    string(output, report.host_identity.release);
    output.put(',');
    key(output, "version");
    string(output, report.host_identity.version);
    output.put(',');
    key(output, "machine");
    string(output, report.host_identity.machine);
    output.put(',');
    key(output, "logical_cpu_count");
    output << report.host_identity.logical_cpu_count;
    output.put(',');
    key(output, "page_size_bytes");
    output << report.host_identity.page_size_bytes;
    output.put(',');
    key(output, "identity_sha256");
    string(output, report.host_identity.identity_sha256);
    output.put('}');
    output.put(',');
    key(output, "postgres_server_version");
    string(output, report.postgres_server_version);
    output.put(',');
    key(output, "configured_samples");
    output << report.configured_samples;
    output.put(',');
    key(output, "configured_warmup_samples");
    output << report.configured_warmup_samples;
    output.put(',');
    key(output, "first_row_measurement");
    string(
        output,
        "PQsetSingleRowMode first PGRES_SINGLE_TUPLE arrival");
    output.put(',');
    key(output, "resource_sampling");
    output.put('{');
    key(output, "measurement_window");
    string(
        output,
        "from the running private-cluster fixture before workload setup "
        "through the final measured cache-hit phase; excludes initdb, "
        "server startup, shutdown, and cleanup");
    output.put(',');
    key(output, "rss_scope");
    string(
        output,
        "sum of current RSS for the owned private PostgreSQL postmaster "
        "and all descendant processes");
    output.put(',');
    key(output, "rss_sample_interval_ms");
    output << rss_sample_interval.count();
    output.put(',');
    key(output, "rss_samples");
    output << report.resources.rss_samples;
    output.put(',');
    key(output, "rss_sampling_failures");
    output << report.resources.rss_sampling_failures;
    output.put(',');
    key(output, "temp_scope");
    string(
        output,
        "all regular-file bytes below the owned private-cluster root");
    output.put(',');
    key(output, "temp_sample_interval_ms");
    output << temp_sample_interval.count();
    output.put(',');
    key(output, "temp_samples");
    output << report.resources.temp_samples;
    output.put(',');
    key(output, "temp_sampling_failures");
    output << report.resources.temp_sampling_failures;
    output.put('}');
    output.put(',');
    key(output, "artifacts");
    output.put('[');
    for (std::size_t index = 0; index < report.artifacts.size(); ++index) {
        if (index != 0) {
            output.put(',');
        }
        const postgres_test::artifact_report& artifact =
            report.artifacts[index];
        output.put('{');
        key(output, "name");
        string(output, artifact.name);
        output.put(',');
        key(output, "preinstalled_stage_root");
        string(output, artifact.preinstalled_stage_root.string());
        output.put(',');
        key(output, "stage_manifest");
        string(output, artifact.stage_manifest.string());
        output.put(',');
        key(output, "stage_manifest_sha256");
        string(output, artifact.stage_manifest_sha256);
        output.put(',');
        key(output, "control_file");
        string(output, artifact.control_file.string());
        output.put(',');
        key(output, "install_sql");
        string(output, artifact.install_sql.string());
        output.put(',');
        key(output, "dso");
        string(output, artifact.dso.string());
        output.put(',');
        key(output, "staged_control_file");
        string(output, artifact.staged_control_file.string());
        output.put(',');
        key(output, "staged_install_sql");
        string(output, artifact.staged_install_sql.string());
        output.put(',');
        key(output, "staged_dso");
        string(output, artifact.staged_dso.string());
        output.put(',');
        key(output, "control_sha256");
        string(output, artifact.control_sha256);
        output.put(',');
        key(output, "install_sql_sha256");
        string(output, artifact.install_sql_sha256);
        output.put(',');
        key(output, "dso_sha256");
        string(output, artifact.dso_sha256);
        output.put(',');
        key(output, "staged_control_sha256");
        string(output, artifact.staged_control_sha256);
        output.put('}');
    }
    output << "]}";
    output.put(',');
    key(output, "scenario");
    string(output, scenario_name(report.selected_scenario));
    output.put(',');
    key(output, "measurement_cohort");
    string(output, report.measurement_cohort);
    output.put(',');
    key(output, "exact_2gbp_host_qualification");
    output.put('{');
    const exact_2gbp_host_qualification_evidence& qualification =
        report.exact_2gbp_host_qualification;
    key(output, "required");
    output << (qualification.required ? "true" : "false");
    output.put(',');
    key(output, "performed");
    output << (qualification.performed ? "true" : "false");
    output.put(',');
    key(output, "same_execution_host");
    output << (qualification.same_execution_host ? "true" : "false");
    output.put(',');
    key(output, "status");
    string(output, qualification.status);
    output.put(',');
    key(output, "evidence_source");
    string(output, qualification.evidence_source);
    output.put(',');
    key(output, "detail");
    string(output, qualification.detail);
    output.put(',');
    key(output, "host_identity_sha256");
    string(output, qualification.host_identity_sha256);
    output.put(',');
    key(output, "effective_total_ram_bytes");
    output << qualification.effective_total_ram_bytes;
    output.put(',');
    key(output, "effective_available_ram_bytes");
    output << qualification.effective_available_ram_bytes;
    output.put(',');
    key(output, "required_total_ram_bytes");
    output << qualification.required_total_ram_bytes;
    output.put(',');
    key(output, "required_available_ram_bytes");
    output << qualification.required_available_ram_bytes;
    output.put(',');
    key(output, "data_filesystem_capacity_bytes");
    output << qualification.data_filesystem_capacity_bytes;
    output.put(',');
    key(output, "data_filesystem_available_bytes");
    output << qualification.data_filesystem_available_bytes;
    output.put(',');
    key(output, "required_data_filesystem_available_bytes");
    output << qualification.required_data_filesystem_available_bytes;
    output.put(',');
    key(output, "effective_cpu_cores");
    output << qualification.effective_cpu_cores;
    output.put(',');
    key(output, "required_logical_cores");
    output << qualification.required_logical_cores;
    output.put(',');
    key(output, "io_probe_bytes");
    output << qualification.io_probe_bytes;
    output.put(',');
    key(output, "io_write_mib_per_second");
    output << qualification.io_write_mib_per_second;
    output.put(',');
    key(output, "io_read_mib_per_second");
    output << qualification.io_read_mib_per_second;
    output.put(',');
    key(output, "required_io_mib_per_second");
    output << qualification.required_io_mib_per_second;
    output.put('}');
    output.put(',');
    key(output, "migration_evidence");
    output.put('{');
    key(output, "migration_evidence_harness");
    string(output, report.migration_evidence_harness);
    output.put(',');
    key(output, "migration_report_role");
    string(output, report.migration_report_role);
    output.put(',');
    key(output, "coverage");
    string(output, report.migration_coverage);
    output.put(',');
    key(output, "ledger");
    string(output, report.migration_ledger.string());
    output.put(',');
    key(output, "ledger_sha256");
    string(output, report.migration_ledger_sha256);
    output.put(',');
    key(output, "evidence_contract_sha256");
    string(output, report.evidence_contract_sha256);
    output.put(',');
    key(output, "migration_phase");
    string(output, report.migration_phase);
    output.put(',');
    key(output, "mapped_rows");
    output << report.migration_mapped_rows;
    output.put(',');
    key(output, "passing_rows");
    output << report.migration_passing_rows;
    output.put(',');
    key(output, "retired_with_proof_rows");
    output << report.migration_retired_with_proof_rows;
    output.put(',');
    key(output, "probes");
    output.put('[');
    for (std::size_t index = 0;
         index < report.migration_probes.size(); ++index) {
        if (index != 0) {
            output.put(',');
        }
        const benchmark_migration_probe& probe =
            report.migration_probes[index];
        output.put('{');
        key(output, "owner");
        string(output, probe.owner);
        output.put(',');
        key(output, "role");
        string(output, probe.role);
        output.put(',');
        key(output, "stable_id");
        string(output, probe.stable_id);
        output.put(',');
        key(output, "origin_kind");
        string(output, probe.origin_kind);
        output.put(',');
        key(output, "origin");
        string(output, probe.origin);
        output.put(',');
        key(output, "permutation");
        string(output, probe.permutation);
        output.put(',');
        key(output, "ordinal");
        output << probe.ordinal;
        output.put(',');
        key(output, "passed");
        output << (probe.passed ? "true" : "false");
        output.put('}');
    }
    output.put(']');
    output.put('}');
    output.put(',');
    key(output, "fixture");
    output.put('{');
    key(output, "relation");
    string(output, report.fixture.relation);
    output.put(',');
    key(output, "source_rows");
    output << report.fixture.source_rows;
    output.put(',');
    key(output, "query_bases");
    output << report.fixture.query_base_count;
    output.put(',');
    key(output, "target_bases");
    output << report.fixture.target_base_count;
    output.put(',');
    key(output, "aggregate_input_bases");
    output << report.fixture.aggregate_input_base_count;
    output.put(',');
    key(output, "query");
    string(output, report.fixture.query);
    output.put('}');
    output.put(',');
    key(output, "correctness_digest");
    string(output, report.correctness_digest);
    output.put(',');
    key(output, "cold_producer");
    if (report.cold_producer) {
        sample(output, *report.cold_producer);
    } else {
        output << "null";
    }
    output.put(',');
    key(output, "samples");
    output.put('[');
    std::vector<std::uint64_t> wall_samples;
    wall_samples.reserve(report.samples.size());
    for (std::size_t index = 0; index < report.samples.size(); ++index) {
        if (index != 0) {
            output.put(',');
        }
        const benchmark_sample& warm_sample = report.samples[index];
        wall_samples.push_back(warm_sample.wall_time_ns);
        sample(output, warm_sample);
    }
    output.put(']');
    output.put(',');
    key(output, "summary");
    output.put('{');
    key(output, "population");
    string(output, "warm samples only");
    output.put(',');
    key(output, "measurement_cohort");
    string(output, report.measurement_cohort);
    output.put(',');
    key(output, "sample_count");
    output << wall_samples.size();
    output.put(',');
    key(output, "p50");
    output << percentile(wall_samples, 50, 100);
    output.put(',');
    key(output, "p95");
    output << percentile(wall_samples, 95, 100);
    output.put(',');
    key(output, "min");
    output << (wall_samples.empty()
                   ? 0
                   : *std::min_element(
                         wall_samples.begin(), wall_samples.end()));
    output.put(',');
    key(output, "max");
    output << (wall_samples.empty()
                   ? 0
                   : *std::max_element(
                         wall_samples.begin(), wall_samples.end()));
    output.put('}');
    output.put(',');
    key(output, "cache_counter_deltas");
    counters(output, report.counter_deltas);
    output.put(',');
    key(output, "logical_bytes");
    output << report.logical_bytes;
    output.put(',');
    key(output, "stored_bytes");
    output << report.stored_bytes;
    output.put(',');
    key(output, "peak_rss_bytes");
    output << report.resources.peak_postgres_rss_bytes;
    output.put(',');
    key(output, "peak_temp_bytes");
    output << report.resources.peak_temp_bytes;
    output.put(',');
    key(output, "cleanup");
    output.put('{');
    key(output, "attempted");
    output << (report.cleanup.attempted ? "true" : "false");
    output.put(',');
    key(output, "succeeded");
    output << (report.cleanup.succeeded ? "true" : "false");
    output.put(',');
    key(output, "private_root_removed");
    output << (report.cleanup.private_root_removed ? "true" : "false");
    output.put(',');
    key(output, "private_root");
    string(output, report.cleanup.private_root.string());
    output.put(',');
    key(output, "detail");
    string(output, report.cleanup.detail);
    output.put(',');
    key(output, "verdict_source");
    string(output, report.cleanup.verdict_source);
    output.put(',');
    key(output, "external_watchdog_scope");
    string(output, report.cleanup.external_watchdog_scope);
    output.put('}');
    output.put('}');
}

}  // namespace json

[[nodiscard]] std::string render_contract_probe(
    bool passed, std::filesystem::path preinstalled_stage_root,
    std::filesystem::path stage_manifest,
    std::string stage_manifest_sha256,
    scenario selected_scenario = scenario::standalone,
    bool inject_mixed_cohort = false) {
    const std::string digest(64, 'a');
    const bool combined =
        selected_scenario == scenario::bioseq_2gbp;
    const std::string cohort{
        combined ? combined_measurement_cohort
                 : standalone_measurement_cohort};
    benchmark_report report{
        .status = passed ? "passed" : "failed",
        .error = passed ? "" : "contract probe failure",
        .selected_scenario = selected_scenario,
        .executable = "bioseq_cache_report_contract_probe",
        .build_identity =
            build_identity_evidence{
                .executable_path = "/build/cache-benchmark-probe",
                .executable_sha256 = digest,
                .compiler = "contract-compiler",
                .compiler_version = "1.0",
                .cxx_standard = 202302,
                .build_mode = "ndebug",
                .identity_sha256 = {}},
        .host_identity =
            host_identity_evidence{
                .system_name = "contract-system",
                .node_name = "contract-node",
                .release = "contract-release",
                .version = "contract-version",
                .machine = "contract-machine",
                .logical_cpu_count = 8,
                .page_size_bytes = 4096,
                .identity_sha256 = digest},
        .postgres_server_version = "PostgreSQL contract",
        .configured_samples = 3,
        .configured_warmup_samples = 1,
        .measurement_cohort = cohort,
        .exact_2gbp_host_qualification =
            exact_2gbp_host_qualification_evidence{
                .required = combined,
                .performed = combined,
                .same_execution_host = combined,
                .status = combined ? "passed" : "not-required",
                .evidence_source =
                    combined
                    ? "contract probe for same benchmark process"
                    : "not-required",
                .detail =
                    combined
                    ? "contract qualification passed"
                    : "not required for standalone scenario",
                .host_identity_sha256 = digest,
                .effective_total_ram_bytes =
                    combined ? 32ULL * gibibyte : 0,
                .effective_available_ram_bytes =
                    combined ? 16ULL * gibibyte : 0,
                .required_total_ram_bytes =
                    0,
                .required_available_ram_bytes =
                    0,
                .data_filesystem_capacity_bytes =
                    combined ? 100ULL * gibibyte : 0,
                .data_filesystem_available_bytes =
                    combined ? 64ULL * gibibyte : 0,
                .required_data_filesystem_available_bytes =
                    combined ? minimum_data_space : 0,
                .effective_cpu_cores = combined ? 16.0 : 0.0,
                .required_logical_cores = combined ? 8U : 0U,
                .io_probe_bytes = combined ? io_probe_bytes : 0,
                .io_write_mib_per_second =
                    combined ? 200.0 : 0.0,
                .io_read_mib_per_second =
                    combined ? 200.0 : 0.0,
                .required_io_mib_per_second =
                    combined ? minimum_io_mib_per_second : 0.0},
        .fixture =
            fixture_evidence{
                .relation =
                    combined ? "cache_bioseq_2gbp"
                             : "contract_fixture",
                .source_rows = combined ? 1ULL : 4ULL,
                .query_base_count =
                    combined ? query_bases : 0,
                .target_base_count =
                    combined ? target_bases : 0,
                .aggregate_input_base_count =
                    combined ? total_input_bases : 0,
                .query = "SELECT contract"},
        .correctness_digest = passed ? digest : "",
        .resources =
            benchmark_resource_report{
                .peak_postgres_rss_bytes = 8192,
                .peak_temp_bytes = 16384,
                .rss_samples = 4,
                .temp_samples = 3},
        .cleanup =
            benchmark_cleanup_evidence{
                .attempted = passed,
                .succeeded = passed,
                .private_root_removed = passed,
                .private_root = "/tmp/contract-private-root",
                .detail =
                    passed
                    ? "postmaster stopped and private root removed"
                    : "contract failure before cleanup"}};
    report.migration_evidence_harness =
        combined ? "cache-bioseq-bench" : "cache-bench";
    report.migration_report_role =
        report.migration_evidence_harness;
    if (passed) {
        report.migration_ledger =
            "/source/legacy_migration_ledger.tsv";
        report.migration_ledger_sha256 = std::string(64, 'd');
        report.evidence_contract_sha256 = std::string(64, 'e');
        report.migration_phase = "mapped";
        report.migration_mapped_rows = 5;
    }
    if (passed && !combined) {
        record_migration_probe(
            report, "python.benchmark.input_validation",
            "native-parser-rejection-contract");
        record_migration_probe(
            report, "python.benchmark.sample_contract",
            "measured-warm-cache-hit-cohort");
        record_migration_probe(
            report, "python.benchmark.correctness_digest",
            "disabled-producer-warm-digest-equivalence");
        record_migration_probe(
            report, "python.benchmark.counter_deltas",
            "aggregate-cache-counter-deltas");
        record_migration_probe(
            report, "python.benchmark.resource_sampling",
            "terminal-zero-failure-resource-sampling");
    }
    report.artifacts.push_back(postgres_test::artifact_report{
        .name = "bioseq_cache",
        .preinstalled_stage_root = std::move(preinstalled_stage_root),
        .stage_manifest = std::move(stage_manifest),
        .stage_manifest_sha256 = std::move(stage_manifest_sha256),
        .control_file = "/source/bioseq_cache.control",
        .install_sql = "/source/bioseq_cache--1.0.sql",
        .dso = "/source/bioseq_cache.so",
        .staged_control_file = "/stage/bioseq_cache.control",
        .staged_install_sql = "/stage/bioseq_cache--1.0.sql",
        .staged_dso = "/stage/bioseq_cache.so",
        .control_sha256 = digest,
        .install_sql_sha256 = digest,
        .dso_sha256 = digest,
        .staged_control_sha256 = digest});
    if (passed) {
        report.cold_producer = benchmark_sample{
            .phase = "producer",
            .measurement_cohort = cohort,
            .ordinal = 0,
            .wall_time_ns = 999,
            .time_to_first_row_ns = 500,
            .rows = 4,
            .correctness_digest = digest,
            .counter_delta = cache_counters{.produced = 1}};
        for (const std::uint64_t wall_time : {30U, 10U, 20U}) {
            report.samples.push_back(benchmark_sample{
                .phase = "warm",
                .measurement_cohort = cohort,
                .ordinal =
                    static_cast<std::uint32_t>(report.samples.size()),
                .wall_time_ns = wall_time,
                .time_to_first_row_ns = wall_time / 2U,
                .rows = 4,
                .correctness_digest = digest,
                .counter_delta =
                    cache_counters{.completed_hits = 1}});
        }
        report.counter_deltas = cache_counters{
            .produced = 1,
            .completed_hits = 4};
        report.logical_bytes = 2048;
        report.stored_bytes = 1024;
        if (inject_mixed_cohort) {
            report.samples.back().measurement_cohort =
                "bioseq-cpu-column-cache-off";
        }
    }
    finalize_build_identity(report);
    std::ostringstream output;
    json::write(output, report);
    return output.str();
}

void write_report(
    const std::filesystem::path& path,
    const benchmark_report& report) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(path);
    const auto directories_created =
        bioseq::durable_file_io::durable_create_directories(
            absolute.parent_path());
    if (!directories_created.has_value()) {
        const bioseq::durable_file_io::file_io_error& error =
            directories_created.error();
        throw std::runtime_error(
            "cannot durably create benchmark report directory: operation "
            + std::to_string(static_cast<unsigned>(error.operation))
            + ", code "
            + std::to_string(static_cast<unsigned>(error.code))
            + ", system error "
            + std::to_string(error.system_error)
            + ", publication "
            + std::to_string(static_cast<unsigned>(error.publication)));
    }
    std::ostringstream output;
    json::write(output, report);
    output.put('\n');
    if (!output) {
        throw std::runtime_error(
            "cannot finish benchmark report");
    }
    const std::string serialized = std::move(output).str();
    const auto published =
        bioseq::durable_file_io::durable_replace_file(
            absolute,
            std::as_bytes(std::span{
                serialized.data(), serialized.size()}),
            bioseq::durable_file_io::durable_publish_options{
                .mode = 0666});  // Preserve ofstream mode before umask.
    if (!published.has_value()) {
        const bioseq::durable_file_io::file_io_error& error =
            published.error();
        throw std::runtime_error(
            "cannot durably publish benchmark report: operation "
            + std::to_string(static_cast<unsigned>(error.operation))
            + ", code "
            + std::to_string(static_cast<unsigned>(error.code))
            + ", system error "
            + std::to_string(error.system_error)
            + ", publication "
            + std::to_string(static_cast<unsigned>(error.publication)));
    }
    if (published->publication
        != bioseq::durable_file_io::publication_state::durable) {
        throw std::runtime_error(
            "benchmark report publication did not become durable");
    }
}

void write_failure_report(
    const std::optional<std::filesystem::path>& path,
    std::optional<benchmark_report>& report,
    scenario selected_scenario, const std::string& executable,
    std::string_view argument_zero, std::string_view error) noexcept {
    if (!path) {
        return;
    }
    try {
        if (!report) {
            report.emplace(make_base_report(
                selected_scenario, executable, argument_zero));
        }
        if (selected_scenario == scenario::bioseq_2gbp
            && !report->exact_2gbp_host_qualification.performed) {
            capture_qualification_evidence_if_available(*report);
        }
        report->status = "failed";
        report->error = error;
        finalize_build_identity(*report);
        write_report(*path, *report);
    } catch (...) {
        // The process still fails closed when its diagnostic report cannot
        // be published.
    }
}

}  // namespace bioseq::cache_benchmark

export namespace bioseq::cache_benchmark {

/// @brief Renders deterministic success/failure documents for schema tests.
[[nodiscard]] std::string render_report_contract_probe(bool passed) {
    return render_contract_probe(
        passed, "/stage", "/stage/stage-manifest.json",
        std::string(64, 'b'));
}

/// @brief Renders a passing combined probe with exact 2 Gbp qualification.
[[nodiscard]] std::string render_bioseq_report_contract_probe() {
    return render_contract_probe(
        true, "/stage", "/stage/stage-manifest.json",
        std::string(64, 'b'), scenario::bioseq_2gbp);
}

/// @brief Exercises the fail-closed mixed-cohort report validator.
[[nodiscard]] std::string render_mixed_cohort_contract_probe() {
    return render_contract_probe(
        true, "/stage", "/stage/stage-manifest.json",
        std::string(64, 'b'), scenario::bioseq_2gbp, true);
}

/// @brief Pure resource-sampling acceptance predicate used by contract tests.
[[nodiscard]] bool resource_sampling_contract_satisfied(
    std::uint64_t rss_samples,
    std::uint64_t rss_sampling_failures,
    std::uint64_t peak_rss_bytes,
    std::uint64_t temp_samples,
    std::uint64_t temp_sampling_failures,
    std::uint64_t peak_temp_bytes) noexcept {
    return bioseq::cache_benchmark::
        resource_sampling_contract_satisfied(
            benchmark_resource_report{
                .peak_postgres_rss_bytes = peak_rss_bytes,
                .peak_temp_bytes = peak_temp_bytes,
                .rss_samples = rss_samples,
                .temp_samples = temp_samples,
                .rss_sampling_failures = rss_sampling_failures,
                .temp_sampling_failures = temp_sampling_failures});
}

/// @brief Runs all deterministic native parser rejection checks.
[[nodiscard]] bool native_parser_contract_satisfied() {
    validate_native_parser_contract();
    return true;
}

/// @brief Renders a probe with caller-selected stage provenance.
///
/// This overload exists so contract tests can prove that the stage-manifest
/// digest participates in build identity while host-dependent stage paths do
/// not.
[[nodiscard]] std::string
render_report_contract_probe_with_stage_provenance(
    bool passed, std::string_view preinstalled_stage_root,
    std::string_view stage_manifest,
    std::string_view stage_manifest_sha256) {
    return render_contract_probe(
        passed, std::string(preinstalled_stage_root),
        std::string(stage_manifest), std::string(stage_manifest_sha256));
}

/// @brief Parse, execute, validate, and report one fixed native benchmark.
int run(int argc, char** argv, scenario required_scenario) noexcept {
    const std::optional<std::filesystem::path> report_hint =
        report_path_hint(argc, argv);
    const std::string_view argument_zero =
        argc > 0 && argv != nullptr && argv[0] != nullptr
            ? std::string_view(argv[0])
            : std::string_view{};
    const std::string executable =
        required_scenario == scenario::standalone
            ? "bioseq_cache_bench"
            : "bioseq_cache_bioseq_bench";
    std::optional<benchmark_report> report;
    try {
        report.emplace(make_base_report(
            required_scenario, executable, argument_zero));
        const benchmark_options options =
            parse_options(argc, argv, required_scenario);
        execute_benchmark(options, *report);
        require_migration_contract_unchanged(*report);
        write_report(options.report, *report);
        require_migration_contract_unchanged(*report);
        return 0;
    } catch (const std::exception& error) {
        write_failure_report(
            report_hint, report, required_scenario, executable,
            argument_zero, error.what());
        return 1;
    } catch (...) {
        write_failure_report(
            report_hint, report, required_scenario, executable,
            argument_zero, "unknown benchmark failure");
        return 1;
    }
}

}  // namespace bioseq::cache_benchmark
