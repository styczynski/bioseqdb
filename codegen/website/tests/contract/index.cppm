module;

#include <string>

module bioseq.codegen.website:tests.contract;

import :build_pipeline;
import :reference_index;
import :search_index;
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
    static_assert(sql_example_executor<postgres_tutorial_executor>,
                  "postgres executor must model the tutorial executor concept");
    // Name the canonical-ID helper to prove reference_index is importable.
    const std::string probe = make_reference_entity_id(reference_entity{
        .domain = entity_domain::cpp, .qualified_name = "bioseq::probe"});
    (void)probe;
    verify_website_pipeline();
}

}  // namespace bioseq::codegen::website
