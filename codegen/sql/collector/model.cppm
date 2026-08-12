module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.sql.collector:model;

import bioseq.codegen.structural_analysis;

export namespace bioseq::codegen::sql {

enum class entity_kind {
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
};

struct option_value {
    std::string name;
    std::string expression;
    structural_analysis::source_range range;
};

struct dependency_reference {
    std::string expression;
    structural_analysis::source_range range;
};

struct routine_parameter {
    std::string name;
    std::string cpp_type;
    std::string sql_type;
    std::string default_expression;
    bool has_default{};
    bool nullable{};
    bool injected_call_context{};
    structural_analysis::source_range range;
    std::string stable_identity_sql_type;
};

enum class routine_result_form {
    scalar,
    void_,
    set_of,
    table_rows,
    named_output,
    named_output_set,
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
    structural_analysis::documentation_record documentation;
    structural_analysis::source_range source_range;
};

struct collected_result_row {
    std::string name;
    std::string cpp_qualified_name;
    std::string source_module;
    std::filesystem::path source_path;
    std::string source_sha256;
    structural_analysis::source_range source_range;
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
    structural_analysis::declaration_kind declaration_kind{
        structural_analysis::declaration_kind::unknown};
    structural_analysis::source_range source_range;
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
    structural_analysis::documentation_record documentation;
    bool public_documentation{};
    bool companion{};
};

struct collected_program {
    std::vector<structural_analysis::translation_unit> translation_units;
    std::vector<structural_analysis::translation_unit>
        enum_authority_units;
    structural_analysis::module_graph module_graph;
    std::string compiler_profile;
    std::string compilation_signature;
    std::vector<collected_entity> entities;
    std::vector<collected_result_row> result_rows;
};

struct source_collection_environment {
    std::filesystem::path compile_database;
    std::filesystem::path pcm_root;
    std::string compiler_profile;
    std::vector<structural_analysis::dependency_module_identity>
        dependency_modules;
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
