module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.codegen.safety:serialization;

import :analyzer_model;
import :json;
import :model;

export namespace bioseq::codegen::safety {

namespace serialization_detail {

inline void require_exact_fields(
    const json_value::object& input,
    std::initializer_list<std::string_view> expected,
    std::string_view context) {
    for (const auto& [actual, value] : input) {
        static_cast<void>(value);
        bool known{};
        for (const std::string_view field : expected) {
            if (actual == field) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw std::runtime_error(
                std::string{context}
                + " has unknown JSON field '" + actual + "'");
        }
    }
    for (const std::string_view field : expected) {
        static_cast<void>(json_field(input, field, context));
    }
}

[[nodiscard]] inline const json_value* optional_field(
    const json_value::object& input,
    std::string_view name) noexcept {
    const auto found = input.find(name);
    return found == input.end() ? nullptr : &found->second;
}

[[nodiscard]] inline std::size_t size_value(
    const json_value& input,
    std::string_view context) {
    const std::int64_t value = json_integer(input, context);
    if (value < 0) {
        throw std::runtime_error(
            std::string{context} + " cannot be negative");
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] inline std::string string_field(
    const json_value::object& input,
    std::string_view name,
    std::string_view context,
    bool allow_empty = false) {
    const std::string value{
        json_string(
            json_field(input, name, context),
            std::string{context} + "." + std::string{name})};
    if (!allow_empty && value.empty()) {
        throw std::runtime_error(
            std::string{context} + "." + std::string{name}
            + " cannot be empty");
    }
    return value;
}

[[nodiscard]] inline bool boolean_field(
    const json_value::object& input,
    std::string_view name,
    std::string_view context) {
    return json_boolean(
        json_field(input, name, context),
        std::string{context} + "." + std::string{name});
}

[[nodiscard]] inline source_position parse_position(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .byte_offset = size_value(
            json_field(value, "byte_offset", context),
            std::string{context} + ".byte_offset"),
        .line = size_value(
            json_field(value, "line", context),
            std::string{context} + ".line"),
        .column = size_value(
            json_field(value, "column", context),
            std::string{context} + ".column"),
    };
}

[[nodiscard]] inline source_range parse_range(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .begin = parse_position(
            json_field(value, "begin", context),
            std::string{context} + ".begin"),
        .end = parse_position(
            json_field(value, "end", context),
            std::string{context} + ".end"),
    };
}

[[nodiscard]] inline source_position parse_exact_position(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    require_exact_fields(
        value,
        {"byte_offset", "column", "line"},
        context);
    return parse_position(input, context);
}

[[nodiscard]] inline source_range parse_exact_range(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    require_exact_fields(value, {"begin", "end"}, context);
    return {
        .begin = parse_exact_position(
            json_field(value, "begin", context),
            std::string{context} + ".begin"),
        .end = parse_exact_position(
            json_field(value, "end", context),
            std::string{context} + ".end"),
    };
}

[[nodiscard]] inline std::vector<std::string> parse_strings(
    const json_value& input,
    std::string_view context) {
    std::vector<std::string> result;
    const json_value::array& values = json_array(input, context);
    result.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::string item{
            json_string(
                values[index],
                std::string{context} + "["
                    + std::to_string(index) + "]")};
        if (item.empty()) {
            throw std::runtime_error(
                std::string{context}
                + " contains an empty string");
        }
        result.push_back(item);
    }
    return result;
}

[[nodiscard]] inline compilation_profile parse_profile(
    std::string_view value) {
    if (value == "production") {
        return compilation_profile::production;
    }
    if (value == "test_indexes") {
        return compilation_profile::test_indexes;
    }
    throw std::runtime_error(
        "unknown compiler profile '" + std::string{value} + "'");
}

[[nodiscard]] inline runtime_relation parse_runtime_relation(
    std::string_view value) {
    if (value == "natural_at_site") {
        return runtime_relation::natural_at_site;
    }
    if (value == "injected_routes_to_site") {
        return runtime_relation::injected_routes_to_site;
    }
    if (value == "injected_after_site_success") {
        return runtime_relation::injected_after_site_success;
    }
    throw std::runtime_error(
        "unknown runtime-probe relation '"
        + std::string{value} + "'");
}

[[nodiscard]] inline runtime_anchor_kind parse_runtime_anchor_kind(
    std::string_view value) {
    if (value == "postgres_error_site") {
        return runtime_anchor_kind::postgres_error_site;
    }
    if (value == "record_site_visit") {
        return runtime_anchor_kind::record_site_visit;
    }
    if (value == "consume_fault") {
        return runtime_anchor_kind::consume_fault;
    }
    throw std::runtime_error(
        "unknown runtime-probe anchor kind '"
        + std::string{value} + "'");
}

[[nodiscard]] inline runtime_probe_ordering
parse_runtime_probe_ordering(std::string_view value) {
    if (value == "visit_only") {
        return runtime_probe_ordering::visit_only;
    }
    if (value == "trigger_before_visit") {
        return runtime_probe_ordering::trigger_before_visit;
    }
    if (value == "visit_before_trigger") {
        return runtime_probe_ordering::visit_before_trigger;
    }
    throw std::runtime_error(
        "unknown runtime-probe ordering '"
        + std::string{value} + "'");
}

inline void validate_runtime_probe_fact_shape(
    const runtime_probe_fact& value,
    std::string_view context) {
    if (value.fault_key.empty()
        || value.site_call_stable_id.empty()
        || value.visit_call_stable_id.empty()) {
        throw std::runtime_error(
            std::string{context}
            + " has an empty fault key or stable call id");
    }
    if (runtime_relation_name(value.relation) == "unknown") {
        throw std::runtime_error(
            std::string{context}
            + " has an unknown runtime-probe relation");
    }
    if (value.trigger_call_stable_id
        && value.trigger_call_stable_id->empty()) {
        throw std::runtime_error(
            std::string{context}
            + ".trigger_call_stable_id cannot be empty");
    }
    if (value.relation == runtime_relation::natural_at_site
        && value.trigger_call_stable_id) {
        throw std::runtime_error(
            std::string{context}
            + " natural_at_site relation requires a null "
              "trigger_call_stable_id");
    }
    if (value.relation != runtime_relation::natural_at_site
        && !value.trigger_call_stable_id) {
        throw std::runtime_error(
            std::string{context}
            + " injected relation requires a non-null "
              "trigger_call_stable_id");
    }
}

[[nodiscard]] inline resolution_status parse_resolution(
    std::string_view value) {
    if (value == "direct") return resolution_status::direct;
    if (value == "resolved_indirect") {
        return resolution_status::resolved_indirect;
    }
    if (value == "unresolved_indirect") {
        return resolution_status::unresolved_indirect;
    }
    if (value == "unavailable_body") {
        return resolution_status::unavailable_body;
    }
    if (value == "control_flow_assembly") {
        return resolution_status::control_flow_assembly;
    }
    if (value == "cycle_without_fixed_point") {
        return resolution_status::cycle_without_fixed_point;
    }
    throw std::runtime_error(
        "unknown call resolution '" + std::string{value} + "'");
}

[[nodiscard]] inline callee_domain parse_domain(
    std::string_view value) {
    if (value == "project") return callee_domain::project;
    if (value == "postgresql_server") {
        return callee_domain::postgresql_server;
    }
    if (value == "standard_library") {
        return callee_domain::standard_library;
    }
    if (value == "system_library") {
        return callee_domain::system_library;
    }
    if (value == "compiler_builtin") {
        return callee_domain::compiler_builtin;
    }
    if (value == "unknown") return callee_domain::unknown;
    throw std::runtime_error(
        "unknown callee domain '" + std::string{value} + "'");
}

[[nodiscard]] inline object_identity_kind parse_object_identity_kind(
    std::string_view value) {
    if (value == "declaration_usr") {
        return object_identity_kind::declaration_usr;
    }
    if (value == "synthetic_temporary") {
        return object_identity_kind::synthetic_temporary;
    }
    if (value == "synthetic_capture") {
        return object_identity_kind::synthetic_capture;
    }
    if (value == "synthetic_coroutine_state") {
        return object_identity_kind::synthetic_coroutine_state;
    }
    throw std::runtime_error(
        "unknown flow-object identity kind '"
        + std::string{value} + "'");
}

[[nodiscard]] inline external_call_mode parse_external_call_mode(
    std::string_view value) {
    if (value == "not_external") {
        return external_call_mode::not_external;
    }
    if (value == "audited_no_user_code") {
        return external_call_mode::audited_no_user_code;
    }
    if (value == "analyze_body") {
        return external_call_mode::analyze_body;
    }
    if (value == "unknown") {
        return external_call_mode::unknown;
    }
    throw std::runtime_error(
        "unknown external call mode '" + std::string{value} + "'");
}

[[nodiscard]] inline bridge_kind parse_bridge(
    std::string_view value) {
    if (value == "none") return bridge_kind::none;
    if (value == "invoke_pg_as_cpp") {
        return bridge_kind::invoke_pg_as_cpp;
    }
    if (value == "sql_subtransaction") {
        return bridge_kind::sql_subtransaction;
    }
    if (value == "sql_call_context") {
        return bridge_kind::sql_call_context;
    }
    if (value == "generated_boundary_tail") {
        return bridge_kind::generated_boundary_tail;
    }
    throw std::runtime_error(
        "unknown bridge kind '" + std::string{value} + "'");
}

[[nodiscard]] inline crossing_origin parse_origin(
    std::string_view value) {
    if (value == "automatic_object") {
        return crossing_origin::automatic_object;
    }
    if (value == "temporary") return crossing_origin::temporary;
    if (value == "lambda_capture") {
        return crossing_origin::lambda_capture;
    }
    if (value == "catch_parameter") {
        return crossing_origin::catch_parameter;
    }
    if (value == "coroutine_state") {
        return crossing_origin::coroutine_state;
    }
    throw std::runtime_error(
        "unknown crossing origin '" + std::string{value} + "'");
}

[[nodiscard]] inline const char* origin_name(
    crossing_origin value) noexcept {
    switch (value) {
        case crossing_origin::automatic_object:
            return "automatic_object";
        case crossing_origin::temporary: return "temporary";
        case crossing_origin::lambda_capture: return "lambda_capture";
        case crossing_origin::catch_parameter:
            return "catch_parameter";
        case crossing_origin::coroutine_state:
            return "coroutine_state";
    }
    return "unknown";
}

[[nodiscard]] inline provenance_kind parse_provenance_kind(
    std::string_view value) {
    if (value == "recoverable_error_seed") {
        return provenance_kind::recoverable_error_seed;
    }
    if (value == "terminal_severity_seed") {
        return provenance_kind::terminal_severity_seed;
    }
    if (value == "bridge_primitive") {
        return provenance_kind::bridge_primitive;
    }
    if (value == "pg_no_longjmp_leaf") {
        return provenance_kind::pg_no_longjmp_leaf;
    }
    if (value == "standard_library_declaration") {
        return provenance_kind::standard_library_declaration;
    }
    if (value == "system_library_declaration") {
        return provenance_kind::system_library_declaration;
    }
    if (value == "compiler_builtin_declaration") {
        return provenance_kind::compiler_builtin_declaration;
    }
    throw std::runtime_error(
        "unknown provenance kind '" + std::string{value} + "'");
}

[[nodiscard]] inline postgres_severity parse_severity(
    std::string_view value) {
    if (value == "error") {
        return postgres_severity::recoverable_error;
    }
    if (value == "fatal") return postgres_severity::fatal;
    if (value == "panic") return postgres_severity::panic;
    if (value == "unresolved") {
        return postgres_severity::unresolved;
    }
    throw std::runtime_error(
        "unknown PostgreSQL severity '" + std::string{value} + "'");
}

[[nodiscard]] inline site_classification parse_classification(
    std::string_view value) {
    if (value == "bridged") {
        return site_classification::bridged;
    }
    if (value == "specialized_bridge") {
        return site_classification::specialized_bridge;
    }
    if (value == "lifetime_free_direct") {
        return site_classification::lifetime_free_direct;
    }
    if (value == "terminal_after_unwind") {
        return site_classification::terminal_after_unwind;
    }
    if (value == "unclassified") {
        return site_classification::unclassified;
    }
    throw std::runtime_error(
        "unknown site classification '"
        + std::string{value} + "'");
}

[[nodiscard]] inline audited_stop_domain parse_audited_stop_domain(
    std::string_view value) {
    if (value == "postgresql_server") {
        return audited_stop_domain::postgresql_server;
    }
    if (value == "standard_library") {
        return audited_stop_domain::standard_library;
    }
    if (value == "system_library") {
        return audited_stop_domain::system_library;
    }
    if (value == "compiler_builtin") {
        return audited_stop_domain::compiler_builtin;
    }
    throw std::runtime_error(
        "unknown audited-stop domain '"
        + std::string{value} + "'");
}

[[nodiscard]] inline located_source_range parse_located_range(
    const json_value& input,
    std::string_view context);

[[nodiscard]] inline runtime_probe_fact parse_runtime_probe_fact(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    require_exact_fields(
        value,
        {
            "fault_key",
            "relation",
            "site_call_stable_id",
            "trigger_call_stable_id",
            "visit_call_stable_id",
        },
        context);
    runtime_probe_fact result{
        .fault_key = string_field(
            value, "fault_key", context),
        .relation = parse_runtime_relation(
            string_field(value, "relation", context)),
        .site_call_stable_id = string_field(
            value, "site_call_stable_id", context),
        .visit_call_stable_id = string_field(
            value, "visit_call_stable_id", context),
    };
    const json_value& trigger = json_field(
        value, "trigger_call_stable_id", context);
    if (!std::holds_alternative<std::nullptr_t>(trigger.value)) {
        result.trigger_call_stable_id = std::string{
            json_string(
                trigger,
                std::string{context}
                    + ".trigger_call_stable_id")};
    }
    validate_runtime_probe_fact_shape(result, context);
    return result;
}

[[nodiscard]] inline postgres_callback_root
parse_postgres_callback_root(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    require_exact_fields(
        value,
        {
            "family",
            "owner_qualified_name",
            "owner_usr",
            "registration",
            "slot",
            "stable_callback_id",
            "target_definition",
            "target_qualified_name",
            "target_usr",
        },
        context);
    return {
        .stable_callback_id = string_field(
            value, "stable_callback_id", context),
        .family = string_field(value, "family", context),
        .slot = string_field(value, "slot", context),
        .owner_usr = string_field(value, "owner_usr", context),
        .owner_qualified_name = string_field(
            value, "owner_qualified_name", context),
        .registration = parse_located_range(
            json_field(value, "registration", context),
            std::string{context} + ".registration"),
        .target_usr = string_field(
            value, "target_usr", context),
        .target_qualified_name = string_field(
            value, "target_qualified_name", context),
        .target_definition = parse_located_range(
            json_field(value, "target_definition", context),
            std::string{context} + ".target_definition"),
    };
}

[[nodiscard]] inline bool lowercase_sha256_value(
    std::string_view value) noexcept {
    return value.size() == 64
        && std::ranges::all_of(
            value,
            [](const char digit) {
                return (digit >= '0' && digit <= '9')
                    || (digit >= 'a' && digit <= 'f');
            });
}

[[nodiscard]] inline bool valid_inventory_fault_key(
    std::string_view value) noexcept {
    return !value.empty()
        && value.size() < 129
        && value.find('\0') == std::string_view::npos
        && std::ranges::all_of(
            value,
            [](const char character) {
                return (character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9')
                    || character == '_'
                    || character == '.';
            });
}

[[nodiscard]] inline bool valid_inventory_range(
    const source_range& value) noexcept {
    return value.begin.line != 0
        && value.begin.column != 0
        && value.end.line != 0
        && value.end.column != 0
        && value.begin.byte_offset < value.end.byte_offset;
}

[[nodiscard]] inline runtime_probe_contract
expected_runtime_probe_contract(runtime_relation relation) {
    switch (relation) {
        case runtime_relation::natural_at_site:
            return {
                .abi_version = 2,
                .expected_visit_count = 1,
                .expected_trigger_count = 0,
                .ordering =
                    runtime_probe_ordering::visit_only,
            };
        case runtime_relation::injected_routes_to_site:
            return {
                .abi_version = 2,
                .expected_visit_count = 1,
                .expected_trigger_count = 1,
                .ordering =
                    runtime_probe_ordering::
                        trigger_before_visit,
            };
        case runtime_relation::injected_after_site_success:
            return {
                .abi_version = 2,
                .expected_visit_count = 1,
                .expected_trigger_count = 1,
                .ordering =
                    runtime_probe_ordering::
                        visit_before_trigger,
            };
    }
    throw std::runtime_error(
        "runtime inventory has an unknown relation");
}

inline void validate_runtime_source_anchor_shape(
    const runtime_source_anchor& anchor,
    std::string_view context) {
    if (runtime_anchor_kind_name(anchor.kind) == "unknown"
        || anchor.compiler_stable_id.empty()
        || anchor.enclosing_function_usr.empty()
        || anchor.source_file.empty()
        || !valid_inventory_range(
            anchor.expanded_source_span)
        || !lowercase_sha256_value(
            anchor.source_fragment_sha256)) {
        throw std::runtime_error(
            std::string{context}
            + " lacks an exact kind, compiler identity, source "
              "range, or fragment hash");
    }
}

inline void validate_runtime_probe_inventory_shape(
    const runtime_probe_inventory& inventory) {
    if (inventory.schema_version != 1
        || inventory.probe_abi_version != 2
        || profile_name(inventory.profile) == "unknown"
        || !lowercase_sha256_value(inventory.ledger_sha256)
        || !lowercase_sha256_value(
            inventory.compiler_facts_sha256)
        || !lowercase_sha256_value(
            inventory.sql_manifest_sha256)
        || !lowercase_sha256_value(
            inventory.source_list_sha256)
        || !lowercase_sha256_value(
            inventory.compile_database_sha256)
        || !lowercase_sha256_value(
            inventory.compile_graph_sha256)
        || !lowercase_sha256_value(
            inventory.pcm_manifest_sha256)
        || !lowercase_sha256_value(
            inventory
                .inventory_producer_executable_sha256)
        || inventory.entries.empty()) {
        throw std::runtime_error(
            "runtime inventory lacks schema-v1/ABI-v2 metadata, "
            "exact hashes, or entries");
    }
    std::set<
        std::pair<std::string, std::string>,
        std::less<>>
        keys;
    std::set<std::string, std::less<>> site_ids;
    for (const runtime_probe_inventory_entry& entry :
         inventory.entries) {
        const runtime_probe_contract expected =
            expected_runtime_probe_contract(entry.relation);
        if (!keys.emplace(
                entry.sql_identity,
                entry.fault_key).second) {
            throw std::runtime_error(
                "runtime inventory duplicates the exact "
                "(sql_identity, fault_key) case key");
        }
        if (!site_ids.insert(entry.site_id).second) {
            throw std::runtime_error(
                "runtime inventory duplicates a site id");
        }
        if (entry.sql_identity.empty()
            || !valid_inventory_fault_key(entry.fault_key)
            || !entry.site_id.starts_with("probe:v2:")
            || entry.site_id.size() != 73
            || !lowercase_sha256_value(
                std::string_view{entry.site_id}.substr(9))
            || !lowercase_sha256_value(
                entry.ledger_site_sha256)
            || entry.transitive_call_path.empty()
            || entry.probe_contract.abi_version
                != inventory.probe_abi_version
            || entry.probe_contract.abi_version
                != expected.abi_version
            || entry.probe_contract.expected_visit_count
                != expected.expected_visit_count
            || entry.probe_contract.expected_trigger_count
                != expected.expected_trigger_count
            || entry.probe_contract.ordering
                != expected.ordering) {
            throw std::runtime_error(
                "runtime inventory entry has an invalid identity, "
                "hash, uniqueness key, path, or probe contract");
        }
        validate_runtime_source_anchor_shape(
            entry.site_anchor,
            "runtime inventory entry.site_anchor");
        validate_runtime_source_anchor_shape(
            entry.visit_anchor,
            "runtime inventory entry.visit_anchor");
        if (entry.site_anchor.kind
                != runtime_anchor_kind::postgres_error_site
            || entry.visit_anchor.kind
                != runtime_anchor_kind::record_site_visit
            || entry.site_anchor.enclosing_function_usr
                != entry.visit_anchor.enclosing_function_usr
            || entry.site_anchor.compiler_stable_id
                == entry.visit_anchor.compiler_stable_id) {
            throw std::runtime_error(
                "runtime inventory site/visit anchors have "
                "inconsistent roles or enclosing functions");
        }
        if (entry.relation
                == runtime_relation::natural_at_site) {
            if (entry.trigger_anchor) {
                throw std::runtime_error(
                    "natural runtime inventory entry carries a "
                    "trigger anchor");
            }
        } else {
            if (!entry.trigger_anchor) {
                throw std::runtime_error(
                    "injected runtime inventory entry lacks a "
                    "trigger anchor");
            }
            validate_runtime_source_anchor_shape(
                *entry.trigger_anchor,
                "runtime inventory entry.trigger_anchor");
            if (entry.trigger_anchor->kind
                    != runtime_anchor_kind::consume_fault
                || entry.trigger_anchor
                        ->enclosing_function_usr
                    != entry.site_anchor
                           .enclosing_function_usr
                || entry.trigger_anchor->compiler_stable_id
                    == entry.site_anchor.compiler_stable_id
                || entry.trigger_anchor->compiler_stable_id
                    == entry.visit_anchor.compiler_stable_id) {
                throw std::runtime_error(
                    "runtime inventory trigger anchor has an "
                    "inconsistent role, identity, or function");
            }
        }
        for (const call_path_step& step :
             entry.transitive_call_path) {
            if (step.usr.empty()
                || step.qualified_declaration.empty()
                || step.source_file.empty()
                || !valid_inventory_range(step.source_span)) {
                throw std::runtime_error(
                    "runtime inventory call path has an empty "
                    "identity or invalid source range");
            }
        }
        if (entry.transitive_call_path.back().usr
            != entry.site_anchor.enclosing_function_usr) {
            throw std::runtime_error(
                "runtime inventory call path does not terminate "
                "at the site anchor function");
        }
    }
}

[[nodiscard]] inline runtime_source_anchor
parse_runtime_source_anchor(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    require_exact_fields(
        value,
        {
            "compiler_stable_id",
            "enclosing_function_usr",
            "expanded_source_span",
            "kind",
            "source_file",
            "source_fragment_sha256",
        },
        context);
    runtime_source_anchor result{
        .kind = parse_runtime_anchor_kind(
            string_field(value, "kind", context)),
        .compiler_stable_id = string_field(
            value, "compiler_stable_id", context),
        .enclosing_function_usr = string_field(
            value, "enclosing_function_usr", context),
        .source_file = string_field(
            value, "source_file", context),
        .expanded_source_span = parse_exact_range(
            json_field(
                value, "expanded_source_span", context),
            std::string{context}
                + ".expanded_source_span"),
        .source_fragment_sha256 = string_field(
            value, "source_fragment_sha256", context),
    };
    validate_runtime_source_anchor_shape(result, context);
    return result;
}

[[nodiscard]] inline runtime_probe_contract
parse_runtime_probe_contract(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    require_exact_fields(
        value,
        {
            "abi_version",
            "expected_trigger_count",
            "expected_visit_count",
            "ordering",
        },
        context);
    const std::size_t abi_version = size_value(
        json_field(value, "abi_version", context),
        std::string{context} + ".abi_version");
    if (abi_version != 2) {
        throw std::runtime_error(
            std::string{context}
            + " requires ABI version 2");
    }
    return {
        .abi_version = 2,
        .expected_visit_count = size_value(
            json_field(
                value, "expected_visit_count", context),
            std::string{context}
                + ".expected_visit_count"),
        .expected_trigger_count = size_value(
            json_field(
                value, "expected_trigger_count", context),
            std::string{context}
                + ".expected_trigger_count"),
        .ordering = parse_runtime_probe_ordering(
            string_field(value, "ordering", context)),
    };
}

[[nodiscard]] inline std::vector<call_path_step>
parse_exact_inventory_call_path(
    const json_value& input,
    std::string_view context) {
    const json_value::array& values = json_array(input, context);
    std::vector<call_path_step> result;
    result.reserve(values.size());
    for (std::size_t index = 0;
         index < values.size();
         ++index) {
        const std::string item_context =
            std::string{context} + "["
            + std::to_string(index) + "]";
        const json_value::object& value =
            json_object(values[index], item_context);
        require_exact_fields(
            value,
            {
                "qualified_declaration",
                "source_file",
                "source_span",
                "usr",
            },
            item_context);
        result.push_back({
            .usr = string_field(
                value, "usr", item_context),
            .qualified_declaration = string_field(
                value,
                "qualified_declaration",
                item_context),
            .source_file = string_field(
                value, "source_file", item_context),
            .source_span = parse_exact_range(
                json_field(
                    value, "source_span", item_context),
                item_context + ".source_span"),
        });
    }
    return result;
}

[[nodiscard]] inline runtime_probe_inventory_entry
parse_runtime_probe_inventory_entry(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    require_exact_fields(
        value,
        {
            "fault_key",
            "ledger_site_sha256",
            "probe_contract",
            "relation",
            "site_anchor",
            "site_id",
            "sql_identity",
            "transitive_call_path",
            "trigger_anchor",
            "visit_anchor",
        },
        context);
    runtime_probe_inventory_entry result{
        .sql_identity = string_field(
            value, "sql_identity", context),
        .fault_key = string_field(
            value, "fault_key", context),
        .site_id = string_field(
            value, "site_id", context),
        .relation = parse_runtime_relation(
            string_field(value, "relation", context)),
        .ledger_site_sha256 = string_field(
            value, "ledger_site_sha256", context),
        .site_anchor = parse_runtime_source_anchor(
            json_field(value, "site_anchor", context),
            std::string{context} + ".site_anchor"),
        .visit_anchor = parse_runtime_source_anchor(
            json_field(value, "visit_anchor", context),
            std::string{context} + ".visit_anchor"),
        .transitive_call_path =
            parse_exact_inventory_call_path(
                json_field(
                    value,
                    "transitive_call_path",
                    context),
                std::string{context}
                    + ".transitive_call_path"),
        .probe_contract = parse_runtime_probe_contract(
            json_field(value, "probe_contract", context),
            std::string{context} + ".probe_contract"),
    };
    const json_value& trigger =
        json_field(value, "trigger_anchor", context);
    if (!std::holds_alternative<std::nullptr_t>(
            trigger.value)) {
        result.trigger_anchor =
            parse_runtime_source_anchor(
                trigger,
                std::string{context}
                    + ".trigger_anchor");
    }
    return result;
}

[[nodiscard]] inline located_source_range parse_located_range(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .source_file = string_field(
            value, "source_file", context),
        .span = parse_range(
            json_field(value, "span", context),
            std::string{context} + ".span"),
    };
}

[[nodiscard]] inline flow_object parse_flow_object(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    flow_object result{
        .stable_object_id = string_field(
            value, "stable_object_id", context),
        .identity_kind = parse_object_identity_kind(
            string_field(value, "identity_kind", context)),
        .canonical_type = string_field(
            value, "canonical_type", context),
        .origin = parse_origin(string_field(
            value, "origin", context)),
        .declaration = parse_located_range(
            json_field(value, "declaration", context),
            std::string{context} + ".declaration"),
        .trivially_destructible = boolean_field(
            value, "trivially_destructible", context),
    };
    if (const json_value* declaration_usr =
            optional_field(value, "declaration_usr");
        declaration_usr != nullptr
        && !std::holds_alternative<std::nullptr_t>(
            declaration_usr->value)) {
        result.declaration_usr = std::string{
            json_string(
                *declaration_usr,
                std::string{context} + ".declaration_usr")};
        if (result.declaration_usr->empty()) {
            throw std::runtime_error(
                std::string{context}
                + ".declaration_usr cannot be empty");
        }
    }
    return result;
}

[[nodiscard]] inline std::vector<flow_object> parse_flow_objects(
    const json_value& input,
    std::string_view context) {
    std::vector<flow_object> result;
    const json_value::array& values = json_array(input, context);
    result.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        result.push_back(parse_flow_object(
            values[index],
            std::string{context} + "["
                + std::to_string(index) + "]"));
    }
    return result;
}

[[nodiscard]] inline provenance_reference parse_reference(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .record_id = string_field(
            value, "record_id", context),
        .postgresql_revision = string_field(
            value, "postgresql_revision", context),
        .upstream_file = string_field(
            value, "upstream_file", context),
        .upstream_span = parse_range(
            json_field(value, "upstream_span", context),
            std::string{context} + ".upstream_span"),
        .fragment_sha256 = string_field(
            value, "fragment_sha256", context),
        .rationale = string_field(
            value, "rationale", context),
    };
}

[[nodiscard]] inline external_declaration_provenance
parse_external_provenance(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .record_id = string_field(
            value, "record_id", context),
        .source_file = string_field(
            value, "source_file", context),
        .declaration_span = parse_range(
            json_field(value, "declaration_span", context),
            std::string{context} + ".declaration_span"),
        .declaration_sha256 = string_field(
            value, "declaration_sha256", context),
        .library_version = string_field(
            value, "library_version", context),
        .user_code_callback_free = boolean_field(
            value, "user_code_callback_free", context),
    };
}

[[nodiscard]] inline bridge_transition_evidence
parse_bridge_transition_evidence(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .primitive = parse_reference(
            json_field(value, "primitive", context),
            std::string{context} + ".primitive"),
        .target_usr = string_field(
            value, "target_usr", context),
        .target_qualified_name = string_field(
            value, "target_qualified_name", context),
        .transition_callsite = parse_located_range(
            json_field(value, "transition_callsite", context),
            std::string{context} + ".transition_callsite"),
        .implementation_usr = string_field(
            value, "implementation_usr", context),
        .implementation_qualified_name = string_field(
            value, "implementation_qualified_name", context),
        .implementation_module = string_field(
            value, "implementation_module", context),
        .implementation_definition = parse_located_range(
            json_field(
                value, "implementation_definition", context),
            std::string{context}
                + ".implementation_definition"),
        .implementation_body_sha256 = string_field(
            value, "implementation_body_sha256", context),
        .implementation_contract_id = string_field(
            value, "implementation_contract_id", context, true),
        .implementation_contract_sha256 = string_field(
            value,
            "implementation_contract_sha256",
            context,
            true),
        .protected_region_id = string_field(
            value, "protected_region_id", context),
    };
}

[[nodiscard]] inline macro_expansion_record parse_macro_expansion(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .name = string_field(value, "name", context),
        .invocation = parse_located_range(
            json_field(value, "invocation", context),
            std::string{context} + ".invocation"),
        .definition = parse_located_range(
            json_field(value, "definition", context),
            std::string{context} + ".definition"),
        .definition_sha256 = string_field(
            value, "definition_sha256", context),
    };
}

[[nodiscard]] inline flow_call_target parse_call_target(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    flow_call_target result{
        .target_usr = string_field(
            value, "target_usr", context, true),
        .target_qualified_name = string_field(
            value, "target_qualified_name", context),
        .domain = parse_domain(
            string_field(value, "domain", context)),
        .external_mode = parse_external_call_mode(
            string_field(value, "external_mode", context)),
    };
    if (const json_value* provenance =
            optional_field(value, "external_provenance");
        provenance != nullptr
        && !std::holds_alternative<std::nullptr_t>(
            provenance->value)) {
        result.external_provenance = parse_external_provenance(
            *provenance,
            std::string{context} + ".external_provenance");
    }
    if (const json_value* reference =
            optional_field(
                value, "pg_no_longjmp_allowlist_evidence");
        reference != nullptr
        && !std::holds_alternative<std::nullptr_t>(
            reference->value)) {
        result.pg_no_longjmp_allowlist_evidence =
            parse_reference(
                *reference,
                std::string{context}
                    + ".pg_no_longjmp_allowlist_evidence");
    }
    return result;
}

[[nodiscard]] inline flow_call_site parse_call(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    flow_call_site result{
        .stable_site_id = string_field(
            value, "stable_site_id", context),
        .caller_usr = string_field(
            value, "caller_usr", context),
        .spelling = parse_located_range(
            json_field(value, "spelling", context),
            std::string{context} + ".spelling"),
        .expansion = parse_located_range(
            json_field(value, "expansion", context),
            std::string{context} + ".expansion"),
        .resolution = parse_resolution(string_field(
            value, "resolution", context)),
        .target_set_complete = boolean_field(
            value, "target_set_complete", context),
        .live_objects = parse_flow_objects(
            json_field(value, "live_objects", context),
            std::string{context} + ".live_objects"),
        .callable_capture_objects = parse_flow_objects(
            json_field(
                value, "callable_capture_objects", context),
            std::string{context}
                + ".callable_capture_objects"),
        .bridge_transition = parse_bridge(string_field(
            value, "bridge_transition", context)),
        .analyzer_entered_bridge_callable = boolean_field(
            value, "analyzer_entered_bridge_callable", context),
        .generated_boundary_tail = boolean_field(
            value, "generated_boundary_tail", context),
    };
    if (const json_value* evidence =
            optional_field(value, "bridge_primitive_evidence");
        evidence != nullptr
        && !std::holds_alternative<std::nullptr_t>(
            evidence->value)) {
        result.bridge_primitive_evidence =
            parse_bridge_transition_evidence(
                *evidence,
                std::string{context}
                    + ".bridge_primitive_evidence");
    }
    const json_value::array& macros = json_array(
        json_field(value, "macro_chain", context),
        std::string{context} + ".macro_chain");
    result.macro_chain.reserve(macros.size());
    for (std::size_t index = 0; index < macros.size(); ++index) {
        result.macro_chain.push_back(parse_macro_expansion(
            macros[index],
            std::string{context} + ".macro_chain["
                + std::to_string(index) + "]"));
    }
    const json_value::array& targets = json_array(
        json_field(value, "targets", context),
        std::string{context} + ".targets");
    result.targets.reserve(targets.size());
    for (std::size_t index = 0; index < targets.size(); ++index) {
        result.targets.push_back(parse_call_target(
            targets[index],
            std::string{context} + ".targets["
                + std::to_string(index) + "]"));
    }
    const json_value::array& arguments = json_array(
        json_field(
            value, "constant_integer_arguments", context),
        std::string{context} + ".constant_integer_arguments");
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string argument_context =
            std::string{context} + ".constant_integer_arguments["
            + std::to_string(index) + "]";
        const json_value::object& argument =
            json_object(arguments[index], argument_context);
        constant_integer_argument parsed{
            .ordinal = size_value(
                json_field(
                    argument, "ordinal", argument_context),
                argument_context + ".ordinal"),
        };
        const json_value& raw_value =
            json_field(argument, "value", argument_context);
        if (!std::holds_alternative<std::nullptr_t>(
                raw_value.value)) {
            parsed.value =
                json_integer(raw_value, argument_context + ".value");
        }
        result.constant_integer_arguments.push_back(
            std::move(parsed));
    }
    return result;
}

[[nodiscard]] inline crossing_type parse_crossing(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .canonical_type = string_field(
            value, "canonical_type", context),
        .origin = parse_origin(
            string_field(value, "origin", context)),
        .source_file = string_field(
            value, "source_file", context),
        .declaration_span = parse_range(
            json_field(value, "declaration_span", context),
            std::string{context} + ".declaration_span"),
        .trivially_destructible = boolean_field(
            value, "trivially_destructible", context),
        .crosses_postgres_longjmp = boolean_field(
            value, "crosses_postgres_longjmp", context),
    };
}

[[nodiscard]] inline std::vector<crossing_type> parse_crossings(
    const json_value& input,
    std::string_view context) {
    const json_value::array& values = json_array(input, context);
    std::vector<crossing_type> result;
    result.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        result.push_back(parse_crossing(
            values[index],
            std::string{context} + "["
                + std::to_string(index) + "]"));
    }
    return result;
}

[[nodiscard]] inline std::vector<call_path_step> parse_call_path(
    const json_value& input,
    std::string_view context) {
    const json_value::array& values = json_array(input, context);
    std::vector<call_path_step> result;
    result.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::string item_context =
            std::string{context} + "["
            + std::to_string(index) + "]";
        const json_value::object& value =
            json_object(values[index], item_context);
        result.push_back({
            .usr = string_field(
                value, "usr", item_context),
            .qualified_declaration = string_field(
                value, "qualified_declaration", item_context),
            .source_file = string_field(
                value, "source_file", item_context),
            .source_span = parse_range(
                json_field(
                    value, "source_span", item_context),
                item_context + ".source_span"),
        });
    }
    return result;
}

[[nodiscard]] inline bridge_transition_proof
parse_bridge_transition_proof(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .primitive = parse_reference(
            json_field(value, "primitive", context),
            std::string{context} + ".primitive"),
        .target_usr = string_field(
            value, "target_usr", context),
        .target_qualified_name = string_field(
            value, "target_qualified_name", context),
        .transition_source_file = string_field(
            value, "transition_source_file", context),
        .transition_source_span = parse_range(
            json_field(
                value, "transition_source_span", context),
            std::string{context}
                + ".transition_source_span"),
        .implementation_usr = string_field(
            value, "implementation_usr", context),
        .implementation_qualified_name = string_field(
            value, "implementation_qualified_name", context),
        .implementation_module = string_field(
            value, "implementation_module", context),
        .implementation_source_file = string_field(
            value, "implementation_source_file", context),
        .implementation_source_span = parse_range(
            json_field(
                value, "implementation_source_span", context),
            std::string{context}
                + ".implementation_source_span"),
        .implementation_body_sha256 = string_field(
            value, "implementation_body_sha256", context),
        .implementation_contract_id = string_field(
            value, "implementation_contract_id", context, true),
        .implementation_contract_sha256 = string_field(
            value,
            "implementation_contract_sha256",
            context,
            true),
        .protected_region_id = string_field(
            value, "protected_region_id", context),
    };
}

[[nodiscard]] inline error_site parse_error_site(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    error_site result{
        .site_id = string_field(value, "site_id", context),
        .source_file = string_field(
            value, "source_file", context),
        .expanded_source_span = parse_range(
            json_field(
                value, "expanded_source_span", context),
            std::string{context} + ".expanded_source_span"),
        .callee_or_macro = string_field(
            value, "callee_or_macro", context),
        .transitive_call_path = parse_call_path(
            json_field(
                value, "transitive_call_path", context),
            std::string{context} + ".transitive_call_path"),
        .severity_constant_propagated = boolean_field(
            value, "severity_constant_propagated", context),
        .resolution = parse_resolution(
            string_field(value, "resolution", context)),
        .classification = parse_classification(
            string_field(value, "classification", context)),
        .bridge = parse_bridge(
            string_field(value, "bridge", context)),
        .crossing_types = parse_crossings(
            json_field(value, "crossing_types", context),
            std::string{context} + ".crossing_types"),
        .bridge_callable_crossing_types = parse_crossings(
            json_field(
                value,
                "bridge_callable_crossing_types",
                context),
            std::string{context}
                + ".bridge_callable_crossing_types"),
        .analyzer_entered_bridge_callable = boolean_field(
            value, "analyzer_entered_bridge_callable", context),
        .generated_boundary_tail = boolean_field(
            value, "generated_boundary_tail", context),
        .converter_families = parse_strings(
            json_field(value, "converter_families", context),
            std::string{context} + ".converter_families"),
        .owner_families = parse_strings(
            json_field(value, "owner_families", context),
            std::string{context} + ".owner_families"),
        .cleanup_phases = parse_strings(
            json_field(value, "cleanup_phases", context),
            std::string{context} + ".cleanup_phases"),
    };
    const json_value::array& severities = json_array(
        json_field(value, "possible_severities", context),
        std::string{context} + ".possible_severities");
    for (std::size_t index = 0;
         index < severities.size();
         ++index) {
        result.possible_severities.push_back(
            parse_severity(json_string(
                severities[index],
                std::string{context}
                    + ".possible_severities["
                    + std::to_string(index) + "]")));
    }
    const json_value& no_longjmp = json_field(
        value, "no_longjmp_allowlist_evidence", context);
    if (!std::holds_alternative<std::nullptr_t>(
            no_longjmp.value)) {
        result.no_longjmp_allowlist_evidence =
            parse_reference(
                no_longjmp,
                std::string{context}
                    + ".no_longjmp_allowlist_evidence");
    }
    const json_value& bridge_evidence =
        json_field(value, "bridge_evidence", context);
    if (!std::holds_alternative<std::nullptr_t>(
            bridge_evidence.value)) {
        result.bridge_evidence =
            parse_bridge_transition_proof(
                bridge_evidence,
                std::string{context} + ".bridge_evidence");
    }
    return result;
}

[[nodiscard]] inline audited_stop parse_audited_stop(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .stop_id = string_field(value, "stop_id", context),
        .source_file = string_field(
            value, "source_file", context),
        .expanded_source_span = parse_range(
            json_field(
                value, "expanded_source_span", context),
            std::string{context} + ".expanded_source_span"),
        .target_usr = string_field(
            value, "target_usr", context),
        .target_qualified_name = string_field(
            value, "target_qualified_name", context),
        .resolution = parse_resolution(
            string_field(value, "resolution", context)),
        .domain = parse_audited_stop_domain(
            string_field(value, "domain", context)),
        .transitive_call_path = parse_call_path(
            json_field(
                value, "transitive_call_path", context),
            std::string{context} + ".transitive_call_path"),
        .evidence = parse_reference(
            json_field(value, "evidence", context),
            std::string{context} + ".evidence"),
        .library_version = string_field(
            value, "library_version", context, true),
        .user_code_callback_free = boolean_field(
            value, "user_code_callback_free", context),
    };
}

[[nodiscard]] inline analysis_completeness parse_analysis(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .template_instantiation_complete = boolean_field(
            value, "template_instantiation_complete", context),
        .macro_expansion_complete = boolean_field(
            value, "macro_expansion_complete", context),
        .cfg_complete = boolean_field(
            value, "cfg_complete", context),
        .lifetime_analysis_complete = boolean_field(
            value, "lifetime_analysis_complete", context),
        .call_graph_fixed_point_complete = boolean_field(
            value, "call_graph_fixed_point_complete", context),
        .unresolved_indirect_targets = parse_strings(
            json_field(
                value, "unresolved_indirect_targets", context),
            std::string{context}
                + ".unresolved_indirect_targets"),
        .unavailable_bodies = parse_strings(
            json_field(value, "unavailable_bodies", context),
            std::string{context} + ".unavailable_bodies"),
        .control_flow_assembly_sites = parse_strings(
            json_field(
                value, "control_flow_assembly_sites", context),
            std::string{context}
                + ".control_flow_assembly_sites"),
    };
}

[[nodiscard]] inline safety_parent_row parse_parent(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    safety_parent_row result{
        .sql_identity = string_field(
            value, "sql_identity", context),
        .owner_usr = string_field(value, "owner_usr", context),
        .qualified_declaration = string_field(
            value, "qualified_declaration", context),
        .owner_source_file = string_field(
            value, "owner_source_file", context),
        .owner_source_span = parse_range(
            json_field(value, "owner_source_span", context),
            std::string{context} + ".owner_source_span"),
        .generated_wrapper = string_field(
            value, "generated_wrapper", context),
        .public_symbol = string_field(
            value, "public_symbol", context),
        .profile = parse_profile(
            string_field(value, "profile", context)),
        .transitive_closure_sha256 = string_field(
            value, "transitive_closure_sha256", context),
        .recoverable_site_count = size_value(
            json_field(
                value, "recoverable_site_count", context),
            std::string{context} + ".recoverable_site_count"),
        .terminal_site_count = size_value(
            json_field(
                value, "terminal_site_count", context),
            std::string{context} + ".terminal_site_count"),
        .audited_stop_count = size_value(
            json_field(
                value, "audited_stop_count", context),
            std::string{context} + ".audited_stop_count"),
        .analysis = parse_analysis(
            json_field(value, "analysis", context),
            std::string{context} + ".analysis"),
    };
    const json_value::array& sites = json_array(
        json_field(value, "sites", context),
        std::string{context} + ".sites");
    for (std::size_t index = 0; index < sites.size(); ++index) {
        result.sites.push_back(parse_error_site(
            sites[index],
            std::string{context} + ".sites["
                + std::to_string(index) + "]"));
    }
    const json_value::array& stops = json_array(
        json_field(value, "audited_stops", context),
        std::string{context} + ".audited_stops");
    for (std::size_t index = 0; index < stops.size(); ++index) {
        result.audited_stops.push_back(parse_audited_stop(
            stops[index],
            std::string{context} + ".audited_stops["
                + std::to_string(index) + "]"));
    }
    return result;
}

[[nodiscard]] inline symbol_group parse_symbol_group(
    const json_value& input,
    std::string_view context) {
    const json_value::object& value = json_object(input, context);
    return {
        .public_symbol = string_field(
            value, "public_symbol", context),
        .finfo_symbol = string_field(
            value, "finfo_symbol", context),
        .generated_wrapper = string_field(
            value, "generated_wrapper", context),
        .sql_identities = parse_strings(
            json_field(value, "sql_identities", context),
            std::string{context} + ".sql_identities"),
    };
}

[[nodiscard]] inline json_value::object position_json(
    const source_position& value) {
    return {
        {"byte_offset", json_value{value.byte_offset}},
        {"column", json_value{value.column}},
        {"line", json_value{value.line}},
    };
}

[[nodiscard]] inline json_value::object range_json(
    const source_range& value) {
    return {
        {"begin", json_value{position_json(value.begin)}},
        {"end", json_value{position_json(value.end)}},
    };
}

[[nodiscard]] inline json_value::array strings_json(
    const std::vector<std::string>& values) {
    json_value::array result;
    result.reserve(values.size());
    for (const std::string& value : values) {
        result.emplace_back(value);
    }
    return result;
}

[[nodiscard]] inline json_value::object crossing_json(
    const crossing_type& value) {
    return {
        {"canonical_type", value.canonical_type},
        {"crosses_postgres_longjmp",
         value.crosses_postgres_longjmp},
        {"declaration_span",
         json_value{range_json(value.declaration_span)}},
        {"origin", origin_name(value.origin)},
        {"source_file", value.source_file},
        {"trivially_destructible",
         value.trivially_destructible},
    };
}

[[nodiscard]] inline json_value::array crossings_json(
    const std::vector<crossing_type>& values) {
    json_value::array result;
    result.reserve(values.size());
    for (const crossing_type& value : values) {
        result.emplace_back(crossing_json(value));
    }
    return result;
}

[[nodiscard]] inline json_value reference_json(
    const std::optional<provenance_reference>& reference) {
    if (!reference) return nullptr;
    return json_value::object{
        {"fragment_sha256", reference->fragment_sha256},
        {"postgresql_revision", reference->postgresql_revision},
        {"rationale", reference->rationale},
        {"record_id", reference->record_id},
        {"upstream_file", reference->upstream_file},
        {"upstream_span",
         json_value{range_json(reference->upstream_span)}},
    };
}

[[nodiscard]] inline json_value::object located_range_json(
    const located_source_range& value) {
    return {
        {"source_file", value.source_file},
        {"span", json_value{range_json(value.span)}},
    };
}

[[nodiscard]] inline json_value flow_object_declaration_usr_json(
    const flow_object& value) {
    return value.declaration_usr
        ? json_value{*value.declaration_usr}
        : json_value{nullptr};
}

[[nodiscard]] inline json_value::object flow_object_json(
    const flow_object& value) {
    return {
        {"canonical_type", value.canonical_type},
        {"declaration",
         json_value{located_range_json(value.declaration)}},
        {"declaration_usr",
         flow_object_declaration_usr_json(value)},
        {"identity_kind",
         object_identity_kind_name(value.identity_kind)},
        {"origin", origin_name(value.origin)},
        {"stable_object_id", value.stable_object_id},
        {"trivially_destructible",
         value.trivially_destructible},
    };
}

[[nodiscard]] inline json_value::array flow_objects_json(
    const std::vector<flow_object>& values) {
    std::vector<const flow_object*> ordered;
    ordered.reserve(values.size());
    for (const flow_object& value : values) {
        ordered.push_back(std::addressof(value));
    }
    std::ranges::sort(
        ordered,
        [](const flow_object* left, const flow_object* right) {
            return left->stable_object_id
                < right->stable_object_id;
        });
    json_value::array result;
    result.reserve(ordered.size());
    for (const flow_object* value : ordered) {
        result.emplace_back(flow_object_json(*value));
    }
    return result;
}

[[nodiscard]] inline json_value external_provenance_json(
    const std::optional<external_declaration_provenance>& value) {
    if (!value) return nullptr;
    return json_value::object{
        {"declaration_sha256", value->declaration_sha256},
        {"declaration_span",
         json_value{range_json(value->declaration_span)}},
        {"library_version", value->library_version},
        {"record_id", value->record_id},
        {"source_file", value->source_file},
        {"user_code_callback_free",
         value->user_code_callback_free},
    };
}

[[nodiscard]] inline json_value::object call_target_json(
    const flow_call_target& value) {
    return {
        {"domain", callee_domain_name(value.domain)},
        {"external_mode",
         external_call_mode_name(value.external_mode)},
        {"external_provenance",
         external_provenance_json(value.external_provenance)},
        {"pg_no_longjmp_allowlist_evidence",
         reference_json(
             value.pg_no_longjmp_allowlist_evidence)},
        {"target_qualified_name", value.target_qualified_name},
        {"target_usr", value.target_usr},
    };
}

[[nodiscard]] inline json_value bridge_transition_evidence_json(
    const std::optional<bridge_transition_evidence>& evidence) {
    if (!evidence) return nullptr;
    return json_value::object{
        {"primitive",
         reference_json(
             std::optional<provenance_reference>{
                 evidence->primitive})},
        {"implementation_body_sha256",
         evidence->implementation_body_sha256},
        {"implementation_contract_id",
         evidence->implementation_contract_id},
        {"implementation_contract_sha256",
         evidence->implementation_contract_sha256},
        {"implementation_definition",
         json_value{located_range_json(
             evidence->implementation_definition)}},
        {"implementation_module",
         evidence->implementation_module},
        {"implementation_qualified_name",
         evidence->implementation_qualified_name},
        {"implementation_usr", evidence->implementation_usr},
        {"protected_region_id", evidence->protected_region_id},
        {"target_qualified_name",
         evidence->target_qualified_name},
        {"target_usr", evidence->target_usr},
        {"transition_callsite",
         json_value{
             located_range_json(
                 evidence->transition_callsite)}},
    };
}

[[nodiscard]] inline json_value::array call_targets_json(
    const std::vector<flow_call_target>& values) {
    std::vector<const flow_call_target*> ordered;
    ordered.reserve(values.size());
    for (const flow_call_target& value : values) {
        ordered.push_back(std::addressof(value));
    }
    std::ranges::sort(
        ordered,
        [](const flow_call_target* left,
           const flow_call_target* right) {
            return std::tie(
                       left->target_usr,
                       left->target_qualified_name)
                < std::tie(
                       right->target_usr,
                       right->target_qualified_name);
        });
    json_value::array result;
    result.reserve(ordered.size());
    for (const flow_call_target* value : ordered) {
        result.emplace_back(call_target_json(*value));
    }
    return result;
}

[[nodiscard]] inline json_value::object macro_expansion_json(
    const macro_expansion_record& value) {
    return {
        {"definition",
         json_value{located_range_json(value.definition)}},
        {"definition_sha256", value.definition_sha256},
        {"invocation",
         json_value{located_range_json(value.invocation)}},
        {"name", value.name},
    };
}

[[nodiscard]] inline json_value::object call_json(
    const flow_call_site& value) {
    json_value::array macros;
    macros.reserve(value.macro_chain.size());
    for (const macro_expansion_record& macro :
         value.macro_chain) {
        macros.emplace_back(macro_expansion_json(macro));
    }

    std::vector<const constant_integer_argument*> arguments;
    arguments.reserve(value.constant_integer_arguments.size());
    for (const constant_integer_argument& argument :
         value.constant_integer_arguments) {
        arguments.push_back(std::addressof(argument));
    }
    std::ranges::sort(
        arguments,
        [](const constant_integer_argument* left,
           const constant_integer_argument* right) {
            return left->ordinal < right->ordinal;
        });
    json_value::array constants;
    constants.reserve(arguments.size());
    for (const constant_integer_argument* argument : arguments) {
        constants.emplace_back(json_value::object{
            {"ordinal", json_value{argument->ordinal}},
            {"value",
             argument->value
                 ? json_value{*argument->value}
                 : json_value{nullptr}},
        });
    }

    return {
        {"analyzer_entered_bridge_callable",
         value.analyzer_entered_bridge_callable},
        {"bridge_primitive_evidence",
         bridge_transition_evidence_json(
             value.bridge_primitive_evidence)},
        {"bridge_transition", bridge_name(value.bridge_transition)},
        {"callable_capture_objects",
         json_value{flow_objects_json(
             value.callable_capture_objects)}},
        {"caller_usr", value.caller_usr},
        {"constant_integer_arguments",
         json_value{std::move(constants)}},
        {"expansion",
         json_value{located_range_json(value.expansion)}},
        {"generated_boundary_tail",
         value.generated_boundary_tail},
        {"live_objects",
         json_value{flow_objects_json(value.live_objects)}},
        {"macro_chain", json_value{std::move(macros)}},
        {"resolution", resolution_name(value.resolution)},
        {"spelling",
         json_value{located_range_json(value.spelling)}},
        {"stable_site_id", value.stable_site_id},
        {"target_set_complete", value.target_set_complete},
        {"targets", json_value{call_targets_json(value.targets)}},
    };
}

[[nodiscard]] inline json_value::array sorted_strings_json(
    const std::vector<std::string>& values) {
    std::vector<std::string_view> ordered;
    ordered.reserve(values.size());
    for (const std::string& value : values) {
        ordered.push_back(value);
    }
    std::ranges::sort(ordered);
    json_value::array result;
    result.reserve(ordered.size());
    for (std::string_view value : ordered) {
        result.emplace_back(value);
    }
    return result;
}

[[nodiscard]] inline json_value::object runtime_probe_fact_json(
    const runtime_probe_fact& value) {
    validate_runtime_probe_fact_shape(
        value, "compiler analysis facts.runtime_probes");
    return {
        {"fault_key", value.fault_key},
        {"relation", runtime_relation_name(value.relation)},
        {"site_call_stable_id", value.site_call_stable_id},
        {"trigger_call_stable_id",
         value.trigger_call_stable_id
             ? json_value{*value.trigger_call_stable_id}
             : json_value{nullptr}},
        {"visit_call_stable_id", value.visit_call_stable_id},
    };
}

[[nodiscard]] inline json_value::array runtime_probes_json(
    const std::vector<runtime_probe_fact>& values) {
    std::vector<const runtime_probe_fact*> ordered;
    ordered.reserve(values.size());
    for (const runtime_probe_fact& value : values) {
        ordered.push_back(std::addressof(value));
    }
    std::ranges::sort(
        ordered,
        [](const runtime_probe_fact* left,
           const runtime_probe_fact* right) {
            const auto key = [](const runtime_probe_fact* value) {
                return std::tuple{
                    std::string_view{value->fault_key},
                    std::string_view{
                        value->site_call_stable_id},
                    std::string_view{
                        value->visit_call_stable_id},
                    value->trigger_call_stable_id
                        ? std::string_view{
                              *value->trigger_call_stable_id}
                        : std::string_view{},
                    runtime_relation_name(value->relation),
                };
            };
            return key(left) < key(right);
        });
    json_value::array result;
    result.reserve(ordered.size());
    for (const runtime_probe_fact* value : ordered) {
        result.emplace_back(runtime_probe_fact_json(*value));
    }
    return result;
}

[[nodiscard]] inline json_value::object
postgres_callback_root_json(const postgres_callback_root& value) {
    return {
        {"family", value.family},
        {"owner_qualified_name", value.owner_qualified_name},
        {"owner_usr", value.owner_usr},
        {"registration",
         json_value{located_range_json(value.registration)}},
        {"slot", value.slot},
        {"stable_callback_id", value.stable_callback_id},
        {"target_definition",
         json_value{located_range_json(value.target_definition)}},
        {"target_qualified_name", value.target_qualified_name},
        {"target_usr", value.target_usr},
    };
}

[[nodiscard]] inline json_value::array postgres_callback_roots_json(
    const std::vector<postgres_callback_root>& values) {
    std::vector<const postgres_callback_root*> ordered;
    ordered.reserve(values.size());
    for (const postgres_callback_root& value : values) {
        ordered.push_back(std::addressof(value));
    }
    std::ranges::sort(
        ordered,
        [](const postgres_callback_root* left,
           const postgres_callback_root* right) {
            return left->stable_callback_id
                < right->stable_callback_id;
        });
    json_value::array result;
    result.reserve(ordered.size());
    for (const postgres_callback_root* value : ordered) {
        result.emplace_back(postgres_callback_root_json(*value));
    }
    return result;
}

[[nodiscard]] inline json_value::object runtime_source_anchor_json(
    const runtime_source_anchor& value) {
    validate_runtime_source_anchor_shape(
        value, "runtime inventory anchor");
    return {
        {"compiler_stable_id", value.compiler_stable_id},
        {"enclosing_function_usr",
         value.enclosing_function_usr},
        {"expanded_source_span",
         json_value{range_json(value.expanded_source_span)}},
        {"kind", runtime_anchor_kind_name(value.kind)},
        {"source_file", value.source_file},
        {"source_fragment_sha256",
         value.source_fragment_sha256},
    };
}

[[nodiscard]] inline json_value::object runtime_probe_contract_json(
    const runtime_probe_contract& value) {
    return {
        {"abi_version",
         json_value{
             static_cast<std::size_t>(value.abi_version)}},
        {"expected_trigger_count",
         json_value{value.expected_trigger_count}},
        {"expected_visit_count",
         json_value{value.expected_visit_count}},
        {"ordering",
         runtime_probe_ordering_name(value.ordering)},
    };
}

[[nodiscard]] inline json_value::array inventory_call_path_json(
    const std::vector<call_path_step>& path) {
    json_value::array result;
    result.reserve(path.size());
    for (const call_path_step& step : path) {
        result.emplace_back(json_value::object{
            {"qualified_declaration",
             step.qualified_declaration},
            {"source_file", step.source_file},
            {"source_span",
             json_value{range_json(step.source_span)}},
            {"usr", step.usr},
        });
    }
    return result;
}

[[nodiscard]] inline json_value::object
runtime_probe_inventory_entry_json(
    const runtime_probe_inventory_entry& value) {
    json_value trigger{nullptr};
    if (value.trigger_anchor) {
        trigger = json_value{
            runtime_source_anchor_json(
                *value.trigger_anchor)};
    }
    return {
        {"fault_key", value.fault_key},
        {"ledger_site_sha256",
         value.ledger_site_sha256},
        {"probe_contract",
         json_value{
             runtime_probe_contract_json(
                 value.probe_contract)}},
        {"relation", runtime_relation_name(value.relation)},
        {"site_anchor",
         json_value{
             runtime_source_anchor_json(
                 value.site_anchor)}},
        {"site_id", value.site_id},
        {"sql_identity", value.sql_identity},
        {"transitive_call_path",
         json_value{
             inventory_call_path_json(
                 value.transitive_call_path)}},
        {"trigger_anchor", std::move(trigger)},
        {"visit_anchor",
         json_value{
             runtime_source_anchor_json(
                 value.visit_anchor)}},
    };
}

[[nodiscard]] inline json_value::object function_flow_json(
    const function_flow& value) {
    std::vector<const flow_call_site*> calls;
    calls.reserve(value.calls.size());
    for (const flow_call_site& call : value.calls) {
        calls.push_back(std::addressof(call));
    }
    std::ranges::sort(
        calls,
        [](const flow_call_site* left,
           const flow_call_site* right) {
            return std::tie(
                       left->stable_site_id,
                       left->expansion.source_file)
                < std::tie(
                       right->stable_site_id,
                       right->expansion.source_file);
        });
    json_value::array rendered_calls;
    rendered_calls.reserve(calls.size());
    for (const flow_call_site* call : calls) {
        rendered_calls.emplace_back(call_json(*call));
    }
    return {
        {"ast_callsite_count",
         json_value{value.ast_callsite_count}},
        {"body_available", value.body_available},
        {"body_sha256", value.body_sha256},
        {"body_span", json_value{range_json(value.body_span)}},
        {"calls", json_value{std::move(rendered_calls)}},
        {"cfg_block_count", json_value{value.cfg_block_count}},
        {"cfg_complete", value.cfg_complete},
        {"cfg_element_count",
         json_value{value.cfg_element_count}},
        {"cfg_implicit_callsite_count",
         json_value{value.cfg_implicit_callsite_count}},
        {"cfg_sha256", value.cfg_sha256},
        {"control_flow_assembly_sites",
         json_value{sorted_strings_json(
             value.control_flow_assembly_sites)}},
        {"declaration_span",
         json_value{range_json(value.declaration_span)}},
        {"emitted_callsite_count",
         json_value{value.emitted_callsite_count}},
        {"implicit_call_edges_complete",
         value.implicit_call_edges_complete},
        {"lifetime_analysis_complete",
         value.lifetime_analysis_complete},
        {"lifetime_inventory_sha256",
         value.lifetime_inventory_sha256},
        {"lifetime_object_ids",
         json_value{sorted_strings_json(
             value.lifetime_object_ids)}},
        {"macro_expansion_complete",
         value.macro_expansion_complete},
        {"module_name", value.module_name},
        {"qualified_name", value.qualified_name},
        {"source_file", value.source_file},
        {"stable_usr", value.stable_usr},
        {"template_instantiation_complete",
         value.template_instantiation_complete},
        {"unavailable_bodies",
         json_value{sorted_strings_json(
             value.unavailable_bodies)}},
        {"unresolved_indirect_targets",
         json_value{sorted_strings_json(
             value.unresolved_indirect_targets)}},
    };
}

[[nodiscard]] inline json_value::object site_json(
    const error_site& site) {
    json_value::array path;
    for (const call_path_step& step :
         site.transitive_call_path) {
        path.emplace_back(json_value::object{
            {"qualified_declaration",
             step.qualified_declaration},
            {"source_file", step.source_file},
            {"source_span",
             json_value{range_json(step.source_span)}},
            {"usr", step.usr},
        });
    }
    json_value::array severities;
    for (postgres_severity severity :
         site.possible_severities) {
        severities.emplace_back(severity_name(severity));
    }
    json_value bridge_evidence{nullptr};
    if (site.bridge_evidence) {
        bridge_evidence = json_value::object{
            {"primitive",
             reference_json(
                 std::optional<provenance_reference>{
                     site.bridge_evidence->primitive})},
            {"implementation_body_sha256",
             site.bridge_evidence->implementation_body_sha256},
            {"implementation_contract_id",
             site.bridge_evidence->implementation_contract_id},
            {"implementation_contract_sha256",
             site.bridge_evidence
                 ->implementation_contract_sha256},
            {"implementation_module",
             site.bridge_evidence->implementation_module},
            {"implementation_qualified_name",
             site.bridge_evidence
                 ->implementation_qualified_name},
            {"implementation_source_file",
             site.bridge_evidence->implementation_source_file},
            {"implementation_source_span",
             json_value{range_json(
                 site.bridge_evidence
                     ->implementation_source_span)}},
            {"implementation_usr",
             site.bridge_evidence->implementation_usr},
            {"protected_region_id",
             site.bridge_evidence->protected_region_id},
            {"target_qualified_name",
             site.bridge_evidence->target_qualified_name},
            {"target_usr",
             site.bridge_evidence->target_usr},
            {"transition_source_file",
             site.bridge_evidence->transition_source_file},
            {"transition_source_span",
             json_value{range_json(
                 site.bridge_evidence
                     ->transition_source_span)}},
        };
    }
    return {
        {"analyzer_entered_bridge_callable",
         site.analyzer_entered_bridge_callable},
        {"bridge", bridge_name(site.bridge)},
        {"bridge_evidence", std::move(bridge_evidence)},
        {"bridge_callable_crossing_types",
         json_value{crossings_json(
             site.bridge_callable_crossing_types)}},
        {"callee_or_macro", site.callee_or_macro},
        {"classification",
         classification_name(site.classification)},
        {"cleanup_phases",
         json_value{strings_json(site.cleanup_phases)}},
        {"converter_families",
         json_value{strings_json(site.converter_families)}},
        {"crossing_types",
         json_value{crossings_json(site.crossing_types)}},
        {"expanded_source_span",
         json_value{range_json(site.expanded_source_span)}},
        {"generated_boundary_tail",
         site.generated_boundary_tail},
        {"no_longjmp_allowlist_evidence",
         reference_json(site.no_longjmp_allowlist_evidence)},
        {"owner_families",
         json_value{strings_json(site.owner_families)}},
        {"possible_severities", json_value{std::move(severities)}},
        {"resolution", resolution_name(site.resolution)},
        {"severity_constant_propagated",
         site.severity_constant_propagated},
        {"site_id", site.site_id},
        {"source_file", site.source_file},
        {"transitive_call_path", json_value{std::move(path)}},
    };
}

[[nodiscard]] inline json_value::object audited_stop_json(
    const audited_stop& stop) {
    json_value::array path;
    for (const call_path_step& step :
         stop.transitive_call_path) {
        path.emplace_back(json_value::object{
            {"qualified_declaration",
             step.qualified_declaration},
            {"source_file", step.source_file},
            {"source_span",
             json_value{range_json(step.source_span)}},
            {"usr", step.usr},
        });
    }
    return {
        {"domain", audited_stop_domain_name(stop.domain)},
        {"evidence",
         reference_json(
             std::optional<provenance_reference>{
                 stop.evidence})},
        {"expanded_source_span",
         json_value{range_json(stop.expanded_source_span)}},
        {"library_version", stop.library_version},
        {"resolution", resolution_name(stop.resolution)},
        {"source_file", stop.source_file},
        {"stop_id", stop.stop_id},
        {"target_qualified_name",
         stop.target_qualified_name},
        {"target_usr", stop.target_usr},
        {"transitive_call_path",
         json_value{std::move(path)}},
        {"user_code_callback_free",
         stop.user_code_callback_free},
    };
}

[[nodiscard]] inline json_value::object analysis_json(
    const analysis_completeness& analysis) {
    return {
        {"call_graph_fixed_point_complete",
         analysis.call_graph_fixed_point_complete},
        {"cfg_complete", analysis.cfg_complete},
        {"control_flow_assembly_sites",
         json_value{strings_json(
             analysis.control_flow_assembly_sites)}},
        {"lifetime_analysis_complete",
         analysis.lifetime_analysis_complete},
        {"macro_expansion_complete",
         analysis.macro_expansion_complete},
        {"template_instantiation_complete",
         analysis.template_instantiation_complete},
        {"unavailable_bodies",
         json_value{strings_json(analysis.unavailable_bodies)}},
        {"unresolved_indirect_targets",
         json_value{strings_json(
             analysis.unresolved_indirect_targets)}},
    };
}

}  // namespace serialization_detail

[[nodiscard]] inline provenance_bundle parse_provenance_bundle_json(
    std::string_view text) {
    const json_value document = parse_json(text);
    const json_value::object& root =
        json_object(document, "provenance");
    provenance_bundle result{
        .schema_version = static_cast<std::uint32_t>(
            serialization_detail::size_value(
                json_field(root, "schema_version", "provenance"),
                "provenance.schema_version")),
        .postgresql_major = static_cast<std::uint32_t>(
            serialization_detail::size_value(
                json_field(
                    root, "postgresql_major", "provenance"),
                "provenance.postgresql_major")),
        .postgresql_tag =
            serialization_detail::string_field(
                root, "postgresql_tag", "provenance"),
        .postgresql_revision =
            serialization_detail::string_field(
                root, "postgresql_revision", "provenance"),
    };
    const json_value::array& records = json_array(
        json_field(root, "records", "provenance"),
        "provenance.records");
    result.records.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        const std::string context =
            "provenance.records[" + std::to_string(index) + "]";
        const json_value::object& record =
            json_object(records[index], context);
        provenance_record parsed{
            .record_id = serialization_detail::string_field(
                record, "record_id", context),
            .kind = serialization_detail::parse_provenance_kind(
                serialization_detail::string_field(
                    record, "kind", context)),
            .spelling = serialization_detail::string_field(
                record, "spelling", context),
            .upstream_file =
                serialization_detail::string_field(
                    record, "upstream_file", context),
            .upstream_span = serialization_detail::parse_range(
                json_field(record, "upstream_span", context),
                context + ".upstream_span"),
            .upstream_file_sha256 =
                serialization_detail::string_field(
                    record,
                    "upstream_file_sha256",
                    context),
            .fragment_sha256 =
                serialization_detail::string_field(
                    record, "fragment_sha256", context),
            .rationale = serialization_detail::string_field(
                record, "rationale", context),
        };
        const json_value::array& severities = json_array(
            json_field(record, "severities", context),
            context + ".severities");
        for (const json_value& severity : severities) {
            parsed.severities.push_back(
                serialization_detail::parse_severity(
                    json_string(severity, context + ".severities")));
        }
        if (const json_value* version =
                serialization_detail::optional_field(
                    record, "library_version")) {
            parsed.library_version = std::string{
                json_string(*version, context + ".library_version")};
        }
        if (const json_value* target_usr =
                serialization_detail::optional_field(
                    record, "target_usr")) {
            parsed.target_usr = std::string{json_string(
                *target_usr, context + ".target_usr")};
        }
        if (const json_value* target_name =
                serialization_detail::optional_field(
                    record, "target_qualified_name")) {
            parsed.target_qualified_name =
                std::string{json_string(
                    *target_name,
                    context + ".target_qualified_name")};
        }
        if (const json_value* authority =
                serialization_detail::optional_field(
                    record, "authority_id")) {
            parsed.authority_id = std::string{json_string(
                *authority, context + ".authority_id")};
        }
        if (const json_value* definition =
                serialization_detail::optional_field(
                    record, "definition")) {
            const std::string definition_context =
                context + ".definition";
            const json_value::object& value =
                json_object(*definition, definition_context);
            serialization_detail::require_exact_fields(
                value,
                {
                    "fragment_sha256",
                    "upstream_file",
                    "upstream_file_sha256",
                    "upstream_span",
                },
                definition_context);
            parsed.definition = provenance_definition{
                .upstream_file =
                    serialization_detail::string_field(
                        value,
                        "upstream_file",
                        definition_context),
                .upstream_span =
                    serialization_detail::parse_range(
                        json_field(
                            value,
                            "upstream_span",
                            definition_context),
                        definition_context + ".upstream_span"),
                .upstream_file_sha256 =
                    serialization_detail::string_field(
                        value,
                        "upstream_file_sha256",
                        definition_context),
                .fragment_sha256 =
                    serialization_detail::string_field(
                        value,
                        "fragment_sha256",
                        definition_context),
            };
        }
        if (const json_value* callback_free =
                serialization_detail::optional_field(
                    record, "user_code_callback_free")) {
            parsed.user_code_callback_free = json_boolean(
                *callback_free,
                context + ".user_code_callback_free");
        }
        result.records.push_back(std::move(parsed));
    }
    return result;
}

[[nodiscard]] inline compiler_analysis_facts
parse_compiler_analysis_facts_json(std::string_view text) {
    const json_value document = parse_json(text);
    const json_value::object& root =
        json_object(document, "compiler analysis facts");
    serialization_detail::require_exact_fields(
        root,
        {
            "callback_roots",
            "clang_version",
            "compile_database_sha256",
            "compile_graph_sha256",
            "compiler_profile",
            "functions",
            "pcm_manifest_sha256",
            "producer_executable_sha256",
            "profile",
            "root_usrs",
            "runtime_probes",
            "schema_version",
            "source_list_sha256",
            "sql_manifest_sha256",
        },
        "compiler analysis facts");
    const std::size_t schema_version =
        serialization_detail::size_value(
            json_field(
                root,
                "schema_version",
                "compiler analysis facts"),
            "compiler analysis facts.schema_version");
    if (schema_version != 2) {
        throw std::runtime_error(
            "compiler analysis facts require schema version 2");
    }
    compiler_analysis_facts result{
        .schema_version = 2,
        .profile = serialization_detail::parse_profile(
            serialization_detail::string_field(
                root, "profile", "compiler analysis facts")),
        .compiler_profile =
            serialization_detail::string_field(
                root,
                "compiler_profile",
                "compiler analysis facts"),
        .clang_version =
            serialization_detail::string_field(
                root,
                "clang_version",
                "compiler analysis facts"),
        .source_list_sha256 =
            serialization_detail::string_field(
                root,
                "source_list_sha256",
                "compiler analysis facts"),
        .compile_database_sha256 =
            serialization_detail::string_field(
                root,
                "compile_database_sha256",
                "compiler analysis facts"),
        .producer_executable_sha256 =
            serialization_detail::string_field(
                root,
                "producer_executable_sha256",
                "compiler analysis facts"),
        .compile_graph_sha256 =
            serialization_detail::string_field(
                root,
                "compile_graph_sha256",
                "compiler analysis facts"),
        .pcm_manifest_sha256 =
            serialization_detail::string_field(
                root,
                "pcm_manifest_sha256",
                "compiler analysis facts"),
        .sql_manifest_sha256 =
            serialization_detail::string_field(
                root,
                "sql_manifest_sha256",
                "compiler analysis facts"),
        .root_usrs = serialization_detail::parse_strings(
            json_field(
                root, "root_usrs", "compiler analysis facts"),
            "compiler analysis facts.root_usrs"),
        .callback_roots = [&] {
            std::vector<postgres_callback_root> parsed;
            const json_value::array& values = json_array(
                json_field(
                    root,
                    "callback_roots",
                    "compiler analysis facts"),
                "compiler analysis facts.callback_roots");
            parsed.reserve(values.size());
            for (std::size_t index = 0;
                 index < values.size();
                 ++index) {
                parsed.push_back(
                    serialization_detail::
                        parse_postgres_callback_root(
                            values[index],
                            "compiler analysis facts."
                            "callback_roots["
                                + std::to_string(index) + "]"));
            }
            return parsed;
        }(),
        .functions = {},
        .runtime_probes = [&] {
            std::vector<runtime_probe_fact> parsed;
            const json_value::array& values = json_array(
                json_field(
                    root,
                    "runtime_probes",
                    "compiler analysis facts"),
                "compiler analysis facts.runtime_probes");
            parsed.reserve(values.size());
            for (std::size_t index = 0;
                 index < values.size();
                 ++index) {
                parsed.push_back(
                    serialization_detail::
                        parse_runtime_probe_fact(
                            values[index],
                            "compiler analysis facts."
                            "runtime_probes["
                                + std::to_string(index) + "]"));
            }
            return parsed;
        }(),
    };
    const json_value::array& functions = json_array(
        json_field(root, "functions", "compiler analysis facts"),
        "compiler analysis facts.functions");
    result.functions.reserve(functions.size());
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const std::string context =
            "compiler analysis facts.functions["
            + std::to_string(index) + "]";
        const json_value::object& function =
            json_object(functions[index], context);
        function_flow parsed{
            .stable_usr = serialization_detail::string_field(
                function, "stable_usr", context),
            .qualified_name =
                serialization_detail::string_field(
                    function, "qualified_name", context),
            .source_file = serialization_detail::string_field(
                function, "source_file", context),
            .module_name = serialization_detail::string_field(
                function, "module_name", context, true),
            .declaration_span =
                serialization_detail::parse_range(
                    json_field(
                        function, "declaration_span", context),
                    context + ".declaration_span"),
            .body_span = serialization_detail::parse_range(
                json_field(function, "body_span", context),
                context + ".body_span"),
            .body_available =
                serialization_detail::boolean_field(
                    function, "body_available", context),
            .template_instantiation_complete =
                serialization_detail::boolean_field(
                    function,
                    "template_instantiation_complete",
                    context),
            .macro_expansion_complete =
                serialization_detail::boolean_field(
                    function,
                    "macro_expansion_complete",
                    context),
            .cfg_complete =
                serialization_detail::boolean_field(
                    function, "cfg_complete", context),
            .lifetime_analysis_complete =
                serialization_detail::boolean_field(
                    function,
                    "lifetime_analysis_complete",
                    context),
            .implicit_call_edges_complete =
                serialization_detail::boolean_field(
                    function,
                    "implicit_call_edges_complete",
                    context),
            .body_sha256 =
                serialization_detail::string_field(
                    function, "body_sha256", context),
            .cfg_sha256 =
                serialization_detail::string_field(
                    function, "cfg_sha256", context),
            .cfg_block_count =
                serialization_detail::size_value(
                    json_field(
                        function, "cfg_block_count", context),
                    context + ".cfg_block_count"),
            .cfg_element_count =
                serialization_detail::size_value(
                    json_field(
                        function, "cfg_element_count", context),
                    context + ".cfg_element_count"),
            .ast_callsite_count =
                serialization_detail::size_value(
                    json_field(
                        function, "ast_callsite_count", context),
                    context + ".ast_callsite_count"),
            .cfg_implicit_callsite_count =
                serialization_detail::size_value(
                    json_field(
                        function,
                        "cfg_implicit_callsite_count",
                        context),
                    context
                        + ".cfg_implicit_callsite_count"),
            .emitted_callsite_count =
                serialization_detail::size_value(
                    json_field(
                        function,
                        "emitted_callsite_count",
                        context),
                    context + ".emitted_callsite_count"),
            .lifetime_object_ids =
                serialization_detail::parse_strings(
                    json_field(
                        function,
                        "lifetime_object_ids",
                        context),
                    context + ".lifetime_object_ids"),
            .lifetime_inventory_sha256 =
                serialization_detail::string_field(
                    function,
                    "lifetime_inventory_sha256",
                    context),
            .unresolved_indirect_targets =
                serialization_detail::parse_strings(
                    json_field(
                        function,
                        "unresolved_indirect_targets",
                        context),
                    context
                        + ".unresolved_indirect_targets"),
            .unavailable_bodies =
                serialization_detail::parse_strings(
                    json_field(
                        function,
                        "unavailable_bodies",
                        context),
                    context + ".unavailable_bodies"),
            .control_flow_assembly_sites =
                serialization_detail::parse_strings(
                    json_field(
                        function,
                        "control_flow_assembly_sites",
                        context),
                    context
                        + ".control_flow_assembly_sites"),
        };
        const json_value::array& calls = json_array(
            json_field(function, "calls", context),
            context + ".calls");
        parsed.calls.reserve(calls.size());
        for (std::size_t call_index = 0;
             call_index < calls.size();
             ++call_index) {
            parsed.calls.push_back(
                serialization_detail::parse_call(
                    calls[call_index],
                    context + ".calls["
                        + std::to_string(call_index) + "]"));
        }
        result.functions.push_back(std::move(parsed));
    }
    return result;
}

[[nodiscard]] inline runtime_probe_inventory
parse_runtime_probe_inventory_json(std::string_view text) {
    const json_value document = parse_json(text);
    const json_value::object& root =
        json_object(document, "runtime probe inventory");
    serialization_detail::require_exact_fields(
        root,
        {
            "compile_database_sha256",
            "compile_graph_sha256",
            "compiler_facts_sha256",
            "entries",
            "inventory_producer_executable_sha256",
            "ledger_sha256",
            "pcm_manifest_sha256",
            "probe_abi_version",
            "profile",
            "schema_version",
            "source_list_sha256",
            "sql_manifest_sha256",
        },
        "runtime probe inventory");
    const std::size_t schema_version =
        serialization_detail::size_value(
            json_field(
                root,
                "schema_version",
                "runtime probe inventory"),
            "runtime probe inventory.schema_version");
    const std::size_t probe_abi_version =
        serialization_detail::size_value(
            json_field(
                root,
                "probe_abi_version",
                "runtime probe inventory"),
            "runtime probe inventory.probe_abi_version");
    if (schema_version != 1 || probe_abi_version != 2) {
        throw std::runtime_error(
            "runtime probe inventory requires schema version 1 "
            "and probe ABI version 2");
    }
    runtime_probe_inventory result{
        .schema_version = 1,
        .profile = serialization_detail::parse_profile(
            serialization_detail::string_field(
                root,
                "profile",
                "runtime probe inventory")),
        .probe_abi_version = 2,
        .ledger_sha256 =
            serialization_detail::string_field(
                root,
                "ledger_sha256",
                "runtime probe inventory"),
        .compiler_facts_sha256 =
            serialization_detail::string_field(
                root,
                "compiler_facts_sha256",
                "runtime probe inventory"),
        .sql_manifest_sha256 =
            serialization_detail::string_field(
                root,
                "sql_manifest_sha256",
                "runtime probe inventory"),
        .source_list_sha256 =
            serialization_detail::string_field(
                root,
                "source_list_sha256",
                "runtime probe inventory"),
        .compile_database_sha256 =
            serialization_detail::string_field(
                root,
                "compile_database_sha256",
                "runtime probe inventory"),
        .compile_graph_sha256 =
            serialization_detail::string_field(
                root,
                "compile_graph_sha256",
                "runtime probe inventory"),
        .pcm_manifest_sha256 =
            serialization_detail::string_field(
                root,
                "pcm_manifest_sha256",
                "runtime probe inventory"),
        .inventory_producer_executable_sha256 =
            serialization_detail::string_field(
                root,
                "inventory_producer_executable_sha256",
                "runtime probe inventory"),
    };
    const json_value::array& entries = json_array(
        json_field(
            root, "entries", "runtime probe inventory"),
        "runtime probe inventory.entries");
    result.entries.reserve(entries.size());
    for (std::size_t index = 0;
         index < entries.size();
         ++index) {
        result.entries.push_back(
            serialization_detail::
                parse_runtime_probe_inventory_entry(
                    entries[index],
                    "runtime probe inventory.entries["
                        + std::to_string(index) + "]"));
    }
    serialization_detail::
        validate_runtime_probe_inventory_shape(result);
    return result;
}

[[nodiscard]] inline safety_ledger parse_safety_ledger_json(
    std::string_view text) {
    const json_value document = parse_json(text);
    const json_value::object& root =
        json_object(document, "safety ledger");
    const std::size_t schema_version =
        serialization_detail::size_value(
            json_field(
                root, "schema_version", "safety ledger"),
            "safety ledger.schema_version");
    const std::size_t postgresql_major =
        serialization_detail::size_value(
            json_field(
                root, "postgresql_major", "safety ledger"),
            "safety ledger.postgresql_major");
    if (schema_version
            > std::numeric_limits<std::uint32_t>::max()
        || postgresql_major
            > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "safety ledger version fields exceed uint32");
    }
    safety_ledger result{
        .schema_version =
            static_cast<std::uint32_t>(schema_version),
        .profile = serialization_detail::parse_profile(
            serialization_detail::string_field(
                root, "profile", "safety ledger")),
        .postgresql_major =
            static_cast<std::uint32_t>(postgresql_major),
        .postgresql_revision =
            serialization_detail::string_field(
                root,
                "postgresql_revision",
                "safety ledger"),
        .provenance_bundle_sha256 =
            serialization_detail::string_field(
                root,
                "provenance_bundle_sha256",
                "safety ledger"),
        .source_list_sha256 =
            serialization_detail::string_field(
                root,
                "source_list_sha256",
                "safety ledger"),
        .compile_database_sha256 =
            serialization_detail::string_field(
                root,
                "compile_database_sha256",
                "safety ledger"),
        .producer_executable_sha256 =
            serialization_detail::string_field(
                root,
                "producer_executable_sha256",
                "safety ledger"),
        .compile_graph_sha256 =
            serialization_detail::string_field(
                root,
                "compile_graph_sha256",
                "safety ledger"),
        .pcm_manifest_sha256 =
            serialization_detail::string_field(
                root,
                "pcm_manifest_sha256",
                "safety ledger"),
        .sql_manifest_sha256 =
            serialization_detail::string_field(
                root,
                "sql_manifest_sha256",
                "safety ledger"),
        .compiler_facts_sha256 =
            serialization_detail::string_field(
                root,
                "compiler_facts_sha256",
                "safety ledger"),
        .clang_version =
            serialization_detail::string_field(
                root, "clang_version", "safety ledger"),
        .analyzer_version =
            serialization_detail::string_field(
                root, "analyzer_version", "safety ledger"),
        .generator_revision =
            serialization_detail::string_field(
                root,
                "generator_revision",
                "safety ledger"),
        .callback_root_count =
            serialization_detail::size_value(
                json_field(
                    root,
                    "callback_root_count",
                    "safety ledger"),
                "safety ledger.callback_root_count"),
        .callback_closure_sha256 =
            serialization_detail::string_field(
                root,
                "callback_closure_sha256",
                "safety ledger",
                true),
    };
    const json_value::array& parents = json_array(
        json_field(root, "parents", "safety ledger"),
        "safety ledger.parents");
    result.parents.reserve(parents.size());
    for (std::size_t index = 0; index < parents.size(); ++index) {
        result.parents.push_back(
            serialization_detail::parse_parent(
                parents[index],
                "safety ledger.parents["
                    + std::to_string(index) + "]"));
    }
    const json_value::array& groups = json_array(
        json_field(root, "symbol_groups", "safety ledger"),
        "safety ledger.symbol_groups");
    result.symbol_groups.reserve(groups.size());
    for (std::size_t index = 0; index < groups.size(); ++index) {
        result.symbol_groups.push_back(
            serialization_detail::parse_symbol_group(
                groups[index],
                "safety ledger.symbol_groups["
                    + std::to_string(index) + "]"));
    }
    return result;
}

[[nodiscard]] inline std::string render_compiler_analysis_facts_json(
    const compiler_analysis_facts& facts) {
    if (facts.schema_version != 2) {
        throw std::runtime_error(
            "compiler analysis facts require schema version 2");
    }
    std::vector<const function_flow*> functions;
    functions.reserve(facts.functions.size());
    for (const function_flow& function : facts.functions) {
        functions.push_back(std::addressof(function));
    }
    std::ranges::sort(
        functions,
        [](const function_flow* left,
           const function_flow* right) {
            return std::tie(
                       left->stable_usr,
                       left->qualified_name,
                       left->source_file)
                < std::tie(
                       right->stable_usr,
                       right->qualified_name,
                       right->source_file);
        });
    json_value::array rendered_functions;
    rendered_functions.reserve(functions.size());
    for (const function_flow* function : functions) {
        rendered_functions.emplace_back(
            serialization_detail::function_flow_json(*function));
    }
    return render_json(json_value::object{
        {"callback_roots",
         json_value{
             serialization_detail::postgres_callback_roots_json(
                 facts.callback_roots)}},
        {"clang_version", facts.clang_version},
        {"compile_database_sha256",
         facts.compile_database_sha256},
        {"compile_graph_sha256", facts.compile_graph_sha256},
        {"compiler_profile", facts.compiler_profile},
        {"functions", json_value{std::move(rendered_functions)}},
        {"pcm_manifest_sha256", facts.pcm_manifest_sha256},
        {"profile", profile_name(facts.profile)},
        {"producer_executable_sha256",
         facts.producer_executable_sha256},
        {"root_usrs",
         json_value{
             serialization_detail::sorted_strings_json(
                 facts.root_usrs)}},
        {"runtime_probes",
         json_value{
             serialization_detail::runtime_probes_json(
                 facts.runtime_probes)}},
        {"schema_version",
         json_value{
             static_cast<std::size_t>(facts.schema_version)}},
        {"source_list_sha256", facts.source_list_sha256},
        {"sql_manifest_sha256", facts.sql_manifest_sha256},
    });
}

[[nodiscard]] inline std::string
render_runtime_probe_inventory_json(
    const runtime_probe_inventory& inventory) {
    serialization_detail::
        validate_runtime_probe_inventory_shape(inventory);
    std::vector<const runtime_probe_inventory_entry*> entries;
    entries.reserve(inventory.entries.size());
    for (const runtime_probe_inventory_entry& entry :
         inventory.entries) {
        entries.push_back(std::addressof(entry));
    }
    std::ranges::sort(
        entries,
        [](const runtime_probe_inventory_entry* left,
           const runtime_probe_inventory_entry* right) {
            return std::tie(
                       left->sql_identity,
                       left->fault_key,
                       left->site_id)
                < std::tie(
                       right->sql_identity,
                       right->fault_key,
                       right->site_id);
        });
    json_value::array rendered_entries;
    rendered_entries.reserve(entries.size());
    for (const runtime_probe_inventory_entry* entry :
         entries) {
        rendered_entries.emplace_back(
            serialization_detail::
                runtime_probe_inventory_entry_json(*entry));
    }
    return render_json(json_value::object{
        {"compile_database_sha256",
         inventory.compile_database_sha256},
        {"compile_graph_sha256",
         inventory.compile_graph_sha256},
        {"compiler_facts_sha256",
         inventory.compiler_facts_sha256},
        {"entries", json_value{std::move(rendered_entries)}},
        {"inventory_producer_executable_sha256",
         inventory.inventory_producer_executable_sha256},
        {"ledger_sha256", inventory.ledger_sha256},
        {"pcm_manifest_sha256",
         inventory.pcm_manifest_sha256},
        {"probe_abi_version",
         json_value{
             static_cast<std::size_t>(
                 inventory.probe_abi_version)}},
        {"profile", profile_name(inventory.profile)},
        {"schema_version",
         json_value{
             static_cast<std::size_t>(
                 inventory.schema_version)}},
        {"source_list_sha256",
         inventory.source_list_sha256},
        {"sql_manifest_sha256",
         inventory.sql_manifest_sha256},
    });
}

[[nodiscard]] inline std::string render_safety_ledger_json(
    const safety_ledger& ledger) {
    json_value::array parents;
    parents.reserve(ledger.parents.size());
    for (const safety_parent_row& parent : ledger.parents) {
        json_value::array sites;
        sites.reserve(parent.sites.size());
        for (const error_site& site : parent.sites) {
            sites.emplace_back(
                serialization_detail::site_json(site));
        }
        json_value::array audited_stops;
        audited_stops.reserve(parent.audited_stops.size());
        for (const audited_stop& stop :
             parent.audited_stops) {
            audited_stops.emplace_back(
                serialization_detail::audited_stop_json(
                    stop));
        }
        parents.emplace_back(json_value::object{
            {"analysis",
             json_value{
                 serialization_detail::analysis_json(
                     parent.analysis)}},
            {"audited_stop_count",
             json_value{parent.audited_stop_count}},
            {"audited_stops",
             json_value{std::move(audited_stops)}},
            {"generated_wrapper", parent.generated_wrapper},
            {"owner_source_file", parent.owner_source_file},
            {"owner_source_span",
             json_value{serialization_detail::range_json(
                 parent.owner_source_span)}},
            {"owner_usr", parent.owner_usr},
            {"profile", profile_name(parent.profile)},
            {"public_symbol", parent.public_symbol},
            {"qualified_declaration",
             parent.qualified_declaration},
            {"recoverable_site_count",
             json_value{parent.recoverable_site_count}},
            {"sites", json_value{std::move(sites)}},
            {"sql_identity", parent.sql_identity},
            {"terminal_site_count",
             json_value{parent.terminal_site_count}},
            {"transitive_closure_sha256",
             parent.transitive_closure_sha256},
        });
    }
    json_value::array groups;
    groups.reserve(ledger.symbol_groups.size());
    for (const symbol_group& group : ledger.symbol_groups) {
        groups.emplace_back(json_value::object{
            {"finfo_symbol", group.finfo_symbol},
            {"generated_wrapper", group.generated_wrapper},
            {"public_symbol", group.public_symbol},
            {"sql_identities",
             json_value{serialization_detail::strings_json(
                 group.sql_identities)}},
        });
    }
    return render_json(json_value::object{
        {"analyzer_version", ledger.analyzer_version},
        {"callback_closure_sha256",
         ledger.callback_closure_sha256},
        {"callback_root_count",
         json_value{ledger.callback_root_count}},
        {"clang_version", ledger.clang_version},
        {"compile_database_sha256",
         ledger.compile_database_sha256},
        {"compile_graph_sha256", ledger.compile_graph_sha256},
        {"compiler_facts_sha256",
         ledger.compiler_facts_sha256},
        {"generator_revision", ledger.generator_revision},
        {"parents", json_value{std::move(parents)}},
        {"pcm_manifest_sha256", ledger.pcm_manifest_sha256},
        {"postgresql_major",
         json_value{
             static_cast<std::size_t>(ledger.postgresql_major)}},
        {"postgresql_revision", ledger.postgresql_revision},
        {"producer_executable_sha256",
         ledger.producer_executable_sha256},
        {"profile", profile_name(ledger.profile)},
        {"provenance_bundle_sha256",
         ledger.provenance_bundle_sha256},
        {"schema_version",
         json_value{
             static_cast<std::size_t>(ledger.schema_version)}},
        {"source_list_sha256", ledger.source_list_sha256},
        {"sql_manifest_sha256", ledger.sql_manifest_sha256},
        {"symbol_groups", json_value{std::move(groups)}},
    });
}

}  // namespace bioseq::codegen::safety
