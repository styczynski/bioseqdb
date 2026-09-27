/**
 * @file concat_reverse.cppm
 * SQL functions for joining sequences and reversing their letters.
 */
module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/builtins.h"
}

export module bioseq.postgres.types.compact_sequence:concat_reverse;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :io;
import :tree_types;
import :view_session;
import :packed_output;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;

namespace {

/**
 * @brief Copy text into a sequence whose alphabet has not been chosen yet.
 * @param[in] value ASCII symbols to copy.
 * @return A sequence allocated in the current PostgreSQL memory context.
 */
[[nodiscard]] inline Datum text_sequence_literal(std::string_view value)
{
    return make_unbound_sequence_from_ascii(
        value.data(), value.size());
}

/**
 * @brief Join text with a sequence and release the temporary text sequence.
 * @param[in] value ASCII text to copy into the result.
 * @param[in] sequence Existing sequence.
 * @param[in] text_on_left Place text before sequence when true.
 * @return The joined sequence.
 */
[[nodiscard]] inline Datum concat_text_and_sequence(
    std::string_view value,
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

export namespace bioseq::pg_types::sql_routines {

/**
 * @brief Join sequences by joining their piece trees.
 * Existing pieces can be shared. Text with no alphabet is checked and packed
 * when the other operand supplies an alphabet.
 * @param[in] left Sequence prefix.
 * @param[in] right Sequence suffix.
 * @return The joined sequence.
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
Datum seq_concat(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right)
{
    return concat_pg_sequences(left, right);
}

/**
 * @brief Append text to a sequence.
 * @param[in] sequence Sequence prefix.
 * @param[in] value ASCII suffix, checked against the sequence alphabet when bound.
 * @return The joined sequence.
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
Datum seq_concat_text(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum sequence,
    const std::string& value)
{
    return concat_text_and_sequence(value, sequence, false);
}

/**
 * @brief Prepend text to a sequence.
 * @param[in] value ASCII prefix, checked against the sequence alphabet when bound.
 * @param[in] sequence Sequence suffix.
 * @return The joined sequence.
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
Datum text_concat_seq(
    const std::string& value,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum sequence)
{
    return concat_text_and_sequence(value, sequence, true);
}

/**
 * @brief Append varchar text to a sequence.
 * @param[in] sequence Sequence prefix.
 * @param[in] value ASCII suffix, checked against the sequence alphabet when bound.
 * @return The joined sequence.
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
Datum seq_concat_varchar(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum sequence,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::varchar,
    })]]
    const std::string& value)
{
    return concat_text_and_sequence(value, sequence, false);
}

/**
 * @brief Prepend varchar text to a sequence.
 * @param[in] value ASCII prefix, checked against the sequence alphabet when bound.
 * @param[in] sequence Sequence suffix.
 * @return The joined sequence.
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
Datum varchar_concat_seq(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::varchar,
    })]]
    const std::string& value,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum sequence)
{
    return concat_text_and_sequence(value, sequence, true);
}

/**
 * @brief Return a sequence with its letter order reversed.
 * The piece tree records the reversal so its letters can be read in reverse
 * order when needed.
 * @param[in] input Sequence to reverse.
 * @return The reversed sequence.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum reverse(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    return transform_pg_sequence(
        input, bioseq::storage::sequence_transform::reversed);
}

/**
 * @brief Reverse a sequence and replace each letter with its complement.
 * The piece tree records this operation until the letters are read.
 * @param[in] input Sequence with a known alphabet and a complement for every letter.
 * @return The reverse-complement sequence.
 * @throws PostgreSQL ERROR if the alphabet is unknown or lacks a complete complement map.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum reverse_complement(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input)
{
    Datum dispatch = input;
    bioseq::pg_types::seq* flat = nullptr;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
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
    return transform_pg_sequence(
        input, bioseq::storage::sequence_transform::reverse_complement);
}

/**
 * @brief Join two sequences through the SQL array_cat function.
 * @param[in] left Sequence prefix.
 * @param[in] right Sequence suffix.
 * @return The joined sequence.
 */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum array_cat(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum right)
{
    return concat_pg_sequences(left, right);
}

}  // namespace bioseq::pg_types::sql_routines
