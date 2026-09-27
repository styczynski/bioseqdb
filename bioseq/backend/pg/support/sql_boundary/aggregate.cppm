/**
 * @file aggregate.cppm
 * @brief Convert SQL aggregate states and Numeric results to C++ wrappers.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/pg_type_d.h"
}

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>

export module bioseq.postgres.support.sql_boundary:aggregate;

import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.vectorization.types;

export namespace bioseq::postgres::support {

/**
 * Read or update the two-bigint state used by smallint and integer AVG.
 * PostgreSQL owns the array. When called outside aggregate evaluation,
 * `read_argument` copies it so a state update leaves the input value intact.
 */
class int64_array_aggregate_state final {
public:
    [[nodiscard]] ::int64_aggregate_state_storage& value() const noexcept {
        return *reinterpret_cast<::int64_aggregate_state_storage*>(
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

/** Borrow PostgreSQL's bigint aggregate state. Argument decoding checks its count and mode. */
class int128_aggregate_state final {
public:
    [[nodiscard]] ::int128_aggregate_state_storage& value() const noexcept {
        return *state_;
    }

    [[nodiscard]] ::int128_aggregate_state_storage* get() const noexcept {
        return state_;
    }

    [[nodiscard]] static int128_aggregate_state from_new(
            ::int128_aggregate_state_storage* state) {
        if (state == nullptr) {
            throw scalar_conversion_error{
                "cannot construct an aggregate state from a null pointer"};
        }
        return int128_aggregate_state{state};
    }

private:
    friend struct detail_aggregate_boundary_access;

    explicit int128_aggregate_state(
            ::int128_aggregate_state_storage* state) noexcept
        : state_{state} {}

    ::int128_aggregate_state_storage* state_ = nullptr;
};

/** Non-null Numeric result whose memory belongs to PostgreSQL. */
class numeric_result final {
public:
    [[nodiscard]] static numeric_result from_numeric(Numeric value) {
        if (value == nullptr) {
            throw scalar_conversion_error{
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
        throw scalar_conversion_error{
            "aggregate array state contains a null pointer"};
    }
    if (ARR_NDIM(array) != 1
        || ARR_DIMS(array)[0] != 2
        || ARR_LBOUND(array)[0] != 1
        || ARR_ELEMTYPE(array) != INT8OID
        || ARR_HASNULL(array)
        || ARR_SIZE(array)
            != ARR_OVERHEAD_NONULLS(1)
                + sizeof(::int64_aggregate_state_storage)) {
        throw scalar_conversion_error{
            "expected a one-dimensional, two-element bigint aggregate "
            "state array without NULLs"};
    }
    const auto* const state =
        reinterpret_cast<const ::int64_aggregate_state_storage*>(
            ARR_DATA_PTR(array));
    if (state->count < 0) {
        throw scalar_conversion_error{
            "aggregate array state contains a negative row count"};
    }
}

inline void validate_int128_state(
        ::int128_aggregate_state_storage* state) {
    if (state == nullptr) {
        throw scalar_conversion_error{
            "aggregate internal state contains a null pointer"};
    }
    if (state->count < 0 || state->calculate_sum_of_squares) {
        throw scalar_conversion_error{
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
            ::int128_aggregate_state_storage* state) noexcept {
        return int128_aggregate_state{state};
    }
};

template<class p_value>
    requires std::same_as<p_value, int64_array_aggregate_state>
[[nodiscard]] inline p_value read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw scalar_conversion_error{
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

template<class p_value>
    requires std::same_as<
        p_value, std::optional<int64_array_aggregate_state>>
[[nodiscard]] inline p_value read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return read_argument<int64_array_aggregate_state>(
        call, zero_based_index);
}

template<class p_value>
    requires std::same_as<p_value, int128_aggregate_state>
[[nodiscard]] inline p_value read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a non-nullable aggregate internal state"};
    }
    auto* const state =
        reinterpret_cast<::int128_aggregate_state_storage*>(
            DatumGetPointer(call->args[zero_based_index].value));
    detail::validate_int128_state(state);
    return detail_aggregate_boundary_access::make_int128_state(state);
}

template<class p_value>
    requires std::same_as<
        p_value, std::optional<int128_aggregate_state>>
[[nodiscard]] inline p_value read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return read_argument<int128_aggregate_state>(
        call, zero_based_index);
}

template<class p_value>
    requires std::same_as<
        std::remove_cvref_t<p_value>,
        int64_array_aggregate_state>
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        p_value&& value) noexcept {
    call->isnull = false;
    return PointerGetDatum(value.array());
}

template<class p_value>
    requires std::same_as<
        std::remove_cvref_t<p_value>,
        int128_aggregate_state>
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        p_value&& value) noexcept {
    call->isnull = false;
    return PointerGetDatum(value.get());
}

[[nodiscard]] inline Datum encode_result(
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

}  // namespace bioseq::postgres::support
