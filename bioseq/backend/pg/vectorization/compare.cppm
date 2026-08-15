/**
 * @file
 * Compares one vector column with one PostgreSQL scalar constant.
 */

/*-------------------------------------------------------------------------
 *
 * vectorization/compare.cppm
 *
 * Typed comparison of one vector column and one scalar constant.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "utils/builtins.h"
#include "utils/varlena.h"
}

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

export module bioseq.postgres.vectorization.compare;

export import bioseq.column.vector_compare;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support.sql_boundary;

export namespace bioseq::column {

using bioseq::column::vec::comparison_operation;

namespace detail {

template <comparison_operation p_operation>
/** Maps a comparison to the equivalent operation with exchanged operands. */
inline constexpr comparison_operation reversed_comparison_operation = [] {
    if constexpr (p_operation == comparison_operation::greater) {
        return comparison_operation::less;
    } else if constexpr (p_operation == comparison_operation::less) {
        return comparison_operation::greater;
    } else if constexpr (p_operation == comparison_operation::less_equal) {
        return comparison_operation::greater_equal;
    } else if constexpr (p_operation == comparison_operation::greater_equal) {
        return comparison_operation::less_equal;
    } else {
        return p_operation;
    }
}();

template <auto p_sql_type, class p_lane>
/** Owns a PostgreSQL vector result while the generic comparator populates it. */
class postgres_comparison_result_sink final {
  public:
    using result_type = bioseq::postgres::support::vector_result<p_sql_type, p_lane>;

    /** Allocates result lanes for one comparison batch. */
    void allocate(std::uint32_t lane_count) {
        result.emplace(result_type::allocate(lane_count));
    }

    /** Returns writable Boolean result lanes. */
    [[nodiscard]] bool* value_data() {
        return result->value_data();
    }

    /** Returns writable PostgreSQL null flags. */
    [[nodiscard]] bool* null_data() {
        return result->null_data();
    }

    /** Transfers the completed result to the caller. */
    [[nodiscard]] result_type release() && {
        return std::move(result).value();
    }

  private:
    std::optional<result_type> result;
};

template <comparison_operation p_operation, class p_column, class p_constant>
/** Applies a fixed-width comparison directly to contiguous vector lanes. */
[[nodiscard]] auto compare_contiguous_column(p_column column, p_constant constant) {
    const std::uint32_t lane_count = static_cast<std::uint32_t>(column.size());
    postgres_comparison_result_sink<bioseq::sql::pg::boolean, bool> sink;
    bioseq::column::vec::compare_column<p_operation>(
        column.contiguous_data(), column.null_data(), lane_count, constant, sink);
    return std::move(sink).release();
}

template <comparison_operation p_operation>
/** Compares normalized character values with PostgreSQL collation semantics. */
[[nodiscard]] bool compare_character_values(const bioseq::postgres::support::bpchar_view& left,
                                            const bioseq::postgres::support::bpchar_view& right) {
    const std::string_view left_bytes = left.normalized_bytes();
    const std::string_view right_bytes = right.normalized_bytes();
    if (left.collation() != right.collation()) {
        throw bioseq::postgres::support::scalar_conversion_error{
            "vector character operands carry different collations"};
    }
    const char* const left_data = left_bytes.data();
    const char* const right_data = right_bytes.data();
    const int left_size = static_cast<int>(left_bytes.size());
    const int right_size = static_cast<int>(right_bytes.size());
    const Oid collation = left.collation();
    const int comparison = bioseq::postgres::support::invoke_pg_as_cpp(
        [left_data, left_size, right_data, right_size, collation] {
            return varstr_cmp(left_data, left_size, right_data, right_size, collation);
        });
    return bioseq::column::vec::apply_comparison<p_operation>(comparison, 0);
}

template <comparison_operation p_operation, bool p_column_is_left, class p_column, class p_constant>
/** Compares decoded character lanes while preserving per-lane null flags. */
[[nodiscard]] auto compare_decoded_column(p_column column, const p_constant& constant) {
    using result_type = bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>;
    result_type result = result_type::allocate(column.size());
    auto result_values = result.values();
    auto result_nulls = result.nulls();
    for (std::size_t lane_index = 0; lane_index < column.size(); ++lane_index) {
        const bool is_null = column.is_null(lane_index);
        result_nulls[lane_index] = is_null;
        if (is_null) {
            result_values[lane_index] = false;
            continue;
        }
        auto lane = column.value(lane_index);
        if constexpr (p_column_is_left) {
            result_values[lane_index] = compare_character_values<p_operation>(lane, constant);
        } else {
            result_values[lane_index] = compare_character_values<p_operation>(constant, lane);
        }
    }
    return result;
}

} // namespace detail

template <comparison_operation p_operation,
          auto p_left_sql_type,
          class p_left_lane,
          auto p_right_sql_type,
          class p_right_lane>
/** Compares exactly one vector column operand with one scalar operand. */
[[nodiscard]] inline bioseq::postgres::support::vector_result<bioseq::sql::pg::boolean, bool>
compare_vector_operands(
    bioseq::postgres::support::vector_operand<p_left_sql_type, p_left_lane> left,
    bioseq::postgres::support::vector_operand<p_right_sql_type, p_right_lane> right) {
    const bool column_is_left = left.is_column() && right.is_constant();
    const bool column_is_right = left.is_constant() && right.is_column();
    if (!column_is_left && !column_is_right) {
        throw bioseq::postgres::support::scalar_conversion_error{
            "vector comparison requires exactly one column and one "
            "constant"};
    }

    constexpr bool requires_decoded_lanes =
        std::same_as<p_left_lane, bioseq::postgres::support::bpchar_view> ||
        std::same_as<p_right_lane, bioseq::postgres::support::bpchar_view>;
    if constexpr (!requires_decoded_lanes) {
        if (column_is_left) {
            return detail::compare_contiguous_column<p_operation>(left.column(), right.constant());
        }
        return detail::compare_contiguous_column<
            detail::reversed_comparison_operation<p_operation>>(right.column(), left.constant());
    } else {
        if (column_is_left) {
            auto constant = right.constant();
            return detail::compare_decoded_column<p_operation, true>(left.column(), constant);
        }
        auto constant = left.constant();
        return detail::compare_decoded_column<p_operation, false>(right.column(), constant);
    }
}

} // namespace bioseq::column
