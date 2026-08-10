module;

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

export module bioseq.codegen;

export import :cli;

export namespace bioseq::codegen {

/// @brief Public codegen entry point dispatching to the CLI layer.
/// @param argc Number of process arguments, including the program name.
/// @param argv Mutable null-terminated process argument array.
/// @return Process-style status code.
int run(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int argument_index = 0; argument_index < argc; ++argument_index) {
        arguments.emplace_back(argv[argument_index]);
    }
    return run_cli(arguments);
}

}
