/**
 * @file readers.cppm
 * @brief Reader and identifier parsing benchmark workloads.
 */
module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <kseq++/seqio.hpp>
#include <map>
#include <simdjson.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

module bioseq.io.benchmark:readers;

import :common;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.io.reader;
import bioseq.io.options;
import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser;

namespace bioseq::io::benchmark::detail {

/// Materializes routed identifier fields into benchmark-owned string slots.
struct bench_identifier_writer {
    std::size_t destination_count_value = 0;
    identifier_benchmark_measurement* measurement = nullptr;
    std::vector<std::string> field_storage;

    explicit bench_identifier_writer(std::size_t destination_count,
                                     identifier_benchmark_measurement& output_measurement)
        : destination_count_value(destination_count), measurement(&output_measurement),
          field_storage(destination_count) {}

    void begin_tuple() {
        for (auto& field : field_storage)
            field.clear();
    }
    void set_field(bioseq::io::identifier_parser::destination_slot slot,
                   const char* data,
                   std::int32_t len) {
        auto& field = field_storage.at(static_cast<std::size_t>(slot));
        field.assign(data, static_cast<std::size_t>(len));
        ++measurement->parsed_field_count;
        measurement->parsed_field_bytes += field.size();
    }
    void set_null(bioseq::io::identifier_parser::destination_slot slot) {
        field_storage.at(static_cast<std::size_t>(slot)).clear();
    }
    void end_tuple() {}
    void finish() {}
    [[nodiscard]] std::size_t destination_count() const noexcept {
        return destination_count_value;
    }
};
static_assert(
    bioseq::io::identifier_parser::identifier_destination_writer<bench_identifier_writer>);

// =====================================================================
//  Reader runners
// =====================================================================
//
// One `dispatch_reader_*` function per reader algorithm. Each returns
// the io_measurement populated with observed_record_count + observed_body_bytes.

/// Run the Bioseq reader with the exact backend and concurrency options supplied.
template <bool p_is_fasta>
[[nodiscard]] io_measurement run_bioseq_io_with_options(const std::string& path,
                                                        const reader_options& options) {
    using alphabet_config = benchmark_alphabet;
    using batch_t = sequence_batch<alphabet_config>;
    constexpr auto file_type =
        p_is_fasta ? sequence_file_format::fasta : sequence_file_format::fastq;

    std::atomic<std::size_t> record_count_atomic{0};
    std::atomic<std::size_t> body_bytes_atomic{0};

    auto handler_fn = [&record_count_atomic, &body_bytes_atomic](batch_t&& batch) {
        const std::size_t local_records = batch.size();
        std::size_t local_body_bytes = 0;
        for (std::size_t record_index = 0; record_index < local_records; ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            using traits = typename decltype(seq_view)::traits;
            local_body_bytes += seq_view.size() * traits::letters_per_unit - seq_view.padding();
        }
        record_count_atomic.fetch_add(local_records, std::memory_order_relaxed);
        body_bytes_atomic.fetch_add(local_body_bytes, std::memory_order_relaxed);
    };
    auto failure_fn = [&path](std::string&& msg) {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] read failed on '{}': {}", path, msg);
    };
    auto outcome = read_sequences_file<seq::dna4, file_type>(path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] read_sequences_file error: {}",
                                         outcome.error());
    }
    io_measurement out;
    out.observed_record_count = record_count_atomic.load(std::memory_order_relaxed);
    out.observed_body_bytes = body_bytes_atomic.load(std::memory_order_relaxed);
    return out;
}

/// Count records and sequence bytes through kseq++.
[[nodiscard]] io_measurement run_kseqpp(const std::string& path) {
    io_measurement out;
    klibpp::SeqStreamIn stream(path.c_str());
    klibpp::KSeq record;
    while (stream >> record) {
        out.observed_body_bytes += record.seq.size();
        out.observed_record_count += 1;
    }
    return out;
}

/// Count FASTA records and body bytes with a line-oriented baseline.
[[nodiscard]] io_measurement run_naive_ifstream_fasta(const std::string& path) {
    io_measurement out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] naive ifstream open failed: '{}'",
                                         path);
        return out;
    }
    std::string line;
    bool in_record = false;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        if (line.front() == '>') {
            if (in_record)
                out.observed_record_count += 1;
            in_record = true;
            continue;
        }
        if (in_record) {
            std::size_t body_size = line.size();
            if (body_size > 0 && line[body_size - 1] == '\r')
                body_size -= 1;
            out.observed_body_bytes += body_size;
        }
    }
    if (in_record)
        out.observed_record_count += 1;
    return out;
}

/// Count canonical four-line FASTQ records with a line-oriented baseline.
[[nodiscard]] io_measurement run_naive_ifstream_fastq(const std::string& path) {
    io_measurement out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] naive ifstream open failed: '{}'",
                                         path);
        return out;
    }
    std::string line;
    enum class fastq_step : std::uint8_t { header, sequence, separator, quality };
    fastq_step step = fastq_step::header;
    while (std::getline(in, line)) {
        switch (step) {
        case fastq_step::header:
            if (line.empty() || line.front() != '@')
                continue;
            step = fastq_step::sequence;
            break;
        case fastq_step::sequence: {
            std::size_t body_size = line.size();
            if (body_size > 0 && line[body_size - 1] == '\r')
                body_size -= 1;
            out.observed_body_bytes += body_size;
            step = fastq_step::separator;
            break;
        }
        case fastq_step::separator:
            step = fastq_step::quality;
            break;
        case fastq_step::quality:
            out.observed_record_count += 1;
            step = fastq_step::header;
            break;
        }
    }
    return out;
}

/// Resolve a configured reader algorithm to its concrete workload.
[[nodiscard]] io_measurement dispatch_reader(std::string_view algo, const reader_input& input) {
    reader_options options{};
    if (algo == "bioseq_io_sync_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_mmap_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap = true;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_async4_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap = false;
        options.concurrency.worker_count = 4;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_sync_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_mmap_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap = true;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_async4_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap = false;
        options.concurrency.worker_count = 4;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_pgzip_fastq") {
        options.io.gzip_auto_detect = true;
        options.io.prefer_mmap = true;
        options.io.parallel_gzip_enabled = true;
        options.io.parallel_gzip_thread_count = 0;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_zlib_fastq") {
        options.io.gzip_auto_detect = true;
        options.io.prefer_mmap = true;
        options.io.parallel_gzip_enabled = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "kseqpp_fasta" || algo == "kseqpp_fastq") {
        return run_kseqpp(input.corpus_path);
    }
    if (algo == "naive_ifstream_fasta") {
        return run_naive_ifstream_fasta(input.corpus_path);
    }
    if (algo == "naive_ifstream_fastq") {
        return run_naive_ifstream_fastq(input.corpus_path);
    }
    bioseq_log<logging_level::error>("[bioseq.io.benchmark] unknown reader algorithm '{}'", algo);
    return {};
}

// =====================================================================
//  Reader benchmark lambdas
// =====================================================================

/// Prepare one reader corpus and record its expected size and format.
[[nodiscard]] reader_input build_reader_input(std::string_view data_dir,
                                              std::string_view test_case_name,
                                              simdjson::dom::element data,
                                              const run_constraints& /*constraints*/) {
    const corpus_descriptor descriptor = prepare_corpus(data_dir, test_case_name, data);
    reader_input input;
    if (!descriptor.valid)
        return input;
    input.corpus_path = descriptor.path;
    input.bytes_on_disk = descriptor.bytes_on_disk;
    input.expected_record_count = descriptor.record_count;
    input.is_fastq = descriptor.is_fastq;
    input.is_compressed = descriptor.is_compressed;
    input.is_synthetic = descriptor.is_synthetic;
    // For gzipped real-data: estimate decompressed size at ~3.2x compressed
    // (matches the SRR FASTQ corpus shape — empirical 6.0 GiB / 1.96 GiB).
    input.decompressed_bytes = input.is_compressed
                                   ? static_cast<std::size_t>(input.bytes_on_disk * 3.2)
                                   : input.bytes_on_disk;
    bioseq_log<logging_level::debug>(
        "[bioseq.io.benchmark] reader corpus ready: '{}' (path={}, bytes={})",
        test_case_name,
        input.corpus_path,
        input.bytes_on_disk);
    return input;
}

/// Run one configured reader workload when corpus preparation succeeded.
[[nodiscard]] io_measurement
run_reader(std::string_view algo, const parameter_map& /*params*/, const reader_input& input) {
    if (input.corpus_path.empty())
        return {};
    return dispatch_reader(algo, input);
}

/// Emit reader size, record, body-byte, and nonempty-corpus metrics.
[[nodiscard]] std::map<std::string, double> evaluate_reader(std::string_view /*algo*/,
                                                            const parameter_map& /*params*/,
                                                            const reader_input& input,
                                                            const io_measurement& output) {
    std::map<std::string, double> metrics;
    metrics.emplace("bytes_on_disk", static_cast<double>(input.bytes_on_disk));
    metrics.emplace("decompressed_bytes", static_cast<double>(input.decompressed_bytes));
    metrics.emplace("record_count", static_cast<double>(output.observed_record_count));
    // Body bytes observed by the reader. For dna4-on-dna5 corpora this
    // is below raw sequence bytes when N is dropped. For dna4-on-dna4 it equals
    // the raw seq bytes. Surface as a metric so equality rules between
    // bioseq paths can detect a path silently dropping or duplicating
    // bytes that the others see.
    metrics.emplace("observed_body_bytes", static_cast<double>(output.observed_body_bytes));
    // Sanity: the reader must observe at least one record on a
    // non-empty corpus. Surface as a 0/1 metric so the regression
    // layer can pin it to 1 across iterations and test cases.
    metrics.emplace("records_nonzero", output.observed_record_count > 0 ? 1.0 : 0.0);
    return metrics;
}

/// Remove only synthetic reader corpora created for the test case.
void cleanup_reader(std::string_view /*data_dir*/,
                    std::string_view /*test_case_name*/,
                    simdjson::dom::element /*data*/,
                    const reader_input& input) {
    if (!input.is_synthetic || input.corpus_path.empty())
        return;
    std::error_code ec;
    filesystem::remove(input.corpus_path, ec);
}

// =====================================================================
// Identifier-parser overhead benchmark
// =====================================================================

/// Resolve the UniProt destination mapping for an identifier workload.
[[nodiscard]] identifier_benchmark_input
build_identifier_input(std::string_view data_dir,
                       std::string_view test_case_name,
                       simdjson::dom::element data,
                       const run_constraints& constraints) {
    identifier_benchmark_input input;
    input.reader = build_reader_input(data_dir, test_case_name, data, constraints);
    if (input.reader.corpus_path.empty())
        return input;

    using namespace bioseq::io::identifier_parser;
    const std::vector<column_descriptor> columns{
        {.name = "header",
         .is_text = true,
         .is_primary_key = false,
         .not_null = false,
         .has_default = false,
         .is_identity_or_generated = false},
        {.name = "accession",
         .is_text = true,
         .is_primary_key = false,
         .not_null = false,
         .has_default = false,
         .is_identity_or_generated = false},
        {.name = "organism_name",
         .is_text = true,
         .is_primary_key = false,
         .not_null = false,
         .has_default = false,
         .is_identity_or_generated = false},
        {.name = "organism_identifier",
         .is_text = false,
         .is_primary_key = false,
         .not_null = false,
         .has_default = false,
         .is_identity_or_generated = false},
        {.name = "protein_existence",
         .is_text = false,
         .is_primary_key = false,
         .not_null = false,
         .has_default = false,
         .is_identity_or_generated = false},
        {.name = "sequence_version",
         .is_text = false,
         .is_primary_key = false,
         .not_null = false,
         .has_default = false,
         .is_identity_or_generated = false},
    };
    auto map = resolve_identifier_mapping(header_format::uniprotkb,
                                          header_format::uniprotkb,
                                          columns,
                                          std::span<const user_column_mapping>{});
    if (!map) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] identifier mapping failed: {}",
                                         map.error());
        input.reader.corpus_path.clear();
        return input;
    }
    input.mapping = std::move(*map);
    return input;
}

/// Parse or copy every FASTA header while preserving identical reader work.
[[nodiscard]] identifier_benchmark_measurement
run_identifier_bench(std::string_view algo,
                     const parameter_map& /*params*/,
                     const identifier_benchmark_input& input) {
    identifier_benchmark_measurement out;
    if (input.reader.corpus_path.empty())
        return out;

    using namespace bioseq::io;
    using namespace bioseq::io::identifier_parser;
    using alphabet_config = benchmark_alphabet;
    using batch_t = sequence_batch<alphabet_config>;

    std::string dumb_header_copy;
    identifier_parse_state parse_state;
    bench_identifier_writer parsed_writer{input.mapping.routing.destination_count, out};
    const auto writer_ref = make_identifier_writer_reference(parsed_writer);

    auto handler_fn =
        [&algo, &dumb_header_copy, &input, &out, &parse_state, &parsed_writer, &writer_ref](
            batch_t&& batch) {
            for (std::size_t i = 0; i < batch.size(); ++i) {
                const auto header = batch.header(i);
                const auto seq_view = batch.sequence(i);
                using traits = typename decltype(seq_view)::traits;
                out.observed_body_bytes +=
                    seq_view.size() * traits::letters_per_unit - seq_view.padding();
                ++out.observed_record_count;
                out.header_bytes += header.size();

                if (algo == "bioseq_identifier_parse_uniprot_fasta") {
                    parsed_writer.begin_tuple();
                    auto parsed = parse_and_route_header(header_format::uniprotkb,
                                                         header,
                                                         input.mapping.routing,
                                                         parse_state,
                                                         writer_ref);
                    if (!parsed)
                        bioseq_log<logging_level::error>(
                            "[bioseq.io.benchmark] identifier parse failed: {}", parsed.error());
                    parsed_writer.end_tuple();
                } else { // bioseq_dumb_header_fasta
                    dumb_header_copy.assign(header.data(), header.size());
                }
            }
        };
    auto failure_fn = [](std::string&& msg) {
        bioseq_log<logging_level::error>(
            "[bioseq.io.benchmark] identifier benchmark read failed: {}", msg);
    };
    reader_options opts{};
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap = false;
    opts.concurrency.worker_count = 0;
    (void)read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        input.reader.corpus_path, handler_fn, failure_fn, opts);
    return out;
}

/// Emit identifier parsing volume and correctness counters.
[[nodiscard]] std::map<std::string, double>
evaluate_identifier_bench(std::string_view /*algo*/,
                          const parameter_map& /*params*/,
                          const identifier_benchmark_input& input,
                          const identifier_benchmark_measurement& out) {
    return {
        {"bytes_on_disk", static_cast<double>(input.reader.bytes_on_disk)},
        {"record_count", static_cast<double>(out.observed_record_count)},
        {"observed_body_bytes", static_cast<double>(out.observed_body_bytes)},
        {"records_nonzero", out.observed_record_count > 0 ? 1.0 : 0.0},
        {"header_bytes", static_cast<double>(out.header_bytes)},
        {"parsed_field_count", static_cast<double>(out.parsed_field_count)},
        {"parsed_field_bytes", static_cast<double>(out.parsed_field_bytes)},
    };
}

/// Reuse reader cleanup for the identifier workload corpus.
void cleanup_identifier_bench(std::string_view data_dir,
                              std::string_view test_case_name,
                              simdjson::dom::element data,
                              const identifier_benchmark_input& input) {
    cleanup_reader(data_dir, test_case_name, data, input.reader);
}

} // namespace bioseq::io::benchmark::detail
