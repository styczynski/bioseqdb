/**
 * @file
 * @brief Runs the build-configuration generator and writes its outputs.
 */
module;

#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <print>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.build_config;

export import :model;
export import :renderer;
import :tests.contract;
export import :validator;
import bioseq.codegen.support;

export namespace bioseq::codegen::build_config {

namespace detail {

[[nodiscard]] inline config_override parse_override(const std::string_view value) {
    const std::size_t separator = value.find('=');
    if (separator == std::string_view::npos || separator == 0U || separator + 1U == value.size() ||
        value.find('=', separator + 1U) != std::string_view::npos) {
        throw std::runtime_error("--override requires one MACRO=VALUE argument");
    }
    return config_override{
        .macro_name = std::string{value.substr(0U, separator)},
        .expression = std::string{value.substr(separator + 1U)},
    };
}

[[nodiscard]] inline command_line parse_command_line(const std::span<std::string_view const> args) {
    std::map<std::string, std::string, std::less<>> singleton;
    std::vector<config_override> overrides;
    const std::set<std::string_view, std::less<>> allowed{
        "--compile-contract", "--manifest", "--output", "--override", "--source", "--stamp"};
    for (std::size_t index = 0U; index < args.size(); ++index) {
        const std::string_view option = args[index];
        if (!allowed.contains(option)) {
            throw std::runtime_error("unknown build-config generator option '" +
                                     std::string{option} + "'");
        }
        if (index + 1U == args.size() || args[index + 1U].empty()) {
            throw std::runtime_error("missing build-config generator value for '" +
                                     std::string{option} + "'");
        }
        const std::string_view value = args[++index];
        if (option == "--override") {
            overrides.push_back(parse_override(value));
        } else if (!singleton.emplace(option, value).second) {
            throw std::runtime_error("build-config generator option '" + std::string{option} +
                                     "' must occur once");
        }
    }
    const auto require_option = [&singleton](const std::string_view option) {
        if (!singleton.contains(option)) {
            throw std::runtime_error("missing required build-config generator option '" +
                                     std::string{option} + "'");
        }
    };
    require_option("--compile-contract");
    require_option("--manifest");
    require_option("--output");
    require_option("--source");
    require_option("--stamp");
    command_line result{
        .source = singleton.at(std::string{"--source"}),
        .output = singleton.at(std::string{"--output"}),
        .compile_contract = singleton.at(std::string{"--compile-contract"}),
        .manifest = singleton.at(std::string{"--manifest"}),
        .stamp = singleton.at(std::string{"--stamp"}),
        .overrides = std::move(overrides),
    };
    if (!result.source.is_absolute() || !result.output.is_absolute() ||
        !result.compile_contract.is_absolute() || !result.manifest.is_absolute() ||
        !result.stamp.is_absolute()) {
        throw std::runtime_error("build-config generator paths must use absolute spellings");
    }
    if (result.output == result.compile_contract || result.output == result.manifest ||
        result.output == result.stamp || result.compile_contract == result.manifest ||
        result.compile_contract == result.stamp || result.manifest == result.stamp) {
        throw std::runtime_error("build-config output paths must be distinct");
    }
    return result;
}

inline int execute(const command_line& command) {
    const config_model model = collect_config_model(command.source);
    const generation_result generated = generate(model, command.overrides);
    bioseq::codegen::publish_artifact_if_changed(command.output, generated.module_source);
    bioseq::codegen::publish_artifact_if_changed(command.compile_contract,
                                                 generated.compile_contract_source);
    bioseq::codegen::publish_artifact_if_changed(command.manifest, generated.manifest_json);
    bioseq::codegen::mark_artifact_complete(command.stamp);
    return 0;
}

} // namespace detail

inline int self_test() {
    try {
        run_build_config_contract_tests();
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_build_config_codegen: {}", error.what());
        return 1;
    }
}

inline int run(const std::span<std::string_view const> args) {
    try {
        return detail::execute(detail::parse_command_line(args));
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_build_config_codegen: {}", error.what());
        return 1;
    }
}

} // namespace bioseq::codegen::build_config
