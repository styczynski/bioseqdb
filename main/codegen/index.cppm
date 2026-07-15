module;

#include <span>
#include <string_view>

export module bioseq.codegen;

import :cli;

export namespace bioseq::codegen {

/// @brief Public codegen entry point dispatching to the CLI layer.
/// @param args Whole argument vector, where args[0] is the program name.
/// @return Process-style status code.
inline int run(std::span<std::string_view const> args) {
    return run_cli(args);
}

}
