/**
 * @file common/durable_file_io/open.cppm
 * @brief Internal no-follow POSIX descriptor opening.
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

export module bioseq.common.durable_file_io:open;
import :types;
import :error;
import :path;

namespace bioseq::durable_file_io::detail {

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

}  // namespace bioseq::durable_file_io::detail
