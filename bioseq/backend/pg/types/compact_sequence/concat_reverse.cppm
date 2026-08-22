/**
 * @file concat_reverse.cppm
 * @brief Concatenation and constant-time logical transform SQL adapters.
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
 * @brief Builds an unbound sequence Datum from a text fragment.
 * @param[in] value ASCII symbols copied into the current PostgreSQL memory context.
 * @return Newly allocated unbound sequence Datum.
 */
[[nodiscard]] inline Datum text_sequence_literal(std::string_view value)
{
    return make_unbound_sequence_from_ascii(
        value.data(), value.size());
}

/**
 * @brief Concatenates a temporary text literal with an existing sequence.
 * @param[in] value ASCII text converted to an unbound sequence leaf.
 * @param[in] sequence Sequence Datum borrowed for the operation.
 * @param[in] text_on_left Whether the text precedes the sequence.
 * @return Persistent concatenation Datum allocated for the SQL result.
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
 * @brief Concatenates two sequence values without flattening their piece trees.
 * @param[in] left Left sequence operand.
 * @param[in] right Right sequence operand.
 * @return Sequence containing both operands in order.
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
 * @brief Appends a text value to a sequence.
 * @param[in] sequence Sequence prefix.
 * @param[in] value ASCII suffix copied into an unbound literal leaf.
 * @return Concatenated sequence Datum.
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
 * @brief Prepends a text value to a sequence.
 * @param[in] value ASCII prefix copied into an unbound literal leaf.
 * @param[in] sequence Sequence suffix.
 * @return Concatenated sequence Datum.
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
 * @brief Appends a `varchar` value to a sequence.
 * @param[in] sequence Sequence prefix.
 * @param[in] value ASCII suffix copied into an unbound literal leaf.
 * @return Concatenated sequence Datum.
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
 * @brief Prepends a `varchar` value to a sequence.
 * @param[in] value ASCII prefix copied into an unbound literal leaf.
 * @param[in] sequence Sequence suffix.
 * @return Concatenated sequence Datum.
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
 * @brief Returns a logical reverse view of a sequence.
 * @param[in] input Sequence Datum whose root transform is composed with reversal.
 * @return Reversed sequence without eager payload materialization.
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
 * @brief Returns a logical reverse-complement view of a bound sequence.
 * @param[in] input Sequence using an alphabet with a complete complement map.
 * @return Reverse-complement sequence without eager payload materialization.
 * @note Unbound or non-complementable alphabets report a PostgreSQL error.
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
 * @brief Implements PostgreSQL array-style concatenation for two sequences.
 * @param[in] left Left sequence operand.
 * @param[in] right Right sequence operand.
 * @return Sequence containing both operands in order.
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
