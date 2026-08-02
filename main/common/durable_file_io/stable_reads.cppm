/**
 * @file common/durable_file_io/stable_reads.cppm
 * @brief Stable no-follow reads and file/directory synchronization.
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

export module bioseq.common.durable_file_io:stable_reads;
export import :types;
import :error;
import :identity;
import :path;
import :open;
import :sync;
import :positional_io;
import :stable_read_at;

export namespace bioseq::durable_file_io {
/**
 * Read a regular file without following its final symlink and prove its
 * identity and size did not change between the before/after fstat snapshots.
 */
[[nodiscard]] inline std::expected<stable_regular_file, file_io_error>
read_stable_regular_file(
    const std::filesystem::path& path,
    const stable_read_options& options = {},
    const file_io_hooks& hooks = {},
    const positional_io_operations& operations = {}) {
    if (path.empty()) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_file,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(path);
    const std::string name = detail::leaf_name(path);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    return detail::read_stable_regular_file_at(
        directory.get(), name, options, hooks, operations);
}

/** Check a path against a captured identity without following a symlink. */
[[nodiscard]] inline std::expected<bool, file_io_error>
same_regular_file_identity(
    const std::filesystem::path& path,
    const regular_file_identity& expected,
    const file_io_hooks& hooks = {}) {
    if (path.empty()) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_file,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(path);
    const std::string name = detail::leaf_name(path);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto opened = detail::open_regular_file_at(
        directory.get(),
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = detail::capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    return *current == expected;
}

/** Fsync one directory path without following its final symlink. */
[[nodiscard]] inline std::expected<void, file_io_error> fsync_directory(
    const std::filesystem::path& directory,
    const file_io_hooks& hooks = {},
    publication_state publication = publication_state::unchanged) {
    if (directory.empty()) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_directory,
            EINVAL,
            0,
            publication));
    }
    auto opened = detail::open_directory_path(
        directory, hooks, publication);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    if (auto synced = detail::sync_descriptor(
            descriptor.get(),
            file_io_operation::sync_directory,
            hooks,
            publication);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    return detail::close_directory_descriptor(
        descriptor, hooks, publication);
}

}  // namespace bioseq::durable_file_io
