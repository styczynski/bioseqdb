module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.sql.collector:model;

import bioseq.codegen.clang_structural;

export namespace bioseq::codegen::sql {

enum class entity_kind {
    alter,
    comment,
    schema,
    session_setting,
    sequence,
    table,
    column,
    primary_key,
    unique,
    foreign_key,
    check,
    index,
    view,
    function,
    procedure,
    sql_function,
    type,
    type_role,
    cast,
    operator_,
    aggregate,
    opclass,
    access_method,
    guc,
    guc_value,
    grant,
    revoke,
    config_dump,
    catalog_dependencies,
    phase,
    install_action,
};

enum class physical_statement_facet : std::uint8_t {
    primary,
    catalog_comment,
    base_type_shell,
    base_type_definition,
};

enum class physical_render_source : std::uint8_t {
    compatibility_sql,
    natural_projection,
};

struct physical_statement_binding {
    entity_kind kind{entity_kind::install_action};
    physical_statement_facet facet{
        physical_statement_facet::primary};
    physical_render_source render_source{
        physical_render_source::compatibility_sql};
    std::string baseline_physical_id;
    std::size_t baseline_ordinal{};
    std::string order_identity;
    std::string compatibility_sql_initializer;
    std::string source_module;
    std::filesystem::path source_path;
    std::string source_sha256;
    clang_structural::source_range source_range;
};

struct option_value {
    std::string name;
    std::string expression;
    clang_structural::source_range range;
};

struct dependency_reference {
    std::string expression;
    clang_structural::source_range range;
};

struct routine_parameter {
    std::string name;
    std::string cpp_type;
    std::string sql_type;
    std::string default_expression;
    bool has_default{};
    bool nullable{};
    bool injected_call_context{};
    clang_structural::source_range range;
    std::string stable_identity_sql_type;
};

enum class routine_result_form {
    scalar,
    void_,
    setof,
    returns_table,
    named_out,
    setof_named_out,
};

struct routine_result_contract {
    routine_result_form form{routine_result_form::scalar};
    std::string cpp_type;
    std::string sql_type;
    std::string row_cpp_name;
};

struct collected_result_field {
    std::string name;
    std::string cpp_type;
    std::string sql_type;
    bool nullable{};
    clang_structural::documentation_record documentation;
    clang_structural::source_range source_range;
};

struct collected_result_row {
    std::string name;
    std::string cpp_qualified_name;
    std::string source_module;
    std::filesystem::path source_path;
    std::string source_sha256;
    clang_structural::source_range source_range;
    std::vector<collected_result_field> fields;
};

struct collected_entity {
    entity_kind kind{entity_kind::phase};
    std::string annotation_name;
    std::string name;
    std::string cpp_qualified_name;
    std::string enclosing_cpp_name;
    std::string source_module;
    std::filesystem::path source_path;
    std::string source_sha256;
    clang_structural::declaration_kind declaration_kind{
        clang_structural::declaration_kind::unknown};
    clang_structural::source_range source_range;
    std::string active_condition;
    std::string schema;
    std::string cpp_signature;
    std::string cpp_return_type;
    std::string initializer_expression;
    std::vector<option_value> options;
    std::vector<dependency_reference> dependencies;
    std::vector<routine_parameter> parameters;
    std::string result_sql_type;
    routine_result_contract result;
    clang_structural::documentation_record documentation;
    bool public_documentation{};
    bool companion{};
    bool typed_install_owner{};
    std::string compatibility_sql_initializer;
    std::string compatibility_order_identity;
    std::string compatibility_source_module;
    std::filesystem::path compatibility_source_path;
    std::string compatibility_source_sha256;
    clang_structural::source_range compatibility_source_range;
    std::string compatibility_baseline_physical_id;
    std::vector<physical_statement_binding> physical_statements;
};

struct collected_program {
    std::vector<clang_structural::translation_unit> translation_units;
    clang_structural::module_graph module_graph;
    std::string compiler_profile;
    std::string compilation_signature;
    std::vector<collected_entity> entities;
    std::vector<collected_result_row> result_rows;
};

struct source_collection_environment {
    std::filesystem::path compile_database;
    std::filesystem::path pcm_root;
    std::string compiler_profile;
};

[[nodiscard]] inline std::vector<const routine_parameter*>
sql_parameters(const collected_entity& entity) {
    std::vector<const routine_parameter*> result;
    result.reserve(entity.parameters.size());
    for (const routine_parameter& parameter : entity.parameters) {
        if (!parameter.injected_call_context) {
            result.push_back(&parameter);
        }
    }
    return result;
}

[[nodiscard]] inline constexpr const char* kind_name(
    entity_kind kind) noexcept {
    switch (kind) {
        case entity_kind::alter: return "alter";
        case entity_kind::comment: return "comment";
        case entity_kind::schema: return "schema";
        case entity_kind::session_setting: return "session_setting";
        case entity_kind::sequence: return "sequence";
        case entity_kind::table: return "table";
        case entity_kind::column: return "column";
        case entity_kind::primary_key: return "primary_key";
        case entity_kind::unique: return "unique";
        case entity_kind::foreign_key: return "foreign_key";
        case entity_kind::check: return "check";
        case entity_kind::index: return "index";
        case entity_kind::view: return "view";
        case entity_kind::function: return "function";
        case entity_kind::procedure: return "procedure";
        case entity_kind::sql_function: return "sql_function";
        case entity_kind::type: return "type";
        case entity_kind::type_role: return "type_role";
        case entity_kind::cast: return "cast";
        case entity_kind::operator_: return "operator";
        case entity_kind::aggregate: return "aggregate";
        case entity_kind::opclass: return "opclass";
        case entity_kind::access_method: return "access_method";
        case entity_kind::guc: return "guc";
        case entity_kind::guc_value: return "guc_value";
        case entity_kind::grant: return "grant";
        case entity_kind::revoke: return "revoke";
        case entity_kind::config_dump: return "config_dump";
        case entity_kind::catalog_dependencies:
            return "catalog_dependencies";
        case entity_kind::phase: return "phase";
        case entity_kind::install_action: return "install_action";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr const char* physical_statement_facet_name(
    physical_statement_facet facet) noexcept {
    switch (facet) {
        case physical_statement_facet::primary: return "primary";
        case physical_statement_facet::catalog_comment:
            return "catalog_comment";
        case physical_statement_facet::base_type_shell:
            return "base_type_shell";
        case physical_statement_facet::base_type_definition:
            return "base_type_definition";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr const char* physical_render_source_name(
    physical_render_source source) noexcept {
    switch (source) {
        case physical_render_source::compatibility_sql:
            return "compatibility_sql";
        case physical_render_source::natural_projection:
            return "natural_projection";
    }
    return "unknown";
}

[[nodiscard]] inline const option_value* find_option(
    const collected_entity& entity,
    std::string_view name) noexcept {
    for (const option_value& option : entity.options) {
        if (option.name == name) return &option;
    }
    return nullptr;
}

}  // namespace bioseq::codegen::sql
