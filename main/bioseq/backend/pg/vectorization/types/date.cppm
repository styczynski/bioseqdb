/*-------------------------------------------------------------------------
 *
 * vectorization/types/date.cppm
 *
 * Vector comparison owners for PostgreSQL temporal types.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <utility>

export module bioseq.vector.types.date;

import bioseq.codegen.sql.annotation_model;
import bioseq.column.vector_compare_pg;
import bioseq.pg_utils.sql_boundary;

export namespace bioseq::vector::sql_routines {

using bioseq::column::vec::CmpOp;

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vdate_eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vdate_ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vdate_gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vdate_lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vdate_le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vdate_ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtime_eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtime_ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtime_gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtime_lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtime_le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtime_ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

}  // namespace bioseq::vector::sql_routines
