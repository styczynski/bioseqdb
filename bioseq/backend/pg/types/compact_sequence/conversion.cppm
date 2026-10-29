/** @file Thin SQL adapters for explicit native biological alphabet conversions. */
module;

#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <type_traits>

#include <bioseq/pg_compat.h>

export module bioseq.postgres.types.compact_sequence:conversion;

import bioseq.codegen.sql.annotation_model;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.translation;
import bioseq.storage.lazy_sequence;
import :flat_abi;
import :view_session;
import :packed_output;

namespace {
using bioseq::sequences::sequence_conversion_error;
using bioseq::sequences::sequence_conversion_error_code;
using conversion_result = std::expected<Datum, sequence_conversion_error>;

/** Identify a named API's required biological direction independently of target packing. */
enum class conversion_direction : std::uint8_t { automatic, translate, back_translate };

} // namespace

/** Report conversion failures only after all conversion and read-session scopes have ended. */
export [[noreturn]] void pg_raise_sequence_conversion_error(
        bioseq::sequences::sequence_conversion_error error) {
    int sqlstate = ERRCODE_INVALID_PARAMETER_VALUE;
    const char* message = "invalid biological sequence conversion";
    switch (error.code) {
        case sequence_conversion_error_code::invalid_frame:
            message = "translation frame must be -3, -2, -1, 1, 2, or 3"; break;
        case sequence_conversion_error_code::invalid_interval:
            message = "biological sequence conversion interval is invalid"; break;
        case sequence_conversion_error_code::unsupported_genetic_code:
            message = "only canonical genetic code 1 is supported"; break;
        case sequence_conversion_error_code::incompatible_alphabets:
            sqlstate = ERRCODE_DATATYPE_MISMATCH;
            message = "sequence alphabets do not support the requested biological conversion"; break;
        case sequence_conversion_error_code::incomplete_codon:
            message = "translation input has an incomplete terminal codon"; break;
        case sequence_conversion_error_code::unrepresentable_symbol:
            message = "source symbol has no representation under the requested conversion"; break;
        case sequence_conversion_error_code::size_overflow:
        case sequence_conversion_error_code::output_capacity:
            sqlstate = ERRCODE_PROGRAM_LIMIT_EXCEEDED;
            message = "converted sequence exceeds the supported output size"; break;
        case sequence_conversion_error_code::allocation_failure:
            sqlstate = ERRCODE_OUT_OF_MEMORY;
            message = "could not allocate converted sequence storage"; break;
        case sequence_conversion_error_code::invalid_source_rank:
            sqlstate = ERRCODE_DATA_CORRUPTED;
            message = "biological sequence contains an invalid stored rank"; break;
        case sequence_conversion_error_code::source_read_failed:
            sqlstate = ERRCODE_DATA_CORRUPTED;
            message = "biological sequence source could not be read"; break;
        case sequence_conversion_error_code::output_contract_violation:
            sqlstate = ERRCODE_INTERNAL_ERROR;
            message = "biological sequence conversion violated its output contract"; break;
    }
    if (error.input_position)
        ereport(ERROR, (errcode(sqlstate), errmsg("%s", message),
            errdetail("Source position: %llu (zero-based).",
                static_cast<unsigned long long>(*error.input_position))));
    ereport(ERROR, (errcode(sqlstate), errmsg("%s", message)));
    pg_unreachable();
}

namespace {

/** Bind a canonical destination name using the existing alphabet registry. */
[[nodiscard]] compact_sequence_stdc_type_params conversion_destination(const std::string& name) {
    const auto code = pgseq_supported_biological_alphabets::try_index_of_name(name.c_str());
    if (!code)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("unsupported biological destination alphabet \"%s\"", name.c_str())));
    compact_sequence_stdc_type_params destination{};
    destination.alphabet_code = *code;
    destination.storage_unit_byte_width = sizeof(std::uint64_t);
    return destination;
}

/** Dispatch alphabet families once and keep every source callback on the backend thread. */
[[nodiscard]] Datum convert_sequence(Datum input, compact_sequence_stdc_type_params destination,
        conversion_direction direction, const bioseq::sequences::translation_options& options) {
    const auto header = classify_and_describe_sequence_header(input);
    if (header.kind == pg_sequence_datum_kind::locator)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    if (!header.alphabet_bound || pgseq_is_cigar_alphabet(header.alphabet_code)
        || pgseq_is_cigar_alphabet(destination.alphabet_code))
        pg_raise_sequence_conversion_error({sequence_conversion_error_code::incompatible_alphabets});

    Datum dispatch = input;
    bioseq::pg_types::seq* detoasted{};
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        detoasted = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(detoasted);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    auto result = with_pg_sequence_session<conversion_result>(
        [dispatch, descriptor, destination, direction, options](pg_sequence_read_session& session) {
        conversion_result converted = std::unexpected(sequence_conversion_error{
            sequence_conversion_error_code::incompatible_alphabets});
        pgseq_supported_biological_alphabets::dispatch_alphabet_by_code(descriptor.alphabet_code,
            [dispatch, descriptor, destination, direction, options, &session, &converted]<class Source>() {
            pgseq_supported_biological_alphabets::dispatch_alphabet_by_code(destination.alphabet_code,
                [dispatch, descriptor, destination, direction, options, &session, &converted]<class Destination>() {
                constexpr bool forward = seqan3::nucleotide_alphabet<Source>
                    && seqan3::aminoacid_alphabet<Destination>;
                constexpr bool backward = seqan3::aminoacid_alphabet<Source>
                    && seqan3::nucleotide_alphabet<Destination>;
                if ((direction == conversion_direction::translate && !forward)
                    || (direction == conversion_direction::back_translate && !backward)) return;

                std::size_t output_count = descriptor.bp_length;
                if constexpr (forward) {
                    const auto geometry = bioseq::sequences::make_translation_geometry(
                        descriptor.bp_length, 0u, descriptor.bp_length, options);
                    if (!geometry) { converted = std::unexpected(geometry.error()); return; }
                    output_count = geometry->output_symbols;
                } else if constexpr (backward) {
                    if (options.code != seqan3::genetic_code::canonical) {
                        converted = std::unexpected(sequence_conversion_error{
                            sequence_conversion_error_code::unsupported_genetic_code}); return;
                    }
                    if (output_count > std::numeric_limits<std::uint32_t>::max() / 3u) {
                        converted = std::unexpected(sequence_conversion_error{
                            sequence_conversion_error_code::size_overflow}); return;
                    }
                    output_count *= 3u;
                }
                if (output_count > std::numeric_limits<std::uint32_t>::max()) {
                    converted = std::unexpected(sequence_conversion_error{
                        sequence_conversion_error_code::size_overflow}); return;
                }
                auto* owner = make_pg_sequence_call_owner<Source>(session, dispatch, descriptor);
                const auto view = owner->view();
                pg_sequence_expression_backend<Destination> backend{session};
                auto output = backend.template generate<sequence_conversion_error>(
                    output_count, destination, [view, options, descriptor](auto& writer, auto units) {
                    using writer_type = std::remove_cvref_t<decltype(writer)>;
                    using instance = sequence_custom_alphabet<Destination, typename writer_type::storage_unit>;
                    const auto source = [view, options] {
                        // Reader capability depends only on Source. Destination remains
                        // dependent while the outer alphabet dispatch is instantiated.
                        if constexpr (seqan3::nucleotide_alphabet<Source>) {
                            if (options.frame < 0) return view.template borrow_rank_source<true>();
                        }
                        return view.template borrow_rank_source<false>();
                    }();
                    // Flat callbacks only extract retained packed words. Tree callbacks
                    // can reach PostgreSQL source sessions, so remain on this thread.
                    const bool parallel = source.worker_shareable
                        && descriptor.kind == pg_sequence_datum_kind::flat_bound;
                    return bioseq::sequences::convert_packed_into<Source, instance>(
                        source, 0u, view.bp_size(), options, units, parallel);
                });
                if (!output) { converted = std::unexpected(output.error()); return; }
                converted = backend.publish(*output);
            });
        });
        return converted;
    });
    if (detoasted != nullptr && reinterpret_cast<Pointer>(detoasted) != DatumGetPointer(input))
        pfree(detoasted);
    if (!result) pg_raise_sequence_conversion_error(result.error());
    return *result;
}
} // namespace

/** Explicit typed sequence casts share native conversion with the named SQL operations. */
export [[nodiscard]] Datum pg_convert_sequence(Datum input,
        compact_sequence_stdc_type_params destination) {
    return convert_sequence(input, destination, conversion_direction::automatic, {});
}

export namespace bioseq::pg_types::sql_routines {

/** Translate all complete codons in a requested reading frame, preserving stop positions. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum translate(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum source,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'aa27'"}})]] const std::string& target_alphabet,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"1"}})]] std::int32_t frame,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"1"}})]] std::int32_t genetic_code,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'reject'"}})]] const std::string& trailing_policy) {
    if (frame == 0 || frame < -3 || frame > 3)
        pg_raise_sequence_conversion_error({sequence_conversion_error_code::invalid_frame});
    if (genetic_code != static_cast<std::int32_t>(seqan3::genetic_code::canonical))
        pg_raise_sequence_conversion_error({sequence_conversion_error_code::unsupported_genetic_code});
    if (trailing_policy != "reject" && trailing_policy != "drop")
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("translation trailing policy must be 'reject' or 'drop'")));
    const bioseq::sequences::translation_options options{
        .frame = static_cast<std::int8_t>(frame), .code = seqan3::genetic_code::canonical,
        .trailing = trailing_policy == "drop" ? bioseq::sequences::incomplete_codon_policy::drop
                                      : bioseq::sequences::incomplete_codon_policy::reject};
    return convert_sequence(source, conversion_destination(target_alphabet), conversion_direction::translate, options);
}

/** Synthesize canonical representative codons; this does not recover original codon choices. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum back_translate(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum source,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'dna4'"}})]] const std::string& target_alphabet,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"1"}})]] std::int32_t genetic_code) {
    if (genetic_code != static_cast<std::int32_t>(seqan3::genetic_code::canonical))
        pg_raise_sequence_conversion_error({sequence_conversion_error_code::unsupported_genetic_code});
    return convert_sequence(source, conversion_destination(target_alphabet), conversion_direction::back_translate, {});
}
} // namespace bioseq::pg_types::sql_routines
