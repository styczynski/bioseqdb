module;

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.codegen.sql:tests.contract;

import :policy;
import bioseq.codegen.sql.graph;
import :tests.contract.guc;
import bioseq.codegen.sql.renderer;
import bioseq.codegen.structural_analysis.tests.contract;

export namespace bioseq::codegen::sql {

namespace contract_test_detail {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{
            "SQL generator contract failed: " + std::string{message}};
    }
}

template <class p_operation>
inline void require_failure(
        p_operation&& operation,
        std::string_view expected,
        std::string_view message) {
    try {
        std::invoke(std::forward<p_operation>(operation));
    } catch (const std::exception& error) {
        if (std::string_view{error.what()}.contains(expected)) {
            return;
        }
        throw std::runtime_error{
            "SQL generator contract received an unexpected diagnostic: "
            + std::string{error.what()}};
    }
    throw std::runtime_error{
        "SQL generator contract expected failure: "
        + std::string{message}};
}

}  // namespace contract_test_detail

/**
 * Runs the current SQL generator contracts.
 */
inline void run_generator_contract_self_tests() {
    bioseq::codegen::structural_analysis::run_preparation_contract_self_tests();
    run_guc_generator_contract_self_tests();
    run_fmgr_renderer_contract_tests();
    run_compact_fmgr_contract_tests();

    sql_graph graph;
    contract_test_detail::require(
        graph.generated_module_prefix
                == "bioseq.postgres.sql.generated"
            && graph.fmgr_boundary_module
                == "bioseq.postgres.support.sql_boundary",
        "default generated-module identities are not canonical");
    validate_codegen_completion_policy(graph, true);

    graph.handwritten_fmgr_export_owners.push_back({
        .macro_name = "PG_FUNCTION_INFO_V1",
        .symbol = "handwritten_fixture_symbol",
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .source_line = 1,
    });
    contract_test_detail::require_failure(
        [&graph] {
            validate_codegen_completion_policy(graph, true);
        },
        "zero handwritten fmgr export owners",
        "complete code generation accepted handwritten FMgr ownership");
}

}  // namespace bioseq::codegen::sql
