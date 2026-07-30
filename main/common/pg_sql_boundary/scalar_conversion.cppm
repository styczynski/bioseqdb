/*-------------------------------------------------------------------------
 *
 * common/pg_sql_boundary/scalar_conversion.cppm
 *
 * Closed scalar conversion surface used by generated fmgr entrypoints.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"
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

export module bioseq.pg_sql_boundary:scalar_conversion;

import :call_context;
import :error_boundary;
import :owning_values;

export namespace bioseq::pg_utils {

class sql_boundary_conversion_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

inline constexpr std::size_t maximum_text_payload =
    static_cast<std::size_t>(MaxAllocSize - VARHDRSZ)
        < static_cast<std::size_t>(
              std::numeric_limits<int>::max() - VARHDRSZ)
    ? static_cast<std::size_t>(MaxAllocSize - VARHDRSZ)
    : static_cast<std::size_t>(
          std::numeric_limits<int>::max() - VARHDRSZ);

inline void require_sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    if (call == nullptr) {
        throw sql_boundary_conversion_error{
            "generated SQL boundary received a null call frame"};
    }
    if (zero_based_index < 0
        || zero_based_index >= static_cast<int>(call->nargs)) {
        throw sql_boundary_conversion_error{
            "generated SQL boundary argument index is out of range"};
    }
}

template<class T>
struct sql_scalar_conversion;

template<>
struct sql_scalar_conversion<bool> final {
    static constexpr const char* key = "bool/by_value";

    [[nodiscard]] static bool read(Datum value) noexcept {
        return DatumGetBool(value);
    }

    [[nodiscard]] static Datum write(bool value) noexcept {
        return BoolGetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<std::int16_t> final {
    static constexpr const char* key = "int16/by_value";

    [[nodiscard]] static std::int16_t read(Datum value) noexcept {
        return DatumGetInt16(value);
    }

    [[nodiscard]] static Datum write(std::int16_t value) noexcept {
        return Int16GetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<std::int32_t> final {
    static constexpr const char* key = "int32/by_value";

    [[nodiscard]] static std::int32_t read(Datum value) noexcept {
        return DatumGetInt32(value);
    }

    [[nodiscard]] static Datum write(std::int32_t value) noexcept {
        return Int32GetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<std::int64_t> final {
    static constexpr const char* key = "int64/by_value";

    [[nodiscard]] static std::int64_t read(Datum value) noexcept {
        return DatumGetInt64(value);
    }

    [[nodiscard]] static Datum write(std::int64_t value) noexcept {
        return Int64GetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<float> final {
    static constexpr const char* key = "float32/by_value";

    [[nodiscard]] static float read(Datum value) noexcept {
        return DatumGetFloat4(value);
    }

    [[nodiscard]] static Datum write(float value) noexcept {
        return Float4GetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<double> final {
    static constexpr const char* key = "float64/by_value";

    [[nodiscard]] static double read(Datum value) noexcept {
        return DatumGetFloat8(value);
    }

    [[nodiscard]] static Datum write(double value) noexcept {
        return Float8GetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<Oid> final {
    static constexpr const char* key = "oid/by_value";

    [[nodiscard]] static Oid read(Datum value) noexcept {
        return DatumGetObjectId(value);
    }

    [[nodiscard]] static Datum write(Oid value) noexcept {
        return ObjectIdGetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<sql_timestamptz> final {
    static constexpr const char* key = "timestamptz/owning";

    [[nodiscard]] static sql_timestamptz read(
            Datum value) noexcept {
        const TimestampTz timestamp = DatumGetTimestampTz(value);
        if (TIMESTAMP_IS_NOBEGIN(timestamp)) {
            return {
                .kind = sql_timestamp_kind::negative_infinity,
            };
        }
        if (TIMESTAMP_IS_NOEND(timestamp)) {
            return {
                .kind = sql_timestamp_kind::positive_infinity,
            };
        }
        return {
            .kind = sql_timestamp_kind::finite,
            .postgres_microseconds =
                static_cast<std::int64_t>(timestamp),
        };
    }

    [[nodiscard]] static Datum write(
            const sql_timestamptz& value) {
        TimestampTz timestamp{};
        switch (value.kind) {
        case sql_timestamp_kind::finite:
            timestamp =
                static_cast<TimestampTz>(
                    value.postgres_microseconds);
            if (TIMESTAMP_NOT_FINITE(timestamp)) {
                throw sql_boundary_conversion_error{
                    "finite timestamptz value equals a PostgreSQL "
                    "infinity sentinel"};
            }
            break;
        case sql_timestamp_kind::negative_infinity:
            timestamp = DT_NOBEGIN;
            break;
        case sql_timestamp_kind::positive_infinity:
            timestamp = DT_NOEND;
            break;
        default:
            throw sql_boundary_conversion_error{
                "timestamptz value has an unknown timestamp kind"};
        }
        return TimestampTzGetDatum(timestamp);
    }
};

template<>
struct sql_scalar_conversion<Datum> final {
    static constexpr const char* key = "datum/opaque";

    [[nodiscard]] static Datum read(Datum value) noexcept {
        return value;
    }

    [[nodiscard]] static Datum write(Datum value) noexcept {
        return value;
    }
};

template<>
struct sql_scalar_conversion<const char*> final {
    static constexpr const char* key = "cstring/borrowed";

    [[nodiscard]] static const char* read(Datum value) noexcept {
        return DatumGetCString(value);
    }

    [[nodiscard]] static Datum write(const char* value) noexcept {
        return CStringGetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<char*> final {
    static constexpr const char* key = "cstring/owned";

    [[nodiscard]] static char* read(Datum value) noexcept {
        return DatumGetCString(value);
    }

    [[nodiscard]] static Datum write(char* value) noexcept {
        return CStringGetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<StringInfo> final {
    static constexpr const char* key = "string_info/borrowed";

    [[nodiscard]] static StringInfo read(Datum value) noexcept {
        return reinterpret_cast<StringInfo>(DatumGetPointer(value));
    }

    [[nodiscard]] static Datum write(StringInfo value) noexcept {
        return PointerGetDatum(value);
    }
};

/*
 * text, varchar, and bytea share PostgreSQL's varlena C representation.
 * Their SQL identity remains distinct in the generated converter registry.
 */
template<>
struct sql_scalar_conversion<text*> final {
    static constexpr const char* key = "varlena/borrowed_or_owned";

    [[nodiscard]] static text* read(Datum value) noexcept {
        return reinterpret_cast<text*>(DatumGetPointer(value));
    }

    [[nodiscard]] static Datum write(text* value) noexcept {
        return PointerGetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<void*> final {
    static constexpr const char* key = "internal/opaque";

    [[nodiscard]] static void* read(Datum value) noexcept {
        return DatumGetPointer(value);
    }

    [[nodiscard]] static Datum write(void* value) noexcept {
        return PointerGetDatum(value);
    }
};

template<>
struct sql_scalar_conversion<table_am_handler_context> final {
    static constexpr const char* key = "table_am_handler/context";

    [[nodiscard]] static table_am_handler_context read(
            Datum value) noexcept {
        static_cast<void>(value);
        return {};
    }

    [[nodiscard]] static Datum write(
            table_am_handler_context value) noexcept {
        static_cast<void>(value);
        return static_cast<Datum>(0);
    }
};

template<>
struct sql_scalar_conversion<subscript_handler_context> final {
    static constexpr const char* key = "subscript_handler/context";

    [[nodiscard]] static subscript_handler_context read(
            Datum value) noexcept {
        static_cast<void>(value);
        return {};
    }

    [[nodiscard]] static Datum write(
            subscript_handler_context value) noexcept {
        static_cast<void>(value);
        return static_cast<Datum>(0);
    }
};

template<>
struct sql_scalar_conversion<const TableAmRoutine*> final {
    static constexpr const char* key = "table_am_handler/pointer";

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

template<>
struct sql_scalar_conversion<NameData> final {
    static constexpr const char* key = "name/owning";

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

template<class T>
concept registered_sql_scalar =
    requires(Datum datum, T value) {
        { sql_scalar_conversion<T>::key } -> std::convertible_to<const char*>;
        { sql_scalar_conversion<T>::read(datum) } -> std::same_as<T>;
        { sql_scalar_conversion<T>::write(value) } -> std::same_as<Datum>;
    };

template<class T>
struct is_optional : std::false_type {};

template<class T>
struct is_optional<std::optional<T>> : std::true_type {
    using value_type = T;
};

template<class T>
concept registered_optional_sql_scalar =
    is_optional<T>::value
    && registered_sql_scalar<typename is_optional<T>::value_type>;

class packed_text_guard final {
public:
    packed_text_guard(
            text* packed,
            bool owns_packed_copy) noexcept
        : packed_{packed},
          owns_packed_copy_{owns_packed_copy} {}

    packed_text_guard(const packed_text_guard&) = delete;
    packed_text_guard& operator=(const packed_text_guard&) = delete;

    packed_text_guard(packed_text_guard&& other) noexcept
        : packed_{std::exchange(other.packed_, nullptr)},
          owns_packed_copy_{
              std::exchange(other.owns_packed_copy_, false)} {}

    packed_text_guard& operator=(packed_text_guard&&) = delete;

    ~packed_text_guard() noexcept {
        if (owns_packed_copy_ && packed_ != nullptr) {
            pfree(packed_);
        }
    }

    [[nodiscard]] text* get() const noexcept {
        return packed_;
    }

private:
    text* packed_ = nullptr;
    bool owns_packed_copy_{};
};

/**
 * Encode text without mutating an fmgr result slot.
 *
 * Row materialization uses this leaf for individual text fields. Keeping the
 * PostgreSQL allocation behind invoke_pg_as_cpp ensures that an ERROR is
 * translated before control returns to a scope that owns C++ row values.
 */
[[nodiscard]] inline Datum encode_sql_text(
        const std::string& value) {
    if (value.size() > maximum_text_payload) {
        throw sql_boundary_conversion_error{
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

}  // namespace detail

/**
 * Copy one PostgreSQL text Datum into an owning C++ string.
 *
 * DatumGetTextPP is invoked behind the PostgreSQL ERROR bridge with a
 * trivially destructible capture.  If PostgreSQL materializes a packed
 * detoast copy, packed_text_guard frees that copy exactly once after the C++
 * string owns the bytes (including when string construction throws).  An
 * inline source Datum remains borrowed and is never freed here.
 */
[[nodiscard]] inline std::string copy_text_datum_to_string(
        Datum source) {
    const auto detoast = [source] {
        return DatumGetTextPP(source);
    };
    static_assert(std::is_trivially_destructible_v<decltype(detoast)>);
    text* const packed = invoke_pg_as_cpp(detoast);
    const bool owns_packed_copy =
        static_cast<const void*>(packed) != DatumGetPointer(source);
    const detail::packed_text_guard packed_owner{
        packed, owns_packed_copy};
    return std::string{
        VARDATA_ANY(packed_owner.get()),
        static_cast<std::size_t>(
            VARSIZE_ANY_EXHDR(packed_owner.get()))};
}

/**
 * Call-scoped PostgreSQL array input.
 *
 * PostgreSQL may return either the original array or a detoasted working
 * copy. The guard releases only the latter after the typed owner returns.
 */
class sql_array_argument_guard final {
public:
    sql_array_argument_guard() noexcept = default;

    sql_array_argument_guard(
            Datum source,
            ArrayType* value) noexcept
        : value_{value},
          owns_value_{
              static_cast<const void*>(value)
              != DatumGetPointer(source)} {}

    sql_array_argument_guard(
        const sql_array_argument_guard&) = delete;
    sql_array_argument_guard& operator=(
        const sql_array_argument_guard&) = delete;

    sql_array_argument_guard(
            sql_array_argument_guard&& other) noexcept
        : value_{std::exchange(other.value_, nullptr)},
          owns_value_{std::exchange(other.owns_value_, false)} {}

    sql_array_argument_guard& operator=(
        sql_array_argument_guard&&) = delete;

    ~sql_array_argument_guard() noexcept {
        if (owns_value_ && value_ != nullptr) {
            pfree(value_);
        }
    }

    [[nodiscard]] ArrayType* get() const noexcept {
        return value_;
    }

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
 * Decode one non-null SQL array and retain its detoast copy for the duration
 * of the generated owner call.
 */
[[nodiscard]] inline sql_array_argument_guard sql_array_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable generated array argument"};
    }
    const Datum source = call->args[zero_based_index].value;
    ArrayType* const value = invoke_pg_as_cpp([source] {
        return DatumGetArrayTypeP(source);
    });
    return sql_array_argument_guard{source, value};
}

/**
 * Decode a nullable SQL array while retaining any detoast copy for the
 * duration of the generated owner call.
 */
[[nodiscard]] inline sql_array_argument_guard sql_nullable_array_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return {};
    }
    const Datum source = call->args[zero_based_index].value;
    ArrayType* const value = invoke_pg_as_cpp([source] {
        return DatumGetArrayTypeP(source);
    });
    return sql_array_argument_guard{source, value};
}

namespace detail {

class palloc_guard final {
public:
    explicit palloc_guard(void* value) noexcept
        : value_{value} {}

    palloc_guard(const palloc_guard&) = delete;
    palloc_guard& operator=(const palloc_guard&) = delete;

    ~palloc_guard() noexcept {
        if (value_ != nullptr) {
            pfree(value_);
        }
    }

private:
    void* value_;
};

[[nodiscard]] inline sql_jsonb decode_sql_jsonb(Datum source) {
    char* const encoded = invoke_pg_as_cpp([source] {
        return DatumGetCString(
            DirectFunctionCall1(jsonb_out, source));
    });
    const palloc_guard owner{encoded};
    return sql_jsonb{std::string{encoded}};
}

[[nodiscard]] inline Datum encode_sql_jsonb(
        const sql_jsonb& value) {
    const char* const encoded = value.encoded().c_str();
    return invoke_pg_as_cpp([encoded] {
        return DirectFunctionCall1(
            jsonb_in, CStringGetDatum(encoded));
    });
}

[[nodiscard]] inline Datum encode_sql_bytea(
        const sql_bytea& value) {
    if (value.bytes().size() > maximum_text_payload) {
        throw sql_boundary_conversion_error{
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

[[nodiscard]] inline sql_bytea decode_sql_bytea(Datum source) {
    bytea* const packed = invoke_pg_as_cpp([source] {
        return DatumGetByteaPP(source);
    });
    const bool owns_packed =
        static_cast<const void*>(packed) != DatumGetPointer(source);
    const palloc_guard owner{
        owns_packed ? static_cast<void*>(packed) : nullptr};
    const auto* const begin = reinterpret_cast<const std::byte*>(
        VARDATA_ANY(packed));
    return sql_bytea{
        std::vector<std::byte>{
            begin,
            begin + VARSIZE_ANY_EXHDR(packed)}};
}

[[nodiscard]] inline Datum encode_sql_int64_array(
        const sql_int64_array& value) {
    if (value.values().size()
        > static_cast<std::size_t>(
              std::numeric_limits<int>::max())) {
        throw sql_boundary_conversion_error{
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

[[nodiscard]] inline Datum encode_sql_interval(
        const sql_interval& value) {
    Interval* const encoded = invoke_pg_as_cpp([value] {
        auto* const result = static_cast<Interval*>(
            palloc(sizeof(Interval)));
        result->time = value.time;
        result->day = value.day;
        result->month = value.month;
        return result;
    });
    return PointerGetDatum(encoded);
}

}  // namespace detail

template<class T>
    requires std::same_as<T, sql_jsonb>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable JSONB argument"};
    }
    return detail::decode_sql_jsonb(
        call->args[zero_based_index].value);
}

template<class T>
    requires std::same_as<T, std::optional<sql_jsonb>>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return sql_argument<sql_jsonb>(call, zero_based_index);
}

template<class T>
    requires std::same_as<T, sql_bytea>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable BYTEA argument"};
    }
    return detail::decode_sql_bytea(
        call->args[zero_based_index].value);
}

template<class T>
    requires std::same_as<T, std::optional<sql_bytea>>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return sql_argument<sql_bytea>(call, zero_based_index);
}

template<class T>
    requires std::same_as<T, sql_interval>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable INTERVAL argument"};
    }
    const Interval* const interval = DatumGetIntervalP(
        call->args[zero_based_index].value);
    return {
        .time = interval->time,
        .day = interval->day,
        .month = interval->month,
    };
}

template<class T>
    requires std::same_as<T, std::optional<sql_interval>>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return sql_argument<sql_interval>(call, zero_based_index);
}

template<class T>
    requires std::same_as<T, sql_tid>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable TID argument"};
    }
    ItemPointer const item = DatumGetItemPointer(
        call->args[zero_based_index].value);
    if (item == nullptr || !ItemPointerIsValid(item)) {
        throw sql_boundary_conversion_error{
            "generated TID argument is invalid"};
    }
    return sql_tid{
        .block_number = ItemPointerGetBlockNumber(item),
        .offset_number = ItemPointerGetOffsetNumber(item),
    };
}

/**
 * Decode one non-null, registered by-value argument from an fmgr call.
 */
template<class T>
    requires detail::registered_sql_scalar<T>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable generated argument"};
    }
    return detail::sql_scalar_conversion<T>::read(
        call->args[zero_based_index].value);
}

/**
 * Decode one nullable argument without inventing a sentinel value.
 */
template<class T>
    requires detail::registered_sql_scalar<T>
[[nodiscard]] inline std::optional<T> sql_argument(
        FunctionCallInfo call,
        int zero_based_index,
        std::type_identity<std::optional<T>>) {
    if constexpr (
            std::same_as<T, table_am_handler_context>
            || std::same_as<T, subscript_handler_context>) {
        /*
         * PostgreSQL declares table-AM and subscripting handlers with one
         * INTERNAL pseudo-argument but invokes them through OidFunctionCall0.
         * Preserve those catalog signatures while representing the absent
         * call-frame value as the handler-specific optional context marker.
         */
        if (call != nullptr
            && zero_based_index == 0
            && call->nargs == 0) {
            return std::nullopt;
        }
    }
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return detail::sql_scalar_conversion<T>::read(
        call->args[zero_based_index].value);
}

template<class T>
    requires detail::registered_optional_sql_scalar<T>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    return sql_argument(
        call,
        zero_based_index,
        std::type_identity<T>{});
}

template<class T>
    requires std::same_as<T, std::string>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable generated text argument"};
    }
    return copy_text_datum_to_string(
        call->args[zero_based_index].value);
}

template<class T>
    requires std::same_as<T, std::optional<std::string>>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return sql_argument<std::string>(call, zero_based_index);
}

/**
 * Construct the one injected, non-SQL call-context parameter.
 */
[[nodiscard]] inline sql_call_context sql_context_argument(
        FunctionCallInfo call) noexcept {
    return sql_boundary_access::make_context(call);
}

/**
 * Encode a registered scalar result into the current fmgr result slot.
 */
template<class T>
    requires detail::registered_sql_scalar<std::remove_cvref_t<T>>
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        T&& value) noexcept(noexcept(
            detail::sql_scalar_conversion<
                std::remove_cvref_t<T>>::write(
                    std::forward<T>(value)))) {
    call->isnull = false;
    return detail::sql_scalar_conversion<
        std::remove_cvref_t<T>>::write(std::forward<T>(value));
}

/**
 * Encode an owning text result. PostgreSQL allocation is bridged so an ERROR
 * cannot skip destruction of the caller's std::string.
 */
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        const std::string& value) {
    const Datum encoded = detail::encode_sql_text(value);
    call->isnull = false;
    return encoded;
}

/**
 * Return an array allocated by a typed owner.
 */
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        ArrayType* value) noexcept {
    call->isnull = false;
    return PointerGetDatum(value);
}

[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        const sql_jsonb& value) {
    const Datum encoded = detail::encode_sql_jsonb(value);
    call->isnull = false;
    return encoded;
}

[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        const sql_bytea& value) {
    const Datum encoded = detail::encode_sql_bytea(value);
    call->isnull = false;
    return encoded;
}

[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        const sql_int64_array& value) {
    const Datum encoded = detail::encode_sql_int64_array(value);
    call->isnull = false;
    return encoded;
}

[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        const sql_interval& value) {
    const Datum encoded = detail::encode_sql_interval(value);
    call->isnull = false;
    return encoded;
}

template<class T>
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        const std::optional<T>& value) {
    if (!value.has_value()) {
        call->isnull = true;
        return static_cast<Datum>(0);
    }
    return sql_result_to_datum(call, *value);
}

/**
 * Mark a generated procedure result as SQL void.
 */
[[nodiscard]] inline Datum sql_void_result(FunctionCallInfo call) noexcept {
    call->isnull = false;
    return static_cast<Datum>(0);
}

namespace detail {

static_assert(registered_sql_scalar<bool>);
static_assert(registered_sql_scalar<std::int16_t>);
static_assert(registered_sql_scalar<std::int32_t>);
static_assert(registered_sql_scalar<std::int64_t>);
static_assert(registered_sql_scalar<float>);
static_assert(registered_sql_scalar<double>);
static_assert(registered_sql_scalar<Oid>);
static_assert(registered_sql_scalar<sql_timestamptz>);
static_assert(registered_sql_scalar<Datum>);
static_assert(registered_sql_scalar<const char*>);
static_assert(registered_sql_scalar<char*>);
static_assert(registered_sql_scalar<StringInfo>);
static_assert(registered_sql_scalar<text*>);
static_assert(registered_sql_scalar<void*>);
static_assert(registered_sql_scalar<table_am_handler_context>);
static_assert(registered_sql_scalar<subscript_handler_context>);
static_assert(registered_sql_scalar<const TableAmRoutine*>);
static_assert(registered_sql_scalar<NameData>);
static_assert(is_optional<std::optional<bool>>::value);
static_assert(!is_optional<bool>::value);
static_assert(
    registered_optional_sql_scalar<std::optional<bool>>);
static_assert(
    registered_optional_sql_scalar<
        std::optional<sql_timestamptz>>);
static_assert(
    !registered_optional_sql_scalar<std::optional<std::string>>);
static_assert(
    maximum_text_payload
    <= static_cast<std::size_t>(
        std::numeric_limits<int>::max() - VARHDRSZ));
static_assert(
    maximum_text_payload
    <= static_cast<std::size_t>(MaxAllocSize - VARHDRSZ));
static_assert(requires(FunctionCallInfo call) {
    {
        sql_argument<std::optional<std::string>>(call, 0)
    } -> std::same_as<std::optional<std::string>>;
    {
        sql_argument<std::optional<sql_timestamptz>>(call, 0)
    } -> std::same_as<std::optional<sql_timestamptz>>;
    {
        sql_argument<std::optional<sql_interval>>(call, 0)
    } -> std::same_as<std::optional<sql_interval>>;
    {
        sql_argument<std::optional<sql_bytea>>(call, 0)
    } -> std::same_as<std::optional<sql_bytea>>;
    {
        sql_nullable_array_argument(call, 0).optional()
    } -> std::same_as<std::optional<ArrayType*>>;
});

}  // namespace detail

}  // namespace bioseq::pg_utils
