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

/// Immutable inputs for one post-FINAL compiler-fact pass.
///
/// The owned compile database and PCM manifest must have been prepared with
/// the `safety` authority for the same exact source list and profile.
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

/// Canonical compiler facts and the independently persisted compile graph
/// whose exact bytes are named by `facts.compile_graph_sha256`.
struct compiler_fact_product {
    compiler_analysis_facts facts;
    std::string compile_graph_json;
};

/// Isolated compiler facts used by structural contract tests.
///
/// Production applies SQL-manifest reachability after the same per-function
/// pass.  Keeping the isolated probe facts beside their flow facts lets tests
/// exercise the exact AST/CFG extraction without fabricating a manifest.
struct isolated_compiler_fact_scan {
    std::vector<function_flow> functions;
    std::vector<runtime_probe_fact> runtime_probes;
    std::vector<postgres_callback_root> callback_roots;
};

/// Runs the authoritative Clang AST/CFG/lifetime pass over an owned prepared
/// compilation closure. Any missing command, body, target, source range,
/// provenance record, CFG edge, or lifetime fact in the reachable SQL-wrapper
/// closure is a hard failure.
[[nodiscard]] compiler_fact_product produce_compiler_analysis_facts(
    const compiler_fact_request& request);

/// Isolated Clang entry point used only by producer contract tests.
///
/// It uses the same AST/CFG implementation as production but has no SQL
/// manifest, compile database, PCM, or artifact-authority semantics.
[[nodiscard]] std::vector<function_flow>
scan_safety_flow_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile = "contract",
    const provenance_bundle* provenance = nullptr);

/// Isolated variant that also returns compiler-derived runtime-probe facts.
[[nodiscard]] isolated_compiler_fact_scan
scan_safety_facts_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile = "contract",
    const provenance_bundle* provenance = nullptr);

}  // namespace bioseq::codegen::safety
