/**
 * @file safety_runtime_harness_support.cppm
 * @brief Export the standalone SQL-boundary safety evidence protocol.
 */

export module bioseq.tests.integration.safety_runtime_harness_support;

export import :core_support;
export import :case_support;
export import :manifest_support;
import :scenario_support;

export namespace bioseq::tests::integration::safety_runtime_harness {

[[nodiscard]] int activate(const command_line& command);
[[nodiscard]] int describe(const command_line& command);
[[nodiscard]] int run_case(const command_line& command);
[[nodiscard]] int self_test(const command_line& command);

}  // namespace bioseq::tests::integration::safety_runtime_harness
