/**
 * @file incremental_state.cppm
 * @brief Current-schema incremental state for external-client generation.
 */
module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

module bioseq.codegen.external:incremental_state;

import :model;
import bioseq.codegen.support;
import bioseq.common.hashing;

namespace fs = std::filesystem;

namespace bioseq::codegen::external {

/// @brief Incremental state loaded from the generator-owned TSV artifact.
struct loaded_incremental_state {
    /// @brief Whether the signature and every row match the current schema.
    bool is_reusable{};
    /// @brief Valid current-schema snapshots, also used for stale-output cleanup.
    source_snapshots snapshots;
};

/// @brief Splits a current-schema state row into its eight required columns.
/// @param row One TSV row without its line terminator.
/// @return Column views, or absence when the row shape is invalid.
[[nodiscard]] std::optional<std::array<std::string_view, 8>>
split_source_snapshot_columns(std::string_view row) noexcept {
    std::array<std::string_view, 8> columns{};
    std::size_t column_begin = 0;
    for (std::size_t column_index = 0; column_index + 1 < columns.size(); ++column_index) {
        const std::size_t delimiter = row.find('\t', column_begin);
        if (delimiter == std::string_view::npos) {
            return std::nullopt;
        }
        columns[column_index] = row.substr(column_begin, delimiter - column_begin);
        column_begin = delimiter + 1;
    }
    columns.back() = row.substr(column_begin);
    if (columns.back().contains('\t')) {
        return std::nullopt;
    }
    return columns;
}

/// @brief Parses an integer only when the complete field has the requested representation.
/// @tparam p_integer_type Integer destination type.
/// @param text Field text.
/// @param base Numeric base accepted by `std::from_chars`.
/// @return Parsed integer, or absence for malformed or out-of-range input.
template<class p_integer_type>
[[nodiscard]] std::optional<p_integer_type> parse_state_integer(
    std::string_view text,
    int base = 10) noexcept {
    p_integer_type value{};
    const char* const field_end = text.data() + text.size();
    const std::from_chars_result parsed =
        std::from_chars(text.data(), field_end, value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != field_end) {
        return std::nullopt;
    }
    return value;
}

/// @brief Parses one strict current-schema source snapshot row.
/// @param row One TSV row without its line terminator.
/// @return Relative source path and snapshot, or absence when any field is invalid.
[[nodiscard]] std::optional<std::pair<std::string, source_snapshot>>
parse_source_snapshot_row(std::string_view row) {
    const std::optional<std::array<std::string_view, 8>> columns =
        split_source_snapshot_columns(row);
    if (!columns || columns->front().empty()) {
        return std::nullopt;
    }

    const std::optional<std::uintmax_t> file_size_bytes =
        parse_state_integer<std::uintmax_t>((*columns)[1]);
    const std::optional<std::int64_t> last_write_ticks =
        parse_state_integer<std::int64_t>((*columns)[2]);
    const std::optional<std::uint64_t> content_hash =
        parse_state_integer<std::uint64_t>((*columns)[3], 16);
    if (!file_size_bytes || !last_write_ticks || !content_hash) {
        return std::nullopt;
    }
    if ((*columns)[4] != "0" && (*columns)[4] != "1") {
        return std::nullopt;
    }

    const bool has_annotation = (*columns)[4] == "1";
    const bool has_annotation_metadata = !(*columns)[5].empty()
        && !(*columns)[6].empty() && !(*columns)[7].empty();
    const bool has_unexpected_metadata = !has_annotation
        && (!(*columns)[5].empty() || !(*columns)[6].empty() || !(*columns)[7].empty());
    if ((has_annotation && !has_annotation_metadata) || has_unexpected_metadata) {
        return std::nullopt;
    }

    return std::pair{
        std::string((*columns)[0]),
        source_snapshot{
            .file_size_bytes = *file_size_bytes,
            .last_write_ticks = *last_write_ticks,
            .content_hash = *content_hash,
            .has_annotation = has_annotation,
            .annotation_name = std::string((*columns)[5]),
            .output_name = std::string((*columns)[6]),
            .owned_output_path = std::string((*columns)[7]),
        },
    };
}

/// @brief Loads current per-source metadata and validates its registry signature.
/// @param state_path Generator-owned state file path.
/// @param expected_signature Signature for the active generator configuration.
/// @return Parsed snapshots plus a single reuse decision.
[[nodiscard]] loaded_incremental_state load_incremental_state(
    const fs::path& state_path,
    std::string_view expected_signature) {
    loaded_incremental_state loaded_state;
    if (!fs::exists(state_path)) {
        return loaded_state;
    }

    std::istringstream state_input(read_artifact_bytes(state_path));
    std::string row;
    if (!std::getline(state_input, row)) {
        return loaded_state;
    }
    loaded_state.is_reusable = row == "#signature\t" + std::string(expected_signature);

    while (std::getline(state_input, row)) {
        std::optional<std::pair<std::string, source_snapshot>> parsed_row =
            parse_source_snapshot_row(row);
        if (!parsed_row) {
            loaded_state.is_reusable = false;
            continue;
        }
        const auto [snapshot_position, inserted] = loaded_state.snapshots.emplace(
            std::move(parsed_row->first),
            std::move(parsed_row->second));
        (void)snapshot_position;
        if (!inserted) {
            loaded_state.is_reusable = false;
        }
    }
    return loaded_state;
}

/// @brief Fingerprints processors, compilers, parser input, and generator binary.
/// @param options Active generator configuration.
/// @param registry Registered annotation processors.
/// @return Hexadecimal FNV-1a signature for the current state contract.
[[nodiscard]] std::string compute_generation_state_signature(
    const generation_options& options,
    const annotation_registry& registry) {
    std::ostringstream signature_input;
    signature_input << "external-client-generation-state\n";
    for (const auto& registry_entry : registry) {
        signature_input << registry_entry.first << '\n';
    }

    const fs::path clang_binary = fs::weakly_canonical(options.clang_binary);
    signature_input
        << options.compiler_id << '\n'
        << clang_binary.generic_string() << '\n'
        << fs::file_size(clang_binary) << '\n'
        << static_cast<std::int64_t>(
               fs::last_write_time(clang_binary).time_since_epoch().count())
        << '\n'
        << fs::file_size(options.generator_binary) << '\n'
        << static_cast<std::int64_t>(
               fs::last_write_time(options.generator_binary).time_since_epoch().count());

    std::ostringstream signature;
    signature << std::hex << bioseq::common::fnv1a_64(signature_input.str());
    return signature.str();
}

/// @brief Detects a registered attribute spelling before invoking cppast.
/// @param source_text Exact module source bytes.
/// @param registry Registered annotation processors.
/// @return True when a registered Bioseq attribute invocation is present.
[[nodiscard]] bool has_registered_annotation(
    std::string_view source_text,
    const annotation_registry& registry) {
    return std::ranges::any_of(registry, [source_text](const auto& registry_entry) {
        const std::string& annotation_name = registry_entry.first;
        std::size_t position = source_text.find(annotation_name);
        while (position != std::string_view::npos) {
            std::size_t prefix = position;
            while (prefix > 0) {
                const char character = source_text[prefix - 1];
                if (character != ' ' && character != '\t') {
                    break;
                }
                --prefix;
            }

            std::size_t suffix = position + annotation_name.size();
            while (suffix < source_text.size()
                   && (source_text[suffix] == ' ' || source_text[suffix] == '\t')) {
                ++suffix;
            }
            if (prefix >= 2 && source_text[prefix - 1] == '['
                && source_text[prefix - 2] == '[' && suffix < source_text.size()
                && source_text[suffix] == '(') {
                return true;
            }
            position = source_text.find(annotation_name, position + annotation_name.size());
        }
        return false;
    });
}

/// @brief Serializes one source snapshot using the current eight-column TSV contract.
/// @param state_output State stream to extend.
/// @param relative_source Source path relative to the scan root.
/// @param snapshot Metadata to persist.
void append_source_snapshot(
    std::ostringstream& state_output,
    std::string_view relative_source,
    const source_snapshot& snapshot) {
    state_output << relative_source << '\t'
                 << snapshot.file_size_bytes << '\t'
                 << snapshot.last_write_ticks << '\t'
                 << std::hex << snapshot.content_hash << std::dec << '\t'
                 << (snapshot.has_annotation ? '1' : '0') << '\t'
                 << snapshot.annotation_name << '\t'
                 << snapshot.output_name << '\t'
                 << snapshot.owned_output_path << '\n';
}

/// @brief Checks whether original and normalized parser copies are staged.
/// @param output_root Generator-owned artifact root.
/// @param relative_source Descriptor path relative to the scan root.
/// @return True when both staged source files exist.
[[nodiscard]] bool has_staged_parser_sources(
    const fs::path& output_root,
    const fs::path& relative_source) {
    fs::path parser_relative = relative_source;
    parser_relative.replace_extension(".shadow.cpp");
    return fs::exists(output_root / "annotation_sources" / relative_source)
        && fs::exists(output_root / "parser_sources" / parser_relative);
}

} // namespace bioseq::codegen::external
