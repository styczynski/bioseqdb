/* Explicit string and array materialization/concat bridges for bioseq.seq. */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <variant>

#include "headers/pg_compat.h"
extern "C" {
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
}

export module bioseq.types.pg_compact_sequence:array_string;

import :flat_abi;
import :trace;
import :tree_types;
import :view_session;
import :packed_output;
import :io;
import :letter;
import :subscript;

namespace {

struct deconstructed_array {
    Datum* values;
    bool* nulls;
    int count;
};

struct literal_array_value {
    Datum literal;
    int count;
    bool has_alphabet;
    std::uint8_t alphabet_code;
};

inline void validate_bridge_array_shape(
    ArrayType const* array,
    char const* kind)
{
    if (ARR_NDIM(array) > 1) {
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bioseq %s array must be zero- or one-dimensional",
                        kind)));
    }
}

[[noreturn]] inline void array_null_error(
    ArrayType const* array,
    int offset)
{
    const int subscript = ARR_NDIM(array) == 1
        ? ARR_LBOUND(array)[0] + offset : offset;
    ereport(ERROR,
            (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
             errmsg("NULL array element is not allowed in bioseq concatenation"),
             errdetail("array subscript=%d", subscript)));
    __builtin_unreachable();
}

[[nodiscard]] inline deconstructed_array deconstruct_text_values(
    ArrayType* array)
{
    deconstructed_array result{};
    deconstruct_array(
        array, TEXTOID, -1, false, 'i',
        &result.values, &result.nulls, &result.count);
    return result;
}

[[nodiscard]] inline deconstructed_array deconstruct_letter_values(
    ArrayType* array)
{
    deconstructed_array result{};
    deconstruct_array(
        array, bioseq_letter_oid(), 2, true, 's',
        &result.values, &result.nulls, &result.count);
    return result;
}

[[nodiscard]] inline literal_array_value text_array_literal(ArrayType* array)
{
    validate_bridge_array_shape(array, "text");
    const auto elements = deconstruct_text_values(array);
    std::size_t total = 0u;
    for (int index = 0; index < elements.count; ++index) {
        if (elements.nulls != nullptr && elements.nulls[index]) {
            array_null_error(array, index);
        }
        text* item = DatumGetTextPP(elements.values[index]);
        const std::size_t length = static_cast<std::size_t>(
            VARSIZE_ANY_EXHDR(item));
        if (length > static_cast<std::size_t>(MaxAllocSize) - total) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq text array literal exceeds MaxAllocSize")));
        }
        total += length;
        if (reinterpret_cast<Pointer>(item)
            != DatumGetPointer(elements.values[index])) {
            pfree(item);
        }
    }
    auto* buffer = static_cast<char*>(palloc(std::max<std::size_t>(total, 1u)));
    std::size_t position = 0u;
    for (int index = 0; index < elements.count; ++index) {
        text* item = DatumGetTextPP(elements.values[index]);
        const std::size_t length = static_cast<std::size_t>(
            VARSIZE_ANY_EXHDR(item));
        if (length != 0u) {
            std::memcpy(buffer + position, VARDATA_ANY(item), length);
        }
        position += length;
        if (reinterpret_cast<Pointer>(item)
            != DatumGetPointer(elements.values[index])) {
            pfree(item);
        }
    }
    const Datum literal = make_unbound_sequence_from_ascii(buffer, total);
    pfree(buffer);
    return {
        .literal = literal,
        .count = elements.count,
        .has_alphabet = false,
        .alphabet_code = pgseq_unbound_alphabet_code,
    };
}

[[noreturn]] inline void mixed_letter_alphabet_error(
    std::uint8_t expected,
    std::uint8_t actual)
{
    const auto expected_name =
        pgseq_supported_alphabets::name_of_alphabet_code(expected);
    const auto actual_name =
        pgseq_supported_alphabets::name_of_alphabet_code(actual);
    ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq.letter array mixes alphabets %.*s and %.*s",
                    static_cast<int>(expected_name.size()),
                    expected_name.data(),
                    static_cast<int>(actual_name.size()),
                    actual_name.data())));
    __builtin_unreachable();
}

[[nodiscard]] inline literal_array_value letter_array_literal(ArrayType* array)
{
    validate_bridge_array_shape(array, "letter");
    const auto elements = deconstruct_letter_values(array);
    auto* ascii = static_cast<char*>(palloc(
        std::max<std::size_t>(static_cast<std::size_t>(elements.count), 1u)));
    bool has_alphabet = false;
    std::uint8_t alphabet = pgseq_unbound_alphabet_code;
    for (int index = 0; index < elements.count; ++index) {
        if (elements.nulls != nullptr && elements.nulls[index]) {
            array_null_error(array, index);
        }
        const auto letter = validate_letter_datum(elements.values[index]);
        if (!has_alphabet) {
            alphabet = letter.alphabet_code;
            has_alphabet = true;
        } else if (alphabet != letter.alphabet_code) {
            mixed_letter_alphabet_error(alphabet, letter.alphabet_code);
        }
        char symbol = '\0';
        (void) pgseq_symbol_for_rank(
            letter.alphabet_code, letter.rank, symbol);
        ascii[index] = symbol;
    }
    Datum literal = Datum{0};
    if (elements.count != 0) {
        literal = make_unbound_sequence_from_ascii(
            ascii, static_cast<std::size_t>(elements.count), true);
    }
    pfree(ascii);
    return {
        .literal = literal,
        .count = elements.count,
        .has_alphabet = has_alphabet,
        .alphabet_code = alphabet,
    };
}

[[nodiscard]] inline pg_sequence_descriptor inspect_sequence(Datum input)
{
    Datum dispatch = input;
    pgseq_t* flat = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(input));
        dispatch = PointerGetDatum(flat);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan")));
    }
    if (flat != nullptr
        && reinterpret_cast<Pointer>(flat) != DatumGetPointer(input)) {
        pfree(flat);
    }
    return descriptor;
}

[[nodiscard]] inline Datum concat_array_literal(
    Datum sequence,
    literal_array_value literal,
    bool array_on_left,
    bool letter_array)
{
    if (literal.count == 0) {
        if (DatumGetPointer(literal.literal) != nullptr) {
            pfree(DatumGetPointer(literal.literal));
        }
        return sequence;
    }
    if (letter_array) {
        const auto descriptor = inspect_sequence(sequence);
        if (!descriptor.alphabet_bound) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("a bioseq.letter array cannot bind an unbound sequence"),
                     errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
        }
        if (!literal.has_alphabet
            || literal.alphabet_code != descriptor.alphabet_code) {
            mixed_letter_alphabet_error(
                descriptor.alphabet_code, literal.alphabet_code);
        }
    }
    const Datum result = array_on_left
        ? concat_pg_sequences(literal.literal, sequence)
        : concat_pg_sequences(sequence, literal.literal);
    pfree(DatumGetPointer(literal.literal));
    return result;
}

[[nodiscard]] inline compact_sequence_stdc_type_params array_target_type(
    std::uint8_t alphabet,
    int32_t width_bits)
{
    if (width_bits != 8 && width_bits != 16
        && width_bits != 32 && width_bits != 64) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq sequence width %d is not supported",
                        width_bits),
                 errhint("Supported widths: 8, 16, 32, 64")));
    }
    return {
        .alphabet_code = alphabet,
        .storage_unit_byte_width = pgseq_normalize_storage_unit_byte_width(
            static_cast<std::uint8_t>(width_bits / 8)),
    };
}

[[nodiscard]] inline Datum packed_from_letter_array(
    ArrayType* array,
    std::uint8_t alphabet,
    int32_t width_bits,
    bool require_nonempty)
{
    if (require_nonempty && ARR_NDIM(array) != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("alphabet-inferred bioseq.from_array requires a one-dimensional array")));
    }
    const auto literal = letter_array_literal(array);
    if (require_nonempty && literal.count == 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot infer a bioseq alphabet from an empty letter array"),
                 errhint("use bioseq.from_array(letters, alphabet_name, width)")));
    }
    if (literal.has_alphabet && literal.alphabet_code != alphabet) {
        mixed_letter_alphabet_error(alphabet, literal.alphabet_code);
    }
    const auto target = array_target_type(alphabet, width_bits);
    if (literal.count == 0) {
        return make_bound_sequence_from_ascii("", 0u, target);
    }
    auto* value = reinterpret_cast<pgseq_t*>(DatumGetPointer(literal.literal));
    const Datum result = make_bound_sequence_from_ascii(
        reinterpret_cast<char const*>(value->data),
        static_cast<std::size_t>(value->bp_size), target);
    pfree(value);
    return result;
}

} // namespace

export inline Datum seq_concat_text_array_impl(FunctionCallInfo fcinfo)
{
    ArrayType* array = PG_GETARG_ARRAYTYPE_P(1);
    const auto literal = text_array_literal(array);
    const Datum result = concat_array_literal(
        PG_GETARG_DATUM(0), literal, false, false);
    PG_FREE_IF_COPY(array, 1);
    PG_RETURN_DATUM(result);
}

export inline Datum text_array_concat_seq_impl(FunctionCallInfo fcinfo)
{
    ArrayType* array = PG_GETARG_ARRAYTYPE_P(0);
    const auto literal = text_array_literal(array);
    const Datum result = concat_array_literal(
        PG_GETARG_DATUM(1), literal, true, false);
    PG_FREE_IF_COPY(array, 0);
    PG_RETURN_DATUM(result);
}

export inline Datum seq_concat_letter_array_impl(FunctionCallInfo fcinfo)
{
    ArrayType* array = PG_GETARG_ARRAYTYPE_P(1);
    const auto literal = letter_array_literal(array);
    const Datum result = concat_array_literal(
        PG_GETARG_DATUM(0), literal, false, true);
    PG_FREE_IF_COPY(array, 1);
    PG_RETURN_DATUM(result);
}

export inline Datum letter_array_concat_seq_impl(FunctionCallInfo fcinfo)
{
    ArrayType* array = PG_GETARG_ARRAYTYPE_P(0);
    const auto literal = letter_array_literal(array);
    const Datum result = concat_array_literal(
        PG_GETARG_DATUM(1), literal, true, true);
    PG_FREE_IF_COPY(array, 0);
    PG_RETURN_DATUM(result);
}

export inline Datum seq_array_length_impl(FunctionCallInfo fcinfo)
{
    if (PG_GETARG_INT32(1) != 1) {
        PG_RETURN_NULL();
    }
    PG_RETURN_INT64(static_cast<std::int64_t>(
        sequence_logical_length(PG_GETARG_DATUM(0))));
}

export inline Datum seq_cardinality_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_INT64(static_cast<std::int64_t>(
        sequence_logical_length(PG_GETARG_DATUM(0))));
}

export inline Datum seq_to_text_impl(FunctionCallInfo fcinfo)
{
    PG_RETURN_TEXT_P(materialize_pg_sequence_text(
        fcinfo, materialization_reason::sql_conversion));
}

export inline Datum seq_to_varchar_impl(FunctionCallInfo fcinfo)
{
    return seq_to_text_impl(fcinfo);
}

export inline Datum seq_to_array_impl(FunctionCallInfo fcinfo)
{
    const Datum input = PG_GETARG_DATUM(0);
    auto const* expanded = VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))
        ? get_pg_sequence_expanded_ro(input) : nullptr;
    auto* trace = expanded == nullptr ? nullptr : expanded->trace;
    const std::uint32_t producer_value_id = expanded == nullptr
        ? 0u : expanded->producer_value_id;
    const std::uint64_t trace_started =
        begin_slice_materialization_timing(trace);
    const auto descriptor = inspect_sequence(input);
    if (!descriptor.alphabet_bound) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("cannot materialize bioseq.letter[] from an unbound sequence"),
                 errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
    }
    const Oid letter_oid = bioseq_letter_oid();
    if (descriptor.bp_length == 0u) {
        ArrayType* result = construct_empty_array(letter_oid);
        record_slice_materialization_success(
            trace, producer_value_id,
            materialization_mechanism::letter_array,
            materialization_reason::sql_conversion,
            0u, 0u, static_cast<std::uint64_t>(VARSIZE(result)), false,
            static_cast<std::uint64_t>(VARSIZE(result)), trace_started);
        PG_RETURN_ARRAYTYPE_P(result);
    }
    if (descriptor.bp_length > static_cast<std::uint64_t>(INT_MAX)
        || descriptor.bp_length
            > static_cast<std::uint64_t>(MaxAllocSize / sizeof(Datum))) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq sequence is too long to materialize as letter[]")));
    }
    const int count = static_cast<int>(descriptor.bp_length);
    const Size values_bytes = checked_mul_size(
        sizeof(Datum), static_cast<Size>(count));
    auto* values = static_cast<Datum*>(palloc(values_bytes));
    const std::uint8_t alphabet = descriptor.alphabet_code;
    with_pg_sequence_view<std::monostate>(
        fcinfo, 0,
        [values, count, alphabet](auto view) {
            std::array<std::uint8_t, 256> scratch{};
            int position = 0;
            while (position < count) {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<int>(static_cast<int>(scratch.size()),
                                  count - position));
                view.template to_encoded_into<false, false>(
                    std::span<std::uint8_t>{scratch.data(), chunk},
                    static_cast<std::size_t>(position),
                    static_cast<std::size_t>(position) + chunk);
                for (std::size_t index = 0; index < chunk; ++index) {
                    char symbol = '\0';
                    if (!pgseq_symbol_for_rank(
                            alphabet, scratch[index], symbol)) {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("bioseq view emitted an invalid rank")));
                    }
                    values[position + static_cast<int>(index)] =
                        make_letter_datum(alphabet, scratch[index]);
                }
                position += static_cast<int>(chunk);
            }
            return std::monostate{};
        });
    ArrayType* result = construct_array(
        values, count, letter_oid, 2, true, 's');
    const std::uint64_t peak_retained_bytes =
        static_cast<std::uint64_t>(values_bytes)
        + static_cast<std::uint64_t>(VARSIZE(result));
    pfree(values);
    record_slice_materialization_success(
        trace, producer_value_id,
        materialization_mechanism::letter_array,
        materialization_reason::sql_conversion,
        descriptor.bp_length, descriptor.bp_length,
        static_cast<std::uint64_t>(VARSIZE(result)), false,
        peak_retained_bytes, trace_started);
    PG_RETURN_ARRAYTYPE_P(result);
}

export inline Datum seq_from_letter_array_infer_impl(FunctionCallInfo fcinfo)
{
    ArrayType* array = PG_GETARG_ARRAYTYPE_P(0);
    validate_bridge_array_shape(array, "letter");
    const auto values = deconstruct_letter_values(array);
    if (ARR_NDIM(array) != 1 || values.count == 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot infer a bioseq alphabet from an empty letter array"),
                 errhint("use bioseq.from_array(letters, alphabet_name, width)")));
    }
    if (values.nulls != nullptr && values.nulls[0]) {
        array_null_error(array, 0);
    }
    const auto first = validate_letter_datum(values.values[0]);
    const Datum result = packed_from_letter_array(
        array, first.alphabet_code, PG_GETARG_INT32(1), true);
    PG_FREE_IF_COPY(array, 0);
    PG_RETURN_DATUM(result);
}

export inline Datum seq_from_letter_array_named_impl(FunctionCallInfo fcinfo)
{
    ArrayType* array = PG_GETARG_ARRAYTYPE_P(0);
    text* name_text = PG_GETARG_TEXT_PP(1);
    char* name = text_to_cstring(name_text);
    const auto alphabet = parse_alphabet_name_checked(name, "bioseq.seq");
    pfree(name);
    const Datum result = packed_from_letter_array(
        array, alphabet, PG_GETARG_INT32(2), false);
    PG_FREE_IF_COPY(name_text, 1);
    PG_FREE_IF_COPY(array, 0);
    PG_RETURN_DATUM(result);
}
