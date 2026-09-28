/**
 * @file common/durable_file_io/namespace_publish.cppm
 * @brief Publish a file name by hard link or renme, then check the resulting file.
 */
module;

#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <utility>

export module bioseq.common.durable_file_io:namespace_publish;
import :types;
import :error;
import :identity;
import :open;
import :sync;
import :path;
import :cleanup;

namespace bioseq::common::durable_file_io::detail {

[[nodiscard]] inline std::expected<publication_state, file_error>
link_name_no_replace_at(
    int source_directory,
    const std::string& source_name,
    int destination_directory,
    const std::string& destination_name,
    const fault_injection_hooks& hooks,
    const regular_file_identity* expected_source = nullptr) {
    if (source_directory < 0 || destination_directory < 0
        || !valid_leaf_name(source_name)
        || !valid_leaf_name(destination_name)) {
        return std::unexpected(make_error(
            file_error_code::invalid_argument,
            file_operation::link_destination,
            EINVAL));
    }
    if (const int injected = injected_errno(
            hooks, file_operation::link_destination);
        injected != 0) {
        return std::unexpected(make_error(
            injected == EEXIST
                ? file_error_code::destination_exists
                : file_error_code::namespace_update_failed,
            file_operation::link_destination,
            injected));
    }
    file_descriptor pinned_source{};
    regular_file_identity pinned_identity{};
    auto opened = open_regular_file_at(
        source_directory,
        source_name,
        O_RDONLY,
        file_operation::open_regular_file,
        {});
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    pinned_source = std::move(*opened);
    auto current = capture_regular_file_identity_snapshot(
        pinned_source.get(), file_operation::inspect_after_io, {});
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (expected_source != nullptr && *current != *expected_source) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0));
    }
    pinned_identity = *current;
    if (const int injected = injected_errno(
            hooks, file_operation::inspect_after_io);
        injected != 0) {
        return std::unexpected(make_error(
            file_error_code::file_status_failed,
            file_operation::inspect_after_io,
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
                ? file_error_code::destination_exists
                : file_error_code::namespace_update_failed,
            file_operation::link_destination,
            saved));
    }
    auto destination_opened = open_regular_file_at(
        destination_directory,
        destination_name,
        O_RDONLY,
        file_operation::open_regular_file,
        {});
    if (!destination_opened.has_value()) {
        file_error error = destination_opened.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    file_descriptor destination_descriptor =
        std::move(*destination_opened);
    auto published = capture_regular_file_identity_snapshot(
        destination_descriptor.get(),
        file_operation::inspect_after_io,
        {});
    if (!published.has_value()) {
        file_error error = published.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    if (!matches_published_file_identity(
            *published, pinned_identity)) {
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
                file_error_code::identity_changed,
                file_operation::remove_staging_file,
                ESTALE,
                0,
                publication_state::visible_not_durable));
        }
        if (auto synced = fsync_descriptor(
                destination_directory,
                file_operation::fsync_directory,
                hooks,
                publication_state::visible_not_durable);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0));
    }
    return publication_state::visible_not_durable;
}

[[nodiscard]] inline std::expected<publication_state, file_error>
rename_name_at(
    int source_directory,
    const std::string& source_name,
    int destination_directory,
    const std::string& destination_name,
    const fault_injection_hooks& hooks,
    const regular_file_identity* expected_source = nullptr) {
    if (source_directory < 0 || destination_directory < 0
        || !valid_leaf_name(source_name)
        || !valid_leaf_name(destination_name)) {
        return std::unexpected(make_error(
            file_error_code::invalid_argument,
            file_operation::rename_destination,
            EINVAL));
    }
    if (const int injected = injected_errno(
            hooks, file_operation::rename_destination);
        injected != 0) {
        return std::unexpected(make_error(
            file_error_code::namespace_update_failed,
            file_operation::rename_destination,
            injected));
    }
    file_descriptor pinned_source{};
    regular_file_identity pinned_identity{};
    auto opened = open_regular_file_at(
        source_directory,
        source_name,
        O_RDONLY,
        file_operation::open_regular_file,
        {});
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    pinned_source = std::move(*opened);
    auto current = capture_regular_file_identity_snapshot(
        pinned_source.get(), file_operation::inspect_after_io, {});
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (expected_source != nullptr && *current != *expected_source) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0));
    }
    pinned_identity = *current;
    if (const int injected = injected_errno(
            hooks, file_operation::inspect_after_io);
        injected != 0) {
        return std::unexpected(make_error(
            file_error_code::file_status_failed,
            file_operation::inspect_after_io,
            injected));
    }
    if (::renameat(
            source_directory,
            source_name.c_str(),
            destination_directory,
            destination_name.c_str()) != 0) {
        return std::unexpected(make_error(
            file_error_code::namespace_update_failed,
            file_operation::rename_destination,
            errno));
    }
    auto destination_opened = open_regular_file_at(
        destination_directory,
        destination_name,
        O_RDONLY,
        file_operation::open_regular_file,
        {});
    if (!destination_opened.has_value()) {
        file_error error = destination_opened.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    file_descriptor destination_descriptor =
        std::move(*destination_opened);
    auto published = capture_regular_file_identity_snapshot(
        destination_descriptor.get(),
        file_operation::inspect_after_io,
        {});
    if (!published.has_value()) {
        file_error error = published.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    if (!matches_published_file_identity(
            *published, pinned_identity)) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0,
            0,
            publication_state::visible_not_durable));
    }
    return publication_state::visible_not_durable;
}


}  // namespace bioseq::common::durable_file_io::detail
