/**
 * @file common/durable_file_io/directory_operations.cppm
 * @brief Durable directory creation and conditional removal operations.
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

export module bioseq.common.durable_file_io:directory_operations;
export import :types;
import :detail;

export namespace bioseq::durable_file_io {
/**
 * Create and durably publish a relative directory chain below a pinned anchor.
 *
 * The anchor itself is never reopened by pathname. Every suffix component is
 * opened with O_NOFOLLOW after mkdirat, including an EEXIST result from a
 * concurrent cooperating creator. Descriptors opened by this function are
 * explicitly closed before success; the caller-owned anchor remains open.
 * Absolute paths and `..` traversal are rejected.
 *
 * `mode` is the maximum requested permission set only for components created
 * by this call; the process umask may remove bits. Existing components are
 * type/symlink validated and durability-synced but are not chmod'd.
 */
[[nodiscard]] inline std::expected<void, file_io_error>
durable_create_directories_at(
    const file_descriptor& anchor,
    const std::filesystem::path& relative_directory,
    mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO,
    const file_io_hooks& hooks = {}) {
    if (!anchor.valid()
        || relative_directory.is_absolute()
        || mode == 0
        || (mode & ~(S_IRWXU | S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_directory,
            EINVAL));
    }
    for (const std::filesystem::path& component : relative_directory) {
        if (component == "..") {
            return std::unexpected(detail::make_error(
                file_io_errc::invalid_argument,
                file_io_operation::create_directory,
                EINVAL));
        }
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
                file_io_errc::invalid_argument,
                file_io_operation::create_directory,
                EINVAL));
        }
        if (const int injected = detail::injected_error(
                hooks, file_io_operation::create_directory);
            injected != 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::directory_create_failed,
                file_io_operation::create_directory,
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
                file_io_errc::directory_create_failed,
                file_io_operation::create_directory,
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
            file_io_error error = child_opened.error();
            error.publication = pending_publication;
            return std::unexpected(error);
        }
        file_descriptor child = std::move(*child_opened);
        if (auto synced = detail::sync_descriptor(
                child.get(),
                file_io_operation::sync_directory,
                hooks,
                pending_publication);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (auto synced = detail::sync_descriptor(
                current_descriptor,
                file_io_operation::sync_directory,
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
 * Create a directory chain below its deepest existing, non-symlink leaf.
 *
 * The anchor pathname may contain trusted intermediate platform aliases (for
 * example Darwin's `/var -> private/var`), but its final component is opened
 * with O_NOFOLLOW and pinned. Only the missing relative suffix is then passed
 * to `durable_create_directories_at`, so an existing symlink at the boundary or
 * a symlink introduced into the suffix is rejected. Every opened suffix
 * component and its parent are fsynced, including EEXIST components; `mode`
 * applies only to components this call creates. `..` traversal is rejected.
 */
[[nodiscard]] inline std::expected<void, file_io_error>
durable_create_directories(
    const std::filesystem::path& directory,
    mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO,
    const file_io_hooks& hooks = {}) {
    if (directory.empty()
        || mode == 0
        || (mode & ~(S_IRWXU | S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_directory,
            EINVAL));
    }
    for (const std::filesystem::path& component : directory) {
        if (component == "..") {
            return std::unexpected(detail::make_error(
                file_io_errc::invalid_argument,
                file_io_operation::create_directory,
                EINVAL));
        }
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
        if (auto synced = detail::sync_descriptor(
                requested.get(),
                file_io_operation::sync_directory,
                hooks,
                publication_state::unchanged);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (auto synced = detail::sync_descriptor(
                actual_parent.get(),
                file_io_operation::sync_directory,
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
            file_io_errc::invalid_argument,
            file_io_operation::create_directory,
            EINVAL));
    }
    missing_components.push_back(requested_name);
    const std::filesystem::path requested_parent =
        candidate.parent_path();
    candidate = requested_parent.empty()
        ? std::filesystem::path{"."}
        : requested_parent;
    std::expected<file_descriptor, file_io_error> anchor =
        std::unexpected(detail::make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
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

/** Open and pin a no-follow directory for subsequent *at operations. */
[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_directory_descriptor(
    const std::filesystem::path& directory,
    const file_io_hooks& hooks = {}) {
    return detail::open_directory_path(directory, hooks);
}

/** Fsync an already-open pinned directory descriptor. */
[[nodiscard]] inline std::expected<void, file_io_error>
fsync_directory_descriptor(
    int descriptor,
    const file_io_hooks& hooks = {},
    publication_state publication = publication_state::unchanged) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::sync_directory,
            EBADF,
            0,
            publication));
    }
    return detail::sync_descriptor(
        descriptor,
        file_io_operation::sync_directory,
        hooks,
        publication);
}

/** Fsync an already-open file descriptor with typed EINTR handling. */
[[nodiscard]] inline std::expected<void, file_io_error>
fsync_file_descriptor(
    int descriptor,
    const file_io_hooks& hooks = {}) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::sync_file,
            EBADF));
    }
    return detail::sync_descriptor(
        descriptor, file_io_operation::sync_file, hooks);
}

/**
 * Atomically add a no-replace hard-link publication.
 *
 * This is the namespace step only. The source must already be fsynced and the
 * caller must unlink its staging name and fsync the containing directory
 * before treating the publication as durable.
 */
[[nodiscard]] inline std::expected<publication_state, file_io_error>
link_file_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const file_io_hooks& hooks = {}) {
    const std::string source_name = detail::leaf_name(source);
    const std::string destination_name = detail::leaf_name(destination);
    if (source.empty() || destination.empty()
        || !detail::valid_leaf_name(source_name)
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::publish_link,
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

/** No-replace hard-link publication relative to pinned directory handles. */
[[nodiscard]] inline std::expected<publication_state, file_io_error>
link_file_no_replace_at(
    int source_directory,
    std::string_view source_name,
    int destination_directory,
    std::string_view destination_name,
    const file_io_hooks& hooks = {},
    const regular_file_identity* expected_source = nullptr) {
    return detail::link_name_no_replace_at(
        source_directory,
        std::string{source_name},
        destination_directory,
        std::string{destination_name},
        hooks,
        expected_source);
}

/** Remove a pinned-directory entry only if it still names `expected`. */
[[nodiscard]] inline std::expected<
    conditional_remove_disposition,
    file_io_error>
remove_file_if_same_inode_at(
    int directory_descriptor,
    std::string_view name,
    const regular_file_identity& expected,
    publication_state publication = publication_state::unchanged,
    const file_io_hooks& hooks = {}) {
    return detail::remove_name_if_same_inode_at(
        directory_descriptor,
        std::string{name},
        expected,
        publication,
        hooks);
}

/** Capture one regular entry relative to a pinned directory descriptor. */
[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
capture_regular_file_identity_at(
    int directory_descriptor,
    std::string_view name,
    const file_io_hooks& hooks = {}) {
    const std::string owned_name{name};
    auto opened = detail::open_regular_file_at(
        directory_descriptor,
        owned_name,
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
    return *current;
}

/** Compare one pinned-directory entry with a captured regular identity. */
[[nodiscard]] inline std::expected<bool, file_io_error>
same_regular_file_identity_at(
    int directory_descriptor,
    std::string_view name,
    const regular_file_identity& expected,
    const file_io_hooks& hooks = {}) {
    const std::string owned_name{name};
    auto opened = detail::open_regular_file_at(
        directory_descriptor,
        owned_name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = detail::capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    return *current == expected;
}

}  // namespace bioseq::durable_file_io
