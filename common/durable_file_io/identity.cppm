/**
 * @file common/durable_file_io/identity.cppm
 * @brief Read file metadata and compare it to detect replacement or changes.
 */
module;

#include <cerrno>
#include <cstdint>
#include <expected>
#include <sys/stat.h>
#include <unistd.h>

export module bioseq.common.durable_file_io:identity;
import :types;
import :error;

namespace bioseq::common::durable_file_io::detail {

[[nodiscard]] inline regular_file_identity identity_from_stat(
    const struct stat& status) noexcept {
    regular_file_identity result{
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
        .size = static_cast<std::uint64_t>(status.st_size),
        .link_count = static_cast<std::uint64_t>(status.st_nlink),
        .owner = static_cast<std::uint64_t>(status.st_uid),
        .mode = static_cast<std::uint64_t>(status.st_mode),
    };
#if defined(__APPLE__)
    result.modified_seconds = status.st_mtimespec.tv_sec;
    result.modified_nanoseconds = status.st_mtimespec.tv_nsec;
    result.changed_seconds = status.st_ctimespec.tv_sec;
    result.changed_nanoseconds = status.st_ctimespec.tv_nsec;
#else
    result.modified_seconds = status.st_mtim.tv_sec;
    result.modified_nanoseconds = status.st_mtim.tv_nsec;
    result.changed_seconds = status.st_ctim.tv_sec;
    result.changed_nanoseconds = status.st_ctim.tv_nsec;
#endif
    return result;
}

[[nodiscard]] inline bool same_regular_file_object(
    const regular_file_identity& left,
    const regular_file_identity& right) noexcept {
    return left.device == right.device
        && left.inode == right.inode
        && left.link_count == right.link_count
        && left.owner == right.owner
        && left.mode == right.mode;
}

[[nodiscard]] inline bool same_inode(
    const regular_file_identity& left,
    const regular_file_identity& right) noexcept {
    return left.device == right.device
        && left.inode == right.inode;
}

[[nodiscard]] inline bool matches_published_file_identity(
    const regular_file_identity& left,
    const regular_file_identity& right) noexcept {
    return same_inode(left, right)
        && left.size == right.size
        && left.owner == right.owner
        && left.mode == right.mode
        && left.modified_seconds == right.modified_seconds
        && left.modified_nanoseconds == right.modified_nanoseconds;
}

[[nodiscard]] inline std::expected<regular_file_identity, file_error>
capture_regular_file_identity_snapshot(
    int descriptor,
    file_operation operation,
    const fault_injection_hooks& hooks) noexcept {
    if (const int injected = injected_errno(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            file_error_code::file_status_failed, operation, injected));
    }
    struct stat status {};
    int status_result = -1;
    do {
        status_result = ::fstat(descriptor, &status);
    } while (status_result != 0 && errno == EINTR);
    if (status_result != 0) {
        return std::unexpected(make_error(
            file_error_code::file_status_failed, operation, errno));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(make_error(
            file_error_code::not_regular_file, operation, 0));
    }
    if (status.st_size < 0) {
        return std::unexpected(make_error(
            file_error_code::invalid_file_size, operation, 0));
    }
    return identity_from_stat(status);
}

}  // namespace bioseq::common::durable_file_io::detail
