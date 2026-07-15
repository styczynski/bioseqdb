module;

#include <cstdio>
#include <exception>
#include <print>

export module bioseq.codegen.external;

import :builder;
import :contract_tests;
import :model;

export namespace bioseq::codegen::external {

/// @brief Runs the pure parser/renderer contract self-tests.
/// @return Zero on success, one after a diagnostic failure.
inline int self_test() {
    try {
        run_generator_contract_self_tests();
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_external_codegen: {}", error.what());
        return 1;
    }
}

/// @brief Runs the generator and converts failures into a diagnostic exit status.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return Process-style success or failure code.
inline int generate(int argc, char** argv) {
    try {
        return execute(parse_command_line(argc, argv));
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_external_codegen: {}", error.what());
        return 1;
    }
}

}
