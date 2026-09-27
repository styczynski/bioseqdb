/**
 * @file common/durable_file_io/cleanup.cppm
 * @brief Remove temporary file names after checking which file they refer to.
 */
module;

#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

export module bioseq.common.durable_file_io:cleanup;
import :types;
import :error;
import :identity;
import :sync;
import :path;

namespace bioseq::common::durable_file_io::detail {

[[nodiscard]] inline std::expected<void, file_error> remove_name_at(
    int directory_descriptor,
    const std::string& name,
    publication_state publication,
    const fault_injection_hooks& hooks = {}) noexcept {
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_error_code::invalid_argument,
            file_operation::remove_staging_file,
            EINVAL,
            0,
            publication));
    }
    if (const int injected = injected_errno(
            hooks, file_operation::remove_staging_file);
        injected != 0) {
        return std::unexpected(make_error(
            file_error_code::cleanup_failed,
            file_operation::remove_staging_file,
            injected,
            0,
            publication));
    }
    if (::unlinkat(directory_descriptor, name.c_str(), 0) == 0
        || errno == ENOENT) {
        return {};
    }
    return std::unexpected(make_error(
        file_error_code::cleanup_failed,
        file_operation::remove_staging_file,
        errno,
        0,
        publication));
}

/**
 * Remove a temporary name if it still refers to the expected inode.
 * The test hook runs before the last identity check. Callers must keep other
 * writers from changing the name between fstatat and unlinkat, since those
 * checks and removal are separate operations.
 */
[[nodiscard]] inline std::expected<
    conditional_remove_disposition,
    file_error>
remove_name_if_same_inode_at(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    publication_state publication,
    const fault_injection_hooks& hooks = {}) noexcept {
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_error_code::invalid_argument,
            file_operation::remove_staging_file,
            EINVAL,
            0,
            publication));
    }
    if (const int injected = injected_errno(
            hooks, file_operation::remove_staging_file);
        injected != 0) {
        return std::unexpected(make_error(
            file_error_code::cleanup_failed,
            file_operation::remove_staging_file,
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
            file_error_code::cleanup_failed,
            file_operation::remove_staging_file,
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
/** Keep the original error only if the temporary name was successfully removed. */
[[nodiscard]] inline file_error staging_cleanup_error(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    publication_state publication,
    file_error primary,
    const fault_injection_hooks& hooks) noexcept {
    auto removed = remove_name_if_same_inode_at(
        directory_descriptor, name, expected, publication, hooks);
    if (!removed.has_value()) {
        return removed.error();
    }
    if (*removed == conditional_remove_disposition::removed) {
        return primary;
    }
    return make_error(
        file_error_code::identity_changed,
        file_operation::remove_staging_file,
        ESTALE,
        0,
        publication);
}

[[nodiscard]] inline std::expected<void, file_error>
remove_published_staging_name(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    const fault_injection_hooks& hooks) noexcept {
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
    auto synced = fsync_descriptor(
        directory_descriptor,
        file_operation::fsync_directory,
        hooks,
        publication_state::visible_not_durable);
    if (!synced.has_value()) {
        return std::unexpected(synced.error());
    }
    return std::unexpected(make_error(
        file_error_code::identity_changed,
        file_operation::remove_staging_file,
        ESTALE,
        0,
        publication_state::durable));
}

}  // namespace bioseq::common::durable_file_io::detail
