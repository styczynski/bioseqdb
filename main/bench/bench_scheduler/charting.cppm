/**
 * @file charting.cppm
 * @brief Generic plotly chart-emit helpers for any bench harness.
 *
 * Every plotly-style report the project produces shares the same
 * structural envelope:
 *
 *   { "<chart_id>": {
 *       "short_description": "...",
 *       "long_description":  "...",
 *       "plotly": { "data": [...], "layout": { ... } } } }
 *
 * and the same `layout` block fields (`title`, `xaxis`, `yaxis`,
 * `legend`, `template`). This module owns those scaffold helpers so
 * every chart body can be a tight local lambda over per-trace data,
 * not 30 lines of writer-key boilerplate.
 *
 * Domain code (the aligner bench's per-chart data layout, the
 * hypothetical IO bench's reader-vs-throughput chart) supplies:
 *   - the chart id and descriptions,
 *   - the `data` array body (per-trace `type`, `x`, `y`, `mode`,
 *     `name`, etc.),
 *   - the layout titles + log-scale flags,
 *   - the marker colour resolved from a domain key
 *     (`colour_for_key("bioseq KSW2 + ...")`).
 *
 * Nothing here references aligner concepts (BenchSample, length
 * buckets, algorithm enums); this module is `bioseq::bench_scheduler`
 * and intentionally cannot import any `aligners.*` module.
 */
module;

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module bench.scheduler.charting;

import bench.scheduler.json_writer;

export {

namespace bioseq::bench_scheduler::charting {

/**
 * @brief Wrap a chart body in the standard `{short_description,
 *        long_description, plotly: {data, layout}}` shell.
 *
 * The body callback is invoked between the plotly object's `{` and
 * `}` and **must emit both** the `data` array AND the `layout`
 * object — `plotly.io.from_json` parses `{"data": [...]}`-only as
 * valid JSON but renders an empty / default-axis chart, so a
 * forgotten `layout` is not loud at parse time. The chart bodies
 * in `bench/aligners/chart_builder.cppm` always pair the two; new
 * chart types should follow the same pattern. Use
 * `emit_plotly_layout` or `emit_plotly_layout_titled` for the
 * `layout` emission so `template`/`legend`/etc. defaults stay
 * consistent across the report.
 *
 * The two-level indirection (chart object → plotly object → data +
 * layout) is fixed by what `plotly.io.from_json` expects, so
 * burying it once here lets every chart in the report share a
 * single implementation of "did we get the wrapping right?".
 *
 * Templated on the body so the call site stays as cheap as a
 * lambda capture; no `std::function` indirection.
 */
template<std::invocable Body>
inline void emit_chart_envelope(
    JsonWriter& writer,
    std::string_view chart_id,
    std::string_view short_description,
    std::string_view long_description,
    Body&& body)
{
    writer.key(chart_id);
    writer.begin_object();
    writer.kv_string("short_description", short_description);
    writer.kv_string("long_description",  long_description);
    writer.key("plotly");
    writer.begin_object();
    body();  // emits "data" and "layout" members of the plotly figure
    writer.end_object();
    writer.end_object();
}

/**
 * @brief Emit the standard `layout` block.
 *
 * Captures the conventions every chart in the project shares: title +
 * axis titles, optional log-scale on either axis, vertical legend,
 * `plotly_white` template. `barmode` is opt-in (empty string means
 * "do not emit the field"; plotly defaults to grouped bars there).
 */
/// Emit a minimal `layout` block carrying only a title and the
/// shared `template = "plotly_white"`. Used by `type=table` traces
/// (and other plotly trace types where x/y axes don't apply) so
/// non-axis charts still go through a scheduler primitive instead
/// of writing the layout-key dance by hand.
inline void emit_plotly_layout_titled(
    JsonWriter& writer,
    std::string_view title)
{
    writer.key("layout");
    writer.begin_object();
    writer.key("title");
    writer.begin_object();
    writer.kv_string("text", title);
    writer.end_object();
    writer.kv_string("template", "plotly_white");
    writer.end_object();
}

inline void emit_plotly_layout(
    JsonWriter& writer,
    std::string_view title,
    std::string_view x_title, bool x_log,
    std::string_view y_title, bool y_log,
    std::string_view barmode = "")
{
    writer.key("layout");
    writer.begin_object();
    writer.key("title");
    writer.begin_object();
    writer.kv_string("text", title);
    writer.end_object();
    writer.key("xaxis");
    writer.begin_object();
    writer.key("title");
    writer.begin_object();
    writer.kv_string("text", x_title);
    writer.end_object();
    if (x_log) writer.kv_string("type", "log");
    writer.end_object();
    writer.key("yaxis");
    writer.begin_object();
    writer.key("title");
    writer.begin_object();
    writer.kv_string("text", y_title);
    writer.end_object();
    if (y_log) writer.kv_string("type", "log");
    writer.end_object();
    writer.key("legend");
    writer.begin_object();
    writer.kv_string("orientation", "v");
    writer.end_object();
    writer.kv_bool("showlegend", true);
    if (!barmode.empty()) writer.kv_string("barmode", barmode);
    writer.kv_string("template", "plotly_white");
    writer.end_object();
}

/**
 * @brief Emit a `marker` + `line` pair for a trace.
 *
 * Both members are stamped with the same `color` so a scatter trace
 * has matching dot + connecting-line colours regardless of which
 * trace `mode` plotly picks. The colour is the caller's domain
 * choice — pull it from `colour_for_key(domain_label)` for
 * deterministic per-key colours, or pass any hex literal.
 */
inline void emit_plotly_marker(
    JsonWriter& writer,
    std::string_view color_hex,
    int marker_size = 7,
    int line_width = 2)
{
    writer.key("marker");
    writer.begin_object();
    writer.kv_string("color", color_hex);
    writer.kv_int("size", marker_size);
    writer.end_object();
    writer.key("line");
    writer.begin_object();
    writer.kv_string("color", color_hex);
    writer.kv_int("width", line_width);
    writer.end_object();
}

/**
 * @brief Map a string key to a stable hex colour from a 16-entry palette.
 *
 * Keeps each domain key's trace colour consistent across charts in
 * the report so the reader can follow the same line across the deck.
 * Uses a deterministic FNV-1a hash; std::hash would be fine for
 * uniqueness but is not specified to be portable across libc++ and
 * libstdc++, and we want the palette index to be the same across
 * builds for diff-friendliness.
 *
 * Returns `std::string` rather than `std::string_view` because the
 * writer's `kv_string` consumes string_view but the palette entries
 * are static literals — both work, but `string` matches what the
 * pre-refactor `colour_for(...)` returned and avoids subtle lifetime
 * surprises if a caller stashes the result.
 *
 * Not `noexcept`: constructing a `std::string` can throw
 * `std::bad_alloc` on OOM. The 7-byte hex literals always hit
 * `std::string`'s SSO buffer in practice, so the throw path is
 * effectively unreachable, but the contract is stated honestly.
 */
[[nodiscard]] inline std::string colour_for_key(std::string_view key) {
    static constexpr std::array<std::string_view, 16> palette = {
        "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728",
        "#9467bd", "#8c564b", "#e377c2", "#7f7f7f",
        "#bcbd22", "#17becf", "#393b79", "#637939",
        "#8c6d31", "#843c39", "#7b4173", "#3182bd"};
    // FNV-1a — small, fast, deterministic across compilers.
    std::size_t hash_val = 1469598103934665603ull;
    for (char c : key) {
        hash_val ^= static_cast<std::size_t>(static_cast<unsigned char>(c));
        hash_val *= 1099511628211ull;
    }
    return std::string{palette[hash_val % palette.size()]};
}

/**
 * @brief Pin a list of labels to the front of `labels` while keeping
 *        every other label in its original order.
 *
 * The aligner bench uses this to put `"minimap2 (DP chaining)"` and
 * `"minimap2 (RMQ chaining)"` ahead of the bioseq variants on every
 * chart legend. An IO bench could pin a `"kseqpp baseline"` label
 * the same way, no engine code changes required.
 *
 * In-place rotation rather than a fresh allocation; the alignment
 * grid has at most 30-ish labels and we want this to be cheap to
 * call from every chart body.
 */
inline void pin_labels_first(
    std::vector<std::string>& labels,
    std::span<const std::string_view> pinned)
{
    std::size_t insert_position = 0;
    for (const auto pin : pinned) {
        for (std::size_t i = insert_position; i < labels.size(); ++i) {
            if (labels[i] == pin) {
                if (i != insert_position) {
                    std::string moved = std::move(labels[i]);
                    labels.erase(labels.begin() + static_cast<std::ptrdiff_t>(i));
                    labels.insert(labels.begin() + static_cast<std::ptrdiff_t>(insert_position),
                                  std::move(moved));
                }
                ++insert_position;
                break;
            }
        }
    }
}

}  // namespace bioseq::bench_scheduler::charting

}  // export
