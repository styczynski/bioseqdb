/**
 * @file json_writer.cppm
 * @brief Forward-streaming JSON writer backed by simdjson::builder.
 *
 * The bench's plotly report is an append-only document: tens of charts,
 * each ~hundreds-of-traces × hundreds-of-points; no random access. The
 * previous incarnation hand-rolled string formatting (escaping, number
 * printing, comma insertion) inside this same wrapper. The wrapper API
 * is unchanged — `kv_string`, `begin_object`, `kv_double_array`, etc.
 * stay byte-identical from the caller's perspective — but every primitive
 * now delegates to `simdjson::builder::string_builder`, which:
 *
 *   * uses SIMD-vectorised escape paths for strings,
 *   * formats floating-point with the shortest-roundtrip algorithm
 *     (slightly different decimal expansions vs. the old `%.17g`, but
 *     IEEE-754 round-trip stable),
 *   * keeps a single growable buffer rather than a `std::string` that
 *     reallocs ~10× during a full report.
 *
 * Comma management stays in this wrapper. simdjson's `string_builder`
 * deliberately leaves comma placement to the caller (see `SIMDJSON_REF.md`
 * §"Overview: string_builder"); the project's chart_builder code is
 * already organised around "the writer knows where the commas go", and
 * pushing comma calls into ~700 chart-builder call sites would be a
 * far bigger and riskier change than keeping a 30-line `maybe_comma`
 * helper.
 *
 * Plotly's parser tolerates `null` in numeric arrays as a missing-value
 * marker, so we coerce non-finite doubles (NaN / +inf / -inf) to JSON
 * `null` rather than emitting raw `nan` (which would fail validation
 * downstream).
 */
module;

#include <simdjson.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

export module bench.scheduler.json_writer;

export namespace bioseq::bench_scheduler {

/**
 * @brief Append-only JSON serializer with implicit comma management.
 *
 * Mirrors the pre-refactor `bioseq::bench::JsonWriter` API one-for-one
 * so the chart builder's ~700 call sites compile unchanged after the
 * `import` line is updated.
 *
 * Thread-safety: not thread-safe. The bench writes its report serially
 * after the parallel matrix-runner has finished joining workers, so
 * one process-wide writer instance is enough.
 */
class JsonWriter {
public:
    /// Pre-reserve ~1 MiB. The full sweep emits a multi-megabyte
    /// document (tens of charts × hundreds of traces × hundreds of
    /// points each). `string_builder` doubles when it has to grow, so
    /// one up-front reserve cuts ~3-4 reallocations during a single
    /// `emit_report()` call.
    JsonWriter() : sb_{kInitialCapacityBytes} {
        stack_.reserve(32);  // typical max nesting in chart JSON is ~10
    }

    void begin_object() {
        maybe_comma();
        sb_.start_object();
        stack_.push_back(false);
    }
    void end_object() {
        sb_.end_object();
        if (!stack_.empty()) stack_.pop_back();
        if (!stack_.empty()) stack_.back() = true;
    }
    void begin_array() {
        maybe_comma();
        sb_.start_array();
        stack_.push_back(false);
    }
    void end_array() {
        sb_.end_array();
        if (!stack_.empty()) stack_.pop_back();
        if (!stack_.empty()) stack_.back() = true;
    }

    /// Emit `"key":` and prime the next call to suppress its leading
    /// comma — the next emission is the *value* for this key, not a
    /// sibling at this level.
    void key(std::string_view k) {
        maybe_comma();
        sb_.escape_and_append_with_quotes(k);
        sb_.append_colon();
        suppress_next_comma_ = true;
    }

    void value_string(std::string_view v) {
        maybe_comma();
        sb_.escape_and_append_with_quotes(v);
    }
    void value_int(std::int64_t v) {
        maybe_comma();
        sb_.append(v);
    }
    void value_uint(std::uint64_t v) {
        maybe_comma();
        sb_.append(v);
    }
    void value_bool(bool v) {
        maybe_comma();
        // simdjson's `append(arithmetic)` formats bool as "false"/"true"
        // per JSON convention. Static-cast keeps it on the bool overload
        // (the integer overload would emit 0/1).
        sb_.append(v);
    }
    void value_null() {
        maybe_comma();
        sb_.append_null();
    }
    void value_double(double v) {
        maybe_comma();
        if (!std::isfinite(v)) {
            // Plotly tolerates `null` in numeric arrays. Emitting raw
            // `nan` would fail downstream validation — the C
            // `nlohmann/json` and Python `plotly.io.from_json` both
            // reject non-JSON-standard literals.
            sb_.append_null();
            return;
        }
        sb_.append(v);
    }

    /// Convenience: key + scalar in a single call. The chart builder's
    /// emitted code reads like a struct literal rather than a tower of
    /// paired calls.
    void kv_string(std::string_view k, std::string_view v) { key(k); value_string(v); }
    void kv_int(std::string_view k, std::int64_t v)        { key(k); value_int(v); }
    void kv_uint(std::string_view k, std::uint64_t v)      { key(k); value_uint(v); }
    void kv_bool(std::string_view k, bool v)               { key(k); value_bool(v); }
    void kv_double(std::string_view k, double v)           { key(k); value_double(v); }
    void kv_null(std::string_view k)                       { key(k); value_null(); }

    /// Emit `"key": [v0, v1, ...]` directly from a numeric vector.
    /// Saves the caller having to write begin_array / end_array around
    /// every per-trace coordinate column. Templated so the same helper
    /// works for std::size_t, double, int, etc. without explicit casts
    /// at the call site.
    template<typename T>
    void kv_double_array(std::string_view k, const std::vector<T>& xs) {
        key(k);
        begin_array();
        for (const auto& x : xs) value_double(static_cast<double>(x));
        end_array();
    }
    template<typename T>
    void kv_int_array(std::string_view k, const std::vector<T>& xs) {
        key(k);
        begin_array();
        for (const auto& x : xs) value_int(static_cast<std::int64_t>(x));
        end_array();
    }
    void kv_string_array(std::string_view k, const std::vector<std::string>& xs) {
        key(k);
        begin_array();
        for (const auto& x : xs) value_string(x);
        end_array();
    }

    /// View into the buffered JSON. Returns an empty view if the
    /// builder reported an allocation failure mid-write — the caller
    /// is expected to log and bail. We do not call `validate_unicode()`
    /// because every string we emit is generated by the bench harness
    /// from controlled sources (algorithm labels, bucket names) — none
    /// of it is operator-supplied UTF-8.
    [[nodiscard]] std::string_view view() {
        std::string_view out;
        const auto err = sb_.view().get(out);
        if (err != simdjson::SUCCESS) return std::string_view{};
        return out;
    }

    /// Spill the buffer to disk, replacing any prior contents at `path`.
    /// Returns false on builder error or I/O failure. The bench main
    /// calls this once at shutdown — we deliberately do not expose a
    /// streaming interface because the report buffer fits comfortably
    /// in memory (a few MB even for the full 14 × 4 × 3 sweep).
    [[nodiscard]] bool write_to_file(const std::string& path) {
        const std::string_view payload = view();
        if (payload.data() == nullptr) return false;
        std::ofstream file(path, std::ios::trunc | std::ios::binary);
        if (!file) return false;
        file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        return file.good();
    }

private:
    /// Insert a comma between siblings at the current nesting level.
    /// `stack_.back()` is `false` when no value has been emitted yet
    /// at this level, `true` once at least one has — the very first
    /// value at any new {} or [] therefore gets no leading comma.
    /// `suppress_next_comma_` short-circuits the comma after a `key()`
    /// call: the value about to be emitted is the value for that key,
    /// not a fresh sibling.
    void maybe_comma() {
        if (suppress_next_comma_) {
            suppress_next_comma_ = false;
            return;
        }
        if (stack_.empty()) return;
        if (stack_.back()) {
            sb_.append_comma();
        } else {
            stack_.back() = true;
        }
    }

    /// 1 MiB up-front. The full sweep emits ~5-7 MiB; one reserve
    /// avoids ~3-4 reallocations during a write.
    static constexpr std::size_t kInitialCapacityBytes = 1u << 20;

    simdjson::builder::string_builder sb_;
    /// One entry per open object/array on the writer's nesting stack.
    /// `false` until the first child is written; flipped to `true`
    /// after, which causes the next sibling to get a leading comma.
    std::vector<bool> stack_;
    /// Set after `key()` so the immediately-following value emission
    /// does not insert a comma between the colon and the value.
    bool suppress_next_comma_ = false;
};

}  // namespace bioseq::bench_scheduler
