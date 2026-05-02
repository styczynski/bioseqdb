/**
 * @file report_envelope.cppm
 * @brief Generic top-level scaffolding for bench JSON reports.
 *
 * Owns the root document shape every bench harness in the project
 * shares:
 *
 *   { "schema":          "<harness-specific schema id>",
 *     "commit_hash":     "<git SHA at run time>",
 *     "sample_count":    <int>,
 *     "algorithm_count": <int>,
 *     "violations":      [...],   // from bench.scheduler.regression
 *     "charts":          { ... }  // body callback emits chart entries
 *   }
 *
 * The chart entries are written by a caller-supplied lambda that
 * iterates the harness's chart catalogue, calling
 * `charting::emit_chart_envelope` for each one. The aligner bench's
 * `emit_report` becomes a tight 30-line function over this scaffold;
 * any future harness gets the same root shape for free.
 *
 * Ownership note: `ReportMetadata::violations` is a non-owning span;
 * the caller keeps the underlying vector alive across the
 * `emit_report_envelope` call. That matches how the existing
 * regression-checker output flows (lives in `main()`'s frame for the
 * duration of the document write).
 */
module;

#include "common.h"

#include <concepts>
#include <cstddef>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

export module bench.scheduler.report_envelope;

import mlogging;
import bench.scheduler.json_writer;
import bench.scheduler.regression;

export {

namespace bioseq::bench_scheduler {

/**
 * @brief Header fields the report's root document carries.
 *
 * The aligner bench fills `schema = "bioseq-bench-report-v1"`; the
 * IO bench will fill its own. `commit_hash` is whatever
 * `detect_git_commit_hash()` returned (or "unknown"). The two count
 * fields are advisory metadata for downstream consumers — they are
 * NOT recomputed by the engine, the caller is responsible for the
 * numbers.
 */
struct ReportMetadata {
    std::string_view schema;
    std::string_view commit_hash;
    std::size_t sample_count    = 0;
    std::size_t algorithm_count = 0;
    std::span<const regression::Violation> violations;
};

/**
 * @brief Emit the full root document. The body callback is invoked
 *        between the `charts` object's `{` and `}` and must populate
 *        chart entries via `charting::emit_chart_envelope`.
 *
 * Templated on the body so the call stays as cheap as a captured
 * lambda. The body is parameterless: the writer is expected to live
 * in the body's closure (same shape as `emit_chart_envelope`'s body
 * — keeps the two scaffolds visually consistent at every call site).
 */
template<std::invocable ChartsBody>
inline void emit_report_envelope(
    JsonWriter& writer,
    const ReportMetadata& meta,
    ChartsBody&& charts_body)
{
    writer.begin_object();  // root
    writer.kv_string("schema",          meta.schema);
    writer.kv_string("commit_hash",     meta.commit_hash);
    writer.kv_uint("sample_count",      meta.sample_count);
    writer.kv_uint("algorithm_count",   meta.algorithm_count);
    // Surface violations near the top of the document so a quick
    // `head report.json` shows the regression status before the
    // multi-megabyte chart payload.
    regression::emit_violations_json(writer, meta.violations);
    writer.key("charts");
    writer.begin_object();  // charts
    charts_body();
    writer.end_object();    // charts
    writer.end_object();    // root
}

/**
 * @brief Spill `writer`'s buffer to a file and log the outcome.
 *
 * Returns false if the writer failed mid-write or the file could not
 * be opened/written. The aligner bench logs and continues on
 * failure (the report is the most useful artifact, but a missing one
 * doesn't justify aborting the run); this helper lets callers share
 * the same log prefix without the boilerplate.
 *
 * `tag` lets the caller pick the log prefix — the aligner bench
 * passes `"bench.aligners.chart_builder"`, the IO bench would pass
 * its own. Defaulting prevents accidental empty logs.
 */
[[nodiscard]] inline bool write_report_to_file(
    JsonWriter& writer,
    std::string_view path,
    std::string_view tag = "bench.scheduler.report")
{
    if (!writer.write_to_file(std::string{path})) {
        JustLog("[{}] FAILED to write report to {}", tag, path);
        return false;
    }
    JustLog("[{}] Wrote report ({} bytes) to {}",
            tag, writer.view().size(), path);
    return true;
}

}  // namespace bioseq::bench_scheduler

}  // export
