/**
 * @file index.cppm
 * @brief Set up and run the configured sequence IO benchmarks.
 */
module;

#include <functional>
#include <map>
#include <simdjson.h>
#include <string>
#include <string_view>

export module bioseq.io.benchmark;

import :common;
import :lazy_expression;
import :readers;
import :writers;
import bioseq.common.logging;
import bioseq.bench.framework;

#ifndef BIOSEQ_IO_REPORT_DIR
#error "BIOSEQ_IO_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_DATA_DIR
#error "BIOSEQ_IO_DATA_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_BENCH_CONFIG_PATH
#error "BIOSEQ_IO_BENCH_CONFIG_PATH must be defined by the build system"
#endif

export namespace bioseq::io::benchmark {

/// Run every sequence IO benchmark selected by the configuration.
int run(int argc, char** argv) {
    using namespace detail;

    configure_logging(true);
    fmtlog::setLogLevel(fmtlog::INF);

    const std::string config_path{BIOSEQ_IO_BENCH_CONFIG_PATH};
    const std::string report_dir{BIOSEQ_IO_REPORT_DIR};
    const std::string data_dir{BIOSEQ_IO_DATA_DIR};

    using bioseq::bench::framework::create_benchmark;

    using resource_estimator =
        std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)>;
    using reader_builder = std::function<reader_input(
        std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>;
    using reader_runner =
        std::function<io_measurement(std::string_view, const parameter_map&, const reader_input&)>;
    using reader_evaluator = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const reader_input&, const io_measurement&)>;
    using reader_cleanup = std::function<void(
        std::string_view, std::string_view, simdjson::dom::element, const reader_input&)>;

    using writer_builder = std::function<writer_input(
        std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>;
    using writer_runner =
        std::function<io_measurement(std::string_view, const parameter_map&, const writer_input&)>;
    using writer_evaluator = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&, const writer_input&, const io_measurement&)>;
    using writer_cleanup = std::function<void(
        std::string_view, std::string_view, simdjson::dom::element, const writer_input&)>;

    // Identifier-parser overhead benchmark function types.
    using identifier_builder = std::function<identifier_benchmark_input(
        std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>;
    using identifier_runner = std::function<identifier_benchmark_measurement(
        std::string_view, const parameter_map&, const identifier_benchmark_input&)>;
    using identifier_evaluator =
        std::function<std::map<std::string, double>(std::string_view,
                                                    const parameter_map&,
                                                    const identifier_benchmark_input&,
                                                    const identifier_benchmark_measurement&)>;
    using identifier_cleanup = std::function<void(std::string_view,
                                                  std::string_view,
                                                  simdjson::dom::element,
                                                  const identifier_benchmark_input&)>;

    using lazy_expression_builder = std::function<lazy_expression_input(
        std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>;
    using lazy_expression_runner = std::function<lazy_expression_measurement(
        std::string_view, const parameter_map&, const lazy_expression_input&)>;
    using lazy_expression_evaluator =
        std::function<std::map<std::string, double>(std::string_view,
                                                    const parameter_map&,
                                                    const lazy_expression_input&,
                                                    const lazy_expression_measurement&)>;
    using lazy_expression_cleanup = std::function<void(
        std::string_view, std::string_view, simdjson::dom::element, const lazy_expression_input&)>;

    return bioseq::bench::framework::run(
        argc,
        argv,
        report_dir,
        data_dir,
        create_benchmark<reader_input, io_measurement>(
            "IO readers — FASTA (plain / async / mmap)",
            "bench_io_readers_fasta",
            config_path,
            resource_estimator{estimate_reader_resources},
            reader_builder{build_reader_input},
            reader_runner{run_reader},
            reader_evaluator{evaluate_reader},
            reader_cleanup{cleanup_reader}),
        create_benchmark<reader_input, io_measurement>(
            "IO readers — FASTQ (plain / async / mmap)",
            "bench_io_readers_fastq",
            config_path,
            resource_estimator{estimate_reader_resources},
            reader_builder{build_reader_input},
            reader_runner{run_reader},
            reader_evaluator{evaluate_reader},
            reader_cleanup{cleanup_reader}),
        create_benchmark<reader_input, io_measurement>(
            "IO readers — gzipped paths",
            "bench_io_readers_gz",
            config_path,
            resource_estimator{estimate_reader_resources},
            reader_builder{build_reader_input},
            reader_runner{run_reader},
            reader_evaluator{evaluate_reader},
            reader_cleanup{cleanup_reader}),
        create_benchmark<writer_input, io_measurement>(
            "IO writers — FASTA (sync / parallel)",
            "bench_io_writers_fasta",
            config_path,
            resource_estimator{estimate_writer_resources},
            writer_builder{build_writer_input},
            writer_runner{run_writer},
            writer_evaluator{evaluate_writer},
            writer_cleanup{cleanup_writer}),
        create_benchmark<writer_input, io_measurement>(
            "IO writers — FASTQ (sync / parallel)",
            "bench_io_writers_fastq",
            config_path,
            resource_estimator{estimate_writer_resources},
            writer_builder{build_writer_input},
            writer_runner{run_writer},
            writer_evaluator{evaluate_writer},
            writer_cleanup{cleanup_writer}),
        create_benchmark<identifier_benchmark_input, identifier_benchmark_measurement>(
            "IO identifier parser — UniProt header overhead",
            "bench_io_identifier_parser",
            config_path,
            resource_estimator{estimate_reader_resources},
            identifier_builder{build_identifier_input},
            identifier_runner{run_identifier_bench},
            identifier_evaluator{evaluate_identifier_bench},
            identifier_cleanup{cleanup_identifier_bench}),
        create_benchmark<lazy_expression_input, lazy_expression_measurement>(
            "Lazy sequence expression — eager versus deferred materialization",
            "bench_io_lazy_sequence_expression",
            config_path,
            resource_estimator{estimate_lazy_expression_resources},
            lazy_expression_builder{build_lazy_expression_input},
            lazy_expression_runner{run_lazy_expression},
            lazy_expression_evaluator{evaluate_lazy_expression},
            lazy_expression_cleanup{cleanup_lazy_expression}));
}

} // namespace bioseq::io::benchmark
