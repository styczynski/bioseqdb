/**
 * @file common/durable_file_io/detail.cppm
 * @brief Internal POSIX helpers shared by durable-file operations.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.common.durable_file_io:detail;
import :types;
namespace bioseq::durable_file_io::detail {

[[nodiscard]] inline file_io_error make_error(
    file_io_errc code,
    file_io_operation operation,
    int system_error,
    std::uint64_t offset = 0,
    publication_state publication = publication_state::unchanged) noexcept {
    return file_io_error{
        .code = code,
        .operation = operation,
        .system_error = system_error,
        .offset = offset,
        .publication = publication,
    };
}

[[nodiscard]] inline int injected_error(
    const file_io_hooks& hooks,
    file_io_operation operation) noexcept {
    if (hooks.before_operation == nullptr) {
        return 0;
    }
    const int injected = hooks.before_operation(hooks.context, operation);
    return injected > 0 ? injected : 0;
}

[[nodiscard]] inline ssize_t system_read_at(
    void*,
    int descriptor,
    void* destination,
    std::size_t bytes,
    off_t offset) noexcept {
    return ::pread(descriptor, destination, bytes, offset);
}

[[nodiscard]] inline ssize_t system_write_at(
    void*,
    int descriptor,
    const void* source,
    std::size_t bytes,
    off_t offset) noexcept {
    return ::pwrite(descriptor, source, bytes, offset);
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

[[nodiscard]] inline regular_file_identity identity_from_stat(
    const struct stat& status) noexcept {
    regular_file_identity result{
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
        .size = static_cast<std::uint64_t>(status.st_size),
        .link_count = static_cast<std::uint64_t>(status.st_nlink),
        .owner = static_cast<std::uint64_t>(status.st_uid),
        .mode = static_cast<std::uint64_t>(status.st_mode),
    };
#if defined(__APPLE__)
    result.modified_seconds = status.st_mtimespec.tv_sec;
    result.modified_nanoseconds = status.st_mtimespec.tv_nsec;
    result.changed_seconds = status.st_ctimespec.tv_sec;
    result.changed_nanoseconds = status.st_ctimespec.tv_nsec;
#else
    result.modified_seconds = status.st_mtim.tv_sec;
    result.modified_nanoseconds = status.st_mtim.tv_nsec;
    result.changed_seconds = status.st_ctim.tv_sec;
    result.changed_nanoseconds = status.st_ctim.tv_nsec;
#endif
    return result;
}

[[nodiscard]] inline bool same_file_object(
    const regular_file_identity& left,
    const regular_file_identity& right) noexcept {
    return left.device == right.device
        && left.inode == right.inode
        && left.link_count == right.link_count
        && left.owner == right.owner
        && left.mode == right.mode;
}

[[nodiscard]] inline bool same_inode(
    const regular_file_identity& left,
    const regular_file_identity& right) noexcept {
    return left.device == right.device
        && left.inode == right.inode;
}

[[nodiscard]] inline bool same_published_identity(
    const regular_file_identity& left,
    const regular_file_identity& right) noexcept {
    return same_inode(left, right)
        && left.size == right.size
        && left.owner == right.owner
        && left.mode == right.mode
        && left.modified_seconds == right.modified_seconds
        && left.modified_nanoseconds == right.modified_nanoseconds;
}

[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
capture_identity(
    int descriptor,
    file_io_operation operation,
    const file_io_hooks& hooks) noexcept {
    if (const int injected = injected_error(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::stat_failed, operation, injected));
    }
    struct stat status {};
    int status_result = -1;
    do {
        status_result = ::fstat(descriptor, &status);
    } while (status_result != 0 && errno == EINTR);
    if (status_result != 0) {
        return std::unexpected(make_error(
            file_io_errc::stat_failed, operation, errno));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(make_error(
            file_io_errc::not_regular_file, operation, 0));
    }
    if (status.st_size < 0) {
        return std::unexpected(make_error(
            file_io_errc::invalid_file_size, operation, 0));
    }
    return identity_from_stat(status);
}

[[nodiscard]] inline int regular_file_open_flags(int access) noexcept {
    int flags = access | O_NONBLOCK;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    return flags;
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

[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_directory_path(
    const std::filesystem::path& directory,
    const file_io_hooks& hooks,
    publication_state publication = publication_state::unchanged) {
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW)
    static_cast<void>(directory);
    static_cast<void>(hooks);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::open_directory,
        0,
        0,
        publication));
#else
    if (directory.empty()
        || directory.string().find('\0') != std::string::npos) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_directory,
            EINVAL,
            0,
            publication));
    }
    const std::filesystem::path leaf_directory =
        directory_leaf_path(directory);
    if (const int injected = injected_error(
            hooks, file_io_operation::open_directory);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            injected,
            0,
            publication));
    }
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(leaf_directory.c_str(), flags);
    if (descriptor < 0) {
        const int saved = errno;
        return std::unexpected(make_error(
            directory_path_is_symlink_after_open_error(
                leaf_directory, saved)
                ? file_io_errc::symlink_rejected
                : file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            saved,
            0,
            publication));
    }
    return file_descriptor{descriptor};
#endif
}

[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_directory_at(
    int directory_descriptor,
    const std::string& name,
    const file_io_hooks& hooks) {
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW)
    static_cast<void>(directory_descriptor);
    static_cast<void>(name);
    static_cast<void>(hooks);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::open_directory,
        0));
#else
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_directory,
            EINVAL));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::open_directory);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            injected));
    }
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::openat(
        directory_descriptor, name.c_str(), flags);
    if (descriptor < 0) {
        const int saved = errno;
        return std::unexpected(make_error(
            directory_name_is_symlink_after_open_error(
                directory_descriptor, name, saved)
                ? file_io_errc::symlink_rejected
                : file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            saved));
    }
    return file_descriptor{descriptor};
#endif
}

[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_parent_directory_at(
    int directory_descriptor,
    const file_io_hooks& hooks) {
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW)
    static_cast<void>(directory_descriptor);
    static_cast<void>(hooks);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::open_directory,
        0));
#else
    if (directory_descriptor < 0) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_directory,
            EINVAL));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::open_directory);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            injected));
    }
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::openat(
        directory_descriptor, "..", flags);
    if (descriptor < 0) {
        return std::unexpected(make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            errno));
    }
    return file_descriptor{descriptor};
#endif
}

[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    int access,
    file_io_operation operation,
    const file_io_hooks& hooks) {
#if !defined(O_NOFOLLOW)
    static_cast<void>(directory_descriptor);
    static_cast<void>(name);
    static_cast<void>(access);
    static_cast<void>(operation);
    static_cast<void>(hooks);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform, operation, 0));
#else
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument, operation, EINVAL));
    }
    if (const int injected = injected_error(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::open_failed, operation, injected));
    }
    const int descriptor = ::openat(
        directory_descriptor,
        name.c_str(),
        regular_file_open_flags(access));
    if (descriptor < 0) {
        const int saved = errno;
        return std::unexpected(make_error(
            saved == ELOOP
                ? file_io_errc::symlink_rejected
                : file_io_errc::open_failed,
            operation,
            saved));
    }
    return file_descriptor{descriptor};
#endif
}

[[nodiscard]] inline std::expected<void, file_io_error> sync_descriptor(
    int descriptor,
    file_io_operation operation,
    const file_io_hooks& hooks,
    publication_state publication = publication_state::unchanged) noexcept {
    if (const int injected = injected_error(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            operation == file_io_operation::sync_directory
                ? file_io_errc::directory_sync_failed
                : file_io_errc::file_sync_failed,
            operation,
            injected,
            0,
            publication));
    }
    int sync_result = -1;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    if (sync_result != 0) {
        return std::unexpected(make_error(
            operation == file_io_operation::sync_directory
                ? file_io_errc::directory_sync_failed
                : file_io_errc::file_sync_failed,
            operation,
            errno,
            0,
            publication));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, file_io_error>
close_directory_descriptor(
    file_descriptor& descriptor,
    const file_io_hooks& hooks,
    publication_state publication) noexcept {
    if (!descriptor.valid()) {
        return {};
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::close_directory);
        injected != 0) {
        descriptor.reset();
        return std::unexpected(make_error(
            file_io_errc::close_failed,
            file_io_operation::close_directory,
            injected,
            0,
            publication));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        file_io_error error = closed.error();
        error.operation = file_io_operation::close_directory;
        error.publication = publication;
        return std::unexpected(error);
    }
    return {};
}

[[nodiscard]] inline std::string unique_temporary_name(
    std::string_view destination_name) {
    static std::atomic<std::uint64_t> sequence{1};
    const std::uint64_t value =
        sequence.fetch_add(1, std::memory_order_relaxed);
    return std::string(destination_name)
        + ".tmp." + std::to_string(static_cast<std::uint64_t>(::getpid()))
        + "." + std::to_string(value);
}

[[nodiscard]] inline std::expected<void, file_io_error> remove_name_at(
    int directory_descriptor,
    const std::string& name,
    publication_state publication,
    const file_io_hooks& hooks = {}) noexcept {
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::remove_temporary,
            EINVAL,
            0,
            publication));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::remove_temporary);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::cleanup_failed,
            file_io_operation::remove_temporary,
            injected,
            0,
            publication));
    }
    if (::unlinkat(directory_descriptor, name.c_str(), 0) == 0
        || errno == ENOENT) {
        return {};
    }
    return std::unexpected(make_error(
        file_io_errc::cleanup_failed,
        file_io_operation::remove_temporary,
        errno,
        0,
        publication));
}

/**
 * Remove a staging entry only while it still names the inode we created.
 *
 * The deterministic hook runs before the final identity snapshot. POSIX has
 * no inode-conditional unlink syscall, so callers must serialize uncooperative
 * writers to the same directory across the final fstatat/unlinkat pair.
 */
[[nodiscard]] inline std::expected<
    conditional_remove_disposition,
    file_io_error>
remove_name_if_same_inode_at(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    publication_state publication,
    const file_io_hooks& hooks = {}) noexcept {
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::remove_temporary,
            EINVAL,
            0,
            publication));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::remove_temporary);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::cleanup_failed,
            file_io_operation::remove_temporary,
            injected,
            0,
            publication));
    }
    struct stat status {};
    int status_result = -1;
    do {
        status_result = ::fstatat(
            directory_descriptor,
            name.c_str(),
            &status,
            AT_SYMLINK_NOFOLLOW);
    } while (status_result != 0 && errno == EINTR);
    if (status_result != 0) {
        if (errno == ENOENT) {
            return conditional_remove_disposition::already_absent;
        }
        return std::unexpected(make_error(
            file_io_errc::cleanup_failed,
            file_io_operation::remove_temporary,
            errno,
            0,
            publication));
    }
    if (!S_ISREG(status.st_mode)
        || !same_inode(identity_from_stat(status), expected)) {
        return conditional_remove_disposition::identity_mismatch;
    }
    auto removed = remove_name_at(
        directory_descriptor, name, publication);
    if (!removed.has_value()) {
        return std::unexpected(removed.error());
    }
    return conditional_remove_disposition::removed;
}

[[nodiscard]] inline std::expected<publication_state, file_io_error>
link_name_no_replace_at(
    int source_directory,
    const std::string& source_name,
    int destination_directory,
    const std::string& destination_name,
    const file_io_hooks& hooks,
    const regular_file_identity* expected_source = nullptr) {
    if (source_directory < 0 || destination_directory < 0
        || !valid_leaf_name(source_name)
        || !valid_leaf_name(destination_name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::publish_link,
            EINVAL));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::publish_link);
        injected != 0) {
        return std::unexpected(make_error(
            injected == EEXIST
                ? file_io_errc::destination_exists
                : file_io_errc::rename_failed,
            file_io_operation::publish_link,
            injected));
    }
    file_descriptor pinned_source{};
    regular_file_identity pinned_identity{};
    auto opened = open_regular_file_at(
        source_directory,
        source_name,
        O_RDONLY,
        file_io_operation::open_file,
        {});
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    pinned_source = std::move(*opened);
    auto current = capture_identity(
        pinned_source.get(), file_io_operation::stat_after, {});
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (expected_source != nullptr && *current != *expected_source) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    pinned_identity = *current;
    if (const int injected = injected_error(
            hooks, file_io_operation::stat_after);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::stat_failed,
            file_io_operation::stat_after,
            injected));
    }
    if (::linkat(
            source_directory,
            source_name.c_str(),
            destination_directory,
            destination_name.c_str(),
            0) != 0) {
        const int saved = errno;
        return std::unexpected(make_error(
            saved == EEXIST
                ? file_io_errc::destination_exists
                : file_io_errc::rename_failed,
            file_io_operation::publish_link,
            saved));
    }
    auto destination_opened = open_regular_file_at(
        destination_directory,
        destination_name,
        O_RDONLY,
        file_io_operation::open_file,
        {});
    if (!destination_opened.has_value()) {
        file_io_error error = destination_opened.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    file_descriptor destination_descriptor =
        std::move(*destination_opened);
    auto published = capture_identity(
        destination_descriptor.get(),
        file_io_operation::stat_after,
        {});
    if (!published.has_value()) {
        file_io_error error = published.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    if (!same_published_identity(*published, pinned_identity)) {
        if (auto removed = remove_name_if_same_inode_at(
                destination_directory,
                destination_name,
                *published,
                publication_state::visible_not_durable,
                hooks);
            !removed.has_value()) {
            return std::unexpected(removed.error());
        } else if (*removed
                   == conditional_remove_disposition::identity_mismatch) {
            return std::unexpected(make_error(
                file_io_errc::identity_changed,
                file_io_operation::remove_temporary,
                ESTALE,
                0,
                publication_state::visible_not_durable));
        }
        if (auto synced = sync_descriptor(
                destination_directory,
                file_io_operation::sync_directory,
                hooks,
                publication_state::visible_not_durable);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    return publication_state::visible_not_durable;
}

[[nodiscard]] inline std::expected<publication_state, file_io_error>
rename_name_at(
    int source_directory,
    const std::string& source_name,
    int destination_directory,
    const std::string& destination_name,
    const file_io_hooks& hooks,
    const regular_file_identity* expected_source = nullptr) {
    if (source_directory < 0 || destination_directory < 0
        || !valid_leaf_name(source_name)
        || !valid_leaf_name(destination_name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::publish_rename,
            EINVAL));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::publish_rename);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::rename_failed,
            file_io_operation::publish_rename,
            injected));
    }
    file_descriptor pinned_source{};
    regular_file_identity pinned_identity{};
    auto opened = open_regular_file_at(
        source_directory,
        source_name,
        O_RDONLY,
        file_io_operation::open_file,
        {});
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    pinned_source = std::move(*opened);
    auto current = capture_identity(
        pinned_source.get(), file_io_operation::stat_after, {});
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (expected_source != nullptr && *current != *expected_source) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    pinned_identity = *current;
    if (const int injected = injected_error(
            hooks, file_io_operation::stat_after);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::stat_failed,
            file_io_operation::stat_after,
            injected));
    }
    if (::renameat(
            source_directory,
            source_name.c_str(),
            destination_directory,
            destination_name.c_str()) != 0) {
        return std::unexpected(make_error(
            file_io_errc::rename_failed,
            file_io_operation::publish_rename,
            errno));
    }
    auto destination_opened = open_regular_file_at(
        destination_directory,
        destination_name,
        O_RDONLY,
        file_io_operation::open_file,
        {});
    if (!destination_opened.has_value()) {
        file_io_error error = destination_opened.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    file_descriptor destination_descriptor =
        std::move(*destination_opened);
    auto published = capture_identity(
        destination_descriptor.get(),
        file_io_operation::stat_after,
        {});
    if (!published.has_value()) {
        file_io_error error = published.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    if (!same_published_identity(*published, pinned_identity)) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0,
            0,
            publication_state::visible_not_durable));
    }
    return publication_state::visible_not_durable;
}

[[nodiscard]] inline file_io_error cleanup_error_or(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    publication_state publication,
    file_io_error primary,
    const file_io_hooks& hooks) noexcept {
    auto removed = remove_name_if_same_inode_at(
        directory_descriptor, name, expected, publication, hooks);
    if (!removed.has_value()) {
        return removed.error();
    }
    if (*removed == conditional_remove_disposition::removed) {
        return primary;
    }
    return make_error(
        file_io_errc::identity_changed,
        file_io_operation::remove_temporary,
        ESTALE,
        0,
        publication);
}

[[nodiscard]] inline std::expected<void, file_io_error>
remove_published_staging_name(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    const file_io_hooks& hooks) noexcept {
    auto removed = remove_name_if_same_inode_at(
        directory_descriptor,
        name,
        expected,
        publication_state::visible_not_durable,
        hooks);
    if (!removed.has_value()) {
        return std::unexpected(removed.error());
    }
    if (*removed == conditional_remove_disposition::removed) {
        return {};
    }
    auto synced = sync_descriptor(
        directory_descriptor,
        file_io_operation::sync_directory,
        hooks,
        publication_state::visible_not_durable);
    if (!synced.has_value()) {
        return std::unexpected(synced.error());
    }
    return std::unexpected(make_error(
        file_io_errc::identity_changed,
        file_io_operation::remove_temporary,
        ESTALE,
        0,
        publication_state::durable));
}

[[nodiscard]] inline std::expected<stable_regular_file, file_io_error>
read_stable_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const stable_read_options& options,
    const file_io_hooks& hooks,
    const positional_io_operations& operations);

}  // namespace bioseq::durable_file_io::detail
namespace bioseq::durable_file_io::detail {

struct staged_file {
    std::string name;
    regular_file_identity identity{};
};

[[nodiscard]] inline std::expected<staged_file, file_io_error> stage_bytes(
    int directory_descriptor,
    const std::string& destination_name,
    std::span<const std::byte> bytes,
    const durable_publish_options& options,
    const file_io_hooks& hooks,
    const positional_io_operations& operations) {
    if (directory_descriptor < 0
        || !valid_leaf_name(destination_name)
        || options.mode == 0
        || (options.mode & ~(S_IRWXU | S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
#if !defined(O_NOFOLLOW)
    static_cast<void>(bytes);
    static_cast<void>(hooks);
    static_cast<void>(operations);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::create_temporary,
        0));
#else
    constexpr std::size_t maximum_attempts = 64;
    std::string temporary{};
    int raw_descriptor = -1;
    for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
        temporary = unique_temporary_name(destination_name);
        if (const int injected = injected_error(
                hooks, file_io_operation::create_temporary);
            injected != 0) {
            return std::unexpected(make_error(
                file_io_errc::open_failed,
                file_io_operation::create_temporary,
                injected));
        }
        int flags = O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        raw_descriptor = ::openat(
            directory_descriptor,
            temporary.c_str(),
            flags,
            options.mode);
        if (raw_descriptor >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return std::unexpected(make_error(
                file_io_errc::open_failed,
                file_io_operation::create_temporary,
                errno));
        }
    }
    if (raw_descriptor < 0) {
        return std::unexpected(make_error(
            file_io_errc::open_failed,
            file_io_operation::create_temporary,
            EEXIST));
    }
    file_descriptor descriptor{raw_descriptor};
    auto pinned_identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, {});
    if (!pinned_identity.has_value()) {
        const file_io_error primary = pinned_identity.error();
        descriptor.reset();
        auto removed = remove_name_at(
            directory_descriptor,
            temporary,
            publication_state::unchanged,
            hooks);
        return std::unexpected(
            removed.has_value() ? primary : removed.error());
    }
    auto identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    const mode_t actual_permissions = identity.has_value()
        ? static_cast<mode_t>(
            identity->mode & (S_IRWXU | S_IRWXG | S_IRWXO))
        : 0;
    if (!identity.has_value()
        || identity->link_count != 1
        || (actual_permissions & ~options.mode) != 0) {
        const file_io_error primary = identity.has_value()
            ? make_error(
                file_io_errc::not_regular_file,
                file_io_operation::stat_before,
                0)
            : identity.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::positional_write);
        injected != 0) {
        const file_io_error primary = make_error(
            file_io_errc::write_failed,
            file_io_operation::positional_write,
            injected);
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto written = positional_write_all(
            descriptor.get(), bytes, 0, operations);
        !written.has_value()) {
        const file_io_error primary = written.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto synced = sync_descriptor(
            descriptor.get(),
            file_io_operation::sync_file,
            hooks);
        !synced.has_value()) {
        const file_io_error primary = synced.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    auto final_identity = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!final_identity.has_value()
        || !same_file_object(*identity, *final_identity)
        || final_identity->size != bytes.size()) {
        const file_io_error primary = final_identity.has_value()
            ? make_error(
                file_io_errc::identity_changed,
                file_io_operation::stat_after,
                0)
            : final_identity.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::close_file);
        injected != 0) {
        const file_io_error primary = make_error(
            file_io_errc::close_failed,
            file_io_operation::close_file,
            injected);
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            closed.error(),
            hooks));
    }
    return staged_file{
        .name = std::move(temporary),
        .identity = *final_identity,
    };
#endif
}

[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
sync_existing_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const file_io_hooks& hooks,
    const regular_file_identity* expected = nullptr) {
    auto opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    if (!identity.has_value()) {
        return std::unexpected(identity.error());
    }
    if (expected != nullptr && *identity != *expected) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_before,
            0));
    }
    if (auto synced = sync_descriptor(
            descriptor.get(), file_io_operation::sync_file, hooks);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    auto after = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!after.has_value()) {
        return std::unexpected(after.error());
    }
    if (*after != *identity) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    auto current_opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!current_opened.has_value()) {
        return std::unexpected(current_opened.error());
    }
    file_descriptor current_descriptor = std::move(*current_opened);
    auto current = capture_identity(
        current_descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    return *after;
}

[[nodiscard]] inline std::expected<void, file_io_error>
verify_staged_path(
    int directory_descriptor,
    const staged_file& staged,
    const file_io_hooks& hooks) {
    auto opened = open_regular_file_at(
        directory_descriptor,
        staged.name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != staged.identity) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    return {};
}

[[nodiscard]] inline std::expected<stable_regular_file, file_io_error>
read_stable_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const stable_read_options& options,
    const file_io_hooks& hooks,
    const positional_io_operations& operations) {
    if (directory_descriptor < 0
        || !valid_leaf_name(name)
        || options.minimum_bytes > options.maximum_bytes
        || (options.require_exact_size
            && (options.exact_bytes < options.minimum_bytes
                || options.exact_bytes > options.maximum_bytes))) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_file,
            EINVAL));
    }
    auto opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto before = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    if (!before.has_value()) {
        return std::unexpected(before.error());
    }
    if (before->size < options.minimum_bytes
        || before->size > options.maximum_bytes
        || (options.require_exact_size
            && before->size != options.exact_bytes)
        || before->size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return std::unexpected(make_error(
            before->size > options.maximum_bytes
                    || before->size
                        > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::size_t>::max)())
                ? file_io_errc::size_limit_exceeded
                : file_io_errc::invalid_file_size,
            file_io_operation::stat_before,
            0));
    }
    stable_regular_file result{};
    result.bytes.resize(static_cast<std::size_t>(before->size));
    if (auto read = positional_read_exact(
            descriptor.get(),
            0,
            std::span<std::byte>{
                result.bytes.data(), result.bytes.size()},
            operations);
        !read.has_value()) {
        return std::unexpected(read.error());
    }
    auto after = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!after.has_value()) {
        return std::unexpected(after.error());
    }
    if (*before != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    auto current_opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!current_opened.has_value()) {
        return std::unexpected(current_opened.error());
    }
    file_descriptor current_descriptor = std::move(*current_opened);
    auto current = capture_identity(
        current_descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    result.identity = *after;
    return result;
}

}  // namespace bioseq::durable_file_io::detail
