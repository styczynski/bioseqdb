/**
 * @file common/durable_file_io/positional_io.cppm
 * @brief Complete positional I/O and regular-file identity capture.
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

export module bioseq.common.durable_file_io:positional_io;
export import :types;

import :error;
import :identity;

namespace bioseq::durable_file_io::detail {

[[nodiscard]] inline ssize_t system_read_at(
    void*, int descriptor, void* destination, std::size_t bytes,
    off_t offset) noexcept {
    return ::pread(descriptor, destination, bytes, offset);
}

[[nodiscard]] inline ssize_t system_write_at(
    void*, int descriptor, const void* source, std::size_t bytes,
    off_t offset) noexcept {
    return ::pwrite(descriptor, source, bytes, offset);
}

}  // namespace bioseq::durable_file_io::detail

export namespace bioseq::durable_file_io {

/** Capture a regular descriptor's complete stable identity. */
[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
capture_regular_file_identity(
    int descriptor,
    const file_io_hooks& hooks = {}) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::stat_before,
            EBADF));
    }
    return detail::capture_identity(
        descriptor, file_io_operation::stat_before, hooks);
}

/** Read exactly `destination.size()` bytes at `offset`. */
[[nodiscard]] inline std::expected<void, file_io_error>
positional_read_exact(
    int descriptor,
    std::uint64_t offset,
    std::span<std::byte> destination,
    const positional_io_operations& operations) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::positional_read,
            EBADF,
            offset));
    }
    const std::uint64_t maximum_offset = static_cast<std::uint64_t>(
        (std::numeric_limits<off_t>::max)());
    if (offset > maximum_offset
        || (!destination.empty()
            && destination.size() > maximum_offset - offset + 1)) {
        return std::unexpected(detail::make_error(
            file_io_errc::offset_overflow,
            file_io_operation::positional_read,
            EOVERFLOW,
            offset));
    }
    const auto read_at = operations.read_at != nullptr
        ? operations.read_at
        : detail::system_read_at;
    std::size_t completed = 0;
    while (completed < destination.size()) {
        if (operations.poll != nullptr) {
            operations.poll(operations.context);
        }
        const std::size_t request = std::min(
            destination.size() - completed,
            static_cast<std::size_t>(
                (std::numeric_limits<ssize_t>::max)()));
        const ssize_t count = read_at(
            operations.context,
            descriptor,
            destination.data() + completed,
            request,
            static_cast<off_t>(offset + completed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::read_failed,
                file_io_operation::positional_read,
                errno,
                offset + completed));
        }
        if (count == 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::unexpected_end_of_file,
                file_io_operation::positional_read,
                0,
                offset + completed));
        }
        const auto progress = static_cast<std::size_t>(count);
        if (progress > destination.size() - completed) {
            return std::unexpected(detail::make_error(
                file_io_errc::read_failed,
                file_io_operation::positional_read,
                EIO,
                offset + completed));
        }
        completed += progress;
    }
    return {};
}

/** Write all bytes at `offset`, retrying EINTR and accepting short writes. */
[[nodiscard]] inline std::expected<void, file_io_error>
positional_write_all(
    int descriptor,
    std::span<const std::byte> bytes,
    std::uint64_t offset,
    const positional_io_operations& operations) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::positional_write,
            EBADF,
            offset));
    }
    const std::uint64_t maximum_offset = static_cast<std::uint64_t>(
        (std::numeric_limits<off_t>::max)());
    if (offset > maximum_offset
        || (!bytes.empty()
            && bytes.size() > maximum_offset - offset + 1)) {
        return std::unexpected(detail::make_error(
            file_io_errc::offset_overflow,
            file_io_operation::positional_write,
            EOVERFLOW,
            offset));
    }
    const auto write_at = operations.write_at != nullptr
        ? operations.write_at
        : detail::system_write_at;
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        if (operations.poll != nullptr) {
            operations.poll(operations.context);
        }
        const std::size_t request = std::min(
            bytes.size() - completed,
            static_cast<std::size_t>(
                (std::numeric_limits<ssize_t>::max)()));
        const ssize_t count = write_at(
            operations.context,
            descriptor,
            bytes.data() + completed,
            request,
            static_cast<off_t>(offset + completed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::write_failed,
                file_io_operation::positional_write,
                errno,
                offset + completed));
        }
        if (count == 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::no_write_progress,
                file_io_operation::positional_write,
                0,
                offset + completed));
        }
        const auto progress = static_cast<std::size_t>(count);
        if (progress > bytes.size() - completed) {
            return std::unexpected(detail::make_error(
                file_io_errc::write_failed,
                file_io_operation::positional_write,
                EIO,
                offset + completed));
        }
        completed += progress;
    }
    return {};
}

}  // namespace bioseq::durable_file_io
