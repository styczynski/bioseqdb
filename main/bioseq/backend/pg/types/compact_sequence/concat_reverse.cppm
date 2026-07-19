/* Scalar concat and O(1) logical transform SQL adapters. */
module;

#include <cstddef>
#include <cstdint>

#include "headers/pg_compat.h"
extern "C" {
#include "utils/builtins.h"
}

export module bioseq.types.pg_compact_sequence:concat_reverse;

import :flat_abi;
import :io;
import :tree_types;
import :view_session;
import :packed_output;
import bioseq.storage.coordinates;
import bioseq.storage.lazy;

namespace {

[[nodiscard]] inline Datum text_sequence_literal(text const* value)
{
    return make_unbound_sequence_from_ascii(
        VARDATA_ANY(value),
        static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value)));
}

[[nodiscard]] inline Datum concat_text_and_sequence(
    text const* value,
    Datum sequence,
    bool text_on_left)
{
    const Datum literal = text_sequence_literal(value);
    const Datum result = text_on_left
        ? concat_pg_sequences(literal, sequence)
        : concat_pg_sequences(sequence, literal);
    pfree(DatumGetPointer(literal));
    return result;
}

[[nodiscard]] inline bool alphabet_has_complete_complement(
    std::uint8_t alphabet_code)
{
    bool complete = true;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        alphabet_code,
        [alphabet_code, &complete]<class Alphabet>() {
            for (std::uint16_t rank = 0u; rank <= UINT8_MAX; ++rank) {
                char original = '\0';
                if (!pgseq_symbol_for_rank(
                        alphabet_code, static_cast<std::uint8_t>(rank),
                        original)) {
                    continue;
                }
                const auto complemented =
                    bioseq::storage::lazy_rank_stream_traits<Alphabet>::
                        letter_complement(static_cast<std::uint8_t>(rank));
                char symbol = '\0';
                if (!pgseq_symbol_for_rank(
                        alphabet_code, complemented, symbol)) {
                    complete = false;
                    return;
                }
            }
        });
    return complete;
}

} // namespace

export inline Datum seq_concat_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_DATUM(concat_pg_sequences(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));
}

export inline Datum seq_concat_text_impl(FunctionCallInfo fcinfo)
{
    text* value = PG_GETARG_TEXT_PP(1);
    const Datum result = concat_text_and_sequence(
        value, PG_GETARG_DATUM(0), false);
    PG_FREE_IF_COPY(value, 1);
    PG_RETURN_DATUM(result);
}

export inline Datum text_concat_seq_impl(FunctionCallInfo fcinfo)
{
    text* value = PG_GETARG_TEXT_PP(0);
    const Datum result = concat_text_and_sequence(
        value, PG_GETARG_DATUM(1), true);
    PG_FREE_IF_COPY(value, 0);
    PG_RETURN_DATUM(result);
}

export inline Datum seq_concat_varchar_impl(FunctionCallInfo fcinfo)
{
    return seq_concat_text_impl(fcinfo);
}

export inline Datum varchar_concat_seq_impl(FunctionCallInfo fcinfo)
{
    return text_concat_seq_impl(fcinfo);
}

export inline Datum seq_reverse_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_DATUM(transform_pg_sequence(
        PG_GETARG_DATUM(0),
        bioseq::storage::sequence_transform::reversed));
}

export inline Datum seq_reverse_complement_impl(FunctionCallInfo fcinfo)
{
    Datum input = PG_GETARG_DATUM(0);
    Datum dispatch = input;
    pgseq_t* flat = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (!descriptor.alphabet_bound) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("reverse complement requires a bound bioseq alphabet"),
                 errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
    }
    if (!alphabet_has_complete_complement(descriptor.alphabet_code)) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq alphabet has no complete complement map")));
    }
    if (flat != nullptr
        && reinterpret_cast<Pointer>(flat) != DatumGetPointer(input)) {
        pfree(flat);
    }
    PG_RETURN_DATUM(transform_pg_sequence(
        input, bioseq::storage::sequence_transform::reverse_complement));
}
