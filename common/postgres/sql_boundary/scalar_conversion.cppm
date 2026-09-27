/**
 * @file common/postgres/sql_boundary/scalar_conversion.cppm
 * @brief Convert SQL arguments and results between PostgreSQL Datums and C++ values.
 */
module;

extern "C" {
#include "postgres.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "lib/stringinfo.h"
#include "storage/itemptr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
}

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.sql_boundary:scalar_conversion;

import :call_context;
import :error_boundary;
import :owning_values;

export namespace bioseq::postgres::support {

/** A SQL argument, or result cannot be converted to the requested C++ type. */
class scalar_conversion_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace bioseq::postgres::support

export namespace bioseq::postgres::support::detail {

/** Largest payload this converter  can allocate after the varlena header. */
inline constexpr std::size_t maximum_varlena_payload_size =
    static_cast<std::size_t>(MaxAllocSize - VARHDRSZ)
        < static_cast<std::size_t>(
              std::numeric_limits<int>::max() - VARHDRSZ)
    ? static_cast<std::size_t>(MaxAllocSize - VARHDRSZ)
    : static_cast<std::size_t>(
          std::numeric_limits<int>::max() - VARHDRSZ);

/** Reject a missing call frame or an argument index outside its argument list. */
inline void require_argument(
        FunctionCallInfo call,
        int argument_index) {
    if (call == nullptr) {
        throw scalar_conversion_error{
            "generated SQL boundary received a null call frame"};
    }
    if (argument_index < 0
        || argument_index >= static_cast<int>(call->nargs)) {
        throw scalar_conversion_error{
            "generated SQL boundary argument index is out of range"};
    }
}

/** Conversions to and from Datum for each supported C++ scalar type. */
template<class p_scalar_type>
struct scalar_codec;

/** Codec for PostgreSQL boolean Datums. */
template<>
struct scalar_codec<bool> final {
    [[nodiscard]] static bool read(Datum value) noexcept {
        return DatumGetBool(value);
    }

    [[nodiscard]] static Datum write(bool value) noexcept {
        return BoolGetDatum(value);
    }
};

/** Codec for PostgreSQL int2 Datums. */
template<>
struct scalar_codec<std::int16_t> final {
    [[nodiscard]] static std::int16_t read(Datum value) noexcept {
        return DatumGetInt16(value);
    }

    [[nodiscard]] static Datum write(std::int16_t value) noexcept {
        return Int16GetDatum(value);
    }
};

/** Codec for PostgreSQL int4 Datums. */
template<>
struct scalar_codec<std::int32_t> final {
    [[nodiscard]] static std::int32_t read(Datum value) noexcept {
        return DatumGetInt32(value);
    }

    [[nodiscard]] static Datum write(std::int32_t value) noexcept {
        return Int32GetDatum(value);
    }
};

/** Codec for PostgreSQL int8 Datums. */
template<>
struct scalar_codec<std::int64_t> final {
    [[nodiscard]] static std::int64_t read(Datum value) noexcept {
        return DatumGetInt64(value);
    }

    [[nodiscard]] static Datum write(std::int64_t value) noexcept {
        return Int64GetDatum(value);
    }
};

/** Codec for PostgreSQL float4 Datums. */
template<>
struct scalar_codec<float> final {
    [[nodiscard]] static float read(Datum value) noexcept {
        return DatumGetFloat4(value);
    }

    [[nodiscard]] static Datum write(float value) noexcept {
        return Float4GetDatum(value);
    }
};

/** Codec for PostgreSQL float8 Datums. */
template<>
struct scalar_codec<double> final {
    [[nodiscard]] static double read(Datum value) noexcept {
        return DatumGetFloat8(value);
    }

    [[nodiscard]] static Datum write(double value) noexcept {
        return Float8GetDatum(value);
    }
};

/** Codec for PostgreSQL object-identifier Datums. */
template<>
struct scalar_codec<Oid> final {
    [[nodiscard]] static Oid read(Datum value) noexcept {
        return DatumGetObjectId(value);
    }

    [[nodiscard]] static Datum write(Oid value) noexcept {
        return ObjectIdGetDatum(value);
    }
};

/** Convert timestamptz values while preserving positive and negative infinity. */
template<>
struct scalar_codec<timestamp_with_time_zone> final {
    [[nodiscard]] static timestamp_with_time_zone read(
            Datum value) noexcept {
        const TimestampTz timestamp = DatumGetTimestampTz(value);
        if (TIMESTAMP_IS_NOBEGIN(timestamp)) {
            return {
                .kind = timestamp_kind::negative_infinity,
            };
        }
        if (TIMESTAMP_IS_NOEND(timestamp)) {
            return {
                .kind = timestamp_kind::positive_infinity,
            };
        }
        return {
            .kind = timestamp_kind::finite,
            .postgres_epoch_microseconds =
                static_cast<std::int64_t>(timestamp),
        };
    }

    [[nodiscard]] static Datum write(
            const timestamp_with_time_zone& value) {
        TimestampTz timestamp{};
        switch (value.kind) {
        case timestamp_kind::finite:
            timestamp =
                static_cast<TimestampTz>(
                    value.postgres_epoch_microseconds);
            if (TIMESTAMP_NOT_FINITE(timestamp)) {
                throw scalar_conversion_error{
                    "finite timestamptz value equals a PostgreSQL "
                    "infinity sentinel"};
            }
            break;
        case timestamp_kind::negative_infinity:
            timestamp = DT_NOBEGIN;
            break;
        case timestamp_kind::positive_infinity:
            timestamp = DT_NOEND;
            break;
        default:
            throw scalar_conversion_error{
                "timestamptz value has an unknown timestamp kind"};
        }
        return TimestampTzGetDatum(timestamp);
    }
};

/** Pass an already encoded Datum through unchanged. */
template<>
struct scalar_codec<Datum> final {
    [[nodiscard]] static Datum read(Datum value) noexcept {
        return value;
    }

    [[nodiscard]] static Datum write(Datum value) noexcept {
        return value;
    }
};

/** Convert a borrowed C-string pointer to or from Datum without copying it. */
template<>
struct scalar_codec<const char*> final {
    [[nodiscard]] static const char* read(Datum value) noexcept {
        return DatumGetCString(value);
    }

    [[nodiscard]] static Datum write(const char* value) noexcept {
        return CStringGetDatum(value);
    }
};

/** Convert a borrowed, utable C-string pointer without copying its bytes. */
template<>
struct scalar_codec<char*> final {
    [[nodiscard]] static char* read(Datum value) noexcept {
        return DatumGetCString(value);
    }

    [[nodiscard]] static Datum write(char* value) noexcept {
        return CStringGetDatum(value);
    }
};

/** Convert a borrowed StringInfo pointer without copying its  buffer. */
template<>
struct scalar_codec<StringInfo> final {
    [[nodiscard]] static StringInfo read(Datum value) noexcept {
        return reinterpret_cast<StringInfo>(DatumGetPointer(value));
    }

    [[nodiscard]] static Datum write(StringInfo value) noexcept {
        return PointerGetDatum(value);
    }
};

/**
 * Borrow a varlena pointer for text, varchar or bytea. These SQL types share
 * the same C representation, while generated call sites keep their SQL types
 * distinct. This conversion neither copies nor detoasts the value
 */
template<>
struct scalar_codec<text*> final {
    [[nodiscard]] static text* read(Datum value) noexcept {
        return reinterpret_cast<text*>(DatumGetPointer(value));
    }

    [[nodiscard]] static Datum write(text* value) noexcept {
        return PointerGetDatum(value);
    }
};

/** Pass a borrowed pointer to or from a PostgreSQL INTERNAL value. */
template<>
struct scalar_codec<void*> final {
    [[nodiscard]] static void* read(Datum value) noexcept {
        return DatumGetPointer(value);
    }

    [[nodiscard]] static Datum write(void* value) noexcept {
        return PointerGetDatum(value);
    }
};

/** Ignore the table-handler pseudo-argument and encode its marker as zero. */
template<>
struct scalar_codec<table_access_method_handler> final {
    [[nodiscard]] static table_access_method_handler read(
            Datum value) noexcept {
        static_cast<void>(value);
        return {};
    }

    [[nodiscard]] static Datum write(
            table_access_method_handler value) noexcept {
        static_cast<void>(value);
        return static_cast<Datum>(0);
    }
};

/** Ignore the subscripting-handler pseudo-argument and encode its marker as zero. */
template<>
struct scalar_codec<subscript_handler> final {
    [[nodiscard]] static subscript_handler read(
            Datum value) noexcept {
        static_cast<void>(value);
        return {};
    }

    [[nodiscard]] static Datum write(
            subscript_handler value) noexcept {
        static_cast<void>(value);
        return static_cast<Datum>(0);
    }
};

/** Pass a borrowed table access-method callback table through a Datum. */
template<>
struct scalar_codec<const TableAmRoutine*> final {
    [[nodiscard]] static const TableAmRoutine* read(
            Datum value) noexcept {
        return reinterpret_cast<const TableAmRoutine*>(
            DatumGetPointer(value));
    }

    [[nodiscard]] static Datum write(
            const TableAmRoutine* value) noexcept {
        return PointerGetDatum(value);
    }
};

/** Copy PostgreSQL NameData values, allocating SQL storage when writing. */
template<>
struct scalar_codec<NameData> final {
    [[nodiscard]] static NameData read(Datum value) noexcept {
        return *DatumGetName(value);
    }

    [[nodiscard]] static Datum write(const NameData& value) {
        Name result = invoke_pg_as_cpp([] {
            return static_cast<Name>(palloc(sizeof(NameData)));
        });
        *result = value;
        return NameGetDatum(result);
    }
};

/** A C++ type  with both read-from-Datum and write-to-Datum conversions. */
template<class p_scalar_type>
concept supported_scalar =
    requires(Datum datum, p_scalar_type value) {
        { scalar_codec<p_scalar_type>::read(datum) }
            -> std::same_as<p_scalar_type>;
        { scalar_codec<p_scalar_type>::write(value) }
            -> std::same_as<Datum>;
    };

template<class p_candidate_type>
struct is_optional : std::false_type {};

template<class p_element_type>
struct is_optional<std::optional<p_element_type>> : std::true_type {
    using value_type = p_element_type;
};

/** An std::optional whose value type supports both Datum conversions. */
template<class p_candidate_type>
concept supported_optional_scalar =
    is_optional<p_candidate_type>::value
    && supported_scalar<
        typename is_optional<p_candidate_type>::value_type>;

/** Free a detoasted text copy on C++ scope exit when this object owns it. */
class packed_text_owner final {
public:
    /** Hold a packed value and record whether this owner must free it. */
    packed_text_owner(
            text* packed,
            bool owns_packed_copy) noexcept
        : packed_{packed},
          owns_packed_copy_{owns_packed_copy} {}

    packed_text_owner(const packed_text_owner&) = delete;
    packed_text_owner& operator=(const packed_text_owner&) = delete;

    packed_text_owner(packed_text_owner&& other) noexcept
        : packed_{std::exchange(other.packed_, nullptr)},
          owns_packed_copy_{
              std::exchange(other.owns_packed_copy_, false)} {}

    packed_text_owner& operator=(packed_text_owner&&) = delete;

    ~packed_text_owner() noexcept {
        if (owns_packed_copy_ && packed_ != nullptr) {
            pfree(packed_);
        }
    }

    /** Return the packed PostgreSQL text pointer. */
    [[nodiscard]] text* get() const noexcept {
        return packed_;
    }

private:
    text* packed_ = nullptr;
    bool owns_packed_copy_{};
};

/**
 * Copy a C++ string into a PostgreSQL text Datum for a scalar, or row field.
 * Leave the call's null flfg unchanged. The allocation goes through
 * invoke_pg_as_cpp so a PostgreSQL error becomes a C++ exception and row
 * objects can  be destroyed normally.
 */
[[nodiscard]] inline Datum encode_text(
        const std::string& value) {
    if (value.size() > maximum_varlena_payload_size) {
        throw scalar_conversion_error{
            "generated text result exceeds PostgreSQL's allocatable "
            "varlena payload"};
    }
    const char* const bytes = value.data();
    const int byte_count = static_cast<int>(value.size());
    text* const encoded = invoke_pg_as_cpp(
        [bytes, byte_count] {
            return cstring_to_text_with_len(bytes, byte_count);
        });
    return PointerGetDatum(encoded);
}

}  // namespace bioseq::postgres::support::detail

export namespace bioseq::postgres::support {

/**
 * Copy a text Datum into a C++ string.
 * If PostgreSQL creates a detoasted copy, free it after copying the bytes or
 * if string construction throws. Keep the original Datum untouched.
 * PostgreSQL errors are converted to C++ exceptions before creating the owner
 */
[[nodiscard]] inline std::string decode_text(
        Datum source) {
    const auto detoast = [source] {
        return DatumGetTextPP(source);
    };
    static_assert(std::is_trivially_destructible_v<decltype(detoast)>);
    text* const packed = invoke_pg_as_cpp(detoast);
    const bool owns_packed_copy =
        static_cast<const void*>(packed) != DatumGetPointer(source);
    const detail::packed_text_owner packed_owner{
        packed, owns_packed_copy};
    return std::string{
        VARDATA_ANY(packed_owner.get()),
        static_cast<std::size_t>(
            VARSIZE_ANY_EXHDR(packed_owner.get()))};
}

/**
 * Keep a SQL array available during a generated function call.
 * Free a detoasted copy on C++ scope exit. An array borrowed directly from the
 * original Datum remains owned by PostgreSQL.
 */
class array_argument final {
public:
    /** Represent a SQL NULL array with no owned allocation. */
    array_argument() noexcept = default;

    /** Hold a decoded array and track whether detoasting allocated it. */
    array_argument(
            Datum source,
            ArrayType* value) noexcept
        : value_{value},
          owns_value_{
              static_cast<const void*>(value)
              != DatumGetPointer(source)} {}

    array_argument(
        const array_argument&) = delete;
    array_argument& operator=(
        const array_argument&) = delete;

    array_argument(
            array_argument&& other) noexcept
        : value_{std::exchange(other.value_, nullptr)},
          owns_value_{std::exchange(other.owns_value_, false)} {}

    array_argument& operator=(
        array_argument&&) = delete;

    ~array_argument() noexcept {
        if (owns_value_ && value_ != nullptr) {
            pfree(value_);
        }
    }

    /** Return the array  pointer, or nullptr when this owner has no array. */
    [[nodiscard]] ArrayType* get() const noexcept {
        return value_;
    }

    /** Return the decoded array or std::nullopt for SQL NULL. */
    [[nodiscard]] std::optional<ArrayType*> optional() const noexcept {
        if (value_ == nullptr) {
            return std::nullopt;
        }
        return value_;
    }

private:
    ArrayType* value_ = nullptr;
    bool owns_value_{};
};

/**
 * Read a required array argument and keep any detoasted copy alive.
 * Reject SQL NULL. The returned owner must outlive use of its array pointer.
 */
[[nodiscard]] inline array_argument read_array_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a non-nullable generated array argument"};
    }
    const Datum source = call->args[argument_index].value;
    ArrayType* const value = invoke_pg_as_cpp([source] {
        return DatumGetArrayTypeP(source);
    });
    return array_argument{source, value};
}

/**
 * Read an array argument, returning an empty owner for SQL NULL.
 * Keep the returned owner alive while using any detoasted array covy.
 */
[[nodiscard]] inline array_argument read_nullable_array_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) {
        return {};
    }
    const Datum source = call->args[argument_index].value;
    ArrayType* const value = invoke_pg_as_cpp([source] {
        return DatumGetArrayTypeP(source);
    });
    return array_argument{source, value};
}

}  // namespace bioseq::postgres::support

export namespace bioseq::postgres::support::detail {

/** Free a PostgreSQL allocation on C++ scope exit, including exception unwinding. */
class palloc_owner final {
public:
    /** Take ownership of one palloc allocation. */
    explicit palloc_owner(void* value) noexcept
        : value_{value} {}

    palloc_owner(const palloc_owner&) = delete;
    palloc_owner& operator=(const palloc_owner&) = delete;

    ~palloc_owner() noexcept {
        if (value_ != nullptr) {
            pfree(value_);
        }
    }

private:
    void* value_;
};

/** Copy JSONB into a C++ string using PostgreSQL's JSON output format. */
[[nodiscard]] inline jsonb_value decode_jsonb(Datum source) {
    char* const encoded = invoke_pg_as_cpp([source] {
        return DatumGetCString(
            DirectFunctionCall1(jsonb_out, source));
    });
    const palloc_owner owner{encoded};
    return jsonb_value{std::string{encoded}};
}

/** Parse the stored JSON text with PostgreSQL to produce a JSONB Datum. */
[[nodiscard]] inline Datum encode_jsonb(
        const jsonb_value& value) {
    const char* const encoded = value.encoded().c_str();
    return invoke_pg_as_cpp([encoded] {
        return DirectFunctionCall1(
            jsonb_in, CStringGetDatum(encoded));
    });
}

/** Copy the owned bytes into a  newly allocated SQL BYTEA value. */
[[nodiscard]] inline Datum encode_bytea(
        const bytea_value& value) {
    if (value.bytes().size() > maximum_varlena_payload_size) {
        throw scalar_conversion_error{
            "generated bytea result exceeds PostgreSQL's allocatable "
            "varlena payload"};
    }
    const std::byte* const bytes = value.bytes().data();
    const std::size_t byte_count = value.bytes().size();
    const int total_size =
        static_cast<int>(byte_count + VARHDRSZ);
    bytea* const encoded = invoke_pg_as_cpp(
        [bytes, byte_count, total_size] {
            auto* result = static_cast<bytea*>(
                palloc(static_cast<Size>(total_size)));
            SET_VARSIZE(result, total_size);
            if (byte_count != 0) {
                std::memcpy(
                    VARDATA(result),
                    bytes,
                    byte_count);
            }
            return result;
        });
    return PointerGetDatum(encoded);
}

/** Copy a SQL BYTEA value into a C++ byte vector. */
[[nodiscard]] inline bytea_value decode_bytea(Datum source) {
    bytea* const packed = invoke_pg_as_cpp([source] {
        return DatumGetByteaPP(source);
    });
    const bool owns_packed =
        static_cast<const void*>(packed) != DatumGetPointer(source);
    const palloc_owner owner{
        owns_packed ? static_cast<void*>(packed) : nullptr};
    const auto* const begin = reinterpret_cast<const std::byte*>(
        VARDATA_ANY(packed));
    return bytea_value{
        std::vector<std::byte>{
            begin,
            begin + VARSIZE_ANY_EXHDR(packed)}};
}

/** Encode integers as a one-dimensional bigint array, or an empty SQL array. */
[[nodiscard]] inline Datum encode_bigint_array(
        const bigint_array& value) {
    if (value.values().size()
        > static_cast<std::size_t>(
              std::numeric_limits<int>::max())) {
        throw scalar_conversion_error{
            "generated bigint[] result has too many elements"};
    }

    std::vector<Datum> elements;
    elements.reserve(value.values().size());
    for (const std::int64_t element : value.values()) {
        elements.push_back(Int64GetDatum(element));
    }
    Datum* const data = elements.data();
    const int count = static_cast<int>(elements.size());
    ArrayType* const encoded = invoke_pg_as_cpp(
        [data, count] {
            if (count == 0) {
                return construct_empty_array(INT8OID);
            }
            return construct_array(
                data,
                count,
                INT8OID,
                static_cast<int>(sizeof(std::int64_t)),
                FLOAT8PASSBYVAL,
                TYPALIGN_DOUBLE);
        });
    return PointerGetDatum(encoded);
}

/** Copy, the time, day and month fields into a SQL INTERVAL value */
[[nodiscard]] inline Datum encode_interval(
        const interval_value& value) {
    Interval* const encoded = invoke_pg_as_cpp([value] {
        auto* const result = static_cast<Interval*>(
            palloc(sizeof(Interval)));
        result->time = value.microseconds;
        result->day = value.days;
        result->month = value.months;
        return result;
    });
    return PointerGetDatum(encoded);
}

}  // namespace bioseq::postgres::support::detail

export namespace bioseq::postgres::support {

/** Copy a JSONB argument into owned text, rejecting SQL NULL. */
template<class p_requested_type>
    requires std::same_as<p_requested_type, jsonb_value>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a required JSONB argument"};
    }
    return detail::decode_jsonb(call->args[argument_index].value);
}

/** Copy a JSONB argument, returning std::nullopt for SQL NULL. */
template<class p_requested_type>
    requires std::same_as<
        p_requested_type,
        std::optional<jsonb_value>>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) return std::nullopt;
    return read_argument<jsonb_value>(call, argument_index);
}

/** Copy a BYTEA argument into owned bytes, rejecting SQL NULL. */
template<class p_requested_type>
    requires std::same_as<p_requested_type, bytea_value>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a required BYTEA argument"};
    }
    return detail::decode_bytea(call->args[argument_index].value);
}

/** Copy a BYTEA argument, returning std::nullopt for SQL NULL. */
template<class p_requested_type>
    requires std::same_as<
        p_requested_type,
        std::optional<bytea_value>>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) return std::nullopt;
    return read_argument<bytea_value>(call, argument_index);
}

/** Copy an INTERVAL argument's fields, rejecting SQL NULL. */
template<class p_requested_type>
    requires std::same_as<p_requested_type, interval_value>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a required INTERVAL argument"};
    }
    const Interval* const interval = DatumGetIntervalP(
        call->args[argument_index].value);
    return {
        .microseconds = interval->time,
        .days = interval->day,
        .months = interval->month,
    };
}

/** Copy an INTERVAL argument, returning std::nullopt for SQL NULL. */
template<class p_requested_type>
    requires std::same_as<
        p_requested_type,
        std::optional<interval_value>>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) return std::nullopt;
    return read_argument<interval_value>(call, argument_index);
}

/** Read a physical row address, rejecting SQL NULL or an invalid item pointer. */
template<class p_requested_type>
    requires std::same_as<p_requested_type, tuple_identifier>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a required TID argument"};
    }
    ItemPointer const item = DatumGetItemPointer(
        call->args[argument_index].value);
    if (item == nullptr || !ItemPointerIsValid(item)) {
        throw scalar_conversion_error{"PostgreSQL TID argument is invalid"};
    }
    return {
        .block_number = ItemPointerGetBlockNumber(item),
        .offset_number = ItemPointerGetOffsetNumber(item),
    };
}

/** Convert a required SQL argument, rejecting SQL NULL. */
template<class p_scalar_type>
    requires detail::supported_scalar<p_scalar_type>
[[nodiscard]] inline p_scalar_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a required scalar argument"};
    }
    return detail::scalar_codec<p_scalar_type>::read(
        call->args[argument_index].value);
}

/** Convert a nullable SQL argument, using std::nullopt for SQL NULL. */
template<class p_scalar_type>
    requires detail::supported_scalar<p_scalar_type>
[[nodiscard]] inline std::optional<p_scalar_type> read_argument(
        FunctionCallInfo call,
        int argument_index,
        std::type_identity<std::optional<p_scalar_type>>) {
    if constexpr (
            std::same_as<
                p_scalar_type,
                table_access_method_handler>
            || std::same_as<p_scalar_type, subscript_handler>) {
        // PostgreSQL declares these handlers with one INTERNAL pseudo-argument
        // but invokes them through OidFunctionCall0.
        if (call != nullptr && argument_index == 0 && call->nargs == 0) {
            return std::nullopt;
        }
    }
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) return std::nullopt;
    return detail::scalar_codec<p_scalar_type>::read(
        call->args[argument_index].value);
}

/** Read an  optional scalar through its nullable argument conversion. */
template<class p_requested_type>
    requires detail::supported_optional_scalar<p_requested_type>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    return read_argument(
        call,
        argument_index,
        std::type_identity<p_requested_type>{});
}

/** Copy a text argument into a C++ string, rejecting SQL NULL. */
template<class p_requested_type>
    requires std::same_as<p_requested_type, std::string>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a required text argument"};
    }
    return decode_text(call->args[argument_index].value);
}

/** Copy a text argument, returning std::nullopt for SQL NULL. */
template<class p_requested_type>
    requires std::same_as<
        p_requested_type,
        std::optional<std::string>>
[[nodiscard]] inline p_requested_type read_argument(
        FunctionCallInfo call,
        int argument_index) {
    detail::require_argument(call, argument_index);
    if (call->args[argument_index].isnull) return std::nullopt;
    return read_argument<std::string>(call, argument_index);
}

/** Give the C++ routine access to its SQL call without adding a SQL argumen. */
[[nodiscard]] inline function_call_context read_call_context(
        FunctionCallInfo call) noexcept {
    return function_call_context::from_call(call);
}

/** Encode a scalar return value and mark the SQL result as non-null. */
template<class p_scalar_type>
    requires detail::supported_scalar<
        std::remove_cvref_t<p_scalar_type>>
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        p_scalar_type&& value) noexcept(noexcept(
            detail::scalar_codec<
                std::remove_cvref_t<p_scalar_type>>::write(
                    std::forward<p_scalar_type>(value)))) {
    call->isnull = false;
    return detail::scalar_codec<
        std::remove_cvref_t<p_scalar_type>>::write(
            std::forward<p_scalar_type>(value));
}

/** Copy a C++ string into the SQL text result, with PostgreSQL errors bridged. */
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        const std::string& value) {
    const Datum encoded = detail::encode_text(value);
    call->isnull = false;
    return encoded;
}

/** Return an existing PostgreSQL array without copying it. */
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        ArrayType* value) noexcept {
    call->isnull = false;
    return PointerGetDatum(value);
}

/** Parse the owned JSON text and return it as SQL JSONB. */
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        const jsonb_value& value) {
    const Datum encoded = detail::encode_jsonb(value);
    call->isnull = false;
    return encoded;
}

/** Copy the owned bytes into the SQL BYTEA result. */
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        const bytea_value& value) {
    const Datum encoded = detail::encode_bytea(value);
    call->isnull = false;
    return encoded;
}

/** Copy the integer elements into  the SQL bigint-array result. */
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        const bigint_array& value) {
    const Datum encoded = detail::encode_bigint_array(value);
    call->isnull = false;
    return encoded;
}

/** Copy the interval fields into the SQL INTERVAL result. */
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        const interval_value& value) {
    const Datum encoded = detail::encode_interval(value);
    call->isnull = false;
    return encoded;
}

/** Encode a nullable result, setting the fmgr null flag when empty. */
template<class p_value_type>
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        const std::optional<p_value_type>& value) {
    if (!value.has_value()) {
        call->isnull = true;
        return static_cast<Datum>(0);
    }
    return encode_result(call, *value);
}

/** Mark a procedure result as SQL void. */
[[nodiscard]] inline Datum encode_void_result(
        FunctionCallInfo call) noexcept {
    call->isnull = false;
    return static_cast<Datum>(0);
}

}  // namespace bioseq::postgres::support
