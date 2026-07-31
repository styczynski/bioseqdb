/**
 * @file common/durable_file_io/sync.cppm
 * @brief Internal durable descriptor synchronization and close handling.
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

export module bioseq.common.durable_file_io:sync;
import :types;
import :error;

namespace bioseq::durable_file_io::detail {

[[nodiscard]] inline std::expected<void, file_io_error> sync_descriptor(
    int descriptor,
    file_io_operation operation,
    const file_io_hooks& hooks,
    publication_state publication = publication_state::unchanged) noexcept {
    if (const int injected = injected_error(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            operation == file_io_operation::sync_directory
                ? file_io_errc::directory_sync_failed
                : file_io_errc::file_sync_failed,
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
            operation == file_io_operation::sync_directory
                ? file_io_errc::directory_sync_failed
                : file_io_errc::file_sync_failed,
            operation,
            errno,
            0,
            publication));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, file_io_error>
close_directory_descriptor(
    file_descriptor& descriptor,
    const file_io_hooks& hooks,
    publication_state publication) noexcept {
    if (!descriptor.valid()) {
        return {};
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::close_directory);
        injected != 0) {
        descriptor.reset();
        return std::unexpected(make_error(
            file_io_errc::close_failed,
            file_io_operation::close_directory,
            injected,
            0,
            publication));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        file_io_error error = closed.error();
        error.operation = file_io_operation::close_directory;
        error.publication = publication;
        return std::unexpected(error);
    }
    return {};
}

}  // namespace bioseq::durable_file_io::detail
