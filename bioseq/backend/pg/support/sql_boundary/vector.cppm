/**
 * @file vector.cppm
 * @brief Validate and read vector arguments passed through PostgreSQL function calls.
 */
module;

#include <bioseq/pg_compat.h>

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

export module bioseq.postgres.support.sql_boundary:vector;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.support.runtime_diagnostics;
import bioseq.postgres.vectorization.types;

export namespace bioseq::postgres::support {

/** Map a SQL type and C++ element type to the vector's stored representation. */
template<auto p_sql_type, class p_lane>
struct vector_lane_traits;

/** Borrow a typed vector column after its storage has been checked. */
template<auto p_sql_type, class p_lane>
class vector_column;

/** Read a vector argument that holds a constant or a column. */
template<auto p_sql_type, class p_lane>
class vector_operand;

/** Borrow the NULL flags from a vector argument. */
class vector_null_mask;

/** Own a result vector until it is  returned to PostgreSQL. */
template<auto p_sql_type, class p_lane>
class vector_result;

namespace detail {
struct vector_boundary_access;
}

/**
 * View a PostgreSQL character value with trailing spaces excluded.
 * Keeps any detoasted copy alive until this view is destroyed. If the
 * original storage is borrowed, the caller must keep it alive for the view.
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
        if (this == &other) {
            return *this;
        }
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
    template<auto p_sql_type, class p_lane>
    friend struct vector_lane_traits;

    [[nodiscard]] static bpchar_view from_datum(
            Datum source,
            Oid collation) {
        const Pointer raw = DatumGetPointer(source);
        using enum runtime_diagnostics::bpchar_source_form;
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
        runtime_diagnostics::record_bpchar_source_form(
            source_form,
            runtime_diagnostics::
                bpchar_source_site::view_decode);
        BpChar* const packed = invoke_pg_as_cpp(
            [source] {
                return DatumGetBpCharPP(source);
            });
        const bool owns_packed_copy =
            reinterpret_cast<Pointer>(packed) != DatumGetPointer(source);
        runtime_diagnostics::record_view_construction(
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
            runtime_diagnostics::record_view_destruction(
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

template<class p_storage, class p_lane>
struct fixed_vector_lane_traits {
    using storage_type = p_storage;
    static constexpr std::size_t expected_type_length = sizeof(p_storage);
    static constexpr bool expected_column_is_val = true;
    static constexpr bool is_contiguous = true;
    static constexpr vector_storage_kind expected_storage_kind =
        vector_storage_kind::inline_fixed;

    [[nodiscard]] static p_lane decode(
            p_storage value,
            Oid) noexcept {
        return static_cast<p_lane>(value);
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
    static constexpr vector_storage_kind expected_storage_kind =
        vector_storage_kind::owned_varlena_datum;

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

template<auto p_sql_type, class p_lane>
concept registered_vector_lane =
    requires(
        typename vector_lane_traits<p_sql_type, p_lane>::storage_type stored,
        Datum datum,
        Oid collation) {
        typename vector_lane_traits<p_sql_type, p_lane>::storage_type;
        {
            vector_lane_traits<p_sql_type, p_lane>::expected_type_length
        } -> std::convertible_to<std::size_t>;
        {
            vector_lane_traits<p_sql_type, p_lane>::expected_column_is_val
        } -> std::convertible_to<bool>;
        {
            vector_lane_traits<p_sql_type, p_lane>::is_contiguous
        } -> std::convertible_to<bool>;
        {
            vector_lane_traits<p_sql_type, p_lane>::decode(stored, collation)
        } -> std::same_as<p_lane>;
        {
            vector_lane_traits<p_sql_type, p_lane>::decode_constant(
                datum, collation)
        } -> std::same_as<p_lane>;
    };

/** Whether a vector argument holds one constant or a column of values. */
enum class vector_operand_kind : std::uint8_t {
    constant,
    column,
};

/** Read a validated vector column without owning its storage. */
template<auto p_sql_type, class p_lane>
class vector_column final {
    static_assert(registered_vector_lane<p_sql_type, p_lane>);

public:
    using lane_type = p_lane;
    static constexpr auto sql_type = p_sql_type;

    [[nodiscard]] std::size_t size() const noexcept {
        return column_->lane_count;
    }

    [[nodiscard]] std::span<const bool> nulls() const noexcept {
        return {column_->is_null.data(), column_->lane_count};
    }

    [[nodiscard]] const bool* null_data() const noexcept {
        return column_->is_null.data();
    }

    [[nodiscard]] bool is_null(std::size_t lane_index) const {
        require_lane(lane_index);
        return column_->is_null[lane_index];
    }

    [[nodiscard]] p_lane value(std::size_t lane_index) const {
        require_lane(lane_index);
        if (column_->is_null[lane_index]) {
            throw scalar_conversion_error{
                "a vector NULL lane has no decoded value"};
        }
        using traits = vector_lane_traits<p_sql_type, p_lane>;
        const auto* const values =
            reinterpret_cast<const typename traits::storage_type*>(
                column_->data);
        return traits::decode(values[lane_index], collation_);
    }

    /* Return a pointer for the comparison kernel's pointer-and-count interface.
     * Keep this signature for Clang builds with reduced  module interfaces, where
     * returning a spav by value changes how the value crosses the module boundary. */
    template<class p_contiguous_lane = p_lane>
    [[nodiscard]] const p_contiguous_lane* contiguous_data() const noexcept
        requires vector_lane_traits<
            p_sql_type, p_contiguous_lane>::is_contiguous {
        return reinterpret_cast<const p_contiguous_lane*>(column_->data);
    }

    [[nodiscard]] Oid collation() const noexcept {
        return collation_;
    }

private:
    friend struct detail::vector_boundary_access;
    template<auto p_operand_sql_type, class p_operand_lane>
    friend class vector_operand;

    explicit vector_column(
            ::vector_column_storage* column,
            Oid collation) noexcept
        : column_{column}, collation_{collation} {}

    void require_lane(std::size_t lane_index) const {
        if (lane_index >= size()) {
            throw scalar_conversion_error{
                "vector lane index is out of range"};
        }
    }

    ::vector_column_storage* column_ = nullptr;
    Oid collation_ = InvalidOid;
};

/**
 * Borrow an aggregate argument's NULL flags for this, function call.
 * This allows counting non-null rows without knowing the column's element type.
 */
class vector_null_mask final {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return column_->lane_count;
    }

    [[nodiscard]] std::span<const bool> nulls() const noexcept {
        return {column_->is_null.data(), column_->lane_count};
    }

    [[nodiscard]] std::size_t non_null_count() const noexcept {
        std::size_t count = 0;
        for (std::size_t lane_index = 0;
             lane_index < column_->lane_count;
             ++lane_index) {
            count += column_->is_null[lane_index] ? 0u : 1u;
        }
        return count;
    }

private:
    friend struct detail::vector_boundary_access;

    explicit vector_null_mask(::vector_column_storage* column) noexcept
        : column_{column} {}

    ::vector_column_storage* column_ = nullptr;
};

/** Read a validated vector argument as a constant or a typed column */
template<auto p_sql_type, class p_lane>
class vector_operand final {
    static_assert(registered_vector_lane<p_sql_type, p_lane>);

public:
    using lane_type = p_lane;
    static constexpr auto sql_type = p_sql_type;

    [[nodiscard]] vector_operand_kind kind() const noexcept {
        return argument_->kind == vector_operand_storage_kind::constant
            ? vector_operand_kind::constant
            : vector_operand_kind::column;
    }

    [[nodiscard]] bool is_constant() const noexcept {
        return kind() == vector_operand_kind::constant;
    }

    [[nodiscard]] bool is_column() const noexcept {
        return kind() == vector_operand_kind::column;
    }

    [[nodiscard]] p_lane constant() const {
        if (!is_constant()) {
            throw scalar_conversion_error{
                "a vector column operand has no scalar constant"};
        }
        return vector_lane_traits<p_sql_type, p_lane>::decode_constant(
            argument_->datum, collation_);
    }

    [[nodiscard]] vector_column<p_sql_type, p_lane> column() const {
        if (!is_column()) {
            throw scalar_conversion_error{
                "a vector constant operand has no column"};
        }
        return vector_column<p_sql_type, p_lane>{
            reinterpret_cast<::vector_column_storage*>(
                DatumGetPointer(argument_->datum)),
            collation_};
    }

    [[nodiscard]] Oid collation() const noexcept {
        return collation_;
    }

private:
    friend struct detail::vector_boundary_access;

    explicit vector_operand(
            ::vector_operand_storage* argument,
            Oid collation) noexcept
        : argument_{argument}, collation_{collation} {}

    ::vector_operand_storage* argument_ = nullptr;
    Oid collation_ = InvalidOid;
};

/** Own a newly allocated result vector until PostgreSQL takes it as a Datum. */
template<auto p_sql_type, class p_lane>
class vector_result final {
    static_assert(registered_vector_lane<p_sql_type, p_lane>);

public:
    vector_result(const vector_result&) = delete;
    vector_result& operator=(const vector_result&) = delete;

    vector_result(vector_result&& other) noexcept
        : column_{std::exchange(other.column_, nullptr)} {}

    vector_result& operator=(vector_result&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        column_ = std::exchange(other.column_, nullptr);
        return *this;
    }

    ~vector_result() noexcept {
        reset();
    }

    [[nodiscard]] static vector_result allocate(
            std::size_t lane_count) {
        if (lane_count > vector_column_capacity) {
            throw scalar_conversion_error{
                "vector result exceeds the executor batch capacity"};
        }
        ::vector_column_storage* const column = invoke_pg_as_cpp(
            [lane_count] {
                return allocate_vector_column(
                    static_cast<std::int16_t>(
                        vector_lane_traits<
                            p_sql_type, p_lane>::expected_type_length),
                    vector_lane_traits<
                        p_sql_type, p_lane>::expected_column_is_val,
                    nullptr,
                    vector_lane_traits<
                        p_sql_type, p_lane>::expected_storage_kind);
            });
        column->lane_count = static_cast<std::uint32_t>(lane_count);
        return vector_result{column};
    }

    [[nodiscard]] std::span<p_lane> values() noexcept {
        return {
            reinterpret_cast<p_lane*>(column_->data),
            column_->lane_count};
    }

    [[nodiscard]] p_lane* value_data() noexcept {
        return reinterpret_cast<p_lane*>(column_->data);
    }

    [[nodiscard]] std::span<bool> nulls() noexcept {
        return {column_->is_null.data(), column_->lane_count};
    }

    [[nodiscard]] bool* null_data() noexcept {
        return column_->is_null.data();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return column_->lane_count;
    }

private:
    friend struct detail::vector_boundary_access;

    explicit vector_result(::vector_column_storage* column) noexcept
        : column_{column} {}

    [[nodiscard]] Datum release_to_datum() && noexcept {
        return PointerGetDatum(std::exchange(column_, nullptr));
    }

    void reset() noexcept {
        if (column_ == nullptr) {
            return;
        }
        if (column_->data != nullptr) {
            pfree(column_->data);
        }
        pfree(column_);
        column_ = nullptr;
    }

    ::vector_column_storage* column_ = nullptr;
};

namespace detail {

struct vector_boundary_access final {
    template<auto p_sql_type, class p_lane>
    [[nodiscard]] static vector_column<p_sql_type, p_lane>
    make_column(
            ::vector_column_storage* column,
            Oid collation) noexcept {
        return vector_column<p_sql_type, p_lane>{column, collation};
    }

    template<auto p_sql_type, class p_lane>
    [[nodiscard]] static vector_operand<p_sql_type, p_lane>
    make_operand(
            ::vector_operand_storage* argument,
            Oid collation) noexcept {
        return vector_operand<p_sql_type, p_lane>{argument, collation};
    }

    template<auto p_sql_type, class p_lane>
    [[nodiscard]] static Datum release_result(
            vector_result<p_sql_type, p_lane>&& result) noexcept {
        return std::move(result).release_to_datum();
    }

    [[nodiscard]] static vector_null_mask make_null_mask(
            ::vector_column_storage* column) noexcept {
        return vector_null_mask{column};
    }
};

template<class p_column>
struct vector_column_descriptor {
    static constexpr bool registered = false;
};

template<auto p_sql_type, class p_lane>
struct vector_column_descriptor<
        vector_column<p_sql_type, p_lane>> {
    static constexpr bool registered = true;
    static constexpr auto sql_type = p_sql_type;
    using lane_type = p_lane;
};

template<class p_column>
concept registered_vector_column =
    vector_column_descriptor<p_column>::registered;

template<class p_operand>
struct vector_operand_descriptor {
    static constexpr bool registered = false;
};

template<auto p_sql_type, class p_lane>
struct vector_operand_descriptor<
        vector_operand<p_sql_type, p_lane>> {
    static constexpr bool registered = true;
    static constexpr auto sql_type = p_sql_type;
    using lane_type = p_lane;
};

template<class p_operand>
concept registered_vector_operand =
    vector_operand_descriptor<p_operand>::registered;

template<auto p_sql_type, class p_lane>
inline void validate_vector_column(::vector_column_storage* column) {
    using traits = vector_lane_traits<p_sql_type, p_lane>;
    if (column == nullptr) {
        throw scalar_conversion_error{
            "vector variable operand contains a null column pointer"};
    }
    if (column->lane_count > vector_column_capacity) {
        throw scalar_conversion_error{
            "vector column row count exceeds the executor batch capacity"};
    }
    if (column->data == nullptr) {
        throw scalar_conversion_error{
            "vector column contains a null value buffer"};
    }
    if (column->element_width != traits::expected_type_length
        || column->stored_by_value != traits::expected_column_is_val
        || column->storage_kind != traits::expected_storage_kind) {
        throw scalar_conversion_error{
            "vector column storage metadata disagrees with its logical "
            "SQL lane type"};
    }
}

inline void validate_vector_null_mask(::vector_column_storage* column) {
    if (column == nullptr) {
        throw scalar_conversion_error{
            "vector aggregate argument contains a null column pointer"};
    }
    if (column->lane_count > vector_column_capacity) {
        throw scalar_conversion_error{
            "vector aggregate column row count exceeds the executor batch "
            "capacity"};
    }
}

}  // namespace detail

template<class p_argument>
    requires detail::registered_vector_column<p_argument>
[[nodiscard]] inline p_argument read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a non-nullable vector column argument"};
    }
    auto* const column = reinterpret_cast<::vector_column_storage*>(
        DatumGetPointer(call->args[zero_based_index].value));
    using descriptor = detail::vector_column_descriptor<p_argument>;
    detail::validate_vector_column<
        descriptor::sql_type,
        typename descriptor::lane_type>(column);
    return detail::vector_boundary_access::make_column<
        descriptor::sql_type,
        typename descriptor::lane_type>(
            column, call->fncollation);
}

template<class p_argument>
    requires detail::registered_vector_column<p_argument>
[[nodiscard]] inline std::optional<p_argument> read_argument(
        FunctionCallInfo call,
        int zero_based_index,
        std::type_identity<std::optional<p_argument>>) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return read_argument<p_argument>(call, zero_based_index);
}

template<class p_argument>
    requires requires {
        typename p_argument::value_type;
    } && detail::registered_vector_column<typename p_argument::value_type>
[[nodiscard]] inline p_argument read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    return read_argument(
        call, zero_based_index, std::type_identity<p_argument>{});
}

template<class p_argument>
    requires std::same_as<p_argument, vector_null_mask>
[[nodiscard]] inline p_argument read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a non-nullable vector NULL-mask argument"};
    }
    auto* const column = reinterpret_cast<::vector_column_storage*>(
        DatumGetPointer(call->args[zero_based_index].value));
    detail::validate_vector_null_mask(column);
    return detail::vector_boundary_access::make_null_mask(column);
}

template<class p_argument>
    requires std::same_as<
        p_argument, std::optional<vector_null_mask>>
[[nodiscard]] inline p_argument read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        return std::nullopt;
    }
    return read_argument<vector_null_mask>(call, zero_based_index);
}

template<class p_argument>
    requires detail::registered_vector_operand<p_argument>
[[nodiscard]] inline p_argument read_argument(
        FunctionCallInfo call,
        int zero_based_index) {
    detail::require_argument(call, zero_based_index);
    if (call->args[zero_based_index].isnull) {
        throw scalar_conversion_error{
            "SQL NULL reached a non-nullable vector argument"};
    }
    auto* const argument = reinterpret_cast<::vector_operand_storage*>(
        DatumGetPointer(call->args[zero_based_index].value));
    if (argument == nullptr) {
        throw scalar_conversion_error{
            "generated vector boundary received a null operand envelope"};
    }
    if (argument->kind != vector_operand_storage_kind::constant
        && argument->kind != vector_operand_storage_kind::column) {
        throw scalar_conversion_error{
            "generated vector boundary received an invalid operand kind"};
    }

    using descriptor = detail::vector_operand_descriptor<p_argument>;
    if (argument->kind == vector_operand_storage_kind::column) {
        detail::validate_vector_column<
            descriptor::sql_type,
            typename descriptor::lane_type>(
                reinterpret_cast<::vector_column_storage*>(
                    DatumGetPointer(argument->datum)));
    }
    return detail::vector_boundary_access::make_operand<
        descriptor::sql_type,
        typename descriptor::lane_type>(
            argument, call->fncollation);
}

template<auto p_sql_type, class p_lane>
[[nodiscard]] inline Datum encode_result(
        FunctionCallInfo call,
        vector_result<p_sql_type, p_lane>&& value) noexcept {
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

}  // namespace bioseq::postgres::support
