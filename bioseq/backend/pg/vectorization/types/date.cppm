/*-------------------------------------------------------------------------
 *
 * vectorization/types/date.cppm
 *
 * Vector comparison owners for PostgreSQL temporal types.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

#include <utility>

export module bioseq.postgres.vectorization.types.date;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.vectorization.compare;
import bioseq.postgres.support.sql_boundary;

export namespace bioseq::vector::sql_routines {

using bioseq::column::vec::comparison_operation;

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vdate_eq(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vdate_ne(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vdate_gt(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vdate_lt(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vdate_le(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vdate_ge(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtime_eq(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtime_ne(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtime_gt(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtime_lt(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtime_le(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtime_ge(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_eq(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_ne(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_gt(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_lt(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_le(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamp_ge(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_eq(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_ne(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_gt(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_lt(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_le(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
vtimestamptz_ge(
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> left,
    bioseq::postgres::support::vector_operand<
        bioseq::sql::pg::timestamptz, TimestampTz> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

}  // namespace bioseq::vector::sql_routines
