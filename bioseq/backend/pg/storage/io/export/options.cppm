/** @file Decode SQL JSONB options into the existing neutral IO settings. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/jsonb.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
}
export module bioseq.postgres.storage.io.exporting.options;
import std;
import bioseq.io.options;
import bioseq.io.record_schema;
import bioseq.io.file_kind;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.storage.io.exporting.query_binding;

export namespace bioseq::postgres::storage::io::exporting {
/** SQL options decoded once; worker scheduling reuses the existing concurrency model. */
struct export_options {
    bioseq::io::writer_options writer;
    bioseq::io::concurrency_options concurrency;
    bool supplied_header{};
};

/** Borrow a typed string directly from PostgreSQL's already parsed JSONB. */
[[nodiscard]] inline std::string_view export_option_string(
    const JsonbValue& value, std::string_view key) {
    if (value.type != jbvString) export_binding_error("write option \"" + std::string(key) + "\" must be a string");
    return {value.val.string.val, static_cast<std::size_t>(value.val.string.len)};
}

/** Decode exact nonnegative integers without rounding PostgreSQL numeric values. */
[[nodiscard]] inline std::optional<std::uint64_t> export_option_integer(const JsonbValue& value) {
    if (value.type != jbvNumeric) return std::nullopt;
    char* text = bioseq::postgres::support::invoke_pg_as_cpp([&value] {
        return DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(value.val.numeric)));
    });
    const std::string_view input{text};
    std::uint64_t result{};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
    const bool valid = parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size();
    bioseq::postgres::support::invoke_pg_as_cpp([text] { pfree(text); });
    return valid ? std::optional{result} : std::nullopt;
}

/** Visit direct object members without materializing a second JSON document. */
template<class Function>
inline void visit_export_option_object(JsonbContainer& object, Function&& consume) {
    if (!JsonContainerIsObject(&object)) export_binding_error("write options/reference declaration must be an object");
    auto* iterator = bioseq::postgres::support::invoke_pg_as_cpp([&object] { return JsonbIteratorInit(&object); });
    std::string_view key;
    for (;;) {
        JsonbValue value{};
        const auto token = bioseq::postgres::support::invoke_pg_as_cpp([&iterator, &value] {
            return JsonbIteratorNext(&iterator, &value, true);
        });
        if (token == WJB_DONE || token == WJB_END_OBJECT) return;
        if (token == WJB_KEY) key = export_option_string(value, "object key");
        else if (token == WJB_VALUE) consume(key, value);
        else if (token != WJB_BEGIN_OBJECT) export_binding_error("invalid JSONB object iterator token");
    }
}

/** Visit array elements while nested containers stay borrowed binary JSONB views. */
template<class Function>
inline void visit_export_option_array(const JsonbValue& array, Function&& consume) {
    if (array.type != jbvBinary || !JsonContainerIsArray(array.val.binary.data))
        export_binding_error("write option must be a JSON array");
    auto* iterator = bioseq::postgres::support::invoke_pg_as_cpp([&array] { return JsonbIteratorInit(array.val.binary.data); });
    for (;;) {
        JsonbValue value{};
        const auto token = bioseq::postgres::support::invoke_pg_as_cpp([&iterator, &value] {
            return JsonbIteratorNext(&iterator, &value, true);
        });
        if (token == WJB_DONE || token == WJB_END_ARRAY) return;
        if (token == WJB_ELEM) consume(value);
        else if (token != WJB_BEGIN_ARRAY) export_binding_error("invalid JSONB array iterator token");
    }
}

/** Parse one explicit reference declaration with exact positive decimal length. */
[[nodiscard]] inline bioseq::io::reference_descriptor decode_export_reference(const JsonbValue& declaration,
    std::uint64_t identity) {
    if (declaration.type != jbvBinary || !JsonContainerIsObject(declaration.val.binary.data))
        export_binding_error("reference declaration must be an object");
    bioseq::io::reference_descriptor reference;
    reference.identity = identity;
    visit_export_option_object(*declaration.val.binary.data, [&reference](std::string_view key, const JsonbValue& value) {
        if (key == "name") reference.name = export_option_string(value, key);
        else if (key == "length") {
            const auto spelling = export_option_string(value, key);
            const auto converted = std::from_chars(spelling.data(), spelling.data() + spelling.size(), reference.length);
            if (converted.ec != std::errc{} || converted.ptr != spelling.data() + spelling.size())
                export_binding_error("reference length must be a positive decimal string");
        } else if (key == "attributes") {
            if (value.type != jbvBinary || !JsonContainerIsObject(value.val.binary.data))
                export_binding_error("reference attributes must be an object");
            visit_export_option_object(*value.val.binary.data, [&reference](std::string_view tag, const JsonbValue& attribute) {
                if (tag.size() != 2) export_binding_error("reference attributes require two-character tags");
                const auto text = export_option_string(attribute, tag);
                reference.attributes.push_back({{tag[0], tag[1]}, std::string{text}});
                if (tag == "TP") {
                    if (text != "linear" && text != "circular") export_binding_error("reference TP must be linear or circular");
                    reference.circular = text == "circular";
                }
            });
        } else export_binding_error("unknown reference declaration field: " + std::string{key});
    });
    if (reference.name.empty() || reference.length == 0)
        export_binding_error("reference requires nonempty name and positive decimal-string length");
    return reference;
}

/** Decode strict options; unknown keys never silently change export behavior. */
[[nodiscard]] inline export_options decode_export_options(std::optional<Datum> input,
    std::string_view destination) {
    export_options options;
    if (input) {
        auto* document = bioseq::postgres::support::invoke_pg_as_cpp([value = *input] { return DatumGetJsonbP(value); });
        if (!JB_ROOT_IS_OBJECT(document)) export_binding_error("write options must be a JSON object");
        visit_export_option_object(document->root, [&options](std::string_view key, const JsonbValue& value) {
            if (key == "format") {
                const auto name = export_option_string(value, key);
                if (name == "fasta") options.writer.format = bioseq::io::sequence_file_format::fasta;
                else if (name == "fastq") options.writer.format = bioseq::io::sequence_file_format::fastq;
                else if (name == "sam") options.writer.format = bioseq::io::sequence_file_format::sam;
                else if (name == "bam") options.writer.format = bioseq::io::sequence_file_format::bam;
                else export_binding_error("write format must be fasta, fastq, sam or bam");
            } else if (key == "compression") {
                const auto name = export_option_string(value, key);
                if (name == "none") options.writer.compression = bioseq::io::sequence_compression::none;
                else if (name == "gzip") options.writer.compression = bioseq::io::sequence_compression::gzip;
                else if (name == "bgzf") options.writer.compression = bioseq::io::sequence_compression::bgzf;
                else export_binding_error("write compression must be none, gzip or bgzf");
            } else if (key == "compression_level") {
                const auto number = export_option_integer(value);
                if (!number || *number > 9) export_binding_error("compression_level must be an integer from 0 through 9");
                options.writer.compression_level = static_cast<int>(*number);
            } else if (key == "overwrite") {
                if (value.type != jbvBool) export_binding_error("overwrite must be a Boolean");
                options.writer.overwrite = value.val.boolean;
            } else if (key == "fasta_line_width") {
                const auto number = export_option_integer(value);
                if (!number || *number > std::numeric_limits<std::size_t>::max())
                    export_binding_error(std::string{key} + " must be a nonnegative platform-size integer");
                options.writer.fasta_line_width = static_cast<std::size_t>(*number);
            } else if (key == "sequence_orientation") {
                const auto name = export_option_string(value, key);
                if (name == "original_query") options.writer.sequence_orientation = bioseq::io::record_orientation::original_query;
                else if (name == "reference_forward") options.writer.sequence_orientation = bioseq::io::record_orientation::reference_forward;
                else export_binding_error("sequence_orientation must be original_query or reference_forward");
            } else if (key == "cigar_form") {
                const auto name = export_option_string(value, key);
                if (name == "alignment_core") options.writer.alignment_cigar_form = bioseq::io::cigar_form::alignment_core;
                else if (name == "sam_complete") options.writer.alignment_cigar_form = bioseq::io::cigar_form::sam_complete;
                else export_binding_error("cigar_form must be alignment_core or sam_complete");
            } else if (key == "sam_header") {
                options.supplied_header = true;
                visit_export_option_array(value, [&options](const JsonbValue& line) {
                    options.writer.sam_header.emplace_back(export_option_string(line, "sam_header"));
                });
            } else if (key == "references") {
                visit_export_option_array(value, [&options](const JsonbValue& declaration) {
                    options.writer.references.push_back(decode_export_reference(declaration,
                        options.writer.references.size() + 1u));
                });
            } else export_binding_error("unknown write option: " + std::string{key});
        });
    }
    const auto resolved = bioseq::io::resolve_output_format(destination, options.writer);
    if (!resolved) export_binding_error(resolved.error());
    options.writer.format = resolved->format;
    options.writer.compression = resolved->compression;
    return options;
}
} // namespace bioseq::postgres::storage::io::exporting
