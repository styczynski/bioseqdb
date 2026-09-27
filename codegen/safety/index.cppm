module;

#include <exception>
#include <print>
#include <span>
#include <string_view>

export module bioseq.codegen.safety;

export import :model;
export import :analyzer_model;
export import :json;
export import :manifest;
export import :validation;
export import :analyzer;
export import :serialization;
export import :fact_producer;
export import :tests.contract.fact_producer;
export import :runtime_matrix;
export import :dso;
export import :tests.contract.analyzer;
export import :tests.contract;
export import :runtime_inventory;
export import :tests.contract.runtime_inventory;
export import :cli;

export namespace bioseq::codegen::safety {

/// Runs every compiled safety contract module.
[[nodiscard]] inline int self_test() {
    try {
        run_safety_contract_self_tests();
        run_fact_producer_contract_self_tests();
        run_runtime_inventory_contract_self_tests();
        return 0;
    } catch (const std::exception& error) {
        std::println(
            stderr,
            "bioseq_codegen safety self-test: {}",
            error.what());
        return 1;
    }
}

/// Dispatches one safety command-line invocation.
[[nodiscard]] inline int run(
    std::span<std::string_view const> args) {
    return run_cli(args);
}

}  // namespace bioseq::codegen::safety
