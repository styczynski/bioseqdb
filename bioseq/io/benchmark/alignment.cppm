/** @file Matched SAM/BAM IO comparison against the pinned samtools HTSlib. */
module;
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <new>
#include <unistd.h>
#include <htslib/sam.h>
#include <htslib/hts.h>
#include <simdjson.h>

module bioseq.io.benchmark:alignment;
import std;
import :common;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.gpu.vulkan.shader_cache;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.simd.utils;
import bioseq.io.record_schema;
import bioseq.io.reader;
import bioseq.io.write_pipeline;
import bioseq.io.alignment;
import bioseq.io.identifier_parser.preservation;
import bioseq.storage.lazy_sequence_view;
import bioseq.storage.size;
import bioseq.parallelism.pool;

namespace bioseq::io::benchmark::detail {

/** Retained original and native representations prepared outside measured writer runs. */
struct alignment_fixture {
    std::string alphabet;
    std::size_t cardinality{};
    std::string sam_path, bam_path, output_path;
    std::vector<std::unique_ptr<decoded_record>> original;
    std::vector<bam1_t*> native;
    sam_hdr_t* header{};
    record_source_metadata metadata;
    bool valid{};
    /** Release the baseline library's owned records and header outside measured loops. */
    ~alignment_fixture() {
        for (auto* record : native) bam_destroy1(record);
        if (header) sam_hdr_destroy(header);
    }
    alignment_fixture() = default;
    alignment_fixture(const alignment_fixture&) = delete;
};

/** Validate publication and decode every native record with the pinned CLI outside timing. */
[[nodiscard]] std::expected<void, std::string> validate_alignment_cli(
        const std::string& path, std::size_t expected_records) {
    const bioseq::gpu::vulkan::run_capture_limits limits{
        .maximum_output_bytes = 64u << 10u};
    const std::array<std::string, 5> quickcheck_arguments{
        BIOSEQ_SAMTOOLS_EXECUTABLE, "quickcheck", "-v", "--", path};
    const auto checked = bioseq::gpu::vulkan::run_capture(quickcheck_arguments, limits);
    if (!checked) return std::unexpected("samtools quickcheck for " + path + ": " + checked.error());
    // quickcheck checks headers/EOF only. view -c traverses and decodes the
    // complete stream, including BGZF CRC and record framing, without a large capture.
    const std::array<std::string, 5> view_arguments{
        BIOSEQ_SAMTOOLS_EXECUTABLE, "view", "-c", "--", path};
    const auto counted = bioseq::gpu::vulkan::run_capture(view_arguments, limits);
    if (!counted) return std::unexpected("samtools view for " + path + ": " + counted.error());
    std::string_view count_text{*counted};
    while (!count_text.empty() && (count_text.back() == '\n' || count_text.back() == '\r'))
        count_text.remove_suffix(1);
    std::uint64_t count{};
    const auto parsed = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != count_text.data() + count_text.size()
        || count != expected_records)
        return std::unexpected("samtools view count for " + path + " differs from "
            + std::to_string(expected_records) + ": " + *counted);
    return {};
}

/** All selected alphabets share one benchmark case and the same alignment geometry. */
struct alignment_benchmark_input {
    std::map<std::string, std::shared_ptr<alignment_fixture>> fixtures;
    std::size_t record_count{}, record_length{};
};

/** Timing is supplied by the framework; these counters prove work and file validity. */
struct alignment_measurement {
    std::size_t records{}, bytes{};
    bool valid{};
    std::string error;
};

/** Apply the native dispatcher accounting and log the shared untimed arena budget. */
[[nodiscard]] io_pool_scope prepare_alignment_iteration(std::string_view algorithm,
        const parameter_map& parameters, std::size_t repeat) {
    const auto requested = parameters.at("workers").as_integer();
    if (requested != 1 && requested < 3)
        return io_pool_scope{std::string{"alignment worker budget is unsupported"}};
    io_pool_scope preparation{requested};
    if (!preparation.error.empty()) return preparation;
    auto& pool = parallelism::global_pool::instance();
    const auto workers = pool.maximum_concurrency();
    // preparation already owns cleanup if parameter lookup or logging throws.
    bioseq_log<logging_level::info>(
        "alignment budget: algorithm={} alphabet={} format={} operation={} repeat={} "
        "requested_workers={} pool_limit={} framework_limit={} hts_worker_threads={} "
        "hts_dispatch_threads={} caller_threads=1 active_workers=unobserved",
        algorithm, parameters.at("alphabet").as_string(), parameters.at("format").as_string(),
        parameters.at("operation").as_string(), repeat, workers, pool.maximum_concurrency(),
        preparation.framework_limit, workers == 1u ? 0u : workers - 2u, workers == 1u ? 0u : 1u);
    return preparation;
}

/** Borrow admitted immutable rows; the fixture retains them for the complete export. */
struct alignment_fixture_batch {
    const alignment_fixture* fixture{};
    std::size_t begin{}, count{};
    /** Number of borrowed rows in this batch. */
    [[nodiscard]] std::size_t size() const { return count; }
    /** Return a stable original record view. */
    [[nodiscard]] export_record_view row(std::size_t index) const { return fixture->original[begin + index]->record; }
    /** Decoded owners already retain their shared-memory permits. */
    [[nodiscard]] std::size_t accounted_capacity_bytes() const { return 0; }
};

/** A single forward traversal over pre-materialized source records. */
struct alignment_fixture_source {
    using batch_type = alignment_fixture_batch;
    const alignment_fixture& fixture;
    std::size_t position{};
    /** Known source types are carried by the manifest metadata. */
    [[nodiscard]] std::span<const record_column> columns() const { return {}; }
    /** Borrow original dictionaries without adding native header or payload copies. */
    [[nodiscard]] const record_source_metadata& metadata() const { return fixture.metadata; }
    /** Issue one bounded row batch, never executing a source twice. */
    [[nodiscard]] std::expected<std::optional<batch_type>, record_error> next() {
        if (position == fixture.original.size()) return std::optional<batch_type>{};
        const auto count = std::min<std::size_t>(512, fixture.original.size() - position);
        batch_type batch{&fixture, position, count};
        position += count;
        return std::optional{batch};
    }
    /** Cancellation stops future batches; the fixture owns all borrowed storage. */
    void cancel() { position = fixture.original.size(); }
};
static_assert(export_record_source<alignment_fixture_source>);

/** Generate only requested reference ranks; the synthetic fixture has a defined periodic reference. */
[[nodiscard]] const std::uint8_t* read_alignment_reference(const void* context,
        bioseq::storage::sequence_size_t offset, std::size_t count, std::span<std::uint8_t> scratch) {
    const auto& fixture = *static_cast<const alignment_fixture*>(context);
    for (std::size_t index = 0; index < count; ++index)
        scratch[index] = static_cast<std::uint8_t>((offset + index) % fixture.cardinality);
    return scratch.data();
}

/** Build true matches, one insertion/deletion/mismatch, and real soft clipping. */
[[nodiscard]] std::expected<std::unique_ptr<decoded_record>, std::string> make_alignment_record(
        std::uint8_t alphabet, std::size_t cardinality, std::size_t ordinal, std::size_t length,
        const bioseq::storage::borrowed_rank_window_source& reference) {
    auto record = std::make_unique<decoded_record>();
    record->record.ordinal = ordinal;
    record->record.reference_sequence = reference;
    const auto name = "read_" + std::to_string(ordinal);
    if (auto field = record->field("header", name); !field) return std::unexpected(field.error());
    if (auto field = record->field("qname", name); !field) return std::unexpected(field.error());
    record->record.identity.template_id = ordinal + 1;
    record->record.identity.segment_id = ordinal + 1;
    record->record.identity.reference_id = 1;
    record->record.identity.classification = alignment_classification_source::explicit_flags;
    record->record.alignment.emplace();
    auto& alignment = *record->record.alignment;
    alignment.flags = 0;
    alignment.mapping_quality = 60;
    alignment.reference_name = "reference";
    alignment.reference_length = 100000000;
    alignment.query_length = length;
    alignment.query_start = 3;
    alignment.query_end = length;
    alignment.target_start = (ordinal * 17u) % (100000000 - length);
    alignment.target_end = *alignment.target_start + length - 3;
    alignment.sequence_orientation = record_orientation::reference_forward;
    alignment.form = cigar_form::sam_complete;
    if (auto context = record->field("header_context", "1"); !context)
        return std::unexpected(context.error());
    const std::array<std::pair<char, std::uint64_t>, 7> operations{{
        {'S', 3}, {'=', 20}, {'I', 1}, {'=', 20}, {'D', 1}, {'X', 1}, {'=', length - 45}}};
    if (auto begun = record->begin_cigar(true); !begun) return std::unexpected(begun.error());
    std::vector<std::uint8_t> ranks;
    ranks.reserve(length);
    auto reference_position = *alignment.target_start;
    for (const auto [operation, count] : operations) {
        const cigar_run run{static_cast<std::uint32_t>(count), seqan3::assign_char_to(operation, cigar_symbol{})};
        if (auto appended = record->append_cigar_run(run); !appended) return std::unexpected(appended.error());
        for (std::uint64_t index = 0; index < count; ++index) {
            if (operation != 'D') ranks.push_back(static_cast<std::uint8_t>(
                (reference_position + (operation == 'X' ? 1u : 0u)) % cardinality));
            if (operation != 'I' && operation != 'S') ++reference_position;
        }
    }
    if (auto ended = record->end_cigar(); !ended) return std::unexpected(ended.error());
    const auto quality_code = supported_storage_alphabets::index_of<seq::phred42>();
    for (const auto role : {record_field_role::sequence, record_field_role::quality}) {
        const preserved_sequence_descriptor descriptor{
            role == record_field_role::sequence ? alphabet : quality_code, true, length, preservation_storage::rank_hex};
        if (auto begun = record->begin_sequence(role, descriptor); !begun) return std::unexpected(begun.error());
        if (role == record_field_role::quality)
            for (std::size_t index = 0; index < ranks.size(); ++index) ranks[index] = static_cast<std::uint8_t>((ordinal + index) % 42);
        if (auto appended = record->append_sequence_ranks(role, ranks); !appended) return std::unexpected(appended.error());
        if (auto ended = record->end_sequence(role); !ended) return std::unexpected(ended.error());
    }
    if (auto metadata = record->metadata("sample", "text", "alignment benchmark", false); !metadata)
        return std::unexpected(metadata.error());
    const auto frozen = record->freeze();
    if (!frozen) return std::unexpected(frozen.error());
    return record;
}

/** Estimate original compact records, baseline native records and bounded worker slabs. */
[[nodiscard]] run_constraints estimate_alignment_resources(std::string_view, std::string_view,
        simdjson::dom::element data) {
    const auto rows = static_cast<std::size_t>(read_int_field(data, "rows", 4096));
    const auto length = static_cast<std::size_t>(read_int_field(data, "length", 150));
    return {.base_memory = rows * (length * 16 + 8192) * supported_biological_alphabets::alphabet_count,
        .extra_memory = 64u * 1024u * 1024u};
}

/** Produce matched SAM/BAM fixtures and retain HTSlib's native records outside timing. */
[[nodiscard]] std::shared_ptr<alignment_fixture> build_alignment_fixture(std::string_view directory,
        std::string alphabet, std::size_t rows, std::size_t length) {
    auto fixture = std::make_shared<alignment_fixture>();
    fixture->alphabet = alphabet;
    const auto prefix = std::string{directory} + "/bioseq_alignment_" + std::to_string(getpid()) + "_" + alphabet;
    fixture->sam_path = prefix + ".sam";
    fixture->bam_path = prefix + ".bam";
    fixture->output_path = prefix + "_output";
    const auto code = supported_biological_alphabets::try_index_of_name(alphabet.c_str());
    if (!code || length < 46 || length >= 100000000) return fixture;
    const auto cardinality = supported_biological_alphabets::cardinality_for_code(*code);
    if (!cardinality) return fixture;
    fixture->cardinality = *cardinality;
    const bioseq::storage::borrowed_rank_window_source reference{
        .context = fixture.get(), .read_at = read_alignment_reference,
        .symbol_count = 100000000, .alphabet_code = *code, .worker_shareable = true};
    fixture->metadata.declared_alphabet_codes.push_back(*code);
    fixture->metadata.declared_quality_alphabet_codes.push_back(supported_storage_alphabets::index_of<seq::phred42>());
    fixture->metadata.header_contexts.push_back({1, {"@HD\tVN:1.6\tSO:unknown"}});
    fixture->metadata.references.push_back({.identity = 1, .name = "reference", .length = 100000000,
        .alphabet_code = *code, .context_id = 1});
    fixture->metadata.reference_dictionary_complete = true;
    fixture->original.reserve(rows);
    for (std::size_t ordinal = 0; ordinal < rows; ++ordinal) {
        auto record = make_alignment_record(*code, *cardinality, ordinal, length, reference);
        if (!record) { bioseq_log<logging_level::error>("alignment fixture: {}", record.error()); return fixture; }
        fixture->original.push_back(std::move(*record));
    }
    for (const auto format : {sequence_file_format::sam, sequence_file_format::bam}) {
        alignment_fixture_source source{*fixture};
        writer_options options;
        options.format = format;
        options.overwrite = true;
        options.compression_level = 6;
        const auto written = write_records(source, format == sequence_file_format::sam ? fixture->sam_path : fixture->bam_path, options);
        if (!written || *written != rows) {
            bioseq_log<logging_level::error>("alignment fixture writer: {}", written ? "record count mismatch" : written.error().message);
            return fixture;
        }
        const auto validated = validate_alignment_cli(
            format == sequence_file_format::sam ? fixture->sam_path : fixture->bam_path, rows);
        if (!validated) {
            bioseq_log<logging_level::error>("alignment fixture CLI validation: {}", validated.error());
            return fixture;
        }
    }
    samFile* input = sam_open(fixture->sam_path.c_str(), "r");
    if (!input) return fixture;
    fixture->header = sam_hdr_read(input);
    if (!fixture->header) { sam_close(input); return fixture; }
    for (std::size_t ordinal = 0; ordinal < rows; ++ordinal) {
        auto* record = bam_init1();
        if (!record || sam_read1(input, fixture->header, record) < 0) {
            if (record) bam_destroy1(record);
            sam_close(input);
            return fixture;
        }
        fixture->native.push_back(record);
    }
    fixture->valid = sam_close(input) == 0;
    bioseq_log<logging_level::info>("alignment baseline: HTSlib {} features {}; alphabet={}; "
        "bioseq_backend={}; bam_public_level=6; bam_effective_libdeflate_level=7",
        hts_version(), hts_feature_string(), alphabet, alignment::bgzf_compression_backend);
    return fixture;
}

/** Materialize only selected alphabet fixtures for this one comparison case. */
[[nodiscard]] alignment_benchmark_input build_alignment_input(std::string_view directory, std::string_view,
        simdjson::dom::element data, const run_constraints& constraints) {
    alignment_benchmark_input input;
    input.record_count = static_cast<std::size_t>(read_int_field(data, "rows", 4096));
    input.record_length = static_cast<std::size_t>(read_int_field(data, "length", 150));
    for (const auto& tuple : constraints.execution_tuples) {
        const auto alphabet = std::string{tuple.params.at("alphabet").as_string()};
        if (!input.fixtures.contains(alphabet))
            input.fixtures.emplace(alphabet, build_alignment_fixture(directory, alphabet, input.record_count, input.record_length));
    }
    return input;
}

/** Count full decoded records; original-field equality is checked outside timed reads. */
struct alignment_count_consumer {
    std::size_t count{};
    /** The reader still validates the complete file dictionary. */
    [[nodiscard]] std::expected<void, std::string> header(const record_source_metadata&) { return {}; }
    /** Private decoding/validation has completed before this ordered callback. */
    [[nodiscard]] std::expected<void, std::string> record(decoded_record&) { ++count; return {}; }
};

/** Execute one matched baseline or Bioseq load/write, with identical worker budget and level. */
[[nodiscard]] alignment_measurement run_alignment(std::string_view algorithm, const parameter_map& parameters,
        const alignment_benchmark_input& input) {
    const auto& fixture = *input.fixtures.at(std::string{parameters.at("alphabet").as_string()});
    const auto format = parameters.at("format").as_string() == "bam" ? sequence_file_format::bam : sequence_file_format::sam;
    const bool reading = parameters.at("operation").as_string() == "read";
    const auto workers = static_cast<std::size_t>(parameters.at("workers").as_integer());
    const bool bioseq = algorithm == "bioseq_alignment";
    alignment_measurement result;
    if (!fixture.valid) { result.error = "fixture preparation failed"; return result; }
    const auto& path = format == sequence_file_format::bam ? fixture.bam_path : fixture.sam_path;
    const auto output = fixture.output_path + (format == sequence_file_format::bam ? ".bam" : ".sam");
    if (reading && bioseq) {
        alignment_count_consumer consumer;
        reader_options options;
        options.concurrency.worker_count = workers;
        options.io.parallel_gzip_thread_count = workers;
        const auto read = read_alignment_records(path, format, consumer, options);
        if (!read) { result.error = read.error(); return result; }
        result.records = consumer.count;
        result.valid = result.records == input.record_count;
    } else if (!reading && bioseq) {
        alignment_fixture_source source{fixture};
        writer_options options;
        options.format = format; options.overwrite = true; options.compression_level = 6;
        concurrency_options concurrency; concurrency.worker_count = workers;
        const auto written = write_records(source, output, options, concurrency);
        if (!written) { result.error = written.error().message; return result; }
        result.records = *written;
        result.valid = result.records == input.record_count;
    } else {
        samFile* file = sam_open((reading ? path : output).c_str(), reading ? "r" : format == sequence_file_format::bam ? "wb6" : "w");
        if (!file) { result.error = "HTSlib open failed"; return result; }
        // Pinned HTSlib adds one I/O/dispatcher thread besides its worker pool
        // (BGZF and SAM), so reserve both that thread and the calling thread.
        bool valid = workers <= 1 || hts_set_threads(file, static_cast<int>(workers - 2)) == 0;
        if (reading) {
            auto* header = sam_hdr_read(file);
            auto* record = bam_init1();
            int status = -2;
            if (header && record && valid) {
                while ((status = sam_read1(file, header, record)) >= 0) ++result.records;
            }
            valid = valid && status == -1;
            if (record) bam_destroy1(record);
            if (header) sam_hdr_destroy(header);
        } else {
            valid = valid && sam_hdr_write(file, fixture.header) == 0;
            for (const auto* record : fixture.native) {
                if (!valid || sam_write1(file, fixture.header, record) < 0) { valid = false; break; }
                ++result.records;
            }
        }
        result.valid = sam_close(file) == 0 && valid && result.records == input.record_count;
    }
    std::error_code error;
    result.bytes = std::filesystem::file_size(reading ? path : output, error);
    if (error) { result.valid = false; result.error = error.message(); }
    return result;
}

/** Compare native semantic text from HTSlib, including all mandatory and auxiliary fields. */
[[nodiscard]] bool compare_alignment_output(const alignment_fixture& fixture, const std::string& path) {
    samFile* file = sam_open(path.c_str(), "r");
    if (!file) return false;
    auto* header = sam_hdr_read(file);
    auto* record = bam_init1();
    bool equal = header && record;
    if (equal) equal = sam_hdr_length(fixture.header) == sam_hdr_length(header)
        && std::string_view{sam_hdr_str(fixture.header), sam_hdr_length(fixture.header)}
            == std::string_view{sam_hdr_str(header), sam_hdr_length(header)};
    kstring_t expected{}, observed{};
    for (std::size_t index = 0; equal && index < fixture.native.size(); ++index) {
        expected.l = 0; observed.l = 0;
        equal = sam_read1(file, header, record) >= 0
            && sam_format1(fixture.header, fixture.native[index], &expected) >= 0
            && sam_format1(header, record, &observed) >= 0
            && expected.l == observed.l
            && std::string_view{expected.s, expected.l} == std::string_view{observed.s, observed.l};
    }
    if (equal) equal = sam_read1(file, header, record) == -1;
    std::free(expected.s); std::free(observed.s);
    if (record) bam_destroy1(record);
    if (header) sam_hdr_destroy(header);
    return sam_close(file) == 0 && equal;
}

/** Small benchmark-only sink; fixture records are bounded independently of production limits. */
struct alignment_comparison_sink {
    std::string bytes;
    /** Preserve every byte of the canonical original-field description. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view text) {
        bytes.append(text);
        return {};
    }
};

/** Check restored sequence ranks, qualities, identities, alignment and metadata outside timing. */
struct alignment_comparison_consumer {
    const alignment_fixture& fixture;
    std::size_t position{};
    /** Validate original dictionaries and source alphabet declarations. */
    [[nodiscard]] std::expected<void, std::string> header(const record_source_metadata& metadata) {
        if (metadata.declared_alphabet_codes != fixture.metadata.declared_alphabet_codes
            || metadata.declared_quality_alphabet_codes != fixture.metadata.declared_quality_alphabet_codes
            || metadata.header_contexts.size() != fixture.metadata.header_contexts.size()
            || metadata.references.size() != fixture.metadata.references.size())
            return std::unexpected("restored manifest shape differs");
        for (std::size_t index = 0; index < metadata.header_contexts.size(); ++index) {
            const auto& expected = fixture.metadata.header_contexts[index];
            const auto& observed = metadata.header_contexts[index];
            if (expected.id != observed.id || expected.original_header_lines != observed.original_header_lines)
                return std::unexpected("restored header context differs");
        }
        for (std::size_t index = 0; index < metadata.references.size(); ++index) {
            const auto& expected = fixture.metadata.references[index];
            const auto& observed = metadata.references[index];
            if (expected.identity != observed.identity || expected.name != observed.name
                || expected.length != observed.length || expected.alphabet_code != observed.alphabet_code
                || expected.context_id != observed.context_id || expected.circular != observed.circular
                || expected.attributes.size() != observed.attributes.size())
                return std::unexpected("restored reference differs");
            for (std::size_t attribute = 0; attribute < expected.attributes.size(); ++attribute)
                if (expected.attributes[attribute].tag != observed.attributes[attribute].tag
                    || expected.attributes[attribute].value != observed.attributes[attribute].value)
                    return std::unexpected("restored reference attribute differs");
        }
        return {};
    }
    /** Compare exact canonical originals, including compact native ranks, after private validation. */
    [[nodiscard]] std::expected<void, std::string> record(decoded_record& record) {
        if (position >= fixture.original.size()) return std::unexpected("unexpected extra record");
        alignment_comparison_sink expected, observed;
        const identifier_parser::preservation_options options{
            .sequence_storage = preservation_storage::rank_hex,
            .quality_storage = preservation_storage::rank_hex};
        if (auto encoded = identifier_parser::append_preservation_record(expected,
                fixture.original[position]->record, options); !encoded) return encoded;
        if (auto encoded = identifier_parser::append_preservation_record(observed,
                record.record, options); !encoded) return encoded;
        if (expected.bytes != observed.bytes) return std::unexpected("restored original record differs");
        ++position;
        return {};
    }
};

/** Check original and projected fields outside timing; failed evidence is a blocking nonfinite sample. */
[[nodiscard]] std::map<std::string, double> evaluate_alignment(std::string_view,
        const parameter_map& parameters, const alignment_benchmark_input& input,
        const alignment_measurement& output) {
    const auto& fixture = *input.fixtures.at(std::string{parameters.at("alphabet").as_string()});
    const auto format = parameters.at("format").as_string() == "bam" ? sequence_file_format::bam : sequence_file_format::sam;
    const auto path = parameters.at("operation").as_string() == "write"
        ? fixture.output_path + (format == sequence_file_format::bam ? ".bam" : ".sam")
        : format == sequence_file_format::bam ? fixture.bam_path : fixture.sam_path;
    bool valid = output.valid && compare_alignment_output(fixture, path);
    std::string error = output.error;
    if (valid) {
        const auto validated = validate_alignment_cli(path, input.record_count);
        valid = validated.has_value();
        if (!validated) error = validated.error();
    }
    if (valid) {
        alignment_comparison_consumer consumer{fixture};
        const auto restored = read_alignment_records(path, format, consumer);
        valid = restored && consumer.position == input.record_count;
        if (!restored) error = restored.error();
    }
    if (!valid) bioseq_log<logging_level::error>("alignment IO validation failed: {}", error);
    return {{"record_count", static_cast<double>(output.records)}, {"bytes_on_disk", static_cast<double>(output.bytes)},
        {"full_record_match", valid ? 1.0 : std::numeric_limits<double>::quiet_NaN()}};
}

/** Remove only generated benchmark data; pinned source and build artifacts are retained. */
void cleanup_alignment(std::string_view, std::string_view, simdjson::dom::element,
        const alignment_benchmark_input& input) {
    for (const auto& [alphabet, fixture] : input.fixtures) {
        std::error_code error;
        for (const auto& path : {fixture->sam_path, fixture->bam_path,
                fixture->output_path + ".sam", fixture->output_path + ".bam"}) std::filesystem::remove(path, error);
    }
}
} // namespace bioseq::io::benchmark::detail
