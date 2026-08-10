/*-------------------------------------------------------------------------
 *
 * support/sql_boundary/row_codec.cppm
 *
 * Generic ownership and validation primitives used by generated SQL row
 * codecs.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "utils/builtins.h"
#include "utils/memutils.h"
}

#include <array>
#include <concepts>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

export module bioseq.postgres.support.sql_boundary:row_codec;

export import bioseq.postgres.support.datums;
import bioseq.postgres.support;

export namespace bioseq::postgres::support {

class row_codec_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class scan_comparison {
    equal,
    less,
    less_equal,
    greater,
    greater_equal,
};

struct row_descriptor_field final {
    AttrNumber attribute_number;
    std::string_view name;
    Oid type_oid;
    bool nullable;
};

template<std::size_t p_field_count>
void validate_row_descriptor(
        TupleDesc tuple_descriptor,
        std::string_view row_name,
        const std::array<row_descriptor_field, p_field_count>& expected) {
    if (tuple_descriptor == nullptr
        || tuple_descriptor->natts != static_cast<int>(p_field_count)) {
        throw row_codec_error{
            std::string{row_name}
            + " tuple descriptor has the wrong arity"};
    }

    for (std::size_t index = 0; index < p_field_count; ++index) {
        const Form_pg_attribute attribute =
            TupleDescAttr(tuple_descriptor, static_cast<int>(index));
        if (attribute->attnum != expected[index].attribute_number
            || attribute->attisdropped
            || attribute->atttypid != expected[index].type_oid
            || attribute->attnotnull != !expected[index].nullable
            || std::string_view{NameStr(attribute->attname)}
                != expected[index].name) {
            throw row_codec_error{
                std::string{row_name}
                + " tuple descriptor mismatch at attribute "
                + std::to_string(index + 1)};
        }
    }
}

inline void require_row_result_context(
        MemoryContext result_context,
        std::string_view row_name) {
    if (result_context == nullptr) {
        throw row_codec_error{
            std::string{row_name}
            + " deformation result context is NULL"};
    }
}

inline void require_row_attribute_not_null(
        bool is_null,
        AttrNumber attribute_number,
        std::string_view row_name) {
    if (is_null) {
        throw row_codec_error{
            std::string{row_name}
            + " required attribute "
            + std::to_string(attribute_number)
            + " is NULL"};
    }
}

/**
 * Copy one non-NULL by-reference Datum into an explicit PostgreSQL context.
 *
 * The generator supplies this policy from the field's closed C++/SQL type
 * mapping.  Keeping typlen/byval in generated compile-time data avoids a
 * catalog/OID lookup on every row encode, deform, or update.
 */
struct by_reference_datum_copy_policy final {
    int16 type_length;
    bool type_by_value;
};

/**
 * Encode an owning C++ text value directly into its PostgreSQL owner context.
 *
 * The input bytes remain owned by the caller.  Only raw pointer, length, and
 * MemoryContext values cross the PostgreSQL ERROR bridge.  On success there is
 * exactly one allocation, already owned by result_context; on ERROR the bridge
 * restores the caller's memory context before throwing postgres_error.
 */
[[nodiscard]] inline Datum encode_text_to_memory_context(
        std::string_view value,
        MemoryContext result_context) {
    require_row_result_context(result_context, "PostgreSQL text");
    if (value.size()
        > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        throw row_codec_error{
            "PostgreSQL text exceeds the generated row-codec length limit"};
    }

    const char* const bytes = value.data();
    const int byte_count = static_cast<int>(value.size());
    const auto encode =
        [bytes, byte_count, result_context]() -> Datum {
            const MemoryContext previous_context =
                MemoryContextSwitchTo(result_context);
            text* const encoded =
                cstring_to_text_with_len(bytes, byte_count);
            MemoryContextSwitchTo(previous_context);
            return PointerGetDatum(encoded);
        };
    static_assert(std::is_trivially_destructible_v<decltype(encode)>);
    return invoke_pg_as_cpp(encode);
}

[[nodiscard]] Datum copy_pg_datum_to_memory_context(
    Datum value,
    by_reference_datum_copy_policy policy,
    MemoryContext result_context);

template<class p_value>
    requires std::is_object_v<p_value>
[[nodiscard]] p_value* copy_pg_datum_to_memory_context(
        pg_datum value,
        by_reference_datum_copy_policy policy,
        MemoryContext result_context) {
    return reinterpret_cast<p_value*>(
        DatumGetPointer(
            copy_pg_datum_to_memory_context(
                value.raw(),
                policy,
                result_context)));
}

[[nodiscard]] constexpr StrategyNumber
row_scan_strategy(scan_comparison comparison) {
    switch (comparison) {
        case scan_comparison::equal:
            return BTEqualStrategyNumber;
        case scan_comparison::less:
            return BTLessStrategyNumber;
        case scan_comparison::less_equal:
            return BTLessEqualStrategyNumber;
        case scan_comparison::greater:
            return BTGreaterStrategyNumber;
        case scan_comparison::greater_equal:
            return BTGreaterEqualStrategyNumber;
    }
    return InvalidStrategy;
}

struct encoded_scan_value final {
    StrategyNumber strategy;
    RegProcedure procedure;
    pg_datum datum;
};

[[nodiscard]] inline ScanKeyData make_row_scan_key(
        AttrNumber attribute_number,
        encoded_scan_value encoded) {
    ScanKeyData result{};
    invoke_pg_as_cpp(
        [&result, attribute_number, encoded] {
            ScanKeyInit(
                &result,
                attribute_number,
                encoded.strategy,
                encoded.procedure,
                encoded.datum.raw());
        });
    return result;
}

template<std::size_t p_field_count>
class encoded_row_data final {
public:
    static constexpr std::size_t field_count = p_field_count;

    encoded_row_data() noexcept = default;
    encoded_row_data(const encoded_row_data&) = delete;
    encoded_row_data& operator=(const encoded_row_data&) = delete;
    encoded_row_data(encoded_row_data&&) noexcept = default;
    encoded_row_data& operator=(encoded_row_data&&) noexcept = default;

    template<class p_value>
        requires is_convertible_to_datum<p_value>
    encoded_row_data& set(AttrNumber attribute_number, p_value value) noexcept {
        storage_.set(attribute_number, value);
        return *this;
    }

    encoded_row_data& set_raw(
            AttrNumber attribute_number,
            pg_datum value) noexcept {
        storage_.set_raw(attribute_number, value);
        return *this;
    }

    encoded_row_data& set_null(
            AttrNumber attribute_number) noexcept {
        storage_.set_null(attribute_number);
        return *this;
    }

    [[nodiscard]] Datum* values_data() noexcept {
        return storage_.values_data();
    }

    [[nodiscard]] const Datum* values_data() const noexcept {
        return storage_.values_data();
    }

    [[nodiscard]] bool* nulls_data() noexcept {
        return storage_.nulls_data();
    }

    [[nodiscard]] const bool* nulls_data() const noexcept {
        return storage_.nulls_data();
    }

    template<std::size_t p_expected_field_count>
        requires (p_expected_field_count == p_field_count)
    [[nodiscard]] HeapTuple form_tuple(
            TupleDesc tuple_descriptor,
            std::string_view row_name,
            const std::array<
                row_descriptor_field,
                p_expected_field_count>& expected) {
        validate_row_descriptor(
            tuple_descriptor,
            row_name,
            expected);
        return invoke_pg_as_cpp(
            [this, tuple_descriptor] {
                return storage_.form_tuple(tuple_descriptor);
            });
    }

    template<std::size_t p_expected_field_count>
        requires (p_expected_field_count == p_field_count)
    void put_values(
            Tuplestorestate* tuplestore,
            TupleDesc tuple_descriptor,
            std::string_view row_name,
            const std::array<
                row_descriptor_field,
                p_expected_field_count>& expected) {
        validate_row_descriptor(
            tuple_descriptor,
            row_name,
            expected);
        invoke_pg_as_cpp(
            [this, tuplestore, tuple_descriptor] {
                storage_.put_values(
                    tuplestore,
                    tuple_descriptor);
            });
    }

private:
    pg_datum_list<p_field_count> storage_{};
};

template<std::size_t p_field_count>
class deformed_row_data final {
public:
    static constexpr std::size_t field_count = p_field_count;

    deformed_row_data(const deformed_row_data&) = delete;
    deformed_row_data& operator=(const deformed_row_data&) = delete;
    deformed_row_data(deformed_row_data&&) noexcept = default;
    deformed_row_data& operator=(deformed_row_data&&) noexcept = default;

    template<std::size_t p_expected_field_count>
        requires (p_expected_field_count == p_field_count)
    [[nodiscard]] static deformed_row_data deform(
            HeapTuple tuple,
            TupleDesc tuple_descriptor,
            MemoryContext result_context,
            std::string_view row_name,
            const std::array<
                row_descriptor_field,
                p_expected_field_count>& expected) {
        require_row_result_context(result_context, row_name);
        validate_row_descriptor(
            tuple_descriptor,
            row_name,
            expected);

        deformed_row_data result{result_context};
        invoke_pg_as_cpp(
            [&result, tuple, tuple_descriptor] {
                result.storage_.deform(
                    tuple,
                    tuple_descriptor);
            });
        return result;
    }

    [[nodiscard]] pg_datum at(
            AttrNumber attribute_number) const noexcept {
        return storage_.at(attribute_number);
    }

    [[nodiscard]] bool is_null(
            AttrNumber attribute_number) const noexcept {
        return storage_.is_null(attribute_number);
    }

    [[nodiscard]] MemoryContext result_context() const noexcept {
        return result_context_;
    }

private:
    explicit deformed_row_data(
            MemoryContext result_context) noexcept
        : result_context_{result_context} {}

    pg_datum_list<p_field_count> storage_{};
    MemoryContext result_context_;
};

template<std::size_t p_field_count>
class row_update_data final {
public:
    static constexpr std::size_t field_count = p_field_count;

    row_update_data() noexcept = default;
    row_update_data(const row_update_data&) = delete;
    row_update_data& operator=(const row_update_data&) = delete;
    row_update_data(row_update_data&&) noexcept = default;
    row_update_data& operator=(row_update_data&&) noexcept = default;

    template<class p_value>
        requires is_convertible_to_datum<p_value>
    row_update_data& set(
            AttrNumber attribute_number,
            p_value value) noexcept {
        storage_.set(attribute_number, value);
        return *this;
    }

    row_update_data& set_raw(
            AttrNumber attribute_number,
            pg_datum value) noexcept {
        storage_.set_raw(attribute_number, value);
        return *this;
    }

    row_update_data& set_null(
            AttrNumber attribute_number) noexcept {
        storage_.set_null(attribute_number);
        return *this;
    }

    template<std::size_t p_expected_field_count>
        requires (p_expected_field_count == p_field_count)
    [[nodiscard]] HeapTuple modify(
            HeapTuple old_tuple,
            TupleDesc tuple_descriptor,
            std::string_view row_name,
            const std::array<
                row_descriptor_field,
                p_expected_field_count>& expected) {
        validate_row_descriptor(
            tuple_descriptor,
            row_name,
            expected);
        return invoke_pg_as_cpp(
            [this, old_tuple, tuple_descriptor] {
                return storage_.modify(
                    old_tuple,
                    tuple_descriptor);
            });
    }

private:
    pg_datum_update<p_field_count> storage_{};
};

}  // namespace bioseq::postgres::support

namespace bioseq::postgres::support {
namespace {

struct datum_copy_state {
    Datum value = static_cast<Datum>(0);
    Datum result = static_cast<Datum>(0);
    int16 type_length = 0;
    MemoryContext result_context = nullptr;
    MemoryContext previous_context = nullptr;
    ResourceOwner previous_owner = nullptr;
    ErrorData* error = nullptr;
};

}  // namespace

Datum copy_pg_datum_to_memory_context(
        Datum value,
        by_reference_datum_copy_policy policy,
        MemoryContext result_context) {
    require_row_result_context(result_context, "PostgreSQL Datum");
    if (policy.type_by_value) {
        throw row_codec_error{
            "PostgreSQL Datum context copy requires a by-reference type"};
    }
    if (policy.type_length != -1 && policy.type_length <= 0) {
        throw row_codec_error{
            "PostgreSQL Datum context copy requires a positive fixed "
            "length or -1 varlena length"};
    }

    auto* state = new datum_copy_state{
        .value = value,
        .type_length = policy.type_length,
        .result_context = result_context,
        .previous_context = CurrentMemoryContext,
        .previous_owner = CurrentResourceOwner,
    };

    PG_TRY();
    {
        MemoryContextSwitchTo(state->result_context);
        if (state->type_length == -1) {
            state->result = PointerGetDatum(
                PG_DETOAST_DATUM_COPY(state->value));
        } else {
            state->result = datumCopy(
                state->value,
                false,
                state->type_length);
        }
        MemoryContextSwitchTo(state->previous_context);
    }
    PG_CATCH();
    {
        CurrentResourceOwner = state->previous_owner;
        MemoryContext error_owner =
            TopTransactionContext != nullptr
            ? TopTransactionContext
            : TopMemoryContext;
        MemoryContextSwitchTo(error_owner);
        state->error = CopyErrorData();
        MemoryContextSwitchTo(state->previous_context);
        FlushErrorState();
    }
    PG_END_TRY();

    CurrentResourceOwner = state->previous_owner;
    MemoryContextSwitchTo(state->previous_context);
    if (state->error != nullptr) {
        ErrorData* error = state->error;
        delete state;
        throw postgres_error{error};
    }
    const Datum result = state->result;
    delete state;
    return result;
}

}  // namespace bioseq::postgres::support

namespace bioseq::postgres::support::detail {

template<class p_value>
concept publicly_default_constructible =
    requires {
        p_value{};
    };

using encoded_probe = encoded_row_data<2>;
using deformed_probe = deformed_row_data<2>;
using update_probe = row_update_data<2>;
using datum_copy_signature =
    Datum (*)(
        Datum,
        by_reference_datum_copy_policy,
        MemoryContext);

[[maybe_unused]] inline constexpr datum_copy_signature
    datum_copy_probe =
        static_cast<datum_copy_signature>(
            &copy_pg_datum_to_memory_context);

static_assert(std::is_base_of_v<
              std::runtime_error,
              row_codec_error>);
static_assert(std::is_aggregate_v<row_descriptor_field>);
static_assert(std::is_aggregate_v<by_reference_datum_copy_policy>);
static_assert(std::is_trivially_copyable_v<
              by_reference_datum_copy_policy>);
static_assert(std::is_trivially_copyable_v<encoded_scan_value>);
static_assert(row_scan_strategy(scan_comparison::equal)
              == BTEqualStrategyNumber);
static_assert(row_scan_strategy(scan_comparison::less)
              == BTLessStrategyNumber);
static_assert(row_scan_strategy(scan_comparison::less_equal)
              == BTLessEqualStrategyNumber);
static_assert(row_scan_strategy(scan_comparison::greater)
              == BTGreaterStrategyNumber);
static_assert(row_scan_strategy(scan_comparison::greater_equal)
              == BTGreaterEqualStrategyNumber);
static_assert(!std::is_copy_constructible_v<encoded_probe>);
static_assert(!std::is_copy_assignable_v<encoded_probe>);
static_assert(std::is_nothrow_move_constructible_v<encoded_probe>);
static_assert(std::is_nothrow_move_assignable_v<encoded_probe>);
static_assert(!publicly_default_constructible<deformed_probe>);
static_assert(!std::is_copy_constructible_v<deformed_probe>);
static_assert(!std::is_copy_assignable_v<deformed_probe>);
static_assert(std::is_nothrow_move_constructible_v<deformed_probe>);
static_assert(std::is_nothrow_move_assignable_v<deformed_probe>);
static_assert(!std::is_copy_constructible_v<update_probe>);
static_assert(!std::is_copy_assignable_v<update_probe>);
static_assert(std::is_nothrow_move_constructible_v<update_probe>);
static_assert(std::is_nothrow_move_assignable_v<update_probe>);
static_assert(noexcept(
    std::declval<encoded_probe&>().values_data()));
static_assert(noexcept(
    std::declval<const encoded_probe&>().values_data()));
static_assert(noexcept(
    std::declval<encoded_probe&>().nulls_data()));
static_assert(noexcept(
    std::declval<const encoded_probe&>().nulls_data()));
static_assert(std::is_same_v<
              decltype(std::declval<encoded_probe&>().values_data()),
              Datum*>);
static_assert(std::is_same_v<
              decltype(std::declval<encoded_probe&>().nulls_data()),
              bool*>);
static_assert(noexcept(
    std::declval<const deformed_probe&>().result_context()));
static_assert(datum_copy_probe != nullptr);

}  // namespace bioseq::postgres::support::detail
