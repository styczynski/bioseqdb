/**
 * @file common/durable_file_io/cleanup.cppm
 * @brief Internal POSIX cleanup with inode-conditional removal.
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

export module bioseq.common.durable_file_io:cleanup;
import :types;
import :error;
import :identity;
import :open;
import :sync;
import :path;

namespace bioseq::durable_file_io::detail {

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

}  // namespace bioseq::durable_file_io::detail
