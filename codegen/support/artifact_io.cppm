module;

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <stdexcept>

export module bioseq.codegen.support:artifact_io;

namespace fs = std::filesystem;

export namespace bioseq::codegen {

/// @brief Borrowed name  of the component that generates a file.
struct artifact_owner_view {
    std::string_view identifier;
};

/// @brief Reads a file as binary data
/// @param path File to read.
/// @return File contents without newline conversion.
[[nodiscard]] std::string read_artifact_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    input.seekg(0, std::ios::end);
    const std::streampos length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("cannot size " + path.string());
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    input.seekg(0, std::ios::beg);
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input && !input.eof()) {
        throw std::runtime_error("cannot finish reading " + path.string());
    }
    return result;
}

/// @brief Replaces a generated file only when its contents, change.
/// @param path Destination fize.
/// @param content Desired bytes.
void publish_artifact_if_changed(
    const fs::path& path, std::string_view content) {
    std::error_code error;
    if (fs::exists(path, error) && !error) {
        const std::uintmax_t size = fs::file_size(path, error);
        if (!error && size == content.size()
            && read_artifact_bytes(path) == content) {
            return;
        }
    }
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write " + temporary.string());
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) {
            throw std::runtime_error("cannot finish writing " + temporary.string());
        }
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
    }
    if (error) {
        throw std::runtime_error("cannot publish " + path.string() + ": " + error.message());
    }
}

/// @brief Creates a completion stamp or updates its modification time.
/// @param path Stamp path.
void mark_artifact_complete(const fs::path& path) {
    fs::create_directories(path.parent_path());
    if (!fs::exists(path)) {
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("cannot create stamp " + path.string());
        }
    }
    fs::last_write_time(path, fs::file_time_type::clock::now());
}

/// @brief Normalizes a generated path and rejects absolute paths or remaining .. components.
/// @param path Candidate relative path.
/// @param owner Component name to include in errors.
/// @return Checked relative path.
[[nodiscard]] fs::path validate_owned_artifact_path(
    fs::path path,
    artifact_owner_view owner) {
    path = path.lexically_normal();
    if (path.empty() || path == "." || path.is_absolute()) {
        throw std::runtime_error(
            "annotation output path must be a non-empty relative path: "
            + std::string(owner.identifier));
    }
    for (const fs::path& component : path) {
        if (component == "..") {
            throw std::runtime_error(
                "annotation output path escapes the generation root: "
                + std::string(owner.identifier));
        }
    }
    return path;
}

}  // namespace bioseq::codegen
