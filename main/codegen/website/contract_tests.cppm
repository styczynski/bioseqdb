module;

#include <string>

module bioseq.codegen.website:contract_tests;

import :site_builder;
import :entity_index;
import :search;
import bioseq.codegen.website.postgres;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Compiler-checked module contract (§7). This consumer TU imports every website
// primary/partition it depends on and uses its promised API, so the module DAG
// is verified by the compiler rather than only by a textual test. The runtime
// half drives the integration smoke.
namespace bioseq::codegen::website {

/// @brief Runs the website contract self-tests.
///
/// Compile-time: this TU importing and naming the partition APIs proves the DAG
/// links. Run-time: the integration smoke exercises each dependency.
void run_website_contract_self_tests() {
    // Name the postgres facade type to prove the executor contract compiles.
    static_assert(tutorial_sql_executor<private_cluster_executor>,
                  "postgres executor must model the tutorial executor concept");
    // Name the canonical-id helper to prove entity_index is importable.
    const std::string probe = canonical_entity_id(documented_entity{
        .domain = entity_domain::cpp, .qualified_name = "bioseq::probe"});
    (void)probe;
    run_website_smoke();
}

}  // namespace bioseq::codegen::website
