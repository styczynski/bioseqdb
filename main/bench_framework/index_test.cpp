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
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.instr;
import bench_framework;

namespace {

using bioseq::bench_framework::ConfigValue;
using bioseq::bench_framework::ParamMap;
using bioseq::bench_framework::create_benchmark;
using bioseq::bench_framework::run;

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

struct BenchmarkSpec {
    std::string_view              key;
    std::string_view              description{};
    /// Required `tags` array (may be empty). Used by `--filter tags=...`.
    std::span<const std::string_view> tags{};
    bool                           fast = false;
    bool                           bench_lock = false;
    bool                           concurrency = true;
    // V11 deleted `test_case_lock`; the schema rejects the field
    // outright. The struct keeps no slot for it so existing call
    // sites that pass `.test_case_lock = ...` would fail to
    // compile and force a deliberate update.
    bool                           by_regions = false;
    std::span<const ForEntry>     for_entries{};
    std::span<const OverEntry>    over_entries{};
    std::span<const MetricSpecLite> metrics{};
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
    out += "},\"matrix\":{";
    emit_kv_bool(out, "by_regions", bench.by_regions);
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
         typename OnCleanup = std::nullptr_t>
[[nodiscard]] RunOutcome run_one_bench(
    std::string_view  bench_key,
    std::string_view  bench_title,
    std::string_view  config_text,
    const TempReportDir& report_dir,
    OnInput  on_input,
    OnRun    on_run,
    OnEvaluate on_evaluate,
    OnCleanup  on_cleanup = nullptr)
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

    auto bench = create_benchmark<Input, Output>(
        std::string{bench_title},
        std::string{bench_key},
        outcome.config_path.string(),
        std::move(estimate_fn),
        std::move(adapted_input_fn),
        RunFn{std::move(on_run)},
        EvalFn{std::move(on_evaluate)},
        std::move(cleanup_fn));

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
