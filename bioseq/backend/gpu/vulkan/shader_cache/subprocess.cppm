/**
 * @file subprocess.cppm
 * @brief Runs compiler processes with limits and writes cache files through temporary files.
 */
module;

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

export module bioseq.gpu.vulkan.shader_cache:subprocess;

import :paths_metadata;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief Returns OS and machine information for the cache build ID.
/// Returns `uname-unavailable` if the system query fails.
[[nodiscard]] inline std::string system_uname_digest_component() {
    utsname info{};
    if (::uname(&info) != 0) {
        return "uname-unavailable";
    }
    return shader_cache_message(
        info.sysname, ' ', info.nodename, ' ', info.release, ' ', info.version, ' ', info.machine);
}

/// @brief Output and time limits for a captured command.
struct run_capture_limits {
    std::size_t maximum_output_bytes = 4u << 20u; ///< Combined stdout and stderr byte cap.
    std::chrono::milliseconds timeout = std::chrono::minutes(5); ///< Time allowed for output capture and child completion.
};

/// @brief Maps an open descriptor to a descriptor number in the child.
struct run_capture_inherited_fd {
    int source_fd = -1; ///< Open descriptor duplicated into the child.
    int target_fd = -1; ///< Child descriptor number receiving the duplicate.
};

/// @brief Closes a subprocess file descriptor when its owner leaves scope.
class capture_file_descriptor {
  public:
    /// @brief Takes ownership of `fd`, which may be released before destruction.
    explicit capture_file_descriptor(int fd = -1) noexcept : fd_(fd) {}

    /// @brief Prevents copying descriptor ownership.
    capture_file_descriptor(const capture_file_descriptor&) = delete;

    /// @brief Prevents assignment from duplicating descriptor ownership.
    capture_file_descriptor& operator=(const capture_file_descriptor&) = delete;

    /// @brief Closes the owned descriptor.
    ~capture_file_descriptor() noexcept {
        close_now();
    }

    /// @brief Returns the descriptor while keeping ownership here.
    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    /// @brief Gives the descriptor to the caller and clears this owner.
    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    /// @brief Closes and clears the descriptor immediately.
    void close_now() noexcept {
        const int fd = std::exchange(fd_, -1);
        if (fd >= 0)
            (void)::close(fd);
    }

  private:
    int fd_ = -1;
};

/// @brief Owns the child's descriptor setup instructions for `posix_spawn`.
class capture_spawn_file_actions {
  public:
    /// @brief Creates an uninitialized spawn-actions owner.
    capture_spawn_file_actions() = default;

    /// @brief Prevents copying POSIX spawn actions.
    capture_spawn_file_actions(const capture_spawn_file_actions&) = delete;

    /// @brief Prevents assignment from copying the spawn actions.
    capture_spawn_file_actions& operator=(const capture_spawn_file_actions&) = delete;

    /// @brief Destroys initialized POSIX spawn actions.
    ~capture_spawn_file_actions() noexcept {
        if (initialized_)
            (void)::posix_spawn_file_actions_destroy(&actions_);
    }

    /// @brief Initializes the spawn action list and returns the POSIX status code.
    [[nodiscard]] int initialize() noexcept {
        const int rc = ::posix_spawn_file_actions_init(&actions_);
        initialized_ = rc == 0;
        return rc;
    }

    /// @brief Schedules duplication of `fd` to `target` in the child.
    /// Returns the POSIX status code.
    [[nodiscard]] int add_dup2(int fd, int target) noexcept {
        return ::posix_spawn_file_actions_adddup2(&actions_, fd, target);
    }

    /// @brief Schedules closing `fd` in the child.
    /// Returns the POSIX status code.
    [[nodiscard]] int add_close(int fd) noexcept {
        return ::posix_spawn_file_actions_addclose(&actions_, fd);
    }

    /// @brief Returns the action list after successful initialization.
    [[nodiscard]] const posix_spawn_file_actions_t* get() const noexcept {
        return &actions_;
    }

  private:
    posix_spawn_file_actions_t actions_{};
    bool initialized_ = false;
};

/// @brief Sets up a separate process group so a failed command can be stopped with its children.
class capture_spawn_attributes {
  public:
    /// @brief Creates an uninitialized spawn-attribute owner.
    capture_spawn_attributes() = default;

    /// @brief Prevents copying POSIX spawn attributes.
    capture_spawn_attributes(const capture_spawn_attributes&) = delete;

    /// @brief Prevents assignment from copying spawn attributes.
    capture_spawn_attributes& operator=(const capture_spawn_attributes&) = delete;

    /// @brief Destroys initialized POSIX spawn attributes.
    ~capture_spawn_attributes() noexcept {
        if (initialized_)
            (void)::posix_spawnattr_destroy(&attributes_);
    }

    /// @brief Makes the child's PID its process-group ID.
    /// Returns the POSIX status code.
    [[nodiscard]] int initialize_process_group() noexcept {
        int rc = ::posix_spawnattr_init(&attributes_);
        if (rc != 0)
            return rc;
        initialized_ = true;
        rc = ::posix_spawnattr_setpgroup(&attributes_, 0);
        if (rc != 0)
            return rc;
        return ::posix_spawnattr_setflags(&attributes_, POSIX_SPAWN_SETPGROUP);
    }

    /// @brief Returns the attributes after successful initialization.
    [[nodiscard]] const posix_spawnattr_t* get() const noexcept {
        return &attributes_;
    }

  private:
    posix_spawnattr_t attributes_{};
    bool initialized_ = false;
};

/// @brief Owns a captured child process and stops its group if capture fails.
class captured_child_process {
  public:
    /// @brief Takes ownership of the output descriptor and child process.
    /// `pid` must also identify the child's process group.
    captured_child_process(int read_fd, pid_t pid) noexcept : read_fd_(read_fd), pid_(pid) {}

    /// @brief Prevents copying child-process ownership.
    captured_child_process(const captured_child_process&) = delete;

    /// @brief Prevents assignment from duplicating child-process ownership.
    captured_child_process& operator=(const captured_child_process&) = delete;

    /// @brief Closes the output pipe and stops the process group unless capture completed.
    ~captured_child_process() noexcept {
        close_read();
        if (!completed_)
            terminate_group_and_reap();
    }

    /// @brief Returns the descriptor used to read captured output.
    [[nodiscard]] int read_fd() const noexcept {
        return read_fd_;
    }

    /// @brief Closes the captured-output descriptor.
    void close_read() noexcept {
        if (read_fd_ >= 0) {
            (void)::close(read_fd_);
            read_fd_ = -1;
        }
    }

    /// @brief Records that `waitpid` has collected the child's exit status.
    void mark_reaped() noexcept {
        direct_child_reaped_ = true;
    }

    /// @brief Marks capture successful so destruction leaves the process group alone.
    void mark_completed() noexcept {
        completed_ = true;
    }

  private:
    /// @brief Result of checking child completion without waiting.
    enum class reap_probe : std::uint8_t {
        running, ///< Direct child is still executing.
        reaped, ///< The child exited and its exit status was collected.
        no_longer_a_child, ///< Process is no longer this parent's direct child.
        error, ///< `waitpid` failed with an unexpected error.
    };

    /// @brief Checks whether the child exited and collects its status if available.
    /// Returns immediately while the child is running.
    [[nodiscard]] reap_probe probe_reap() noexcept {
        if (direct_child_reaped_)
            return reap_probe::reaped;
        int status = 0;
        for (;;) {
            const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                direct_child_reaped_ = true;
                return reap_probe::reaped;
            }
            if (waited == 0)
                return reap_probe::running;
            if (waited < 0 && errno == EINTR)
                continue;
            if (waited < 0 && errno == ECHILD) {
                direct_child_reaped_ = true;
                return reap_probe::no_longer_a_child;
            }
            return reap_probe::error;
        }
    }

    /// @brief Sends SIGKILL to the process group and tries to collect the child's exit status.
    /// Polls for child exit for up to one second after the signal.
    void terminate_group_and_reap() noexcept {
        if (pid_ <= 0)
            return;
        const reap_probe before = probe_reap();
        if (before == reap_probe::no_longer_a_child)
            return;
        // The child leads its process group. Kill that group so descendants that
        // stay in it also stop after a timeout or rejected capture.
        (void)::kill(-pid_, SIGKILL);
        if (direct_child_reaped_)
            return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            const reap_probe probe = probe_reap();
            if (probe == reap_probe::reaped || probe == reap_probe::no_longer_a_child) {
                return;
            }
            (void)::poll(nullptr, 0u, 5);
        }
        (void)probe_reap();
    }

    int read_fd_ = -1;
    pid_t pid_ = 0;
    bool direct_child_reaped_ = false;
    bool completed_ = false;
};

/**
 * @brief Runs a command and captures stdout and stderr in one bounded string.
 * Failure cleanup stops processes that remain in the child's process group.
 * Waits for both the child and the output pipe, applying the supplied limits.
 * @param[in] args Executable followed by its arguments.
 * @param[in] limits Output and capture-time limits.
 * @param[in] inherited_fds Extra descriptor mappings available to the child.
 * @return Combined output, or a command or capture error.
 */
[[nodiscard]] inline std::expected<std::string, std::string>
run_capture(std::span<const std::string> args,
            run_capture_limits limits = {},
            std::span<const run_capture_inherited_fd> inherited_fds = {}) {
    if (args.empty()) {
        return std::unexpected("empty command");
    }
    if (limits.maximum_output_bytes == 0u || limits.timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected("invalid command capture limits");
    }
    std::array<int, 2> pipe_fds{-1, -1};
    if (::pipe(pipe_fds.data()) != 0) {
        return std::unexpected(shader_cache_message("pipe failed: errno ", errno));
    }
    capture_file_descriptor read_owner{pipe_fds[0]};
    capture_file_descriptor write_owner{pipe_fds[1]};
    // Close the original pipe descriptor during exec. Spawn actions keep the
    // stdout and stderr duplicates open.
    const auto set_close_on_exec = [](int fd) noexcept -> int {
        const int flags = ::fcntl(fd, F_GETFD, 0);
        if (flags < 0)
            return errno;
        if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
            return errno;
        return 0;
    };
    if (const int cloexec_error = set_close_on_exec(read_owner.get()); cloexec_error != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not seal capture read fd: errno ", cloexec_error));
    }
    if (const int cloexec_error = set_close_on_exec(write_owner.get()); cloexec_error != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not seal capture write fd: errno ", cloexec_error));
    }

    capture_spawn_file_actions actions;
    if (const int action_error = actions.initialize(); action_error != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not initialize spawn actions: errno ", action_error));
    }
    const std::array action_errors{
        actions.add_dup2(write_owner.get(), STDOUT_FILENO),
        actions.add_dup2(write_owner.get(), STDERR_FILENO),
        actions.add_close(read_owner.get()),
        actions.add_close(write_owner.get()),
    };
    for (const int action_error : action_errors) {
        if (action_error != 0) {
            return std::unexpected(shader_cache_message(
                args[0], " could not configure spawn actions: errno ", action_error));
        }
    }
    for (std::size_t index = 0u; index < inherited_fds.size(); ++index) {
        const run_capture_inherited_fd inherited = inherited_fds[index];
        if (inherited.source_fd < 0 || inherited.target_fd < 3 ||
            inherited.source_fd == inherited.target_fd) {
            return std::unexpected("invalid inherited descriptor for command capture");
        }
        for (std::size_t prior = 0u; prior < index; ++prior) {
            if (inherited_fds[prior].target_fd == inherited.target_fd) {
                return std::unexpected("duplicate inherited descriptor target for command capture");
            }
        }
        if (const int action_error = actions.add_dup2(inherited.source_fd, inherited.target_fd);
            action_error != 0) {
            return std::unexpected(shader_cache_message(
                args[0], " could not inherit command descriptor: errno ", action_error));
        }
    }

    capture_spawn_attributes attributes;
    if (const int attribute_error = attributes.initialize_process_group(); attribute_error != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not isolate capture process group: errno ", attribute_error));
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc =
        ::posix_spawnp(&pid, argv[0], actions.get(), attributes.get(), argv.data(), environ);
    write_owner.close_now();
    if (rc != 0) {
        return std::unexpected(shader_cache_message(args[0], " spawn failed: errno ", rc));
    }
    captured_child_process child{read_owner.release(), pid};
    const int original_flags = ::fcntl(child.read_fd(), F_GETFL, 0);
    if (original_flags < 0 || ::fcntl(child.read_fd(), F_SETFL, original_flags | O_NONBLOCK) != 0) {
        return std::unexpected(shader_cache_message(
            args[0], " could not make capture pipe nonblocking: errno ", errno));
    }

    std::string output;
    output.reserve(std::min<std::size_t>(limits.maximum_output_bytes, 4096u));
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + limits.timeout;
    bool pipe_eof = false;
    bool child_exited = false;
    int status = 0;
    while (!pipe_eof || !child_exited) {
        if (!pipe_eof) {
            for (;;) {
                const ssize_t bytes_read = ::read(child.read_fd(), buffer.data(), buffer.size());
                if (bytes_read > 0) {
                    const std::size_t bytes = static_cast<std::size_t>(bytes_read);
                    if (bytes > limits.maximum_output_bytes - output.size()) {
                        return std::unexpected(
                            shader_cache_message(args[0],
                                                 " exceeded capture output cap of ",
                                                 limits.maximum_output_bytes,
                                                 " bytes"));
                    }
                    output.append(buffer.data(), bytes);
                    continue;
                }
                if (bytes_read == 0) {
                    pipe_eof = true;
                    child.close_read();
                    break;
                }
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return std::unexpected(
                    shader_cache_message(args[0], " read failed: errno ", errno));
            }
        }

        if (!child_exited) {
            for (;;) {
                const pid_t waited = ::waitpid(pid, &status, WNOHANG);
                if (waited == pid) {
                    child_exited = true;
                    child.mark_reaped();
                    break;
                }
                if (waited == 0)
                    break;
                if (waited < 0 && errno == EINTR)
                    continue;
                return std::unexpected(
                    shader_cache_message(args[0], " waitpid failed: errno ", errno));
            }
        }
        if (pipe_eof && child_exited)
            break;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::unexpected(
                shader_cache_message(args[0], " timed out after ", limits.timeout.count(), " ms"));
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int poll_timeout =
            static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, 100));
        pollfd descriptor{
            .fd = pipe_eof ? -1 : child.read_fd(),
            .events = POLLIN | POLLHUP | POLLERR,
            .revents = 0,
        };
        const int poll_result = ::poll(&descriptor, 1u, poll_timeout);
        if (poll_result < 0 && errno != EINTR) {
            return std::unexpected(shader_cache_message(args[0], " poll failed: errno ", errno));
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(shader_cache_message(args[0],
                                                    " exited with status ",
                                                    WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                                                    ": ",
                                                    output));
    }
    child.mark_completed();
    return std::expected<std::string, std::string>{std::in_place, std::move(output)};
}

/// @brief Finds `xcrun` for Metal compilation.
/// Uses `/usr/bin/xcrun` when executable, otherwise searches PATH at spawn time.
[[nodiscard]] inline std::string xcrun_executable() {
    if (::access("/usr/bin/xcrun", X_OK) == 0) {
        return "/usr/bin/xcrun";
    }
    return "xcrun";
}

/// @brief Returns the Metal compiler version for the cache build ID.
/// Returns an unavailable marker if the command fails.
[[nodiscard]] inline std::string probe_metal_version() {
    const std::array<std::string, 4> args{xcrun_executable(), "-sdk", "macosx", "metal"};
    std::vector<std::string> version_args(args.begin(), args.end());
    version_args.push_back("--version");
    auto result = run_capture(version_args);
    return result ? *result : std::string{"xcrun metal unavailable"};
}

/// @brief Flushes the parent directory after a cache file is renamed into place.
/// Returns a filesystem error if the directory cannot be opened or synced.
[[nodiscard]] inline std::expected<void, std::string>
fsync_parent_dir(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    const int fd = ::open(parent.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected(
            shader_cache_message("open dir ", parent.string(), " failed: errno ", errno));
    }
    const int rc = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
    if (rc != 0) {
        return std::unexpected(
            shader_cache_message("fsync dir ", parent.string(), " failed: errno ", saved));
    }
    return {};
}

/// @brief Writes all bytes to `fd`, retrying interrupted writes.
/// Returns an error if a write fails or makes no progress.
[[nodiscard]] inline std::expected<void, std::string>
write_all_fd(int fd, std::span<const std::byte> bytes) {
    const std::byte* cursor = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        const ssize_t bytes_written = ::write(fd, cursor, remaining);
        if (bytes_written > 0) {
            cursor += bytes_written;
            remaining -= static_cast<std::size_t>(bytes_written);
            continue;
        }
        if (bytes_written < 0 && errno == EINTR)
            continue;
        return std::unexpected(shader_cache_message("write failed: errno ", errno));
    }
    return {};
}

/// @brief Writes and syncs a temporary file, then renames it to `final_path`.
/// Syncs the parent directory after the rename. The temporary name is fixed,
/// so callers must serialize writes to the same destination.
[[nodiscard]] inline std::expected<void, std::string>
durable_write_file(const std::filesystem::path& final_path, std::span<const std::byte> bytes) {
    const std::filesystem::path temporary_path = final_path.string() + ".tmp";
    const int fd = ::open(temporary_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        return std::unexpected(
            shader_cache_message("open ", temporary_path.string(), " failed: errno ", errno));
    }
    auto write_result = write_all_fd(fd, bytes);
    if (!write_result) {
        ::close(fd);
        return write_result;
    }
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        return std::unexpected(
            shader_cache_message("fsync ", temporary_path.string(), " failed: errno ", saved));
    }
    if (::close(fd) != 0) {
        return std::unexpected(
            shader_cache_message("close ", temporary_path.string(), " failed: errno ", errno));
    }
    if (std::rename(temporary_path.c_str(), final_path.c_str()) != 0) {
        return std::unexpected(shader_cache_message("rename ",
                                                    temporary_path.string(),
                                                    " -> ",
                                                    final_path.string(),
                                                    " failed: errno ",
                                                    errno));
    }
    return fsync_parent_dir(final_path);
}

/// @brief Syncs a completed temporary file and renames it to `final_path`.
/// Flushes the destination directory after the rename and reports open, sync, or rename errors.
[[nodiscard]] inline std::expected<void, std::string>
durable_rename_existing(const std::filesystem::path& temporary_path,
                        const std::filesystem::path& final_path) {
    const int fd = ::open(temporary_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected(
            shader_cache_message("open ", temporary_path.string(), " failed: errno ", errno));
    }
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        return std::unexpected(
            shader_cache_message("fsync ", temporary_path.string(), " failed: errno ", saved));
    }
    ::close(fd);
    if (std::rename(temporary_path.c_str(), final_path.c_str()) != 0) {
        return std::unexpected(shader_cache_message("rename ",
                                                    temporary_path.string(),
                                                    " -> ",
                                                    final_path.string(),
                                                    " failed: errno ",
                                                    errno));
    }
    return fsync_parent_dir(final_path);
}

/// @brief Runs a command with capture limits and discards successful output.
/// Returns the capture error if the command fails.
[[nodiscard]] inline std::expected<void, std::string>
spawn_wait(std::span<const std::string> args) {
    auto result = run_capture(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

} // namespace bioseq::gpu::vulkan
