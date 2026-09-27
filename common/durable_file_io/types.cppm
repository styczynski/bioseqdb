/**
 * @file common/durable_file_io/types.cppm
 * @brief Public durable-file types and descriptor ownership.
 */
module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.common.durable_file_io:types;

export namespace bioseq::common::durable_file_io {

/** The syscall/checkpoint at which a durable-file operation failed. */
enum class file_operation : std::uint8_t {
    none,
    open_regular_file,
    inspect_before_io,
    positional_read,
    positional_write,
    inspect_after_io,
    fsync_file,
    close_file,
    create_staging_file,
    create_directory,
    link_destination,
    rename_destination,
    remove_staging_file,
    open_directory,
    fsync_directory,
    close_directory,
};

/** Stable, source-neutral error categories for durable-file consumers. */
enum class file_error_code : std::uint8_t {
    invalid_argument,
    unsupported_platform,
    offset_overflow,
    file_open_failed,
    symlink_rejected,
    file_status_failed,
    not_regular_file,
    invalid_file_size,
    size_limit_exceeded,
    read_failed,
    unexpected_end_of_file,
    write_failed,
    no_write_progress,
    identity_changed,
    file_fsync_failed,
    descriptor_close_failed,
    destination_exists,
    content_mismatch,
    namespace_update_failed,
    directory_open_failed,
    directory_create_failed,
    directory_fsync_failed,
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

/**
 * Trivially copyable durable-file failure.
 *
 * `system_error` is the captured POSIX errno, or zero when the failure is a
 * semantic invariant violation. `offset` is populated only for positional
 * I/O failures. `publication` records whether a destination name became
 * visible before the failure, so a caller never mistakes visibility for
 * durability.
 */
struct file_error {
    file_error_code code{file_error_code::invalid_argument};
    file_operation operation{file_operation::none};
    int system_error{};
    std::uint64_t offset{};
    publication_state publication{publication_state::unchanged};
};

static_assert(std::is_trivially_copyable_v<file_error>);

/**
 * Optional syscall callbacks for complete positional I/O.
 *
 * `poll` may invoke PostgreSQL's CHECK_FOR_INTERRUPTS. The complete-I/O
 * functions keep only trivially destructible state live while invoking it.
 */
struct positional_io_callbacks {
    using read_at_callback = ssize_t (*)(
        void*, int, void*, std::size_t, off_t) noexcept;
    using write_at_callback = ssize_t (*)(
        void*, int, const void*, std::size_t, off_t) noexcept;
    using poll_callback = void (*)(void*) noexcept;

    void* context{};
    read_at_callback read_at{};
    write_at_callback write_at{};
    poll_callback poll{};
};

/**
 * Explicit fault-injection hook used by deterministic contract tests.
 *
 * Returning zero allows the real syscall. Returning an errno value injects a
 * failure before it. A hook may also mutate its own fixture at a checkpoint.
 * Production callers leave this empty.
 */
struct fault_injection_hooks {
    using before_operation_callback = int (*)(
        void*, file_operation) noexcept;

    void* context{};
    before_operation_callback before_operation{};
};

/**
 * Stable regular-file snapshot used to detect replacement and mutation.
 *
 * Device/inode identify the file object; size, link count, ownership, mode,
 * and timestamps detect mutations relevant to safe publication and reads.
 */
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

/**
 * Bounds for a symlink-safe stable regular-file read.
 *
 * When `require_exact_size` is true, `exact_bytes` must lie within the
 * inclusive minimum/maximum range and the file must match it exactly.
 */
struct stable_read_options {
    std::uint64_t minimum_bytes{};
    std::uint64_t maximum_bytes{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::uint64_t exact_bytes{};
    bool require_exact_size{};
};

/** How a successful durable namespace publication completed. */
enum class publication_disposition : std::uint8_t {
    created,
    already_present_identical,
    replaced,
    renamed,
};

/** Namespace replacement policy for `durable_publish_file`. */
enum class publication_policy : std::uint8_t {
    exclusive_no_replace,
    replace_existing,
};

/** Result of inode-conditional staging-name removal. */
enum class conditional_remove_disposition : std::uint8_t {
    removed,
    already_absent,
    identity_mismatch,
};

/** Durable namespace publication outcome. */
struct publication_result {
    publication_state publication{publication_state::unchanged};
    publication_disposition disposition{
        publication_disposition::created};
};

/** Maximum requested permissions; the process umask may remove bits. */
struct publication_options {
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

    [[nodiscard]] std::expected<void, file_error> close() noexcept {
        const int descriptor = release();
        if (descriptor < 0 || ::close(descriptor) == 0) {
            return {};
        }
        return std::unexpected(file_error{
            .code = file_error_code::descriptor_close_failed,
            .operation = file_operation::close_file,
            .system_error = errno,
        });
    }

private:
    int descriptor_{-1};
};

}  // namespace bioseq::common::durable_file_io
