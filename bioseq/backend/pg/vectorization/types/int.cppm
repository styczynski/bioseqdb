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

/** Compare CHARACTER values for  equality.
 * ```sql
 * -- Bioseq vectorized scans use vbpchareq for this char column and char scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value char) USING bioseq;
 * INSERT INTO doc_values VALUES ('A'::char), ('B'::char), (NULL);
 * SELECT value FROM doc_values WHERE value = 'B'::char;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpchareq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether CHARACTER values differ.
 * ```sql
 * -- Bioseq vectorized scans use vbpcharne for this char column and char scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value char) USING bioseq;
 * INSERT INTO doc_values VALUES ('A'::char), ('B'::char), (NULL);
 * SELECT value FROM doc_values WHERE value <> 'B'::char;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpcharne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for CHARACTER values.
 * ```sql
 * -- Bioseq vectorized scans use vbpchargt for this char column and char scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value char) USING bioseq;
 * INSERT INTO doc_values VALUES ('A'::char), ('B'::char), (NULL);
 * SELECT value FROM doc_values WHERE value > 'B'::char;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpchargt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for CHARACTER values.
 * ```sql
 * -- Bioseq vectorized scans use vbpcharlt for this char column and char scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value char) USING bioseq;
 * INSERT INTO doc_values VALUES ('A'::char), ('B'::char), (NULL);
 * SELECT value FROM doc_values WHERE value < 'B'::char;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpcharlt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for CHARACTER values.
 * ```sql
 * -- Bioseq vectorized scans use vbpcharle for this char column and char scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value char) USING bioseq;
 * INSERT INTO doc_values VALUES ('A'::char), ('B'::char), (NULL);
 * SELECT value FROM doc_values WHERE value <= 'B'::char;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpcharle(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for CHARACTER values.
 * ```sql
 * -- Bioseq vectorized scans use vbpcharge for this char column and char scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value char) USING bioseq;
 * INSERT INTO doc_values VALUES ('A'::char), ('B'::char), (NULL);
 * SELECT value FROM doc_values WHERE value >= 'B'::char;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vbpcharge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::character,
                                                  bioseq::postgres::support::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare SMALLINT values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vint2eq for this smallint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether SMALLINT values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint2ne for this smallint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for SMALLINT values.
 * ```sql
 * -- Bioseq vectorized scans use vint2gt for this smallint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for SMALLINT values.
 * ```sql
 * -- Bioseq vectorized scans use vint2lt for this smallint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for SMALLINT values.
 * ```sql
 * -- Bioseq vectorized scans use vint2le for this smallint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for SMALLINT values.
 * ```sql
 * -- Bioseq vectorized scans use vint2ge for this smallint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint2ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare SMALLINT and INTEGER values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vint24eq for this smallint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether SMALLINT and INTEGER values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint24ne for this smallint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test SMALLINT left > INTEGER right.
 * ```sql
 * -- Bioseq vectorized scans use vint24gt for this smallint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test SMALLINT left < INTEGER right.
 * ```sql
 * -- Bioseq vectorized scans use vint24lt for this smallint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test SMALLINT left <= INTEGER right.
 * ```sql
 * -- Bioseq vectorized scans use vint24le for this smallint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test SMALLINT left >= INTEGER right.
 * ```sql
 * -- Bioseq vectorized scans use vint24ge for this smallint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint24ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare SMALLINT and BIGINT values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vint28eq for this smallint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test wherher SMALLINT and BIGINT values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint28ne for this smallint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test SMALLINT left > BIGINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint28gt for this smallint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test SMALLINT left < BIGINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint28lt for this smallint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test SMALLINT left <= BIGINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint28le for this smallint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test SMALLINT left >= BIGINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint28ge for this smallint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value smallint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::smallint), (2::smallint), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint28ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare INTEGER values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vint4eq for this integer column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether INTEGER values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint4ne for this integer column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for INTEGER values.
 * ```sql
 * -- Bioseq vectorized scans use vint4gt for this integer column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for INTEGER values.
 * ```sql
 * -- Bioseq vectorized scans use vint4lt for this integer column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for INTEGER values.
 * ```sql
 * -- Bioseq vectorized scans use vint4le for this integer column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for INTEGER values.
 * ```sql
 * -- Bioseq vectorized scans use vint4ge for this integer column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint4ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare INTEGER and SMALLINT values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vint42eq for this integer column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether INTEGER and SMALLINT values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint42ne for this integer column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test INTEGER left > SMALLINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint42gt for this integer column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test INTEGER left < SMALLINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint42lt for this integer column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test INTEGER left <= SMALLINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint42le for this integer column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test INTEGER left >= SMALLINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint42ge for this integer column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint42ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare INTEGER and BIGINT values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vint48eq for this integer column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether INTEGER and BIGINT values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint48ne for this integer column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test INTEGER left > BIGINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint48gt for this integer column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test INTEGER left < BIGINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint48lt for this integer column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test INTEGER left <= BIGINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint48le for this integer column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test INTEGER left >= BIGINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint48ge for this integer column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value integer) USING bioseq;
 * INSERT INTO doc_values VALUES (1::integer), (2::integer), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint48ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare BIGINT values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vint8eq for this bigint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether BIGINT values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint8ne for this bigint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for BIGINT values.
 * ```sql
 * -- Bioseq vectorized scans use vint8gt for this bigint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for BIGINT values.
 * ```sql
 * -- Bioseq vectorized scans use vint8lt for this bigint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for BIGINT values.
 * ```sql
 * -- Bioseq vectorized scans use vint8le for this bigint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for BIGINT values.
 * ```sql
 * -- Bioseq vectorized scans use vint8ge for this bigint column and bigint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::bigint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint8ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare BIGINT and SMALLINT values for  equality.
 * ```sql
 * -- Bioseq vectorized scans use vint82eq for this bigint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether BIGINT and SMALLINT values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint82ne for this bigint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test BIGINT left > SMALLINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint82gt for this bigint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test BIGINT left < SMALLINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint82lt for this bigint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test BIGINT left <= SMALLINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint82le for this bigint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test BIGINT left >= SMALLINT right.
 * ```sql
 * -- Bioseq vectorized scans use vint82ge for this bigint column and smallint scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::smallint;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint82ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare BIGINT and INTEGER values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vint84eq for this bigint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value = 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether BIGINT and INTEGER values differ.
 * ```sql
 * -- Bioseq vectorized scans use vint84ne for this bigint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value <> 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test BIGINT left > INTEGER right.
 * ```sql
 * -- Bioseq vectorized scans use vint84gt for this bigint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value > 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test BIGINT left < INTEGER right.
 * ```sql
 * -- Bioseq vectorized scans use vint84lt for this bigint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value < 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test BIGINT left <= INTEGER right.
 * ```sql
 * -- Bioseq vectorized scans use vint84le for this bigint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value <= 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test BIGINT left >= INTEGER right.
 * ```sql
 * -- Bioseq vectorized scans use vint84ge for this bigint column and integer scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value bigint) USING bioseq;
 * INSERT INTO doc_values VALUES (1::bigint), (2::bigint), (NULL);
 * SELECT value FROM doc_values WHERE value >= 2::integer;
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vint84ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::bigint, std::int64_t> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

} // namespace bioseq::vector::sql_routines
