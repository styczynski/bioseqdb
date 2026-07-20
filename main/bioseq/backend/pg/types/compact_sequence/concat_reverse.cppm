/* Scalar concat and O(1) logical transform SQL adapters. */
module;

#include <cstddef>
#include <cstdint>

#include "headers/pg_compat.h"
#include "fmgr_boundary.h"
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

} // namespace

export Datum seq_concat_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_DATUM(concat_pg_sequences(
        PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));
}

export Datum seq_concat_text_impl(FunctionCallInfo fcinfo)
{
    text* value = PG_GETARG_TEXT_PP(1);
    const Datum result = concat_text_and_sequence(
        value, PG_GETARG_DATUM(0), false);
    PG_FREE_IF_COPY(value, 1);
    PG_RETURN_DATUM(result);
}

export Datum text_concat_seq_impl(FunctionCallInfo fcinfo)
{
    text* value = PG_GETARG_TEXT_PP(0);
    const Datum result = concat_text_and_sequence(
        value, PG_GETARG_DATUM(1), true);
    PG_FREE_IF_COPY(value, 0);
    PG_RETURN_DATUM(result);
}

export Datum seq_concat_varchar_impl(FunctionCallInfo fcinfo)
{
    return seq_concat_text_impl(fcinfo);
}

export Datum varchar_concat_seq_impl(FunctionCallInfo fcinfo)
{
    return text_concat_seq_impl(fcinfo);
}

export Datum seq_reverse_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_DATUM(transform_pg_sequence(
        PG_GETARG_DATUM(0),
        bioseq::storage::sequence_transform::reversed));
}

export Datum seq_reverse_complement_impl(FunctionCallInfo fcinfo)
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

extern "C" {
CXX_PG_FUNCTION_INFO_V1(seq_concat);
PGDLLEXPORT Datum seq_concat(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_concat_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_concat_text);
PGDLLEXPORT Datum seq_concat_text(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_concat_text_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(text_concat_seq);
PGDLLEXPORT Datum text_concat_seq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&text_concat_seq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_concat_varchar);
PGDLLEXPORT Datum seq_concat_varchar(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_concat_varchar_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(varchar_concat_seq);
PGDLLEXPORT Datum varchar_concat_seq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&varchar_concat_seq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_reverse);
PGDLLEXPORT Datum seq_reverse(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_reverse_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_reverse_complement);
PGDLLEXPORT Datum seq_reverse_complement(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_reverse_complement_impl, fcinfo);
}
} // extern "C"
