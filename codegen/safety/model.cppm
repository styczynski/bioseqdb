module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.safety:model;

import bioseq.codegen.structural_analysis;

export namespace bioseq::codegen::safety {

using source_range = structural_analysis::source_range;
using source_position = structural_analysis::source_position;

/// Build configuration used to analyze the generated SQL wrappers.
enum class compilation_profile : std::uint8_t {
    production,
    test_indexes,
};

/// PostgreSQL error levels that a call can report.
enum class postgres_severity : std::uint8_t {
    recoverable_error,
    fatal,
    panic,
    unresolved,
};

/// How a PostgreSQL ERROR call handles C++ object cleanup.
enum class site_classification : std::uint8_t {
    bridged,
    specialized_bridge,
    lifetime_free_direct,
    terminal_after_unwind,
    unclassified,
};

/// How a call prevents PostgreSQL longjmp from skipping C++ destructors.
enum class bridge_kind : std::uint8_t {
    none,
    invoke_pg_as_cpp,
    subtransaction,
    cross_transaction_callback,
    generated_boundary_tail,
};

/// Whether the analyzer found every, possible target and body for a call.
enum class resolution_status : std::uint8_t {
    direct,
    resolved_indirect,
    unresolved_indirect,
    unavailable_body,
    control_flow_assembly,
    cycle_without_fixed_point,
};

/// Kind of C++ object that is alive during a PostgreSQL call.
enum class crossing_origin : std::uint8_t {
    automatic_object,
    temporary,
    lambda_capture,
    catch_parameter,
    coroutine_state,
};

/// Kind of reviewed declaration or error-handling  helper.
enum class provenance_kind : std::uint8_t {
    recoverable_error_seed,
    terminal_severity_seed,
    bridge_primitive,
    pg_no_longjmp_leaf,
    standard_library_declaration,
    system_library_declaration,
    compiler_builtin_declaration,
};

/// Property that runtime tests must cover in the safety report.
enum class coverage_dimension : std::uint8_t {
    classification,
    bridge,
    converter_family,
    owner_family,
    cleanup_phase,
};

/// Where a runtime failure occurs relative to the PostgreSQL ERROR call.
enum class runtime_relation : std::uint8_t {
    natural_at_site,
    injected_routes_to_site,
    injected_after_site_success,
};

/// Role of a call recorded by a runtime safety test.
enum class runtime_anchor_kind : std::uint8_t {
    postgres_error_site,
    record_site_visit,
    consume_fault,
};

/// Required order of a test visit and an injected failure
enum class runtime_probe_ordering : std::uint8_t {
    visit_only,
    trigger_before_visit,
    visit_before_trigger,
};

/// Library whose reviewed calls need no further body analysis.
enum class audited_stop_domain : std::uint8_t {
    postgresql_server,
    standard_library,
    system_library,
    compiler_builtin,
};

/// Returns the compilation profile name used in JSON.
[[nodiscard]] inline constexpr std::string_view profile_name(
    compilation_profile value) noexcept {
    switch (value) {
        case compilation_profile::production: return "production";
        case compilation_profile::test_indexes: return "test_indexes";
    }
    return "unknown";
}

/// Returns the PostgreSQL severity name used in JSON.
[[nodiscard]] inline constexpr std::string_view severity_name(
    postgres_severity value) noexcept {
    switch (value) {
        case postgres_severity::recoverable_error: return "error";
        case postgres_severity::fatal: return "fatal";
        case postgres_severity::panic: return "panic";
        case postgres_severity::unresolved: return "unresolved";
    }
    return "unknown";
}

/// Returns the error-site classification name used in JSON.
[[nodiscard]] inline constexpr std::string_view classification_name(
    site_classification value) noexcept {
    switch (value) {
        case site_classification::bridged: return "bridged";
        case site_classification::specialized_bridge:
            return "specialized_bridge";
        case site_classification::lifetime_free_direct:
            return "lifetime_free_direct";
        case site_classification::terminal_after_unwind:
            return "terminal_after_unwind";
        case site_classification::unclassified: return "unclassified";
    }
    return "unknown";
}

/// Returns the error-handling helper name used in JSON.
[[nodiscard]] inline constexpr std::string_view bridge_name(
    bridge_kind value) noexcept {
    switch (value) {
        case bridge_kind::none: return "none";
        case bridge_kind::invoke_pg_as_cpp: return "invoke_pg_as_cpp";
        case bridge_kind::subtransaction: return "sql_subtransaction";
        case bridge_kind::cross_transaction_callback:
            return "sql_call_context";
        case bridge_kind::generated_boundary_tail:
            return "generated_boundary_tail";
    }
    return "unknown";
}

/// Returns the call-resolution status used in JSON.
[[nodiscard]] inline constexpr std::string_view resolution_name(
    resolution_status value) noexcept {
    switch (value) {
        case resolution_status::direct: return "direct";
        case resolution_status::resolved_indirect:
            return "resolved_indirect";
        case resolution_status::unresolved_indirect:
            return "unresolved_indirect";
        case resolution_status::unavailable_body:
            return "unavailable_body";
        case resolution_status::control_flow_assembly:
            return "control_flow_assembly";
        case resolution_status::cycle_without_fixed_point:
            return "cycle_without_fixed_point";
    }
    return "unknown";
}

/// Returns the reviewed tecord kind used in JSON.
[[nodiscard]] inline constexpr std::string_view provenance_kind_name(
    provenance_kind value) noexcept {
    switch (value) {
        case provenance_kind::recoverable_error_seed:
            return "recoverable_error_seed";
        case provenance_kind::terminal_severity_seed:
            return "terminal_severity_seed";
        case provenance_kind::bridge_primitive:
            return "bridge_primitive";
        case provenance_kind::pg_no_longjmp_leaf:
            return "pg_no_longjmp_leaf";
        case provenance_kind::standard_library_declaration:
            return "standard_library_declaration";
        case provenance_kind::system_library_declaration:
            return "system_library_declaration";
        case provenance_kind::compiler_builtin_declaration:
            return "compiler_builtin_declaration";
    }
    return "unknown";
}

/// Returns the runtime coverage property  name used in JSON.
[[nodiscard]] inline constexpr std::string_view coverage_dimension_name(
    coverage_dimension value) noexcept {
    switch (value) {
        case coverage_dimension::classification: return "classification";
        case coverage_dimension::bridge: return "bridge";
        case coverage_dimension::converter_family: return "converter_family";
        case coverage_dimension::owner_family: return "owner_family";
        case coverage_dimension::cleanup_phase: return "cleanup_phase";
    }
    return "unknown";
}

/// Returns the failure-injection relation used in JSON.
[[nodiscard]] inline constexpr std::string_view runtime_relation_name(
    runtime_relation value) noexcept {
    switch (value) {
        case runtime_relation::natural_at_site:
            return "natural_at_site";
        case runtime_relation::injected_routes_to_site:
            return "injected_routes_to_site";
        case runtime_relation::injected_after_site_success:
            return "injected_after_site_success";
    }
    return "unknown";
}

/// Returns the runtime probe role used in JSON.
[[nodiscard]] inline constexpr std::string_view runtime_anchor_kind_name(
    runtime_anchor_kind value) noexcept {
    switch (value) {
        case runtime_anchor_kind::postgres_error_site:
            return "postgres_error_site";
        case runtime_anchor_kind::record_site_visit:
            return "record_site_visit";
        case runtime_anchor_kind::consume_fault:
            return "consume_fault";
    }
    return "unknown";
}

/// Returns the required probe order used in JSON.
[[nodiscard]] inline constexpr std::string_view
runtime_probe_ordering_name(
    runtime_probe_ordering value) noexcept {
    switch (value) {
        case runtime_probe_ordering::visit_only:
            return "visit_only";
        case runtime_probe_ordering::trigger_before_visit:
            return "trigger_before_visit";
        case runtime_probe_ordering::visit_before_trigger:
            return "visit_before_trigger";
    }
    return "unknown";
}

/// Returns the reviewed library category used in JSON.
[[nodiscard]] inline constexpr std::string_view audited_stop_domain_name(
    audited_stop_domain value) noexcept {
    switch (value) {
        case audited_stop_domain::postgresql_server:
            return "postgresql_server";
        case audited_stop_domain::standard_library:
            return "standard_library";
        case audited_stop_domain::system_library:
            return "system_library";
        case audited_stop_domain::compiler_builtin:
            return "compiler_builtin";
    }
    return "unknown";
}

/// Location and hashes of the upstream definition reviewed for safety.
struct provenance_definition {
    std::string upstream_file;
    source_range upstream_span;
    std::string upstream_file_sha256;
    std::string fragment_sha256;
};

/// Reviewed declaration and the evidence used to decide how to analyze it.
struct provenance_record {
    std::string record_id;
    provenance_kind kind{provenance_kind::recoverable_error_seed};
    std::string spelling;
    std::string upstream_file;
    source_range upstream_span;
    std::string upstream_file_sha256;
    std::string fragment_sha256;
    std::string rationale;
    std::vector<postgres_severity> severities;
    std::string library_version;
    std::string target_usr;
    std::string target_qualified_name;
    std::string authority_id;
    std::optional<provenance_definition> definition;
    bool user_code_callback_free{};
};

/// Reviewed safety records tied to a PostgreSQL source revision.
struct provenance_bundle {
    std::uint32_t schema_revision{2};
    std::uint32_t postgresql_major{17};
    std::string postgresql_tag;
    std::string postgresql_revision;
    std::vector<provenance_record> records;
};

/// Reference from a safety report to the reviewed source record it uses.
struct provenance_reference {
    std::string record_id;
    std::string postgresql_revision;
    std::string upstream_file;
    source_range upstream_span;
    std::string fragment_sha256;
    std::string rationale;
};

/// C++ object alive across a PostgreSQL call, with its cleanup requirements.
struct crossing_type {
    std::string canonical_type;
    crossing_origin origin{crossing_origin::automatic_object};
    std::string source_file;
    source_range declaration_span;
    bool trivially_destructible{};
    bool crosses_postgres_longjmp{};
};

/// Function and source  location along a chain of calls.
struct call_path_step {
    std::string usr;
    std::string qualified_declaration;
    std::string source_file;
    source_range source_span;
};

/// Checks completed by the analyzer and calls it could not resolve.
struct analysis_completeness {
    bool template_instantiation_complete{};
    bool macro_expansion_complete{};
    bool cfg_complete{};
    bool lifetime_analysis_complete{};
    bool call_graph_fixed_point_complete{};
    std::vector<std::string> unresolved_indirect_targets;
    std::vector<std::string> unavailable_bodies;
    std::vector<std::string> control_flow_assembly_sites;
};

/// Evidence linking a protected call to its reviewed error-handling helper.
struct bridge_transition_validation {
    provenance_reference primitive;
    std::string target_usr;
    std::string target_qualified_name;
    std::string transition_source_file;
    source_range transition_source_span;
    std::string implementation_usr;
    std::string implementation_qualified_name;
    std::string implementation_module;
    std::string implementation_source_file;
    source_range implementation_source_span;
    std::string implementation_body_sha256;
    std::string implementation_contract_id;
    std::string implementation_contract_sha256;
    std::string protected_region_id;
};

/// Analysis of a PostgreSQL error call and the C++ objects alive there
struct error_site {
    std::string site_id;
    std::string source_file;
    source_range expanded_source_span;
    std::string callee_or_macro;
    std::vector<call_path_step> transitive_call_path;
    std::vector<postgres_severity> possible_severities;
    bool severity_constant_propagated{};
    resolution_status resolution{resolution_status::direct};
    site_classification classification{
        site_classification::unclassified};
    bridge_kind bridge{bridge_kind::none};
    std::vector<crossing_type> crossing_types;
    std::vector<crossing_type> bridge_callable_crossing_types;
    std::optional<provenance_reference> no_longjmp_allowlist_evidence;
    std::optional<bridge_transition_validation>
        bridge_evidence;
    bool analyzer_entered_bridge_callable{};
    bool generated_boundary_tail{};
    std::vector<std::string> converter_families;
    std::vector<std::string> owner_families;
    std::vector<std::string> cleanup_phases;
};

/// Reviewed library call where analysis stops because it cannot, call user code.
struct audited_stop {
    std::string stop_id;
    std::string source_file;
    source_range expanded_source_span;
    std::string target_usr;
    std::string target_qualified_name;
    resolution_status resolution{resolution_status::direct};
    audited_stop_domain domain{
        audited_stop_domain::postgresql_server};
    std::vector<call_path_step> transitive_call_path;
    provenance_reference evidence;
    std::string library_version;
    bool user_code_callback_free{};
};

/// Error-safety results for all calls reachable from one SQL wrapper.
struct safety_parent_row {
    std::string sql_identity;
    std::string owner_usr;
    std::string qualified_declaration;
    std::string owner_source_file;
    source_range owner_source_span;
    std::string generated_wrapper;
    std::string public_symbol;
    compilation_profile profile{compilation_profile::production};
    std::string transitive_closure_sha256;
    std::size_t recoverable_site_count{};
    std::size_t terminal_site_count{};
    std::size_t audited_stop_count{};
    analysis_completeness analysis;
    std::vector<error_site> sites;
    std::vector<audited_stop> audited_stops;
};

/// SQL functions that share a generated wrapper and its exported C symbols.
struct symbol_group {
    std::string public_symbol;
    std::string finfo_symbol;
    std::string generated_wrapper;
    std::vector<std::string> sql_identities;
};

/// Identifies the analyzzr rules and Clang version used  for a safety report.
[[nodiscard]] inline std::string clang_cfg_lifetime_identity(
    std::string_view clang_version) {
    return "bioseq-clang-cfg-lifetime\n"
        "revision=2\n"
        "clang_version=" + std::string{clang_version};
}

/// Safety report for one build configuration, with hashes of its inputs.
struct safety_ledger {
    std::uint32_t schema_revision{2};
    compilation_profile profile{compilation_profile::production};
    std::uint32_t postgresql_major{17};
    std::string postgresql_revision;
    std::string provenance_bundle_sha256;
    std::string source_list_sha256;
    std::string compile_database_sha256;
    std::string producer_executable_sha256;
    std::string compile_graph_sha256;
    std::string pcm_manifest_sha256;
    std::string sql_manifest_sha256;
    std::string compiler_facts_sha256;
    std::string clang_version;
    std::string analyzer_version;
    std::string generator_revision;
    std::size_t callback_root_count{};
    std::string callback_closure_sha256;
    std::vector<safety_parent_row> parents;
    std::vector<symbol_group> symbol_groups;
};

/// Source location and hash identifying a call used by a runtime safety test.
struct runtime_source_anchor {
    runtime_anchor_kind kind{
        runtime_anchor_kind::postgres_error_site};
    std::string compiler_stable_id;
    std::string enclosing_function_usr;
    std::string source_file;
    source_range expanded_source_span;
    std::string source_fragment_sha256;
};

/// Expected visit and failure counts, and their required order.
struct runtime_probe_contract {
    std::uint32_t abi_revision{2};
    std::size_t expected_visit_count{1};
    std::size_t expected_trigger_count{};
    runtime_probe_ordering ordering{
        runtime_probe_ordering::visit_only};
};

/// Returns the expected probe counts and order for a revision 2 test.
[[nodiscard]] inline runtime_probe_contract
runtime_probe_contract_for(runtime_relation relation) {
    switch (relation) {
        case runtime_relation::natural_at_site:
            return {
                .abi_revision = 2,
                .expected_visit_count = 1,
                .expected_trigger_count = 0,
                .ordering = runtime_probe_ordering::visit_only,
            };
        case runtime_relation::injected_routes_to_site:
            return {
                .abi_revision = 2,
                .expected_visit_count = 1,
                .expected_trigger_count = 1,
                .ordering = runtime_probe_ordering::trigger_before_visit,
            };
        case runtime_relation::injected_after_site_success:
            return {
                .abi_revision = 2,
                .expected_visit_count = 1,
                .expected_trigger_count = 1,
                .ordering = runtime_probe_ordering::visit_before_trigger,
            };
    }
    throw std::invalid_argument("unknown runtime probe relation");
}

/// Links a runtime test probe to the error call in the safety report.
struct runtime_probe_inventory_entry {
    std::string sql_identity;
    std::string fault_key;
    std::string site_id;
    std::uint32_t site_revision{2};
    runtime_relation relation{runtime_relation::natural_at_site};
    std::string ledger_site_sha256;
    runtime_source_anchor site_anchor;
    runtime_source_anchor visit_anchor{
        .kind = runtime_anchor_kind::record_site_visit};
    std::optional<runtime_source_anchor> trigger_anchor;
    std::vector<call_path_step> transitive_call_path;
    runtime_probe_contract probe_contract;
};

/// Runtime test probes and hashes of the compiler inputs used to locate them.
struct runtime_probe_inventory {
    std::uint32_t schema_revision{1};
    compilation_profile profile{compilation_profile::production};
    std::uint32_t probe_abi_revision{2};
    std::string ledger_sha256;
    std::string compiler_facts_sha256;
    std::string sql_manifest_sha256;
    std::string source_list_sha256;
    std::string compile_database_sha256;
    std::string compile_graph_sha256;
    std::string pcm_manifest_sha256;
    std::string inventory_producer_executable_sha256;
    std::vector<runtime_probe_inventory_entry> entries;
};

/// One property that runtime tests must cover for a build  configuration.
struct coverage_key {
    compilation_profile profile{compilation_profile::production};
    coverage_dimension dimension{coverage_dimension::classification};
    std::string value;
};

/// Expected and observed cleanup and error results for one runtime test.
struct runtime_case {
    std::string case_id;
    compilation_profile profile{compilation_profile::test_indexes};
    std::string ledger_sha256;
    std::string sql_manifest_sha256;
    std::vector<coverage_key> coverage;
    std::vector<std::string> expected_destructor_sentinels;
    std::vector<std::string> observed_destructor_sentinels;
    std::vector<std::string> expected_cleanup_events;
    std::vector<std::string> observed_cleanup_events;
    std::vector<std::string> expected_postgres_cleanup_events;
    std::vector<std::string> observed_postgres_cleanup_events;
    std::uint64_t expected_fmgr_abort_count{};
    std::uint64_t observed_fmgr_abort_count{};
    std::string expected_sqlstate;
    std::string observed_sqlstate;
    std::string expected_message;
    std::string observed_message;
    std::string expected_recovery_postcondition;
    std::string observed_recovery_postcondition;
    bool exercised_terminal_severity{};
    std::string sql_identity;
    std::string site_id;
    std::string invocation_key;
    std::string fault_key;
    std::string active_dso_sha256;
    std::string producer_executable_sha256;
    std::string test_executable_sha256;
    std::string immutable_test_log_path;
    std::string immutable_test_log_sha256;
    std::string immutable_result_path;
    std::string immutable_result_sha256;
};

/// Runtime test results and required coverage for both build configurations.
struct runtime_matrix {
    std::uint32_t schema_revision{2};
    std::string production_ledger_sha256;
    std::string test_indexes_ledger_sha256;
    std::string production_sql_manifest_sha256;
    std::string test_indexes_sql_manifest_sha256;
    std::string runtime_policy_sha256;
    std::string production_dso_sha256;
    std::string test_indexes_dso_sha256;
    std::string runtime_producer_executable_sha256;
    std::string production_test_executable_sha256;
    std::string test_indexes_test_executable_sha256;
    std::vector<coverage_key> required_coverage;
    std::vector<runtime_case> cases;
};

/// One symbol occurrence reported by an ABI inspection tool.
struct dso_symbol_occurrence {
    std::string name;
    std::string binding;
    std::string symbol_type;
    std::string source_tool;
    bool defined{};
    bool dynamic_visible{};
};

/// Dynamic-library symbols used to check who exports each SQL function.
struct dso_inventory {
    std::vector<dso_symbol_occurrence> symbols;
};

/// Expected SQL function counts and symbols exported by each extension.
struct dso_contract {
    std::size_t main_identity_count{272};
    std::size_t main_symbol_count{270};
    std::size_t external_symbol_count{1};
    std::vector<std::string> shared_symbols;
    std::vector<std::string> external_symbols;
};

}  // namespace bioseq::codegen::safety
