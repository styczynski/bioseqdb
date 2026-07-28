module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

export module bioseq.codegen.clang_structural:model;

export namespace bioseq::codegen::clang_structural {

/// Wire contract shared by the GCC/Clang host generator and the Clang tooling
/// implementation. Increment this when a field is removed or changes meaning.
inline constexpr std::uint32_t model_schema_version = 1;

enum class declaration_kind {
    unknown,
    namespace_,
    record,
    enum_,
    enumerator,
    alias,
    concept_,
    function,
    parameter,
    field,
    variable,
};

enum class attribute_placement {
    declaration_prefix,
    class_head,
    parameter_prefix,
    enumerator_tail,
};

enum class aggregate_kind {
    struct_,
    class_,
    union_,
};

enum class member_access {
    public_,
    protected_,
    private_,
};

struct source_position {
    std::size_t byte_offset{};
    std::size_t line{1};
    std::size_t column{1};
};

struct source_range {
    source_position begin{};
    source_position end{};
};

struct lexical_token {
    std::string spelling;
    source_range range;
};

struct attribute_record {
    std::string qualified_name;
    std::string vendor_namespace;
    std::string name;
    std::string argument_text;
    std::string target_usr;
    attribute_placement placement{
        attribute_placement::declaration_prefix};
    source_range range;
    source_range argument_range;
};

struct documented_parameter {
    std::string name;
    std::string text;
};

struct documentation_record {
    std::string associated_usr;
    bool has_explicit_brief{};
    std::string brief;
    std::string details;
    std::vector<std::string> remarks;
    std::vector<std::string> groups;
    std::vector<documented_parameter> parameters;
    std::string returns;
    std::vector<std::string> runnable_examples;
    source_range range;
};

struct parameter_record {
    std::string stable_usr;
    std::string name;
    std::string cpp_type;
    std::string default_expression;
    bool has_default{};
    std::size_t ordinal{};
    std::vector<attribute_record> attributes;
    source_range range;
    source_range name_range;
};

struct declaration_record {
    std::string stable_usr;
    declaration_kind kind{declaration_kind::unknown};
    std::string name;
    std::string qualified_name;
    std::string enclosing_qualified_name;
    std::string signature_text;
    std::string return_type;
    std::string initializer_text;
    std::string active_condition;
    std::string owning_module;
    std::string member_kind;
    std::vector<parameter_record> parameters;
    std::vector<attribute_record> attributes;
    documentation_record documentation;
    source_range range;
    source_range name_range;
    aggregate_kind record_kind{aggregate_kind::struct_};
    bool exported{};
};

struct aggregate_field_record {
    std::string stable_usr;
    std::string name;
    std::string qualified_name;
    std::string cpp_type;
    std::string initializer_text;
    std::string active_condition;
    std::size_t ordinal{};
    std::vector<attribute_record> attributes;
    documentation_record documentation;
    source_range range;
    source_range name_range;
    member_access access{member_access::public_};
    bool has_default_initializer{};
    bool bit_field{};
    bool multiple_declarators{};
};

struct aggregate_record {
    std::string stable_usr;
    std::string name;
    std::string qualified_name;
    std::string enclosing_qualified_name;
    aggregate_kind kind{aggregate_kind::struct_};
    source_range range;
    source_range name_range;
    documentation_record documentation;
    std::vector<aggregate_field_record> fields;
    std::vector<std::string> structural_errors;
    bool exported{};
    bool anonymous{};
    bool has_base_classes{};
    bool has_virtual_members{};
};

struct translation_unit {
    std::uint32_t schema_version{model_schema_version};
    std::filesystem::path source_path;
    std::string module_name;
    std::string source_sha256;
    std::string compiler_profile;
    std::string clang_version;
    std::vector<lexical_token> tokens;
    std::vector<declaration_record> declarations;
    std::vector<aggregate_record> aggregate_records;
};

struct module_graph_unit {
    std::string source_path;
    std::string provided_module;
    std::vector<std::string> exported_imports;
    std::vector<std::string> internal_imports;
};

struct structural_index {
    std::vector<translation_unit> translation_units;
    std::string clang_version;
    std::string compilation_signature;
};

struct module_graph {
    std::vector<module_graph_unit> units;
    std::string clang_version;
    std::string compilation_signature;
};

struct prepared_compilation_environment {
    std::string compile_database_json;
    std::string pcm_manifest_json;
    std::string depfile_text;
    std::string compilation_signature;
    std::string relocatable_compilation_signature;
    std::string relocatable_compile_database_sha256;
    std::string relocatable_pcm_manifest_sha256;
    std::vector<std::filesystem::path> dependencies;
};

struct prepared_compilation_validation {
    std::string manifest_sha256;
    std::string compilation_signature;
    std::string relocatable_compilation_signature;
};

[[nodiscard]] inline constexpr const char* kind_name(
    declaration_kind kind) noexcept {
    switch (kind) {
        case declaration_kind::namespace_: return "namespace";
        case declaration_kind::record: return "record";
        case declaration_kind::enum_: return "enum";
        case declaration_kind::enumerator: return "enumerator";
        case declaration_kind::alias: return "alias";
        case declaration_kind::concept_: return "concept";
        case declaration_kind::function: return "function";
        case declaration_kind::parameter: return "parameter";
        case declaration_kind::field: return "field";
        case declaration_kind::variable: return "variable";
        case declaration_kind::unknown: return "unknown";
    }
    return "unknown";
}

}  // namespace bioseq::codegen::clang_structural
