/**
 * @file subprocess.cppm
 * @brief Owns bounded subprocess execution and durable shader-cache publication.
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

/// @brief Captures operating-system identity used by the cache digest.
/// @return Stable uname component or an unavailable marker.
[[nodiscard]] inline std::string system_uname_digest_component() {
    utsname info{};
    if (::uname(&info) != 0) {
        return "uname-unavailable";
    }
    return shader_cache_message(
        info.sysname, ' ', info.nodename, ' ', info.release, ' ', info.version, ' ', info.machine);
}

/// @brief Resource bounds for one captured subprocess invocation.
struct run_capture_limits {
    std::size_t maximum_output_bytes = 4u << 20u;
    std::chrono::milliseconds timeout = std::chrono::minutes(5);
};

/// @brief Descriptor mapping inherited by a captured subprocess.
struct run_capture_inherited_fd {
    int source_fd = -1;
    int target_fd = -1;
};

/// @brief Move-disabled owner for one subprocess file descriptor.
class capture_file_descriptor {
  public:
    /// @brief Adopts one subprocess-related descriptor.
    /// @param[in] fd Descriptor to close unless transferred.
    explicit capture_file_descriptor(int fd = -1) noexcept : fd_(fd) {}

    /// @brief Prevents copying descriptor ownership.
    capture_file_descriptor(const capture_file_descriptor&) = delete;

    /// @brief Prevents copy assignment of descriptor ownership.
    capture_file_descriptor& operator=(const capture_file_descriptor&) = delete;

    /// @brief Closes the owned descriptor.
    ~capture_file_descriptor() noexcept {
        close_now();
    }

    /// @brief Returns the owned descriptor without transferring it.
    /// @return Borrowed file descriptor.
    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    /// @brief Transfers the descriptor out of this owner.
    /// @return Owned file descriptor.
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

/// @brief Owns initialized POSIX spawn file actions.
class capture_spawn_file_actions {
  public:
    /// @brief Creates an uninitialized spawn-actions owner.
    capture_spawn_file_actions() = default;

    /// @brief Prevents copying POSIX spawn actions.
    capture_spawn_file_actions(const capture_spawn_file_actions&) = delete;

    /// @brief Prevents copy assignment of POSIX spawn actions.
    capture_spawn_file_actions& operator=(const capture_spawn_file_actions&) = delete;

    /// @brief Destroys initialized POSIX spawn actions.
    ~capture_spawn_file_actions() noexcept {
        if (initialized_)
            (void)::posix_spawn_file_actions_destroy(&actions_);
    }

    /// @brief Initializes the POSIX spawn action list.
    /// @return POSIX status code.
    [[nodiscard]] int initialize() noexcept {
        const int rc = ::posix_spawn_file_actions_init(&actions_);
        initialized_ = rc == 0;
        return rc;
    }

    /// @brief Adds one descriptor duplication action.
    /// @param[in] fd Source descriptor.
    /// @param[in] target Child descriptor number.
    /// @return POSIX status code.
    [[nodiscard]] int add_dup2(int fd, int target) noexcept {
        return ::posix_spawn_file_actions_adddup2(&actions_, fd, target);
    }

    /// @brief Adds one child-side descriptor close action.
    /// @param[in] fd Descriptor to close in the child.
    /// @return POSIX status code.
    [[nodiscard]] int add_close(int fd) noexcept {
        return ::posix_spawn_file_actions_addclose(&actions_, fd);
    }

    /// @brief Returns the initialized POSIX spawn action list.
    /// @return Borrowed action-list pointer.
    [[nodiscard]] const posix_spawn_file_actions_t* get() const noexcept {
        return &actions_;
    }

  private:
    posix_spawn_file_actions_t actions_{};
    bool initialized_ = false;
};

/// @brief Owns spawn attributes that isolate the child process group.
class capture_spawn_attributes {
  public:
    /// @brief Creates an uninitialized spawn-attribute owner.
    capture_spawn_attributes() = default;

    /// @brief Prevents copying POSIX spawn attributes.
    capture_spawn_attributes(const capture_spawn_attributes&) = delete;

    /// @brief Prevents copy assignment of POSIX spawn attributes.
    capture_spawn_attributes& operator=(const capture_spawn_attributes&) = delete;

    /// @brief Destroys initialized POSIX spawn attributes.
    ~capture_spawn_attributes() noexcept {
        if (initialized_)
            (void)::posix_spawnattr_destroy(&attributes_);
    }

    /// @brief Initializes attributes that place the child in its own process group.
    /// @return POSIX status code.
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

    /// @brief Returns the initialized POSIX spawn attributes.
    /// @return Borrowed attribute pointer.
    [[nodiscard]] const posix_spawnattr_t* get() const noexcept {
        return &attributes_;
    }

  private:
    posix_spawnattr_t attributes_{};
    bool initialized_ = false;
};

/// @brief Reaps a captured subprocess and terminates its group on early exit.
class captured_child_process {
  public:
    /// @brief Adopts the child PID and captured-output descriptor.
    /// @param[in] read_fd Captured-output read descriptor.
    /// @param[in] pid Direct child process identifier and process-group identifier.
    captured_child_process(int read_fd, pid_t pid) noexcept : read_fd_(read_fd), pid_(pid) {}

    /// @brief Prevents copying child-process ownership.
    captured_child_process(const captured_child_process&) = delete;

    /// @brief Prevents copy assignment of child-process ownership.
    captured_child_process& operator=(const captured_child_process&) = delete;

    /// @brief Closes capture output and terminates unfinished child groups.
    ~captured_child_process() noexcept {
        close_read();
        if (!completed_)
            terminate_group_and_reap();
    }

    /// @brief Returns the captured-output descriptor.
    /// @return Borrowed read descriptor.
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

    /// @brief Records that the direct child has been reaped.
    void mark_reaped() noexcept {
        direct_child_reaped_ = true;
    }

    /// @brief Records successful capture completion so destruction need not terminate the group.
    void mark_completed() noexcept {
        completed_ = true;
    }

  private:
    /// @brief Result of a nonblocking direct-child reap attempt.
    enum class reap_probe : std::uint8_t {
        running,
        reaped,
        no_longer_a_child,
        error,
    };

    /// @brief Attempts a nonblocking direct-child reap.
    /// @return Current direct-child state.
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

    /// @brief Kills the child process group and performs a bounded direct-child reap.
    void terminate_group_and_reap() noexcept {
        if (pid_ <= 0)
            return;
        const reap_probe before = probe_reap();
        if (before == reap_probe::no_longer_a_child)
            return;
        // POSIX_SPAWN_SETPGROUP made the child PID the process-group ID, so
        // ordinary descendants cannot outlive a timeout or continue touching
        // helper outputs after capture rejection.
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
 * @brief Runs a command in an isolated process group and captures bounded output.
 * @param[in] args Executable and argument vector.
 * @param[in] limits Output and elapsed-time limits.
 * @param[in] inherited_fds Additional descriptor mappings for the child.
 * @return Captured combined output or a spawn, timeout, I/O, or exit diagnostic.
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

/// @brief Resolves the platform tool dispatcher used for Metal compilation.
/// @return Absolute system path when available, otherwise the PATH lookup name.
[[nodiscard]] inline std::string xcrun_executable() {
    if (::access("/usr/bin/xcrun", X_OK) == 0) {
        return "/usr/bin/xcrun";
    }
    return "xcrun";
}

/// @brief Captures the installed Metal compiler version for cache identity.
/// @return Compiler version output or an unavailable marker.
[[nodiscard]] inline std::string probe_metal_version() {
    const std::array<std::string, 4> args{xcrun_executable(), "-sdk", "macosx", "metal"};
    std::vector<std::string> version_args(args.begin(), args.end());
    version_args.push_back("--version");
    auto result = run_capture(version_args);
    return result ? *result : std::string{"xcrun metal unavailable"};
}

/// @brief Synchronizes the directory entry containing a published file.
/// @param[in] path Published file path.
/// @return Success or a filesystem diagnostic.
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

/// @brief Writes every byte to an open descriptor while retrying interruptions.
/// @param[in] fd Open output descriptor.
/// @param[in] bytes Bytes to write.
/// @return Success or an I/O diagnostic.
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

/// @brief Atomically publishes exact bytes after syncing file and parent directory.
/// @param[in] final_path Published destination path.
/// @param[in] bytes File payload.
/// @return Success or an I/O diagnostic.
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

/// @brief Syncs and atomically publishes an existing temporary file.
/// @param[in] temporary_path Complete temporary file.
/// @param[in] final_path Published destination path.
/// @return Success or an I/O diagnostic.
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

/// @brief Runs a command and discards successful captured output.
/// @param[in] args Executable and argument vector.
/// @return Success or the captured subprocess diagnostic.
[[nodiscard]] inline std::expected<void, std::string>
spawn_wait(std::span<const std::string> args) {
    auto result = run_capture(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

} // namespace bioseq::gpu::vulkan
