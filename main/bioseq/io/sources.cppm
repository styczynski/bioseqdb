module;

#include "common.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

export module bioseq_io.sources;

import mlogging;
import bioseq_io.options;

export namespace bioseq::io {

/// Result of one `read_into` call on a source.
struct read_result {
    std::size_t bytes_read = 0;
    bool        is_eof = false;
    bool        has_error = false;
};

/// Concept: an I/O source supplies bytes via `read_into(dst, cap)` and
/// surfaces errors via `last_error_message()`. Sources are monomorphic
/// per template instantiation; no virtuals.
template <typename source_t>
concept io_source = requires(source_t& source, std::uint8_t* dst, std::size_t cap) {
    { source.read_into(dst, cap) } -> std::same_as<read_result>;
    { source.last_error_message() } -> std::convertible_to<std::string>;
};

/// Strip the UTF-8 BOM `0xEF 0xBB 0xBF` if it appears at the start of
/// `dst`. Returns the number of bytes the cursor should advance past
/// (0 or 3). Used by every source's first `read_into` call. Spec §16.2.
[[nodiscard]] inline std::size_t strip_leading_bom(const std::uint8_t* dst,
                                                   std::size_t size) noexcept {
    if (size >= 3 && dst[0] == 0xEF && dst[1] == 0xBB && dst[2] == 0xBF) {
        return 3;
    }
    return 0;
}

/// POSIX `read(2)`-backed source. Construct via `try_open` to surface
/// open-time errors via `std::expected`. Spec Appendix J / §6.2.
class posix_read_source {
public:
    posix_read_source() = default;

    posix_read_source(posix_read_source&& other) noexcept
        : _fd(other._fd),
          _global_offset(other._global_offset),
          _bom_consumed(other._bom_consumed),
          _last_error_message(std::move(other._last_error_message)) {
        other._fd = -1;
    }

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

    posix_read_source(const posix_read_source&) = delete;
    posix_read_source& operator=(const posix_read_source&) = delete;

    ~posix_read_source() { close_fd(); }

    [[nodiscard]] static std::expected<posix_read_source, std::string> try_open(
        std::string_view path) {
        const std::string path_str(path);
        const int fd = ::open(path_str.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(fmt::format(
                "cannot open '{}': {}", path, std::strerror(errno)));
        }
        #ifndef __APPLE__
            ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        #endif
        posix_read_source source;
        source._fd = fd;
        return source;
    }

    [[nodiscard]] read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        if (_fd < 0) {
            _last_error_message = "posix_read_source: file descriptor closed";
            return read_result{0, false, true};
        }
        ssize_t bytes_read = 0;
        for (;;) {
            bytes_read = ::read(_fd, dst, cap);
            if (bytes_read >= 0) break;
            if (errno == EINTR) continue;
            _last_error_message = fmt::format(
                "read error at file position {}: {}",
                _global_offset, std::strerror(errno));
            return read_result{0, false, true};
        }
        if (bytes_read == 0) {
            return read_result{0, true, false};
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
        return read_result{scaled, false, false};
    }

    [[nodiscard]] std::string last_error_message() const { return _last_error_message; }

    [[nodiscard]] std::size_t global_offset() const noexcept { return _global_offset; }

private:
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

/// `mmap(2)`-backed source. The file is mapped read-only and `read_into`
/// performs a `memcpy` into the caller's slab. The zero-copy direct
/// mode (parser sees mmap'd bytes directly) is a future optimisation —
/// for v1 we always copy. Spec Appendix L / §6.3.
class mmap_source {
public:
    mmap_source() = default;

    mmap_source(mmap_source&& other) noexcept
        : _addr(other._addr),
          _size(other._size),
          _cursor(other._cursor),
          _fd(other._fd),
          _bom_consumed(other._bom_consumed),
          _last_error_message(std::move(other._last_error_message)) {
        other._addr = nullptr;
        other._fd = -1;
    }

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

    mmap_source(const mmap_source&) = delete;
    mmap_source& operator=(const mmap_source&) = delete;

    ~mmap_source() { release(); }

    [[nodiscard]] static std::expected<mmap_source, std::string> try_open(
        std::string_view path,
        const reader_options& options) {
        const std::string path_str(path);
        const int fd = ::open(path_str.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(fmt::format(
                "cannot open '{}': {}", path, std::strerror(errno)));
        }
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            const int saved = errno;
            ::close(fd);
            return std::unexpected(fmt::format(
                "fstat failed on '{}': {}", path, std::strerror(saved)));
        }
        if (st.st_size <= 0 ||
            static_cast<std::size_t>(st.st_size) > options.io.max_mmap_bytes) {
            ::close(fd);
            return std::unexpected(std::string{
                "file outside mmap range; caller should use posix_read_source"});
        }
        const std::size_t mapped_size = static_cast<std::size_t>(st.st_size);
        void* const addr = ::mmap(nullptr, mapped_size,
                                  PROT_READ, MAP_PRIVATE | MAP_FILE, fd, 0);
        if (addr == MAP_FAILED) {
            const int saved = errno;
            ::close(fd);
            return std::unexpected(fmt::format(
                "mmap failed for '{}': {}", path, std::strerror(saved)));
        }
        if (options.io.stream_madvise) {
            ::madvise(addr, mapped_size, MADV_SEQUENTIAL);
        }
        mmap_source source;
        source._addr = addr;
        source._size = mapped_size;
        source._fd = fd;
        return source;
    }

    [[nodiscard]] read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        const std::size_t available = (_cursor < _size) ? (_size - _cursor) : 0;
        const std::size_t copy_size = std::min(cap, available);
        if (copy_size == 0) {
            return read_result{0, true, false};
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
        return read_result{scaled, _cursor >= _size, false};
    }

    [[nodiscard]] std::string last_error_message() const { return _last_error_message; }

private:
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

/// `gzread`-backed source. Uses zlib. zlib's `gzopen` transparently
/// handles plain (non-gzipped) files too, so this source doubles as a
/// "POSIX with auto-decompression". Spec Appendix K / §6.4.
class gzip_source {
public:
    gzip_source() = default;

    gzip_source(gzip_source&& other) noexcept
        : _file(other._file),
          _global_offset(other._global_offset),
          _bom_consumed(other._bom_consumed),
          _last_error_message(std::move(other._last_error_message)) {
        other._file = nullptr;
    }

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

    gzip_source(const gzip_source&) = delete;
    gzip_source& operator=(const gzip_source&) = delete;

    ~gzip_source() { release(); }

    [[nodiscard]] static std::expected<gzip_source, std::string> try_open(
        std::string_view path) {
        const std::string path_str(path);
        gzFile handle = ::gzopen(path_str.c_str(), "rb");
        if (handle == nullptr) {
            return std::unexpected(fmt::format(
                "cannot open gzip file '{}': {}", path,
                std::strerror(errno)));
        }
        gzip_source source;
        source._file = handle;
        return source;
    }

    [[nodiscard]] read_result read_into(std::uint8_t* dst, std::size_t cap) noexcept {
        if (_file == nullptr) {
            _last_error_message = "gzip_source: file handle closed";
            return read_result{0, false, true};
        }
        const unsigned to_read = static_cast<unsigned>(
            std::min<std::size_t>(cap, static_cast<std::size_t>(UINT_MAX)));
        const int n = ::gzread(_file, dst, to_read);
        if (n < 0) {
            int errnum = 0;
            const char* message = ::gzerror(_file, &errnum);
            _last_error_message = fmt::format(
                "gzip error at file position {}: {} (errno={})",
                _global_offset,
                (message != nullptr ? message : "<null>"),
                errnum);
            return read_result{0, false, true};
        }
        if (n == 0) {
            int errnum = 0;
            ::gzerror(_file, &errnum);
            if (errnum != Z_OK && errnum != Z_STREAM_END) {
                _last_error_message = fmt::format(
                    "gzip stream error at file position {}: errno={}",
                    _global_offset, errnum);
                return read_result{0, false, true};
            }
            return read_result{0, true, false};
        }
        std::size_t scaled = static_cast<std::size_t>(n);
        if (!_bom_consumed && _global_offset == 0) {
            const std::size_t bom_advance = strip_leading_bom(dst, scaled);
            if (bom_advance > 0 && bom_advance <= scaled) {
                std::memmove(dst, dst + bom_advance, scaled - bom_advance);
                scaled -= bom_advance;
            }
            _bom_consumed = true;
        }
        _global_offset += static_cast<std::size_t>(n);
        return read_result{scaled, false, false};
    }

    [[nodiscard]] std::string last_error_message() const { return _last_error_message; }

private:
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

}  // namespace bioseq::io
