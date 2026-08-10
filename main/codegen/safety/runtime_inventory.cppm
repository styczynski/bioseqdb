module;

#include <algorithm>
#include <cstddef>
#include <filesystem>
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

export module bioseq.codegen.safety:runtime_inventory;

import :analyzer_model;
import :json;
import :model;
import :serialization;
import :validation;
import bioseq.codegen.support;

export namespace bioseq::codegen::safety {

namespace runtime_inventory_detail {

namespace fs = std::filesystem;

[[noreturn]] inline void inventory_failure(
    std::string_view context,
    std::string_view message) {
    throw std::runtime_error(
        "runtime inventory " + std::string{context}
        + ": " + std::string{message});
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

struct source_authority_record {
    std::string sha256;
    std::optional<std::string> verified_bytes;
};

using source_authority = std::map<
    std::string,
    source_authority_record,
    std::less<>>;

inline void add_source_authority(
    source_authority& result,
    std::string source,
    std::string sha256,
    std::string_view context) {
    if (source.empty() || !detail::lowercase_sha256(sha256)) {
        inventory_failure(
            context,
            "source authority has an empty path or invalid hash");
    }
    const source_authority::iterator found = result.find(source);
    if (found != result.end()) {
        if (found->second.sha256 != sha256) {
            inventory_failure(
                context,
                "one source path has conflicting sealed hashes");
        }
        return;
    }
    result.emplace(
        std::move(source),
        source_authority_record{
            .sha256 = std::move(sha256),
        });
}

inline void validate_string_array(
    const json_value& input,
    std::string_view context) {
    const json_value::array& values = json_array(input, context);
    for (std::size_t index = 0;
         index < values.size();
         ++index) {
        if (json_string(
                values[index],
                std::string{context} + "["
                    + std::to_string(index) + "]")
                .empty()) {
            inventory_failure(
                context, "contains an empty string");
        }
    }
}

[[nodiscard]] inline std::string compile_graph_string(
    const json_value::object& root,
    std::string_view field) {
    return serialization_detail::string_field(
        root, field, "runtime inventory compile graph");
}

[[nodiscard]] inline source_authority
parse_source_authority(
    std::string_view compile_graph_json,
    const compiler_analysis_facts& facts) {
    const json_value document = parse_json(compile_graph_json);
    if (render_json(document) != compile_graph_json) {
        inventory_failure(
            "compile graph",
            "bytes are not canonical JSON");
    }
    const json_value::object& root =
        json_object(document, "runtime inventory compile graph");
    serialization_detail::require_exact_fields(
        root,
        {
            "clang_version",
            "compilation_signature",
            "compile_database",
            "compile_database_sha256",
            "modules",
            "pcm_manifest",
            "pcm_manifest_sha256",
            "pcm_root",
            "postgresql_authenticated_headers",
            "postgresql_header_roots",
            "postgresql_rejected_header_roots",
            "profile",
            "schema",
            "source_list",
            "source_list_sha256",
            "translation_units",
        },
        "runtime inventory compile graph");
    if (compile_graph_string(root, "schema")
            != "bioseq_safety_compile_graph/v1"
        || compile_graph_string(root, "profile")
            != profile_name(facts.profile)
        || compile_graph_string(root, "clang_version")
            != facts.clang_version
        || compile_graph_string(root, "source_list_sha256")
            != facts.source_list_sha256
        || compile_graph_string(root, "compile_database_sha256")
            != facts.compile_database_sha256
        || compile_graph_string(root, "pcm_manifest_sha256")
            != facts.pcm_manifest_sha256
        || compile_graph_string(root, "compilation_signature").empty()
        || compile_graph_string(root, "compile_database").empty()
        || compile_graph_string(root, "pcm_manifest").empty()
        || compile_graph_string(root, "pcm_root").empty()
        || compile_graph_string(root, "source_list").empty()) {
        inventory_failure(
            "compile graph",
            "profile, compiler, or sealed input hashes differ "
            "from compiler facts");
    }
    validate_string_array(
        json_field(
            root,
            "postgresql_header_roots",
            "runtime inventory compile graph"),
        "runtime inventory compile graph."
        "postgresql_header_roots");
    validate_string_array(
        json_field(
            root,
            "postgresql_rejected_header_roots",
            "runtime inventory compile graph"),
        "runtime inventory compile graph."
        "postgresql_rejected_header_roots");

    source_authority result;
    const json_value::array& units = json_array(
        json_field(
            root,
            "translation_units",
            "runtime inventory compile graph"),
        "runtime inventory compile graph.translation_units");
    for (std::size_t index = 0;
         index < units.size();
         ++index) {
        const std::string context =
            "runtime inventory compile graph.translation_units["
            + std::to_string(index) + "]";
        const json_value::object& unit =
            json_object(units[index], context);
        serialization_detail::require_exact_fields(
            unit,
            {
                "arguments",
                "arguments_sha256",
                "directory",
                "source",
                "source_sha256",
            },
            context);
        validate_string_array(
            json_field(unit, "arguments", context),
            context + ".arguments");
        if (!detail::lowercase_sha256(
                serialization_detail::string_field(
                    unit,
                    "arguments_sha256",
                    context))
            || serialization_detail::string_field(
                   unit, "directory", context)
                   .empty()) {
            inventory_failure(
                context,
                "translation-unit command authority is invalid");
        }
        add_source_authority(
            result,
            serialization_detail::string_field(
                unit, "source", context),
            serialization_detail::string_field(
                unit, "source_sha256", context),
            context);
    }

    const json_value::array& modules = json_array(
        json_field(
            root, "modules", "runtime inventory compile graph"),
        "runtime inventory compile graph.modules");
    for (std::size_t index = 0;
         index < modules.size();
         ++index) {
        const std::string context =
            "runtime inventory compile graph.modules["
            + std::to_string(index) + "]";
        const json_value::object& module =
            json_object(modules[index], context);
        add_source_authority(
            result,
            serialization_detail::string_field(
                module, "source", context),
            serialization_detail::string_field(
                module, "source_sha256", context),
            context);
    }

    const json_value::array& headers = json_array(
        json_field(
            root,
            "postgresql_authenticated_headers",
            "runtime inventory compile graph"),
        "runtime inventory compile graph."
        "postgresql_authenticated_headers");
    for (std::size_t index = 0;
         index < headers.size();
         ++index) {
        const std::string context =
            "runtime inventory compile graph."
            "postgresql_authenticated_headers["
            + std::to_string(index) + "]";
        const json_value::object& header =
            json_object(headers[index], context);
        serialization_detail::require_exact_fields(
            header, {"path", "sha256"}, context);
        add_source_authority(
            result,
            serialization_detail::string_field(
                header, "path", context),
            serialization_detail::string_field(
                header, "sha256", context),
            context);
    }
    if (result.empty()) {
        inventory_failure(
            "compile graph",
            "contains no sealed source authority");
    }
    return result;
}

[[nodiscard]] inline std::string source_fragment_sha256(
    const located_source_range& location,
    source_authority& authority,
    std::string_view context) {
    const source_authority::iterator found =
        authority.find(location.source_file);
    if (found == authority.end()) {
        inventory_failure(
            context,
            "expanded source file is absent from the sealed "
            "compile graph");
    }
    source_authority_record& record = found->second;
    if (!record.verified_bytes) {
        if (!fs::is_regular_file(location.source_file)) {
            inventory_failure(
                context,
                "sealed source file is unavailable");
        }
        std::string bytes =
            bioseq::codegen::read_artifact_bytes(location.source_file);
        if (bioseq::common::sha256_hex(bytes)
            != record.sha256) {
            inventory_failure(
                context,
                "source bytes differ from the compile-graph "
                "authority");
        }
        record.verified_bytes = std::move(bytes);
    }
    const source_range& span = location.span;
    if (span.begin.line == 0
        || span.begin.column == 0
        || span.end.line == 0
        || span.end.column == 0
        || span.begin.byte_offset >= span.end.byte_offset
        || span.end.byte_offset
            > record.verified_bytes->size()) {
        inventory_failure(
            context,
            "expanded source span is not a nonempty half-open "
            "range inside the sealed source");
    }
    return bioseq::common::sha256_hex(
        std::string_view{*record.verified_bytes}.substr(
            span.begin.byte_offset,
            span.end.byte_offset
                - span.begin.byte_offset));
}

struct owned_call {
    const function_flow* owner{};
    const flow_call_site* call{};
};

using owned_call_index =
    std::map<std::string, owned_call, std::less<>>;

[[nodiscard]] inline bool exact_direct_target(
    const flow_call_site& call,
    callee_domain domain,
    std::string_view target_name) noexcept {
    return call.resolution == resolution_status::direct
        && call.target_set_complete
        && call.targets.size() == 1
        && call.targets.front().domain == domain
        && call.targets.front().target_qualified_name
            == target_name;
}

[[nodiscard]] inline runtime_source_anchor make_anchor(
    runtime_anchor_kind kind,
    const owned_call& value,
    source_authority& authority) {
    if (value.owner == nullptr || value.call == nullptr
        || value.call->caller_usr != value.owner->stable_usr
        || value.call->stable_site_id.empty()
        || value.call->expansion.source_file.empty()) {
        inventory_failure(
            "anchor",
            "compiler call lacks an exact owner or expanded "
            "source identity");
    }
    return {
        .kind = kind,
        .compiler_stable_id =
            value.call->stable_site_id,
        .enclosing_function_usr =
            value.owner->stable_usr,
        .source_file =
            value.call->expansion.source_file,
        .expanded_source_span =
            value.call->expansion.span,
        .source_fragment_sha256 =
            source_fragment_sha256(
                value.call->expansion,
                authority,
                value.call->stable_site_id),
    };
}

[[nodiscard]] inline std::string expected_site_callee(
    const flow_call_site& call) {
    if (!call.macro_chain.empty()) {
        return call.macro_chain.front().name;
    }
    if (call.targets.size() != 1) {
        inventory_failure(
            call.stable_site_id,
            "site call has no exact callee");
    }
    return call.targets.front().target_qualified_name;
}

[[nodiscard]] inline bool ledger_site_matches(
    const error_site& site,
    const owned_call& site_call) {
    return site_call.owner != nullptr
        && site_call.call != nullptr
        && site.source_file
            == site_call.call->expansion.source_file
        && same_range(
            site.expanded_source_span,
            site_call.call->expansion.span)
        && site.callee_or_macro
            == expected_site_callee(*site_call.call)
        && site.resolution == site_call.call->resolution
        && site.possible_severities.size() == 1
        && site.possible_severities.front()
            == postgres_severity::recoverable_error
        && !site.transitive_call_path.empty()
        && site.transitive_call_path.back().usr
            == site_call.owner->stable_usr;
}

}  // namespace runtime_inventory_detail

/// Returns the deterministic ABI-v2 identity for a SQL/fault probe pair.
[[nodiscard]] inline std::string runtime_probe_site_id(
    std::string_view sql_identity,
    std::string_view fault_key) {
    if (sql_identity.empty() || fault_key.empty()) {
        runtime_inventory_detail::inventory_failure(
            "site id",
            "SQL identity and fault key must be nonempty");
    }
    return "probe:v2:"
        + bioseq::common::sha256_hex(
            "bioseq.safety_runtime_probe.site.v2\n"
            + std::string{sql_identity} + "\n"
            + std::string{fault_key} + "\n");
}

/// Validates canonical runtime-probe identities and inventory shape.
inline void validate_runtime_probe_inventory(
    const runtime_probe_inventory& inventory) {
    serialization_detail::
        validate_runtime_probe_inventory_shape(inventory);
    for (const runtime_probe_inventory_entry& entry :
         inventory.entries) {
        if (entry.site_id
            != runtime_probe_site_id(
                entry.sql_identity,
                entry.fault_key)) {
            runtime_inventory_detail::inventory_failure(
                entry.sql_identity,
                "site id differs from the canonical v2 digest");
        }
    }
}

/// Joins compiler probe facts to exact recoverable ERROR ledger sites.
[[nodiscard]] inline runtime_probe_inventory
build_runtime_probe_inventory(
    const safety_ledger& ledger,
    const compiler_analysis_facts& facts,
    std::string_view ledger_json,
    std::string_view compiler_facts_json,
    std::string_view compile_graph_json,
    std::string_view sql_manifest_json,
    std::string_view inventory_producer_executable_bytes) {
    using namespace runtime_inventory_detail;
    if (ledger.schema_version != 2
        || facts.schema_version != 2
        || ledger.profile != facts.profile
        || facts.compiler_profile != profile_name(facts.profile)
        || ledger.parents.empty()
        || facts.runtime_probes.empty()
        || ledger_json.empty()
        || compiler_facts_json.empty()
        || compile_graph_json.empty()
        || sql_manifest_json.empty()
        || inventory_producer_executable_bytes.empty()) {
        inventory_failure(
            "inputs",
            "schema/profile authority or required nonempty "
            "artifacts are missing");
    }
    if (render_safety_ledger_json(ledger) != ledger_json
        || render_compiler_analysis_facts_json(facts)
            != compiler_facts_json) {
        inventory_failure(
            "inputs",
            "ledger or compiler-fact bytes are not canonical");
    }
    const std::string ledger_sha256 =
        bioseq::common::sha256_hex(ledger_json);
    const std::string compiler_facts_sha256 =
        bioseq::common::sha256_hex(compiler_facts_json);
    const std::string compile_graph_sha256 =
        bioseq::common::sha256_hex(compile_graph_json);
    const std::string sql_manifest_sha256 =
        bioseq::common::sha256_hex(sql_manifest_json);
    const std::string inventory_producer_sha256 =
        bioseq::common::sha256_hex(
            inventory_producer_executable_bytes);
    if (ledger.compiler_facts_sha256
            != compiler_facts_sha256
        || ledger.compile_graph_sha256
            != compile_graph_sha256
        || facts.compile_graph_sha256
            != compile_graph_sha256
        || ledger.sql_manifest_sha256
            != sql_manifest_sha256
        || facts.sql_manifest_sha256
            != sql_manifest_sha256
        || ledger.source_list_sha256
            != facts.source_list_sha256
        || ledger.compile_database_sha256
            != facts.compile_database_sha256
        || ledger.pcm_manifest_sha256
            != facts.pcm_manifest_sha256
        || ledger.producer_executable_sha256
            != facts.producer_executable_sha256
        || ledger.producer_executable_sha256
            != inventory_producer_sha256
        || ledger.clang_version != facts.clang_version) {
        inventory_failure(
            "inputs",
            "ledger, compiler facts, compile graph, or SQL "
            "manifest/producer do not form one exact authority "
            "chain");
    }
    source_authority authority =
        parse_source_authority(compile_graph_json, facts);

    owned_call_index calls;
    for (const function_flow& function : facts.functions) {
        for (const flow_call_site& call : function.calls) {
            if (!calls.emplace(
                    call.stable_site_id,
                    owned_call{
                        .owner = std::addressof(function),
                        .call = std::addressof(call)})
                    .second) {
                inventory_failure(
                    call.stable_site_id,
                    "compiler stable call identity is duplicated");
            }
        }
    }

    runtime_probe_inventory result{
        .schema_version = 1,
        .profile = facts.profile,
        .probe_abi_version = 2,
        .ledger_sha256 = ledger_sha256,
        .compiler_facts_sha256 =
            compiler_facts_sha256,
        .sql_manifest_sha256 =
            sql_manifest_sha256,
        .source_list_sha256 =
            facts.source_list_sha256,
        .compile_database_sha256 =
            facts.compile_database_sha256,
        .compile_graph_sha256 =
            compile_graph_sha256,
        .pcm_manifest_sha256 =
            facts.pcm_manifest_sha256,
        .inventory_producer_executable_sha256 =
            inventory_producer_sha256,
    };
    static constexpr std::string_view visit_name =
        "bioseq::postgres::support::safety_runtime_probe::"
        "record_site_visit";
    static constexpr std::string_view trigger_name =
        "bioseq::postgres::support::safety_runtime_probe::"
        "consume_fault";
    for (const runtime_probe_fact& probe :
         facts.runtime_probes) {
        const owned_call_index::const_iterator site_found =
            calls.find(probe.site_call_stable_id);
        const owned_call_index::const_iterator visit_found =
            calls.find(probe.visit_call_stable_id);
        if (site_found == calls.end()
            || visit_found == calls.end()) {
            inventory_failure(
                probe.fault_key,
                "runtime fact references an unknown site or "
                "visit call");
        }
        const owned_call& site_call = site_found->second;
        const owned_call& visit_call = visit_found->second;
        std::optional<owned_call> trigger_call;
        if (probe.trigger_call_stable_id) {
            const owned_call_index::const_iterator trigger_found =
                calls.find(*probe.trigger_call_stable_id);
            if (trigger_found == calls.end()) {
                inventory_failure(
                    probe.fault_key,
                    "runtime fact references an unknown trigger "
                    "call");
            }
            trigger_call = trigger_found->second;
        }
        if (site_call.owner->stable_usr
                != visit_call.owner->stable_usr
            || (trigger_call
                && trigger_call->owner->stable_usr
                    != site_call.owner->stable_usr)
            || probe.site_call_stable_id
                == probe.visit_call_stable_id
            || (probe.trigger_call_stable_id
                && (*probe.trigger_call_stable_id
                        == probe.site_call_stable_id
                    || *probe.trigger_call_stable_id
                        == probe.visit_call_stable_id))
            || site_call.call->resolution
                != resolution_status::direct
            || !site_call.call->target_set_complete
            || site_call.call->targets.size() != 1
            || site_call.call->targets.front().domain
                != callee_domain::postgresql_server
            || !exact_direct_target(
                *visit_call.call,
                callee_domain::project,
                visit_name)
            || (probe.relation
                    == runtime_relation::natural_at_site)
                != !trigger_call.has_value()
            || (trigger_call
                && !exact_direct_target(
                    *trigger_call->call,
                    callee_domain::project,
                    trigger_name))) {
            inventory_failure(
                probe.fault_key,
                "runtime fact calls do not have exact distinct "
                "site/visit/trigger roles in one function");
        }
        const runtime_source_anchor site_anchor =
            make_anchor(
                runtime_anchor_kind::postgres_error_site,
                site_call,
                authority);
        const runtime_source_anchor visit_anchor =
            make_anchor(
                runtime_anchor_kind::record_site_visit,
                visit_call,
                authority);
        std::optional<runtime_source_anchor> trigger_anchor;
        if (trigger_call) {
            trigger_anchor = make_anchor(
                runtime_anchor_kind::consume_fault,
                *trigger_call,
                authority);
        }

        std::size_t match_count{};
        for (const safety_parent_row& parent :
             ledger.parents) {
            if (parent.profile != ledger.profile
                || parent.sql_identity.empty()) {
                inventory_failure(
                    "ledger",
                    "parent has an empty identity or wrong profile");
            }
            const error_site* match = nullptr;
            for (const error_site& site : parent.sites) {
                if (!ledger_site_matches(site, site_call)) {
                    continue;
                }
                if (match != nullptr) {
                    inventory_failure(
                        parent.sql_identity,
                        "one runtime fact matches multiple ledger "
                        "sites in one SQL parent");
                }
                match = std::addressof(site);
            }
            if (match == nullptr) continue;
            ++match_count;
            const std::string canonical_site = render_json(
                json_value{
                    serialization_detail::site_json(*match)});
            result.entries.push_back({
                .sql_identity = parent.sql_identity,
                .fault_key = probe.fault_key,
                .site_id = runtime_probe_site_id(
                    parent.sql_identity,
                    probe.fault_key),
                .relation = probe.relation,
                .ledger_site_sha256 =
                    bioseq::common::sha256_hex(
                        canonical_site),
                .site_anchor = site_anchor,
                .visit_anchor = visit_anchor,
                .trigger_anchor = trigger_anchor,
                .transitive_call_path =
                    match->transitive_call_path,
                .probe_contract =
                    runtime_probe_contract_for(probe.relation),
            });
        }
        if (match_count == 0) {
            inventory_failure(
                probe.fault_key,
                "runtime fact does not join to any exact "
                "recoverable ledger site");
        }
    }
    std::ranges::sort(
        result.entries,
        [](const runtime_probe_inventory_entry& left,
           const runtime_probe_inventory_entry& right) {
            return std::tie(
                       left.sql_identity,
                       left.fault_key,
                       left.site_id)
                < std::tie(
                       right.sql_identity,
                       right.fault_key,
                       right.site_id);
        });
    validate_runtime_probe_inventory(result);
    return result;
}

}  // namespace bioseq::codegen::safety
