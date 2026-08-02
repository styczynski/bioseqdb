module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

export module bioseq.codegen.safety:analyzer;

import :analyzer_model;
import :manifest;
import :model;
import :validation;
import bioseq.codegen.support;

export namespace bioseq::codegen::safety {

namespace analyzer_detail {

struct reachable_site {
    const flow_call_site* call{};
    const flow_call_target* target{};
    std::vector<const function_flow*> path;
    std::vector<postgres_severity> severities;
    bridge_kind active_bridge{bridge_kind::none};
    const flow_call_site* bridge_transition_call{};
    std::vector<flow_object> crossing_objects;
    std::vector<flow_object> capture_objects;
};

struct reachable_stop {
    const flow_call_site* call{};
    const flow_call_target* target{};
    const provenance_record* record{};
    std::vector<const function_flow*> path;
};

struct closure_result {
    std::vector<reachable_site> sites;
    std::vector<reachable_stop> audited_stops;
    std::set<std::string, std::less<>> reachable_function_usrs;
    std::string canonical_facts;
    analysis_completeness completeness{
        .template_instantiation_complete = true,
        .macro_expansion_complete = true,
        .cfg_complete = true,
        .lifetime_analysis_complete = true,
        .call_graph_fixed_point_complete = true,
    };
};

[[noreturn]] inline void analysis_failure(
    std::string_view context,
    std::string_view message) {
    throw std::runtime_error(
        "PostgreSQL ERROR-safety analysis rejected "
        + std::string{context} + ": " + std::string{message});
}

[[nodiscard]] inline bool lowercase_sha256(
    std::string_view value) noexcept {
    return value.size() == 64
        && std::ranges::all_of(
            value,
            [](unsigned char character) {
                return std::isdigit(character)
                    || (character >= 'a' && character <= 'f');
            });
}

[[nodiscard]] inline std::string unqualified_name(
    std::string_view value) {
    while (value.starts_with("::")) value.remove_prefix(2);
    const std::size_t arguments = value.find('(');
    if (arguments != std::string_view::npos) {
        value = value.substr(0, arguments);
    }
    const std::size_t separator = value.rfind("::");
    return std::string{
        separator == std::string_view::npos
            ? value
            : value.substr(separator + 2)};
}

[[nodiscard]] inline bool path_matches(
    std::string_view actual,
    std::string_view expected) {
    const std::string normalized_actual =
        std::filesystem::path{actual}.lexically_normal().generic_string();
    const std::string normalized_expected =
        std::filesystem::path{expected}.lexically_normal().generic_string();
    return normalized_actual == normalized_expected
        || (normalized_actual.size() > normalized_expected.size()
            && normalized_actual.ends_with(normalized_expected)
            && normalized_actual[
                normalized_actual.size()
                - normalized_expected.size() - 1] == '/')
        || (normalized_expected.size() > normalized_actual.size()
            && normalized_expected.ends_with(normalized_actual)
            && normalized_expected[
                normalized_expected.size()
                - normalized_actual.size() - 1] == '/');
}

[[nodiscard]] inline bool same_position(
    const source_position& left,
    const source_position& right) noexcept {
    return left.byte_offset == right.byte_offset
        && left.line == right.line
        && left.column == right.column;
}

[[nodiscard]] inline bool same_range(
    const source_range& left,
    const source_range& right) noexcept {
    return same_position(left.begin, right.begin)
        && same_position(left.end, right.end);
}

[[nodiscard]] inline bool range_contains(
    const source_range& outer,
    const source_range& inner) noexcept {
    return outer.begin.byte_offset <= inner.begin.byte_offset
        && outer.end.byte_offset >= inner.end.byte_offset;
}

[[nodiscard]] inline bool same_flow_object(
    const flow_object& left,
    const flow_object& right) noexcept {
    return left.stable_object_id == right.stable_object_id
        && left.declaration_usr == right.declaration_usr
        && left.identity_kind == right.identity_kind
        && left.canonical_type == right.canonical_type
        && left.origin == right.origin
        && left.declaration.source_file
            == right.declaration.source_file
        && same_range(
            left.declaration.span, right.declaration.span)
        && left.trivially_destructible
            == right.trivially_destructible;
}

[[nodiscard]] inline bool valid_range(
    const source_range& value) noexcept {
    return value.begin.line > 0 && value.begin.column > 0
        && value.end.line > 0 && value.end.column > 0
        && value.end.byte_offset >= value.begin.byte_offset
        && (value.end.line > value.begin.line
            || (value.end.line == value.begin.line
                && value.end.column >= value.begin.column));
}

[[nodiscard]] inline bool valid_located_range(
    const located_source_range& value) noexcept {
    return !value.source_file.empty() && valid_range(value.span);
}

[[nodiscard]] inline std::string position_fact(
    const source_position& value) {
    return std::to_string(value.byte_offset) + ":"
        + std::to_string(value.line) + ":"
        + std::to_string(value.column);
}

[[nodiscard]] inline std::string range_fact(
    const source_range& value) {
    return position_fact(value.begin) + "-"
        + position_fact(value.end);
}

[[nodiscard]] inline constexpr std::string_view origin_fact(
    crossing_origin value) noexcept {
    switch (value) {
        case crossing_origin::automatic_object: return "automatic";
        case crossing_origin::temporary: return "temporary";
        case crossing_origin::lambda_capture: return "lambda_capture";
        case crossing_origin::catch_parameter: return "catch_parameter";
        case crossing_origin::coroutine_state: return "coroutine_state";
    }
    return "unknown";
}

[[nodiscard]] inline const provenance_record* find_provenance(
    const provenance_bundle& bundle,
    std::string_view record_id) noexcept {
    const auto record = std::ranges::find(
        bundle.records,
        record_id,
        &provenance_record::record_id);
    return record == bundle.records.end()
        ? nullptr
        : std::addressof(*record);
}

struct postgresql_severity_authority {
    std::int64_t error_level{};
    std::int64_t fatal_level{};
    std::int64_t panic_level{};
};

[[nodiscard]] inline postgresql_severity_authority
require_postgresql_severity_authority(
    const provenance_bundle& provenance) {
    const provenance_record* record =
        find_provenance(provenance, "pg17.severity_levels");
    const std::set<postgres_severity> expected{
        postgres_severity::recoverable_error,
        postgres_severity::fatal,
        postgres_severity::panic,
    };
    if (record == nullptr
        || record->kind
            != provenance_kind::terminal_severity_seed
        || record->upstream_file
            != "src/include/utils/elog.h"
        || record->fragment_sha256
            != "b1c2030376e5858d484b65928856171de3"
               "b9f62957f2d26566795150027b5f6d"
        || std::set<postgres_severity>(
               record->severities.begin(),
               record->severities.end())
            != expected
        || record->severities.size() != expected.size()) {
        analysis_failure(
            "PostgreSQL severity authority",
            "numeric severity interpretation is not bound to "
            "the exact reviewed PostgreSQL-17 declaration");
    }
    // These numeric values are admitted only under the exact declaration
    // fragment above. A PostgreSQL revision changing them must first change
    // the reviewed provenance and this mapping together.
    return {
        .error_level = 20,
        .fatal_level = 21,
        .panic_level = 22,
    };
}

[[nodiscard]] inline const constant_integer_argument*
integer_argument(
    const flow_call_site& call,
    std::size_t ordinal) noexcept {
    const auto found = std::ranges::find(
        call.constant_integer_arguments,
        ordinal,
        &constant_integer_argument::ordinal);
    return found == call.constant_integer_arguments.end()
        ? nullptr
        : std::addressof(*found);
}

inline void validate_flow_object(
    const flow_object& object,
    std::string_view context) {
    if (object.stable_object_id.empty()
        || object.canonical_type.empty()
        || !valid_located_range(object.declaration)
        || (object.identity_kind
                == object_identity_kind::declaration_usr
            && (!object.declaration_usr
                || object.declaration_usr->empty()))
        || (object.identity_kind
                != object_identity_kind::declaration_usr
            && object.declaration_usr)) {
        analysis_failure(
            context,
            "live object/capture lacks stable identity, canonical "
            "type, or source range");
    }
}

inline void validate_call_fact(
    const flow_call_site& call,
    const function_flow& function) {
    if (call.caller_usr != function.stable_usr
        || call.stable_site_id.empty()
        || !valid_located_range(call.spelling)
        || !valid_located_range(call.expansion)
        || !path_matches(
            call.expansion.source_file, function.source_file)) {
        analysis_failure(
            function.qualified_name,
            "call fact lacks caller/site identity or valid "
            "spelling/expansion ranges");
    }
    if ((call.resolution == resolution_status::direct
         || call.resolution
             == resolution_status::resolved_indirect)
        && (!call.target_set_complete || call.targets.empty())) {
        analysis_failure(
            function.qualified_name,
            "resolved call does not carry a complete target set");
    }
    if (call.resolution == resolution_status::direct
        && call.targets.size() != 1) {
        analysis_failure(
            function.qualified_name,
            "direct call must have exactly one target");
    }
    if (call.resolution != resolution_status::direct
        && call.resolution != resolution_status::resolved_indirect
        && call.target_set_complete) {
        analysis_failure(
            function.qualified_name,
            "unresolved call claims a complete target set");
    }
    if (call.generated_boundary_tail
            != (call.bridge_transition
                == bridge_kind::generated_boundary_tail)
        || (call.bridge_transition == bridge_kind::none
            && call.analyzer_entered_bridge_callable)
        || (call.bridge_transition == bridge_kind::none)
            != !call.bridge_primitive_evidence.has_value()
        || (call.bridge_transition
                == bridge_kind::generated_boundary_tail
            && call.analyzer_entered_bridge_callable)) {
        analysis_failure(
            function.qualified_name,
            "bridge/tail transition flags are inconsistent");
    }
    std::set<std::pair<std::string, std::string>> target_identities;
    for (const flow_call_target& target : call.targets) {
        if (target.target_qualified_name.empty()
            || !target_identities.emplace(
                    target.target_usr,
                    target.target_qualified_name)
                    .second) {
            analysis_failure(
                function.qualified_name,
                "call target set has an empty or duplicate identity");
        }
        const bool external =
            target.domain == callee_domain::standard_library
            || target.domain == callee_domain::system_library
            || target.domain == callee_domain::compiler_builtin;
        if (external) {
            if (target.external_mode
                    == external_call_mode::not_external
                || !target.external_provenance
                || target.pg_no_longjmp_allowlist_evidence) {
                analysis_failure(
                    target.target_qualified_name,
                    "standard/system target lacks its external "
                    "analysis contract or carries PostgreSQL "
                    "allowlist evidence");
            }
        } else {
            if (target.external_mode
                    != external_call_mode::not_external
                || target.external_provenance
                || (target.domain
                        != callee_domain::postgresql_server
                    && target
                        .pg_no_longjmp_allowlist_evidence)) {
                analysis_failure(
                    target.target_qualified_name,
                    "non-library target carries an inapplicable "
                    "external contract or provenance");
            }
        }
    }
    for (const macro_expansion_record& macro : call.macro_chain) {
        if (macro.name.empty()
            || !valid_located_range(macro.invocation)
            || !valid_located_range(macro.definition)
            || !lowercase_sha256(macro.definition_sha256)) {
            analysis_failure(
                function.qualified_name,
                "macro expansion lacks located, hashed provenance");
        }
    }
    if (!call.macro_chain.empty()) {
        const macro_expansion_record& outermost =
            call.macro_chain.front();
        const macro_expansion_record& innermost =
            call.macro_chain.back();
        if (!path_matches(
                outermost.invocation.source_file,
                call.expansion.source_file)
            || !range_contains(
                outermost.invocation.span,
                call.expansion.span)
            || !path_matches(
                innermost.definition.source_file,
                call.spelling.source_file)
            || !range_contains(
                innermost.definition.span,
                call.spelling.span)) {
            analysis_failure(
                function.qualified_name,
                "macro chain does not connect the call spelling to "
                "its expansion");
        }
        for (std::size_t index = 1;
             index < call.macro_chain.size();
             ++index) {
            const macro_expansion_record& outer =
                call.macro_chain[index - 1];
            const macro_expansion_record& inner =
                call.macro_chain[index];
            if (!path_matches(
                    outer.definition.source_file,
                    inner.invocation.source_file)
                || !range_contains(
                    outer.definition.span,
                    inner.invocation.span)) {
                analysis_failure(
                    function.qualified_name,
                    "macro chain is not ordered outermost to "
                    "innermost");
            }
        }
    }
    std::set<std::size_t> argument_ordinals;
    for (const constant_integer_argument& argument :
         call.constant_integer_arguments) {
        if (!argument_ordinals.insert(argument.ordinal).second) {
            analysis_failure(
                function.qualified_name,
                "call fact duplicates a constant argument ordinal");
        }
    }
    for (const flow_object& object : call.live_objects) {
        validate_flow_object(object, call.stable_site_id);
    }
    for (const flow_object& object :
         call.callable_capture_objects) {
        validate_flow_object(object, call.stable_site_id);
    }
}

[[nodiscard]] inline bool macro_contains(
    const flow_call_site& call,
    std::string_view name) noexcept {
    return std::ranges::any_of(
        call.macro_chain,
        [name](const macro_expansion_record& macro) {
            return macro.name == name;
        });
}

[[nodiscard]] inline std::optional<
    std::vector<postgres_severity>>
postgres_error_severities(
    const flow_call_site& call,
    const flow_call_target& target,
    const postgresql_severity_authority& authority) {
    if (target.domain != callee_domain::postgresql_server) {
        return std::nullopt;
    }
    const std::string leaf =
        unqualified_name(target.target_qualified_name);
    const bool error_macro =
        macro_contains(call, "ereport")
        || macro_contains(call, "elog")
        || macro_contains(call, "ereport_domain")
        || macro_contains(call, "elog_start");
    if (leaf == "ReThrowError") {
        return std::vector{
            postgres_severity::recoverable_error};
    }
    if (leaf == "pg_re_throw") {
        return std::vector{
            postgres_severity::recoverable_error,
            postgres_severity::fatal};
    }
    if (leaf == "errfinish" || leaf == "errfinish_cold") {
        return std::vector{
            postgres_severity::recoverable_error};
    }
    if (leaf != "errstart" && leaf != "errstart_cold"
        && !error_macro) {
        if (!target.pg_no_longjmp_allowlist_evidence) {
            return std::vector{
                postgres_severity::recoverable_error};
        }
        return std::nullopt;
    }
    if (leaf != "errstart" && leaf != "errstart_cold") {
        return std::vector{
            postgres_severity::recoverable_error};
    }
    const constant_integer_argument* severity =
        integer_argument(call, 0);
    if (severity == nullptr || !severity->value) {
        return std::vector{postgres_severity::unresolved};
    }
    if (*severity->value < authority.error_level) {
        return std::vector<postgres_severity>{};
    }
    if (*severity->value == authority.error_level) {
        return std::vector{
            postgres_severity::recoverable_error};
    }
    if (*severity->value == authority.fatal_level) {
        return std::vector{postgres_severity::fatal};
    }
    if (*severity->value >= authority.panic_level) {
        return std::vector{postgres_severity::panic};
    }
    return std::vector{postgres_severity::unresolved};
}

[[nodiscard]] inline bool valid_runtime_fault_key(
    std::string_view key) noexcept {
    return !key.empty()
        && key.size() < 129
        && key.find('\0') == std::string_view::npos
        && std::ranges::all_of(
            key,
            [](const char value) {
                return (value >= 'a' && value <= 'z')
                    || (value >= '0' && value <= '9')
                    || value == '_'
                    || value == '.';
            });
}

[[nodiscard]] inline bool exact_runtime_probe_call(
    const flow_call_site& call,
    std::string_view target_name) noexcept {
    return call.resolution == resolution_status::direct
        && call.target_set_complete
        && call.targets.size() == 1
        && call.targets.front().domain
            == callee_domain::project
        && call.targets.front().target_qualified_name
            == target_name;
}

inline void validate_runtime_probe_facts(
    const compiler_analysis_facts& facts,
    const postgresql_severity_authority& severity_authority) {
    static constexpr std::string_view visit_name =
        "bioseq::pg_utils::safety_runtime_probe::"
        "record_site_visit";
    static constexpr std::string_view trigger_name =
        "bioseq::pg_utils::safety_runtime_probe::"
        "consume_fault";
    struct owned_call {
        const function_flow* owner{};
        const flow_call_site* call{};
    };
    std::map<std::string, owned_call, std::less<>>
        calls;
    std::set<std::string, std::less<>> unclaimed_visits;
    std::set<std::string, std::less<>> unclaimed_triggers;
    for (const function_flow& function : facts.functions) {
        for (const flow_call_site& call : function.calls) {
            if (!calls.emplace(
                    call.stable_site_id,
                    owned_call{
                        .owner = std::addressof(function),
                        .call = std::addressof(call)})
                    .second) {
                analysis_failure(
                    call.stable_site_id,
                    "stable call identity is duplicated before "
                    "runtime-probe validation");
            }
            if (exact_runtime_probe_call(call, visit_name)) {
                unclaimed_visits.insert(call.stable_site_id);
            } else if (
                exact_runtime_probe_call(call, trigger_name)) {
                unclaimed_triggers.insert(call.stable_site_id);
            }
        }
    }

    std::set<
        std::pair<std::string, std::string>,
        std::less<>>
        function_keys;
    for (const runtime_probe_fact& probe :
         facts.runtime_probes) {
        if (!valid_runtime_fault_key(probe.fault_key)
            || probe.site_call_stable_id.empty()
            || probe.visit_call_stable_id.empty()
            || runtime_relation_name(probe.relation)
                == "unknown"
            || (probe.relation
                    == runtime_relation::natural_at_site)
                != !probe.trigger_call_stable_id.has_value()) {
            analysis_failure(
                probe.fault_key,
                "runtime-probe fact has an invalid key, relation, "
                "or trigger shape");
        }
        std::vector<const owned_call*> referenced;
        for (const std::string* id : {
                 std::addressof(probe.site_call_stable_id),
                 std::addressof(probe.visit_call_stable_id)}) {
            const auto found = calls.find(*id);
            if (found == calls.end()) {
                analysis_failure(
                    *id,
                    "runtime-probe fact references an unknown "
                    "stable call identity");
            }
            referenced.push_back(
                std::addressof(found->second));
        }
        if (probe.trigger_call_stable_id) {
            const auto found = calls.find(
                *probe.trigger_call_stable_id);
            if (found == calls.end()) {
                analysis_failure(
                    *probe.trigger_call_stable_id,
                    "runtime-probe trigger references an unknown "
                    "stable call identity");
            }
            referenced.push_back(
                std::addressof(found->second));
        }
        if (std::ranges::any_of(
                referenced,
                [&](const owned_call* value) {
                    return value->owner->stable_usr
                        != referenced.front()
                               ->owner->stable_usr;
                })
            || probe.site_call_stable_id
                == probe.visit_call_stable_id
            || (probe.trigger_call_stable_id
                && (*probe.trigger_call_stable_id
                        == probe.site_call_stable_id
                    || *probe.trigger_call_stable_id
                        == probe.visit_call_stable_id))) {
            analysis_failure(
                probe.fault_key,
                "runtime-probe calls are not distinct calls in "
                "one exact function");
        }
        if (!function_keys.emplace(
                referenced.front()->owner->stable_usr,
                probe.fault_key).second) {
            analysis_failure(
                probe.fault_key,
                "one function contains duplicate runtime-probe "
                "facts for one key");
        }
        const flow_call_site& site =
            *referenced[0]->call;
        const flow_call_site& visit =
            *referenced[1]->call;
        if (!exact_runtime_probe_call(visit, visit_name)
            || unclaimed_visits.erase(
                   visit.stable_site_id)
                != 1) {
            analysis_failure(
                probe.visit_call_stable_id,
                "runtime-probe visit is not one exact unclaimed "
                "record_site_visit call");
        }
        if (probe.trigger_call_stable_id) {
            const flow_call_site& trigger =
                *referenced[2]->call;
            if (!exact_runtime_probe_call(
                    trigger, trigger_name)
                || unclaimed_triggers.erase(
                       trigger.stable_site_id)
                    != 1) {
                analysis_failure(
                    *probe.trigger_call_stable_id,
                    "runtime-probe trigger is not one exact "
                    "unclaimed consume_fault call");
            }
        }
        if (site.resolution != resolution_status::direct
            || !site.target_set_complete
            || site.targets.size() != 1) {
            analysis_failure(
                probe.site_call_stable_id,
                "runtime-probe site is not one exact direct call");
        }
        const std::optional<std::vector<postgres_severity>>
            severities =
                postgres_error_severities(
                    site,
                    site.targets.front(),
                    severity_authority);
        if (!severities || severities->size() != 1
            || severities->front()
                != postgres_severity::recoverable_error) {
            analysis_failure(
                probe.site_call_stable_id,
                "runtime-probe site is not one recoverable "
                "PostgreSQL ERROR call");
        }
    }
    if (!unclaimed_visits.empty()
        || !unclaimed_triggers.empty()) {
        analysis_failure(
            "runtime-probes",
            "reachable literal runtime-probe calls are absent "
            "from compiler-derived runtime facts");
    }
}

[[nodiscard]] inline const provenance_record&
validate_external_provenance(
    const flow_call_target& target,
    const provenance_bundle& provenance) {
    if (target.domain != callee_domain::standard_library
        && target.domain != callee_domain::system_library
        && target.domain != callee_domain::compiler_builtin) {
        analysis_failure(
            target.target_qualified_name,
            "external provenance is attached to a non-library call");
    }
    if (!target.external_provenance
        || target.external_provenance->record_id.empty()
        || target.external_provenance->source_file.empty()
        || target.external_provenance->library_version.empty()
        || !valid_range(
            target.external_provenance->declaration_span)
        || !lowercase_sha256(
            target.external_provenance->declaration_sha256)) {
        analysis_failure(
            target.target_qualified_name,
            "standard/system declaration lacks versioned hash provenance");
    }
    const provenance_record* record = find_provenance(
        provenance, target.external_provenance->record_id);
    const provenance_kind required_kind =
        target.domain == callee_domain::standard_library
        ? provenance_kind::standard_library_declaration
        : target.domain == callee_domain::system_library
        ? provenance_kind::system_library_declaration
        : provenance_kind::compiler_builtin_declaration;
    if (record == nullptr || record->kind != required_kind) {
        analysis_failure(
            target.target_qualified_name,
            "standard/system declaration provenance does not name "
            "a reviewed record of the correct domain");
    }
    if (target.external_provenance->source_file
            != record->upstream_file
        || !same_range(
            target.external_provenance->declaration_span,
            record->upstream_span)
        || target.external_provenance->declaration_sha256
            != record->fragment_sha256
        || target.external_provenance->library_version
            != record->library_version
        || record->target_usr != target.target_usr
        || record->target_qualified_name
            != target.target_qualified_name
        || record->authority_id.empty()
        || target.external_provenance->user_code_callback_free
            != record->user_code_callback_free
        || !record->severities.empty()) {
        analysis_failure(
            target.target_qualified_name,
            "standard/system declaration provenance is stale or "
            "does not match the reviewed record");
    }
    return *record;
}

[[nodiscard]] inline const provenance_record&
validate_pg_allowlist(
    const flow_call_target& target,
    const provenance_bundle& provenance) {
    if (target.domain != callee_domain::postgresql_server
        || !target.pg_no_longjmp_allowlist_evidence) {
        analysis_failure(
            target.target_qualified_name,
            "PostgreSQL no-longjmp evidence is attached to the "
            "wrong call domain");
    }
    const provenance_reference& reference =
        *target.pg_no_longjmp_allowlist_evidence;
    const provenance_record* record =
        find_provenance(provenance, reference.record_id);
    if (record == nullptr
        || record->kind != provenance_kind::pg_no_longjmp_leaf) {
        analysis_failure(
            target.target_qualified_name,
            "PostgreSQL no-longjmp evidence does not name a "
            "reviewed leaf");
    }
    if (reference.postgresql_revision
            != provenance.postgresql_revision
        || reference.upstream_file != record->upstream_file
        || !valid_range(reference.upstream_span)
        || !same_range(reference.upstream_span, record->upstream_span)
        || reference.fragment_sha256 != record->fragment_sha256
        || !lowercase_sha256(reference.fragment_sha256)
        || reference.rationale.empty()
        || reference.rationale != record->rationale
        || record->target_usr != target.target_usr
        || record->target_qualified_name
            != target.target_qualified_name
        || record->authority_id != provenance.postgresql_revision
        || !record->severities.empty()) {
        analysis_failure(
            target.target_qualified_name,
            "PostgreSQL no-longjmp evidence is stale or incomplete");
    }
    return *record;
}

inline void validate_bridge_transition(
    const flow_call_site& call,
    const provenance_bundle& provenance) {
    if (call.bridge_transition == bridge_kind::none) {
        if (call.bridge_primitive_evidence) {
            analysis_failure(
                call.stable_site_id,
                "non-bridge call carries bridge primitive evidence");
        }
        return;
    }
    if (!call.bridge_primitive_evidence
        || (call.resolution != resolution_status::direct
            && call.resolution
                != resolution_status::resolved_indirect)
        || call.targets.size() != 1) {
        analysis_failure(
            call.stable_site_id,
            "bridge transition lacks one exact resolved target and "
            "primitive evidence");
    }
    const bridge_transition_evidence& evidence =
        *call.bridge_primitive_evidence;
    const flow_call_target& target = call.targets.front();
    std::string expected_implementation;
    std::string expected_module;
    std::string expected_region;
    std::string expected_contract_id;
    std::string expected_contract_sha256;
    if (call.bridge_transition == bridge_kind::invoke_pg_as_cpp) {
        expected_implementation =
            "bioseq::pg_utils::invoke_pg_as_cpp";
        expected_module = "bioseq.pg_support:error_boundary";
        expected_region =
            "bioseq.pg_utils.error_bridge/invoke_pg_as_cpp";
        expected_contract_id =
            "bioseq.pg_utils.error_bridge/error_capture/v1";
        expected_contract_sha256 =
            "cbf583abe710257bf2a1999718c076fa"
            "2d70e203ae424bbba083eee0b4fcc83c";
    } else if (
        call.bridge_transition == bridge_kind::sql_call_context) {
        expected_implementation =
            "bioseq::pg_utils::invoke_pg_across_transactions_as_cpp";
        expected_module = "bioseq.pg_support:error_boundary";
        expected_region =
            "bioseq.pg_utils.error_bridge/"
            "invoke_pg_across_transactions_as_cpp";
        expected_contract_id =
            "bioseq.pg_utils.error_bridge/error_capture/v1";
        expected_contract_sha256 =
            "cbf583abe710257bf2a1999718c076fa"
            "2d70e203ae424bbba083eee0b4fcc83c";
    } else if (
        call.bridge_transition
        == bridge_kind::sql_subtransaction) {
        static const std::set<std::string, std::less<>>
            exact_regions{
                "bioseq::pg_utils::sql_subtransaction::begin",
                "bioseq::pg_utils::sql_subtransaction::commit",
                "bioseq::pg_utils::sql_subtransaction::rollback",
                "bioseq::pg_utils::sql_subtransaction::~sql_subtransaction",
            };
        if (!exact_regions.contains(
                evidence.implementation_qualified_name)) {
            analysis_failure(
                call.stable_site_id,
                "SQL subtransaction bridge implementation is not "
                "an exact reviewed region");
        }
        expected_implementation =
            evidence.implementation_qualified_name;
        expected_module = "bioseq.pg_sql_boundary";
        expected_region =
            "bioseq.pg_utils.sql_boundary.context/"
            + std::string{expected_implementation.substr(
                expected_implementation.rfind("::") + 2)};
    } else if (
        call.bridge_transition
        == bridge_kind::generated_boundary_tail) {
        expected_implementation =
            "bioseq::pg_utils::invoke_sql_boundary";
        expected_module = "bioseq.pg_sql_boundary";
        expected_region =
            "bioseq.pg_utils.sql_boundary/rethrow_tail";
    } else {
        analysis_failure(
            call.stable_site_id,
            "bridge kind has no exact protected implementation");
    }
    const std::string_view expected_record =
        call.bridge_transition
                == bridge_kind::generated_boundary_tail
        ? std::string_view{"pg17.rethrow_declarations"}
        : std::string_view{"pg17.pg_try_rethrow_macros"};
    const provenance_record* record =
        find_provenance(provenance, evidence.primitive.record_id);
    if (record == nullptr
        || record->record_id != expected_record
        || record->kind != provenance_kind::bridge_primitive
        || evidence.primitive.postgresql_revision
            != provenance.postgresql_revision
        || evidence.primitive.upstream_file
            != record->upstream_file
        || !same_range(
            evidence.primitive.upstream_span,
            record->upstream_span)
        || evidence.primitive.fragment_sha256
            != record->fragment_sha256
        || !lowercase_sha256(
            evidence.primitive.fragment_sha256)
        || evidence.primitive.rationale.empty()
        || evidence.primitive.rationale != record->rationale
        || evidence.target_usr.empty()
        || evidence.target_usr != target.target_usr
        || evidence.target_qualified_name
            != target.target_qualified_name
        || !valid_located_range(evidence.transition_callsite)
        || evidence.transition_callsite.source_file
            != call.expansion.source_file
        || !same_range(
            evidence.transition_callsite.span,
            call.expansion.span)
        || evidence.implementation_usr.empty()
        || evidence.implementation_qualified_name
            != expected_implementation
        || evidence.implementation_module != expected_module
        || !valid_located_range(
            evidence.implementation_definition)
        || !lowercase_sha256(
            evidence.implementation_body_sha256)
        || evidence.implementation_contract_id
            != expected_contract_id
        || evidence.implementation_contract_sha256
            != expected_contract_sha256
        || evidence.protected_region_id != expected_region) {
        analysis_failure(
            call.stable_site_id,
            "bridge transition is not joined to the pinned "
            "primitive, exact target, and callsite range");
    }
}

[[nodiscard]] inline std::string canonical_call_fact(
    const flow_call_site& call) {
    std::string result =
        call.stable_site_id + "\t" + call.caller_usr + "\t"
        + call.spelling.source_file + "\t"
        + range_fact(call.spelling.span) + "\t"
        + call.expansion.source_file + "\t"
        + range_fact(call.expansion.span)
        + "\t" + std::string{resolution_name(call.resolution)}
        + "\t"
        + (call.target_set_complete
               ? "targets-complete"
               : "targets-incomplete")
        + "\t" + std::string{bridge_name(call.bridge_transition)}
        + "\t"
        + (call.analyzer_entered_bridge_callable
               ? "entered"
               : "outside")
        + "\t" + (call.generated_boundary_tail ? "tail" : "ordinary");
    if (call.bridge_primitive_evidence) {
        const bridge_transition_evidence& evidence =
            *call.bridge_primitive_evidence;
        result += "\nB\t" + evidence.primitive.record_id + "\t"
            + evidence.primitive.postgresql_revision + "\t"
            + evidence.primitive.upstream_file + "\t"
            + range_fact(evidence.primitive.upstream_span) + "\t"
            + evidence.primitive.fragment_sha256 + "\t"
            + evidence.primitive.rationale + "\t"
            + evidence.target_usr + "\t"
            + evidence.target_qualified_name + "\t"
            + evidence.transition_callsite.source_file + "\t"
            + range_fact(evidence.transition_callsite.span) + "\t"
            + evidence.implementation_usr + "\t"
            + evidence.implementation_qualified_name + "\t"
            + evidence.implementation_module + "\t"
            + evidence.implementation_definition.source_file + "\t"
            + range_fact(evidence.implementation_definition.span)
            + "\t" + evidence.implementation_body_sha256 + "\t"
            + evidence.implementation_contract_id + "\t"
            + evidence.implementation_contract_sha256 + "\t"
            + evidence.protected_region_id;
    }
    std::vector<std::string> targets;
    targets.reserve(call.targets.size());
    for (const flow_call_target& target : call.targets) {
        std::string fact =
            target.target_usr + "\t"
            + target.target_qualified_name + "\t"
            + callee_domain_name(target.domain) + "\t"
            + external_call_mode_name(target.external_mode);
        if (target.external_provenance) {
            fact += "\tX\t"
                + target.external_provenance->record_id + "\t"
                + target.external_provenance->source_file + "\t"
                + range_fact(
                    target.external_provenance->declaration_span)
                + "\t"
                + target.external_provenance->declaration_sha256
                + "\t"
                + target.external_provenance->library_version
                + "\t"
                + (target.external_provenance
                           ->user_code_callback_free
                       ? "callback-free"
                       : "may-callback");
        }
        if (target.pg_no_longjmp_allowlist_evidence) {
            fact += "\tA\t"
                + target.pg_no_longjmp_allowlist_evidence->record_id
                + "\t"
                + target.pg_no_longjmp_allowlist_evidence
                    ->postgresql_revision
                + "\t"
                + target.pg_no_longjmp_allowlist_evidence
                    ->upstream_file
                + "\t"
                + range_fact(
                    target.pg_no_longjmp_allowlist_evidence
                        ->upstream_span)
                + "\t"
                + target.pg_no_longjmp_allowlist_evidence
                    ->fragment_sha256
                + "\t"
                + target.pg_no_longjmp_allowlist_evidence->rationale;
        }
        targets.push_back(std::move(fact));
    }
    std::ranges::sort(targets);
    for (const std::string& target : targets) {
        result += "\nT\t" + target;
    }
    for (const macro_expansion_record& macro : call.macro_chain) {
        result += "\nM\t" + macro.name + "\t"
            + macro.invocation.source_file + "\t"
            + range_fact(macro.invocation.span) + "\t"
            + macro.definition.source_file + "\t"
            + range_fact(macro.definition.span) + "\t"
            + macro.definition_sha256;
    }
    std::vector<std::string> arguments;
    arguments.reserve(call.constant_integer_arguments.size());
    for (const constant_integer_argument& argument :
         call.constant_integer_arguments) {
        arguments.push_back(
            std::to_string(argument.ordinal) + "\t"
            + (argument.value
                   ? std::to_string(*argument.value)
                   : "unresolved"));
    }
    std::ranges::sort(arguments);
    for (const std::string& argument : arguments) {
        result += "\nI\t" + argument;
    }
    const auto append_objects =
        [&result](
            std::string_view prefix,
            const std::vector<flow_object>& objects) {
            std::vector<std::string> facts;
            facts.reserve(objects.size());
            for (const flow_object& object : objects) {
                facts.push_back(
                    object.stable_object_id + "\t"
                    + object_identity_kind_name(
                        object.identity_kind)
                    + "\t"
                    + (object.declaration_usr
                           ? *object.declaration_usr
                           : "-")
                    + "\t"
                    + object.canonical_type + "\t"
                    + std::string{origin_fact(object.origin)} + "\t"
                    + object.declaration.source_file + "\t"
                    + range_fact(object.declaration.span) + "\t"
                    + (object.trivially_destructible
                           ? "trivial"
                           : "nontrivial"));
            }
            std::ranges::sort(facts);
            for (const std::string& fact : facts) {
                result += "\n" + std::string{prefix}
                    + "\t" + fact;
            }
        };
    append_objects("L", call.live_objects);
    append_objects("C", call.callable_capture_objects);
    return result;
}

[[nodiscard]] inline bool terminal_severity(
    const std::vector<postgres_severity>& severities) noexcept {
    return std::ranges::any_of(
        severities,
        [](postgres_severity value) {
            return value == postgres_severity::fatal
                || value == postgres_severity::panic;
        });
}

[[nodiscard]] inline const function_flow& require_wrapper_root(
    const std::multimap<
        std::string,
        const function_flow*,
        std::less<>>& by_qualified_name,
    const safety_manifest_owner& owner,
    std::string_view profile) {
    const auto [begin, end] =
        by_qualified_name.equal_range(owner.public_symbol);
    const function_flow* result = nullptr;
    for (auto iterator = begin; iterator != end; ++iterator) {
        const function_flow* candidate = iterator->second;
        if (!candidate->body_available) continue;
        if (result != nullptr) {
            analysis_failure(
                owner.public_symbol,
                "profile " + std::string{profile}
                    + " contains multiple generated wrapper definitions");
        }
        result = candidate;
    }
    if (result == nullptr) {
        analysis_failure(
            owner.public_symbol,
            "profile " + std::string{profile}
                + " has no generated Stage-B wrapper definition");
    }
    if (result->module_name.empty()
        || owner.generated_wrapper_module.empty()
        || result->module_name != owner.generated_wrapper_module) {
        analysis_failure(
            owner.public_symbol,
            "generated wrapper module differs from the SQL "
            "manifest owner");
    }
    return *result;
}

[[nodiscard]] inline const function_flow& require_owner_function(
    const compiler_analysis_facts& facts,
    const safety_manifest_overload& overload) {
    const function_flow* result = nullptr;
    for (const function_flow& function : facts.functions) {
        if (function.qualified_name != overload.cpp_declaration
            || !path_matches(
                function.source_file, overload.source_path)
            || overload.source_line
                < function.declaration_span.begin.line
            || overload.source_line
                > function.declaration_span.end.line) {
            continue;
        }
        if (result != nullptr
            && result->stable_usr != function.stable_usr) {
            analysis_failure(
                overload.sql_identity,
                "implementation declaration resolves multiple Clang USRs");
        }
        result = std::addressof(function);
    }
    if (result == nullptr || result->stable_usr.empty()) {
        analysis_failure(
            overload.sql_identity,
            "implementation declaration has no authoritative Clang USR");
    }
    return *result;
}

inline void require_complete_function(
    const function_flow& function,
    analysis_completeness& completeness) {
    std::vector<std::string> lifetime_ids =
        function.lifetime_object_ids;
    std::ranges::sort(lifetime_ids);
    const auto unique_end =
        std::unique(lifetime_ids.begin(), lifetime_ids.end());
    const bool unique_lifetime_ids =
        unique_end == lifetime_ids.end();
    std::string lifetime_material;
    for (const std::string& object_id : lifetime_ids) {
        lifetime_material += std::to_string(object_id.size())
            + ":" + object_id + "\n";
    }
    bool call_lifetimes_in_inventory = true;
    for (const flow_call_site& call : function.calls) {
        const auto check_objects =
            [&](const std::vector<flow_object>& objects) {
                for (const flow_object& object : objects) {
                    call_lifetimes_in_inventory &=
                        std::binary_search(
                            lifetime_ids.begin(),
                            lifetime_ids.end(),
                            object.stable_object_id);
                }
            };
        check_objects(call.live_objects);
        check_objects(call.callable_capture_objects);
    }
    completeness.template_instantiation_complete &=
        function.template_instantiation_complete;
    completeness.macro_expansion_complete &=
        function.macro_expansion_complete;
    completeness.cfg_complete &= function.cfg_complete;
    completeness.lifetime_analysis_complete &=
        function.lifetime_analysis_complete;
    completeness.cfg_complete &=
        function.implicit_call_edges_complete
        && lowercase_sha256(function.body_sha256)
        && lowercase_sha256(function.cfg_sha256)
        && function.cfg_block_count != 0
        && function.emitted_callsite_count
            == function.calls.size()
        && function.emitted_callsite_count
            == function.ast_callsite_count
                + function.cfg_implicit_callsite_count
        && unique_lifetime_ids
        && call_lifetimes_in_inventory
        && lowercase_sha256(
            function.lifetime_inventory_sha256)
        && function.lifetime_inventory_sha256
            == sha256_hex(lifetime_material);
    completeness.unresolved_indirect_targets.insert(
        completeness.unresolved_indirect_targets.end(),
        function.unresolved_indirect_targets.begin(),
        function.unresolved_indirect_targets.end());
    completeness.unavailable_bodies.insert(
        completeness.unavailable_bodies.end(),
        function.unavailable_bodies.begin(),
        function.unavailable_bodies.end());
    completeness.control_flow_assembly_sites.insert(
        completeness.control_flow_assembly_sites.end(),
        function.control_flow_assembly_sites.begin(),
        function.control_flow_assembly_sites.end());
    if (!function.body_available) {
        completeness.unavailable_bodies.push_back(
            function.qualified_name);
    }
}

[[nodiscard]] inline closure_result compute_closure(
    const function_flow& root,
    const std::map<std::string, const function_flow*, std::less<>>&
        functions_by_usr,
    const provenance_bundle& provenance,
    const postgresql_severity_authority& severity_authority) {
    closure_result result;
    std::set<std::string, std::less<>> canonicalized_functions;
    std::set<
        std::pair<std::string, std::string>,
        std::less<>>
        canonicalized_calls;
    struct seen_context {
        bool entered{};
        std::set<std::string, std::less<>> crossing_ids;
        std::set<std::string, std::less<>> capture_ids;
    };
    std::map<
        std::tuple<std::string, bridge_kind, std::string>,
        seen_context>
        seen_contexts;
    std::map<std::string, std::size_t, std::less<>> site_by_key;
    std::map<std::string, std::size_t, std::less<>> stop_by_key;
    std::map<std::string, flow_object, std::less<>>
        objects_by_identity;
    std::map<std::string, std::string, std::less<>>
        callsite_owner_by_identity;

    const auto append_unique_objects =
        [](std::vector<flow_object>& destination,
           const std::vector<flow_object>& incoming,
           std::string_view context) {
            for (const flow_object& object : incoming) {
                const auto existing = std::ranges::find(
                    destination,
                    object.stable_object_id,
                    &flow_object::stable_object_id);
                if (existing == destination.end()) {
                    destination.push_back(object);
                } else if (!same_flow_object(*existing, object)) {
                    analysis_failure(
                        context,
                        "one stable object identity has conflicting "
                        "lifetime facts");
                }
            }
            std::ranges::sort(
                destination,
                {},
                &flow_object::stable_object_id);
        };

    std::function<void(
        const function_flow&,
        bridge_kind,
        const flow_call_site*,
        std::vector<flow_object>,
        std::vector<flow_object>,
        std::vector<const function_flow*>)>
        visit;
    visit =
        [&](const function_flow& function,
            bridge_kind active_bridge,
            const flow_call_site* active_bridge_transition,
            std::vector<flow_object> crossing_objects,
            std::vector<flow_object> capture_objects,
            std::vector<const function_flow*> path) {
            result.reachable_function_usrs.insert(
                function.stable_usr);
            const std::tuple context_key{
                function.stable_usr,
                active_bridge,
                active_bridge_transition == nullptr
                    ? std::string{}
                    : active_bridge_transition->stable_site_id};
            seen_context& seen = seen_contexts[context_key];
            bool adds_information = !seen.entered;
            seen.entered = true;
            for (const flow_object& object : crossing_objects) {
                adds_information |=
                    seen.crossing_ids.insert(
                        object.stable_object_id).second;
            }
            for (const flow_object& object : capture_objects) {
                adds_information |=
                    seen.capture_ids.insert(
                        object.stable_object_id).second;
            }
            if (!adds_information) {
                return;
            }

            if (canonicalized_functions.insert(
                    function.stable_usr).second) {
                require_complete_function(
                    function, result.completeness);
                result.canonical_facts +=
                    "\nF\t" + function.stable_usr + "\t"
                    + function.qualified_name + "\t"
                    + function.source_file + "\t"
                    + function.module_name + "\t"
                    + range_fact(function.declaration_span) + "\t"
                    + range_fact(function.body_span) + "\t"
                    + (function.body_available
                           ? "body"
                           : "declaration")
                    + "\t"
                    + (function.template_instantiation_complete
                           ? "templates-complete"
                           : "templates-incomplete")
                    + "\t"
                    + (function.macro_expansion_complete
                           ? "macros-complete"
                           : "macros-incomplete")
                    + "\t"
                    + (function.cfg_complete
                           ? "cfg-complete"
                           : "cfg-incomplete")
                    + "\t"
                    + (function.lifetime_analysis_complete
                           ? "lifetimes-complete"
                           : "lifetimes-incomplete")
                    + "\t"
                    + (function.implicit_call_edges_complete
                           ? "implicit-edges-complete"
                           : "implicit-edges-incomplete")
                    + "\t" + function.body_sha256
                    + "\t" + function.cfg_sha256
                    + "\t"
                    + std::to_string(function.cfg_block_count)
                    + "\t"
                    + std::to_string(function.cfg_element_count)
                    + "\t"
                    + std::to_string(function.ast_callsite_count)
                    + "\t"
                    + std::to_string(
                        function.cfg_implicit_callsite_count)
                    + "\t"
                    + std::to_string(
                        function.emitted_callsite_count)
                    + "\t"
                    + function.lifetime_inventory_sha256;
                for (const std::string& object_id :
                     function.lifetime_object_ids) {
                    result.canonical_facts +=
                        "\nO\t" + function.stable_usr + "\t"
                        + object_id;
                }
            }
            path.push_back(std::addressof(function));
            std::vector<const flow_call_site*> calls;
            calls.reserve(function.calls.size());
            std::set<std::string, std::less<>> callsite_ids;
            for (const flow_call_site& call : function.calls) {
                validate_call_fact(call, function);
                const auto [site_owner, inserted_site] =
                    callsite_owner_by_identity.emplace(
                        call.stable_site_id, function.stable_usr);
                if (!inserted_site
                    && site_owner->second != function.stable_usr) {
                    analysis_failure(
                        call.stable_site_id,
                        "stable call-site identity is reused by "
                        "multiple functions");
                }
                const auto register_object =
                    [&](const flow_object& object) {
                        const auto [existing, inserted] =
                            objects_by_identity.emplace(
                                object.stable_object_id, object);
                        if (!inserted
                            && !same_flow_object(
                                existing->second, object)) {
                            analysis_failure(
                                call.stable_site_id,
                                "one stable object identity has "
                                "conflicting lifetime facts");
                        }
                    };
                for (const flow_object& object :
                     call.live_objects) {
                    register_object(object);
                }
                for (const flow_object& object :
                     call.callable_capture_objects) {
                    register_object(object);
                }
                if (!callsite_ids.insert(
                        call.stable_site_id).second) {
                    analysis_failure(
                        function.qualified_name,
                        "function facts duplicate a stable call-site "
                        "identity");
                }
                calls.push_back(std::addressof(call));
            }
            std::ranges::sort(
                calls,
                [](const flow_call_site* left,
                   const flow_call_site* right) {
                    return std::tuple{
                        left->expansion.span.begin.byte_offset,
                        left->stable_site_id,
                        left->expansion.source_file}
                        < std::tuple{
                            right->expansion.span.begin.byte_offset,
                            right->stable_site_id,
                            right->expansion.source_file};
                });
            for (const flow_call_site* call : calls) {
                if (canonicalized_calls.emplace(
                        function.stable_usr,
                        call->stable_site_id).second) {
                    result.canonical_facts +=
                        "\n" + canonical_call_fact(*call);
                }
                if (call->resolution
                        != resolution_status::direct
                    && call->resolution
                        != resolution_status::resolved_indirect) {
                    if (call->resolution
                        == resolution_status::unresolved_indirect) {
                        result.completeness
                            .unresolved_indirect_targets.push_back(
                                call->stable_site_id);
                    } else if (call->resolution
                               == resolution_status::unavailable_body) {
                        result.completeness.unavailable_bodies.push_back(
                            call->stable_site_id);
                    } else {
                        result.completeness
                            .control_flow_assembly_sites.push_back(
                                call->stable_site_id);
                    }
                    continue;
                }

                bridge_kind next_bridge = active_bridge;
                const flow_call_site* next_bridge_transition =
                    active_bridge_transition;
                std::vector<flow_object> next_crossings =
                    crossing_objects;
                std::vector<flow_object> next_captures =
                    capture_objects;
                if (call->bridge_transition != bridge_kind::none) {
                    validate_bridge_transition(*call, provenance);
                    if (!call->analyzer_entered_bridge_callable
                        && call->bridge_transition
                            != bridge_kind::generated_boundary_tail
                        && call->bridge_transition
                            != bridge_kind::sql_subtransaction) {
                        analysis_failure(
                            call->stable_site_id,
                            "bridge transition does not enter its "
                            "callable");
                    }
                    next_bridge = call->bridge_transition;
                    next_bridge_transition = call;
                    if (call->bridge_transition
                        == bridge_kind::generated_boundary_tail) {
                        if (!call->callable_capture_objects.empty()) {
                            analysis_failure(
                                call->stable_site_id,
                                "generated boundary tail carries "
                                "callable captures");
                        }
                        append_unique_objects(
                            next_crossings,
                            call->live_objects,
                            call->stable_site_id);
                    } else if (
                        call->bridge_transition
                        == bridge_kind::sql_subtransaction) {
                        next_crossings =
                            call->live_objects;
                        next_captures.clear();
                    } else {
                        next_crossings.clear();
                        next_captures =
                            call->callable_capture_objects;
                    }
                } else {
                    if (call->analyzer_entered_bridge_callable
                        || !call->callable_capture_objects.empty()) {
                        analysis_failure(
                            call->stable_site_id,
                            "callable entry/captures lack a bridge "
                            "transition");
                    }
                    append_unique_objects(
                        next_crossings,
                        call->live_objects,
                        call->stable_site_id);
                }

                std::vector<const flow_call_target*> targets;
                targets.reserve(call->targets.size());
                for (const flow_call_target& target : call->targets) {
                    targets.push_back(std::addressof(target));
                }
                std::ranges::sort(
                    targets,
                    [](const flow_call_target* left,
                       const flow_call_target* right) {
                        return std::tie(
                                   left->target_usr,
                                   left->target_qualified_name)
                            < std::tie(
                                   right->target_usr,
                                   right->target_qualified_name);
                    });
                for (const flow_call_target* target_pointer : targets) {
                    const flow_call_target& target = *target_pointer;
                    const provenance_record* audited_record = nullptr;
                    if (target.pg_no_longjmp_allowlist_evidence) {
                        audited_record = std::addressof(
                            validate_pg_allowlist(
                                target, provenance));
                    }
                    if (target.domain
                            == callee_domain::standard_library
                        || target.domain
                            == callee_domain::system_library
                        || target.domain
                            == callee_domain::compiler_builtin) {
                        const provenance_record& external_record =
                            validate_external_provenance(
                                target, provenance);
                        if (target.external_mode
                                == external_call_mode::
                                    audited_no_user_code) {
                            audited_record =
                                std::addressof(external_record);
                        }
                    }
                    if (audited_record != nullptr) {
                        const std::string stop_key =
                            call->stable_site_id + "\n"
                            + target.target_usr + "\n"
                            + target.target_qualified_name;
                        if (!stop_by_key.contains(stop_key)) {
                            stop_by_key.emplace(
                                stop_key,
                                result.audited_stops.size());
                            result.audited_stops.push_back({
                                .call = call,
                                .target = std::addressof(target),
                                .record = audited_record,
                                .path = path,
                            });
                        }
                    }
                    if (const auto severities =
                            postgres_error_severities(
                                *call,
                                target,
                                severity_authority)) {
                        if (terminal_severity(*severities)) {
                            analysis_failure(
                                target.target_qualified_name,
                                "FATAL/PANIC is reachable from "
                                    + root.qualified_name);
                        }
                        if (*severities
                            == std::vector{
                                postgres_severity::unresolved}) {
                            analysis_failure(
                                target.target_qualified_name,
                                "PostgreSQL severity is not a "
                                "compile-time constant");
                        }
                        if (!severities->empty()) {
                            const std::string site_key =
                                call->stable_site_id + "\n"
                                + target.target_usr + "\n"
                                + target.target_qualified_name;
                            const auto existing =
                                site_by_key.find(site_key);
                            if (existing == site_by_key.end()) {
                                site_by_key.emplace(
                                    site_key,
                                    result.sites.size());
                                result.sites.push_back({
                                    .call = call,
                                    .target = std::addressof(target),
                                    .path = path,
                                    .severities = *severities,
                                    .active_bridge = next_bridge,
                                    .bridge_transition_call =
                                        next_bridge_transition,
                                    .crossing_objects =
                                        next_crossings,
                                    .capture_objects =
                                        next_captures,
                                });
                            } else {
                                reachable_site& site =
                                    result.sites[existing->second];
                                if (site.active_bridge
                                        == next_bridge
                                    && ((site.bridge_transition_call
                                                == nullptr
                                            && next_bridge_transition
                                                == nullptr)
                                        || (site.bridge_transition_call
                                                != nullptr
                                            && next_bridge_transition
                                                != nullptr
                                            && site
                                                    .bridge_transition_call
                                                    ->stable_site_id
                                                == next_bridge_transition
                                                    ->stable_site_id))) {
                                    append_unique_objects(
                                        site.crossing_objects,
                                        next_crossings,
                                        call->stable_site_id);
                                    append_unique_objects(
                                        site.capture_objects,
                                        next_captures,
                                        call->stable_site_id);
                                } else if (
                                    site.active_bridge
                                        == bridge_kind::none
                                    || next_bridge
                                        == bridge_kind::none) {
                                    const auto nontrivial =
                                        [](const flow_object& object) {
                                            return !object
                                                .trivially_destructible;
                                        };
                                    if (std::ranges::any_of(
                                            site.crossing_objects,
                                            nontrivial)
                                        || std::ranges::any_of(
                                            site.capture_objects,
                                            nontrivial)
                                        || std::ranges::any_of(
                                            next_crossings,
                                            nontrivial)
                                        || std::ranges::any_of(
                                            next_captures,
                                            nontrivial)) {
                                        analysis_failure(
                                            call->stable_site_id,
                                            "one PostgreSQL leaf has "
                                            "an unsafe direct or "
                                            "bridged path");
                                    }
                                    if (next_bridge
                                        == bridge_kind::none) {
                                        site.crossing_objects =
                                            next_crossings;
                                        site.capture_objects =
                                            next_captures;
                                        site.path = path;
                                    }
                                    site.active_bridge =
                                        bridge_kind::none;
                                    site.bridge_transition_call =
                                        nullptr;
                                } else {
                                    analysis_failure(
                                        call->stable_site_id,
                                        "one PostgreSQL leaf is "
                                        "reachable through distinct "
                                        "bridge contracts");
                                }
                            }
                        }
                        continue;
                    }

                    if (target.pg_no_longjmp_allowlist_evidence) {
                        continue;
                    }
                    if (target.domain
                            == callee_domain::standard_library
                        || target.domain
                            == callee_domain::system_library
                        || target.domain
                            == callee_domain::compiler_builtin) {
                        if (target.external_mode
                                == external_call_mode::
                                    audited_no_user_code) {
                            if (!target.external_provenance
                                    ->user_code_callback_free) {
                                analysis_failure(
                                    target.target_qualified_name,
                                    "external declaration was "
                                    "stopped without callback-free "
                                    "provenance");
                            }
                            continue;
                        }
                        if (target.external_mode
                            != external_call_mode::analyze_body) {
                            result.completeness.unavailable_bodies
                                .push_back(
                                    target.target_qualified_name);
                            continue;
                        }
                    }
                    if (target.domain == callee_domain::unknown) {
                        result.completeness.unavailable_bodies
                            .push_back(
                                target.target_qualified_name);
                        continue;
                    }
                    const auto definition =
                        functions_by_usr.find(target.target_usr);
                    if (target.target_usr.empty()
                        || definition == functions_by_usr.end()
                        || !definition->second->body_available) {
                        result.completeness.unavailable_bodies
                            .push_back(
                                target.target_qualified_name);
                        continue;
                    }
                    if (definition->second->qualified_name
                        != target.target_qualified_name) {
                        analysis_failure(
                            target.target_qualified_name,
                            "resolved target USR names a different "
                            "qualified declaration");
                    }
                    visit(
                        *definition->second,
                        next_bridge,
                        next_bridge_transition,
                        next_crossings,
                        next_captures,
                        path);
                }
            }
        };
    visit(
        root,
        bridge_kind::none,
        nullptr,
        {},
        {},
        {});
    auto normalize = [](std::vector<std::string>& values) {
        std::ranges::sort(values);
        values.erase(
            std::unique(values.begin(), values.end()),
            values.end());
    };
    normalize(result.completeness.unresolved_indirect_targets);
    normalize(result.completeness.unavailable_bodies);
    normalize(result.completeness.control_flow_assembly_sites);
    result.completeness.call_graph_fixed_point_complete =
        result.completeness.unresolved_indirect_targets.empty()
        && result.completeness.unavailable_bodies.empty()
        && result.completeness.control_flow_assembly_sites.empty();
    if (!result.completeness.template_instantiation_complete
        || !result.completeness.macro_expansion_complete
        || !result.completeness.cfg_complete
        || !result.completeness.lifetime_analysis_complete
        || !result.completeness.call_graph_fixed_point_complete) {
        analysis_failure(
            root.qualified_name,
            "transitive call/CFG/lifetime closure is incomplete");
    }
    return result;
}

[[nodiscard]] inline crossing_type ledger_crossing(
    const flow_object& object) {
    return {
        .canonical_type = object.canonical_type,
        .origin = object.origin,
        .source_file = object.declaration.source_file,
        .declaration_span = object.declaration.span,
        .trivially_destructible = object.trivially_destructible,
        .crosses_postgres_longjmp = true,
    };
}

[[nodiscard]] inline std::vector<crossing_type>
ledger_crossings(const std::vector<flow_object>& objects) {
    std::vector<crossing_type> result;
    result.reserve(objects.size());
    for (const flow_object& object : objects) {
        result.push_back(ledger_crossing(object));
    }
    return result;
}

[[nodiscard]] inline site_classification classify_site(
    const reachable_site& site) {
    const flow_call_site& call = *site.call;
    const flow_call_target& target = *site.target;
    const auto nontrivial = [](const flow_object& object) {
        return !object.trivially_destructible;
    };
    if (call.generated_boundary_tail) {
        if (site.active_bridge
                != bridge_kind::generated_boundary_tail
            || call.analyzer_entered_bridge_callable
            || std::ranges::any_of(
                site.crossing_objects, nontrivial)
            || !site.capture_objects.empty()) {
            analysis_failure(
                target.target_qualified_name,
                "generated boundary tail is not lifetime-free");
        }
        return site_classification::terminal_after_unwind;
    }
    if (site.active_bridge == bridge_kind::invoke_pg_as_cpp) {
        if (std::ranges::any_of(
                site.crossing_objects, nontrivial)
            || std::ranges::any_of(
                site.capture_objects, nontrivial)) {
            analysis_failure(
                target.target_qualified_name,
                "invoke_pg_as_cpp callable has a nontrivial crossing "
                "object or capture");
        }
        return site_classification::bridged;
    }
    if (site.active_bridge == bridge_kind::sql_subtransaction
        || site.active_bridge == bridge_kind::sql_call_context) {
        if (std::ranges::any_of(
                site.crossing_objects, nontrivial)
            || std::ranges::any_of(
                site.capture_objects, nontrivial)) {
            analysis_failure(
                target.target_qualified_name,
                "specialized PostgreSQL bridge has a nontrivial "
                "crossing object or capture");
        }
        return site_classification::specialized_bridge;
    }
    if (site.active_bridge != bridge_kind::none
        || std::ranges::any_of(
            site.crossing_objects, nontrivial)
        || std::ranges::any_of(
            site.capture_objects, nontrivial)) {
        analysis_failure(
            target.target_qualified_name,
            "direct PostgreSQL ERROR site is not lifetime-free");
    }
    return site_classification::lifetime_free_direct;
}

[[nodiscard]] inline std::vector<std::string>
converter_families(const safety_manifest_overload& overload) {
    std::set<std::string, std::less<>> result;
    const auto add = [&result](std::string_view key) {
        const std::size_t direction = key.find('.');
        const std::string_view body =
            direction == std::string_view::npos
            ? key
            : key.substr(direction + 1);
        if (body.starts_with("aggregate_state")) {
            result.insert("aggregate_state");
        } else if (body.starts_with("returns_table[")
                   || body.starts_with("setof")) {
            result.insert("set_returning_function");
        } else if (body.starts_with("vector_")
                   || body.starts_with("vector.")) {
            result.insert("vector");
        } else if (body == "bioseq.seq"
                   || body.starts_with("bioseq.seq.")
                   || body.starts_with("bioseq.seq[")
                   || body == "bioseq.letter"
                   || body.starts_with("bioseq.letter.")
                   || body.starts_with("bioseq.letter[")) {
            result.insert("base_type");
        } else {
            result.insert("scalar");
        }
        if (key.contains("[any]")
            || body.starts_with("polymorphic.")
            || body.starts_with("polymorphic[")) {
            result.insert("polymorphic");
        }
    };
    for (const std::string& key :
         overload.argument_converter_keys) {
        add(key);
    }
    add(overload.result_converter_key);
    return {result.begin(), result.end()};
}

[[nodiscard]] inline std::vector<std::string> owner_families(
    std::string_view identity) {
    const std::size_t arguments = identity.find('(');
    const std::string owner{
        arguments == std::string_view::npos
            ? identity
            : identity.substr(0, arguments)};
    constexpr std::string_view required[] = {
        "bioseq.reap_orphan_seq_sub_tables",
        "bioseq.vacuum_seq_storage",
        "bioseq_internal.add_to_extension_secdef",
        "bioseq_internal.bioseq_ensure_am_depends_catalog",
        "pg_catalog.alter_bioseq_table_reset",
        "pg_catalog.alter_bioseq_table_set",
    };
    return std::ranges::contains(required, owner)
        ? std::vector<std::string>{owner}
        : std::vector<std::string>{};
}

[[nodiscard]] inline std::vector<std::string> cleanup_phases(
    const reachable_site& site) {
    std::set<std::string, std::less<>> result;
    switch (site.active_bridge) {
        case bridge_kind::invoke_pg_as_cpp:
        case bridge_kind::sql_call_context:
            result.insert("memory_context_restore");
            result.insert("resource_owner_restore");
            break;
        case bridge_kind::sql_subtransaction:
            result.insert("subtransaction_abort");
            result.insert("subtransaction_begin");
            result.insert("subtransaction_commit");
            break;
        case bridge_kind::none:
        case bridge_kind::generated_boundary_tail:
            break;
    }
    for (const function_flow* step : site.path) {
        const std::string_view name = step->qualified_name;
        if (step->module_name == "bioseq.pg_sql_boundary"
            && (name
                    == "bioseq::pg_utils::scoped_spi_session::connect"
                || name
                    == "bioseq::pg_utils::scoped_spi_session::finish")) {
            result.insert("spi_connect");
            result.insert("spi_finish");
        }
    }
    return {result.begin(), result.end()};
}

[[nodiscard]] inline error_site make_error_site(
    const reachable_site& reachable,
    std::string_view identity,
    compilation_profile profile,
    const safety_manifest_overload& overload) {
    const flow_call_site& call = *reachable.call;
    const flow_call_target& target = *reachable.target;
    error_site result{
        .source_file = call.expansion.source_file,
        .expanded_source_span = call.expansion.span,
        .callee_or_macro =
            !call.macro_chain.empty()
            ? call.macro_chain.front().name
            : target.target_qualified_name,
        .possible_severities = reachable.severities,
        .severity_constant_propagated = true,
        .resolution = call.resolution,
        .classification = classify_site(reachable),
        .bridge = reachable.active_bridge,
        .crossing_types =
            ledger_crossings(reachable.crossing_objects),
        .bridge_callable_crossing_types =
            ledger_crossings(reachable.capture_objects),
        .analyzer_entered_bridge_callable =
            reachable.active_bridge
                == bridge_kind::invoke_pg_as_cpp
            || reachable.active_bridge
                == bridge_kind::sql_call_context,
        .generated_boundary_tail = call.generated_boundary_tail,
        .converter_families = converter_families(overload),
        .owner_families = owner_families(identity),
        .cleanup_phases = cleanup_phases(reachable),
    };
    if (reachable.active_bridge == bridge_kind::none) {
        if (reachable.bridge_transition_call != nullptr) {
            analysis_failure(
                call.stable_site_id,
                "direct error site retained bridge transition "
                "evidence");
        }
    } else {
        if (reachable.bridge_transition_call == nullptr
            || !reachable.bridge_transition_call
                    ->bridge_primitive_evidence) {
            analysis_failure(
                call.stable_site_id,
                "bridged error site lost its primitive evidence");
        }
        const bridge_transition_evidence& evidence =
            *reachable.bridge_transition_call
                 ->bridge_primitive_evidence;
        result.bridge_evidence = bridge_transition_proof{
            .primitive = evidence.primitive,
            .target_usr = evidence.target_usr,
            .target_qualified_name =
                evidence.target_qualified_name,
            .transition_source_file =
                evidence.transition_callsite.source_file,
            .transition_source_span =
                evidence.transition_callsite.span,
            .implementation_usr = evidence.implementation_usr,
            .implementation_qualified_name =
                evidence.implementation_qualified_name,
            .implementation_module =
                evidence.implementation_module,
            .implementation_source_file =
                evidence.implementation_definition.source_file,
            .implementation_source_span =
                evidence.implementation_definition.span,
            .implementation_body_sha256 =
                evidence.implementation_body_sha256,
            .implementation_contract_id =
                evidence.implementation_contract_id,
            .implementation_contract_sha256 =
                evidence.implementation_contract_sha256,
            .protected_region_id = evidence.protected_region_id,
        };
    }
    const std::string site_material =
        std::string{profile_name(profile)} + "\n"
        + std::string{identity} + "\n"
        + call.stable_site_id + "\n"
        + target.target_usr + "\n"
        + target.target_qualified_name;
    result.site_id = std::string{profile_name(profile)}
        + ":" + sha256_hex(site_material).substr(0, 24);
    result.transitive_call_path.reserve(reachable.path.size());
    for (const function_flow* step : reachable.path) {
        result.transitive_call_path.push_back({
            .usr = step->stable_usr,
            .qualified_declaration = step->qualified_name,
            .source_file = step->source_file,
            .source_span = step->declaration_span,
        });
    }
    return result;
}

[[nodiscard]] inline audited_stop make_audited_stop(
    const reachable_stop& reachable,
    std::string_view identity,
    compilation_profile profile,
    std::string_view postgresql_revision) {
    const flow_call_site& call = *reachable.call;
    const flow_call_target& target = *reachable.target;
    const provenance_record& record = *reachable.record;
    audited_stop_domain domain{
        audited_stop_domain::postgresql_server};
    switch (target.domain) {
        case callee_domain::postgresql_server:
            domain = audited_stop_domain::postgresql_server;
            break;
        case callee_domain::standard_library:
            domain = audited_stop_domain::standard_library;
            break;
        case callee_domain::system_library:
            domain = audited_stop_domain::system_library;
            break;
        case callee_domain::compiler_builtin:
            domain = audited_stop_domain::compiler_builtin;
            break;
        default:
            analysis_failure(
                target.target_qualified_name,
                "audited stop has an unsupported call domain");
    }
    const std::string stop_material =
        std::string{profile_name(profile)} + "\n"
        + std::string{identity} + "\n"
        + call.stable_site_id + "\n"
        + target.target_usr + "\n"
        + target.target_qualified_name + "\n"
        + record.record_id;
    audited_stop result{
        .stop_id = std::string{profile_name(profile)}
            + ":stop:"
            + sha256_hex(stop_material).substr(0, 24),
        .source_file = call.expansion.source_file,
        .expanded_source_span = call.expansion.span,
        .target_usr = target.target_usr,
        .target_qualified_name = target.target_qualified_name,
        .resolution = call.resolution,
        .domain = domain,
        .evidence = {
            .record_id = record.record_id,
            .postgresql_revision =
                std::string{postgresql_revision},
            .upstream_file = record.upstream_file,
            .upstream_span = record.upstream_span,
            .fragment_sha256 = record.fragment_sha256,
            .rationale = record.rationale,
        },
        .library_version = record.library_version,
        .user_code_callback_free =
            record.user_code_callback_free,
    };
    result.transitive_call_path.reserve(reachable.path.size());
    for (const function_flow* step : reachable.path) {
        result.transitive_call_path.push_back({
            .usr = step->stable_usr,
            .qualified_declaration = step->qualified_name,
            .source_file = step->source_file,
            .source_span = step->declaration_span,
        });
    }
    return result;
}

}  // namespace analyzer_detail

[[nodiscard]] inline safety_ledger analyze_postgres_error_safety(
    const safety_manifest& manifest,
    const compiler_analysis_facts& facts,
    const provenance_bundle& provenance,
    std::string provenance_bundle_sha256,
    std::string compiler_facts_sha256,
    std::string generator_revision,
    const ledger_policy& policy = {}) {
    validate_provenance_bundle(provenance, policy);
    const analyzer_detail::postgresql_severity_authority
        severity_authority =
            analyzer_detail::require_postgresql_severity_authority(
                provenance);
    if (facts.schema_version != 2
        || facts.compiler_profile != profile_name(facts.profile)
        || facts.clang_version.empty()
        || !analyzer_detail::lowercase_sha256(
            facts.source_list_sha256)
        || !analyzer_detail::lowercase_sha256(
            facts.compile_database_sha256)
        || !analyzer_detail::lowercase_sha256(
            facts.producer_executable_sha256)
        || !analyzer_detail::lowercase_sha256(
            facts.compile_graph_sha256)
        || !analyzer_detail::lowercase_sha256(
            facts.pcm_manifest_sha256)
        || !analyzer_detail::lowercase_sha256(
            facts.sql_manifest_sha256)
        || facts.root_usrs.empty()
        || !analyzer_detail::lowercase_sha256(
            provenance_bundle_sha256)
        || !analyzer_detail::lowercase_sha256(
            compiler_facts_sha256)
        || generator_revision.empty()) {
        analyzer_detail::analysis_failure(
            "compiler-profile",
            "schema/profile/version/hash metadata is incomplete");
    }
    if (manifest.c_routine_identities.size()
            != policy.expected_parent_count
        || manifest.owners.size()
            != policy.expected_symbol_count) {
        analyzer_detail::analysis_failure(
            profile_name(facts.profile),
            "SQL manifest cardinalities differ from ledger policy");
    }

    std::map<std::string, const function_flow*, std::less<>>
        functions_by_usr;
    std::multimap<std::string, const function_flow*, std::less<>>
        functions_by_qualified_name;
    for (const function_flow& function : facts.functions) {
        if (function.stable_usr.empty()
            || function.qualified_name.empty()
            || function.source_file.empty()
            || !analyzer_detail::valid_range(
                function.declaration_span)
            || (function.body_available
                && !analyzer_detail::valid_range(
                    function.body_span))
            || !functions_by_usr.emplace(
                    function.stable_usr,
                    std::addressof(function)).second) {
            analyzer_detail::analysis_failure(
                profile_name(facts.profile),
                "function facts have an empty or duplicate identity");
        }
        std::string qualified = function.qualified_name;
        while (qualified.starts_with("::")) qualified.erase(0, 2);
        functions_by_qualified_name.emplace(
            std::move(qualified), std::addressof(function));
    }
    analyzer_detail::validate_runtime_probe_facts(
        facts, severity_authority);

    std::vector<const postgres_callback_root*> callback_roots;
    callback_roots.reserve(facts.callback_roots.size());
    for (const postgres_callback_root& root :
         facts.callback_roots) {
        callback_roots.push_back(std::addressof(root));
    }
    std::ranges::sort(
        callback_roots,
        [](const postgres_callback_root* left,
           const postgres_callback_root* right) {
            return left->stable_callback_id
                < right->stable_callback_id;
        });
    static const std::set<std::string, std::less<>>
        callback_families{
            "background_worker",
            "custom_exec",
            "custom_path",
            "custom_scan",
            "error_context",
            "guc_callback",
            "memory_context",
            "postgres_hook",
            "process_exit_callback",
            "relcache_callback",
            "subxact_callback",
            "syscache_callback",
            "table_am",
            "tuple_table_slot",
            "xact_callback",
        };
    std::set<std::string, std::less<>> callback_ids;
    std::string callback_closure_material;
    for (const postgres_callback_root* root :
         callback_roots) {
        if (root == nullptr
            || !root->stable_callback_id.starts_with(
                "pg-callback:")
            || root->stable_callback_id.size()
                != std::string_view{"pg-callback:"}.size() + 32
            || !callback_families.contains(root->family)
            || root->slot.empty()
            || root->owner_usr.empty()
            || root->owner_qualified_name.empty()
            || !analyzer_detail::valid_located_range(
                root->registration)
            || root->target_usr.empty()
            || root->target_qualified_name.empty()
            || !analyzer_detail::valid_located_range(
                root->target_definition)
            || !callback_ids.insert(
                    root->stable_callback_id).second) {
            analyzer_detail::analysis_failure(
                root == nullptr
                    ? "<null-callback-root>"
                    : root->stable_callback_id,
                "PostgreSQL callback provenance/ownership is "
                "incomplete or duplicated");
        }
        const std::string stable_material =
            root->family + "\n" + root->slot + "\n"
            + root->owner_usr + "\n" + root->target_usr + "\n"
            + root->registration.source_file + "\n"
            + std::to_string(
                root->registration.span.begin.byte_offset)
            + "\n"
            + std::to_string(
                root->registration.span.end.byte_offset);
        if (root->stable_callback_id
            != "pg-callback:"
                + sha256_hex(stable_material).substr(0, 32)) {
            analyzer_detail::analysis_failure(
                root->stable_callback_id,
                "PostgreSQL callback stable identity is stale");
        }
        const auto function =
            functions_by_usr.find(root->target_usr);
        if (function == functions_by_usr.end()
            || function->second->qualified_name
                != root->target_qualified_name
            || function->second->source_file
                != root->target_definition.source_file
            || !analyzer_detail::same_range(
                function->second->declaration_span,
                root->target_definition.span)) {
            analyzer_detail::analysis_failure(
                root->stable_callback_id,
                "PostgreSQL callback target does not match its "
                "exact owned definition");
        }
        const analyzer_detail::closure_result closure =
            analyzer_detail::compute_closure(
                *function->second,
                functions_by_usr,
                provenance,
                severity_authority);
        for (const analyzer_detail::reachable_site& site :
             closure.sites) {
            static_cast<void>(
                analyzer_detail::classify_site(site));
        }
        callback_closure_material +=
            "\nR\t" + root->stable_callback_id + "\t"
            + root->family + "\t" + root->slot + "\t"
            + root->owner_usr + "\t"
            + root->owner_qualified_name + "\t"
            + root->registration.source_file + "\t"
            + analyzer_detail::range_fact(
                root->registration.span)
            + "\t" + root->target_usr + "\t"
            + root->target_qualified_name + "\t"
            + root->target_definition.source_file + "\t"
            + analyzer_detail::range_fact(
                root->target_definition.span)
            + closure.canonical_facts;
    }

    safety_ledger result{
        .profile = facts.profile,
        .postgresql_major = provenance.postgresql_major,
        .postgresql_revision = provenance.postgresql_revision,
        .provenance_bundle_sha256 =
            std::move(provenance_bundle_sha256),
        .source_list_sha256 = facts.source_list_sha256,
        .compile_database_sha256 =
            facts.compile_database_sha256,
        .producer_executable_sha256 =
            facts.producer_executable_sha256,
        .compile_graph_sha256 = facts.compile_graph_sha256,
        .pcm_manifest_sha256 = facts.pcm_manifest_sha256,
        .sql_manifest_sha256 = facts.sql_manifest_sha256,
        .compiler_facts_sha256 =
            std::move(compiler_facts_sha256),
        .clang_version = facts.clang_version,
        .analyzer_version =
            "bioseq-clang-cfg-lifetime-v2/" + facts.clang_version,
        .generator_revision = std::move(generator_revision),
        .callback_root_count = callback_roots.size(),
        .callback_closure_sha256 =
            callback_roots.empty()
            ? std::string{}
            : sha256_hex(callback_closure_material),
    };
    result.parents.reserve(policy.expected_parent_count);
    result.symbol_groups.reserve(policy.expected_symbol_count);
    std::set<std::string, std::less<>> expected_root_usrs;
    for (const safety_manifest_owner& owner : manifest.owners) {
        const function_flow& wrapper =
            analyzer_detail::require_wrapper_root(
                functions_by_qualified_name,
                owner,
                profile_name(facts.profile));
        expected_root_usrs.insert(wrapper.stable_usr);
        const analyzer_detail::closure_result closure =
            analyzer_detail::compute_closure(
                wrapper,
                functions_by_usr,
                provenance,
                severity_authority);
        symbol_group group{
            .public_symbol = owner.public_symbol,
            .finfo_symbol = owner.finfo_symbol,
            .generated_wrapper = wrapper.qualified_name,
            .sql_identities = owner.sql_identities,
        };
        for (const safety_manifest_overload& overload :
             owner.overloads) {
            const function_flow& implementation =
                analyzer_detail::require_owner_function(
                    facts, overload);
            if (!closure.reachable_function_usrs.contains(
                    implementation.stable_usr)) {
                analyzer_detail::analysis_failure(
                    overload.sql_identity,
                    "generated wrapper closure does not reach the "
                    "manifest implementation owner");
            }
            safety_parent_row parent{
                .sql_identity = overload.sql_identity,
                .owner_usr = implementation.stable_usr,
                .qualified_declaration =
                    implementation.qualified_name,
                .owner_source_file = implementation.source_file,
                .owner_source_span =
                    implementation.declaration_span,
                .generated_wrapper = wrapper.qualified_name,
                .public_symbol = owner.public_symbol,
                .profile = facts.profile,
                .transitive_closure_sha256 = sha256_hex(
                    closure.canonical_facts),
                .terminal_site_count = 0,
                .analysis = closure.completeness,
            };
            parent.sites.reserve(closure.sites.size());
            for (const analyzer_detail::reachable_site& site :
                 closure.sites) {
                parent.sites.push_back(
                    analyzer_detail::make_error_site(
                        site,
                        overload.sql_identity,
                        facts.profile,
                        overload));
            }
            parent.recoverable_site_count = parent.sites.size();
            parent.audited_stops.reserve(
                closure.audited_stops.size());
            for (const analyzer_detail::reachable_stop& stop :
                 closure.audited_stops) {
                parent.audited_stops.push_back(
                    analyzer_detail::make_audited_stop(
                        stop,
                        overload.sql_identity,
                        facts.profile,
                        provenance.postgresql_revision));
            }
            parent.audited_stop_count =
                parent.audited_stops.size();
            result.parents.push_back(std::move(parent));
        }
        result.symbol_groups.push_back(std::move(group));
    }
    const std::set<std::string, std::less<>> declared_root_usrs{
        facts.root_usrs.begin(), facts.root_usrs.end()};
    if (declared_root_usrs.size() != facts.root_usrs.size()
        || declared_root_usrs != expected_root_usrs) {
        analyzer_detail::analysis_failure(
            profile_name(facts.profile),
            "compiler fact roots differ from the generated wrapper "
            "manifest");
    }
    std::ranges::sort(
        result.parents,
        {},
        &safety_parent_row::sql_identity);
    std::ranges::sort(
        result.symbol_groups,
        {},
        &symbol_group::public_symbol);
    validate_safety_ledger(
        result, provenance, result.provenance_bundle_sha256, policy);
    return result;
}

}  // namespace bioseq::codegen::safety
