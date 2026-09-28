/**
 * @file common/durable_file_io/publication.cppm
 * @brief Publish file contents and sync the files and directory changes.
 */
module;

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <sys/stat.h>
#include <utility>

export module bioseq.common.durable_file_io:publication;
export import :types;
import :error;
import :path;
import :open;
import :sync;
import :staging;
import :stable_read_at;
import :cleanup;
import :namespace_publish;

namespace bioseq::common::durable_file_io::detail {

/** Remove the temporary name and sync the directory to finish  publication */
[[nodiscard]] inline std::expected<publication_result, file_error>
complete_staging_publication(
    int directory_descriptor,
    const staged_regular_file& staged,
    publication_disposition disposition,
    const fault_injection_hooks& hooks) {
    if (auto removed = remove_published_staging_name(
            directory_descriptor,
            staged.name,
            staged.identity,
            hooks);
        !removed.has_value()) {
        return std::unexpected(removed.error());
    }
    if (auto synced = fsync_descriptor(
            directory_descriptor,
            file_operation::fsync_directory,
            hooks,
            publication_state::visible_not_durable);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    return publication_result{
        .publication = publication_state::durable,
        .disposition = disposition,
    };
}

}  // namespace bioseq::common::durable_file_io::detail

export namespace bioseq::common::durable_file_io {

/**
 * Create the destination, or accept an existing file with the same bytes.
 * For an existing file, reject a final symlink and check that its metadata stays
 * unchanged while reading. Its permissions must be a subset of options.mode.
 * Sync the fipe and its parent directory before returning already_present_identical.
 */
[[nodiscard]] inline std::expected<publication_result, file_error>
durable_create_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    const publication_options& options = {},
    const fault_injection_hooks& hooks = {},
    const positional_io_callbacks& callbacks = {}) {
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::create_staging_file,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_verified_regular_file(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        callbacks);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    const std::string staging_name = staged->name;
    if (auto linked = detail::link_name_no_replace_at(
            directory.get(),
            staging_name,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !linked.has_value()) {
        if (linked.error().code != file_error_code::destination_exists) {
            return std::unexpected(detail::staging_cleanup_error(
                directory.get(),
                staging_name,
                staged->identity,
                linked.error().publication,
                linked.error(),
                hooks));
        }
        const stable_read_options read_options{
            .minimum_bytes = bytes.size(),
            .maximum_bytes = bytes.size(),
            .exact_bytes = bytes.size(),
            .require_exact_size = true,
        };
        auto existing = detail::read_stable_regular_file_at(
            directory.get(),
            destination_name,
            read_options,
            hooks,
            callbacks);
        if (!existing.has_value()) {
            return std::unexpected(detail::staging_cleanup_error(
                directory.get(),
                staging_name,
                staged->identity,
                publication_state::unchanged,
                existing.error(),
                hooks));
        }
        constexpr std::uint64_t controlled_permissions =
            S_IRWXU | S_IRWXG | S_IRWXO
            | S_ISUID | S_ISGID | S_ISVTX;
        const std::uint64_t existing_permissions =
            existing->identity.mode & controlled_permissions;
        if ((existing_permissions
             & ~static_cast<std::uint64_t>(options.mode)) != 0) {
            return std::unexpected(detail::staging_cleanup_error(
                directory.get(),
                staging_name,
                staged->identity,
                publication_state::unchanged,
                detail::make_error(
                    file_error_code::permission_mismatch,
                    file_operation::inspect_after_io,
                    EACCES),
                hooks));
        }
        if (existing->bytes.size() != bytes.size()
            || !std::equal(
                existing->bytes.begin(), existing->bytes.end(),
                bytes.begin(), bytes.end())) {
            return std::unexpected(detail::staging_cleanup_error(
                directory.get(),
                staging_name,
                staged->identity,
                publication_state::unchanged,
                detail::make_error(
                    file_error_code::content_mismatch,
                    file_operation::link_destination,
                    EEXIST),
                hooks));
        }
        if (auto synced = detail::sync_existing_regular_file_at(
                directory.get(),
                destination_name,
                hooks,
                &existing->identity);
            !synced.has_value()) {
            return std::unexpected(detail::staging_cleanup_error(
                directory.get(),
                staging_name,
                staged->identity,
                publication_state::unchanged,
                synced.error(),
                hooks));
        }
        return detail::complete_staging_publication(
            directory.get(),
            *staged,
            publication_disposition::already_present_identical,
            hooks);
    }
    return detail::complete_staging_publication(
        directory.get(),
        *staged,
        publication_disposition::created,
        hooks);
}

/**
 * Write the bytes to a temporary file, then atomically replace the destination.
 * Sync the file before the rename and its directory afterward.
 */
[[nodiscard]] inline std::expected<publication_result, file_error>
durable_replace_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    const publication_options& options = {},
    const fault_injection_hooks& hooks = {},
    const positional_io_callbacks& callbacks = {}) {
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::create_staging_file,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_verified_regular_file(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        callbacks);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    const std::string staging_name = staged->name;
    if (auto renamed = detail::rename_name_at(
            directory.get(),
            staging_name,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !renamed.has_value()) {
        return std::unexpected(detail::staging_cleanup_error(
            directory.get(),
            staging_name,
            staged->identity,
            renamed.error().publication,
            renamed.error(),
            hooks));
    }
    if (auto directory_synced = detail::fsync_descriptor(
            directory.get(),
            file_operation::fsync_directory,
            hooks,
            publication_state::visible_not_durable);
        !directory_synced.has_value()) {
        return std::unexpected(directory_synced.error());
    }
    return publication_result{
        .publication = publication_state::durable,
        .disposition = publication_disposition::replaced,
    };
}

/**
 * Publish bytes according  to the chosen replacement policy.
 * exclusive_no_replace fails whenever the destination exists, even when its
 * contents match. Use durable_create_file if an identical existing file should
 * count as success.
 */
[[nodiscard]] inline std::expected<publication_result, file_error>
durable_publish_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    publication_policy policy,
    const publication_options& options = {},
    const fault_injection_hooks& hooks = {},
    const positional_io_callbacks& callbacks = {}) {
    if (policy == publication_policy::replace_existing) {
        return durable_replace_file(
            destination, bytes, options, hooks, callbacks);
    }
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::create_staging_file,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_verified_regular_file(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        callbacks);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    const std::string staging_name = staged->name;
    if (auto linked = detail::link_name_no_replace_at(
            directory.get(),
            staging_name,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !linked.has_value()) {
        return std::unexpected(detail::staging_cleanup_error(
            directory.get(),
            staging_name,
            staged->identity,
            linked.error().publication,
            linked.error(),
            hooks));
    }
    return detail::complete_staging_publication(
        directory.get(),
        *staged,
        publication_disposition::created,
        hooks);
}

/**
 * Sync a regular file, then atomically rename it.
 * Sync both affected directories when they differ, or the one shared directory.
 */
[[nodiscard]] inline std::expected<publication_result, file_error>
durable_rename_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const fault_injection_hooks& hooks = {}) {
    const std::filesystem::path source_parent =
        detail::parent_directory(source);
    const std::filesystem::path destination_parent =
        detail::parent_directory(destination);
    const std::string source_name = detail::leaf_name(source);
    const std::string destination_name =
        detail::leaf_name(destination);
    if (source.empty() || destination.empty()
        || !detail::valid_leaf_name(source_name)
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::rename_destination,
            EINVAL));
    }
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
    auto source_identity = detail::sync_existing_regular_file_at(
        source_directory.get(), source_name, hooks);
    if (!source_identity.has_value()) {
        return std::unexpected(source_identity.error());
    }
    if (auto renamed = detail::rename_name_at(
            source_directory.get(),
            source_name,
            destination_descriptor,
            destination_name,
            hooks,
            &*source_identity);
        !renamed.has_value()) {
        return std::unexpected(renamed.error());
    }
    if (auto synced = detail::fsync_descriptor(
            destination_descriptor,
            file_operation::fsync_directory,
            hooks,
            publication_state::visible_not_durable);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    if (source_parent.lexically_normal()
            != destination_parent.lexically_normal()) {
        if (auto synced = detail::fsync_descriptor(
                source_directory.get(),
                file_operation::fsync_directory,
                hooks,
                publication_state::visible_not_durable);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
    }
    return publication_result{
        .publication = publication_state::durable,
        .disposition = publication_disposition::renamed,
    };
}

}  // namespace bioseq::common::durable_file_io
