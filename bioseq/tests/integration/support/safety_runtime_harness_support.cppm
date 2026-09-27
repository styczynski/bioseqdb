/**
 * @file safety_runtime_harness_support.cppm
 * @brief Public commands for the PostgreSQL runtime-safety test harness.
 */

export module bioseq.tests.integration.safety_runtime_harness_support;

export import :core_support;
export import :case_support;
export import :manifest_support;
import :scenario_support;

export namespace bioseq::tests::integration::safety_runtime_harness {

/// Install the checked build files in the private cluster, and record the active build.
[[nodiscard]] int activate(const command_line& command);
/// Emit the compiled catalog of runtime-safety cases.
[[nodiscard]] int describe(const command_line& command);
/// Run  one checked runtime-safety case and write its results.
[[nodiscard]] int run_case(const command_line& command);
/// Test the harness's parsers and validation rules locally.
[[nodiscard]] int self_test(const command_line& command);

}  // namespace bioseq::tests::integration::safety_runtime_harness
