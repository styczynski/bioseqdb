module;

#include <cstddef>
#include <cstdint>

export module bioseq.codegen.sql.annotation_model:validation;

import :entities;
import :options;

export namespace bioseq::sql {

enum class annotation_kind : std::uint8_t {
    alter,
    comment,
    session_setting,
    function,
    procedure,
    param,
    result,
    table,
    column,
    schema,
    sequence,
    view,
    sql_function,
    operator_,
    type,
    cast,
    aggregate,
    opclass,
    access_method,
    guc,
    guc_value,
    primary_key,
    unique,
    index,
    check,
    foreign_key,
    grant,
    revoke,
    depends,
    config_dump,
    phase,
    type_role,
    catalog_dependencies,
    install_action,
};

template<class Options>
struct annotation_spec {
    annotation_kind kind{};
    Options options{};
};

struct empty_options {
};

#define BIOSEQ_SQL_CONCRETE_BUILDER(Name, Kind, Options)                    \
    [[nodiscard]] consteval annotation_spec<Options> Name(Options options) { \
        return {.kind = annotation_kind::Kind, .options = options};          \
    }                                                                        \
    [[nodiscard]] consteval annotation_spec<Options> Name() {                \
        return {.kind = annotation_kind::Kind, .options = {}};               \
    }

BIOSEQ_SQL_CONCRETE_BUILDER(function, function, function_options)
BIOSEQ_SQL_CONCRETE_BUILDER(procedure, procedure, procedure_options)
BIOSEQ_SQL_CONCRETE_BUILDER(param, param, param_options)
BIOSEQ_SQL_CONCRETE_BUILDER(result, result, result_options)
BIOSEQ_SQL_CONCRETE_BUILDER(table, table, table_options)
BIOSEQ_SQL_CONCRETE_BUILDER(column, column, column_options)
BIOSEQ_SQL_CONCRETE_BUILDER(schema, schema, schema_options)
BIOSEQ_SQL_CONCRETE_BUILDER(sequence, sequence, sequence_options)
BIOSEQ_SQL_CONCRETE_BUILDER(view, view, view_options)
BIOSEQ_SQL_CONCRETE_BUILDER(sql_function, sql_function, sql_function_options)
BIOSEQ_SQL_CONCRETE_BUILDER(operator_, operator_, operator_options)
BIOSEQ_SQL_CONCRETE_BUILDER(type, type, type_options)
BIOSEQ_SQL_CONCRETE_BUILDER(cast, cast, cast_options)
BIOSEQ_SQL_CONCRETE_BUILDER(aggregate, aggregate, aggregate_options)
BIOSEQ_SQL_CONCRETE_BUILDER(opclass, opclass, opclass_options)
BIOSEQ_SQL_CONCRETE_BUILDER(
    access_method, access_method, access_method_options)
BIOSEQ_SQL_CONCRETE_BUILDER(guc, guc, guc_options)
BIOSEQ_SQL_CONCRETE_BUILDER(guc_value, guc_value, guc_value_options)
BIOSEQ_SQL_CONCRETE_BUILDER(primary_key, primary_key, primary_key_options)
BIOSEQ_SQL_CONCRETE_BUILDER(unique, unique, unique_options)
BIOSEQ_SQL_CONCRETE_BUILDER(index, index, index_options)
BIOSEQ_SQL_CONCRETE_BUILDER(check, check, check_options)
BIOSEQ_SQL_CONCRETE_BUILDER(foreign_key, foreign_key, foreign_key_options)
BIOSEQ_SQL_CONCRETE_BUILDER(grant, grant, grant_options)
BIOSEQ_SQL_CONCRETE_BUILDER(revoke, revoke, revoke_options)
#undef BIOSEQ_SQL_CONCRETE_BUILDER

[[nodiscard]] consteval annotation_spec<empty_options> config_dump() {
    return {.kind = annotation_kind::config_dump};
}

[[nodiscard]] consteval annotation_spec<empty_options> phase() {
    return {.kind = annotation_kind::phase};
}

[[nodiscard]] consteval annotation_spec<empty_options> type_input() {
    return {.kind = annotation_kind::type_role};
}

[[nodiscard]] consteval annotation_spec<type_role_options> type_input(
    type_role_options options) {
    return {.kind = annotation_kind::type_role, .options = options};
}

[[nodiscard]] consteval annotation_spec<empty_options> type_output() {
    return {.kind = annotation_kind::type_role};
}

[[nodiscard]] consteval annotation_spec<type_role_options> type_output(
    type_role_options options) {
    return {.kind = annotation_kind::type_role, .options = options};
}

[[nodiscard]] consteval annotation_spec<empty_options> type_receive() {
    return {.kind = annotation_kind::type_role};
}

[[nodiscard]] consteval annotation_spec<type_role_options> type_receive(
    type_role_options options) {
    return {.kind = annotation_kind::type_role, .options = options};
}

[[nodiscard]] consteval annotation_spec<empty_options> type_send() {
    return {.kind = annotation_kind::type_role};
}

[[nodiscard]] consteval annotation_spec<type_role_options> type_send(
    type_role_options options) {
    return {.kind = annotation_kind::type_role, .options = options};
}

[[nodiscard]] consteval annotation_spec<empty_options> type_typmod_in() {
    return {.kind = annotation_kind::type_role};
}

[[nodiscard]] consteval annotation_spec<type_role_options> type_typmod_in(
    type_role_options options) {
    return {.kind = annotation_kind::type_role, .options = options};
}

[[nodiscard]] consteval annotation_spec<empty_options> type_typmod_out() {
    return {.kind = annotation_kind::type_role};
}

[[nodiscard]] consteval annotation_spec<type_role_options> type_typmod_out(
    type_role_options options) {
    return {.kind = annotation_kind::type_role, .options = options};
}

[[nodiscard]] consteval annotation_spec<empty_options> type_analyze() {
    return {.kind = annotation_kind::type_role};
}

[[nodiscard]] consteval annotation_spec<type_role_options> type_analyze(
    type_role_options options) {
    return {.kind = annotation_kind::type_role, .options = options};
}

[[nodiscard]] consteval annotation_spec<empty_options> type_subscript() {
    return {.kind = annotation_kind::type_role};
}

[[nodiscard]] consteval annotation_spec<type_role_options> type_subscript(
    type_role_options options) {
    return {.kind = annotation_kind::type_role, .options = options};
}

[[nodiscard]] consteval annotation_spec<empty_options>
am_catalog_dependencies() {
    return {.kind = annotation_kind::catalog_dependencies};
}

[[nodiscard]] consteval annotation_spec<catalog_dependencies_options>
am_catalog_dependencies(catalog_dependencies_options options) {
    return {
        .kind = annotation_kind::catalog_dependencies,
        .options = options,
    };
}

template<class... References>
[[nodiscard]] consteval annotation_spec<empty_options>
depends(References&&...) {
    static_assert(
        sizeof...(References) > 0,
        "bioseq_sql::depends requires at least one declaration reference");
    return {.kind = annotation_kind::depends};
}

}  // namespace bioseq::sql
