/**
 * @file incremental_state.cppm
 * @brief Saves and reloads the information used to skip unchanged client generation.
 */
module;

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

/// @brief Source records and reuse status loaded from generation_state.tsv.
struct loaded_incremental_state {
    /// @brief Whether the signature and every row match the current schema.
    bool is_reusable{};
    /// @brief Valid source records, kept even when regeneration is required.
    source_snapshots snapshots;
};

/// @brief Splits a saved source record into eight tab-separated fields.
/// @param row TSV row without its line ending.
/// @return Field views, or std::nullopt if the field count is wrong.
[[nodiscard]] auto split_source_snapshot_columns(std::string_view row) noexcept
    -> std::optional<std::array<std::string_view, 8>> {
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

/// @brief Reads an integer, rejecting incomplete values and overflow.
/// @tparam p_integer_type Integer type to read.
/// @param text Field text.
/// @param base Number base accepted by std::from_chars.
/// @return Parsed value, or std::nullopt for invalid input.
template <class p_integer_type>
[[nodiscard]] auto parse_state_integer(std::string_view text, int base = 10) noexcept
    -> std::optional<p_integer_type> {
    p_integer_type value{};
    const char* const field_end = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(text.data(), field_end, value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != field_end) {
        return std::nullopt;
    }
    return value;
}

/// @brief Reads and validates one saved source record.
/// @param row TSV row without its line ending.
/// @return Relative source path and saved metadata, or std::nullopt for invalid input.
[[nodiscard]] auto parse_source_snapshot_row(std::string_view row)
    -> std::optional<std::pair<std::string, source_snapshot>> {
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
    const bool has_annotation_metadata =
        !(*columns)[5].empty() && !(*columns)[6].empty() && !(*columns)[7].empty();
    const bool has_unexpected_metadata =
        !has_annotation &&
        (!(*columns)[5].empty() || !(*columns)[6].empty() || !(*columns)[7].empty());
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

/// @brief Loads saved source records and checks whether they can be reused.
/// @param state_path Path to generation_state.tsv.
/// @param expected_signature Signature of the current generator configuration.
/// @return Valid records and a reuse flag.
/// @details A signature mismatch or invalid row requires regeneration. Valid rows
/// remain available so the builder can remove obsolete output.
[[nodiscard]] auto load_incremental_state(const fs::path& state_path,
                                          std::string_view expected_signature)
    -> loaded_incremental_state {
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
            std::move(parsed_row->first), std::move(parsed_row->second));
        (void)snapshot_position;
        if (!inserted) {
            loaded_state.is_reusable = false;
        }
    }
    return loaded_state;
}

/// @brief Hashes the generator configuration to decide whether saved state is reusable.
/// @param options Generator settings.
/// @param registry Registered annotation handlers.
/// @return FNV-1a hash written in hexadecimal.
/// @details The signature includes handler names, compiler identity and the paths,
/// sizes or write times used to identify the Clang and generator binaries.
[[nodiscard]] auto compute_generation_state_signature(const generation_options& options,
                                                      const annotation_registry& registry)
    -> std::string {
    std::ostringstream signature_input;
    signature_input << "external-client-generation-state-v2-fragment-closure\n";
    for (const auto& registry_entry : registry) {
        signature_input << registry_entry.first << '\n';
    }

    const fs::path clang_binary = fs::weakly_canonical(options.clang_binary);
    signature_input
        << options.compiler_id << '\n'
        << clang_binary.generic_string() << '\n'
        << fs::file_size(clang_binary) << '\n'
        << static_cast<std::int64_t>(fs::last_write_time(clang_binary).time_since_epoch().count())
        << '\n'
        << fs::file_size(options.generator_binary) << '\n'
        << static_cast<std::int64_t>(
               fs::last_write_time(options.generator_binary).time_since_epoch().count());

    std::ostringstream signature;
    signature << std::hex << bioseq::common::fnv1a_64(signature_input.str());
    return signature.str();
}

/// @brief Writes one source record in the eight-field TSV format.
/// @param state_output Output stream for the state file.
/// @param relative_source Source path relative to the scan root.
/// @param snapshot Source information to save.
auto append_source_snapshot(std::ostringstream& state_output,
                            std::string_view relative_source,
                            const source_snapshot& snapshot) -> void {
    state_output << relative_source << '\t' << snapshot.file_size_bytes << '\t'
                 << snapshot.last_write_ticks << '\t' << std::hex << snapshot.content_hash
                 << std::dec << '\t' << (snapshot.has_annotation ? '1' : '0') << '\t'
                 << snapshot.annotation_name << '\t' << snapshot.output_name << '\t'
                 << snapshot.owned_output_path << '\n';
}

/// @brief Checks that the saved source and its cppast input copy both exist.
/// @param output_root Directory for generated output.
/// @param relative_source Descriptor path relative to the scan root.
/// @return True when both files exist.
[[nodiscard]] auto has_staged_parser_sources(const fs::path& output_root,
                                             const fs::path& relative_source) -> bool {
    fs::path parser_relative = relative_source;
    parser_relative.replace_extension(".shadow.cpp");
    return fs::exists(output_root / "annotation_sources" / relative_source) &&
           fs::exists(output_root / "parser_sources" / parser_relative);
}

} // namespace bioseq::codegen::external
