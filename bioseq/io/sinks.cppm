/** @file Checked positioned IO and atomic publication of private staging files. */
module;

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <vector>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <oneapi/tbb/spin_mutex.h>

export module bioseq.io.sinks;

import bioseq.common.logging;
import bioseq.parallelism.memory_admission;

export namespace bioseq::io {

/** Share one bounded, already-admitted tranche across a quiescent IO wave.
 * Actual allocations always take independent owned permits. The bank is stable
 * and outlives every borrower; a short TBB lock protects only unused credit.
 */
class io_memory_credit {
    parallelism::memory_permit permit_;
    oneapi::tbb::spin_mutex mutex_;
public:
    /** Start empty; absence of speculative admission preserves the exact path. */
    io_memory_credit() = default;
    /** Adopt one existing reservation without sampling memory again. */
    explicit io_memory_credit(parallelism::memory_permit permit) : permit_(std::move(permit)) {}
    io_memory_credit(const io_memory_credit&) = delete;
    io_memory_credit& operator=(const io_memory_credit&) = delete;
    io_memory_credit(io_memory_credit&&) = delete;
    io_memory_credit& operator=(io_memory_credit&&) = delete;

    /** Replace spare credit only between waves, after all previous borrowers stop. */
    void reset(parallelism::memory_permit permit) {
        const oneapi::tbb::spin_mutex::scoped_lock lock{mutex_};
        permit_ = std::move(permit);
    }

    /** Release all unused speculation before external callbacks or abandonment. */
    void release() {
        const oneapi::tbb::spin_mutex::scoped_lock lock{mutex_};
        permit_.release();
    }

    /** Split exact owned bytes, or drop all spare before the caller's normal fallback.
     * No host sampling, allocation, or controller reservation runs under this lock.
     */
    [[nodiscard]] std::optional<parallelism::memory_permit> take(
        const std::uint64_t bytes, parallelism::memory_admission_controller& controller) {
        const oneapi::tbb::spin_mutex::scoped_lock lock{mutex_};
        if (permit_.controller() == &controller && bytes <= permit_.bytes())
            return permit_.split_off(bytes);
        permit_.release();
        return std::nullopt;
    }
};

/** Acquire exact owned bytes from a shared tranche or the unchanged live controller. */
[[nodiscard]] inline std::optional<parallelism::memory_permit> reserve_io_memory(
    const std::uint64_t bytes, io_memory_credit* credit = nullptr) {
    auto& controller = parallelism::memory_admission_controller::instance();
    if (credit != nullptr)
        if (auto reserved = credit->take(bytes, controller); reserved) return reserved;
    return controller.try_reserve(bytes);
}

/** Grow an actual allocation's owned permit while preserving its existing peak charge.
 * Only the additional bytes transfer from the bank. Exhaustion releases all spare
 * before exact global admission, preventing another row's speculation from blocking it.
 */
[[nodiscard]] inline bool grow_io_memory(parallelism::memory_permit& owner,
    const std::uint64_t wanted, io_memory_credit* credit = nullptr) {
    if (wanted <= owner.bytes()) return true;
    if (!owner) {
        auto reserved = reserve_io_memory(wanted, credit);
        if (!reserved) return false;
        owner = std::move(*reserved);
        return true;
    }
    if (credit != nullptr)
        if (auto extra = credit->take(wanted - owner.bytes(), *owner.controller()); extra)
            return owner.absorb(std::move(*extra));
    return owner.grow_to(wanted);
}

/** Optional caller-thread cancellation checkpoint; never copied into worker jobs.
 * The callback may unwind through its caller's existing error bridge. It must
 * remain valid until publication or abandonment; neutral callers leave it empty.
 */
struct io_caller_checkpoint {
    void* context{};
    void (*poll)(void*){};
    /** Invoke the caller's cancellation hook without introducing platform state. */
    void operator()() const { if (poll != nullptr) poll(context); }
};

/** Positioned output reports errors by value, with no shared mutable diagnostic. */
template<class Sink>
concept io_sink = requires(Sink& sink, std::span<const std::byte> bytes,
                           std::uint64_t offset) {
    { sink.write_at(bytes, offset) } -> std::same_as<std::expected<std::size_t, std::string>>;
    { sink.reserve_offset(bytes.size()) } -> std::same_as<std::expected<std::uint64_t, std::string>>;
};

/** Concurrent positional reads never mutate a shared file cursor. */
template<class Source>
concept positional_byte_source = requires(const Source& source,
        std::uint64_t offset, std::span<std::byte> destination) {
    { source.size_bytes() } -> std::same_as<std::uint64_t>;
    { source.read_at(offset, destination) }
        -> std::same_as<std::expected<std::size_t, std::string>>;
};

/** Bounded read-only mapping into a sealed private replay file; no copied payload. */
class mapped_read_window {
    void* mapping_{};
    std::size_t mapped_bytes_{}, prefix_{}, bytes_{};
public:
    /** Construct an empty mapping for an empty requested window. */
    mapped_read_window() = default;
    mapped_read_window(const mapped_read_window&) = delete;
    mapped_read_window& operator=(const mapped_read_window&) = delete;
    /** Transfer one mapping without changing any borrowed byte addresses. */
    mapped_read_window(mapped_read_window&& other) noexcept
        : mapping_{std::exchange(other.mapping_, nullptr)}, mapped_bytes_{std::exchange(other.mapped_bytes_, 0u)},
          prefix_{other.prefix_}, bytes_{other.bytes_} {}
    /** Replace a stopped mapping and transfer ownership without changing source addresses. */
    mapped_read_window& operator=(mapped_read_window&& other) noexcept {
        if (this != &other) {
            if (mapping_ != nullptr) ::munmap(mapping_, mapped_bytes_);
            mapping_ = std::exchange(other.mapping_, nullptr);
            mapped_bytes_ = std::exchange(other.mapped_bytes_, 0u);
            prefix_ = other.prefix_;
            bytes_ = other.bytes_;
        }
        return *this;
    }
    /** Release one read-only view; the separate replay owner retains its file. */
    ~mapped_read_window() { if (mapping_ != nullptr) ::munmap(mapping_, mapped_bytes_); }

    /** Map an already range-checked file interval using the OS page alignment. */
    [[nodiscard]] static std::expected<mapped_read_window, std::string> open(
            int descriptor, std::uint64_t offset, std::size_t bytes) {
        mapped_read_window result;
        if (bytes == 0u) return result;
        const auto page_size = ::sysconf(_SC_PAGESIZE);
        if (page_size <= 0) return std::unexpected(std::string{"cannot query mapping page size"});
        const auto prefix = static_cast<std::size_t>(offset % static_cast<std::uint64_t>(page_size));
        if (bytes > std::numeric_limits<std::size_t>::max() - prefix
            || offset - prefix > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            return std::unexpected(std::string{"replay mapping geometry overflows"});
        result.mapped_bytes_ = bytes + prefix;
        result.prefix_ = prefix;
        result.bytes_ = bytes;
        result.mapping_ = ::mmap(nullptr, result.mapped_bytes_, PROT_READ, MAP_PRIVATE,
            descriptor, static_cast<off_t>(offset - prefix));
        if (result.mapping_ == MAP_FAILED) {
            result.mapping_ = nullptr;
            return std::unexpected(fmt::format("map replay window failed: {}", std::strerror(errno)));
        }
        return result;
    }

    /** Borrow the exact requested bytes rather than the aligned prefix. */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        if (mapping_ == nullptr) return {};
        return {static_cast<const std::byte*>(mapping_) + prefix_, bytes_};
    }
};

/** A private sibling file; only explicit publication exposes the completed output.
 * Writes may run concurrently at disjoint ranges; reservation ordering belongs
 * to the caller. Seal/publication/destruction require all tasks to have joined.
 */
class posix_pwrite_sink {
    int descriptor_{-1};
    int directory_descriptor_{-1};
    std::string temporary_name_;
    std::string destination_name_;
    std::atomic<std::uint64_t> extent_{};
    std::optional<std::uint64_t> maximum_output_bytes_;
    bool overwrite_{};
    bool sealed_{};
    bool published_{};

    /** Report one saved errno without reading shared diagnostic state. */
    [[nodiscard]] static std::string system_error(std::string_view operation, int error) {
        return fmt::format("{} failed: {}", operation, std::strerror(error));
    }

    /** Check the endpoint before any signed offset conversion or syscall. */
    [[nodiscard]] static bool representable_range(std::uint64_t offset, std::size_t count) {
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        return offset <= maximum && count <= maximum - offset;
    }

    /** Release unpublished resources; never turn destructor cleanup into publication. */
    void cleanup() noexcept {
        if (descriptor_ >= 0) { ::close(descriptor_); descriptor_ = -1; }
        if (directory_descriptor_ >= 0) {
            if (!temporary_name_.empty())
                ::unlinkat(directory_descriptor_, temporary_name_.c_str(), 0);
            ::close(directory_descriptor_);
            directory_descriptor_ = -1;
        }
    }

public:
    /** Construct a closed owner without side effects. */
    posix_pwrite_sink() = default;
    posix_pwrite_sink(const posix_pwrite_sink&) = delete;
    posix_pwrite_sink& operator=(const posix_pwrite_sink&) = delete;

    /** Transfer open file and cleanup responsibility after workers have joined. */
    posix_pwrite_sink(posix_pwrite_sink&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)),
          directory_descriptor_(std::exchange(other.directory_descriptor_, -1)),
          temporary_name_(std::move(other.temporary_name_)),
          destination_name_(std::move(other.destination_name_)),
          extent_(other.extent_.load(std::memory_order_relaxed)),
          maximum_output_bytes_(other.maximum_output_bytes_),
          overwrite_(other.overwrite_), sealed_(other.sealed_), published_(other.published_) {}

    /** Abandon previous unpublished output and adopt a stopped source owner. */
    posix_pwrite_sink& operator=(posix_pwrite_sink&& other) noexcept {
        if (this != &other) {
            cleanup();
            descriptor_ = std::exchange(other.descriptor_, -1);
            directory_descriptor_ = std::exchange(other.directory_descriptor_, -1);
            temporary_name_ = std::move(other.temporary_name_);
            destination_name_ = std::move(other.destination_name_);
            extent_.store(other.extent_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            maximum_output_bytes_ = other.maximum_output_bytes_;
            overwrite_ = other.overwrite_;
            sealed_ = other.sealed_;
            published_ = other.published_;
        }
        return *this;
    }

    /** Abandon unfinished output after the caller has joined every dependent task. */
    ~posix_pwrite_sink() { cleanup(); }

    /** Create a mode-0600 sibling with exclusive creation and a pinned parent directory.
     * The final path is untouched until publish(), even if it already exists.
     * An optional byte limit bounds reservations; zero permits only empty output.
     */
    [[nodiscard]] static std::expected<posix_pwrite_sink, std::string>
    try_open(std::string_view path, bool overwrite = false,
            std::optional<std::uint64_t> maximum_output_bytes = std::nullopt) {
        if (maximum_output_bytes && !representable_range(*maximum_output_bytes, 0u))
            return std::unexpected("maximum_output_bytes exceeds the supported output offset range");
        if (path.empty() || path.find('\0') != std::string_view::npos)
            return std::unexpected("output path is empty or contains NUL");
        const auto slash = path.find_last_of('/');
        const auto basename = slash == std::string_view::npos ? path : path.substr(slash + 1);
        const auto parent = slash == std::string_view::npos ? std::string_view{"."}
            : slash == 0 ? std::string_view{"/"} : path.substr(0, slash);
        if (basename.empty() || basename == "." || basename == "..")
            return std::unexpected("output path does not name a regular file");
        const std::string parent_path{parent};
        const int directory = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) return std::unexpected(system_error("open output directory", errno));
        posix_pwrite_sink sink;
        sink.directory_descriptor_ = directory;
        sink.destination_name_ = basename;
        sink.overwrite_ = overwrite;
        sink.maximum_output_bytes_ = maximum_output_bytes;
        static std::atomic<std::uint64_t> next_temporary{};
        const auto epoch = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::size_t attempt = 0; attempt < 128; ++attempt) {
            const auto serial = next_temporary.fetch_add(1, std::memory_order_relaxed);
            sink.temporary_name_ = fmt::format(".bioseq-{}-{}-{}", ::getpid(), epoch, serial);
            sink.descriptor_ = ::openat(directory, sink.temporary_name_.c_str(),
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (sink.descriptor_ >= 0) return sink;
            const int error = errno;
            sink.temporary_name_.clear(); // an existing collision is not ours to unlink
            if (error != EEXIST)
                return std::unexpected(system_error("create private output", error));
        }
        return std::unexpected("could not allocate a unique private output name");
    }

    /** Reserve a checked extent in caller order; rejected bounds never advance it. */
    [[nodiscard]] std::expected<std::uint64_t, std::string>
    reserve_offset(std::size_t byte_count) {
        if (descriptor_ < 0 || sealed_) return std::unexpected("output is closed or sealed");
        auto offset = extent_.load(std::memory_order_relaxed);
        for (;;) {
            if (!representable_range(offset, byte_count))
                return std::unexpected("output file extent exceeds the supported offset range");
            if (maximum_output_bytes_ && (offset > *maximum_output_bytes_
                || byte_count > *maximum_output_bytes_ - offset))
                return std::unexpected("output file extent exceeds maximum_output_bytes");
            if (extent_.compare_exchange_weak(offset, offset + byte_count,
                    std::memory_order_relaxed, std::memory_order_relaxed)) return offset;
        }
    }

    /** Write a fully reserved range, handling interruptions and short writes. */
    [[nodiscard]] std::expected<std::size_t, std::string>
    write_at(std::span<const std::byte> bytes, std::uint64_t offset) {
        if (descriptor_ < 0 || sealed_) return std::unexpected("output is closed or sealed");
        const auto extent = extent_.load(std::memory_order_relaxed);
        if (!representable_range(offset, bytes.size()) || offset > extent || bytes.size() > extent - offset)
            return std::unexpected("positioned write lies outside its reserved output extent");
        std::size_t written{};
        constexpr auto maximum_write = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
        while (written < bytes.size()) {
            const auto remaining = bytes.size() - written;
            const auto count = remaining < maximum_write ? remaining : maximum_write;
            const auto result = ::pwrite(descriptor_, bytes.data() + written, count,
                                         static_cast<off_t>(offset + written));
            if (result < 0) {
                const int error = errno;
                if (error == EINTR) continue;
                return std::unexpected(system_error("pwrite", error));
            }
            if (result == 0) return std::unexpected("pwrite made no progress");
            written += static_cast<std::size_t>(result);
        }
        return written;
    }

    /** Append all bytes on the owning thread and return their replay offset. */
    [[nodiscard]] std::expected<std::uint64_t, std::string>
    append_bytes(std::span<const std::byte> bytes) {
        const auto offset = reserve_offset(bytes.size());
        if (!offset) return std::unexpected(offset.error());
        const auto written = write_at(bytes, *offset);
        if (!written) return std::unexpected(written.error());
        return *offset;
    }

    /** Read a sealed immutable range without mutating a shared file cursor. */
    [[nodiscard]] std::expected<std::size_t, std::string>
    read_at(std::uint64_t offset, std::span<std::byte> destination) const {
        if (descriptor_ < 0 || !sealed_) return std::unexpected("replay source is not sealed and open");
        const auto extent = size_bytes();
        if (!representable_range(offset, destination.size()) || offset > extent)
            return std::unexpected("replay read lies outside the file");
        const auto available = extent - offset;
        const auto wanted = available < destination.size()
            ? static_cast<std::size_t>(available) : destination.size();
        std::size_t consumed{};
        constexpr auto maximum_read = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
        while (consumed < wanted) {
            const auto remaining = wanted - consumed;
            const auto count = remaining < maximum_read ? remaining : maximum_read;
            const auto result = ::pread(descriptor_, destination.data() + consumed, count,
                                       static_cast<off_t>(offset + consumed));
            if (result < 0) {
                const int error = errno;
                if (error == EINTR) continue;
                return std::unexpected(system_error("pread", error));
            }
            if (result == 0) return std::unexpected("replay file ended before its sealed extent");
            consumed += static_cast<std::size_t>(result);
        }
        return consumed;
    }

    /** Borrow one immutable replay payload; the mapping never owns the file. */
    [[nodiscard]] std::expected<mapped_read_window, std::string> map_read_at(
            std::uint64_t offset, std::size_t bytes) const {
        if (!sealed_ || descriptor_ < 0 || offset > size_bytes() || bytes > size_bytes() - offset)
            return std::unexpected(std::string{"replay mapping lies outside a sealed file"});
        return mapped_read_window::open(descriptor_, offset, bytes);
    }

    /** Flush accepted bytes and freeze the file for independent replay readers. */
    [[nodiscard]] std::expected<void, std::string> seal() {
        if (sealed_) return {};
        if (descriptor_ < 0) return std::unexpected("cannot seal a closed output");
        if (::fsync(descriptor_) != 0) return std::unexpected(system_error("fsync output", errno));
        sealed_ = true;
        return {};
    }

    /** Publish only after all output tasks finish; default creates without replacement. */
    [[nodiscard]] std::expected<void, std::string> publish(io_caller_checkpoint checkpoint = {}) {
        if (published_) return {};
        const auto flushed = seal();
        if (!flushed) return flushed;
        const int closing = std::exchange(descriptor_, -1);
        if (::close(closing) != 0) return std::unexpected(system_error("close output", errno));
        // All work and file fsync are complete. A rejected checkpoint still owns
        // only the private name, so ordinary unwinding removes it safely.
        checkpoint();
        if (overwrite_) {
            if (::renameat(directory_descriptor_, temporary_name_.c_str(),
                           directory_descriptor_, destination_name_.c_str()) != 0)
                return std::unexpected(system_error("publish output", errno));
            temporary_name_.clear();
        } else {
            // Same-directory hard-link creation is an atomic no-replace publication.
            if (::linkat(directory_descriptor_, temporary_name_.c_str(),
                         directory_descriptor_, destination_name_.c_str(), 0) != 0)
                return std::unexpected(system_error("publish output without replacement", errno));
            if (::unlinkat(directory_descriptor_, temporary_name_.c_str(), 0) == 0)
                temporary_name_.clear();
        }
        published_ = true;
        if (::fsync(directory_descriptor_) != 0)
            return std::unexpected(system_error("output published but directory fsync", errno));
        return {};
    }

    /** Return reserved logical bytes, retained even after publication. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept {
        return extent_.load(std::memory_order_relaxed);
    }
};
static_assert(io_sink<posix_pwrite_sink>);
static_assert(positional_byte_source<posix_pwrite_sink>);

/** Admitted immutable bytes, spilling only when resident growth cannot be reserved.
 * Files use the existing exclusive private staging owner and disappear without publication.
 */
class admitted_byte_buffer {
    parallelism::memory_permit permit_;
    io_memory_credit* credit_{};
    std::vector<std::byte> bytes_;
    std::optional<posix_pwrite_sink> file_;
    std::optional<mapped_read_window> mapped_;
    std::size_t size_{};
    bool sealed_{};

    /** Keep current bytes resident when possible; otherwise transfer them once to private storage. */
    [[nodiscard]] std::expected<void, std::string> grow(std::size_t wanted) {
        if (file_ || wanted <= bytes_.capacity()) return {};
        const auto doubled = bytes_.capacity() > std::numeric_limits<std::size_t>::max() / 2u
            ? wanted : bytes_.capacity() * 2u;
        const auto capacity = std::max(std::max(wanted, doubled), std::size_t{64u});
        if (capacity > std::numeric_limits<std::size_t>::max() - bytes_.capacity())
            return std::unexpected("byte-buffer growth accounting overflows");
        const auto peak = capacity + bytes_.capacity();
        bool admitted = grow_io_memory(permit_, peak, credit_);
        if (admitted) { bytes_.reserve(capacity); permit_.shrink_to(bytes_.capacity()); return {}; }
        if (bytes_.capacity() > std::numeric_limits<std::size_t>::max() - 4096u)
            return std::unexpected("byte-buffer spill accounting overflows");
        const auto spill_peak = bytes_.capacity() + 4096u;
        admitted = grow_io_memory(permit_, spill_peak, credit_);
        if (!admitted) return std::unexpected("cannot admit private byte-buffer ownership");
        auto opened = posix_pwrite_sink::try_open("/tmp/bioseq_byte_spool", true);
        if (!opened) return std::unexpected(opened.error());
        if (!bytes_.empty()) {
            const auto written = opened->append_bytes(bytes_);
            if (!written) return std::unexpected(written.error());
        }
        file_.emplace(std::move(*opened));
        std::vector<std::byte>{}.swap(bytes_);
        permit_.shrink_to(4096u);
        return {};
    }
public:
    /** Start empty; no allocation or IO occurs until bytes arrive. */
    admitted_byte_buffer() = default;
    /** Borrow stable spare admission while retaining every allocation's own permit. */
    explicit admitted_byte_buffer(io_memory_credit* credit) : credit_(credit) {}
    admitted_byte_buffer(const admitted_byte_buffer&) = delete;
    admitted_byte_buffer& operator=(const admitted_byte_buffer&) = delete;
    admitted_byte_buffer(admitted_byte_buffer&&) noexcept = default;
    /** Release previous storage before its permit, then adopt a stopped owner's storage. */
    admitted_byte_buffer& operator=(admitted_byte_buffer&& other) noexcept {
        if (this != &other) {
            mapped_.reset();
            file_.reset();
            std::vector<std::byte>{}.swap(bytes_);
            permit_.release();
            permit_ = std::move(other.permit_);
            credit_ = other.credit_;
            bytes_ = std::move(other.bytes_);
            file_ = std::move(other.file_);
            mapped_ = std::move(other.mapped_);
            size_ = other.size_;
            sealed_ = other.sealed_;
        }
        return *this;
    }

    /** Append synchronous bytes after successful existing-ledger admission. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::byte> bytes) {
        if (sealed_ || bytes.size() > std::numeric_limits<std::size_t>::max() - size_)
            return std::unexpected("private byte-buffer size overflows or is already sealed");
        if (bytes.empty()) return {};
        if (auto reserved = grow(size_ + bytes.size()); !reserved) return reserved;
        if (file_) {
            const auto written = file_->append_bytes(bytes);
            if (!written) return std::unexpected(written.error());
        } else bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        size_ += bytes.size();
        return {};
    }
    /** Text and binary consumers share one byte owner and admission path. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view bytes) {
        return append(std::as_bytes(std::span{bytes.data(), bytes.size()}));
    }
    /** Patch a reserved prefix, such as an auxiliary array count, before sealing. */
    [[nodiscard]] std::expected<void, std::string> patch_prefix(std::span<const std::byte> bytes) {
        if (sealed_ || bytes.size() > size_) return std::unexpected("byte-buffer prefix patch is out of bounds or sealed");
        if (file_) {
            const auto written = file_->write_at(bytes, 0u);
            if (!written) return std::unexpected(written.error());
        } else std::copy(bytes.begin(), bytes.end(), bytes_.begin());
        return {};
    }
    /** Freeze an immutable contiguous view, mapping private storage only on the spill path. */
    [[nodiscard]] std::expected<void, std::string> seal() {
        if (sealed_) return {};
        if (file_) {
            if (auto sealed = file_->seal(); !sealed) return sealed;
            if (size_ != 0u) {
                auto mapped = file_->map_read_at(0u, size_);
                if (!mapped) return std::unexpected(mapped.error());
                mapped_.emplace(std::move(*mapped));
            }
        }
        sealed_ = true;
        return {};
    }
    /** Borrow only frozen bytes; owner lifetime bounds every returned view. */
    [[nodiscard]] std::span<const std::byte> view() const noexcept {
        if (!sealed_) return {};
        return mapped_ ? mapped_->bytes() : std::span<const std::byte>{bytes_};
    }
    /** Borrow frozen text without allocating a second copy. */
    [[nodiscard]] std::string_view text() const noexcept {
        const auto bytes = view();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    /** Logical file or resident size, independent of the current backing. */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    /** Retained memory charged to the existing controller, for bounded queues. */
    [[nodiscard]] std::uint64_t accounted_capacity_bytes() const noexcept { return permit_.bytes(); }
};

} // namespace bioseq::io
