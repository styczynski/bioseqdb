/** @file Writer workloads with ordered, exact original-record validation. */
module;

// Canonicalize Highway's internal-linkage templates before importing streaming codecs.
#include <hwy/highway.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <simdjson.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

module bioseq.io.benchmark:writers;

import :common;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.simd.utils;
import bioseq.io.reader;
import bioseq.io.options;
import bioseq.io.writer;
import bioseq.io.record_schema;
import bioseq.io.write_pipeline;
import bioseq.io.identifier_parser.canonical;
import bioseq.io.identifier_parser.preservation;
import bioseq.io.alignment.sequence_codec;
import bioseq.parallelism.pool;

namespace bioseq::io::benchmark::detail {

/** Canonical-only scalar workload: escaped text, empty text, NULL and a typed integer. */
inline constexpr std::array<scalar_field_view, 4> writer_metadata_fields{{
    {"sample", column_value_kind::text, "pg_catalog.text", false, "sample [A=B] %\t"},
    {"empty", column_value_kind::text, "pg_catalog.text", false, ""},
    {"missing", column_value_kind::text, "pg_catalog.text", true, ""},
    {"replicate", column_value_kind::signed_integer, "pg_catalog.int8", false, "7"}
}};

/** Keep untimed fixture acquisition and readback in physical file order. */
[[nodiscard]] reader_options writer_validation_options() {
    reader_options options{};
    options.io.gzip_auto_detect = false;
    options.io.prefer_mmap = true;
    // run_pipeline_for_source dispatches any nonzero count into the active pool.
    // Zero plus one parser selects the synchronous, physical-file-order route.
    options.concurrency.worker_count = 0u;
    options.concurrency.parser_count = 1u;
    options.encoder.strict_alphabet = true;
    options.encoder.strict_fastq = true;
    return options;
}

/** Retain generic packed batches before timing; callbacks cannot shard or reorder. */
template<sequence_alphabet Alphabet, bool IsFasta>
[[nodiscard]] bool slurp_corpus(const std::string& path, writer_corpus<Alphabet>& corpus,
        std::size_t& record_count) {
    constexpr auto format = IsFasta ? sequence_file_format::fasta : sequence_file_format::fastq;
    record_count = 0u;
    const auto handler = [&corpus, &record_count](typename writer_corpus<Alphabet>::batch_type&& batch) {
        record_count += batch.size();
        corpus.batches.push_back(std::move(batch));
    };
    const auto failure = [&path](std::string&& message) {
        bioseq_log<logging_level::error>("writer fixture read failed on '{}': {}", path, message);
    };
    const auto parsed = read_sequences_file<Alphabet, format>(path, handler, failure, writer_validation_options());
    return parsed.has_value() && record_count != 0u;
}

/** Borrow ASCII quality through the existing checked SIMD rank adapter, one tile at a time. */
[[nodiscard]] const std::uint8_t* read_writer_quality(const void* context,
        storage::sequence_size_t from, std::size_t count, std::span<std::uint8_t> scratch) {
    const auto& source = *static_cast<const writer_quality_source*>(context);
    const auto quality = source.bytes;
    if (from > quality.size() || count > quality.size() - from || count > scratch.size()) return nullptr;
    const auto decoded = alignment::decode_sam_rank_tile<seq::phred94>(
        quality.subspan(static_cast<std::size_t>(from), count), scratch.first(count), source.transform);
    return decoded ? scratch.data() : nullptr;
}

/** Freeze source descriptors after all backing vectors have their final size. */
template<sequence_alphabet Alphabet>
void prepare_writer_records(writer_input& input, writer_corpus<Alphabet>& corpus) {
    const auto transform = alignment::sequence_codec_kernel();
    if (transform == nullptr) { input.valid = false; return; }
    corpus.sequences.reserve(input.source_record_count);
    input.qualities.reserve(input.source_record_count);
    input.canonical_headers.reserve(input.source_record_count);
    input.records.reserve(input.source_record_count);
    for (const auto& batch : corpus.batches) {
        for (std::size_t index = 0u; index < batch.size(); ++index) {
            corpus.sequences.push_back(batch.sequence(index));
            input.qualities.push_back({batch.quality(index), transform});
            input.canonical_headers.emplace_back(batch.header(index));
            input.canonical_headers.back().append(" writer [note=x] %\t\n");
        }
    }
    for (std::size_t index = 0u; index < corpus.sequences.size(); ++index) {
        const auto& sequence = corpus.sequences[index];
        using sequence_traits = typename std::remove_cvref_t<decltype(sequence)>::traits;
        export_record_view row;
        row.ordinal = index + 1u;
        row.header = input.canonical_headers[index];
        row.sequence = storage::borrowed_rank_window_source{&sequence,
            &sequence_writer<Alphabet, sequence_file_format::fasta>::read_sequence,
            sequence.bp_size(), supported_storage_alphabets::template index_of<Alphabet>(),
            storage::sequence_transform::identity, true,
            storage::resident_packed_sequence{sequence.data(), sequence.size(), sequence.bp_size(),
                sizeof(std::uint64_t), sequence_traits::bits_per_char, 0u,
                static_cast<std::uint16_t>(sequence.padding())}};
        if (input.is_fastq) {
            row.quality = storage::borrowed_rank_window_source{&input.qualities[index], &read_writer_quality,
                input.qualities[index].bytes.size(), supported_storage_alphabets::template index_of<seq::phred94>(),
                storage::sequence_transform::identity, true};
        }
        row.metadata = writer_metadata_fields;
        input.records.push_back(row);
    }
    input.metadata.declared_alphabet_codes.push_back(supported_biological_alphabets::template index_of<Alphabet>());
    if (input.is_fastq)
        input.metadata.declared_quality_alphabet_codes.push_back(supported_storage_alphabets::template index_of<seq::phred94>());
}

/** Use the existing corpus generator's declared alphabet; real input keeps its original identity. */
[[nodiscard]] writer_input build_writer_input(std::string_view data_dir, std::string_view test_case_name,
        simdjson::dom::element data, const run_constraints&) {
    writer_input input;
    input.alphabet = read_string_field(data, "alphabet", "dna4");
    const auto code = supported_biological_alphabets::try_index_of_name(input.alphabet.c_str());
    if (!code || read_bool_field(data, "compressed", false)) return input;
    const auto descriptor = prepare_corpus(data_dir, test_case_name, data);
    if (!descriptor.valid) return input;
    input.source_corpus_path = descriptor.path;
    input.source_bytes_on_disk = descriptor.bytes_on_disk;
    input.is_fastq = descriptor.is_fastq;
    input.is_synthetic_source = descriptor.is_synthetic;
    supported_biological_alphabets::dispatch_alphabet_by_code(*code, [&input]<class Alphabet>() {
        auto& corpus = input.corpus.template emplace<writer_corpus<Alphabet>>();
        input.valid = input.is_fastq
            ? slurp_corpus<Alphabet, false>(input.source_corpus_path, corpus, input.source_record_count)
            : slurp_corpus<Alphabet, true>(input.source_corpus_path, corpus, input.source_record_count);
        if (input.valid) prepare_writer_records(input, corpus);
    });
    if (!input.valid) return input;
    input.output_scratch_path = allocate_temp_path(std::string{"writer_out_"} + std::string{test_case_name});
    bioseq_log<logging_level::info>("writer fixture: case={} alphabet={} records={} "
        "native_fields=header,sequence,quality canonical_fields=escaped_header,sequence,quality,4_typed_scalars",
        test_case_name, input.alphabet, input.source_record_count);
    return input;
}

/// Write prepared batches within the current shared-arena participant budget.
template <sequence_alphabet Alphabet, bool p_is_fasta>
[[nodiscard]] io_measurement
run_bioseq_writer(const writer_input& input, const writer_corpus<Alphabet>& corpus,
        std::size_t thread_count, bool bound_output = false) {
    constexpr auto file_type =
        p_is_fasta ? sequence_file_format::fasta : sequence_file_format::fastq;
    writer_options options{};
    options.overwrite = true;
    concurrency_options concurrency{};
    concurrency.worker_count = thread_count;
    if (bound_output) {
        // Preserve the existing oversized-output check without writable mappings.
        const auto estimated_output_bytes = bioseq::common::checked_add(
            input.source_bytes_on_disk, input.source_bytes_on_disk / 2u);
        if (!estimated_output_bytes) {
            bioseq_log<logging_level::error>("[bioseq.io.benchmark] output byte limit overflows");
            return {};
        }
        options.maximum_output_bytes = std::max(std::size_t{4096u}, *estimated_output_bytes);
    }
    auto writer_or_err =
        sequence_writer<Alphabet, file_type>::open(input.output_scratch_path, options, concurrency);
    if (!writer_or_err) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer open failed: {}",
                                         writer_or_err.error());
        return {};
    }
    auto& writer = *writer_or_err;
    for (const auto& batch : corpus.batches) {
        if (!writer.write_batch(batch)) {
            bioseq_log<logging_level::error>("[bioseq.io.benchmark] ordered writer failed");
            return {};
        }
    }
    auto close_outcome = writer.close();
    if (!close_outcome) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer close failed: {}",
                                         close_outcome.error());
        return {};
    }
    io_measurement out;
    out.observed_record_count = input.source_record_count;
    std::error_code ec;
    out.bytes_written = filesystem::file_size(input.output_scratch_path, ec);
    return ec ? io_measurement{} : out;
}

/// Serialize prepared batches through a line-oriented ofstream baseline.
template <sequence_alphabet Alphabet, bool p_is_fasta>
[[nodiscard]] io_measurement run_naive_ofstream(const writer_input& input, const writer_corpus<Alphabet>& corpus) {
    std::ofstream out(input.output_scratch_path, std::ios::binary);
    if (!out) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] naive ofstream open failed: '{}'",
                                         input.output_scratch_path);
        return {};
    }
    std::string scratch;
    scratch.reserve(64 * 1024);
    constexpr std::size_t fasta_wrap = 80;
    const auto transform = alignment::sequence_codec_kernel();
    if (transform == nullptr) return {};

    for (const auto& batch : corpus.batches) {
        const std::size_t record_count_in_batch = batch.size();
        for (std::size_t record_index = 0; record_index < record_count_in_batch; ++record_index) {
            scratch.clear();
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            using seq_traits = typename std::remove_cvref_t<decltype(seq_view)>::traits;
            constexpr std::size_t letters_per_word = seq_traits::letters_per_unit;
            const std::size_t total_letters =
                seq_view.size() * letters_per_word - seq_view.padding();
            scratch.push_back(p_is_fasta ? '>' : '@');
            if (header_view.find_first_of("\r\n") != std::string_view::npos) return {};
            scratch.append(header_view);
            scratch.push_back('\n');
            std::size_t in_line_count = 0;
            for (std::size_t letter_index = 0; letter_index < total_letters; ++letter_index) {
                const auto word = seq_view.data()[letter_index / letters_per_word];
                const std::size_t letter_in_word = letter_index % letters_per_word;
                const std::uint8_t rank = static_cast<std::uint8_t>(
                    (word >> (seq_traits::bits_per_char * letter_in_word)) & seq_traits::char_mask);
                // Decode packed ranks through the alphabet's character mapping.
                scratch.push_back(seq_traits::letter_to_char(rank));
                if constexpr (p_is_fasta) {
                    ++in_line_count;
                    if (in_line_count == fasta_wrap && letter_index + 1 < total_letters) {
                        scratch.push_back('\n');
                        in_line_count = 0;
                    }
                }
            }
            if constexpr (!p_is_fasta) {
                scratch.push_back('\n');
                scratch.push_back('+');
                scratch.push_back('\n');
                const auto quality_view = batch.quality(record_index);
                if (quality_view.size() != total_letters
                    || !transform(bioseq::simd::sequence_rank_transform::validate_fastq,
                        quality_view, 0u, {}, {})) {
                    bioseq_log<logging_level::error>("naive FASTQ requires exact valid quality length");
                    return {};
                }
                scratch.append(reinterpret_cast<const char*>(quality_view.data()), quality_view.size());
            }
            scratch.push_back('\n');
            out.write(scratch.data(), static_cast<std::streamsize>(scratch.size()));
        }
    }
    out.flush();
    out.close();
    if (!out) return {};
    io_measurement result;
    result.observed_record_count = input.source_record_count;
    std::error_code ec;
    result.bytes_written = filesystem::file_size(input.output_scratch_path, ec);
    return ec ? io_measurement{} : result;
}

/** Immutable views keep the original packed corpus alive throughout ordered formatting. */
struct writer_record_batch {
    std::span<const export_record_view> records;
    /** Return the number of immutable rows in this borrowed batch. */
    [[nodiscard]] std::size_t size() const { return records.size(); }
    /** Return one descriptor while its fixture retains every underlying owner. */
    [[nodiscard]] export_record_view row(std::size_t index) const { return records[index]; }
    /** Fixture storage is held by the benchmark's resident-input reservation. */
    [[nodiscard]] std::size_t accounted_capacity_bytes() const { return 0u; }
};

/** One alphabet-erased source avoids an alphabet-by-format writer template product. */
struct writer_record_source {
    using batch_type = writer_record_batch;
    const writer_input& input;
    std::size_t position{};
    /** The explicit row descriptors require no column mapping in this neutral API benchmark. */
    [[nodiscard]] std::span<const record_column> columns() const { return {}; }
    /** Declare native sequence and quality identities for the entire fixture. */
    [[nodiscard]] const record_source_metadata& metadata() const { return input.metadata; }
    /** Yield a bounded borrowed batch without copying or decoding biological bodies. */
    [[nodiscard]] std::expected<std::optional<batch_type>, record_error> next() {
        if (position == input.records.size()) return std::optional<batch_type>{};
        const auto count = std::min(std::size_t{512u}, input.records.size() - position);
        const batch_type batch{std::span{input.records}.subspan(position, count)};
        position += count;
        return std::optional{batch};
    }
    /** Stop acquisition; immutable fixture owners outlive all outstanding workers. */
    void cancel() { position = input.records.size(); }
};
static_assert(export_record_source<writer_record_source>);

/** Plain buffered output for measuring the shared canonical formatter's sink overhead. */
struct writer_ofstream_sink {
    std::ofstream& stream;
    /** Write one checked byte span; text and sequence codecs share this binary core. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
            return std::unexpected(std::string{"canonical ofstream append exceeds streamsize"});
        if (!bytes.empty())
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) return std::unexpected(std::string{"canonical ofstream write failed"});
        return {};
    }
    /** Forward textual fragments without a second narrowing or copying path. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view bytes) {
        return append(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    }
};

/** Measure canonical write_records or its same-formatter ofstream baseline, including close. */
[[nodiscard]] io_measurement run_canonical_writer(const writer_input& input,
        std::size_t workers, bool plain_ofstream) {
    writer_options options;
    options.overwrite = true;
    options.format = input.is_fastq ? sequence_file_format::fastq : sequence_file_format::fasta;
    if (plain_ofstream) {
        std::ofstream stream(input.output_scratch_path, std::ios::binary);
        if (!stream) return {};
        writer_ofstream_sink sink{stream};
        const auto transform = alignment::sequence_codec_kernel();
        if (transform == nullptr) return {};
        for (const auto& row : input.records) {
            const auto written = emit_text_record(sink, row, *options.format, options, transform);
            if (!written) {
                bioseq_log<logging_level::error>("canonical ofstream failed: {}", written.error());
                return {};
            }
        }
        stream.flush();
        stream.close();
        if (!stream) return {};
    } else {
        concurrency_options concurrency;
        concurrency.worker_count = workers;
        writer_record_source source{input};
        const auto written = write_records(source, input.output_scratch_path, options, concurrency);
        if (!written || *written != input.source_record_count) {
            if (!written) bioseq_log<logging_level::error>("write_records failed: {}", written.error().message);
            return {};
        }
    }
    std::error_code error;
    const auto bytes = filesystem::file_size(input.output_scratch_path, error);
    if (error) return {};
    return {.observed_record_count = input.source_record_count, .bytes_written = bytes};
}

/** Distinguish canonical metadata output from the historical native text algorithms. */
[[nodiscard]] bool canonical_writer_algorithm(std::string_view algorithm) {
    return algorithm.starts_with("bioseq_records_") || algorithm.starts_with("canonical_ofstream_");
}

/** Resolve the total participant budget for preparation and measured dispatch. */
[[nodiscard]] std::size_t writer_worker_budget(std::string_view algorithm) {
    if (algorithm == "bioseq_writer_sync_fasta" || algorithm == "bioseq_writer_sync_fastq"
        || algorithm == "naive_ofstream_fasta" || algorithm == "naive_ofstream_fastq"
        || algorithm == "bioseq_records_sync_fasta" || algorithm == "bioseq_records_sync_fastq"
        || algorithm == "canonical_ofstream_fasta" || algorithm == "canonical_ofstream_fastq") return 1u;
    if (algorithm == "bioseq_writer_parallel4_fasta" || algorithm == "bioseq_writer_parallel4_fastq"
        || algorithm == "bioseq_writer_bounded_pwrite_fasta" || algorithm == "bioseq_writer_bounded_pwrite_fastq"
        || algorithm == "bioseq_records_parallel4_fasta" || algorithm == "bioseq_records_parallel4_fastq") return 4u;
    if (algorithm == "bioseq_writer_parallel8_fasta" || algorithm == "bioseq_writer_parallel8_fastq") return 8u;
    return 0u;
}

/** Prepare the existing shared arena outside timing; report incomparable durability explicitly. */
[[nodiscard]] io_pool_scope prepare_writer_iteration(std::string_view algorithm, std::size_t repeat) {
    const auto requested = writer_worker_budget(algorithm);
    io_pool_scope preparation{static_cast<std::int64_t>(requested)};
    if (!preparation.error.empty()) return preparation;
    const auto& pool = parallelism::global_pool::instance();
    bioseq_log<logging_level::info>(
        "writer budget: algorithm={} repeat={} requested_workers={} pool_limit={} framework_limit={} "
        "active_workers=unobserved representation={} durability={}",
        algorithm, repeat, requested, pool.maximum_concurrency(), preparation.framework_limit,
        canonical_writer_algorithm(algorithm) ? "canonical_metadata" : "native_text",
        algorithm.starts_with("naive_ofstream_") || algorithm.starts_with("canonical_ofstream_")
            ? "flush_close_only" : "fsync_atomic_publication");
    return preparation;
}

/** Dispatch native writers by the actual fixture type, canonical writers by one erased source. */
[[nodiscard]] io_measurement run_writer(std::string_view algorithm, const parameter_map&,
        const writer_input& input) {
    const auto workers = writer_worker_budget(algorithm);
    if (!input.valid || input.output_scratch_path.empty() || workers == 0u) return {};
    if (canonical_writer_algorithm(algorithm))
        return run_canonical_writer(input, workers, algorithm.starts_with("canonical_ofstream_"));
    return std::visit([algorithm, workers, &input](const auto& corpus) {
        using Alphabet = typename std::remove_cvref_t<decltype(corpus)>::alphabet_type;
        if (algorithm == "naive_ofstream_fasta") return run_naive_ofstream<Alphabet, true>(input, corpus);
        if (algorithm == "naive_ofstream_fastq") return run_naive_ofstream<Alphabet, false>(input, corpus);
        const bool bounded = algorithm == "bioseq_writer_bounded_pwrite_fasta"
            || algorithm == "bioseq_writer_bounded_pwrite_fastq";
        return input.is_fastq ? run_bioseq_writer<Alphabet, false>(input, corpus, workers, bounded)
            : run_bioseq_writer<Alphabet, true>(input, corpus, workers, bounded);
    }, input.corpus);
}

/** Reuse the strict text-shape decoder while independently checking original SQL type names. */
struct writer_canonical_shape : identifier_parser::text_record_shape {
    std::size_t field_index{};
    /** Borrow the decoded envelope through the existing strict shape validator. */
    explicit writer_canonical_shape(const identifier_parser::canonical_header& header)
        : identifier_parser::text_record_shape{header} {}
    /** Match type declarations in their original order before delegating envelope consistency checks. */
    [[nodiscard]] std::expected<void, std::string> metadata(std::string_view name, std::string_view type,
            std::optional<std::string_view> value, bool types_only) {
        if (field_index >= writer_metadata_fields.size() || writer_metadata_fields[field_index].name != name
            || writer_metadata_fields[field_index].qualified_type != type || !types_only)
            return std::unexpected(std::string{"canonical original metadata type/order differs"});
        ++field_index;
        return identifier_parser::text_record_shape::metadata(name, type, value, types_only);
    }
};

/** Compare decoded identifiers, scalar values/types/NULL, and both native sequence descriptors. */
[[nodiscard]] bool canonical_header_matches(std::string_view wire, const export_record_view& expected,
        std::string_view alphabet) {
    const auto parsed = identifier_parser::parse_canonical_header(wire);
    if (!parsed || !*parsed) return false;
    const auto& header = **parsed;
    if (header.header != *expected.header || header.alphabet != alphabet
        || header.fields.size() != writer_metadata_fields.size()) return false;
    constexpr std::array discriminators{'s', 's', 'n', 'i'};
    for (std::size_t index = 0u; index < writer_metadata_fields.size(); ++index) {
        const auto& field = writer_metadata_fields[index];
        const auto& actual = header.fields[index];
        const auto value = field.is_null ? std::optional<std::string_view>{} : std::optional{field.bytes};
        if (actual.name != field.name || actual.value != value || actual.discriminator != discriminators[index])
            return false;
    }
    writer_canonical_shape shape{header};
    if (!identifier_parser::decode_preservation_record(header.payload, shape, true, header.payload.size())
        || shape.header_is_null || shape.field_index != writer_metadata_fields.size()) return false;
    const std::array sources{expected.sequence, expected.quality};
    for (std::size_t index = 0u; index < sources.size(); ++index) {
        const auto& actual = shape.sequences[index];
        if (actual.present != sources[index].has_value()
            || actual.storage != preservation_storage::native) return false;
        const auto expected_alphabet = sources[index] ? std::optional{sources[index]->alphabet_code}
            : std::optional<std::uint8_t>{};
        const auto expected_length = sources[index] ? sources[index]->symbol_count : 0u;
        if (actual.alphabet_code != expected_alphabet || actual.original_length != expected_length) return false;
    }
    return true;
}

/** Exact rank comparison ignores packed padding and never allocates a whole decoded sequence. */
template<class Sequence>
[[nodiscard]] bool writer_sequences_equal(const Sequence& expected, const Sequence& observed) {
    if (expected.bp_size() != observed.bp_size()) return false;
    std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity> expected_ranks, observed_ranks;
    for (std::size_t position = 0u; position < expected.bp_size();) {
        const auto count = std::min(expected_ranks.size(), expected.bp_size() - position);
        expected.template to_encoded_into<false>(std::span{expected_ranks}.first(count), position, position + count);
        observed.template to_encoded_into<false>(std::span{observed_ranks}.first(count), position, position + count);
        if (!std::equal(expected_ranks.begin(), expected_ranks.begin() + count, observed_ranks.begin())) return false;
        position += count;
    }
    return true;
}

/** Compare every output record with the next original, independent of reader batch boundaries. */
template<sequence_alphabet Alphabet, bool IsFasta>
[[nodiscard]] bool roundtrip_match(const writer_input& input, const writer_corpus<Alphabet>& corpus,
        bool canonical) {
    constexpr auto format = IsFasta ? sequence_file_format::fasta : sequence_file_format::fastq;
    std::size_t expected_batch{}, expected_row{}, observed_records{};
    bool valid = true;
    const auto handler = [&input, &corpus, canonical, &expected_batch, &expected_row, &observed_records, &valid]
            (typename writer_corpus<Alphabet>::batch_type&& observed) {
        for (std::size_t index = 0u; index < observed.size(); ++index) {
            while (expected_batch < corpus.batches.size() && expected_row == corpus.batches[expected_batch].size()) {
                ++expected_batch;
                expected_row = 0u;
            }
            if (expected_batch == corpus.batches.size()) { valid = false; ++observed_records; continue; }
            const auto& expected = corpus.batches[expected_batch];
            const bool header_equal = canonical
                ? canonical_header_matches(observed.header(index), input.records[observed_records], input.alphabet)
                : observed.header(index) == expected.header(expected_row);
            const auto actual_quality = observed.quality(index);
            const auto expected_quality = expected.quality(expected_row);
            valid = valid && header_equal
                && writer_sequences_equal(expected.sequence(expected_row), observed.sequence(index))
                && (IsFasta || (actual_quality.size() == expected_quality.size()
                    && std::equal(actual_quality.begin(), actual_quality.end(), expected_quality.begin())));
            ++expected_row;
            ++observed_records;
        }
    };
    const auto failure = [&input](std::string&& message) {
        bioseq_log<logging_level::error>("writer exact readback failed on '{}': {}", input.output_scratch_path, message);
    };
    const auto parsed = read_sequences_file<Alphabet, format>(input.output_scratch_path, handler, failure,
        writer_validation_options());
    return parsed.has_value() && valid && observed_records == input.source_record_count;
}

/** Every failed exact comparison is nonfinite, so all variants failing cannot satisfy equality rules. */
[[nodiscard]] std::map<std::string, double> evaluate_writer(std::string_view algorithm, const parameter_map&,
        const writer_input& input, const io_measurement& output) {
    bool valid = input.valid && output.bytes_written != 0u && output.observed_record_count == input.source_record_count;
    if (valid) {
        valid = std::visit([&input, algorithm](const auto& corpus) {
            using Alphabet = typename std::remove_cvref_t<decltype(corpus)>::alphabet_type;
            return input.is_fastq
                ? roundtrip_match<Alphabet, false>(input, corpus, canonical_writer_algorithm(algorithm))
                : roundtrip_match<Alphabet, true>(input, corpus, canonical_writer_algorithm(algorithm));
        }, input.corpus);
    }
    if (!valid) bioseq_log<logging_level::error>("writer measurement rejected: algorithm={} alphabet={} records={}",
        algorithm, input.alphabet, output.observed_record_count);
    std::map<std::string, double> metrics{{"bytes_on_disk", static_cast<double>(output.bytes_written)},
        {"record_count", static_cast<double>(output.observed_record_count)},
        {"output_bytes_nonzero", output.bytes_written != 0u ? 1.0 : 0.0},
        {"roundtrip_record_match", valid ? 1.0 : std::numeric_limits<double>::quiet_NaN()}};
    metrics.merge(io_resource_metrics(output.resources));
    return metrics;
}

/** Remove only generated fixture files and this run's private output path. */
void cleanup_writer(std::string_view, std::string_view, simdjson::dom::element, const writer_input& input) {
    std::error_code error;
    if (input.is_synthetic_source && !input.source_corpus_path.empty())
        filesystem::remove(input.source_corpus_path, error);
    if (!input.output_scratch_path.empty()) filesystem::remove(input.output_scratch_path, error);
}

} // namespace bioseq::io::benchmark::detail
