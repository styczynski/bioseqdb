/**
 * @file common/durable_file_io/directory_operations.cppm
 * @brief Create and sync directories, and manage files through directry handles.
 */
module;

#include <cerrno>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

export module bioseq.common.durable_file_io:directory_operations;
export import :types;
import :error;
import :identity;
import :path;
import :open;
import :sync;
import :cleanup;
import :namespace_publish;

export namespace bioseq::common::durable_file_io {
/**
 * Create the relative directory path below an open anchor directory.
 * Reject absolute paths and parent traversal through "..".
 *
 * Each component is opened with O_NOFOLLOW, even if  another caller has just
 * created it. Sync the component and its parent before continuing. The anchor
 * stays open, and handles opened here are closed before success.
 *
 * mode applies to new directories, subject to umask. Existing directories
 * keep their permissions and are still checked and synced.
 */
[[nodiscard]] inline std::expected<void, file_error>
durable_create_directories_at(
    const file_descriptor& anchor,
    const std::filesystem::path& relative_directory,
    mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO,
    const fault_injection_hooks& hooks = {}) {
    if (!anchor.valid()
        || relative_directory.is_absolute()
        || !detail::valid_creation_mode(mode)
        || detail::contains_parent_traversal(relative_directory)) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::create_directory,
            EINVAL));
    }
    file_descriptor owned_current;
    int current_descriptor = anchor.get();
    publication_state completed_publication =
        publication_state::unchanged;
    const std::filesystem::path normalized =
        relative_directory.lexically_normal();
    for (const std::filesystem::path& component : normalized) {
        const std::string name = component.string();
        if (name.empty() || name == ".") {
            continue;
        }
        if (!detail::valid_leaf_name(name)) {
            return std::unexpected(detail::make_error(
                file_error_code::invalid_argument,
                file_operation::create_directory,
                EINVAL));
        }
        if (const int injected = detail::injected_errno(
                hooks, file_operation::create_directory);
            injected != 0) {
            return std::unexpected(detail::make_error(
                file_error_code::directory_create_failed,
                file_operation::create_directory,
                injected,
                0,
                completed_publication));
        }
        int create_result = -1;
        do {
            create_result = ::mkdirat(
                current_descriptor, name.c_str(), mode);
        } while (create_result != 0 && errno == EINTR);
        if (create_result != 0 && errno != EEXIST) {
            return std::unexpected(detail::make_error(
                file_error_code::directory_create_failed,
                file_operation::create_directory,
                errno,
                0,
                completed_publication));
        }
        const bool created = create_result == 0;
        const publication_state pending_publication = created
            ? publication_state::visible_not_durable
            : completed_publication;
        auto child_opened = detail::open_directory_at(
            current_descriptor, name, hooks);
        if (!child_opened.has_value()) {
            file_error error = child_opened.error();
            error.publication = pending_publication;
            return std::unexpected(error);
        }
        file_descriptor child = std::move(*child_opened);
        if (auto synced = detail::fsync_descriptor(
                child.get(),
                file_operation::fsync_directory,
                hooks,
                pending_publication);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (auto synced = detail::fsync_descriptor(
                current_descriptor,
                file_operation::fsync_directory,
                hooks,
                pending_publication);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (created) {
            completed_publication = publication_state::durable;
        }
        if (auto closed = detail::close_directory_descriptor(
                owned_current, hooks, completed_publication);
            !closed.has_value()) {
            return std::unexpected(closed.error());
        }
        owned_current = std::move(child);
        current_descriptor = owned_current.get();
    }
    return detail::close_directory_descriptor(
        owned_current, hooks, completed_publication);
}

/**
 * Create missing directories beneath the deepest existing directory.
 * Keep that directory open while creating the missing path components, and
 * reject ".." traversal or a symlink at the boundary or in the new suffix
 *
 * Earlier path components may use trusted platform aliases such as Darwin's
 * /var symlink. Each opened suffix  directory and its parent are synced, even
 * if another caller created the directory, first. mode applies only to newly
 * created directories, subject to umask.
 */
[[nodiscard]] inline std::expected<void, file_error>
durable_create_directories(
    const std::filesystem::path& directory,
    mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO,
    const fault_injection_hooks& hooks = {}) {
    if (directory.empty()
        || !detail::valid_creation_mode(mode)
        || detail::contains_parent_traversal(directory)) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::create_directory,
            EINVAL));
    }

    std::filesystem::path candidate =
        detail::directory_leaf_path(directory).lexically_normal();
    std::vector<std::string> missing_components;
    auto requested_opened =
        detail::open_directory_path(candidate, hooks);
    if (requested_opened.has_value()) {
        file_descriptor requested = std::move(*requested_opened);
        auto parent_opened = detail::open_parent_directory_at(
            requested.get(), hooks);
        if (!parent_opened.has_value()) {
            return std::unexpected(parent_opened.error());
        }
        file_descriptor actual_parent = std::move(*parent_opened);
        if (auto synced = detail::fsync_descriptor(
                requested.get(),
                file_operation::fsync_directory,
                hooks,
                publication_state::unchanged);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (auto synced = detail::fsync_descriptor(
                actual_parent.get(),
                file_operation::fsync_directory,
                hooks,
                publication_state::unchanged);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (auto closed = detail::close_directory_descriptor(
                actual_parent, hooks, publication_state::durable);
            !closed.has_value()) {
            return std::unexpected(closed.error());
        }
        return detail::close_directory_descriptor(
            requested, hooks, publication_state::durable);
    }
    if (requested_opened.error().system_error != ENOENT) {
        return std::unexpected(requested_opened.error());
    }
    const std::string requested_name = candidate.filename().string();
    if (!detail::valid_leaf_name(requested_name)) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::create_directory,
            EINVAL));
    }
    missing_components.push_back(requested_name);
    const std::filesystem::path requested_parent =
        candidate.parent_path();
    candidate = requested_parent.empty()
        ? std::filesystem::path{"."}
        : requested_parent;
    std::expected<file_descriptor, file_error> anchor =
        std::unexpected(detail::make_error(
            file_error_code::directory_open_failed,
            file_operation::open_directory,
            ENOENT));
    for (;;) {
        anchor = detail::open_directory_path(candidate, hooks);
        if (anchor.has_value()) {
            break;
        }
        if (anchor.error().system_error != ENOENT) {
            return std::unexpected(anchor.error());
        }

        const std::string name = candidate.filename().string();
        if (!detail::valid_leaf_name(name)) {
            return std::unexpected(anchor.error());
        }
        missing_components.push_back(name);
        const std::filesystem::path parent = candidate.parent_path();
        candidate = parent.empty()
            ? std::filesystem::path{"."}
            : parent;
    }

    std::filesystem::path relative_suffix;
    for (auto component = missing_components.rbegin();
         component != missing_components.rend();
         ++component) {
        relative_suffix /= *component;
    }
    auto created = durable_create_directories_at(
        *anchor, relative_suffix, mode, hooks);
    if (!created.has_value()) {
        return std::unexpected(created.error());
    }
    return detail::close_directory_descriptor(
        *anchor, hooks, publication_state::durable);
}

/** Open a directory for lfter operations, rejecting a symlink at its final name. */
[[nodiscard]] inline std::expected<file_descriptor, file_error>
open_directory_descriptor(
    const std::filesystem::path& directory,
    const fault_injection_hooks& hooks = {}) {
    return detail::open_directory_path(directory, hooks);
}

/** Sync an open directory and report any error. */
[[nodiscard]] inline std::expected<void, file_error>
fsync_directory_descriptor(
    int descriptor,
    const fault_injection_hooks& hooks = {},
    publication_state publication = publication_state::unchanged) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::fsync_directory,
            EBADF,
            0,
            publication));
    }
    return detail::fsync_descriptor(
        descriptor,
        file_operation::fsync_directory,
        hooks,
        publication);
}

/** Sync an open file, retrying if a signal interrupts fsync. */
[[nodiscard]] inline std::expected<void, file_error>
fsync_file_descriptor(
    int descriptor,
    const fault_injection_hooks& hooks = {}) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::fsync_file,
            EBADF));
    }
    return detail::fsync_descriptor(
        descriptor, file_operation::fsync_file, hooks);
}

/**
 * Add a hard link at destination, failing if that name already exists.
 * The source must already be synced. After linking, the caller must remove
 * its temporary name and sync  the affected directories before reporting a
 * durable publication.
 */
[[nodiscard]] inline std::expected<publication_state, file_error>
link_file_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const fault_injection_hooks& hooks = {}) {
    const std::string source_name = detail::leaf_name(source);
    const std::string destination_name = detail::leaf_name(destination);
    if (source.empty() || destination.empty()
        || !detail::valid_leaf_name(source_name)
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::link_destination,
            EINVAL));
    }
    const std::filesystem::path source_parent =
        detail::parent_directory(source);
    const std::filesystem::path destination_parent =
        detail::parent_directory(destination);
    auto source_opened = detail::open_directory_path(
        source_parent, hooks);
    if (!source_opened.has_value()) {
        return std::unexpected(source_opened.error());
    }
    file_descriptor source_directory = std::move(*source_opened);
    file_descriptor destination_directory{};
    int destination_descriptor = source_directory.get();
    if (source_parent.lexically_normal()
            != destination_parent.lexically_normal()) {
        auto destination_opened = detail::open_directory_path(
            destination_parent, hooks);
        if (!destination_opened.has_value()) {
            return std::unexpected(destination_opened.error());
        }
        destination_directory = std::move(*destination_opened);
        destination_descriptor = destination_directory.get();
    }
    return detail::link_name_no_replace_at(
            source_directory.get(),
            source_name,
            destination_descriptor,
            destination_name,
            hooks);
}

/**
 * Add a hard link using open directory handles, failing if destination exists.
 * When expected_source is supplied, check it against the source file first.
 * The caller still needs to sync the affected directories.
 */
[[nodiscard]] inline std::expected<publication_state, file_error>
link_file_no_replace_at(
    int source_directory,
    std::string_view source_name,
    int destination_directory,
    std::string_view destination_name,
    const fault_injection_hooks& hooks = {},
    const regular_file_identity* expected_source = nullptr) {
    return detail::link_name_no_replace_at(
        source_directory,
        std::string{source_name},
        destination_directory,
        std::string{destination_name},
        hooks,
        expected_source);
}

/**
 * Remove the name if it still identifies expected's inode.
 * The caller must prevent other writers from replacing it between the check
 * and removal.
 */
[[nodiscard]] inline std::expected<
    conditional_remove_disposition,
    file_error>
remove_file_if_same_inode_at(
    int directory_descriptor,
    std::string_view name,
    const regular_file_identity& expected,
    publication_state publication = publication_state::unchanged,
    const fault_injection_hooks& hooks = {}) {
    return detail::remove_name_if_same_inode_at(
        directory_descriptor,
        std::string{name},
        expected,
        publication,
        hooks);
}

/** Read regular-file metadata for a name in an open directory */
[[nodiscard]] inline std::expected<regular_file_identity, file_error>
capture_regular_file_identity_at(
    int directory_descriptor,
    std::string_view name,
    const fault_injection_hooks& hooks = {}) {
    const std::string owned_name{name};
    auto opened = detail::open_regular_file_at(
        directory_descriptor,
        owned_name,
        O_RDONLY,
        file_operation::open_regular_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = detail::capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_before_io, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    return *current;
}

/** Compare a file in an open directory with previously captured metadata. */
[[nodiscard]] inline std::expected<bool, file_error>
matches_regular_file_identity_at(
    int directory_descriptor,
    std::string_view name,
    const regular_file_identity& expected,
    const fault_injection_hooks& hooks = {}) {
    const std::string owned_name{name};
    auto opened = detail::open_regular_file_at(
        directory_descriptor,
        owned_name,
        O_RDONLY,
        file_operation::open_regular_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = detail::capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_after_io, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    return *current == expected;
}

}  // namespace bioseq::common::durable_file_io
