/**
 * @file common/durable_file_io/namespace_publish.cppm
 * @brief Internal no-replace link and rename namespace publication.
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

export module bioseq.common.durable_file_io:namespace_publish;
import :types;
import :error;
import :identity;
import :open;
import :sync;
import :path;
import :cleanup;

namespace bioseq::durable_file_io::detail {

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


}  // namespace bioseq::durable_file_io::detail
