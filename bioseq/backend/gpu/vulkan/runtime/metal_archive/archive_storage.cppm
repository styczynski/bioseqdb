/**
 * @file archive_storage.cppm
 * @brief Loads Metal archives from checked temporary files and keeps their load records.
 */
module;

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <libassert/assert.hpp>
#include <new>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:metal_archive.archive_storage;

import :metal_archive.command_interpose;
import :receipts;
import :metal_archive.configuration;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.instrumentation;
import std;

export namespace bioseq::gpu::vulkan {

#if defined(__APPLE__)
/// Pipeline-creation scope that uses the process's loaded archives.
struct active_metal_archive_pipeline_scope;

/// @brief Loaded Metal archives, pipeline wrappers, and their process-wide check results.
struct metal_pipeline_archive_interpose {
    objc_runtime_functions objc{};
    std::shared_ptr<const runtime_metal_archive_configuration_image> configuration_image;
    std::array<std::uint8_t, VK_UUID_SIZE> runtime_vulkan_device_uuid{};
    runtime_vulkan_device_identity runtime_vulkan_device_identity{};
    std::uint64_t runtime_metal_registry_id = 0u;
    runtime_metal_archive_set_identity configured_identity{};
    runtime_metal_archive_set_identity loaded_identity{};
    // Keep the memory reservation until the destructor has released the
    // Objective-C archive array and its archives.
    bioseq::parallelism::memory_permit archive_residency_permit;
    std::uint64_t archive_residency_peak_bytes = 0u;
    std::uint64_t archive_residency_retained_bytes = 0u;
    void* loader_device = nullptr;
    void* archive_array = nullptr;
    runtime_metal_archive_process_qualification process_qualification{};
    void* interposed_new_pipeline_async_method = nullptr;
    void* interposed_new_pipeline_sync_method = nullptr;
    void* original_new_pipeline_async = nullptr;
    void* original_new_pipeline_sync = nullptr;
    void* sel_new_pipeline_sync = nullptr;
    void* sel_set_binary_archives = nullptr;
    void* sel_compute_function = nullptr;
    void* sel_name = nullptr;
    void* sel_utf8_string = nullptr;
    void* sel_error_domain = nullptr;
    void* sel_error_code = nullptr;
    void* sel_count = nullptr;
    void* sel_registry_id = nullptr;
    void* sel_release = nullptr;
    void* sel_max_total_threads = nullptr;
    void* sel_threadgroup_multiple = nullptr;
    std::uint64_t loader_registry_id = 0u;
    std::uint64_t loaded_pre_post_identity_checks = 0u;
    bool configuration_validated = false;
    bool installed = false;
    std::atomic<bool> sticky_failure{false};
    std::mutex failure_mu;
    std::string failure_reason;
    std::atomic<std::uint64_t> next_scope_identity{1u};
    std::atomic<std::uint64_t> next_receipt_identity{1u};
    std::mutex active_scope_mu;
    std::shared_ptr<active_metal_archive_pipeline_scope> active_scope;
    std::atomic<std::uint64_t> outside_scope_interceptions{0u};

    /// @brief Releases the archive array before releasing its Metal device.
    ~metal_pipeline_archive_interpose() {
        if (sel_release != nullptr && objc.msg_send != nullptr) {
            using release_function = void (*)(void*, void*);
            if (archive_array != nullptr) {
                objc_msg_cast<release_function>(objc.msg_send)(archive_array, sel_release);
                archive_array = nullptr;
            }
            if (loader_device != nullptr) {
                objc_msg_cast<release_function>(objc.msg_send)(loader_device, sel_release);
                loader_device = nullptr;
            }
        }
    }
};

/// @brief Returns the process-wide state for Metal archive loading and pipeline wrappers.
metal_pipeline_archive_interpose& metal_pipeline_archive_interpose_state() noexcept {
    static metal_pipeline_archive_interpose state;
    return state;
}

/// @brief Frees loaded archives and clears their check records, keeping any recorded failure.
/// @param[in,out] state Process state to reset.
void rollback_metal_archive_loaded_state(metal_pipeline_archive_interpose& state) noexcept {
    if (state.archive_array != nullptr && state.sel_release != nullptr &&
        state.objc.msg_send != nullptr) {
        using release_function = void (*)(void*, void*);
        objc_msg_cast<release_function>(state.objc.msg_send)(state.archive_array,
                                                             state.sel_release);
        state.archive_array = nullptr;
    }
    state.archive_residency_permit.release();
    state.archive_residency_peak_bytes = 0u;
    state.archive_residency_retained_bytes = 0u;
    state.loaded_identity = {};
    state.loaded_pre_post_identity_checks = 0u;
    state.process_qualification = {};
}

/// @brief Undoes a partial wrapper installation and frees its loaded objects.
/// @param[in,out] state Process state whose original Metal methods are restored.
void rollback_metal_pipeline_archive_interpose_install(
    metal_pipeline_archive_interpose& state) noexcept {
    if (state.objc.method_set_implementation != nullptr) {
        if (state.interposed_new_pipeline_async_method != nullptr &&
            state.original_new_pipeline_async != nullptr) {
            (void)state.objc.method_set_implementation(state.interposed_new_pipeline_async_method,
                                                       state.original_new_pipeline_async);
        }
        if (state.interposed_new_pipeline_sync_method != nullptr &&
            state.original_new_pipeline_sync != nullptr) {
            (void)state.objc.method_set_implementation(state.interposed_new_pipeline_sync_method,
                                                       state.original_new_pipeline_sync);
        }
    }
    state.installed = false;
    state.interposed_new_pipeline_async_method = nullptr;
    state.interposed_new_pipeline_sync_method = nullptr;
    state.original_new_pipeline_async = nullptr;
    state.original_new_pipeline_sync = nullptr;
    rollback_metal_archive_loaded_state(state);
    if (state.loader_device != nullptr && state.sel_release != nullptr &&
        state.objc.msg_send != nullptr) {
        using release_function = void (*)(void*, void*);
        objc_msg_cast<release_function>(state.objc.msg_send)(state.loader_device,
                                                             state.sel_release);
    }
    state.loader_device = nullptr;
    state.configuration_image.reset();
    state.configured_identity = {};
    state.configuration_validated = false;
    state.runtime_vulkan_device_uuid = {};
    state.runtime_vulkan_device_identity = {};
    state.runtime_metal_registry_id = 0u;
    state.loader_registry_id = 0u;
    state.sel_new_pipeline_sync = nullptr;
    state.sel_set_binary_archives = nullptr;
    state.sel_compute_function = nullptr;
    state.sel_name = nullptr;
    state.sel_utf8_string = nullptr;
    state.sel_error_domain = nullptr;
    state.sel_error_code = nullptr;
    state.sel_count = nullptr;
    state.sel_registry_id = nullptr;
    state.sel_release = nullptr;
    state.sel_max_total_threads = nullptr;
    state.sel_threadgroup_multiple = nullptr;
    state.objc = {};
}

/// @brief Copies an NSString into a C++ UTF-8 string.
/// @param[in] objc Objective-C messaging functions.
/// @param[in] ns_string String to copy, or null.
/// @param[in] sel_utf8_string Registered UTF8String selector.
/// @return Copied text, or an empty string if conversion is unavailable.
[[nodiscard]] std::string
bioseq_ns_string_utf8(objc_runtime_functions& objc, void* ns_string, void* sel_utf8_string) {
    if (ns_string == nullptr || sel_utf8_string == nullptr) {
        return {};
    }
    using utf8_function = const char* (*)(void*, void*);
    const char* text = objc_msg_cast<utf8_function>(objc.msg_send)(ns_string, sel_utf8_string);
    return text == nullptr ? std::string{} : std::string(text);
}

/// @brief Copies UTF-8 text into an autoreleased NSString.
/// @param[in] objc Objective-C messaging functions.
/// @param[in] value Text to copy.
/// @return NSString managed by the autorelease pool, or null if creation is unavailable.
[[nodiscard]] void* bioseq_make_ns_string(objc_runtime_functions& objc, std::string_view value) {
    void* ns_string_cls = objc.get_class("NSString");
    if (ns_string_cls == nullptr) {
        return nullptr;
    }
    std::string owned(value);
    using string_function = void* (*)(void*, void*, const char*);
    return objc_msg_cast<string_function>(objc.msg_send)(
        ns_string_cls, objc.sel_register_name("stringWithUTF8String:"), owned.c_str());
}

/// @brief Creates an autoreleased file URL for an archive path.
/// @param[in] objc Objective-C messaging functions.
/// @param[in] path Path to copy into the URL.
/// @return NSURL managed by the autorelease pool, or null if creation is unavailable.
[[nodiscard]] void* bioseq_make_file_url(objc_runtime_functions& objc,
                                         const std::filesystem::path& path) {
    void* url_cls = objc.get_class("NSURL");
    void* ns_path = bioseq_make_ns_string(objc, path.string());
    if (url_cls == nullptr || ns_path == nullptr) {
        return nullptr;
    }
    using url_function = void* (*)(void*, void*, void*);
    return objc_msg_cast<url_function>(objc.msg_send)(
        url_cls, objc.sel_register_name("fileURLWithPath:"), ns_path);
}

/// @brief Copies a Metal error's localized description into a C++ string.
/// @param[in] objc Objective-C messaging functions.
/// @param[in] error NSError to inspect, or null.
/// @return Copied description, or an empty string for a null error.
[[nodiscard]] std::string bioseq_ns_error_description(objc_runtime_functions& objc, void* error) {
    if (error == nullptr) {
        return {};
    }
    using get_object_function = void* (*)(void*, void*);
    void* desc = objc_msg_cast<get_object_function>(objc.msg_send)(
        error, objc.sel_register_name("localizedDescription"));
    return bioseq_ns_string_utf8(objc, desc, objc.sel_register_name("UTF8String"));
}

/// @brief Records a failure that makes later archive checks fail.
/// @param[in,out] state Process state to mark as failed.
/// @param[in] reason Message saved if no earlier message was recorded.
void mark_metal_archive_sticky_failure(metal_pipeline_archive_interpose& state,
                                       std::string reason) noexcept {
    state.sticky_failure.store(true, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> guard(state.failure_mu);
        if (state.failure_reason.empty())
            state.failure_reason = std::move(reason);
    } catch (...) {
        // The failure flag remains set even if saving its message runs out of memory.
    }
}

/// @brief Records a failure using a C string without letting allocation errors escape.
/// @param[in,out] state Process state to mark as failed.
/// @param[in] reason Null-terminated message copied when possible.
void mark_metal_archive_sticky_failure_literal(metal_pipeline_archive_interpose& state,
                                               const char* reason) noexcept {
    state.sticky_failure.store(true, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> guard(state.failure_mu);
        if (state.failure_reason.empty())
            state.failure_reason.assign(reason);
    } catch (...) {
    }
}

/// @brief Copies the first recorded failure message under its mutex.
/// @param[in] state Process state to inspect.
/// @return Saved message, or a generic archive-check error.
[[nodiscard]] std::string metal_archive_failure_reason(metal_pipeline_archive_interpose& state) {
    std::lock_guard<std::mutex> guard(state.failure_mu);
    return state.failure_reason.empty() ? std::string{"immutable Metal archive receipt failed"}
                                        : state.failure_reason;
}

/// @brief Archive digest, file metadata, and byte counts captured during verification.
struct metal_archive_file_snapshot {
    runtime_metal_archive_spec actual{};
    std::uint64_t device = 0u;
    std::uint64_t inode = 0u;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_nanoseconds = 0;
    std::uint64_t streamed_bytes = 0u;
    std::uint64_t scratch_capacity_bytes = 0u;
    bool exact_seed_compared = false;

    /// @brief Compares the archive details, file metadata, and verification measurements.
    [[nodiscard]] bool operator==(const metal_archive_file_snapshot&) const noexcept = default;
};

/// @brief Checks an archive's size and hash, comparing path metadata before and after reading.
/// @param[in] physical_path Canonical path to a regular file.
/// @param[in] expected Expected archive details, including size and SHA-256.
/// @param[in] exact_seed_bytes Optional seed bytes to compare with every file byte.
/// @return Captured file details, or the failed validation or read check.
[[nodiscard]] std::expected<metal_archive_file_snapshot, std::string>
capture_metal_archive_file_snapshot(const std::filesystem::path& physical_path,
                                    const runtime_metal_archive_spec& expected,
                                    std::span<const std::byte> exact_seed_bytes = {}) {
    const std::filesystem::path path = physical_path;
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    if (ec || canonical != path) {
        return std::unexpected(
            fmt::format("private Metal archive snapshot path is not canonical for {}",
                        expected.artifact_filename));
    }

    struct stat before{};
    if (::lstat(path.c_str(), &before) != 0 || !S_ISREG(before.st_mode)) {
        return std::unexpected(fmt::format(
            "sealed Metal archive is missing, non-regular, or a symlink: {}", path.string()));
    }
    if (before.st_size <= 0) {
        return std::unexpected(fmt::format("sealed Metal archive is empty: {}", path.string()));
    }
    if (static_cast<std::uint64_t>(before.st_size) != expected.size ||
        (!exact_seed_bytes.empty() && exact_seed_bytes.size() != expected.size)) {
        return std::unexpected(
            fmt::format("private Metal archive size differs from retained seed for {}",
                        expected.artifact_filename));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(fmt::format("cannot open private Metal archive {}", path.string()));
    }
    constexpr std::size_t scratch_bytes = 64u * 1024u;
    std::array<std::byte, scratch_bytes> scratch{};
    sealed_shader_cache_detail::sha256_hasher hasher;
    std::uint64_t streamed_bytes = 0u;
    bool seed_matches = true;
    for (;;) {
        input.read(reinterpret_cast<char*>(scratch.data()),
                   static_cast<std::streamsize>(scratch.size()));
        const std::streamsize count = input.gcount();
        if (count < 0) {
            return std::unexpected(
                fmt::format("cannot stream private Metal archive {}", path.string()));
        }
        if (count == 0)
            break;
        const std::size_t chunk_bytes = static_cast<std::size_t>(count);
        const std::span<const std::byte> chunk{scratch.data(), chunk_bytes};
        hasher.update(chunk);
        if (!exact_seed_bytes.empty()) {
            const std::size_t offset = static_cast<std::size_t>(streamed_bytes);
            if (offset > exact_seed_bytes.size() ||
                chunk_bytes > exact_seed_bytes.size() - offset ||
                !std::equal(chunk.begin(),
                            chunk.end(),
                            exact_seed_bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
                seed_matches = false;
            }
        }
        streamed_bytes += static_cast<std::uint64_t>(chunk_bytes);
        if (!input && !input.eof()) {
            return std::unexpected(
                fmt::format("cannot stream private Metal archive {}", path.string()));
        }
    }
    if (streamed_bytes != expected.size || !seed_matches) {
        return std::unexpected(
            fmt::format("private Metal archive bytes differ from retained seed for {}",
                        expected.artifact_filename));
    }
    struct stat after{};
    if (::lstat(path.c_str(), &after) != 0 || !S_ISREG(after.st_mode) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
        before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
        before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) {
        return std::unexpected(
            fmt::format("sealed Metal archive changed while hashing: {}", path.string()));
    }
    runtime_metal_archive_spec actual{
        .artifact_ordinal = expected.artifact_ordinal,
        .artifact_filename = expected.artifact_filename,
        .size = static_cast<std::uint64_t>(after.st_size),
        .sha256 = hasher.finalize(),
        .negative_error_domain = expected.negative_error_domain,
        .negative_error_code = expected.negative_error_code,
    };
    if (actual != expected) {
        return std::unexpected(fmt::format("private Metal archive size or SHA-256 mismatch for {}",
                                           expected.artifact_filename));
    }
    return metal_archive_file_snapshot{
        .actual = std::move(actual),
        .device = static_cast<std::uint64_t>(after.st_dev),
        .inode = static_cast<std::uint64_t>(after.st_ino),
        .modified_seconds = after.st_mtimespec.tv_sec,
        .modified_nanoseconds = after.st_mtimespec.tv_nsec,
        .changed_seconds = after.st_ctimespec.tv_sec,
        .changed_nanoseconds = after.st_ctimespec.tv_nsec,
        .streamed_bytes = streamed_bytes,
        .scratch_capacity_bytes = scratch.size(),
        .exact_seed_compared = !exact_seed_bytes.empty(),
    };
}

/// @brief Owns a temporary archive path and attempts to remove its file on destruction.
class private_metal_archive_snapshot {
  public:
    /// @brief Creates an empty snapshot-path owner.
    private_metal_archive_snapshot() = default;

    /// @brief Takes responsibility for removing a temporary archive file.
    /// @param[in] path File to remove unless ownership is transferred.
    explicit private_metal_archive_snapshot(std::filesystem::path path) : path_(std::move(path)) {}

    /// @brief Prevents copying private snapshot ownership.
    private_metal_archive_snapshot(const private_metal_archive_snapshot&) = delete;

    /// @brief Prevents copy assignment of private snapshot ownership.
    private_metal_archive_snapshot& operator=(const private_metal_archive_snapshot&) = delete;

    /// @brief Moves the file path and leaves the source empty.
    private_metal_archive_snapshot(private_metal_archive_snapshot&& other) noexcept
        : path_(std::exchange(other.path_, {})) {}

    /// @brief Attempts to remove this file, then takes ownership of another temporary path.
    private_metal_archive_snapshot& operator=(private_metal_archive_snapshot&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }

    /// @brief Attempts to remove the owned temporary file.
    ~private_metal_archive_snapshot() {
        cleanup();
    }

    /// @brief Returns the temporary file path without transferring ownership.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// @brief Returns the file path and gives the caller responsibility for removing it.
    [[nodiscard]] std::filesystem::path release_path() noexcept {
        return std::exchange(path_, {});
    }

  private:
    /// @brief Attempts to remove the file and clears its saved path.
    void cleanup() noexcept {
        if (!path_.empty()) {
            (void)::unlink(path_.c_str());
            path_.clear();
        }
    }

    std::filesystem::path path_;
};
static_assert(std::is_nothrow_move_constructible_v<private_metal_archive_snapshot>);

/// @brief Closes a temporary archive file descriptor unless ownership is transferred.
class private_snapshot_file_descriptor {
  public:
    /// @brief Takes responsibility for closing a file descriptor.
    /// @param[in] fd Descriptor to own.
    explicit private_snapshot_file_descriptor(int fd) noexcept : fd_(fd) {}

    /// @brief Prevents copying descriptor ownership.
    private_snapshot_file_descriptor(const private_snapshot_file_descriptor&) = delete;

    /// @brief Prevents copy assignment of descriptor ownership.
    private_snapshot_file_descriptor& operator=(const private_snapshot_file_descriptor&) = delete;

    /// @brief Closes the owned descriptor.
    ~private_snapshot_file_descriptor() noexcept {
        if (fd_ >= 0)
            (void)::close(fd_);
    }

    /// @brief Returns the descriptor while this object keeps ownership.
    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    /// @brief Returns the descriptor and gives the caller responsibility for closing it.
    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    /// @brief Closes the descriptor now.
    /// @return Zero if closed or already empty, otherwise the close error from errno.
    [[nodiscard]] int close_now() noexcept {
        const int fd = std::exchange(fd_, -1);
        if (fd < 0 || ::close(fd) == 0)
            return 0;
        return errno;
    }

  private:
    int fd_ = -1;
};

/// @brief Attempts to remove an unfinished temporary file unless cleanup is handed to another owner.
class private_snapshot_unlink_guard {
  public:
    /// @brief Records a temporary path to remove on scope exit.
    /// @param[in] path Path text that must remain valid until release or destruction.
    explicit private_snapshot_unlink_guard(const char* path) noexcept : path_(path) {}

    /// @brief Prevents copying responsibility for removing the file.
    private_snapshot_unlink_guard(const private_snapshot_unlink_guard&) = delete;

    /// @brief Prevents copying file-removal responsibility by assignment.
    private_snapshot_unlink_guard& operator=(const private_snapshot_unlink_guard&) = delete;

    /// @brief Attempts to remove the file if this object still owns cleanup.
    ~private_snapshot_unlink_guard() noexcept {
        if (path_ != nullptr)
            (void)::unlink(path_);
    }

    /// @brief Stops this object from removing a file now owned elsewhere.
    void release() noexcept {
        path_ = nullptr;
    }

  private:
    const char* path_ = nullptr;
};

/// @brief Writes seed bytes to an owner-only temporary file and flushes them to disk.
/// @param[in] artifact_filename Archive name used in the temporary filename and errors.
/// @param[in] bytes Archive contents to write.
/// @param[in] temp_parent_override Optional directory used by tests.
/// @return Owner that attempts removal on destruction, or a file-creation or I/O error.
[[nodiscard]] std::expected<private_metal_archive_snapshot, std::string>
write_private_metal_archive_snapshot(std::string_view artifact_filename,
                                     std::span<const std::byte> bytes,
                                     const std::filesystem::path& temp_parent_override = {}) {
    std::error_code temp_error;
    const std::filesystem::path temp_dir = temp_parent_override.empty()
                                               ? std::filesystem::temp_directory_path(temp_error)
                                               : temp_parent_override;
    if (temp_error || !temp_dir.is_absolute()) {
        return std::unexpected(
            fmt::format("cannot resolve private Metal archive snapshot directory for {}: {}",
                        artifact_filename,
                        temp_error ? temp_error.message() : "path is not absolute"));
    }
    std::error_code canonical_error;
    const std::filesystem::path canonical_temp_dir =
        std::filesystem::canonical(temp_dir, canonical_error);
    if (canonical_error || !canonical_temp_dir.is_absolute()) {
        return std::unexpected(
            fmt::format("cannot canonicalize private Metal archive snapshot directory for {}: {}",
                        artifact_filename,
                        canonical_error ? canonical_error.message() : "path is not absolute"));
    }
    std::filesystem::path pattern =
        canonical_temp_dir /
        (".bioseq-metalar-snapshot-" + std::string(artifact_filename) + "-XXXXXX");
    std::string owned_pattern = pattern.string();
    std::vector<char> writable(owned_pattern.begin(), owned_pattern.end());
    writable.push_back('\0');
    const int fd = ::mkstemp(writable.data());
    if (fd < 0) {
        return std::unexpected(
            fmt::format("cannot create private Metal archive snapshot for {}: {}",
                        artifact_filename,
                        std::strerror(errno)));
    }
    private_snapshot_file_descriptor fd_owner{fd};
    private_snapshot_unlink_guard unlink_owner{writable.data()};
    std::filesystem::path snapshot_path(writable.data());
    if (::fchmod(fd_owner.get(), S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected(fmt::format("cannot restrict private Metal archive snapshot {}: {}",
                                           snapshot_path.string(),
                                           std::strerror(errno)));
    }
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written =
            ::write(fd_owner.get(), bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            return std::unexpected(fmt::format("cannot write private Metal archive snapshot {}: {}",
                                               snapshot_path.string(),
                                               std::strerror(errno)));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd_owner.get()) != 0) {
        return std::unexpected(fmt::format("cannot fsync private Metal archive snapshot {}: {}",
                                           snapshot_path.string(),
                                           std::strerror(errno)));
    }
    const int close_error = fd_owner.close_now();
    if (close_error != 0) {
        return std::unexpected(fmt::format("cannot close private Metal archive snapshot {}: {}",
                                           snapshot_path.string(),
                                           std::strerror(close_error)));
    }
    private_metal_archive_snapshot result{std::move(snapshot_path)};
    unlink_owner.release();
    return std::expected<private_metal_archive_snapshot, std::string>{std::in_place,
                                                                      std::move(result)};
}

/// @brief Owns the private directory used while loading archive files.
class private_metal_archive_process_directory {
  public:
    /// @brief Creates an empty private-directory owner.
    private_metal_archive_process_directory() = default;

    /// @brief Takes responsibility for removing a private archive directory.
    /// @param[in] path Directory to remove unless ownership is moved.
    explicit private_metal_archive_process_directory(std::filesystem::path path)
        : path_(std::move(path)) {}

    /// @brief Prevents copying private-directory ownership.
    private_metal_archive_process_directory(const private_metal_archive_process_directory&) =
        delete;

    /// @brief Prevents copy assignment of private-directory ownership.
    private_metal_archive_process_directory&
    operator=(const private_metal_archive_process_directory&) = delete;

    /// @brief Moves the directory path and leaves the source empty.
    private_metal_archive_process_directory(
        private_metal_archive_process_directory&& other) noexcept
        : path_(std::exchange(other.path_, {})) {}

    /// @brief Attempts to remove the owned directory.
    ~private_metal_archive_process_directory() noexcept {
        if (!path_.empty())
            (void)::rmdir(path_.c_str());
    }

    /// @brief Returns the directory path without transferring ownership.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// @brief Removes the owned directory now.
    /// @return Zero if removed or no path is owned, otherwise the rmdir error from errno.
    [[nodiscard]] int remove_now() noexcept {
        if (path_.empty())
            return 0;
        if (::rmdir(path_.c_str()) == 0) {
            path_.clear();
            return 0;
        }
        return errno;
    }

  private:
    std::filesystem::path path_;
};

/// @brief Creates an owner-only temporary directory for loading Metal archives.
/// @return Directory owner, or an error if creation or permission checks fail.
[[nodiscard]] std::expected<private_metal_archive_process_directory, std::string>
make_private_metal_archive_process_directory() {
    std::error_code temp_error;
    const std::filesystem::path temp_parent = std::filesystem::temp_directory_path(temp_error);
    std::error_code canonical_error;
    const std::filesystem::path canonical_parent =
        temp_error ? std::filesystem::path{}
                   : std::filesystem::canonical(temp_parent, canonical_error);
    if (temp_error || canonical_error || !canonical_parent.is_absolute()) {
        return std::unexpected("cannot resolve private Metal archive process directory");
    }
    std::string pattern = (canonical_parent / ".bioseq-metalar-process-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) {
        return std::unexpected(fmt::format(
            "cannot create private Metal archive process directory: {}", std::strerror(errno)));
    }
    private_metal_archive_process_directory owner{std::filesystem::path(created)};
    if (::chmod(owner.path().c_str(), S_IRWXU) != 0) {
        return std::unexpected(fmt::format(
            "cannot restrict private Metal archive process directory: {}", std::strerror(errno)));
    }
    struct stat status{};
    if (::lstat(owner.path().c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected("private Metal archive process directory identity is invalid");
    }
    return owner;
}

/// @brief Owns an open temporary archive and the file metadata checked when it was written.
class open_private_metal_archive {
  public:
    /// @brief Creates an empty open-archive owner.
    open_private_metal_archive() = default;

    /// @brief Takes ownership of an open archive and its temporary path.
    /// @param[in] fd Open file descriptor.
    /// @param[in] path File path to remove during cleanup.
    /// @param[in] identity Expected file metadata for both the path and descriptor.
    open_private_metal_archive(int fd,
                               std::filesystem::path path,
                               sealed_shader_cache_detail::regular_file_identity identity) noexcept
        : fd_(fd), path_(std::move(path)), identity_(identity) {}

    /// @brief Prevents copying open archive ownership.
    open_private_metal_archive(const open_private_metal_archive&) = delete;

    /// @brief Prevents copy assignment of open archive ownership.
    open_private_metal_archive& operator=(const open_private_metal_archive&) = delete;

    /// @brief Moves the open file, path, and saved metadata into this owner.
    open_private_metal_archive(open_private_metal_archive&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), path_(std::exchange(other.path_, {})),
          identity_(other.identity_), linked_(std::exchange(other.linked_, false)) {}

    /// @brief Attempts to remove any remaining file name and closes the descriptor.
    ~open_private_metal_archive() noexcept {
        if (linked_ && !path_.empty())
            (void)::unlink(path_.c_str());
        if (fd_ >= 0)
            (void)::close(fd_);
    }

    /// @brief Returns the descriptor while this object keeps ownership.
    [[nodiscard]] int fd() const noexcept {
        return fd_;
    }

    /// @brief Returns the saved archive path without transferring ownership.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /// @brief Returns the file metadata saved when the archive was written.
    [[nodiscard]] const sealed_shader_cache_detail::regular_file_identity&
    identity() const noexcept {
        return identity_;
    }

    /// @brief Checks that the path and descriptor still match the saved file metadata.
    [[nodiscard]] bool do_path_and_fd_identities_match() const noexcept {
        if (fd_ < 0 || path_.empty() || !linked_)
            return false;
        struct stat fd_status{};
        struct stat path_status{};
        return ::fstat(fd_, &fd_status) == 0 && ::lstat(path_.c_str(), &path_status) == 0 &&
               S_ISREG(fd_status.st_mode) && S_ISREG(path_status.st_mode) &&
               !S_ISLNK(path_status.st_mode) &&
               sealed_shader_cache_detail::regular_file_identity_from_stat(fd_status) ==
                   identity_ &&
               sealed_shader_cache_detail::regular_file_identity_from_stat(path_status) ==
                   identity_;
    }

    /// @brief Removes the verified file name and closes its descriptor.
    /// Checks that the name is gone and the still-open file has no other links.
    /// @return Success, or the first validation, removal, or close error.
    [[nodiscard]] std::expected<void, std::string> unlink_close_and_require_absent() {
        if (!do_path_and_fd_identities_match()) {
            return std::unexpected("private Metal archive identity changed before unlink");
        }
        if (::unlink(path_.c_str()) != 0) {
            return std::unexpected(fmt::format(
                "cannot unlink private Metal archive immediately: {}", std::strerror(errno)));
        }
        linked_ = false;
        struct stat recreated{};
        errno = 0;
        if (::lstat(path_.c_str(), &recreated) == 0 || errno != ENOENT) {
            return std::unexpected(
                "private Metal archive name remained or was recreated after unlink");
        }
        struct stat fd_status{};
        if (::fstat(fd_, &fd_status) != 0) {
            return std::unexpected("unlinked private Metal archive descriptor identity changed");
        }
        const auto unlinked_identity =
            sealed_shader_cache_detail::regular_file_identity_from_stat(fd_status);
        // Unlinking changes the link count and can advance ctime. Require zero
        // remaining links and unchanged inode, size, and modification time.
        // Allow ctime to stay the same or increase.
        const bool ctime_not_older =
            unlinked_identity.changed_seconds > identity_.changed_seconds ||
            (unlinked_identity.changed_seconds == identity_.changed_seconds &&
             unlinked_identity.changed_nanoseconds >= identity_.changed_nanoseconds);
        const bool immutable_identity_matches =
            S_ISREG(fd_status.st_mode) && fd_status.st_uid == ::geteuid() &&
            (fd_status.st_mode & (S_IRWXG | S_IRWXO)) == 0 && fd_status.st_nlink == 0 &&
            unlinked_identity.device == identity_.device &&
            unlinked_identity.inode == identity_.inode &&
            unlinked_identity.size == identity_.size &&
            unlinked_identity.modified_seconds == identity_.modified_seconds &&
            unlinked_identity.modified_nanoseconds == identity_.modified_nanoseconds &&
            ctime_not_older;
        if (!immutable_identity_matches) {
            return std::unexpected("unlinked private Metal archive descriptor identity changed");
        }
        const int fd = std::exchange(fd_, -1);
        if (::close(fd) != 0) {
            return std::unexpected(
                fmt::format("cannot close unlinked private Metal archive descriptor: {}",
                            std::strerror(errno)));
        }
        path_.clear();
        return {};
    }

  private:
    int fd_ = -1;
    std::filesystem::path path_;
    sealed_shader_cache_detail::regular_file_identity identity_{};
    bool linked_ = true;
};

/// @brief Creates an owner-only archive file, writes its bytes, and keeps it open.
/// Refuses existing paths and symlinks. Flushes the file before returning.
/// @param[in] directory Private directory for this process.
/// @param[in] ordinal Archive index used in the filename.
/// @param[in] bytes Archive contents to write.
/// @return Open file owner with checked metadata, or a creation or write error.
[[nodiscard]] std::expected<open_private_metal_archive, std::string>
write_open_private_metal_archive(const std::filesystem::path& directory,
                                 std::size_t ordinal,
                                 std::span<const std::byte> bytes) {
    const std::filesystem::path path =
        directory / ("archive-" + std::to_string(ordinal) + ".metalar");
    const int fd =
        ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return std::unexpected(
            fmt::format("cannot create open private Metal archive: {}", std::strerror(errno)));
    }
    private_snapshot_file_descriptor fd_owner{fd};
    private_snapshot_unlink_guard unlink_owner{path.c_str()};
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected("cannot restrict open private Metal archive");
    }
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            return std::unexpected("cannot write open private Metal archive");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        return std::unexpected("cannot fsync open private Metal archive");
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) != bytes.size()) {
        return std::unexpected("open private Metal archive identity is invalid");
    }
    open_private_metal_archive result{
        fd_owner.get(), path, sealed_shader_cache_detail::regular_file_identity_from_stat(status)};
    (void)fd_owner.release();
    unlink_owner.release();
    return result;
}

/// @brief Reads an open archive once to check its hash and every byte against the seed.
/// Also checks the file metadata before and after reading.
/// @param[in,out] image Open archive, read from its beginning.
/// @param[in] expected Expected size and SHA-256 digest.
/// @param[in] exact_seed_bytes Seed bytes to compare.
/// @return Bytes read, or a file, content, or hash error.
[[nodiscard]] std::expected<std::uint64_t, std::string>
hash_open_private_metal_archive_once(open_private_metal_archive& image,
                                     const runtime_metal_archive_spec& expected,
                                     std::span<const std::byte> exact_seed_bytes) {
    if (!image.do_path_and_fd_identities_match() || exact_seed_bytes.size() != expected.size ||
        ::lseek(image.fd(), 0, SEEK_SET) < 0) {
        return std::unexpected("private Metal archive is not stable before qualification hash");
    }
    sealed_shader_cache_detail::sha256_hasher hasher;
    std::array<std::byte, 64u << 10u> scratch{};
    std::uint64_t streamed = 0u;
    for (;;) {
        const ssize_t count = ::read(image.fd(), scratch.data(), scratch.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            return std::unexpected("private Metal archive qualification hash read failed");
        }
        if (count == 0)
            break;
        const std::size_t chunk = static_cast<std::size_t>(count);
        if (streamed > exact_seed_bytes.size() || chunk > exact_seed_bytes.size() - streamed ||
            !std::equal(scratch.begin(),
                        scratch.begin() + static_cast<std::ptrdiff_t>(chunk),
                        exact_seed_bytes.begin() + static_cast<std::ptrdiff_t>(streamed))) {
            return std::unexpected("private Metal archive differs from retained seed bytes");
        }
        hasher.update(std::span<const std::byte>{scratch.data(), chunk});
        streamed += chunk;
    }
    if (streamed != expected.size || hasher.finalize() != expected.sha256 ||
        !image.do_path_and_fd_identities_match()) {
        return std::unexpected("private Metal archive qualification hash mismatch");
    }
    return streamed;
}

/// @brief Loads a Metal binary archive from its temporary file URL.
/// @param[in] state Objective-C functions and selectors.
/// @param[in] loader_device Metal device used to create the archive.
/// @param[in] path File path kept valid until loading finishes.
/// @return Owned archive object the caller must release, or null on failure.
[[nodiscard]] void* load_metal_binary_archive(metal_pipeline_archive_interpose& state,
                                              void* loader_device,
                                              const std::filesystem::path& path) {
    void* desc_cls = state.objc.get_class("MTLBinaryArchiveDescriptor");
    if (desc_cls == nullptr || loader_device == nullptr) {
        return nullptr;
    }
    using new_function = void* (*)(void*, void*);
    void* desc = objc_msg_cast<new_function>(state.objc.msg_send)(
        desc_cls, state.objc.sel_register_name("new"));
    scoped_objc_release desc_owner{&state.objc, state.sel_release, desc};
    void* url = bioseq_make_file_url(state.objc, path);
    if (desc == nullptr || url == nullptr) {
        return nullptr;
    }
    using set_object_function = void (*)(void*, void*, void*);
    objc_msg_cast<set_object_function>(state.objc.msg_send)(
        desc, state.objc.sel_register_name("setUrl:"), url);
    void* error = nullptr;
    using new_archive_function = void* (*)(void*, void*, void*, void**);
    void* archive = objc_msg_cast<new_archive_function>(state.objc.msg_send)(
        loader_device,
        state.objc.sel_register_name("newBinaryArchiveWithDescriptor:error:"),
        desc,
        &error);
    if (archive == nullptr) {
        bioseq_log<logging_level::debug>(
            "[vulkan_runtime] failed to load Metal binary archive {}: {}",
            path.string(),
            bioseq_ns_error_description(state.objc, error));
    }
    return archive;
}

/// @brief Loads the accepted archives from temporary files and saves their check results.
/// Writes and verifies each file, loads its Metal object, then removes the file.
/// Keeps the objects and their memory reservation after removing the directory.
/// @param[in,out] state Receives the archive array and load record.
/// @param[in] loader_device Metal device used for loading.
/// @return Whether loading, cleanup, and record checks succeeded.
[[nodiscard]] bool load_exact_metal_binary_archives(metal_pipeline_archive_interpose& state,
                                                    void* loader_device) {
    state.loaded_pre_post_identity_checks = 0u;
    if (!state.configuration_image || !state.configuration_image->is_valid()) {
        mark_metal_archive_sticky_failure(state,
                                          "immutable Metal archive configuration image is absent");
        return false;
    }
    const runtime_metal_archive_configuration& configuration =
        state.configuration_image->configuration();
    const runtime_metal_archive_set_identity expected_identity =
        make_runtime_metal_archive_set_identity(configuration.archives);
    const std::uint64_t residency_metadata = bioseq::common::saturating_add(
        bioseq::common::saturating_mul(expected_identity.archive_count,
                                       sizeof(runtime_metal_archive_spec) + 2u * sizeof(void*)),
        2u * 1024u);
    const std::uint64_t residency_peak = bioseq::common::saturating_mul(
        bioseq::common::saturating_add(expected_identity.total_size, residency_metadata),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (!expected_identity.is_valid() ||
        residency_peak == (std::numeric_limits<std::uint64_t>::max)()) {
        mark_metal_archive_sticky_failure(state,
                                          "immutable Metal archive residency admission saturated");
        return false;
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto residency_permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            residency_peak, &admission_failure);
    if (!residency_permit) {
        mark_metal_archive_sticky_failure(
            state,
            shader_cache_message(
                "immutable Metal archive residency admission denied before load: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(admission_failure.reason),
                " requested=",
                admission_failure.requested_bytes));
        return false;
    }
    void* array_cls = state.objc.get_class("NSMutableArray");
    if (array_cls == nullptr) {
        mark_metal_archive_sticky_failure(
            state, "NSMutableArray is unavailable for immutable Metal archives");
        return false;
    }
    using allocate_function = void* (*)(void*, void*);
    using initialize_capacity_function = void* (*)(void*, void*, unsigned long);
    void* allocated = objc_msg_cast<allocate_function>(state.objc.msg_send)(
        array_cls, state.objc.sel_register_name("alloc"));
    void* array = objc_msg_cast<initialize_capacity_function>(state.objc.msg_send)(
        allocated,
        state.objc.sel_register_name("initWithCapacity:"),
        static_cast<unsigned long>(configuration.archives.size()));
    scoped_objc_release array_owner{&state.objc, state.sel_release, array};
    if (array == nullptr) {
        mark_metal_archive_sticky_failure(state,
                                          "cannot allocate exact immutable Metal archive array");
        return false;
    }

    auto private_directory = make_private_metal_archive_process_directory();
    if (!private_directory) {
        mark_metal_archive_sticky_failure(state, std::move(private_directory.error()));
        return false;
    }
    using add_object_function = void (*)(void*, void*, void*);
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    for (std::size_t archive_index = 0u; archive_index < configuration.archives.size();
         ++archive_index) {
        const runtime_metal_archive_spec& expected = configuration.archives[archive_index];
        const sealed_shader_cache_artifact_image* seed_artifact =
            configuration.seed_image->artifact(expected.artifact_ordinal);
        if (seed_artifact == nullptr ||
            seed_artifact->descriptor.kind !=
                sealed_shader_cache_artifact_kind::metal_binary_archive ||
            seed_artifact->descriptor.filename != expected.artifact_filename ||
            seed_artifact->descriptor.size != expected.size ||
            seed_artifact->descriptor.sha256 != expected.sha256) {
            mark_metal_archive_sticky_failure(
                state,
                fmt::format(
                    "runtime Metal archive spec no longer matches retained seed bytes for {}",
                    expected.artifact_filename));
            return false;
        }
        auto private_image = write_open_private_metal_archive(
            private_directory->path(), archive_index, seed_artifact->bytes);
        if (!private_image) {
            mark_metal_archive_sticky_failure(state, std::move(private_image.error()));
            return false;
        }
        ++private_files_created;
        auto hashed =
            hash_open_private_metal_archive_once(*private_image, expected, seed_artifact->bytes);
        if (!hashed ||
            hashed_bytes > (std::numeric_limits<std::uint64_t>::max)() - (hashed ? *hashed : 0u)) {
            mark_metal_archive_sticky_failure(
                state,
                hashed ? "private Metal archive hashed byte count overflow"
                       : std::move(hashed.error()));
            return false;
        }
        hashed_bytes += *hashed;
        if (!private_image->do_path_and_fd_identities_match()) {
            mark_metal_archive_sticky_failure(
                state,
                "private Metal archive path/descriptor identity changed before synchronous load");
            return false;
        }
        if (void* archive = load_metal_binary_archive(state, loader_device, private_image->path());
            archive != nullptr) {
            scoped_objc_release archive_owner{&state.objc, state.sel_release, archive};
            if (!private_image->do_path_and_fd_identities_match()) {
                mark_metal_archive_sticky_failure(
                    state,
                    fmt::format(
                        "private Metal archive identity changed across synchronous load: {}",
                        expected.artifact_filename));
                return false;
            }
            objc_msg_cast<add_object_function>(state.objc.msg_send)(
                array, state.objc.sel_register_name("addObject:"), archive);
            auto unlinked = private_image->unlink_close_and_require_absent();
            if (!unlinked) {
                mark_metal_archive_sticky_failure(state, std::move(unlinked.error()));
                return false;
            }
            ++private_files_unlinked;
            ++state.loaded_pre_post_identity_checks;
        } else {
            mark_metal_archive_sticky_failure(
                state,
                fmt::format("Metal rejected private exact-byte archive snapshot for {}",
                            expected.artifact_filename));
            return false;
        }
    }
    if (const int remove_error = private_directory->remove_now(); remove_error != 0) {
        mark_metal_archive_sticky_failure(
            state,
            fmt::format("private Metal archive process directory did not disappear: {}",
                        std::strerror(remove_error)));
        return false;
    }
    state.loaded_identity = expected_identity;
    if (state.loaded_pre_post_identity_checks != expected_identity.archive_count) {
        mark_metal_archive_sticky_failure(
            state, "loaded Metal archive count differs from sealed expected set");
        return false;
    }
    using copy_function = void* (*)(void*, void*);
    void* immutable_array = objc_msg_cast<copy_function>(state.objc.msg_send)(
        array, state.objc.sel_register_name("copy"));
    scoped_objc_release immutable_array_owner{&state.objc, state.sel_release, immutable_array};
    using count_function = unsigned long (*)(void*, void*);
    const std::uint64_t immutable_count =
        immutable_array == nullptr ? 0u
                                   : static_cast<std::uint64_t>(objc_msg_cast<count_function>(
                                         state.objc.msg_send)(immutable_array, state.sel_count));
    if (immutable_array == nullptr || immutable_count != expected_identity.archive_count) {
        mark_metal_archive_sticky_failure(state, "cannot retain immutable Metal archive NSArray");
        return false;
    }
    const sealed_shader_cache_manifest& manifest = configuration.seed_image->manifest();
    const sealed_shader_cache_seed_receipt& seed_receipt = configuration.seed_image->receipt();
    runtime_metal_archive_process_qualification qualification{
        .wire_revision = 2u,
        .archive_set = expected_identity,
        .receipt_count = configuration.qualification_receipt_count,
        .hash_passes = 1u,
        .hashed_bytes = hashed_bytes,
        .private_files_created = private_files_created,
        .private_files_unlinked = private_files_unlinked,
        .live_private_paths = 0u,
        .pipeline_filesystem_hash_passes = 0u,
        .pipeline_filesystem_hashed_bytes = 0u,
        .helper_wire_revision = configuration.helper_wire_revision,
        .helper_build_identity = configuration.helper_build_identity,
        .helper_source = configuration.helper_source,
        .helper_executable_size = configuration.helper_executable_size,
        .helper_executable_sha256 = configuration.helper_executable_sha256,
        .helper_metal_registry_id = configuration.helper_metal_registry_id,
        .vulkan_device_uuid = state.runtime_vulkan_device_identity.device_uuid,
        .api_version = state.runtime_vulkan_device_identity.api_version,
        .driver_version = state.runtime_vulkan_device_identity.driver_version,
        .vendor_id = state.runtime_vulkan_device_identity.vendor_id,
        .device_id = state.runtime_vulkan_device_identity.device_id,
        .pipeline_cache_uuid = state.runtime_vulkan_device_identity.pipeline_cache_uuid,
        .seed_receipt_identity = seed_receipt.identity(),
        .seed_sha256 = manifest.seed_sha256,
        .seed_size = manifest.seed_size,
        .manifest_checksum = manifest.manifest_checksum,
        .saturated = false,
    };
    qualification.identity = runtime_metal_archive_process_qualification_identity(qualification);
    if (!qualification.is_internally_consistent()) {
        mark_metal_archive_sticky_failure(state,
                                          "process Metal archive qualification is incomplete");
        return false;
    }
    state.archive_residency_peak_bytes = residency_peak;
    state.archive_residency_retained_bytes =
        bioseq::common::saturating_add(expected_identity.total_size, residency_metadata);
    if (state.archive_residency_retained_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        state.archive_residency_retained_bytes > residency_permit->bytes()) {
        mark_metal_archive_sticky_failure(state,
                                          "Metal archive residency reconciliation saturated");
        state.archive_residency_retained_bytes = 0u;
        return false;
    }
    residency_permit->shrink_to(state.archive_residency_retained_bytes);
    state.archive_residency_permit = std::move(*residency_permit);
    state.archive_array = immutable_array_owner.release();
    state.process_qualification = std::move(qualification);
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] pso-interpose: loaded exact sealed archive set count=%llu "
                 "bytes=%llu checksum=%llu\n",
                 static_cast<unsigned long long>(state.loaded_identity.archive_count),
                 static_cast<unsigned long long>(state.loaded_identity.total_size),
                 static_cast<unsigned long long>(state.loaded_identity.ordered_checksum));
    bioseq_log<logging_level::debug>(
        "[vulkan_runtime] loaded exact sealed Metal archive set count={} bytes={} checksum={}",
        state.loaded_identity.archive_count,
        state.loaded_identity.total_size,
        state.loaded_identity.ordered_checksum);
    return true;
}

/// @brief Checks the saved archive-load record without opening files.
/// @param[in] state Process state holding the loaded archive array.
/// @param[in] operation_label Operation name included in errors.
/// @return Zero bytes read from disk on success, or a failed record check.
[[nodiscard]] std::expected<std::uint64_t, std::string>
verify_resident_metal_archive_snapshots(metal_pipeline_archive_interpose& state,
                                        std::string_view operation_label) {
    if (!state.configuration_image || state.archive_array == nullptr ||
        !state.process_qualification.is_internally_consistent()) {
        return std::unexpected(
            fmt::format("{} process Metal archive qualification is absent", operation_label));
    }
    if (state.loaded_identity != state.process_qualification.archive_set ||
        state.process_qualification.live_private_paths != 0u ||
        state.process_qualification.pipeline_filesystem_hash_passes != 0u ||
        state.process_qualification.pipeline_filesystem_hashed_bytes != 0u) {
        return std::unexpected(fmt::format(
            "{} process Metal archive qualification identity mismatch", operation_label));
    }
    return 0u;
}
#endif

} // namespace bioseq::gpu::vulkan
