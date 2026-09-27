/**
 * @file
 * Compare an integer or CHARACTER vector with a scalar, preserving NULL rows.
 * Integer comparisons accept different integer widths. CHARACTER comparisons
 * ignore trailing spaces and use the PostgreSQL collation.
 */


module;

#include <bioseq/pg_compat.h>
#include <cstdint>
#include <utility>

export module bioseq.postgres.vectorization.types.integer;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.vectorization.compare;
import bioseq.postgres.support.sql_boundary;

export namespace bioseq::vector::sql_routines {

using bioseq::column::vec::comparison_operation;

/** Compare CHARACTER values for  equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpchareq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether CHARACTER values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpcharne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for CHARACTER values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpchargt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for CHARACTER values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpcharlt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for CHARACTER values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpcharle(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for CHARACTER values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpcharge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare SMALLINT values for equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether SMALLINT values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for SMALLINT values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for SMALLINT values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for SMALLINT values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for SMALLINT values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare SMALLINT and INTEGER values for equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether SMALLINT and INTEGER values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test SMALLINT left > INTEGER right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test SMALLINT left < INTEGER right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test SMALLINT left <= INTEGER right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test SMALLINT left >= INTEGER right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare SMALLINT and BIGINT values for equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test wherher SMALLINT and BIGINT values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test SMALLINT left > BIGINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test SMALLINT left < BIGINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test SMALLINT left <= BIGINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test SMALLINT left >= BIGINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare INTEGER values for equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether INTEGER values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for INTEGER values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for INTEGER values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for INTEGER values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for INTEGER values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare INTEGER and SMALLINT values for equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether INTEGER and SMALLINT values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test INTEGER left > SMALLINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test INTEGER left < SMALLINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test INTEGER left <= SMALLINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test INTEGER left >= SMALLINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare INTEGER and BIGINT values for equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether INTEGER and BIGINT values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test INTEGER left > BIGINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test INTEGER left < BIGINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test INTEGER left <= BIGINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test INTEGER left >= BIGINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare BIGINT values for equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether BIGINT values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for BIGINT values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for BIGINT values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for BIGINT values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for BIGINT values. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare BIGINT and SMALLINT values for  equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether BIGINT and SMALLINT values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test BIGINT left > SMALLINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test BIGINT left < SMALLINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test BIGINT left <= SMALLINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test BIGINT left >= SMALLINT right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare BIGINT and INTEGER values for equality. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether BIGINT and INTEGER values differ. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test BIGINT left > INTEGER right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test BIGINT left < INTEGER right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test BIGINT left <= INTEGER right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test BIGINT left >= INTEGER right. */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

} // namespace bioseq::vector::sql_routines
