module;

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.cache.benchmark;

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

constexpr std::uint64_t reference_bases = 2'000'000'000ULL;
constexpr std::uint64_t standalone_fixture_rows = 20'000ULL;
constexpr std::string_view report_schema{"bioseq-cache-benchmark-v1"};
constexpr std::string_view standalone_query{
    "SELECT id, group_id, optional_value, payload "
    "FROM cache_benchmark_rows "
    "ORDER BY group_id, id, optional_value, payload"};
constexpr std::string_view bioseq_query{
    "SELECT id, "
    "bioseq.target_start(id), "
    "bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), "
    "bioseq.query_end(id), "
    "bioseq.query_len(id), "
    "bioseq.is_reverse(id) "
    "FROM cache_bioseq_2gbp "
    "WHERE mmsearch(target, query) "
    "ORDER BY id, "
    "bioseq.target_start(id), "
    "bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), "
    "bioseq.query_end(id), "
    "bioseq.query_len(id), "
    "bioseq.is_reverse(id)"};

struct benchmark_options {
    scenario selected_scenario{scenario::standalone};
    std::filesystem::path postgres_bin_dir;
    std::filesystem::path cache_control;
    std::filesystem::path cache_install_sql;
    std::filesystem::path cache_dso;
    std::optional<std::filesystem::path> bioseq_control;
    std::optional<std::filesystem::path> bioseq_install_sql;
    std::optional<std::filesystem::path> bioseq_dso;
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
    std::uint64_t reference_base_count{};
    std::string query;
};

struct benchmark_report {
    std::string status{"passed"};
    std::string error;
    scenario selected_scenario{scenario::standalone};
    std::string executable;
    std::string postgres_server_version;
    std::uint32_t configured_samples{};
    std::uint32_t configured_warmup_samples{};
    fixture_evidence fixture;
    std::string correctness_digest;
    std::vector<benchmark_sample> samples;
    cache_counters counter_deltas;
    std::uint64_t logical_bytes{};
    std::uint64_t stored_bytes{};
    postgres_test::resource_report resources;
    std::vector<postgres_test::artifact_report> artifacts;
};

struct measured_result {
    libpq::result rows;
    std::uint64_t wall_time_ns{};
    std::uint64_t time_to_first_row_ns{};
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
             {options.cache_control, "--cache-control"},
             {options.cache_install_sql, "--cache-install-sql"},
             {options.cache_dso, "--cache-dso"},
             {options.report, "--report"}}) {
        if (path.empty()) {
            throw std::invalid_argument(
                "required benchmark option is missing: "
                + std::string(name));
        }
    }
    if (required_scenario == scenario::bioseq_2gbp) {
        if (!options.bioseq_control || !options.bioseq_install_sql
            || !options.bioseq_dso) {
            throw std::invalid_argument(
                "bioseq-2gbp requires all Bioseq artifact paths");
        }
        if (options.samples < 3) {
            throw std::invalid_argument(
                "bioseq-2gbp requires at least three measured warm samples");
        }
    }
    return options;
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

void create_bioseq_fixture(libpq::connection& connection) {
    configure_cpu_path(connection);
    execute(
        connection,
        "SET bioseq.update_index_on_insert = off; "
        "SELECT bioseq.create_query_target_sample_table("
        "'cache_bioseq_2gbp', 1, 5000000, 100000.0, 2000000000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.5, 3219876)",
        40min);
    execute(
        connection,
        "SELECT bioseq.update_index("
        "'cache_bioseq_2gbp'::regclass, 'minimizers_per_fragment'); "
        "SELECT bioseq.set_active_read_index("
        "'cache_bioseq_2gbp'::regclass, 'minimizers_per_fragment')",
        40min);
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
    return postgres_test::cluster_options{
        .postgres_bin_dir = options.postgres_bin_dir,
        .extensions = extension_artifacts(options),
        .preload_libraries = std::move(preloads),
        .startup_gucs = {
            {"max_parallel_workers", "0"},
            {"max_parallel_workers_per_gather", "0"}},
        .startup_timeout = 90s,
        .stop_timeout = 30s};
}

[[nodiscard]] benchmark_sample run_sample(
    libpq::connection& data, libpq::connection& admin,
    std::string_view query, std::chrono::milliseconds timeout,
    std::string phase, std::uint32_t ordinal,
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

[[nodiscard]] benchmark_report execute_benchmark(
    const benchmark_options& options, std::string executable) {
    benchmark_report report{
        .selected_scenario = options.selected_scenario,
        .executable = std::move(executable),
        .configured_samples = options.samples,
        .configured_warmup_samples = options.warmup_samples};

    postgres_test::postgres_fixture fixture{cluster_options(options)};
    auto& admin = fixture.admin();
    auto data = fixture.connect(90s);
    const bool combined =
        options.selected_scenario == scenario::bioseq_2gbp;
    const std::chrono::milliseconds query_timeout =
        combined ? std::chrono::duration_cast<std::chrono::milliseconds>(30min)
                 : std::chrono::duration_cast<std::chrono::milliseconds>(2min);
    const std::string_view query =
        combined ? bioseq_query : standalone_query;

    if (combined) {
        create_bioseq_fixture(admin);
        configure_cpu_path(data);
        report.fixture = fixture_evidence{
            .relation = "cache_bioseq_2gbp",
            .source_rows = 1,
            .reference_base_count = reference_bases,
            .query = std::string(query)};
    } else {
        create_standalone_fixture(admin);
        report.fixture = fixture_evidence{
            .relation = "cache_benchmark_rows",
            .source_rows = standalone_fixture_rows,
            .reference_base_count = 0,
            .query = std::string(query)};
    }

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
        data, admin, query, query_timeout, "producer", 0,
        oracle_digest, oracle_rows);
    require_producer(producer);
    report.samples.push_back(std::move(producer));

    for (std::uint32_t warmup = 0;
         warmup < options.warmup_samples; ++warmup) {
        benchmark_sample discarded = run_sample(
            data, admin, query, query_timeout, "warmup", warmup,
            oracle_digest, oracle_rows);
        require_hit(discarded);
    }

    for (std::uint32_t sample = 0; sample < options.samples; ++sample) {
        benchmark_sample warm = run_sample(
            data, admin, query, query_timeout, "warm", sample,
            oracle_digest, oracle_rows);
        require_hit(warm);
        report.samples.push_back(std::move(warm));
        if (combined && sample == 0) {
            execute(
                data, "SET bioseq.enable_column_cache = off", 30s);
        }
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
    const auto [logical_bytes, stored_bytes] =
        entry_sizes(admin, query_timeout);
    report.logical_bytes = logical_bytes;
    report.stored_bytes = stored_bytes;
    report.postgres_server_version = data.server_version();
    report.artifacts = postgres_test::artifact_reports(
        fixture.cluster().staged_artifacts());
    const std::filesystem::path temp_root =
        fixture.cluster().root_directory();
    report.resources =
        postgres_test::capture_resource_report(
            std::span<const std::filesystem::path>(&temp_root, 1));
    return report;
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
        key(output, "control_file");
        string(output, artifact.control_file.string());
        output.put(',');
        key(output, "install_sql");
        string(output, artifact.install_sql.string());
        output.put(',');
        key(output, "dso");
        string(output, artifact.dso.string());
        output.put(',');
        key(output, "control_sha256");
        string(output, artifact.control_sha256);
        output.put(',');
        key(output, "install_sql_sha256");
        string(output, artifact.install_sql_sha256);
        output.put(',');
        key(output, "dso_sha256");
        string(output, artifact.dso_sha256);
        output.put('}');
    }
    output << "]}";
    output.put(',');
    key(output, "scenario");
    string(output, scenario_name(report.selected_scenario));
    output.put(',');
    key(output, "fixture");
    output.put('{');
    key(output, "relation");
    string(output, report.fixture.relation);
    output.put(',');
    key(output, "source_rows");
    output << report.fixture.source_rows;
    output.put(',');
    key(output, "reference_bases");
    output << report.fixture.reference_base_count;
    output.put(',');
    key(output, "query");
    string(output, report.fixture.query);
    output.put('}');
    output.put(',');
    key(output, "correctness_digest");
    string(output, report.correctness_digest);
    output.put(',');
    key(output, "samples");
    output.put('[');
    std::vector<std::uint64_t> wall_samples;
    wall_samples.reserve(report.samples.size());
    for (std::size_t index = 0; index < report.samples.size(); ++index) {
        if (index != 0) {
            output.put(',');
        }
        const benchmark_sample& sample = report.samples[index];
        wall_samples.push_back(sample.wall_time_ns);
        output.put('{');
        key(output, "phase");
        string(output, sample.phase);
        output.put(',');
        key(output, "ordinal");
        output << sample.ordinal;
        output.put(',');
        key(output, "wall_time_ns");
        output << sample.wall_time_ns;
        output.put(',');
        key(output, "time_to_first_row_ns");
        output << sample.time_to_first_row_ns;
        output.put(',');
        key(output, "rows");
        output << sample.rows;
        output.put(',');
        key(output, "correctness_digest");
        string(output, sample.correctness_digest);
        output.put(',');
        key(output, "cache_counter_delta");
        counters(output, sample.counter_delta);
        output.put('}');
    }
    output.put(']');
    output.put(',');
    key(output, "summary");
    output.put('{');
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
    output << report.resources.peak_rss_bytes;
    output.put(',');
    key(output, "peak_temp_bytes");
    output << report.resources.observed_temp_bytes;
    output.put('}');
}

}  // namespace json

void write_report(
    const std::filesystem::path& path,
    const benchmark_report& report) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(path);
    std::filesystem::create_directories(absolute.parent_path());
    const std::filesystem::path temporary =
        absolute.parent_path()
        / (absolute.filename().string() + ".partial");
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "cannot create benchmark report");
    }
    json::write(output, report);
    output.put('\n');
    output.close();
    if (!output) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        throw std::runtime_error(
            "cannot finish benchmark report");
    }
    std::error_code error;
    std::filesystem::rename(temporary, absolute, error);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        throw std::runtime_error(
            "cannot publish benchmark report: " + error.message());
    }
}

}  // namespace bioseq::cache_benchmark

export namespace bioseq::cache_benchmark {

/// @brief Parse, execute, validate, and report one fixed native benchmark.
int run(int argc, char** argv, scenario required_scenario) noexcept {
    const std::optional<std::filesystem::path> report_hint =
        report_path_hint(argc, argv);
    try {
        const benchmark_options options =
            parse_options(argc, argv, required_scenario);
        benchmark_report report = execute_benchmark(
            options,
            required_scenario == scenario::standalone
                ? "bioseq_cache_bench"
                : "bioseq_cache_bioseq_bench");
        write_report(options.report, report);
        return 0;
    } catch (const std::exception& error) {
        if (report_hint) {
            try {
                benchmark_report report{
                    .status = "failed",
                    .error = error.what(),
                    .selected_scenario = required_scenario,
                    .executable =
                        required_scenario == scenario::standalone
                            ? "bioseq_cache_bench"
                            : "bioseq_cache_bioseq_bench"};
                write_report(*report_hint, report);
            } catch (...) {
                // The process still fails closed when its diagnostic report
                // cannot be published.
            }
        }
        return 1;
    } catch (...) {
        return 1;
    }
}

}  // namespace bioseq::cache_benchmark
