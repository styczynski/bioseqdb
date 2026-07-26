/*-------------------------------------------------------------------------
 *
 * vectorization/types/int.cppm
 *
 * Vector comparison owners for PostgreSQL character and integer types.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <cstdint>
#include <utility>

export module bioseq.vector.types.integer;

import bioseq.codegen.sql.annotation_model;
import bioseq.column.vector_compare_pg;
import bioseq.pg_utils.sql_boundary;

export namespace bioseq::vector::sql_routines {

using bioseq::column::vec::CmpOp;

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vbpchareq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vbpcharne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vbpchargt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vbpcharlt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vbpcharle(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vbpcharge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::character,
        bioseq::pg_utils::bpchar_view> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint2eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint2ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint2gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint2lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint2le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint2ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint24eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint24ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint24gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint24lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint24le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint24ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint28eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint28ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint28gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint28lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint28le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint28ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint4eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint4ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint4gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint4lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint4le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint4ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint42eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint42ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint42gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint42lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint42le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint42ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint48eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint48ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint48gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint48lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint48le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint48ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint8eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint8ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint8gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint8lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint8le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint8ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint82eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint82ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint82gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint82lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint82le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint82ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::smallint, std::int16_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint84eq(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::eq>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint84ne(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ne>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint84gt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::gt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint84lt(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::lt>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint84le(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::le>(
        std::move(left), std::move(right));
}

[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
bioseq::pg_utils::vector_result<bioseq::sql::pg::boolean, bool>
vint84ge(
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::bigint, std::int64_t> left,
    bioseq::pg_utils::vector_operand<
        bioseq::sql::pg::integer, std::int32_t> right) {
    return bioseq::column::compare_vector_operands<CmpOp::ge>(
        std::move(left), std::move(right));
}

}  // namespace bioseq::vector::sql_routines
