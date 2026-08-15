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

/// Concept: an I/O sink writes a contiguous byte range into a
/// pre-reserved offset and surfaces errors via
/// `last_error_message()`. Sinks are monomorphic per template
/// instantiation without virtual dispatch.
template <typename p_sink>
concept io_sink =
    requires(p_sink& sink, const std::uint8_t* src, std::size_t size, std::uint64_t offset) {
        { sink.write_at(src, size, offset) } -> std::same_as<sink_write_result>;
        { sink.reserve_offset(size) } -> std::same_as<std::uint64_t>;
        { sink.last_error_message() } -> std::convertible_to<std::string>;
    };

/// POSIX `pwrite(2)`-backed sink. Lock-free in the steady state:
/// callers reserve a disjoint offset range via the atomic counter,
/// then pwrite into it without contention. Construct via `try_open`
/// to surface open-time errors via `std::expected`.
class posix_pwrite_sink {
  public:
    posix_pwrite_sink() = default;

    posix_pwrite_sink(posix_pwrite_sink&& other) noexcept
        : _fd(other._fd), _next_offset(other._next_offset.load(std::memory_order_relaxed)),
          _last_error_message(std::move(other._last_error_message)) {
        other._fd = -1;
    }

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

    posix_pwrite_sink(const posix_pwrite_sink&) = delete;
    posix_pwrite_sink& operator=(const posix_pwrite_sink&) = delete;

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

    /// Reserve a disjoint byte range for the caller. Returns the base
    /// offset of the range. The caller commits the bytes through
    /// `write_at(bytes, byte_count, base)`. The reservation is lock-free
    /// — every concurrent caller gets a unique range.
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

    /// Close the file descriptor without calling `fsync`. The call is
    /// idempotent, and callers control any durability barrier separately.
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

    /// Total byte count reserved so far. After every producer's
    /// final commit this equals the on-disk file size.
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

/// mmap-backed sink. Bypasses the kernel `pwrite` memcpy by writing
/// directly into a memory-mapped file region. Each producer's
/// `write_at` becomes a userspace `memcpy` into the mapped region.
/// the kernel marks pages dirty and writes back asynchronously.
///
/// Requires a size hint at open time — the file is `ftruncate`d to
/// the hint and the whole region mapped at once. After close, the
/// file is `ftruncate`d to the actual bytes written. The hint should
/// be a generous upper bound. Over-estimation costs only sparse-file
/// metadata, not data.
class mmap_pwrite_sink {
  public:
    mmap_pwrite_sink() = default;

    mmap_pwrite_sink(mmap_pwrite_sink&& other) noexcept
        : _fd(other._fd), _addr(other._addr), _capacity(other._capacity),
          _next_offset(other._next_offset.load(std::memory_order_relaxed)),
          _last_error_message(std::move(other._last_error_message)) {
        other._fd = -1;
        other._addr = nullptr;
        other._capacity = 0;
    }

    mmap_pwrite_sink(const mmap_pwrite_sink&) = delete;
    mmap_pwrite_sink& operator=(const mmap_pwrite_sink&) = delete;

    ~mmap_pwrite_sink() {
        cleanup();
    }

    /// Open `path` for writing, ftruncate to `capacity_hint`, and
    /// mmap the region with PROT_WRITE.
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

    /// Reserve `byte_count` bytes for a subsequent `write_at`. The
    /// reservation atomically advances the global offset and returns
    /// the base of the claimed range. If the reservation would
    /// overflow the mmap'd capacity, the offset still advances (so
    /// concurrent reservations remain disjoint) — `write_at` then
    /// catches the overflow and stamps an error. Atomicity:
    /// Relaxed ordering suffices because the counter only assigns
    /// disjoint ranges. Byte publication does not depend on it.
    [[nodiscard]] std::uint64_t reserve_offset(std::size_t byte_count) noexcept {
        return _next_offset.fetch_add(static_cast<std::uint64_t>(byte_count),
                                      std::memory_order_relaxed);
    }

    /// Write `size` bytes at `offset` via memcpy into the mapped
    /// region. Caller MUST have called `reserve_offset(size)` to
    /// claim this range. Overflow of the mmap'd capacity is caught
    /// here and surfaced via `sink_write_result::has_error` — the offset
    /// counter still advanced for concurrent callers but no memory
    /// is touched, so the file is left valid up to the last
    /// successful write.
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

    /// Unmap the sink, truncate it to the written size, and close it.
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

    /// Return the most recent mapping or write diagnostic.
    [[nodiscard]] std::string last_error_message() const {
        return _last_error_message;
    }

  private:
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
