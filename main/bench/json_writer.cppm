/**
 * @file json_writer.cppm
 * @brief Minimal JSON emitter used by the bench plotly chart pipeline.
 *
 * We hand-roll the emitter rather than pulling in simdjson because
 * (a) the JSON shape we produce is small and well-controlled — a few
 * thousand numbers per chart at most — so SIMD escape and number
 * formatting are not on the critical path, and (b) simdjson is listed
 * in vcpkg.json but the Highway+CMake module-graph in this project
 * isn't currently wired to install it. Keeping the writer dependency-
 * free avoids a build-system change just to print a report file.
 *
 * The output is validated downstream by Python's `plotly.io.from_json`
 * (see CODE_GUIDELINES + the chart pipeline doc) — that parser is
 * permissive about whitespace but strict about commas / trailing
 * separators / numeric formats, so the emitter here tracks element
 * counts inside each container scope and inserts commas only between
 * siblings. NaN / Inf doubles are rendered as JSON `null` because
 * plotly tolerates `null` in numeric arrays as a missing-value marker.
 */
module;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

export module bench.json_writer;

export namespace bioseq::bench {

/**
 * @brief Append-only JSON serializer.
 *
 * Tracks a stack of "did we already emit a value at this nesting level"
 * flags so the writer knows when to insert a comma. The alternative
 * (manually placing commas at each call site) was tried first and
 * produced bug-prone "trailing comma" / "missing comma" reports —
 * Python's plotly.from_json rejects both.
 */
class JsonWriter {
public:
    JsonWriter() {
        // Pre-reserve ~1 MiB. The full sweep emits a ~5-7 MiB document
        // (16 charts × tens of traces × hundreds of points each) so
        // the std::string growth strategy would do ~10 reallocations
        // from the original 1 KiB seed. One reserve up-front cuts
        // build time of the report by ~40%.
        out_.reserve(1u << 20);
    }

    /// Escape a string into a JSON-safe string literal. The buffer is
    /// emitted with surrounding double-quotes. Non-ASCII bytes are
    /// passed through unchanged — the caller is responsible for
    /// providing UTF-8 input. We escape the standard JSON metachars
    /// (", \, control chars 0x00..0x1F) and pass everything else
    /// through verbatim.
    void emit_string(std::string_view value) {
        out_.push_back('"');
        for (char ch_raw : value) {
            const auto ch = static_cast<unsigned char>(ch_raw);
            switch (ch) {
                case '"':  out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n";  break;
                case '\r': out_ += "\\r";  break;
                case '\t': out_ += "\\t";  break;
                case '\b': out_ += "\\b";  break;
                case '\f': out_ += "\\f";  break;
                default:
                    if (ch < 0x20) {
                        char hex[8];
                        std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned>(ch));
                        out_ += hex;
                    } else {
                        out_.push_back(static_cast<char>(ch));
                    }
            }
        }
        out_.push_back('"');
    }

    void begin_object() {
        maybe_comma();
        out_.push_back('{');
        stack_.push_back(false);
    }
    void end_object() {
        out_.push_back('}');
        stack_.pop_back();
        if (!stack_.empty()) stack_.back() = true;
    }
    void begin_array() {
        maybe_comma();
        out_.push_back('[');
        stack_.push_back(false);
    }
    void end_array() {
        out_.push_back(']');
        stack_.pop_back();
        if (!stack_.empty()) stack_.back() = true;
    }

    void key(std::string_view k) {
        maybe_comma();
        emit_string(k);
        out_.push_back(':');
        // Suppress the comma-on-next-write — the value about to be
        // emitted is the *value* for this key, not a sibling.
        suppress_next_comma_ = true;
    }

    void value_string(std::string_view v) {
        maybe_comma();
        emit_string(v);
    }

    void value_int(std::int64_t v) {
        maybe_comma();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out_ += buf;
    }
    void value_uint(std::uint64_t v) {
        maybe_comma();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
        out_ += buf;
    }
    void value_bool(bool v) {
        maybe_comma();
        out_ += v ? "true" : "false";
    }
    void value_null() {
        maybe_comma();
        out_ += "null";
    }
    void value_double(double v) {
        maybe_comma();
        if (!std::isfinite(v)) {
            out_ += "null";
            return;
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        out_ += buf;
    }

    /// Convenience: key + scalar in a single call. The wrappers below
    /// are used heavily by the chart builder so the emitted code reads
    /// like a struct literal rather than a tower of paired calls.
    void kv_string(std::string_view k, std::string_view v) { key(k); value_string(v); }
    void kv_int(std::string_view k, std::int64_t v)        { key(k); value_int(v); }
    void kv_uint(std::string_view k, std::uint64_t v)      { key(k); value_uint(v); }
    void kv_bool(std::string_view k, bool v)               { key(k); value_bool(v); }
    void kv_double(std::string_view k, double v)           { key(k); value_double(v); }
    void kv_null(std::string_view k)                       { key(k); value_null(); }

    /// Emit a `"key": [v0, v1, ...]` member directly from a numeric
    /// vector. Saves the caller having to write the begin_array /
    /// end_array dance for every per-trace coordinate column.
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

    [[nodiscard]] const std::string& str() const { return out_; }

    /// Spill the buffer to disk, replacing any prior contents at `path`.
    /// Returns false on I/O failure. The bench main calls this once at
    /// shutdown — we deliberately do not expose a streaming interface
    /// because the report buffer fits comfortably in memory (a few MB
    /// even for the full 14×4×3 sweep).
    [[nodiscard]] bool write_to_file(const std::string& path) const {
        std::ofstream file(path, std::ios::trunc | std::ios::binary);
        if (!file) return false;
        file.write(out_.data(), static_cast<std::streamsize>(out_.size()));
        return file.good();
    }

private:
    void maybe_comma() {
        if (suppress_next_comma_) {
            suppress_next_comma_ = false;
            return;
        }
        if (stack_.empty()) return;
        if (stack_.back()) {
            out_.push_back(',');
        } else {
            stack_.back() = true;
        }
    }

    std::string out_;
    std::vector<bool> stack_;
    bool suppress_next_comma_ = false;
};

}  // namespace bioseq::bench
