/**
 * @file common/durable_file_io/index.cppm
 * @brief Canonical POSIX file-descriptor and durable publication primitives.
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

export module bioseq.common.durable_file_io;

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

namespace bioseq::durable_file_io::detail {

[[nodiscard]] inline file_io_error make_error(
    file_io_errc code,
    file_io_operation operation,
    int system_error,
    std::uint64_t offset = 0,
    publication_state publication = publication_state::unchanged) noexcept {
    return file_io_error{
        .code = code,
        .operation = operation,
        .system_error = system_error,
        .offset = offset,
        .publication = publication,
    };
}

[[nodiscard]] inline int injected_error(
    const file_io_hooks& hooks,
    file_io_operation operation) noexcept {
    if (hooks.before_operation == nullptr) {
        return 0;
    }
    const int injected = hooks.before_operation(hooks.context, operation);
    return injected > 0 ? injected : 0;
}

[[nodiscard]] inline ssize_t system_read_at(
    void*,
    int descriptor,
    void* destination,
    std::size_t bytes,
    off_t offset) noexcept {
    return ::pread(descriptor, destination, bytes, offset);
}

[[nodiscard]] inline ssize_t system_write_at(
    void*,
    int descriptor,
    const void* source,
    std::size_t bytes,
    off_t offset) noexcept {
    return ::pwrite(descriptor, source, bytes, offset);
}

[[nodiscard]] inline std::filesystem::path parent_directory(
    const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    return parent.empty() ? std::filesystem::path{"."} : parent;
}

[[nodiscard]] inline std::string leaf_name(
    const std::filesystem::path& path) {
    return path.filename().string();
}

[[nodiscard]] inline bool valid_leaf_name(
    std::string_view name) noexcept {
    return !name.empty() && name != "." && name != ".."
        && name.find('\0') == std::string_view::npos
        && name.find('/') == std::string_view::npos;
}

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

[[nodiscard]] inline bool same_file_object(
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

[[nodiscard]] inline bool same_published_identity(
    const regular_file_identity& left,
    const regular_file_identity& right) noexcept {
    return same_inode(left, right)
        && left.size == right.size
        && left.owner == right.owner
        && left.mode == right.mode
        && left.modified_seconds == right.modified_seconds
        && left.modified_nanoseconds == right.modified_nanoseconds;
}

[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
capture_identity(
    int descriptor,
    file_io_operation operation,
    const file_io_hooks& hooks) noexcept {
    if (const int injected = injected_error(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::stat_failed, operation, injected));
    }
    struct stat status {};
    int status_result = -1;
    do {
        status_result = ::fstat(descriptor, &status);
    } while (status_result != 0 && errno == EINTR);
    if (status_result != 0) {
        return std::unexpected(make_error(
            file_io_errc::stat_failed, operation, errno));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(make_error(
            file_io_errc::not_regular_file, operation, 0));
    }
    if (status.st_size < 0) {
        return std::unexpected(make_error(
            file_io_errc::invalid_file_size, operation, 0));
    }
    return identity_from_stat(status);
}

[[nodiscard]] inline int regular_file_open_flags(int access) noexcept {
    int flags = access | O_NONBLOCK;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

[[nodiscard]] inline std::filesystem::path directory_leaf_path(
    std::filesystem::path directory) {
    while (directory != directory.root_path()
           && (directory.filename().empty()
               || directory.filename() == ".")) {
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return directory.empty()
        ? std::filesystem::path{"."}
        : directory;
}

[[nodiscard]] inline bool directory_path_is_symlink_after_open_error(
    const std::filesystem::path& directory,
    int open_error) noexcept {
    if (open_error == ELOOP) {
        return true;
    }
    if (open_error != ENOTDIR) {
        return false;
    }
    struct stat status {};
    int result = -1;
    do {
        result = ::lstat(directory.c_str(), &status);
    } while (result != 0 && errno == EINTR);
    return result == 0 && S_ISLNK(status.st_mode);
}

[[nodiscard]] inline bool directory_name_is_symlink_after_open_error(
    int directory_descriptor,
    const std::string& name,
    int open_error) noexcept {
    if (open_error == ELOOP) {
        return true;
    }
#if defined(AT_SYMLINK_NOFOLLOW)
    if (open_error != ENOTDIR) {
        return false;
    }
    struct stat status {};
    int result = -1;
    do {
        result = ::fstatat(
            directory_descriptor,
            name.c_str(),
            &status,
            AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    return result == 0 && S_ISLNK(status.st_mode);
#else
    static_cast<void>(directory_descriptor);
    static_cast<void>(name);
    return false;
#endif
}

[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_directory_path(
    const std::filesystem::path& directory,
    const file_io_hooks& hooks,
    publication_state publication = publication_state::unchanged) {
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW)
    static_cast<void>(directory);
    static_cast<void>(hooks);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::open_directory,
        0,
        0,
        publication));
#else
    if (directory.empty()
        || directory.string().find('\0') != std::string::npos) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_directory,
            EINVAL,
            0,
            publication));
    }
    const std::filesystem::path leaf_directory =
        directory_leaf_path(directory);
    if (const int injected = injected_error(
            hooks, file_io_operation::open_directory);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            injected,
            0,
            publication));
    }
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(leaf_directory.c_str(), flags);
    if (descriptor < 0) {
        const int saved = errno;
        return std::unexpected(make_error(
            directory_path_is_symlink_after_open_error(
                leaf_directory, saved)
                ? file_io_errc::symlink_rejected
                : file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            saved,
            0,
            publication));
    }
    return file_descriptor{descriptor};
#endif
}

[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_directory_at(
    int directory_descriptor,
    const std::string& name,
    const file_io_hooks& hooks) {
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW)
    static_cast<void>(directory_descriptor);
    static_cast<void>(name);
    static_cast<void>(hooks);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::open_directory,
        0));
#else
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_directory,
            EINVAL));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::open_directory);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            injected));
    }
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::openat(
        directory_descriptor, name.c_str(), flags);
    if (descriptor < 0) {
        const int saved = errno;
        return std::unexpected(make_error(
            directory_name_is_symlink_after_open_error(
                directory_descriptor, name, saved)
                ? file_io_errc::symlink_rejected
                : file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            saved));
    }
    return file_descriptor{descriptor};
#endif
}

[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_parent_directory_at(
    int directory_descriptor,
    const file_io_hooks& hooks) {
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW)
    static_cast<void>(directory_descriptor);
    static_cast<void>(hooks);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::open_directory,
        0));
#else
    if (directory_descriptor < 0) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_directory,
            EINVAL));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::open_directory);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            injected));
    }
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::openat(
        directory_descriptor, "..", flags);
    if (descriptor < 0) {
        return std::unexpected(make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            errno));
    }
    return file_descriptor{descriptor};
#endif
}

[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    int access,
    file_io_operation operation,
    const file_io_hooks& hooks) {
#if !defined(O_NOFOLLOW)
    static_cast<void>(directory_descriptor);
    static_cast<void>(name);
    static_cast<void>(access);
    static_cast<void>(operation);
    static_cast<void>(hooks);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform, operation, 0));
#else
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument, operation, EINVAL));
    }
    if (const int injected = injected_error(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::open_failed, operation, injected));
    }
    const int descriptor = ::openat(
        directory_descriptor,
        name.c_str(),
        regular_file_open_flags(access));
    if (descriptor < 0) {
        const int saved = errno;
        return std::unexpected(make_error(
            saved == ELOOP
                ? file_io_errc::symlink_rejected
                : file_io_errc::open_failed,
            operation,
            saved));
    }
    return file_descriptor{descriptor};
#endif
}

[[nodiscard]] inline std::expected<void, file_io_error> sync_descriptor(
    int descriptor,
    file_io_operation operation,
    const file_io_hooks& hooks,
    publication_state publication = publication_state::unchanged) noexcept {
    if (const int injected = injected_error(hooks, operation);
        injected != 0) {
        return std::unexpected(make_error(
            operation == file_io_operation::sync_directory
                ? file_io_errc::directory_sync_failed
                : file_io_errc::file_sync_failed,
            operation,
            injected,
            0,
            publication));
    }
    int sync_result = -1;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    if (sync_result != 0) {
        return std::unexpected(make_error(
            operation == file_io_operation::sync_directory
                ? file_io_errc::directory_sync_failed
                : file_io_errc::file_sync_failed,
            operation,
            errno,
            0,
            publication));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, file_io_error>
close_directory_descriptor(
    file_descriptor& descriptor,
    const file_io_hooks& hooks,
    publication_state publication) noexcept {
    if (!descriptor.valid()) {
        return {};
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::close_directory);
        injected != 0) {
        descriptor.reset();
        return std::unexpected(make_error(
            file_io_errc::close_failed,
            file_io_operation::close_directory,
            injected,
            0,
            publication));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        file_io_error error = closed.error();
        error.operation = file_io_operation::close_directory;
        error.publication = publication;
        return std::unexpected(error);
    }
    return {};
}

[[nodiscard]] inline std::string unique_temporary_name(
    std::string_view destination_name) {
    static std::atomic<std::uint64_t> sequence{1};
    const std::uint64_t value =
        sequence.fetch_add(1, std::memory_order_relaxed);
    return std::string(destination_name)
        + ".tmp." + std::to_string(static_cast<std::uint64_t>(::getpid()))
        + "." + std::to_string(value);
}

[[nodiscard]] inline std::expected<void, file_io_error> remove_name_at(
    int directory_descriptor,
    const std::string& name,
    publication_state publication,
    const file_io_hooks& hooks = {}) noexcept {
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::remove_temporary,
            EINVAL,
            0,
            publication));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::remove_temporary);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::cleanup_failed,
            file_io_operation::remove_temporary,
            injected,
            0,
            publication));
    }
    if (::unlinkat(directory_descriptor, name.c_str(), 0) == 0
        || errno == ENOENT) {
        return {};
    }
    return std::unexpected(make_error(
        file_io_errc::cleanup_failed,
        file_io_operation::remove_temporary,
        errno,
        0,
        publication));
}

/**
 * Remove a staging entry only while it still names the inode we created.
 *
 * The deterministic hook runs before the final identity snapshot. POSIX has
 * no inode-conditional unlink syscall, so callers must serialize uncooperative
 * writers to the same directory across the final fstatat/unlinkat pair.
 */
[[nodiscard]] inline std::expected<
    conditional_remove_disposition,
    file_io_error>
remove_name_if_same_inode_at(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    publication_state publication,
    const file_io_hooks& hooks = {}) noexcept {
    if (directory_descriptor < 0 || !valid_leaf_name(name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::remove_temporary,
            EINVAL,
            0,
            publication));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::remove_temporary);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::cleanup_failed,
            file_io_operation::remove_temporary,
            injected,
            0,
            publication));
    }
    struct stat status {};
    int status_result = -1;
    do {
        status_result = ::fstatat(
            directory_descriptor,
            name.c_str(),
            &status,
            AT_SYMLINK_NOFOLLOW);
    } while (status_result != 0 && errno == EINTR);
    if (status_result != 0) {
        if (errno == ENOENT) {
            return conditional_remove_disposition::already_absent;
        }
        return std::unexpected(make_error(
            file_io_errc::cleanup_failed,
            file_io_operation::remove_temporary,
            errno,
            0,
            publication));
    }
    if (!S_ISREG(status.st_mode)
        || !same_inode(identity_from_stat(status), expected)) {
        return conditional_remove_disposition::identity_mismatch;
    }
    auto removed = remove_name_at(
        directory_descriptor, name, publication);
    if (!removed.has_value()) {
        return std::unexpected(removed.error());
    }
    return conditional_remove_disposition::removed;
}

[[nodiscard]] inline std::expected<publication_state, file_io_error>
link_name_no_replace_at(
    int source_directory,
    const std::string& source_name,
    int destination_directory,
    const std::string& destination_name,
    const file_io_hooks& hooks,
    const regular_file_identity* expected_source = nullptr) {
    if (source_directory < 0 || destination_directory < 0
        || !valid_leaf_name(source_name)
        || !valid_leaf_name(destination_name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::publish_link,
            EINVAL));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::publish_link);
        injected != 0) {
        return std::unexpected(make_error(
            injected == EEXIST
                ? file_io_errc::destination_exists
                : file_io_errc::rename_failed,
            file_io_operation::publish_link,
            injected));
    }
    file_descriptor pinned_source{};
    regular_file_identity pinned_identity{};
    auto opened = open_regular_file_at(
        source_directory,
        source_name,
        O_RDONLY,
        file_io_operation::open_file,
        {});
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    pinned_source = std::move(*opened);
    auto current = capture_identity(
        pinned_source.get(), file_io_operation::stat_after, {});
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (expected_source != nullptr && *current != *expected_source) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    pinned_identity = *current;
    if (const int injected = injected_error(
            hooks, file_io_operation::stat_after);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::stat_failed,
            file_io_operation::stat_after,
            injected));
    }
    if (::linkat(
            source_directory,
            source_name.c_str(),
            destination_directory,
            destination_name.c_str(),
            0) != 0) {
        const int saved = errno;
        return std::unexpected(make_error(
            saved == EEXIST
                ? file_io_errc::destination_exists
                : file_io_errc::rename_failed,
            file_io_operation::publish_link,
            saved));
    }
    auto destination_opened = open_regular_file_at(
        destination_directory,
        destination_name,
        O_RDONLY,
        file_io_operation::open_file,
        {});
    if (!destination_opened.has_value()) {
        file_io_error error = destination_opened.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    file_descriptor destination_descriptor =
        std::move(*destination_opened);
    auto published = capture_identity(
        destination_descriptor.get(),
        file_io_operation::stat_after,
        {});
    if (!published.has_value()) {
        file_io_error error = published.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    if (!same_published_identity(*published, pinned_identity)) {
        if (auto removed = remove_name_if_same_inode_at(
                destination_directory,
                destination_name,
                *published,
                publication_state::visible_not_durable,
                hooks);
            !removed.has_value()) {
            return std::unexpected(removed.error());
        } else if (*removed
                   == conditional_remove_disposition::identity_mismatch) {
            return std::unexpected(make_error(
                file_io_errc::identity_changed,
                file_io_operation::remove_temporary,
                ESTALE,
                0,
                publication_state::visible_not_durable));
        }
        if (auto synced = sync_descriptor(
                destination_directory,
                file_io_operation::sync_directory,
                hooks,
                publication_state::visible_not_durable);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    return publication_state::visible_not_durable;
}

[[nodiscard]] inline std::expected<publication_state, file_io_error>
rename_name_at(
    int source_directory,
    const std::string& source_name,
    int destination_directory,
    const std::string& destination_name,
    const file_io_hooks& hooks,
    const regular_file_identity* expected_source = nullptr) {
    if (source_directory < 0 || destination_directory < 0
        || !valid_leaf_name(source_name)
        || !valid_leaf_name(destination_name)) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::publish_rename,
            EINVAL));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::publish_rename);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::rename_failed,
            file_io_operation::publish_rename,
            injected));
    }
    file_descriptor pinned_source{};
    regular_file_identity pinned_identity{};
    auto opened = open_regular_file_at(
        source_directory,
        source_name,
        O_RDONLY,
        file_io_operation::open_file,
        {});
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    pinned_source = std::move(*opened);
    auto current = capture_identity(
        pinned_source.get(), file_io_operation::stat_after, {});
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (expected_source != nullptr && *current != *expected_source) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    pinned_identity = *current;
    if (const int injected = injected_error(
            hooks, file_io_operation::stat_after);
        injected != 0) {
        return std::unexpected(make_error(
            file_io_errc::stat_failed,
            file_io_operation::stat_after,
            injected));
    }
    if (::renameat(
            source_directory,
            source_name.c_str(),
            destination_directory,
            destination_name.c_str()) != 0) {
        return std::unexpected(make_error(
            file_io_errc::rename_failed,
            file_io_operation::publish_rename,
            errno));
    }
    auto destination_opened = open_regular_file_at(
        destination_directory,
        destination_name,
        O_RDONLY,
        file_io_operation::open_file,
        {});
    if (!destination_opened.has_value()) {
        file_io_error error = destination_opened.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    file_descriptor destination_descriptor =
        std::move(*destination_opened);
    auto published = capture_identity(
        destination_descriptor.get(),
        file_io_operation::stat_after,
        {});
    if (!published.has_value()) {
        file_io_error error = published.error();
        error.publication = publication_state::visible_not_durable;
        return std::unexpected(error);
    }
    if (!same_published_identity(*published, pinned_identity)) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0,
            0,
            publication_state::visible_not_durable));
    }
    return publication_state::visible_not_durable;
}

[[nodiscard]] inline file_io_error cleanup_error_or(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    publication_state publication,
    file_io_error primary,
    const file_io_hooks& hooks) noexcept {
    auto removed = remove_name_if_same_inode_at(
        directory_descriptor, name, expected, publication, hooks);
    if (!removed.has_value()) {
        return removed.error();
    }
    if (*removed == conditional_remove_disposition::removed) {
        return primary;
    }
    return make_error(
        file_io_errc::identity_changed,
        file_io_operation::remove_temporary,
        ESTALE,
        0,
        publication);
}

[[nodiscard]] inline std::expected<void, file_io_error>
remove_published_staging_name(
    int directory_descriptor,
    const std::string& name,
    const regular_file_identity& expected,
    const file_io_hooks& hooks) noexcept {
    auto removed = remove_name_if_same_inode_at(
        directory_descriptor,
        name,
        expected,
        publication_state::visible_not_durable,
        hooks);
    if (!removed.has_value()) {
        return std::unexpected(removed.error());
    }
    if (*removed == conditional_remove_disposition::removed) {
        return {};
    }
    auto synced = sync_descriptor(
        directory_descriptor,
        file_io_operation::sync_directory,
        hooks,
        publication_state::visible_not_durable);
    if (!synced.has_value()) {
        return std::unexpected(synced.error());
    }
    return std::unexpected(make_error(
        file_io_errc::identity_changed,
        file_io_operation::remove_temporary,
        ESTALE,
        0,
        publication_state::durable));
}

[[nodiscard]] inline std::expected<stable_regular_file, file_io_error>
read_stable_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const stable_read_options& options,
    const file_io_hooks& hooks,
    const positional_io_operations& operations);

}  // namespace bioseq::durable_file_io::detail

export namespace bioseq::durable_file_io {

/** Capture a regular descriptor's complete stable identity. */
[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
capture_regular_file_identity(
    int descriptor,
    const file_io_hooks& hooks = {}) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::stat_before,
            EBADF));
    }
    return detail::capture_identity(
        descriptor, file_io_operation::stat_before, hooks);
}

/** Read exactly `destination.size()` bytes at `offset`. */
[[nodiscard]] inline std::expected<void, file_io_error>
positional_read_exact(
    int descriptor,
    std::uint64_t offset,
    std::span<std::byte> destination,
    const positional_io_operations& operations = {}) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::positional_read,
            EBADF,
            offset));
    }
    const std::uint64_t maximum_offset = static_cast<std::uint64_t>(
        (std::numeric_limits<off_t>::max)());
    if (offset > maximum_offset
        || (!destination.empty()
            && destination.size() > maximum_offset - offset + 1)) {
        return std::unexpected(detail::make_error(
            file_io_errc::offset_overflow,
            file_io_operation::positional_read,
            EOVERFLOW,
            offset));
    }
    const auto read_at = operations.read_at != nullptr
        ? operations.read_at
        : detail::system_read_at;
    std::size_t completed = 0;
    while (completed < destination.size()) {
        if (operations.poll != nullptr) {
            operations.poll(operations.context);
        }
        const std::size_t request = std::min(
            destination.size() - completed,
            static_cast<std::size_t>(
                (std::numeric_limits<ssize_t>::max)()));
        const ssize_t count = read_at(
            operations.context,
            descriptor,
            destination.data() + completed,
            request,
            static_cast<off_t>(offset + completed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::read_failed,
                file_io_operation::positional_read,
                errno,
                offset + completed));
        }
        if (count == 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::unexpected_end_of_file,
                file_io_operation::positional_read,
                0,
                offset + completed));
        }
        const auto progress = static_cast<std::size_t>(count);
        if (progress > destination.size() - completed) {
            return std::unexpected(detail::make_error(
                file_io_errc::read_failed,
                file_io_operation::positional_read,
                EIO,
                offset + completed));
        }
        completed += progress;
    }
    return {};
}

/** Write all bytes at `offset`, retrying EINTR and accepting short writes. */
[[nodiscard]] inline std::expected<void, file_io_error>
positional_write_all(
    int descriptor,
    std::span<const std::byte> bytes,
    std::uint64_t offset = 0,
    const positional_io_operations& operations = {}) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::positional_write,
            EBADF,
            offset));
    }
    const std::uint64_t maximum_offset = static_cast<std::uint64_t>(
        (std::numeric_limits<off_t>::max)());
    if (offset > maximum_offset
        || (!bytes.empty()
            && bytes.size() > maximum_offset - offset + 1)) {
        return std::unexpected(detail::make_error(
            file_io_errc::offset_overflow,
            file_io_operation::positional_write,
            EOVERFLOW,
            offset));
    }
    const auto write_at = operations.write_at != nullptr
        ? operations.write_at
        : detail::system_write_at;
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        if (operations.poll != nullptr) {
            operations.poll(operations.context);
        }
        const std::size_t request = std::min(
            bytes.size() - completed,
            static_cast<std::size_t>(
                (std::numeric_limits<ssize_t>::max)()));
        const ssize_t count = write_at(
            operations.context,
            descriptor,
            bytes.data() + completed,
            request,
            static_cast<off_t>(offset + completed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::write_failed,
                file_io_operation::positional_write,
                errno,
                offset + completed));
        }
        if (count == 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::no_write_progress,
                file_io_operation::positional_write,
                0,
                offset + completed));
        }
        const auto progress = static_cast<std::size_t>(count);
        if (progress > bytes.size() - completed) {
            return std::unexpected(detail::make_error(
                file_io_errc::write_failed,
                file_io_operation::positional_write,
                EIO,
                offset + completed));
        }
        completed += progress;
    }
    return {};
}

/**
 * Read a regular file without following its final symlink and prove its
 * identity and size did not change between the before/after fstat snapshots.
 */
[[nodiscard]] inline std::expected<stable_regular_file, file_io_error>
read_stable_regular_file(
    const std::filesystem::path& path,
    const stable_read_options& options = {},
    const file_io_hooks& hooks = {},
    const positional_io_operations& operations = {}) {
    if (path.empty()) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_file,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(path);
    const std::string name = detail::leaf_name(path);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    return detail::read_stable_regular_file_at(
        directory.get(), name, options, hooks, operations);
}

/** Check a path against a captured identity without following a symlink. */
[[nodiscard]] inline std::expected<bool, file_io_error>
same_regular_file_identity(
    const std::filesystem::path& path,
    const regular_file_identity& expected,
    const file_io_hooks& hooks = {}) {
    if (path.empty()) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_file,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(path);
    const std::string name = detail::leaf_name(path);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto opened = detail::open_regular_file_at(
        directory.get(),
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = detail::capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    return *current == expected;
}

/** Fsync one directory path without following its final symlink. */
[[nodiscard]] inline std::expected<void, file_io_error> fsync_directory(
    const std::filesystem::path& directory,
    const file_io_hooks& hooks = {},
    publication_state publication = publication_state::unchanged) {
    if (directory.empty()) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_directory,
            EINVAL,
            0,
            publication));
    }
    auto opened = detail::open_directory_path(
        directory, hooks, publication);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    if (auto synced = detail::sync_descriptor(
            descriptor.get(),
            file_io_operation::sync_directory,
            hooks,
            publication);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    return detail::close_directory_descriptor(
        descriptor, hooks, publication);
}

/**
 * Create and durably publish a relative directory chain below a pinned anchor.
 *
 * The anchor itself is never reopened by pathname. Every suffix component is
 * opened with O_NOFOLLOW after mkdirat, including an EEXIST result from a
 * concurrent cooperating creator. Descriptors opened by this function are
 * explicitly closed before success; the caller-owned anchor remains open.
 * Absolute paths and `..` traversal are rejected.
 *
 * `mode` is the maximum requested permission set only for components created
 * by this call; the process umask may remove bits. Existing components are
 * type/symlink validated and durability-synced but are not chmod'd.
 */
[[nodiscard]] inline std::expected<void, file_io_error>
durable_create_directories_at(
    const file_descriptor& anchor,
    const std::filesystem::path& relative_directory,
    mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO,
    const file_io_hooks& hooks = {}) {
    if (!anchor.valid()
        || relative_directory.is_absolute()
        || mode == 0
        || (mode & ~(S_IRWXU | S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_directory,
            EINVAL));
    }
    for (const std::filesystem::path& component : relative_directory) {
        if (component == "..") {
            return std::unexpected(detail::make_error(
                file_io_errc::invalid_argument,
                file_io_operation::create_directory,
                EINVAL));
        }
    }
    file_descriptor owned_current;
    int current_descriptor = anchor.get();
    publication_state completed_publication =
        publication_state::unchanged;
    const std::filesystem::path normalized =
        relative_directory.lexically_normal();
    for (const std::filesystem::path& component : normalized) {
        const std::string name = component.string();
        if (name.empty() || name == ".") {
            continue;
        }
        if (!detail::valid_leaf_name(name)) {
            return std::unexpected(detail::make_error(
                file_io_errc::invalid_argument,
                file_io_operation::create_directory,
                EINVAL));
        }
        if (const int injected = detail::injected_error(
                hooks, file_io_operation::create_directory);
            injected != 0) {
            return std::unexpected(detail::make_error(
                file_io_errc::directory_create_failed,
                file_io_operation::create_directory,
                injected,
                0,
                completed_publication));
        }
        int create_result = -1;
        do {
            create_result = ::mkdirat(
                current_descriptor, name.c_str(), mode);
        } while (create_result != 0 && errno == EINTR);
        if (create_result != 0 && errno != EEXIST) {
            return std::unexpected(detail::make_error(
                file_io_errc::directory_create_failed,
                file_io_operation::create_directory,
                errno,
                0,
                completed_publication));
        }
        const bool created = create_result == 0;
        const publication_state pending_publication = created
            ? publication_state::visible_not_durable
            : completed_publication;
        auto child_opened = detail::open_directory_at(
            current_descriptor, name, hooks);
        if (!child_opened.has_value()) {
            file_io_error error = child_opened.error();
            error.publication = pending_publication;
            return std::unexpected(error);
        }
        file_descriptor child = std::move(*child_opened);
        if (auto synced = detail::sync_descriptor(
                child.get(),
                file_io_operation::sync_directory,
                hooks,
                pending_publication);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (auto synced = detail::sync_descriptor(
                current_descriptor,
                file_io_operation::sync_directory,
                hooks,
                pending_publication);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (created) {
            completed_publication = publication_state::durable;
        }
        if (auto closed = detail::close_directory_descriptor(
                owned_current, hooks, completed_publication);
            !closed.has_value()) {
            return std::unexpected(closed.error());
        }
        owned_current = std::move(child);
        current_descriptor = owned_current.get();
    }
    return detail::close_directory_descriptor(
        owned_current, hooks, completed_publication);
}

/**
 * Create a directory chain below its deepest existing, non-symlink leaf.
 *
 * The anchor pathname may contain trusted intermediate platform aliases (for
 * example Darwin's `/var -> private/var`), but its final component is opened
 * with O_NOFOLLOW and pinned. Only the missing relative suffix is then passed
 * to `durable_create_directories_at`, so an existing symlink at the boundary or
 * a symlink introduced into the suffix is rejected. Every opened suffix
 * component and its parent are fsynced, including EEXIST components; `mode`
 * applies only to components this call creates. `..` traversal is rejected.
 */
[[nodiscard]] inline std::expected<void, file_io_error>
durable_create_directories(
    const std::filesystem::path& directory,
    mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO,
    const file_io_hooks& hooks = {}) {
    if (directory.empty()
        || mode == 0
        || (mode & ~(S_IRWXU | S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_directory,
            EINVAL));
    }
    for (const std::filesystem::path& component : directory) {
        if (component == "..") {
            return std::unexpected(detail::make_error(
                file_io_errc::invalid_argument,
                file_io_operation::create_directory,
                EINVAL));
        }
    }

    std::filesystem::path candidate =
        detail::directory_leaf_path(directory).lexically_normal();
    std::vector<std::string> missing_components;
    auto requested_opened =
        detail::open_directory_path(candidate, hooks);
    if (requested_opened.has_value()) {
        file_descriptor requested = std::move(*requested_opened);
        auto parent_opened = detail::open_parent_directory_at(
            requested.get(), hooks);
        if (!parent_opened.has_value()) {
            return std::unexpected(parent_opened.error());
        }
        file_descriptor actual_parent = std::move(*parent_opened);
        if (auto synced = detail::sync_descriptor(
                requested.get(),
                file_io_operation::sync_directory,
                hooks,
                publication_state::unchanged);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (auto synced = detail::sync_descriptor(
                actual_parent.get(),
                file_io_operation::sync_directory,
                hooks,
                publication_state::unchanged);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
        if (auto closed = detail::close_directory_descriptor(
                actual_parent, hooks, publication_state::durable);
            !closed.has_value()) {
            return std::unexpected(closed.error());
        }
        return detail::close_directory_descriptor(
            requested, hooks, publication_state::durable);
    }
    if (requested_opened.error().system_error != ENOENT) {
        return std::unexpected(requested_opened.error());
    }
    const std::string requested_name = candidate.filename().string();
    if (!detail::valid_leaf_name(requested_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_directory,
            EINVAL));
    }
    missing_components.push_back(requested_name);
    const std::filesystem::path requested_parent =
        candidate.parent_path();
    candidate = requested_parent.empty()
        ? std::filesystem::path{"."}
        : requested_parent;
    std::expected<file_descriptor, file_io_error> anchor =
        std::unexpected(detail::make_error(
            file_io_errc::directory_open_failed,
            file_io_operation::open_directory,
            ENOENT));
    for (;;) {
        anchor = detail::open_directory_path(candidate, hooks);
        if (anchor.has_value()) {
            break;
        }
        if (anchor.error().system_error != ENOENT) {
            return std::unexpected(anchor.error());
        }

        const std::string name = candidate.filename().string();
        if (!detail::valid_leaf_name(name)) {
            return std::unexpected(anchor.error());
        }
        missing_components.push_back(name);
        const std::filesystem::path parent = candidate.parent_path();
        candidate = parent.empty()
            ? std::filesystem::path{"."}
            : parent;
    }

    std::filesystem::path relative_suffix;
    for (auto component = missing_components.rbegin();
         component != missing_components.rend();
         ++component) {
        relative_suffix /= *component;
    }
    auto created = durable_create_directories_at(
        *anchor, relative_suffix, mode, hooks);
    if (!created.has_value()) {
        return std::unexpected(created.error());
    }
    return detail::close_directory_descriptor(
        *anchor, hooks, publication_state::durable);
}

/** Open and pin a no-follow directory for subsequent *at operations. */
[[nodiscard]] inline std::expected<file_descriptor, file_io_error>
open_directory_descriptor(
    const std::filesystem::path& directory,
    const file_io_hooks& hooks = {}) {
    return detail::open_directory_path(directory, hooks);
}

/** Fsync an already-open pinned directory descriptor. */
[[nodiscard]] inline std::expected<void, file_io_error>
fsync_directory_descriptor(
    int descriptor,
    const file_io_hooks& hooks = {},
    publication_state publication = publication_state::unchanged) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::sync_directory,
            EBADF,
            0,
            publication));
    }
    return detail::sync_descriptor(
        descriptor,
        file_io_operation::sync_directory,
        hooks,
        publication);
}

/** Fsync an already-open file descriptor with typed EINTR handling. */
[[nodiscard]] inline std::expected<void, file_io_error>
fsync_file_descriptor(
    int descriptor,
    const file_io_hooks& hooks = {}) noexcept {
    if (descriptor < 0) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::sync_file,
            EBADF));
    }
    return detail::sync_descriptor(
        descriptor, file_io_operation::sync_file, hooks);
}

/**
 * Atomically add a no-replace hard-link publication.
 *
 * This is the namespace step only. The source must already be fsynced and the
 * caller must unlink its staging name and fsync the containing directory
 * before treating the publication as durable.
 */
[[nodiscard]] inline std::expected<publication_state, file_io_error>
link_file_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const file_io_hooks& hooks = {}) {
    const std::string source_name = detail::leaf_name(source);
    const std::string destination_name = detail::leaf_name(destination);
    if (source.empty() || destination.empty()
        || !detail::valid_leaf_name(source_name)
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::publish_link,
            EINVAL));
    }
    const std::filesystem::path source_parent =
        detail::parent_directory(source);
    const std::filesystem::path destination_parent =
        detail::parent_directory(destination);
    auto source_opened = detail::open_directory_path(
        source_parent, hooks);
    if (!source_opened.has_value()) {
        return std::unexpected(source_opened.error());
    }
    file_descriptor source_directory = std::move(*source_opened);
    file_descriptor destination_directory{};
    int destination_descriptor = source_directory.get();
    if (source_parent.lexically_normal()
            != destination_parent.lexically_normal()) {
        auto destination_opened = detail::open_directory_path(
            destination_parent, hooks);
        if (!destination_opened.has_value()) {
            return std::unexpected(destination_opened.error());
        }
        destination_directory = std::move(*destination_opened);
        destination_descriptor = destination_directory.get();
    }
    return detail::link_name_no_replace_at(
            source_directory.get(),
            source_name,
            destination_descriptor,
            destination_name,
            hooks);
}

/** No-replace hard-link publication relative to pinned directory handles. */
[[nodiscard]] inline std::expected<publication_state, file_io_error>
link_file_no_replace_at(
    int source_directory,
    std::string_view source_name,
    int destination_directory,
    std::string_view destination_name,
    const file_io_hooks& hooks = {},
    const regular_file_identity* expected_source = nullptr) {
    return detail::link_name_no_replace_at(
        source_directory,
        std::string{source_name},
        destination_directory,
        std::string{destination_name},
        hooks,
        expected_source);
}

/** Remove a pinned-directory entry only if it still names `expected`. */
[[nodiscard]] inline std::expected<
    conditional_remove_disposition,
    file_io_error>
remove_file_if_same_inode_at(
    int directory_descriptor,
    std::string_view name,
    const regular_file_identity& expected,
    publication_state publication = publication_state::unchanged,
    const file_io_hooks& hooks = {}) {
    return detail::remove_name_if_same_inode_at(
        directory_descriptor,
        std::string{name},
        expected,
        publication,
        hooks);
}

/** Capture one regular entry relative to a pinned directory descriptor. */
[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
capture_regular_file_identity_at(
    int directory_descriptor,
    std::string_view name,
    const file_io_hooks& hooks = {}) {
    const std::string owned_name{name};
    auto opened = detail::open_regular_file_at(
        directory_descriptor,
        owned_name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = detail::capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    return *current;
}

/** Compare one pinned-directory entry with a captured regular identity. */
[[nodiscard]] inline std::expected<bool, file_io_error>
same_regular_file_identity_at(
    int directory_descriptor,
    std::string_view name,
    const regular_file_identity& expected,
    const file_io_hooks& hooks = {}) {
    const std::string owned_name{name};
    auto opened = detail::open_regular_file_at(
        directory_descriptor,
        owned_name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = detail::capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    return *current == expected;
}

}  // namespace bioseq::durable_file_io

namespace bioseq::durable_file_io::detail {

struct staged_file {
    std::string name;
    regular_file_identity identity{};
};

[[nodiscard]] inline std::expected<staged_file, file_io_error> stage_bytes(
    int directory_descriptor,
    const std::string& destination_name,
    std::span<const std::byte> bytes,
    const durable_publish_options& options,
    const file_io_hooks& hooks,
    const positional_io_operations& operations) {
    if (directory_descriptor < 0
        || !valid_leaf_name(destination_name)
        || options.mode == 0
        || (options.mode & ~(S_IRWXU | S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
#if !defined(O_NOFOLLOW)
    static_cast<void>(bytes);
    static_cast<void>(hooks);
    static_cast<void>(operations);
    return std::unexpected(make_error(
        file_io_errc::unsupported_platform,
        file_io_operation::create_temporary,
        0));
#else
    constexpr std::size_t maximum_attempts = 64;
    std::string temporary{};
    int raw_descriptor = -1;
    for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
        temporary = unique_temporary_name(destination_name);
        if (const int injected = injected_error(
                hooks, file_io_operation::create_temporary);
            injected != 0) {
            return std::unexpected(make_error(
                file_io_errc::open_failed,
                file_io_operation::create_temporary,
                injected));
        }
        int flags = O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        raw_descriptor = ::openat(
            directory_descriptor,
            temporary.c_str(),
            flags,
            options.mode);
        if (raw_descriptor >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return std::unexpected(make_error(
                file_io_errc::open_failed,
                file_io_operation::create_temporary,
                errno));
        }
    }
    if (raw_descriptor < 0) {
        return std::unexpected(make_error(
            file_io_errc::open_failed,
            file_io_operation::create_temporary,
            EEXIST));
    }
    file_descriptor descriptor{raw_descriptor};
    auto pinned_identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, {});
    if (!pinned_identity.has_value()) {
        const file_io_error primary = pinned_identity.error();
        descriptor.reset();
        auto removed = remove_name_at(
            directory_descriptor,
            temporary,
            publication_state::unchanged,
            hooks);
        return std::unexpected(
            removed.has_value() ? primary : removed.error());
    }
    auto identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    const mode_t actual_permissions = identity.has_value()
        ? static_cast<mode_t>(
            identity->mode & (S_IRWXU | S_IRWXG | S_IRWXO))
        : 0;
    if (!identity.has_value()
        || identity->link_count != 1
        || (actual_permissions & ~options.mode) != 0) {
        const file_io_error primary = identity.has_value()
            ? make_error(
                file_io_errc::not_regular_file,
                file_io_operation::stat_before,
                0)
            : identity.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::positional_write);
        injected != 0) {
        const file_io_error primary = make_error(
            file_io_errc::write_failed,
            file_io_operation::positional_write,
            injected);
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto written = positional_write_all(
            descriptor.get(), bytes, 0, operations);
        !written.has_value()) {
        const file_io_error primary = written.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto synced = sync_descriptor(
            descriptor.get(),
            file_io_operation::sync_file,
            hooks);
        !synced.has_value()) {
        const file_io_error primary = synced.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    auto final_identity = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!final_identity.has_value()
        || !same_file_object(*identity, *final_identity)
        || final_identity->size != bytes.size()) {
        const file_io_error primary = final_identity.has_value()
            ? make_error(
                file_io_errc::identity_changed,
                file_io_operation::stat_after,
                0)
            : final_identity.error();
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (const int injected = injected_error(
            hooks, file_io_operation::close_file);
        injected != 0) {
        const file_io_error primary = make_error(
            file_io_errc::close_failed,
            file_io_operation::close_file,
            injected);
        descriptor.reset();
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            primary,
            hooks));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(cleanup_error_or(
            directory_descriptor,
            temporary,
            *pinned_identity,
            publication_state::unchanged,
            closed.error(),
            hooks));
    }
    return staged_file{
        .name = std::move(temporary),
        .identity = *final_identity,
    };
#endif
}

[[nodiscard]] inline std::expected<regular_file_identity, file_io_error>
sync_existing_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const file_io_hooks& hooks,
    const regular_file_identity* expected = nullptr) {
    auto opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto identity = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    if (!identity.has_value()) {
        return std::unexpected(identity.error());
    }
    if (expected != nullptr && *identity != *expected) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_before,
            0));
    }
    if (auto synced = sync_descriptor(
            descriptor.get(), file_io_operation::sync_file, hooks);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    auto after = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!after.has_value()) {
        return std::unexpected(after.error());
    }
    if (*after != *identity) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    auto current_opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!current_opened.has_value()) {
        return std::unexpected(current_opened.error());
    }
    file_descriptor current_descriptor = std::move(*current_opened);
    auto current = capture_identity(
        current_descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    return *after;
}

[[nodiscard]] inline std::expected<void, file_io_error>
verify_staged_path(
    int directory_descriptor,
    const staged_file& staged,
    const file_io_hooks& hooks) {
    auto opened = open_regular_file_at(
        directory_descriptor,
        staged.name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto current = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != staged.identity) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    return {};
}

[[nodiscard]] inline std::expected<stable_regular_file, file_io_error>
read_stable_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const stable_read_options& options,
    const file_io_hooks& hooks,
    const positional_io_operations& operations) {
    if (directory_descriptor < 0
        || !valid_leaf_name(name)
        || options.minimum_bytes > options.maximum_bytes
        || (options.require_exact_size
            && (options.exact_bytes < options.minimum_bytes
                || options.exact_bytes > options.maximum_bytes))) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_file,
            EINVAL));
    }
    auto opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto before = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
    if (!before.has_value()) {
        return std::unexpected(before.error());
    }
    if (before->size < options.minimum_bytes
        || before->size > options.maximum_bytes
        || (options.require_exact_size
            && before->size != options.exact_bytes)
        || before->size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return std::unexpected(make_error(
            before->size > options.maximum_bytes
                    || before->size
                        > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::size_t>::max)())
                ? file_io_errc::size_limit_exceeded
                : file_io_errc::invalid_file_size,
            file_io_operation::stat_before,
            0));
    }
    stable_regular_file result{};
    result.bytes.resize(static_cast<std::size_t>(before->size));
    if (auto read = positional_read_exact(
            descriptor.get(),
            0,
            std::span<std::byte>{
                result.bytes.data(), result.bytes.size()},
            operations);
        !read.has_value()) {
        return std::unexpected(read.error());
    }
    auto after = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!after.has_value()) {
        return std::unexpected(after.error());
    }
    if (*before != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    auto current_opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!current_opened.has_value()) {
        return std::unexpected(current_opened.error());
    }
    file_descriptor current_descriptor = std::move(*current_opened);
    auto current = capture_identity(
        current_descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    result.identity = *after;
    return result;
}

}  // namespace bioseq::durable_file_io::detail

export namespace bioseq::durable_file_io {

/**
 * Publish bytes without replacing an existing destination.
 *
 * An existing destination is accepted only when a stable no-follow read is
 * byte-identical and its permission bits are no broader than `options.mode`;
 * that inode and its parent directory are then fsynced before returning
 * `already_present_identical`.
 */
[[nodiscard]] inline std::expected<durable_publish_result, file_io_error>
durable_create_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    const durable_publish_options& options = {},
    const file_io_hooks& hooks = {},
    const positional_io_operations& operations = {}) {
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_bytes(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        operations);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    if (auto verified = detail::verify_staged_path(
            directory.get(), *staged, hooks);
        !verified.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            staged->name,
            staged->identity,
            verified.error().publication,
            verified.error(),
            hooks));
    }
    const std::string temporary = staged->name;
    if (auto linked = detail::link_name_no_replace_at(
            directory.get(),
            temporary,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !linked.has_value()) {
        if (linked.error().code != file_io_errc::destination_exists) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
                staged->identity,
                linked.error().publication,
                linked.error(),
                hooks));
        }
        const stable_read_options read_options{
            .minimum_bytes = bytes.size(),
            .maximum_bytes = bytes.size(),
            .exact_bytes = bytes.size(),
            .require_exact_size = true,
        };
        auto existing = detail::read_stable_regular_file_at(
            directory.get(),
            destination_name,
            read_options,
            hooks,
            operations);
        if (!existing.has_value()) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
                staged->identity,
                publication_state::unchanged,
                existing.error(),
                hooks));
        }
        constexpr std::uint64_t controlled_permissions =
            S_IRWXU | S_IRWXG | S_IRWXO
            | S_ISUID | S_ISGID | S_ISVTX;
        const std::uint64_t existing_permissions =
            existing->identity.mode & controlled_permissions;
        if ((existing_permissions
             & ~static_cast<std::uint64_t>(options.mode)) != 0) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
                staged->identity,
                publication_state::unchanged,
                detail::make_error(
                    file_io_errc::permission_mismatch,
                    file_io_operation::stat_after,
                    EACCES),
                hooks));
        }
        if (existing->bytes.size() != bytes.size()
            || !std::equal(
                existing->bytes.begin(), existing->bytes.end(),
                bytes.begin(), bytes.end())) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
                staged->identity,
                publication_state::unchanged,
                detail::make_error(
                    file_io_errc::content_mismatch,
                    file_io_operation::publish_link,
                    EEXIST),
                hooks));
        }
        if (auto synced = detail::sync_existing_regular_file_at(
                directory.get(),
                destination_name,
                hooks,
                &existing->identity);
            !synced.has_value()) {
            return std::unexpected(detail::cleanup_error_or(
                directory.get(),
                temporary,
                staged->identity,
                publication_state::unchanged,
                synced.error(),
                hooks));
        }
        if (auto removed = detail::remove_published_staging_name(
                directory.get(),
                temporary,
                staged->identity,
                hooks);
            !removed.has_value()) {
            return std::unexpected(removed.error());
        }
        if (auto directory_synced = fsync_directory_descriptor(
                directory.get(),
                hooks,
                publication_state::visible_not_durable);
            !directory_synced.has_value()) {
            return std::unexpected(directory_synced.error());
        }
        return durable_publish_result{
            .publication = publication_state::durable,
            .disposition =
                durable_create_disposition::already_present_identical,
        };
    }
    if (auto removed = detail::remove_published_staging_name(
            directory.get(),
            temporary,
            staged->identity,
            hooks);
        !removed.has_value()) {
        return std::unexpected(removed.error());
    }
    if (auto directory_synced = fsync_directory_descriptor(
            directory.get(),
            hooks,
            publication_state::visible_not_durable);
        !directory_synced.has_value()) {
        return std::unexpected(directory_synced.error());
    }
    return durable_publish_result{
        .publication = publication_state::durable,
        .disposition = durable_create_disposition::created,
    };
}

/** Atomically replace a destination and durably publish its directory entry. */
[[nodiscard]] inline std::expected<durable_publish_result, file_io_error>
durable_replace_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    const durable_publish_options& options = {},
    const file_io_hooks& hooks = {},
    const positional_io_operations& operations = {}) {
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_bytes(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        operations);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    if (auto verified = detail::verify_staged_path(
            directory.get(), *staged, hooks);
        !verified.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            staged->name,
            staged->identity,
            verified.error().publication,
            verified.error(),
            hooks));
    }
    const std::string temporary = staged->name;
    if (auto renamed = detail::rename_name_at(
            directory.get(),
            temporary,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !renamed.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            temporary,
            staged->identity,
            renamed.error().publication,
            renamed.error(),
            hooks));
    }
    if (auto directory_synced = fsync_directory_descriptor(
            directory.get(),
            hooks,
            publication_state::visible_not_durable);
        !directory_synced.has_value()) {
        return std::unexpected(directory_synced.error());
    }
    return durable_publish_result{
        .publication = publication_state::durable,
        .disposition = durable_create_disposition::replaced,
    };
}

/**
 * Publish bytes with an explicit namespace policy.
 *
 * `exclusive_no_replace` never accepts or alters an existing destination.
 * Callers which need idempotent byte-identical reconciliation may use
 * `durable_create_file` explicitly after deciding that policy is appropriate.
 */
[[nodiscard]] inline std::expected<durable_publish_result, file_io_error>
durable_publish_file(
    const std::filesystem::path& destination,
    std::span<const std::byte> bytes,
    publish_policy policy,
    const durable_publish_options& options = {},
    const file_io_hooks& hooks = {},
    const positional_io_operations& operations = {}) {
    if (policy == publish_policy::replace_existing) {
        return durable_replace_file(
            destination, bytes, options, hooks, operations);
    }
    const std::string destination_name =
        detail::leaf_name(destination);
    if (destination.empty()
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::create_temporary,
            EINVAL));
    }
    const std::filesystem::path parent =
        detail::parent_directory(destination);
    auto directory_opened = detail::open_directory_path(parent, hooks);
    if (!directory_opened.has_value()) {
        return std::unexpected(directory_opened.error());
    }
    file_descriptor directory = std::move(*directory_opened);
    auto staged = detail::stage_bytes(
        directory.get(),
        destination_name,
        bytes,
        options,
        hooks,
        operations);
    if (!staged.has_value()) {
        return std::unexpected(staged.error());
    }
    if (auto verified = detail::verify_staged_path(
            directory.get(), *staged, hooks);
        !verified.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            staged->name,
            staged->identity,
            verified.error().publication,
            verified.error(),
            hooks));
    }
    const std::string temporary = staged->name;
    if (auto linked = detail::link_name_no_replace_at(
            directory.get(),
            temporary,
            directory.get(),
            destination_name,
            hooks,
            &staged->identity);
        !linked.has_value()) {
        return std::unexpected(detail::cleanup_error_or(
            directory.get(),
            temporary,
            staged->identity,
            linked.error().publication,
            linked.error(),
            hooks));
    }
    if (auto removed = detail::remove_published_staging_name(
            directory.get(),
            temporary,
            staged->identity,
            hooks);
        !removed.has_value()) {
        return std::unexpected(removed.error());
    }
    if (auto directory_synced = fsync_directory_descriptor(
            directory.get(),
            hooks,
            publication_state::visible_not_durable);
        !directory_synced.has_value()) {
        return std::unexpected(directory_synced.error());
    }
    return durable_publish_result{
        .publication = publication_state::durable,
        .disposition = durable_create_disposition::created,
    };
}

/**
 * Fsync and atomically rename an existing regular file, then fsync every
 * directory whose namespace changed.
 */
[[nodiscard]] inline std::expected<durable_publish_result, file_io_error>
durable_rename_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const file_io_hooks& hooks = {}) {
    const std::filesystem::path source_parent =
        detail::parent_directory(source);
    const std::filesystem::path destination_parent =
        detail::parent_directory(destination);
    const std::string source_name = detail::leaf_name(source);
    const std::string destination_name =
        detail::leaf_name(destination);
    if (source.empty() || destination.empty()
        || !detail::valid_leaf_name(source_name)
        || !detail::valid_leaf_name(destination_name)) {
        return std::unexpected(detail::make_error(
            file_io_errc::invalid_argument,
            file_io_operation::publish_rename,
            EINVAL));
    }
    auto source_opened = detail::open_directory_path(
        source_parent, hooks);
    if (!source_opened.has_value()) {
        return std::unexpected(source_opened.error());
    }
    file_descriptor source_directory = std::move(*source_opened);
    file_descriptor destination_directory{};
    int destination_descriptor = source_directory.get();
    if (source_parent.lexically_normal()
            != destination_parent.lexically_normal()) {
        auto destination_opened = detail::open_directory_path(
            destination_parent, hooks);
        if (!destination_opened.has_value()) {
            return std::unexpected(destination_opened.error());
        }
        destination_directory = std::move(*destination_opened);
        destination_descriptor = destination_directory.get();
    }
    auto source_identity = detail::sync_existing_regular_file_at(
        source_directory.get(), source_name, hooks);
    if (!source_identity.has_value()) {
        return std::unexpected(source_identity.error());
    }
    if (auto renamed = detail::rename_name_at(
            source_directory.get(),
            source_name,
            destination_descriptor,
            destination_name,
            hooks,
            &*source_identity);
        !renamed.has_value()) {
        return std::unexpected(renamed.error());
    }
    if (auto synced = fsync_directory_descriptor(
            destination_descriptor,
            hooks,
            publication_state::visible_not_durable);
        !synced.has_value()) {
        return std::unexpected(synced.error());
    }
    if (source_parent.lexically_normal()
            != destination_parent.lexically_normal()) {
        if (auto synced = fsync_directory_descriptor(
                source_directory.get(),
                hooks,
                publication_state::visible_not_durable);
            !synced.has_value()) {
            return std::unexpected(synced.error());
        }
    }
    return durable_publish_result{
        .publication = publication_state::durable,
        .disposition = durable_create_disposition::renamed,
    };
}

}  // namespace bioseq::durable_file_io
