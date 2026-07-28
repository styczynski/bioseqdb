module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

module bioseq.codegen.external:incremental_state;

import :model;
import bioseq.codegen.support;
import bioseq.common.hashing;

namespace fs = std::filesystem;

namespace bioseq::codegen::external {

/// @brief Loads cached per-source metadata and checks its registry signature.
/// @param path State file path.
/// @param signature Expected state signature.
/// @param signature_matches Receives whether the cache is reusable.
/// @return Parsed states, including legacy cleanup metadata.
[[nodiscard]] std::unordered_map<std::string, source_state>
load_source_state(
    const fs::path& path,
    std::string_view signature,
    bool& signature_matches) {
    std::unordered_map<std::string, source_state> result;
    signature_matches = false;
    if (!fs::exists(path)) return result;
    std::istringstream input(read_file(path));
    std::string line;
    if (!std::getline(input, line)) return result;
    signature_matches = line == "#signature\t" + std::string(signature);
    while (std::getline(input, line)) {
        std::array<std::string, 8> columns{};
        std::size_t begin = 0;
        std::size_t count = 0;
        while (count < columns.size()) {
            const auto tab = line.find('\t', begin);
            columns[count++] = line.substr(begin, tab - begin);
            if (tab == std::string::npos) break;
            begin = tab + 1;
        }
        if (count < 4) continue;
        source_state state;
        const auto size_result = std::from_chars(
            columns[1].data(), columns[1].data() + columns[1].size(), state.size);
        const auto ticks_result = std::from_chars(
            columns[2].data(), columns[2].data() + columns[2].size(), state.ticks);
        const auto hash_result = std::from_chars(
            columns[3].data(), columns[3].data() + columns[3].size(), state.hash, 16);
        if (size_result.ec != std::errc{} || ticks_result.ec != std::errc{}
            || hash_result.ec != std::errc{}) {
            continue;
        }
        state.annotated = count < 5 || columns[4] == "1";
        if (state.annotated) {
            if (count >= 7 && !columns[5].empty() && !columns[6].empty()) {
                state.annotation_name = columns[5];
                state.output_name = columns[6];
            } else {
                // State v1 only supported api_client, whose output name was the source stem.
                state.annotation_name = "bioseq::api_client";
                state.output_name = fs::path(columns[0]).stem().string();
            }
            if (count >= 8 && !columns[7].empty()) {
                state.owned_output_path = columns[7];
            } else if (state.annotation_name == "bioseq::api_client") {
                state.owned_output_path =
                    (fs::path("api") / state.output_name).generic_string();
            }
        }
        result.emplace(columns[0], state);
    }
    return result;
}

/// @brief Fingerprints handlers, renderer compiler, parser, and generator.
/// @param command Active generator configuration.
/// @param handlers Registered annotation contracts.
/// @return Hexadecimal cache signature.
[[nodiscard]] std::string state_signature(
    const command_line& command,
    const annotation_handlers& handlers) {
    std::vector<std::string> annotation_names;
    annotation_names.reserve(handlers.size());
    for (const auto& [name, handler] : handlers) {
        (void)handler;
        annotation_names.push_back(name);
    }
    std::ranges::sort(annotation_names);
    std::ostringstream input;
    input << "state-v5-codegen\n";
    for (const auto& name : annotation_names) input << name << '\n';
    const fs::path clang_binary = fs::weakly_canonical(command.clang_binary);
    input << command.compiler_id << '\n'
          << clang_binary.generic_string() << '\n'
          << fs::file_size(clang_binary) << '\n'
          << static_cast<std::int64_t>(
                 fs::last_write_time(clang_binary).time_since_epoch().count())
          << '\n'
          << fs::file_size(command.generator_binary) << '\n'
          << static_cast<std::int64_t>(
                 fs::last_write_time(command.generator_binary)
                     .time_since_epoch()
                     .count());
    std::ostringstream output;
    output << std::hex << bioseq::common::fnv1a_64(input.str());
    return output.str();
}

/// @brief Cheaply detects registered attribute spellings before cppast parsing.
/// @param content Source text to scan.
/// @param handlers Registered annotation contracts.
/// @return True when a registered Bioseq attribute is present.
[[nodiscard]] bool contains_registered_annotation(
    std::string_view content,
    const annotation_handlers& handlers) {
    return std::ranges::any_of(handlers, [content](const auto& entry) {
        std::size_t position = content.find(entry.first);
        while (position != std::string_view::npos) {
            std::size_t prefix = position;
            while (prefix > 0) {
                const char character = content[prefix - 1];
                if (character != ' ' && character != '\t') {
                    break;
                }
                --prefix;
            }
            std::size_t suffix = position + entry.first.size();
            while (suffix < content.size()
                   && (content[suffix] == ' ' || content[suffix] == '\t')) {
                ++suffix;
            }
            if (prefix >= 2 && content[prefix - 1] == '['
                && content[prefix - 2] == '[' && suffix < content.size()
                && content[suffix] == '(') {
                return true;
            }
            position = content.find(entry.first, position + entry.first.size());
        }
        return false;
    });
}

/// @brief Serializes one source's incremental metadata as TSV.
/// @param output State stream to extend.
/// @param relative Source path relative to the scan root.
/// @param state Metadata to persist.
void append_source_state(
    std::ostringstream& output,
    std::string_view relative,
    const source_state& state) {
    output << relative << '\t' << state.size << '\t' << state.ticks << '\t'
           << std::hex << state.hash << std::dec << '\t'
           << (state.annotated ? '1' : '0') << '\t'
           << state.annotation_name << '\t' << state.output_name << '\t'
           << state.owned_output_path << '\n';
}

/// @brief Checks whether original and normalized parser copies are staged.
/// @param output_root Generator-owned artifact root.
/// @param relative_path Descriptor path relative to the scan root.
/// @return True when both staged files exist.
[[nodiscard]] bool staged_sources_exist(
    const fs::path& output_root,
    const fs::path& relative_path) {
    fs::path shadow_relative = relative_path;
    shadow_relative.replace_extension(".shadow.cpp");
    return fs::exists(output_root / "annotation_sources" / relative_path)
        && fs::exists(output_root / "parser_sources" / shadow_relative);
}

}
