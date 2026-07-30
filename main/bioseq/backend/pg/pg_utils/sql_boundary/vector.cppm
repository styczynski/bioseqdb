/*-------------------------------------------------------------------------
 *
 * pg_utils/sql_boundary/vector.cppm
 *
 * Checked, typed views over the private vector-executor fmgr ABI.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/timestamp.h"
}

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

export module bioseq.pg_utils.sql_boundary:vector;

import bioseq.codegen.sql.annotation_model;
import bioseq.pg_utils.error_bridge;
import bioseq.pg_sql_boundary;
import bioseq.pg_utils.runtime_test_counters;
import bioseq.vector.types;

export namespace bioseq::pg_utils {

template<auto SqlType, class Lane>
struct vector_lane_traits;

template<auto SqlType, class Lane>
class vector_column;

template<auto SqlType, class Lane>
class vector_operand;

class vector_null_mask;

template<auto SqlType, class Lane>
class vector_result;

namespace detail {
struct vector_boundary_access;
}

/**
 * Invocation-scoped, normalized view of one PostgreSQL character datum.
 */
class bpchar_view final {
public:
    bpchar_view(const bpchar_view&) = delete;
    bpchar_view& operator=(const bpchar_view&) = delete;

    bpchar_view(bpchar_view&& other) noexcept
        : source_{std::exchange(other.source_, Datum{})},
          packed_{std::exchange(other.packed_, nullptr)},
          normalized_byte_count_{
              std::exchange(other.normalized_byte_count_, 0)},
          collation_{std::exchange(other.collation_, InvalidOid)},
          owns_packed_copy_{
              std::exchange(other.owns_packed_copy_, false)} {}

    bpchar_view& operator=(bpchar_view&& other) noexcept {
        if (this == &other) return *this;
        reset();
        source_ = std::exchange(other.source_, Datum{});
        packed_ = std::exchange(other.packed_, nullptr);
        normalized_byte_count_ =
            std::exchange(other.normalized_byte_count_, 0);
        collation_ = std::exchange(other.collation_, InvalidOid);
        owns_packed_copy_ =
            std::exchange(other.owns_packed_copy_, false);
        return *this;
    }

    ~bpchar_view() noexcept {
        reset();
    }

    [[nodiscard]] std::string_view normalized_bytes() const noexcept {
        return {
            VARDATA_ANY(packed_),
            static_cast<std::size_t>(normalized_byte_count_)};
    }

    [[nodiscard]] Oid collation() const noexcept {
        return collation_;
    }

private:
    template<auto, class>
    friend struct vector_lane_traits;

    [[nodiscard]] static bpchar_view from_datum(
            Datum source,
            Oid collation) {
        const Pointer raw = DatumGetPointer(source);
        using enum runtime_test_counters::bpchar_source_form;
        auto source_form = unknown;
        if (raw != nullptr) {
            if (VARATT_IS_EXTERNAL_ONDISK(raw)) {
                source_form = external;
            } else if (VARATT_IS_COMPRESSED(raw)) {
                source_form = compressed;
            } else if (VARATT_IS_4B_U(raw)) {
                source_form = four_byte_uncompressed;
            } else if (VARATT_IS_SHORT(raw)) {
                source_form = short_header;
            }
        }
        runtime_test_counters::note_bpchar_source_form(
            source_form,
            runtime_test_counters::
                bpchar_source_site::view_decode);
        BpChar* const packed = invoke_pg_as_cpp(
            [source] {
                return DatumGetBpCharPP(source);
            });
        const bool owns_packed_copy =
            reinterpret_cast<Pointer>(packed) != DatumGetPointer(source);
        runtime_test_counters::note_view_construction(
            owns_packed_copy);
        return bpchar_view{
            source,
            packed,
            bpchartruelen(
                VARDATA_ANY(packed),
                VARSIZE_ANY_EXHDR(packed)),
            collation,
            owns_packed_copy};
    }

    bpchar_view(
            Datum source,
            BpChar* packed,
            std::int32_t normalized_byte_count,
            Oid collation,
            bool owns_packed_copy) noexcept
        : source_{source},
          packed_{packed},
          normalized_byte_count_{normalized_byte_count},
          collation_{collation},
          owns_packed_copy_{owns_packed_copy} {}

    void reset() noexcept {
        if (packed_ != nullptr) {
            runtime_test_counters::note_view_destruction(
                owns_packed_copy_);
        }
        if (owns_packed_copy_ && packed_ != nullptr) {
            pfree(packed_);
        }
        source_ = Datum{};
        packed_ = nullptr;
        normalized_byte_count_ = 0;
        collation_ = InvalidOid;
        owns_packed_copy_ = false;
    }

    Datum source_{};
    BpChar* packed_ = nullptr;
    std::int32_t normalized_byte_count_{};
    Oid collation_ = InvalidOid;
    bool owns_packed_copy_{};
};

namespace detail {

template<class Storage, class Lane>
struct fixed_vector_lane_traits {
    using storage_type = Storage;
    static constexpr std::size_t expected_type_length = sizeof(Storage);
    static constexpr bool expected_column_is_val = true;
    static constexpr bool is_contiguous = true;
    static constexpr VectorStorageKind expected_storage_kind =
        VectorStorageKind::inline_fixed;

    [[nodiscard]] static Lane decode(
            Storage value,
            Oid) noexcept {
        return static_cast<Lane>(value);
    }
};

}  // namespace detail

template<>
struct vector_lane_traits<
        bioseq::sql::pg::character,
        bpchar_view> final {
    using storage_type = Datum;
    static constexpr std::size_t expected_type_length = sizeof(Datum);
    static constexpr bool expected_column_is_val = true;
    static constexpr bool is_contiguous = false;
    static constexpr VectorStorageKind expected_storage_kind =
        VectorStorageKind::owned_varlena_datum;

    [[nodiscard]] static bpchar_view decode(
            Datum value,
            Oid collation) {
        return bpchar_view::from_datum(value, collation);
    }

    [[nodiscard]] static bpchar_view decode_constant(
            Datum value,
            Oid collation) {
        return decode(value, collation);
    }
};

template<>
struct vector_lane_traits<
        bioseq::sql::pg::smallint,
        std::int16_t> final
    : detail::fixed_vector_lane_traits<
          std::int16_t, std::int16_t> {
    [[nodiscard]] static std::int16_t decode_constant(
            Datum value,
            Oid) noexcept {
        return DatumGetInt16(value);
    }
};

template<>
struct vector_lane_traits<
        bioseq::sql::pg::integer,
        std::int32_t> final
    : detail::fixed_vector_lane_traits<
          std::int32_t, std::int32_t> {
    [[nodiscard]] static std::int32_t decode_constant(
            Datum value,
            Oid) noexcept {
        return DatumGetInt32(value);
    }
};

template<>
struct vector_lane_traits<
        bioseq::sql::pg::bigint,
        std::int64_t> final
    : detail::fixed_vector_lane_traits<
          std::int64_t, std::int64_t> {
    [[nodiscard]] static std::int64_t decode_constant(
            Datum value,
            Oid) noexcept {
        return DatumGetInt64(value);
    }
};

template<>
struct vector_lane_traits<
        bioseq::sql::pg::date,
        DateADT> final
    : detail::fixed_vector_lane_traits<DateADT, DateADT> {
    [[nodiscard]] static DateADT decode_constant(
            Datum value,
            Oid) noexcept {
        return DatumGetDateADT(value);
    }
};

template<>
struct vector_lane_traits<
        bioseq::sql::pg::time,
        TimeADT> final
    : detail::fixed_vector_lane_traits<TimeADT, TimeADT> {
    [[nodiscard]] static TimeADT decode_constant(
            Datum value,
            Oid) noexcept {
        return DatumGetTimeADT(value);
    }
};

template<>
struct vector_lane_traits<
        bioseq::sql::pg::timestamp,
        Timestamp> final
    : detail::fixed_vector_lane_traits<Timestamp, Timestamp> {
    [[nodiscard]] static Timestamp decode_constant(
            Datum value,
            Oid) noexcept {
        return DatumGetTimestamp(value);
    }
};

template<>
struct vector_lane_traits<
        bioseq::sql::pg::timestamptz,
        TimestampTz> final
    : detail::fixed_vector_lane_traits<TimestampTz, TimestampTz> {
    [[nodiscard]] static TimestampTz decode_constant(
            Datum value,
            Oid) noexcept {
        return DatumGetTimestampTz(value);
    }
};

template<>
struct vector_lane_traits<
        bioseq::sql::pg::boolean,
        bool> final
    : detail::fixed_vector_lane_traits<bool, bool> {
    [[nodiscard]] static bool decode_constant(
            Datum value,
            Oid) noexcept {
        return DatumGetBool(value);
    }
};

template<auto SqlType, class Lane>
concept registered_vector_lane =
    requires(
        typename vector_lane_traits<SqlType, Lane>::storage_type stored,
        Datum datum,
        Oid collation) {
        typename vector_lane_traits<SqlType, Lane>::storage_type;
        {
            vector_lane_traits<SqlType, Lane>::expected_type_length
        } -> std::convertible_to<std::size_t>;
        {
            vector_lane_traits<SqlType, Lane>::expected_column_is_val
        } -> std::convertible_to<bool>;
        {
            vector_lane_traits<SqlType, Lane>::is_contiguous
        } -> std::convertible_to<bool>;
        {
            vector_lane_traits<SqlType, Lane>::decode(stored, collation)
        } -> std::same_as<Lane>;
        {
            vector_lane_traits<SqlType, Lane>::decode_constant(
                datum, collation)
        } -> std::same_as<Lane>;
    };

enum class vector_operand_kind : std::uint8_t {
    constant,
    column,
};

template<auto SqlType, class Lane>
class vector_column final {
    static_assert(registered_vector_lane<SqlType, Lane>);

public:
    using lane_type = Lane;
    static constexpr auto sql_type = SqlType;

    [[nodiscard]] std::size_t size() const noexcept {
        return column_->dimension;
    }

    [[nodiscard]] std::span<const bool> nulls() const noexcept {
        return {column_->isnull, column_->dimension};
    }

    [[nodiscard]] bool is_null(std::size_t lane_index) const {
        require_lane(lane_index);
        return column_->isnull[lane_index];
    }

    [[nodiscard]] Lane value(std::size_t lane_index) const {
        require_lane(lane_index);
        if (column_->isnull[lane_index]) {
            throw sql_boundary_conversion_error{
                "a vector NULL lane has no decoded value"};
        }
        using traits = vector_lane_traits<SqlType, Lane>;
        const auto* const values =
            reinterpret_cast<const typename traits::storage_type*>(
                column_->value);
        return traits::decode(values[lane_index], collation_);
    }

    template<class T = Lane>
    [[nodiscard]] std::span<const T> contiguous_values() const
        requires vector_lane_traits<SqlType, T>::is_contiguous {
        return {
            reinterpret_cast<const T*>(column_->value),
            column_->dimension};
    }

    [[nodiscard]] Oid collation() const noexcept {
        return collation_;
    }

private:
    friend struct detail::vector_boundary_access;
    template<auto, class>
    friend class vector_operand;

    explicit vector_column(
            VectorColumn* column,
            Oid collation) noexcept
        : column_{column}, collation_{collation} {}

    void require_lane(std::size_t lane_index) const {
        if (lane_index >= size()) {
            throw sql_boundary_conversion_error{
                "vector lane index is out of range"};
        }
    }

    VectorColumn* column_ = nullptr;
    Oid collation_ = InvalidOid;
};

/**
 * Invocation-scoped view of the NULL bitmap carried by an aggregate vector
 * argument whose SQL pseudo-type does not identify a lane representation.
 */
class vector_null_mask final {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return column_->dimension;
    }

    [[nodiscard]] std::span<const bool> nulls() const noexcept {
        return {column_->isnull, column_->dimension};
    }

    [[nodiscard]] std::size_t non_null_count() const noexcept {
        std::size_t count = 0;
        for (std::size_t lane_index = 0;
             lane_index < column_->dimension;
             ++lane_index) {
            count += column_->isnull[lane_index] ? 0u : 1u;
        }
        return count;
    }

private:
    friend struct detail::vector_boundary_access;

    explicit vector_null_mask(VectorColumn* column) noexcept
        : column_{column} {}

    VectorColumn* column_ = nullptr;
};

template<auto SqlType, class Lane>
class vector_operand final {
    static_assert(registered_vector_lane<SqlType, Lane>);

public:
    using lane_type = Lane;
    static constexpr auto sql_type = SqlType;

    [[nodiscard]] vector_operand_kind kind() const noexcept {
        return argument_->type == VECTOR_FN_ARG_CONSTANT
            ? vector_operand_kind::constant
            : vector_operand_kind::column;
    }

    [[nodiscard]] bool is_constant() const noexcept {
        return kind() == vector_operand_kind::constant;
    }

    [[nodiscard]] bool is_column() const noexcept {
        return kind() == vector_operand_kind::column;
    }

    [[nodiscard]] Lane constant() const {
        if (!is_constant()) {
            throw sql_boundary_conversion_error{
                "a vector column operand has no scalar constant"};
        }
        return vector_lane_traits<SqlType, Lane>::decode_constant(
            argument_->arg, collation_);
    }

    [[nodiscard]] vector_column<SqlType, Lane> column() const {
        if (!is_column()) {
            throw sql_boundary_conversion_error{
                "a vector constant operand has no column"};
        }
        return vector_column<SqlType, Lane>{
            reinterpret_cast<VectorColumn*>(
                DatumGetPointer(argument_->arg)),
            collation_};
    }

    [[nodiscard]] Oid collation() const noexcept {
        return collation_;
    }

private:
    friend struct detail::vector_boundary_access;

    explicit vector_operand(
            VectorFnArgument* argument,
            Oid collation) noexcept
        : argument_{argument}, collation_{collation} {}

    VectorFnArgument* argument_ = nullptr;
    Oid collation_ = InvalidOid;
};

template<auto SqlType, class Lane>
class vector_result final {
    static_assert(registered_vector_lane<SqlType, Lane>);

public:
    vector_result(const vector_result&) = delete;
    vector_result& operator=(const vector_result&) = delete;

    vector_result(vector_result&& other) noexcept
        : column_{std::exchange(other.column_, nullptr)} {}

    vector_result& operator=(vector_result&& other) noexcept {
        if (this == &other) return *this;
        reset();
        column_ = std::exchange(other.column_, nullptr);
        return *this;
    }

    ~vector_result() noexcept {
        reset();
    }

    [[nodiscard]] static vector_result allocate(
            std::size_t lane_count) {
        if (lane_count > BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE) {
            throw sql_boundary_conversion_error{
                "vector result exceeds the executor batch capacity"};
        }
        VectorColumn* const column = invoke_pg_as_cpp(
            [lane_count] {
                return BuildVectorColumn(
                    static_cast<std::int16_t>(lane_count),
                    static_cast<std::int16_t>(
                        vector_lane_traits<
                            SqlType, Lane>::expected_type_length),
                    vector_lane_traits<
                        SqlType, Lane>::expected_column_is_val,
                    nullptr,
                    vector_lane_traits<
                        SqlType, Lane>::expected_storage_kind);
            });
        column->dimension = static_cast<std::uint32_t>(lane_count);
        return vector_result{column};
    }

    [[nodiscard]] std::span<Lane> values() noexcept {
        return {
            reinterpret_cast<Lane*>(column_->value),
            column_->dimension};
    }

    [[nodiscard]] std::span<bool> nulls() noexcept {
        return {column_->isnull, column_->dimension};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return column_->dimension;
    }

private:
    friend struct detail::vector_boundary_access;

    explicit vector_result(VectorColumn* column) noexcept
        : column_{column} {}

    [[nodiscard]] Datum release_to_datum() && noexcept {
        return PointerGetDatum(std::exchange(column_, nullptr));
    }

    void reset() noexcept {
        if (column_ == nullptr) return;
        if (column_->value != nullptr) pfree(column_->value);
        pfree(column_);
        column_ = nullptr;
    }

    VectorColumn* column_ = nullptr;
};

namespace detail {

struct vector_boundary_access final {
    template<auto SqlType, class Lane>
    [[nodiscard]] static vector_column<SqlType, Lane>
    make_column(
            VectorColumn* column,
            Oid collation) noexcept {
        return vector_column<SqlType, Lane>{column, collation};
    }

    template<auto SqlType, class Lane>
    [[nodiscard]] static vector_operand<SqlType, Lane>
    make_operand(
            VectorFnArgument* argument,
            Oid collation) noexcept {
        return vector_operand<SqlType, Lane>{argument, collation};
    }

    template<auto SqlType, class Lane>
    [[nodiscard]] static Datum release_result(
            vector_result<SqlType, Lane>&& result) noexcept {
        return std::move(result).release_to_datum();
    }

    [[nodiscard]] static vector_null_mask make_null_mask(
            VectorColumn* column) noexcept {
        return vector_null_mask{column};
    }
};

template<class T>
struct vector_column_descriptor {
    static constexpr bool registered = false;
};

template<auto SqlType, class Lane>
struct vector_column_descriptor<
        vector_column<SqlType, Lane>> {
    static constexpr bool registered = true;
    static constexpr auto sql_type = SqlType;
    using lane_type = Lane;
};

template<class T>
concept registered_vector_column =
    vector_column_descriptor<T>::registered;

template<class T>
struct vector_operand_descriptor {
    static constexpr bool registered = false;
};

template<auto SqlType, class Lane>
struct vector_operand_descriptor<
        vector_operand<SqlType, Lane>> {
    static constexpr bool registered = true;
    static constexpr auto sql_type = SqlType;
    using lane_type = Lane;
};

template<class T>
concept registered_vector_operand =
    vector_operand_descriptor<T>::registered;

template<auto SqlType, class Lane>
inline void validate_vector_column(VectorColumn* column) {
    using traits = vector_lane_traits<SqlType, Lane>;
    if (column == nullptr) {
        throw sql_boundary_conversion_error{
            "vector variable operand contains a null column pointer"};
    }
    if (column->dimension > BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE) {
        throw sql_boundary_conversion_error{
            "vector column dimension exceeds the executor batch capacity"};
    }
    if (column->value == nullptr) {
        throw sql_boundary_conversion_error{
            "vector column contains a null value buffer"};
    }
    if (column->columnTypeLen != traits::expected_type_length
        || column->columnIsVal != traits::expected_column_is_val
        || column->storageKind != traits::expected_storage_kind) {
        throw sql_boundary_conversion_error{
            "vector column storage metadata disagrees with its logical "
            "SQL lane type"};
    }
}

inline void validate_vector_null_mask(VectorColumn* column) {
    if (column == nullptr) {
        throw sql_boundary_conversion_error{
            "vector aggregate argument contains a null column pointer"};
    }
    if (column->dimension > BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE) {
        throw sql_boundary_conversion_error{
            "vector aggregate column dimension exceeds the executor batch "
            "capacity"};
    }
}

}  // namespace detail

template<class T>
    requires detail::registered_vector_column<T>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable vector column argument"};
    }
    auto* const column = reinterpret_cast<VectorColumn*>(
        DatumGetPointer(call->args[zero_based_index].value));
    using descriptor = detail::vector_column_descriptor<T>;
    detail::validate_vector_column<
        descriptor::sql_type,
        typename descriptor::lane_type>(column);
    return detail::vector_boundary_access::make_column<
        descriptor::sql_type,
        typename descriptor::lane_type>(
            column, call->fncollation);
}

template<class T>
    requires detail::registered_vector_column<T>
[[nodiscard]] inline std::optional<T> sql_argument(
        FunctionCallInfo call,
        int zero_based_index,
        std::type_identity<std::optional<T>>) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return sql_argument<T>(call, zero_based_index);
}

template<class T>
    requires requires {
        typename T::value_type;
    } && detail::registered_vector_column<typename T::value_type>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    return sql_argument(
        call, zero_based_index, std::type_identity<T>{});
}

template<class T>
    requires std::same_as<T, vector_null_mask>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable vector NULL-mask argument"};
    }
    auto* const column = reinterpret_cast<VectorColumn*>(
        DatumGetPointer(call->args[zero_based_index].value));
    detail::validate_vector_null_mask(column);
    return detail::vector_boundary_access::make_null_mask(column);
}

template<class T>
    requires std::same_as<T, std::optional<vector_null_mask>>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return sql_argument<vector_null_mask>(call, zero_based_index);
}

template<class T>
    requires detail::registered_vector_operand<T>
[[nodiscard]] inline T sql_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_sql_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw sql_boundary_conversion_error{
            "SQL NULL reached a non-nullable vector argument"};
    }
    auto* const argument = reinterpret_cast<VectorFnArgument*>(
        DatumGetPointer(call->args[zero_based_index].value));
    if (argument == nullptr) {
        throw sql_boundary_conversion_error{
            "generated vector boundary received a null operand envelope"};
    }
    if (argument->type != VECTOR_FN_ARG_CONSTANT
        && argument->type != VECTOR_FN_ARG_VAR) {
        throw sql_boundary_conversion_error{
            "generated vector boundary received an invalid operand kind"};
    }

    using descriptor = detail::vector_operand_descriptor<T>;
    if (argument->type == VECTOR_FN_ARG_VAR) {
        detail::validate_vector_column<
            descriptor::sql_type,
            typename descriptor::lane_type>(
                reinterpret_cast<VectorColumn*>(
                    DatumGetPointer(argument->arg)));
    }
    return detail::vector_boundary_access::make_operand<
        descriptor::sql_type,
        typename descriptor::lane_type>(
            argument, call->fncollation);
}

template<auto SqlType, class Lane>
[[nodiscard]] inline Datum sql_result_to_datum(
        FunctionCallInfo call,
        vector_result<SqlType, Lane>&& value) noexcept {
    call->isnull = false;
    return detail::vector_boundary_access::release_result(
        std::move(value));
}

static_assert(
    registered_vector_lane<
        bioseq::sql::pg::character, bpchar_view>);
static_assert(
    registered_vector_lane<
        bioseq::sql::pg::smallint, std::int16_t>);
static_assert(
    registered_vector_lane<
        bioseq::sql::pg::integer, std::int32_t>);
static_assert(
    registered_vector_lane<
        bioseq::sql::pg::bigint, std::int64_t>);
static_assert(
    registered_vector_lane<
        bioseq::sql::pg::date, DateADT>);
static_assert(
    registered_vector_lane<
        bioseq::sql::pg::time, TimeADT>);
static_assert(
    registered_vector_lane<
        bioseq::sql::pg::timestamp, Timestamp>);
static_assert(
    registered_vector_lane<
        bioseq::sql::pg::timestamptz, TimestampTz>);
static_assert(
    registered_vector_lane<
        bioseq::sql::pg::boolean, bool>);
static_assert(!std::default_initializable<
              vector_column<
                  bioseq::sql::pg::integer, std::int32_t>>);
static_assert(!std::default_initializable<vector_null_mask>);
static_assert(!std::default_initializable<
              vector_operand<
                  bioseq::sql::pg::integer, std::int32_t>>);
static_assert(!std::default_initializable<
              vector_result<
                  bioseq::sql::pg::boolean, bool>>);
static_assert(!std::copy_constructible<
              vector_result<
                  bioseq::sql::pg::boolean, bool>>);

}  // namespace bioseq::pg_utils
