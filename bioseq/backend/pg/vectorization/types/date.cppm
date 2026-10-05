/**
 * @file
 * Compare a date or time vector with a scalar of the  same SQL type.
 * Exactly one operand must be a vector. NULL rows stay NULL in the result.
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

/** Compare DATE values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vdate_eq for this date column and date scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value date) USING bioseq;
 * INSERT INTO doc_values VALUES (DATE '2026-01-01'), (DATE '2026-01-02'), (NULL);
 * SELECT value FROM doc_values WHERE value = DATE '2026-01-02';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vdate_eq(bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether DATE values differ.
 * ```sql
 * -- Bioseq vectorized scans use vdate_ne for this date column and date scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value date) USING bioseq;
 * INSERT INTO doc_values VALUES (DATE '2026-01-01'), (DATE '2026-01-02'), (NULL);
 * SELECT value FROM doc_values WHERE value <> DATE '2026-01-02';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vdate_ne(bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for DATE values.
 * ```sql
 * -- Bioseq vectorized scans use vdate_gt for this date column and date scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value date) USING bioseq;
 * INSERT INTO doc_values VALUES (DATE '2026-01-01'), (DATE '2026-01-02'), (NULL);
 * SELECT value FROM doc_values WHERE value > DATE '2026-01-02';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vdate_gt(bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for DATE values.
 * ```sql
 * -- Bioseq vectorized scans use vdate_lt for this date column and date scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value date) USING bioseq;
 * INSERT INTO doc_values VALUES (DATE '2026-01-01'), (DATE '2026-01-02'), (NULL);
 * SELECT value FROM doc_values WHERE value < DATE '2026-01-02';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vdate_lt(bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for DATE values.
 * ```sql
 * -- Bioseq vectorized scans use vdate_le for this date column and date scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value date) USING bioseq;
 * INSERT INTO doc_values VALUES (DATE '2026-01-01'), (DATE '2026-01-02'), (NULL);
 * SELECT value FROM doc_values WHERE value <= DATE '2026-01-02';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vdate_le(bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for DATE values.
 * ```sql
 * -- Bioseq vectorized scans use vdate_ge for this date column and date scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value date) USING bioseq;
 * INSERT INTO doc_values VALUES (DATE '2026-01-01'), (DATE '2026-01-02'), (NULL);
 * SELECT value FROM doc_values WHERE value >= DATE '2026-01-02';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vdate_ge(bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::date, DateADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare TIME values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vtime_eq for this time column and time scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value time) USING bioseq;
 * INSERT INTO doc_values VALUES (TIME '01:00:00'), (TIME '02:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value = TIME '02:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vtime_eq(bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether TIME values differ.
 * ```sql
 * -- Bioseq vectorized scans use vtime_ne for this time column and time scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value time) USING bioseq;
 * INSERT INTO doc_values VALUES (TIME '01:00:00'), (TIME '02:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value <> TIME '02:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vtime_ne(bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for TIME values.
 * ```sql
 * -- Bioseq vectorized scans use vtime_gt for this time column and time scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value time) USING bioseq;
 * INSERT INTO doc_values VALUES (TIME '01:00:00'), (TIME '02:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value > TIME '02:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vtime_gt(bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for TIME values.
 * ```sql
 * -- Bioseq vectorized scans use vtime_lt for this time column and time scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value time) USING bioseq;
 * INSERT INTO doc_values VALUES (TIME '01:00:00'), (TIME '02:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value < TIME '02:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vtime_lt(bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for TIME values.
 * ```sql
 * -- Bioseq vectorized scans use vtime_le for this time column and time scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value time) USING bioseq;
 * INSERT INTO doc_values VALUES (TIME '01:00:00'), (TIME '02:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value <= TIME '02:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vtime_le(bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for TIME values.
 * ```sql
 * -- Bioseq vectorized scans use vtime_ge for this time column and time scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value time) USING bioseq;
 * INSERT INTO doc_values VALUES (TIME '01:00:00'), (TIME '02:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value >= TIME '02:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool>
        vtime_ge(bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> left,
                 bioseq::postgres::support::vector_operand<bioseq::sql::pg::time, TimeADT> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare TIMESTAMP values for equality.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamp_eq for this timestamp column and timestamp scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamp) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMP '2026-01-01 12:00:00'), (TIMESTAMP '2026-01-02 12:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value = TIMESTAMP '2026-01-02 12:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamp_eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether TIMESTAMP values differ.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamp_ne for this timestamp column and timestamp scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamp) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMP '2026-01-01 12:00:00'), (TIMESTAMP '2026-01-02 12:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value <> TIMESTAMP '2026-01-02 12:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamp_ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for TIMESTAMP values.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamp_gt for this timestamp column and timestamp scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamp) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMP '2026-01-01 12:00:00'), (TIMESTAMP '2026-01-02 12:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value > TIMESTAMP '2026-01-02 12:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamp_gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for TIMESTAMP values.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamp_lt for this timestamp column and timestamp scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamp) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMP '2026-01-01 12:00:00'), (TIMESTAMP '2026-01-02 12:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value < TIMESTAMP '2026-01-02 12:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamp_lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for TIMESTAMP values.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamp_le for this timestamp column and timestamp scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamp) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMP '2026-01-01 12:00:00'), (TIMESTAMP '2026-01-02 12:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value <= TIMESTAMP '2026-01-02 12:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamp_le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for TIMESTAMP values.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamp_ge for this timestamp column and timestamp scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamp) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMP '2026-01-01 12:00:00'), (TIMESTAMP '2026-01-02 12:00:00'), (NULL);
 * SELECT value FROM doc_values WHERE value >= TIMESTAMP '2026-01-02 12:00:00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamp_ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamp, Timestamp> right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

/** Compare TIMESTAMPTZ values for equality
 * ```sql
 * -- Bioseq vectorized scans use vtimestamptz_eq for this timestamptz column and timestamptz scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamptz) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMPTZ '2026-01-01 12:00:00+00'), (TIMESTAMPTZ '2026-01-02 12:00:00+00'), (NULL);
 * SELECT value FROM doc_values WHERE value = TIMESTAMPTZ '2026-01-02 12:00:00+00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamptz_eq(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz>
            right) {
    return bioseq::column::compare_vector_operands<comparison_operation::equal>(std::move(left),
                                                                                std::move(right));
}

/** Test whether TIMESTAMPTZ values differ.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamptz_ne for this timestamptz column and timestamptz scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamptz) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMPTZ '2026-01-01 12:00:00+00'), (TIMESTAMPTZ '2026-01-02 12:00:00+00'), (NULL);
 * SELECT value FROM doc_values WHERE value <> TIMESTAMPTZ '2026-01-02 12:00:00+00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamptz_ne(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz>
            right) {
    return bioseq::column::compare_vector_operands<comparison_operation::not_equal>(
        std::move(left), std::move(right));
}

/** Test left > right for TIMESTAMPTZ values.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamptz_gt for this timestamptz column and timestamptz scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamptz) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMPTZ '2026-01-01 12:00:00+00'), (TIMESTAMPTZ '2026-01-02 12:00:00+00'), (NULL);
 * SELECT value FROM doc_values WHERE value > TIMESTAMPTZ '2026-01-02 12:00:00+00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamptz_gt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz>
            right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater>(std::move(left),
                                                                                  std::move(right));
}

/** Test left < right for TIMESTAMPTZ values.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamptz_lt for this timestamptz column and timestamptz scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamptz) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMPTZ '2026-01-01 12:00:00+00'), (TIMESTAMPTZ '2026-01-02 12:00:00+00'), (NULL);
 * SELECT value FROM doc_values WHERE value < TIMESTAMPTZ '2026-01-02 12:00:00+00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamptz_lt(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz>
            right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less>(std::move(left),
                                                                               std::move(right));
}

/** Test left <= right for TIMESTAMPTZ values.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamptz_le for this timestamptz column and timestamptz scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamptz) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMPTZ '2026-01-01 12:00:00+00'), (TIMESTAMPTZ '2026-01-02 12:00:00+00'), (NULL);
 * SELECT value FROM doc_values WHERE value <= TIMESTAMPTZ '2026-01-02 12:00:00+00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamptz_le(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz>
            right) {
    return bioseq::column::compare_vector_operands<comparison_operation::less_equal>(
        std::move(left), std::move(right));
}

/** Test left >= right for TIMESTAMPTZ values.
 * ```sql
 * -- Bioseq vectorized scans use vtimestamptz_ge for this timestamptz column and timestamptz scalar comparison. NULL input rows do not pass the filter.
 * BEGIN;
 * SET LOCAL bioseq.vectorization.enabled = on;
 * CREATE TABLE doc_values (value timestamptz) USING bioseq;
 * INSERT INTO doc_values VALUES (TIMESTAMPTZ '2026-01-01 12:00:00+00'), (TIMESTAMPTZ '2026-01-02 12:00:00+00'), (NULL);
 * SELECT value FROM doc_values WHERE value >= TIMESTAMPTZ '2026-01-02 12:00:00+00';
 * ROLLBACK;
 * ```
 */
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]] bioseq::postgres::
    support::vector_result<bioseq::sql::pg::boolean, bool> vtimestamptz_ge(
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz> left,
        bioseq::postgres::support::vector_operand<bioseq::sql::pg::timestamptz, TimestampTz>
            right) {
    return bioseq::column::compare_vector_operands<comparison_operation::greater_equal>(
        std::move(left), std::move(right));
}

} // namespace bioseq::vector::sql_routines
