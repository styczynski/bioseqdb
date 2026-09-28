/**
 * @file array_string.cppm
 * Convert sequences to text and letter arrays, and join arrays with sequences.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>

#include <bioseq/pg_compat.h>
extern "C" {
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:array_string;

import bioseq.postgres.support;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :tree_types;
import :view_session;
import :packed_output;
import :io;
import :letter;
import :subscript;
import bioseq.sequences.rank_pack;

namespace {

/** @brief Array values with a NULL flag for each element. */
struct deconstructed_array {
    Datum* values;
    bool* nulls;
    int count;
};

/** @brief Joined array text, its element count, and any known letter alphabet. */
struct literal_array_value {
    Datum literal;
    int count;
    bool has_alphabet;
    std::uint8_t alphabet_code;
};

/**
 * @brief Accept an  empty array or a one-dimensional array.
 * @param array Array to check.
 * @param kind Element name used in errors, such as text or letter.
 */
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

/**
 * @brief Report a NULL element using its original PostgreSQL array subscript.
 * @param array Array whose lower ound determines the reported subscript.
 * @param offset Zero-based position in the extracted elements.
 */
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

/**
 * @brief Extract text[] values and NULL flags into PostgreSQL-allocated arrays.
 * Text values still refer to the input array's storage
 * @param array Input text array.
 * @return Values, NULL flags, and element count.
 */
[[nodiscard]] inline deconstructed_array deconstruct_text_values(
    ArrayType* array)
{
    deconstructed_array result{};
    deconstruct_array(
        array, TEXTOID, -1, false, 'i',
        &result.values, &result.nulls, &result.count);
    return result;
}

/**
 * @brief Extract bioseq.letter[] values and NULL flags into allocated arrays.
 * @param array Input letter array.
 * @return Values, NULL flags, and element count.
 */
[[nodiscard]] inline deconstructed_array deconstruct_letter_values(
    ArrayType* array)
{
    deconstructed_array result{};
    deconstruct_array(
        array, bioseq_letter_oid(), 2, true, 's',
        &result.values, &result.nulls, &result.count);
    return result;
}

/**
 * @brief Join text array elements in order into a sequence  with no alphabet yet.
 * @param array Empty or one-dimensional text array with no NULL elements.
 * @return The sequence, element count, and an unset alphabet.
 */
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

/**
 * @brief Report an error when a letter uses a different alphabet.
 * @param expected Required alphabet code.
 * @param actual Alphabet code from the conflicting letter.
 */
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

/**
 * @brief Join letter array elements as text and remember their shared alphabet.
 * @param array Empty or one-dimensional letter array with no NULL elements.
 * @return The text sequence, element count, and alphabet. An empty array has
 * no sequence storage  or alphabet.
 */
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

/**
 * @brief Read sequence length and alphabet, information.
 * @param input Inline, expanded, or toasted sequence value.
 * @return Its descriptor. A bave storage locator raises an error because the
 * owning table scan is needed to resolve it.
 */
[[nodiscard]] inline pg_sequence_descriptor inspect_sequence(Datum input)
{
    Datum dispatch = input;
    bioseq::pg_types::seq* flat = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
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

/**
 * @brief Join an array-derived sequence with an existing sequence.
 * @param sequence Existing sequence.
 * @param literal Joined array elements. Its temporary storage is freed here.
 * @param array_on_left Place the array elements before the sequence when true.
 * @param letter_array Require the letters to match the sequence's bound alphabet.
 * @return The  concatenation, or sequence unchanged for an empty array
 */
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

/**
 * @brief Validate a packing width and combine it with the chosen alphabet.
 * @param alphabet Alphabet code for the result.
 * @param width_bits Bits per storage unit: 8, 16, 32, or 64.
 * @return Alphabet and storage-unit width in bytes.
 */
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

/**
 * @brief Pack letter array ranks into a sequence with a known alphabet.
 * Every element must be non-NULL and use the same alphabet.
 * @param array Empty or one-dimensional letter array.
 * @param alphabet Expected alphabet when require_nonempty is false.
 * @param width_bits Bits per storage unit: 8, 16, 32, or 64.
 * @param require_nonempty Infer the alphabet from the first element and reject
 * empty arrays when true.
 * @return A  flat sequence allocated in the current memory context.
 */
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
    validate_bridge_array_shape(array, "letter");
    const auto elements = deconstruct_letter_values(array);
    if (require_nonempty && elements.count == 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot infer a bioseq alphabet from an empty letter array"),
                 errhint("use bioseq.from_array(letters, alphabet_name, width)")));
    }
    sequence_letter_value first{};
    bool have_first = false;
    if (elements.count != 0) {
        if (elements.nulls != nullptr && elements.nulls[0]) {
            array_null_error(array, 0);
        }
        const auto validated_first =
            validate_letter_datum(elements.values[0]);
        first = {
            .alphabet_code = validated_first.alphabet_code,
            .rank = validated_first.rank,
        };
        have_first = true;
        if (require_nonempty) {
            alphabet = first.alphabet_code;
        } else if (first.alphabet_code != alphabet) {
            mixed_letter_alphabet_error(alphabet, first.alphabet_code);
        }
    }
    const auto target = array_target_type(alphabet, width_bits);
    bioseq::pg_types::seq* result = nullptr;
    bioseq::pg_types::seq::dispatch_instance(
        target.alphabet_code,
        target.storage_unit_byte_width,
        [&result, target, elements, array,
         first, have_first]<class p_instance>() {
            using traits = typename compact_sequence_range<p_instance>::traits;
            using unit = typename traits::storage_unit;
            const std::uint64_t length = static_cast<std::uint64_t>(
                elements.count);
            const std::uint64_t unit_count = length == 0u ? 0u
                : length / traits::letters_per_unit
                    + static_cast<std::uint64_t>(
                        length % traits::letters_per_unit != 0u);
            const Size total = bound_flat_size<p_instance>(length);
            result = pg_alloc(total);
            if (result == nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_OUT_OF_MEMORY),
                         errmsg("out of memory packing bioseq.letter array")));
            }
            std::memset(result, 0, total);
            SET_VARSIZE(result, total);
            result->alphabet_code = target.alphabet_code;
            result->storage_unit_byte_width =
                target.storage_unit_byte_width;
            result->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(length);

            using sink = bioseq::sequences::
                fixed_rank_pack_unit_sink<unit>;
            using writer_type = bioseq::sequences::checked_rank_pack_sink<
                p_instance,
                bioseq::sequences::invalid_rank_policy::reject,
                bioseq::sequences::pack_capacity_policy::fixed_span,
                sink>;
            auto units = std::span<unit>{
                reinterpret_cast<unit*>(result->data),
                static_cast<std::size_t>(unit_count)};
            writer_type writer{sink{units}};

            int index = 0;
            if (have_first) {
                const auto status = writer.append_rank(first.rank);
                if (status != bioseq::sequences::rank_pack_status::ok) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("invalid rank while packing "
                                    "bioseq.letter array")));
                }
                index = 1;
            }
            for (; index < elements.count; ++index) {
                if (elements.nulls != nullptr && elements.nulls[index]) {
                    array_null_error(array, index);
                }
                const auto letter = validate_letter_datum(
                    elements.values[index]);
                if (letter.alphabet_code != target.alphabet_code) {
                    mixed_letter_alphabet_error(
                        target.alphabet_code, letter.alphabet_code);
                }
                const auto status = writer.append_rank(letter.rank);
                if (status != bioseq::sequences::rank_pack_status::ok) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("invalid rank while packing "
                                    "bioseq.letter array")));
                }
            }
            const auto packed = writer.finish(length, unit_count);
            if (!packed) {
                ereport(ERROR,
                        (errcode(packed.status
                                == bioseq::sequences::rank_pack_status::
                                    capacity_overflow
                            ? ERRCODE_PROGRAM_LIMIT_EXCEEDED
                            : ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq.letter array rank packing failed"),
                         errdetail("rank_pack_status=%u",
                                   static_cast<unsigned>(packed.status))));
            }
            result->padding_letters = packed.padding_letters;
        });
    if (result == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq.letter array type dispatch produced no result")));
    }
    return PointerGetDatum(result);
}

} // namespace

export namespace bioseq::pg_types::sql_routines {

/**
 * @brief Append text array elements to a sequence with the || operator.
 * @param sequence Sequence prefix.
 * @param array Text elements in order, with no NULL elements.
 * @return The joined sequence. An empty array leaves sequence unchanged.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = "||",
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum seq_concat_text_array(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum sequence,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"text[]">,
    })]]
    ArrayType* array)
{
    const auto literal = text_array_literal(array);
    return concat_array_literal(
        sequence, literal, false, false);
}

/**
 * @brief Prepend text array elements to a sequence with the || operator.
 * @param array Text elements in order, with no NULL elements.
 * @param sequence Sequence suffix.
 * @return The joined sequence. An empny array leaves sequence unchanged.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = "||",
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum text_array_concat_seq(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"text[]">,
    })]]
    ArrayType* array,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum sequence)
{
    const auto literal = text_array_literal(array);
    return concat_array_literal(
        sequence, literal, true, false);
}

/**
 * @brief Append letter array elements to a sequence with the || operator.
 * @param sequence Sequence with a known alphabet.
 * @param array Non-NULL letters using the sequence's alphabet.
 * @return The joined sequence. An empty array leaves sequence unchanged.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = "||",
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum seq_concat_letter_array(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum sequence,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter[]">,
    })]]
    ArrayType* array)
{
    const auto literal = letter_array_literal(array);
    return concat_array_literal(
        sequence, literal, false, true);
}

/**
 * @brief Prepend letter array elements to a sequence with the || operator.
 * @param array Non-NULL letters using the sequence's alphabet.
 * @param sequence Sequence with a known alphabet.
 * @return The joined sequence. An empty array leaves sequence unchanged.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::operator_({
    .symbol = "||",
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum letter_array_concat_seq(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter[]">,
    })]]
    ArrayType* array,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum sequence)
{
    const auto literal = letter_array_literal(array);
    return concat_array_literal(
        sequence, literal, true, true);
}

/**
 * @brief Return the sequence length for array dimension 1.
 * @param input Sequence  to measure.
 * @param dimension Requested dimension.
 * @return Letter count for dimension 1, or SQL NULL for any other dimension.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
std::optional<std::int64_t> array_length(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input,
    std::int32_t dimension)
{
    if (dimension != 1) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(
        sequence_logical_length(input));
}

/**
 * @brief Count letters using sequence metadata without reading their contents.
 * @param input Sequence to measure.
 * @return Letter count as a signed 64-bit integer
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
std::int64_t cardinality(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    return static_cast<std::int64_t>(
        sequence_logical_length(input));
}

/**
 * @brief Convert a sequence to PostgreSQL text for an assignment cast.
 * @param input Sequence, whose letters are written as text.
 * @return Text allocated in the current memory context.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::cast({
    .assignment = true,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::text,
})]]
text* to_text(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    return materialize_pg_sequence_text(
        input, materialization_reason::sql_conversion);
}

/**
 * @brief Convert a sequence to PostgreSQL varchar for an assignment cast.
 * @param input Sequence  whose letters are written as text.
 * @return A varchar value allocated in the current memory context.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::cast({
    .assignment = true,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::varchar,
})]]
text* to_varchar(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    return materialize_pg_sequence_text(
        input, materialization_reason::sql_conversion);
}

/**
 * @brief Copy a sequence's letters into a bioseq.letter[] rray.
 * @param input Sequence with a known alphabet.
 * @return An allocated one-dimensional array, or an empty PostgreSQL array for
 * a sequence of length zero.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::custom_type<"bioseq.letter[]">,
})]]
ArrayType* to_array(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
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
            0u, 0u, static_cast<std::uint64_t>(bioseq::postgres::support::varlena_4byte_size(result)), false,
            static_cast<std::uint64_t>(bioseq::postgres::support::varlena_4byte_size(result)), trace_started);
        return result;
    }
    if (descriptor.bp_length >
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq sequence is too long to materialize as letter[]")));
    }
    const int count = static_cast<int>(descriptor.bp_length);
    const Size values_bytes = checked_materialization_allocation_size(
        descriptor.bp_length * sizeof(Datum), 0u,
        "letter array workspace");
    (void) checked_materialization_allocation_size(
        descriptor.bp_length * sizeof(bioseq::pg_types::letter),
        ARR_OVERHEAD_NONULLS(1), "letter array");
    auto* values = static_cast<Datum*>(palloc(values_bytes));
    const std::uint8_t alphabet = descriptor.alphabet_code;
    with_pg_sequence_view<std::monostate>(
        input,
        [values, count, alphabet](auto view) {
            std::array<std::uint8_t, 256> scratch{};
            int position = 0;
            while (position < count) {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<int>(static_cast<int>(scratch.size()),
                                  count - position));
                view.template copy_ranks<false, false>(
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
        + static_cast<std::uint64_t>(bioseq::postgres::support::varlena_4byte_size(result));
    pfree(values);
    record_slice_materialization_success(
        trace, producer_value_id,
        materialization_mechanism::letter_array,
        materialization_reason::sql_conversion,
        descriptor.bp_length, descriptor.bp_length,
        static_cast<std::uint64_t>(bioseq::postgres::support::varlena_4byte_size(result)), false,
        peak_retained_bytes, trace_started);
    return result;
}

/**
 * @brief Build a sequence using the first array element's alphabet.
 * @param array Nonempty one-dimensional array of non-NULL letters sharing an alphabet.
 * @param width_bits Bits per storage unit: 8, 16, 32, or 64.
 * @return A flat sequence allocated in the current memory context.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum from_array(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter[]">,
    })]]
    ArrayType* array,
    std::int32_t width_bits)
{
    return packed_from_letter_array(
        array, pgseq_unbound_alphabet_code, width_bits, true);
}

/**
 * @brief Build a sequence using a named alphabet, including from  an empty array.
 * @param array Empty or one-dimensional array of non-NULL letters matching the alphabet.
 * @param alphabet_name Supported alphabet name.
 * @param width_bits Bits per storage unit: 8, 16, 32, or 64.
 * @return A flat sequence allocated in the current memory context.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum from_array(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter[]">,
    })]]
    ArrayType* array,
    const std::string& alphabet_name,
    std::int32_t width_bits)
{
    const auto alphabet = parse_alphabet_name_checked(
        alphabet_name.c_str(), "bioseq.seq");
    return packed_from_letter_array(
        array, alphabet, width_bits, false);
}

}  // namespace bioseq::pg_types::sql_routines
