/* Scalar concat and O(1) logical transform SQL adapters. */
module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "headers/pg_compat.h"
extern "C" {
#include "utils/builtins.h"
}

export module bioseq.types.pg_compact_sequence:concat_reverse;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :io;
import :tree_types;
import :view_session;
import :packed_output;
import bioseq.storage.coordinates;
import bioseq.storage.lazy;

namespace {

[[nodiscard]] inline Datum text_sequence_literal(std::string_view value)
{
    return make_unbound_sequence_from_ascii(
        value.data(), value.size());
}

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
