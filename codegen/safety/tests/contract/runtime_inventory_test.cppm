module;

#include <cstddef>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

export module bioseq.codegen.safety:tests.contract.runtime_inventory;

import :analyzer_model;
import :json;
import :model;
import :runtime_inventory;
import :serialization;
import bioseq.codegen.support;

export namespace bioseq::codegen::safety {

namespace runtime_inventory_contract_detail {

[[nodiscard]] inline source_range span(
    std::size_t begin,
    std::size_t end,
    std::size_t line,
    std::size_t end_column) {
    return {
        .begin = {
            .byte_offset = begin,
            .line = line,
            .column = 1,
        },
        .end = {
            .byte_offset = end,
            .line = line,
            .column = end_column,
        },
    };
}

template <class p_operation>
inline void require_failure(
    p_operation&& operation,
    std::string_view expected) {
    try {
        std::invoke(std::forward<p_operation>(operation));
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()}.contains(expected)) {
            return;
        }
        throw std::runtime_error(
            "runtime-inventory contract received unexpected "
            "diagnostic: " + std::string{error.what()});
    }
    throw std::runtime_error(
        "runtime-inventory contract expected failure: "
        + std::string{expected});
}

[[nodiscard]] inline flow_call_site call(
    std::string id,
    std::string caller,
    std::string source,
    source_range location,
    std::string target,
    callee_domain domain) {
    return {
        .stable_site_id = std::move(id),
        .caller_usr = std::move(caller),
        .spelling = {
            .source_file = source,
            .span = location,
        },
        .expansion = {
            .source_file = std::move(source),
            .span = location,
        },
        .resolution = resolution_status::direct,
        .target_set_complete = true,
        .targets = {{
            .target_usr = "c:@F@" + target,
            .target_qualified_name = std::move(target),
            .domain = domain,
        }},
    };
}

[[nodiscard]] inline error_site site(
    std::string ledger_site_id,
    std::string source,
    source_range location) {
    return {
        .site_id = std::move(ledger_site_id),
        .source_file = source,
        .expanded_source_span = location,
        .callee_or_macro = "errfinish",
        .transitive_call_path = {{
            .usr = "c:@F@owner#",
            .qualified_declaration = "owner",
            .source_file = std::move(source),
            .source_span = span(0, 27, 1, 28),
        }},
        .possible_severities = {
            postgres_severity::recoverable_error,
        },
        .severity_constant_propagated = true,
        .resolution = resolution_status::direct,
        .classification =
            site_classification::lifetime_free_direct,
    };
}

[[nodiscard]] inline safety_parent_row parent(
    std::string identity,
    error_site value) {
    return {
        .sql_identity = std::move(identity),
        .owner_usr = "c:@F@owner#",
        .qualified_declaration = "owner",
        .owner_source_file = value.source_file,
        .owner_source_span = span(0, 27, 1, 28),
        .generated_wrapper = "generated.owner",
        .public_symbol = "owner",
        .profile = compilation_profile::production,
        .transitive_closure_sha256 = std::string(64, '7'),
        .recoverable_site_count = 1,
        .sites = {std::move(value)},
    };
}

}  // namespace runtime_inventory_contract_detail

inline void run_runtime_inventory_contract_self_tests() {
    using namespace runtime_inventory_contract_detail;
    const std::filesystem::path source_path =
        std::filesystem::temp_directory_path()
        / "bioseq_runtime_inventory_contract.cppm";
    const std::string source{
        "site();\nvisit();\ntrigger();\n"};
    bioseq::codegen::publish_artifact_if_changed(
        source_path, source);
    const std::string source_file =
        std::filesystem::weakly_canonical(source_path)
            .generic_string();
    const std::string source_sha256 =
        bioseq::common::sha256_hex(source);
    const std::string source_list_sha256(64, '1');
    const std::string compile_database_sha256(64, '2');
    const std::string pcm_manifest_sha256(64, '3');
    const std::string sql_manifest{"{}\n"};
    const std::string compile_graph = render_json(
        json_value::object{
            {"clang_version", "22.0.0-contract"},
            {"compilation_signature", std::string(64, '5')},
            {"compile_database", "/contract/compile_commands.json"},
            {"compile_database_sha256",
             compile_database_sha256},
            {"modules", json_value::array{}},
            {"pcm_manifest", "/contract/pcm-manifest.json"},
            {"pcm_manifest_sha256", pcm_manifest_sha256},
            {"pcm_root", "/contract/pcm"},
            {"postgresql_authenticated_headers",
             json_value::array{}},
            {"postgresql_header_roots",
             json_value::array{}},
            {"postgresql_rejected_header_roots",
             json_value::array{}},
            {"profile", "production"},
            {"schema", "bioseq_safety_compile_graph"},
            {"schema_revision", json_value{std::size_t{1}}},
            {"source_list", "/contract/sources.list"},
            {"source_list_sha256", source_list_sha256},
            {"translation_units",
             json_value::array{json_value::object{
                 {"arguments",
                  json_value::array{"clang++"}},
                 {"arguments_sha256", std::string(64, '4')},
                 {"directory",
                  source_path.parent_path().generic_string()},
                 {"source", source_file},
                 {"source_sha256", source_sha256},
             }}},
        });
    const source_range site_span = span(0, 7, 1, 8);
    const source_range visit_span = span(8, 16, 2, 9);
    const source_range trigger_span = span(17, 27, 3, 11);
    function_flow owner{
        .stable_usr = "c:@F@owner#",
        .qualified_name = "owner",
        .source_file = source_file,
        .module_name = "contract.owner",
        .declaration_span = span(0, 27, 1, 28),
        .body_span = span(0, 27, 1, 28),
        .body_available = true,
    };
    owner.calls = {
        call(
            "site-call",
            owner.stable_usr,
            source_file,
            site_span,
            "errfinish",
            callee_domain::postgresql_server),
        call(
            "visit-call",
            owner.stable_usr,
            source_file,
            visit_span,
            "bioseq::postgres::support::safety_runtime_probe::"
            "record_site_visit",
            callee_domain::project),
        call(
            "trigger-call",
            owner.stable_usr,
            source_file,
            trigger_span,
            "bioseq::postgres::support::safety_runtime_probe::"
            "consume_fault",
            callee_domain::project),
    };
    compiler_analysis_facts facts{
        .profile = compilation_profile::production,
        .compiler_profile = "production",
        .clang_version = "22.0.0-contract",
        .source_list_sha256 = source_list_sha256,
        .compile_database_sha256 =
            compile_database_sha256,
        .producer_executable_sha256 =
            bioseq::common::sha256_hex(
                "contract-producer"),
        .compile_graph_sha256 =
            bioseq::common::sha256_hex(compile_graph),
        .pcm_manifest_sha256 = pcm_manifest_sha256,
        .sql_manifest_sha256 =
            bioseq::common::sha256_hex(sql_manifest),
        .root_usrs = {owner.stable_usr},
        .functions = {std::move(owner)},
        .runtime_probes = {{
            .fault_key = "contract.failure",
            .relation =
                runtime_relation::injected_routes_to_site,
            .site_call_stable_id = "site-call",
            .visit_call_stable_id = "visit-call",
            .trigger_call_stable_id = "trigger-call",
        }},
    };
    const std::string facts_json =
        render_compiler_analysis_facts_json(facts);
    safety_ledger ledger{
        .profile = compilation_profile::production,
        .postgresql_revision =
            "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
        .provenance_bundle_sha256 = std::string(64, '8'),
        .source_list_sha256 = source_list_sha256,
        .compile_database_sha256 =
            compile_database_sha256,
        .producer_executable_sha256 =
            facts.producer_executable_sha256,
        .compile_graph_sha256 =
            facts.compile_graph_sha256,
        .pcm_manifest_sha256 = pcm_manifest_sha256,
        .sql_manifest_sha256 =
            facts.sql_manifest_sha256,
        .compiler_facts_sha256 =
            bioseq::common::sha256_hex(facts_json),
        .clang_version = facts.clang_version,
        .analyzer_version = "contract",
        .generator_revision = "contract",
        .parents = {
            parent(
                "public.z()",
                site("ledger-z", source_file, site_span)),
            parent(
                "public.a()",
                site("ledger-a", source_file, site_span)),
        },
    };
    const std::string ledger_json =
        render_safety_ledger_json(ledger);
    const std::string producer_bytes{"contract-producer"};
    const runtime_probe_inventory inventory =
        build_runtime_probe_inventory(
            ledger,
            facts,
            ledger_json,
            facts_json,
            compile_graph,
            sql_manifest,
            producer_bytes);
    if (inventory.schema_revision != 1
        || inventory.probe_abi_revision != 2
        || inventory.entries.size() != 2
        || inventory.inventory_producer_executable_sha256
            != facts.producer_executable_sha256
        || inventory.ledger_sha256
            != bioseq::common::sha256_hex(ledger_json)
        || inventory.compiler_facts_sha256
            != bioseq::common::sha256_hex(facts_json)
        || inventory.sql_manifest_sha256
            != bioseq::common::sha256_hex(sql_manifest)
        || inventory.entries.front().sql_identity
            != "public.a()"
        || inventory.entries.front().site_id
            != runtime_probe_site_id(
                "public.a()", "contract.failure")
        || inventory.entries.front().site_revision != 2
        || inventory.entries.front().site_anchor.kind
            != runtime_anchor_kind::postgres_error_site
        || inventory.entries.front().visit_anchor.kind
            != runtime_anchor_kind::record_site_visit
        || !inventory.entries.front().trigger_anchor
        || inventory.entries.front().trigger_anchor->kind
            != runtime_anchor_kind::consume_fault
        || inventory.entries.front().probe_contract.ordering
            != runtime_probe_ordering::trigger_before_visit
        || inventory.entries.front().probe_contract.abi_revision
            != 2
        || inventory.entries.front()
                .site_anchor.source_fragment_sha256
            != bioseq::common::sha256_hex("site();")
        || inventory.entries.front().ledger_site_sha256
            != bioseq::common::sha256_hex(
                render_json(json_value{
                    serialization_detail::site_json(
                        ledger.parents.back().sites.front())}))) {
        throw std::runtime_error(
            "runtime-inventory compiler/ledger join changed");
    }
    const std::string rendered =
        render_runtime_probe_inventory_json(inventory);
    if (!rendered.contains("\"schema_revision\"")
        || !rendered.contains("\"probe_abi_revision\"")
        || !rendered.contains("\"site_revision\"")
        || !rendered.contains("\"abi_revision\"")) {
        throw std::runtime_error(
            "runtime-inventory revision framing changed");
    }
    const runtime_probe_inventory parsed =
        parse_runtime_probe_inventory_json(rendered);
    validate_runtime_probe_inventory(parsed);
    if (render_runtime_probe_inventory_json(parsed)
        != rendered) {
        throw std::runtime_error(
            "runtime-inventory canonical round trip changed");
    }
    {
        runtime_probe_inventory bad = inventory;
        bad.entries.front().site_id =
            "probe:" + std::string(64, '0');
        require_failure(
            [&] {
                validate_runtime_probe_inventory(bad);
            },
            "site id differs");
    }
    {
        runtime_probe_inventory bad = inventory;
        runtime_probe_inventory_entry duplicate =
            bad.entries.front();
        duplicate.relation =
            runtime_relation::natural_at_site;
        duplicate.trigger_anchor.reset();
        duplicate.probe_contract = {
            .abi_revision = 2,
            .expected_visit_count = 1,
            .expected_trigger_count = 0,
            .ordering =
                runtime_probe_ordering::visit_only,
        };
        bad.entries.push_back(std::move(duplicate));
        require_failure(
            [&] {
                validate_runtime_probe_inventory(bad);
            },
            "(sql_identity, fault_key) case key");
    }
    {
        safety_ledger bad = ledger;
        bad.parents.front().sites.clear();
        bad.parents.back().sites.clear();
        const std::string bad_json =
            render_safety_ledger_json(bad);
        require_failure(
            [&] {
                static_cast<void>(
                    build_runtime_probe_inventory(
                        bad,
                        facts,
                        bad_json,
                        facts_json,
                        compile_graph,
                        sql_manifest,
                        producer_bytes));
            },
            "does not join");
    }
    require_failure(
        [&] {
            static_cast<void>(
                build_runtime_probe_inventory(
                    ledger,
                    facts,
                    ledger_json,
                    facts_json,
                    compile_graph,
                    sql_manifest,
                    "different-producer"));
        },
        "do not form one exact authority chain");
    {
        json_value bad = parse_json(rendered);
        std::get<json_value::object>(bad.value).emplace(
            "unexpected", json_value{true});
        require_failure(
            [&] {
                static_cast<void>(
                    parse_runtime_probe_inventory_json(
                        render_json(bad)));
            },
            "unknown JSON field 'unexpected'");
    }
}

}  // namespace bioseq::codegen::safety
