module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.test.postgres:artifact;

import :row_hash;

export namespace bioseq::postgres_test {

/// @brief Build outputs required to install one extension in a private cluster.
struct extension_artifact {
    std::string name;
    std::filesystem::path control_file;
    std::filesystem::path install_sql;
    std::filesystem::path dso;
};

/// @brief Source/staged paths and SHA-256 evidence for one extension triple.
struct staged_extension_artifact {
    std::string name;
    std::filesystem::path source_control_file;
    std::filesystem::path source_install_sql;
    std::filesystem::path source_dso;
    std::filesystem::path staged_control_file;
    std::filesystem::path staged_install_sql;
    std::filesystem::path staged_dso;
    sha256_digest control_digest;
    sha256_digest install_sql_digest;
    sha256_digest dso_digest;
    sha256_digest staged_control_digest;
};

/// @brief Exact private directories holding staged extension artifacts.
struct extension_stage {
    std::filesystem::path root;
    std::filesystem::path control_directory;
    std::filesystem::path library_directory;
    std::vector<staged_extension_artifact> extensions;
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::artifact_detail {

/// @brief Rejects names that cannot safely become SQL/configuration identities.
inline void validate_extension_name(std::string_view name) {
    if (name.empty() || name.size() > 63
        || !(std::islower(static_cast<unsigned char>(name.front()))
             || name.front() == '_')) {
        throw std::invalid_argument("invalid extension artifact name");
    }
    for (const char character : name) {
        if (!std::islower(static_cast<unsigned char>(character))
            && !std::isdigit(static_cast<unsigned char>(character))
            && character != '_') {
            throw std::invalid_argument("invalid extension artifact name: "
                                        + std::string(name));
        }
    }
}

/// @brief Resolves a non-symlink regular artifact file.
[[nodiscard]] inline std::filesystem::path require_regular_file(
    const std::filesystem::path& path,
    std::string_view label) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status)
        || std::filesystem::is_symlink(status)) {
        throw std::invalid_argument(std::string(label)
                                    + " is not a regular non-symlink file: "
                                    + path.string());
    }
    const std::filesystem::path canonical =
        std::filesystem::canonical(path, error);
    if (error) {
        throw std::invalid_argument("cannot resolve "
                                    + std::string(label) + ": "
                                    + path.string());
    }
    return canonical;
}

/// @brief Returns the platform suffix that PostgreSQL's loader will append.
[[nodiscard]] inline std::string shared_library_suffix(
    const std::filesystem::path& source) {
    const std::string filename = source.filename().string();
    constexpr std::array<std::string_view, 4> suffixes{
        ".dylib", ".bundle", ".dll", ".so"};
    for (const std::string_view suffix : suffixes) {
        const std::size_t position = filename.find(suffix);
        if (position != std::string::npos) {
            return std::string(filename.substr(position));
        }
    }
    throw std::invalid_argument("unrecognized PostgreSQL DSO suffix: "
                                + source.string());
}

/// @brief Overwrites only one explicit destination with one source artifact.
inline void copy_exact(const std::filesystem::path& source,
                       const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::remove(destination, error);
    error.clear();
    if (!std::filesystem::copy_file(
            source, destination, std::filesystem::copy_options::none, error)
        || error) {
        throw std::runtime_error("cannot stage " + source.string() + " as "
                                 + destination.string() + ": "
                                 + error.message());
    }
}

/// @brief Escapes a PostgreSQL configuration/control quoted string.
[[nodiscard]] inline std::string control_literal(std::string_view value) {
    if (value.find('\n') != std::string_view::npos
        || value.find('\r') != std::string_view::npos) {
        throw std::invalid_argument("control value contains a newline");
    }
    std::string escaped{"'"};
    for (const char character : value) {
        if (character == '\'') {
            escaped.push_back('\'');
        }
        escaped.push_back(character);
    }
    escaped.push_back('\'');
    return escaped;
}

/// @brief Copies a control file and pins MODULE_PATHNAME to the staged DSO stem.
///
/// The staged control/install files form the fixture's immutable artifact
/// bundle. Rewriting only the staged control copy directs MODULE_PATHNAME to
/// the per-case DSO without mutating the source artifact or relying on a
/// server-version-specific control-file search setting.
inline void stage_control_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::filesystem::path& staged_dso_stem) {
    std::ifstream input(source);
    if (!input) {
        throw std::runtime_error("cannot read extension control file: "
                                 + source.string());
    }
    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(input, line)) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos
            && std::string_view(line).substr(first).starts_with(
                "module_pathname")) {
            line = "module_pathname = "
                + control_literal(staged_dso_stem.string());
            replaced = true;
        }
        lines.push_back(std::move(line));
    }
    if (!input.eof()) {
        throw std::runtime_error("failed reading extension control file: "
                                 + source.string());
    }
    if (!replaced) {
        lines.push_back("module_pathname = "
                        + control_literal(staged_dso_stem.string()));
    }
    std::error_code error;
    std::filesystem::remove(destination, error);
    std::ofstream output(destination, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot stage extension control file: "
                                 + destination.string());
    }
    for (const std::string& output_line : lines) {
        output << output_line << '\n';
    }
    output.close();
    if (!output) {
        throw std::runtime_error("failed staging extension control file: "
                                 + destination.string());
    }
}

}  // namespace bioseq::postgres_test::artifact_detail

export namespace bioseq::postgres_test {

/// @brief Validates and stages extension triples without glob deletion.
[[nodiscard]] inline extension_stage stage_extension_artifacts(
    const std::filesystem::path& root,
    std::span<const extension_artifact> artifacts) {
    if (artifacts.empty()) {
        throw std::invalid_argument(
            "private PostgreSQL staging requires at least one extension");
    }
    extension_stage stage{
        .root = root,
        .control_directory = root / "share" / "postgresql" / "extension",
        .library_directory = root / "lib" / "postgresql",
        .extensions = {}};
    std::filesystem::create_directories(stage.control_directory);
    std::filesystem::create_directories(stage.library_directory);

    std::set<std::string> extension_names;
    stage.extensions.reserve(artifacts.size());
    for (const extension_artifact& artifact : artifacts) {
        artifact_detail::validate_extension_name(artifact.name);
        if (!extension_names.insert(artifact.name).second) {
            throw std::invalid_argument("duplicate extension artifact name: "
                                        + artifact.name);
        }
        const std::filesystem::path source_control =
            artifact_detail::require_regular_file(
                artifact.control_file, "control file");
        const std::filesystem::path source_sql =
            artifact_detail::require_regular_file(
                artifact.install_sql, "install SQL");
        const std::filesystem::path source_dso =
            artifact_detail::require_regular_file(artifact.dso, "extension DSO");

        const std::string sql_name = source_sql.filename().string();
        if (!sql_name.starts_with(artifact.name + "--")
            || !sql_name.ends_with(".sql")) {
            throw std::invalid_argument(
                "install SQL must use canonical name--version.sql basename: "
                + source_sql.string());
        }
        const std::string dso_suffix =
            artifact_detail::shared_library_suffix(source_dso);
        const std::filesystem::path staged_control =
            stage.control_directory / (artifact.name + ".control");
        const std::filesystem::path staged_sql =
            stage.control_directory / sql_name;
        const std::filesystem::path staged_dso =
            stage.library_directory / (artifact.name + dso_suffix);

        artifact_detail::copy_exact(source_sql, staged_sql);
        artifact_detail::copy_exact(source_dso, staged_dso);
        artifact_detail::stage_control_file(
            source_control, staged_control,
            stage.library_directory / artifact.name);

        stage.extensions.push_back(staged_extension_artifact{
            .name = artifact.name,
            .source_control_file = source_control,
            .source_install_sql = source_sql,
            .source_dso = source_dso,
            .staged_control_file = staged_control,
            .staged_install_sql = staged_sql,
            .staged_dso = staged_dso,
            .control_digest = sha256_file(source_control),
            .install_sql_digest = sha256_file(source_sql),
            .dso_digest = sha256_file(source_dso),
            .staged_control_digest = sha256_file(staged_control)});
    }
    return stage;
}

}  // namespace bioseq::postgres_test
