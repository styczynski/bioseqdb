module;

#include <cstdio>
#include <exception>
#include <print>
#include <span>
#include <string_view>

export module bioseq.codegen.website;

import :build_pipeline;
import :tests.contract;

export namespace bioseq::codegen::website {

/// @brief Runs the website contract checks.
/// @return Zero on success, or one after reporting a failure.
inline int self_test() {
    try {
        run_website_contract_self_tests();
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_website_codegen: {}", error.what());
        return 1;
    }
}

/// @brief Dispatches a `website` subcommand to its stage handler.
/// @param args Arguments after the leading `website` token.
/// @return The stage exit code, or one on an unexpected exception.
inline int run(std::span<std::string_view const> args) {
    try {
        return dispatch_website_command(args);
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_website_codegen: {}", error.what());
        return 1;
    }
}

}  // namespace bioseq::codegen::website
