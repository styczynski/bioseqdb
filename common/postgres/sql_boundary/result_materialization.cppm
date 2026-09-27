/**
 * @file common/postgres/sql_boundary/result_materialization.cppm
 * @brief Convert C++ rows into SQL composite values or materialized result sets.
 */
module;

extern "C" {
#include "postgres.h"
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

export module bioseq.postgres.sql_boundary:result_materialization;

import bioseq.postgres.support;

import :error_boundary;
import :owning_values;
import :scalar_conversion;
import :shape;

export namespace bioseq::postgres::support {

/** The SQL result cannot be built from the supplied call context or row description. */
class result_materialization_error final
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** One encoded field and its PostgreSQL null flag. */
struct encoded_result_field final {
    Datum value{};  ///< Encoded PostgreSQL field value.
    bool is_null{};  ///< Whether PostgreSQL must ignore `value` as NULL.
};

}  // namespace bioseq::postgres::support

namespace bioseq::postgres::support::detail {

/** A type with a scalar_codec conversion, excluding an untyped Datum. */
template<class p_value_type>
concept typed_result_scalar =
    supported_scalar<std::remove_cvref_t<p_value_type>>
    && !std::same_as<std::remove_cvref_t<p_value_type>, Datum>;

/** A C++ type accepted by the non-null field-encoding overloads. */
template<class p_value_type>
concept direct_result_field =
    typed_result_scalar<p_value_type>
    || std::same_as<
        std::remove_cvref_t<p_value_type>,
        std::string>
    || std::same_as<
        std::remove_cvref_t<p_value_type>,
        jsonb_value>
    || std::same_as<
        std::remove_cvref_t<p_value_type>,
        bytea_value>
    || std::same_as<
        std::remove_cvref_t<p_value_type>,
        bigint_array>
    || std::same_as<
        std::remove_cvref_t<p_value_type>,
        interval_value>;

/** Trait identifying an optional value accepted as a result field. */
template<class p_candidate_type>
struct optional_result_field : std::false_type {};

template<class p_element_type>
struct optional_result_field<std::optional<p_element_type>>
    : std::bool_constant<direct_result_field<p_element_type>> {
    using value_type = p_element_type;
};

/** A supported field value, optionally wrapped in std::optional for SQL NULL. */
template<class p_value_type>
concept encodable_result_field =
    direct_result_field<p_value_type>
    || optional_result_field<
           std::remove_cvref_t<p_value_type>>::value;

template<class>
inline constexpr bool unsupported_result_field_type = false;

/** The SQL type for a C++ field, with an optional second accepted type. */
struct result_field_type_contract final {
    Oid primary{};
    Oid alternative{};
};

/** Choose the accepted SQL type OIDs for a C++ field at compile time. */
template<class p_field_type>
    requires encodable_result_field<p_field_type>
[[nodiscard]] consteval result_field_type_contract
result_field_types() {
    using value_type = std::remove_cvref_t<p_field_type>;
    if constexpr (optional_result_field<value_type>::value) {
        return result_field_types<
            typename optional_result_field<value_type>::value_type>();
    } else if constexpr (std::same_as<value_type, bool>) {
        return {BOOLOID, InvalidOid};
    } else if constexpr (std::same_as<value_type, std::int16_t>) {
        return {INT2OID, InvalidOid};
    } else if constexpr (std::same_as<value_type, std::int32_t>) {
        return {INT4OID, InvalidOid};
    } else if constexpr (std::same_as<value_type, std::int64_t>) {
        return {INT8OID, InvalidOid};
    } else if constexpr (std::same_as<value_type, float>) {
        return {FLOAT4OID, InvalidOid};
    } else if constexpr (std::same_as<value_type, double>) {
        return {FLOAT8OID, InvalidOid};
    } else if constexpr (std::same_as<value_type, Oid>) {
        return {OIDOID, REGCLASSOID};
    } else if constexpr (
            std::same_as<value_type, timestamp_with_time_zone>) {
        return {TIMESTAMPTZOID, InvalidOid};
    } else if constexpr (std::same_as<value_type, std::string>) {
        return {TEXTOID, InvalidOid};
    } else if constexpr (std::same_as<value_type, jsonb_value>) {
        return {JSONBOID, InvalidOid};
    } else if constexpr (std::same_as<value_type, bytea_value>) {
        return {BYTEAOID, InvalidOid};
    } else if constexpr (
            std::same_as<value_type, bigint_array>) {
        return {INT8ARRAYOID, InvalidOid};
    } else if constexpr (std::same_as<value_type, interval_value>) {
        return {INTERVALOID, InvalidOid};
    } else {
        static_assert(
            unsupported_result_field_type<value_type>,
            "supported result field is missing a PostgreSQL type OID");
        return {InvalidOid, InvalidOid};
    }
}

}  // namespace bioseq::postgres::support::detail

export namespace bioseq::postgres::support {

/** Encode a scalar field and mark it non-null. */
template<class p_scalar_type>
    requires detail::typed_result_scalar<p_scalar_type>
[[nodiscard]] inline encoded_result_field encode_result_field(
        p_scalar_type&& value) noexcept(noexcept(
            detail::scalar_codec<
                std::remove_cvref_t<p_scalar_type>>::write(
                    std::forward<p_scalar_type>(value)))) {
    using value_type = std::remove_cvref_t<p_scalar_type>;
    return {
        .value =
            detail::scalar_codec<value_type>::write(
                std::forward<p_scalar_type>(value)),
        .is_null = false,
    };
}

/** Copy a C++ string into a SQL text field. */
[[nodiscard]] inline encoded_result_field encode_result_field(
        const std::string& value) {
    return {
        .value = detail::encode_text(value),
        .is_null = false,
    };
}

/** Parse owned JSON text into a SQL JSONB field. */
[[nodiscard]] inline encoded_result_field encode_result_field(
        const jsonb_value& value) {
    return {
        .value = detail::encode_jsonb(value),
        .is_null = false,
    };
}

/** Copy owned bytes into a SQL BYTEA field. */
[[nodiscard]] inline encoded_result_field encode_result_field(
        const bytea_value& value) {
    return {
        .value = detail::encode_bytea(value),
        .is_null = false,
    };
}

/** Copy integer elements into a SQL bigint[] field. */
[[nodiscard]] inline encoded_result_field encode_result_field(
        const bigint_array& value) {
    return {
        .value = detail::encode_bigint_array(value),
        .is_null = false,
    };
}

/** Copy interval fields into a SQL INTERVAL value. */
[[nodiscard]] inline encoded_result_field encode_result_field(
        const interval_value& value) {
    return {
        .value = detail::encode_interval(value),
        .is_null = false,
    };
}

/** Encode an optional field, setting its SQL null flag when empty. */
template<class p_value_type>
    requires detail::direct_result_field<p_value_type>
[[nodiscard]] inline encoded_result_field encode_result_field(
        const std::optional<p_value_type>& value) {
    if (!value.has_value()) {
        return {
            .value = static_cast<Datum>(0),
            .is_null = true,
        };
    }
    return encode_result_field(*value);
}

/**
 * A row's Datums and null flags, with SQL types for checking its descriptor.
 *
 * @tparam p_field_types C++ field types in SQL column order.
 */
template<class... p_field_types>
    requires (
        sizeof...(p_field_types) > 0
        && (detail::encodable_result_field<p_field_types> && ...))
class encoded_result_row final {
public:
    /** Number of fields in this row. */
    static constexpr std::size_t field_count =
        sizeof...(p_field_types);

    /** Split encoded fields into PostgreSQL value and null arrays. */
    explicit encoded_result_row(
            std::array<
                encoded_result_field,
                field_count> fields) noexcept {
        for (std::size_t index = 0;
             index < field_count;
             ++index) {
            values_[index] = fields[index].value;
            nulls_[index] = fields[index].is_null;
        }
    }

    /** Return mutable encoded values for PostgreSQL tuple formation. */
    [[nodiscard]] Datum* values_data() noexcept {
        return values_.data();
    }

    /** Return read-only encoded values. */
    [[nodiscard]] const Datum* values_data() const noexcept {
        return values_.data();
    }

    /** Return mutable PostgreSQL null flags. */
    [[nodiscard]] bool* nulls_data() noexcept {
        return nulls_.data();
    }

    /** Return read-only PostgreSQL null flags. */
    [[nodiscard]] const bool* nulls_data() const noexcept {
        return nulls_.data();
    }

    /** Return the primary PostgreSQL type OID for every field. */
    [[nodiscard]] static constexpr const std::array<
        Oid,
        field_count>& expected_type_oids() noexcept {
        return expected_type_oids_;
    }

    /** Check whether this SQL type is accepted at the given field position. */
    [[nodiscard]] static constexpr bool accepts_type_oid(
            std::size_t index,
            Oid actual) noexcept {
        if (index >= field_count) {
            return false;
        }
        const detail::result_field_type_contract contract =
            type_contracts_[index];
        return !OidIsValid(contract.primary)
            || actual == contract.primary
            || (OidIsValid(contract.alternative)
                && actual == contract.alternative);
    }

private:
    inline static constexpr std::array<
        detail::result_field_type_contract,
        field_count> type_contracts_{
            detail::result_field_types<p_field_types>()...};
    inline static constexpr std::array<Oid, field_count>
        expected_type_oids_{
            (detail::result_field_types<
                p_field_types>().primary)...};
    std::array<Datum, field_count> values_{};
    std::array<bool, field_count> nulls_{};
};

/**
 * Encode the fields of one row and keep their C++ types so the SQL result
 * descriptor can be checked before building tuples.
 */
template<class... p_field_types>
    requires (
        sizeof...(p_field_types) > 0
        && (detail::encodable_result_field<p_field_types> && ...))
[[nodiscard]] inline auto encode_result_row(
        p_field_types&&... fields) {
    using encoded_type = encoded_result_row<
        std::remove_cvref_t<p_field_types>...>;
    return encoded_type{
        std::array<
            encoded_result_field,
            sizeof...(p_field_types)>{
                encode_result_field(
                    std::forward<p_field_types>(fields))...}};
}

}  // namespace bioseq::postgres::support

namespace bioseq::postgres::support::detail {

/** Trait identifying an encoded_result_row specialization. */
template<class p_candidate_type>
struct is_encoded_result_row : std::false_type {};

template<class... p_field_types>
struct is_encoded_result_row<
    encoded_result_row<p_field_types...>> : std::true_type {};

/** A specialization of encoded_result_row, ignoring references and const. */
template<class p_candidate_type>
concept encoded_result_row =
    is_encoded_result_row<
        std::remove_cvref_t<p_candidate_type>>::value;

/** Check that column_names is a string_view array with one entry per field. */
template<
    class p_encoder_type,
    class p_encoded_row_type,
    class = void>
struct has_exact_result_column_names : std::false_type {};

template<class p_encoder_type, class p_encoded_row_type>
    requires encoded_result_row<p_encoded_row_type>
struct has_exact_result_column_names<
    p_encoder_type,
    p_encoded_row_type,
    std::void_t<
        decltype(
            std::remove_cvref_t<p_encoder_type>::column_names)>>
    : std::bool_constant<
          std::same_as<
              std::remove_cvref_t<
                  decltype(
                      std::remove_cvref_t<
                          p_encoder_type>::column_names)>,
              std::array<
                  std::string_view,
                  p_encoded_row_type::field_count>>> {};

/** Check that a callable returns an encoded row and declares column names. */
template<class p_encoder_type, class p_row_type, class = void>
struct is_result_row_encoder : std::false_type {};

/** Check the encoder's returned row type and column-name array size. */
template<class p_encoder_type, class p_row_type>
struct is_result_row_encoder<
    p_encoder_type,
    p_row_type,
    std::void_t<
        std::invoke_result_t<p_encoder_type&, const p_row_type&>>>
    : has_exact_result_column_names<
          p_encoder_type,
          std::remove_cvref_t<
              std::invoke_result_t<
                  p_encoder_type&,
                  const p_row_type&>>> {};

/** A row encoder with a column-name array matching its encoded field count. */
template<class p_encoder_type, class p_row_type>
concept result_row_encoder =
    is_result_row_encoder<
        std::remove_reference_t<p_encoder_type>,
        p_row_type>::value;

/** Encoded row type produced by an encoder. */
template<class p_encoder_type, class p_row_type>
using encoded_result_row_t = std::remove_cvref_t<
    std::invoke_result_t<
        std::remove_reference_t<p_encoder_type>&,
        const p_row_type&>>;

/** Require a call frame with function metadata before constructing SQL results. */
inline void require_result_call(FunctionCallInfo call) {
    if (call == nullptr || call->flinfo == nullptr) {
        throw result_materialization_error{
            "generated SQL result materialization received an incomplete "
            "call frame"};
    }
}

/** Check column count, order, names and SQL types, and reject dropped columns. */
template<
    encoded_result_row p_encoded_row_type,
    class p_encoder_type>
    requires has_exact_result_column_names<
        p_encoder_type,
        p_encoded_row_type>::value
void validate_result_descriptor(
        TupleDesc tuple_descriptor,
        std::string_view result_shape_name) {
    constexpr std::size_t expected_fields =
        p_encoded_row_type::field_count;
    if (tuple_descriptor == nullptr
        || tuple_descriptor->natts
            != static_cast<int>(expected_fields)) {
        throw result_materialization_error{
            std::string{result_shape_name}
            + " result descriptor has the wrong arity"};
    }

    for (std::size_t index = 0;
         index < expected_fields;
         ++index) {
        const Form_pg_attribute attribute =
            bioseq::postgres::support::tuple_descriptor_attribute(
                tuple_descriptor,
                static_cast<int>(index));
        if (attribute->attnum
                != static_cast<AttrNumber>(index + 1)
            || attribute->attisdropped) {
            throw result_materialization_error{
                std::string{result_shape_name}
                + " result descriptor has an unusable attribute "
                + std::to_string(index + 1)};
        }
        const std::string_view expected_name =
            std::remove_cvref_t<
                p_encoder_type>::column_names[index];
        if (std::string_view{NameStr(attribute->attname)}
            != expected_name) {
            throw result_materialization_error{
                std::string{result_shape_name}
                + " result descriptor name mismatch at attribute "
                + std::to_string(index + 1)};
        }
        if (!p_encoded_row_type::accepts_type_oid(
                index,
                attribute->atttypid)) {
            throw result_materialization_error{
                std::string{result_shape_name}
                + " result descriptor type mismatch at attribute "
                + std::to_string(index + 1)};
        }
    }
}

/** Get the composite descriptor for named OUT parameters and register its row type. */
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
        throw result_materialization_error{
            "named OUT result requires a composite result descriptor"};
    }
    return invoke_pg_as_cpp(
        [tuple_descriptor] {
            return BlessTupleDesc(tuple_descriptor);
        });
}

/** The PostgreSQL row store and tuple descriptor for a result set. */
struct result_materialization_context final {
    Tuplestorestate* tuplestore{};
    TupleDesc tuple_descriptor{};
};

/**
 * Prepare a PostgreSQL row store for this set-returning call.
 * Require materialization support and a query memory context, then check the
 * result descriptor against the encoder's columns.
 */
template<
    encoded_result_row p_encoded_row_type,
    class p_encoder_type>
    requires has_exact_result_column_names<
        p_encoder_type,
        p_encoded_row_type>::value
[[nodiscard]] result_materialization_context
initialize_result_materialization(FunctionCallInfo call) {
    require_result_call(call);
    if (call->resultinfo == nullptr
        || !IsA(call->resultinfo, ReturnSetInfo)) {
        throw result_materialization_error{
            "row-set result requires a set-returning call context"};
    }

    auto* const result_info =
        reinterpret_cast<ReturnSetInfo*>(call->resultinfo);
    if ((result_info->allowedModes & SFRM_Materialize) == 0) {
        throw result_materialization_error{
            "row-set call context does not allow materialization"};
    }
    if (result_info->econtext == nullptr
        || result_info->econtext->ecxt_per_query_memory == nullptr) {
        throw result_materialization_error{
            "row-set call context has no per-query memory context"};
    }

    invoke_pg_as_cpp(
        [call] {
            InitMaterializedSRF(call, MAT_SRF_BLESS);
        });
    if (result_info->setResult == nullptr
        || result_info->setDesc == nullptr) {
        throw result_materialization_error{
            "PostgreSQL did not initialize the materialized row set"};
    }
    validate_result_descriptor<p_encoded_row_type, p_encoder_type>(
        result_info->setDesc,
        "row-set");
    return {
        .tuplestore = result_info->setResult,
        .tuple_descriptor = result_info->setDesc,
    };
}

/** Encode every C++ row and append it to PostgreSQL's result store. */
template<
    class p_row_type,
    result_shape p_shape,
    class p_encoder_type>
    requires result_row_encoder<p_encoder_type, p_row_type>
[[nodiscard]] Datum materialize_row_set(
        FunctionCallInfo call,
        result_rows<p_row_type, p_shape>& rows,
        p_encoder_type& encoder) {
    using encoded_type =
        encoded_result_row_t<p_encoder_type, p_row_type>;
    const result_materialization_context context =
        initialize_result_materialization<
            encoded_type,
            p_encoder_type>(call);

    for (const p_row_type& row : rows) {
        encoded_type encoded = std::invoke(encoder, row);
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

}  // namespace bioseq::postgres::support::detail

export namespace bioseq::postgres::support {

/** Build one SQL composite value from a routine's named OUT fields. */
template<class p_row_type, class p_encoder_type>
    requires detail::result_row_encoder<p_encoder_type, p_row_type>
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        named_output<p_row_type>&& result,
        p_encoder_type&& encoder) {
    using encoded_type =
        detail::encoded_result_row_t<p_encoder_type, p_row_type>;
    const TupleDesc tuple_descriptor =
        detail::composite_result_descriptor(call);
    detail::validate_result_descriptor<encoded_type, p_encoder_type>(
        tuple_descriptor,
        "named OUT");

    encoded_type encoded = std::invoke(
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

/** Store all rows from a RETURNS TABLE routine in PostgreSQL's result store. */
template<class p_row_type, class p_encoder_type>
    requires detail::result_row_encoder<p_encoder_type, p_row_type>
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        table_rows<p_row_type>&& result,
        p_encoder_type&& encoder) {
    auto& encoder_reference = encoder;
    return detail::materialize_row_set(
        call,
        result,
        encoder_reference);
}

/** Store all rows from a SETOF routine with named output columns. */
template<class p_row_type, class p_encoder_type>
    requires detail::result_row_encoder<p_encoder_type, p_row_type>
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        named_output_set<p_row_type>&& result,
        p_encoder_type&& encoder) {
    auto& encoder_reference = encoder;
    return detail::materialize_row_set(
        call,
        result,
        encoder_reference);
}

}  // namespace bioseq::postgres::support
