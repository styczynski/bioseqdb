/*-------------------------------------------------------------------------
 *
 * pg_utils/sql_boundary/aggregate.cppm
 *
 * Checked aggregate-state and Numeric result boundaries for generated fmgr
 * entrypoints.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "catalog/pg_type_d.h"
}

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>

export module bioseq.pg_utils.sql_boundary:aggregate;

import bioseq.pg_utils.error_bridge;
import bioseq.pg_sql_boundary;
import bioseq.vector.types;

export namespace bioseq::pg_utils {

/**
 * Validated view of the two-bigint array state used by smallint/integer AVG.
 *
 * PostgreSQL owns the array storage. Outside an aggregate call the decoder
 * copies the array before a transition mutates it, matching fmgr's normal
 * pass-by-reference convention.
 */
class int64_array_aggregate_state final {
public:
    [[nodiscard]] Int64AggState& value() const noexcept {
        return *reinterpret_cast<Int64AggState*>(
            ARR_DATA_PTR(array_));
    }

    [[nodiscard]] ArrayType* array() const noexcept {
        return array_;
    }

private:
    friend struct detail_aggregate_boundary_access;

    explicit int64_array_aggregate_state(ArrayType* array) noexcept
        : array_{array} {}

    ArrayType* array_ = nullptr;
};

/**
 * Validated non-owning view of PostgreSQL's int8 aggregate transition state.
 */
class int128_aggregate_state final {
public:
    [[nodiscard]] Int128AggState& value() const noexcept {
        return *state_;
    }

    [[nodiscard]] Int128AggState* get() const noexcept {
        return state_;
    }

    [[nodiscard]] static int128_aggregate_state from_new(
            Int128AggState* state) {
        if (state == nullptr) {
            throw sql_boundary_conversion_error{
                "cannot construct an aggregate state from a null pointer"};
        }
        return int128_aggregate_state{state};
    }

private:
    friend struct detail_aggregate_boundary_access;

    explicit int128_aggregate_state(Int128AggState* state) noexcept
        : state_{state} {}

    Int128AggState* state_ = nullptr;
};

/**
 * Typed, PostgreSQL-owned Numeric result.
 */
class numeric_result final {
public:
    [[nodiscard]] static numeric_result from_numeric(Numeric value) {
        if (value == nullptr) {
            throw sql_boundary_conversion_error{
                "cannot construct a Numeric result from a null pointer"};
        }
        return numeric_result{value};
    }

    [[nodiscard]] Numeric get() const noexcept {
        return value_;
    }

private:
    explicit numeric_result(Numeric value) noexcept
        : value_{value} {}

    Numeric value_ = nullptr;
};

namespace detail {

inline void validate_int64_array_state(ArrayType* array) {
    if (array == nullptr) {
        throw sql_boundary_conversion_error{
            "aggregate array state contains a null pointer"};
    }
    if (ARR_NDIM(array) != 1
        || ARR_DIMS(array)[0] != 2
        || ARR_LBOUND(array)[0] != 1
        || ARR_ELEMTYPE(array) != INT8OID
        || ARR_HASNULL(array)
        || ARR_SIZE(array)
            != ARR_OVERHEAD_NONULLS(1) + sizeof(Int64AggState)) {
        throw sql_boundary_conversion_error{
            "expected a one-dimensional, two-element bigint aggregate "
            "state array without NULLs"};
    }
    const auto* const state =
        reinterpret_cast<const Int64AggState*>(ARR_DATA_PTR(array));
    if (state->N < 0) {
        throw sql_boundary_conversion_error{
            "aggregate array state contains a negative row count"};
    }
}

inline void validate_int128_state(Int128AggState* state) {
    if (state == nullptr) {
        throw sql_boundary_conversion_error{
            "aggregate internal state contains a null pointer"};
    }
    if (state->N < 0 || state->calcSumX2) {
        throw sql_boundary_conversion_error{
            "aggregate internal state has an invalid count or mode"};
    }
}

}  // namespace detail

struct detail_aggregate_boundary_access final {
    [[nodiscard]] static int64_array_aggregate_state make_array_state(
            ArrayType* array) noexcept {
        return int64_array_aggregate_state{array};
    }

    [[nodiscard]] static int128_aggregate_state make_int128_state(
            Int128AggState* state) noexcept {
        return int128_aggregate_state{state};
    }
};

template<class T>
    requires std::same_as<T, int64_array_aggregate_state>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable aggregate array state"};
    }
    const Datum source = call->args[zero_based_index].value;
    const bool aggregate_call =
        AggCheckCallContext(call, nullptr) != 0;
    ArrayType* const array = invoke_pg_as_cpp(
        [source, aggregate_call] {
            return aggregate_call
                ? DatumGetArrayTypeP(source)
                : DatumGetArrayTypePCopy(source);
        });
    detail::validate_int64_array_state(array);
    return detail_aggregate_boundary_access::make_array_state(array);
}

template<class T>
    requires std::same_as<
        T, std::optional<int64_array_aggregate_state>>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return sql_argument<int64_array_aggregate_state>(
        call, zero_based_index);
}

template<class T>
    requires std::same_as<T, int128_aggregate_state>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable aggregate internal state"};
    }
    auto* const state = reinterpret_cast<Int128AggState*>(
        DatumGetPointer(call->args[zero_based_index].value));
    detail::validate_int128_state(state);
    return detail_aggregate_boundary_access::make_int128_state(state);
}

template<class T>
    requires std::same_as<
        T, std::optional<int128_aggregate_state>>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return sql_argument<int128_aggregate_state>(
        call, zero_based_index);
}

template<class T>
    requires std::same_as<
        std::remove_cvref_t<T>,
        int64_array_aggregate_state>
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        T&& value) noexcept {
    call->isnull = false;
    return PointerGetDatum(value.array());
}

template<class T>
    requires std::same_as<
        std::remove_cvref_t<T>,
        int128_aggregate_state>
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        T&& value) noexcept {
    call->isnull = false;
    return PointerGetDatum(value.get());
}

[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        const numeric_result& value) noexcept {
    call->isnull = false;
    return NumericGetDatum(value.get());
}

static_assert(!std::default_initializable<
              int64_array_aggregate_state>);
static_assert(!std::default_initializable<
              int128_aggregate_state>);
static_assert(!std::default_initializable<numeric_result>);
static_assert(std::copy_constructible<
              int64_array_aggregate_state>);
static_assert(std::copy_constructible<
              int128_aggregate_state>);
static_assert(std::copy_constructible<numeric_result>);

}  // namespace bioseq::pg_utils
