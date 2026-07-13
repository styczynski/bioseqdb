/**
 * @file bench_framework/index_test.cpp
 * @brief End-to-end Catch2 unit tests for `bioseq::bench_framework`.
 *
 * Design choices (per main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V7_IMPL_PLAN__DOC0203.md §2):
 *   - One file, no helper subdir. Heavily-templated utilities at the
 *     top so each TEST_CASE stays focused on what it actually
 *     verifies.
 *   - Bigger tests covering many features (the user explicitly
 *     prefers "one big test with all features" over a flotilla of
 *     micro-tests).
 *   - Each test runs in its own `TempReportDir` so report-index
 *     allocation and history walks are deterministic.
 *   - `StderrCapture` redirects the framework's `std::fprintf(stderr, ...)`
 *     diagnostic banners; mlogging output (`loge` / `logi` / `logd`) is
 *     NOT captured because fmtlog uses its own static FILE* sink.
 *     None of the assertions target mlogging output.
 *   - Counter-metric tests use a per-test counter name (line-derived)
 *     because `bioseq::instr::counter()` registers in a process-wide
 *     map that persists across TEST_CASEs.
 *   - All test code follows main/DOCS/GUIDELINES/CODE_STYLE__DOC0145.md §1: descriptive variable
 *     names, `[[nodiscard]]` on every value-returning helper, no raw
 *     `printf`/`cerr` outside `StderrCapture`'s redirect plumbing,
 *     and no helper that throws on cleanup.
 */
#include "common_tests.h"
#include "common.h"

#include <simdjson.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.instr;
import bench_framework;

namespace {

using bioseq::bench_framework::ConfigValue;
using bioseq::bench_framework::ParamMap;
using bioseq::bench_framework::RawEvidenceIoFaultForTesting;
using bioseq::bench_framework::RunConstraints;
using bioseq::bench_framework::create_benchmark;
using bioseq::bench_framework::run;
using bioseq::bench_framework::set_raw_evidence_io_fault_for_testing;

/// Tame mlogging output so the framework's `loge`/`logi`/`logd` lines
/// (which bypass `StderrCapture` because they go through fmtlog's
/// own static FILE* sink) don't pollute Catch2's pass/fail report.
/// Test 7 deliberately triggers a schema-failure path that emits an
/// `[ERROR]`-level log; bumping the level to OFF for tests prevents
/// confusing trailing output after Catch2's "All tests passed"
/// banner. RAII so the previous level is restored on destruction.
struct TestLogSilencer {
    fmtlog::LogLevel saved_level;
    explicit TestLogSilencer(fmtlog::LogLevel level = fmtlog::OFF) noexcept
        : saved_level(fmtlog::getLogLevel())
    {
        log_configure(true);
        fmtlog::setLogLevel(level);
    }
    ~TestLogSilencer() noexcept {
        fmtlog::setLogLevel(saved_level);
    }
    TestLogSilencer(const TestLogSilencer&) = delete;
    TestLogSilencer& operator=(const TestLogSilencer&) = delete;
    TestLogSilencer(TestLogSilencer&&) = delete;
    TestLogSilencer& operator=(TestLogSilencer&&) = delete;
};

// ---------------------------------------------------------------------------
//  Test scaffolding
// ---------------------------------------------------------------------------

/// Owned, unique-per-instance temporary directory under the OS's
/// `temp_directory_path()`. Recursively removed on destruction.
/// Move/copy deleted because the underlying directory is owned and
/// the ownership transfer would be ambiguous if we ever held two
/// handles to the same path.
struct TempReportDir {
    std::filesystem::path path;

    TempReportDir() {
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
        // Last-resort: caller will see assertion failures when path is empty.
        path = root / fmt::format("fallback_{}", ::getpid());
        std::filesystem::create_directories(path, ec);
    }

    ~TempReportDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        // Cleanup failures are non-fatal — the OS reaps tmp eventually.
    }

    TempReportDir(const TempReportDir&) = delete;
    TempReportDir& operator=(const TempReportDir&) = delete;
    TempReportDir(TempReportDir&&) = delete;
    TempReportDir& operator=(TempReportDir&&) = delete;
};

/// RAII redirect of `stderr` (file descriptor 2) into a temp file for
/// the lifetime of this object. On destruction, restores the original
/// fd and the captured bytes are accessible via `text()`.
///
/// **Caveat**: only captures `std::fprintf(stderr, ...)` and other
/// `fileno(stderr)` writes. mlogging's fmtlog backend uses its own
/// static FILE* sink configured via `log_configure`, so `loge` /
/// `logi` / `logd` calls do NOT land in the capture. Tests that need
/// to assert on validation diagnostics rely on the fact that
/// `report_diagnostics` (and the violations / regressions / sanity
/// printers) all go through raw fprintf.
struct StderrCapture {
    std::FILE*  redirect_file = nullptr;
    int         saved_fd = -1;
    std::filesystem::path tmp_path{};

    StderrCapture() {
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

    ~StderrCapture() {
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

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;
    StderrCapture(StderrCapture&&) = delete;
    StderrCapture& operator=(StderrCapture&&) = delete;

    /// Read everything written to `stderr` since construction. Calling
    /// `text()` flushes the redirected file first so partial writes
    /// land before we read.
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

// ---------------------------------------------------------------------------
//  Config-rendering DSL
// ---------------------------------------------------------------------------
//
// Every test composes a JSON config from a few small descriptors, so
// the framework's parser sees a realistic file. The descriptors are
// `string_view` based so the caller can declare them inline as
// constexpr literals and pass them via `std::span`. The renderer is a
// plain `std::string` builder — composability beats compile-time
// purity here because tests need to mutate values across runs.

/// One parameter of one algorithm. `default_json` is the raw JSON
/// literal for the default value (e.g., `"\"SCALAR\""` for a string,
/// `"10"` for an integer). When empty, no default is emitted; matrix
/// expansion must then list this parameter explicitly.
struct AlgoParamSpec {
    std::string_view name;
    std::string_view json_type;       // "string" | "integer" | "number" | "boolean"
    std::string_view default_json{};
    std::string_view enum_array_json{}; // raw JSON array, e.g. "[\"A\", \"B\"]"
};

struct AlgorithmSpec {
    std::string_view              name;
    std::span<const AlgoParamSpec> params{};
};

/// The framework's schema requires every `params.<algo>` block to
/// declare at least one parameter (`minProperties: 1`). Tests that
/// don't otherwise care about parameters use this single-element span
/// for an opaque integer that defaults to 0.
inline constexpr std::array<AlgoParamSpec, 1> kDummyParams{{
    {"unused", "integer", "0", ""}
}};

struct TestCaseDataSpec {
    std::string_view name;
    std::string_view description{};
    std::string_view data_object_json{"{}"};   // raw JSON object body
    /// Cap on the number of parallel groups the framework may
    /// split this test case across (V11+). Default 0 ⇒ omit the
    /// `concurrency` JSON field, which lets the framework apply
    /// its own default (`max(1, hardware_concurrency() - 1)`).
    /// Tests that need a deterministic single-group lifecycle
    /// (e.g. on_cleanup invoked exactly once per used test case)
    /// should set this to 1.
    int              concurrency = 0;
};

struct MetricSpecLite {
    std::string_view name;
    std::string_view type{"builtin"};        // "builtin"|"region"|"loop"|"counter"|"evaluation"
    std::string_view region_filter{};
    std::string_view counter_name{};
    std::string_view rules_json{"[]"};       // raw JSON array
};

struct OverEntry {
    std::string_view algorithm;
    std::string_view params_object_json{"{}"};   // raw JSON: { param: [vals], ... }
};

struct ForEntry {
    std::string_view test_case;
    std::int64_t     iterations = 0;
};

struct NamedRegionSampleMultiplicitySpec {
    std::string_view algorithm;
    std::string_view test_case;
    std::string_view region;
    std::string_view metric;
    std::size_t exact_samples = 0;
};

struct BenchmarkSpec {
    std::string_view              key;
    std::string_view              description{};
    /// Required `tags` array (may be empty). Used by `--filter tags=...`.
    std::span<const std::string_view> tags{};
    bool                           fast = false;
    bool                           bench_lock = false;
    bool                           concurrency = true;
    std::string_view              iteration_order_mode{};
    std::optional<std::uint64_t>  iteration_order_seed{};
    // V11 deleted `test_case_lock`; the schema rejects the field
    // outright. The struct keeps no slot for it so existing call
    // sites that pass `.test_case_lock = ...` would fail to
    // compile and force a deliberate update.
    bool                           by_regions = false;
    std::span<const ForEntry>     for_entries{};
    std::span<const OverEntry>    over_entries{};
    std::span<const MetricSpecLite> metrics{};
    /// Optional fail-closed acceptance contract. Empty rows/cases and false
    /// coverage omit the whole object so legacy fixture configs stay minimal.
    std::span<const std::string_view> acceptance_exact_algorithm_rows{};
    std::span<const ForEntry> acceptance_exact_test_case_iterations{};
    bool acceptance_require_named_region_iteration_coverage = false;
    std::span<const NamedRegionSampleMultiplicitySpec>
        acceptance_exact_named_region_sample_multiplicities{};
};

// ---------------------------------------------------------------------------
//  Generic JSON emit primitives — the render_* helpers compose these.
// ---------------------------------------------------------------------------

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

/// Emit `<sep_open><body><sep_close>` where `body` is produced by
/// invoking `each(out, element)` once per range element with a comma
/// inserted between successive invocations. `sep_open` / `sep_close`
/// are typically `'{'`/`'}'` for objects or `'['`/`']'` for arrays.
template<typename Range, typename EachFn>
inline void emit_block(std::string& out, char sep_open, char sep_close,
                        Range&& range, EachFn each_fn)
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

template<typename Range, typename EachFn>
inline void emit_object_range(std::string& out, Range&& range, EachFn each_fn) {
    emit_block(out, '{', '}', std::forward<Range>(range), std::move(each_fn));
}

// ---------------------------------------------------------------------------
//  Per-spec renderers
// ---------------------------------------------------------------------------

inline void emit_param_schema(std::string& out, const AlgoParamSpec& param) {
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

inline void emit_algo_block(std::string& out, const AlgorithmSpec& algo) {
    emit_quoted(out, algo.name);
    out += ':';
    emit_object_range(out, algo.params,
        [](std::string& s, const AlgoParamSpec& p) { emit_param_schema(s, p); });
}

inline void emit_metric(std::string& out, const MetricSpecLite& metric) {
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

inline void emit_test_case(std::string& out, const TestCaseDataSpec& spec) {
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

inline void emit_for_entry(std::string& out, const ForEntry& entry) {
    emit_quoted(out, entry.test_case);
    out += ':';
    out += fmt::format("{}", entry.iterations);
}

inline void emit_named_region_sample_multiplicity(
    std::string& out,
    const NamedRegionSampleMultiplicitySpec& spec)
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

inline void emit_over_entry(std::string& out, const OverEntry& entry) {
    emit_quoted(out, entry.algorithm);
    out += ':';
    out.append(entry.params_object_json);
}

inline void emit_benchmark(std::string& out, const BenchmarkSpec& bench) {
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

/// Assemble a JSON config from per-section descriptors. The output is
/// guaranteed to round-trip through `nlohmann::json::parse` and the
/// framework's hardcoded schema. The body is composed entirely from
/// the templated `emit_block` / `emit_object_range` primitives above
/// — there's exactly one place where comma-tracking lives, so the
/// per-section emitters only describe their own field structure.
[[nodiscard]] inline std::string render_config(
    std::span<const AlgorithmSpec>    algos,
    std::span<const TestCaseDataSpec> test_cases,
    std::span<const BenchmarkSpec>    benches,
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

// ---------------------------------------------------------------------------
//  Run driver
// ---------------------------------------------------------------------------

struct RunOutcome {
    int           exit_code = -1;
    std::filesystem::path config_path{};
    std::filesystem::path report_path{};
    nlohmann::json report = nlohmann::json::object();
    std::string    captured_stderr{};
};

/// Detect the report file the framework just wrote. The framework
/// allocates `report_<N>.json` with `N` = smallest unused integer,
/// so we re-run the same scan after the run finishes.
[[nodiscard]] inline std::optional<std::filesystem::path> latest_report(
    const std::filesystem::path& report_dir)
{
    std::error_code ec;
    if (!std::filesystem::exists(report_dir, ec)) return std::nullopt;
    std::optional<std::size_t> max_index;
    std::filesystem::path winner;
    constexpr std::string_view kPrefix = "report_";
    constexpr std::string_view kSuffix = ".json";
    for (const auto& entry : std::filesystem::directory_iterator(report_dir, ec)) {
        if (ec) break;
        const auto filename = entry.path().filename().string();
        if (!filename.starts_with(kPrefix) || !filename.ends_with(kSuffix)) continue;
        if (filename.size() <= kPrefix.size() + kSuffix.size()) continue;
        const auto digits = std::string_view{filename}.substr(
            kPrefix.size(),
            filename.size() - kPrefix.size() - kSuffix.size());
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

/// Build an `argv[]` from an in-memory list of strings. Returns
/// pointers into the supplied `arena` so the lifetime is bounded by
/// the caller.
struct ArgvBuilder {
    std::vector<std::string>          arena;
    std::vector<char*>                pointers;

    explicit ArgvBuilder(std::span<const std::string> args) {
        arena.assign(args.begin(), args.end());
        pointers.reserve(arena.size() + 1);
        for (auto& s : arena) pointers.push_back(s.data());
        pointers.push_back(nullptr);
    }

    [[nodiscard]] int    argc() const noexcept { return static_cast<int>(arena.size()); }
    [[nodiscard]] char** argv() noexcept       { return pointers.data(); }
};

/// Heavily-templated end-to-end driver. The `Input` / `Output` types
/// are deduced from the lambda signatures — call sites pass raw
/// lambdas (the legacy 3-arg `on_input` shape) and never have to
/// spell out `std::function<...>` casts. The wrapper bridges the
/// legacy shape to the framework's V12+ API by injecting a no-op
/// `on_estimate_resources` (returns `RunConstraints{}` ⇒ scheduler
/// treats the batch as unconstrained) and adapting the 3-arg
/// `on_input` to the 4-arg form by ignoring the
/// `RunConstraints&` parameter.
template<typename OnInput, typename OnRun, typename OnEvaluate,
         typename OnCleanup = std::nullptr_t,
         typename OnRawSerializer = std::nullptr_t>
[[nodiscard]] RunOutcome run_one_bench(
    std::string_view  bench_key,
    std::string_view  bench_title,
    std::string_view  config_text,
    const TempReportDir& report_dir,
    OnInput  on_input,
    OnRun    on_run,
    OnEvaluate on_evaluate,
    OnCleanup  on_cleanup = nullptr,
    OnRawSerializer on_raw_serializer = nullptr)
{
    using bioseq::bench_framework::RunConstraints;
    using Input = std::invoke_result_t<
        OnInput, std::string_view, std::string_view, simdjson::dom::element>;
    using Output = std::invoke_result_t<
        OnRun, std::string_view, const ParamMap&, const Input&>;
    static_assert(std::is_same_v<
        std::invoke_result_t<OnEvaluate, std::string_view, const ParamMap&, const Input&, const Output&>,
        std::map<std::string, double>>,
        "on_evaluate must return std::map<std::string, double> when invoked with (algo, params, input, output)");

    RunOutcome outcome;
    outcome.config_path = report_dir.path / "config.json";
    {
        std::ofstream cfg(outcome.config_path, std::ios::trunc | std::ios::binary);
        cfg.write(config_text.data(), static_cast<std::streamsize>(config_text.size()));
    }

    using EstimateFn = std::function<RunConstraints(std::string_view,
                                                     std::string_view,
                                                     simdjson::dom::element)>;
    using InputFn   = std::function<Input(std::string_view, std::string_view,
                                            simdjson::dom::element,
                                            const RunConstraints&)>;
    using RunFn     = std::function<Output(std::string_view, const ParamMap&, const Input&)>;
    using EvalFn    = std::function<std::map<std::string, double>(std::string_view,
                                                                  const ParamMap&,
                                                                  const Input&,
                                                                  const Output&)>;
    using CleanupFn = std::function<void(std::string_view, std::string_view,
                                          simdjson::dom::element, const Input&)>;
    using RawSerializerFn = std::function<std::string(
        std::string_view, const ParamMap&, const Input&, const Output&)>;

    EstimateFn estimate_fn = [](std::string_view, std::string_view,
                                 simdjson::dom::element) noexcept {
        return RunConstraints{};
    };
    InputFn adapted_input_fn = [legacy_input = std::move(on_input)](
        std::string_view data_dir, std::string_view test_case_name,
        simdjson::dom::element data_element,
        const RunConstraints& /*constraints*/) mutable -> Input {
        return legacy_input(data_dir, test_case_name, data_element);
    };

    // The wrapper accepts `nullptr` (default) or a callable. When the
    // sentinel is passed we hand the framework a default-empty
    // `std::function`, exercising the "no cleanup" path; otherwise we
    // bind the callable into the typed `CleanupFn`.
    CleanupFn cleanup_fn{};
    if constexpr (!std::is_same_v<OnCleanup, std::nullptr_t>) {
        cleanup_fn = CleanupFn{std::move(on_cleanup)};
    }
    RawSerializerFn raw_serializer_fn{};
    if constexpr (!std::is_same_v<OnRawSerializer, std::nullptr_t>) {
        raw_serializer_fn = RawSerializerFn{std::move(on_raw_serializer)};
    }

    auto bench = create_benchmark<Input, Output>(
        std::string{bench_title},
        std::string{bench_key},
        outcome.config_path.string(),
        std::move(estimate_fn),
        std::move(adapted_input_fn),
        RunFn{std::move(on_run)},
        EvalFn{std::move(on_evaluate)},
        std::move(cleanup_fn),
        std::move(raw_serializer_fn));

    const std::array<std::string, 5> args = {
        "bench_framework_test",
        "--report",
        report_dir.path.string(),
        "--threads",
        "2",
    };
    ArgvBuilder argv_holder{std::span<const std::string>{args}};

    {
        StderrCapture capture;
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
            // Per main/DOCS/GUIDELINES/CODE_STYLE__DOC0145.md §1 the project is exception-free; nlohmann
            // throws by default, so we explicitly disable it and treat any
            // structural failure as "empty report" — tests then see an
            // empty `outcome.report` and the assertions naturally fail
            // with a clear diagnostic.
            outcome.report = nlohmann::json::parse(
                body, /*cb=*/nullptr, /*allow_exceptions=*/false);
            if (outcome.report.is_discarded()) {
                outcome.report = nlohmann::json::object();
            }
        }
    }
    return outcome;
}

// ---------------------------------------------------------------------------
//  Report-walking helpers
// ---------------------------------------------------------------------------

/// Walk down to a single
/// `(benchmark, algorithm, test_case, region, metric)` cell and
/// return the V17 aggregated-samples object as a
/// `nlohmann::json` value. Returns `nullopt` when any path
/// component is missing — letting tests distinguish "no data"
/// from "wrong data" with a clean
/// `REQUIRE(aggregate_at(...).has_value())`-style assertion.
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

/// Convenience: `samples_count` for the cell, or 0 when the cell
/// is absent. Replaces the pre-V17 `samples_at(...).size()`
/// idiom that several tests used to validate iteration counts.
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

/// Convenience: pull a numeric `stats` field (`min`, `max`, `mean`,
/// `mean90`, `sum`, `stddev`) from the cell. Returns NaN when the
/// cell or field is missing — tests assert against finite values.
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

[[nodiscard]] inline bool stderr_contains(
    std::string_view captured, std::string_view needle)
{
    return captured.find(needle) != std::string_view::npos;
}

// ---------------------------------------------------------------------------
//  Test fixture types
// ---------------------------------------------------------------------------
//
// Bench inputs are deterministic vectors of integers; outputs carry a
// per-iteration counter delta plus a fixed-or-varying evaluation
// metric. The fixture is intentionally thin — the goal is to exercise
// the framework, not to test the algorithm.

struct ToyInput {
    std::vector<std::int64_t> values;
    std::int64_t              iteration_seed = 0;
};

struct ToyOutput {
    std::int64_t sum = 0;
    std::int64_t parity_count = 0;
};

/// Untouched P50 source metrics from report_76.json's
/// bench_aligners_curated/XXL_HIFI cell. The relative gate test scales these
/// values in common and independently to prove ratio-only behaviour.
struct Report76XxlHifiP50 {
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

[[nodiscard]] inline ToyInput build_toy_input(
    std::string_view /*data_dir*/,
    std::string_view test_case_name,
    simdjson::dom::element data_element)
{
    ToyInput input;
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
          "[bench_framework][scheduler][bench-lock]") {
    TestLogSilencer silence_mlogging;
    static constexpr std::array<AlgoParamSpec, 1> scheduler_params{{
        {"unused", "integer", "0", ""}
    }};
    static constexpr std::array<TestCaseDataSpec, 1> scheduler_test_cases{{
        {"SMALL", "8 ints", R"({"length":8,"seed":3})", 1}
    }};
    static constexpr std::array<MetricSpecLite, 1> scheduler_metrics{{
        {"sum_value", "evaluation", "", "", "[]"},
    }};
    static constexpr std::array<AlgorithmSpec, 3> algorithms{{
        {"older", std::span<const AlgoParamSpec>{scheduler_params}},
        {"locked", std::span<const AlgoParamSpec>{scheduler_params}},
        {"newer", std::span<const AlgoParamSpec>{scheduler_params}},
    }};
    static constexpr std::array<ForEntry, 1> one_run{{{"SMALL", 1}}};
    static constexpr std::array<OverEntry, 1> older_over{{{"older", "{}"}}};
    static constexpr std::array<OverEntry, 1> locked_over{{{"locked", "{}"}}};
    static constexpr std::array<OverEntry, 1> newer_over{{{"newer", "{}"}}};
    const std::array<BenchmarkSpec, 3> specs{{
        BenchmarkSpec{.key="bench_older", .description="older",
            .concurrency=false, .for_entries=std::span<const ForEntry>{one_run},
            .over_entries=std::span<const OverEntry>{older_over},
            .metrics=std::span<const MetricSpecLite>{scheduler_metrics}},
        BenchmarkSpec{.key="bench_locked", .description="locked",
            .bench_lock=true, .concurrency=false,
            .for_entries=std::span<const ForEntry>{one_run},
            .over_entries=std::span<const OverEntry>{locked_over},
            .metrics=std::span<const MetricSpecLite>{scheduler_metrics}},
        BenchmarkSpec{.key="bench_newer", .description="newer",
            .concurrency=false, .for_entries=std::span<const ForEntry>{one_run},
            .over_entries=std::span<const OverEntry>{newer_over},
            .metrics=std::span<const MetricSpecLite>{scheduler_metrics}},
    }};
    const std::string config = render_config(
        std::span<const AlgorithmSpec>{algorithms},
        std::span<const TestCaseDataSpec>{scheduler_test_cases},
        std::span<const BenchmarkSpec>{specs});
    TempReportDir report_dir;
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

    using Estimate = std::function<RunConstraints(
        std::string_view, std::string_view, simdjson::dom::element)>;
    using InputFn = std::function<ToyInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>;
    using RunFn = std::function<ToyOutput(
        std::string_view, const ParamMap&, const ToyInput&)>;
    using EvalFn = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&, const ToyInput&, const ToyOutput&)>;
    const InputFn input = [](std::string_view data, std::string_view name,
        simdjson::dom::element element, const RunConstraints&) {
        return build_toy_input(data, name, element);
    };
    const EvalFn evaluate = [](std::string_view, const ParamMap&,
        const ToyInput&, const ToyOutput& output) {
        return std::map<std::string,double>{{"sum_value",
            static_cast<double>(output.sum)}};
    };
    const auto make = [&](std::string key, Estimate estimate, RunFn run_fn) {
        return create_benchmark<ToyInput,ToyOutput>(
            key, key, config_path, std::move(estimate), input,
            std::move(run_fn), evaluate);
    };
    auto older = make("bench_older",
        [](std::string_view, std::string_view, simdjson::dom::element) {
            return RunConstraints{};
        },
        [&](std::string_view, const ParamMap&, const ToyInput&) {
            unlocked_active.fetch_add(1);
            if (locked_active.load() != 0) overlap.fetch_add(1);
            older_start.store(sequence.fetch_add(1) + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            if (locked_active.load() != 0) overlap.fetch_add(1);
            older_end.store(sequence.fetch_add(1) + 1);
            unlocked_active.fetch_sub(1);
            return ToyOutput{.sum=1};
        });
    auto locked = make("bench_locked",
        [&](std::string_view, std::string_view, simdjson::dom::element) {
            while (older_start.load() == 0) std::this_thread::yield();
            lock_estimator_returned.store(true);
            return RunConstraints{};
        },
        [&](std::string_view, const ParamMap&, const ToyInput&) {
            locked_active.fetch_add(1);
            if (unlocked_active.load() != 0) overlap.fetch_add(1);
            lock_start.store(sequence.fetch_add(1) + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (unlocked_active.load() != 0) overlap.fetch_add(1);
            lock_end.store(sequence.fetch_add(1) + 1);
            locked_active.fetch_sub(1);
            return ToyOutput{.sum=2};
        });
    auto newer = make("bench_newer",
        [&](std::string_view, std::string_view, simdjson::dom::element) {
            while (!lock_estimator_returned.load()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return RunConstraints{};
        },
        [&](std::string_view, const ParamMap&, const ToyInput&) {
            unlocked_active.fetch_add(1);
            if (locked_active.load() != 0) overlap.fetch_add(1);
            newer_start.store(sequence.fetch_add(1) + 1);
            unlocked_active.fetch_sub(1);
            return ToyOutput{.sum=3};
        });
    const std::vector<std::string> args{
        "bench_framework_test", "--report", report_dir.path.string(),
        "--threads", "6",
    };
    ArgvBuilder argv_holder{std::span<const std::string>{args}};
    int exit_code = -1;
    {
        StderrCapture capture;
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

TEST_CASE("worker exceptions fail through the explicit join and retain partial samples",
          "[bench_framework][scheduler][exceptions]") {
    TestLogSilencer silence_mlogging;

    static constexpr std::array<AlgorithmSpec, 1> algorithms{{
        {"throws_on_second_iteration",
         std::span<const AlgoParamSpec>{kDummyParams}},
    }};
    static constexpr std::array<TestCaseDataSpec, 1> test_cases{{
        {"SMALL", "first iteration completes before the worker failure",
         R"({"length":8,"seed":89})", 1},
    }};
    static constexpr std::array<ForEntry, 1> for_entries{{
        {"SMALL", 2},
    }};
    static constexpr std::array<OverEntry, 1> over_entries{{
        {"throws_on_second_iteration", "{}"},
    }};
    static constexpr std::array<MetricSpecLite, 1> metrics{{
        {"sum_value", "evaluation", "", "", "[]"},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_worker_exception",
            .description = "worker exception ownership",
            .concurrency = false,
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{over_entries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        },
    }};
    const std::string config_text = render_config(
        std::span<const AlgorithmSpec>{algorithms},
        std::span<const TestCaseDataSpec>{test_cases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;
    std::atomic<std::size_t> calls{0};
    const auto on_run = [&calls](std::string_view, const ParamMap&,
                                  const ToyInput&) -> ToyOutput {
        if (calls.fetch_add(1, std::memory_order_relaxed) == 1) {
            throw std::runtime_error{"report89-worker-failure"};
        }
        return ToyOutput{.sum = 89};
    };
    const auto on_evaluate = [](std::string_view, const ParamMap&,
                                 const ToyInput&, const ToyOutput& output) {
        return std::map<std::string, double>{{
            "sum_value", static_cast<double>(output.sum)}};
    };

    const auto outcome = run_one_bench(
        "bench_worker_exception", "worker exception ownership", config_text,
        report_dir, build_toy_input, on_run, on_evaluate);

    // Reaching these assertions proves the noexcept scheduler destructor was
    // not the first observation site (the old implementation terminated).
    REQUIRE(calls.load(std::memory_order_relaxed) == 2);
    REQUIRE(outcome.exit_code == 1);
    REQUIRE_FALSE(outcome.report_path.empty());
    REQUIRE(stderr_contains(
        outcome.captured_stderr, "batch_worker_exception"));
    REQUIRE(stderr_contains(
        outcome.captured_stderr, "report89-worker-failure"));
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

// ---------------------------------------------------------------------------
//  Test 1 — End-to-end happy path covering most metric kinds
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework end-to-end happy path", "[bench_framework]") {
    TestLogSilencer silence_mlogging;

    // Counter names live in a process-global registry shared with
    // every other test in this binary (and the example main if it
    // ever runs concurrently). Embed `__LINE__` so this test owns
    // its counter even if another test starts probing the same
    // namespace.
    const std::string counter_name = fmt::format(
        "test.bench_framework.line_{}.counter", __LINE__);
    const std::string_view kCounterName{counter_name};

    static constexpr std::array<AlgoParamSpec, 1> kSortParams{{
        {"size_hint", "integer", "0", ""}
    }};
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"toy_sort", std::span<const AlgoParamSpec>{kSortParams}}
    }};
    static constexpr std::array<TestCaseDataSpec, 2> kTestCases{{
        {"TINY",  "16 random ints", R"({"length":16,"seed":11})"},
        {"SMALL", "64 random ints", R"({"length":64,"seed":29})"}
    }};
    static constexpr std::array<ForEntry, 2> kForEntries{{
        {"TINY",  4},
        {"SMALL", 3}
    }};
    static constexpr std::array<OverEntry, 1> kOverEntries{{
        {"toy_sort", "{}"}
    }};
    // Match the metrics list to what we'll exercise in run/evaluate
    // below. Counter metric uses an explicit name distinct from the
    // metric label so we exercise the rename path. We deliberately
    // omit `region_visits`: each iteration enters `toy_sort.sum`
    // exactly once, so visits would be a constant 1.0 across every
    // cell — a correct `metric_looks_constant` trip, but it muddies
    // the assertion. `region_total_ns` varies because timing varies.
    const std::array<MetricSpecLite, 4> metrics{{
        {"wall_clock_ns",         "builtin",     "",           "",                         "[]"},
        {"region_total_ns",       "region",      "",           "",                         "[]"},
        {"my_counter",            "counter",     "",           kCounterName,               "[]"},
        {"sum_value",             "evaluation",  "",           "",                         "[]"},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_happy",
            .description = "happy path",
            .fast = false,
            .bench_lock = false,
            .concurrency = false,    // serial keeps counter delta easy to assert
            .by_regions = true,
            .for_entries = std::span<const ForEntry>{kForEntries},
            .over_entries = std::span<const OverEntry>{kOverEntries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        }
    }};

    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;

    auto on_run = [&](std::string_view /*algo*/,
                       const ParamMap& /*params*/,
                       const ToyInput& input) -> ToyOutput {
        static const auto* const sum_region =
            bioseq::instr::register_region_static("toy_sort.sum");
        auto& counter = bioseq::instr::counter(kCounterName);
        ToyOutput output;
        {
            auto _scope = bioseq::instr::region(sum_region);
            for (const auto value : input.values) {
                output.sum += value;
                if ((value & 1) == 0) ++output.parity_count;
            }
        }
        bioseq::instr::counter_add<bioseq::instr::context_args_profiling_timing_only>(
            counter, static_cast<std::int64_t>(input.values.size()));
        return output;
    };

    auto on_evaluate = [](std::string_view /*algo*/,
                           const ParamMap& /*params*/,
                           const ToyInput& /*input*/,
                           const ToyOutput& output)
        -> std::map<std::string, double>
    {
        // Two keys returned. Only `sum_value` is declared (the
        // generic-evaluator pattern this PR actively supports).
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

    // Counter: 16 values per TINY iteration -> every sample is
    // exactly 16.0, so min == max == 16.0 across 4 iterations.
    REQUIRE(samples_count_at(outcome.report, "bench_happy",
                              "toy_sort", "TINY",
                              "all_regions", "my_counter") == 4);
    REQUIRE(stat_at(outcome.report, "bench_happy",
                     "toy_sort", "TINY",
                     "all_regions", "my_counter", "min") == 16.0);
    REQUIRE(stat_at(outcome.report, "bench_happy",
                     "toy_sort", "TINY",
                     "all_regions", "my_counter", "max") == 16.0);

    // sum_value is deterministic within TINY (length 16, seed 11):
    // every iteration produces the same sum, so min == max.
    REQUIRE(samples_count_at(outcome.report, "bench_happy",
                              "toy_sort", "TINY",
                              "all_regions", "sum_value") == 4);
    REQUIRE(stat_at(outcome.report, "bench_happy",
                     "toy_sort", "TINY",
                     "all_regions", "sum_value", "min")
            == stat_at(outcome.report, "bench_happy",
                        "toy_sort", "TINY",
                        "all_regions", "sum_value", "max"));

    // by_regions: true ⇒ per-descriptor bucket exists for region metrics.
    REQUIRE(samples_count_at(outcome.report, "bench_happy",
                              "toy_sort", "TINY",
                              "toy_sort.sum", "region_total_ns") == 4);

    // The "unused_extra" key returned by on_evaluate is silently
    // dropped — the V7 relaxation. It must not appear anywhere.
    REQUIRE_FALSE(aggregate_at(outcome.report, "bench_happy",
                                "toy_sort", "TINY",
                                "all_regions", "unused_extra").has_value());
    REQUIRE_FALSE(stderr_contains(outcome.captured_stderr,
                                    "evaluation_key_not_declared"));

    // Sanity warnings: sum_value differs across test_cases (different
    // length / seed → different sums), so no `metric_looks_constant`
    // warning fires for it. wall_clock_ns / region_total_ns vary too.
    const auto stuck_count = count_sanity_warnings_of_type(
        outcome.report, "metric_looks_constant");
    INFO("sanity_warnings dump: " << outcome.report["sanity_warnings"].dump());
    REQUIRE(stuck_count == 0);
}

TEST_CASE("raw-enabled benchmark embeds lossless payload objects in report",
          "[bench_framework][raw_samples]") {
    TestLogSilencer silence_mlogging;

    static constexpr auto& kNoParams = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 3> kAlgos{{
        {"raw_a", std::span<const AlgoParamSpec>{kNoParams}},
        {"raw_b", std::span<const AlgoParamSpec>{kNoParams}},
        {"raw_c", std::span<const AlgoParamSpec>{kNoParams}},
    }};
    static constexpr std::array<TestCaseDataSpec, 1> kTestCases{{
        {"ONE", "raw evidence", R"({"length":4})", 1},
    }};
    static constexpr std::array<ForEntry, 1> kForEntries{{{"ONE", 2}}};
    static constexpr std::array<OverEntry, 3> kOverEntries{{
        {"raw_a", "{}"},
        {"raw_b", "{}"},
        {"raw_c", "{}"},
    }};
    const std::array<MetricSpecLite, 2> metrics{{
        {"wall_clock_ns", "builtin", "", "", "[]"},
        {"measured_region_ns_manual", "evaluation", "", "", "[]"},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_raw",
            .description = "raw evidence",
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .for_entries = std::span<const ForEntry>{kForEntries},
            .over_entries = std::span<const OverEntry>{kOverEntries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        },
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;
    // Crash-left sidecar owns its index even without report_3.json.  The new
    // run must allocate 4 with O_EXCL rather than overwrite evidence.
    {
        std::ofstream orphan(
            report_dir.path / "report_3.raw.jsonl",
            std::ios::trunc | std::ios::binary);
        orphan << "{\"kind\":\"header\",\"schema\":1}\n";
    }
    set_raw_evidence_io_fault_for_testing(
        RawEvidenceIoFaultForTesting::short_write);
    auto outcome = run_one_bench(
        "bench_raw", "raw evidence", config_text, report_dir,
        build_toy_input,
        [](std::string_view, const ParamMap&, const ToyInput&) {
            return ToyOutput{.sum = 7};
        },
        [](std::string_view, const ParamMap&, const ToyInput&,
           const ToyOutput& output) -> std::map<std::string, double> {
            return {{"measured_region_ns_manual",
                static_cast<double>(output.sum)}};
        },
        nullptr,
        [](std::string_view, const ParamMap&, const ToyInput&,
           const ToyOutput& output) {
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
        RawEvidenceIoFaultForTesting::none);

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
    TempReportDir failure_dir;
    set_raw_evidence_io_fault_for_testing(
        RawEvidenceIoFaultForTesting::sync_failure);
    auto failed = run_one_bench(
        "bench_raw", "raw evidence", config_text, failure_dir,
        build_toy_input,
        [](std::string_view, const ParamMap&, const ToyInput&) {
            return ToyOutput{.sum = 7};
        },
        [](std::string_view, const ParamMap&, const ToyInput&,
           const ToyOutput& output) -> std::map<std::string, double> {
            return {{"measured_region_ns_manual",
                static_cast<double>(output.sum)}};
        },
        [&cleanup_calls](std::string_view, std::string_view,
            simdjson::dom::element, const ToyInput&) {
            cleanup_calls.fetch_add(1, std::memory_order_relaxed);
        },
        [](std::string_view, const ParamMap&, const ToyInput&,
           const ToyOutput& output) {
            return nlohmann::json{
                {"index_and_map_ns", output.sum},
                {"sum", output.sum},
            }.dump();
        });
    set_raw_evidence_io_fault_for_testing(
        RawEvidenceIoFaultForTesting::none);
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
    TempReportDir partial_dir;
    set_raw_evidence_io_fault_for_testing(
        RawEvidenceIoFaultForTesting::partial_write_failure);
    auto partial = run_one_bench(
        "bench_raw", "raw evidence", config_text, partial_dir,
        build_toy_input,
        [](std::string_view, const ParamMap&, const ToyInput&) {
            return ToyOutput{.sum = 7};
        },
        [](std::string_view, const ParamMap&, const ToyInput&,
           const ToyOutput& output) -> std::map<std::string, double> {
            return {{"measured_region_ns_manual",
                static_cast<double>(output.sum)}};
        },
        [&partial_cleanup_calls](std::string_view, std::string_view,
            simdjson::dom::element, const ToyInput&) {
            partial_cleanup_calls.fetch_add(1, std::memory_order_relaxed);
        },
        [](std::string_view, const ParamMap&, const ToyInput&,
           const ToyOutput& output) {
            return nlohmann::json{
                {"index_and_map_ns", output.sum},
                {"sum", output.sum},
            }.dump();
        });
    set_raw_evidence_io_fault_for_testing(
        RawEvidenceIoFaultForTesting::none);
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

    TempReportDir tamper_dir;
    std::atomic<int> tamper_cleanup_calls{0};
    auto tampered = run_one_bench(
        "bench_raw", "raw evidence", config_text, tamper_dir,
        build_toy_input,
        [](std::string_view, const ParamMap&, const ToyInput&) {
            return ToyOutput{.sum = 7};
        },
        [](std::string_view, const ParamMap&, const ToyInput&,
           const ToyOutput& output) -> std::map<std::string, double> {
            return {{"measured_region_ns_manual",
                static_cast<double>(output.sum)}};
        },
        [&tamper_dir, &tamper_cleanup_calls](
            std::string_view, std::string_view,
            simdjson::dom::element, const ToyInput&) {
            tamper_cleanup_calls.fetch_add(1, std::memory_order_relaxed);
            std::ofstream append(
                tamper_dir.path / "report_1.raw.jsonl",
                std::ios::app | std::ios::binary);
            append << "malformed-interior-line\n";
        },
        [](std::string_view, const ParamMap&, const ToyInput&,
           const ToyOutput& output) {
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
    // The prefix was corrupt before authority was granted: no complete=true
    // footer was ever written, even transiently.
    REQUIRE(footer_completion_states(
        tamper_dir.path / "report_1.raw.jsonl")
        == std::vector<bool>{false});
}

TEST_CASE("iteration_order parser rejects ambiguous seed contracts",
          "[bench_framework][iteration_order]") {
    TestLogSilencer silence_mlogging;
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
        TempReportDir report_dir;
        return run_one_bench(
            "bench_order", "order parser", config.dump(), report_dir,
            build_toy_input,
            [](std::string_view, const ParamMap&, const ToyInput&) {
                return ToyOutput{.sum = 1};
            },
            [](std::string_view, const ParamMap&, const ToyInput&,
               const ToyOutput& output) -> std::map<std::string, double> {
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

// ---------------------------------------------------------------------------
//  Test 2 — Rule violation fires on threshold
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework rule violation fires on threshold", "[bench_framework]") {
    TestLogSilencer silence_mlogging;

    static constexpr auto& kNoParams = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 2> kAlgos{{
        {"slow_algo", std::span<const AlgoParamSpec>{kNoParams}},
        {"fast_algo", std::span<const AlgoParamSpec>{kNoParams}},
    }};
    static constexpr std::array<TestCaseDataSpec, 1> kTestCases{{
        {"ONE", "one case", R"({"length":4})"}
    }};
    static constexpr std::array<ForEntry, 1> kForEntries{{ {"ONE", 5} }};
    static constexpr std::array<OverEntry, 2> kOverEntries{{
        {"slow_algo", "{}"},
        {"fast_algo", "{}"},
    }};
    // Rule: slow_algo wall_clock_ns < fast_algo wall_clock_ns. We make
    // slow_algo deliberately slower so the rule fails.
    static constexpr std::string_view kRulesJson =
        R"([["slow_under_fast",{"key":"slow_algo"},{"compare":"<","threshold%":0,"tolerance%":0,"aggregate":"p50"},{"key":"fast_algo"}]])";
    const std::array<MetricSpecLite, 1> metrics{{
        {"wall_clock_ns", "builtin", "", "", kRulesJson},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_compare",
            .description = "rule",
            .concurrency = false,
            .for_entries = std::span<const ForEntry>{kForEntries},
            .over_entries = std::span<const OverEntry>{kOverEntries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        }
    }};

    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;

    // Slow-algo busy-wait threshold. 200µs comfortably exceeds the
    // empty-fast-algo wall_clock_ns on every CI runner we care about
    // (cycles-only kernel ≪ 1µs); large enough that scheduler jitter
    // on `fast_algo` won't accidentally exceed it. Busy-wait beats
    // `std::this_thread::sleep_for` because it doesn't yield the
    // worker thread.
    constexpr std::int64_t kSlowAlgoSpinNs = 200'000;
    auto on_run = [](std::string_view algo,
                      const ParamMap& /*params*/,
                      const ToyInput& /*input*/) -> ToyOutput {
        if (algo == "slow_algo") {
            const auto start = bioseq::instr::clock::now_ns();
            while (bioseq::instr::clock::now_ns() - start < kSlowAlgoSpinNs) {
                /* spin */
            }
        }
        return ToyOutput{};
    };

    auto on_evaluate = [](std::string_view, const ParamMap&,
                           const ToyInput&, const ToyOutput&)
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
    // Rule failed because slow >= fast at the p50 aggregate.
    REQUIRE(violation["args"][0].get<double>() >= violation["args"][1].get<double>());
}

TEST_CASE("paired rules aggregate repeat-wise ratios and fail closed on schedule",
          "[bench_framework][relative-gate][paired]") {
    TestLogSilencer silence_mlogging;

    static constexpr auto& no_params = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 2> algorithms{{
        {"lhs", std::span<const AlgoParamSpec>{no_params}},
        {"rhs", std::span<const AlgoParamSpec>{no_params}},
    }};
    static constexpr std::array<TestCaseDataSpec, 1> test_cases{{
        {"ONE", "one paired group", R"({"length":1})", 1},
    }};
    static constexpr std::array<ForEntry, 1> three_repeats{{{"ONE", 3}}};
    static constexpr std::array<OverEntry, 2> over_entries{{
        {"lhs", "{}"}, {"rhs", "{}"},
    }};

    const auto run_fixture = [&](
        const std::string_view rules_json,
        const bool bench_lock,
        const std::array<std::int64_t, 3>& lhs_values,
        const std::array<std::int64_t, 3>& rhs_values) {
        const std::array<MetricSpecLite, 1> metrics{{
            {"synthetic_ns", "evaluation", "", "", rules_json},
        }};
        const std::array<BenchmarkSpec, 1> benches{{
            BenchmarkSpec{
                .key = "bench_paired",
                .description = "paired rule fixture",
                .bench_lock = bench_lock,
                .concurrency = false,
                .iteration_order_mode = "interleaved_seeded",
                .iteration_order_seed = 370,
                .for_entries = std::span<const ForEntry>{three_repeats},
                .over_entries = std::span<const OverEntry>{over_entries},
                .metrics = std::span<const MetricSpecLite>{metrics},
            },
        }};
        const auto config_text = render_config(
            std::span<const AlgorithmSpec>{algorithms},
            std::span<const TestCaseDataSpec>{test_cases},
            std::span<const BenchmarkSpec>{benches});

        auto call_counts =
            std::make_shared<std::map<std::string, std::size_t>>();
        const auto on_run = [call_counts, lhs_values, rhs_values](
            const std::string_view algorithm,
            const ParamMap& /*params*/,
            const ToyInput& /*input*/) -> ToyOutput {
            auto& ordinal = (*call_counts)[std::string{algorithm}];
            const auto& values = algorithm == "lhs" ? lhs_values : rhs_values;
            const auto bounded = std::min(ordinal, values.size() - 1);
            ++ordinal;
            return ToyOutput{.sum = values[bounded]};
        };
        const auto on_evaluate = [](
            std::string_view, const ParamMap&, const ToyInput&,
            const ToyOutput& output) -> std::map<std::string, double> {
            return {{"synthetic_ns", static_cast<double>(output.sum)}};
        };
        auto report_dir = std::make_unique<TempReportDir>();
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

    // Independent medians are 60/60 and pass the 10% window. Pairing keeps
    // the correlated repeats: ratios {1.2, 1.166..., 0.01}, whose P50 fails.
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
    const auto& violation = paired.report["rule_violations"][0];
    REQUIRE(violation["comparison_mode"] == "paired_iteration_ratio");
    REQUIRE(violation["warmup_repetitions"] == 0);
    REQUIRE(violation["args"][0].get<double>()
            == Catch::Approx(70.0 / 60.0));
    REQUIRE(violation["args"][1] == 1.0);

    static constexpr std::string_view diagnostic_rules = R"json([
      ["paired_diagnostic",{"key":"lhs"},{"compare":"<","tolerance%":10,"aggregate":"p50","pairing":"by_iteration_ratio","blocking":false},{"key":"rhs"}]
    ])json";
    auto [diagnostic_dir, diagnostic] = run_fixture(
        diagnostic_rules, true, lhs_values, rhs_values);
    (void)diagnostic_dir;
    REQUIRE(diagnostic.exit_code == 0);
    REQUIRE(diagnostic.report["rule_violations"].size() == 1);
    REQUIRE(diagnostic.report["rule_violations"][0]["blocking"] == false);

    // Pairing cannot be enabled on a benchmark that may contend with a
    // sibling benchmark. Configuration validation rejects it before work.
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
}

TEST_CASE("paired named-region rules rebuild duplicate descriptors per repeat",
          "[bench_framework][relative-gate][paired][regions]") {
    TestLogSilencer silence_mlogging;

    static constexpr auto& no_params = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 2> algorithms{{
        {"lhs", std::span<const AlgoParamSpec>{no_params}},
        {"rhs", std::span<const AlgoParamSpec>{no_params}},
    }};
    static constexpr std::array<TestCaseDataSpec, 1> test_cases{{
        {"ONE", "one paired group", R"({"length":1})", 1},
    }};
    static constexpr std::array<ForEntry, 1> for_entries{{{"ONE", 3}}};
    static constexpr std::array<OverEntry, 2> over_entries{{
        {"lhs", "{}"}, {"rhs", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> acceptance_rows{{
        "lhs", "rhs",
    }};
    static constexpr std::array<NamedRegionSampleMultiplicitySpec, 2>
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
    const std::array<MetricSpecLite, 1> metrics{{
        {"region_total_ns", "region", "", "", rules},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_paired_regions",
            .description = "paired named region fixture",
            .bench_lock = true,
            .concurrency = false,
            .iteration_order_mode = "interleaved_seeded",
            .iteration_order_seed = 370,
            .by_regions = true,
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{over_entries},
            .metrics = std::span<const MetricSpecLite>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const ForEntry>{for_entries},
            .acceptance_require_named_region_iteration_coverage = true,
            .acceptance_exact_named_region_sample_multiplicities =
                std::span<const NamedRegionSampleMultiplicitySpec>{
                    exact_named_samples},
        },
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{algorithms},
        std::span<const TestCaseDataSpec>{test_cases},
        std::span<const BenchmarkSpec>{benches});

    const auto spin_for = [](const std::int64_t duration_ns) {
        const auto start = bioseq::instr::clock::now_ns();
        while (bioseq::instr::clock::now_ns() - start < duration_ns) {
            /* spin */
        }
    };
    const auto on_run = [spin_for](
        const std::string_view algorithm,
        const ParamMap&, const ToyInput&) -> ToyOutput {
        static const auto* const lhs =
            bioseq::instr::register_region_static("paired.region.lhs");
        static const auto* const rhs_a =
            bioseq::instr::register_region_static("paired.region.rhs");
        static const auto* const rhs_b =
            bioseq::instr::register_region_static("paired.region.rhs");
        if (algorithm == "lhs") {
            auto scope = bioseq::instr::region(lhs);
            spin_for(50'000);
        } else {
            {
                auto scope = bioseq::instr::region(rhs_a);
                spin_for(200'000);
            }
            {
                auto scope = bioseq::instr::region(rhs_b);
                spin_for(200'000);
            }
        }
        return {};
    };
    const auto on_evaluate = [](
        std::string_view, const ParamMap&, const ToyInput&,
        const ToyOutput&) -> std::map<std::string, double> {
        return {};
    };

    TempReportDir report_dir;
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
}

TEST_CASE("within-run relative gates preserve report76 ratios under common scaling",
          "[bench_framework][relative-gate]") {
    TestLogSilencer silence_mlogging;

    // Source P50 values are copied verbatim from
    // main/reporting/bioseq_bench/report_76.json at
    // bench_aligners_curated/XXL_HIFI. They deliberately remain raw stage
    // metrics; the framework rule compares their P50 aggregates without
    // replacing the report cells with a precomputed ratio.
    REQUIRE(Report76XxlHifiP50::block_wall / Report76XxlHifiP50::simd_wall
            == Catch::Approx(1.057921).margin(0.000001));
    REQUIRE(Report76XxlHifiP50::batched_wall / Report76XxlHifiP50::simd_wall
            == Catch::Approx(1.000654).margin(0.000001));
    REQUIRE(Report76XxlHifiP50::vulkan_wall / Report76XxlHifiP50::simd_wall
            == Catch::Approx(1.011221).margin(0.000001));
    REQUIRE(Report76XxlHifiP50::simd_wall / Report76XxlHifiP50::minimap2_wall
            == Catch::Approx(0.797045).margin(0.000001));
    REQUIRE(Report76XxlHifiP50::simd_wall / Report76XxlHifiP50::scalar_wall
            == Catch::Approx(0.540314).margin(0.000001));
    REQUIRE(Report76XxlHifiP50::simd_sketch / Report76XxlHifiP50::scalar_sketch
            == Catch::Approx(0.210496957).margin(0.000000001));
    REQUIRE(Report76XxlHifiP50::simd_chain / Report76XxlHifiP50::scalar_chain
            == Catch::Approx(0.579268421).margin(0.000000001));
    REQUIRE(Report76XxlHifiP50::simd_backtrack / Report76XxlHifiP50::scalar_backtrack
            == Catch::Approx(0.955048972).margin(0.000000001));
    REQUIRE(Report76XxlHifiP50::block_alignment / Report76XxlHifiP50::simd_alignment
            == Catch::Approx(0.591278239).margin(0.000000001));

    static constexpr auto& no_params = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 6> algorithms{{
        {"minimap2_dp", std::span<const AlgoParamSpec>{no_params}},
        {"bioseq_ksw2_scalar_dp_scalar_mm", std::span<const AlgoParamSpec>{no_params}},
        {"bioseq_ksw2_simd_dp_two_stack_nthash", std::span<const AlgoParamSpec>{no_params}},
        {"bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash", std::span<const AlgoParamSpec>{no_params}},
        {"bioseq_ksw2_batched_simd_dp_two_stack_nthash", std::span<const AlgoParamSpec>{no_params}},
        {"bioseq_block_simd_dp_two_stack_nthash", std::span<const AlgoParamSpec>{no_params}},
    }};
    static constexpr std::array<TestCaseDataSpec, 2> test_cases{{
        {"XXL_HIFI", "report76 source cell", R"({"length":1})", 1},
        {"OTHER", "must not be checked by the scoped rules", R"({"length":2})", 1},
    }};
    static constexpr std::array<ForEntry, 2> for_entries{{
        {"XXL_HIFI", 1}, {"OTHER", 1},
    }};
    static constexpr std::array<OverEntry, 6> over_entries{{
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
    const std::array<MetricSpecLite, 6> metrics{{
        {"report76_wall_ns", "evaluation", "", "", wall_rules},
        {"report76_nthash_ns", "evaluation", "", "", sketch_rules},
        {"report76_chaining_ns", "evaluation", "", "", chain_rules},
        {"report76_backtrack_ns", "evaluation", "", "", backtrack_rules},
        {"report76_alignment_ns", "evaluation", "", "", alignment_rules},
        {"region_visits", "region", "", "", region_rules},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_relative_report76",
            .description = "relative report76 fixture",
            .concurrency = false,
            .by_regions = true,
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{over_entries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        }
    }};
    const std::string config_text = render_config(
        std::span<const AlgorithmSpec>{algorithms},
        std::span<const TestCaseDataSpec>{test_cases},
        std::span<const BenchmarkSpec>{benches});

    const auto run_fixture = [&config_text](const double common_scale,
                                            const std::size_t visit_scale,
                                            const bool regress_nthash,
                                            const bool omit_simd_region,
                                            const bool unavailable_vulkan) {
        TempReportDir report_dir;
        const auto on_run = [visit_scale, omit_simd_region](
            const std::string_view algorithm,
            const ParamMap& /*params*/,
            const ToyInput& input) -> ToyOutput {
            static const auto* const simd_region =
                bioseq::instr::register_region_static("relative.fixture.simd");
            static const auto* const scalar_region =
                bioseq::instr::register_region_static("relative.fixture.scalar");
            const bool other_case = input.values.size() == 2;
            if (algorithm == "bioseq_ksw2_simd_dp_two_stack_nthash") {
                const std::size_t visits = (other_case ? 3u : 2u) * visit_scale;
                if (!omit_simd_region) {
                    for (std::size_t visit = 0; visit < visits; ++visit) {
                        auto scope = bioseq::instr::region(simd_region);
                    }
                }
            } else if (algorithm == "bioseq_ksw2_scalar_dp_scalar_mm") {
                const std::size_t visits = 10u * visit_scale;
                for (std::size_t visit = 0; visit < visits; ++visit) {
                    auto scope = bioseq::instr::region(scalar_region);
                }
            }
            return ToyOutput{};
        };
        const auto on_evaluate = [common_scale, regress_nthash,
                                  unavailable_vulkan](
            const std::string_view algorithm,
            const ParamMap& /*params*/,
            const ToyInput& input,
            const ToyOutput& /*output*/) -> std::map<std::string, double> {
            const bool other_case = input.values.size() == 2;
            const double scoped_bad_factor = other_case ? 2.0 : 1.0;

            if (unavailable_vulkan
                && algorithm
                    == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
                const double not_measured =
                    std::numeric_limits<double>::quiet_NaN();
                return {
                    {"report76_wall_ns", not_measured},
                    {"report76_nthash_ns", not_measured},
                    {"report76_chaining_ns", not_measured},
                    {"report76_backtrack_ns", not_measured},
                    {"report76_alignment_ns", not_measured},
                };
            }

            double wall = 1.0;
            double nthash = 1.0;
            double chaining = 1.0;
            double backtrack = 1.0;
            double alignment = 1.0;
            if (algorithm == "minimap2_dp") {
                wall = Report76XxlHifiP50::minimap2_wall;
            } else if (algorithm == "bioseq_ksw2_scalar_dp_scalar_mm") {
                wall = Report76XxlHifiP50::scalar_wall;
                nthash = Report76XxlHifiP50::scalar_sketch;
                chaining = Report76XxlHifiP50::scalar_chain;
                backtrack = Report76XxlHifiP50::scalar_backtrack;
            } else if (algorithm == "bioseq_ksw2_simd_dp_two_stack_nthash") {
                wall = Report76XxlHifiP50::simd_wall * scoped_bad_factor;
                nthash = Report76XxlHifiP50::simd_sketch * scoped_bad_factor
                        * (regress_nthash && !other_case ? 1.01 : 1.0);
                chaining = Report76XxlHifiP50::simd_chain * scoped_bad_factor;
                backtrack = Report76XxlHifiP50::simd_backtrack * scoped_bad_factor;
                alignment = Report76XxlHifiP50::simd_alignment;
            } else if (algorithm == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
                wall = Report76XxlHifiP50::vulkan_wall * scoped_bad_factor;
            } else if (algorithm == "bioseq_ksw2_batched_simd_dp_two_stack_nthash") {
                wall = Report76XxlHifiP50::batched_wall * scoped_bad_factor;
            } else if (algorithm == "bioseq_block_simd_dp_two_stack_nthash") {
                wall = Report76XxlHifiP50::block_wall * scoped_bad_factor;
                alignment = Report76XxlHifiP50::block_alignment * scoped_bad_factor;
            }
            return {
                {"report76_wall_ns", wall * common_scale},
                {"report76_nthash_ns", nthash * common_scale},
                {"report76_chaining_ns", chaining * common_scale},
                {"report76_backtrack_ns", backtrack * common_scale},
                {"report76_alignment_ns", alignment * common_scale},
            };
        };
        return run_one_bench(
            "bench_relative_report76", "relative report76 fixture",
            config_text, report_dir, build_toy_input, on_run, on_evaluate);
    };

    const auto original_scale = run_fixture(1.0, 1u, false, false, false);
    REQUIRE(original_scale.exit_code == 0);
    REQUIRE(original_scale.report["rule_violations"].empty());
    REQUIRE(stat_at(original_scale.report, "bench_relative_report76",
                    "bioseq_ksw2_simd_dp_two_stack_nthash", "XXL_HIFI",
                    "all_regions", "report76_nthash_ns", "min")
            == Report76XxlHifiP50::simd_sketch);
    REQUIRE(stat_at(original_scale.report, "bench_relative_report76",
                    "bioseq_ksw2_scalar_dp_scalar_mm", "XXL_HIFI",
                    "all_regions", "report76_nthash_ns", "min")
            == Report76XxlHifiP50::scalar_sketch);

    // Multiplying every source metric and visit count by one common factor
    // cannot change a within-run ratio. An absolute historical-time gate
    // would fail this fixture; the relative gate must remain green.
    const auto common_scaled =
        run_fixture(1'000.0, 10u, false, false, false);
    REQUIRE(common_scaled.exit_code == 0);
    REQUIRE(common_scaled.report["rule_violations"].empty());
    REQUIRE(stat_at(common_scaled.report, "bench_relative_report76",
                    "bioseq_ksw2_simd_dp_two_stack_nthash", "XXL_HIFI",
                    "relative.fixture.simd", "region_visits", "min") == 20.0);
    REQUIRE(stat_at(common_scaled.report, "bench_relative_report76",
                    "bioseq_ksw2_scalar_dp_scalar_mm", "XXL_HIFI",
                    "relative.fixture.scalar", "region_visits", "min") == 100.0);

    // Only the rewritten nthash stage loses its report76 advantage. The
    // violation retains the two scaled raw P50 inputs in `args`.
    const auto regressed = run_fixture(1'000.0, 10u, true, false, false);
    REQUIRE(regressed.exit_code == 1);
    REQUIRE(regressed.report["rule_violations"].size() == 1);
    const auto& violation = regressed.report["rule_violations"][0];
    REQUIRE(violation["rule_name"].get<std::string>() == "nthash_stage");
    REQUIRE(violation["test_case"].get<std::string>() == "XXL_HIFI");
    REQUIRE(violation["args"][0].get<double>()
            == Catch::Approx(
                Report76XxlHifiP50::simd_sketch * 1'000.0 * 1.01));
    REQUIRE(violation["args"][1].get<double>()
            == Report76XxlHifiP50::scalar_sketch * 1'000.0);

    // A stage rename/bypass must not make the comparison disappear. The
    // selected raw source is absent, so the rule fails with an explicit
    // presence bitmap instead of silently continuing.
    const auto missing_stage = run_fixture(1.0, 1u, false, true, false);
    REQUIRE(missing_stage.exit_code == 1);
    REQUIRE(missing_stage.report["rule_violations"].size() == 1);
    const auto& missing_violation = missing_stage.report["rule_violations"][0];
    REQUIRE(missing_violation["rule_name"].get<std::string>()
            == "region_source_scope");
    REQUIRE_FALSE(missing_violation["args_present"][0].get<bool>());
    REQUIRE(missing_violation["args_present"][1].get<bool>());

    // A missing hardware row is represented by all-NaN evaluation metrics.
    // Its bookkeeping wall clock must not be accepted as a real GPU sample.
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

// ---------------------------------------------------------------------------
//  Test 3 — Sanity warning + history walk
// ---------------------------------------------------------------------------

TEST_CASE("metric_looks_constant fires when evaluator returns hardcoded value",
          "[bench_framework][sanity]") {
    TestLogSilencer silence_mlogging;

    static constexpr auto& kNoParams = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"identity", std::span<const AlgoParamSpec>{kNoParams}}
    }};
    static constexpr std::array<TestCaseDataSpec, 1> kTestCases{{
        {"ONE", "constant", R"({"length":4})"}
    }};
    static constexpr std::array<ForEntry, 1> kForEntries{{ {"ONE", 3} }};
    static constexpr std::array<OverEntry, 1> kOverEntries{{ {"identity", "{}"} }};
    const std::array<MetricSpecLite, 1> metrics{{
        {"frozen", "evaluation", "", "", "[]"},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_const",
            .description = "constant",
            .concurrency = false,
            .for_entries = std::span<const ForEntry>{kForEntries},
            .over_entries = std::span<const OverEntry>{kOverEntries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        }
    }};

    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;

    auto on_run = [](std::string_view, const ParamMap&, const ToyInput&) -> ToyOutput {
        return ToyOutput{};
    };
    auto on_evaluate_constant = [](std::string_view, const ParamMap&,
                                     const ToyInput&, const ToyOutput&)
        -> std::map<std::string, double>
    {
        return { {"frozen", 42.0} };
    };

    // Run 1: writes report_1.json. Warning should fire with
    // warning_since == report_1.json (first run, no history).
    {
        auto outcome = run_one_bench(
            "bench_const", "constant run 1", config_text, report_dir,
            build_toy_input, on_run, on_evaluate_constant);
        REQUIRE(outcome.exit_code == 0);   // sanity warnings are non-blocking
        REQUIRE(outcome.report_path.filename() == "report_1.json");
        REQUIRE(count_sanity_warnings_of_type(outcome.report, "metric_looks_constant") == 1);
        const auto& warning = outcome.report["sanity_warnings"][0];
        REQUIRE(warning["metric"].get<std::string>() == "frozen");
        REQUIRE(warning["warning_since"].get<std::string>() == "report_1.json");
        REQUIRE(warning["message"].get<std::string>().find("'42'")
                != std::string::npos);
    }

    // Run 2: same constant evaluator — history walk back to report_1
    // should still find the same constancy.
    {
        auto outcome = run_one_bench(
            "bench_const", "constant run 2", config_text, report_dir,
            build_toy_input, on_run, on_evaluate_constant);
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(outcome.report_path.filename() == "report_2.json");
        REQUIRE(count_sanity_warnings_of_type(outcome.report, "metric_looks_constant") == 1);
        const auto& warning = outcome.report["sanity_warnings"][0];
        REQUIRE(warning["warning_since"].get<std::string>() == "report_1.json");
    }

    // Run 3: switch evaluator to vary by iteration — warning should
    // disappear (combined { 42.0 } ∪ { iteration_specific } has > 1
    // distinct value, so the run breaks constancy).
    {
        std::atomic<std::int64_t> iteration_counter{0};
        auto on_evaluate_varying = [&iteration_counter](
            std::string_view, const ParamMap&,
            const ToyInput&, const ToyOutput&)
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
        REQUIRE(count_sanity_warnings_of_type(outcome.report, "metric_looks_constant") == 0);
    }
}

// ---------------------------------------------------------------------------
//  Test 4 — Regression rule walks history
// ---------------------------------------------------------------------------

TEST_CASE("regression rule fires after value drift", "[bench_framework][regression]") {
    TestLogSilencer silence_mlogging;

    static constexpr auto& kNoParams = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"identity", std::span<const AlgoParamSpec>{kNoParams}}
    }};
    static constexpr std::array<TestCaseDataSpec, 1> kTestCases{{
        {"ONE", "regression", R"({"length":4})"}
    }};
    static constexpr std::array<ForEntry, 1> kForEntries{{ {"ONE", 2} }};
    static constexpr std::array<OverEntry, 1> kOverEntries{{ {"identity", "{}"} }};
    const std::array<MetricSpecLite, 1> metrics{{
        {"value", "evaluation", "", "", "[]"},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_reg",
            .description = "regression",
            .concurrency = false,
            .for_entries = std::span<const ForEntry>{kForEntries},
            .over_entries = std::span<const OverEntry>{kOverEntries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        }
    }};
    static constexpr std::string_view kRegressionRules = R"([{
        "name":"value_eq",
        "metric":"value",
        "aggregate":"sum",
        "operator":{"compare":"=","tolerance%":0}
    }])";
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kTestCases},
        std::span<const BenchmarkSpec>{benches},
        kRegressionRules);

    TempReportDir report_dir;

    auto on_run = [](std::string_view, const ParamMap&, const ToyInput&) -> ToyOutput {
        return ToyOutput{};
    };
    auto on_eval_one = [](std::string_view, const ParamMap&,
                           const ToyInput&, const ToyOutput&)
        -> std::map<std::string, double> { return { {"value", 1.0} }; };
    auto on_eval_two = [](std::string_view, const ParamMap&,
                           const ToyInput&, const ToyOutput&)
        -> std::map<std::string, double> { return { {"value", 2.0} }; };

    // Run 1: baseline with value=1.0. No prior data → 0 regressions
    // (just baseline_missing warnings, which are non-blocking).
    {
        auto outcome = run_one_bench(
            "bench_reg", "regression baseline", config_text, report_dir,
            build_toy_input, on_run, on_eval_one);
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(outcome.report.contains("regressions"));
        REQUIRE(outcome.report["regressions"].empty());
    }

    // Run 2: same config but evaluator returns 2.0 — sum=4.0 vs prior
    // sum=2.0 → regression (=, tolerance% 0).
    {
        auto outcome = run_one_bench(
            "bench_reg", "regression now", config_text, report_dir,
            build_toy_input, on_run, on_eval_two);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(outcome.report["regressions"].size() == 1);
        const auto& entry = outcome.report["regressions"][0];
        REQUIRE(entry["rule_name"].get<std::string>() == "value_eq");
        REQUIRE(entry["regression_since"].get<std::string>() == "report_1.json");
        REQUIRE(entry["previously"].get<double>() == 2.0);   // 1.0 + 1.0
        REQUIRE(entry["now"].get<double>()        == 4.0);   // 2.0 + 2.0
    }
}

// ---------------------------------------------------------------------------
//  Test 5 — V7 behaviour: extra evaluator keys are silently dropped
// ---------------------------------------------------------------------------

TEST_CASE("evaluator returns extra unused key — silently dropped", "[bench_framework][v7]") {
    TestLogSilencer silence_mlogging;

    static constexpr auto& kNoParams = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"identity", std::span<const AlgoParamSpec>{kNoParams}}
    }};
    static constexpr std::array<TestCaseDataSpec, 1> kTestCases{{
        {"ONE", "case", R"({"length":4})"}
    }};
    static constexpr std::array<ForEntry, 1> kForEntries{{ {"ONE", 2} }};
    static constexpr std::array<OverEntry, 1> kOverEntries{{ {"identity", "{}"} }};
    // Declare ONLY `accuracy`. Evaluator will ALSO return `latency`
    // (unused for this benchmark) — the V7 rule says drop silently.
    const std::array<MetricSpecLite, 1> metrics{{
        {"accuracy", "evaluation", "", "", "[]"}
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_extra",
            .concurrency = false,
            .for_entries = std::span<const ForEntry>{kForEntries},
            .over_entries = std::span<const OverEntry>{kOverEntries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        }
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;

    auto on_run = [](std::string_view, const ParamMap&, const ToyInput&) -> ToyOutput {
        return ToyOutput{};
    };
    auto on_evaluate = [](std::string_view, const ParamMap&,
                           const ToyInput&, const ToyOutput&)
        -> std::map<std::string, double>
    {
        return {
            {"accuracy",  0.95},
            {"latency",   123.0},   // not declared — must be dropped
            {"throughput", 7.0},    // also not declared — must be dropped
        };
    };

    auto outcome = run_one_bench(
        "bench_extra", "v7 extra keys", config_text, report_dir,
        build_toy_input, on_run, on_evaluate);

    REQUIRE(outcome.exit_code == 0);
    REQUIRE_FALSE(stderr_contains(outcome.captured_stderr, "evaluation_key_not_declared"));

    REQUIRE(samples_count_at(outcome.report, "bench_extra",
                              "identity", "ONE",
                              "all_regions", "accuracy") == 2);
    // Every iteration emits 0.95 → min == max == 0.95.
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

// ---------------------------------------------------------------------------
//  Test 6 — Symmetric: declared eval metric never returned still errors
// ---------------------------------------------------------------------------

TEST_CASE("declared evaluator metric never returned still errors", "[bench_framework][v7]") {
    TestLogSilencer silence_mlogging;

    static constexpr auto& kNoParams = kDummyParams;
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"identity", std::span<const AlgoParamSpec>{kNoParams}}
    }};
    static constexpr std::array<TestCaseDataSpec, 1> kTestCases{{
        {"ONE", "missing", R"({"length":4})"}
    }};
    static constexpr std::array<ForEntry, 1> kForEntries{{ {"ONE", 2} }};
    static constexpr std::array<OverEntry, 1> kOverEntries{{ {"identity", "{}"} }};
    const std::array<MetricSpecLite, 1> metrics{{
        {"never_returned", "evaluation", "", "", "[]"}
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_missing",
            .concurrency = false,
            .for_entries = std::span<const ForEntry>{kForEntries},
            .over_entries = std::span<const OverEntry>{kOverEntries},
            .metrics = std::span<const MetricSpecLite>{metrics},
        }
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;

    auto on_run = [](std::string_view, const ParamMap&, const ToyInput&) -> ToyOutput {
        return ToyOutput{};
    };
    auto on_evaluate_empty = [](std::string_view, const ParamMap&,
                                  const ToyInput&, const ToyOutput&)
        -> std::map<std::string, double> { return {}; };

    auto outcome = run_one_bench(
        "bench_missing", "missing eval", config_text, report_dir,
        build_toy_input, on_run, on_evaluate_empty);

    REQUIRE(outcome.exit_code == 1);
    REQUIRE(stderr_contains(outcome.captured_stderr, "missing_evaluation_metric"));
    // V7 must NOT bring back the deleted error code.
    REQUIRE_FALSE(stderr_contains(outcome.captured_stderr, "evaluation_key_not_declared"));
}

// ---------------------------------------------------------------------------
//  Test 7 — Schema validation rejects malformed config
// ---------------------------------------------------------------------------

TEST_CASE("schema validation rejects config missing 'params'", "[bench_framework][schema]") {
    TestLogSilencer silence_mlogging;

    // Hand-rolled bad config: missing top-level `params`. The schema
    // demands it; the framework must refuse the run with a `schema`
    // diagnostic.
    constexpr std::string_view bad_config =
        R"({"test_cases":{"X":{"data":{}}},"bench_x":{"matrix":{"for":{"X":1},"over":{},"metrics":{"wall_clock_ns":{"rules":[]}}}}})";

    TempReportDir report_dir;

    auto on_run = [](std::string_view, const ParamMap&, const ToyInput&) -> ToyOutput {
        return ToyOutput{};
    };
    auto on_evaluate = [](std::string_view, const ParamMap&,
                           const ToyInput&, const ToyOutput&)
        -> std::map<std::string, double> { return {}; };

    auto outcome = run_one_bench(
        "bench_x", "schema fail", bad_config, report_dir,
        build_toy_input, on_run, on_evaluate);

    REQUIRE(outcome.exit_code == 1);
    // The schema error code is "schema"; the human banner says
    // "configuration validation". Either substring is fine to assert
    // on — the second is more specific.
    REQUIRE(stderr_contains(outcome.captured_stderr, "configuration validation"));
}

// ---------------------------------------------------------------------------
//  Test 8 — V9 on_cleanup hook: invocation, ordering, zero-iteration skip
// ---------------------------------------------------------------------------

TEST_CASE("on_cleanup runs once per cached test_case after iterations finish",
          "[bench_framework][cleanup][v9]") {
    TestLogSilencer silence_mlogging;

    // Schema requires per-algorithm params to be non-empty; carry a
    // single defaulted scalar so this minimal benchmark validates.
    static constexpr std::array<AlgoParamSpec, 1> kParams{{
        {"size_hint", "integer", "0", ""}
    }};
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"toy", std::span<const AlgoParamSpec>{kParams}}
    }};
    // Three test cases; one is given zero iterations so the cleanup
    // path can prove it skips that slot. `.concurrency = 1` pins
    // each test case to a single group so on_cleanup runs exactly
    // once per used test case (V11 splits multi-group test cases
    // across workers and calls cleanup once per group).
    static constexpr std::array<TestCaseDataSpec, 3> kTestCases{{
        {"TINY",  "first",  R"({"length":4,"seed":1})",  1},
        {"MID",   "second", R"({"length":8,"seed":2})",  1},
        {"NEVER", "zero",   R"({"length":16,"seed":3})", 1},
    }};
    static constexpr std::array<ForEntry, 3> kForEntries{{
        {"TINY",  3},
        {"MID",   2},
        {"NEVER", 0},
    }};
    static constexpr std::array<OverEntry, 1> kOverEntries{{
        {"toy", "{}"}
    }};
    static constexpr std::array<MetricSpecLite, 1> kMetrics{{
        {"wall_clock_ns", "builtin", "", "", "[]"}
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_cleanup",
            .description = "cleanup hook",
            .fast = false,
            .bench_lock = false,
            .concurrency = false,
            .by_regions = false,
            .for_entries  = std::span<const ForEntry>{kForEntries},
            .over_entries = std::span<const OverEntry>{kOverEntries},
            .metrics      = std::span<const MetricSpecLite>{kMetrics},
        }
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;

    auto on_run = [](std::string_view, const ParamMap&, const ToyInput&) -> ToyOutput {
        return ToyOutput{};
    };
    auto on_evaluate = [](std::string_view, const ParamMap&,
                           const ToyInput&, const ToyOutput&)
        -> std::map<std::string, double> { return {}; };

    SECTION("default-empty cleanup is a silent no-op") {
        // Sanity check the no-cleanup path: the wrapper's
        // `OnCleanup = std::nullptr_t` default leaves the framework's
        // `std::function` empty, and the run still completes cleanly.
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
                              const ToyInput& input)
        {
            seen_names.emplace_back(test_case_name);
            seen_lengths.push_back(input.values.size());
        };

        const auto outcome = run_one_bench(
            "bench_cleanup", "with cleanup", config_text, report_dir,
            build_toy_input, on_run, on_evaluate, on_cleanup);
        REQUIRE(outcome.exit_code == 0);

        // Both used test cases get exactly one cleanup call. The
        // zero-iteration slot ("NEVER") is skipped because the
        // input was never built.
        REQUIRE(seen_names.size() == 2);
        const std::vector<std::string> expected_names{"TINY", "MID"};
        REQUIRE(seen_names == expected_names);
        // Lengths reflect the JSON `length` field — ToyInput stores
        // exactly that many randomised values.
        REQUIRE(seen_lengths == std::vector<std::size_t>{4, 8});
    }
}

// ===========================================================================
//  DOC0349 — `tags` config field, `--filter` selection, `--debug` JSONL stream
// ===========================================================================
//
// These tests are black-box: `FilterSpec` / `parse_one_filter_into` /
// `DebugReportSink` live in the framework's internal `detail` namespace (not
// exported), so the suite drives the public `run()` entry point with
// `--filter` / `--debug` argv tokens and asserts on the exit code, the
// report_<N>.json contents, the report_debug_<N>.jsonl event stream, and the
// stderr diagnostic banner (ValidationDiag codes printed by report_diagnostics,
// captured by StderrCapture).

namespace {

using bioseq::bench_framework::ErasedBenchmark;
using bioseq::bench_framework::RunConstraints;

/// on_estimate override: returns a per-test-case `RunConstraints`. Lets the
/// abort-sweep test force `memory_budget_not_satisfiable` on one test case.
using Estimator = std::function<RunConstraints(
    std::string_view, std::string_view, simdjson::dom::element)>;

/// Build one toy ErasedBenchmark over (ToyInput, ToyOutput). `estimate`
/// defaults to the unconstrained estimator; pass a custom one to provoke
/// memory-budget skips. All benches share the trivial sum/eval bodies.
[[nodiscard]] inline ErasedBenchmark make_toy_bench(
    std::string_view key, std::string_view title, const std::string& config_path,
    Estimator estimate = nullptr)
{
    using InputFn = std::function<ToyInput(std::string_view, std::string_view,
                                           simdjson::dom::element, const RunConstraints&)>;
    using RunFn = std::function<ToyOutput(std::string_view, const ParamMap&, const ToyInput&)>;
    using EvalFn = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&, const ToyInput&, const ToyOutput&)>;
    using CleanupFn = std::function<void(std::string_view, std::string_view,
                                         simdjson::dom::element, const ToyInput&)>;

    Estimator est = estimate
        ? std::move(estimate)
        : Estimator{[](std::string_view, std::string_view,
                       simdjson::dom::element) noexcept { return RunConstraints{}; }};
    InputFn inp = [](std::string_view data_dir, std::string_view test_case_name,
                     simdjson::dom::element data, const RunConstraints&) {
        return build_toy_input(data_dir, test_case_name, data);
    };
    RunFn run_fn = [](std::string_view, const ParamMap&, const ToyInput& in) {
        ToyOutput out;
        for (const auto value : in.values) out.sum += value;
        return out;
    };
    EvalFn eval_fn = [](std::string_view, const ParamMap&, const ToyInput&,
                        const ToyOutput& out) {
        return std::map<std::string, double>{{"sum_value", static_cast<double>(out.sum)}};
    };
    return create_benchmark<ToyInput, ToyOutput>(
        std::string{title}, std::string{key}, config_path,
        std::move(est), std::move(inp), std::move(run_fn),
        std::move(eval_fn), CleanupFn{});
}

/// Write `config_text` into a stable file under `report_dir` and return its
/// path. Multiple benchmark blocks may share one file (mirrors bench_v10).
[[nodiscard]] inline std::string write_config_file(const TempReportDir& report_dir,
                                                   std::string_view config_text) {
    const auto path = report_dir.path / "config.json";
    std::ofstream cfg(path, std::ios::trunc | std::ios::binary);
    cfg.write(config_text.data(), static_cast<std::streamsize>(config_text.size()));
    cfg.close();
    return path.string();
}

/// Variadic driver: run `benches` with argv = base flags + `extra_args`,
/// capturing the exit code, the written report JSON, and stderr.
template<typename... Benches>
[[nodiscard]] RunOutcome run_with_args(const TempReportDir& report_dir,
                                       std::span<const std::string> extra_args,
                                       Benches&&... benches) {
    std::vector<std::string> args = {
        "bench_framework_test", "--report", report_dir.path.string(),
        "--threads", "2",
    };
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    ArgvBuilder argv_holder{std::span<const std::string>{args}};

    RunOutcome outcome;
    {
        StderrCapture capture;
        outcome.exit_code = run(argv_holder.argc(), argv_holder.argv(),
                                report_dir.path.string(), report_dir.path.string(),
                                std::forward<Benches>(benches)...);
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

/// Locate the single report_debug_<N>.jsonl under `report_dir` (tests use a
/// fresh dir, so there is at most one). Returns nullopt when absent.
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

/// Parse a JSONL file into one nlohmann::json per non-empty line.
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

/// The set of algorithm (over-key) names present under one benchmark.
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

// Shared toy fixtures for the filter/debug tests: one algorithm parameter,
// two test cases, the `sum_value` evaluation metric.
inline constexpr std::array<AlgoParamSpec, 1> kFilterParams{{
    {"unused", "integer", "0", ""}
}};
inline constexpr std::array<TestCaseDataSpec, 2> kFilterTestCases{{
    {"SMALL", "8 ints",  R"({"length":8,"seed":3})",  1},
    {"BIG",   "16 ints", R"({"length":16,"seed":5})", 1},
}};
inline constexpr std::array<MetricSpecLite, 1> kFilterMetrics{{
    {"sum_value", "evaluation", "", "", "[]"},
}};

}  // namespace

TEST_CASE("XXL_HIFI relative benchmark config has one input and nine rows",
          "[bench_framework][xxl-relative][configuration]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        / "bench" / "bench_v10_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "main" / "bench" / "bench_v10_configuration.json";
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
                .at("concurrency").get<std::size_t>() == 2);

    const std::set<std::string> expected_algorithms{
        "minimap2_dp",
        "bioseq_ksw2_scalar_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_scalar_mm",
        "bioseq_ksw2_simd_dp_two_stack_nthash",
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
        "bioseq_ksw2_batched_simd_dp_two_stack_nthash",
        "bioseq_block_simd_dp_two_stack_nthash",
        "bioseq_block_vulkan_dp_vulkan_minimap_int",
        "bioseq_block_vulkan_phase_c_frontend",
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
    REQUIRE(metrics.at("wall_clock_ns").at("rules").size() == 7);
    REQUIRE(metrics.at("region_total_ns").at("rules").size() == 4);
    REQUIRE(metrics.at("match_count").at("rules").size() == 8);
    REQUIRE(metrics.at("primary_correct_fraction").at("rules").size() == 8);
    REQUIRE(metrics.contains("historical_partial_gpu_probe"));
    REQUIRE(metrics.contains("all_gpu_acceptance_eligible"));
    REQUIRE(relative.at("description").get<std::string>().find(
                "historical partial-GPU") != std::string::npos);
    for (const auto* partial_gpu_algorithm : {
            "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_phase_c_frontend"}) {
        const std::string description = config.at("params")
            .at(partial_gpu_algorithm).at("_").at("description")
            .get<std::string>();
        REQUIRE(description.find("partial-GPU") != std::string::npos);
        REQUIRE(description.find("not all-GPU acceptance evidence")
                != std::string::npos);
    }
    const std::set<std::string> expected_rule_names{
        "report76_xxl_hifi_block_simd_over_ksw2_simd_wall_ratio_max_1_057921",
        "report76_xxl_hifi_ksw2_batched_over_ksw2_simd_wall_ratio_max_1_000654",
        "report76_xxl_hifi_vulkan_tiled_over_ksw2_simd_wall_ratio_max_1_011221",
        "report76_xxl_hifi_legacy_block_vulkan_over_block_simd_wall_ratio_max_1_714040",
        "report76_xxl_hifi_phase_c_block_vulkan_over_block_simd_wall_ratio_max_1_722027",
        "report76_xxl_hifi_ksw2_simd_over_minimap2_wall_ratio_max_0_797045",
        "report76_xxl_hifi_ksw2_simd_over_scalar_wall_ratio_max_0_540314",
        "report76_xxl_hifi_nthash_simd_over_scalar_sketch_ratio_max_0_210496957",
        "report76_xxl_hifi_chaining_simd_over_scalar_ratio_max_0_579268421",
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

    const std::map<std::string, double> expected_report76_ratios{
        {"report76_xxl_hifi_block_simd_over_ksw2_simd_wall_ratio_max_1_057921",
         Report76XxlHifiP50::block_wall / Report76XxlHifiP50::simd_wall},
        {"report76_xxl_hifi_ksw2_batched_over_ksw2_simd_wall_ratio_max_1_000654",
         Report76XxlHifiP50::batched_wall / Report76XxlHifiP50::simd_wall},
        {"report76_xxl_hifi_vulkan_tiled_over_ksw2_simd_wall_ratio_max_1_011221",
         Report76XxlHifiP50::vulkan_wall / Report76XxlHifiP50::simd_wall},
        {"report76_xxl_hifi_legacy_block_vulkan_over_block_simd_wall_ratio_max_1_714040",
         17'180'439'666.0 / Report76XxlHifiP50::block_wall},
        {"report76_xxl_hifi_phase_c_block_vulkan_over_block_simd_wall_ratio_max_1_722027",
         17'260'490'000.0 / Report76XxlHifiP50::block_wall},
        {"report76_xxl_hifi_ksw2_simd_over_minimap2_wall_ratio_max_0_797045",
         Report76XxlHifiP50::simd_wall / Report76XxlHifiP50::minimap2_wall},
        {"report76_xxl_hifi_ksw2_simd_over_scalar_wall_ratio_max_0_540314",
         Report76XxlHifiP50::simd_wall / Report76XxlHifiP50::scalar_wall},
        {"report76_xxl_hifi_nthash_simd_over_scalar_sketch_ratio_max_0_210496957",
         Report76XxlHifiP50::simd_sketch / Report76XxlHifiP50::scalar_sketch},
        {"report76_xxl_hifi_chaining_simd_over_scalar_ratio_max_0_579268421",
         Report76XxlHifiP50::simd_chain / Report76XxlHifiP50::scalar_chain},
        {"report76_xxl_hifi_backtrack_simd_over_scalar_ratio_max_0_955048972",
         Report76XxlHifiP50::simd_backtrack / Report76XxlHifiP50::scalar_backtrack},
        {"report76_xxl_hifi_block_over_ksw2_alignment_ratio_max_0_591278239",
         Report76XxlHifiP50::block_alignment / Report76XxlHifiP50::simd_alignment},
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
                        expected_report76_ratios.at(rule_name))
                        .margin(allowed_rounding));
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

TEST_CASE("XXL_HIFI paired companion is serial interleaved and ratio-paired",
          "[bench_framework][xxl-relative][paired][configuration]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        / "bench" / "bench_v10_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "main" / "bench" / "bench_v10_configuration.json";
    }
    REQUIRE(std::filesystem::exists(config_path));

    std::ifstream config_stream{config_path};
    REQUIRE(config_stream.good());
    const nlohmann::json config = nlohmann::json::parse(config_stream);
    REQUIRE(config.contains("bench_aligners_xxl_hifi_relative"));
    REQUIRE(config.contains("bench_aligners_xxl_hifi_paired"));

    // The exact report_76 compatibility gate remains a distinct four-repeat
    // benchmark. Its comparators keep the historical independent-P50 shape.
    const auto& compatibility =
        config.at("bench_aligners_xxl_hifi_relative");
    REQUIRE(compatibility.at("matrix").at("for").at("XXL_HIFI") == 4);
    for (const auto& metric_name : {"wall_clock_ns", "region_total_ns"}) {
        for (const auto& rule :
             compatibility.at("matrix").at("metrics")
                 .at(metric_name).at("rules")) {
            REQUIRE_FALSE(rule.at(2).contains("pairing"));
            REQUIRE_FALSE(rule.at(2).contains("paired_baseline"));
            REQUIRE_FALSE(rule.at(2).contains("blocking"));
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
        "bioseq_block_vulkan_phase_c_frontend",
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
    REQUIRE(metrics.at("wall_clock_ns").at("rules").size() == 8);
    REQUIRE(metrics.at("region_total_ns").at("rules").size() == 4);
    std::set<std::string> paired_rule_names;
    for (const auto& metric_name : {"wall_clock_ns", "region_total_ns"}) {
        for (const auto& rule : metrics.at(metric_name).at("rules")) {
            paired_rule_names.insert(rule.at(0).get<std::string>());
            const auto& comparator = rule.at(2);
            REQUIRE(comparator.at("pairing") == "by_iteration_ratio");
            REQUIRE(comparator.at("blocking") == false);
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
            {"paired_report76_block_simd_over_ksw2_simd_wall_with_10pct_noise",
             {Report76XxlHifiP50::block_wall
                  / Report76XxlHifiP50::simd_wall, 0.10}},
            {"paired_report76_ksw2_batched_over_ksw2_simd_wall_with_10pct_noise",
             {Report76XxlHifiP50::batched_wall
                  / Report76XxlHifiP50::simd_wall, 0.10}},
            {"paired_report76_vulkan_tiled_over_ksw2_simd_wall_with_15pct_noise",
             {Report76XxlHifiP50::vulkan_wall
                  / Report76XxlHifiP50::simd_wall, 0.15}},
            {"paired_report76_ksw2_simd_over_minimap2_wall_with_10pct_noise",
             {Report76XxlHifiP50::simd_wall
                  / Report76XxlHifiP50::minimap2_wall, 0.10}},
            {"paired_report76_ksw2_simd_over_scalar_wall_with_10pct_noise",
             {Report76XxlHifiP50::simd_wall
                  / Report76XxlHifiP50::scalar_wall, 0.10}},
            {"paired_report76_legacy_block_vulkan_over_block_simd_wall_with_15pct_noise",
             {17'180'439'666.0 / Report76XxlHifiP50::block_wall, 0.15}},
            {"paired_report76_phase_c_block_vulkan_over_block_simd_wall_with_15pct_noise",
             {17'260'490'000.0 / Report76XxlHifiP50::block_wall, 0.15}},
            {"paired_report76_nthash_simd_over_scalar_sketch_with_15pct_noise",
             {Report76XxlHifiP50::simd_sketch
                  / Report76XxlHifiP50::scalar_sketch, 0.15}},
            {"paired_report76_chaining_simd_over_scalar_with_15pct_noise",
             {Report76XxlHifiP50::simd_chain
                  / Report76XxlHifiP50::scalar_chain, 0.15}},
            {"paired_report76_backtrack_simd_over_scalar_with_15pct_noise",
             {Report76XxlHifiP50::simd_backtrack
                  / Report76XxlHifiP50::scalar_backtrack, 0.15}},
            {"paired_report76_block_over_ksw2_alignment_with_15pct_noise",
             {Report76XxlHifiP50::block_alignment
                  / Report76XxlHifiP50::simd_alignment, 0.15}},
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

    for (const auto& proof_metric : {
            "historical_partial_gpu_probe",
            "all_gpu_acceptance_eligible",
            "phase_c_gpu_index_ownership_proof_accepted",
            "vulkan_query_sketch_dispatches",
            "vulkan_target_sketch_dispatches",
            "vulkan_target_index_build_dispatches",
            "vulkan_seed_lookup_dispatches",
            "vulkan_chain_score_dispatches",
            "gpu_index_count_phase_admissions",
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
        REQUIRE(metrics.contains(proof_metric));
    }
    const std::string description =
        paired.at("description").get<std::string>();
    REQUIRE(description.find("first-repeat") != std::string::npos);
    REQUIRE(description.find("not a globally cold") != std::string::npos);
    REQUIRE(description.find("Raw evidence") != std::string::npos);
}

TEST_CASE("XXL_HIFI relative config fail-closes partial GPU evidence and Block Vulkan ratios",
          "[bench_framework][xxl-relative][configuration][vulkan]") {
    const auto source_path = std::filesystem::path{__FILE__};
    auto config_path = source_path.parent_path().parent_path()
        / "bench" / "bench_v10_configuration.json";
    if (!std::filesystem::exists(config_path)) {
        config_path = std::filesystem::current_path()
            / "main" / "bench" / "bench_v10_configuration.json";
    }
    REQUIRE(std::filesystem::exists(config_path));

    std::ifstream config_stream{config_path};
    REQUIRE(config_stream.good());
    const nlohmann::json config = nlohmann::json::parse(config_stream);
    const auto& metrics = config.at("bench_aligners_xxl_hifi_relative")
                              .at("matrix").at("metrics");

    const std::set<std::string> partial_gpu_algorithms{
        "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
        "bioseq_block_vulkan_dp_vulkan_minimap_int",
        "bioseq_block_vulkan_phase_c_frontend",
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

    // The classification must be executable acceptance policy, not merely a
    // description or an otherwise-unused metric. A boolean partial-GPU row is
    // positive relative to the minimap2 control; all-GPU eligibility is the
    // same false value as that control.
    const auto require_positive_partial_gpu_rules = [
            &metrics, &partial_gpu_algorithms, &require_zero_knobs] {
        const auto& rules = metrics.at("historical_partial_gpu_probe")
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
        REQUIRE(covered == partial_gpu_algorithms);
    };
    require_positive_partial_gpu_rules();

    // The complement is explicit too. Because the metric is boolean, every
    // non-Vulkan row being strictly below a positively attested Vulkan row
    // proves a zero classification without relying on minimap2 as an
    // unproven/circular zero control.
    const auto& partial_gpu_rules =
        metrics.at("historical_partial_gpu_probe").at("rules");
    REQUIRE(partial_gpu_rules.size()
            == partial_gpu_algorithms.size() + non_vulkan_algorithms.size());
    std::set<std::string> negatively_classified;
    for (const auto& rule : partial_gpu_rules) {
        if (rule.at(2).at("compare").get<std::string>() != "<") continue;
        REQUIRE(rule.size() == 4);
        require_zero_knobs(rule.at(2));
        REQUIRE(rule.at(2).at("aggregate").get<std::string>() == "max");
        REQUIRE(partial_gpu_algorithms.contains(
            rule.at(3).at("key").get<std::string>()));
        negatively_classified.insert(
            rule.at(1).at("key").get<std::string>());
    }
    REQUIRE(negatively_classified == non_vulkan_algorithms);

    const auto& all_gpu_rules =
        metrics.at("all_gpu_acceptance_eligible").at("rules");
    REQUIRE(all_gpu_rules.size() == partial_gpu_algorithms.size());
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
    REQUIRE(disqualified_from_all_gpu == partial_gpu_algorithms);

    // Every GPU stage selected by each historical mixed pipeline must carry a
    // positive dispatch counter. This deliberately does not demand GPU target
    // index construction from the legacy row, or GPU sketching from the tiled
    // chaining-only row: those stages are CPU by their declared contracts.
    const std::map<std::string, std::set<std::string>> expected_positive_rows{
        {"vulkan_query_sketch_dispatches", {
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_phase_c_frontend"}},
        {"vulkan_target_sketch_dispatches", {
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_phase_c_frontend"}},
        {"vulkan_target_index_build_dispatches", {
            "bioseq_block_vulkan_phase_c_frontend"}},
        {"vulkan_seed_lookup_dispatches", {
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_phase_c_frontend"}},
        {"vulkan_chain_score_dispatches", {
            "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
            "bioseq_block_vulkan_dp_vulkan_minimap_int",
            "bioseq_block_vulkan_phase_c_frontend"}},
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

    // Phase C is accepted only with the complete operation-originated cold
    // ownership/gated-build contract. Positive fields use the minimap2 zero
    // control and `min`, while forbidden counters use equality and `max` so a
    // single bad repeat fails the benchmark.
    const std::set<std::string> positive_phase_c_ownership_metrics{
        "phase_c_gpu_index_ownership_proof_accepted",
        "gpu_index_count_phase_admissions",
        "gpu_index_main_peak_admissions",
        "gpu_index_resident_transfers",
        "gpu_index_adoption_successes",
        "gpu_index_owned_resident_bytes",
        "gpu_index_gate_enabled",
        "gpu_index_gate_word_count",
        "gpu_index_gate_mask",
        "gpu_index_gate_set_bits",
        "gpu_index_gate_count_dispatches",
        "gpu_index_gated_emit_dispatches",
        "gpu_index_retained_minimizer_count",
        "gpu_index_distinct_minimizer_count",
    };
    for (const auto& metric_name : positive_phase_c_ownership_metrics) {
        const auto& metric = metrics.at(metric_name);
        REQUIRE(metric.at("type") == "evaluation");
        REQUIRE(metric.at("rules").size() == 1u);
        const auto& rule = metric.at("rules").at(0);
        REQUIRE(rule.size() == 4u);
        REQUIRE(rule.at(1).at("key")
                == "bioseq_block_vulkan_phase_c_frontend");
        REQUIRE(rule.at(1).at("test_case") == "XXL_HIFI");
        const auto& comparator = rule.at(2);
        REQUIRE(comparator.at("compare") == ">");
        REQUIRE(comparator.at("aggregate") == "min");
        REQUIRE(comparator.value("blocking", true) == true);
        require_zero_knobs(comparator);
        REQUIRE(rule.at(3).at("key") == "minimap2_dp");
        REQUIRE(rule.at(3).at("test_case") == "XXL_HIFI");
    }

    const std::set<std::string> zero_phase_c_ownership_metrics{
        "gpu_index_adoption_rereserve_attempts",
        "gpu_index_ungated_emit_dispatches",
    };
    for (const auto& metric_name : zero_phase_c_ownership_metrics) {
        const auto& metric = metrics.at(metric_name);
        REQUIRE(metric.at("type") == "evaluation");
        REQUIRE(metric.at("rules").size() == 1u);
        const auto& rule = metric.at("rules").at(0);
        REQUIRE(rule.size() == 4u);
        REQUIRE(rule.at(1).at("key")
                == "bioseq_block_vulkan_phase_c_frontend");
        const auto& comparator = rule.at(2);
        REQUIRE(comparator.at("compare") == "=");
        REQUIRE(comparator.at("aggregate") == "max");
        REQUIRE(comparator.value("blocking", true) == true);
        require_zero_knobs(comparator);
        REQUIRE(rule.at(3).at("key") == "minimap2_dp");
    }

    // report_76 P50 wall-clock sources. Both historical Block-Vulkan rows
    // need explicit within-run gates against the unchanged Block-SIMD control.
    static constexpr double block_simd_wall = 10'023'361'667.0;
    const std::map<std::string, double> expected_ratios{
        {"bioseq_block_vulkan_dp_vulkan_minimap_int",
         17'180'439'666.0 / block_simd_wall},
        {"bioseq_block_vulkan_phase_c_frontend",
         17'260'490'000.0 / block_simd_wall},
    };
    std::set<std::string> ratio_covered_algorithms;
    for (const auto& rule : metrics.at("wall_clock_ns").at("rules")) {
        const std::string lhs = rule.at(1).at("key").get<std::string>();
        if (!expected_ratios.contains(lhs)) continue;
        REQUIRE(rule.at(3).at("key").get<std::string>()
                == "bioseq_block_simd_dp_two_stack_nthash");
        const auto& comparator = rule.at(2);
        REQUIRE(comparator.at("compare").get<std::string>() == "<");
        REQUIRE(comparator.at("aggregate").get<std::string>() == "p50");
        const double configured_max_ratio =
            1.0
            - comparator.value("threshold%", 0.0) / 100.0
            + comparator.value("tolerance%", 0.0) / 100.0;
        const double report76_ratio = expected_ratios.at(lhs);
        REQUIRE(configured_max_ratio >= report76_ratio);
        REQUIRE(configured_max_ratio < report76_ratio + 0.000001);
        ratio_covered_algorithms.insert(lhs);
    }
    REQUIRE(ratio_covered_algorithms.size() == expected_ratios.size());
}

// ---------------------------------------------------------------------------
//  DOC0349 Test 1 — `tags` config field: parse + schema enforcement
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework tags config field parse + schema", "[bench_framework][tags]") {
    TestLogSilencer silence_mlogging;

    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"algo_a", std::span<const AlgoParamSpec>{kFilterParams}}
    }};
    static constexpr std::array<ForEntry, 1> kFor{{ {"SMALL", 2} }};
    static constexpr std::array<OverEntry, 1> kOver{{ {"algo_a", "{}"} }};

    SECTION("non-empty tags array parses and runs") {
        static constexpr std::array<std::string_view, 2> kTags{{"aligner", "grid"}};
        const std::array<BenchmarkSpec, 1> benches{{
            BenchmarkSpec{.key = "bench_tagged", .description = "tagged",
                          .tags = std::span<const std::string_view>{kTags},
                          .for_entries = std::span<const ForEntry>{kFor},
                          .over_entries = std::span<const OverEntry>{kOver},
                          .metrics = std::span<const MetricSpecLite>{kFilterMetrics}}
        }};
        const auto config_text = render_config(
            std::span<const AlgorithmSpec>{kAlgos},
            std::span<const TestCaseDataSpec>{kFilterTestCases},
            std::span<const BenchmarkSpec>{benches});
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        auto outcome = run_with_args(report_dir, std::span<const std::string>{},
                                     make_toy_bench("bench_tagged", "tagged", config_path));
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(report_benchmark_keys(outcome.report).count("bench_tagged") == 1);
    }

    SECTION("empty tags array is legal") {
        static constexpr std::array<std::string_view, 0> kNoTags{};
        const std::array<BenchmarkSpec, 1> benches{{
            BenchmarkSpec{.key = "bench_empty_tags", .description = "no tags",
                          .tags = std::span<const std::string_view>{kNoTags},
                          .for_entries = std::span<const ForEntry>{kFor},
                          .over_entries = std::span<const OverEntry>{kOver},
                          .metrics = std::span<const MetricSpecLite>{kFilterMetrics}}
        }};
        const auto config_text = render_config(
            std::span<const AlgorithmSpec>{kAlgos},
            std::span<const TestCaseDataSpec>{kFilterTestCases},
            std::span<const BenchmarkSpec>{benches});
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        auto outcome = run_with_args(report_dir, std::span<const std::string>{},
                                     make_toy_bench("bench_empty_tags", "no tags", config_path));
        REQUIRE(outcome.exit_code == 0);
    }

    SECTION("missing tags array fails schema validation") {
        // Hand-rolled config whose benchmark block omits the now-required
        // `tags` array. additionalProperties:false + required:["tags"] reject it.
        constexpr std::string_view bad_config =
            R"({"params":{"algo_a":{"unused":{"type":"integer","default":0}}},)"
            R"("test_cases":{"SMALL":{"data":{"length":8}}},)"
            R"("bench_no_tags":{"matrix":{"for":{"SMALL":2},"over":{"algo_a":{}},)"
            R"("metrics":{"sum_value":{"type":"evaluation"}}}}})";
        TempReportDir report_dir;
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
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, bad_config);
        auto outcome = run_with_args(report_dir, std::span<const std::string>{},
                                     make_toy_bench("bench_bad_tag", "bad tag", config_path));
        REQUIRE(outcome.exit_code == 1);
    }
}

// ---------------------------------------------------------------------------
//  DOC0349 Test 2 — `--filter` parse validation (errors + warnings)
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework --filter parse validation", "[bench_framework][filter]") {
    TestLogSilencer silence_mlogging;

    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"algo_a", std::span<const AlgoParamSpec>{kFilterParams}}
    }};
    static constexpr std::array<ForEntry, 1> kFor{{ {"SMALL", 2} }};
    static constexpr std::array<OverEntry, 1> kOver{{ {"algo_a", "{}"} }};
    static constexpr std::array<std::string_view, 1> kTags{{"aligner"}};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{.key = "bench_a", .description = "a",
                      .tags = std::span<const std::string_view>{kTags},
                      .for_entries = std::span<const ForEntry>{kFor},
                      .over_entries = std::span<const OverEntry>{kOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}}
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kFilterTestCases},
        std::span<const BenchmarkSpec>{benches});

    SECTION("tags=a selects the matching benchmark (exit 0)") {
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "tags=aligner"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 0);
        REQUIRE(report_benchmark_keys(outcome.report).count("bench_a") == 1);
    }

    SECTION("benchmark=key selects by key (exit 0)") {
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "benchmark=bench_a"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 0);
    }

    SECTION("duplicate dimension is a launch error") {
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 4> extra{"--filter", "tags=x", "--filter", "tags=y"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_duplicate_dimension"));
    }

    SECTION("malformed value (no '=') is a launch error") {
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "=x"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_malformed"));
    }

    SECTION("empty value list is a launch error") {
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "tags="};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_malformed"));
    }

    SECTION("empty item in list is a launch error") {
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "tags=a,,b"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_malformed"));
    }

    SECTION("unknown filter key is a launch error") {
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        const std::array<std::string, 2> extra{"--filter", "nope=x"};
        auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                     make_toy_bench("bench_a", "a", config_path));
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(outcome.captured_stderr, "filter_unknown_key"));
    }
}

// ---------------------------------------------------------------------------
//  DOC0349 Test 3 — selection across a multi-benchmark universe
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework --filter selection + warnings", "[bench_framework][filter]") {
    TestLogSilencer silence_mlogging;

    // Universe mirrors §6: grid+curated carry "aligner"; storage/rmq do not.
    static constexpr std::array<AlgorithmSpec, 3> kAlgos{{
        {"bioseq",   std::span<const AlgoParamSpec>{kFilterParams}},
        {"minimap2", std::span<const AlgoParamSpec>{kFilterParams}},
        {"bwa",      std::span<const AlgoParamSpec>{kFilterParams}},
    }};
    static constexpr std::array<ForEntry, 1> kFor{{ {"SMALL", 2} }};
    static constexpr std::array<OverEntry, 2> kGridOver{{ {"bioseq", "{}"}, {"minimap2", "{}"} }};
    static constexpr std::array<OverEntry, 3> kCuratedOver{{ {"bioseq", "{}"}, {"minimap2", "{}"}, {"bwa", "{}"} }};
    static constexpr std::array<OverEntry, 1> kStorageOver{{ {"bioseq", "{}"} }};
    static constexpr std::array<OverEntry, 1> kRmqOver{{ {"minimap2", "{}"} }};
    static constexpr std::array<std::string_view, 2> kGridTags{{"aligner", "grid"}};
    static constexpr std::array<std::string_view, 2> kCuratedTags{{"aligner", "curated"}};
    static constexpr std::array<std::string_view, 1> kStorageTags{{"storage"}};
    static constexpr std::array<std::string_view, 1> kRmqTags{{"rmq"}};
    const std::array<BenchmarkSpec, 4> benches{{
        BenchmarkSpec{.key = "bench_grid",    .description = "grid",    .tags = std::span<const std::string_view>{kGridTags},
                      .for_entries = std::span<const ForEntry>{kFor}, .over_entries = std::span<const OverEntry>{kGridOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}},
        BenchmarkSpec{.key = "bench_curated", .description = "curated", .tags = std::span<const std::string_view>{kCuratedTags},
                      .for_entries = std::span<const ForEntry>{kFor}, .over_entries = std::span<const OverEntry>{kCuratedOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}},
        BenchmarkSpec{.key = "bench_storage", .description = "storage", .tags = std::span<const std::string_view>{kStorageTags},
                      .for_entries = std::span<const ForEntry>{kFor}, .over_entries = std::span<const OverEntry>{kStorageOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}},
        BenchmarkSpec{.key = "bench_rmq",     .description = "rmq",     .tags = std::span<const std::string_view>{kRmqTags},
                      .for_entries = std::span<const ForEntry>{kFor}, .over_entries = std::span<const OverEntry>{kRmqOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}},
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kFilterTestCases},
        std::span<const BenchmarkSpec>{benches});

    SECTION("tags=aligner + cases=bioseq,bwa selects grid{bioseq} and curated{bioseq,bwa}") {
        TempReportDir report_dir;
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
        REQUIRE(keys.count("bench_storage") == 0);   // not aligner-tagged → pruned
        REQUIRE(keys.count("bench_rmq") == 0);
        // grid keeps only {bioseq} (bwa not in grid's over); curated keeps {bioseq,bwa}.
        REQUIRE(report_algorithms(outcome.report, "bench_grid") == std::set<std::string>{"bioseq"});
        REQUIRE(report_algorithms(outcome.report, "bench_curated") == std::set<std::string>{"bioseq", "bwa"});
        // 'bwa' exists in the kept subset (curated) → no unknown-case warning.
        REQUIRE_FALSE(stderr_contains(outcome.captured_stderr, "filter_unknown_case"));
    }

    SECTION("filter_unknown_case fires for a case absent from the kept subset") {
        TempReportDir report_dir;
        const auto config_path = write_config_file(report_dir, config_text);
        // bwa exists only in curated; pair it with benchmark=bench_grid (kept
        // subset = {grid}) so bwa is unknown there → warning, and grid still has
        // no matching case → selected_nothing error.
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
        TempReportDir report_dir;
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
        // Nothing ran → no report written.
        REQUIRE(report_benchmark_keys(outcome.report).empty());
    }
}

TEST_CASE("MCP benchmark/case filters select all nine XXL_HIFI rows only",
          "[bench_framework][filter][xxl-relative]") {
    TestLogSilencer silence_mlogging;

    static constexpr std::array<AlgorithmSpec, 9> algorithms{{
        {"minimap2_dp", std::span<const AlgoParamSpec>{kFilterParams}},
        {"bioseq_ksw2_scalar_dp_scalar_mm",
         std::span<const AlgoParamSpec>{kFilterParams}},
        {"bioseq_ksw2_simd_dp_scalar_mm",
         std::span<const AlgoParamSpec>{kFilterParams}},
        {"bioseq_ksw2_simd_dp_two_stack_nthash",
         std::span<const AlgoParamSpec>{kFilterParams}},
        {"bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash",
         std::span<const AlgoParamSpec>{kFilterParams}},
        {"bioseq_ksw2_batched_simd_dp_two_stack_nthash",
         std::span<const AlgoParamSpec>{kFilterParams}},
        {"bioseq_block_simd_dp_two_stack_nthash",
         std::span<const AlgoParamSpec>{kFilterParams}},
        {"bioseq_block_vulkan_dp_vulkan_minimap_int",
         std::span<const AlgoParamSpec>{kFilterParams}},
        {"bioseq_block_vulkan_phase_c_frontend",
         std::span<const AlgoParamSpec>{kFilterParams}},
    }};
    static constexpr std::array<TestCaseDataSpec, 1> test_cases{{
        {"XXL_HIFI", "toy XXL selection cell",
         R"({"length":4,"seed":76})", 1},
    }};
    static constexpr std::array<ForEntry, 1> for_entries{{
        {"XXL_HIFI", 4},
    }};
    static constexpr std::array<OverEntry, 9> over_entries{{
        {"minimap2_dp", "{}"},
        {"bioseq_ksw2_scalar_dp_scalar_mm", "{}"},
        {"bioseq_ksw2_simd_dp_scalar_mm", "{}"},
        {"bioseq_ksw2_simd_dp_two_stack_nthash", "{}"},
        {"bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash", "{}"},
        {"bioseq_ksw2_batched_simd_dp_two_stack_nthash", "{}"},
        {"bioseq_block_simd_dp_two_stack_nthash", "{}"},
        {"bioseq_block_vulkan_dp_vulkan_minimap_int", "{}"},
        {"bioseq_block_vulkan_phase_c_frontend", "{}"},
    }};
    static constexpr std::array<OverEntry, 1> decoy_over{{
        {"minimap2_dp", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> relative_tags{{
        "aligner", "xxl-hifi",
    }};
    static constexpr std::array<std::string_view, 1> decoy_tags{{
        "aligner",
    }};
    const std::array<BenchmarkSpec, 2> benches{{
        BenchmarkSpec{
            .key = "bench_aligners_xxl_hifi_relative",
            .description = "target benchmark",
            .tags = std::span<const std::string_view>{relative_tags},
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{over_entries},
            .metrics = std::span<const MetricSpecLite>{kFilterMetrics},
        },
        BenchmarkSpec{
            .key = "bench_decoy",
            .description = "must be filtered out",
            .tags = std::span<const std::string_view>{decoy_tags},
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{decoy_over},
            .metrics = std::span<const MetricSpecLite>{kFilterMetrics},
        },
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{algorithms},
        std::span<const TestCaseDataSpec>{test_cases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;
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
        "bioseq_block_vulkan_phase_c_frontend",
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
        "bioseq_block_vulkan_phase_c_frontend",
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
          "[bench_framework][filter][fast][fail-closed]") {
    TestLogSilencer silence_mlogging;

    static constexpr std::array<AlgorithmSpec, 1> algorithms{{
        {"algo_a", std::span<const AlgoParamSpec>{kFilterParams}},
    }};
    static constexpr std::array<ForEntry, 1> for_entries{{
        {"SMALL", 1},
    }};
    static constexpr std::array<OverEntry, 1> over_entries{{
        {"algo_a", "{}"},
    }};
    const std::array<BenchmarkSpec, 2> benches{{
        BenchmarkSpec{
            .key = "bench_explicit_slow",
            .description = "explicitly selected non-fast benchmark",
            .fast = false,
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{over_entries},
            .metrics = std::span<const MetricSpecLite>{kFilterMetrics},
        },
        BenchmarkSpec{
            .key = "bench_fast_decoy",
            .description = "must not make the explicit selection look successful",
            .fast = true,
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{over_entries},
            .metrics = std::span<const MetricSpecLite>{kFilterMetrics},
        },
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{algorithms},
        std::span<const TestCaseDataSpec>{kFilterTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;
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
          "[bench_framework][filter][rules][fail-closed]") {
    TestLogSilencer silence_mlogging;

    static constexpr std::array<AlgorithmSpec, 2> algorithms{{
        {"gate_lhs", std::span<const AlgoParamSpec>{kFilterParams}},
        {"gate_rhs", std::span<const AlgoParamSpec>{kFilterParams}},
    }};
    static constexpr std::array<ForEntry, 1> for_entries{{
        {"SMALL", 4},
    }};
    static constexpr std::array<OverEntry, 2> over_entries{{
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
    const std::array<MetricSpecLite, 1> metrics{{
        {"sum_value", "evaluation", "", "", comparison_rules},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_exact_acceptance",
            .description = "two-sided acceptance gate",
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{over_entries},
            .metrics = std::span<const MetricSpecLite>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const ForEntry>{for_entries},
        },
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{algorithms},
        std::span<const TestCaseDataSpec>{kFilterTestCases},
        std::span<const BenchmarkSpec>{benches});

    SECTION("the complete exact benchmark is valid") {
        TempReportDir report_dir;
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
        TempReportDir report_dir;
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
          "[bench_framework][rules][regions][provenance][fail-closed]") {
    TestLogSilencer silence_mlogging;

    static constexpr std::array<AlgorithmSpec, 2> algorithms{{
        {"partial_stage", std::span<const AlgoParamSpec>{kFilterParams}},
        {"full_stage", std::span<const AlgoParamSpec>{kFilterParams}},
    }};
    static constexpr std::array<TestCaseDataSpec, 1> test_cases{{
        {"XXL_HIFI", "four provenance samples",
         R"({"length":4,"seed":76})", 2},
    }};
    static constexpr std::array<ForEntry, 1> for_entries{{
        {"XXL_HIFI", 4},
    }};
    static constexpr std::array<OverEntry, 2> over_entries{{
        {"partial_stage", "{}"},
        {"full_stage", "{}"},
    }};
    static constexpr std::array<std::string_view, 2> acceptance_rows{{
        "partial_stage", "full_stage",
    }};
    static constexpr std::array<NamedRegionSampleMultiplicitySpec, 2>
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
    const std::array<MetricSpecLite, 1> metrics{{
        {"region_visits", "region", "", "", region_rules},
    }};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{
            .key = "bench_region_provenance",
            .description = "named region completeness fixture",
            .concurrency = false,
            .by_regions = true,
            .for_entries = std::span<const ForEntry>{for_entries},
            .over_entries = std::span<const OverEntry>{over_entries},
            .metrics = std::span<const MetricSpecLite>{metrics},
            .acceptance_exact_algorithm_rows =
                std::span<const std::string_view>{acceptance_rows},
            .acceptance_exact_test_case_iterations =
                std::span<const ForEntry>{for_entries},
            .acceptance_require_named_region_iteration_coverage = true,
            .acceptance_exact_named_region_sample_multiplicities =
                std::span<const NamedRegionSampleMultiplicitySpec>{
                    exact_named_region_samples},
        },
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{algorithms},
        std::span<const TestCaseDataSpec>{test_cases},
        std::span<const BenchmarkSpec>{benches});

    enum class PartialCoverageFixture {
        complete,
        missing_final_iteration,
        wrong_raw_sample_multiplicity,
        laundered_raw_sample_count,
        redistributed_one_one_one_five,
        redistributed_one_two_two_three,
    };
    const auto run_fixture_with_config = [](
            std::string_view active_config,
            PartialCoverageFixture fixture) {
        TempReportDir report_dir;
        std::atomic<std::size_t> partial_iterations{0};
        const auto on_run = [&partial_iterations, fixture](
                std::string_view algorithm, const ParamMap&, const ToyInput&) {
            // Two descriptors intentionally share one report-region name. A
            // correct completeness check tracks contributing iteration IDs;
            // it cannot equate raw descriptor sample count with iterations.
            static const auto* const partial_a =
                bioseq::instr::register_region_static("gate.provenance.partial");
            static const auto* const partial_b =
                bioseq::instr::register_region_static("gate.provenance.partial");
            static const auto* const partial_c =
                bioseq::instr::register_region_static("gate.provenance.partial");
            static const auto* const partial_d =
                bioseq::instr::register_region_static("gate.provenance.partial");
            static const auto* const partial_e =
                bioseq::instr::register_region_static("gate.provenance.partial");
            static const auto* const full =
                bioseq::instr::register_region_static("gate.provenance.full");
            if (algorithm == "partial_stage") {
                const auto iteration = partial_iterations.fetch_add(1);
                std::size_t descriptor_count = 2;
                switch (fixture) {
                    case PartialCoverageFixture::complete:
                        break;
                    case PartialCoverageFixture::missing_final_iteration:
                        descriptor_count = iteration < 3 ? 2 : 0;
                        break;
                    case PartialCoverageFixture::wrong_raw_sample_multiplicity:
                        descriptor_count = iteration == 2 ? 3 : 2;
                        break;
                    case PartialCoverageFixture::laundered_raw_sample_count:
                        descriptor_count = iteration < 2
                            ? 2
                            : (iteration == 2 ? 4 : 0);
                        break;
                    case PartialCoverageFixture::redistributed_one_one_one_five:
                        descriptor_count = iteration < 3 ? 1 : 5;
                        break;
                    case PartialCoverageFixture::redistributed_one_two_two_three:
                        descriptor_count = iteration == 0
                            ? 1
                            : (iteration == 3 ? 3 : 2);
                        break;
                }
                const std::array descriptors{
                    partial_a, partial_b, partial_c, partial_d, partial_e};
                for (std::size_t descriptor = 0;
                     descriptor < descriptor_count; ++descriptor) {
                    auto scope = bioseq::instr::region(
                        descriptors[descriptor]);
                }
            } else if (algorithm == "full_stage") {
                for (std::size_t visit = 0; visit < 10; ++visit) {
                    auto scope = bioseq::instr::region(full);
                }
            }
            return ToyOutput{};
        };
        const auto on_evaluate = [](std::string_view, const ParamMap&,
                                    const ToyInput&, const ToyOutput&) {
            return std::map<std::string, double>{};
        };
        return run_one_bench(
            "bench_region_provenance", "named region provenance fixture",
            active_config, report_dir, build_toy_input, on_run, on_evaluate);
    };
    const auto run_fixture = [&config_text, &run_fixture_with_config](
            PartialCoverageFixture fixture) {
        return run_fixture_with_config(config_text, fixture);
    };

    SECTION("duplicate descriptors present in all four iterations are complete") {
        const auto outcome = run_fixture(PartialCoverageFixture::complete);
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
            PartialCoverageFixture::missing_final_iteration);
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
            PartialCoverageFixture::wrong_raw_sample_multiplicity);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr,
            "acceptance_named_region_multiplicity_mismatch"));
        // Provenance is 4/4, but 2 + 2 + 3 + 2 is not report_76's exact
        // scalar-sketch multiplicity of eight.
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
            PartialCoverageFixture::laundered_raw_sample_count);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE_FALSE(stderr_contains(
            outcome.captured_stderr,
            "acceptance_named_region_multiplicity_mismatch"));
        REQUIRE(stderr_contains(
            outcome.captured_stderr,
            "acceptance_named_region_iteration_multiplicity_mismatch"));
        // 2 + 2 + 4 + 0 has the expected report_76 raw multiplicity, while
        // provenance still correctly observes only three iterations.
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
        [&run_fixture](PartialCoverageFixture fixture) {
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
            // Both sides contributed in every iteration and the numerical
            // rule itself passes. Only the per-iteration topology gate may
            // reject this otherwise-laundered report.
            REQUIRE(outcome.report.at("rule_violations").empty());
        };

    SECTION("one plus one plus one plus five cannot launder two-per-iteration") {
        require_redistributed_multiplicity_failure(
            PartialCoverageFixture::redistributed_one_one_one_five);
    }

    SECTION("one plus two plus two plus three cannot launder two-per-iteration") {
        require_redistributed_multiplicity_failure(
            PartialCoverageFixture::redistributed_one_two_two_three);
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
            invalid_config_text, PartialCoverageFixture::complete);
        REQUIRE(outcome.exit_code == 1);
        REQUIRE(stderr_contains(
            outcome.captured_stderr,
            "acceptance_named_region_multiplicity_not_uniform"));
    }
}

// ---------------------------------------------------------------------------
//  DOC0349 Test 4 — `--debug` requires a non-empty `--filter`
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework --debug requires --filter", "[bench_framework][debug]") {
    TestLogSilencer silence_mlogging;
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"algo_a", std::span<const AlgoParamSpec>{kFilterParams}}
    }};
    static constexpr std::array<ForEntry, 1> kFor{{ {"SMALL", 2} }};
    static constexpr std::array<OverEntry, 1> kOver{{ {"algo_a", "{}"} }};
    static constexpr std::array<std::string_view, 1> kTags{{"aligner"}};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{.key = "bench_a", .description = "a",
                      .tags = std::span<const std::string_view>{kTags},
                      .for_entries = std::span<const ForEntry>{kFor},
                      .over_entries = std::span<const OverEntry>{kOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}}
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kFilterTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    const std::array<std::string, 1> extra{"--debug"};   // no --filter
    auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                 make_toy_bench("bench_a", "a", config_path));
    REQUIRE(outcome.exit_code == 1);
    REQUIRE(stderr_contains(outcome.captured_stderr, "debug_requires_filter"));
    // No debug stream produced when the launch check fails.
    REQUIRE_FALSE(find_debug_jsonl(report_dir.path).has_value());
}

// ---------------------------------------------------------------------------
//  DOC0349 Test 5 — `--debug` JSONL event stream (4 event kinds + structure)
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework --debug JSONL stream", "[bench_framework][debug]") {
    TestLogSilencer silence_mlogging;
    static constexpr std::array<AlgorithmSpec, 2> kAlgos{{
        {"algo_x", std::span<const AlgoParamSpec>{kFilterParams}},
        {"algo_y", std::span<const AlgoParamSpec>{kFilterParams}},
    }};
    static constexpr std::array<ForEntry, 1> kFor{{ {"SMALL", 3} }};
    static constexpr std::array<OverEntry, 2> kOver{{ {"algo_x", "{}"}, {"algo_y", "{}"} }};
    static constexpr std::array<std::string_view, 1> kTags{{"unit"}};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{.key = "bench_dbg", .description = "debug",
                      .tags = std::span<const std::string_view>{kTags},
                      .for_entries = std::span<const ForEntry>{kFor},
                      .over_entries = std::span<const OverEntry>{kOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}}
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kFilterTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    const std::array<std::string, 3> extra{"--filter", "benchmark=bench_dbg", "--debug"};
    auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                 make_toy_bench("bench_dbg", "debug", config_path));
    REQUIRE(outcome.exit_code == 0);

    const auto jsonl = find_debug_jsonl(report_dir.path);
    REQUIRE(jsonl.has_value());
    const auto events = read_jsonl(*jsonl);
    REQUIRE_FALSE(events.empty());

    // One BENCHMARK_STARTED, one CASE_STARTED + one CASE_RESULTS per algo,
    // and exactly one trailing EXECUTION_FINISHED.
    REQUIRE(count_events(events, "BENCHMARK_STARTED") == 1);
    REQUIRE(count_events(events, "CASE_STARTED") == 2);
    REQUIRE(count_events(events, "CASE_RESULTS") == 2);
    REQUIRE(count_events(events, "EXECUTION_FINISHED") == 1);
    REQUIRE(case_results_names(events) == std::set<std::string>{"algo_x", "algo_y"});

    // EXECUTION_FINISHED is the last line and its results_path == the report.
    REQUIRE(events.back().value("event", std::string{}) == "EXECUTION_FINISHED");
    REQUIRE(events.back().value("results_path", std::string{}) == outcome.report_path.string());

    // Each CASE_RESULTS carries the execution_tuple_record shape and matches the
    // report's slice for that algorithm (same tuple/test_cases structure).
    for (const auto& e : events) {
        if (e.value("event", std::string{}) != "CASE_RESULTS") continue;
        REQUIRE(e.contains("timestamp"));
        REQUIRE(e["timestamp"].is_number_integer());
        REQUIRE(e.contains("results"));
        REQUIRE(e["results"].contains("execution_tuples"));
        const auto& tuples = e["results"]["execution_tuples"];
        REQUIRE(tuples.is_array());
        REQUIRE(tuples.size() == 1);                       // one tuple per single-param algo
        REQUIRE(tuples[0].contains("tuple"));
        REQUIRE(tuples[0].contains("test_cases"));
        const std::string algo = e.value("case_name", std::string{});
        REQUIRE(tuples[0]["tuple"].contains(algo));        // keyed on the case name
        REQUIRE(tuples[0]["test_cases"].contains("SMALL"));
        // The aggregated samples_count equals the report's (3 iterations).
        REQUIRE(samples_count_at(outcome.report, "bench_dbg", algo, "SMALL",
                                 "all_regions", "sum_value") == 3);
    }
}

// ---------------------------------------------------------------------------
//  DOC0349 Test 6 — crash resilience: every line is independently flushed
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework --debug lines are flushed per event", "[bench_framework][debug]") {
    TestLogSilencer silence_mlogging;
    // A later case reads the jsonl mid-run and asserts an earlier case's
    // CASE_RESULTS is already on disk (crash-resilient incremental flush).
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"algo_a", std::span<const AlgoParamSpec>{kFilterParams}}
    }};
    static constexpr std::array<ForEntry, 1> kFor{{ {"SMALL", 2} }};
    static constexpr std::array<OverEntry, 1> kOver{{ {"algo_a", "{}"} }};
    static constexpr std::array<std::string_view, 1> kTags{{"unit"}};
    const std::array<BenchmarkSpec, 1> benches{{
        BenchmarkSpec{.key = "bench_flush", .description = "flush",
                      .tags = std::span<const std::string_view>{kTags},
                      .for_entries = std::span<const ForEntry>{kFor},
                      .over_entries = std::span<const OverEntry>{kOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}}
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kFilterTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;
    const auto config_path = write_config_file(report_dir, config_text);
    const std::array<std::string, 3> extra{"--filter", "benchmark=bench_flush", "--debug"};
    auto outcome = run_with_args(report_dir, std::span<const std::string>{extra},
                                 make_toy_bench("bench_flush", "flush", config_path));
    REQUIRE(outcome.exit_code == 0);

    // Post-run: each line parses independently as one JSON object (proves the
    // writer emits one flushed object per line — the crash-resilience contract).
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

// ---------------------------------------------------------------------------
//  DOC0349 Test 7 — abort backstop / end-of-benchmark sweep
// ---------------------------------------------------------------------------

TEST_CASE("bench_framework --debug abort backstop sweep", "[bench_framework][debug]") {
    TestLogSilencer silence_mlogging;

    // `bench_partial`: algo over {SMALL, BIG}. BIG's batch is skipped
    // (memory_budget_not_satisfiable), SMALL completes. The case counter never
    // reaches 0 → the end-of-benchmark sweep flushes its CASE_RESULTS exactly
    // once, carrying only SMALL's samples. `bench_allskip`: algo over {BIG}
    // only → every batch skipped → no CASE_STARTED/CASE_RESULTS at all.
    static constexpr std::array<AlgorithmSpec, 1> kAlgos{{
        {"algo_a", std::span<const AlgoParamSpec>{kFilterParams}}
    }};
    static constexpr std::array<ForEntry, 2> kForPartial{{ {"SMALL", 2}, {"BIG", 2} }};
    static constexpr std::array<ForEntry, 1> kForSkip{{ {"BIG", 2} }};
    static constexpr std::array<OverEntry, 1> kOver{{ {"algo_a", "{}"} }};
    static constexpr std::array<std::string_view, 1> kTags{{"unit"}};
    const std::array<BenchmarkSpec, 2> benches{{
        BenchmarkSpec{.key = "bench_partial", .description = "partial",
                      .tags = std::span<const std::string_view>{kTags},
                      .for_entries = std::span<const ForEntry>{kForPartial},
                      .over_entries = std::span<const OverEntry>{kOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}},
        BenchmarkSpec{.key = "bench_allskip", .description = "allskip",
                      .tags = std::span<const std::string_view>{kTags},
                      .for_entries = std::span<const ForEntry>{kForSkip},
                      .over_entries = std::span<const OverEntry>{kOver},
                      .metrics = std::span<const MetricSpecLite>{kFilterMetrics}},
    }};
    const auto config_text = render_config(
        std::span<const AlgorithmSpec>{kAlgos},
        std::span<const TestCaseDataSpec>{kFilterTestCases},
        std::span<const BenchmarkSpec>{benches});

    TempReportDir report_dir;
    const auto config_path = write_config_file(report_dir, config_text);

    // on_estimate: BIG demands more RAM than any host has → forces
    // memory_budget_not_satisfiable (skip at enqueue, no item loop).
    Estimator big_oom = [](std::string_view, std::string_view test_case_name,
                           simdjson::dom::element) noexcept {
        RunConstraints rc;
        if (test_case_name == "BIG") rc.base_memory = std::size_t{1} << 60;  // ~1 EiB
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

    // The run still reaches EXECUTION_FINISHED (counter-stuck hangs nothing).
    REQUIRE(count_events(events, "EXECUTION_FINISHED") == 1);

    // bench_partial/algo_a: started once (SMALL ran) + flushed once by the
    // sweep. bench_allskip/algo_a: never started → never emitted. So across the
    // run there is exactly ONE CASE_STARTED and ONE CASE_RESULTS, no duplicates.
    REQUIRE(count_events(events, "CASE_STARTED") == 1);
    REQUIRE(count_events(events, "CASE_RESULTS") == 1);
    REQUIRE(case_results_names(events) == std::set<std::string>{"algo_a"});

    // The single CASE_RESULTS carries only SMALL's samples (BIG never ran);
    // SMALL had 2 iterations.
    for (const auto& e : events) {
        if (e.value("event", std::string{}) != "CASE_RESULTS") continue;
        const auto& tuples = e["results"]["execution_tuples"];
        REQUIRE(tuples.size() == 1);
        REQUIRE(tuples[0]["test_cases"].contains("SMALL"));
        REQUIRE_FALSE(tuples[0]["test_cases"].contains("BIG"));
    }
}
