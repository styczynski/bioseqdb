/**
 * @file io.cppm
 * Read and write sequence text and binary data, and handle alphabet and width casts.
 */
module;

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <bioseq/pg_compat.h>
extern "C" {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"
#include "libpq/pqformat.h"
#pragma GCC diagnostic pop
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:io;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :tree_types;
import :tree_ops;
import :view_session;
import :packed_output;
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.sequences.rank_pack;
import bioseq.sequences.simd;
import bioseq.sequences.cigar;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;

/**
 * @brief Decode the alphabet and packing width stored in a sequence type modifier.
 * @param typmod Nonnegative PostgreSQL type modifier.
 * @return Parameters adjusted for  this build and checked against its supported types.
 */
export [[nodiscard]] compact_sequence_stdc_type_params
decode_seq_typmod_checked(int32_t typmod)
{
    if (typmod < 0 || (static_cast<std::uint32_t>(typmod) & 0xff000000u) != 0u
        || (typmod & 0xff) > 3
        || ((typmod >> 16) & 0xff)
            >= static_cast<int>(bioseq::cigar_space::count)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid bioseq.seq destination typmod %d", typmod)));
    }
    auto params = pgseq_normalize_type_params(
        compact_sequence_stdc_type_params::decode(typmod));
    if (!is_supported_alphabet_code(params.alphabet_code)
        || !pgseq_supported_storage_unit_byte_widths::contains_runtime(
            params.storage_unit_byte_width)
        || (pgseq_is_cigar_alphabet(params.alphabet_code)
            ? params.storage_unit_byte_width != sizeof(bioseq::cigar_run)
            : params.space != bioseq::cigar_space::compatibility)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid bioseq.seq typmod %d", typmod)));
    }
    return params;
}

/**
 * @brief Parses a supported alphabet name and reports invalid names through PostgreSQL.
 * @param alphabet_name C string naming the alphabet.
 * @param type_name SQL type label used in error messages.
 * @return Compiled alphabet code for the name.
 */
export [[nodiscard]] std::uint8_t parse_alphabet_name_checked(
    char const* alphabet_name,
    char const* type_name)
{
    const std::string_view requested_name{alphabet_name};
    if ((requested_name == "cigar" || requested_name.starts_with("cigar-"))
        && bioseq::cigar_space_from_name(requested_name).has_value()) {
        return pgseq_cigar_alphabet_code;
    }
    const auto code =
        pgseq_supported_alphabets::try_index_of_name(alphabet_name);
    if (!code.has_value()) {
        constexpr auto valid =
            pgseq_supported_alphabets::valid_alphabet_names_csv();
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s alphabet \"%s\" is not supported",
                        type_name, alphabet_name),
                 errhint("Valid alphabets: %.*s",
                         static_cast<int>(valid.size()), valid.data())));
    }
    return static_cast<std::uint8_t>(*code);
}

/**
 * @brief Return the name of an alphabet enabled in this build.
 * @param alphabet_code Alphabet code.
 * @return A string view owned by the compiled alphabet table.
 */
export [[nodiscard]] std::string_view supported_alphabet_name(
    std::uint8_t alphabet_code) noexcept
{
    return pgseq_supported_alphabets::name_of_alphabet_code(alphabet_code);
}

/**
 * @brief Convert a symbol to its alphabet rank through a shared function.
 * @param alphabet_code Alphabet to use.
 * @param input Input byte.
 * @param rank Rank written on success.
 * @param canonical Optional normalized character written on, success.
 * @return Whether the alphabet accepts the symbol.
 */
export [[nodiscard]] bool pgseq_rank_for_symbol_out_of_line(
    std::uint8_t alphabet_code,
    unsigned char input,
    std::uint8_t& rank,
    char* canonical = nullptr) noexcept
{
    return pgseq_rank_for_symbol(
        alphabet_code, input, rank, canonical);
}

/**
 * @brief Convert an alphabet rank to a character through a shared function.
 * @param alphabet_code Alphabet to use.
 * @param rank Rank to  convert.
 * @param symbol Normalized character written on success.
 * @return Whether the alphabet and rank are valid.
 */
export [[nodiscard]] bool pgseq_symbol_for_rank_out_of_line(
    std::uint8_t alphabet_code,
    std::uint8_t rank,
    char& symbol) noexcept
{
    return pgseq_symbol_for_rank(alphabet_code, rank, symbol);
}

namespace {

/**
 * @brief Validate and copy text into a sequence with no alphabet chosen yet.
 * @param input ASCII symbols to chpy.
 * @param length Number of input bytes.
 * @param require_already_canonical Reject input that needs case or spelling normalization.
 * @param invalid_sqlstate SQL error code for invalid symbols.
 * @return A flat sequence allocated in the current memory context
 */
[[nodiscard]] inline bioseq::pg_types::seq* allocate_unbound_ascii(
    char const* input,
    std::size_t length,
    bool require_already_canonical,
    int invalid_sqlstate = ERRCODE_INVALID_TEXT_REPRESENTATION)
{
    if (length > std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq.seq input length %zu exceeds uint32", length)));
    }
    if (length > std::numeric_limits<std::size_t>::max() - 7u) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq.seq input size overflows size_t")));
    }
    const std::size_t total = bioseq::pg_types::seq::header_bytes()
        + round_up_to_uint64(length);
    if (total > MaxAllocSize) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq.seq input length %zu exceeds MaxAllocSize",
                        length)));
    }
    bioseq::pg_types::seq* result = pg_alloc(total);
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory constructing unbound bioseq.seq")));
    }
    SET_VARSIZE(result, total);
    result->alphabet_code = pgseq_unbound_alphabet_code;
    result->storage_unit_byte_width = pgseq_unbound_width_tag;
    result->padding_letters = 0u;
    result->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(length);
    result->encoding = bioseq::sequence_encoding::rank_packed;
    result->space = bioseq::cigar_space::compatibility;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char raw = static_cast<unsigned char>(input[index]);
        const auto canonical = pgseq_canonical_unbound_symbol(raw);
        if (!canonical.valid
            || (require_already_canonical
                && static_cast<unsigned char>(canonical.value) != raw)) {
            pfree(result);
            ereport(ERROR,
                    (errcode(invalid_sqlstate),
                     errmsg("invalid symbol at byte %zu in unbound bioseq.seq",
                            index),
                     errdetail("byte value=%u", static_cast<unsigned>(raw))));
        }
        result->data[index] = static_cast<std::uint8_t>(canonical.value);
    }
    return result;
}

/** Probe numeric prefixes for complete CIGAR grammar; preserve ordinary alphabet diagnostics. */
[[nodiscard]] inline bioseq::pg_types::seq* allocate_sequence_text(
    char const* input,
    std::size_t length,
    bool require_already_canonical)
{
    if (length != 0u && input[0] >= '0' && input[0] <= '9') {
        const std::string_view text{input, length};
        const auto measured = bioseq::measure_cigar_text(text);
        if (measured) {
            return make_pg_cigar_from_text(text, bioseq::cigar_space::compatibility,
                ERRCODE_INVALID_TEXT_REPRESENTATION, *measured);
        }
        bool ordinary_spelling = true;
        for (std::size_t index = 0u; index < length; ++index) {
            if (!pgseq_canonical_unbound_symbol(
                    static_cast<unsigned char>(input[index])).valid) {
                ordinary_spelling = false;
                break;
            }
        }
        if (!ordinary_spelling) {
            pg_raise_cigar_error(measured.error());
        }
    }
    return allocate_unbound_ascii(input, length, require_already_canonical);
}

/**
 * @brief Validate letters against the target alphabet and pack them into a sequence.
 * @param ascii Input symbols.
 * @param length Number of symbols.
 * @param target Destination  alphabet and storage width.
 * @return A flat sequence allocated in the current memory context.
 */
[[nodiscard]] inline bioseq::pg_types::seq* pack_ascii_for_target(
    char const* ascii,
    std::size_t length,
    compact_sequence_stdc_type_params target)
{
    if (pgseq_is_cigar_alphabet(target.alphabet_code)) {
        return make_pg_cigar_from_text(
            std::string_view{ascii, length}, target.space);
    }
    for (std::size_t index = 0; index < length; ++index) {
        std::uint8_t rank = 0;
        if (!pgseq_rank_for_symbol(
                target.alphabet_code,
                static_cast<unsigned char>(ascii[index]), rank)) {
            const auto alphabet =
                pgseq_supported_alphabets::name_of_alphabet_code(
                    target.alphabet_code);
            bioseq::postgres::support::safety_runtime_probe::
                record_site_visit("seq_in.invalid_symbol");
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid symbol '%c' for bioseq alphabet %.*s",
                            ascii[index], static_cast<int>(alphabet.size()),
                            alphabet.data()),
                     errdetail("symbol offset=%zu", index)));
        }
    }
    if (length > std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq sequence is too long to pack")));
    }
    pgseq_dispatch_biological_instance(
        target.alphabet_code, target.storage_unit_byte_width,
        [length]<class p_instance>() {
            (void) bound_flat_size<p_instance>(length);
        });
    bioseq::pg_types::seq* result = nullptr;
    pgseq_supported_biological_alphabets::dispatch_alphabet_by_code(
        target.alphabet_code,
        [&result, target, length, ascii]<class p_alphabet>() {
            result = ::bioseq::simd::active_target::
                stdc_convert_from_cstr<bioseq::pg_types::seq, p_alphabet>(
                    target.storage_unit_byte_width,
                    static_cast<bioseq::pg_types::seq::bp_size_t>(length),
                    ascii,
                    pg_alloc);
        });
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory packing bioseq sequence")));
    }
    result->encoding = bioseq::sequence_encoding::rank_packed;
    result->space = bioseq::cigar_space::compatibility;
    return result;
}

/**
 * @brief Obtain flat sequence storage from an inline, toasted, or expanded value.
 * @param datum Input sequence.
 * @param must_free Set when the caller must pfree the returned copy.
 * @return A flat pointer. Already-flat input may be borrowed unchanged.
 */
[[nodiscard]] inline bioseq::pg_types::seq* flatten_sequence_datum(
    Datum datum,
    bool& must_free)
{
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
        ExpandedObjectHeader* header = DatumGetEOHP(datum);
        const Size size = EOH_get_flat_size(header);
        auto* result = static_cast<bioseq::pg_types::seq*>(palloc0(size));
        EOH_flatten_into(header, result, size);
        must_free = true;
        return result;
    }
    auto* result = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(datum));
    must_free = reinterpret_cast<Pointer>(result) != DatumGetPointer(datum);
    return result;
}

/**
 * @brief Read one packed storage unit from PostgreSQL binary input.
 * @tparam p_alphabet Alphabet parameter required by the decoder interface.
 * @tparam p_output Integer type of the decoded unit.
 * @param buffer Input positioned  at the next unit.
 * @param width Bytes per unit: 1, 2, 4, or 8.
 * @return The decoded unit.
 */
[[nodiscard]] inline constexpr auto pg_recv_byte_reader =
    []<class p_alphabet, class p_output>(
        StringInfoData& buffer,
        std::uint8_t width) -> p_output {
        switch (width) {
            case 1u: return static_cast<p_output>(pq_getmsgint(&buffer, 1));
            case 2u: return static_cast<p_output>(pq_getmsgint(&buffer, 2));
            case 4u: return static_cast<p_output>(pq_getmsgint(&buffer, 4));
            case 8u: return static_cast<p_output>(pq_getmsgint64(&buffer));
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid bioseq wire storage width %u", width)));
        __builtin_unreachable();
    };

/**
 * @brief Write one packed storage unit to PostgreSQL binary output.
 * @tparam p_input Integer type required by the encoder interface.
 * @param buffer Output buffer.
 * @param unit Unit to wrte.
 * @param width Bytes per unit: 1, 2, 4, or 8.
 */
[[nodiscard]] inline constexpr auto pg_send_byte_writer =
    []<class p_input>(StringInfoData& buffer,
                 std::uint64_t unit,
                 std::uint8_t width) {
        switch (width) {
            case 1u: pq_sendint8(&buffer, static_cast<std::uint8_t>(unit)); return;
            case 2u: pq_sendint16(&buffer, static_cast<std::uint16_t>(unit)); return;
            case 4u: pq_sendint32(&buffer, static_cast<std::uint32_t>(unit)); return;
            case 8u: pq_sendint64(&buffer, unit); return;
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid bioseq wire storage width %u", width)));
    };

/** @brief Trace record and producing expression borrowed from an expanded sequence. */
struct sequence_trace_provenance {
    /// @brief Borrowed slice trace attachment when the Datum is expanded.
    slice_trace_attachment* attachment;
    /// @brief ID of the expression value that produced this, sequence.
    std::uint32_t producer_value_id;
};

/**
 * @brief Borrow tracing information from an expanded sequence.
 * @param input Sequence that may carry a trace attachment.
 * @return Its attachment  and producing value ID, or an empty result for flat input.
 */
[[nodiscard]] inline sequence_trace_provenance trace_provenance_for(
    Datum input)
{
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        return {};
    }
    auto const* expanded = get_pg_sequence_expanded_ro(input);
    return {
        .attachment = expanded->trace,
        .producer_value_id = expanded->producer_value_id,
    };
}

/**
 * @brief Write units produced by the rank packer into a binary output buffer
 * @tparam p_unit Integer type of each packed unit.
 */
template<class p_unit>
struct pg_wire_rank_unit_sink {
    /// @brief Storage-unit type encoded on the wire.
    using storage_unit = p_unit;
    /// @brief Require the rank packer to stay within the declared unit count.
    static constexpr auto capacity_policy =
        bioseq::sequences::pack_capacity_policy::fixed_span;

    /// @brief libpq send buffer receiving encoded units.
    StringInfoData* buffer;
    /// @brief Maximum number of packed units this output may contain.
    std::uint64_t maximum_units;
    /// @brief Number of wire units written so far.
    std::uint64_t written{};

    /// @brief Write a unit, returning false when the declared output size is full.
    [[nodiscard]] bool try_write(p_unit value)
    {
        if (written >= maximum_units) {
            return false;
        }
        pg_send_byte_writer.template operator()<p_unit>(
            *buffer, static_cast<std::uint64_t>(value),
            static_cast<std::uint8_t>(sizeof(p_unit)));
        ++written;
        return true;
    }

    /// @brief Returns how many wire units have been written so far.
    [[nodiscard]] std::size_t written_units() const noexcept
    {
        return static_cast<std::size_t>(written);
    }
};

/**
 * @brief Report a failed attempt to pack ranks for binary output.
 * @param status Failure reported by the checked rank packer.
 */
[[noreturn]] inline void wire_rank_pack_error(
    bioseq::sequences::rank_pack_status status)
{
    ereport(ERROR,
            (errcode(status
                == bioseq::sequences::rank_pack_status::capacity_overflow
                    ? ERRCODE_PROGRAM_LIMIT_EXCEEDED
                    : ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq wire rank packing failed (status=%u)",
                    static_cast<unsigned>(status))));
    __builtin_unreachable();
}

/**
 * @brief Pack a  sequence view directly into the binary output buffer.
 * The view supplies letters in their final order, including any transforms.
 * @tparam p_instance Alphabet and packing-widh type.
 * @tparam p_view Sequence view type.
 * @param view Sequence to read.
 * @param buffer Binary output buffer.
 * @param length Number of letters to write.
 * @param expected_units Required number of packed storage units.
 */
template<class p_instance, class p_view>
void stream_view_to_wire(
    p_view view,
    StringInfoData& buffer,
    std::uint64_t length,
    std::uint64_t expected_units)
{
    using traits = typename compact_sequence_range<p_instance>::traits;
    using unit = typename traits::storage_unit;
    using sink = pg_wire_rank_unit_sink<unit>;
    using writer_type = bioseq::sequences::checked_rank_pack_sink<
        p_instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        sink>;
    writer_type writer{sink{&buffer, expected_units}};
    bioseq::storage::stream_transformed_ranks<
        bioseq::storage::sequence_transform::identity>(
            view, 0u, length, writer);
    const auto result = writer.finish(length, expected_units);
    if (!result) {
        wire_rank_pack_error(result.status);
    }
}

/**
 * @brief Write a sequence view's letters as ASCII characters.
 * @tparam p_alphabet Alphabet used to decode ranks.
 * @param view Sequence view to read.
 * @param output Buffer with space for one byte per letter. No terminator is added.
 */
template<class p_alphabet>
void view_to_ascii(
    bioseq::storage::lazy_sequence_view<p_alphabet> view,
    char* output)
{
    std::array<std::uint8_t, 256> scratch{};
    std::uint64_t position = 0u;
    while (position < view.bp_size()) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(scratch.size(), view.bp_size() - position));
        view.template copy_ranks<false, false>(
            std::span<std::uint8_t>{scratch.data(), count},
            static_cast<std::size_t>(position),
            static_cast<std::size_t>(position + count));
        for (std::size_t index = 0; index < count; ++index) {
            char symbol = '\0';
            if (!pgseq_symbol_for_rank(
                    pgseq_supported_alphabets::template index_of<p_alphabet>(),
                    scratch[index], symbol)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq view emitted an invalid rank")));
            }
            output[position + index] = symbol;
        }
        position += count;
    }
}

/** Bounded PostgreSQL-owned destination used by the shared CIGAR formatter. */
struct pg_cigar_text_sink {
    char* data;
    std::size_t capacity;
    std::size_t written{};

    /** Append one formatting chunk without allocating or raising a PG error. */
    [[nodiscard]] std::expected<void, bioseq::cigar_error> append(
        std::span<const char> chunk)
    {
        if (chunk.size() > capacity - written) {
            return std::unexpected(bioseq::cigar_error{
                bioseq::cigar_error_code::output_too_large});
        }
        std::memcpy(data + written, chunk.data(), chunk.size());
        written += chunk.size();
        return {};
    }
};

/** One owning output allocation and its canonical text length. */
struct pg_cigar_text_result {
    Datum value;
    std::size_t text_bytes;
    std::size_t allocation_bytes;
};

/** Measure then format CIGAR text, without expanding logical positions. */
[[nodiscard]] inline pg_cigar_text_result allocate_cigar_text(
    Datum input,
    bool cstring_output)
{
    return with_pg_cigar_sequence<pg_cigar_text_result>(
        input,
        [cstring_output](auto& backend, auto view) {
            const auto measured = bioseq::measure_cigar_format(backend, view);
            if (!measured) {
                pg_raise_cigar_error(measured.error());
            }
            const std::size_t overhead = cstring_output ? 1u : VARHDRSZ;
            const Size total = checked_materialization_allocation_size(
                *measured, overhead, "CIGAR text output");
            auto* allocation = static_cast<char*>(palloc(total));
            auto* output = allocation + (cstring_output ? 0u : VARHDRSZ);
            pg_cigar_text_sink sink{output, *measured};
            const auto formatted = bioseq::format_cigar_to(backend, view, sink);
            if (!formatted || sink.written != *measured) {
                pfree(allocation);
                pg_raise_cigar_error(!formatted ? formatted.error()
                    : bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding});
            }
            if (cstring_output) {
                output[*measured] = '\0';
            } else {
                SET_VARSIZE(allocation, total);
            }
            return pg_cigar_text_result{
                PointerGetDatum(allocation), *measured,
                static_cast<std::size_t>(total)};
        });
}

/** Encode the canonical run stream with the versioned CIGAR wire framing. */
[[nodiscard]] inline pg_cigar_text_result allocate_cigar_wire(Datum input)
{
    return with_pg_cigar_sequence<pg_cigar_text_result>(
        input,
        [](auto& backend, auto view) {
            const auto run_count = bioseq::cigar_op_count(backend, view);
            if (!run_count) {
                pg_raise_cigar_error(run_count.error());
            }
            const auto payload_bytes = std::uint64_t{16u}
                + std::uint64_t{5u} * *run_count;
            (void) checked_materialization_allocation_size(
                payload_bytes, VARHDRSZ + 1u, "CIGAR binary output");
            StringInfoData buffer;
            pq_begintypsend(&buffer);
            enlargeStringInfo(&buffer, static_cast<int>(payload_bytes));
            constexpr std::uint8_t width_code = sizeof(bioseq::cigar_run) == 1u ? 0u
                : sizeof(bioseq::cigar_run) == 2u ? 1u
                : sizeof(bioseq::cigar_run) == 4u ? 2u : 3u;
            pq_sendint8(&buffer, pgseq_cigar_alphabet_code);
            pq_sendint8(&buffer, 1u);
            pq_sendint8(&buffer,
                        static_cast<std::uint8_t>(bioseq::sequence_encoding::cigar_rle));
            pq_sendint8(&buffer, static_cast<std::uint8_t>(backend.space(view)));
            pq_sendint8(&buffer, width_code);
            pq_sendint8(&buffer, 0u);
            pq_sendint16(&buffer, 0u);
            pq_sendint32(&buffer, static_cast<std::uint32_t>(backend.length(view)));
            pq_sendint32(&buffer, static_cast<std::uint32_t>(*run_count));
            auto cursor = backend.runs(view, 0u, backend.length(view));
            for (;;) {
                const auto next = cursor.next();
                if (!next) {
                    pg_raise_cigar_error(next.error());
                }
                if (!next->has_value()) {
                    break;
                }
                pq_sendint32(&buffer, (**next).length);
                pq_sendint8(&buffer, static_cast<std::uint8_t>(
                    seqan3::to_char((**next).operation)));
            }
            auto* output = pq_endtypsend(&buffer);
            return pg_cigar_text_result{
                PointerGetDatum(output), static_cast<std::size_t>(payload_bytes),
                static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(output))};
        });
}

} // namespace

/* Copy literals into flat values that own their bytes. PostgreSQL can keep
 * them in Const nodes, and concatenation,  can later add them to a piece tree. */
export [[nodiscard]] Datum make_unbound_sequence_from_ascii(
    char const* input,
    std::size_t length,
    bool require_already_canonical)
{
    return PointerGetDatum(allocate_sequence_text(
        input, length, require_already_canonical));
}

/**
 * @brief Validate and pack ASCII letters using a chosen alphabet and width.
 * @param input ASCII symbols.
 * @param length Number of symbols.
 * @param target Destination alphabet and storage width.
 * @return A Datum pointing to newly allocated flat sequence storage
 */
export [[nodiscard]] inline Datum make_bound_sequence_from_ascii(
    char const* input,
    std::size_t length,
    compact_sequence_stdc_type_params target)
{
    return PointerGetDatum(pack_ascii_for_target(input, length, target));
}

export namespace bioseq::pg_types::sql_routines {

/**
 * @brief Parse the alphabet and width in a type such as bioseq.seq('dna4', 64).
 * @param array One-dimensional cstring array containing the alphabet name and width in bits.
 * @return The encoded PostgreSQL type modifier.
 *
 * ```sql
 * -- A sequence cast invokes seq_typmod_in through PostgreSQL's input and type-modifier machinery. This value uses the dna4 alphabet and 64-bit packing.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64);
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_typmod_in({
    .for_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
std::int32_t seq_typmod_in(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"cstring[]">,
    })]]
    ArrayType* array)
{
    if (ARR_NDIM(array) != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.seq typmod must be one-dimensional")));
    }
    if (ARR_HASNULL(array) && array_contains_nulls(array)) {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bioseq.seq typmod arguments must not be NULL")));
    }
    Datum* values = nullptr;
    bool* nulls = nullptr;
    int count = 0;
    deconstruct_array(
        array, CSTRINGOID, -2, false, 'c', &values, &nulls, &count);
    if (count != 1 && count != 2) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.seq takes one or two type modifiers; got %d",
                        count),
                 errhint("Examples: bioseq.seq('dna4', 64), bioseq.seq('cigar')")));
    }
    const auto alphabet = parse_alphabet_name_checked(
        DatumGetCString(values[0]), "bioseq.seq");
    long width_bits = pgseq_is_cigar_alphabet(alphabet)
        ? sizeof(bioseq::cigar_run) * 8u : 64u;
    if (count == 2) {
        const char* width_text = DatumGetCString(values[1]);
        char* end = nullptr;
        errno = 0;
        width_bits = std::strtol(width_text, &end, 10);
        if (end == width_text || *end != '\0' || errno != 0
            || (width_bits != 8 && width_bits != 16
                && width_bits != 32 && width_bits != 64)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bioseq.seq width \"%s\" is not supported", width_text),
                     errhint("Supported widths: 8, 16, 32, 64")));
        }
    }
    if (pgseq_is_cigar_alphabet(alphabet)
        && width_bits != sizeof(bioseq::cigar_run) * 8u) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq CIGAR storage width must be %zu bits",
                        sizeof(bioseq::cigar_run) * 8u)));
    }
    auto params = compact_sequence_stdc_type_params{
        .alphabet_code = alphabet,
        .storage_unit_byte_width = static_cast<std::uint8_t>(width_bits / 8),
        .encoding = pgseq_is_cigar_alphabet(alphabet)
            ? bioseq::sequence_encoding::cigar_rle
            : bioseq::sequence_encoding::rank_packed,
        .space = bioseq::cigar_space::compatibility,
    };
    if (pgseq_is_cigar_alphabet(alphabet)) {
        params.space = *bioseq::cigar_space_from_name(DatumGetCString(values[0]));
    }
    params = pgseq_normalize_type_params(params);
    return params.encode();
}


/**
 * @brief Format the alphabet and width  as a SQL type suffix such as ('dna4',64).
 * @param typmod Type modifier, or negative when none was specified.
 * @return An allocated C string, empty when no modifier was specified.
 *
 * ```sql
 * -- Read the formatted alphabet and packing width of a sequence column from the PostgreSQL catalog.
 * BEGIN;
 * CREATE TEMP TABLE doc_types (seq bioseq.seq('dna4', 64));
 * SELECT format_type(atttypid, atttypmod)
 * FROM pg_attribute WHERE attrelid = 'doc_types'::regclass AND attname = 'seq';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_typmod_out({
    .for_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::cstring,
})]]
char* seq_typmod_out(std::int32_t typmod)
{
    if (typmod < 0) {
        return pstrdup("");
    }
    const auto params = decode_seq_typmod_checked(typmod);
    const auto name = pgseq_is_cigar_alphabet(params.alphabet_code)
        ? bioseq::cigar_space_name(params.space)
        : pgseq_supported_alphabets::name_of_alphabet_code(params.alphabet_code);
    const std::size_t capacity = name.size() + 16u;
    auto* result = static_cast<char*>(palloc(capacity));
    std::snprintf(result, capacity, "('%.*s',%u)",
                  static_cast<int>(name.size()), name.data(),
                  static_cast<unsigned>(params.storage_unit_byte_width) * 8u);
    return result;
}


/**
 * @brief Parse sequence text, packing it when the destination specifies an alphabet.
 * @param input Null-terminated ASCII sequence text.
 * @param type_oid Declared type OID, nused.
 * @param typmod Destination type modifier, or negative to leave the alphabet unset.
 * @return A flat sequence allocated in the current memory context.
 *
 * ```sql
 * -- A sequence cast invokes seq_in through PostgreSQL's input and type-modifier machinery. This value uses the dna4 alphabet and 64-bit packing.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64);
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_input]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
bioseq::pg_types::seq* seq_in(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::cstring,
    })]]
    const char* input,
    Oid type_oid,
    std::int32_t typmod)
{
    static_cast<void>(type_oid);
    const std::size_t length = std::strlen(input);
    if (typmod < 0) {
        return allocate_sequence_text(input, length, false);
    }
    const auto target = decode_seq_typmod_checked(typmod);
    return pack_ascii_for_target(input, length, target);
}

/**
 * @brief Write a sequence's letters as a C string for PostgreSQL text output.
 * @param input Inline, expanded, or toasted sequence.
 * @return Null-terminated ASCII allocated in  the current memory context.
 *
 * ```sql
 * -- Return the canonical text spelling of a bound sequence.
 * SELECT bioseq.seq_out('ACGT'::bioseq.seq('dna4', 64));
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_output]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::cstring,
})]]
char* seq_out(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    const auto provenance = trace_provenance_for(input);
    const std::uint64_t trace_started =
        begin_slice_materialization_timing(provenance.attachment);
    Datum dispatch = input;
    bioseq::pg_types::seq* flat_copy = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat_copy = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat_copy);
    }
    const auto descriptor = classify_and_describe_sequence_header(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    if (descriptor.alphabet_bound
        && pgseq_is_cigar_alphabet(descriptor.alphabet_code)) {
        const auto output = allocate_cigar_text(dispatch, true);
        if (flat_copy != nullptr
            && reinterpret_cast<Pointer>(flat_copy) != DatumGetPointer(input)) {
            pfree(flat_copy);
        }
        record_slice_materialization_success(
            provenance.attachment, provenance.producer_value_id,
            materialization_mechanism::text,
            materialization_reason::client_output,
            descriptor.bp_length, descriptor.bp_length,
            output.allocation_bytes, false,
            output.allocation_bytes, trace_started);
        return static_cast<char*>(DatumGetPointer(output.value));
    }
    (void) classify_and_describe_sequence(dispatch);
    const Size output_size = checked_materialization_allocation_size(
        descriptor.bp_length, 1u, "text output");
    auto* output = static_cast<char*>(palloc(output_size));
    if (!descriptor.alphabet_bound) {
        if (flat_copy != nullptr) {
            std::memcpy(output, flat_copy->data,
                        static_cast<std::size_t>(descriptor.bp_length));
        } else if (!emit_pg_unbound_ascii(
                       get_pg_sequence_expanded_ro(input)->root,
                       0u, descriptor.bp_length,
                       std::span<std::byte>{
                           reinterpret_cast<std::byte*>(output),
                           static_cast<std::size_t>(descriptor.bp_length)})) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("invalid unbound bioseq piece tree")));
        }
    } else {
        with_pg_sequence_view<std::monostate>(
            input,
            [output](auto view) {
                view_to_ascii(view, output);
                return std::monostate{};
            });
    }
    output[descriptor.bp_length] = '\0';
    if (flat_copy != nullptr
        && reinterpret_cast<Pointer>(flat_copy)
            != DatumGetPointer(input)) {
        pfree(flat_copy);
    }
    record_slice_materialization_success(
        provenance.attachment, provenance.producer_value_id,
        materialization_mechanism::text,
        materialization_reason::client_output,
        descriptor.bp_length, descriptor.bp_length,
        descriptor.bp_length + 1u, false,
        descriptor.bp_length + 1u, trace_started);
    return output;
}

/**
 * @brief Copy a sequence's letters into a PostgreSQL text value.
 * @param input Inline, expanded, or toasted sequence.
 * @param reason Reason recorded in the slice trace.
 * @return Text allocated in the current memory context.
 */
[[nodiscard]] inline text* materialize_pg_sequence_text(
    Datum input,
    materialization_reason reason)
{
    const auto provenance = trace_provenance_for(input);
    const std::uint64_t trace_started =
        begin_slice_materialization_timing(provenance.attachment);
    Datum dispatch = input;
    bioseq::pg_types::seq* flat_copy = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat_copy = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat_copy);
    }
    const auto descriptor = classify_and_describe_sequence_header(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    if (descriptor.alphabet_bound
        && pgseq_is_cigar_alphabet(descriptor.alphabet_code)) {
        const auto output = allocate_cigar_text(dispatch, false);
        if (flat_copy != nullptr
            && reinterpret_cast<Pointer>(flat_copy) != DatumGetPointer(input)) {
            pfree(flat_copy);
        }
        record_slice_materialization_success(
            provenance.attachment, provenance.producer_value_id,
            materialization_mechanism::text, reason,
            descriptor.bp_length, descriptor.bp_length,
            output.allocation_bytes, false,
            output.allocation_bytes, trace_started);
        return reinterpret_cast<text*>(DatumGetPointer(output.value));
    }
    (void) classify_and_describe_sequence(dispatch);
    const Size total = checked_materialization_allocation_size(
        descriptor.bp_length, VARHDRSZ, "text output");
    auto* result = static_cast<text*>(palloc(total));
    SET_VARSIZE(result, total);
    auto* output = VARDATA(result);
    if (!descriptor.alphabet_bound) {
        if (flat_copy != nullptr) {
            std::memcpy(output, flat_copy->data,
                        static_cast<std::size_t>(descriptor.bp_length));
        } else if (!emit_pg_unbound_ascii(
                       get_pg_sequence_expanded_ro(input)->root,
                       0u, descriptor.bp_length,
                       std::span<std::byte>{
                           reinterpret_cast<std::byte*>(output),
                           static_cast<std::size_t>(descriptor.bp_length)})) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("invalid unbound bioseq piece tree")));
        }
    } else {
        with_pg_sequence_view<std::monostate>(
            input,
            [output](auto view) {
                view_to_ascii(view, output);
                return std::monostate{};
            });
    }
    if (flat_copy != nullptr
        && reinterpret_cast<Pointer>(flat_copy)
            != DatumGetPointer(input)) {
        pfree(flat_copy);
    }
    record_slice_materialization_success(
        provenance.attachment, provenance.producer_value_id,
        materialization_mechanism::text, reason,
        descriptor.bp_length, descriptor.bp_length,
        static_cast<std::uint64_t>(total), false,
        static_cast<std::uint64_t>(total), trace_started);
    return result;
}

/**
 * @brief Apply an alphabet and packing width to a sequence.
 * An unset alphabet can be bound here. An already-bound sequence must keep its
 * alphabet, though its packing width may change.
 * @param input Source sequence
 * @param target_typmod Destination modifier, or, negative to keep  the input.
 * @param explicit_cast Whether SQL used an explicit cast, unused.
 * @return The result, which may be the unchanged input.
 *
 * ```sql
 * -- A sequence cast invokes seq_cast through PostgreSQL's input and type-modifier machinery. This value uses the dna4 alphabet and 64-bit packing.
 * SELECT 'ACGT'::bioseq.seq('dna4', 64);
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::cast({
    .assignment = true,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum seq_cast(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input,
    std::int32_t target_typmod,
    bool explicit_cast)
{
    static_cast<void>(explicit_cast);
    const auto descriptor = classify_and_describe_sequence_header(input);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    if (target_typmod < 0) {
        return input;
    }
    const auto target = decode_seq_typmod_checked(target_typmod);
    if (descriptor.alphabet_bound
        && descriptor.alphabet_code != target.alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("cannot cast bound bioseq.seq across alphabets")));
    }
    if (pgseq_is_cigar_alphabet(target.alphabet_code)) {
        if (!descriptor.alphabet_bound) {
            bool must_free = false;
            auto* text_value = flatten_sequence_datum(input, must_free);
            auto* result = make_pg_cigar_from_text(
                std::string_view{
                    reinterpret_cast<const char*>(text_value->data),
                    static_cast<std::size_t>(text_value->bp_size)},
                target.space);
            if (must_free) {
                pfree(text_value);
            }
            return PointerGetDatum(result);
        }
        if (descriptor.space == target.space) {
            return input;
        }
        return with_pg_cigar_sequence<Datum>(
            input,
            [target](auto& backend, auto view) {
                auto converted = bioseq::cigar_convert(backend, view, target.space);
                if (!converted) {
                    pg_raise_cigar_error(converted.error(), ERRCODE_INVALID_PARAMETER_VALUE);
                }
                return backend.publish(*converted);
            });
    }
    if (descriptor.alphabet_bound
        && descriptor.storage_unit_byte_width
            == target.storage_unit_byte_width) {
        return input;
    }

    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))
        || descriptor.alphabet_bound) {
        auto acquired = acquire_writable_pg_sequence(input);
        replace_root_error_safe(
            *acquired.value, target, acquired.value->bp_length, true,
            [&acquired, target](
                piece_tree_arena& arena,
                piece_edge_cleanup_ledger& ledger) {
                return acquired.value->alphabet_bound
                    ? retain_piece_edge_error_atomic(
                        ledger, acquired.value->root)
                    : bind_literals_copy_error_atomic(
                        arena, acquired.value->root, target, ledger);
            });
        return pg_sequence_rw_datum(acquired.value);
    }

    auto* flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
    (void) classify_and_describe_sequence(PointerGetDatum(flat));

    bioseq::pg_types::seq* result = pack_ascii_for_target(
        reinterpret_cast<char const*>(flat->data),
        static_cast<std::size_t>(flat->bp_size), target);
    if (reinterpret_cast<Pointer>(flat) != DatumGetPointer(input)) {
        pfree(flat);
    }
    return PointerGetDatum(result);
}

/**
 * @brief Encode a sequence for PostgreSQL binary output.
 * Expanded sequences are read into the output buffer without buildig an
 * intermediate flat sequence.
 * @param input Inline, expanded, or toasted sequence.
 * @return An allocated bytea containing the sequence's binary representation.
 *
 * ```sql
 * -- Inspect the binary wire representation of a bound sequence.
 * SELECT encode(bioseq.seq_send('ACGT'::bioseq.seq('dna4', 64)), 'hex');
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_send]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::bytea,
})]]
text* seq_send(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    const auto provenance = trace_provenance_for(input);
    const std::uint64_t trace_started =
        begin_slice_materialization_timing(provenance.attachment);
    const auto header_descriptor = classify_and_describe_sequence_header(input);
    if (header_descriptor.alphabet_bound
        && pgseq_is_cigar_alphabet(header_descriptor.alphabet_code)) {
        const auto output = allocate_cigar_wire(input);
        record_slice_materialization_success(
            provenance.attachment, provenance.producer_value_id,
            materialization_mechanism::binary,
            materialization_reason::client_output,
            header_descriptor.bp_length, header_descriptor.bp_length,
            output.text_bytes, false,
            output.allocation_bytes, trace_started);
        return reinterpret_cast<text*>(DatumGetPointer(output.value));
    }
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        const auto descriptor = classify_and_describe_sequence(input);
        if (descriptor.kind == pg_sequence_datum_kind::locator) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("cannot send an unresolved bioseq locator")));
        }
        if (descriptor.bp_length
            > (std::numeric_limits<std::uint32_t>::max)()) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq sequence is too long for binary output")));
        }
        std::uint64_t unit_count = 0u;
        std::uint16_t padding = 0u;
        if (descriptor.alphabet_bound) {
            pgseq_dispatch_biological_instance(
                descriptor.alphabet_code,
                descriptor.storage_unit_byte_width,
                [descriptor, &unit_count, &padding]<class p_instance>() {
                    using traits =
                        typename compact_sequence_range<p_instance>::traits;
                    unit_count = checked_ceil_div(
                        descriptor.bp_length, traits::letters_per_unit);
                    padding = unit_count == 0u ? 0u
                        : static_cast<std::uint16_t>(
                            unit_count * traits::letters_per_unit
                            - descriptor.bp_length);
                });
            if (unit_count
                > (std::numeric_limits<std::uint32_t>::max)()) {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq wire unit count exceeds uint32")));
            }
        }
        const std::uint64_t payload_bytes = descriptor.alphabet_bound
            ? unit_count * descriptor.storage_unit_byte_width
            : descriptor.bp_length;
        /*
         * Include the bytea header, sequence header, and StringInfo terminator
         * in the allocation limit. The  sequence header uses 7 bytes for unbound
         * text and 12 bytes for packed values.
         */
        const Size wire_overhead = static_cast<Size>(
            VARHDRSZ + (descriptor.alphabet_bound ? 12u : 7u) + 1u);
        (void) checked_materialization_allocation_size(
            payload_bytes, wire_overhead, "binary output");
        StringInfoData buffer;
        pq_begintypsend(&buffer);
        if (!descriptor.alphabet_bound) {
            pq_sendint8(&buffer, pgseq_unbound_alphabet_code);
            pq_sendint8(&buffer, 1u);
            pq_sendint8(&buffer, pgseq_unbound_width_tag);
            pq_sendint32(
                &buffer, static_cast<std::uint32_t>(descriptor.bp_length));
            std::array<std::byte, 256> scratch{};
            std::uint64_t position = 0u;
            auto const* expanded = get_pg_sequence_expanded_ro(input);
            while (position < descriptor.bp_length) {
                const std::size_t count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        scratch.size(), descriptor.bp_length - position));
                if (!emit_pg_unbound_ascii(
                        expanded->root, position, position + count,
                        std::span<std::byte>{scratch.data(), count})) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("invalid unbound bioseq piece tree")));
                }
                pq_sendbytes(
                    &buffer, reinterpret_cast<char const*>(scratch.data()),
                    static_cast<int>(count));
                position += count;
            }
        } else {
            const std::uint8_t width = descriptor.storage_unit_byte_width;
            const std::uint8_t width_code = width == 1u ? 0u
                : width == 2u ? 1u : width == 4u ? 2u : 3u;
            pq_sendint8(&buffer, descriptor.alphabet_code);
            pq_sendint8(&buffer, width_code);
            pq_sendint16(&buffer, padding);
            pq_sendint32(
                &buffer, static_cast<std::uint32_t>(descriptor.bp_length));
            pq_sendint32(&buffer, static_cast<std::uint32_t>(unit_count));
            with_pg_sequence_view<std::monostate>(
                input,
                [&buffer, descriptor, unit_count](auto view) {
                    pgseq_dispatch_biological_instance(
                        descriptor.alphabet_code,
                        descriptor.storage_unit_byte_width,
                        [&view, &buffer, descriptor,
                         unit_count]<class p_instance>() {
                            stream_view_to_wire<p_instance>(
                                view, buffer, descriptor.bp_length,
                                unit_count);
                        });
                    return std::monostate{};
                });
        }
        bytea* result = pq_endtypsend(&buffer);
        record_slice_materialization_success(
            provenance.attachment, provenance.producer_value_id,
            materialization_mechanism::binary,
            materialization_reason::client_output,
            descriptor.bp_length, descriptor.bp_length,
            static_cast<std::uint64_t>(VARSIZE_ANY_EXHDR(result)), false,
            static_cast<std::uint64_t>(bioseq::postgres::support::varlena_4byte_size(result)), trace_started);
        return result;
    }

    bool must_free = false;
    bioseq::pg_types::seq* value = flatten_sequence_datum(
        input, must_free);
    const auto descriptor = classify_and_describe_sequence(
        PointerGetDatum(value));
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot send an unresolved bioseq locator")));
    }
    const std::uint64_t payload_bytes = descriptor.alphabet_bound
        ? static_cast<std::uint64_t>(value->unit_count())
            * value->storage_unit_byte_width
        : value->bp_size;
    const Size wire_overhead = static_cast<Size>(
        VARHDRSZ + (descriptor.alphabet_bound ? 12u : 7u) + 1u);
    (void) checked_materialization_allocation_size(
        payload_bytes, wire_overhead, "binary output");
    StringInfoData buffer;
    pq_begintypsend(&buffer);
    if (!descriptor.alphabet_bound) {
        pq_sendint8(&buffer, pgseq_unbound_alphabet_code);
        pq_sendint8(&buffer, 1u);
        pq_sendint8(&buffer, pgseq_unbound_width_tag);
        pq_sendint32(&buffer, value->bp_size);
        pq_sendbytes(&buffer, reinterpret_cast<char const*>(value->data),
                     value->bp_size);
    } else {
        const std::uint8_t width = value->storage_unit_byte_width;
        const std::uint8_t width_code = width == 1u ? 0u
            : width == 2u ? 1u : width == 4u ? 2u : 3u;
        pq_sendint8(&buffer, value->alphabet_code);
        pq_sendint8(&buffer, width_code);
        pq_sendint16(&buffer, value->padding_letters);
        pq_sendint32(&buffer, value->bp_size);
        pq_sendint32(&buffer, static_cast<std::uint32_t>(value->unit_count()));
        value->send_to(buffer, pg_send_byte_writer);
    }
    if (must_free) {
        pfree(value);
    }
    bytea* result = pq_endtypsend(&buffer);
    record_slice_materialization_success(
        provenance.attachment, provenance.producer_value_id,
        materialization_mechanism::binary,
        materialization_reason::client_output,
        descriptor.bp_length, descriptor.bp_length,
        static_cast<std::uint64_t>(VARSIZE_ANY_EXHDR(result)), false,
        static_cast<std::uint64_t>(bioseq::postgres::support::varlena_4byte_size(result)), trace_started);
    return result;
}

/**
 * @brief Decode a sequence received in PostgreSQL binary format.
 * Check the format tags, lengths, and padding before accepting the value.
 * @param buffer Input positioned at the sequence header.
 * @param type_oid Declared type OID, unused.
 * @param typmod Destination modifier. Bound input must match it, while unbound
 * text is packed using it when present.
 * @return A flat sequence allocated in the current memory  context.
 *
 * ```sql
 * -- Use PostgreSQL binary COPY to round-trip sequences. PostgreSQL calls seq_recv while reading the file; run these psql commands in the same session.
 * CREATE TABLE doc_sequences (seq bioseq.seq('dna4', 64));
 * INSERT INTO doc_sequences VALUES ('ACGT');
 * \copy doc_sequences TO '/tmp/bioseq-sequences.bin' WITH (FORMAT binary)
 * TRUNCATE doc_sequences;
 * \copy doc_sequences FROM '/tmp/bioseq-sequences.bin' WITH (FORMAT binary)
 * SELECT * FROM doc_sequences;
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_receive]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
bioseq::pg_types::seq* seq_recv(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::internal,
    })]]
    StringInfo buffer,
    Oid type_oid,
    std::int32_t typmod)
{
    static_cast<void>(type_oid);
    const std::uint8_t alphabet =
        static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
    if (pgseq_is_cigar_alphabet(alphabet)) {
        const auto version = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto encoding = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto space_byte = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto width_code = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto reserved_byte = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto reserved_word = static_cast<std::uint16_t>(pq_getmsgint(buffer, 2));
        const auto logical_length = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
        const auto run_count = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
        constexpr std::uint8_t codec_width_code = sizeof(bioseq::cigar_run) == 1u ? 0u
            : sizeof(bioseq::cigar_run) == 2u ? 1u
            : sizeof(bioseq::cigar_run) == 4u ? 2u : 3u;
        const auto encoded_bytes = std::uint64_t{5u} * run_count;
        if (version != 1u
            || encoding != static_cast<std::uint8_t>(bioseq::sequence_encoding::cigar_rle)
            || space_byte >= static_cast<std::uint8_t>(bioseq::cigar_space::count)
            || width_code != codec_width_code
            || reserved_byte != 0u || reserved_word != 0u
            || encoded_bytes != static_cast<std::uint64_t>(buffer->len - buffer->cursor)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                     errmsg("invalid bioseq CIGAR binary framing")));
        }
        const auto space = static_cast<bioseq::cigar_space>(space_byte);
        auto* result = allocate_pg_cigar_payload(run_count, space);
        bioseq::cigar_payload_writer<bioseq::cigar_symbol> writer{
            std::span<std::byte>{reinterpret_cast<std::byte*>(result->data),
                static_cast<std::size_t>(VARSIZE(result))
                    - bioseq::pg_types::seq::header_bytes()},
            run_count, space};
        for (std::size_t index = 0u; index < run_count; ++index) {
            const auto length = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
            const auto operation = static_cast<char>(pq_getmsgint(buffer, 1));
            if (length == 0u || !seqan3::char_is_valid_for<bioseq::cigar_symbol>(operation)) {
                pfree(result);
                pg_raise_cigar_error({length == 0u
                    ? bioseq::cigar_error_code::zero_count
                    : bioseq::cigar_error_code::invalid_operation, index},
                    ERRCODE_INVALID_BINARY_REPRESENTATION);
            }
            const auto appended = writer.append(
                {length, bioseq::cigar_symbol{}.assign_char(operation)});
            if (!appended) {
                pfree(result);
                pg_raise_cigar_error(appended.error(), ERRCODE_INVALID_BINARY_REPRESENTATION);
            }
        }
        const auto finished = writer.finish();
        if (!finished || *finished != logical_length) {
            pfree(result);
            pg_raise_cigar_error(!finished ? finished.error()
                : bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding},
                ERRCODE_INVALID_BINARY_REPRESENTATION);
        }
        result->bp_size = *finished;
        pq_getmsgend(buffer);
        if (typmod >= 0) {
            const auto target = decode_seq_typmod_checked(typmod);
            if (!pgseq_is_cigar_alphabet(target.alphabet_code)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("CIGAR wire input requires a CIGAR destination")));
            }
            return reinterpret_cast<bioseq::pg_types::seq*>(
                DatumGetPointer(seq_cast(PointerGetDatum(result), typmod, false)));
        }
        return result;
    }
    if (alphabet == pgseq_unbound_alphabet_code) {
        const auto version = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto tag = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
        const auto length = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
        if (version != 1u || tag != pgseq_unbound_width_tag
            || buffer->len - buffer->cursor != static_cast<int>(length)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                     errmsg("invalid unbound bioseq.seq wire framing")));
        }
        const char* ascii = pq_getmsgbytes(buffer, static_cast<int>(length));
        bioseq::pg_types::seq* unbound = allocate_unbound_ascii(
            ascii, length, /* already canonical */ true,
            ERRCODE_INVALID_BINARY_REPRESENTATION);
        pq_getmsgend(buffer);
        if (typmod < 0) {
            return unbound;
        }
        const auto target = decode_seq_typmod_checked(typmod);
        bioseq::pg_types::seq* packed = pack_ascii_for_target(
            reinterpret_cast<char const*>(unbound->data), length, target);
        pfree(unbound);
        return packed;
    }

    const auto width_code =
        static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
    if (!is_supported_alphabet_code(alphabet) || width_code > 3u) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid bound bioseq.seq wire type tags")));
    }
    const std::uint8_t width = static_cast<std::uint8_t>(1u << width_code);
    if (!pgseq_supported_storage_unit_byte_widths::contains_runtime(width)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("bioseq wire width %u is disabled", width * 8u)));
    }
    const auto padding = static_cast<std::uint16_t>(pq_getmsgint(buffer, 2));
    const auto bp_size = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
    const auto unit_count = static_cast<std::uint32_t>(pq_getmsgint(buffer, 4));
    if (typmod >= 0) {
        const auto target = decode_seq_typmod_checked(typmod);
        if (target.alphabet_code != alphabet
            || target.storage_unit_byte_width != width) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                     errmsg("bioseq wire type does not match destination typmod")));
        }
    }
    const std::uint64_t required =
        static_cast<std::uint64_t>(unit_count) * width;
    if (required > static_cast<std::uint64_t>(buffer->len - buffer->cursor)
        || bioseq::pg_types::seq::header_bytes()
                + round_up_to_uint64(static_cast<std::size_t>(required))
            > MaxAllocSize) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid or oversized bound bioseq wire payload")));
    }
    bool metadata_ok = false;
    pgseq_dispatch_biological_instance(
        alphabet, width,
        [&metadata_ok, unit_count, padding, bp_size]<class p_instance>() {
        using traits = typename compact_sequence_range<p_instance>::traits;
        const std::uint64_t derived = unit_count == 0u ? 0u
            : static_cast<std::uint64_t>(unit_count)
                    * traits::letters_per_unit
                - padding;
        metadata_ok = (unit_count == 0u ? padding == 0u
                                        : padding < traits::letters_per_unit)
            && derived == bp_size;
    });
    if (!metadata_ok) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("inconsistent bound bioseq wire length/padding")));
    }
    bioseq::pg_types::seq* result = nullptr;
    pgseq_supported_biological_alphabets::dispatch_alphabet_by_code(
        alphabet,
        [&result, width, padding, bp_size, unit_count,
         buffer]<class p_alphabet>() {
            result = bioseq::pg_types::seq::convert_from<p_alphabet>(
                width, padding, bp_size, unit_count,
                *buffer, pg_recv_byte_reader, pg_alloc);
        });
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory receiving bioseq.seq")));
    }
    result->encoding = bioseq::sequence_encoding::rank_packed;
    result->space = bioseq::cigar_space::compatibility;
    pq_getmsgend(buffer);
    return result;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
/**
 * @brief Count sequence letters using the stored length.
 * @ingroup bioseq_sql_public_api
 *
 * ```sql
 * -- Read the number of biological letters in a sequence.
 * SELECT bioseq.len('ACGT'::bioseq.seq('dna4', 64));
 * ```
 */
std::int64_t len(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    const auto descriptor = classify_and_describe_sequence_header(
        input);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    return static_cast<std::int64_t>(descriptor.bp_length);
}

/**
 * @brief Count packed storage units using the sequence header.
 * @param input Sequence whose header may be fetched from TOAST storage.
 * @return Packed unit count rounded up to hold every letter, or the letter
 * count when the alphabet is unset
 *
 * ```sql
 * -- Read the number of packed storage units in a sequence.
 * SELECT bioseq.unit_len('ACGT'::bioseq.seq('dna4', 64));
 * ```
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
std::int64_t unit_len(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    const auto descriptor = classify_and_describe_sequence_header(
        input);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    if (descriptor.alphabet_bound
        && pgseq_is_cigar_alphabet(descriptor.alphabet_code)) {
        return with_pg_cigar_sequence<std::int64_t>(
            input,
            [](auto& backend, auto view) {
                const auto count = bioseq::cigar_op_count(backend, view);
                if (!count) {
                    pg_raise_cigar_error(count.error());
                }
                return static_cast<std::int64_t>(*count);
            });
    }
    std::uint64_t units = descriptor.bp_length;
    if (descriptor.alphabet_bound) {
        pgseq_dispatch_biological_instance(
            descriptor.alphabet_code,
            descriptor.storage_unit_byte_width,
            [&units, descriptor]<class p_instance>() {
                using traits = typename compact_sequence_range<p_instance>::traits;
                units = checked_ceil_div(
                    descriptor.bp_length, traits::letters_per_unit);
            });
    }
    return static_cast<std::int64_t>(units);
}

}  // namespace bioseq::pg_types::sql_routines
