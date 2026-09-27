module;

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

export module bioseq.codegen;

export import :cli;

export namespace bioseq::codegen {

/// @brief Passes process arguments to the generator command-line handler.
/// @param argc Number of arguments, including the program name.
/// @param argv Process argument array.
/// @return The command-line handler's exit status.
int run(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int argument_index = 0; argument_index < argc; ++argument_index) {
        arguments.emplace_back(argv[argument_index]);
    }
    return run_cli(arguments);
}

}
