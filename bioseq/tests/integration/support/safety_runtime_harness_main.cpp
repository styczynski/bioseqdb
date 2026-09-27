/**
 * @file safety_runtime_harness_main.cpp
 * @brief Dispatch the standalone SQL-boundary safety evidence protocol.
 */

#include <cstdio>
#include <exception>

import bioseq.tests.integration.safety_runtime_harness_support;

using namespace bioseq::tests::integration::safety_runtime_harness;

/// Dispatch one standalone runtime-evidence protocol command.
int main(int argc, char** argv) {
    try {
        const command_line command =
            parse_command_line(argc, argv);
        if (command.command
            == "--bioseq-safety-runtime-activate") {
            return activate(command);
        }
        if (command.command
            == "--bioseq-safety-runtime-describe") {
            return describe(command);
        }
        if (command.command
            == "--bioseq-safety-runtime-run") {
            return run_case(command);
        }
        if (command.command
            == "--bioseq-safety-runtime-self-test") {
            return self_test(command);
        }
        fail("command-line", "unsupported protocol command");
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "bioseq safety runtime harness: %s\n",
            error.what());
        return 1;
    } catch (...) {
        std::fprintf(
            stderr,
            "bioseq safety runtime harness: unknown failure\n");
        return 1;
    }
}
