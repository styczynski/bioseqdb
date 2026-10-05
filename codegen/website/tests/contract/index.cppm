module;

#include <string>
#include <stdexcept>

module bioseq.codegen.website:tests.contract;

import :build_pipeline;
import :reference_index;
import :search_index;
import bioseq.codegen.website.postgres;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Import the website modules and call  their APIs so compilation checks
// the declared dependencies. The test entrypoint also runs the website
// pipeline checks.
namespace bioseq::codegen::website {

/// @brief Checks the imported website APIs and runs the pipeline tests.
void run_website_contract_self_tests() {
    // Check that the PostgreSQL executor provides the required operations.
    static_assert(sql_example_executor<postgres_tutorial_executor>,
                  "postgres executor must model the tutorial executor concept");
    // Exercise the reference-ID API through its module import.
    const std::string probe = make_reference_entity_id(reference_entity{
        .domain = entity_domain::cpp, .qualified_name = "bioseq::probe"});
    (void)probe;
    // A GUC's leaf is ambiguous: preserve its full setting path in search.
    reference_entity setting;
    setting.domain = entity_domain::sql;
    setting.kind = entity_kind::sql_object;
    setting.canonical_id = "sql:guc:bioseq.index.insert_maintenance.enabled";
    setting.qualified_name = "bioseq.index.insert_maintenance.enabled";
    setting.display_name = "enabled";
    setting.attributes["sql_kind"] = "guc";
    const auto search_setting = build_entity_search_entry(setting);
    if (search_setting.kind != "setting"
        || search_setting.group != "SQL"
        || search_setting.title != "index.insert_maintenance.enabled"
        || search_setting.qualified_name != setting.qualified_name
        || search_setting.name_source.find("index.insert_maintenance.enabled")
            == std::string::npos) {
        throw std::runtime_error("GUC search must retain the full setting label and SQL identity");
    }
    setting.attributes.clear();
    if (build_entity_search_entry(setting).kind != "setting") {
        throw std::runtime_error("GUC canonical identities must retain the Setting search kind");
    }
    setting.canonical_id = "sql:guc:bioseq_cache.enabled";
    setting.qualified_name = "bioseq_cache.enabled";
    if (build_entity_search_entry(setting).title != "bioseq_cache.enabled") {
        throw std::runtime_error("Setting search may strip only the leading bioseq. prefix");
    }
    setting.canonical_id = "sql:operator:bioseq.=(bioseq.seq,bioseq.seq)";
    setting.qualified_name = "bioseq.=";
    setting.display_name = "=";
    if (build_entity_search_entry(setting).kind != "sql_object"
        || build_entity_search_entry(setting).title != "=") {
        throw std::runtime_error("Non-GUC SQL objects must not be relabeled as settings");
    }
    verify_website_pipeline();
}

}  // namespace bioseq::codegen::website
