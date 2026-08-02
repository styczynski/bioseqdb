/**
 * @file common/durable_file_io/staging.cppm
 * @brief Internal temporary-file staging and verification.
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

export module bioseq.common.durable_file_io:staging;
import :types;
import :error;
import :identity;
import :open;
import :sync;
import :path;
import :positional_io;
import :cleanup;

namespace bioseq::durable_file_io::detail {

[[nodiscard]] inline std::string unique_temporary_name(
    std::string_view destination_name) {
    static std::atomic<std::uint64_t> sequence{1};
    const std::uint64_t value =
        sequence.fetch_add(1, std::memory_order_relaxed);
    return std::string(destination_name)
        + ".tmp." + std::to_string(static_cast<std::uint64_t>(::getpid()))
        + "." + std::to_string(value);
}

struct staged_file {
    std::string name;
    regular_file_identity identity{};
};

[[nodiscard]] inline std::expected<staged_file, file_io_error> stage_bytes(
    int directory_descriptor,
    const std::string& destination_name,
    std::span<const std::byte> bytes,
    const durable_publish_options& options,
    const file_io_hooks& hooks,
    const positional_io_operations& operations) {
    if (directory_descriptor < 0
        || !valid_leaf_name(destination_name)
        || options.mode == 0
        || (options.mode & ~(S_IRWXU | S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
#if !defined(O_NOFOLLOW)
    static_cast<void>(bytes);
    static_cast<void>(hooks);
    static_cast<void>(operations);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::create_temporary,
        0));
#else
    constexpr std::size_t maximum_attempts = 64;
    std::string temporary{};
    int raw_descriptor = -1;
    for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
        temporary = unique_temporary_name(destination_name);
        if (const int injected = injected_error(
                hooks, file_io_operation::create_temporary);
            injected != 0) {
            return std::unexpected(make_error(
                file_io_errc::open_failed,
                file_io_operation::create_temporary,
                injected));
        }
        int flags = O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        raw_descriptor = ::openat(
            directory_descriptor,
            temporary.c_str(),
            flags,
            options.mode);
        if (raw_descriptor >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return std::unexpected(make_error(
                file_io_errc::open_failed,
                file_io_operation::create_temporary,
                errno));
        }
    }
    if (raw_descriptor < 0) {
        return std::unexpected(make_error(
            file_io_errc::open_failed,
            file_io_operation::create_temporary,
            EEXIST));
    }
    file_descriptor descriptor{raw_descriptor};
    auto pinned_identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, {});
    if (!pinned_identity.has_value()) {
        const file_io_error primary = pinned_identity.error();
        descriptor.reset();
        auto removed = remove_name_at(
            directory_descriptor,
            temporary,
            publication_state::unchanged,
            hooks);
        return std::unexpected(
            removed.has_value() ? primary : removed.error());
    }
    auto identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    const mode_t actual_permissions = identity.has_value()
        ? static_cast<mode_t>(
            identity->mode & (S_IRWXU | S_IRWXG | S_IRWXO))
        : 0;
    if (!identity.has_value()
        || identity->link_count != 1
        || (actual_permissions & ~options.mode) != 0) {
        const file_io_error primary = identity.has_value()
            ? make_error(
                file_io_errc::not_regular_file,
                file_io_operation::stat_before,
                0)
            : identity.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::positional_write);
        injected != 0) {
        const file_io_error primary = make_error(
            file_io_errc::write_failed,
            file_io_operation::positional_write,
            injected);
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto written = positional_write_all(
            descriptor.get(), bytes, 0, operations);
        !written.has_value()) {
        const file_io_error primary = written.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto synced = sync_descriptor(
            descriptor.get(),
            file_io_operation::sync_file,
            hooks);
        !synced.has_value()) {
        const file_io_error primary = synced.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    auto final_identity = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!final_identity.has_value()
        || !same_file_object(*identity, *final_identity)
        || final_identity->size != bytes.size()) {
        const file_io_error primary = final_identity.has_value()
            ? make_error(
                file_io_errc::identity_changed,
                file_io_operation::stat_after,
                0)
            : final_identity.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::close_file);
        injected != 0) {
        const file_io_error primary = make_error(
            file_io_errc::close_failed,
            file_io_operation::close_file,
            injected);
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            closed.error(),
            hooks));
    }
    return staged_file{
        .name = std::move(temporary),
        .identity = *final_identity,
    };
#endif
}

[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
sync_existing_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const file_io_hooks& hooks,
    const regular_file_identity* expected = nullptr) {
    auto opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    if (!identity.has_value()) {
        return std::unexpected(identity.error());
    }
    if (expected != nullptr && *identity != *expected) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_before,
            0));
    }
    if (auto synced = sync_descriptor(
            descriptor.get(), file_io_operation::sync_file, hooks);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    auto after = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!after.has_value()) {
        return std::unexpected(after.error());
    }
    if (*after != *identity) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    auto current_opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!current_opened.has_value()) {
        return std::unexpected(current_opened.error());
    }
    file_descriptor current_descriptor = std::move(*current_opened);
    auto current = capture_identity(
        current_descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    return *after;
}

[[nodiscard]] inline std::expected<void, file_io_error>
verify_staged_path(
    int directory_descriptor,
    const staged_file& staged,
    const file_io_hooks& hooks) {
    auto opened = open_regular_file_at(
        directory_descriptor,
        staged.name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != staged.identity) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    return {};
}


}  // namespace bioseq::durable_file_io::detail
