/**
 * @file common/durable_file_io/publication.cppm
 * @brief Atomic durable file creation, replacement, publication, and rename.
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

export module bioseq.common.durable_file_io:publication;
export import :types;
import :detail;
import :positional_io;
import :stable_reads;
import :directory_operations;
export namespace bioseq::durable_file_io {

/**
 * Publish bytes without replacing an existing destination.
 *
 * An existing destination is accepted only when a stable no-follow read is
 * byte-identical and its permission bits are no broader than `options.mode`;
 * that inode and its parent directory are then fsynced before returning
 * `already_present_identical`.
 */
[[nodiscard]] inline std::expected<durable_publish_result, file_io_error>
durable_create_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    const durable_publish_options& options = {},
    const file_io_hooks& hooks = {},
    const positional_io_operations& operations = {}) {
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_bytes(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        operations);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    if (auto verified = detail::verify_staged_path(
            directory.get(), *staged, hooks);
        !verified.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            staged->name,
            staged->identity,
            verified.error().publication,
            verified.error(),
            hooks));
    }
    const std::string temporary = staged->name;
    if (auto linked = detail::link_name_no_replace_at(
            directory.get(),
            temporary,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !linked.has_value()) {
        if (linked.error().code != file_io_errc::destination_exists) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
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
            operations);
        if (!existing.has_value()) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
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
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
                staged->identity,
                publication_state::unchanged,
                detail::make_error(
                    file_io_errc::permission_mismatch,
                    file_io_operation::stat_after,
                    EACCES),
                hooks));
        }
        if (existing->bytes.size() != bytes.size()
            || !std::equal(
                existing->bytes.begin(), existing->bytes.end(),
                bytes.begin(), bytes.end())) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
                staged->identity,
                publication_state::unchanged,
                detail::make_error(
                    file_io_errc::content_mismatch,
                    file_io_operation::publish_link,
                    EEXIST),
                hooks));
        }
        if (auto synced = detail::sync_existing_regular_file_at(
                directory.get(),
                destination_name,
                hooks,
                &existing->identity);
            !synced.has_value()) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
                staged->identity,
                publication_state::unchanged,
                synced.error(),
                hooks));
        }
        if (auto removed = detail::remove_published_staging_name(
                directory.get(),
                temporary,
                staged->identity,
                hooks);
            !removed.has_value()) {
            return std::unexpected(removed.error());
        }
        if (auto directory_synced = fsync_directory_descriptor(
                directory.get(),
                hooks,
                publication_state::visible_not_durable);
            !directory_synced.has_value()) {
            return std::unexpected(directory_synced.error());
        }
        return durable_publish_result{
            .publication = publication_state::durable,
            .disposition =
                durable_create_disposition::already_present_identical,
        };
    }
    if (auto removed = detail::remove_published_staging_name(
            directory.get(),
            temporary,
            staged->identity,
            hooks);
        !removed.has_value()) {
        return std::unexpected(removed.error());
    }
    if (auto directory_synced = fsync_directory_descriptor(
            directory.get(),
            hooks,
            publication_state::visible_not_durable);
        !directory_synced.has_value()) {
        return std::unexpected(directory_synced.error());
    }
    return durable_publish_result{
        .publication = publication_state::durable,
        .disposition = durable_create_disposition::created,
    };
}

/** Atomically replace a destination and durably publish its directory entry. */
[[nodiscard]] inline std::expected<durable_publish_result, file_io_error>
durable_replace_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    const durable_publish_options& options = {},
    const file_io_hooks& hooks = {},
    const positional_io_operations& operations = {}) {
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_bytes(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        operations);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    if (auto verified = detail::verify_staged_path(
            directory.get(), *staged, hooks);
        !verified.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            staged->name,
            staged->identity,
            verified.error().publication,
            verified.error(),
            hooks));
    }
    const std::string temporary = staged->name;
    if (auto renamed = detail::rename_name_at(
            directory.get(),
            temporary,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !renamed.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            temporary,
            staged->identity,
            renamed.error().publication,
            renamed.error(),
            hooks));
    }
    if (auto directory_synced = fsync_directory_descriptor(
            directory.get(),
            hooks,
            publication_state::visible_not_durable);
        !directory_synced.has_value()) {
        return std::unexpected(directory_synced.error());
    }
    return durable_publish_result{
        .publication = publication_state::durable,
        .disposition = durable_create_disposition::replaced,
    };
}

/**
 * Publish bytes with an explicit namespace policy.
 *
 * `exclusive_no_replace` never accepts or alters an existing destination.
 * Callers which need idempotent byte-identical reconciliation may use
 * `durable_create_file` explicitly after deciding that policy is appropriate.
 */
[[nodiscard]] inline std::expected<durable_publish_result, file_io_error>
durable_publish_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    publish_policy policy,
    const durable_publish_options& options = {},
    const file_io_hooks& hooks = {},
    const positional_io_operations& operations = {}) {
    if (policy == publish_policy::replace_existing) {
        return durable_replace_file(
            destination, bytes, options, hooks, operations);
    }
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_bytes(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        operations);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    if (auto verified = detail::verify_staged_path(
            directory.get(), *staged, hooks);
        !verified.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            staged->name,
            staged->identity,
            verified.error().publication,
            verified.error(),
            hooks));
    }
    const std::string temporary = staged->name;
    if (auto linked = detail::link_name_no_replace_at(
            directory.get(),
            temporary,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !linked.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            temporary,
            staged->identity,
            linked.error().publication,
            linked.error(),
            hooks));
    }
    if (auto removed = detail::remove_published_staging_name(
            directory.get(),
            temporary,
            staged->identity,
            hooks);
        !removed.has_value()) {
        return std::unexpected(removed.error());
    }
    if (auto directory_synced = fsync_directory_descriptor(
            directory.get(),
            hooks,
            publication_state::visible_not_durable);
        !directory_synced.has_value()) {
        return std::unexpected(directory_synced.error());
    }
    return durable_publish_result{
        .publication = publication_state::durable,
        .disposition = durable_create_disposition::created,
    };
}

/**
 * Fsync and atomically rename an existing regular file, then fsync every
 * directory whose namespace changed.
 */
[[nodiscard]] inline std::expected<durable_publish_result, file_io_error>
durable_rename_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const file_io_hooks& hooks = {}) {
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
            file_io_errc::invalid_argument,
            file_io_operation::publish_rename,
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
    if (auto synced = fsync_directory_descriptor(
            destination_descriptor,
            hooks,
            publication_state::visible_not_durable);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    if (source_parent.lexically_normal()
            != destination_parent.lexically_normal()) {
        if (auto synced = fsync_directory_descriptor(
                source_directory.get(),
                hooks,
                publication_state::visible_not_durable);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
    }
    return durable_publish_result{
        .publication = publication_state::durable,
        .disposition = durable_create_disposition::renamed,
    };
}

}  // namespace bioseq::durable_file_io
