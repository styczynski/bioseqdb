/**
 * @file index.cppm
 * @brief Public command boundary for external-client generation.
 */
module;

#include <cstddef>
#include <cstdio>
#include <exception>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.external;

import :builder;
import :tests.contract;

export namespace bioseq::codegen::external {

/// @brief Runs the pure parser and renderer contract tests.
/// @return Zero on success, or one after emitting the failure diagnostic.
inline int run_self_tests() {
    try {
        run_generator_contract_self_tests();
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_external_codegen: {}", error.what());
        return 1;
    }
}

/// @brief Runs external-client generation behind the process diagnostic boundary.
/// @param argument_count Argument count including the executable name.
/// @param argument_values Mutable C argument vector supplied by the launcher.
/// @return Zero on success, or one after emitting the failure diagnostic.
inline int generate_clients(int argument_count, char** argument_values) {
    try {
        return generate_external_clients(
            parse_generation_options(argument_count, argument_values));
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_external_codegen: {}", error.what());
        return 1;
    }
}

/// @brief Runs an `external` subcommand from the shared component registry.
/// @param arguments Arguments after the `external` token, with the subcommand first.
/// @return Exit code two for a usage error, otherwise the generator status.
inline int run_command(std::span<const std::string_view> arguments) {
    if (arguments.empty() || arguments.front() != "generate") {
        std::println(stderr, "usage: bioseq_codegen external generate [options]");
        return 2;
    }

    std::vector<std::string> argument_storage;
    argument_storage.reserve(arguments.size());
    argument_storage.emplace_back("bioseq_codegen");
    for (std::size_t argument_index = 1;
         argument_index < arguments.size();
         ++argument_index) {
        argument_storage.emplace_back(arguments[argument_index]);
    }

    std::vector<char*> argument_pointers;
    argument_pointers.reserve(argument_storage.size());
    for (std::string& argument : argument_storage) {
        argument_pointers.push_back(argument.data());
    }
    return generate_clients(
        static_cast<int>(argument_pointers.size()),
        argument_pointers.data());
}

} // namespace bioseq::codegen::external
