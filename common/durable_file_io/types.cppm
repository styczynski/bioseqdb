/**
 * @file common/durable_file_io/types.cppm
 * @brief File handles, read limits and results for durable file operations.
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

/** The file operation being attempted, also used as a test failure point. */
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

/** File errors reported independently of the operating system's errno values. */
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
 * How far a file publication got.
 *
 * With visible_not_durable, the destination name is already visible, but the
 * required directory syncs have not all completed. Report this as a failure.
 */
enum class publication_state : std::uint8_t {
    unchanged,
    visible_not_durable,
    durable,
};

/**
 * A  file failure and the operation that reported it.
 *
 * The record is trivially copyable. system_error holds an errno value when
 * available and may be zero for a failed metadata check. Positional I/O fills
 * in offset. publication records whether the destination was already visible
 * or synced before the faiure
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
 * Optional replacements for offset-based reads and writes, plus a poll callback.
 *
 * The complete-I/O loops may call poll to check PostgreSQL interrupts. Their
 * local state needs no destructors, so an, interrupt can leave the loop through
 * PostgreSQL's error  handling.
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
 * Let tests fail a chosen file operation before its system call runs.
 * Return a positive errno to inject a failure, or zero to continue normally.
 * The callback can also change the test fixture at that point. Production
 * callers leave the hook empty.
 */
struct fault_injection_hooks {
    using before_operation_callback = int (*)(
        void*, file_operation) noexcept;

    void* context{};
    before_operation_callback before_operation{};
};

/**
 * File metadata used to detect replacement or changes.
 * Device and inode identify the file. Readers also compare its size, link
 * count, owner, permissions and timestamps before and  after an operation.
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

/** Read bytes and the matching file metadata checked before and after the read. */
struct stable_regular_file {
    std::vector<std::byte> bytes;
    regular_file_identity identity{};
};

/**
 * Allowed file size for a read that rejects a symlink at the final name.
 * If require_exact_size is set, the file must contain eactly exact_bytes,
 * which must also lie within the inclusive minimum and maximum.
 */
struct stable_read_options {
    std::uint64_t minimum_bytes{};
    std::uint64_t maximum_bytes{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::uint64_t exact_bytes{};
    bool require_exact_size{};
};

/** How the destination was created, reused, replaced or renamed. */
enum class publication_disposition : std::uint8_t {
    created,
    already_present_identical,
    replaced,
    renamed,
};

/** Whether durable_publish_file may replace an existing destination. */
enum class publication_policy : std::uint8_t {
    exclusive_no_replace,
    replace_existing,
};

/** Whether cleanup removed the name, found it absent or found a different file */
enum class conditional_remove_disposition : std::uint8_t {
    removed,
    already_absent,
    identity_mismatch,
};

/** The  publication state and what happened to the destination name. */
struct publication_result {
    publication_state publication{publication_state::unchanged};
    publication_disposition disposition{
        publication_disposition::created};
};

/** Requested file permissions. The process umask can remove permission bits. */
struct publication_options {
    mode_t mode{S_IRUSR | S_IWUSR};
};

/*
 * Callers must control the affected names for the whole operation, including
 * directory names that do not exist yet. Use external locking, if necessary.
 *
 * These helpers keep directories open and reject final symlinks. Cooperating
 * callers can run concurrently. An outside writer can still replace a name
 * between its identity check and unlink, between mkdirat and openat, or
 * before  the final directory fsync.
 */

/**
 * Own a file descriptor and close it on destruction. Ownership can be moved.
 *
 * A PostgreSQL error can skip this destructor. Across raw PG_TRY error
 * boundaries, keep a raw descriptor and close it explicitly in PG_CATCH.
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
