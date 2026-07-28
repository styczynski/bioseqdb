/*-------------------------------------------------------------------------
 *
 * pg_utils/sql_boundary/result_materialization.cppm
 *
 * Composite and materialized-set result encoding for generated SQL
 * boundaries.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/tuplestore.h"
}

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

export module bioseq.pg_utils.sql_boundary:result_materialization;

import bioseq.pg_utils.error_bridge;
import :conversions;
import :results;
import :values;

export namespace bioseq::pg_utils {

class sql_result_materialization_error final
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct encoded_sql_result_field final {
    Datum value;
    bool is_null;
};

namespace detail {

template<class T>
concept typed_sql_result_scalar =
    registered_sql_scalar<std::remove_cvref_t<T>>
    && !std::same_as<std::remove_cvref_t<T>, Datum>;

template<class T>
concept direct_sql_result_field =
    typed_sql_result_scalar<T>
    || std::same_as<std::remove_cvref_t<T>, std::string>
    || std::same_as<std::remove_cvref_t<T>, sql_jsonb>
    || std::same_as<std::remove_cvref_t<T>, sql_interval>;

template<class T>
struct optional_sql_result_field : std::false_type {};

template<class T>
struct optional_sql_result_field<std::optional<T>>
    : std::bool_constant<direct_sql_result_field<T>> {
    using value_type = T;
};

template<class T>
concept encodable_sql_result_field =
    direct_sql_result_field<T>
    || optional_sql_result_field<
           std::remove_cvref_t<T>>::value;

template<class>
inline constexpr bool unsupported_result_field_type = false;

struct sql_result_field_type_contract final {
    Oid primary;
    Oid alternative;
};

template<class T>
    requires encodable_sql_result_field<T>
[[nodiscard]] consteval sql_result_field_type_contract
sql_result_field_types() {
    using Value = std::remove_cvref_t<T>;
    if constexpr (optional_sql_result_field<Value>::value) {
        return sql_result_field_types<
            typename optional_sql_result_field<Value>::value_type>();
    } else if constexpr (std::same_as<Value, bool>) {
        return {BOOLOID, InvalidOid};
    } else if constexpr (std::same_as<Value, std::int16_t>) {
        return {INT2OID, InvalidOid};
    } else if constexpr (std::same_as<Value, std::int32_t>) {
        return {INT4OID, InvalidOid};
    } else if constexpr (std::same_as<Value, std::int64_t>) {
        return {INT8OID, InvalidOid};
    } else if constexpr (std::same_as<Value, float>) {
        return {FLOAT4OID, InvalidOid};
    } else if constexpr (std::same_as<Value, double>) {
        return {FLOAT8OID, InvalidOid};
    } else if constexpr (std::same_as<Value, Oid>) {
        return {OIDOID, REGCLASSOID};
    } else if constexpr (std::same_as<Value, std::string>) {
        return {TEXTOID, InvalidOid};
    } else if constexpr (std::same_as<Value, sql_jsonb>) {
        return {JSONBOID, InvalidOid};
    } else if constexpr (std::same_as<Value, sql_interval>) {
        return {INTERVALOID, InvalidOid};
    } else {
        static_assert(
            unsupported_result_field_type<Value>,
            "registered SQL result field is missing a PostgreSQL type OID");
        return {InvalidOid, InvalidOid};
    }
}

}  // namespace detail

/**
 * Encode one registered, by-value scalar as a result-row field.
 */
template<class T>
    requires detail::typed_sql_result_scalar<T>
[[nodiscard]] inline encoded_sql_result_field sql_result_field(
        T&& value) noexcept {
    using Value = std::remove_cvref_t<T>;
    return {
        .value =
            detail::sql_scalar_conversion<Value>::write(
                std::forward<T>(value)),
        .is_null = false,
    };
}

/**
 * Encode one owning text value as a result-row field.
 */
[[nodiscard]] inline encoded_sql_result_field sql_result_field(
        const std::string& value) {
    return {
        .value = detail::encode_sql_text(value),
        .is_null = false,
    };
}

/**
 * Encode one owning JSONB value as a result-row field.
 */
[[nodiscard]] inline encoded_sql_result_field sql_result_field(
        const sql_jsonb& value) {
    return {
        .value = detail::encode_sql_jsonb(value),
        .is_null = false,
    };
}

/**
 * Encode one INTERVAL value as a result-row field.
 */
[[nodiscard]] inline encoded_sql_result_field sql_result_field(
        const sql_interval& value) {
    return {
        .value = detail::encode_sql_interval(value),
        .is_null = false,
    };
}

/**
 * Encode one nullable scalar or text value without a sentinel.
 */
template<class T>
    requires detail::direct_sql_result_field<T>
[[nodiscard]] inline encoded_sql_result_field sql_result_field(
        const std::optional<T>& value) {
    if (!value.has_value()) {
        return {
            .value = static_cast<Datum>(0),
            .is_null = true,
        };
    }
    return sql_result_field(*value);
}

/**
 * Encoded values, null flags, and a compile-time PostgreSQL type contract for
 * one generated result row.
 */
template<class... FieldTypes>
    requires (
        sizeof...(FieldTypes) > 0
        && (detail::encodable_sql_result_field<FieldTypes> && ...))
class encoded_sql_result_row final {
public:
    static constexpr std::size_t fields_count =
        sizeof...(FieldTypes);

    explicit encoded_sql_result_row(
            std::array<
                encoded_sql_result_field,
                fields_count> fields) noexcept {
        for (std::size_t index = 0;
             index < fields_count;
             ++index) {
            values_[index] = fields[index].value;
            nulls_[index] = fields[index].is_null;
        }
    }

    [[nodiscard]] Datum* values_data() noexcept {
        return values_.data();
    }

    [[nodiscard]] const Datum* values_data() const noexcept {
        return values_.data();
    }

    [[nodiscard]] bool* nulls_data() noexcept {
        return nulls_.data();
    }

    [[nodiscard]] const bool* nulls_data() const noexcept {
        return nulls_.data();
    }

    [[nodiscard]] static constexpr const std::array<
        Oid,
        fields_count>& expected_type_oids() noexcept {
        return expected_type_oids_;
    }

    [[nodiscard]] static constexpr bool accepts_type_oid(
            std::size_t index,
            Oid actual) noexcept {
        if (index >= fields_count) {
            return false;
        }
        const detail::sql_result_field_type_contract contract =
            type_contracts_[index];
        return !OidIsValid(contract.primary)
            || actual == contract.primary
            || (OidIsValid(contract.alternative)
                && actual == contract.alternative);
    }

private:
    inline static constexpr std::array<
        detail::sql_result_field_type_contract,
        fields_count> type_contracts_{
            detail::sql_result_field_types<FieldTypes>()...};
    inline static constexpr std::array<Oid, fields_count>
        expected_type_oids_{
            (detail::sql_result_field_types<
                FieldTypes>().primary)...};
    std::array<Datum, fields_count> values_{};
    std::array<bool, fields_count> nulls_{};
};

/**
 * Encode one result row while retaining its field types for descriptor
 * validation.
 */
template<class... Fields>
    requires (
        sizeof...(Fields) > 0
        && (detail::encodable_sql_result_field<Fields> && ...))
[[nodiscard]] inline auto sql_result_row(Fields&&... fields) {
    using Encoded = encoded_sql_result_row<
        std::remove_cvref_t<Fields>...>;
    return Encoded{
        std::array<
            encoded_sql_result_field,
            sizeof...(Fields)>{
                sql_result_field(
                    std::forward<Fields>(fields))...}};
}

namespace detail {

template<class T>
struct is_encoded_sql_result_row : std::false_type {};

template<class... FieldTypes>
struct is_encoded_sql_result_row<
    encoded_sql_result_row<FieldTypes...>> : std::true_type {};

template<class T>
concept encoded_result_row =
    is_encoded_sql_result_row<
        std::remove_cvref_t<T>>::value;

template<class Encoder, class Encoded, class = void>
struct has_exact_result_column_names : std::false_type {};

template<class Encoder, class Encoded>
    requires encoded_result_row<Encoded>
struct has_exact_result_column_names<
    Encoder,
    Encoded,
    std::void_t<
        decltype(
            std::remove_cvref_t<Encoder>::column_names)>>
    : std::bool_constant<
          std::same_as<
              std::remove_cvref_t<
                  decltype(
                      std::remove_cvref_t<
                          Encoder>::column_names)>,
              std::array<
                  std::string_view,
                  Encoded::fields_count>>> {};

template<class Encoder, class Row, class = void>
struct is_sql_result_row_encoder : std::false_type {};

template<class Encoder, class Row>
struct is_sql_result_row_encoder<
    Encoder,
    Row,
    std::void_t<
        std::invoke_result_t<Encoder&, const Row&>>>
    : has_exact_result_column_names<
          Encoder,
          std::remove_cvref_t<
              std::invoke_result_t<Encoder&, const Row&>>> {};

template<class Encoder, class Row>
concept sql_result_row_encoder =
    is_sql_result_row_encoder<
        std::remove_reference_t<Encoder>,
        Row>::value;

template<class Encoder, class Row>
using encoded_result_row_t = std::remove_cvref_t<
    std::invoke_result_t<
        std::remove_reference_t<Encoder>&,
        const Row&>>;

inline void require_result_call(FunctionCallInfo call) {
    if (call == nullptr || call->flinfo == nullptr) {
        throw sql_result_materialization_error{
            "generated SQL result materialization received an incomplete "
            "call frame"};
    }
}

template<encoded_result_row Encoded, class Encoder>
    requires has_exact_result_column_names<
        Encoder,
        Encoded>::value
void validate_result_descriptor(
        TupleDesc tuple_descriptor,
        std::string_view result_kind) {
    constexpr std::size_t expected_fields =
        Encoded::fields_count;
    if (tuple_descriptor == nullptr
        || tuple_descriptor->natts
            != static_cast<int>(expected_fields)) {
        throw sql_result_materialization_error{
            std::string{result_kind}
            + " result descriptor has the wrong arity"};
    }

    for (std::size_t index = 0;
         index < expected_fields;
         ++index) {
        const Form_pg_attribute attribute =
            TupleDescAttr(
                tuple_descriptor,
                static_cast<int>(index));
        if (attribute->attnum
                != static_cast<AttrNumber>(index + 1)
            || attribute->attisdropped) {
            throw sql_result_materialization_error{
                std::string{result_kind}
                + " result descriptor has an unusable attribute "
                + std::to_string(index + 1)};
        }
        const std::string_view expected_name =
            std::remove_cvref_t<Encoder>::column_names[index];
        if (std::string_view{NameStr(attribute->attname)}
            != expected_name) {
            throw sql_result_materialization_error{
                std::string{result_kind}
                + " result descriptor name mismatch at attribute "
                + std::to_string(index + 1)};
        }
        if (!Encoded::accepts_type_oid(
                index,
                attribute->atttypid)) {
            throw sql_result_materialization_error{
                std::string{result_kind}
                + " result descriptor type mismatch at attribute "
                + std::to_string(index + 1)};
        }
    }
}

[[nodiscard]] inline TupleDesc composite_result_descriptor(
        FunctionCallInfo call) {
    require_result_call(call);
    TupleDesc tuple_descriptor = nullptr;
    const TypeFuncClass result_class = invoke_pg_as_cpp(
        [call, &tuple_descriptor] {
            return get_call_result_type(
                call,
                nullptr,
                &tuple_descriptor);
        });
    if (result_class != TYPEFUNC_COMPOSITE
        || tuple_descriptor == nullptr) {
        throw sql_result_materialization_error{
            "named OUT result requires a composite result descriptor"};
    }
    return invoke_pg_as_cpp(
        [tuple_descriptor] {
            return BlessTupleDesc(tuple_descriptor);
        });
}

template<encoded_result_row Encoded>
struct materialized_result_context final {
    Tuplestorestate* tuplestore;
    TupleDesc tuple_descriptor;
};

template<encoded_result_row Encoded, class Encoder>
    requires has_exact_result_column_names<
        Encoder,
        Encoded>::value
[[nodiscard]] materialized_result_context<Encoded>
initialize_materialized_result(FunctionCallInfo call) {
    require_result_call(call);
    if (call->resultinfo == nullptr
        || !IsA(call->resultinfo, ReturnSetInfo)) {
        throw sql_result_materialization_error{
            "row-set result requires a set-returning call context"};
    }

    auto* const result_info =
        reinterpret_cast<ReturnSetInfo*>(call->resultinfo);
    if ((result_info->allowedModes & SFRM_Materialize) == 0) {
        throw sql_result_materialization_error{
            "row-set call context does not allow materialization"};
    }
    if (result_info->econtext == nullptr
        || result_info->econtext->ecxt_per_query_memory == nullptr) {
        throw sql_result_materialization_error{
            "row-set call context has no per-query memory context"};
    }

    invoke_pg_as_cpp(
        [call] {
            InitMaterializedSRF(call, MAT_SRF_BLESS);
        });
    if (result_info->setResult == nullptr
        || result_info->setDesc == nullptr) {
        throw sql_result_materialization_error{
            "PostgreSQL did not initialize the materialized row set"};
    }
    validate_result_descriptor<Encoded, Encoder>(
        result_info->setDesc,
        "row-set");
    return {
        .tuplestore = result_info->setResult,
        .tuple_descriptor = result_info->setDesc,
    };
}

template<
    class Row,
    sql_result_kind Kind,
    class Encoder>
    requires sql_result_row_encoder<Encoder, Row>
[[nodiscard]] Datum materialize_row_set(
        FunctionCallInfo call,
        owning_sql_rows<Row, Kind>& rows,
        Encoder& encoder) {
    using Encoded = encoded_result_row_t<Encoder, Row>;
    const materialized_result_context<Encoded> context =
        initialize_materialized_result<Encoded, Encoder>(call);

    for (const Row& row : rows) {
        Encoded encoded = std::invoke(encoder, row);
        invoke_pg_as_cpp(
            [tuplestore = context.tuplestore,
             tuple_descriptor = context.tuple_descriptor,
             &encoded] {
                tuplestore_putvalues(
                    tuplestore,
                    tuple_descriptor,
                    encoded.values_data(),
                    encoded.nulls_data());
            });
    }
    call->isnull = false;
    return static_cast<Datum>(0);
}

}  // namespace detail

/**
 * Form one composite Datum for a routine with named OUT parameters.
 */
template<class Row, class Encoder>
    requires detail::sql_result_row_encoder<Encoder, Row>
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        named_out<Row>&& result,
        Encoder&& encoder) {
    using Encoded =
        detail::encoded_result_row_t<Encoder, Row>;
    const TupleDesc tuple_descriptor =
        detail::composite_result_descriptor(call);
    detail::validate_result_descriptor<Encoded, Encoder>(
        tuple_descriptor,
        "named OUT");

    Encoded encoded = std::invoke(
        encoder,
        std::as_const(result).value());
    const Datum tuple = invoke_pg_as_cpp(
        [tuple_descriptor, &encoded] {
            HeapTuple formed = heap_form_tuple(
                tuple_descriptor,
                encoded.values_data(),
                encoded.nulls_data());
            return HeapTupleGetDatum(formed);
        });
    call->isnull = false;
    return tuple;
}

/**
 * Materialize all rows returned by a RETURNS TABLE routine.
 */
template<class Row, class Encoder>
    requires detail::sql_result_row_encoder<Encoder, Row>
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        returns_table<Row>&& result,
        Encoder&& encoder) {
    auto& encoder_reference = encoder;
    return detail::materialize_row_set(
        call,
        result,
        encoder_reference);
}

/**
 * Materialize all rows returned by a SETOF routine with named outputs.
 */
template<class Row, class Encoder>
    requires detail::sql_result_row_encoder<Encoder, Row>
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        setof_named_out<Row>&& result,
        Encoder&& encoder) {
    auto& encoder_reference = encoder;
    return detail::materialize_row_set(
        call,
        result,
        encoder_reference);
}

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils::detail {

using scalar_result_probe =
    encoded_sql_result_row<std::int64_t, bool>;
using nullable_result_probe =
    encoded_sql_result_row<
        std::optional<std::string>,
        std::optional<std::int64_t>>;
using oid_result_probe = encoded_sql_result_row<Oid>;
using exact_boundary_result_probe =
    encoded_sql_result_row<
        sql_jsonb,
        std::optional<sql_interval>>;

static_assert(scalar_result_probe::fields_count == 2);
static_assert(
    scalar_result_probe::expected_type_oids()[0] == INT8OID);
static_assert(
    scalar_result_probe::expected_type_oids()[1] == BOOLOID);
static_assert(
    nullable_result_probe::expected_type_oids()[0] == TEXTOID);
static_assert(
    nullable_result_probe::expected_type_oids()[1] == INT8OID);
static_assert(
    oid_result_probe::accepts_type_oid(0, OIDOID));
static_assert(
    oid_result_probe::accepts_type_oid(0, REGCLASSOID));
static_assert(
    exact_boundary_result_probe::expected_type_oids()[0]
        == JSONBOID);
static_assert(
    exact_boundary_result_probe::expected_type_oids()[1]
        == INTERVALOID);
static_assert(
    !oid_result_probe::accepts_type_oid(0, TEXTOID));
static_assert(!encodable_sql_result_field<Datum>);
static_assert(encoded_result_row<scalar_result_probe>);
static_assert(std::is_trivially_destructible_v<
              encoded_sql_result_field>);
static_assert(noexcept(
    std::declval<scalar_result_probe&>().values_data()));
static_assert(noexcept(
    std::declval<const scalar_result_probe&>().nulls_data()));

}  // namespace bioseq::pg_utils::detail
