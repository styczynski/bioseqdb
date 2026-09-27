/**
 * @file common/durable_file_io/positional_io.cppm
 * @brief Read or write a complete byte range at a file offset.
 */
module;

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <sys/types.h>
#include <unistd.h>

export module bioseq.common.durable_file_io:positional_io;
export import :types;

import bioseq.common.checked_arithmetic;

import :error;
import :identity;

namespace bioseq::common::durable_file_io::detail {

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

[[nodiscard]] inline bool positional_range_fits(
    std::uint64_t offset,
    std::size_t byte_count) noexcept {
    const std::uint64_t maximum_offset = static_cast<std::uint64_t>(
        (std::numeric_limits<off_t>::max)());
    const std::uint64_t length = static_cast<std::uint64_t>(byte_count);
    if (offset > maximum_offset
        || static_cast<std::size_t>(length) != byte_count) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    const auto last_offset = bioseq::common::checked_add(
        offset, length - 1);
    return last_offset.has_value() && *last_offset <= maximum_offset;
}

}  // namespace bioseq::common::durable_file_io::detail

export namespace bioseq::common::durable_file_io {

/** Read metadata for an open regular file so callers can detect  later changes. */
[[nodiscard]] inline std::expected<regular_file_identity, file_error>
capture_regular_file_identity(
    int descriptor,
    const fault_injection_hooks& hooks = {}) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::inspect_before_io,
            EBADF));
    }
    return detail::capture_regular_file_identity_snapshot(
        descriptor, file_operation::inspect_before_io, hooks);
}

/**
 * Read exactly destination.size() bytes starting at offset.
 * Retry interrupted and short reads. Reaching EOF early reports
 * unexpected_end_of_file. A failure records the first unread offset.
 */
[[nodiscard]] inline std::expected<void, file_error>
read_exact_at(
    int descriptor,
    std::uint64_t offset,
    std::span<std::byte> destination,
    const positional_io_callbacks& callbacks) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::positional_read,
            EBADF,
            offset));
    }
    if (!detail::positional_range_fits(offset, destination.size())) {
        return std::unexpected(detail::make_error(
            file_error_code::offset_overflow,
            file_operation::positional_read,
            EOVERFLOW,
            offset));
    }
    const auto read_at = callbacks.read_at != nullptr
        ? callbacks.read_at
        : detail::system_read_at;
    std::size_t completed = 0;
    while (completed < destination.size()) {
        if (callbacks.poll != nullptr) {
            callbacks.poll(callbacks.context);
        }
        const std::size_t request = std::min(
            destination.size() - completed,
            static_cast<std::size_t>(
                (std::numeric_limits<ssize_t>::max)()));
        const ssize_t count = read_at(
            callbacks.context,
            descriptor,
            destination.data() + completed,
            request,
            static_cast<off_t>(offset + completed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return std::unexpected(detail::make_error(
                file_error_code::read_failed,
                file_operation::positional_read,
                errno,
                offset + completed));
        }
        if (count == 0) {
            return std::unexpected(detail::make_error(
                file_error_code::unexpected_end_of_file,
                file_operation::positional_read,
                0,
                offset + completed));
        }
        const auto progress = static_cast<std::size_t>(count);
        if (progress > destination.size() - completed) {
            return std::unexpected(detail::make_error(
                file_error_code::read_failed,
                file_operation::positional_read,
                EIO,
                offset + completed));
        }
        completed += progress;
    }
    return {};
}

/**
 * Write the, complete byte span starting at offset.
 * Retry interrupted and short writes. A write that makes no progress reports
 * no_write_progress. A failure records the first unwritten offset.
 */
[[nodiscard]] inline std::expected<void, file_error>
write_all_at(
    int descriptor,
    std::span<const std::byte> bytes,
    std::uint64_t offset,
    const positional_io_callbacks& callbacks) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_error_code::invalid_argument,
            file_operation::positional_write,
            EBADF,
            offset));
    }
    if (!detail::positional_range_fits(offset, bytes.size())) {
        return std::unexpected(detail::make_error(
            file_error_code::offset_overflow,
            file_operation::positional_write,
            EOVERFLOW,
            offset));
    }
    const auto write_at = callbacks.write_at != nullptr
        ? callbacks.write_at
        : detail::system_write_at;
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        if (callbacks.poll != nullptr) {
            callbacks.poll(callbacks.context);
        }
        const std::size_t request = std::min(
            bytes.size() - completed,
            static_cast<std::size_t>(
                (std::numeric_limits<ssize_t>::max)()));
        const ssize_t count = write_at(
            callbacks.context,
            descriptor,
            bytes.data() + completed,
            request,
            static_cast<off_t>(offset + completed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return std::unexpected(detail::make_error(
                file_error_code::write_failed,
                file_operation::positional_write,
                errno,
                offset + completed));
        }
        if (count == 0) {
            return std::unexpected(detail::make_error(
                file_error_code::no_write_progress,
                file_operation::positional_write,
                0,
                offset + completed));
        }
        const auto progress = static_cast<std::size_t>(count);
        if (progress > bytes.size() - completed) {
            return std::unexpected(detail::make_error(
                file_error_code::write_failed,
                file_operation::positional_write,
                EIO,
                offset + completed));
        }
        completed += progress;
    }
    return {};
}

}  // namespace bioseq::common::durable_file_io
