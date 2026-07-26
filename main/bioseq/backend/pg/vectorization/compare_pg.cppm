/*-------------------------------------------------------------------------
 *
 * vectorization/compare_pg.cppm
 *
 * Typed comparison of one vector column and one scalar constant.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "utils/builtins.h"
#include "utils/varlena.h"
}

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

export module bioseq.column.vector_compare_pg;

export import bioseq.column.vector_compare;
import bioseq.codegen.sql.annotation_model;
import bioseq.pg_utils.sql_boundary;

export namespace bioseq::column {

using bioseq::column::vec::CmpOp;

template<CmpOp Operation>
inline constexpr CmpOp inverse_comparison =
    Operation == CmpOp::gt ? CmpOp::lt
    : Operation == CmpOp::lt ? CmpOp::gt
    : Operation == CmpOp::ge ? CmpOp::le
    : Operation == CmpOp::le ? CmpOp::ge
    : Operation;

class vector_result_sink final {
public:
    void allocate(std::uint32_t lane_count) {
        result_.emplace(
            bioseq::pg_utils::vector_result<
                bioseq::sql::pg::boolean, bool>::allocate(lane_count));
    }

    [[nodiscard]] bool* value_data() {
        return result_->values().data();
    }

    [[nodiscard]] bool* null_data() {
        return result_->nulls().data();
    }

    [[nodiscard]] bioseq::pg_utils::vector_result<
        bioseq::sql::pg::boolean, bool>
    take() {
        return std::move(*result_);
    }

private:
    std::optional<bioseq::pg_utils::vector_result<
        bioseq::sql::pg::boolean, bool>> result_;
};

static_assert(
    bioseq::column::vec::is_compare_result_sink<vector_result_sink>);

template<CmpOp Operation, class Left, class Right>
[[nodiscard]] inline bool compare_scalar_values(
        const Left& left,
        const Right& right) {
    if constexpr (Operation == CmpOp::eq) {
        return left == right;
    } else if constexpr (Operation == CmpOp::ne) {
        return left != right;
    } else if constexpr (Operation == CmpOp::gt) {
        return left > right;
    } else if constexpr (Operation == CmpOp::lt) {
        return left < right;
    } else if constexpr (Operation == CmpOp::le) {
        return left <= right;
    } else {
        return left >= right;
    }
}

template<CmpOp Operation>
[[nodiscard]] inline bool compare_scalar_values(
        const bioseq::pg_utils::bpchar_view& left,
        const bioseq::pg_utils::bpchar_view& right) {
    const std::string_view left_bytes = left.normalized_bytes();
    const std::string_view right_bytes = right.normalized_bytes();
    if (left.collation() != right.collation()) {
        throw bioseq::pg_utils::sql_boundary_conversion_error{
            "vector character operands carry different collations"};
    }
    const char* const left_data = left_bytes.data();
    const char* const right_data = right_bytes.data();
    const int left_size = static_cast<int>(left_bytes.size());
    const int right_size = static_cast<int>(right_bytes.size());
    const Oid collation = left.collation();
    const int comparison = bioseq::pg_utils::invoke_pg_as_cpp(
        [left_data, left_size, right_data, right_size, collation] {
            return varstr_cmp(
                left_data,
                left_size,
                right_data,
                right_size,
                collation);
        });
    return compare_scalar_values<Operation>(comparison, 0);
}

template<
    CmpOp Operation,
    auto LeftSqlType,
    class LeftLane,
    auto RightSqlType,
    class RightLane>
[[nodiscard]] inline bioseq::pg_utils::vector_result<
    bioseq::sql::pg::boolean, bool>
compare_vector_operands(
        bioseq::pg_utils::vector_operand<
            LeftSqlType, LeftLane> left,
        bioseq::pg_utils::vector_operand<
            RightSqlType, RightLane> right) {
    const bool column_is_left =
        left.is_column() && right.is_constant();
    const bool column_is_right =
        left.is_constant() && right.is_column();
    if (!column_is_left && !column_is_right) {
        throw bioseq::pg_utils::sql_boundary_conversion_error{
            "vector comparison requires exactly one column and one "
            "constant"};
    }

    constexpr bool character_comparison =
        std::same_as<LeftLane, bioseq::pg_utils::bpchar_view>
        || std::same_as<RightLane, bioseq::pg_utils::bpchar_view>;
    if constexpr (!character_comparison) {
        vector_result_sink sink;
        if (column_is_left) {
            auto column = left.column();
            const auto constant = right.constant();
            const std::span<const LeftLane> values =
                column.contiguous_values();
            const std::span<const bool> nulls = column.nulls();
            bioseq::column::vec::compare_column<
                Operation, LeftLane, RightLane>(
                    values.data(),
                    nulls.data(),
                    static_cast<std::uint32_t>(column.size()),
                    constant,
                    sink);
        } else {
            auto column = right.column();
            const auto constant = left.constant();
            const std::span<const RightLane> values =
                column.contiguous_values();
            const std::span<const bool> nulls = column.nulls();
            bioseq::column::vec::compare_column<
                inverse_comparison<Operation>,
                RightLane,
                LeftLane>(
                    values.data(),
                    nulls.data(),
                    static_cast<std::uint32_t>(column.size()),
                    constant,
                    sink);
        }
        return sink.take();
    } else {
        const std::size_t lane_count = column_is_left
            ? left.column().size()
            : right.column().size();
        auto result = bioseq::pg_utils::vector_result<
            bioseq::sql::pg::boolean, bool>::allocate(lane_count);
        std::span<bool> result_values = result.values();
        std::span<bool> result_nulls = result.nulls();

        if (column_is_left) {
            auto column = left.column();
            auto constant = right.constant();
            for (std::size_t lane_index = 0;
                 lane_index < lane_count;
                 ++lane_index) {
                result_nulls[lane_index] = column.is_null(lane_index);
                if (result_nulls[lane_index]) {
                    result_values[lane_index] = false;
                    continue;
                }
                auto lane = column.value(lane_index);
                result_values[lane_index] =
                    compare_scalar_values<Operation>(lane, constant);
            }
        } else {
            auto constant = left.constant();
            auto column = right.column();
            for (std::size_t lane_index = 0;
                 lane_index < lane_count;
                 ++lane_index) {
                result_nulls[lane_index] = column.is_null(lane_index);
                if (result_nulls[lane_index]) {
                    result_values[lane_index] = false;
                    continue;
                }
                auto lane = column.value(lane_index);
                result_values[lane_index] =
                    compare_scalar_values<Operation>(constant, lane);
            }
        }
        return result;
    }
}

}  // namespace bioseq::column
