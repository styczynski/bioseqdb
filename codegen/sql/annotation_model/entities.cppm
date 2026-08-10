module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

export module bioseq.codegen.sql.annotation_model:entities;

export namespace bioseq::sql {

template<std::size_t p_capacity>
struct fixed_text {
    std::array<char, p_capacity> bytes{};
    std::size_t size{};

    consteval fixed_text() = default;

    template<std::size_t p_size>
    consteval fixed_text(const char (&value)[p_size])
        : size{p_size - 1} {
        if (p_size == 1 || p_size > p_capacity) {
            throw "SQL annotation text is empty or too long";
        }
        for (std::size_t index = 0; index < p_size; ++index) {
            bytes[index] = value[index];
        }
    }

    [[nodiscard]] consteval bool operator==(
        const fixed_text&) const = default;
};

using identifier = fixed_text<64>;
using documentation_text = fixed_text<1024>;

struct checked_sql {
    fixed_text<8192> text{};

    consteval checked_sql() = default;

    template<std::size_t p_size>
    consteval checked_sql(const char (&value)[p_size])
        : text{value} {
    }

    [[nodiscard]] consteval bool operator==(const checked_sql&) const = default;
};

/// A compiler-checked direct declaration reference used by options whose
/// semantic target is recovered from the annotated source expression.
///
/// The value intentionally carries no authored SQL spelling: collection
/// resolves the referenced C++ declaration and derives its SQL identity.
struct declaration_reference {
    bool present{};

    consteval declaration_reference() = default;

    template<class p_declaration>
    consteval declaration_reference(p_declaration&&) noexcept
        : present{true} {
    }
};

/// Type-erased compile-time holder for a heterogeneous `entities(...)` result.
/// The collector retains and resolves every declaration token from the source
/// expression; this object exists only to make the annotation aggregate
/// compiler-checkable without introducing a string/name registry.
struct entity_reference_list {
    bool present{};

    consteval entity_reference_list() = default;

    template<class p_entities>
    consteval entity_reference_list(const p_entities&) noexcept
        : present{true} {
    }
};

struct schema_entity final {
};

template<class p_setting, class p_value>
struct session_setting_action final {
    p_setting const* setting{};
    p_value value{};
};

template<class p_setting, class p_value>
[[nodiscard]] consteval session_setting_action<p_setting, p_value>
set_after_guard(p_setting const& setting, p_value value) {
    return {.setting = &setting, .value = value};
}

enum class schema_binding : std::uint8_t {
    extension,
    fixed,
    pg_catalog,
    enclosing,
};

struct schema_ref {
    schema_binding binding{schema_binding::extension};
    identifier name{"extension"};

    [[nodiscard]] consteval bool operator==(const schema_ref&) const = default;
};

inline constexpr schema_ref extension_schema{};
inline constexpr schema_ref pg_catalog{
    .binding = schema_binding::pg_catalog,
    .name = "pg_catalog",
};

template<identifier p_name>
inline constexpr schema_ref fixed_schema{
    .binding = schema_binding::fixed,
    .name = p_name,
};

enum class pg_type_id : std::uint16_t {
    infer,
    void_,
    boolean,
    smallint,
    integer,
    bigint,
    real,
    double_precision,
    numeric,
    text,
    varchar,
    character,
    name,
    bytea,
    date,
    time,
    timestamp,
    timestamptz,
    interval,
    oid,
    regclass,
    cstring,
    internal,
    record,
    table_am_handler,
    tid,
    xid8,
    jsonb,
    anyelement,
    quoted_any,
    custom,
};

struct pg_type {
    pg_type_id id{pg_type_id::infer};
    identifier custom_name{"infer"};

    [[nodiscard]] consteval bool operator==(const pg_type&) const = default;
};

namespace pg {
inline constexpr pg_type infer{};
inline constexpr pg_type void_{.id = pg_type_id::void_, .custom_name = "void"};
inline constexpr pg_type boolean{
    .id = pg_type_id::boolean, .custom_name = "boolean"};
inline constexpr pg_type smallint{
    .id = pg_type_id::smallint, .custom_name = "smallint"};
inline constexpr pg_type integer{
    .id = pg_type_id::integer, .custom_name = "integer"};
inline constexpr pg_type bigint{
    .id = pg_type_id::bigint, .custom_name = "bigint"};
inline constexpr pg_type real{.id = pg_type_id::real, .custom_name = "real"};
inline constexpr pg_type double_precision{
    .id = pg_type_id::double_precision,
    .custom_name = "double_precision",
};
inline constexpr pg_type numeric{
    .id = pg_type_id::numeric, .custom_name = "numeric"};
inline constexpr pg_type text{.id = pg_type_id::text, .custom_name = "text"};
inline constexpr pg_type varchar{
    .id = pg_type_id::varchar, .custom_name = "varchar"};
inline constexpr pg_type character{
    .id = pg_type_id::character, .custom_name = "character"};
inline constexpr pg_type name{.id = pg_type_id::name, .custom_name = "name"};
inline constexpr pg_type bytea{.id = pg_type_id::bytea, .custom_name = "bytea"};
inline constexpr pg_type date{.id = pg_type_id::date, .custom_name = "date"};
inline constexpr pg_type time{.id = pg_type_id::time, .custom_name = "time"};
inline constexpr pg_type timestamp{
    .id = pg_type_id::timestamp, .custom_name = "timestamp"};
inline constexpr pg_type timestamptz{
    .id = pg_type_id::timestamptz, .custom_name = "timestamptz"};
inline constexpr pg_type interval{
    .id = pg_type_id::interval, .custom_name = "interval"};
inline constexpr pg_type oid{.id = pg_type_id::oid, .custom_name = "oid"};
inline constexpr pg_type regclass{
    .id = pg_type_id::regclass, .custom_name = "regclass"};
inline constexpr pg_type cstring{
    .id = pg_type_id::cstring, .custom_name = "cstring"};
inline constexpr pg_type internal{
    .id = pg_type_id::internal, .custom_name = "internal"};
inline constexpr pg_type record{
    .id = pg_type_id::record, .custom_name = "record"};
inline constexpr pg_type table_am_handler{
    .id = pg_type_id::table_am_handler,
    .custom_name = "table_am_handler",
};
inline constexpr pg_type tid{.id = pg_type_id::tid, .custom_name = "tid"};
inline constexpr pg_type xid8{.id = pg_type_id::xid8, .custom_name = "xid8"};
inline constexpr pg_type jsonb{.id = pg_type_id::jsonb, .custom_name = "jsonb"};
inline constexpr pg_type anyelement{
    .id = pg_type_id::anyelement, .custom_name = "anyelement"};
inline constexpr pg_type quoted_any{
    .id = pg_type_id::quoted_any, .custom_name = "quoted_any"};
}  // namespace pg

template<identifier p_name>
inline constexpr pg_type custom_type{
    .id = pg_type_id::custom,
    .custom_name = p_name,
};

template<class p_type>
inline constexpr pg_type type_of{
    .id = pg_type_id::custom,
    .custom_name = "custom",
};

template<class... p_types, class... p_values>
[[nodiscard]] consteval auto entities(p_values&&... values) {
    static_assert(
        sizeof...(p_types) + sizeof...(p_values) > 0,
        "an SQL entity list cannot be empty");
    return std::tuple<p_types..., std::remove_cvref_t<p_values>...>{
        p_types{}..., std::forward<p_values>(values)...};
}

struct aggregate_tag final {
};

struct opclass_tag final {
};

enum class aggregate_initial_condition_kind : std::uint8_t {
    absent,
    signed_scalar,
    negative_infinity,
    positive_infinity,
    input_text,
};

enum class pg_infinity : std::uint8_t {
    negative,
    positive,
};

struct pg_input_text {
    fixed_text<256> text;
};

struct aggregate_initial_condition {
    aggregate_initial_condition_kind kind{
        aggregate_initial_condition_kind::absent};
    std::int64_t signed_scalar{};
    fixed_text<256> input_text{"unset"};

    consteval aggregate_initial_condition() = default;

    consteval aggregate_initial_condition(std::int64_t value)
        : kind{aggregate_initial_condition_kind::signed_scalar},
          signed_scalar{value} {
    }

    consteval aggregate_initial_condition(pg_infinity value)
        : kind{value == pg_infinity::negative
                   ? aggregate_initial_condition_kind::negative_infinity
                   : aggregate_initial_condition_kind::positive_infinity} {
    }

    consteval aggregate_initial_condition(pg_input_text value)
        : kind{aggregate_initial_condition_kind::input_text},
          input_text{value.text} {
    }
};

struct opclass_operator_options {
    std::uint16_t strategy{};
    declaration_reference function{};
};

struct opclass_function_options {
    std::uint16_t support_number{};
    declaration_reference function{};
};

template<std::size_t p_capacity>
struct opclass_entry_list {
    std::array<opclass_operator_options, p_capacity> operator_entries{};
    std::array<opclass_function_options, p_capacity> function_entries{};
    std::size_t size{};
};

using opclass_operator_list = opclass_entry_list<16>;
using opclass_function_list = opclass_entry_list<16>;

template<class p_function>
[[nodiscard]] consteval opclass_operator_options opclass_operator(
    std::uint16_t strategy,
    p_function&& function) {
    if (strategy == 0) {
        throw "an operator-class strategy number must be positive";
    }
    return {
        .strategy = strategy,
        .function = declaration_reference{
            std::forward<p_function>(function)},
    };
}

template<class p_function>
[[nodiscard]] consteval opclass_function_options opclass_function(
    std::uint16_t support_number,
    p_function&& function) {
    if (support_number == 0) {
        throw "an operator-class support number must be positive";
    }
    return {
        .support_number = support_number,
        .function = declaration_reference{
            std::forward<p_function>(function)},
    };
}

[[nodiscard]] consteval opclass_operator_list opclass_operators(
    std::initializer_list<opclass_operator_options> entries) {
    if (entries.size() == 0
        || entries.size()
            > opclass_operator_list{}.operator_entries.size()) {
        throw "an operator class requires 1 to 16 operator entries";
    }
    opclass_operator_list result;
    for (const opclass_operator_options& entry : entries) {
        for (std::size_t index = 0; index < result.size; ++index) {
            if (result.operator_entries[index].strategy == entry.strategy) {
                throw "duplicate operator-class strategy number";
            }
        }
        result.operator_entries[result.size++] = entry;
    }
    return result;
}

[[nodiscard]] consteval opclass_function_list opclass_functions(
    std::initializer_list<opclass_function_options> entries) {
    if (entries.size() == 0
        || entries.size()
            > opclass_function_list{}.function_entries.size()) {
        throw "an operator class requires 1 to 16 support entries";
    }
    opclass_function_list result;
    for (const opclass_function_options& entry : entries) {
        for (std::size_t index = 0; index < result.size; ++index) {
            if (result.function_entries[index].support_number
                == entry.support_number) {
                throw "duplicate operator-class support number";
            }
        }
        result.function_entries[result.size++] = entry;
    }
    return result;
}

struct cast_tag final {
};

struct script_phase final {
};

template<class p_sql_result>
struct sql_function_body final {
    checked_sql body;
};

inline constexpr std::int32_t variable_length = -1;
inline constexpr identifier public_role{"public"};

}  // namespace bioseq::sql
