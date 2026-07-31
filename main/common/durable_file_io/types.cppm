/**
 * @file common/durable_file_io/types.cppm
 * @brief Public durable-file types, descriptor ownership, and declarations.
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

export module bioseq.common.durable_file_io:types;

export namespace bioseq::durable_file_io {

/** The syscall/checkpoint at which a durable-file operation failed. */
enum class file_io_operation : std::uint8_t {
    none,
    open_file,
    stat_before,
    positional_read,
    positional_write,
    stat_after,
    sync_file,
    close_file,
    create_temporary,
    create_directory,
    publish_link,
    publish_rename,
    remove_temporary,
    open_directory,
    sync_directory,
    close_directory,
};

/** Stable, source-neutral error categories for file consumers. */
enum class file_io_errc : std::uint8_t {
    invalid_argument,
    unsupported_platform,
    offset_overflow,
    open_failed,
    symlink_rejected,
    stat_failed,
    not_regular_file,
    invalid_file_size,
    size_limit_exceeded,
    read_failed,
    unexpected_end_of_file,
    write_failed,
    no_write_progress,
    identity_changed,
    file_sync_failed,
    close_failed,
    destination_exists,
    content_mismatch,
    rename_failed,
    directory_open_failed,
    directory_create_failed,
    directory_sync_failed,
    cleanup_failed,
    permission_mismatch,
};

/**
 * Visibility/durability state accompanying an error.
 *
 * `visible_not_durable` is deliberately not success: the destination name is
 * visible after the atomic namespace operation, but its directory fsync did
 * not complete. Callers must not issue a success receipt for that state.
 */
enum class publication_state : std::uint8_t {
    unchanged,
    visible_not_durable,
    durable,
};

/** Trivially copyable error safe to map at PostgreSQL longjmp boundaries. */
struct file_io_error {
    file_io_errc code{file_io_errc::invalid_argument};
    file_io_operation operation{file_io_operation::none};
    int system_error{};
    std::uint64_t offset{};
    publication_state publication{publication_state::unchanged};
};

static_assert(std::is_trivially_copyable_v<file_io_error>);

/**
 * Optional syscall policy for complete positional I/O.
 *
 * `poll` may invoke PostgreSQL's CHECK_FOR_INTERRUPTS. The complete-I/O
 * functions keep only trivially destructible state live while invoking it.
 */
struct positional_io_operations {
    using read_at_fn = ssize_t (*)(
        void*, int, void*, std::size_t, off_t) noexcept;
    using write_at_fn = ssize_t (*)(
        void*, int, const void*, std::size_t, off_t) noexcept;
    using poll_fn = void (*)(void*) noexcept;

    void* context{};
    read_at_fn read_at{};
    write_at_fn write_at{};
    poll_fn poll{};
};

/**
 * Explicit operation hook used by deterministic contract tests.
 *
 * Returning zero allows the real syscall. Returning an errno value injects a
 * failure before it. A hook may also mutate its own fixture at a checkpoint.
 * Production callers leave this empty.
 */
struct file_io_hooks {
    using before_operation_fn = int (*)(
        void*, file_io_operation) noexcept;

    void* context{};
    before_operation_fn before_operation{};
};

/** Stable identity used to detect replacement and in-place mutation. */
struct regular_file_identity {
    std::uint64_t device{};
    std::uint64_t inode{};
    std::uint64_t size{};
    std::uint64_t link_count{};
    std::uint64_t owner{};
    std::uint64_t mode{};
    std::int64_t modified_seconds{};
    std::int64_t modified_nanoseconds{};
    std::int64_t changed_seconds{};
    std::int64_t changed_nanoseconds{};

    [[nodiscard]] bool operator==(
        const regular_file_identity&) const noexcept = default;
};

/** Bytes plus the identity proven unchanged over their complete read. */
struct stable_regular_file {
    std::vector<std::byte> bytes;
    regular_file_identity identity{};
};

/** Bounds for a symlink-safe stable regular-file read. */
struct stable_read_options {
    std::uint64_t minimum_bytes{};
    std::uint64_t maximum_bytes{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::uint64_t exact_bytes{};
    bool require_exact_size{};
};

/** Result of an atomic durable publication. */
enum class durable_create_disposition : std::uint8_t {
    created,
    already_present_identical,
    replaced,
    renamed,
};

/** Namespace replacement policy for `durable_publish_file`. */
enum class publish_policy : std::uint8_t {
    exclusive_no_replace,
    replace_existing,
};

enum class conditional_remove_disposition : std::uint8_t {
    removed,
    already_absent,
    identity_mismatch,
};

struct durable_publish_result {
    publication_state publication{publication_state::unchanged};
    durable_create_disposition disposition{
        durable_create_disposition::created};
};

/** Maximum requested permissions; the process umask may remove bits. */
struct durable_publish_options {
    mode_t mode{S_IRUSR | S_IWUSR};
};

/*
 * Namespace concurrency contract.
 *
 * Pathname publication, conditional cleanup, and directory creation pin
 * directory descriptors, reject final symlinks, and safely arbitrate
 * cooperating callers. POSIX does not provide an inode-conditional unlink, an
 * inode-bound mkdirat/openat pair, or a transaction spanning post-publication
 * identity verification and directory fsync. Callers must therefore own the
 * affected names (including a missing directory suffix) or externally
 * serialize uncooperative writers to those entries for the complete operation.
 */

/**
 * Move-only descriptor owner.
 *
 * PostgreSQL code must not retain this object across a raw PG_TRY boundary;
 * use raw descriptors plus explicit PG_CATCH cleanup there.
 */
class file_descriptor {
public:
    file_descriptor() noexcept = default;
    explicit file_descriptor(int descriptor) noexcept
        : descriptor_(descriptor) {}

    file_descriptor(const file_descriptor&) = delete;
    file_descriptor& operator=(const file_descriptor&) = delete;

    file_descriptor(file_descriptor&& other) noexcept
        : descriptor_(other.release()) {}

    file_descriptor& operator=(file_descriptor&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~file_descriptor() noexcept {
        reset();
    }

    [[nodiscard]] bool valid() const noexcept {
        return descriptor_ >= 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    void reset(int replacement = -1) noexcept {
        const int previous = std::exchange(descriptor_, replacement);
        if (previous >= 0) {
            static_cast<void>(::close(previous));
        }
    }

    [[nodiscard]] std::expected<void, file_io_error> close() noexcept {
        const int descriptor = release();
        if (descriptor < 0 || ::close(descriptor) == 0) {
            return {};
        }
        return std::unexpected(file_io_error{
            .code = file_io_errc::close_failed,
            .operation = file_io_operation::close_file,
            .system_error = errno,
        });
    }

private:
    int descriptor_{-1};
};

}  // namespace bioseq::durable_file_io

export namespace bioseq::durable_file_io {
[[nodiscard]] inline std::expected<void, file_io_error>
positional_read_exact(int descriptor, std::uint64_t offset,
                      std::span<std::byte> destination,
                      const positional_io_operations& operations = {}) noexcept;
[[nodiscard]] inline std::expected<void, file_io_error>
positional_write_all(int descriptor, std::span<const std::byte> bytes,
                     std::uint64_t offset = 0,
                     const positional_io_operations& operations = {}) noexcept;
}  // namespace bioseq::durable_file_io
