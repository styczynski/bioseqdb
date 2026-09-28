/**
 * @file sinks.cppm
 * @brief Positioned output sinks for FASTA and FASTQ writers.
 *
 * Concurrent producers reserve disjoint byte ranges before writing through
 * either `pwrite(2)` or a shared memory mapping.
 */
module;

#include <atomic>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

export module bioseq.io.sinks;

import bioseq.common.logging;

export namespace bioseq::io {

/// Result of one `write_at` call on a sink.
struct sink_write_result {
    std::size_t bytes_written = 0;
    bool has_error = false;
};

/// Requires a sink that reserves byte ranges, writes at those offsets,
/// and provides an error message when a write fails.
template <typename p_sink>
concept io_sink =
    requires(p_sink& sink, const std::uint8_t* src, std::size_t size, std::uint64_t offset) {
        { sink.write_at(src, size, offset) } -> std::same_as<sink_write_result>;
        { sink.reserve_offset(size) } -> std::same_as<std::uint64_t>;
        { sink.last_error_message() } -> std::convertible_to<std::string>;
    };

/// Writes reserved file ranges with POSIX pwrite. Producers share an atomic
/// offset counter, so each successful reservation has a separate  range.
/// Create the sink with `try_open` to receive open errors.
class posix_pwrite_sink {
  public:
    /// Construct a closed sink without an owned file descriptor.
    posix_pwrite_sink() = default;

    /// Transfer the file descriptor, reservation cursor, and diagnostic state.
    posix_pwrite_sink(posix_pwrite_sink&& other) noexcept
        : _fd(other._fd), _next_offset(other._next_offset.load(std::memory_order_relaxed)),
          _last_error_message(std::move(other._last_error_message)) {
        other._fd = -1;
    }

    /// Replace this sink with transferred descriptor and reservation state.
    posix_pwrite_sink& operator=(posix_pwrite_sink&& other) noexcept {
        if (this != &other) {
            close_fd();
            _fd = other._fd;
            _next_offset.store(other._next_offset.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
            _last_error_message = std::move(other._last_error_message);
            other._fd = -1;
        }
        return *this;
    }

    /// Sinks exclusively own their file descriptor.
    posix_pwrite_sink(const posix_pwrite_sink&) = delete;

    /// Sinks cannot copy file-descriptor ownership.
    posix_pwrite_sink& operator=(const posix_pwrite_sink&) = delete;

    /// Close the owned file descriptor.
    ~posix_pwrite_sink() {
        close_fd();
    }

    /// Open `path` for writing, truncating any existing contents.
    /// Returns the sink or an error message on failure.
    [[nodiscard]] static std::expected<posix_pwrite_sink, std::string>
    try_open(std::string_view path) {
        std::string null_terminated_path{path};
        const int fd = ::open(null_terminated_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            return std::unexpected(
                fmt::format("open('{}', O_WRONLY | O_CREAT | O_TRUNC) failed: {}",
                            null_terminated_path,
                            std::strerror(errno)));
        }
        posix_pwrite_sink sink;
        sink._fd = fd;
        return sink;
    }

    /// Reserves a byte range and returns its starting offset for `write_at`.
    /// Concurrent reservations advance the same atomic counter.
    [[nodiscard]] std::uint64_t reserve_offset(std::size_t byte_count) noexcept {
        return _next_offset.fetch_add(static_cast<std::uint64_t>(byte_count),
                                      std::memory_order_relaxed);
    }

    /// Write `size` bytes starting at `offset`. Caller MUST have
    /// reserved this offset range via `reserve_offset(size)`.
    [[nodiscard]] sink_write_result
    write_at(const std::uint8_t* src, std::size_t size, std::uint64_t offset) noexcept {
        sink_write_result result;
        std::size_t bytes_remaining = size;
        const std::uint8_t* cursor = src;
        std::uint64_t cursor_offset = offset;
        while (bytes_remaining > 0) {
            const ssize_t n_written =
                ::pwrite(_fd, cursor, bytes_remaining, static_cast<off_t>(cursor_offset));
            if (n_written < 0) {
                if (errno == EINTR)
                    continue;
                _last_error_message = fmt::format("pwrite(fd={}, size={}, offset={}) failed: {}",
                                                  _fd,
                                                  bytes_remaining,
                                                  cursor_offset,
                                                  std::strerror(errno));
                result.has_error = true;
                result.bytes_written = size - bytes_remaining;
                return result;
            }
            const std::size_t advanced = static_cast<std::size_t>(n_written);
            cursor += advanced;
            cursor_offset += advanced;
            bytes_remaining -= advanced;
        }
        result.bytes_written = size;
        return result;
    }

    /// Closes the file without fsync. Repeated calls after closing succeed.
    [[nodiscard]] std::expected<void, std::string> close() noexcept {
        if (_fd < 0)
            return {};
        const int rc_close = ::close(_fd);
        const int saved_errno = errno;
        const int closed_fd = _fd;
        _fd = -1;
        if (rc_close < 0) {
            return std::unexpected(
                fmt::format("close(fd={}) failed: {}", closed_fd, std::strerror(saved_errno)));
        }
        return {};
    }

    /// Returns the total bytes reserved, including ranges whose writes failed.
    [[nodiscard]] std::uint64_t reserved_byte_count() const noexcept {
        return _next_offset.load(std::memory_order_relaxed);
    }

    /// Return the most recent write diagnostic.
    [[nodiscard]] std::string last_error_message() const {
        return _last_error_message;
    }

    /// Return the owned file descriptor, or -1 after close.
    [[nodiscard]] int fd() const noexcept {
        return _fd;
    }

  private:
    /// Close the owned descriptor when one is active.
    void close_fd() noexcept {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    int _fd = -1;
    std::atomic<std::uint64_t> _next_offset{0};
    std::string _last_error_message;
};

/// Writes reserved ranges by copying bytes into a shared file mapping.
/// The capacity hint sets the mapped file size and musi cover the output.
/// `close()` removes the mapping and resizes the file to the reservation
/// count. Destruction alone releases resources without resizing the file.
class mmap_pwrite_sink {
  public:
    /// Construct a closed sink without a mapping.
    mmap_pwrite_sink() = default;

    /// Transfer mapping ownership and the reservation cursor.
    mmap_pwrite_sink(mmap_pwrite_sink&& other) noexcept
        : _fd(other._fd), _addr(other._addr), _capacity(other._capacity),
          _next_offset(other._next_offset.load(std::memory_order_relaxed)),
          _last_error_message(std::move(other._last_error_message)) {
        other._fd = -1;
        other._addr = nullptr;
        other._capacity = 0;
    }

    /// Sinks exclusively own their mapping and file descriptor.
    mmap_pwrite_sink(const mmap_pwrite_sink&) = delete;

    /// Sinks cannot copy mapping ownership.
    mmap_pwrite_sink& operator=(const mmap_pwrite_sink&) = delete;

    /// Unmap the region and close its backing descriptor.
    ~mmap_pwrite_sink() {
        cleanup();
    }

    /// Creates or truncates a file and maps it for reading  and writing.
    /// `capacity_hint` sets its size Zero requests one 4096-byte region.
    [[nodiscard]] static std::expected<mmap_pwrite_sink, std::string>
    try_open(std::string_view path, std::size_t capacity_hint) {
        std::string null_terminated_path{path};
        const int fd = ::open(null_terminated_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            return std::unexpected(fmt::format("open('{}', O_RDWR | O_CREAT | O_TRUNC) failed: {}",
                                               null_terminated_path,
                                               std::strerror(errno)));
        }
        if (capacity_hint == 0)
            capacity_hint = 4096;
        if (::ftruncate(fd, static_cast<off_t>(capacity_hint)) < 0) {
            const std::string err = fmt::format(
                "ftruncate(fd={}, {}) failed: {}", fd, capacity_hint, std::strerror(errno));
            ::close(fd);
            return std::unexpected(err);
        }
        void* addr = ::mmap(nullptr, capacity_hint, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            const std::string err = fmt::format(
                "mmap(fd={}, size={}) failed: {}", fd, capacity_hint, std::strerror(errno));
            ::close(fd);
            return std::unexpected(err);
        }
        mmap_pwrite_sink sink;
        sink._fd = fd;
        sink._addr = static_cast<std::uint8_t*>(addr);
        sink._capacity = capacity_hint;
        return sink;
    }

    /// Reserves a range by advancing the shared byte counter. Capacity is checked
    /// later by `write_at`, so even a failed write keeps its reservation.
    /// The counter assigns offsets only and does not publish the written bytes.
    [[nodiscard]] std::uint64_t reserve_offset(std::size_t byte_count) noexcept {
        return _next_offset.fetch_add(static_cast<std::uint64_t>(byte_count),
                                      std::memory_order_relaxed);
    }

    /// Copies bytes into a range previously claimed with `reserve_offset`.
    /// Returns an error if the range ends beyond the mapping. The reservation
    /// remains counted after that failure.
    [[nodiscard]] sink_write_result
    write_at(const std::uint8_t* src, std::size_t size, std::uint64_t offset) noexcept {
        sink_write_result result;
        if (offset + size > _capacity) {
            _last_error_message =
                fmt::format("mmap_pwrite_sink: write at offset {} of size {} exceeds "
                            "capacity {} (caller did not pass a sufficient size hint)",
                            offset,
                            size,
                            _capacity);
            result.has_error = true;
            return result;
        }
        std::memcpy(_addr + offset, src, size);
        result.bytes_written = size;
        return result;
    }

    /// Unmaps the file, resizes it to the reserved byte count, and closes it.
    [[nodiscard]] std::expected<void, std::string> close() noexcept {
        if (_fd < 0)
            return {};
        const std::size_t actual_size =
            static_cast<std::size_t>(_next_offset.load(std::memory_order_relaxed));
        if (_addr != nullptr) {
            ::munmap(_addr, _capacity);
            _addr = nullptr;
        }
        if (::ftruncate(_fd, static_cast<off_t>(actual_size)) < 0) {
            const std::string err = fmt::format(
                "ftruncate(fd={}, {}) failed: {}", _fd, actual_size, std::strerror(errno));
            ::close(_fd);
            _fd = -1;
            return std::unexpected(err);
        }
        const int closed_fd = _fd;
        _fd = -1;
        if (::close(closed_fd) < 0) {
            return std::unexpected(
                fmt::format("close(fd={}) failed: {}", closed_fd, std::strerror(errno)));
        }
        return {};
    }

    /// Return the total byte count claimed by producers.
    [[nodiscard]] std::uint64_t reserved_byte_count() const noexcept {
        return _next_offset.load(std::memory_order_relaxed);
    }

    /// Returns the most recent write diagnostic.
    [[nodiscard]] std::string last_error_message() const {
        return _last_error_message;
    }

  private:
    /// Release the mapping before closing its backing descriptor.
    void cleanup() noexcept {
        if (_addr != nullptr) {
            ::munmap(_addr, _capacity);
            _addr = nullptr;
        }
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    int _fd = -1;
    std::uint8_t* _addr = nullptr;
    std::size_t _capacity = 0;
    std::atomic<std::uint64_t> _next_offset{0};
    std::string _last_error_message;
};

} // namespace bioseq::io
