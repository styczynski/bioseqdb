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
inline constexpr std::uint32_t model_schema_version = 2;

/// Identifies how a translation-unit record was collected.  The token-slice
/// mode is intentionally syntax-only; declaration probes are body-free inputs
/// whose semantic facts come from Clang.
enum class collection_mode : std::uint8_t {
    legacy_lexical,
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

/// Whether a declaration belongs to the global or named-module purview.
enum class module_attachment : std::uint8_t {
    global_module,
    named_module,
};

/// The source payload retained by a declaration record.
enum class declaration_payload_kind : std::uint8_t {
    none,
    function_body,
    variable_initializer,
    alias_rhs,
};

enum class preprocessor_directive_kind : std::uint8_t {
    other,
    if_,
    ifdef,
    ifndef,
    elif,
    else_,
    endif,
    define,
    undef,
    include,
    pragma,
    error,
    line,
};

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
    namespace_head,
};

/// The syntactic form of a callable whose definition body has a proven
/// source insertion point.  Consumers use this to apply attributes without
/// re-parsing C++.
enum class callable_kind {
    function,
    member_function,
    constructor,
    destructor,
    operator_function,
    conversion_function,
    lambda,
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
    /// Complete attribute segment range. `range` remains as the legacy alias.
    source_range complete_range;
};

/// Compiler- and token-derived declaration properties used by generic source
/// materializers.  Policy components compose this record instead of mirroring
/// individual booleans.
struct declaration_specifier_facts {
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

/// A callable definition inside a structurally marked namespace.  All ranges
/// are half-open byte ranges.  hwy_attr_insertion is the compiler-proven
/// declaration-prefix position for functions (after any outer template head)
/// and the legal GNU-attribute slot after a lambda's parameter list, or after
/// its complete template head/front attributes when parentheses are omitted.
struct callable_insertion_record {
    callable_kind kind{callable_kind::function};
    std::string qualified_name;
    std::string stable_usr;
    bool templated{};
    source_range declaration_range;
    source_range body_range;
    source_position hwy_attr_insertion;
};

/// Exact lexical geometry for a namespace selected by a structural marker.
/// body_range excludes the namespace braces; all other token ranges include
/// the named token.  The boolean policy facts are reported rather than hidden
/// so generators can fail closed with a domain-specific diagnostic.
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

    /// Stable association key emitted into declaration probes.  Unlike a USR,
    /// it identifies the original source declaration and coordinate.
    std::string provenance_id;
    std::string provenance_coordinate;
    std::string immediate_namespace_provenance_id;
    std::string immediate_namespace_stable_usr;
    std::string explicit_declared_type;
    std::vector<std::string> referenced_declaration_provenance_ids;
    source_range declaration_range;
    source_range declaration_head_range;
    source_range body_or_initializer_range;
    entity_linkage linkage{entity_linkage::none};
    module_attachment attachment{module_attachment::global_module};
    declaration_payload_kind payload_kind{
        declaration_payload_kind::none};
    declaration_specifier_facts specifiers;
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

/// Module declaration and import facts recovered without compiling the source.
struct module_record {
    std::filesystem::path source_path;
    source_range declaration_range;
    std::string provided_module;
    std::vector<std::string> imported_modules;
    bool is_interface{};
    bool is_partition{};
};

/// Exact source geometry and semantic identity of one namespace declaration.
struct namespace_record {
    source_range declaration_range;
    source_range body_range;
    source_range keyword_range;
    source_range name_range;
    source_range open_brace_range;
    source_range close_brace_range;
    std::string provenance_id;
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

/// Raw preprocessor geometry intersecting a token slice.  No condition is
/// evaluated in token-slice mode; `active` is populated only by a semantic
/// collection mode.
struct preprocessor_directive_record {
    preprocessor_directive_kind kind{
        preprocessor_directive_kind::other};
    source_range complete_range;
    source_range argument_range;
    std::string spelling;
    std::size_t nesting_depth{};
    bool active{};
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
    std::vector<marked_namespace_record> marked_namespaces;
    collection_mode mode{collection_mode::legacy_lexical};
    module_record source_module;
    std::vector<namespace_record> namespaces;
    std::vector<preprocessor_directive_record> preprocessor_directives;
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

/// Stable source identity used to join dependency-only PREPARE observations
/// to the canonical token authority and the authenticated PCM manifest.
struct preparation_module_identity {
    std::string module_name;
    std::filesystem::path source_path;
    std::string source_sha256;
};

/// The closed set of frontend actions whose execution or validated cache hit
/// is observed by PREPARE. The canonical source may only produce the first.
enum class preparation_action_kind : std::uint8_t {
    canonical_token_slice,
    p1689_dependency_discovery,
    make_dependency_scan,
    pcm_compilation,
};

enum class preparation_input_class : std::uint8_t {
    canonical_scan_only,
    signature_donor,
    prepared_dependency,
};

enum class preparation_action_disposition : std::uint8_t {
    executed,
    validated_cache_hit,
};

struct preparation_action_record {
    preparation_action_kind kind{
        preparation_action_kind::canonical_token_slice};
    preparation_input_class input_class{
        preparation_input_class::canonical_scan_only};
    preparation_action_disposition disposition{
        preparation_action_disposition::executed};
    std::string module_name;
    std::string source_sha256;
};

struct preparation_action_summary {
    std::uint32_t canonical_token_slice_actions{};
    std::uint32_t canonical_semantic_frontend_actions{};
};

struct preparation_action_trace_validation {
    std::string trace_identity;
    preparation_action_summary summary;
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
    /// Exact files whose bytes/identities must remain stable from the final
    /// manifest validation through completion-stamp publication. This is an
    /// in-memory transaction guard and is intentionally not a wire field.
    std::vector<std::filesystem::path> completion_authority_files;
    std::vector<std::filesystem::path> dependency_root_sources;
    std::string canonical_token_import_sha256;
    std::string compiler_profile;
    std::string authority;
    std::string clang_binary_sha256;
    std::string clang_scan_deps_sha256;
    std::string llvm_config_sha256;
    std::string compiler_module_inventory_sha256;
    std::vector<preparation_action_record> observed_actions;
};

/// Canonical raw-token authority for dependency-only preparation. Root paths
/// are the distinct attached donor plus providers of every canonical import;
/// canonical body-bearing sources themselves are never roots.
struct dependency_compilation_authority {
    std::vector<std::filesystem::path> root_source_paths;
    preparation_module_identity signature_donor;
    std::vector<preparation_module_identity> canonical_inputs;
    std::string canonical_token_import_sha256;
};

struct prepared_compilation_validation {
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
    std::vector<preparation_module_identity> modules;
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
