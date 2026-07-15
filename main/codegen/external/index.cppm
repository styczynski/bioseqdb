module;

#include <cstdio>
#include <exception>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// @brief Runs an `external` subcommand from the shared component registry.
/// @param args Arguments after the `external` token (subcommand first).
/// @return Exit code two for a usage error, otherwise the command's status.
inline int run(std::span<std::string_view const> args) {
    if (args.empty() || args.front() != "generate") {
        std::println(stderr, "usage: bioseq_codegen external generate [options]");
        return 2;
    }
    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size());
    argv_storage.emplace_back("bioseq_codegen");
    for (std::size_t index = 1; index < args.size(); ++index) {
        argv_storage.emplace_back(args[index]);
    }
    std::vector<char*> argv_pointers;
    argv_pointers.reserve(argv_storage.size());
    for (auto& argument : argv_storage) {
        argv_pointers.push_back(argument.data());
    }
    return generate(static_cast<int>(argv_pointers.size()), argv_pointers.data());
}

}
