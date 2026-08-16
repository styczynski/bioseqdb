/**
 * @file sources.cppm
 * @brief POSIX, memory-mapped, and gzip byte sources.
 */
module;

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>

export module bioseq.io.sources;

import bioseq.common.logging;
import bioseq.io.options;

// Re-export the source protocol with its concrete implementations.
export import bioseq.io.source;

export namespace bioseq::io {

/// Strip the UTF-8 BOM `0xEF 0xBB 0xBF` if it appears at the start of
/// `dst`. Returns the number of bytes the cursor should advance past
/// (0 or 3). Used by every source's first `read_into` call.
[[nodiscard]] inline std::size_t strip_leading_bom(const std::uint8_t* dst,
                                                   std::size_t size) noexcept {
    if (size >= 3 && dst[0] == 0xEF && dst[1] == 0xBB && dst[2] == 0xBF) {
        return 3;
    }
    return 0;
}

/// POSIX `read(2)`-backed source. Construct via `try_open` to surface
/// open-time errors via `std::expected`.
class posix_read_source {
  public:
    /// Construct a closed source without an owned file descriptor.
    posix_read_source() = default;

    /// Transfer the file descriptor, offset, and diagnostic state.
    posix_read_source(posix_read_source&& other) noexcept
        : _fd(other._fd), _global_offset(other._global_offset), _bom_consumed(other._bom_consumed),
          _last_error_message(std::move(other._last_error_message)) {
        other._fd = -1;
    }

    /// Replace this source with transferred descriptor and cursor state.
    posix_read_source& operator=(posix_read_source&& other) noexcept {
        if (this != &other) {
            close_fd();
            _fd = other._fd;
            _global_offset = other._global_offset;
            _bom_consumed = other._bom_consumed;
            _last_error_message = std::move(other._last_error_message);
            other._fd = -1;
        }
        return *this;
    }

    /// Sources exclusively own their file descriptor.
    posix_read_source(const posix_read_source&) = delete;

    /// Sources cannot copy file-descriptor ownership.
    posix_read_source& operator=(const posix_read_source&) = delete;

    /// Close the owned file descriptor.
    ~posix_read_source() {
        close_fd();
    }

    /// Open `path` as a sequential read-only byte source.
    [[nodiscard]] static std::expected<posix_read_source, std::string>
    try_open(std::string_view path) {
        const std::string path_str(path);
        const int fd = ::open(path_str.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(fmt::format("cannot open '{}': {}", path, std::strerror(errno)));
        }
#ifndef __APPLE__
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
        posix_read_source source;
        source._fd = fd;
        return source;
    }

    /// Read at most `cap` bytes, stripping a leading UTF-8 BOM once.
    [[nodiscard]] source_read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        if (_fd < 0) {
            _last_error_message = "posix_read_source: file descriptor closed";
            return source_read_result{0, false, true};
        }
        ssize_t bytes_read = 0;
        for (;;) {
            bytes_read = ::read(_fd, dst, cap);
            if (bytes_read >= 0)
                break;
            if (errno == EINTR)
                continue;
            _last_error_message = fmt::format(
                "read error at file position {}: {}", _global_offset, std::strerror(errno));
            return source_read_result{0, false, true};
        }
        if (bytes_read == 0) {
            return source_read_result{0, true, false};
        }
        std::size_t scaled = static_cast<std::size_t>(bytes_read);
        if (!_bom_consumed && _global_offset == 0) {
            const std::size_t bom_advance = strip_leading_bom(dst, scaled);
            if (bom_advance > 0 && bom_advance <= scaled) {
                std::memmove(dst, dst + bom_advance, scaled - bom_advance);
                scaled -= bom_advance;
            }
            _bom_consumed = true;
        }
        _global_offset += static_cast<std::size_t>(bytes_read);
        return source_read_result{scaled, false, false};
    }

    /// Return the most recent read diagnostic.
    [[nodiscard]] std::string last_error_message() const {
        return _last_error_message;
    }

    /// Return the number of source bytes consumed, including any BOM.
    [[nodiscard]] std::size_t global_offset() const noexcept {
        return _global_offset;
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
    std::size_t _global_offset = 0;
    bool _bom_consumed = false;
    std::string _last_error_message;
};

/// Mapping-backed source returned alongside its initial magic-byte probe.
struct mmap_source;

/// Result of `mmap_source::try_open` when extended with magic-byte
/// sniffing. Carries the freshly-mapped source plus the
/// 2-byte magic check + total file size so the dispatcher can route
/// gzipped files to the parallel decoder without a second `open`.
struct mmap_source_with_magic;

/// `mmap(2)`-backed source. `read_into` copies mapped bytes into a slab.
/// `direct_view` exposes the mapped bytes to the synchronous direct path.
/// Compressed input is copied through its decompressor.
class mmap_source {
  public:
    /// Construct an empty source without a mapping.
    mmap_source() = default;

    /// Transfer mapping ownership and the current read cursor.
    mmap_source(mmap_source&& other) noexcept
        : _addr(other._addr), _size(other._size), _cursor(other._cursor), _fd(other._fd),
          _bom_consumed(other._bom_consumed),
          _last_error_message(std::move(other._last_error_message)) {
        other._addr = nullptr;
        other._fd = -1;
    }

    /// Replace this source with transferred mapping and cursor state.
    mmap_source& operator=(mmap_source&& other) noexcept {
        if (this != &other) {
            release();
            _addr = other._addr;
            _size = other._size;
            _cursor = other._cursor;
            _fd = other._fd;
            _bom_consumed = other._bom_consumed;
            _last_error_message = std::move(other._last_error_message);
            other._addr = nullptr;
            other._fd = -1;
        }
        return *this;
    }

    /// Sources exclusively own their mapping and file descriptor.
    mmap_source(const mmap_source&) = delete;

    /// Sources cannot copy mapping ownership.
    mmap_source& operator=(const mmap_source&) = delete;

    /// Unmap the owned byte region and close its descriptor.
    ~mmap_source() {
        release();
    }

    /// Open a mapped source without returning gzip detection metadata.
    [[nodiscard]] static std::expected<mmap_source, std::string>
    try_open(std::string_view path, const reader_options& options);

    /// Magic-byte-aware open. Reads the first 2 bytes via
    /// `pread(2)` after `fstat` and returns the source bundled with
    /// the gzip-magic flag and the file size. Used by the dispatcher
    /// to route gzipped files to the parallel decoder without a
    /// second `open`.
    [[nodiscard]] static std::expected<mmap_source_with_magic, std::string>
    try_open_with_magic(std::string_view path, const reader_options& options);

    /// Copy at most `cap` mapped bytes, stripping a leading UTF-8 BOM once.
    [[nodiscard]] source_read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        const std::size_t available = (_cursor < _size) ? (_size - _cursor) : 0;
        const std::size_t copy_size = std::min(cap, available);
        if (copy_size == 0) {
            return source_read_result{0, true, false};
        }
        std::memcpy(dst, static_cast<const std::uint8_t*>(_addr) + _cursor, copy_size);
        _cursor += copy_size;
        std::size_t scaled = copy_size;
        if (!_bom_consumed && _cursor == copy_size) {
            const std::size_t bom_advance = strip_leading_bom(dst, scaled);
            if (bom_advance > 0 && bom_advance <= scaled) {
                std::memmove(dst, dst + bom_advance, scaled - bom_advance);
                scaled -= bom_advance;
            }
            _bom_consumed = true;
        }
        return source_read_result{scaled, _cursor >= _size, false};
    }

    /// Return the most recent mapping or read diagnostic.
    [[nodiscard]] std::string last_error_message() const {
        return _last_error_message;
    }

    /// Direct view of the mapped region. Used by the zero-copy mmap
    /// batch pipeline to
    /// hand the parser the whole file in one contiguous span without
    /// the slab refill / `make_room` memmove overhead. The leading
    /// UTF-8 BOM is stripped from the returned `(ptr, size)` pair.
    [[nodiscard]] std::pair<const std::uint8_t*, std::size_t> direct_view() noexcept {
        const std::uint8_t* base = static_cast<const std::uint8_t*>(_addr);
        std::size_t total = _size;
        if (!_bom_consumed && total >= 3 && base[0] == 0xEF && base[1] == 0xBB && base[2] == 0xBF) {
            base += 3;
            total -= 3;
        }
        _bom_consumed = true;
        _cursor = _size; // mark fully consumed
        return {base, total};
    }

    /// Total mapped size, for reservation-hint purposes.
    [[nodiscard]] std::size_t mapped_size() const noexcept {
        return _size;
    }

  private:
    /// Release the mapping before closing its backing descriptor.
    void release() noexcept {
        if (_addr != nullptr) {
            ::munmap(_addr, _size);
            _addr = nullptr;
        }
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    void* _addr = nullptr;
    std::size_t _size = 0;
    std::size_t _cursor = 0;
    int _fd = -1;
    bool _bom_consumed = false;
    std::string _last_error_message;
};

/// Magic-byte sniff result. The reader dispatcher uses
/// this to route gzipped files to the parallel decoder. Aggregate so
/// `std::expected` can move-construct it from designated-init.
struct mmap_source_with_magic {
    mmap_source source;
    bool is_gzipped = false;
    std::size_t file_size = 0;
};

[[nodiscard]] inline std::expected<mmap_source, std::string>
mmap_source::try_open(std::string_view path, const reader_options& options) {
    auto with_magic = try_open_with_magic(path, options);
    if (!with_magic) {
        return std::unexpected(std::move(with_magic.error()));
    }
    return std::move(with_magic->source);
}

[[nodiscard]] inline std::expected<mmap_source_with_magic, std::string>
mmap_source::try_open_with_magic(std::string_view path, const reader_options& options) {
    const std::string path_str(path);
    const int fd = ::open(path_str.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(fmt::format("cannot open '{}': {}", path, std::strerror(errno)));
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int saved = errno;
        ::close(fd);
        return std::unexpected(fmt::format("fstat failed on '{}': {}", path, std::strerror(saved)));
    }
    if (st.st_size <= 0 || static_cast<std::size_t>(st.st_size) > options.io.max_mmap_bytes) {
        ::close(fd);
        return std::unexpected(
            std::string{"file outside mmap range; caller should use posix_read_source"});
    }

    std::array<std::uint8_t, 2> magic_bytes{};
    if (st.st_size >= 2) {
        const ssize_t read_count = ::pread(fd, magic_bytes.data(), 2, 0);
        if (read_count < 0) {
            const int saved = errno;
            ::close(fd);
            return std::unexpected(
                fmt::format("magic-byte pread failed on '{}': {}", path, std::strerror(saved)));
        }
    }

    const std::size_t mapped_size = static_cast<std::size_t>(st.st_size);
    void* const addr = ::mmap(nullptr, mapped_size, PROT_READ, MAP_PRIVATE | MAP_FILE, fd, 0);
    if (addr == MAP_FAILED) {
        const int saved = errno;
        ::close(fd);
        return std::unexpected(fmt::format("mmap failed for '{}': {}", path, std::strerror(saved)));
    }
    if (options.io.stream_madvise) {
        ::madvise(addr, mapped_size, MADV_SEQUENTIAL);
    }
    mmap_source source;
    source._addr = addr;
    source._size = mapped_size;
    source._fd = fd;

    mmap_source_with_magic out;
    out.source = std::move(source);
    out.is_gzipped = (st.st_size >= 2 && magic_bytes[0] == 0x1F && magic_bytes[1] == 0x8B);
    out.file_size = mapped_size;
    return out;
}

/// `gzread`-backed source. Uses zlib. zlib's `gzopen` transparently
/// handles plain (non-gzipped) files too, so this source doubles as a
/// "POSIX with auto-decompression".
class gzip_source {
  public:
    /// Construct a closed source without a zlib handle.
    gzip_source() = default;

    /// Transfer the zlib handle, offset, and diagnostic state.
    gzip_source(gzip_source&& other) noexcept
        : _file(other._file), _global_offset(other._global_offset),
          _bom_consumed(other._bom_consumed),
          _last_error_message(std::move(other._last_error_message)) {
        other._file = nullptr;
    }

    /// Replace this source with transferred zlib state.
    gzip_source& operator=(gzip_source&& other) noexcept {
        if (this != &other) {
            release();
            _file = other._file;
            _global_offset = other._global_offset;
            _bom_consumed = other._bom_consumed;
            _last_error_message = std::move(other._last_error_message);
            other._file = nullptr;
        }
        return *this;
    }

    /// Sources exclusively own their zlib handle.
    gzip_source(const gzip_source&) = delete;

    /// Sources cannot copy zlib-handle ownership.
    gzip_source& operator=(const gzip_source&) = delete;

    /// Close the owned zlib handle.
    ~gzip_source() {
        release();
    }

    /// Open `path` through zlib's transparent gzip reader.
    [[nodiscard]] static std::expected<gzip_source, std::string> try_open(std::string_view path) {
        const std::string path_str(path);
        gzFile handle = ::gzopen(path_str.c_str(), "rb");
        if (handle == nullptr) {
            return std::unexpected(
                fmt::format("cannot open gzip file '{}': {}", path, std::strerror(errno)));
        }
        gzip_source source;
        source._file = handle;
        return source;
    }

    /// Read at most `cap` decompressed bytes and report zlib failures.
    [[nodiscard]] source_read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        if (_file == nullptr) {
            _last_error_message = "gzip_source: file handle closed";
            return source_read_result{0, false, true};
        }
        const unsigned to_read =
            static_cast<unsigned>(std::min<std::size_t>(
                cap, static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())));
        const int read_count = ::gzread(_file, dst, to_read);
        if (read_count < 0) {
            int errnum = 0;
            const char* message = ::gzerror(_file, &errnum);
            _last_error_message = fmt::format("gzip error at file position {}: {} (errno={})",
                                              _global_offset,
                                              (message != nullptr ? message : "<null>"),
                                              errnum);
            return source_read_result{0, false, true};
        }
        if (read_count == 0) {
            int errnum = 0;
            ::gzerror(_file, &errnum);
            if (errnum != Z_OK && errnum != Z_STREAM_END) {
                _last_error_message = fmt::format(
                    "gzip stream error at file position {}: errno={}", _global_offset, errnum);
                return source_read_result{0, false, true};
            }
            return source_read_result{0, true, false};
        }
        std::size_t scaled = static_cast<std::size_t>(read_count);
        if (!_bom_consumed && _global_offset == 0) {
            const std::size_t bom_advance = strip_leading_bom(dst, scaled);
            if (bom_advance > 0 && bom_advance <= scaled) {
                std::memmove(dst, dst + bom_advance, scaled - bom_advance);
                scaled -= bom_advance;
            }
            _bom_consumed = true;
        }
        _global_offset += static_cast<std::size_t>(read_count);
        return source_read_result{scaled, false, false};
    }

    /// Return the most recent zlib diagnostic.
    [[nodiscard]] std::string last_error_message() const {
        return _last_error_message;
    }

  private:
    /// Close the zlib handle when one is active.
    void release() noexcept {
        if (_file != nullptr) {
            ::gzclose(_file);
            _file = nullptr;
        }
    }

    gzFile _file = nullptr;
    std::size_t _global_offset = 0;
    bool _bom_consumed = false;
    std::string _last_error_message;
};

} // namespace bioseq::io
