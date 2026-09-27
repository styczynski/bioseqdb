module;

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.safety:fact_producer;

import :analyzer_model;
import :model;

export namespace bioseq::codegen::safety {

/// Inputs for one Clang safety-analysis pass.
///
/// Prepare the compilation database and precompiled-module manifest with
/// the `safety` authority for the same source list and build configuration.
struct compiler_fact_request {
    std::uint32_t schema_version{2};
    compilation_profile profile{compilation_profile::production};
    std::filesystem::path source_list;
    std::filesystem::path compile_database;
    std::filesystem::path pcm_root;
    std::filesystem::path pcm_manifest;
    std::filesystem::path sql_manifest;
    std::filesystem::path provenance;
    std::filesystem::path provenance_candidates_output;
    std::filesystem::path producer_executable;
};

/// Compiler analysis results and the JSON compile graph saved beside them.
/// `facts.compile_graph_sha256` identifies the exact graph bytes.
struct compiler_fact_product {
    compiler_analysis_facts facts;
    std::string compile_graph_json;
};

/// Clang results for a test source, before filtering by SQL wrapper reachability.
///
/// Tests use these function and probe records to check the same analysis as
/// production without preparing a SQL manifest.
struct isolated_compiler_fact_scan {
    std::vector<function_flow> functions;
    std::vector<runtime_probe_fact> runtime_probes;
    std::vector<postgres_callback_root> callback_roots;
};

/// Uses Clang to follow SQL-wrapper calls and check C++ object lifetimes.
///
/// Fails if the reachable calls have missing bodies, unresolved targets, or
/// incomplete control-flow or lifetime information. The prepared build inputs
/// and reviewed source records must also pass validation.
[[nodiscard]] compiler_fact_product produce_compiler_analysis_facts(
    const compiler_fact_request& request);

/// Analyzes an in-memory test source with the production Clang pass.
///
/// Accepts compiler arguments directly, so tests need no SQL manifest,
/// prepared compilation database, or precompiled-module manifest.
[[nodiscard]] std::vector<function_flow>
scan_safety_flow_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile = "contract",
    const provenance_bundle* provenance = nullptr);

/// Analyzes a test source and also returns runtime probes and PostgreSQL callbacks.
[[nodiscard]] isolated_compiler_fact_scan
scan_safety_facts_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile = "contract",
    const provenance_bundle* provenance = nullptr);

}  // namespace bioseq::codegen::safety
