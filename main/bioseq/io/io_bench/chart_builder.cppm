/**
 * @file chart_builder.cppm
 * @brief Plotly chart catalogue for the IO bench's report.
 *
 * Three charts:
 *
 *   1. `throughput_by_length` — bar chart, x = length_bucket,
 *      y = mean MiB/s, one trace per (reader, format). Headline view
 *      of "how fast is each reader on each corpus shape".
 *   2. `throughput_table` — table view, one row per (reader, format,
 *      length_bucket, source_kind). Mean MiB/s + standard deviation.
 *      Detailed sibling for `throughput_by_length`.
 *   3. `record_throughput_by_length` — same shape as #1 but with
 *      records/sec on the y axis. Useful for short-record workloads
 *      where MiB/s alone undersells "this reader emits a record
 *      every 50 ns".
 *
 * `emit_io_report` ties the three charts together into the standard
 * `bench.scheduler.report_envelope` document shape (see
 * `bench.scheduler.report_envelope::emit_report_envelope`).
 */
module;

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq_io_bench.chart_builder;

import mlogging;
import bench.scheduler.charting;
import bench.scheduler.json_writer;
import bench.scheduler.regression;
import bench.scheduler.report_envelope;
import bench.scheduler.stats;
import bioseq_io_bench.samples;

export {

namespace bioseq::io_bench {

namespace charting = bench_scheduler::charting;

namespace detail {

[[nodiscard]] inline double mean_throughput_mibs(
    const ReaderSample& sample) noexcept {
    if (sample.sample_latencies_ns.empty()) return 0.0;
    if (sample.corpus_bytes_on_disk == 0) return 0.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kNsPerSec = 1e9;
    double sum_throughput = 0.0;
    std::size_t valid_count = 0;
    for (double latency_ns : sample.sample_latencies_ns) {
        if (latency_ns <= 0.0) continue;
        sum_throughput += (static_cast<double>(sample.corpus_bytes_on_disk)
                            / kMiB)
                        * (kNsPerSec / latency_ns);
        ++valid_count;
    }
    if (valid_count == 0) return 0.0;
    return sum_throughput / static_cast<double>(valid_count);
}

[[nodiscard]] inline double mean_records_per_sec(
    const ReaderSample& sample) noexcept {
    if (sample.sample_latencies_ns.empty()) return 0.0;
    if (sample.record_count == 0) return 0.0;
    constexpr double kNsPerSec = 1e9;
    double sum_rate = 0.0;
    std::size_t valid_count = 0;
    for (double latency_ns : sample.sample_latencies_ns) {
        if (latency_ns <= 0.0) continue;
        sum_rate += static_cast<double>(sample.record_count)
                  * kNsPerSec / latency_ns;
        ++valid_count;
    }
    if (valid_count == 0) return 0.0;
    return sum_rate / static_cast<double>(valid_count);
}

[[nodiscard]] inline std::string trace_name(const ReaderSample& sample) {
    return fmt::format("{} ({})", sample.reader_label,
                       io_format_name(sample.format));
}

/// Stable order over the length buckets used as x-categories.
[[nodiscard]] inline std::vector<std::string> length_bucket_axis_labels() {
    return {
        std::string{io_length_bucket_name(IoLengthBucket::SHORT)},
        std::string{io_length_bucket_name(IoLengthBucket::MEDIUM)},
        std::string{io_length_bucket_name(IoLengthBucket::LONG)},
        std::string{io_length_bucket_name(IoLengthBucket::VERY_LONG)},
        std::string{io_length_bucket_name(IoLengthBucket::REAL_HUMAN_GENOME)},
        std::string{io_length_bucket_name(IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB)},
    };
}

[[nodiscard]] inline IoLengthBucket bucket_from_label(
    std::string_view label) noexcept {
    if (label == io_length_bucket_name(IoLengthBucket::SHORT))
        return IoLengthBucket::SHORT;
    if (label == io_length_bucket_name(IoLengthBucket::MEDIUM))
        return IoLengthBucket::MEDIUM;
    if (label == io_length_bucket_name(IoLengthBucket::LONG))
        return IoLengthBucket::LONG;
    if (label == io_length_bucket_name(IoLengthBucket::VERY_LONG))
        return IoLengthBucket::VERY_LONG;
    if (label == io_length_bucket_name(IoLengthBucket::REAL_HUMAN_GENOME))
        return IoLengthBucket::REAL_HUMAN_GENOME;
    return IoLengthBucket::REAL_HUMAN_GENOME_LARGE_FASTQ_6GB;
}

}  // namespace detail

inline void emit_throughput_by_length_chart(
    bench_scheduler::JsonWriter& writer,
    std::span<const ReaderSample> samples) {

    charting::emit_chart_envelope(
        writer,
        "throughput_by_length",
        "Reader throughput per length bucket",
        "Mean MiB/s for each (reader, format) trace across the four "
        "length buckets. Higher is better. Computed as "
        "corpus_bytes_on_disk / mean(per-iteration wall_clock).",
        [&]() {
            // Group samples by trace (reader + format).
            std::map<std::string, std::vector<const ReaderSample*>> traces;
            for (const auto& sample : samples) {
                traces[detail::trace_name(sample)].push_back(&sample);
            }
            const auto x_labels = detail::length_bucket_axis_labels();
            writer.key("data");
            writer.begin_array();
            for (auto& [trace_label, sample_ptrs] : traces) {
                writer.begin_object();
                writer.kv_string("type", "bar");
                writer.kv_string("name", trace_label);
                writer.key("x");
                writer.begin_array();
                for (const auto& bucket_label : x_labels) {
                    writer.value_string(bucket_label);
                }
                writer.end_array();
                writer.key("y");
                writer.begin_array();
                for (const auto& bucket_label : x_labels) {
                    const auto target_bucket =
                        detail::bucket_from_label(bucket_label);
                    double sum_mibs = 0.0;
                    std::size_t hits = 0;
                    for (const auto* sample_ptr : sample_ptrs) {
                        if (sample_ptr->length_bucket != target_bucket) continue;
                        // Prefer the POSIX_READ source for the bar
                        // chart so each (reader, format, bucket)
                        // collapses to a single bar; mmap variant
                        // gets its own reader_label so it surfaces
                        // separately.
                        sum_mibs += detail::mean_throughput_mibs(*sample_ptr);
                        ++hits;
                    }
                    writer.value_double(
                        hits > 0 ? sum_mibs / static_cast<double>(hits) : 0.0);
                }
                writer.end_array();
                charting::emit_plotly_marker(
                    writer, charting::colour_for_key(trace_label));
                writer.end_object();
            }
            writer.end_array();
            charting::emit_plotly_layout(
                writer,
                "Reader throughput per length bucket",
                "length bucket", false,
                "MiB/s", false,
                "group");
        });
}

inline void emit_record_throughput_chart(
    bench_scheduler::JsonWriter& writer,
    std::span<const ReaderSample> samples) {
    charting::emit_chart_envelope(
        writer,
        "records_per_second_by_length",
        "Reader records/sec per length bucket",
        "Mean records/sec for each (reader, format) trace. Useful for "
        "short-record corpora where the per-record dispatch cost "
        "dominates.",
        [&]() {
            std::map<std::string, std::vector<const ReaderSample*>> traces;
            for (const auto& sample : samples) {
                traces[detail::trace_name(sample)].push_back(&sample);
            }
            const auto x_labels = detail::length_bucket_axis_labels();
            writer.key("data");
            writer.begin_array();
            for (auto& [trace_label, sample_ptrs] : traces) {
                writer.begin_object();
                writer.kv_string("type", "bar");
                writer.kv_string("name", trace_label);
                writer.key("x");
                writer.begin_array();
                for (const auto& bucket_label : x_labels) {
                    writer.value_string(bucket_label);
                }
                writer.end_array();
                writer.key("y");
                writer.begin_array();
                for (const auto& bucket_label : x_labels) {
                    const auto target_bucket =
                        detail::bucket_from_label(bucket_label);
                    double sum_rps = 0.0;
                    std::size_t hits = 0;
                    for (const auto* sample_ptr : sample_ptrs) {
                        if (sample_ptr->length_bucket != target_bucket) continue;
                        sum_rps += detail::mean_records_per_sec(*sample_ptr);
                        ++hits;
                    }
                    writer.value_double(
                        hits > 0 ? sum_rps / static_cast<double>(hits) : 0.0);
                }
                writer.end_array();
                charting::emit_plotly_marker(
                    writer, charting::colour_for_key(trace_label));
                writer.end_object();
            }
            writer.end_array();
            charting::emit_plotly_layout(
                writer,
                "Records/sec per length bucket",
                "length bucket", false,
                "records / sec", true,
                "group");
        });
}

inline void emit_throughput_table_chart(
    bench_scheduler::JsonWriter& writer,
    std::span<const ReaderSample> samples) {
    charting::emit_chart_envelope(
        writer,
        "throughput_table",
        "Per-cell throughput table",
        "One row per (reader, format, length_bucket, source_kind) "
        "with mean and standard deviation of MiB/s across the cell's "
        "samples.",
        [&]() {
            std::vector<std::string> cells_reader;
            std::vector<std::string> cells_format;
            std::vector<std::string> cells_bucket;
            std::vector<std::string> cells_source;
            std::vector<double> cells_mean_mibs;
            std::vector<double> cells_stddev_mibs;
            std::vector<std::size_t> cells_record_count;
            std::vector<double> cells_corpus_mb;

            for (const auto& sample : samples) {
                if (sample.sample_latencies_ns.empty()) continue;
                std::vector<double> per_iteration_mibs;
                per_iteration_mibs.reserve(sample.sample_latencies_ns.size());
                constexpr double kMiB = 1024.0 * 1024.0;
                constexpr double kNsPerSec = 1e9;
                for (double latency_ns : sample.sample_latencies_ns) {
                    if (latency_ns <= 0.0) continue;
                    per_iteration_mibs.push_back(
                        (static_cast<double>(sample.corpus_bytes_on_disk)
                            / kMiB) * (kNsPerSec / latency_ns));
                }
                if (per_iteration_mibs.empty()) continue;
                const double mean_mibs = std::accumulate(
                    per_iteration_mibs.begin(),
                    per_iteration_mibs.end(), 0.0)
                    / static_cast<double>(per_iteration_mibs.size());
                double sum_sq = 0.0;
                for (double v : per_iteration_mibs) {
                    const double centred = v - mean_mibs;
                    sum_sq += centred * centred;
                }
                const double stddev_mibs = (per_iteration_mibs.size() > 1)
                    ? std::sqrt(sum_sq / static_cast<double>(
                        per_iteration_mibs.size() - 1))
                    : 0.0;

                cells_reader.push_back(sample.reader_label);
                cells_format.push_back(sample.format_label);
                cells_bucket.push_back(sample.length_bucket_label);
                cells_source.push_back(sample.source_kind_label);
                cells_mean_mibs.push_back(mean_mibs);
                cells_stddev_mibs.push_back(stddev_mibs);
                cells_record_count.push_back(sample.record_count);
                cells_corpus_mb.push_back(
                    static_cast<double>(sample.corpus_bytes_on_disk)
                        / (1024.0 * 1024.0));
            }

            writer.key("data");
            writer.begin_array();
            writer.begin_object();
            writer.kv_string("type", "table");
            writer.key("header");
            writer.begin_object();
            writer.key("values");
            writer.begin_array();
            writer.value_string("reader");
            writer.value_string("format");
            writer.value_string("length_bucket");
            writer.value_string("source_kind");
            writer.value_string("mean MiB/s");
            writer.value_string("stddev MiB/s");
            writer.value_string("record_count");
            writer.value_string("corpus_MB");
            writer.end_array();
            writer.end_object();
            writer.key("cells");
            writer.begin_object();
            // Plotly expects `cells.values` as a 2-D array — outer
            // index is column, inner is row. Emit each column as a
            // bare array element (no per-column key).
            writer.key("values");
            writer.begin_array();
            auto emit_string_column = [&](const std::vector<std::string>& column) {
                writer.begin_array();
                for (const auto& cell : column) writer.value_string(cell);
                writer.end_array();
            };
            auto emit_double_column = [&](const std::vector<double>& column) {
                writer.begin_array();
                for (double cell : column) writer.value_double(cell);
                writer.end_array();
            };
            auto emit_int_column = [&](const std::vector<std::size_t>& column) {
                writer.begin_array();
                for (std::size_t cell : column) {
                    writer.value_int(static_cast<std::int64_t>(cell));
                }
                writer.end_array();
            };
            emit_string_column(cells_reader);
            emit_string_column(cells_format);
            emit_string_column(cells_bucket);
            emit_string_column(cells_source);
            emit_double_column(cells_mean_mibs);
            emit_double_column(cells_stddev_mibs);
            emit_int_column   (cells_record_count);
            emit_double_column(cells_corpus_mb);
            writer.end_array();
            writer.end_object();
            writer.end_object();
            writer.end_array();
            charting::emit_plotly_layout_titled(
                writer, "Per-cell throughput table");
        });
}

/// Top-level emitter for the IO bench report. Builds the standard
/// envelope (schema / commit_hash / sample_count / reader_count /
/// violations / charts) via `bench.scheduler.report_envelope`.
inline void emit_io_report(
    bench_scheduler::JsonWriter& writer,
    std::span<const bench_scheduler::regression::Violation> violations,
    std::string_view commit_hash) {
    const auto& samples = io_bench_samples();
    std::vector<std::string> readers_seen;
    for (const auto& sample : samples) {
        if (std::find(readers_seen.begin(), readers_seen.end(),
                      sample.reader_label) == readers_seen.end()) {
            readers_seen.push_back(sample.reader_label);
        }
    }

    bench_scheduler::ReportMetadata metadata;
    metadata.schema = "bioseq-io-bench-report-v1";
    metadata.commit_hash = commit_hash;
    metadata.sample_count = samples.size();
    metadata.algorithm_count = readers_seen.size();
    metadata.violations = violations;

    bench_scheduler::emit_report_envelope(writer, metadata, [&]() {
        emit_throughput_by_length_chart(writer, samples);
        emit_record_throughput_chart(writer, samples);
        emit_throughput_table_chart(writer, samples);
    });
}

}  // namespace bioseq::io_bench

}  // export
