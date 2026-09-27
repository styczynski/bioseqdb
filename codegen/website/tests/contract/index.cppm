module;

#include <string>

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
    verify_website_pipeline();
}

}  // namespace bioseq::codegen::website
