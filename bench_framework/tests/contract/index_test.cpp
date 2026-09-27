/**
 * @file bench_framework/tests/contract/index_test.cpp
 * @brief Contract tests for the benchmark runner, reports and comparison rules.
 *
 * Fixtures write configuration files and run benchmarks in temporary directories.
 * The assertions check report data and captured stderr diagnostics. Test helpers
 * also rebuild paired comparisons from the saved samples so the reports can be
 * checked independently of the production aggregation code.
 */
#include <catch2/catch_approx.hpp>

#include <simdjson.h>

#include <nlohmann/json.hpp>

#include <oneapi/tbb/global_control.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.instrumentation;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.bench.framework;
import bioseq.bench.prefilter_algorithm_selection;

namespace {

using bioseq::bench::framework::config_value;
using bioseq::bench::framework::create_benchmark_with_iteration_boundaries;
using bioseq::bench::framework::iteration_boundary_callbacks;
using bioseq::bench::framework::iteration_measurement_window;
using bioseq::bench::framework::parameter_map;
using bioseq::bench::framework::paired_schedule_evidence_fault;
using bioseq::bench::framework::raw_evidence_io_fault;
using bioseq::bench::framework::repeat_finalization_callbacks;
using bioseq::bench::framework::repeat_output_reference;
using bioseq::bench::framework::run_constraints;
using bioseq::bench::framework::create_benchmark;
using bioseq::bench::framework::run;
using bioseq::bench::framework::set_paired_schedule_evidence_fault_for_testing;
using bioseq::bench::framework::set_raw_evidence_io_fault_for_testing;

/// Silence framework logging while a test checks reports and stderr diagnostics.
/// Restore the previous log level when this object is destroyed.
struct test_log_silencer {
    fmtlog::LogLevel saved_level;
    explicit test_log_silencer(fmtlog::LogLevel level = fmtlog::OFF) noexcept
        : saved_level(fmtlog::getLogLevel())
    {
        configure_logging(true);
        fmtlog::setLogLevel(level);
    }
    ~test_log_silencer() noexcept {
        fmtlog::setLogLevel(saved_level);
    }
    test_log_silencer(const test_log_silencer&) = delete;
    test_log_silencer& operator=(const test_log_silencer&) = delete;
    test_log_silencer(test_log_silencer&&) = delete;
    test_log_silencer& operator=(test_log_silencer&&) = delete;
};

/// Rebuild a rule aggregate from the samples saved in paired-rule evidence.
/// This independent calculation checks that a report reader can reproduce
/// the framework's result from the recorded samples.
[[nodiscard]] double reconstruct_paired_evidence_aggregate(
    const nlohmann::json& samples,
    const std::string_view aggregate)
{
    std::vector<double> finite;
    finite.reserve(samples.size());
    for (const auto& sample : samples) {
        if (!sample.is_number()) continue;
        const double value = sample.get<double>();
        if (std::isfinite(value)) finite.push_back(value);
    }
    if (finite.empty()) return 0.0;
    if (aggregate == "sum") {
        double total = 0.0;
        for (const double value : finite) total += value;
        return total;
    }
    if (aggregate == "min") {
        return *std::min_element(finite.begin(), finite.end());
    }
    if (aggregate == "max") {
        return *std::max_element(finite.begin(), finite.end());
    }

    double percentile_fraction = 0.0;
    if (aggregate == "p01") {
        percentile_fraction = 0.01;
    } else if (aggregate == "p50") {
        percentile_fraction = 0.50;
    } else if (aggregate == "p99") {
        percentile_fraction = 0.99;
    } else {
        throw std::invalid_argument{"unsupported paired evidence aggregate"};
    }
    std::sort(finite.begin(), finite.end());
    const auto target_index = static_cast<std::size_t>(std::floor(
        percentile_fraction * static_cast<double>(finite.size() - 1)));
    return finite[target_index];
}

[[nodiscard]] double reconstruct_paired_evidence_aggregate(
    const std::vector<double>& samples,
    const std::string_view aggregate)
{
    const nlohmann::json json_samples = samples;
    return reconstruct_paired_evidence_aggregate(
        json_samples, aggregate);
}

/// Clear the injected schedule error when a test scope exits, including
/// when a Catch2 assertion ends it early.
struct paired_schedule_evidence_fault_reset {
    ~paired_schedule_evidence_fault_reset() noexcept {
        set_paired_schedule_evidence_fault_for_testing(
            paired_schedule_evidence_fault::none);
    }
    paired_schedule_evidence_fault_reset() = default;
    paired_schedule_evidence_fault_reset(
        const paired_schedule_evidence_fault_reset&) = delete;
    paired_schedule_evidence_fault_reset& operator=(
        const paired_schedule_evidence_fault_reset&) = delete;
};

// Temporary files and output capture for tests.

/// Create a temporary report directory and remove it when this object dies.
/// Each instance tries random names before falling back to a process-specific path.
struct temporary_report_directory {
    std::filesystem::path path;

    temporary_report_directory() {
        auto root = std::filesystem::temp_directory_path() / "bioseq_bench_framework_tests";
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        std::random_device rd;
        std::mt19937_64 rng{rd()};
        for (int attempt = 0; attempt < 64; ++attempt) {
            auto candidate = root / fmt::format("run_{:016x}", rng());
            if (std::filesystem::create_directory(candidate, ec) && !ec) {
                path = std::move(candidate);
                return;
            }
        }
        // Fall back to a directory named after this process.
        path = root / fmt::format("fallback_{}", ::getpid());
        std::filesystem::create_directories(path, ec);
    }

    ~temporary_report_directory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        // Leave cleanup errors out of the test result.
    }

    temporary_report_directory(const temporary_report_directory&) = delete;
    temporary_report_directory& operator=(const temporary_report_directory&) = delete;
    temporary_report_directory(temporary_report_directory&&) = delete;
    temporary_report_directory& operator=(temporary_report_directory&&) = delete;
};

/// Capture writes to stderr in a temporary file while this object is alive.
/// Call text() to read the output before destruction restores stderr and
/// removes the file.
struct standard_error_capture {
    std::FILE*  redirect_file = nullptr;
    int         saved_fd = -1;
    std::filesystem::path tmp_path{};

    standard_error_capture() {
        std::fflush(stderr);
        saved_fd = ::dup(STDERR_FILENO);
        tmp_path = std::filesystem::temp_directory_path()
                 / fmt::format("bench_framework_stderr_{:08x}.log",
                               static_cast<unsigned>(::getpid()) ^
                               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(this)));
        redirect_file = std::fopen(tmp_path.string().c_str(), "w+");
        if (redirect_file != nullptr) {
            ::dup2(::fileno(redirect_file), STDERR_FILENO);
        }
    }

    ~standard_error_capture() {
        if (redirect_file != nullptr) {
            std::fflush(stderr);
            std::fclose(redirect_file);
        }
        if (saved_fd >= 0) {
            ::dup2(saved_fd, STDERR_FILENO);
            ::close(saved_fd);
        }
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
    }

    standard_error_capture(const standard_error_capture&) = delete;
    standard_error_capture& operator=(const standard_error_capture&) = delete;
    standard_error_capture(standard_error_capture&&) = delete;
    standard_error_capture& operator=(standard_error_capture&&) = delete;

    /// Flush stderr and read all output captured so far.
    [[nodiscard]] std::string text() const {
        if (redirect_file == nullptr) return {};
        std::fflush(stderr);
        std::ifstream input(tmp_path, std::ios::binary);
        if (!input) return {};
        std::string content{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
        return content;
    }
};

// Build test configurations from small descriptors.
// String views and spans let fixtures declare their fields inline, while
// raw JSON fields allow each test to supply the values it needs.

/// One algorithm parameter and its schema.
/// default_json contains a JSON value, including quotes for strings.
/// An empty default leaves the parameter for the matrix to supply.
struct algorithm_parameter_specification {
    std::string_view name;
    std::string_view json_type;       // "string" | "integer" | "number" | "boolean"
    std::string_view default_json{};
    std::string_view enum_array_json{}; // raw JSON array, e.g. "[\"A\", \"B\"]"
};

struct algorithm_specification {
    std::string_view              name;
    std::span<const algorithm_parameter_specification> params{};
};

/// A placeholder parameter for tests whose algorithms need no settings.
/// The schema requires at least one declared parameter per algorithm.
inline constexpr std::array<algorithm_parameter_specification, 1> k_dummy_parameters{{
    {"unused", "integer", "0", ""}
}};

struct test_case_data_specification {
    std::string_view name;
    std::string_view description{};
    std::string_view data_object_json{"{}"};   // Raw JSON object, including braces.
    /// Maximum number of groups for this test case.
    /// Zero omits the field and uses the framework default. Set one when
    /// the test needs a single input and cleanup cycle.
    int              concurrency = 0;
};

struct metric_specification {
    std::string_view name;
    std::string_view type{"builtin"};        // "builtin"|"region"|"loop"|"counter"|"evaluation"
    std::string_view region_filter{};
    std::string_view counter_name{};
    std::string_view rules_json{"[]"};       // raw JSON array
};

struct algorithm_parameter_matrix_entry {
    std::string_view algorithm;
    std::string_view params_object_json{"{}"};   // raw JSON: { param: [vals], ... }
};

struct iteration_count_entry {
    std::string_view test_case;
    std::int64_t     iterations = 0;
};

struct named_region_sample_multiplicity_specification {
    std::string_view algorithm;
    std::string_view test_case;
    std::string_view region;
    std::string_view metric;
    std::size_t exact_samples = 0;
};

struct benchmark_specification {
    std::string_view              key;
    std::string_view              description{};
    /// Tags written to the required JSON array, which may be empty.
    /// The --filter tags=... option selects benchmarks by these values.
    std::span<const std::string_view> tags{};
    bool                           fast = false;
    bool                           bench_lock = false;
    bool                           concurrency = true;
    std::string_view              iteration_order_mode{};
    std::optional<std::uint64_t>  iteration_order_seed{};
    // test_case_lock is rejected by the schema, so this fixture has no field for it.
    bool                           by_regions = false;
    std::span<const iteration_count_entry>     for_entries{};
    std::span<const algorithm_parameter_matrix_entry>    over_entries{};
    std::span<const metric_specification> metrics{};
    /// Optional acceptance requirements for algorithm rows, repeats and region samples.
    /// Emit the object only when at least one requirement is set.
    std::span<const std::string_view> acceptance_exact_algorithm_rows{};
    std::span<const iteration_count_entry> acceptance_exact_test_case_iterations{};
    bool acceptance_require_named_region_iteration_coverage = false;
    std::span<const named_region_sample_multiplicity_specification>
        acceptance_exact_named_region_sample_multiplicities{};
};

// Helpers for writing JSON fields and lists.

inline void emit_quoted(std::string& out, std::string_view text) {
    out += '"';
    out.append(text);
    out += '"';
}

inline void emit_kv_quoted(std::string& out, std::string_view key, std::string_view value) {
    emit_quoted(out, key);
    out += ':';
    emit_quoted(out, value);
}

inline void emit_kv_raw(std::string& out, std::string_view key, std::string_view raw_value) {
    emit_quoted(out, key);
    out += ':';
    out.append(raw_value);
}

inline void emit_kv_bool(std::string& out, std::string_view key, bool value) {
    emit_kv_raw(out, key, value ? std::string_view{"true"} : std::string_view{"false"});
}

/// Write a delimited list, calling each_fn once per element.
/// Insert commas between elements. Braces make an object and brackets an array.
template<typename p_range, typename p_each_function>
inline void emit_block(std::string& out, char sep_open, char sep_close,
                        p_range&& range, p_each_function each_fn)
{
    out += sep_open;
    bool first = true;
    for (const auto& element : range) {
        if (!first) out += ',';
        first = false;
        each_fn(out, element);
    }
    out += sep_close;
}

template<typename p_range, typename p_each_function>
inline void emit_object_range(std::string& out, p_range&& range, p_each_function each_fn) {
    emit_block(out, '{', '}', std::forward<p_range>(range), std::move(each_fn));
}

// Write each configuration descriptor as JSON.

inline void emit_param_schema(std::string& out, const algorithm_parameter_specification& param) {
    emit_quoted(out, param.name);
    out += ":{";
    emit_kv_quoted(out, "type", param.json_type);
    if (!param.default_json.empty()) {
        out += ',';
        emit_kv_raw(out, "default", param.default_json);
    }
    if (!param.enum_array_json.empty()) {
        out += ',';
        emit_kv_raw(out, "enum", param.enum_array_json);
    }
    out += '}';
}

inline void emit_algo_block(std::string& out, const algorithm_specification& algo) {
    emit_quoted(out, algo.name);
    out += ':';
    emit_object_range(out, algo.params,
        [](std::string& s, const algorithm_parameter_specification& p) { emit_param_schema(s, p); });
}

inline void emit_metric(std::string& out, const metric_specification& metric) {
    emit_quoted(out, metric.name);
    out += ":{";
    emit_kv_quoted(out, "type", metric.type);
    if (!metric.region_filter.empty()) {
        out += ',';
        emit_kv_quoted(out, "region", metric.region_filter);
    }
    if (!metric.counter_name.empty()) {
        out += ',';
        emit_kv_quoted(out, "counter", metric.counter_name);
    }
    out += ',';
    emit_kv_raw(out, "rules", metric.rules_json);
    out += '}';
}

inline void emit_test_case(std::string& out, const test_case_data_specification& spec) {
    emit_quoted(out, spec.name);
    out += ":{";
    emit_kv_quoted(out, "description", spec.description);
    out += ',';
    if (spec.concurrency > 0) {
        emit_quoted(out, "concurrency");
        out += ':';
        out += std::to_string(spec.concurrency);
        out += ',';
    }
    emit_kv_raw(out, "data", spec.data_object_json);
    out += '}';
}

inline void emit_for_entry(std::string& out, const iteration_count_entry& entry) {
    emit_quoted(out, entry.test_case);
    out += ':';
    out += fmt::format("{}", entry.iterations);
}

inline void emit_named_region_sample_multiplicity(
    std::string& out,
    const named_region_sample_multiplicity_specification& spec)
{
    out += '{';
    emit_kv_quoted(out, "algorithm", spec.algorithm);
    out += ',';
    emit_kv_quoted(out, "test_case", spec.test_case);
    out += ',';
    emit_kv_quoted(out, "region", spec.region);
    out += ',';
    emit_kv_quoted(out, "metric", spec.metric);
    out += ',';
    emit_quoted(out, "exact_samples");
    out += ':';
    out += std::to_string(spec.exact_samples);
    out += '}';
}

inline void emit_over_entry(std::string& out, const algorithm_parameter_matrix_entry& entry) {
    emit_quoted(out, entry.algorithm);
    out += ':';
    out.append(entry.params_object_json);
}

inline void emit_benchmark(std::string& out, const benchmark_specification& bench) {
    emit_quoted(out, bench.key);
    out += ":{";
    emit_kv_quoted(out, "description", bench.description);
    out += ',';
    out += "\"tags\":";
    emit_block(out, '[', ']', bench.tags,
        [](std::string& s, std::string_view tag) { emit_quoted(s, tag); });
    out += ',';
    emit_kv_bool(out, "fast", bench.fast);
    out += ",\"concurrency\":{";
    emit_kv_bool(out, "bench_lock", bench.bench_lock);
    out += ',';
    emit_kv_bool(out, "concurrency", bench.concurrency);
    out += '}';
    if (!bench.iteration_order_mode.empty()) {
        out += ",\"iteration_order\":{";
        emit_kv_quoted(out, "mode", bench.iteration_order_mode);
        if (bench.iteration_order_seed.has_value()) {
            out += ',';
            emit_quoted(out, "seed");
            out += ':';
            out += std::to_string(*bench.iteration_order_seed);
        }
        out += '}';
    }
    out += ",\"matrix\":{";
    emit_kv_bool(out, "by_regions", bench.by_regions);
    if (!bench.acceptance_exact_algorithm_rows.empty()
        || !bench.acceptance_exact_test_case_iterations.empty()
        || bench.acceptance_require_named_region_iteration_coverage
        || !bench.acceptance_exact_named_region_sample_multiplicities.empty()) {
        out += ",\"acceptance\":{";
        out += "\"exact_algorithm_rows\":";
        emit_block(out, '[', ']', bench.acceptance_exact_algorithm_rows,
            [](std::string& s, std::string_view algorithm) {
                emit_quoted(s, algorithm);
            });
        out += ",\"exact_test_case_iterations\":";
        emit_object_range(out, bench.acceptance_exact_test_case_iterations,
                          emit_for_entry);
        out += ',';
        emit_kv_bool(out, "require_named_region_iteration_coverage",
                     bench.acceptance_require_named_region_iteration_coverage);
        out += ",\"exact_named_region_sample_multiplicities\":";
        emit_block(
            out, '[', ']',
            bench.acceptance_exact_named_region_sample_multiplicities,
            emit_named_region_sample_multiplicity);
        out += '}';
    }
    out += ",\"for\":";
    emit_object_range(out, bench.for_entries, emit_for_entry);
    out += ",\"over\":";
    emit_object_range(out, bench.over_entries, emit_over_entry);
    out += ",\"metrics\":";
    emit_object_range(out, bench.metrics, emit_metric);
    out += "}}";
}

/// Assemble a configuration from algorithm, test-case and benchmark descriptors.
/// Raw JSON fields must already be valid JSON.
[[nodiscard]] inline std::string render_config(
    std::span<const algorithm_specification>    algos,
    std::span<const test_case_data_specification> test_cases,
    std::span<const benchmark_specification>    benches,
    std::string_view                   regression_rules_json = "[]")
{
    std::string out;
    out += "{\"params\":";
    emit_object_range(out, algos, emit_algo_block);
    out += ",\"test_cases\":";
    emit_object_range(out, test_cases, emit_test_case);
    for (const auto& bench : benches) {
        out += ',';
        emit_benchmark(out, bench);
    }
    if (!regression_rules_json.empty() && regression_rules_json != "[]") {
        out += ',';
        emit_kv_raw(out, "regression_rules", regression_rules_json);
    }
    out += '}';
    return out;
}

// Run a benchmark and collect its report and diagnostics.

struct run_outcome {
    int           exit_code = -1;
    std::filesystem::path config_path{};
    std::filesystem::path report_path{};
    nlohmann::json report = nlohmann::json::object();
    std::string    captured_stderr{};
};

/// Find the report file with the highest numeric index in this directory.
[[nodiscard]] inline std::optional<std::filesystem::path> latest_report(
    const std::filesystem::path& report_dir)
{
    std::error_code ec;
    if (!std::filesystem::exists(report_dir, ec)) return std::nullopt;
    std::optional<std::size_t> max_index;
    std::filesystem::path winner;
    constexpr std::string_view k_prefix = "report_";
    constexpr std::string_view k_suffix = ".json";
    for (const auto& entry : std::filesystem::directory_iterator(report_dir, ec)) {
        if (ec) break;
        const auto filename = entry.path().filename().string();
        if (!filename.starts_with(k_prefix) || !filename.ends_with(k_suffix)) continue;
        if (filename.size() <= k_prefix.size() + k_suffix.size()) continue;
        const auto digits = std::string_view{filename}.substr(
            k_prefix.size(),
            filename.size() - k_prefix.size() - k_suffix.size());
        std::size_t parsed = 0;
        const bool digits_only = !digits.empty()
            && std::all_of(digits.begin(), digits.end(), [](char ch) {
                return std::isdigit(static_cast<unsigned char>(ch)) != 0;
            });
        if (!digits_only) continue;
        for (const char ch : digits) {
            parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
        }
        if (!max_index.has_value() || parsed > *max_index) {
            max_index = parsed;
            winner = entry.path();
        }
    }
    if (!max_index.has_value()) return std::nullopt;
    return winner;
}

/// Own argument strings and expose the argv pointers needed by the runner.
/// The pointers remain valid while this builder and its strings stay in place.
struct argument_vector_builder {
    std::vector<std::string>          arena;
    std::vector<char*>                pointers;

    explicit argument_vector_builder(std::span<const std::string> args) {
        arena.assign(args.begin(), args.end());
        pointers.reserve(arena.size() + 1);
        for (auto& s : arena) pointers.push_back(s.data());
        pointers.push_back(nullptr);
    }

    [[nodiscard]] int    argc() const noexcept { return static_cast<int>(arena.size()); }
    [[nodiscard]] char** argv() noexcept       { return pointers.data(); }
};

/// Run one benchmark fixture and return its report, exit code and stderr output.
/// Infer input and output types from the callbacks. Resource estimates are empty,
/// and the input adapter accepts the test's three-argument callback.
template<typename p_on_input, typename p_on_run, typename p_on_evaluate,
         typename p_on_cleanup = std::nullptr_t,
         typename p_on_raw_serializer = std::nullptr_t>
[[nodiscard]] run_outcome run_one_bench(
    std::string_view  bench_key,
    std::string_view  bench_title,
    std::string_view  config_text,
    const temporary_report_directory& report_dir,
    p_on_input  on_input,
    p_on_run    on_run,
    p_on_evaluate on_evaluate,
    p_on_cleanup  on_cleanup = nullptr,
    p_on_raw_serializer on_raw_serializer = nullptr)
{
    using bioseq::bench::framework::run_constraints;
    using input_type = std::invoke_result_t<
        p_on_input, std::string_view, std::string_view, simdjson::dom::element>;
    using output_type = std::invoke_result_t<
        p_on_run, std::string_view, const parameter_map&, const input_type&>;
    static_assert(std::is_same_v<
        std::invoke_result_t<p_on_evaluate, std::string_view, const parameter_map&, const input_type&, const output_type&>,
        std::map<std::string, double>>,
        "on_evaluate must return std::map<std::string, double> when invoked with (algo, params, input, output)");

    run_outcome outcome;
    outcome.config_path = report_dir.path / "config.json";
    {
        std::ofstream cfg(outcome.config_path, std::ios::trunc | std::ios::binary);
        cfg.write(config_text.data(), static_cast<std::streamsize>(config_text.size()));
    }

    using estimate_function = std::function<run_constraints(std::string_view,
                                                     std::string_view,
                                                     simdjson::dom::element)>;
    using input_function   = std::function<input_type(std::string_view, std::string_view,
                                            simdjson::dom::element,
                                            const run_constraints&)>;
    using run_function     = std::function<output_type(std::string_view, const parameter_map&, const input_type&)>;
    using evaluation_function    = std::function<std::map<std::string, double>(std::string_view,
                                                                  const parameter_map&,
                                                                  const input_type&,
                                                                  const output_type&)>;
    using cleanup_function = std::function<void(std::string_view, std::string_view,
                                          simdjson::dom::element, const input_type&)>;
    using raw_serializer_function = std::function<std::string(
        std::string_view, const parameter_map&, const input_type&, const output_type&)>;

    estimate_function estimate_fn = [](std::string_view, std::string_view,
                                 simdjson::dom::element) noexcept {
        return run_constraints{};
    };
    input_function adapted_input_fn = [input_builder = std::move(on_input)](
        std::string_view data_dir, std::string_view test_case_name,
        simdjson::dom::element data_element,
        const run_constraints& /*constraints*/) mutable -> input_type {
        return input_builder(data_dir, test_case_name, data_element);
    };

    // An omitted cleanup callback stays empty, exercising that runner path.
    cleanup_function cleanup_fn{};
    if constexpr (!std::is_same_v<p_on_cleanup, std::nullptr_t>) {
        cleanup_fn = cleanup_function{std::move(on_cleanup)};
    }
    raw_serializer_function raw_serializer_fn{};
    if constexpr (!std::is_same_v<p_on_raw_serializer, std::nullptr_t>) {
        raw_serializer_fn = raw_serializer_function{std::move(on_raw_serializer)};
    }

    auto bench = create_benchmark<input_type, output_type>(
        std::string{bench_title},
        std::string{bench_key},
        outcome.config_path.string(),
        std::move(estimate_fn),
        std::move(adapted_input_fn),
        run_function{std::move(on_run)},
        evaluation_function{std::move(on_evaluate)},
        std::move(cleanup_fn),
        std::move(raw_serializer_fn));

    const std::array<std::string, 5> args = {
        "bench_framework_test",
        "--report",
        report_dir.path.string(),
        "--threads",
        "2",
    };
    argument_vector_builder argv_holder{std::span<const std::string>{args}};

    {
        standard_error_capture capture;
        outcome.exit_code = run(argv_holder.argc(), argv_holder.argv(),
                                 report_dir.path.string(),
                                 report_dir.path.string(),
                                 std::move(bench));
        outcome.captured_stderr = capture.text();
    }

    if (auto path = latest_report(report_dir.path); path.has_value()) {
        outcome.report_path = *path;
        std::ifstream input(outcome.report_path, std::ios::binary);
        if (input) {
            std::string body{std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{}};
            // Treat invalid report JSON as an empty report so the test's assertions
            // can report the missing data.
            outcome.report = nlohmann::json::parse(
                body, /*cb=*/nullptr, /*allow_exceptions=*/false);
            if (outcome.report.is_discarded()) {
                outcome.report = nlohmann::json::object();
            }
        }
    }
    return outcome;
}

// Find values in generated reports.

/// Copy the first matching aggregate for an algorithm, test case, region and metric.
/// Return nullopt when the path is absent. Parameters are not part of this lookup.
[[nodiscard]] inline std::optional<nlohmann::json> aggregate_at(
    const nlohmann::json& report,
    std::string_view bench_key,
    std::string_view algorithm,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric)
{
    if (!report.contains("benchmarks")) return std::nullopt;
    const auto& benches = report["benchmarks"];
    if (!benches.contains(bench_key)) return std::nullopt;
    const auto& bench = benches[std::string{bench_key}];
    if (!bench.contains("execution_tuples")) return std::nullopt;
    for (const auto& tuple_record : bench["execution_tuples"]) {
        if (!tuple_record.contains("tuple")) continue;
        const auto& tuple_obj = tuple_record["tuple"];
        if (!tuple_obj.contains(std::string{algorithm})) continue;
        if (!tuple_record.contains("test_cases")) continue;
        const auto& tcs = tuple_record["test_cases"];
        if (!tcs.contains(std::string{test_case})) continue;
        const auto& tc = tcs[std::string{test_case}];
        if (!tc.contains(std::string{region})) continue;
        const auto& region_obj = tc[std::string{region}];
        if (!region_obj.contains(std::string{metric})) continue;
        return std::optional<nlohmann::json>{region_obj[std::string{metric}]};
    }
    return std::nullopt;
}

/// Find an aggregate that a history test can modify in place.
/// Return nullptr when it is absent.
[[nodiscard]] inline nlohmann::json* mutable_aggregate_at(
    nlohmann::json& report,
    std::string_view bench_key,
    std::string_view algorithm,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric)
{
    if (!report.contains("benchmarks")) return nullptr;
    auto& benches = report["benchmarks"];
    if (!benches.contains(bench_key)) return nullptr;
    auto& bench = benches[std::string{bench_key}];
    if (!bench.contains("execution_tuples")) return nullptr;
    for (auto& tuple_record : bench["execution_tuples"]) {
        if (!tuple_record.contains("tuple")) continue;
        auto& tuple_obj = tuple_record["tuple"];
        if (!tuple_obj.contains(std::string{algorithm})) continue;
        if (!tuple_record.contains("test_cases")) continue;
        auto& test_cases = tuple_record["test_cases"];
        if (!test_cases.contains(std::string{test_case})) continue;
        auto& selected_case = test_cases[std::string{test_case}];
        if (!selected_case.contains(std::string{region})) continue;
        auto& region_obj = selected_case[std::string{region}];
        if (!region_obj.contains(std::string{metric})) continue;
        return &region_obj[std::string{metric}];
    }
    return nullptr;
}

/// Write a modified history report for corruption tests.
/// Return false if opening, writing or closing the file fails.
[[nodiscard]] inline bool rewrite_report_json(
    const std::filesystem::path& path,
    const nlohmann::json& report)
{
    const std::string body = report.dump();
    std::ofstream output(path, std::ios::trunc | std::ios::binary);
    if (!output) return false;
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    output.close();
    return output.good();
}

/// Read the aggregate's samples_count, or zero when the cell or count is absent.
[[nodiscard]] inline std::size_t samples_count_at(
    const nlohmann::json& report,
    std::string_view bench_key,
    std::string_view algorithm,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric)
{
    const auto cell = aggregate_at(report, bench_key, algorithm,
                                    test_case, region, metric);
    if (!cell.has_value() || !cell->contains("samples_count")) return 0;
    return cell->at("samples_count").get<std::size_t>();
}

/// Read a named statistic such as mean or sum from an aggregate.
/// Return NaN when the cell or statistic is absent.
[[nodiscard]] inline double stat_at(
    const nlohmann::json& report,
    std::string_view bench_key,
    std::string_view algorithm,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric,
    std::string_view stat_name)
{
    const auto cell = aggregate_at(report, bench_key, algorithm,
                                    test_case, region, metric);
    if (!cell.has_value() || !cell->contains("stats")) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto& stats = cell->at("stats");
    if (!stats.contains(std::string{stat_name})) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return stats.at(std::string{stat_name}).get<double>();
}

[[nodiscard]] inline std::size_t count_sanity_warnings_of_type(
    const nlohmann::json& report, std::string_view warning_type)
{
    if (!report.contains("sanity_warnings")) return 0;
    std::size_t total = 0;
    for (const auto& w : report["sanity_warnings"]) {
        if (w.value("type", std::string{}) == warning_type) ++total;
    }
    return total;
}

[[nodiscard]] inline std::optional<nlohmann::json> sanity_warning_for_metric(
    const nlohmann::json& report,
    std::string_view warning_type,
    std::string_view metric_name)
{
    if (!report.contains("sanity_warnings")) return std::nullopt;
    for (const auto& warning : report["sanity_warnings"]) {
        if (warning.value("type", std::string{}) == warning_type
            && warning.value("metric", std::string{}) == metric_name) {
            return std::optional<nlohmann::json>{std::in_place, warning};
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool stderr_contains(
    std::string_view captured, std::string_view needle)
{
    return captured.find(needle) != std::string_view::npos;
}

// Small deterministic inputs and outputs used by benchmark fixtures.

struct toy_input {
    std::vector<std::int64_t> values;
    std::int64_t              iteration_seed = 0;
};

struct toy_output {
    std::int64_t sum = 0;
    std::int64_t parity_count = 0;
};

/// Reference p50 measurements for the alignment comparison tests.
/// Tests scale these values together or change one stage to check the ratio rules.
struct curated_alignment_reference_p50 {
    static constexpr double block_wall = 10'023'361'667.0;
    static constexpr double batched_wall = 9'480'783'958.0;
    static constexpr double vulkan_wall = 9'580'900'708.0;
    static constexpr double simd_wall = 9'474'587'625.0;
    static constexpr double minimap2_wall = 11'887'135'209.0;
    static constexpr double scalar_wall = 17'535'332'166.0;
    static constexpr double simd_sketch = 1'982'142'583.0;
    static constexpr double scalar_sketch = 9'416'490'458.0;
    static constexpr double simd_chain = 64'720'333.0;
    static constexpr double scalar_chain = 111'727'708.0;
    static constexpr double simd_backtrack = 12'132'584.0;
    static constexpr double scalar_backtrack = 12'703'625.0;
    static constexpr double block_alignment = 349'910'209.0;
    static constexpr double simd_alignment = 591'786'042.0;
};

[[nodiscard]] inline toy_input build_toy_input(
    std::string_view /*data_dir*/,
    std::string_view test_case_name,
    simdjson::dom::element data_element)
{
    toy_input input;
    std::int64_t length = 16;
    (void)data_element["length"].get(length);
    std::int64_t seed = 7;
    (void)data_element["seed"].get(seed);
    std::mt19937_64 rng{static_cast<std::uint64_t>(seed)};
    input.values.resize(static_cast<std::size_t>(length));
    std::uniform_int_distribution<std::int64_t> dist(0, 1'000);
    for (auto& value : input.values) value = dist(rng);
    input.iteration_seed = seed ^ static_cast<std::int64_t>(test_case_name.size());
    return input;
}

}  // namespace

TEST_CASE("bench_lock drains older work, blocks newer work and is globally exclusive",
          "[component:benchmark_framework][layer:contract][feature:scheduling][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    static constexpr std::array<algorithm_parameter_specification, 1> scheduler_params{{
        {"unused", "integer", "0", ""}
    }};
    static constexpr std::array<test_case_data_specification, 1> scheduler_test_cases{{
        {"SMALL", "8 ints", R"({"length":8,"seed":3})", 1}
    }};
    static constexpr std::array<metric_specification, 1> scheduler_metrics{{
        {"sum_value", "evaluation", "", "", "[]"},
    }};
    static constexpr std::array<algorithm_specification, 3> algorithms{{
        {"older", std::span<const algorithm_parameter_specification>{scheduler_params}},
        {"locked", std::span<const algorithm_parameter_specification>{scheduler_params}},
        {"newer", std::span<const algorithm_parameter_specification>{scheduler_params}},
    }};
    static constexpr std::array<iteration_count_entry, 1> one_run{{{"SMALL", 1}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> older_over{{{"older", "{}"}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> locked_over{{{"locked", "{}"}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> newer_over{{{"newer", "{}"}}};
    const std::array<benchmark_specification, 3> specs{{
        benchmark_specification{.key="bench_older", .description="older",
            .concurrency=false, .for_entries=std::span<const iteration_count_entry>{one_run},
            .over_entries=std::span<const algorithm_parameter_matrix_entry>{older_over},
            .metrics=std::span<const metric_specification>{scheduler_metrics}},
        benchmark_specification{.key="bench_locked", .description="locked",
            .bench_lock=true, .concurrency=false,
            .for_entries=std::span<const iteration_count_entry>{one_run},
            .over_entries=std::span<const algorithm_parameter_matrix_entry>{locked_over},
            .metrics=std::span<const metric_specification>{scheduler_metrics}},
        benchmark_specification{.key="bench_newer", .description="newer",
            .concurrency=false, .for_entries=std::span<const iteration_count_entry>{one_run},
            .over_entries=std::span<const algorithm_parameter_matrix_entry>{newer_over},
            .metrics=std::span<const metric_specification>{scheduler_metrics}},
    }};
    const std::string config = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{scheduler_test_cases},
        std::span<const benchmark_specification>{specs});
    temporary_report_directory report_dir;
    const auto config_file = report_dir.path / "config.json";
    {
        std::ofstream output(config_file, std::ios::trunc | std::ios::binary);
        REQUIRE(output.good());
        output.write(config.data(), static_cast<std::streamsize>(config.size()));
        output.close();
        REQUIRE(output.good());
    }
    const std::string config_path = config_file.string();

    std::atomic<int> sequence{0};
    std::atomic<int> unlocked_active{0};
    std::atomic<int> locked_active{0};
    std::atomic<int> overlap{0};
    std::atomic<int> older_start{0}, older_end{0};
    std::atomic<int> lock_start{0}, lock_end{0}, newer_start{0};
    std::atomic<bool> lock_estimator_returned{false};

    using estimate_function = std::function<run_constraints(
        std::string_view, std::string_view, simdjson::dom::element)>;
    using input_function = std::function<toy_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>;
    using run_function = std::function<toy_output(
        std::string_view, const parameter_map&, const toy_input&)>;
    using evaluation_function = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)>;
    const input_function input = [](std::string_view data, std::string_view name,
        simdjson::dom::element element, const run_constraints&) {
        return build_toy_input(data, name, element);
    };
    const evaluation_function evaluate = [](std::string_view, const parameter_map&,
        const toy_input&, const toy_output& output) {
        return std::map<std::string,double>{{"sum_value",
            static_cast<double>(output.sum)}};
    };
    const auto make = [&](std::string key, estimate_function estimate, run_function run_fn) {
        return create_benchmark<toy_input,toy_output>(
            key, key, config_path, std::move(estimate), input,
            std::move(run_fn), evaluate);
    };
    auto older = make("bench_older",
        [](std::string_view, std::string_view, simdjson::dom::element) {
            return run_constraints{};
        },
        [&](std::string_view, const parameter_map&, const toy_input&) {
            unlocked_active.fetch_add(1);
            if (locked_active.load() != 0) overlap.fetch_add(1);
            older_start.store(sequence.fetch_add(1) + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            if (locked_active.load() != 0) overlap.fetch_add(1);
            older_end.store(sequence.fetch_add(1) + 1);
            unlocked_active.fetch_sub(1);
            return toy_output{.sum=1};
        });
    auto locked = make("bench_locked",
        [&](std::string_view, std::string_view, simdjson::dom::element) {
            while (older_start.load() == 0) std::this_thread::yield();
            lock_estimator_returned.store(true);
            return run_constraints{};
        },
        [&](std::string_view, const parameter_map&, const toy_input&) {
            locked_active.fetch_add(1);
            if (unlocked_active.load() != 0) overlap.fetch_add(1);
            lock_start.store(sequence.fetch_add(1) + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (unlocked_active.load() != 0) overlap.fetch_add(1);
            lock_end.store(sequence.fetch_add(1) + 1);
            locked_active.fetch_sub(1);
            return toy_output{.sum=2};
        });
    auto newer = make("bench_newer",
        [&](std::string_view, std::string_view, simdjson::dom::element) {
            while (!lock_estimator_returned.load()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return run_constraints{};
        },
        [&](std::string_view, const parameter_map&, const toy_input&) {
            unlocked_active.fetch_add(1);
            if (locked_active.load() != 0) overlap.fetch_add(1);
            newer_start.store(sequence.fetch_add(1) + 1);
            unlocked_active.fetch_sub(1);
            return toy_output{.sum=3};
        });
    const std::vector<std::string> args{
        "bench_framework_test", "--report", report_dir.path.string(),
        "--threads", "6",
    };
    argument_vector_builder argv_holder{std::span<const std::string>{args}};
    int exit_code = -1;
    {
        standard_error_capture capture;
        exit_code = run(argv_holder.argc(), argv_holder.argv(),
            report_dir.path.string(), report_dir.path.string(),
            std::move(older), std::move(locked), std::move(newer));
    }
    REQUIRE(exit_code == 0);
    REQUIRE(overlap.load() == 0);
    REQUIRE(older_start.load() < older_end.load());
    REQUIRE(older_end.load() < lock_start.load());
    REQUIRE(lock_start.load() < lock_end.load());
    REQUIRE(lock_end.load() < newer_start.load());
}

TEST_CASE("all benchmark drivers prepare before caller-thread finalization",
          "[component:benchmark_framework][layer:contract][feature:scheduling][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    static constexpr std::size_t benchmark_count = 5;
    static constexpr std::array<algorithm_specification, 1> algorithms{{
        {"prepared_body", std::span<const algorithm_parameter_specification>{k_dummy_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"SMALL", "one scheduled body", R"({"length":8,"seed":19})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{{"SMALL", 1}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> over_entries{{
        {"prepared_body", "{}"},
    }};
    static constexpr std::array<metric_specification, 1> metrics{{
        {"sum_value", "evaluation", "", "", "[]"},
    }};
    static constexpr std::array<benchmark_specification, benchmark_count> specs{{
        benchmark_specification{
            .key = "bench_prepare_a",
            .description = "nonblocking driver A",
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
        benchmark_specification{
            .key = "bench_prepare_b",
            .description = "nonblocking driver B",
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
        benchmark_specification{
            .key = "bench_prepare_c",
            .description = "nonblocking driver C",
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
        benchmark_specification{
            .key = "bench_prepare_d",
            .description = "nonblocking driver D",
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
        benchmark_specification{
            .key = "bench_prepare_e",
            .description = "nonblocking driver E",
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
    }};
    const std::string config = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{specs});
    temporary_report_directory report_dir;
    const auto config_file = report_dir.path / "config.json";
    {
        std::ofstream output(config_file, std::ios::trunc | std::ios::binary);
        REQUIRE(output.good());
        output.write(config.data(), static_cast<std::streamsize>(config.size()));
        output.close();
        REQUIRE(output.good());
    }

    auto& global_pool = bioseq::parallelism::global_pool::instance();
    global_pool.shutdown();
    struct shutdown_pool_on_exit {
        bioseq::parallelism::global_pool& pool;
        ~shutdown_pool_on_exit() noexcept { pool.shutdown(); }
    } shutdown_pool_on_exit{global_pool};

    std::atomic<std::size_t> estimator_calls{0};
    std::atomic<std::size_t> body_calls{0};
    std::atomic<std::size_t> preparation_timeouts{0};
    std::mutex readiness_mu;
    std::condition_variable readiness_cv;
    std::once_flag configure_pool_once;
    using estimate_function = std::function<run_constraints(
        std::string_view, std::string_view, simdjson::dom::element)>;
    using input_function = std::function<toy_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>;
    using run_function = std::function<toy_output(
        std::string_view, const parameter_map&, const toy_input&)>;
    using evaluation_function = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)>;
    std::vector<bioseq::bench::framework::erased_benchmark> benchmarks;
    benchmarks.reserve(benchmark_count);
    for (const auto& spec : specs) {
        benchmarks.push_back(create_benchmark<toy_input, toy_output>(
            std::string{spec.description},
            std::string{spec.key},
            config_file.string(),
            estimate_function{[&estimator_calls, &readiness_cv](
                         std::string_view, std::string_view,
                         simdjson::dom::element) {
                estimator_calls.fetch_add(1, std::memory_order_release);
                readiness_cv.notify_all();
                return run_constraints{};
            }},
            input_function{[](std::string_view data, std::string_view name,
                       simdjson::dom::element element,
                       const run_constraints&) {
                return build_toy_input(data, name, element);
            }},
            run_function{[&estimator_calls, &body_calls, &preparation_timeouts,
                   &readiness_mu, &readiness_cv, &configure_pool_once,
                   &global_pool](std::string_view, const parameter_map&,
                                 const toy_input&) {
                std::unique_lock<std::mutex> lock(readiness_mu);
                const bool all_prepared = readiness_cv.wait_for(
                    lock, std::chrono::seconds{2}, [&estimator_calls] {
                        return estimator_calls.load(std::memory_order_acquire)
                            == benchmark_count;
                    });
                lock.unlock();
                if (!all_prepared) {
                    preparation_timeouts.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    std::call_once(configure_pool_once, [&global_pool] {
                        // Configure the pool after every benchmark has reached its resource estimator.
                        global_pool.ensure_configured();
                    });
                }
                body_calls.fetch_add(1, std::memory_order_relaxed);
                return toy_output{.sum = 1};
            }},
            evaluation_function{[](std::string_view, const parameter_map&, const toy_input&,
                      const toy_output& output) {
                return std::map<std::string, double>{
                    {"sum_value", static_cast<double>(output.sum)},
                };
            }}));
    }
    const std::vector<std::string> args{
        "bench_framework_test", "--report", report_dir.path.string(),
        "--threads", "4",
    };
    argument_vector_builder argv_holder{std::span<const std::string>{args}};
    int exit_code = -1;
    {
        standard_error_capture capture;
        exit_code = bioseq::bench::framework::detail::run_impl(
            argv_holder.argc(), argv_holder.argv(),
            report_dir.path.string(), report_dir.path.string(),
            std::move(benchmarks));
    }
    REQUIRE(exit_code == 0);
    REQUIRE(estimator_calls.load(std::memory_order_acquire) == benchmark_count);
    REQUIRE(preparation_timeouts.load(std::memory_order_relaxed) == 0);
    REQUIRE(body_calls.load(std::memory_order_relaxed) == benchmark_count);
}

TEST_CASE("global_pool concurrency is arena-local and does not clamp unrelated TBB work",
          "[component:benchmark_framework][layer:contract][feature:scheduling]") {
    auto& pool = bioseq::parallelism::global_pool::instance();
    pool.shutdown();
    struct shutdown_pool_on_exit {
        bioseq::parallelism::global_pool& pool;
        ~shutdown_pool_on_exit() noexcept { pool.shutdown(); }
    } shutdown_pool_on_exit{pool};

    using global_control = oneapi::tbb::global_control;
    const std::size_t baseline = global_control::active_value(
        global_control::max_allowed_parallelism);
    if (baseline <= 1) {
        SKIP("process already has a oneTBB parallelism limit of one");
    }

    pool.configure(bioseq::parallelism::pool_config{
        .maximum_concurrency = 1,
        .mode = bioseq::parallelism::pipeline_concurrency_mode::single_thread,
    });
    REQUIRE(global_control::active_value(
                global_control::max_allowed_parallelism) == baseline);

    std::atomic<std::size_t> range_calls{0};
    pool.parallel_for(0, 8, 1, bioseq::parallelism::range_body{
        [&range_calls](std::size_t begin, std::size_t end) {
            REQUIRE(begin == 0);
            REQUIRE(end == 8);
            range_calls.fetch_add(1, std::memory_order_relaxed);
        }});
    REQUIRE(range_calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("resource estimator is evaluated once per selected group and its exact answer reaches on_input",
          "[component:benchmark_framework][layer:contract][feature:scheduling][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"algo_a", std::span<const algorithm_parameter_specification>{k_dummy_parameters}},
        {"algo_b", std::span<const algorithm_parameter_specification>{k_dummy_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"GROUPED", "three independently scheduled groups",
         R"({"length":12,"seed":17})", 3},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"GROUPED", 3},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> over_entries{{
        {"algo_a", "{}"},
        {"algo_b", "{}"},
    }};
    static constexpr std::array<metric_specification, 1> metrics{{
        {"sum_value", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benchmarks{{
        benchmark_specification{
            .key = "bench_resource_once",
            .description = "once-only resource estimator",
            .concurrency = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
    }};
    const std::string config = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benchmarks});

    temporary_report_directory report_dir;
    const auto config_file = report_dir.path / "config.json";
    {
        std::ofstream output(config_file, std::ios::trunc | std::ios::binary);
        REQUIRE(output.good());
        output.write(config.data(), static_cast<std::streamsize>(config.size()));
        output.close();
        REQUIRE(output.good());
    }

    constexpr std::size_t selected_group_count = 3;
    constexpr std::size_t base_first = 4'096;
    constexpr std::size_t extra_first = 8'192;
    constexpr std::size_t base_stride = 64;
    constexpr std::size_t extra_stride = 128;
    std::atomic<std::size_t> estimator_calls{0};
    std::atomic<std::size_t> input_calls{0};
    std::atomic<std::size_t> mismatched_constraints{0};
    std::atomic<std::size_t> mismatched_execution_tuples{0};
    std::array<std::atomic<std::size_t>, selected_group_count>
        input_seen_for_estimate{};

    using estimate_function = std::function<run_constraints(
        std::string_view, std::string_view, simdjson::dom::element)>;
    using input_function = std::function<toy_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>;
    using run_function = std::function<toy_output(
        std::string_view, const parameter_map&, const toy_input&)>;
    using evaluation_function = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)>;

    estimate_function estimate = [&estimator_calls](
          std::string_view, std::string_view,
          simdjson::dom::element) noexcept {
        const auto estimate_index = estimator_calls.fetch_add(
            1, std::memory_order_relaxed);
        return run_constraints{
            .base_memory = base_first + estimate_index * base_stride,
            .extra_memory = extra_first + estimate_index * extra_stride,
        };
    };
    input_function input = [
            &input_calls,
            &mismatched_constraints,
            &mismatched_execution_tuples,
            &input_seen_for_estimate
        ](std::string_view data_dir,
          std::string_view test_case_name,
          simdjson::dom::element data,
          const run_constraints& constraints) {
        input_calls.fetch_add(1, std::memory_order_relaxed);
        const bool tuple_shape_matches =
            constraints.execution_tuples.size() == 2
            && constraints.execution_tuples[0].algorithm_name == "algo_a"
            && constraints.execution_tuples[1].algorithm_name == "algo_b"
            && constraints.execution_tuples[0].params.at("unused") == 0
            && constraints.execution_tuples[1].params.at("unused") == 0;
        if (!tuple_shape_matches) {
            mismatched_execution_tuples.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (constraints.base_memory < base_first
            || (constraints.base_memory - base_first) % base_stride != 0) {
            mismatched_constraints.fetch_add(1, std::memory_order_relaxed);
        } else {
            const auto estimate_index =
                (constraints.base_memory - base_first) / base_stride;
            const auto expected_extra =
                extra_first + estimate_index * extra_stride;
            if (estimate_index >= selected_group_count
                || constraints.extra_memory != expected_extra) {
                mismatched_constraints.fetch_add(1, std::memory_order_relaxed);
            } else {
                input_seen_for_estimate[estimate_index].fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        return build_toy_input(data_dir, test_case_name, data);
    };
    run_function run_fn = [](std::string_view, const parameter_map&, const toy_input& input) {
        toy_output output;
        for (const auto value : input.values) output.sum += value;
        return output;
    };
    evaluation_function evaluate = [](std::string_view, const parameter_map&, const toy_input&,
                         const toy_output& output) {
        return std::map<std::string, double>{{
            "sum_value", static_cast<double>(output.sum)}};
    };

    auto benchmark = create_benchmark<toy_input, toy_output>(
        "once-only resource estimator", "bench_resource_once",
        config_file.string(), std::move(estimate), std::move(input),
        std::move(run_fn), std::move(evaluate));
    const std::vector<std::string> args{
        "bench_framework_test", "--report", report_dir.path.string(),
        "--threads", "3",
    };
    argument_vector_builder argv_holder{std::span<const std::string>{args}};
    int exit_code = -1;
    {
        standard_error_capture capture;
        exit_code = run(argv_holder.argc(), argv_holder.argv(),
            report_dir.path.string(), report_dir.path.string(),
            std::move(benchmark));
    }

    REQUIRE(exit_code == 0);
    REQUIRE(estimator_calls.load(std::memory_order_relaxed)
            == selected_group_count);
    REQUIRE(input_calls.load(std::memory_order_relaxed)
            == selected_group_count);
    REQUIRE(mismatched_constraints.load(std::memory_order_relaxed) == 0);
    REQUIRE(mismatched_execution_tuples.load(std::memory_order_relaxed) == 0);
    for (const auto& seen : input_seen_for_estimate) {
        REQUIRE(seen.load(std::memory_order_relaxed) == 1);
    }
}

TEST_CASE("unrepresentable and over-system resource estimates are rejected before input",
          "[component:benchmark_framework][layer:contract][feature:scheduling][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 1> algorithms{{
        {"algo_a", std::span<const algorithm_parameter_specification>{k_dummy_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"REJECTED", "resource estimate must fail before input",
         R"({"length":4,"seed":23})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"REJECTED", 1},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> over_entries{{
        {"algo_a", "{}"},
    }};
    static constexpr std::array<metric_specification, 1> metrics{{
        {"sum_value", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benchmarks{{
        benchmark_specification{
            .key = "bench_resource_rejected",
            .description = "reject invalid resource totals",
            .concurrency = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
    }};
    const std::string config = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benchmarks});

    run_constraints requested{};
    std::string expected_reason;
    SECTION("near-SIZE_MAX base plus extra overflows") {
        requested = run_constraints{
            .base_memory = (std::numeric_limits<std::size_t>::max)() - 7u,
            .extra_memory = 8u,
        };
        expected_reason = "unrepresentable memory request";
    }
    SECTION("representable request larger than system RAM") {
        // Request memory above the test host's RAM limit with a representable size.
        requested = run_constraints{
            .base_memory = std::size_t{1} << 60,
            .extra_memory = 0u,
        };
        expected_reason = "but the host has only";
    }

    temporary_report_directory report_dir;
    const auto config_file = report_dir.path / "config.json";
    {
        std::ofstream output(config_file, std::ios::trunc | std::ios::binary);
        REQUIRE(output.good());
        output.write(config.data(), static_cast<std::streamsize>(config.size()));
        output.close();
        REQUIRE(output.good());
    }

    std::atomic<std::size_t> estimator_calls{0};
    std::atomic<std::size_t> input_calls{0};
    using estimate_function = std::function<run_constraints(
        std::string_view, std::string_view, simdjson::dom::element)>;
    using input_function = std::function<toy_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>;
    using run_function = std::function<toy_output(
        std::string_view, const parameter_map&, const toy_input&)>;
    using evaluation_function = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)>;

    estimate_function estimate = [&estimator_calls, requested](
          std::string_view, std::string_view,
          simdjson::dom::element) noexcept {
        estimator_calls.fetch_add(1, std::memory_order_relaxed);
        return requested;
    };
    input_function input = [&input_calls](
          std::string_view data_dir,
          std::string_view test_case_name,
          simdjson::dom::element data,
          const run_constraints&) {
        input_calls.fetch_add(1, std::memory_order_relaxed);
        return build_toy_input(data_dir, test_case_name, data);
    };
    run_function run_fn = [](std::string_view, const parameter_map&, const toy_input& input) {
        toy_output output;
        for (const auto value : input.values) output.sum += value;
        return output;
    };
    evaluation_function evaluate = [](std::string_view, const parameter_map&, const toy_input&,
                         const toy_output& output) {
        return std::map<std::string, double>{{
            "sum_value", static_cast<double>(output.sum)}};
    };

    auto benchmark = create_benchmark<toy_input, toy_output>(
        "rejected resource estimate", "bench_resource_rejected",
        config_file.string(), std::move(estimate), std::move(input),
        std::move(run_fn), std::move(evaluate));
    const std::vector<std::string> args{
        "bench_framework_test", "--report", report_dir.path.string(),
        "--threads", "2",
    };
    argument_vector_builder argv_holder{std::span<const std::string>{args}};
    int exit_code = -1;
    std::string captured_stderr;
    {
        standard_error_capture capture;
        exit_code = run(argv_holder.argc(), argv_holder.argv(),
            report_dir.path.string(), report_dir.path.string(),
            std::move(benchmark));
        captured_stderr = capture.text();
    }

    REQUIRE(exit_code != -1);
    REQUIRE(estimator_calls.load(std::memory_order_relaxed) == 1u);
    REQUIRE(input_calls.load(std::memory_order_relaxed) == 0u);
    REQUIRE(captured_stderr.find("memory_budget_not_satisfiable")
            != std::string::npos);
    REQUIRE(captured_stderr.find(expected_reason) != std::string::npos);
    REQUIRE(captured_stderr.find("memory_budget_constraint_timeout")
            == std::string::npos);
}

TEST_CASE("worker exceptions fail through the explicit join and retain partial samples",
          "[component:benchmark_framework][layer:contract][feature:scheduling][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 1> algorithms{{
        {"throws_on_second_iteration",
         std::span<const algorithm_parameter_specification>{k_dummy_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"SMALL", "first iteration completes before the worker failure",
         R"({"length":8,"seed":89})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"SMALL", 2},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> over_entries{{
        {"throws_on_second_iteration", "{}"},
    }};
    static constexpr std::array<metric_specification, 1> metrics{{
        {"sum_value", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_worker_exception",
            .description = "worker exception ownership",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
    }};
    const std::string config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});
    temporary_report_directory report_dir;
    std::atomic<std::size_t> calls{0};
    const auto on_run = [&calls](std::string_view, const parameter_map&,
                                  const toy_input&) -> toy_output {
        if (calls.fetch_add(1, std::memory_order_relaxed) == 1) {
            throw std::runtime_error{"simulated-worker-failure"};
        }
        return toy_output{.sum = 89};
    };
    const auto on_evaluate = [](std::string_view, const parameter_map&,
                                 const toy_input&, const toy_output& output) {
        return std::map<std::string, double>{{
            "sum_value", static_cast<double>(output.sum)}};
    };

    const auto outcome = run_one_bench(
        "bench_worker_exception", "worker exception ownership", config_text,
        report_dir, build_toy_input, on_run, on_evaluate);

    // The runner must return the worker failure and keep the completed sample.
    // Throwing from the scheduler destructor would terminate the test instead.
    REQUIRE(calls.load(std::memory_order_relaxed) == 2);
    REQUIRE(outcome.exit_code == 1);
    REQUIRE_FALSE(outcome.report_path.empty());
    REQUIRE(stderr_contains(
        outcome.captured_stderr, "batch_worker_exception"));
    REQUIRE(stderr_contains(
        outcome.captured_stderr, "simulated-worker-failure"));
    REQUIRE(stderr_contains(
        outcome.captured_stderr, "Completed samples are retained"));
    REQUIRE(samples_count_at(
        outcome.report,
        "bench_worker_exception",
        "throws_on_second_iteration",
        "SMALL",
        "all_regions",
        "sum_value") == 1);
}

// End-to-end report and metric checks.

TEST_CASE("bench_framework end-to-end happy path",
          "[component:benchmark_framework][layer:contract][feature:e2e_driver][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    // Give this test its own counter name because the registry is shared
    // by every test in the process.
    const std::string counter_name = fmt::format(
        "test.bench_framework.line_{}.counter", __LINE__);
    const std::string_view k_counter_name{counter_name};

    static constexpr std::array<algorithm_parameter_specification, 1> k_sort_parameters{{
        {"size_hint", "integer", "0", ""}
    }};
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"toy_sort", std::span<const algorithm_parameter_specification>{k_sort_parameters}}
    }};
    static constexpr std::array<test_case_data_specification, 2> k_test_cases{{
        {"TINY",  "16 random ints", R"({"length":16,"seed":11})"},
        {"SMALL", "64 random ints", R"({"length":64,"seed":29})"}
    }};
    static constexpr std::array<iteration_count_entry, 2> k_for_entries{{
        {"TINY",  4},
        {"SMALL", 3}
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over_entries{{
        {"toy_sort", "{}"}
    }};
    // Use a different report label for the counter to test name mapping.
    // Omit region_visits because its constant value would trigger a warning
    // unrelated to the measurements checked here.
    const std::array<metric_specification, 4> metrics{{
        {"wall_clock_ns",         "builtin",     "",           "",                         "[]"},
        {"region_total_ns",       "region",      "",           "",                         "[]"},
        {"my_counter",            "counter",     "",           k_counter_name,               "[]"},
        {"sum_value",             "evaluation",  "",           "",                         "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_happy",
            .description = "happy path",
            .fast = false,
            .bench_lock = false,
            .concurrency = false,    // Keep counter updates serial.
            .by_regions = true,
            .for_entries = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};

    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;

    auto on_run = [&](std::string_view /*algo*/,
                       const parameter_map& /*params*/,
                       const toy_input& input) -> toy_output {
        static const auto* const sum_region =
            bioseq::instrumentation::register_static_region("toy_sort.sum");
        auto& counter = bioseq::instrumentation::get_named_counter(k_counter_name);
        toy_output output;
        {
            auto _scope = bioseq::instrumentation::region(sum_region);
            for (const auto value : input.values) {
                output.sum += value;
                if ((value & 1) == 0) ++output.parity_count;
            }
        }
        bioseq::instrumentation::add_to_counter<bioseq::instrumentation::benchmark_profiling_configuration>(
            counter, static_cast<std::int64_t>(input.values.size()));
        return output;
    };

    auto on_evaluate = [](std::string_view /*algo*/,
                           const parameter_map& /*params*/,
                           const toy_input& /*input*/,
                           const toy_output& output)
        -> std::map<std::string, double>
    {
        // Only sum_value is declared. The extra evaluator result should be ignored.
        return {
            {"sum_value",       static_cast<double>(output.sum)},
            {"unused_extra",    static_cast<double>(output.parity_count)},
        };
    };

    auto outcome = run_one_bench(
        "bench_happy", "happy path", config_text, report_dir,
        build_toy_input, on_run, on_evaluate);

    REQUIRE(outcome.exit_code == 0);
    REQUIRE_FALSE(outcome.report.empty());
    REQUIRE(outcome.report.contains("meta"));
    REQUIRE(outcome.report.contains("rule_violations"));
    REQUIRE(outcome.report.contains("regressions"));
    REQUIRE(outcome.report.contains("sanity_warnings"));
    REQUIRE(outcome.report.contains("benchmarks"));

    const auto& meta = outcome.report["meta"];
    REQUIRE(meta.contains("commit"));
    REQUIRE(meta.contains("dirty"));
    REQUIRE(meta.contains("timestamp"));
    REQUIRE(meta.contains("created"));
    REQUIRE(meta.contains("fast"));
    REQUIRE(meta.contains("history_eligible"));
    REQUIRE(meta.at("history_eligible") == true);
    auto promotion_path = outcome.report_path;
    promotion_path += ".history-promotion.tmp";
    REQUIRE_FALSE(std::filesystem::exists(promotion_path));
    {
        const auto commit = meta["commit"].get<std::string>();
        const bool empty_or_hex_40 =
            commit.empty()
            || (commit.size() == 40
                && std::all_of(commit.begin(), commit.end(), [](char ch) {
                    return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
                }));
        REQUIRE(empty_or_hex_40);
    }

    REQUIRE(outcome.report["rule_violations"].empty());
    REQUIRE(outcome.report["regressions"].empty());

    REQUIRE(samples_count_at(outcome.report, "bench_happy",
                              "toy_sort", "TINY",
                              "all_regions", "wall_clock_ns") == 4);
    REQUIRE(samples_count_at(outcome.report, "bench_happy",
                              "toy_sort", "SMALL",
                              "all_regions", "wall_clock_ns") == 3);
    REQUIRE(stat_at(outcome.report, "bench_happy",
                     "toy_sort", "TINY",
                     "all_regions", "wall_clock_ns", "min") >= 0.0);

    // Each TINY iteration adds 16 to the counter.
    // All four recorded deltas should therefore be 16.
    REQUIRE(samples_count_at(outcome.report, "bench_happy",
                              "toy_sort", "TINY",
                              "all_regions", "my_counter") == 4);
    REQUIRE(stat_at(outcome.report, "bench_happy",
                     "toy_sort", "TINY",
                     "all_regions", "my_counter", "min") == 16.0);
    REQUIRE(stat_at(outcome.report, "bench_happy",
                     "toy_sort", "TINY",
                     "all_regions", "my_counter", "max") == 16.0);

    // TINY uses the same input on every iteration, so every sum should match.
    REQUIRE(samples_count_at(outcome.report, "bench_happy",
                              "toy_sort", "TINY",
                              "all_regions", "sum_value") == 4);
    REQUIRE(stat_at(outcome.report, "bench_happy",
                     "toy_sort", "TINY",
                     "all_regions", "sum_value", "min")
            == stat_at(outcome.report, "bench_happy",
                        "toy_sort", "TINY",
                        "all_regions", "sum_value", "max"));

    // Separate region output includes the toy_sort.sum measurements.
    REQUIRE(samples_count_at(outcome.report, "bench_happy",
                              "toy_sort", "TINY",
                              "toy_sort.sum", "region_total_ns") == 4);

    // Undeclared evaluator keys should produce neither report cells nor errors.
    REQUIRE_FALSE(aggregate_at(outcome.report, "bench_happy",
                                "toy_sort", "TINY",
                                "all_regions", "unused_extra").has_value());
    REQUIRE_FALSE(stderr_contains(outcome.captured_stderr,
                                    "evaluation_key_not_declared"));

    // The two test cases produce different sums and counter values.
    // These metrics should pass the check for values stuck at one constant.
    const auto stuck_count = count_sanity_warnings_of_type(
        outcome.report, "metric_looks_constant");
    INFO("sanity_warnings dump: " << outcome.report["sanity_warnings"].dump());
    REQUIRE(stuck_count == 0);
}

TEST_CASE("raw-enabled benchmark embeds lossless payload objects in report",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& k_no_parameters = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 3> k_algorithms{{
        {"raw_a", std::span<const algorithm_parameter_specification>{k_no_parameters}},
        {"raw_b", std::span<const algorithm_parameter_specification>{k_no_parameters}},
        {"raw_c", std::span<const algorithm_parameter_specification>{k_no_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> k_test_cases{{
        {"ONE", "raw evidence", R"({"length":4})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for_entries{{{"ONE", 2}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 3> k_over_entries{{
        {"raw_a", "{}"},
        {"raw_b", "{}"},
        {"raw_c", "{}"},
    }};
    const std::array<metric_specification, 2> metrics{{
        {"wall_clock_ns", "builtin", "", "", "[]"},
        {"measured_region_ns_manual", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_raw",
            .description = "raw evidence",
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .for_entries = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    // A sidecar left by a crashed run still reserves report number 3.
    // The next run must use 4 and preserve the existing evidence.
    {
        std::ofstream orphan(
            report_dir.path / "report_3.raw.jsonl",
            std::ios::trunc | std::ios::binary);
        orphan << "{\"kind\":\"header\",\"schema\":1}\n";
    }
    set_raw_evidence_io_fault_for_testing(
        raw_evidence_io_fault::short_write);
    auto outcome = run_one_bench(
        "bench_raw", "raw evidence", config_text, report_dir,
        build_toy_input,
        [](std::string_view, const parameter_map&, const toy_input&) {
            return toy_output{.sum = 7};
        },
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) -> std::map<std::string, double> {
            return {{"measured_region_ns_manual",
                static_cast<double>(output.sum)}};
        },
        nullptr,
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) {
            return nlohmann::json{
                {"payload_schema", "test.raw"},
                {"payload_schema_version", 1},
                {"index_and_map_ns", output.sum},
                {"alignment_snapshot", {
                    {"expected_row_count", 20},
                    {"actual_row_count", 20},
                    {"expected_column_count", 142},
                }},
                {"sum", output.sum},
            }.dump();
        });
    set_raw_evidence_io_fault_for_testing(
        raw_evidence_io_fault::none);

    REQUIRE(outcome.report.contains("benchmarks"));
    REQUIRE(outcome.report_path.filename() == "report_4.json");
    REQUIRE(outcome.report["raw_evidence"]["complete"] == true);
    REQUIRE(outcome.report["raw_evidence"]["record_count"] == 6);
    REQUIRE(outcome.report["raw_evidence"]["path"]
        == "report_4.raw.jsonl");
    REQUIRE(outcome.report["raw_evidence"]["outer_e2e_ns"]["count"] == 6);
    REQUIRE(outcome.report["raw_evidence"]["index_and_map_ns"]["count"] == 6);
    REQUIRE(outcome.report["raw_evidence"]["index_and_map_ns"]["min"] == 7);
    REQUIRE(outcome.report["raw_evidence"]["index_and_map_ns"]["p50"] == 7);
    REQUIRE(outcome.report["raw_evidence"]["index_and_map_ns"]["mean"] == 7);
    REQUIRE(outcome.report["raw_evidence"]["index_and_map_ns"]["p95"] == 7);
    REQUIRE(outcome.report["raw_evidence"]["cells"].size() == 3);
    for (const auto& cell : outcome.report["raw_evidence"]["cells"]) {
        REQUIRE(cell["record_count"] == 2);
        REQUIRE(cell["first_repeat_outer_e2e_ns"]["count"] == 1);
        REQUIRE(cell["steady_state_outer_e2e_ns"]["count"] == 1);
        REQUIRE(cell["first_repeat_outer_e2e_ns"]["p50"].is_number_integer());
        REQUIRE(cell["steady_state_outer_e2e_ns"]["p50"].is_number_integer());
    }
    const auto& raw_samples =
        outcome.report["benchmarks"]["bench_raw"]["raw_samples"];
    REQUIRE(raw_samples.is_array());
    REQUIRE(raw_samples.size() == 6);
    const std::array<std::size_t, 6> expected_tuple_order{{2, 0, 1, 2, 1, 0}};
    const std::array<std::string_view, 3> expected_algorithms{{
        "raw_a", "raw_b", "raw_c",
    }};
    const std::array<std::string_view, 2> expected_shuffle_seeds{{
        "d0a0823f87782282", "c5b0e57c3e8b2409",
    }};
    for (std::size_t raw_id = 0; raw_id < raw_samples.size(); ++raw_id) {
        const auto repeat = raw_id / 3;
        const auto position = raw_id % 3;
        const auto& raw = raw_samples[raw_id];
        REQUIRE(raw["schema"] == 1);
        REQUIRE(raw["benchmark"] == "bench_raw");
        REQUIRE(raw["test_case"] == "ONE");
        REQUIRE(raw["algorithm"].get<std::string>()
            == expected_algorithms[expected_tuple_order[raw_id]]);
        REQUIRE(raw["repeat"] == repeat);
        REQUIRE(raw["payload_schema_version"] == 1);
        REQUIRE(raw["raw_record_id"] == raw_id);
        REQUIRE(raw["schedule"]["repeat"] == repeat);
        REQUIRE(raw["schedule"]["tuple_index"]
            == expected_tuple_order[raw_id]);
        REQUIRE(raw["schedule"]["benchmark_index"] == 0);
        REQUIRE(raw["schedule"]["order_position"] == position);
        REQUIRE(raw["schedule"]["configured_seed"] == "0000000000000172");
        REQUIRE(raw["schedule"]["shuffle_seed"].get<std::string>()
            == expected_shuffle_seeds[repeat]);
        REQUIRE(raw["record_checksum"].get<std::string>().size() == 16);
        REQUIRE(raw["alignment_snapshot"]["actual_row_count"] == 20);
        REQUIRE(raw["alignment_snapshot"]["expected_column_count"] == 142);
        REQUIRE(raw["sum"] == 7);
    }

    const auto sidecar_path = report_dir.path / "report_4.raw.jsonl";
    std::ifstream sidecar(sidecar_path, std::ios::binary);
    REQUIRE(sidecar.good());
    std::vector<nlohmann::json> lines;
    for (std::string line; std::getline(sidecar, line); ) {
        auto parsed = nlohmann::json::parse(line, nullptr, false);
        REQUIRE_FALSE(parsed.is_discarded());
        lines.push_back(std::move(parsed));
    }
    REQUIRE(lines.size() == 8);
    REQUIRE(lines.front()["kind"] == "header");
    REQUIRE(lines.front().size() == 3);
    for (std::size_t index = 0; index < raw_samples.size(); ++index) {
        REQUIRE(lines[index + 1] == raw_samples[index]);
    }
    REQUIRE(lines.back()["kind"] == "footer");
    REQUIRE(lines.back().size() == 3);
    REQUIRE(lines.back()["manifest"] == outcome.report["raw_evidence"]);
    REQUIRE(std::filesystem::file_size(sidecar_path)
        == outcome.report["raw_evidence"]["prefix_bytes"]
                .get<std::uint64_t>()
            + lines.back().dump().size() + 1);

    const auto terminal_complete = [](const std::filesystem::path& path)
            -> std::optional<bool> {
        std::ifstream input(path, std::ios::binary);
        std::string line;
        std::string terminal;
        while (std::getline(input, line)) {
            if (!line.empty()) terminal = line;
        }
        const auto parsed = nlohmann::json::parse(
            terminal, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()
            || parsed.value("kind", std::string{}) != "footer"
            || !parsed.contains("manifest")
            || !parsed["manifest"].contains("complete")) {
            return std::nullopt;
        }
        return parsed["manifest"]["complete"].get<bool>();
    };
    const auto footer_completion_states = [](
            const std::filesystem::path& path) {
        std::vector<bool> states;
        std::ifstream input(path, std::ios::binary);
        for (std::string line; std::getline(input, line); ) {
            const auto parsed = nlohmann::json::parse(
                line, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()
                && parsed.value("kind", std::string{}) == "footer"
                && parsed.contains("manifest")
                && parsed["manifest"].contains("complete")) {
                states.push_back(
                    parsed["manifest"]["complete"].get<bool>());
            }
        }
        return states;
    };
    REQUIRE(footer_completion_states(sidecar_path)
        == std::vector<bool>{true});

    std::atomic<int> cleanup_calls{0};
    temporary_report_directory failure_dir;
    set_raw_evidence_io_fault_for_testing(
        raw_evidence_io_fault::sync_failure);
    auto failed = run_one_bench(
        "bench_raw", "raw evidence", config_text, failure_dir,
        build_toy_input,
        [](std::string_view, const parameter_map&, const toy_input&) {
            return toy_output{.sum = 7};
        },
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) -> std::map<std::string, double> {
            return {{"measured_region_ns_manual",
                static_cast<double>(output.sum)}};
        },
        [&cleanup_calls](std::string_view, std::string_view,
            simdjson::dom::element, const toy_input&) {
            cleanup_calls.fetch_add(1, std::memory_order_relaxed);
        },
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) {
            return nlohmann::json{
                {"index_and_map_ns", output.sum},
                {"sum", output.sum},
            }.dump();
        });
    set_raw_evidence_io_fault_for_testing(
        raw_evidence_io_fault::none);
    REQUIRE(failed.exit_code == 1);
    REQUIRE(cleanup_calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(failed.captured_stderr.find("raw_evidence_append_failed")
        != std::string::npos);
    REQUIRE(failed.report["raw_evidence"]["complete"] == false);
    REQUIRE(terminal_complete(
        failure_dir.path / "report_1.raw.jsonl") == false);
    REQUIRE(footer_completion_states(
        failure_dir.path / "report_1.raw.jsonl")
        == std::vector<bool>{false});

    std::atomic<int> partial_cleanup_calls{0};
    temporary_report_directory partial_dir;
    set_raw_evidence_io_fault_for_testing(
        raw_evidence_io_fault::partial_write_failure);
    auto partial = run_one_bench(
        "bench_raw", "raw evidence", config_text, partial_dir,
        build_toy_input,
        [](std::string_view, const parameter_map&, const toy_input&) {
            return toy_output{.sum = 7};
        },
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) -> std::map<std::string, double> {
            return {{"measured_region_ns_manual",
                static_cast<double>(output.sum)}};
        },
        [&partial_cleanup_calls](std::string_view, std::string_view,
            simdjson::dom::element, const toy_input&) {
            partial_cleanup_calls.fetch_add(1, std::memory_order_relaxed);
        },
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) {
            return nlohmann::json{
                {"index_and_map_ns", output.sum},
                {"sum", output.sum},
            }.dump();
        });
    set_raw_evidence_io_fault_for_testing(
        raw_evidence_io_fault::none);
    REQUIRE(partial.exit_code == 1);
    REQUIRE(partial_cleanup_calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(partial.captured_stderr.find("raw_evidence_append_failed")
        != std::string::npos);
    REQUIRE(partial.report["raw_evidence"]["complete"] == false);
    REQUIRE(terminal_complete(
        partial_dir.path / "report_1.raw.jsonl") == false);
    REQUIRE(footer_completion_states(
        partial_dir.path / "report_1.raw.jsonl")
        == std::vector<bool>{false});

    temporary_report_directory tamper_dir;
    std::atomic<int> tamper_cleanup_calls{0};
    auto tampered = run_one_bench(
        "bench_raw", "raw evidence", config_text, tamper_dir,
        build_toy_input,
        [](std::string_view, const parameter_map&, const toy_input&) {
            return toy_output{.sum = 7};
        },
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) -> std::map<std::string, double> {
            return {{"measured_region_ns_manual",
                static_cast<double>(output.sum)}};
        },
        [&tamper_dir, &tamper_cleanup_calls](
            std::string_view, std::string_view,
            simdjson::dom::element, const toy_input&) {
            tamper_cleanup_calls.fetch_add(1, std::memory_order_relaxed);
            std::ofstream append(
                tamper_dir.path / "report_1.raw.jsonl",
                std::ios::app | std::ios::binary);
            append << "malformed-interior-line\n";
        },
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) {
            return nlohmann::json{
                {"index_and_map_ns", output.sum},
                {"sum", output.sum},
            }.dump();
        });
    REQUIRE(tampered.exit_code == 1);
    REQUIRE(tamper_cleanup_calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(tampered.report["raw_evidence"]["complete"] == false);
    REQUIRE(terminal_complete(
        tamper_dir.path / "report_1.raw.jsonl") == false);
    // Corrupt raw data must prevent a complete=true footer.
    REQUIRE(footer_completion_states(
        tamper_dir.path / "report_1.raw.jsonl")
        == std::vector<bool>{false});
}

TEST_CASE("iteration_order parser rejects ambiguous seed contracts",
          "[component:benchmark_framework][layer:contract][feature:scheduling][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    const auto run_config = [](nlohmann::json order) {
        nlohmann::json config = nlohmann::json::parse(R"json({
          "params": {
            "algo": {"unused": {"type": "integer", "default": 0}}
          },
          "test_cases": {
            "ONE": {
              "description": "order parser",
              "concurrency": 1,
              "data": {"length": 4}
            }
          },
          "bench_order": {
            "description": "order parser",
            "tags": [],
            "concurrency": {"bench_lock": true, "concurrency": false},
            "matrix": {
              "by_regions": false,
              "for": {"ONE": 1},
              "over": {"algo": {}},
              "metrics": {
                "sum_value": {"type": "evaluation", "rules": []}
              }
            }
          }
        })json");
        config["bench_order"]["iteration_order"] = std::move(order);
        temporary_report_directory report_dir;
        return run_one_bench(
            "bench_order", "order parser", config.dump(), report_dir,
            build_toy_input,
            [](std::string_view, const parameter_map&, const toy_input&) {
                return toy_output{.sum = 1};
            },
            [](std::string_view, const parameter_map&, const toy_input&,
               const toy_output& output) -> std::map<std::string, double> {
                return {{"sum_value", static_cast<double>(output.sum)}};
            });
    };

    SECTION("interleaved_seeded requires an explicit seed") {
        const auto outcome = run_config({{"mode", "interleaved_seeded"}});
        REQUIRE(outcome.exit_code == 1);
    }
    SECTION("item_major forbids a seed") {
        const auto outcome = run_config({
            {"mode", "item_major"}, {"seed", 370}});
        REQUIRE(outcome.exit_code == 1);
    }
    SECTION("interleaved_seeded accepts an unsigned explicit seed") {
        const auto outcome = run_config({
            {"mode", "interleaved_seeded"}, {"seed", 370}});
        REQUIRE(outcome.exit_code == 0);
    }
}

// A failed timing comparison must appear in the report.

TEST_CASE("bench_framework rule violation fires on threshold",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& k_no_parameters = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 2> k_algorithms{{
        {"slow_algo", std::span<const algorithm_parameter_specification>{k_no_parameters}},
        {"fast_algo", std::span<const algorithm_parameter_specification>{k_no_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> k_test_cases{{
        {"ONE", "one case", R"({"length":4})"}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for_entries{{ {"ONE", 5} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> k_over_entries{{
        {"slow_algo", "{}"},
        {"fast_algo", "{}"},
    }};
    // Make slow_algo fail the rule that requires it to be faster than fast_algo.
    static constexpr std::string_view k_rules_json =
        R"([["slow_under_fast",{"key":"slow_algo"},{"compare":"<","threshold%":0,"tolerance%":0,"aggregate":"p50"},{"key":"fast_algo"}]])";
    const std::array<metric_specification, 1> metrics{{
        {"wall_clock_ns", "builtin", "", "", k_rules_json},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_compare",
            .description = "rule",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};

    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;

    // Give slow_algo a clear timing difference by busy-waiting for 200 microseconds.
    // A busy wait keeps the worker active throughout the delay.
    constexpr std::int64_t k_slow_algorithm_spin_ns = 200'000;
    auto on_run = [](std::string_view algo,
                      const parameter_map& /*params*/,
                      const toy_input& /*input*/) -> toy_output {
        if (algo == "slow_algo") {
            const auto start = bioseq::instrumentation::clock::now_ns();
            while (bioseq::instrumentation::clock::now_ns() - start < k_slow_algorithm_spin_ns) {
                /* spin */
            }
        }
        return toy_output{};
    };

    auto on_evaluate = [](std::string_view, const parameter_map&,
                           const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {}; };

    auto outcome = run_one_bench(
        "bench_compare", "rule violation", config_text, report_dir,
        build_toy_input, on_run, on_evaluate);

    REQUIRE(outcome.exit_code == 1);
    REQUIRE(outcome.report.contains("rule_violations"));
    const auto& violations = outcome.report["rule_violations"];
    REQUIRE(violations.size() == 1);
    const auto& violation = violations[0];
    REQUIRE(violation["benchmark"].get<std::string>() == "bench_compare");
    REQUIRE(violation["rule_name"].get<std::string>() == "slow_under_fast");
    REQUIRE(violation["test_case"].get<std::string>() == "ONE");
    REQUIRE(violation["metric"].get<std::string>() == "wall_clock_ns");
    REQUIRE(violation["args"].size() == 2);
    // The recorded medians must explain why the comparison failed.
    REQUIRE(violation["args"][0].get<double>() >= violation["args"][1].get<double>());
}

TEST_CASE("paired rules aggregate repeat-wise ratios and fail closed on schedule",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& no_params = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"lhs", std::span<const algorithm_parameter_specification>{no_params}},
        {"rhs", std::span<const algorithm_parameter_specification>{no_params}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"ONE", "one paired group", R"({"length":1})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> three_repeats{{{"ONE", 3}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> over_entries{{
        {"lhs", "{}"}, {"rhs", "{}"},
    }};

    const auto run_fixture = [&](
        const std::string_view rules_json,
        const bool bench_lock,
        const std::array<std::int64_t, 3>& lhs_values,
        const std::array<std::int64_t, 3>& rhs_values) {
        const std::array<metric_specification, 1> metrics{{
            {"synthetic_ns", "evaluation", "", "", rules_json},
        }};
        const std::array<benchmark_specification, 1> benches{{
            benchmark_specification{
                .key = "bench_paired",
                .description = "paired rule fixture",
                .bench_lock = bench_lock,
                .concurrency = false,
                .iteration_order_mode = "interleaved_seeded",
                .iteration_order_seed = 370,
                .for_entries = std::span<const iteration_count_entry>{three_repeats},
                .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
                .metrics = std::span<const metric_specification>{metrics},
            },
        }};
        const auto config_text = render_config(
            std::span<const algorithm_specification>{algorithms},
            std::span<const test_case_data_specification>{test_cases},
            std::span<const benchmark_specification>{benches});

        auto call_counts =
            std::make_shared<std::map<std::string, std::size_t>>();
        const auto on_run = [call_counts, lhs_values, rhs_values](
            const std::string_view algorithm,
            const parameter_map& /*params*/,
            const toy_input& /*input*/) -> toy_output {
            auto& ordinal = (*call_counts)[std::string{algorithm}];
            const auto& values = algorithm == "lhs" ? lhs_values : rhs_values;
            const auto bounded = std::min(ordinal, values.size() - 1);
            ++ordinal;
            return toy_output{.sum = values[bounded]};
        };
        const auto on_evaluate = [](
            std::string_view, const parameter_map&, const toy_input&,
            const toy_output& output) -> std::map<std::string, double> {
            return {{"synthetic_ns", static_cast<double>(output.sum)}};
        };
        auto report_dir = std::make_unique<temporary_report_directory>();
        auto outcome = run_one_bench(
            "bench_paired", "paired rule fixture", config_text, *report_dir,
            build_toy_input, on_run, on_evaluate);
        return std::pair{std::move(report_dir), std::move(outcome)};
    };

    static constexpr std::string_view independent_rules = R"json([
      ["independent_p50",{"key":"lhs"},{"compare":"<","tolerance%":10,"aggregate":"p50"},{"key":"rhs"}]
    ])json";
    static constexpr std::string_view paired_rules = R"json([
      ["paired_p50",{"key":"lhs"},{"compare":"<","tolerance%":10,"aggregate":"p50","pairing":"by_iteration_ratio","blocking":true},{"key":"rhs"}]
    ])json";
    const std::array<std::int64_t, 3> lhs_values{{60, 70, 1}};
    const std::array<std::int64_t, 3> rhs_values{{50, 60, 100}};

    // Both independent medians are 60, which passes the 10% tolerance.
    // Pairing by repeat gives ratios 1.2, 70/60 and 0.01. Their median fails.
    auto [independent_dir, independent] = run_fixture(
        independent_rules, true, lhs_values, rhs_values);
    (void)independent_dir;
    REQUIRE(independent.exit_code == 0);
    REQUIRE(independent.report["rule_violations"].empty());

    auto [paired_dir, paired] = run_fixture(
        paired_rules, true, lhs_values, rhs_values);
    (void)paired_dir;
    REQUIRE(paired.exit_code == 1);
    REQUIRE(paired.report["rule_violations"].size() == 1);
    REQUIRE(paired.report["meta"]["history_eligible"] == false);
    const auto& violation = paired.report["rule_violations"][0];
    REQUIRE(violation["comparison_mode"] == "paired_iteration_ratio");
    REQUIRE(violation["warmup_repetitions"] == 0);
    REQUIRE(violation["args"][0].get<double>()
            == Catch::Approx(70.0 / 60.0));
    REQUIRE(violation["args"][1] == 1.0);
    REQUIRE(paired.report["paired_rule_evidence"].size() == 1);
    const auto& paired_evidence =
        paired.report["paired_rule_evidence"][0];
    REQUIRE(paired_evidence["rule_name"] == "paired_p50");
    REQUIRE(paired_evidence["numerator_route"]["region"]
            == "all_regions");
    REQUIRE(paired_evidence["denominator_route"]["region"]
            == "all_regions");
    REQUIRE(paired_evidence["observed_paired_ratio"].get<double>()
            == Catch::Approx(70.0 / 60.0));
    REQUIRE(paired_evidence["accepted"] == false);

    static constexpr std::string_view independent_min_equality_rules = R"json([
      ["independent_min_equality",{"key":"lhs"},{"compare":"=","tolerance%":0,"aggregate":"min"},{"key":"rhs"}]
    ])json";
    static constexpr std::string_view exact_iteration_rules = R"json([
      ["exact_iteration_equality",{"key":"lhs"},{"compare":"=","tolerance%":0,"aggregate":"min","pairing":"by_iteration_exact"},{"key":"rhs"}]
    ])json";
    const std::array<std::int64_t, 3> equal_min_lhs{{10, 10, 11}};
    const std::array<std::int64_t, 3> equal_min_rhs{{10, 10, 12}};

    // Both minima are 10, but the last repeat differs: 11 versus 12.
    // Exact pairing must report those unequal values.
    auto [equal_min_dir, equal_min] = run_fixture(
        independent_min_equality_rules, true,
        equal_min_lhs, equal_min_rhs);
    (void)equal_min_dir;
    REQUIRE(equal_min.exit_code == 0);
    REQUIRE(equal_min.report["rule_violations"].empty());

    auto [exact_dir, exact] = run_fixture(
        exact_iteration_rules, true, equal_min_lhs, equal_min_rhs);
    (void)exact_dir;
    REQUIRE(exact.exit_code == 1);
    REQUIRE(exact.report["rule_violations"].size() == 1);
    const auto& exact_violation = exact.report["rule_violations"][0];
    REQUIRE(exact_violation["comparison_mode"] == "paired_iteration_exact");
    REQUIRE(exact_violation["warmup_repetitions"] == 0);
    REQUIRE(exact_violation["args"]
            == nlohmann::json::array({11.0, 12.0}));
    REQUIRE(exact_violation["args_present"]
            == nlohmann::json::array({true, true}));

    auto [exact_equal_dir, exact_equal] = run_fixture(
        exact_iteration_rules, true,
        std::array<std::int64_t, 3>{{0, 10, 12}},
        std::array<std::int64_t, 3>{{0, 10, 12}});
    (void)exact_equal_dir;
    REQUIRE(exact_equal.exit_code == 0);
    REQUIRE(exact_equal.report["rule_violations"].empty());

    static constexpr std::string_view invalid_exact_tolerance_rules = R"json([
      ["invalid_exact_tolerance",{"key":"lhs"},{"compare":"=","tolerance%":1,"aggregate":"min","pairing":"by_iteration_exact"},{"key":"rhs"}]
    ])json";
    auto [invalid_exact_dir, invalid_exact] = run_fixture(
        invalid_exact_tolerance_rules, true,
        equal_min_lhs, equal_min_rhs);
    (void)invalid_exact_dir;
    REQUIRE(invalid_exact.exit_code == 1);
    REQUIRE(stderr_contains(
        invalid_exact.captured_stderr,
        "exact_iteration_rule_requires_zero_tolerance_equality"));

    static constexpr std::string_view diagnostic_rules = R"json([
      ["paired_diagnostic",{"key":"lhs"},{"compare":"<","tolerance%":10,"aggregate":"p50","pairing":"by_iteration_ratio","blocking":false},{"key":"rhs"}]
    ])json";
    auto [diagnostic_dir, diagnostic] = run_fixture(
        diagnostic_rules, true, lhs_values, rhs_values);
    (void)diagnostic_dir;
    REQUIRE(diagnostic.exit_code == 0);
    REQUIRE(diagnostic.report["rule_violations"].size() == 1);
    REQUIRE(diagnostic.report["rule_violations"][0]["blocking"] == false);
    REQUIRE(diagnostic.report["meta"]["history_eligible"] == true);

    // Paired comparisons require bench_lock so other benchmarks cannot overlap.
    auto [invalid_dir, invalid] = run_fixture(
        paired_rules, false, lhs_values, rhs_values);
    (void)invalid_dir;
    REQUIRE(invalid.exit_code == 1);
    REQUIRE(stderr_contains(
        invalid.captured_stderr,
        "paired_rule_requires_serial_interleaving"));

    auto [zero_dir, zero_denominator] = run_fixture(
        paired_rules, true, lhs_values,
        std::array<std::int64_t, 3>{{0, 0, 0}});
    (void)zero_dir;
    REQUIRE(zero_denominator.exit_code == 1);
    REQUIRE(zero_denominator.report["rule_violations"].size() == 1);
    REQUIRE(zero_denominator.report["rule_violations"][0]
                ["args_present"] == nlohmann::json::array({false, false}));

    static constexpr std::string_view warm_rules = R"json([
      ["paired_max_after_cold",{"key":"lhs"},{"compare":"<","aggregate":"max","pairing":"by_iteration_ratio","warmup_repetitions":1},{"key":"rhs"}]
    ])json";
    auto [warm_dir, warm] = run_fixture(
        warm_rules, true,
        std::array<std::int64_t, 3>{{1000, 50, 50}},
        std::array<std::int64_t, 3>{{1, 100, 100}});
    (void)warm_dir;
    REQUIRE(warm.exit_code == 0);
    REQUIRE(warm.report["rule_violations"].empty());
    REQUIRE(warm.report["paired_rule_evidence"].size() == 1);
    const auto& warm_evidence = warm.report["paired_rule_evidence"][0];
    REQUIRE(warm_evidence["rule_name"] == "paired_max_after_cold");
    REQUIRE(warm_evidence["comparison_mode"]
            == "paired_iteration_ratio");
    REQUIRE(warm_evidence["metric"] == "synthetic_ns");
    REQUIRE(warm_evidence["numerator_route"]["region"] == "all_regions");
    REQUIRE(warm_evidence["denominator_route"]["region"] == "all_regions");
    REQUIRE(warm_evidence["comparator"]["warmup_repetitions"] == 1);
    REQUIRE(warm_evidence["inputs_present"]
            == nlohmann::json::array({true, true}));
    REQUIRE(warm_evidence["repeats"].size() == 3);
    REQUIRE(warm_evidence["repeats"][0]["included_after_warmup"]
            == false);
    REQUIRE(warm_evidence["repeats"][1]["included_after_warmup"]
            == true);
    REQUIRE(warm_evidence["repeats"][2]["included_after_warmup"]
            == true);
    for (const auto& repeat : warm_evidence["repeats"]) {
        REQUIRE(repeat["numerator_samples"].size() == 1);
        REQUIRE(repeat["denominator_samples"].size() == 1);
        REQUIRE(repeat["numerator_schedule"].is_object());
        REQUIRE(repeat["denominator_schedule"].is_object());
        REQUIRE(repeat["schedule_identity_matches"] == true);
    }
    REQUIRE(warm_evidence["observed_paired_ratio"].get<double>()
            == Catch::Approx(0.5));
    REQUIRE(warm_evidence["accepted"] == true);

    // Reject a corrupted shuffle seed even when every numeric ratio would pass.
    // The recorded schedule must also match the configured run.
    {
        paired_schedule_evidence_fault_reset reset_fault;
        set_paired_schedule_evidence_fault_for_testing(
            paired_schedule_evidence_fault::wrong_shuffle_seed);
        auto [corrupt_dir, corrupt] = run_fixture(
            paired_rules, true,
            std::array<std::int64_t, 3>{{50, 50, 50}},
            std::array<std::int64_t, 3>{{100, 100, 100}});
        (void)corrupt_dir;
        REQUIRE(corrupt.exit_code == 1);
        REQUIRE(corrupt.report["paired_rule_evidence"].size() == 1);
        const auto& corrupt_evidence =
            corrupt.report["paired_rule_evidence"][0];
        REQUIRE(corrupt_evidence["numerator_route"]["region"]
                == "all_regions");
        REQUIRE(corrupt_evidence["denominator_route"]["region"]
                == "all_regions");
        REQUIRE(corrupt_evidence["inputs_present"]
                == nlohmann::json::array({true, true}));
        REQUIRE(corrupt_evidence["observed_paired_ratio"].is_null());
        REQUIRE(corrupt_evidence["accepted"] == false);
        for (const auto& repeat : corrupt_evidence["repeats"]) {
            REQUIRE(repeat["schedule_identity_matches"] == false);
        }
        REQUIRE(corrupt.report["rule_violations"].size() == 1);
        REQUIRE(corrupt.report["rule_violations"][0]["args_present"]
                == nlohmann::json::array({false, false}));
    }
}

TEST_CASE("paired named-region rules rebuild duplicate descriptors per repeat",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& no_params = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"lhs", std::span<const algorithm_parameter_specification>{no_params}},
        {"rhs", std::span<const algorithm_parameter_specification>{no_params}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"ONE", "one paired group", R"({"length":1})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{{"ONE", 3}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> over_entries{{
        {"lhs", "{}"}, {"rhs", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> acceptance_rows{{
        "lhs", "rhs",
    }};
    static constexpr std::array<named_region_sample_multiplicity_specification, 2>
        exact_named_samples{{
            {"lhs", "ONE", "paired.region.lhs", "region_total_ns", 3},
            {"rhs", "ONE", "paired.region.rhs", "region_total_ns", 6},
        }};
    static constexpr std::string_view rules = R"json([
      ["paired_named_total",
       {"key":"lhs","test_case":"ONE","region":"paired.region.lhs"},
       {"compare":"<","threshold%":25,"aggregate":"p50","pairing":"by_iteration_ratio","blocking":true},
       {"key":"rhs","test_case":"ONE","region":"paired.region.rhs"}]
    ])json";
    const std::array<metric_specification, 1> metrics{{
        {"region_total_ns", "region", "", "", rules},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_paired_regions",
            .description = "paired named region fixture",
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .by_regions = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const iteration_count_entry>{for_entries},
            .acceptance_require_named_region_iteration_coverage = true,
            .acceptance_exact_named_region_sample_multiplicities =
                std::span<const named_region_sample_multiplicity_specification>{
                    exact_named_samples},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});

    const auto spin_for = [](const std::int64_t duration_ns) {
        const auto start = bioseq::instrumentation::clock::now_ns();
        while (bioseq::instrumentation::clock::now_ns() - start < duration_ns) {
            /* spin */
        }
    };
    const auto on_run = [spin_for](
        const std::string_view algorithm,
        const parameter_map&, const toy_input&) -> toy_output {
        static const auto* const lhs =
            bioseq::instrumentation::register_static_region("paired.region.lhs");
        static const auto* const rhs_a =
            bioseq::instrumentation::register_static_region("paired.region.rhs");
        static const auto* const rhs_b =
            bioseq::instrumentation::register_static_region("paired.region.rhs");
        if (algorithm == "lhs") {
            auto scope = bioseq::instrumentation::region(lhs);
            spin_for(50'000);
        } else {
            {
                auto scope = bioseq::instrumentation::region(rhs_a);
                spin_for(200'000);
            }
            {
                auto scope = bioseq::instrumentation::region(rhs_b);
                spin_for(200'000);
            }
        }
        return {};
    };
    const auto on_evaluate = [](
        std::string_view, const parameter_map&, const toy_input&,
        const toy_output&) -> std::map<std::string, double> {
        return {};
    };

    temporary_report_directory report_dir;
    const auto outcome = run_one_bench(
        "bench_paired_regions", "paired named region fixture",
        config_text, report_dir, build_toy_input, on_run, on_evaluate);
    REQUIRE(outcome.exit_code == 0);
    REQUIRE(outcome.report["rule_violations"].empty());
    REQUIRE(samples_count_at(
        outcome.report, "bench_paired_regions", "lhs", "ONE",
        "paired.region.lhs", "region_total_ns") == 3);
    REQUIRE(samples_count_at(
        outcome.report, "bench_paired_regions", "rhs", "ONE",
        "paired.region.rhs", "region_total_ns") == 6);

    REQUIRE(outcome.report.contains("paired_rule_evidence"));
    REQUIRE(outcome.report["paired_rule_evidence"].size() == 1);
    const auto& evidence = outcome.report["paired_rule_evidence"][0];
    REQUIRE(evidence["benchmark"] == "bench_paired_regions");
    REQUIRE(evidence["rule_name"] == "paired_named_total");
    REQUIRE(evidence["comparison_mode"] == "paired_iteration_ratio");
    REQUIRE(evidence["numerator_route"]["algorithm"] == "lhs");
    REQUIRE(evidence["numerator_route"]["region"]
            == "paired.region.lhs");
    REQUIRE(evidence["denominator_route"]["algorithm"] == "rhs");
    REQUIRE(evidence["denominator_route"]["region"]
            == "paired.region.rhs");
    REQUIRE(evidence["comparator"]["operator"] == "<");
    REQUIRE(evidence["comparator"]["threshold_pct"] == 25.0);
    REQUIRE(evidence["comparator"]["tolerance_pct"] == 0.0);
    REQUIRE(evidence["comparator"]["paired_baseline"] == 1.0);
    REQUIRE(evidence["schedule_identity"]["mode"]
            == "interleaved_seeded");
    REQUIRE(evidence["schedule_identity"]["configured_seed"]
            == "0000000000000172");
    REQUIRE(evidence["inputs_present"]
            == nlohmann::json::array({true, true}));
    REQUIRE(evidence["repeats"].size() == 3);
    const std::string configured_aggregate =
        evidence["comparator"]["aggregate"].get<std::string>();

    std::vector<double> reconstructed_ratios;
    for (std::size_t repeat = 0; repeat < 3; ++repeat) {
        const auto& repeat_evidence = evidence["repeats"][repeat];
        REQUIRE(repeat_evidence["repeat"] == repeat);
        REQUIRE(repeat_evidence["included_after_warmup"] == true);
        REQUIRE(repeat_evidence["numerator_samples"].size() == 1);
        REQUIRE(repeat_evidence["denominator_samples"].size() == 2);
        const double numerator =
            repeat_evidence["numerator_value"].get<double>();
        const double denominator =
            repeat_evidence["denominator_value"].get<double>();
        const double ratio = repeat_evidence["ratio"].get<double>();
        const double reconstructed_numerator =
            reconstruct_paired_evidence_aggregate(
                repeat_evidence["numerator_samples"],
                configured_aggregate);
        const double reconstructed_denominator =
            reconstruct_paired_evidence_aggregate(
                repeat_evidence["denominator_samples"],
                configured_aggregate);
        const double reconstructed_ratio = reconstructed_numerator
            / reconstructed_denominator;
        REQUIRE(numerator == Catch::Approx(reconstructed_numerator));
        REQUIRE(denominator == Catch::Approx(reconstructed_denominator));
        REQUIRE(ratio == Catch::Approx(reconstructed_ratio));
        reconstructed_ratios.push_back(reconstructed_ratio);

        REQUIRE(repeat_evidence["schedule_identity_matches"] == true);
        const auto& numerator_schedule =
            repeat_evidence["numerator_schedule"];
        const auto& denominator_schedule =
            repeat_evidence["denominator_schedule"];
        REQUIRE(numerator_schedule["repeat"] == repeat);
        REQUIRE(denominator_schedule["repeat"] == repeat);
        REQUIRE(numerator_schedule["shuffle_seed"]
                == denominator_schedule["shuffle_seed"]);
        REQUIRE(numerator_schedule["order_position"]
                != denominator_schedule["order_position"]);
    }
    const double reconstructed_observed =
        reconstruct_paired_evidence_aggregate(
            reconstructed_ratios, configured_aggregate);
    REQUIRE(evidence["observed_paired_ratio"].get<double>()
            == Catch::Approx(reconstructed_observed));
    const double acceptance_boundary =
        evidence["comparator"]["paired_baseline"].get<double>()
        * (1.0
           - evidence["comparator"]["threshold_pct"].get<double>() / 100.0
           + evidence["comparator"]["tolerance_pct"].get<double>() / 100.0);
    REQUIRE(evidence["accepted"].get<bool>()
            == (reconstructed_observed < acceptance_boundary));
}

TEST_CASE("paired named-stage evidence rejects corrupted scheduler coordinates",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    paired_schedule_evidence_fault_reset reset_fault;

    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"schedule_lhs", std::span<const algorithm_parameter_specification>{k_dummy_parameters}},
        {"schedule_rhs", std::span<const algorithm_parameter_specification>{k_dummy_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"PAIR", "corrupted schedule evidence", R"({"length":1})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{{"PAIR", 2}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> over_entries{{
        {"schedule_lhs", "{}"}, {"schedule_rhs", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> acceptance_rows{{
        "schedule_lhs", "schedule_rhs",
    }};
    static constexpr std::array<named_region_sample_multiplicity_specification, 2>
        exact_named_samples{{
            {"schedule_lhs", "PAIR", "paired.schedule.lhs",
             "region_total_ns", 2},
            {"schedule_rhs", "PAIR", "paired.schedule.rhs",
             "region_total_ns", 2},
        }};
    static constexpr std::string_view rules = R"json([
      ["schedule_must_be_authentic",
       {"key":"schedule_lhs","test_case":"PAIR","region":"paired.schedule.lhs"},
       {"compare":"<","threshold%":50,"aggregate":"p50","pairing":"by_iteration_ratio","blocking":true},
       {"key":"schedule_rhs","test_case":"PAIR","region":"paired.schedule.rhs"}]
    ])json";
    const std::array<metric_specification, 1> metrics{{
        {"region_total_ns", "region", "", "", rules},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_corrupted_paired_schedule",
            .description = "corrupted paired schedule evidence",
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .by_regions = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const iteration_count_entry>{for_entries},
            .acceptance_require_named_region_iteration_coverage = true,
            .acceptance_exact_named_region_sample_multiplicities =
                std::span<const named_region_sample_multiplicity_specification>{
                    exact_named_samples},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});
    const auto spin_for = [](const std::int64_t duration_ns) {
        const auto start = bioseq::instrumentation::clock::now_ns();
        while (bioseq::instrumentation::clock::now_ns() - start < duration_ns) {
            /* spin */
        }
    };
    const auto on_run = [spin_for](
        const std::string_view algorithm,
        const parameter_map&, const toy_input&) -> toy_output {
        static const auto* const lhs =
            bioseq::instrumentation::register_static_region("paired.schedule.lhs");
        static const auto* const rhs =
            bioseq::instrumentation::register_static_region("paired.schedule.rhs");
        const auto* descriptor = algorithm == "schedule_lhs" ? lhs : rhs;
        auto scope = bioseq::instrumentation::region(descriptor);
        spin_for(algorithm == "schedule_lhs" ? 30'000 : 500'000);
        return {};
    };
    const auto on_evaluate = [](
        std::string_view, const parameter_map&, const toy_input&,
        const toy_output&) -> std::map<std::string, double> {
        return {};
    };

    const std::array faults{
        paired_schedule_evidence_fault::wrong_shuffle_seed,
        paired_schedule_evidence_fault::
            wrong_first_tuple_order_position,
    };
    for (const auto fault : faults) {
        set_paired_schedule_evidence_fault_for_testing(fault);
        temporary_report_directory report_dir;
        const auto outcome = run_one_bench(
            "bench_corrupted_paired_schedule",
            "corrupted paired schedule evidence",
            config_text, report_dir, build_toy_input, on_run, on_evaluate);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(outcome.report["paired_rule_evidence"].size() == 1);
        const auto& evidence = outcome.report["paired_rule_evidence"][0];
        REQUIRE(evidence["inputs_present"]
                == nlohmann::json::array({true, true}));
        REQUIRE(evidence["observed_paired_ratio"].is_null());
        REQUIRE(evidence["accepted"] == false);
        REQUIRE(evidence["repeats"].size() == 2);
        for (const auto& repeat : evidence["repeats"]) {
            REQUIRE(repeat["schedule_identity_matches"] == false);
        }
        REQUIRE(outcome.report["rule_violations"].size() == 1);
        REQUIRE(outcome.report["rule_violations"][0]["rule_name"]
                == "schedule_must_be_authentic");
        REQUIRE(outcome.report["rule_violations"][0]["args_present"]
                == nlohmann::json::array({false, false}));
    }
}

TEST_CASE("paired named regions in one cell share valid schedule evidence",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 1> algorithms{{
        {"same_cell", std::span<const algorithm_parameter_specification>{k_dummy_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"PAIR", "same-cell paired regions", R"({"length":1})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{{"PAIR", 2}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> over_entries{{
        {"same_cell", "{}"},
    }};
    static constexpr std::array<std::string_view, 1> acceptance_rows{{
        "same_cell",
    }};
    static constexpr std::array<named_region_sample_multiplicity_specification, 2>
        exact_named_samples{{
            {"same_cell", "PAIR", "paired.same.fast",
             "region_total_ns", 2},
            {"same_cell", "PAIR", "paired.same.slow",
             "region_total_ns", 2},
        }};
    static constexpr std::string_view rules = R"json([
      ["same_cell_regions",
       {"key":"same_cell","test_case":"PAIR","region":"paired.same.fast"},
       {"compare":"<","aggregate":"p50","pairing":"by_iteration_ratio","blocking":true},
       {"key":"same_cell","test_case":"PAIR","region":"paired.same.slow"}]
    ])json";
    const std::array<metric_specification, 1> metrics{{
        {"region_total_ns", "region", "", "", rules},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_same_cell_paired_schedule",
            .description = "same-cell paired schedule evidence",
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .by_regions = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const iteration_count_entry>{for_entries},
            .acceptance_require_named_region_iteration_coverage = true,
            .acceptance_exact_named_region_sample_multiplicities =
                std::span<const named_region_sample_multiplicity_specification>{
                    exact_named_samples},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});
    const auto spin_for = [](const std::int64_t duration_ns) {
        const auto start = bioseq::instrumentation::clock::now_ns();
        while (bioseq::instrumentation::clock::now_ns() - start < duration_ns) {
            /* spin */
        }
    };
    const auto on_run = [spin_for](
        std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        static const auto* const fast =
            bioseq::instrumentation::register_static_region("paired.same.fast");
        static const auto* const slow =
            bioseq::instrumentation::register_static_region("paired.same.slow");
        {
            auto scope = bioseq::instrumentation::region(fast);
            spin_for(30'000);
        }
        {
            auto scope = bioseq::instrumentation::region(slow);
            spin_for(500'000);
        }
        return {};
    };
    const auto on_evaluate = [](
        std::string_view, const parameter_map&, const toy_input&,
        const toy_output&) -> std::map<std::string, double> {
        return {};
    };

    temporary_report_directory report_dir;
    const auto outcome = run_one_bench(
        "bench_same_cell_paired_schedule",
        "same-cell paired schedule evidence",
        config_text, report_dir, build_toy_input, on_run, on_evaluate);
    REQUIRE(outcome.exit_code == 0);
    REQUIRE(outcome.report["rule_violations"].empty());
    REQUIRE(outcome.report["paired_rule_evidence"].size() == 1);
    const auto& evidence = outcome.report["paired_rule_evidence"][0];
    REQUIRE(evidence["accepted"] == true);
    REQUIRE(evidence["numerator_route"]["tuple_index"]
            == evidence["denominator_route"]["tuple_index"]);
    for (const auto& repeat : evidence["repeats"]) {
        REQUIRE(repeat["schedule_identity_matches"] == true);
        REQUIRE(repeat["numerator_schedule"]
                == repeat["denominator_schedule"]);
    }
}

TEST_CASE("within-run relative gates preserve curated_reference ratios under common scaling",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    // Keep the source measurements in report cells.
    // The rules calculate comparisons from their p50 aggregates.
    REQUIRE(curated_alignment_reference_p50::block_wall / curated_alignment_reference_p50::simd_wall
            == Catch::Approx(1.057921).margin(0.000001));
    REQUIRE(curated_alignment_reference_p50::batched_wall / curated_alignment_reference_p50::simd_wall
            == Catch::Approx(1.000654).margin(0.000001));
    REQUIRE(curated_alignment_reference_p50::vulkan_wall / curated_alignment_reference_p50::simd_wall
            == Catch::Approx(1.011221).margin(0.000001));
    REQUIRE(curated_alignment_reference_p50::simd_wall / curated_alignment_reference_p50::minimap2_wall
            == Catch::Approx(0.797045).margin(0.000001));
    REQUIRE(curated_alignment_reference_p50::simd_wall / curated_alignment_reference_p50::scalar_wall
            == Catch::Approx(0.540314).margin(0.000001));
    REQUIRE(curated_alignment_reference_p50::simd_sketch / curated_alignment_reference_p50::scalar_sketch
            == Catch::Approx(0.210496957).margin(0.000000001));
    REQUIRE(curated_alignment_reference_p50::simd_chain / curated_alignment_reference_p50::scalar_chain
            == Catch::Approx(0.579268421).margin(0.000000001));
    REQUIRE(curated_alignment_reference_p50::simd_backtrack / curated_alignment_reference_p50::scalar_backtrack
            == Catch::Approx(0.955048972).margin(0.000000001));
    REQUIRE(curated_alignment_reference_p50::block_alignment / curated_alignment_reference_p50::simd_alignment
            == Catch::Approx(0.591278239).margin(0.000000001));

    static constexpr auto& no_params = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 6> algorithms{{
        {"minimap2_dp", std::span<const algorithm_parameter_specification>{no_params}},
        {"bioseq_ksw2_scalar_dp_scalar_mm", std::span<const algorithm_parameter_specification>{no_params}},
        {"bioseq_ksw2_simd_dp_two_stack_nthash", std::span<const algorithm_parameter_specification>{no_params}},
        {"bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash", std::span<const algorithm_parameter_specification>{no_params}},
        {"bioseq_ksw2_batched_simd_dp_two_stack_nthash", std::span<const algorithm_parameter_specification>{no_params}},
        {"bioseq_block_simd_dp_two_stack_nthash", std::span<const algorithm_parameter_specification>{no_params}},
    }};
    static constexpr std::array<test_case_data_specification, 2> test_cases{{
        {"XXL_HIFI", "curated_reference source cell", R"({"length":1})", 1},
        {"OTHER", "must not be checked by the scoped rules", R"({"length":2})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 2> for_entries{{
        {"XXL_HIFI", 1}, {"OTHER", 1},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 6> over_entries{{
        {"minimap2_dp", "{}"},
        {"bioseq_ksw2_scalar_dp_scalar_mm", "{}"},
        {"bioseq_ksw2_simd_dp_two_stack_nthash", "{}"},
        {"bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash", "{}"},
        {"bioseq_ksw2_batched_simd_dp_two_stack_nthash", "{}"},
        {"bioseq_block_simd_dp_two_stack_nthash", "{}"},
    }};

    static constexpr std::string_view wall_rules = R"json([
      ["block_wall",{"key":"bioseq_block_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":0,"tolerance%":5.7921,"aggregate":"p50"},{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"}],
      ["batched_wall",{"key":"bioseq_ksw2_batched_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":0,"tolerance%":0.0654,"aggregate":"p50"},{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"}],
      ["vulkan_wall",{"key":"bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":0,"tolerance%":1.1221,"aggregate":"p50"},{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"}],
      ["simd_minimap2_wall",{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":20.2955,"tolerance%":0.0001,"aggregate":"p50"},{"key":"minimap2_dp","test_case":"XXL_HIFI"}],
      ["simd_scalar_wall",{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":45.9686,"tolerance%":0.0001,"aggregate":"p50"},{"key":"bioseq_ksw2_scalar_dp_scalar_mm","test_case":"XXL_HIFI"}]
    ])json";
    static constexpr std::string_view sketch_rules = R"json([
      ["nthash_stage",{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":78.9503043,"tolerance%":0.0000001,"aggregate":"p50"},{"key":"bioseq_ksw2_scalar_dp_scalar_mm","test_case":"XXL_HIFI"}]
    ])json";
    static constexpr std::string_view chain_rules = R"json([
      ["chaining_stage",{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":42.0731579,"tolerance%":0.0000001,"aggregate":"p50"},{"key":"bioseq_ksw2_scalar_dp_scalar_mm","test_case":"XXL_HIFI"}]
    ])json";
    static constexpr std::string_view backtrack_rules = R"json([
      ["backtrack_stage",{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":4.4951028,"tolerance%":0.0000001,"aggregate":"p50"},{"key":"bioseq_ksw2_scalar_dp_scalar_mm","test_case":"XXL_HIFI"}]
    ])json";
    static constexpr std::string_view alignment_rules = R"json([
      ["alignment_stage",{"key":"bioseq_block_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"},{"compare":"<","threshold%":40.8721761,"tolerance%":0.0000001,"aggregate":"p50"},{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI"}]
    ])json";
    static constexpr std::string_view region_rules = R"json([
      ["region_source_scope",{"key":"bioseq_ksw2_simd_dp_two_stack_nthash","test_case":"XXL_HIFI","region":"relative.fixture.simd"},{"compare":"<","threshold%":79,"aggregate":"p50"},{"key":"bioseq_ksw2_scalar_dp_scalar_mm","test_case":"XXL_HIFI","region":"relative.fixture.scalar"}]
    ])json";
    const std::array<metric_specification, 6> metrics{{
        {"curated_reference_wall_ns", "evaluation", "", "", wall_rules},
        {"curated_reference_nthash_ns", "evaluation", "", "", sketch_rules},
        {"curated_reference_chaining_ns", "evaluation", "", "", chain_rules},
        {"curated_reference_backtrack_ns", "evaluation", "", "", backtrack_rules},
        {"curated_reference_alignment_ns", "evaluation", "", "", alignment_rules},
        {"region_visits", "region", "", "", region_rules},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_relative_curated_reference",
            .description = "relative curated_reference fixture",
            .concurrency = false,
            .by_regions = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};
    const std::string config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});

    const auto run_fixture = [&config_text](const double common_scale,
                                            const std::size_t visit_scale,
                                            const bool regress_nthash,
                                            const bool omit_simd_region,
                                            const bool unavailable_vulkan) {
        temporary_report_directory report_dir;
        const auto on_run = [visit_scale, omit_simd_region](
            const std::string_view algorithm,
            const parameter_map& /*params*/,
            const toy_input& input) -> toy_output {
            static const auto* const simd_region =
                bioseq::instrumentation::register_static_region("relative.fixture.simd");
            static const auto* const scalar_region =
                bioseq::instrumentation::register_static_region("relative.fixture.scalar");
            const bool other_case = input.values.size() == 2;
            if (algorithm == "bioseq_ksw2_simd_dp_two_stack_nthash") {
                const std::size_t visits = (other_case ? 3u : 2u) * visit_scale;
                if (!omit_simd_region) {
                    for (std::size_t visit = 0; visit < visits; ++visit) {
                        auto scope = bioseq::instrumentation::region(simd_region);
                    }
                }
            } else if (algorithm == "bioseq_ksw2_scalar_dp_scalar_mm") {
                const std::size_t visits = 10u * visit_scale;
                for (std::size_t visit = 0; visit < visits; ++visit) {
                    auto scope = bioseq::instrumentation::region(scalar_region);
                }
            }
            return toy_output{};
        };
        const auto on_evaluate = [common_scale, regress_nthash,
                                  unavailable_vulkan](
            const std::string_view algorithm,
            const parameter_map& /*params*/,
            const toy_input& input,
            const toy_output& /*output*/) -> std::map<std::string, double> {
            const bool other_case = input.values.size() == 2;
            const double scoped_bad_factor = other_case ? 2.0 : 1.0;

            if (unavailable_vulkan
                && algorithm
                    == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
                const double not_measured =
                    std::numeric_limits<double>::quiet_NaN();
                return {
                    {"curated_reference_wall_ns", not_measured},
                    {"curated_reference_nthash_ns", not_measured},
                    {"curated_reference_chaining_ns", not_measured},
                    {"curated_reference_backtrack_ns", not_measured},
                    {"curated_reference_alignment_ns", not_measured},
                };
            }

            double wall = 1.0;
            double nthash = 1.0;
            double chaining = 1.0;
            double backtrack = 1.0;
            double alignment = 1.0;
            if (algorithm == "minimap2_dp") {
                wall = curated_alignment_reference_p50::minimap2_wall;
            } else if (algorithm == "bioseq_ksw2_scalar_dp_scalar_mm") {
                wall = curated_alignment_reference_p50::scalar_wall;
                nthash = curated_alignment_reference_p50::scalar_sketch;
                chaining = curated_alignment_reference_p50::scalar_chain;
                backtrack = curated_alignment_reference_p50::scalar_backtrack;
            } else if (algorithm == "bioseq_ksw2_simd_dp_two_stack_nthash") {
                wall = curated_alignment_reference_p50::simd_wall * scoped_bad_factor;
                nthash = curated_alignment_reference_p50::simd_sketch * scoped_bad_factor
                        * (regress_nthash && !other_case ? 1.01 : 1.0);
                chaining = curated_alignment_reference_p50::simd_chain * scoped_bad_factor;
                backtrack = curated_alignment_reference_p50::simd_backtrack * scoped_bad_factor;
                alignment = curated_alignment_reference_p50::simd_alignment;
            } else if (algorithm == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
                wall = curated_alignment_reference_p50::vulkan_wall * scoped_bad_factor;
            } else if (algorithm == "bioseq_ksw2_batched_simd_dp_two_stack_nthash") {
                wall = curated_alignment_reference_p50::batched_wall * scoped_bad_factor;
            } else if (algorithm == "bioseq_block_simd_dp_two_stack_nthash") {
                wall = curated_alignment_reference_p50::block_wall * scoped_bad_factor;
                alignment = curated_alignment_reference_p50::block_alignment * scoped_bad_factor;
            }
            return {
                {"curated_reference_wall_ns", wall * common_scale},
                {"curated_reference_nthash_ns", nthash * common_scale},
                {"curated_reference_chaining_ns", chaining * common_scale},
                {"curated_reference_backtrack_ns", backtrack * common_scale},
                {"curated_reference_alignment_ns", alignment * common_scale},
            };
        };
        return run_one_bench(
            "bench_relative_curated_reference", "relative curated_reference fixture",
            config_text, report_dir, build_toy_input, on_run, on_evaluate);
    };

    const auto original_scale = run_fixture(1.0, 1u, false, false, false);
    REQUIRE(original_scale.exit_code == 0);
    REQUIRE(original_scale.report["rule_violations"].empty());
    REQUIRE(stat_at(original_scale.report, "bench_relative_curated_reference",
                    "bioseq_ksw2_simd_dp_two_stack_nthash", "XXL_HIFI",
                    "all_regions", "curated_reference_nthash_ns", "min")
            == curated_alignment_reference_p50::simd_sketch);
    REQUIRE(stat_at(original_scale.report, "bench_relative_curated_reference",
                    "bioseq_ksw2_scalar_dp_scalar_mm", "XXL_HIFI",
                    "all_regions", "curated_reference_nthash_ns", "min")
            == curated_alignment_reference_p50::scalar_sketch);

    // Scale all timing values together, and all visit counts together.
    // The ratios within each metric must stay the same.
    const auto common_scaled =
        run_fixture(1'000.0, 10u, false, false, false);
    REQUIRE(common_scaled.exit_code == 0);
    REQUIRE(common_scaled.report["rule_violations"].empty());
    REQUIRE(stat_at(common_scaled.report, "bench_relative_curated_reference",
                    "bioseq_ksw2_simd_dp_two_stack_nthash", "XXL_HIFI",
                    "relative.fixture.simd", "region_visits", "min") == 20.0);
    REQUIRE(stat_at(common_scaled.report, "bench_relative_curated_reference",
                    "bioseq_ksw2_scalar_dp_scalar_mm", "XXL_HIFI",
                    "relative.fixture.scalar", "region_visits", "min") == 100.0);

    // Increase only the nthash time enough to fail its comparison.
    // The violation must keep both scaled source values in args.
    const auto regressed = run_fixture(1'000.0, 10u, true, false, false);
    REQUIRE(regressed.exit_code == 1);
    REQUIRE(regressed.report["rule_violations"].size() == 1);
    const auto& violation = regressed.report["rule_violations"][0];
    REQUIRE(violation["rule_name"].get<std::string>() == "nthash_stage");
    REQUIRE(violation["test_case"].get<std::string>() == "XXL_HIFI");
    REQUIRE(violation["args"][0].get<double>()
            == Catch::Approx(
                curated_alignment_reference_p50::simd_sketch * 1'000.0 * 1.01));
    REQUIRE(violation["args"][1].get<double>()
            == curated_alignment_reference_p50::scalar_sketch * 1'000.0);

    // Removing a selected stage must fail the rule and identify the missing input.
    const auto missing_stage = run_fixture(1.0, 1u, false, true, false);
    REQUIRE(missing_stage.exit_code == 1);
    REQUIRE(missing_stage.report["rule_violations"].size() == 1);
    const auto& missing_violation = missing_stage.report["rule_violations"][0];
    REQUIRE(missing_violation["rule_name"].get<std::string>()
            == "region_source_scope");
    REQUIRE_FALSE(missing_violation["args_present"][0].get<bool>());
    REQUIRE(missing_violation["args_present"][1].get<bool>());

    // All-NaN evaluation metrics mark an unavailable GPU result.
    // The comparison must reject that missing measurement.
    const auto missing_hardware =
        run_fixture(1.0, 1u, false, false, true);
    REQUIRE(missing_hardware.exit_code == 1);
    REQUIRE(missing_hardware.report["rule_violations"].size() == 1);
    const auto& unavailable_violation =
        missing_hardware.report["rule_violations"][0];
    REQUIRE(unavailable_violation["rule_name"].get<std::string>()
            == "vulkan_wall");
    REQUIRE_FALSE(
        unavailable_violation["args_present"][0].get<bool>());
    REQUIRE(
        unavailable_violation["args_present"][1].get<bool>());
}

// Constant-value warnings across report history.

TEST_CASE("metric_looks_constant fires when evaluator returns hardcoded value",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& k_no_parameters = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"identity", std::span<const algorithm_parameter_specification>{k_no_parameters}}
    }};
    static constexpr std::array<test_case_data_specification, 1> k_test_cases{{
        {"ONE", "constant", R"({"length":4})"}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for_entries{{ {"ONE", 3} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over_entries{{ {"identity", "{}"} }};
    const std::array<metric_specification, 1> metrics{{
        {"frozen", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_const",
            .description = "constant",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};

    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;

    auto on_run = [](std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        return toy_output{};
    };
    auto on_evaluate_constant = [](std::string_view, const parameter_map&,
                                     const toy_input&, const toy_output&)
        -> std::map<std::string, double>
    {
        return { {"frozen", 42.0} };
    };

    // The first report starts the constant-value warning history.
    {
        auto outcome = run_one_bench(
            "bench_const", "constant run 1", config_text, report_dir,
            build_toy_input, on_run, on_evaluate_constant);
        REQUIRE(outcome.exit_code == 0);   // Sanity warnings allow a successful exit.
        REQUIRE(outcome.report_path.filename() == "report_1.json");
        const auto warning = sanity_warning_for_metric(
            outcome.report, "metric_looks_constant", "frozen");
        REQUIRE(warning.has_value());
        REQUIRE(warning->at("warning_since").get<std::string>()
                == "report_1.json");
        REQUIRE(warning->at("message").get<std::string>().find("'42'")
                != std::string::npos);
    }

    // The unchanged value should keep warning_since at report_1.json.
    {
        auto outcome = run_one_bench(
            "bench_const", "constant run 2", config_text, report_dir,
            build_toy_input, on_run, on_evaluate_constant);
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(outcome.report_path.filename() == "report_2.json");
        const auto warning = sanity_warning_for_metric(
            outcome.report, "metric_looks_constant", "frozen");
        REQUIRE(warning.has_value());
        REQUIRE(warning->at("warning_since").get<std::string>()
                == "report_1.json");
    }

    // Vary the value by iteration so the constant-value warning disappears.
    {
        std::atomic<std::int64_t> iteration_counter{0};
        auto on_evaluate_varying = [&iteration_counter](
            std::string_view, const parameter_map&,
            const toy_input&, const toy_output&)
            -> std::map<std::string, double>
        {
            const auto value = iteration_counter.fetch_add(1, std::memory_order_relaxed);
            return { {"frozen", static_cast<double>(value)} };
        };
        auto outcome = run_one_bench(
            "bench_const", "constant run 3", config_text, report_dir,
            build_toy_input, on_run, on_evaluate_varying);
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(outcome.report_path.filename() == "report_3.json");
        REQUIRE_FALSE(sanity_warning_for_metric(
            outcome.report, "metric_looks_constant", "frozen").has_value());
    }
}

// Compare current measurements with report history.

TEST_CASE("regression rule fires after value drift",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& k_no_parameters = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"identity", std::span<const algorithm_parameter_specification>{k_no_parameters}}
    }};
    static constexpr std::array<test_case_data_specification, 1> k_test_cases{{
        {"ONE", "regression", R"({"length":4})"}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for_entries{{ {"ONE", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over_entries{{ {"identity", "{}"} }};
    const std::array<metric_specification, 1> metrics{{
        {"value", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_reg",
            .description = "regression",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};
    static constexpr std::string_view k_regression_rules = R"([{
        "name":"value_eq",
        "metric":"value",
        "aggregate":"sum",
        "operator":{"compare":"=","tolerance%":0}
    }])";
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches},
        k_regression_rules);

    temporary_report_directory report_dir;

    auto on_run = [](std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        return toy_output{};
    };
    auto on_eval_one = [](std::string_view, const parameter_map&,
                           const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return { {"value", 1.0} }; };
    auto on_eval_two = [](std::string_view, const parameter_map&,
                           const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return { {"value", 2.0} }; };

    // Establish a baseline. With no earlier report, there is no regression.
    {
        auto outcome = run_one_bench(
            "bench_reg", "regression baseline", config_text, report_dir,
            build_toy_input, on_run, on_eval_one);
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(outcome.report.contains("regressions"));
        REQUIRE(outcome.report["regressions"].empty());
    }

    // Doubling both samples changes the sum from 2 to 4 and fails exact equality.
    {
        auto outcome = run_one_bench(
            "bench_reg", "regression now", config_text, report_dir,
            build_toy_input, on_run, on_eval_two);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(outcome.report["regressions"].size() == 1);
        const auto& entry = outcome.report["regressions"][0];
        REQUIRE(entry["rule_name"].get<std::string>() == "value_eq");
        REQUIRE(entry["blocking"].get<bool>() == true);
        REQUIRE(entry["regression_since"].get<std::string>() == "report_1.json");
        REQUIRE(entry["previously"].get<double>() == 2.0);   // 1.0 + 1.0
        REQUIRE(entry["now"].get<double>()        == 4.0);   // 2.0 + 2.0
        REQUIRE(outcome.report["meta"]["history_eligible"] == false);
    }
}

TEST_CASE("regression history skips every ineligible candidate",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& no_params = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 1> algorithms{{
        {"identity", std::span<const algorithm_parameter_specification>{no_params}}
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"ONE", "history eligibility", R"({"length":4})"}
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{ {"ONE", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> over_entries{{ {"identity", "{}"} }};
    const std::array<metric_specification, 1> metrics{{
        {"value", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_history_eligibility",
            .description = "strict historical candidate eligibility",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};
    static constexpr std::string_view regression_rules = R"([{
        "name":"value_eq","metric":"value","aggregate":"sum",
        "operator":{"compare":"=","tolerance%":0}
    }])";
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches}, regression_rules);
    const auto on_run = [](
        std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        return toy_output{};
    };
    const auto evaluate_one = [](
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {{"value", 1.0}}; };
    const auto evaluate_two = [](
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {{"value", 2.0}}; };

    // A null raw_evidence field keeps this historical report eligible.
    {
        temporary_report_directory report_dir;
        const auto baseline = run_one_bench(
            "bench_history_eligibility", "null raw baseline", config_text,
            report_dir, build_toy_input, on_run, evaluate_one);
        REQUIRE(baseline.report["raw_evidence"].is_null());
        const auto current = run_one_bench(
            "bench_history_eligibility", "null raw current", config_text,
            report_dir, build_toy_input, on_run, evaluate_two);
        REQUIRE(current.report["regressions"].size() == 1);
        REQUIRE(current.report["regressions"][0]["previously"] == 2.0);
    }

    enum class mutation {
        incomplete_raw,
        missing_raw_complete,
        malformed_record_count,
        missing_fast,
        mismatched_fast,
        missing_history_seal,
        malformed_history_seal,
        missing_rule_violations,
        malformed_rule_violations,
        missing_regressions,
        malformed_regressions,
        missing_sanity_warnings,
        malformed_sanity_warnings,
        sample_count,
        finite_count,
        aggregate,
    };
    struct mutation_case {
        mutation kind;
        std::string_view reason;
    };
    static constexpr std::array<mutation_case, 16> mutations{{
        {mutation::incomplete_raw, "raw_evidence.complete is false"},
        {mutation::missing_raw_complete,
         "raw_evidence.complete is missing or malformed"},
        {mutation::malformed_record_count,
         "raw_evidence.record_count is missing or malformed"},
        {mutation::missing_fast, "meta.fast is missing or malformed"},
        {mutation::mismatched_fast, "does not match current meta.fast"},
        {mutation::missing_history_seal,
         "meta.history_eligible is missing"},
        {mutation::malformed_history_seal,
         "meta.history_eligible is malformed"},
        {mutation::missing_rule_violations, "rule_violations is missing"},
        {mutation::malformed_rule_violations,
         "rule_violations is malformed"},
        {mutation::missing_regressions, "regressions is missing"},
        {mutation::malformed_regressions, "regressions is malformed"},
        {mutation::missing_sanity_warnings, "sanity_warnings is missing"},
        {mutation::malformed_sanity_warnings,
         "sanity_warnings is malformed"},
        {mutation::sample_count, "sample cardinality"},
        {mutation::finite_count, "finite histogram count"},
        {mutation::aggregate,
         "selected sum aggregate is missing, malformed, or non-finite"},
    }};

    for (const auto& mutation_case_entry : mutations) {
        INFO("candidate mutation "
             << static_cast<int>(mutation_case_entry.kind));
        temporary_report_directory report_dir;
        const auto oldest = run_one_bench(
            "bench_history_eligibility", "oldest valid", config_text,
            report_dir, build_toy_input, on_run, evaluate_one);
        REQUIRE(oldest.report_path.filename() == "report_1.json");
        auto candidate = run_one_bench(
            "bench_history_eligibility", "candidate", config_text,
            report_dir, build_toy_input, on_run, evaluate_one);
        REQUIRE(candidate.report_path.filename() == "report_2.json");
        REQUIRE(candidate.report["meta"]["history_eligible"] == true);

        if (mutation_case_entry.kind == mutation::incomplete_raw) {
            candidate.report["raw_evidence"] = {
                {"complete", false}, {"record_count", 2}};
        } else if (mutation_case_entry.kind == mutation::missing_raw_complete) {
            candidate.report["raw_evidence"] = {{"record_count", 2}};
        } else if (mutation_case_entry.kind == mutation::malformed_record_count) {
            candidate.report["raw_evidence"] = {
                {"complete", true}, {"record_count", "two"}};
        } else if (mutation_case_entry.kind == mutation::missing_fast) {
            candidate.report["meta"].erase("fast");
        } else if (mutation_case_entry.kind == mutation::mismatched_fast) {
            candidate.report["meta"]["fast"] =
                !candidate.report["meta"]["fast"].get<bool>();
        } else if (mutation_case_entry.kind == mutation::missing_history_seal) {
            candidate.report["meta"].erase("history_eligible");
        } else if (mutation_case_entry.kind == mutation::malformed_history_seal) {
            candidate.report["meta"]["history_eligible"] = "yes";
        } else if (mutation_case_entry.kind == mutation::missing_rule_violations) {
            candidate.report.erase("rule_violations");
        } else if (mutation_case_entry.kind == mutation::malformed_rule_violations) {
            candidate.report["rule_violations"] = "not-an-array";
        } else if (mutation_case_entry.kind == mutation::missing_regressions) {
            candidate.report.erase("regressions");
        } else if (mutation_case_entry.kind == mutation::malformed_regressions) {
            candidate.report["regressions"] = "not-an-array";
        } else if (mutation_case_entry.kind == mutation::missing_sanity_warnings) {
            candidate.report.erase("sanity_warnings");
        } else if (mutation_case_entry.kind == mutation::malformed_sanity_warnings) {
            candidate.report["sanity_warnings"] = "not-an-array";
        } else {
            auto* aggregate = mutable_aggregate_at(
                candidate.report, "bench_history_eligibility", "identity",
                "ONE", "all_regions", "value");
            REQUIRE(aggregate != nullptr);
            if (mutation_case_entry.kind == mutation::sample_count) {
                (*aggregate)["samples_count"] = 3;
            } else if (mutation_case_entry.kind == mutation::finite_count) {
                // Make the historical finite-sample count disagree with the current report.
                (*aggregate)["histogram"][0]["count"] = 1;
            } else {
                (*aggregate)["stats"]["sum"] = "not-a-number";
            }
        }
        REQUIRE(rewrite_report_json(candidate.report_path, candidate.report));

        const auto current = run_one_bench(
            "bench_history_eligibility", "current", config_text,
            report_dir, build_toy_input, on_run, evaluate_two);
        REQUIRE(current.report["regressions"].size() == 1);
        const auto& regression = current.report["regressions"][0];
        REQUIRE(regression["previously"] == 2.0);
        REQUIRE(regression["now"] == 4.0);
        REQUIRE(regression["regression_since"] == "report_1.json");
        REQUIRE(current.captured_stderr.find(
                    "regression_history_candidate_skipped")
                != std::string::npos);
        REQUIRE(current.captured_stderr.find(mutation_case_entry.reason)
                != std::string::npos);
    }

    // Skip a report with a blocking regression when choosing the next baseline
    // or the report that started the regression.
    {
        temporary_report_directory report_dir;
        const auto accepted = run_one_bench(
            "bench_history_eligibility", "accepted baseline", config_text,
            report_dir, build_toy_input, on_run, evaluate_one);
        REQUIRE(accepted.report["meta"]["history_eligible"] == true);
        const auto failed = run_one_bench(
            "bench_history_eligibility", "blocking regression", config_text,
            report_dir, build_toy_input, on_run, evaluate_two);
        REQUIRE(failed.exit_code == 1);
        REQUIRE(failed.report["meta"]["history_eligible"] == false);
        const auto current = run_one_bench(
            "bench_history_eligibility", "skip failed baseline", config_text,
            report_dir, build_toy_input, on_run, evaluate_two);
        REQUIRE(current.report["regressions"].size() == 1);
        REQUIRE(current.report["regressions"][0]["previously"] == 2.0);
        REQUIRE(current.report["regressions"][0]["regression_since"]
                == "report_1.json");
        REQUIRE(current.captured_stderr.find(
                    "meta.history_eligible is false")
                != std::string::npos);
    }
}

TEST_CASE("ordered regression treats only exact zero as unchanged",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& no_params = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 1> algorithms{{
        {"identity", std::span<const algorithm_parameter_specification>{no_params}}
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"ONE", "ordered zero", R"({"length":4})"}
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{ {"ONE", 1} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> over_entries{{ {"identity", "{}"} }};
    const std::array<metric_specification, 1> metrics{{
        {"value", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_ordered_zero",
            .description = "ordered zero boundary",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};
    static constexpr std::string_view regression_rules = R"([{
        "name":"value_gt","metric":"value","aggregate":"sum",
        "operator":{"compare":">","tolerance%":0}
    }])";
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches}, regression_rules);
    const auto on_run = [](
        std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        return toy_output{};
    };
    const auto evaluate_positive_zero = [](
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {{"value", 0.0}}; };
    const auto evaluate_negative_zero = [](
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {{"value", -0.0}}; };
    const auto evaluate_one = [](
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {{"value", 1.0}}; };
    const auto evaluate_ninety_percent = [](
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {{"value", 0.90}}; };

    {
        temporary_report_directory report_dir;
        const auto baseline = run_one_bench(
            "bench_ordered_zero", "positive zero baseline", config_text,
            report_dir, build_toy_input, on_run, evaluate_positive_zero);
        REQUIRE(baseline.report_path.filename() == "report_1.json");
        const auto unchanged = run_one_bench(
            "bench_ordered_zero", "negative zero unchanged", config_text,
            report_dir, build_toy_input, on_run, evaluate_negative_zero);
        REQUIRE(unchanged.exit_code == 0);
        REQUIRE(unchanged.report["regressions"].empty());
    }

    {
        temporary_report_directory report_dir;
        const auto baseline = run_one_bench(
            "bench_ordered_zero", "one baseline", config_text,
            report_dir, build_toy_input, on_run, evaluate_one);
        REQUIRE(baseline.report_path.filename() == "report_1.json");
        const auto regressed = run_one_bench(
            "bench_ordered_zero", "ninety percent current", config_text,
            report_dir, build_toy_input, on_run, evaluate_ninety_percent);
        REQUIRE(regressed.exit_code == 1);
        REQUIRE(regressed.report["regressions"].size() == 1);
        REQUIRE(regressed.report["regressions"][0]["previously"] == 1.0);
        REQUIRE(regressed.report["regressions"][0]["now"].get<double>()
                == Catch::Approx(0.90));
    }
}

TEST_CASE("diagnostic regression is reported without blocking and algorithm deny-list is scoped",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& k_no_parameters = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 2> k_algorithms{{
        {"cpu", std::span<const algorithm_parameter_specification>{k_no_parameters}},
        {"gpu", std::span<const algorithm_parameter_specification>{k_no_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> k_test_cases{{
        {"ONE", "diagnostic regression", R"({"length":4})"}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for_entries{{ {"ONE", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> k_over_entries{{
        {"cpu", "{}"}, {"gpu", "{}"},
    }};
    const std::array<metric_specification, 1> metrics{{
        {"value", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_reg_diagnostic",
            .description = "diagnostic regression",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};
    static constexpr std::string_view k_regression_rules = R"([{
        "name":"gpu_value_confirmation",
        "metric":"value",
        "aggregate":"sum",
        "operator":{"compare":"=","tolerance%":0},
        "exclude_algorithms":["cpu"],
        "blocking":false
    }])";
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches},
        k_regression_rules);

    temporary_report_directory report_dir;
    auto on_run = [](std::string_view, const parameter_map&, const toy_input&) {
        return toy_output{};
    };
    auto on_eval_one = [](std::string_view, const parameter_map&,
                           const toy_input&, const toy_output&) {
        return std::map<std::string, double>{{"value", 1.0}};
    };
    auto on_eval_two = [](std::string_view, const parameter_map&,
                           const toy_input&, const toy_output&) {
        return std::map<std::string, double>{{"value", 2.0}};
    };

    REQUIRE(run_one_bench(
        "bench_reg_diagnostic", "baseline", config_text, report_dir,
        build_toy_input, on_run, on_eval_one).exit_code == 0);
    const auto outcome = run_one_bench(
        "bench_reg_diagnostic", "drift", config_text, report_dir,
        build_toy_input, on_run, on_eval_two);
    REQUIRE(outcome.exit_code == 0);
    REQUIRE(outcome.report.at("regressions").size() == 1);
    const auto& regression = outcome.report.at("regressions").at(0);
    REQUIRE(regression.at("algorithm_name") == "gpu");
    REQUIRE(regression.at("blocking") == false);
    REQUIRE(stderr_contains(outcome.captured_stderr, "classification=diagnostic"));
}

// Evaluator results for undeclared metrics are ignored.

TEST_CASE("evaluator returns extra unused key — silently dropped",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& k_no_parameters = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"identity", std::span<const algorithm_parameter_specification>{k_no_parameters}}
    }};
    static constexpr std::array<test_case_data_specification, 1> k_test_cases{{
        {"ONE", "case", R"({"length":4})"}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for_entries{{ {"ONE", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over_entries{{ {"identity", "{}"} }};
    // Declare accuracy and return two extra metrics to check that they are ignored.
    const std::array<metric_specification, 1> metrics{{
        {"accuracy", "evaluation", "", "", "[]"}
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_extra",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;

    auto on_run = [](std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        return toy_output{};
    };
    auto on_evaluate = [](std::string_view, const parameter_map&,
                           const toy_input&, const toy_output&)
        -> std::map<std::string, double>
    {
        return {
            {"accuracy",  0.95},
            {"latency",   123.0},   // Undeclared metric: omit it from the report.
            {"throughput", 7.0},    // Undeclared metric: omit it from the report.
        };
    };

    auto outcome = run_one_bench(
        "bench_extra", "extra evaluator keys", config_text, report_dir,
        build_toy_input, on_run, on_evaluate);

    REQUIRE(outcome.exit_code == 0);
    REQUIRE_FALSE(stderr_contains(outcome.captured_stderr, "evaluation_key_not_declared"));

    REQUIRE(samples_count_at(outcome.report, "bench_extra",
                              "identity", "ONE",
                              "all_regions", "accuracy") == 2);
    // Every accuracy sample is 0.95, so both bounds must equal 0.95.
    REQUIRE(stat_at(outcome.report, "bench_extra",
                     "identity", "ONE",
                     "all_regions", "accuracy", "min") == 0.95);
    REQUIRE(stat_at(outcome.report, "bench_extra",
                     "identity", "ONE",
                     "all_regions", "accuracy", "max") == 0.95);

    REQUIRE_FALSE(aggregate_at(outcome.report, "bench_extra",
                                "identity", "ONE",
                                "all_regions", "latency").has_value());
    REQUIRE_FALSE(aggregate_at(outcome.report, "bench_extra",
                                "identity", "ONE",
                                "all_regions", "throughput").has_value());
}

// A declared evaluation metric must be returned.

TEST_CASE("declared evaluator metric never returned still errors",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr auto& k_no_parameters = k_dummy_parameters;
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"identity", std::span<const algorithm_parameter_specification>{k_no_parameters}}
    }};
    static constexpr std::array<test_case_data_specification, 1> k_test_cases{{
        {"ONE", "missing", R"({"length":4})"}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for_entries{{ {"ONE", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over_entries{{ {"identity", "{}"} }};
    const std::array<metric_specification, 1> metrics{{
        {"never_returned", "evaluation", "", "", "[]"}
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_missing",
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        }
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;

    auto on_run = [](std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        return toy_output{};
    };
    auto on_evaluate_empty = [](std::string_view, const parameter_map&,
                                  const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {}; };

    auto outcome = run_one_bench(
        "bench_missing", "missing eval", config_text, report_dir,
        build_toy_input, on_run, on_evaluate_empty);

    REQUIRE(outcome.exit_code == 1);
    REQUIRE(stderr_contains(outcome.captured_stderr, "missing_evaluation_metric"));
    // Missing declared metrics must use the missing-metric diagnostic.
    REQUIRE_FALSE(stderr_contains(outcome.captured_stderr, "evaluation_key_not_declared"));
}

// Reject a configuration that fails schema validation.

TEST_CASE("schema validation rejects config missing 'params'",
          "[component:benchmark_framework][layer:contract][feature:configuration][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    // Omit the required params object to trigger configuration validation.
    constexpr std::string_view bad_config =
        R"({"test_cases":{"X":{"data":{}}},"bench_x":{"matrix":{"for":{"X":1},"over":{},"metrics":{"wall_clock_ns":{"rules":[]}}}}})";

    temporary_report_directory report_dir;

    auto on_run = [](std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        return toy_output{};
    };
    auto on_evaluate = [](std::string_view, const parameter_map&,
                           const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {}; };

    auto outcome = run_one_bench(
        "bench_x", "schema fail", bad_config, report_dir,
        build_toy_input, on_run, on_evaluate);

    REQUIRE(outcome.exit_code == 1);
    // Check the configuration-validation banner shown to the user.
    REQUIRE(stderr_contains(outcome.captured_stderr, "configuration validation"));
}

// Call cleanup after each used input and skip test cases with no iterations.

TEST_CASE("on_cleanup runs once per cached test_case after iterations finish",
          "[component:benchmark_framework][layer:contract][feature:e2e_driver][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    // Add one defaulted parameter because the schema requires a nonempty set.
    static constexpr std::array<algorithm_parameter_specification, 1> k_parameters{{
        {"size_hint", "integer", "0", ""}
    }};
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"toy", std::span<const algorithm_parameter_specification>{k_parameters}}
    }};
    // NEVER has no iterations, so it must not build or clean up an input.
    // Give each used test case one group to expect one cleanup call per case.
    static constexpr std::array<test_case_data_specification, 3> k_test_cases{{
        {"TINY",  "first",  R"({"length":4,"seed":1})",  1},
        {"MID",   "second", R"({"length":8,"seed":2})",  1},
        {"NEVER", "zero",   R"({"length":16,"seed":3})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 3> k_for_entries{{
        {"TINY",  3},
        {"MID",   2},
        {"NEVER", 0},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over_entries{{
        {"toy", "{}"}
    }};
    static constexpr std::array<metric_specification, 1> k_metrics{{
        {"wall_clock_ns", "builtin", "", "", "[]"}
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_cleanup",
            .description = "cleanup hook",
            .fast = false,
            .bench_lock = false,
            .concurrency = false,
            .by_regions = false,
            .for_entries  = std::span<const iteration_count_entry>{k_for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over_entries},
            .metrics      = std::span<const metric_specification>{k_metrics},
        }
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;

    auto on_run = [](std::string_view, const parameter_map&, const toy_input&) -> toy_output {
        return toy_output{};
    };
    auto on_evaluate = [](std::string_view, const parameter_map&,
                           const toy_input&, const toy_output&)
        -> std::map<std::string, double> { return {}; };

    SECTION("default-empty cleanup is a silent no-op") {
        // The default cleanup callback is empty. Check that a run can finish with it.
        const auto outcome = run_one_bench(
            "bench_cleanup", "no cleanup", config_text, report_dir,
            build_toy_input, on_run, on_evaluate);
        REQUIRE(outcome.exit_code == 0);
    }

    SECTION("user-supplied cleanup fires once per used test case") {
        std::vector<std::string> seen_names;
        std::vector<std::size_t> seen_lengths;
        auto on_cleanup = [&](std::string_view /*data_dir*/,
                              std::string_view test_case_name,
                              simdjson::dom::element /*data*/,
                              const toy_input& input)
        {
            seen_names.emplace_back(test_case_name);
            seen_lengths.push_back(input.values.size());
        };

        const auto outcome = run_one_bench(
            "bench_cleanup", "with cleanup", config_text, report_dir,
            build_toy_input, on_run, on_evaluate, on_cleanup);
        REQUIRE(outcome.exit_code == 0);

        // TINY and MID each need one cleanup call. NEVER never built an input.
        REQUIRE(seen_names.size() == 2);
        const std::vector<std::string> expected_names{"TINY", "MID"};
        REQUIRE(seen_names == expected_names);
        // Cleanup receives the input lengths requested in the JSON configuration.
        REQUIRE(seen_lengths == std::vector<std::size_t>{4, 8});
    }
}

// Test tags, filtering, and debug reports through run().
// The assertions inspect exit codes, saved JSON reports and JSONL events,
// and diagnostics captured from stderr.

namespace {

using bioseq::bench::framework::erased_benchmark;
using bioseq::bench::framework::run_constraints;

/// Supply per-test-case resource estimates, including requests that force a skip.
using estimator_function = std::function<run_constraints(
    std::string_view, std::string_view, simdjson::dom::element)>;

/// Build a toy benchmark that sums its input and reports the result.
/// A custom estimator can make selected test cases exceed the memory budget.
[[nodiscard]] inline erased_benchmark make_toy_bench(
    std::string_view key, std::string_view title, const std::string& config_path,
    estimator_function estimate = nullptr, double evaluation_scale = 1.0)
{
    using input_function = std::function<toy_input(std::string_view, std::string_view,
                                           simdjson::dom::element, const run_constraints&)>;
    using run_function = std::function<toy_output(std::string_view, const parameter_map&, const toy_input&)>;
    using evaluation_function = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)>;
    using cleanup_function = std::function<void(std::string_view, std::string_view,
                                         simdjson::dom::element, const toy_input&)>;

    estimator_function est = estimate
        ? std::move(estimate)
        : estimator_function{[](std::string_view, std::string_view,
                       simdjson::dom::element) noexcept { return run_constraints{}; }};
    input_function inp = [](std::string_view data_dir, std::string_view test_case_name,
                     simdjson::dom::element data, const run_constraints&) {
        return build_toy_input(data_dir, test_case_name, data);
    };
    run_function run_fn = [](std::string_view, const parameter_map&, const toy_input& in) {
        toy_output out;
        for (const auto value : in.values) out.sum += value;
        return out;
    };
    evaluation_function eval_fn = [evaluation_scale](
        std::string_view, const parameter_map&, const toy_input&,
        const toy_output& out) {
        return std::map<std::string, double>{{
            "sum_value", evaluation_scale * static_cast<double>(out.sum)}};
    };
    return create_benchmark<toy_input, toy_output>(
        std::string{title}, std::string{key}, config_path,
        std::move(est), std::move(inp), std::move(run_fn),
        std::move(eval_fn), cleanup_function{});
}

/// Save the configuration under report_dir and return its path.
[[nodiscard]] inline std::string write_config_file(const temporary_report_directory& report_dir,
                                                   std::string_view config_text) {
    const auto path = report_dir.path / "config.json";
    std::ofstream cfg(path, std::ios::trunc | std::ios::binary);
    cfg.write(config_text.data(), static_cast<std::streamsize>(config_text.size()));
    cfg.close();
    return path.string();
}

/// Run the supplied benchmarks with extra command-line arguments and capture
/// their report, exit code, and stderr.
template<typename... p_benchmarks>
[[nodiscard]] run_outcome run_with_args(const temporary_report_directory& report_dir,
                                       std::span<const std::string> extra_args,
                                       p_benchmarks&&... benches) {
    std::vector<std::string> args = {
        "bench_framework_test", "--report", report_dir.path.string(),
        "--threads", "2",
    };
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    argument_vector_builder argv_holder{std::span<const std::string>{args}};

    run_outcome outcome;
    {
        standard_error_capture capture;
        outcome.exit_code = run(argv_holder.argc(), argv_holder.argv(),
                                report_dir.path.string(), report_dir.path.string(),
                                std::forward<p_benchmarks>(benches)...);
        outcome.captured_stderr = capture.text();
    }
    if (auto path = latest_report(report_dir.path); path.has_value()) {
        outcome.report_path = *path;
        std::ifstream input(outcome.report_path, std::ios::binary);
        if (input) {
            std::string body{std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{}};
            outcome.report = nlohmann::json::parse(body, nullptr, false);
            if (outcome.report.is_discarded()) outcome.report = nlohmann::json::object();
        }
    }
    return outcome;
}

/// Find a report_debug_<N>.jsonl file, or return nullopt if none exists.
[[nodiscard]] inline std::optional<std::filesystem::path> find_debug_jsonl(
    const std::filesystem::path& report_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(report_dir, ec)) return std::nullopt;
    for (const auto& entry : std::filesystem::directory_iterator(report_dir, ec)) {
        if (ec) break;
        const auto name = entry.path().filename().string();
        if (name.starts_with("report_debug_") && name.ends_with(".jsonl"))
            return entry.path();
    }
    return std::nullopt;
}

/// Read the valid, nonempty JSON lines and skip lines that fail to parse.
[[nodiscard]] inline std::vector<nlohmann::json> read_jsonl(
    const std::filesystem::path& path) {
    std::vector<nlohmann::json> events;
    std::ifstream in(path, std::ios::binary);
    if (!in) return events;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (!parsed.is_discarded()) events.push_back(std::move(parsed));
    }
    return events;
}

/// Count events whose "event" field equals `kind`.
[[nodiscard]] inline std::size_t count_events(
    const std::vector<nlohmann::json>& events, std::string_view kind) {
    std::size_t total = 0;
    for (const auto& e : events)
        if (e.value("event", std::string{}) == kind) ++total;
    return total;
}

/// Collect the algorithm (case) names that appear in CASE_RESULTS events.
[[nodiscard]] inline std::set<std::string> case_results_names(
    const std::vector<nlohmann::json>& events) {
    std::set<std::string> names;
    for (const auto& e : events)
        if (e.value("event", std::string{}) == "CASE_RESULTS")
            names.insert(e.value("case_name", std::string{}));
    return names;
}

/// The set of benchmark keys present in a report's `benchmarks` object.
[[nodiscard]] inline std::set<std::string> report_benchmark_keys(
    const nlohmann::json& report) {
    std::set<std::string> keys;
    if (report.contains("benchmarks"))
        for (const auto& [key, _] : report["benchmarks"].items()) keys.insert(key);
    return keys;
}

/// Collect the algorithm names in one benchmark's execution tuples.
[[nodiscard]] inline std::set<std::string> report_algorithms(
    const nlohmann::json& report, std::string_view bench_key) {
    std::set<std::string> algos;
    if (!report.contains("benchmarks")) return algos;
    const auto& benches = report["benchmarks"];
    if (!benches.contains(std::string{bench_key})) return algos;
    const auto& bench = benches[std::string{bench_key}];
    if (!bench.contains("execution_tuples")) return algos;
    for (const auto& tuple_record : bench["execution_tuples"])
        if (tuple_record.contains("tuple"))
            for (const auto& [algo, _] : tuple_record["tuple"].items()) algos.insert(algo);
    return algos;
}

// Filter and debug tests share a dummy parameter, SMALL and BIG inputs,
// and the sum_value metric.
inline constexpr std::array<algorithm_parameter_specification, 1> k_filter_parameters{{
    {"unused", "integer", "0", ""}
}};
inline constexpr std::array<test_case_data_specification, 2> k_filter_test_cases{{
    {"SMALL", "8 ints",  R"({"length":8,"seed":3})",  1},
    {"BIG",   "16 ints", R"({"length":16,"seed":5})", 1},
}};
inline constexpr std::array<metric_specification, 1> k_filter_metrics{{
    {"sum_value", "evaluation", "", "", "[]"},
}};

inline constexpr char k_small_seed_benchmark_key[] =
    "bench_prefilter_small_seeds";

[[nodiscard]] inline std::filesystem::path production_benchmark_configuration_path()
{
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        .parent_path().parent_path()
        / "bench" / "alignment_benchmark_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "bench" / "alignment_benchmark_configuration.json";
    }
    return config_path;
}

[[nodiscard]] inline nlohmann::json load_production_benchmark_configuration()
{
    std::ifstream config_stream{production_benchmark_configuration_path()};
    if (!config_stream.good()) return nlohmann::json::object();
    auto config = nlohmann::json::parse(
        config_stream, nullptr, /*allow_exceptions=*/false);
    if (config.is_discarded()) return nlohmann::json::object();
    return config;
}

/// Keep the production small-seed benchmark and the declarations it uses.
/// Tests then exercise its configured rules and repeat counts with small inputs.
[[nodiscard]] inline nlohmann::json small_seed_only_config(
    const nlohmann::json& production_config)
{
    const auto& benchmark = production_config.at(k_small_seed_benchmark_key);
    const auto& matrix = benchmark.at("matrix");

    nlohmann::json config = nlohmann::json::object();
    config["params"] = nlohmann::json::object();
    for (const auto& [algorithm, ignored] : matrix.at("over").items()) {
        (void)ignored;
        config["params"][algorithm] =
            production_config.at("params").at(algorithm);
    }
    config["test_cases"] = nlohmann::json::object();
    for (const auto& [test_case, ignored] : matrix.at("for").items()) {
        (void)ignored;
        config["test_cases"][test_case] =
            production_config.at("test_cases").at(test_case);
    }
    config[k_small_seed_benchmark_key] = benchmark;
    return config;
}

struct small_seed_rule_source {
    std::string algorithm;
    std::string test_case;

    [[nodiscard]] bool operator<(
        const small_seed_rule_source& other) const noexcept
    {
        return std::tie(algorithm, test_case)
            < std::tie(other.algorithm, other.test_case);
    }

    [[nodiscard]] bool operator==(
        const small_seed_rule_source& other) const noexcept = default;
};

enum class small_seed_rule_fault {
    none,
    unavailable_backend,
    nonfinite_metric,
    incomplete_metric,
};

struct small_seed_rule_fixture {
    std::map<small_seed_rule_source, double> source_values;
    std::vector<std::string> evaluation_metrics;
    std::optional<small_seed_rule_source> fault_source;
    std::size_t rule_count = 0u;
    small_seed_rule_fault fault = small_seed_rule_fault::none;
};

[[nodiscard]] inline small_seed_rule_source small_seed_rule_source_from_selector(
    const nlohmann::json& selector)
{
    return {
        .algorithm = selector.at("key").get<std::string>(),
        .test_case = selector.at("test_case").get<std::string>(),
    };
}

/// Build test timings from the production rules. Use gatekeeper_value for
/// each left-hand selector and 100.0 for each right-hand selector.
[[nodiscard]] inline small_seed_rule_fixture make_small_seed_rule_fixture(
    const nlohmann::json& small_seed_config,
    double gatekeeper_value,
    small_seed_rule_fault fault = small_seed_rule_fault::none)
{
    const auto& rules = small_seed_config.at(k_small_seed_benchmark_key)
        .at("matrix").at("metrics").at("backend_batch_ns").at("rules");
    small_seed_rule_fixture fixture;
    fixture.rule_count = rules.size();
    fixture.fault = fault;
    for (const auto& [metric_name, metric] :
         small_seed_config.at(k_small_seed_benchmark_key)
             .at("matrix").at("metrics").items()) {
        if (metric.value("type", std::string{"builtin"}) == "evaluation") {
            fixture.evaluation_metrics.push_back(metric_name);
        }
    }
    for (const auto& rule : rules) {
        const small_seed_rule_source lhs =
            small_seed_rule_source_from_selector(rule.at(1));
        const small_seed_rule_source rhs =
            small_seed_rule_source_from_selector(rule.at(3));
        fixture.source_values.insert_or_assign(lhs, gatekeeper_value);
        fixture.source_values.insert_or_assign(rhs, 100.0);
        if (!fixture.fault_source.has_value()) fixture.fault_source = lhs;
    }
    return fixture;
}

struct small_seed_rule_input {
    std::string test_case;
};

struct small_seed_rule_output {
    std::size_t source_ordinal = 0u;
};

/// Build the production small-seed benchmark with fixed test timings
/// in place of GPU work.
[[nodiscard]] inline erased_benchmark make_small_seed_rule_bench(
    const std::string& config_path,
    small_seed_rule_fixture fixture)
{
    using input_function = std::function<small_seed_rule_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>;
    using run_function = std::function<small_seed_rule_output(
        std::string_view, const parameter_map&, const small_seed_rule_input&)>;
    using evaluation_function = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const small_seed_rule_input&,
        const small_seed_rule_output&)>;
    using cleanup_function = std::function<void(
        std::string_view, std::string_view, simdjson::dom::element,
        const small_seed_rule_input&)>;

    estimator_function estimate = [](
        std::string_view, std::string_view,
        simdjson::dom::element) noexcept {
        return run_constraints{};
    };
    input_function input = [](
        std::string_view, std::string_view test_case,
        simdjson::dom::element, const run_constraints&) {
        return small_seed_rule_input{.test_case = std::string{test_case}};
    };
    auto source_ordinals = std::make_shared<
        std::map<small_seed_rule_source, std::size_t>>();
    run_function run_fn = [source_ordinals](
        std::string_view algorithm, const parameter_map&,
        const small_seed_rule_input& input_value) {
        small_seed_rule_source source{
            .algorithm = std::string{algorithm},
            .test_case = input_value.test_case,
        };
        auto& ordinal = (*source_ordinals)[source];
        return small_seed_rule_output{.source_ordinal = ordinal++};
    };
    evaluation_function evaluate = [fixture = std::move(fixture)](
        std::string_view algorithm, const parameter_map&,
        const small_seed_rule_input& input_value,
        const small_seed_rule_output& output) {
        const small_seed_rule_source source{
            .algorithm = std::string{algorithm},
            .test_case = input_value.test_case,
        };
        const bool faulted = fixture.fault_source.has_value()
            && source == *fixture.fault_source;
        const bool unavailable = faulted
            && fixture.fault == small_seed_rule_fault::unavailable_backend;

        std::map<std::string, double> metrics;
        for (const auto& metric : fixture.evaluation_metrics) {
            metrics.emplace(metric, 1.0);
        }
        metrics.insert_or_assign(
            "backend_available", unavailable ? 0.0 : 1.0);
        if (unavailable) {
            metrics.erase("backend_batch_ns");
            return metrics;
        }
        if (faulted
            && fixture.fault == small_seed_rule_fault::incomplete_metric
            && output.source_ordinal == 4u) {
            metrics.erase("backend_batch_ns");
            return metrics;
        }

        double backend_batch_ns = fixture.source_values.at(source);
        if (faulted
            && fixture.fault == small_seed_rule_fault::nonfinite_metric
            && output.source_ordinal == 4u) {
            backend_batch_ns =
                std::numeric_limits<double>::quiet_NaN();
        }
        metrics.insert_or_assign("backend_batch_ns", backend_batch_ns);
        return metrics;
    };

    return create_benchmark<small_seed_rule_input, small_seed_rule_output>(
        "GateKeeper small-seed rule regression fixture",
        std::string{k_small_seed_benchmark_key}, config_path,
        std::move(estimate), std::move(input), std::move(run_fn),
        std::move(evaluate), cleanup_function{});
}

}  // namespace

TEST_CASE("logical-repeat finalization is post-timing and pre-publication",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"logical_a", std::span<const algorithm_parameter_specification>{
            k_dummy_parameters}},
        {"logical_b", std::span<const algorithm_parameter_specification>{
            k_dummy_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"PAIR", "logical repeat", R"({})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"PAIR", 1},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2>
        over_entries{{{"logical_a", "{}"}, {"logical_b", "{}"}}};
    const std::array<metric_specification, 4> metrics{{
        {"wall_clock_ns", "builtin", "", "", "[]"},
        {"tuple_accepted", "evaluation", "", "", "[]"},
        {"repeat_finalized", "evaluation", "", "", "[]"},
        {"measured_region_ns_manual", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_logical_repeat_finalization",
            .description = "logical repeat finalization",
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries =
                std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
    }};
    const std::string config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    const std::string config_path = write_config_file(report_dir, config_text);
    std::atomic<int> finalization_calls{0};
    std::atomic<bool> finalizer_saw_complete_repeat{false};
    auto benchmark = create_benchmark<toy_input, toy_output>(
        "logical repeat finalization",
        "bench_logical_repeat_finalization",
        config_path,
        [](std::string_view, std::string_view,
           simdjson::dom::element) noexcept { return run_constraints{}; },
        [](std::string_view, std::string_view, simdjson::dom::element,
           const run_constraints&) { return toy_input{}; },
        [](std::string_view algorithm, const parameter_map&,
           const toy_input&) {
            const auto begin = bioseq::instrumentation::clock::now_ns();
            while (bioseq::instrumentation::clock::now_ns() - begin < 50'000) {
                /* Keep the measured region longer than its published duration. */
            }
            return toy_output{
                .sum = algorithm == "logical_a" ? 1'000 : 2'000,
                .parity_count = 0,
            };
        },
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) {
            return std::map<std::string, double>{
                {"tuple_accepted", 1.0},
                {"repeat_finalized",
                    output.parity_count == 3'000 ? 1.0 : 0.0},
                {"measured_region_ns_manual", static_cast<double>(output.sum)},
            };
        },
        {},
        [](std::string_view, const parameter_map&, const toy_input&,
           const toy_output& output) {
            return nlohmann::json{
                {"payload_schema", "bioseq.e2e.raw-sample"},
                {"payload_schema_version", 11},
                {"tuple_status", nlohmann::json{
                    {"accepted", 1}, {"disposition", "accepted"}}},
                {"e2e_interval_metric_contract", nlohmann::json{
                    {"schema", "bioseq.e2e.interval-metrics"},
                    {"version", 2},
                    {"framework_finalize_adjusted_metric", false}}},
                {"index_and_map_ns", output.sum},
                {"repeat_finalized", output.parity_count == 3'000},
                {"peer_sum", output.parity_count},
            }.dump();
        },
        {},
        repeat_finalization_callbacks<toy_input, toy_output>{
            .enabled = [](const toy_input&) noexcept { return true; },
            .published_wall_clock_ns = [](
                    std::string_view, const parameter_map&, const toy_input&,
                    const toy_output& output) {
                return static_cast<std::uint64_t>(output.sum);
            },
            .finalize_repeat = [
                    &finalization_calls,
                    &finalizer_saw_complete_repeat](
                    const toy_input&, const std::size_t repeat,
                    std::span<repeat_output_reference<toy_output>> outputs) {
                std::int64_t sum = 0;
                bool complete = repeat == 0 && outputs.size() == 2;
                for (auto& reference : outputs) {
                    complete = complete && reference.output != nullptr;
                    if (reference.output != nullptr) {
                        sum += reference.output->sum;
                    }
                }
                complete = complete && sum == 3'000;
                finalizer_saw_complete_repeat.store(
                    complete, std::memory_order_relaxed);
                for (auto& reference : outputs) {
                    if (reference.output != nullptr) {
                        reference.output->parity_count = sum;
                    }
                }
                finalization_calls.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            },
        });

    const auto outcome = run_with_args(
        report_dir, std::span<const std::string>{}, std::move(benchmark));
    INFO(outcome.captured_stderr);
    REQUIRE(outcome.exit_code == 0);
    REQUIRE(finalization_calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(finalizer_saw_complete_repeat.load(std::memory_order_relaxed));
    REQUIRE(stat_at(
        outcome.report, "bench_logical_repeat_finalization", "logical_a",
        "PAIR", "all_regions", "wall_clock_ns", "min") == 1'000.0);
    REQUIRE(stat_at(
        outcome.report, "bench_logical_repeat_finalization", "logical_b",
        "PAIR", "all_regions", "wall_clock_ns", "min") == 2'000.0);
    const auto& raw_samples = outcome.report["benchmarks"]
        ["bench_logical_repeat_finalization"]["raw_samples"];
    REQUIRE(raw_samples.size() == 2);
    for (const auto& raw : raw_samples) {
        REQUIRE(raw["repeat_finalized"] == true);
        REQUIRE(raw["peer_sum"] == 3'000);
        REQUIRE(raw["published_wall_clock_ns"] == raw["index_and_map_ns"]);
        REQUIRE(raw["outer_e2e_ns"].get<std::uint64_t>()
            >= raw["published_wall_clock_ns"].get<std::uint64_t>());
    }
}

TEST_CASE("iteration boundaries isolate immediate and deferred measurement",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"boundary_a", std::span<const algorithm_parameter_specification>{
            k_dummy_parameters}},
        {"boundary_b", std::span<const algorithm_parameter_specification>{
            k_dummy_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"BOUNDARY", "iteration boundary", R"({})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"BOUNDARY", 1},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2>
        over_entries{{{"boundary_a", "{}"}, {"boundary_b", "{}"}}};
    static constexpr std::array<metric_specification, 2> metrics{{
        {"wall_clock_ns", "builtin", "", "", "[]"},
        {"post_mutated", "evaluation", "", "", "[]"},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_iteration_boundaries",
            .description = "iteration boundaries",
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries =
                std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
        },
    }};
    const std::string config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    const std::string config_path = write_config_file(report_dir, config_text);

    struct move_only_preparation {
        void* outer_profiler = nullptr;
        std::int64_t before_end_ns = 0;
        move_only_preparation() = default;
        move_only_preparation(const move_only_preparation&) = delete;
        move_only_preparation& operator=(const move_only_preparation&) = delete;
        move_only_preparation(move_only_preparation&&) = default;
        move_only_preparation& operator=(move_only_preparation&&) = default;
    };
    struct move_only_output {
        std::int64_t inner_begin_ns = 0;
        std::int64_t inner_end_ns = 0;
        std::uint64_t measured_elapsed_ns = 0;
        bool post_mutated = false;
        move_only_output() = default;
        move_only_output(const move_only_output&) = delete;
        move_only_output& operator=(const move_only_output&) = delete;
        move_only_output(move_only_output&&) = default;
        move_only_output& operator=(move_only_output&&) = default;
    };

    bool boundaries_enabled = true;
    bool deferred_publication = false;
    SECTION("immediate publication") {}
    SECTION("deferred logical-repeat publication") {
        deferred_publication = true;
    }
    SECTION("disabled predicate preserves on_run") {
        boundaries_enabled = false;
    }

    std::map<std::string, int> stage{{"boundary_a", 0}, {"boundary_b", 0}};
    bool ordering_valid = true;
    bool profiler_scope_valid = true;
    bool window_valid = true;
    bool post_mutation_visible = true;
    std::size_t fallback_calls = 0;
    std::size_t finalization_calls = 0;
    using benchmark_profile =
        bioseq::instrumentation::benchmark_profiling_configuration;

    iteration_boundary_callbacks<
        toy_input, move_only_preparation, move_only_output> boundaries{
        .enabled = [&boundaries_enabled](
                std::string_view, const parameter_map&, const toy_input&) {
            return boundaries_enabled;
        },
        .before_iteration = [&stage, &ordering_valid](
                std::string_view algorithm, const parameter_map&,
                const toy_input&, std::size_t) {
            ordering_valid = ordering_valid && stage.at(std::string{algorithm}) == 0;
            stage.at(std::string{algorithm}) = 1;
            move_only_preparation preparation;
            preparation.outer_profiler =
                bioseq::instrumentation::active_thread_profiler<benchmark_profile>;
            preparation.before_end_ns =
                bioseq::instrumentation::clock::now_ns();
            return preparation;
        },
        .measured_iteration = [
                &stage, &ordering_valid, &profiler_scope_valid](
                std::string_view algorithm, const parameter_map&,
                const toy_input&, move_only_preparation& preparation) {
            ordering_valid = ordering_valid && stage.at(std::string{algorithm}) == 1;
            stage.at(std::string{algorithm}) = 2;
            profiler_scope_valid = profiler_scope_valid
                && bioseq::instrumentation::active_thread_profiler<benchmark_profile>
                    != preparation.outer_profiler;
            move_only_output output;
            output.inner_begin_ns = bioseq::instrumentation::clock::now_ns();
            output.inner_end_ns = bioseq::instrumentation::clock::now_ns();
            return output;
        },
        .after_iteration = [
                &stage, &ordering_valid, &profiler_scope_valid, &window_valid](
                std::string_view algorithm, const parameter_map&,
                const toy_input&, move_only_preparation& preparation,
                move_only_output& output,
                const iteration_measurement_window& window) {
            ordering_valid = ordering_valid && stage.at(std::string{algorithm}) == 2;
            stage.at(std::string{algorithm}) = 3;
            profiler_scope_valid = profiler_scope_valid
                && bioseq::instrumentation::active_thread_profiler<benchmark_profile>
                    == preparation.outer_profiler;
            window_valid = window_valid
                && window.begin_ns >= preparation.before_end_ns
                && window.begin_ns <= output.inner_begin_ns
                && window.end_ns >= output.inner_end_ns
                && window.end_ns >= window.begin_ns
                && window.elapsed_ns == static_cast<std::uint64_t>(
                    window.end_ns - window.begin_ns);
            output.measured_elapsed_ns = window.elapsed_ns;
            output.post_mutated = true;
        },
    };

    auto benchmark = create_benchmark_with_iteration_boundaries<
        toy_input, move_only_preparation, move_only_output>(
        "iteration boundaries", "bench_iteration_boundaries",
        config_path,
        [](std::string_view, std::string_view,
           simdjson::dom::element) noexcept { return run_constraints{}; },
        [](std::string_view, std::string_view, simdjson::dom::element,
           const run_constraints&) { return toy_input{}; },
        [&stage, &ordering_valid, &fallback_calls](
                std::string_view algorithm, const parameter_map&,
                const toy_input&) {
            ordering_valid = ordering_valid && stage.at(std::string{algorithm}) == 0;
            stage.at(std::string{algorithm}) = 1;
            ++fallback_calls;
            return move_only_output{};
        },
        [&stage, &ordering_valid, &post_mutation_visible,
         boundaries_enabled, deferred_publication](
                std::string_view algorithm, const parameter_map&,
                const toy_input&, const move_only_output& output) {
            const int expected_stage = boundaries_enabled
                ? (deferred_publication ? 4 : 3)
                : 1;
            ordering_valid = ordering_valid
                && stage.at(std::string{algorithm}) == expected_stage;
            stage.at(std::string{algorithm}) = expected_stage + 1;
            if (boundaries_enabled) {
                post_mutation_visible = post_mutation_visible
                    && output.post_mutated;
            }
            return std::map<std::string, double>{
                {"post_mutated", output.post_mutated ? 1.0 : 0.0},
            };
        },
        std::move(boundaries), {},
        [](std::string_view, const parameter_map&, const toy_input&,
           const move_only_output& output) {
            return nlohmann::json{
                {"post_mutated", output.post_mutated},
                {"observed_window_ns", output.measured_elapsed_ns},
            }.dump();
        }, {},
        repeat_finalization_callbacks<toy_input, move_only_output>{
            .enabled = [deferred_publication](const toy_input&) {
                return deferred_publication;
            },
            .published_wall_clock_ns = [](
                    std::string_view, const parameter_map&, const toy_input&,
                    const move_only_output& output) {
                return output.measured_elapsed_ns;
            },
            .finalize_repeat = [
                    &stage, &ordering_valid, &post_mutation_visible,
                    &finalization_calls](
                    const toy_input&, std::size_t,
                    std::span<repeat_output_reference<move_only_output>> outputs) {
                ++finalization_calls;
                for (auto& reference : outputs) {
                    const std::string algorithm{reference.algorithm_name};
                    ordering_valid = ordering_valid && stage.at(algorithm) == 3;
                    stage.at(algorithm) = 4;
                    post_mutation_visible = post_mutation_visible
                        && reference.output != nullptr
                        && reference.output->post_mutated;
                }
            },
        });

    const auto outcome = run_with_args(
        report_dir, std::span<const std::string>{}, std::move(benchmark));
    INFO(outcome.captured_stderr);
    REQUIRE(outcome.exit_code == 0);
    REQUIRE(ordering_valid);
    REQUIRE(profiler_scope_valid);
    REQUIRE(window_valid);
    REQUIRE(post_mutation_visible);
    REQUIRE(fallback_calls == (boundaries_enabled ? 0u : 2u));
    REQUIRE(finalization_calls == (deferred_publication ? 1u : 0u));
    const int expected_final_stage = boundaries_enabled
        ? (deferred_publication ? 5 : 4)
        : 2;
    REQUIRE(stage.at("boundary_a") == expected_final_stage);
    REQUIRE(stage.at("boundary_b") == expected_final_stage);
    const auto& raw_samples = outcome.report["benchmarks"]
        ["bench_iteration_boundaries"]["raw_samples"];
    REQUIRE(raw_samples.size() == 2u);
    for (const auto& raw : raw_samples) {
        REQUIRE(raw["post_mutated"] == boundaries_enabled);
        if (boundaries_enabled) {
            REQUIRE(raw["outer_e2e_ns"] == raw["observed_window_ns"]);
            REQUIRE(raw["published_wall_clock_ns"] == raw["observed_window_ns"]);
        }
    }
}

TEST_CASE("GateKeeper small-seed config binds fixed keys to E and exact rules",
          "[component:benchmark_framework][layer:contract][feature:configuration][resource:filesystem]") {
    const auto config_path = production_benchmark_configuration_path();
    REQUIRE(std::filesystem::exists(config_path));
    const nlohmann::json production_config =
        load_production_benchmark_configuration();
    REQUIRE(production_config.contains(k_small_seed_benchmark_key));

    const auto& benchmark = production_config.at(k_small_seed_benchmark_key);
    const auto& matrix = benchmark.at("matrix");
    REQUIRE(benchmark.at("concurrency").at("bench_lock") == true);
    REQUIRE(benchmark.at("concurrency").at("concurrency") == false);
    REQUIRE(matrix.at("for").size() == 4u);
    REQUIRE(matrix.at("over").size() == 4u);

    for (const auto& [test_case, repeats] : matrix.at("for").items()) {
        CAPTURE(test_case);
        REQUIRE(repeats.get<std::size_t>() == 5u);
    }

    const std::map<std::string, std::uint32_t> expected_gatekeeper_edits{
        {"gatekeeper_vulkan_e2", 2u},
        {"gatekeeper_vulkan_e4", 4u},
    };
    std::map<std::string, std::uint32_t> configured_gatekeeper_edits;
    for (const auto& [algorithm, ignored] : matrix.at("over").items()) {
        (void)ignored;
        if (!bioseq::bench::prefilter_algorithm_selection::
                is_gatekeeper_algorithm(algorithm)) {
            continue;
        }
        const auto max_edits = bioseq::bench::prefilter_algorithm_selection::
            fixed_max_edits_for_algorithm(algorithm);
        REQUIRE(max_edits.has_value());
        configured_gatekeeper_edits.emplace(algorithm, *max_edits);
    }
    REQUIRE(configured_gatekeeper_edits == expected_gatekeeper_edits);
    REQUIRE_FALSE(bioseq::bench::prefilter_algorithm_selection::
        fixed_max_edits_for_algorithm("gatekeeper_vulkan").has_value());

    using bioseq::bench::prefilter_algorithm_selection::
        hardware_validation_stats;
    const hardware_validation_stats valid_hardware_stats{
        .input_valid = true,
        .backend_available = true,
        .validation_performed = true,
        .validation_passed = true,
        .result_count = 4'096u,
        .expected_result_count = 4'096u,
    };
    REQUIRE(bioseq::bench::prefilter_algorithm_selection::
        has_required_hardware_validation(valid_hardware_stats));
    auto unavailable_hardware_stats = valid_hardware_stats;
    unavailable_hardware_stats.backend_available = false;
    REQUIRE_FALSE(bioseq::bench::prefilter_algorithm_selection::
        has_required_hardware_validation(unavailable_hardware_stats));

    const auto& rules = matrix.at("metrics")
        .at("backend_batch_ns").at("rules");
    REQUIRE(rules.size()
            == matrix.at("for").size()
                * expected_gatekeeper_edits.size());
    std::set<small_seed_rule_source> configured_lhs_sources;
    for (const auto& rule : rules) {
        REQUIRE(rule.size() == 4u);
        const auto& lhs_selector = rule.at(1);
        const auto& comparator = rule.at(2);
        const auto& rhs_selector = rule.at(3);
        const small_seed_rule_source lhs =
            small_seed_rule_source_from_selector(lhs_selector);
        const small_seed_rule_source rhs =
            small_seed_rule_source_from_selector(rhs_selector);
        CAPTURE(lhs.algorithm, lhs.test_case, rhs.algorithm, rhs.test_case);

        REQUIRE(matrix.at("for").contains(lhs.test_case));
        REQUIRE(lhs.test_case == rhs.test_case);
        const auto lhs_edits = bioseq::bench::prefilter_algorithm_selection::
            fixed_max_edits_for_algorithm(lhs.algorithm);
        const auto rhs_edits = bioseq::bench::prefilter_algorithm_selection::
            fixed_max_edits_for_algorithm(rhs.algorithm);
        REQUIRE(lhs_edits.has_value());
        REQUIRE(rhs_edits.has_value());
        REQUIRE(*lhs_edits == *rhs_edits);
        REQUIRE(bioseq::bench::prefilter_algorithm_selection::
            is_gatekeeper_algorithm(lhs.algorithm));
        REQUIRE_FALSE(bioseq::bench::prefilter_algorithm_selection::
            is_gatekeeper_algorithm(rhs.algorithm));

        REQUIRE(comparator.at("compare") == "<");
        REQUIRE(comparator.at("threshold%") == 10);
        REQUIRE(comparator.at("tolerance%") == 0);
        REQUIRE(comparator.at("aggregate") == "p50");
        REQUIRE(comparator.at("pairing") == "independent");
        REQUIRE(comparator.at("blocking") == true);
        REQUIRE(configured_lhs_sources.insert(lhs).second);
    }
}

TEST_CASE("production small-seed rules enforce the strict 0.900 ratio boundary",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    const nlohmann::json production_config =
        load_production_benchmark_configuration();
    REQUIRE(production_config.contains(k_small_seed_benchmark_key));
    const nlohmann::json config =
        small_seed_only_config(production_config);

    const auto execute = [&config](small_seed_rule_fixture fixture) {
        temporary_report_directory report_dir;
        const std::string config_path = write_config_file(
            report_dir, config.dump());
        return run_with_args(
            report_dir, std::span<const std::string>{},
            make_small_seed_rule_bench(config_path, std::move(fixture)));
    };

    const auto exact_fixture = make_small_seed_rule_fixture(config, 90.0);
    const std::size_t rule_count = exact_fixture.rule_count;
    const auto exact_boundary = execute(exact_fixture);
    REQUIRE(exact_boundary.exit_code == 1);
    REQUIRE(exact_boundary.report.at("rule_violations").size()
            == rule_count);
    for (const auto& violation :
         exact_boundary.report.at("rule_violations")) {
        REQUIRE(violation.at("blocking") == true);
        REQUIRE(violation.at("args_present")
                == nlohmann::json::array({true, true}));
        REQUIRE(violation.at("args").at(0).get<double>() == 90.0);
        REQUIRE(violation.at("args").at(1).get<double>() == 100.0);
    }

    const double immediately_below_boundary = std::nextafter(90.0, 0.0);
    REQUIRE(immediately_below_boundary / 100.0 < 0.900);
    const auto below_boundary = execute(make_small_seed_rule_fixture(
        config, immediately_below_boundary));
    REQUIRE(below_boundary.exit_code == 0);
    REQUIRE(below_boundary.report.at("rule_violations").empty());
}

TEST_CASE("production small-seed selectors reject wrong keys and fixtures",
          "[component:benchmark_framework][layer:contract][feature:selection][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    const nlohmann::json production_config =
        load_production_benchmark_configuration();
    REQUIRE(production_config.contains(k_small_seed_benchmark_key));
    const nlohmann::json original_config =
        small_seed_only_config(production_config);

    const auto execute = [](const nlohmann::json& config) {
        temporary_report_directory report_dir;
        const std::string config_path = write_config_file(
            report_dir, config.dump());
        return run_with_args(
            report_dir, std::span<const std::string>{},
            make_small_seed_rule_bench(
                config_path, make_small_seed_rule_fixture(config, 89.0)));
    };

    SECTION("unknown algorithm key") {
        nlohmann::json config = original_config;
        auto& selector = config[k_small_seed_benchmark_key]["matrix"]
            ["metrics"]["backend_batch_ns"]["rules"][0][1];
        selector["key"] =
            selector.at("key").get<std::string>() + "_wrong";
        const auto outcome = execute(config);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr, "unknown_algorithm"));
    }

    SECTION("unknown fixture key") {
        nlohmann::json config = original_config;
        auto& selector = config[k_small_seed_benchmark_key]["matrix"]
            ["metrics"]["backend_batch_ns"]["rules"][0][1];
        selector["test_case"] =
            selector.at("test_case").get<std::string>() + "_wrong";
        const auto outcome = execute(config);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr, "unknown_rule_test_case"));
    }
}

TEST_CASE("production small-seed rules reject unavailable nonfinite and incomplete stats",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    const nlohmann::json production_config =
        load_production_benchmark_configuration();
    REQUIRE(production_config.contains(k_small_seed_benchmark_key));
    const nlohmann::json config =
        small_seed_only_config(production_config);
    const auto& rules = config.at(k_small_seed_benchmark_key)
        .at("matrix").at("metrics").at("backend_batch_ns").at("rules");
    REQUIRE_FALSE(rules.empty());
    const std::string expected_rule_name =
        rules.at(0).at(0).get<std::string>();
    const small_seed_rule_source fault_source =
        small_seed_rule_source_from_selector(rules.at(0).at(1));

    const auto execute = [&config](small_seed_rule_fault fault) {
        temporary_report_directory report_dir;
        const std::string config_path = write_config_file(
            report_dir, config.dump());
        return run_with_args(
            report_dir, std::span<const std::string>{},
            make_small_seed_rule_bench(
                config_path,
                make_small_seed_rule_fixture(config, 89.0, fault)));
    };
    const auto require_single_missing_lhs = [
        &expected_rule_name](const run_outcome& outcome) {
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(outcome.report.at("rule_violations").size() == 1u);
        const auto& violation =
            outcome.report.at("rule_violations").at(0);
        REQUIRE(violation.at("rule_name") == expected_rule_name);
        REQUIRE(violation.at("blocking") == true);
        REQUIRE(violation.at("args_present")
                == nlohmann::json::array({false, true}));
    };

    SECTION("backend unavailable") {
        const auto outcome = execute(
            small_seed_rule_fault::unavailable_backend);
        require_single_missing_lhs(outcome);
        const auto metric = aggregate_at(
            outcome.report, k_small_seed_benchmark_key,
            fault_source.algorithm, fault_source.test_case,
            "all_regions", "backend_batch_ns");
        REQUIRE_FALSE(metric.has_value());
        REQUIRE(stat_at(
            outcome.report, k_small_seed_benchmark_key,
            fault_source.algorithm, fault_source.test_case,
            "all_regions", "backend_available", "max") == 0.0);
    }

    SECTION("one non-finite timing sample") {
        const auto outcome = execute(
            small_seed_rule_fault::nonfinite_metric);
        require_single_missing_lhs(outcome);
        const auto metric = aggregate_at(
            outcome.report, k_small_seed_benchmark_key,
            fault_source.algorithm, fault_source.test_case,
            "all_regions", "backend_batch_ns");
        REQUIRE(metric.has_value());
        REQUIRE(metric->at("samples_count") == 5u);
        REQUIRE(metric->at("k-best").at("top10").size() == 4u);
        REQUIRE(stat_at(
            outcome.report, k_small_seed_benchmark_key,
            fault_source.algorithm, fault_source.test_case,
            "all_regions", "backend_available", "min") == 1.0);
    }

    SECTION("only four of five timing samples") {
        const auto outcome = execute(
            small_seed_rule_fault::incomplete_metric);
        require_single_missing_lhs(outcome);
        REQUIRE(samples_count_at(
            outcome.report, k_small_seed_benchmark_key,
            fault_source.algorithm, fault_source.test_case,
            "all_regions", "backend_batch_ns") == 4u);
        REQUIRE(samples_count_at(
            outcome.report, k_small_seed_benchmark_key,
            fault_source.algorithm, fault_source.test_case,
            "all_regions", "backend_available") == 5u);
    }
}

TEST_CASE("XXL_HIFI relative benchmark config has one input and nine rows",
          "[component:benchmark_framework][layer:contract][feature:configuration][resource:filesystem]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        .parent_path().parent_path()
        / "bench" / "alignment_benchmark_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "bench" / "alignment_benchmark_configuration.json";
    }
    REQUIRE(std::filesystem::exists(config_path));

    std::ifstream config_stream{config_path};
    REQUIRE(config_stream.good());
    const nlohmann::json config = nlohmann::json::parse(config_stream);
    REQUIRE(config.contains("bench_aligners_xxl_hifi_relative"));

    const auto& relative = config.at("bench_aligners_xxl_hifi_relative");
    const auto& matrix = relative.at("matrix");
    const auto& for_each = matrix.at("for");
    REQUIRE(for_each.size() == 1);
    REQUIRE(for_each.at("XXL_HIFI").get<std::size_t>() == 4);
    REQUIRE(config.at("test_cases").at("XXL_HIFI")
                .at("concurrency").get<std::size_t>() == 1);
    REQUIRE(config.at("test_cases").at("XXL_HIFI").at("data")
                .at("reference_length").get<std::uint64_t>()
            >= (std::uint64_t{256} << 20u));

    const std::set<std::string> expected_algorithms{
        "minimap2_dp",
        "bioseq_ksw2_scalar_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_two_stack_nthash",
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
        "bioseq_ksw2_batched_simd_dp_two_stack_nthash",
        "bioseq_block_simd_dp_two_stack_nthash",
        "bioseq_block_vulkan_dp_vulkan_minimap_int",
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    std::set<std::string> configured_algorithms;
    for (const auto& [algorithm, ignored] : matrix.at("over").items()) {
        (void)ignored;
        configured_algorithms.insert(algorithm);
    }
    REQUIRE(configured_algorithms == expected_algorithms);

    const auto& acceptance = matrix.at("acceptance");
    REQUIRE(acceptance.at("exact_algorithm_rows").is_array());
    std::set<std::string> accepted_algorithms;
    for (const auto& algorithm : acceptance.at("exact_algorithm_rows")) {
        accepted_algorithms.insert(algorithm.get<std::string>());
    }
    REQUIRE(accepted_algorithms == expected_algorithms);
    REQUIRE(acceptance.at("exact_test_case_iterations").size() == 1);
    REQUIRE(acceptance.at("exact_test_case_iterations")
                .at("XXL_HIFI").get<std::size_t>() == 4);
    REQUIRE(acceptance.at("require_named_region_iteration_coverage")
                .get<bool>());

    const auto selector_key = [](const nlohmann::json& selector) {
        return fmt::format("{}|{}|{}|{}",
            selector.at("algorithm").get<std::string>(),
            selector.at("test_case").get<std::string>(),
            selector.at("region").get<std::string>(),
            selector.at("metric").get<std::string>());
    };
    const std::map<std::string, std::size_t>
        expected_named_region_sample_multiplicities{
            {"bioseq_ksw2_simd_dp_two_stack_nthash|XXL_HIFI|mm_sketch_simd|region_total_ns", 4},
            {"bioseq_ksw2_scalar_dp_scalar_mm|XXL_HIFI|mm_sketch|region_total_ns", 8},
            {"bioseq_ksw2_simd_dp_two_stack_nthash|XXL_HIFI|mg_lchain_simd|region_total_ns", 4},
            {"bioseq_ksw2_scalar_dp_scalar_mm|XXL_HIFI|mg_lchain_dp|region_total_ns", 4},
            {"bioseq_ksw2_simd_dp_two_stack_nthash|XXL_HIFI|backtrack_chains|region_total_ns", 4},
            {"bioseq_ksw2_scalar_dp_scalar_mm|XXL_HIFI|backtrack_chains|region_total_ns", 4},
            {"bioseq_block_simd_dp_two_stack_nthash|XXL_HIFI|mm_align_skeleton|region_total_ns", 4},
            {"bioseq_ksw2_simd_dp_two_stack_nthash|XXL_HIFI|mm_align_skeleton|region_total_ns", 4},
        };
    const auto& configured_multiplicities =
        acceptance.at("exact_named_region_sample_multiplicities");
    REQUIRE(configured_multiplicities.is_array());
    REQUIRE(configured_multiplicities.size()
            == expected_named_region_sample_multiplicities.size());
    std::map<std::string, std::size_t> actual_named_region_multiplicities;
    for (const auto& selector : configured_multiplicities) {
        REQUIRE(selector.size() == 5);
        const auto [ignored, inserted] =
            actual_named_region_multiplicities.emplace(
                selector_key(selector),
                selector.at("exact_samples").get<std::size_t>());
        (void)ignored;
        REQUIRE(inserted);
    }
    REQUIRE(actual_named_region_multiplicities
            == expected_named_region_sample_multiplicities);
    const std::string scalar_sketch_coordinate =
        "bioseq_ksw2_scalar_dp_scalar_mm|XXL_HIFI|mm_sketch|"
        "region_total_ns";
    for (const auto& [coordinate, exact_samples] :
         expected_named_region_sample_multiplicities) {
        REQUIRE(exact_samples % 4 == 0);
        const std::size_t exact_samples_per_iteration = exact_samples / 4;
        REQUIRE(exact_samples_per_iteration
                == (coordinate == scalar_sketch_coordinate ? 2 : 1));
    }

    const auto& metrics = matrix.at("metrics");
    REQUIRE(metrics.at("wall_clock_ns").at("rules").size() == 8);
    REQUIRE(metrics.at("region_total_ns").at("rules").size() == 4);
    REQUIRE(metrics.at("match_count").at("rules").size() == 8);
    REQUIRE(metrics.at("primary_correct_fraction").at("rules").size() == 8);
    REQUIRE(metrics.contains("mixed_gpu_pipeline_selected"));
    REQUIRE(metrics.contains("all_gpu_acceptance_eligible"));
    REQUIRE(relative.at("description").get<std::string>().find(
                "mixed GPU/CPU Vulkan wall ceiling") != std::string::npos);
    REQUIRE(relative.at("description").get<std::string>().find(
                "route-validated resident GPU-index frontend")
            != std::string::npos);
    for (const auto* mixed_gpu_cpu_algorithm : {
            "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_resident_gpu_index_frontend"}) {
        const std::string description = config.at("params")
            .at(mixed_gpu_cpu_algorithm).at("_").at("description")
            .get<std::string>();
        REQUIRE(description.find("Mixed GPU/CPU") != std::string::npos);
        REQUIRE(description.find("not all-GPU acceptance stats")
                != std::string::npos);
    }
    const std::set<std::string> expected_rule_names{
        "report76_xxl_hifi_block_simd_over_ksw2_simd_wall_ratio_max_1_057921",
        "complete_reports120_148_151_153_xxl_hifi_ksw2_batched_over_ksw2_simd_wall_ratio_max_1_005000",
        "report76_xxl_hifi_vulkan_tiled_over_ksw2_simd_wall_ratio_max_1_011221",
        "report76_xxl_hifi_legacy_block_vulkan_over_block_simd_wall_ratio_max_1_714040",
        "report76_xxl_hifi_resident_gpu_frontend_block_vulkan_over_block_simd_wall_ratio_max_1_722027",
        "xxl_resident_gpu_frontend_wall_less_than_route_validated_source_direct",
        "report76_xxl_hifi_ksw2_simd_over_minimap2_wall_ratio_max_0_797045",
        "report76_xxl_hifi_ksw2_simd_over_scalar_wall_ratio_max_0_540314",
        "complete_paired_medians_xxl_hifi_nthash_simd_over_scalar_sketch_ratio_max_0_214000",
        "complete_paired_medians_xxl_hifi_chaining_simd_over_scalar_ratio_max_0_600000",
        "report76_xxl_hifi_backtrack_simd_over_scalar_ratio_max_0_955048972",
        "report76_xxl_hifi_block_over_ksw2_alignment_ratio_max_0_591278239",
    };
    std::set<std::string> configured_rule_names;
    for (const auto& metric_name : {"wall_clock_ns", "region_total_ns"}) {
        for (const auto& rule : metrics.at(metric_name).at("rules")) {
            configured_rule_names.insert(rule.at(0).get<std::string>());
        }
    }
    REQUIRE(configured_rule_names == expected_rule_names);

    const auto& xxl_wall_rules =
        metrics.at("wall_clock_ns").at("rules");
    const auto direct_comparison = std::find_if(
        xxl_wall_rules.begin(), xxl_wall_rules.end(),
        [](const auto& rule) {
            return rule.at(0)
                == "xxl_resident_gpu_frontend_wall_less_than_route_validated_source_direct";
        });
    REQUIRE(direct_comparison != xxl_wall_rules.end());
    REQUIRE(direct_comparison->at(1).at("key")
            == "bioseq_block_vulkan_resident_gpu_index_frontend");
    REQUIRE(direct_comparison->at(1).at("test_case") == "XXL_HIFI");
    REQUIRE(direct_comparison->at(2).at("compare") == "<");
    REQUIRE(direct_comparison->at(2).at("threshold%") == 0);
    REQUIRE(direct_comparison->at(2).at("tolerance%") == 0);
    REQUIRE(direct_comparison->at(2).at("aggregate") == "p50");
    REQUIRE_FALSE(direct_comparison->at(2).contains("blocking"));
    REQUIRE(direct_comparison->at(3).at("key")
            == "bioseq_block_vulkan_dp_vulkan_minimap_int");
    REQUIRE(direct_comparison->at(3).at("test_case") == "XXL_HIFI");

    const std::map<std::string, double> expected_policy_max_ratios{
        {"report76_xxl_hifi_block_simd_over_ksw2_simd_wall_ratio_max_1_057921",
         curated_alignment_reference_p50::block_wall / curated_alignment_reference_p50::simd_wall},
        {"complete_reports120_148_151_153_xxl_hifi_ksw2_batched_over_ksw2_simd_wall_ratio_max_1_005000",
         1.005},
        {"report76_xxl_hifi_vulkan_tiled_over_ksw2_simd_wall_ratio_max_1_011221",
         curated_alignment_reference_p50::vulkan_wall / curated_alignment_reference_p50::simd_wall},
        {"report76_xxl_hifi_legacy_block_vulkan_over_block_simd_wall_ratio_max_1_714040",
         17'180'439'666.0 / curated_alignment_reference_p50::block_wall},
        {"report76_xxl_hifi_resident_gpu_frontend_block_vulkan_over_block_simd_wall_ratio_max_1_722027",
         17'260'490'000.0 / curated_alignment_reference_p50::block_wall},
        {"xxl_resident_gpu_frontend_wall_less_than_route_validated_source_direct",
         1.0},
        {"report76_xxl_hifi_ksw2_simd_over_minimap2_wall_ratio_max_0_797045",
         curated_alignment_reference_p50::simd_wall / curated_alignment_reference_p50::minimap2_wall},
        {"report76_xxl_hifi_ksw2_simd_over_scalar_wall_ratio_max_0_540314",
         curated_alignment_reference_p50::simd_wall / curated_alignment_reference_p50::scalar_wall},
        {"complete_paired_medians_xxl_hifi_nthash_simd_over_scalar_sketch_ratio_max_0_214000",
         0.214},
        {"complete_paired_medians_xxl_hifi_chaining_simd_over_scalar_ratio_max_0_600000",
         0.600},
        {"report76_xxl_hifi_backtrack_simd_over_scalar_ratio_max_0_955048972",
         curated_alignment_reference_p50::simd_backtrack / curated_alignment_reference_p50::scalar_backtrack},
        {"report76_xxl_hifi_block_over_ksw2_alignment_ratio_max_0_591278239",
         curated_alignment_reference_p50::block_alignment / curated_alignment_reference_p50::simd_alignment},
    };
    for (const auto& metric_name : {"wall_clock_ns", "region_total_ns"}) {
        for (const auto& rule : metrics.at(metric_name).at("rules")) {
            const std::string rule_name = rule.at(0).get<std::string>();
            const auto& comparator = rule.at(2);
            REQUIRE(comparator.at("compare").get<std::string>() == "<");
            REQUIRE(comparator.at("aggregate").get<std::string>() == "p50");
            const double configured_max_ratio =
                1.0
                - comparator.value("threshold%", 0.0) / 100.0
                + comparator.value("tolerance%", 0.0) / 100.0;
            const double allowed_rounding = metric_name == "wall_clock_ns"
                ? 0.000001
                : 0.000000001;
            REQUIRE(configured_max_ratio == Catch::Approx(
                        expected_policy_max_ratios.at(rule_name))
                        .margin(allowed_rounding));
            if (rule_name
                == "report76_xxl_hifi_vulkan_tiled_over_ksw2_simd_wall_ratio_max_1_011221") {
                REQUIRE(comparator.at("blocking") == false);
            } else {
                REQUIRE_FALSE(comparator.contains("blocking"));
            }
        }
    }

    const auto require_strict_correctness_rules = [&metrics](
            std::string_view metric_name) {
        std::set<std::string> covered_algorithms{"minimap2_dp"};
        for (const auto& rule : metrics.at(std::string{metric_name}).at("rules")) {
            REQUIRE(rule.at(2).at("compare").get<std::string>() == "=");
            REQUIRE(rule.at(2).at("tolerance%").get<double>() == 0.0);
            REQUIRE(rule.at(2).at("aggregate").get<std::string>() == "p50");
            REQUIRE(rule.at(3).at("key").get<std::string>() == "minimap2_dp");
            covered_algorithms.insert(rule.at(1).at("key").get<std::string>());
        }
        return covered_algorithms;
    };
    REQUIRE(require_strict_correctness_rules("match_count")
            == expected_algorithms);
    REQUIRE(require_strict_correctness_rules("primary_correct_fraction")
            == expected_algorithms);
    REQUIRE_FALSE(config.at("bench_aligners_curated")
                          .at("matrix").at("for").contains("XXL_HIFI"));
}

TEST_CASE("default and signoff E2E matrices are exact six-cell cold minimizer-fragment comparisons",
          "[component:benchmark_framework][layer:contract][feature:configuration][resource:filesystem]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        .parent_path().parent_path()
        / "bench" / "bench_e2e_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "bench" / "bench_e2e_configuration.json";
    }
    REQUIRE(std::filesystem::exists(config_path));

    std::ifstream config_stream{config_path};
    REQUIRE(config_stream.good());
    const nlohmann::json config = nlohmann::json::parse(config_stream);

    const std::set<std::string> expected_cases{
        "E2E_2GBP_DNA4",
        "E2E_2GBP_DNA5",
        "E2E_4GBP_DNA4",
        "E2E_4GBP_DNA5",
    };
    const std::set<std::string> expected_algorithms{
        "bioseq_block_simd_dp_two_stack_nthash_no_index",
        "bioseq_block_simd_dp_two_stack_nthash_just_index",
        "bioseq_block_gpu_dp_gpu_seed_no_index",
        "bioseq_block_gpu_dp_gpu_seed_just_index",
        "minimap2_no_index",
        "minimap2_just_index",
    };
    static constexpr std::array<std::string_view, 4> bioseq_algorithms{
        "bioseq_block_simd_dp_two_stack_nthash_no_index",
        "bioseq_block_simd_dp_two_stack_nthash_just_index",
        "bioseq_block_gpu_dp_gpu_seed_no_index",
        "bioseq_block_gpu_dp_gpu_seed_just_index",
    };
    static constexpr std::array<std::string_view, 2> minimap_algorithms{
        "minimap2_no_index",
        "minimap2_just_index",
    };
    static constexpr std::array<std::pair<std::string_view, std::size_t>, 2>
        blocks{{
            {"bench_e2e", 3},
            {"bench_e2e_signoff", 5},
        }};

    REQUIRE(config.at("params").contains(
        "bioseq_ksw2_scalar_dp_scalar_mm_no_index"));
    REQUIRE(config.at("params").contains(
        "bioseq_ksw2_scalar_dp_scalar_mm_just_index"));

    for (const auto& [block_name, repetitions] : blocks) {
        REQUIRE(config.contains(std::string{block_name}));
        const auto& block = config.at(std::string{block_name});
        REQUIRE_FALSE(block.at("fast").get<bool>());
        REQUIRE(block.at("concurrency").size() == 2);
        REQUIRE(block.at("concurrency").at("bench_lock").get<bool>());
        REQUIRE_FALSE(block.at("concurrency").at("concurrency").get<bool>());
        REQUIRE(block.at("iteration_order").size() == 2);
        REQUIRE(block.at("iteration_order").at("mode").get<std::string>()
                == "interleaved_seeded");
        REQUIRE(block.at("iteration_order").at("seed").get<std::size_t>()
                == 370);

        const nlohmann::json expected_tags = repetitions == 3
            ? nlohmann::json::array(
                {"e2e", "main", "accelerator-comparison", "cold-index"})
            : nlohmann::json::array(
                {"e2e", "signoff", "accelerator-comparison", "cold-index"});
        REQUIRE(block.at("tags") == expected_tags);

        const auto& matrix = block.at("matrix");
        REQUIRE(matrix.at("metrics").contains(
            "persisted_policy_audit_accepted"));
        REQUIRE(matrix.at("for").size() == expected_cases.size());
        std::set<std::string> configured_cases;
        for (const auto& [test_case, count] : matrix.at("for").items()) {
            configured_cases.insert(test_case);
            REQUIRE(count.get<std::size_t>() == repetitions);
        }
        REQUIRE(configured_cases == expected_cases);

        const auto& over = matrix.at("over");
        REQUIRE(over.size() == expected_algorithms.size());
        std::set<std::string> configured_algorithms;
        for (const auto& [algorithm, ignored] : over.items()) {
            (void)ignored;
            configured_algorithms.insert(algorithm);
        }
        REQUIRE(configured_algorithms == expected_algorithms);

        for (const std::string_view algorithm : bioseq_algorithms) {
            const auto& options = over.at(std::string{algorithm});
            REQUIRE(options.size() == 2);
            REQUIRE(options.at("minimizer_index")
                    == nlohmann::json::array({"minimizers_per_fragment"}));
            REQUIRE(options.at("pipeline_mode")
                    == nlohmann::json::array({"task_parallel"}));
        }
        for (const std::string_view algorithm : minimap_algorithms) {
            const auto& options = over.at(std::string{algorithm});
            REQUIRE(options.size() == 1);
            REQUIRE(options.at("threads") == nlohmann::json::array({0}));
        }

        const auto& acceptance = matrix.at("acceptance");
        std::set<std::string> accepted_algorithms;
        for (const auto& algorithm : acceptance.at("exact_algorithm_rows")) {
            accepted_algorithms.insert(algorithm.get<std::string>());
        }
        REQUIRE(accepted_algorithms == expected_algorithms);
        REQUIRE(acceptance.at("exact_test_case_iterations")
                    == matrix.at("for"));
    }

    REQUIRE(config.at("bench_e2e").at("matrix").at("over")
            == config.at("bench_e2e_signoff").at("matrix").at("over"));
    REQUIRE(config.at("bench_e2e").at("matrix").at("metrics")
            == config.at("bench_e2e_signoff").at("matrix").at("metrics"));
}

TEST_CASE("XXL_HIFI paired companion is serial interleaved and ratio-paired",
          "[component:benchmark_framework][layer:contract][feature:configuration][resource:filesystem]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        .parent_path().parent_path()
        / "bench" / "alignment_benchmark_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "bench" / "alignment_benchmark_configuration.json";
    }
    REQUIRE(std::filesystem::exists(config_path));

    std::ifstream config_stream{config_path};
    REQUIRE(config_stream.good());
    const nlohmann::json config = nlohmann::json::parse(config_stream);
    REQUIRE(config.contains("bench_aligners_xxl_hifi_relative"));
    REQUIRE(config.contains("bench_aligners_xxl_hifi_paired"));

    // The four-repeat monitoring benchmark compares independent P50 values.
    // Its mixed GPU/CPU Vulkan limit is nonblocking.
    const auto& relative =
        config.at("bench_aligners_xxl_hifi_relative");
    REQUIRE(relative.at("matrix").at("for").at("XXL_HIFI") == 4);
    REQUIRE(relative.at("concurrency").at("bench_lock") == true);
    REQUIRE(relative.at("concurrency").at("concurrency") == false);
    for (const auto& metric_name : {"wall_clock_ns", "region_total_ns"}) {
        for (const auto& rule :
             relative.at("matrix").at("metrics")
                 .at(metric_name).at("rules")) {
            REQUIRE_FALSE(rule.at(2).contains("pairing"));
            REQUIRE_FALSE(rule.at(2).contains("paired_baseline"));
            if (rule.at(0)
                == "report76_xxl_hifi_vulkan_tiled_over_ksw2_simd_wall_ratio_max_1_011221") {
                REQUIRE(rule.at(2).at("blocking") == false);
            } else {
                REQUIRE_FALSE(rule.at(2).contains("blocking"));
            }
            REQUIRE_FALSE(rule.at(2).contains("warmup_repetitions"));
        }
    }

    const auto& paired = config.at("bench_aligners_xxl_hifi_paired");
    REQUIRE(paired.at("fast") == false);
    REQUIRE(paired.at("concurrency").at("bench_lock") == true);
    REQUIRE(paired.at("concurrency").at("concurrency") == false);
    REQUIRE(paired.at("iteration_order").at("mode")
            == "interleaved_seeded");
    REQUIRE(paired.at("iteration_order").at("seed") == 370076);
    REQUIRE(config.at("test_cases").at("XXL_HIFI_PAIRED")
                .at("concurrency") == 1);
    REQUIRE(config.at("test_cases").at("XXL_HIFI_PAIRED").at("data")
            == config.at("test_cases").at("XXL_HIFI").at("data"));

    const auto& matrix = paired.at("matrix");
    REQUIRE(matrix.at("for").size() == 1);
    REQUIRE(matrix.at("for").at("XXL_HIFI_PAIRED") == 11);
    const std::set<std::string> expected_algorithms{
        "minimap2_dp",
        "bioseq_ksw2_scalar_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_two_stack_nthash",
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
        "bioseq_ksw2_batched_simd_dp_two_stack_nthash",
        "bioseq_block_simd_dp_two_stack_nthash",
        "bioseq_block_vulkan_dp_vulkan_minimap_int",
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    std::set<std::string> configured_algorithms;
    for (const auto& [algorithm, ignored] : matrix.at("over").items()) {
        (void)ignored;
        configured_algorithms.insert(algorithm);
    }
    REQUIRE(configured_algorithms == expected_algorithms);
    std::set<std::string> accepted_algorithms;
    for (const auto& algorithm :
         matrix.at("acceptance").at("exact_algorithm_rows")) {
        accepted_algorithms.insert(algorithm.get<std::string>());
    }
    REQUIRE(accepted_algorithms == expected_algorithms);
    REQUIRE(matrix.at("acceptance").at("exact_test_case_iterations")
                .at("XXL_HIFI_PAIRED") == 11);
    REQUIRE(matrix.at("acceptance")
                .at("require_named_region_iteration_coverage") == true);

    const auto& multiplicities = matrix.at("acceptance")
        .at("exact_named_region_sample_multiplicities");
    REQUIRE(multiplicities.size() == 8);
    for (const auto& source : multiplicities) {
        const bool scalar_sketch =
            source.at("algorithm") == "bioseq_ksw2_scalar_dp_scalar_mm"
            && source.at("region") == "mm_sketch";
        REQUIRE(source.at("exact_samples") == (scalar_sketch ? 22 : 11));
    }

    const auto& metrics = matrix.at("metrics");
    REQUIRE(metrics.at("wall_clock_ns").at("rules").size() == 9);
    REQUIRE(metrics.at("region_total_ns").at("rules").size() == 4);
    std::set<std::string> paired_rule_names;
    for (const auto& metric_name : {"wall_clock_ns", "region_total_ns"}) {
        for (const auto& rule : metrics.at(metric_name).at("rules")) {
            const std::string rule_name = rule.at(0).get<std::string>();
            paired_rule_names.insert(rule_name);
            const auto& comparator = rule.at(2);
            REQUIRE(comparator.at("pairing") == "by_iteration_ratio");
            if (rule_name
                == "paired_resident_gpu_frontend_over_source_direct_wall_ratio_lt_1") {
                REQUIRE(comparator.value("blocking", true) == true);
            } else {
                REQUIRE(comparator.at("blocking") == false);
            }
            REQUIRE(comparator.at("warmup_repetitions") == 1);
            REQUIRE(comparator.at("aggregate") == "p50");
            REQUIRE(comparator.at("compare") == "<");
            const double max_ratio =
                comparator.at("paired_baseline").get<double>()
                * (1.0 - comparator.value("threshold%", 0.0) / 100.0
                   + comparator.value("tolerance%", 0.0) / 100.0);
            REQUIRE(max_ratio < 2.0);
        }
    }
    REQUIRE(paired_rule_names.contains(
        "paired_diagnostic_simd_chain_scalar_mm_no_material_wall_reversal"));
    REQUIRE(paired_rule_names.contains(
        "paired_resident_gpu_frontend_over_source_direct_wall_ratio_lt_1"));
    REQUIRE(paired_rule_names.size() == 13);

    const auto require_max_ratio = [&metrics](
        const std::string_view rule_name,
        const double baseline_ratio,
        const double relative_noise) {
        for (const auto& metric_name : {"wall_clock_ns", "region_total_ns"}) {
            for (const auto& rule :
                 metrics.at(metric_name).at("rules")) {
                if (rule.at(0).get<std::string>() != rule_name) continue;
                const auto& comparator = rule.at(2);
                REQUIRE(comparator.at("paired_baseline").get<double>()
                        == Catch::Approx(baseline_ratio)
                            .margin(0.000000001));
                REQUIRE(comparator.at("tolerance%").get<double>()
                        == Catch::Approx(relative_noise * 100.0));
                const double configured_max =
                    comparator.at("paired_baseline").get<double>()
                    * (1.0
                       - comparator.value("threshold%", 0.0) / 100.0
                       + comparator.value("tolerance%", 0.0) / 100.0);
                REQUIRE(configured_max
                        == Catch::Approx(
                            baseline_ratio * (1.0 + relative_noise))
                            .margin(0.000000001));
                return true;
            }
        }
        return false;
    };
    const std::map<std::string, std::pair<double, double>>
        expected_baseline_and_noise{
            {"paired_resident_gpu_frontend_over_source_direct_wall_ratio_lt_1",
             {1.0, 0.0}},
            {"paired_report76_block_simd_over_ksw2_simd_wall_with_10pct_noise",
             {curated_alignment_reference_p50::block_wall
                  / curated_alignment_reference_p50::simd_wall, 0.10}},
            {"paired_report76_ksw2_batched_over_ksw2_simd_wall_with_10pct_noise",
             {curated_alignment_reference_p50::batched_wall
                  / curated_alignment_reference_p50::simd_wall, 0.10}},
            {"paired_report76_vulkan_tiled_over_ksw2_simd_wall_with_15pct_noise",
             {curated_alignment_reference_p50::vulkan_wall
                  / curated_alignment_reference_p50::simd_wall, 0.15}},
            {"paired_report76_ksw2_simd_over_minimap2_wall_with_10pct_noise",
             {curated_alignment_reference_p50::simd_wall
                  / curated_alignment_reference_p50::minimap2_wall, 0.10}},
            {"paired_report76_ksw2_simd_over_scalar_wall_with_10pct_noise",
             {curated_alignment_reference_p50::simd_wall
                  / curated_alignment_reference_p50::scalar_wall, 0.10}},
            {"paired_report76_legacy_block_vulkan_over_block_simd_wall_with_15pct_noise",
             {17'180'439'666.0 / curated_alignment_reference_p50::block_wall, 0.15}},
            {"paired_report76_resident_gpu_frontend_block_vulkan_over_block_simd_wall_with_15pct_noise",
             {17'260'490'000.0 / curated_alignment_reference_p50::block_wall, 0.15}},
            {"paired_report76_nthash_simd_over_scalar_sketch_with_15pct_noise",
             {curated_alignment_reference_p50::simd_sketch
                  / curated_alignment_reference_p50::scalar_sketch, 0.15}},
            {"paired_report76_chaining_simd_over_scalar_with_15pct_noise",
             {curated_alignment_reference_p50::simd_chain
                  / curated_alignment_reference_p50::scalar_chain, 0.15}},
            {"paired_report76_backtrack_simd_over_scalar_with_15pct_noise",
             {curated_alignment_reference_p50::simd_backtrack
                  / curated_alignment_reference_p50::scalar_backtrack, 0.15}},
            {"paired_report76_block_over_ksw2_alignment_with_15pct_noise",
             {curated_alignment_reference_p50::block_alignment
                  / curated_alignment_reference_p50::simd_alignment, 0.15}},
        };
    for (const auto& [rule_name, baseline_and_noise] :
         expected_baseline_and_noise) {
        REQUIRE(require_max_ratio(
            rule_name,
            baseline_and_noise.first,
            baseline_and_noise.second));
    }
    REQUIRE(require_max_ratio(
        "paired_diagnostic_simd_chain_scalar_mm_no_material_wall_reversal",
        1.0, 0.10));

    for (const auto& validation_metric : {
            "mixed_gpu_pipeline_selected",
            "all_gpu_acceptance_eligible",
            "gpu_index_ownership_validation_consistent",
            "vulkan_query_sketch_dispatches",
            "vulkan_target_sketch_dispatches",
            "vulkan_target_index_build_dispatches",
            "vulkan_seed_lookup_dispatches",
            "vulkan_chain_score_dispatches",
            "gpu_index_count_stage_admissions",
            "gpu_index_main_peak_admissions",
            "gpu_index_resident_transfers",
            "gpu_index_adoption_rereserve_attempts",
            "gpu_index_adoption_successes",
            "gpu_index_owned_resident_bytes",
            "gpu_index_gate_enabled",
            "gpu_index_gate_word_count",
            "gpu_index_gate_mask",
            "gpu_index_gate_set_bits",
            "gpu_index_gate_count_dispatches",
            "gpu_index_gated_emit_dispatches",
            "gpu_index_ungated_emit_dispatches",
            "gpu_index_retained_minimizer_count",
            "gpu_index_distinct_minimizer_count",
            "post_seed_owned_gpu_cache_evictions",
            "post_seed_owned_gpu_resident_bytes_released",
            "post_seed_owned_cpu_index_releases",
            "post_seed_owned_cpu_position_bytes_released"}) {
        REQUIRE(metrics.contains(validation_metric));
    }
    const std::string description =
        paired.at("description").get<std::string>();
    REQUIRE(description.find("first-repeat") != std::string::npos);
    REQUIRE(description.find("not a globally cold") != std::string::npos);
    REQUIRE(description.find("Raw stats") != std::string::npos);
    REQUIRE(description.find(
                "resident GPU-index frontend/source-direct ratio")
            != std::string::npos);
}

TEST_CASE("Bounded ntHash unequal-work sketch comparisons are nonblocking diagnostics",
          "[component:benchmark_framework][layer:contract][feature:configuration][resource:filesystem]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        .parent_path().parent_path()
        / "bench" / "alignment_benchmark_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "bench" / "alignment_benchmark_configuration.json";
    }
    REQUIRE(std::filesystem::exists(config_path));

    std::ifstream config_stream{config_path};
    REQUIRE(config_stream.good());
    const nlohmann::json config = nlohmann::json::parse(config_stream);
    const auto& probe =
        config.at("bench_bounded_nthash_xxl_hifi_validation");
    const std::string description =
        probe.at("description").get<std::string>();
    REQUIRE(description.find("unequal work") != std::string::npos);
    REQUIRE(description.find("1.46-1.48") != std::string::npos);
    REQUIRE(description.find("visible nonblocking comparison diagnostic")
            != std::string::npos);

    const auto& rules = probe.at("matrix").at("metrics")
        .at("region_total_ns").at("rules");
    REQUIRE(rules.size() == 2);
    const std::set<std::string> expected_rule_names{
        "bounded_nthash_sketch_vs_exactsimd_lt_1_comparison_diagnostic",
        "bounded_nthash_block_sketch_vs_exactsimd_lt_1_comparison_diagnostic",
    };
    std::set<std::string> configured_rule_names;
    for (const auto& rule : rules) {
        configured_rule_names.insert(rule.at(0).get<std::string>());
        const auto& comparator = rule.at(2);
        REQUIRE(comparator.at("compare") == "<");
        REQUIRE(comparator.value("threshold%", 0.0) == 0.0);
        REQUIRE(comparator.at("tolerance%") == 0.0);
        REQUIRE(comparator.at("aggregate") == "p50");
        REQUIRE(comparator.at("pairing") == "by_iteration_ratio");
        REQUIRE(comparator.at("paired_baseline") == 1.0);
        REQUIRE(comparator.at("blocking") == false);
        REQUIRE(comparator.at("warmup_repetitions") == 1);
    }
    REQUIRE(configured_rule_names == expected_rule_names);
}

TEST_CASE("XXL_HIFI relative config fail-closes mixed GPU/CPU stats and Block Vulkan ratios",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        .parent_path().parent_path()
        / "bench" / "alignment_benchmark_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "bench" / "alignment_benchmark_configuration.json";
    }
    REQUIRE(std::filesystem::exists(config_path));

    std::ifstream config_stream{config_path};
    REQUIRE(config_stream.good());
    const nlohmann::json config = nlohmann::json::parse(config_stream);
    const auto& metrics = config.at("bench_aligners_xxl_hifi_relative")
                              .at("matrix").at("metrics");
    const auto& paired_metrics = config.at("bench_aligners_xxl_hifi_paired")
                                     .at("matrix").at("metrics");

    const std::set<std::string> source_direct_metrics{
        "source_direct_gpu_index_materialization_validation_accepted",
        "gpu_index_source_direct_operations",
        "gpu_index_source_direct_terminal_successes",
        "gpu_index_source_direct_empty_bypasses",
        "gpu_index_source_direct_zero_query_bypasses",
        "gpu_index_source_direct_planning_receipts",
        "gpu_index_source_direct_terminal_receipts",
        "gpu_index_source_direct_materializations",
        "gpu_index_source_direct_resident_publications",
        "gpu_index_source_direct_logical_control_bytes",
        "gpu_index_source_direct_logical_position_bytes",
        "gpu_index_source_direct_control_requirement_bytes",
        "gpu_index_source_direct_position_requirement_bytes",
        "gpu_index_source_direct_allocation_request_bytes",
        "gpu_index_source_direct_actual_control_bytes",
        "gpu_index_source_direct_actual_position_bytes",
        "gpu_index_source_direct_source_releases",
        "gpu_index_source_direct_source_released_bytes",
        "gpu_index_source_direct_checksum_matches",
        "gpu_index_source_direct_source_traversals",
        "gpu_index_source_direct_mapped_traversals",
        "gpu_index_source_direct_reflected_authorities",
        "gpu_index_source_direct_retained_authorities",
        "gpu_index_source_direct_retained_resident_permit_bytes",
        "gpu_index_source_direct_cache_preinsertions",
        "gpu_index_source_direct_expected_seed_consumers",
        "gpu_index_source_direct_completed_seed_consumers",
        "gpu_index_source_direct_required_lookups",
        "gpu_index_source_direct_required_hits",
        "gpu_index_source_direct_required_misses",
        "gpu_index_source_direct_fallback_attempts",
        "gpu_index_source_direct_mapped_prefix_passes",
        "gpu_index_source_direct_mapped_prefix_lookups",
        "gpu_index_source_direct_mapped_prefix_comparisons",
        "gpu_index_source_direct_seed_slab_requested_bytes",
        "gpu_index_source_direct_seed_slab_initial_capacity",
        "gpu_index_source_direct_seed_slab_actual_bytes",
        "gpu_index_source_direct_seed_slab_retained_bytes",
        "gpu_index_source_direct_seed_slab_permit_releases",
        "gpu_index_source_direct_seed_reserved_bytes",
        "gpu_index_source_direct_seed_planned_vulkan_bytes",
        "gpu_index_source_direct_seed_actual_vulkan_bytes",
        "gpu_index_source_direct_seed_roundtrips",
        "gpu_index_source_direct_zero_anchor_bypasses",
        "gpu_index_source_direct_seed_fence_completions",
        "gpu_index_source_direct_cache_cleanup_releases",
        "gpu_index_source_direct_resident_cleanup_bytes",
        "gpu_index_source_direct_pipeline_completions",
    };
    for (const auto& metric_name : source_direct_metrics) {
        INFO(metric_name);
        REQUIRE(metrics.contains(metric_name));
        REQUIRE(metrics.at(metric_name).at("type") == "evaluation");
        REQUIRE(paired_metrics.contains(metric_name));
        REQUIRE(paired_metrics.at(metric_name).at("type") == "evaluation");
    }

    const std::string direct_description = config.at("params")
        .at("bioseq_block_vulkan_dp_vulkan_minimap_int")
        .at("_").at("description").get<std::string>();
    REQUIRE(direct_description.find("source-direct") != std::string::npos);
    REQUIRE(direct_description.find("CPU target-index build")
            == std::string::npos);

    const std::set<std::string> mixed_gpu_cpu_algorithms{
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
        "bioseq_block_vulkan_dp_vulkan_minimap_int",
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    const std::set<std::string> non_vulkan_algorithms{
        "minimap2_dp",
        "bioseq_ksw2_scalar_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_two_stack_nthash",
        "bioseq_ksw2_batched_simd_dp_two_stack_nthash",
        "bioseq_block_simd_dp_two_stack_nthash",
    };
    const auto require_zero_knobs = [](const nlohmann::json& comparator) {
        REQUIRE(comparator.value("threshold%", 0.0) == 0.0);
        REQUIRE(comparator.value("tolerance%", 0.0) == 0.0);
    };

    const std::string direct_row =
        "bioseq_block_vulkan_dp_vulkan_minimap_int";
    const std::set<std::string> source_direct_neutral_rows{
        "minimap2_dp",
        "bioseq_ksw2_scalar_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_two_stack_nthash",
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
        "bioseq_ksw2_batched_simd_dp_two_stack_nthash",
        "bioseq_block_simd_dp_two_stack_nthash",
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    const auto& accepted_direct_rules = metrics.at(
        "source_direct_gpu_index_materialization_validation_accepted")
        .at("rules");
    REQUIRE(accepted_direct_rules.size() == 1u);
    REQUIRE(accepted_direct_rules.at(0).at(1).at("key") == direct_row);
    REQUIRE(accepted_direct_rules.at(0).at(2).at("compare") == ">");
    REQUIRE(accepted_direct_rules.at(0).at(2).at("aggregate") == "min");
    require_zero_knobs(accepted_direct_rules.at(0).at(2));
    REQUIRE(accepted_direct_rules.at(0).at(3).at("key") == "minimap2_dp");

    const auto& direct_operation_rules =
        metrics.at("gpu_index_source_direct_operations").at("rules");
    REQUIRE(direct_operation_rules.size()
            == source_direct_neutral_rows.size() + 1u);
    std::size_t positive_direct_rules = 0u;
    std::set<std::string> neutral_rows;
    for (const auto& rule : direct_operation_rules) {
        REQUIRE(rule.size() == 4u);
        const auto& comparator = rule.at(2);
        require_zero_knobs(comparator);
        if (comparator.at("compare") == ">") {
            ++positive_direct_rules;
            REQUIRE(rule.at(1).at("key") == direct_row);
            REQUIRE(comparator.at("aggregate") == "min");
            REQUIRE(rule.at(3).at("key") == "minimap2_dp");
        } else {
            REQUIRE(comparator.at("compare") == "<");
            REQUIRE(comparator.at("aggregate") == "max");
            REQUIRE(rule.at(3).at("key") == direct_row);
            neutral_rows.insert(rule.at(1).at("key").get<std::string>());
        }
    }
    REQUIRE(positive_direct_rules == 1u);
    REQUIRE(neutral_rows == source_direct_neutral_rows);

    const std::set<std::string> biological_parity_metrics{
        "biological_collection_count",
        "biological_match_result_count",
        "biological_cigar_result_count",
        "biological_cigar_operation_count",
        "biological_match_digest_lane0_hi32",
        "biological_match_digest_lane0_lo32",
        "biological_match_digest_lane1_hi32",
        "biological_match_digest_lane1_lo32",
        "biological_cigar_digest_lane0_hi32",
        "biological_cigar_digest_lane0_lo32",
        "biological_cigar_digest_lane1_hi32",
        "biological_cigar_digest_lane1_lo32",
        "vulkan_seed_canonical_anchor_count",
        "vulkan_seed_anchor_digest_lane0_hi32",
        "vulkan_seed_anchor_digest_lane0_lo32",
        "vulkan_seed_anchor_digest_lane1_hi32",
        "vulkan_seed_anchor_digest_lane1_lo32",
    };
    const auto require_exact_iteration_parity_rule = [
        &direct_row,
        &require_zero_knobs](
            const nlohmann::json& matrix_metrics,
            const std::string& metric_name,
            const char* test_case) {
        REQUIRE(matrix_metrics.contains(metric_name));
        const auto& rules = matrix_metrics.at(metric_name).at("rules");
        REQUIRE(rules.size() == 1u);
        const auto& rule = rules.at(0);
        REQUIRE(rule.size() == 4u);
        REQUIRE(rule.at(1).at("key") == direct_row);
        REQUIRE(rule.at(1).at("test_case") == test_case);
        REQUIRE(rule.at(2).at("compare") == "=");
        REQUIRE(rule.at(2).at("aggregate") == "min");
        REQUIRE(rule.at(2).at("pairing") == "by_iteration_exact");
        require_zero_knobs(rule.at(2));
        REQUIRE(rule.at(3).at("key")
                == "bioseq_block_vulkan_resident_gpu_index_frontend");
        REQUIRE(rule.at(3).at("test_case") == test_case);
    };
    for (const auto& metric_name : biological_parity_metrics) {
        INFO(metric_name);
        require_exact_iteration_parity_rule(
            metrics, metric_name, "XXL_HIFI");
        require_exact_iteration_parity_rule(
            paired_metrics, metric_name, "XXL_HIFI_PAIRED");
    }

    const std::set<std::string> ordered_gpu_seed_rows{
        direct_row,
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    for (const std::string metric_name : {
            "vulkan_seed_canonical_anchor_order_checks",
            "vulkan_seed_canonical_anchor_order_accepts"}) {
        INFO(metric_name);
        const auto& rules = metrics.at(metric_name).at("rules");
        REQUIRE(rules.size() == ordered_gpu_seed_rows.size());
        std::set<std::string> covered;
        for (const auto& rule : rules) {
            REQUIRE(rule.at(2).at("compare") == ">");
            REQUIRE(rule.at(2).at("aggregate") == "min");
            require_zero_knobs(rule.at(2));
            REQUIRE(rule.at(3).at("key") == "minimap2_dp");
            covered.insert(rule.at(1).at("key").get<std::string>());
        }
        REQUIRE(covered == ordered_gpu_seed_rows);
    }

    // Require mixed-GPU/CPU classification in every Vulkan repeat.
    // These pipelines must also report false for all-GPU eligibility.
    const auto require_positive_mixed_gpu_cpu_rules = [
            &metrics, &mixed_gpu_cpu_algorithms, &require_zero_knobs] {
        const auto& rules = metrics.at("mixed_gpu_pipeline_selected")
                                .at("rules");
        std::set<std::string> covered;
        for (const auto& rule : rules) {
            if (rule.at(2).at("compare").get<std::string>() != ">") {
                continue;
            }
            REQUIRE(rule.size() == 4);
            require_zero_knobs(rule.at(2));
            REQUIRE(rule.at(2).at("aggregate").get<std::string>()
                    == "min");
            REQUIRE(rule.at(3).at("key").get<std::string>() == "minimap2_dp");
            covered.insert(rule.at(1).at("key").get<std::string>());
        }
        REQUIRE(covered == mixed_gpu_cpu_algorithms);
    };
    require_positive_mixed_gpu_cpu_rules();

    // Require each non-Vulkan boolean classification to be below the validated
    // positive Vulkan value, which makes it zero.
    const auto& partial_gpu_rules =
        metrics.at("mixed_gpu_pipeline_selected").at("rules");
    REQUIRE(partial_gpu_rules.size()
            == mixed_gpu_cpu_algorithms.size()
                + non_vulkan_algorithms.size());
    std::set<std::string> negatively_classified;
    for (const auto& rule : partial_gpu_rules) {
        if (rule.at(2).at("compare").get<std::string>() != "<") continue;
        REQUIRE(rule.size() == 4);
        require_zero_knobs(rule.at(2));
        REQUIRE(rule.at(2).at("aggregate").get<std::string>() == "max");
        REQUIRE(mixed_gpu_cpu_algorithms.contains(
            rule.at(3).at("key").get<std::string>()));
        negatively_classified.insert(
            rule.at(1).at("key").get<std::string>());
    }
    REQUIRE(negatively_classified == non_vulkan_algorithms);

    const auto& all_gpu_rules =
        metrics.at("all_gpu_acceptance_eligible").at("rules");
    REQUIRE(all_gpu_rules.size() == mixed_gpu_cpu_algorithms.size());
    std::set<std::string> disqualified_from_all_gpu;
    for (const auto& rule : all_gpu_rules) {
        REQUIRE(rule.size() == 4);
        REQUIRE(rule.at(2).at("compare").get<std::string>() == "=");
        require_zero_knobs(rule.at(2));
        REQUIRE(rule.at(2).at("aggregate").get<std::string>() == "max");
        REQUIRE(rule.at(3).at("key").get<std::string>() == "minimap2_dp");
        disqualified_from_all_gpu.insert(
            rule.at(1).at("key").get<std::string>());
    }
    REQUIRE(disqualified_from_all_gpu == mixed_gpu_cpu_algorithms);

    // Require a positive dispatch count for each stage assigned to the GPU
    // in the configured pipeline.
    const std::map<std::string, std::set<std::string>> expected_positive_rows{
        {"vulkan_query_sketch_dispatches", {
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_resident_gpu_index_frontend"}},
        {"vulkan_target_sketch_dispatches", {
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_resident_gpu_index_frontend"}},
        {"vulkan_target_index_build_dispatches", {
            "bioseq_block_vulkan_resident_gpu_index_frontend"}},
        {"vulkan_seed_lookup_dispatches", {
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_resident_gpu_index_frontend"}},
        {"vulkan_chain_score_dispatches", {
            "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_resident_gpu_index_frontend"}},
    };
    for (const auto& [metric_name, expected_rows] : expected_positive_rows) {
        const auto& metric = metrics.at(metric_name);
        REQUIRE(metric.at("type").get<std::string>() == "evaluation");
        std::set<std::string> covered_rows;
        for (const auto& rule : metric.at("rules")) {
            const std::string lhs = rule.at(1).at("key").get<std::string>();
            if (!expected_rows.contains(lhs)) continue;
            REQUIRE(rule.at(2).at("compare").get<std::string>() == ">");
            require_zero_knobs(rule.at(2));
            REQUIRE(rule.at(2).at("aggregate").get<std::string>() == "min");
            REQUIRE(rule.at(3).at("key").get<std::string>() == "minimap2_dp");
            covered_rows.insert(lhs);
        }
        REQUIRE(covered_rows == expected_rows);
    }

    // Check resident index ownership and construction on every repeat.
    // Required counters use min above the zero control. Forbidden work uses
    // max equal to zero, so one bad repeat fails the check.
    const std::set<std::string>
        positive_resident_gpu_frontend_ownership_metrics{
        "gpu_index_ownership_validation_consistent",
        "gpu_index_count_stage_admissions",
        "gpu_index_main_peak_admissions",
        "gpu_index_resident_transfers",
        "gpu_index_adoption_successes",
        "gpu_index_owned_resident_bytes",
        "gpu_index_ungated_emit_dispatches",
        "gpu_index_retained_minimizer_count",
        "gpu_index_distinct_minimizer_count",
    };
    for (const auto& metric_name :
         positive_resident_gpu_frontend_ownership_metrics) {
        const auto& metric = metrics.at(metric_name);
        REQUIRE(metric.at("type") == "evaluation");
        REQUIRE(metric.at("rules").size() == 1u);
        const auto& rule = metric.at("rules").at(0);
        REQUIRE(rule.size() == 4u);
        REQUIRE(rule.at(1).at("key")
                == "bioseq_block_vulkan_resident_gpu_index_frontend");
        REQUIRE(rule.at(1).at("test_case") == "XXL_HIFI");
        const auto& comparator = rule.at(2);
        REQUIRE(comparator.at("compare") == ">");
        REQUIRE(comparator.at("aggregate") == "min");
        REQUIRE(comparator.value("blocking", true) == true);
        require_zero_knobs(comparator);
        REQUIRE(rule.at(3).at("key") == "minimap2_dp");
        REQUIRE(rule.at(3).at("test_case") == "XXL_HIFI");
    }

    const std::set<std::string>
        zero_resident_gpu_frontend_ownership_metrics{
        "gpu_index_adoption_rereserve_attempts",
        "gpu_index_gate_enabled",
        "gpu_index_gate_word_count",
        "gpu_index_gate_mask",
        "gpu_index_gate_set_bits",
        "gpu_index_gate_count_dispatches",
        "gpu_index_gated_emit_dispatches",
    };
    for (const auto& metric_name :
         zero_resident_gpu_frontend_ownership_metrics) {
        const auto& metric = metrics.at(metric_name);
        REQUIRE(metric.at("type") == "evaluation");
        REQUIRE(metric.at("rules").size() == 1u);
        const auto& rule = metric.at("rules").at(0);
        REQUIRE(rule.size() == 4u);
        REQUIRE(rule.at(1).at("key")
                == "bioseq_block_vulkan_resident_gpu_index_frontend");
        const auto& comparator = rule.at(2);
        REQUIRE(comparator.at("compare") == "=");
        REQUIRE(comparator.at("aggregate") == "max");
        REQUIRE(comparator.value("blocking", true) == true);
        require_zero_knobs(comparator);
        REQUIRE(rule.at(3).at("key") == "minimap2_dp");
    }

    // Check both Block-Vulkan wall-time limits against the Block-SIMD reference.
    static constexpr double block_simd_wall = 10'023'361'667.0;
    const std::map<std::string, double> expected_ratios{
        {"bioseq_block_vulkan_dp_vulkan_minimap_int",
         17'180'439'666.0 / block_simd_wall},
        {"bioseq_block_vulkan_resident_gpu_index_frontend",
         17'260'490'000.0 / block_simd_wall},
    };
    std::set<std::string> ratio_covered_algorithms;
    for (const auto& rule : metrics.at("wall_clock_ns").at("rules")) {
        const std::string lhs = rule.at(1).at("key").get<std::string>();
        if (!expected_ratios.contains(lhs)) continue;
        if (rule.at(3).at("key").get<std::string>()
            != "bioseq_block_simd_dp_two_stack_nthash") {
            continue;
        }
        const auto& comparator = rule.at(2);
        REQUIRE(comparator.at("compare").get<std::string>() == "<");
        REQUIRE(comparator.at("aggregate").get<std::string>() == "p50");
        const double configured_max_ratio =
            1.0
            - comparator.value("threshold%", 0.0) / 100.0
            + comparator.value("tolerance%", 0.0) / 100.0;
        const double reference_ratio = expected_ratios.at(lhs);
        REQUIRE(configured_max_ratio >= reference_ratio);
        REQUIRE(configured_max_ratio < reference_ratio + 0.000001);
        ratio_covered_algorithms.insert(lhs);
    }
    REQUIRE(ratio_covered_algorithms.size() == expected_ratios.size());
}

// Parse benchmark tags and require them in the configuration.

TEST_CASE("bench_framework tags config field parse + schema",
          "[component:benchmark_framework][layer:contract][feature:configuration][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"algo_a", std::span<const algorithm_parameter_specification>{k_filter_parameters}}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for{{ {"SMALL", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over{{ {"algo_a", "{}"} }};

    SECTION("non-empty tags array parses and runs") {
        static constexpr std::array<std::string_view, 2> k_tags{{"aligner", "grid"}};
        const std::array<benchmark_specification, 1> benches{{
            benchmark_specification{.key = "bench_tagged", .description = "tagged",
                          .tags = std::span<const std::string_view>{k_tags},
                          .for_entries = std::span<const iteration_count_entry>{k_for},
                          .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over},
                          .metrics = std::span<const metric_specification>{k_filter_metrics}}
        }};
        const auto config_text = render_config(
            std::span<const algorithm_specification>{k_algorithms},
            std::span<const test_case_data_specification>{k_filter_test_cases},
            std::span<const benchmark_specification>{benches});
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        auto outcome = run_with_args(report_dir, std::span<const std::string>{},
                                     make_toy_bench("bench_tagged", "tagged", config_path));
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(report_benchmark_keys(outcome.report).count("bench_tagged") == 1);
    }

    SECTION("empty tags array is legal") {
        static constexpr std::array<std::string_view, 0> k_no_tags{};
        const std::array<benchmark_specification, 1> benches{{
            benchmark_specification{.key = "bench_empty_tags", .description = "no tags",
                          .tags = std::span<const std::string_view>{k_no_tags},
                          .for_entries = std::span<const iteration_count_entry>{k_for},
                          .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over},
                          .metrics = std::span<const metric_specification>{k_filter_metrics}}
        }};
        const auto config_text = render_config(
            std::span<const algorithm_specification>{k_algorithms},
            std::span<const test_case_data_specification>{k_filter_test_cases},
            std::span<const benchmark_specification>{benches});
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        auto outcome = run_with_args(report_dir, std::span<const std::string>{},
                                     make_toy_bench("bench_empty_tags", "no tags", config_path));
        REQUIRE(outcome.exit_code == 0);
    }

    SECTION("missing tags array fails schema validation") {
        // Omit the required tags array from this benchmark.
        constexpr std::string_view bad_config =
            R"({"params":{"algo_a":{"unused":{"type":"integer","default":0}}},)"
            R"("test_cases":{"SMALL":{"data":{"length":8}}},)"
            R"("bench_no_tags":{"matrix":{"for":{"SMALL":2},"over":{"algo_a":{}},)"
            R"("metrics":{"sum_value":{"type":"evaluation"}}}}})";
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, bad_config);
        auto outcome = run_with_args(report_dir, std::span<const std::string>{},
                                     make_toy_bench("bench_no_tags", "missing tags", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "configuration"));
    }

    SECTION("non-string tag entry fails schema validation") {
        constexpr std::string_view bad_config =
            R"({"params":{"algo_a":{"unused":{"type":"integer","default":0}}},)"
            R"("test_cases":{"SMALL":{"data":{"length":8}}},)"
            R"("bench_bad_tag":{"tags":[123],"matrix":{"for":{"SMALL":2},"over":{"algo_a":{}},)"
            R"("metrics":{"sum_value":{"type":"evaluation"}}}}})";
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, bad_config);
        auto outcome = run_with_args(report_dir, std::span<const std::string>{},
                                     make_toy_bench("bench_bad_tag", "bad tag", config_path));
        REQUIRE(outcome.exit_code == 1);
    }
}

// Check filter syntax errors and warnings.

TEST_CASE("bench_framework --filter parse validation",
          "[component:benchmark_framework][layer:contract][feature:selection][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"algo_a", std::span<const algorithm_parameter_specification>{k_filter_parameters}}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for{{ {"SMALL", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over{{ {"algo_a", "{}"} }};
    static constexpr std::array<std::string_view, 1> k_tags{{"aligner"}};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{.key = "bench_a", .description = "a",
                      .tags = std::span<const std::string_view>{k_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for},
                      .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}}
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_filter_test_cases},
        std::span<const benchmark_specification>{benches});

    SECTION("tags=a selects the matching benchmark (exit 0)") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "tags=aligner"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(report_benchmark_keys(outcome.report).count("bench_a") == 1);
    }

    SECTION("benchmark=key selects by key (exit 0)") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "benchmark=bench_a"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 0);
    }

    SECTION("duplicate dimension is a launch error") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 4> extra{"--filter", "tags=x", "--filter", "tags=y"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_duplicate_dimension"));
    }

    SECTION("malformed value (no '=') is a launch error") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "=x"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_malformed"));
    }

    SECTION("empty value list is a launch error") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "tags="};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_malformed"));
    }

    SECTION("empty item in list is a launch error") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "tags=a,,b"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_malformed"));
    }

    SECTION("unknown filter key is a launch error") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "nope=x"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_unknown_key"));
    }
}

// Apply filters across several benchmarks.

TEST_CASE("bench_framework --filter selection + warnings",
          "[component:benchmark_framework][layer:contract][feature:selection][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    // Only grid and curated have the aligner tag.
    static constexpr std::array<algorithm_specification, 3> k_algorithms{{
        {"bioseq",   std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"minimap2", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bwa",      std::span<const algorithm_parameter_specification>{k_filter_parameters}},
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for{{ {"SMALL", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> k_grid_over{{ {"bioseq", "{}"}, {"minimap2", "{}"} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 3> k_curated_over{{ {"bioseq", "{}"}, {"minimap2", "{}"}, {"bwa", "{}"} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_storage_over{{ {"bioseq", "{}"} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_rmq_over{{ {"minimap2", "{}"} }};
    static constexpr std::array<std::string_view, 2> k_grid_tags{{"aligner", "grid"}};
    static constexpr std::array<std::string_view, 2> k_curated_tags{{"aligner", "curated"}};
    static constexpr std::array<std::string_view, 1> k_storage_tags{{"storage"}};
    static constexpr std::array<std::string_view, 1> k_rmq_tags{{"rmq"}};
    const std::array<benchmark_specification, 4> benches{{
        benchmark_specification{.key = "bench_grid",    .description = "grid",    .tags = std::span<const std::string_view>{k_grid_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for}, .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_grid_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}},
        benchmark_specification{.key = "bench_curated", .description = "curated", .tags = std::span<const std::string_view>{k_curated_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for}, .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_curated_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}},
        benchmark_specification{.key = "bench_storage", .description = "storage", .tags = std::span<const std::string_view>{k_storage_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for}, .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_storage_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}},
        benchmark_specification{.key = "bench_rmq",     .description = "rmq",     .tags = std::span<const std::string_view>{k_rmq_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for}, .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_rmq_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}},
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_filter_test_cases},
        std::span<const benchmark_specification>{benches});

    SECTION("tags=aligner + cases=bioseq,bwa selects grid{bioseq} and curated{bioseq,bwa}") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 4> extra{"--filter", "tags=aligner", "--filter", "cases=bioseq,bwa"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
            make_toy_bench("bench_grid", "grid", config_path),
            make_toy_bench("bench_curated", "curated", config_path),
            make_toy_bench("bench_storage", "storage", config_path),
            make_toy_bench("bench_rmq", "rmq", config_path));
        REQUIRE(outcome.exit_code == 0);
        const auto keys = report_benchmark_keys(outcome.report);
        REQUIRE(keys.count("bench_grid") == 1);
        REQUIRE(keys.count("bench_curated") == 1);
        REQUIRE(keys.count("bench_storage") == 0);   // Excluded by the aligner tag filter.
        REQUIRE(keys.count("bench_rmq") == 0);
        // grid contains bioseq. curated also contains bwa.
        REQUIRE(report_algorithms(outcome.report, "bench_grid") == std::set<std::string>{"bioseq"});
        REQUIRE(report_algorithms(outcome.report, "bench_curated") == std::set<std::string>{"bioseq", "bwa"});
        // bwa belongs to curated, so this selection needs no unknown-case warning.
        REQUIRE_FALSE(stderr_contains(outcome.captured_stderr, "filter_unknown_case"));
    }

    SECTION("filter_unknown_case fires for a case absent from the kept subset") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        // Selecting grid alone leaves bwa outside the selected benchmarks.
        // Expect an unknown-case warning and an error because no case matches.
        const std::array<std::string, 4> extra{"--filter", "benchmark=bench_grid", "--filter", "cases=bwa"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
            make_toy_bench("bench_grid", "grid", config_path),
            make_toy_bench("bench_curated", "curated", config_path),
            make_toy_bench("bench_storage", "storage", config_path),
            make_toy_bench("bench_rmq", "rmq", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_unknown_case"));
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_selected_nothing"));
    }

    SECTION("filter_selected_nothing when no benchmark matches") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "tags=does_not_exist"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
            make_toy_bench("bench_grid", "grid", config_path),
            make_toy_bench("bench_curated", "curated", config_path),
            make_toy_bench("bench_storage", "storage", config_path),
            make_toy_bench("bench_rmq", "rmq", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_unknown_tag"));
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_selected_nothing"));
        // An empty selection must not produce benchmark results.
        REQUIRE(report_benchmark_keys(outcome.report).empty());
    }
}

TEST_CASE("regression partitions remain valid under targeted selection",
          "[component:benchmark_framework][layer:contract][feature:selection][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"cpu", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"gpu", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"SMALL", "partitioned regression", R"({"length":8,"seed":3})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"SMALL", 2},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> other_over{{
        {"cpu", "{}"},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> curated_over{{
        {"cpu", "{}"}, {"gpu", "{}"},
    }};
    const std::array<benchmark_specification, 2> benches{{
        benchmark_specification{
            .key = "bench_other",
            .description = "global partition",
            .fast = true,
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{other_over},
            .metrics = std::span<const metric_specification>{k_filter_metrics},
        },
        benchmark_specification{
            .key = "bench_curated",
            .description = "curated partition",
            .fast = false,
            .concurrency = false,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{curated_over},
            .metrics = std::span<const metric_specification>{k_filter_metrics},
        },
    }};
    static constexpr std::string_view partitioned_rules = R"([
      {
        "name":"global_except_curated",
        "metric":"sum_value",
        "aggregate":"sum",
        "operator":{"compare":"=","tolerance%":0},
        "exclude_benchmarks":["bench_curated"]
      },
      {
        "name":"curated_cpu_blocking",
        "benchmarks":["bench_curated"],
        "metric":"sum_value",
        "aggregate":"sum",
        "operator":{"compare":"=","tolerance%":0},
        "exclude_algorithms":["gpu"]
      },
      {
        "name":"curated_gpu_diagnostic",
        "benchmarks":["bench_curated"],
        "metric":"sum_value",
        "aggregate":"sum",
        "operator":{"compare":"=","tolerance%":0},
        "exclude_algorithms":["cpu"],
        "blocking":false
      }
    ])";
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches}, partitioned_rules);

    SECTION("benchmark filter skips only rules outside the selected partition") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> filter{
            "--filter", "benchmark=bench_other"};
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{filter},
            make_toy_bench("bench_other", "other", config_path),
            make_toy_bench("bench_curated", "curated", config_path));
        REQUIRE(outcome.exit_code == 0);
        REQUIRE_FALSE(stderr_contains(
            outcome.captured_stderr, "regression_unknown_benchmark"));
        REQUIRE_FALSE(stderr_contains(
            outcome.captured_stderr, "regression_rule_matched_nothing"));
    }

    SECTION("GPU-only curated drift is diagnostic and CPU partition is skipped") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 4> filter{
            "--filter", "benchmark=bench_curated",
            "--filter", "cases=gpu"};
        REQUIRE(run_with_args(
            report_dir, std::span<const std::string>{filter},
            make_toy_bench("bench_other", "other baseline", config_path),
            make_toy_bench("bench_curated", "curated baseline", config_path))
                    .exit_code == 0);
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{filter},
            make_toy_bench("bench_other", "other drift", config_path,
                           estimator_function{}, 2.0),
            make_toy_bench("bench_curated", "curated drift", config_path,
                           estimator_function{}, 2.0));
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(outcome.report.at("regressions").size() == 1);
        REQUIRE(outcome.report.at("regressions").at(0).at("rule_name")
                == "curated_gpu_diagnostic");
        REQUIRE(outcome.report.at("regressions").at(0).at("blocking")
                == false);
        REQUIRE_FALSE(stderr_contains(
            outcome.captured_stderr, "regression_rule_matched_nothing"));
    }

    SECTION("CPU-only curated drift remains blocking and GPU partition is skipped") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 4> filter{
            "--filter", "benchmark=bench_curated",
            "--filter", "cases=cpu"};
        REQUIRE(run_with_args(
            report_dir, std::span<const std::string>{filter},
            make_toy_bench("bench_other", "other baseline", config_path),
            make_toy_bench("bench_curated", "curated baseline", config_path))
                    .exit_code == 0);
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{filter},
            make_toy_bench("bench_other", "other drift", config_path,
                           estimator_function{}, 2.0),
            make_toy_bench("bench_curated", "curated drift", config_path,
                           estimator_function{}, 2.0));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(outcome.report.at("regressions").size() == 1);
        REQUIRE(outcome.report.at("regressions").at(0).at("rule_name")
                == "curated_cpu_blocking");
        REQUIRE(outcome.report.at("regressions").at(0).at("blocking")
                == true);
        REQUIRE_FALSE(stderr_contains(
            outcome.captured_stderr, "regression_rule_matched_nothing"));
    }

    SECTION("fast mode treats slow-only rule partitions as unselected") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 1> fast{"--fast"};
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{fast},
            make_toy_bench("bench_other", "fast", config_path),
            make_toy_bench("bench_curated", "slow", config_path));
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(report_benchmark_keys(outcome.report)
                == std::set<std::string>{"bench_other"});
        REQUIRE_FALSE(stderr_contains(
            outcome.captured_stderr, "regression_rule_matched_nothing"));
    }

    SECTION("filtered-out declaration typos still fail closed") {
        static constexpr std::string_view typo_rules = R"([
          {
            "name":"unknown_include",
            "benchmarks":["bench_curtaed"],
            "metric":"sum_value",
            "aggregate":"sum",
            "operator":{"compare":"=","tolerance%":0}
          },
          {
            "name":"unknown_exclude",
            "metric":"sum_value",
            "aggregate":"sum",
            "operator":{"compare":"=","tolerance%":0},
            "exclude_benchmarks":["bench_missing"]
          },
          {
            "name":"unknown_algorithm",
            "benchmarks":["bench_curated"],
            "metric":"sum_value",
            "aggregate":"sum",
            "operator":{"compare":"=","tolerance%":0},
            "exclude_algorithms":["tpu"]
          }
        ])";
        const auto typo_config = render_config(
            std::span<const algorithm_specification>{algorithms},
            std::span<const test_case_data_specification>{test_cases},
            std::span<const benchmark_specification>{benches}, typo_rules);
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, typo_config);
        const std::array<std::string, 2> filter{
            "--filter", "benchmark=bench_other"};
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{filter},
            make_toy_bench("bench_other", "other", config_path),
            make_toy_bench("bench_curated", "curated", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr, "benchmark 'bench_curtaed' is not declared"));
        REQUIRE(stderr_contains(
            outcome.captured_stderr, "exclude_benchmarks entry 'bench_missing'"));
        REQUIRE(stderr_contains(
            outcome.captured_stderr, "exclude_algorithms entry 'tpu'"));
    }

    SECTION("filtered-out unknown exclude_cases still fails closed") {
        static constexpr std::string_view unknown_case_rule = R"([{
          "name":"unknown_test_case",
          "benchmarks":["bench_curated"],
          "metric":"sum_value",
          "aggregate":"sum",
          "operator":{"compare":"=","tolerance%":0},
          "exclude_cases":["NOT_DECLARED"]
        }])";
        const auto unknown_case_config = render_config(
            std::span<const algorithm_specification>{algorithms},
            std::span<const test_case_data_specification>{test_cases},
            std::span<const benchmark_specification>{benches}, unknown_case_rule);
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(
            report_dir, unknown_case_config);
        const std::array<std::string, 2> filter{
            "--filter", "benchmark=bench_other"};
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{filter},
            make_toy_bench("bench_other", "other", config_path),
            make_toy_bench("bench_curated", "curated", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr, "regression_unknown_test_case"));
        REQUIRE(stderr_contains(
            outcome.captured_stderr,
            "exclude_cases entry 'NOT_DECLARED' is not declared"));
    }

    SECTION("unfiltered excludes-all remains a semantic error") {
        static constexpr std::string_view excludes_all_rule = R"([{
          "name":"impossible_partition",
          "benchmarks":["bench_curated"],
          "exclude_benchmarks":["bench_curated"],
          "metric":"sum_value",
          "aggregate":"sum",
          "operator":{"compare":"=","tolerance%":0}
        }])";
        const auto excludes_all_config = render_config(
            std::span<const algorithm_specification>{algorithms},
            std::span<const test_case_data_specification>{test_cases},
            std::span<const benchmark_specification>{benches}, excludes_all_rule);
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(
            report_dir, excludes_all_config);
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{},
            make_toy_bench("bench_other", "other", config_path),
            make_toy_bench("bench_curated", "curated", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr,
            "regression_excludes_all_benchmarks"));
    }

    SECTION("selected applicable rule with no runtime region still fails closed") {
        static constexpr std::string_view missing_region_rule = R"([{
          "name":"missing_region",
          "benchmarks":["bench_other"],
          "regions":["never_visited"],
          "metric":"sum_value",
          "aggregate":"sum",
          "operator":{"compare":"=","tolerance%":0}
        }])";
        const auto missing_region_config = render_config(
            std::span<const algorithm_specification>{algorithms},
            std::span<const test_case_data_specification>{test_cases},
            std::span<const benchmark_specification>{benches}, missing_region_rule);
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(
            report_dir, missing_region_config);
        const std::array<std::string, 2> filter{
            "--filter", "benchmark=bench_other"};
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{filter},
            make_toy_bench("bench_other", "other", config_path),
            make_toy_bench("bench_curated", "curated", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr, "regression_rule_matched_nothing"));
    }
}

TEST_CASE("MCP benchmark/case filters select all nine XXL_HIFI rows only",
          "[component:benchmark_framework][layer:contract][feature:selection][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 9> algorithms{{
        {"minimap2_dp", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bioseq_ksw2_scalar_dp_scalar_mm",
         std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bioseq_ksw2_simd_dp_scalar_mm",
         std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bioseq_ksw2_simd_dp_two_stack_nthash",
         std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
         std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bioseq_ksw2_batched_simd_dp_two_stack_nthash",
         std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bioseq_block_simd_dp_two_stack_nthash",
         std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bioseq_block_vulkan_dp_vulkan_minimap_int",
         std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"bioseq_block_vulkan_resident_gpu_index_frontend",
         std::span<const algorithm_parameter_specification>{k_filter_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"XXL_HIFI", "toy XXL selection cell",
         R"({"length":4,"seed":76})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"XXL_HIFI", 4},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 9> over_entries{{
        {"minimap2_dp", "{}"},
        {"bioseq_ksw2_scalar_dp_scalar_mm", "{}"},
        {"bioseq_ksw2_simd_dp_scalar_mm", "{}"},
        {"bioseq_ksw2_simd_dp_two_stack_nthash", "{}"},
        {"bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash", "{}"},
        {"bioseq_ksw2_batched_simd_dp_two_stack_nthash", "{}"},
        {"bioseq_block_simd_dp_two_stack_nthash", "{}"},
        {"bioseq_block_vulkan_dp_vulkan_minimap_int", "{}"},
        {"bioseq_block_vulkan_resident_gpu_index_frontend", "{}"},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> decoy_over{{
        {"minimap2_dp", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> relative_tags{{
        "aligner", "xxl-hifi",
    }};
    static constexpr std::array<std::string_view, 1> decoy_tags{{
        "aligner",
    }};
    const std::array<benchmark_specification, 2> benches{{
        benchmark_specification{
            .key = "bench_aligners_xxl_hifi_relative",
            .description = "target benchmark",
            .tags = std::span<const std::string_view>{relative_tags},
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{k_filter_metrics},
        },
        benchmark_specification{
            .key = "bench_decoy",
            .description = "must be filtered out",
            .tags = std::span<const std::string_view>{decoy_tags},
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{decoy_over},
            .metrics = std::span<const metric_specification>{k_filter_metrics},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    const std::array<std::string, 4> extra{
        "--filter", "benchmark=bench_aligners_xxl_hifi_relative",
        "--filter",
        "cases=minimap2_dp,bioseq_ksw2_scalar_dp_scalar_mm,"
        "bioseq_ksw2_simd_dp_scalar_mm,"
        "bioseq_ksw2_simd_dp_two_stack_nthash,"
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash,"
        "bioseq_ksw2_batched_simd_dp_two_stack_nthash,"
        "bioseq_block_simd_dp_two_stack_nthash,"
        "bioseq_block_vulkan_dp_vulkan_minimap_int,"
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    auto outcome = run_with_args(
        report_dir,
        std::span<const std::string>{extra},
        make_toy_bench(
            "bench_aligners_xxl_hifi_relative", "target", config_path),
        make_toy_bench("bench_decoy", "decoy", config_path));

    REQUIRE(outcome.exit_code == 0);
    REQUIRE(report_benchmark_keys(outcome.report)
            == std::set<std::string>{"bench_aligners_xxl_hifi_relative"});
    const std::set<std::string> expected_algorithms{
        "minimap2_dp",
        "bioseq_ksw2_scalar_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_two_stack_nthash",
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
        "bioseq_ksw2_batched_simd_dp_two_stack_nthash",
        "bioseq_block_simd_dp_two_stack_nthash",
        "bioseq_block_vulkan_dp_vulkan_minimap_int",
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    REQUIRE(report_algorithms(
                outcome.report, "bench_aligners_xxl_hifi_relative")
            == expected_algorithms);
    for (const auto& algorithm : expected_algorithms) {
        REQUIRE(samples_count_at(
                    outcome.report,
                    "bench_aligners_xxl_hifi_relative",
                    algorithm,
                    "XXL_HIFI",
                    "all_regions",
                    "sum_value") == 4);
    }
}

TEST_CASE("explicit non-fast benchmark selection cannot be silently skipped by --fast",
          "[component:benchmark_framework][layer:contract][feature:selection][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 1> algorithms{{
        {"algo_a", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"SMALL", 1},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> over_entries{{
        {"algo_a", "{}"},
    }};
    const std::array<benchmark_specification, 2> benches{{
        benchmark_specification{
            .key = "bench_explicit_slow",
            .description = "explicitly selected non-fast benchmark",
            .fast = false,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{k_filter_metrics},
        },
        benchmark_specification{
            .key = "bench_fast_decoy",
            .description = "must not make the explicit selection look successful",
            .fast = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{k_filter_metrics},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{k_filter_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    const std::array<std::string, 3> extra{
        "--filter", "benchmark=bench_explicit_slow", "--fast",
    };
    const auto outcome = run_with_args(
        report_dir, std::span<const std::string>{extra},
        make_toy_bench("bench_explicit_slow", "slow", config_path),
        make_toy_bench("bench_fast_decoy", "fast decoy", config_path));

    REQUIRE(outcome.exit_code == 1);
    REQUIRE(report_benchmark_keys(outcome.report).count(
                "bench_explicit_slow") == 0);
}

TEST_CASE("case-pruned exact acceptance cannot make a comparison rule vacuous",
          "[component:benchmark_framework][layer:contract][feature:selection][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"gate_lhs", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"gate_rhs", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"SMALL", 4},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> over_entries{{
        {"gate_lhs", "{}"},
        {"gate_rhs", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> acceptance_rows{{
        "gate_lhs", "gate_rhs",
    }};
    static constexpr std::string_view comparison_rules = R"json([
      ["both_sides_must_execute",
       {"key":"gate_lhs","test_case":"SMALL"},
       {"compare":"=","tolerance%":0,"aggregate":"p50"},
       {"key":"gate_rhs","test_case":"SMALL"}]
    ])json";
    const std::array<metric_specification, 1> metrics{{
        {"sum_value", "evaluation", "", "", comparison_rules},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_exact_acceptance",
            .description = "two-sided acceptance gate",
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const iteration_count_entry>{for_entries},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{k_filter_test_cases},
        std::span<const benchmark_specification>{benches});

    SECTION("the complete exact benchmark is valid") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{
            "--filter", "benchmark=bench_exact_acceptance",
        };
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{extra},
            make_toy_bench("bench_exact_acceptance", "acceptance", config_path));
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(report_algorithms(outcome.report, "bench_exact_acceptance")
                == std::set<std::string>{"gate_lhs", "gate_rhs"});
    }

    SECTION("a cases filter cannot erase one comparison side") {
        temporary_report_directory report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 4> extra{
            "--filter", "benchmark=bench_exact_acceptance",
            "--filter", "cases=gate_lhs",
        };
        const auto outcome = run_with_args(
            report_dir, std::span<const std::string>{extra},
            make_toy_bench("bench_exact_acceptance", "acceptance", config_path));
        REQUIRE(outcome.exit_code == 1);
    }
}

TEST_CASE("named-region rules require provenance from every configured iteration",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"partial_stage", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"full_stage", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"XXL_HIFI", "four provenance samples",
         R"({"length":4,"seed":76})", 2},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{
        {"XXL_HIFI", 4},
    }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> over_entries{{
        {"partial_stage", "{}"},
        {"full_stage", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> acceptance_rows{{
        "partial_stage", "full_stage",
    }};
    static constexpr std::array<named_region_sample_multiplicity_specification, 2>
        exact_named_region_samples{{
            {"partial_stage", "XXL_HIFI", "gate.provenance.partial",
             "region_visits", 8},
            {"full_stage", "XXL_HIFI", "gate.provenance.full",
             "region_visits", 4},
        }};
    static constexpr std::string_view region_rules = R"json([
      ["every_iteration_must_contribute",
       {"key":"partial_stage","test_case":"XXL_HIFI","region":"gate.provenance.partial"},
       {"compare":"<","threshold%":80,"tolerance%":0,"aggregate":"p50"},
       {"key":"full_stage","test_case":"XXL_HIFI","region":"gate.provenance.full"}]
    ])json";
    const std::array<metric_specification, 1> metrics{{
        {"region_visits", "region", "", "", region_rules},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_region_provenance",
            .description = "named region completeness fixture",
            .concurrency = false,
            .by_regions = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const iteration_count_entry>{for_entries},
            .acceptance_require_named_region_iteration_coverage = true,
            .acceptance_exact_named_region_sample_multiplicities =
                std::span<const named_region_sample_multiplicity_specification>{
                    exact_named_region_samples},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});

    enum class partial_coverage_fixture {
        complete,
        missing_final_iteration,
        wrong_raw_sample_multiplicity,
        laundered_raw_sample_count,
        redistributed_one_one_one_five,
        redistributed_one_two_two_three,
    };
    const auto run_fixture_with_config = [](
            std::string_view active_config,
            partial_coverage_fixture fixture) {
        temporary_report_directory report_dir;
        std::atomic<std::size_t> partial_iterations{0};
        const auto on_run = [&partial_iterations, fixture](
                std::string_view algorithm, const parameter_map&, const toy_input&) {
            // Several descriptors share one report-region name. Count the repeats
            // that contributed samples separately from the number of samples.
            static const auto* const partial_a =
                bioseq::instrumentation::register_static_region("gate.provenance.partial");
            static const auto* const partial_b =
                bioseq::instrumentation::register_static_region("gate.provenance.partial");
            static const auto* const partial_c =
                bioseq::instrumentation::register_static_region("gate.provenance.partial");
            static const auto* const partial_d =
                bioseq::instrumentation::register_static_region("gate.provenance.partial");
            static const auto* const partial_e =
                bioseq::instrumentation::register_static_region("gate.provenance.partial");
            static const auto* const full =
                bioseq::instrumentation::register_static_region("gate.provenance.full");
            if (algorithm == "partial_stage") {
                const auto iteration = partial_iterations.fetch_add(1);
                std::size_t descriptor_count = 2;
                switch (fixture) {
                    case partial_coverage_fixture::complete:
                        break;
                    case partial_coverage_fixture::missing_final_iteration:
                        descriptor_count = iteration < 3 ? 2 : 0;
                        break;
                    case partial_coverage_fixture::wrong_raw_sample_multiplicity:
                        descriptor_count = iteration == 2 ? 3 : 2;
                        break;
                    case partial_coverage_fixture::laundered_raw_sample_count:
                        descriptor_count = iteration < 2
                            ? 2
                            : (iteration == 2 ? 4 : 0);
                        break;
                    case partial_coverage_fixture::redistributed_one_one_one_five:
                        descriptor_count = iteration < 3 ? 1 : 5;
                        break;
                    case partial_coverage_fixture::redistributed_one_two_two_three:
                        descriptor_count = iteration == 0
                            ? 1
                            : (iteration == 3 ? 3 : 2);
                        break;
                }
                const std::array descriptors{
                    partial_a, partial_b, partial_c, partial_d, partial_e};
                for (std::size_t descriptor = 0;
                     descriptor < descriptor_count; ++descriptor) {
                    auto scope = bioseq::instrumentation::region(
                        descriptors[descriptor]);
                }
            } else if (algorithm == "full_stage") {
                for (std::size_t visit = 0; visit < 10; ++visit) {
                    auto scope = bioseq::instrumentation::region(full);
                }
            }
            return toy_output{};
        };
        const auto on_evaluate = [](std::string_view, const parameter_map&,
                                    const toy_input&, const toy_output&) {
            return std::map<std::string, double>{};
        };
        return run_one_bench(
            "bench_region_provenance", "named region provenance fixture",
            active_config, report_dir, build_toy_input, on_run, on_evaluate);
    };
    const auto run_fixture = [&config_text, &run_fixture_with_config](
            partial_coverage_fixture fixture) {
        return run_fixture_with_config(config_text, fixture);
    };

    SECTION("duplicate descriptors present in all four iterations are complete") {
        const auto outcome = run_fixture(partial_coverage_fixture::complete);
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(outcome.report.at("rule_violations").empty());
        REQUIRE(samples_count_at(
                    outcome.report, "bench_region_provenance",
                    "partial_stage", "XXL_HIFI", "gate.provenance.partial",
                    "region_visits") == 8);
        REQUIRE(samples_count_at(
                    outcome.report, "bench_region_provenance",
                    "full_stage", "XXL_HIFI", "gate.provenance.full",
                    "region_visits") == 4);
    }

    SECTION("three contributing iterations out of four fail the rule") {
        const auto outcome = run_fixture(
            partial_coverage_fixture::missing_final_iteration);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE_FALSE(outcome.report.at("rule_violations").empty());
        const auto& violation = outcome.report.at("rule_violations").at(0);
        REQUIRE(violation.at("rule_name").get<std::string>()
                == "every_iteration_must_contribute");
        REQUIRE_FALSE(violation.at("args_present").at(0).get<bool>());
        REQUIRE(violation.at("args_present").at(1).get<bool>());
    }

    SECTION("all iterations with the wrong raw multiplicity fail acceptance") {
        const auto outcome = run_fixture(
            partial_coverage_fixture::wrong_raw_sample_multiplicity);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr,
            "acceptance_named_region_multiplicity_mismatch"));
        // All four repeats contributed, but one produced an extra sample.
        REQUIRE(samples_count_at(
                    outcome.report, "bench_region_provenance",
                    "partial_stage", "XXL_HIFI", "gate.provenance.partial",
                    "region_visits") == 9);
        REQUIRE(samples_count_at(
                    outcome.report, "bench_region_provenance",
                    "full_stage", "XXL_HIFI", "gate.provenance.full",
                    "region_visits") == 4);
    }

    SECTION("correct raw count cannot launder a missing iteration") {
        const auto outcome = run_fixture(
            partial_coverage_fixture::laundered_raw_sample_count);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE_FALSE(stderr_contains(
            outcome.captured_stderr,
            "acceptance_named_region_multiplicity_mismatch"));
        REQUIRE(stderr_contains(
            outcome.captured_stderr,
            "acceptance_named_region_iteration_multiplicity_mismatch"));
        // The total is eight samples, but the last repeat contributed none.
        REQUIRE(samples_count_at(
                    outcome.report, "bench_region_provenance",
                    "partial_stage", "XXL_HIFI", "gate.provenance.partial",
                    "region_visits") == 8);
        REQUIRE(samples_count_at(
                    outcome.report, "bench_region_provenance",
                    "full_stage", "XXL_HIFI", "gate.provenance.full",
                    "region_visits") == 4);
        REQUIRE_FALSE(outcome.report.at("rule_violations").empty());
        const auto& violation = outcome.report.at("rule_violations").at(0);
        REQUIRE(violation.at("rule_name").get<std::string>()
                == "every_iteration_must_contribute");
        REQUIRE_FALSE(violation.at("args_present").at(0).get<bool>());
        REQUIRE(violation.at("args_present").at(1).get<bool>());
    }

    const auto require_redistributed_multiplicity_failure =
        [&run_fixture](partial_coverage_fixture fixture) {
            const auto outcome = run_fixture(fixture);
            REQUIRE(outcome.exit_code == 1);
            REQUIRE_FALSE(stderr_contains(
                outcome.captured_stderr,
                "acceptance_named_region_multiplicity_mismatch"));
            REQUIRE(stderr_contains(
                outcome.captured_stderr,
                "acceptance_named_region_iteration_multiplicity_mismatch"));
            REQUIRE(samples_count_at(
                        outcome.report, "bench_region_provenance",
                        "partial_stage", "XXL_HIFI",
                        "gate.provenance.partial", "region_visits") == 8);
            REQUIRE(samples_count_at(
                        outcome.report, "bench_region_provenance",
                        "full_stage", "XXL_HIFI", "gate.provenance.full",
                        "region_visits") == 4);
            // Every repeat contributed and the region-visits comparison passes. Reject
            // the report because some repeats contributed the wrong number of samples.
            REQUIRE(outcome.report.at("rule_violations").empty());
        };

    SECTION("one plus one plus one plus five cannot launder two-per-iteration") {
        require_redistributed_multiplicity_failure(
            partial_coverage_fixture::redistributed_one_one_one_five);
    }

    SECTION("one plus two plus two plus three cannot launder two-per-iteration") {
        require_redistributed_multiplicity_failure(
            partial_coverage_fixture::redistributed_one_two_two_three);
    }

    SECTION("non-divisible exact total cannot define per-iteration topology") {
        auto invalid_config = nlohmann::json::parse(config_text);
        auto& multiplicities = invalid_config
            .at("bench_region_provenance").at("matrix").at("acceptance")
            .at("exact_named_region_sample_multiplicities");
        REQUIRE(multiplicities.at(0).at("exact_samples").get<std::size_t>()
                == 8);
        multiplicities.at(0)["exact_samples"] = 7;
        const std::string invalid_config_text = invalid_config.dump();

        const auto outcome = run_fixture_with_config(
            invalid_config_text, partial_coverage_fixture::complete);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr,
            "acceptance_named_region_multiplicity_not_uniform"));
    }
}

// Require a nonempty filter before opening a debug stream.

TEST_CASE("bench_framework --debug requires --filter",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"algo_a", std::span<const algorithm_parameter_specification>{k_filter_parameters}}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for{{ {"SMALL", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over{{ {"algo_a", "{}"} }};
    static constexpr std::array<std::string_view, 1> k_tags{{"aligner"}};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{.key = "bench_a", .description = "a",
                      .tags = std::span<const std::string_view>{k_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for},
                      .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}}
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_filter_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    const std::array<std::string, 1> extra{"--debug"};   // no --filter
    auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                 make_toy_bench("bench_a", "a", config_path));
    REQUIRE(outcome.exit_code == 1);
    REQUIRE(stderr_contains(outcome.captured_stderr, "debug_requires_filter"));
    // No debug stream produced when the launch check fails.
    REQUIRE_FALSE(find_debug_jsonl(report_dir.path).has_value());
}

// Check debug event counts and JSONL record fields.

TEST_CASE("bench_framework --debug JSONL stream",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    static constexpr std::array<algorithm_specification, 2> k_algorithms{{
        {"algo_x", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"algo_y", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for{{ {"SMALL", 3} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> k_over{{ {"algo_x", "{}"}, {"algo_y", "{}"} }};
    static constexpr std::array<std::string_view, 1> k_tags{{"unit"}};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{.key = "bench_dbg", .description = "debug",
                      .tags = std::span<const std::string_view>{k_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for},
                      .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}}
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_filter_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    const std::array<std::string, 3> extra{"--filter", "benchmark=bench_dbg", "--debug"};
    auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                 make_toy_bench("bench_dbg", "debug", config_path));
    REQUIRE(outcome.exit_code == 0);

    const auto jsonl = find_debug_jsonl(report_dir.path);
    REQUIRE(jsonl.has_value());
    const auto events = read_jsonl(*jsonl);
    REQUIRE_FALSE(events.empty());

    // Emit one benchmark start, one start and result per algorithm,
    // and one execution-finished event.
    REQUIRE(count_events(events, "BENCHMARK_STARTED") == 1);
    REQUIRE(count_events(events, "CASE_STARTED") == 2);
    REQUIRE(count_events(events, "CASE_RESULTS") == 2);
    REQUIRE(count_events(events, "EXECUTION_FINISHED") == 1);
    REQUIRE(case_results_names(events) == std::set<std::string>{"algo_x", "algo_y"});

    // The final event points to the saved report.
    REQUIRE(events.back().value("event", std::string{}) == "EXECUTION_FINISHED");
    REQUIRE(events.back().value("results_path", std::string{}) == outcome.report_path.string());

    // Check that each case result names its algorithm and contains SMALL results.
    for (const auto& e : events) {
        if (e.value("event", std::string{}) != "CASE_RESULTS") continue;
        REQUIRE(e.contains("timestamp"));
        REQUIRE(e["timestamp"].is_number_integer());
        REQUIRE(e.contains("results"));
        REQUIRE(e["results"].contains("execution_tuples"));
        const auto& tuples = e["results"]["execution_tuples"];
        REQUIRE(tuples.is_array());
        REQUIRE(tuples.size() == 1);                       // One parameter combination per algorithm.
        REQUIRE(tuples[0].contains("tuple"));
        REQUIRE(tuples[0].contains("test_cases"));
        const std::string algo = e.value("case_name", std::string{});
        REQUIRE(tuples[0]["tuple"].contains(algo));        // keyed on the case name
        REQUIRE(tuples[0]["test_cases"].contains("SMALL"));
        // The saved report must contain all three repeats.
        REQUIRE(samples_count_at(outcome.report, "bench_dbg", algo, "SMALL",
                                 "all_regions", "sum_value") == 3);
    }
}

TEST_CASE("paired named-stage evidence is identical in report and debug JSONL",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    static constexpr std::array<algorithm_specification, 2> algorithms{{
        {"evidence_lhs", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
        {"evidence_rhs", std::span<const algorithm_parameter_specification>{k_filter_parameters}},
    }};
    static constexpr std::array<test_case_data_specification, 1> test_cases{{
        {"PAIR", "paired evidence", R"({"length":1,"seed":370})", 1},
    }};
    static constexpr std::array<iteration_count_entry, 1> for_entries{{{"PAIR", 2}}};
    static constexpr std::array<algorithm_parameter_matrix_entry, 2> over_entries{{
        {"evidence_lhs", "{}"}, {"evidence_rhs", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> acceptance_rows{{
        "evidence_lhs", "evidence_rhs",
    }};
    static constexpr std::array<named_region_sample_multiplicity_specification, 2>
        exact_named_samples{{
            {"evidence_lhs", "PAIR", "paired.evidence.lhs",
             "region_total_ns", 2},
            {"evidence_rhs", "PAIR", "paired.evidence.rhs",
             "region_total_ns", 2},
        }};
    static constexpr std::array<std::string_view, 1> tags{{"unit"}};
    static constexpr std::string_view rules = R"json([
      ["persisted_named_stage_gate",
       {"key":"evidence_lhs","test_case":"PAIR","region":"paired.evidence.lhs"},
       {"compare":"<","threshold%":50,"tolerance%":0,"aggregate":"p50","pairing":"by_iteration_ratio","paired_baseline":1.0,"blocking":true,"warmup_repetitions":1},
       {"key":"evidence_rhs","test_case":"PAIR","region":"paired.evidence.rhs"}]
    ])json";
    const std::array<metric_specification, 1> metrics{{
        {"region_total_ns", "region", "", "", rules},
    }};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{
            .key = "bench_paired_evidence_debug",
            .description = "paired evidence debug fixture",
            .tags = std::span<const std::string_view>{tags},
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .by_regions = true,
            .for_entries = std::span<const iteration_count_entry>{for_entries},
            .over_entries = std::span<const algorithm_parameter_matrix_entry>{over_entries},
            .metrics = std::span<const metric_specification>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const iteration_count_entry>{for_entries},
            .acceptance_require_named_region_iteration_coverage = true,
            .acceptance_exact_named_region_sample_multiplicities =
                std::span<const named_region_sample_multiplicity_specification>{
                    exact_named_samples},
        },
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{algorithms},
        std::span<const test_case_data_specification>{test_cases},
        std::span<const benchmark_specification>{benches});

    const auto spin_for = [](const std::int64_t duration_ns) {
        const auto start = bioseq::instrumentation::clock::now_ns();
        while (bioseq::instrumentation::clock::now_ns() - start < duration_ns) {
            /* spin */
        }
    };
    using input_function = std::function<toy_input(
        std::string_view, std::string_view,
        simdjson::dom::element, const run_constraints&)>;
    using run_function = std::function<toy_output(
        std::string_view, const parameter_map&, const toy_input&)>;
    using evaluation_function = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const toy_input&, const toy_output&)>;
    using cleanup_function = std::function<void(
        std::string_view, std::string_view,
        simdjson::dom::element, const toy_input&)>;

    temporary_report_directory report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    auto benchmark = create_benchmark<toy_input, toy_output>(
        "paired evidence debug fixture", "bench_paired_evidence_debug",
        config_path,
        [](std::string_view, std::string_view,
           simdjson::dom::element) noexcept { return run_constraints{}; },
        input_function{[](std::string_view data_dir,
                   std::string_view test_case_name,
                   simdjson::dom::element data,
                   const run_constraints&) {
            return build_toy_input(data_dir, test_case_name, data);
        }},
        run_function{[spin_for](std::string_view algorithm, const parameter_map&,
                         const toy_input&) {
            static const auto* const lhs =
                bioseq::instrumentation::register_static_region(
                    "paired.evidence.lhs");
            static const auto* const rhs =
                bioseq::instrumentation::register_static_region(
                    "paired.evidence.rhs");
            const auto* descriptor = algorithm == "evidence_lhs" ? lhs : rhs;
            auto scope = bioseq::instrumentation::region(descriptor);
            spin_for(algorithm == "evidence_lhs" ? 50'000 : 500'000);
            return toy_output{};
        }},
        evaluation_function{[](std::string_view, const parameter_map&, const toy_input&,
                  const toy_output&) {
            return std::map<std::string, double>{};
        }},
        cleanup_function{});
    const std::array<std::string, 3> extra{
        "--filter", "benchmark=bench_paired_evidence_debug", "--debug"};
    const auto outcome = run_with_args(
        report_dir, std::span<const std::string>{extra},
        std::move(benchmark));
    REQUIRE(outcome.exit_code == 0);
    REQUIRE(outcome.report["paired_rule_evidence"].size() == 1);
    const auto& report_evidence =
        outcome.report["paired_rule_evidence"][0];
    REQUIRE(report_evidence["accepted"] == true);
    REQUIRE(report_evidence["comparator"]["warmup_repetitions"] == 1);
    REQUIRE(report_evidence["repeats"].size() == 2);
    REQUIRE(report_evidence["repeats"][0]["included_after_warmup"]
            == false);
    REQUIRE(report_evidence["repeats"][1]["included_after_warmup"]
            == true);
    const std::string configured_aggregate =
        report_evidence["comparator"]["aggregate"].get<std::string>();
    std::vector<double> included_reconstructed_ratios;
    for (const auto& repeat_evidence : report_evidence["repeats"]) {
        const double reconstructed_numerator =
            reconstruct_paired_evidence_aggregate(
                repeat_evidence["numerator_samples"],
                configured_aggregate);
        const double reconstructed_denominator =
            reconstruct_paired_evidence_aggregate(
                repeat_evidence["denominator_samples"],
                configured_aggregate);
        const double reconstructed_ratio = reconstructed_numerator
            / reconstructed_denominator;
        REQUIRE(repeat_evidence["numerator_value"].get<double>()
                == Catch::Approx(reconstructed_numerator));
        REQUIRE(repeat_evidence["denominator_value"].get<double>()
                == Catch::Approx(reconstructed_denominator));
        REQUIRE(repeat_evidence["ratio"].get<double>()
                == Catch::Approx(reconstructed_ratio));
        if (repeat_evidence["included_after_warmup"].get<bool>()) {
            included_reconstructed_ratios.push_back(reconstructed_ratio);
        }
    }
    const double reconstructed_observed =
        reconstruct_paired_evidence_aggregate(
            included_reconstructed_ratios, configured_aggregate);
    REQUIRE(report_evidence["observed_paired_ratio"].get<double>()
            == Catch::Approx(reconstructed_observed));

    const auto jsonl = find_debug_jsonl(report_dir.path);
    REQUIRE(jsonl.has_value());
    const auto events = read_jsonl(*jsonl);
    REQUIRE(count_events(events, "PAIRED_RULE_EVIDENCE") == 1);
    const auto evidence_event = std::find_if(
        events.begin(), events.end(), [](const nlohmann::json& event) {
            return event.value("event", std::string{})
                == "PAIRED_RULE_EVIDENCE";
        });
    REQUIRE(evidence_event != events.end());
    REQUIRE(evidence_event->contains("timestamp"));
    REQUIRE(evidence_event->at("evidence") == report_evidence);
    REQUIRE(std::next(evidence_event) != events.end());
    REQUIRE(events.back().value("event", std::string{})
            == "EXECUTION_FINISHED");
}

// Check that debug events occupy separate JSONL lines.

TEST_CASE("bench_framework --debug lines are flushed per event",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;
    // Run one algorithm, then read its completed debug stream.
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"algo_a", std::span<const algorithm_parameter_specification>{k_filter_parameters}}
    }};
    static constexpr std::array<iteration_count_entry, 1> k_for{{ {"SMALL", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over{{ {"algo_a", "{}"} }};
    static constexpr std::array<std::string_view, 1> k_tags{{"unit"}};
    const std::array<benchmark_specification, 1> benches{{
        benchmark_specification{.key = "bench_flush", .description = "flush",
                      .tags = std::span<const std::string_view>{k_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for},
                      .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}}
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_filter_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    const std::array<std::string, 3> extra{"--filter", "benchmark=bench_flush", "--debug"};
    auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                 make_toy_bench("bench_flush", "flush", config_path));
    REQUIRE(outcome.exit_code == 0);

    // Parse each nonempty line separately after the run finishes.
    const auto jsonl = find_debug_jsonl(report_dir.path);
    REQUIRE(jsonl.has_value());
    std::ifstream in(*jsonl, std::ios::binary);
    REQUIRE(in.good());
    std::string line;
    std::size_t parsed_lines = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto parsed = nlohmann::json::parse(line, nullptr, false);
        REQUIRE_FALSE(parsed.is_discarded());
        REQUIRE(parsed.contains("event"));
        ++parsed_lines;
    }
    // BENCHMARK_STARTED + CASE_STARTED + CASE_RESULTS + EXECUTION_FINISHED.
    REQUIRE(parsed_lines == 4);
}

// Save partial debug results when memory limits skip a batch.

TEST_CASE("bench_framework --debug abort backstop sweep",
          "[component:benchmark_framework][layer:contract][feature:reporting][resource:filesystem]") {
    test_log_silencer silence_mlogging;

    // BIG exceeds the memory budget. bench_partial runs SMALL and must save
    // its results when the benchmark ends, even though its case counter is
    // still waiting for BIG. bench_allskip runs no batches, so it must emit
    // neither CASE_STARTED nor CASE_RESULTS.
    static constexpr std::array<algorithm_specification, 1> k_algorithms{{
        {"algo_a", std::span<const algorithm_parameter_specification>{k_filter_parameters}}
    }};
    static constexpr std::array<iteration_count_entry, 2> k_for_partial{{ {"SMALL", 2}, {"BIG", 2} }};
    static constexpr std::array<iteration_count_entry, 1> k_for_skip{{ {"BIG", 2} }};
    static constexpr std::array<algorithm_parameter_matrix_entry, 1> k_over{{ {"algo_a", "{}"} }};
    static constexpr std::array<std::string_view, 1> k_tags{{"unit"}};
    const std::array<benchmark_specification, 2> benches{{
        benchmark_specification{.key = "bench_partial", .description = "partial",
                      .tags = std::span<const std::string_view>{k_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for_partial},
                      .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}},
        benchmark_specification{.key = "bench_allskip", .description = "allskip",
                      .tags = std::span<const std::string_view>{k_tags},
                      .for_entries = std::span<const iteration_count_entry>{k_for_skip},
                      .over_entries = std::span<const algorithm_parameter_matrix_entry>{k_over},
                      .metrics = std::span<const metric_specification>{k_filter_metrics}},
    }};
    const auto config_text = render_config(
        std::span<const algorithm_specification>{k_algorithms},
        std::span<const test_case_data_specification>{k_filter_test_cases},
        std::span<const benchmark_specification>{benches});

    temporary_report_directory report_dir;
    const auto config_path = write_config_file(report_dir, config_text);

    // Request 1 EiB for BIG so the scheduler rejects its memory estimate
    // before executing the batch.
    estimator_function big_oom = [](std::string_view, std::string_view test_case_name,
                           simdjson::dom::element) noexcept {
        run_constraints rc;
        if (test_case_name == "BIG") rc.base_memory = std::size_t{1} << 60;  // 1 EiB.
        return rc;
    };

    const std::array<std::string, 3> extra{
        "--filter", "benchmark=bench_partial,bench_allskip", "--debug"};
    auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
        make_toy_bench("bench_partial", "partial", config_path, big_oom),
        make_toy_bench("bench_allskip", "allskip", config_path, big_oom));

    const auto jsonl = find_debug_jsonl(report_dir.path);
    REQUIRE(jsonl.has_value());
    const auto events = read_jsonl(*jsonl);

    // A pending case counter must not prevent the run from finishing.
    REQUIRE(count_events(events, "EXECUTION_FINISHED") == 1);

    // Only bench_partial starts a case. Save that case once at the end,
    // and emit no case events for bench_allskip.
    REQUIRE(count_events(events, "CASE_STARTED") == 1);
    REQUIRE(count_events(events, "CASE_RESULTS") == 1);
    REQUIRE(case_results_names(events) == std::set<std::string>{"algo_a"});

    // The case result contains SMALL and omits the skipped BIG input.
    for (const auto& e : events) {
        if (e.value("event", std::string{}) != "CASE_RESULTS") continue;
        const auto& tuples = e["results"]["execution_tuples"];
        REQUIRE(tuples.size() == 1);
        REQUIRE(tuples[0]["test_cases"].contains("SMALL"));
        REQUIRE_FALSE(tuples[0]["test_cases"].contains("BIG"));
    }
}

TEST_CASE("GPU historical timing policies isolate curated concurrency diagnostics and serial gates",
          "[component:benchmark_framework][layer:contract][feature:regression_policy][resource:filesystem]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        .parent_path().parent_path()
        / "bench" / "alignment_benchmark_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "bench" / "alignment_benchmark_configuration.json";
    }
    std::ifstream config_stream{config_path};
    REQUIRE(config_stream.good());
    const nlohmann::json config = nlohmann::json::parse(config_stream);

    const auto& kernels = config.at("bench_prefilter_kernels");
    REQUIRE(kernels.at("concurrency").at("bench_lock") == true);
    REQUIRE(kernels.at("concurrency").at("concurrency") == false);
    REQUIRE(kernels.at("iteration_order").at("mode")
            == "interleaved_seeded");
    REQUIRE(kernels.at("iteration_order").at("seed") == 5121008);
    for (const auto& [_case_name, iterations] :
         kernels.at("matrix").at("for").items()) {
        REQUIRE(iterations == 11);
    }
    REQUIRE(kernels.at("description").get<std::string>().find(
                "Reports 151/153/154") != std::string::npos);

    const auto& curated = config.at("bench_aligners_curated");
    REQUIRE(curated.at("concurrency").at("bench_lock") == false);
    REQUIRE(curated.at("concurrency").at("concurrency") == true);
    const auto& curated_matrix = curated.at("matrix");
    const auto& curated_acceptance = curated_matrix.at("acceptance");
    const std::set<std::string> expected_curated_algorithms{
        "minimap2_dp",
        "bioseq_ksw2_scalar_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_two_stack_nthash",
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
        "bioseq_ksw2_batched_simd_dp_two_stack_nthash",
        "bioseq_block_simd_dp_two_stack_nthash",
        "bioseq_block_vulkan_dp_vulkan_minimap_int",
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    std::set<std::string> curated_acceptance_algorithms;
    for (const auto& algorithm :
         curated_acceptance.at("exact_algorithm_rows")) {
        curated_acceptance_algorithms.insert(algorithm.get<std::string>());
    }
    std::set<std::string> curated_configured_algorithms;
    for (const auto& [algorithm, _params] :
         curated_matrix.at("over").items()) {
        (void)_params;
        curated_configured_algorithms.insert(algorithm);
    }
    REQUIRE(curated_acceptance_algorithms == expected_curated_algorithms);
    REQUIRE(curated_acceptance_algorithms
            == curated_configured_algorithms);
    REQUIRE(curated_acceptance.at("exact_test_case_iterations")
            == curated_matrix.at("for"));
    REQUIRE(curated_acceptance.at(
                "require_named_region_iteration_coverage") == false);
    REQUIRE(curated_acceptance.at(
                "exact_named_region_sample_multiplicities").empty());
    REQUIRE(curated.at("description").get<std::string>().find(
                "Wall timing is diagnostic only")
            != std::string::npos);
    REQUIRE(curated.at("description").get<std::string>().find(
                "five concurrent groups share a process-wide Vulkan queue mutex")
            != std::string::npos);
    REQUIRE(curated.at("description").get<std::string>().find(
                "independent row P50/P99 estimates are not acceptance-grade")
            != std::string::npos);
    REQUIRE(curated.at("description").get<std::string>().find(
                "Blocking GPU speed acceptance")
            != std::string::npos);
    REQUIRE(curated.at("description").get<std::string>().find(
                "below the 256 Mi-bp source-direct threshold")
            != std::string::npos);

    const auto& curated_metrics = curated.at("matrix").at("metrics");
    auto find_curated_metric_rule = [&curated_metrics](
        std::string_view metric_name,
        std::string_view rule_name) -> const nlohmann::json& {
        const auto& metric_rules =
            curated_metrics.at(metric_name).at("rules");
        const auto found = std::find_if(
            metric_rules.begin(), metric_rules.end(),
            [rule_name](const auto& rule) {
                return rule.at(0).template get<std::string>() == rule_name;
            });
        REQUIRE(found != metric_rules.end());
        return *found;
    };

    struct curated_resident_gpu_frontend_case {
        std::string_view test_case;
        std::string_view rule_suffix;
    };
    static constexpr std::array<curated_resident_gpu_frontend_case, 2>
        resident_gpu_frontend_cases{{
        {"FAST_LONG", "fast_long"},
        {"FAST_VERY_LONG", "fast_very_long"},
    }};
    for (const auto& resident_gpu_frontend_case :
         resident_gpu_frontend_cases) {
        REQUIRE(config.at("test_cases")
                    .at(resident_gpu_frontend_case.test_case)
                    .at("data").at("reference_length")
                    .get<std::uint64_t>()
                < (std::uint64_t{256} << 20u));
        const auto& wall_rule = find_curated_metric_rule(
            "wall_clock_ns",
            "resident_gpu_frontend_"
                + std::string{resident_gpu_frontend_case.rule_suffix}
                + "_wall_no_slower_than_small_target_vulkan_minimap_int_sibling");
        REQUIRE(wall_rule.at(1).at("key")
                == "bioseq_block_vulkan_resident_gpu_index_frontend");
        REQUIRE(wall_rule.at(1).at("test_case")
                == resident_gpu_frontend_case.test_case);
        REQUIRE(wall_rule.at(2).at("compare") == "<");
        REQUIRE(wall_rule.at(2).at("threshold%") == 0);
        REQUIRE(wall_rule.at(2).at("tolerance%") == 0);
        REQUIRE(wall_rule.at(2).at("aggregate") == "p50");
        REQUIRE(wall_rule.at(2).at("blocking") == false);
        REQUIRE(wall_rule.at(3).at("key")
                == "bioseq_block_vulkan_dp_vulkan_minimap_int");
        REQUIRE(wall_rule.at(3).at("test_case")
                == resident_gpu_frontend_case.test_case);

        auto require_parity_bound = [
            &find_curated_metric_rule,
            &resident_gpu_frontend_case
        ](std::string_view metric_name,
          std::string_view rule_metric_name,
          std::string_view aggregate) {
            const auto& parity_rule = find_curated_metric_rule(
                metric_name,
                "resident_gpu_frontend_"
                    + std::string{resident_gpu_frontend_case.rule_suffix}
                    + "_" + std::string{rule_metric_name}
                    + "_equals_minimap2");
            REQUIRE(parity_rule.at(1).at("key")
                    == "bioseq_block_vulkan_resident_gpu_index_frontend");
            REQUIRE(parity_rule.at(1).at("test_case")
                    == resident_gpu_frontend_case.test_case);
            REQUIRE(parity_rule.at(2).at("compare") == "=");
            REQUIRE(parity_rule.at(2).at("tolerance%") == 0);
            REQUIRE(parity_rule.at(2).at("aggregate") == aggregate);
            REQUIRE_FALSE(parity_rule.at(2).contains("pairing"));
            REQUIRE(parity_rule.at(2).value("blocking", true) == true);
            REQUIRE(parity_rule.at(3).at("key") == "minimap2_dp");
            REQUIRE(parity_rule.at(3).at("test_case")
                    == resident_gpu_frontend_case.test_case);
        };
        require_parity_bound("match_count", "match_count_min", "min");
        require_parity_bound("match_count", "match_count_max", "max");
        require_parity_bound(
            "primary_correct_fraction", "correct_fraction_min", "min");
        require_parity_bound(
            "mean_position_offset_bp", "position_offset_max", "max");
    }
    // These recorded P50 comparisons change direction across concurrent runs.
    // Keep the corresponding timing checks nonblocking.
    REQUIRE(559'625'166.0 < 754'953'708.0);
    REQUIRE(1'122'048'084.0 < 2'969'685'750.0);
    REQUIRE_FALSE(1'933'880'916.0 < 1'735'887'750.0);

    const auto& rules = config.at("regression_rules");
    auto find_rule = [&rules](std::string_view name) -> const nlohmann::json& {
        const auto found = std::find_if(rules.begin(), rules.end(),
            [name](const auto& rule) { return rule.at("name") == name; });
        REQUIRE(found != rules.end());
        return *found;
    };
    const auto& p50 = find_rule("wall_clock_p50_no_blowup");
    REQUIRE(p50.at("aggregate") == "p50");
    REQUIRE_FALSE(p50.contains("blocking"));
    REQUIRE(p50.at("exclude_benchmarks")
            == nlohmann::json::array({"bench_aligners_curated"}));

    const auto& curated_p50 =
        find_rule("curated_wall_clock_p50_concurrency_diagnostic");
    REQUIRE(curated_p50.at("aggregate") == "p50");
    REQUIRE(curated_p50.at("blocking") == false);
    REQUIRE(curated_p50.at("benchmarks")
            == nlohmann::json::array({"bench_aligners_curated"}));

    const auto& global_p99 = find_rule("wall_clock_p99_no_runaway_outlier");
    REQUIRE(global_p99.at("aggregate") == "p99");
    REQUIRE(global_p99.at("exclude_benchmarks")
            == nlohmann::json::array({"bench_aligners_curated"}));
    REQUIRE_FALSE(global_p99.contains("blocking"));

    const std::set<std::string> high_variance_gpu{
        "bioseq_block_vulkan_dp_vulkan_minimap_int",
        "bioseq_block_vulkan_resident_gpu_index_frontend",
    };
    const auto& curated_p99_diagnostic =
        find_rule("curated_wall_clock_p99_no_runaway_outlier");
    REQUIRE(curated_p99_diagnostic.at("aggregate") == "p99");
    REQUIRE(curated_p99_diagnostic.at("blocking") == false);
    std::set<std::string> configured_gpu_exclusions;
    for (const auto& algorithm :
         curated_p99_diagnostic.at("exclude_algorithms")) {
        configured_gpu_exclusions.insert(algorithm.get<std::string>());
    }
    REQUIRE(configured_gpu_exclusions == high_variance_gpu);

    const auto& confirmation =
        find_rule("curated_high_variance_partial_gpu_p99_confirmation");
    REQUIRE(confirmation.at("aggregate") == "p99");
    REQUIRE(confirmation.at("blocking") == false);
    REQUIRE(confirmation.at("benchmarks")
            == nlohmann::json::array({"bench_aligners_curated"}));
    for (const auto& algorithm : high_variance_gpu) {
        REQUIRE(std::find(
            confirmation.at("exclude_algorithms").begin(),
            confirmation.at("exclude_algorithms").end(), algorithm)
            == confirmation.at("exclude_algorithms").end());
    }
}
