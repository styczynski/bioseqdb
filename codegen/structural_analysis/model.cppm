module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

export module bioseq.codegen.structural_analysis:model;

export namespace bioseq::codegen::structural_analysis {

/// Model format shared by the host generator and Clang tooling.
/// Increment this when a field is removed or changes meaning.
inline constexpr std::uint32_t k_model_schema_revision = 2;

/// How a source file was inspected. Token slicing reads syntax only.
/// Declaration probes let Clang check declarations after their bodies are removed
enum class collection_mode : std::uint8_t {
    lexical_scan,
    full_translation_unit,
    token_slice,
    declaration_probe,
};

/// Linkage of an entity after Clang has applied named-module rules.
enum class entity_linkage : std::uint8_t {
    none,
    internal,
    module,
    external,
};

/// Whether a declaration  is attached to a named module or the global module.
enum class module_attachment : std::uint8_t {
    global_module,
    named_module,
};

/// Kind of souce text kept with a declaration, such as its body or initializer.
enum class declaration_payload_kind : std::uint8_t {
    none,
    function_body,
    variable_initializer,
    alias_rhs,
};

/// Classifies a preprocessor directive without evaluating its condition.
enum class preprocessor_directive_kind : std::uint8_t {
    other,
    if_directive,
    ifdef,
    ifndef,
    elif,
    else_directive,
    endif,
    define,
    undef,
    include,
    pragma,
    error,
    line,
};

/// Kind of C++ declaration found by the scanner.
enum class declaration_kind : std::uint8_t {
    unknown,
    namespace_declaration,
    record,
    enumeration,
    enumerator,
    alias,
    concept_declaration,
    function,
    parameter,
    field,
    variable,
};

/// Position of an attribute within its declaration.
enum class attribute_placement : std::uint8_t {
    declaration_prefix,
    class_head,
    parameter_prefix,
    enumerator_tail,
    namespace_head,
};

/// Kind of function or lambda that needs an inserted attribute.
/// Generators use this with the recorded insertion point.
enum class callable_kind : std::uint8_t {
    function,
    member_function,
    constructor,
    destructor,
    operator_function,
    conversion_function,
    lambda,
};

/// Whether a declaration uses struct, class, or union.
enum class aggregate_kind : std::uint8_t {
    structure,
    class_record,
    union_record,
};

/// Records effective access for an aggregate member.
enum class member_access : std::uint8_t {
    public_access,
    protected_access,
    private_access,
};

/// Source position with a byte offset and one-based line and  column.
struct source_position {
    std::size_t byte_offset{};
    std::size_t line{1};
    std::size_t column{1};
};

/// Source range including begin and excluding end.
struct source_range {
    source_position begin{};
    source_position end{};
};

/// Raw token spelling and its exact source range.
struct lexical_token {
    std::string spelling;
    source_range range;
};

/// Parsed C++ attribute name, arguments, and source ranges.
struct attribute_record {
    std::string qualified_name;
    std::string vendor_namespace;
    std::string name;
    std::string argument_text;
    std::string target_usr;
    attribute_placement placement{
        attribute_placement::declaration_prefix};
    /// Range of this attribute without surrounding commas or brackets.
    source_range spelling_range;
    source_range argument_range;
    /// Range to remove when replacing the attribute with generated code.
    source_range removal_range;
};

/// Declaration properties shared by source generators.
/// Scanners fill these flags from tokens and, Clang declarations.
struct declaration_specifier_state {
    bool direct_namespace_member{};
    bool is_definition{};
    bool is_using_alias{};
    bool is_variable{};
    bool is_function{};
    bool is_static{};
    bool is_constexpr{};
    bool is_inline{};
    bool is_consteval{};
    bool is_extern{};
    bool is_thread_local{};
    bool has_explicit_type{};
    bool has_deduced_type{};
    bool has_default_arguments{};
    bool is_template{};
    bool is_overloaded{};
    bool is_operator{};
    bool has_c_language_linkage{};
    bool multiple_declarators{};
    bool range_contains_macro_expansion{};
};

/// Function or lambda inside a marked namespace, with its attribute insertion point.
///
/// Ranges include their beginning and exclude their end. For functions,
/// insert after the outer template header. For lambdas, insert  cfter the
/// capture list and any explicit template header, before existing attributes
/// and the parameter list
struct callable_insertion_record {
    callable_kind kind{callable_kind::function};
    std::string qualified_name;
    std::string stable_usr;
    bool templated{};
    source_range declaration_range;
    source_range body_range;
    source_position highway_attribute_insertion;
};

/// Source ranges for a marked namespace and its functions.
///
/// body_range excludes the braces. Other ranges include the named tokens.
/// Generators check the export and preprocessor flags before rewriting.
struct marked_namespace_record {
    std::string qualified_name;
    std::string target_usr;
    attribute_record marker;
    source_range marker_block_range;
    source_range declaration_range;
    source_range keyword_range;
    source_range name_range;
    source_range open_brace_range;
    source_range body_range;
    source_range close_brace_range;
    std::vector<callable_insertion_record> callables;
    bool has_explicit_export{};
    bool crosses_preprocessor_region{};
};

/// Documentation attached to one named parameter.
struct documented_parameter {
    std::string name;
    std::string text;
};

/// Parsed Doxygen description and tags for one declaration.
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

/// Function parameter type, default value, attributes, and source location.
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

/// C++ declaration information used by SQL collection and source generation.
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
    aggregate_kind record_kind{aggregate_kind::structure};
    bool exported{};

    /// Source identity used with source_coordinate to match this  declaration.
    std::string source_identity;
    std::string source_coordinate;
    std::string immediate_namespace_source_identity;
    std::string immediate_namespace_stable_usr;
    std::string explicit_declared_type;
    std::vector<std::string> referenced_declaration_source_identities;
    source_range declaration_range;
    source_range declaration_head_range;
    source_range body_or_initializer_range;
    entity_linkage linkage{entity_linkage::none};
    module_attachment attachment{module_attachment::global_module};
    declaration_payload_kind payload_kind{
        declaration_payload_kind::none};
    declaration_specifier_state specifiers;
};

/// A struct, class, or union field with its type, access, and source information.
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
    member_access access{member_access::public_access};
    bool has_default_initializer{};
    bool bit_field{};
    bool multiple_declarators{};
};

/// Struct, class, or union with its fields and properties checked by generators.
struct aggregate_record {
    std::string stable_usr;
    std::string name;
    std::string qualified_name;
    std::string enclosing_qualified_name;
    aggregate_kind kind{aggregate_kind::structure};
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

/// Module declaration and import facts recovered without compiling the source.
struct module_record {
    std::filesystem::path source_path;
    source_range declaration_range;
    std::string provided_module;
    std::vector<std::string> imported_modules;
    bool is_interface{};
    bool is_partition{};
};

/// Namespace identity and the source ranges needed to rewrite its declaration.
struct namespace_record {
    source_range declaration_range;
    source_range body_range;
    source_range keyword_range;
    source_range name_range;
    source_range open_brace_range;
    source_range close_brace_range;
    std::string source_identity;
    std::string stable_usr;
    std::string qualified_name;
    std::vector<attribute_record> attributes;
    bool is_inline{};
    bool is_anonymous{};
    bool is_reopened{};
    bool is_nested_namespace_definition{};
    bool has_explicit_export{};
    bool is_in_export_context{};
};

/// Preprocessor directive and its source ranges.
/// Token slicing leaves conditions unevaluated. Clang collection records
/// whether each directive is active.
struct preprocessor_directive_record {
    preprocessor_directive_kind kind{
        preprocessor_directive_kind::other};
    source_range complete_range;
    source_range argument_range;
    std::string spelling;
    std::size_t nesting_depth{};
    bool active{};
};

/// Scanner results for one source file.
struct translation_unit {
    std::uint32_t schema_revision{k_model_schema_revision};
    std::filesystem::path source_path;
    std::string module_name;
    std::string source_sha256;
    std::string compiler_profile;
    std::string clang_release;
    std::vector<lexical_token> tokens;
    std::vector<declaration_record> declarations;
    std::vector<aggregate_record> aggregate_records;
    std::vector<marked_namespace_record> marked_namespaces;
    collection_mode mode{collection_mode::lexical_scan};
    module_record source_module;
    std::vector<namespace_record> namespaces;
    std::vector<preprocessor_directive_record> preprocessor_directives;
};

/// Module provided by a source file, plus its imports and re-exports.
struct module_graph_unit {
    /// Repository-uelative, source  path using forward slashes.
    std::string source_identity;
    std::string provided_module;
    std::vector<std::string> exported_imports;
    std::vector<std::string> internal_imports;
};

/// Structural results collected under one compiler configuration.
struct structural_index {
    std::vector<translation_unit> translation_units;
    std::string clang_release;
    std::string compilation_signature;
};

/// Module dependency graph for one compiler configuration.
struct module_graph {
    std::vector<module_graph_unit> units;
    std::string clang_release;
    std::string compilation_signature;
};

/// Declaration and active import results collected from the same Clang AST.
struct structural_module_graph_scan {
    structural_index structural;
    module_graph graph;
};

/// Module name, source path, and hash used to match a precompiled module.
struct dependency_module_identity {
    std::string module_name;
    std::filesystem::path source_path;
    std::string source_sha256;
};

/// Kind of scan or compilation performed while preparing module dependencies.
enum class frontend_action_kind : std::uint8_t {
    canonical_token_slice,
    p1689_dependency_discovery,
    make_dependency_scan,
    pcm_compilation,
};

/// Why a source is included in dependency preparation
enum class compilation_input_role : std::uint8_t {
    canonical_scan_only,
    signature_donor,
    dependency_provider,
};

/// Whether a scan or compilation ran or reused checked cached output.
enum class frontend_action_outcome : std::uint8_t {
    executed,
    validated_cache_hit,
};

/// Recorded scan or compilation, including its source role and cache  result.
struct frontend_action_record {
    frontend_action_kind kind{
        frontend_action_kind::canonical_token_slice};
    compilation_input_role input_role{
        compilation_input_role::canonical_scan_only};
    frontend_action_outcome outcome{
        frontend_action_outcome::executed};
    std::string module_name;
    std::string source_sha256;
};

/// Counts token scans and compiler analysis of the source being generated.
struct frontend_action_counts {
    std::uint32_t canonical_token_slice_actions{};
    std::uint32_t canonical_semantic_frontend_actions{};
};

/// Checked action-trace identity and counts.
struct frontend_trace_validation {
    std::string trace_identity;
    frontend_action_counts counts;
};

/// Prepared compilation commands, module files, and hashes used by the scanner.
struct dependency_compilation_environment {
    std::string compile_database_json;
    std::string pcm_manifest_json;
    std::string depfile_text;
    std::string compilation_signature;
    std::string relocatable_compilation_signature;
    std::string relocatable_compile_database_sha256;
    std::string relocatable_pcm_manifest_sha256;
    std::vector<std::filesystem::path> dependencies;
    /// Files that must stay unchanged between final validation and publishing
    /// the completion stamp. Kept in memory for that check, outside the JSON format.
    std::vector<std::filesystem::path> completion_authority_files;
    std::vector<std::filesystem::path> dependency_root_sources;
    std::string canonical_token_import_sha256;
    std::string compiler_profile;
    std::string authority;
    std::string clang_binary_sha256;
    std::string clang_scan_deps_sha256;
    std::string llvm_config_sha256;
    std::string compiler_module_inventory_sha256;
    std::vector<frontend_action_record> observed_actions;
};

/// Inputs used to prepare dependencies while leaving the originl sources uncompiled.
/// Roots are the separate signature module and imported module providers.
struct dependency_compilation_authority {
    std::vector<std::filesystem::path> root_source_paths;
    dependency_module_identity signature_donor;
    std::vector<dependency_module_identity> canonical_inputs;
    std::string canonical_token_import_sha256;
};

/// Verified hashes and  module identities for a prepared compilation environment.
struct dependency_compilation_validation {
    std::string manifest_sha256;
    std::string owned_compile_database_sha256;
    std::string compilation_signature;
    std::string relocatable_compilation_signature;
    std::string compiler_profile;
    std::string authority;
    std::string clang_binary_sha256;
    std::string clang_scan_deps_sha256;
    std::string llvm_config_sha256;
    std::string compiler_module_inventory_sha256;
    std::vector<dependency_module_identity> modules;
};

/// @brief Returns the name used for a declaration kind.
/// @return A lowercase name, or `"unknown"` for an invalid value.
[[nodiscard]] inline constexpr const char* spell_declaration_kind(
    declaration_kind kind) noexcept {
    switch (kind) {
        case declaration_kind::namespace_declaration: return "namespace";
        case declaration_kind::record: return "record";
        case declaration_kind::enumeration: return "enum";
        case declaration_kind::enumerator: return "enumerator";
        case declaration_kind::alias: return "alias";
        case declaration_kind::concept_declaration: return "concept";
        case declaration_kind::function: return "function";
        case declaration_kind::parameter: return "parameter";
        case declaration_kind::field: return "field";
        case declaration_kind::variable: return "variable";
        case declaration_kind::unknown: return "unknown";
    }
    return "unknown";
}

}  // namespace bioseq::codegen::structural_analysis
