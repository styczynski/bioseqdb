/**
 * @file common/durable_file_io/staging.cppm
 * @brief Internal staging-file creation, synchronization, and verification.
 */
module;

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

export module bioseq.common.durable_file_io:staging;
import :types;
import :error;
import :identity;
import :open;
import :sync;
import :path;
import :positional_io;
import :cleanup;

namespace bioseq::common::durable_file_io::detail {

[[nodiscard]] inline std::string next_staging_name(
    std::string_view destination_name) {
    static std::atomic<std::uint64_t> sequence{1};
    const std::uint64_t value =
        sequence.fetch_add(1, std::memory_order_relaxed);
    return std::string(destination_name)
        + ".tmp." + std::to_string(static_cast<std::uint64_t>(::getpid()))
        + "." + std::to_string(value);
}

struct staged_regular_file {
    std::string name;
    regular_file_identity identity{};
};

[[nodiscard]] inline std::expected<staged_regular_file, file_error>
stage_regular_file(
    int directory_descriptor,
    const std::string& destination_name,
    std::span<const std::byte> bytes,
    const publication_options& options,
    const fault_injection_hooks& hooks,
    const positional_io_callbacks& callbacks) {
    if (directory_descriptor < 0
        || !valid_leaf_name(destination_name)
        || !valid_creation_mode(options.mode)) {
        return std::unexpected(make_error(
            file_error_code::invalid_argument,
            file_operation::create_staging_file,
            EINVAL));
    }
#if !defined(O_NOFOLLOW)
    static_cast<void>(bytes);
    static_cast<void>(hooks);
    static_cast<void>(callbacks);
    return std::unexpected(make_error(
        file_error_code::unsupported_platform,
        file_operation::create_staging_file,
        0));
#else
    constexpr std::size_t maximum_attempts = 64;
    std::string staging_name{};
    int raw_descriptor = -1;
    for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
        staging_name = next_staging_name(destination_name);
        if (const int injected = injected_errno(
                hooks, file_operation::create_staging_file);
            injected != 0) {
            return std::unexpected(make_error(
                file_error_code::file_open_failed,
                file_operation::create_staging_file,
                injected));
        }
        int flags = O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        raw_descriptor = ::openat(
            directory_descriptor,
            staging_name.c_str(),
            flags,
            options.mode);
        if (raw_descriptor >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return std::unexpected(make_error(
                file_error_code::file_open_failed,
                file_operation::create_staging_file,
                errno));
        }
    }
    if (raw_descriptor < 0) {
        return std::unexpected(make_error(
            file_error_code::file_open_failed,
            file_operation::create_staging_file,
            EEXIST));
    }
    file_descriptor descriptor{raw_descriptor};
    auto pinned_identity = capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_before_io, {});
    if (!pinned_identity.has_value()) {
        const file_error primary = pinned_identity.error();
        descriptor.reset();
        auto removed = remove_name_at(
            directory_descriptor,
            staging_name,
            publication_state::unchanged,
            hooks);
        return std::unexpected(
            removed.has_value() ? primary : removed.error());
    }
    auto identity = capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_before_io, hooks);
    const mode_t actual_permissions = identity.has_value()
        ? static_cast<mode_t>(
            identity->mode & creation_permission_mask)
        : 0;
    if (!identity.has_value()
        || identity->link_count != 1
        || (actual_permissions & ~options.mode) != 0) {
        const file_error primary = identity.has_value()
            ? make_error(
                file_error_code::not_regular_file,
                file_operation::inspect_before_io,
                0)
            : identity.error();
        descriptor.reset();
        return std::unexpected(staging_cleanup_error(
            directory_descriptor,
            staging_name,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (const int injected = injected_errno(
            hooks, file_operation::positional_write);
        injected != 0) {
        const file_error primary = make_error(
            file_error_code::write_failed,
            file_operation::positional_write,
            injected);
        descriptor.reset();
        return std::unexpected(staging_cleanup_error(
            directory_descriptor,
            staging_name,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto written = write_all_at(
            descriptor.get(), bytes, 0, callbacks);
        !written.has_value()) {
        const file_error primary = written.error();
        descriptor.reset();
        return std::unexpected(staging_cleanup_error(
            directory_descriptor,
            staging_name,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto synced = fsync_descriptor(
            descriptor.get(),
            file_operation::fsync_file,
            hooks);
        !synced.has_value()) {
        const file_error primary = synced.error();
        descriptor.reset();
        return std::unexpected(staging_cleanup_error(
            directory_descriptor,
            staging_name,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    auto final_identity = capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_after_io, hooks);
    if (!final_identity.has_value()
        || !same_regular_file_object(*identity, *final_identity)
        || final_identity->size != bytes.size()) {
        const file_error primary = final_identity.has_value()
            ? make_error(
                file_error_code::identity_changed,
                file_operation::inspect_after_io,
                0)
            : final_identity.error();
        descriptor.reset();
        return std::unexpected(staging_cleanup_error(
            directory_descriptor,
            staging_name,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (const int injected = injected_errno(
            hooks, file_operation::close_file);
        injected != 0) {
        const file_error primary = make_error(
            file_error_code::descriptor_close_failed,
            file_operation::close_file,
            injected);
        descriptor.reset();
        return std::unexpected(staging_cleanup_error(
            directory_descriptor,
            staging_name,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(staging_cleanup_error(
            directory_descriptor,
            staging_name,
            *pinned_identity,
            publication_state::unchanged,
            closed.error(),
            hooks));
    }
    return staged_regular_file{
        .name = std::move(staging_name),
        .identity = *final_identity,
    };
#endif
}

[[nodiscard]] inline std::expected<regular_file_identity, file_error>
sync_existing_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const fault_injection_hooks& hooks,
    const regular_file_identity* expected = nullptr) {
    auto opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_operation::open_regular_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto identity = capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_before_io, hooks);
    if (!identity.has_value()) {
        return std::unexpected(identity.error());
    }
    if (expected != nullptr && *identity != *expected) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_before_io,
            0));
    }
    if (auto synced = fsync_descriptor(
            descriptor.get(), file_operation::fsync_file, hooks);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    auto after = capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_after_io, hooks);
    if (!after.has_value()) {
        return std::unexpected(after.error());
    }
    if (*after != *identity) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    auto current_opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_operation::open_regular_file,
        hooks);
    if (!current_opened.has_value()) {
        return std::unexpected(current_opened.error());
    }
    file_descriptor current_descriptor = std::move(*current_opened);
    auto current = capture_regular_file_identity_snapshot(
        current_descriptor.get(), file_operation::inspect_after_io, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != *after) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0));
    }
    return *after;
}

[[nodiscard]] inline std::expected<void, file_error>
verify_staged_file(
    int directory_descriptor,
    const staged_regular_file& staged,
    const fault_injection_hooks& hooks) {
    auto opened = open_regular_file_at(
        directory_descriptor,
        staged.name,
        O_RDONLY,
        file_operation::open_regular_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_after_io, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != staged.identity) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0));
    }
    return {};
}

/** Stage, fsync, close, reopen, and identity-check one publication payload. */
[[nodiscard]] inline std::expected<staged_regular_file, file_error>
stage_verified_regular_file(
    int directory_descriptor,
    const std::string& destination_name,
    std::span<const std::byte> bytes,
    const publication_options& options,
    const fault_injection_hooks& hooks,
    const positional_io_callbacks& callbacks) {
    auto staged = stage_regular_file(
        directory_descriptor,
        destination_name,
        bytes,
        options,
        hooks,
        callbacks);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    if (auto verified = verify_staged_file(
            directory_descriptor, *staged, hooks);
        !verified.has_value()) {
        return std::unexpected(staging_cleanup_error(
            directory_descriptor,
            staged->name,
            staged->identity,
            verified.error().publication,
            verified.error(),
            hooks));
    }
    return std::move(*staged);
}


}  // namespace bioseq::common::durable_file_io::detail
