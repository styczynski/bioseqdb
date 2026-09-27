/**
 * @file common/durable_file_io/sync.cppm
 * @brief Sync file descriptors and report errors when closing directories.
 */
module;

#include <cerrno>
#include <expected>
#include <unistd.h>

export module bioseq.common.durable_file_io:sync;
import :types;
import :error;

namespace bioseq::common::durable_file_io::detail {

[[nodiscard]] inline std::expected<void, file_error> fsync_descriptor(
    int descriptor,
    file_operation operation,
    const fault_injection_hooks& hooks,
    publication_state publication = publication_state::unchanged) noexcept {
    if (const int injected = injected_errno(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            operation == file_operation::fsync_directory
                ? file_error_code::directory_fsync_failed
                : file_error_code::file_fsync_failed,
            operation,
            injected,
            0,
            publication));
    }
    int sync_result = -1;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    if (sync_result != 0) {
        return std::unexpected(make_error(
            operation == file_operation::fsync_directory
                ? file_error_code::directory_fsync_failed
                : file_error_code::file_fsync_failed,
            operation,
            errno,
            0,
            publication));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, file_error>
close_directory_descriptor(
    file_descriptor& descriptor,
    const fault_injection_hooks& hooks,
    publication_state publication) noexcept {
    if (!descriptor.valid()) {
        return {};
    }
    if (const int injected = injected_errno(
            hooks, file_operation::close_directory);
        injected != 0) {
        descriptor.reset();
        return std::unexpected(make_error(
            file_error_code::descriptor_close_failed,
            file_operation::close_directory,
            injected,
            0,
            publication));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        file_error error = closed.error();
        error.operation = file_operation::close_directory;
        error.publication = publication;
        return std::unexpected(error);
    }
    return {};
}

}  // namespace bioseq::common::durable_file_io::detail
