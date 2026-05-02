/**
 * @file harness.cppm
 * @brief One-shot post-session pipeline shared across bench harnesses.
 *
 * Every bench in the project runs the same shutdown sequence after
 * `Catch::Session::run()` returns:
 *
 *   1. Stop the live FTXUI dashboard so subsequent stderr lines
 *      land in the operator's scrollback rather than under the next
 *      dashboard frame.
 *   2. Compute the next free `report_build_<N>.json` path under
 *      `report_dir`, stamp the document with the current git SHA.
 *   3. Build the report into a fresh `JsonWriter` via the harness's
 *      domain-specific chart-emit callback.
 *   4. Spill to disk; log success/failure.
 *   5. Print the regression-violations table on stderr.
 *   6. Convert regression failures into a non-zero exit code unless
 *      Catch2 already returned non-zero (preserve its dedicated
 *      codes for compilation issues, command-line errors, etc.).
 *
 * `finalize_bench_run` packages all six steps. The aligner's
 * `bench_main.cpp` shrinks from ~120 lines of post-session code to
 * ~10: it only computes domain-specific violations and supplies a
 * chart-emit lambda.
 */
module;

#include "common.h"

#include <concepts>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

export module bench.scheduler.harness;

import mlogging;
import bench.scheduler.json_writer;
import bench.scheduler.progress_ui;
import bench.scheduler.regression;
import bench.scheduler.report_envelope;
import bench.scheduler.report_files;

export {

namespace bioseq::bench_scheduler {

/**
 * @brief Path-and-identity inputs to `finalize_bench_run`.
 *
 * All fields are non-owning views: the caller (typically `main()`)
 * is responsible for keeping the underlying storage alive across the
 * call. In practice every value is a `BIOSEQ_BENCH_*` build-time
 * macro that lives in `.rodata`, so lifetime is the binary's, but
 * the contract is documented honestly.
 */
struct BenchHarnessOptions {
    /// Repository root passed to `git -C <root> rev-parse HEAD`. Used
    /// to stamp the report's `commit_hash` field.
    std::string_view repo_root;

    /// Directory where numbered `report_build_<N>.json` files live.
    /// Created on demand if missing.
    std::filesystem::path report_dir;

    /// Optional human-readable log prefix for the
    /// "wrote report to ..." / "FAILED to write report" lines. Pass
    /// the bench's name; defaults to `"bench.scheduler.harness"`.
    std::string_view log_tag = "bench.scheduler.harness";
};

/**
 * @brief Stop the dashboard, write the report, print the violations
 *        table, return the exit code.
 *
 * `emit_report_to_writer` is the harness's domain-specific chart
 * emitter. The aligner passes a lambda that calls
 * `bioseq::bench::emit_report(writer, violations, commit_hash)`; an
 * IO bench would pass its own. Same shape: writer + violations +
 * commit hash in, populated writer out.
 *
 * `catch2_exit_code` is the value `Catch::Session::run()` returned.
 * The function preserves Catch2's non-zero codes (compile errors,
 * command-line errors) but folds regression-rule failures into exit
 * code 2 when Catch2 itself succeeded.
 */
template<std::invocable<JsonWriter&, std::span<const regression::Violation>,
                       std::string_view> EmitReportFn>
[[nodiscard]] inline int finalize_bench_run(
    int catch2_exit_code,
    const BenchHarnessOptions& opts,
    std::span<const regression::Violation> violations,
    EmitReportFn&& emit_report_to_writer)
{
    // Step 1 — quiesce the dashboard.
    progress::dashboard().stop();

    // Step 2 — pick the next numbered report path + stamp commit.
    const std::string report_path = next_numbered_report_path(opts.report_dir);
    const std::string commit_hash = detect_git_commit_hash(opts.repo_root);

    // Step 3 + 4 — build and persist the report.
    JsonWriter writer;
    emit_report_to_writer(writer, violations, std::string_view{commit_hash});
    if (!write_report_to_file(writer, report_path, opts.log_tag)) {
        loge("[{}] failed to write report to {}", opts.log_tag, report_path);
    } else {
        loge("[{}] wrote report to {} (commit {})",
             opts.log_tag, report_path, commit_hash);
    }

    // Step 5 — operator-facing violations table.
    regression::print_violations_table(violations);

    // Step 6 — exit-code translation. Regression failures fold into
    // exit code 2 only when Catch2 itself succeeded; any non-zero
    // Catch2 return (command-line error, compile issue) wins.
    if (!violations.empty() && catch2_exit_code == 0) {
        return 2;
    }
    return catch2_exit_code;
}

}  // namespace bioseq::bench_scheduler

}  // export
