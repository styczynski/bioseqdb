/**
 * @file common/durable_file_io/path.cppm
 * @brief Internal durable-file pathname validation and symlink classification.
 */
module;

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

export module bioseq.common.durable_file_io:path;

namespace bioseq::common::durable_file_io::detail {

inline constexpr mode_t creation_permission_mask =
    S_IRWXU | S_IRWXG | S_IRWXO;

[[nodiscard]] inline bool valid_creation_mode(mode_t mode) noexcept {
    return mode != 0 && (mode & ~creation_permission_mask) == 0;
}

[[nodiscard]] inline bool contains_parent_traversal(
    const std::filesystem::path& path) {
    for (const std::filesystem::path& component : path) {
        if (component == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::filesystem::path parent_directory(
    const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    return parent.empty() ? std::filesystem::path{"."} : parent;
}

[[nodiscard]] inline std::string leaf_name(
    const std::filesystem::path& path) {
    return path.filename().string();
}

[[nodiscard]] inline bool valid_leaf_name(
    std::string_view name) noexcept {
    return !name.empty() && name != "." && name != ".."
        && name.find('\0') == std::string_view::npos
        && name.find('/') == std::string_view::npos;
}

[[nodiscard]] inline std::filesystem::path directory_leaf_path(
    std::filesystem::path directory) {
    while (directory != directory.root_path()
           && (directory.filename().empty()
               || directory.filename() == ".")) {
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return directory.empty()
        ? std::filesystem::path{"."}
        : directory;
}

[[nodiscard]] inline bool directory_path_is_symlink_after_open_error(
    const std::filesystem::path& directory,
    int open_error) noexcept {
    if (open_error == ELOOP) {
        return true;
    }
    if (open_error != ENOTDIR) {
        return false;
    }
    struct stat status {};
    int result = -1;
    do {
        result = ::lstat(directory.c_str(), &status);
    } while (result != 0 && errno == EINTR);
    return result == 0 && S_ISLNK(status.st_mode);
}

[[nodiscard]] inline bool directory_name_is_symlink_after_open_error(
    int directory_descriptor,
    const std::string& name,
    int open_error) noexcept {
    if (open_error == ELOOP) {
        return true;
    }
#if defined(AT_SYMLINK_NOFOLLOW)
    if (open_error != ENOTDIR) {
        return false;
    }
    struct stat status {};
    int result = -1;
    do {
        result = ::fstatat(
            directory_descriptor,
            name.c_str(),
            &status,
            AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    return result == 0 && S_ISLNK(status.st_mode);
#else
    static_cast<void>(directory_descriptor);
    static_cast<void>(name);
    return false;
#endif
}

}  // namespace bioseq::common::durable_file_io::detail
