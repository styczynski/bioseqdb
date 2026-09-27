module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.safety:analyzer_model;

import :model;

export namespace bioseq::codegen::safety {

/// Project or library that defines a  resolved function.
enum class callee_domain : std::uint8_t {
    project,
    postgresql_server,
    standard_library,
    system_library,
    compiler_builtin,
    unknown,
};

/// Returns the function ownership category used in JSON.
[[nodiscard]] inline constexpr std::string_view callee_domain_name(
    callee_domain value) noexcept {
    switch (value) {
        case callee_domain::project: return "project";
        case callee_domain::postgresql_server:
            return "postgresql_server";
        case callee_domain::standard_library:
            return "standard_library";
        case callee_domain::system_library:
            return "system_library";
        case callee_domain::compiler_builtin:
            return "compiler_builtin";
        case callee_domain::unknown: return "unknown";
    }
    return "unknown";
}

/// How the analyzer identifies an object that is alive at a call.
enum class object_identity_kind : std::uint8_t {
    declaration_usr,
    synthetic_temporary,
    synthetic_capture,
    synthetic_coroutine_state,
};

/// Returns the object identity kins used in JSON.
[[nodiscard]] inline constexpr std::string_view object_identity_kind_name(
    object_identity_kind value) noexcept {
    switch (value) {
        case object_identity_kind::declaration_usr:
            return "declaration_usr";
        case object_identity_kind::synthetic_temporary:
            return "synthetic_temporary";
        case object_identity_kind::synthetic_capture:
            return "synthetic_capture";
        case object_identity_kind::synthetic_coroutine_state:
            return "synthetic_coroutine_state";
    }
    return "unknown";
}

/// Analysis policy for a call target outside the project.
enum class external_call_mode : std::uint8_t {
    not_external,
    audited_no_user_code,
    analyze_body,
    unknown,
};

/// Returns the external call policy used in JSON.
[[nodiscard]] inline constexpr std::string_view external_call_mode_name(
    external_call_mode value) noexcept {
    switch (value) {
        case external_call_mode::not_external:
            return "not_external";
        case external_call_mode::audited_no_user_code:
            return "audited_no_user_code";
        case external_call_mode::analyze_body:
            return "analyze_body";
        case external_call_mode::unknown:
            return "unknown";
    }
    return "unknown";
}

/// Source range paired with its normalized source path.
struct located_source_range {
    std::string source_file;
    source_range span;
};

/// One macro expansion, including where it was invoked and defined
struct macro_expansion_record {
    std::string name;
    located_source_range invocation;
    located_source_range definition;
    std::string definition_sha256;
};

/// A call argument's index and optional integer value evaluated by Clang.
struct constant_integer_argument {
    std::size_t ordinal{};
    std::optional<std::int64_t> value;
};

/// Object that Clang found to be alive at a call.
struct flow_object {
    std::string stable_object_id;
    std::optional<std::string> declaration_usr;
    object_identity_kind identity_kind{
        object_identity_kind::declaration_usr};
    std::string canonical_type;
    crossing_origin origin{crossing_origin::automatic_object};
    located_source_range declaration;
    bool trivially_destructible{};
};

/// Reviewed external declaration identified by its source location and hash.
struct external_declaration_provenance {
    std::string record_id;
    std::string source_file;
    source_range declaration_span;
    std::string declaration_sha256;
    std::string library_version;
    bool user_code_callback_free{};
};

/// Possible function called at a source location, with its analysis  policy.
struct flow_call_target {
    std::string target_usr;
    std::string target_qualified_name;
    callee_domain domain{callee_domain::unknown};
    external_call_mode external_mode{
        external_call_mode::not_external};
    std::optional<external_declaration_provenance>
        external_provenance;
    std::optional<provenance_reference>
        pg_no_longjmp_allowlist_evidence;
};

/// Evidence that a call uses the reviewed implementation of an error bridge.
struct bridge_transition_evidence {
    provenance_reference primitive;
    std::string target_usr;
    std::string target_qualified_name;
    located_source_range transition_callsite;
    std::string implementation_usr;
    std::string implementation_qualified_name;
    std::string implementation_module;
    located_source_range implementation_definition;
    std::string implementation_body_sha256;
    std::string implementation_contract_id;
    std::string implementation_contract_sha256;
    std::string protected_region_id;
};

/// Clang results for a call, its possible targets, and objects alive there.
struct flow_call_site {
    std::string stable_site_id;
    std::string caller_usr;
    located_source_range spelling;
    located_source_range expansion;
    std::vector<macro_expansion_record> macro_chain;
    resolution_status resolution{resolution_status::direct};
    bool target_set_complete{};
    std::vector<flow_call_target> targets;
    std::vector<constant_integer_argument> constant_integer_arguments;
    std::vector<flow_object> live_objects;
    std::vector<flow_object> callable_capture_objects;
    bridge_kind bridge_transition{bridge_kind::none};
    bool analyzer_entered_bridge_callable{};
    bool generated_boundary_tail{};
    std::optional<bridge_transition_evidence>
        bridge_primitive_evidence;
};

/// Calls and object lifetimes found in a function, with checks for missing analysis.
struct function_flow {
    std::string stable_usr;
    std::string qualified_name;
    std::string source_file;
    std::string module_name;
    source_range declaration_span;
    source_range body_span;
    bool body_available{};
    bool template_instantiation_complete{};
    bool macro_expansion_complete{};
    bool cfg_complete{};
    bool lifetime_analysis_complete{};
    bool implicit_call_edges_complete{};
    std::string body_sha256;
    std::string cfg_sha256;
    std::size_t cfg_block_count{};
    std::size_t cfg_element_count{};
    std::size_t ast_callsite_count{};
    std::size_t cfg_implicit_callsite_count{};
    std::size_t emitted_callsite_count{};
    std::vector<std::string> lifetime_object_ids;
    std::string lifetime_inventory_sha256;
    std::vector<flow_call_site> calls;
    std::vector<std::string> unresolved_indirect_targets;
    std::vector<std::string> unavailable_bodies;
    std::vector<std::string> control_flow_assembly_sites;
};

/// Links an error call to its visit marker and optional failure trigger.
struct runtime_probe_fact {
    std::string fault_key;
    runtime_relation relation{runtime_relation::natural_at_site};
    std::string site_call_stable_id;
    std::string visit_call_stable_id;
    std::optional<std::string> trigger_call_stable_id;
};

/// PostgreSQL callback registration and its exact project target.
struct postgres_callback_root {
    std::string stable_callback_id;
    std::string family;
    std::string slot;
    std::string owner_usr;
    std::string owner_qualified_name;
    located_source_range registration;
    std::string target_usr;
    std::string target_qualified_name;
    located_source_range target_definition;
};

/// Clang analysis results for the sources in one prepared build configuration.
struct compiler_analysis_facts {
    std::uint32_t schema_revision{2};
    compilation_profile profile{compilation_profile::production};
    std::string compiler_profile;
    std::string clang_version;
    std::string source_list_sha256;
    std::string compile_database_sha256;
    std::string producer_executable_sha256;
    std::string compile_graph_sha256;
    std::string pcm_manifest_sha256;
    std::string sql_manifest_sha256;
    std::vector<std::string> root_usrs;
    std::vector<postgres_callback_root> callback_roots;
    std::vector<function_flow> functions;
    std::vector<runtime_probe_fact> runtime_probes;
};

}  // namespace bioseq::codegen::safety
