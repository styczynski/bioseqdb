module;

#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

export module bioseq.test.postgres:process;

import :supervision;

namespace bioseq::postgres_test::process_detail {

#if !defined(BIOSEQ_POSTGRES_TEST_LAUNCH_BROKER_PATH)
#error "BIOSEQ_POSTGRES_TEST_LAUNCH_BROKER_PATH must name the launch broker"
#endif

[[nodiscard]] inline std::filesystem::path launch_broker_executable() {
    return std::filesystem::path{
        BIOSEQ_POSTGRES_TEST_LAUNCH_BROKER_PATH};
}

inline void close_descriptor(int& descriptor) noexcept {
    if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
        descriptor = -1;
    }
}

/**
 * Use one lock for local process launches and pipe setup.
 * The pipe-plus-fcntl fallback needs this lock so another local spawn cannot
 * inherit an endpoint before close-on-exec is set.
 */
[[nodiscard]] inline std::mutex& spawn_serialization_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

/// Create a pipe with close-on-exec set on both endpoints.
[[nodiscard]] inline bool create_cloexec_pipe(
    int (&descriptors)[2]) noexcept {
#if defined(__linux__) && defined(O_CLOEXEC)
    if (::pipe2(descriptors, O_CLOEXEC) == 0) {
        return true;
    }
    if (errno != ENOSYS) {
        return false;
    }
#endif
    if (::pipe(descriptors) != 0) {
        return false;
    }
    for (int descriptor : descriptors) {
        const int flags = ::fcntl(descriptor, F_GETFD);
        if (flags < 0
            || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
            const int failure = errno == 0 ? EIO : errno;
            static_cast<void>(::close(descriptors[0]));
            static_cast<void>(::close(descriptors[1]));
            descriptors[0] = -1;
            descriptors[1] = -1;
            errno = failure;
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool normalize_cloexec_pipe_descriptor(
    int& descriptor) noexcept {
    if (descriptor > STDERR_FILENO) {
        return true;
    }
#if defined(F_DUPFD_CLOEXEC)
    const int replacement =
        ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
    const int replacement =
        ::fcntl(descriptor, F_DUPFD, STDERR_FILENO + 1);
#endif
    if (replacement < 0) {
        return false;
    }
#if !defined(F_DUPFD_CLOEXEC)
    const int flags = ::fcntl(replacement, F_GETFD);
    if (flags < 0
        || ::fcntl(replacement, F_SETFD, flags | FD_CLOEXEC) != 0) {
        const int failure = errno == 0 ? EIO : errno;
        static_cast<void>(::close(replacement));
        errno = failure;
        return false;
    }
#endif
    static_cast<void>(::close(descriptor));
    descriptor = replacement;
    return true;
}

[[nodiscard]] inline int duplicate_cloexec_descriptor(
    int descriptor) noexcept {
#if defined(F_DUPFD_CLOEXEC)
    return ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
    const int duplicate =
        ::fcntl(descriptor, F_DUPFD, STDERR_FILENO + 1);
    if (duplicate < 0) {
        return -1;
    }
    const int flags = ::fcntl(duplicate, F_GETFD);
    if (flags < 0
        || ::fcntl(duplicate, F_SETFD, flags | FD_CLOEXEC) != 0) {
        const int failure = errno == 0 ? EIO : errno;
        static_cast<void>(::close(duplicate));
        errno = failure;
        return -1;
    }
    return duplicate;
#endif
}

/**
 * Release the launch broker with one byte while blocking SIGPIPE.
 * Consume a SIGPIPE caused by this write before restoring the thread's
 * previous signal mask. Preserve a signal that was already pending.
 */
[[nodiscard]] inline bool release_launch_broker(int descriptor) noexcept {
    if (descriptor < 0) {
        return false;
    }
    sigset_t blocked;
    sigset_t previous;
    sigset_t pending;
    if (sigemptyset(&blocked) != 0
        || sigaddset(&blocked, SIGPIPE) != 0) {
        return false;
    }
    if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
        return false;
    }
    if (::sigpending(&pending) != 0) {
        static_cast<void>(
            ::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
        return false;
    }
    const int was_pending = sigismember(&pending, SIGPIPE);
    if (was_pending < 0) {
        static_cast<void>(
            ::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
        return false;
    }

    constexpr char k_release = 'G';
    ssize_t written = -1;
    do {
        written = ::write(descriptor, &k_release, sizeof(k_release));
    } while (written < 0 && errno == EINTR);
    const int write_error = errno;
    if (written < 0 && write_error == EPIPE && was_pending == 0) {
        sigset_t generated;
        if (::sigpending(&generated) == 0
            && sigismember(&generated, SIGPIPE) == 1) {
            int delivered = 0;
            static_cast<void>(::sigwait(&blocked, &delivered));
        }
    }
    const bool restored =
        ::pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0;
    return written == 1 && restored;
}

}  // namespace bioseq::postgres_test::process_detail

export namespace bioseq::postgres_test {

/// Failure while launching or managing a test subprocess.
class process_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Deadline failure reported after attempting to stop the owned process group.
class process_timeout : public process_error {
public:
    using process_error::process_error;
};

/// Executable, arguments and settings for a process launched without a shell.
struct process_launch_options {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> environment;
    std::optional<std::filesystem::path> preferred_path_directory;
    std::optional<std::filesystem::path> output_file;
    bool supervise{};
    std::optional<std::filesystem::path> supervision_data_directory;
};

/// @brief Decoded child exit status.
struct process_exit_status {
    std::optional<int> exit_code;
    std::optional<int> signal;

    /// @brief True only for an ordinary zero exit.
    [[nodiscard]] bool is_successful() const noexcept {
        return exit_code.has_value() && *exit_code == 0;
    }
};

/**
 * Copy selected environment variables for test subprocesses, then apply the
 * caller's overrides. Omit inherited PostgreSQL settings and reject PG-prefixed
 * override names. Optionally put a validated directory first in PATH.
 */
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
build_sanitized_process_environment(
    std::span<const std::pair<std::string, std::string>> overrides = {},
    const std::optional<std::filesystem::path>& preferred_path_directory =
        std::nullopt) {
    const auto allowed_exact = [](std::string_view name) noexcept {
        return name == "PATH" || name == "HOME" || name == "LANG"
            || name == "LANGUAGE" || name == "TZ" || name == "TERM"
            || name == "TMPDIR" || name == "USER" || name == "LOGNAME"
            || name == "LD_LIBRARY_PATH" || name == "LD_PRELOAD"
            || name == "DYLD_LIBRARY_PATH" || name == "ASAN_OPTIONS"
            || name == "UBSAN_OPTIONS" || name == "TSAN_OPTIONS"
            || name == "MSAN_OPTIONS" || name == "ASAN_SYMBOLIZER_PATH"
            || name == "BIOSEQ_TEST_BUILD_PRESET"
            || name == "BIOSEQ_TEST_BUILD_PRESET_DEBUG"
            || name == "BIOSEQ_SANITIZER_RUNTIME";
    };
    std::map<std::string, std::string> values;
    if (environ != nullptr) {
        for (char** entry = environ; *entry != nullptr; ++entry) {
            const std::string_view item{*entry};
            const std::size_t separator = item.find('=');
            if (separator == std::string_view::npos) {
                continue;
            }
            const std::string_view name = item.substr(0, separator);
            if (!allowed_exact(name) && !name.starts_with("LC_")) {
                continue;
            }
            values.insert_or_assign(
                std::string(name), std::string(item.substr(separator + 1)));
        }
    }
    for (const auto& [name, value] : overrides) {
        if (name.empty() || name.find('=') != std::string::npos
            || name.find('\0') != std::string::npos
            || value.find('\0') != std::string::npos
            || name.starts_with("PG")) {
            throw process_error("invalid child environment override");
        }
        values.insert_or_assign(name, value);
    }
    if (preferred_path_directory) {
        std::error_code path_error;
        const std::filesystem::path preferred =
            std::filesystem::canonical(
                *preferred_path_directory, path_error);
        if (path_error
            || !std::filesystem::is_directory(preferred, path_error)
            || path_error) {
            throw process_error(
                "preferred child PATH directory is unavailable: "
                + preferred_path_directory->string());
        }
        const std::string prefix = preferred.string();
        if (prefix.empty() || prefix.find('\0') != std::string::npos
            || prefix.find(':') != std::string::npos) {
            throw process_error(
                "preferred child PATH directory is not representable");
        }
        auto path = values.find("PATH");
        if (path == values.end() || path->second.empty()) {
            values.insert_or_assign("PATH", prefix);
        } else if (path->second != prefix
                   && !path->second.starts_with(prefix + ":")) {
            path->second = prefix + ":" + path->second;
        }
    }
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(values.size());
    for (auto& [name, value] : values) {
        result.emplace_back(std::move(name), std::move(value));
    }
    return result;
}

/**
 * Own a child process, its process group and its capture-file path.
 * Ownership can be moved. Destruction attempts to stop the group and remove
 * a log created for this object. It keeps logs at caller-supplied paths.
 */
class owned_process {
public:
    /**
     * Launch an absolute executable in a new process group without a shell.
     * Redirect stdin to /dev/null and stdout and stderr to one file. Supervised
     * launches wait in a broker until the parent has recorded them for cleanup.
     */
    [[nodiscard]] static owned_process spawn(const process_launch_options& options) {
        std::scoped_lock spawn_lock{
            process_detail::spawn_serialization_mutex()};
        if (options.executable.empty() || !options.executable.is_absolute()) {
            throw process_error("process executable must be an absolute path");
        }
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(options.executable, file_error)
            || file_error) {
            throw process_error("process executable is not a regular file: "
                                + options.executable.string());
        }

        bool remove_log = false;
        std::filesystem::path output =
            options.output_file.value_or(make_temporary_log());
        if (!options.output_file) {
            remove_log = true;
        } else {
            output = std::filesystem::absolute(output);
            if (!output.parent_path().empty()) {
                std::filesystem::create_directories(output.parent_path());
            }
        }

        std::vector<std::string> argument_storage;
        argument_storage.reserve(options.arguments.size() + 1);
        argument_storage.push_back(options.executable.string());
        argument_storage.insert(argument_storage.end(),
                                options.arguments.begin(),
                                options.arguments.end());
        std::vector<char*> arguments;
        arguments.reserve(argument_storage.size() + 1);
        for (std::string& argument : argument_storage) {
            arguments.push_back(argument.data());
        }
        arguments.push_back(nullptr);

        const auto environment_pairs =
            build_sanitized_process_environment(
                options.environment, options.preferred_path_directory);
        std::vector<std::string> environment_storage;
        environment_storage.reserve(environment_pairs.size());
        for (const auto& [name, value] : environment_pairs) {
            environment_storage.push_back(name + "=" + value);
        }
        std::vector<char*> environment;
        environment.reserve(environment_storage.size() + 1);
        for (std::string& variable : environment_storage) {
            environment.push_back(variable.data());
        }
        environment.push_back(nullptr);

        std::filesystem::path spawn_executable = options.executable;
        std::vector<std::string> broker_argument_storage;
        std::vector<char*> broker_arguments;
        int control_read = -1;
        int control_write = -1;
        int broker_control = -1;
        const auto close_control_descriptors =
            [&control_read, &control_write, &broker_control]() noexcept {
                process_detail::close_descriptor(control_read);
                process_detail::close_descriptor(control_write);
                process_detail::close_descriptor(broker_control);
            };
        if (options.supervise) {
            spawn_executable =
                process_detail::launch_broker_executable();
            file_error.clear();
            if (!spawn_executable.is_absolute()
                || !std::filesystem::is_regular_file(
                    spawn_executable, file_error)
                || file_error) {
                if (remove_log) {
                    std::filesystem::remove(output, file_error);
                }
                throw process_error(
                    "supervised launch broker is unavailable: "
                    + spawn_executable.string());
            }
        }

        posix_spawn_file_actions_t actions{};
        posix_spawnattr_t attributes{};
        bool actions_ready = false;
        bool attributes_ready = false;
        const auto destroy = [&actions, &attributes, &actions_ready,
                              &attributes_ready]() noexcept {
            if (actions_ready) {
                posix_spawn_file_actions_destroy(&actions);
            }
            if (attributes_ready) {
                posix_spawnattr_destroy(&attributes);
            }
        };
        if (posix_spawn_file_actions_init(&actions) != 0) {
            throw process_error("posix_spawn file-action initialization failed");
        }
        actions_ready = true;
        if (posix_spawnattr_init(&attributes) != 0) {
            destroy();
            throw process_error("posix_spawn attribute initialization failed");
        }
        attributes_ready = true;

        int status = posix_spawn_file_actions_addopen(
            &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (status == 0) {
            status = posix_spawn_file_actions_addopen(
                &actions, STDOUT_FILENO, output.c_str(),
                O_WRONLY | O_CREAT | O_TRUNC, 0600);
        }
        if (status == 0) {
            status = posix_spawn_file_actions_adddup2(
                &actions, STDOUT_FILENO, STDERR_FILENO);
        }
        short flags = POSIX_SPAWN_SETPGROUP;
        if (status == 0) {
            status = posix_spawnattr_setflags(&attributes, flags);
        }
        if (status == 0) {
            status = posix_spawnattr_setpgroup(&attributes, 0);
        }
        std::optional<supervision_launch_intent> launch_intent;
        if (status == 0 && options.supervise) {
            launch_intent = begin_supervised_launch();
            if (!launch_intent) {
                destroy();
                if (remove_log) {
                    std::filesystem::remove(output, file_error);
                }
                throw process_error(
                    "cannot durably journal supervised launch intent");
            }
            try {
                int descriptors[2]{-1, -1};
                if (process_detail::create_cloexec_pipe(descriptors)) {
                    control_read = descriptors[0];
                    control_write = descriptors[1];
                    if (!process_detail::normalize_cloexec_pipe_descriptor(
                            control_read)
                        || !process_detail::normalize_cloexec_pipe_descriptor(
                            control_write)) {
                        status = errno == 0 ? EMFILE : errno;
                    }
                    if (status == 0) {
                        broker_control =
                            process_detail::duplicate_cloexec_descriptor(
                                control_read);
                        if (broker_control < 0) {
                            status = errno == 0 ? EMFILE : errno;
                        }
                    }
                } else {
                    status = errno == 0 ? EMFILE : errno;
                }
                if (status == 0) {
                    status = posix_spawn_file_actions_adddup2(
                        &actions, control_read, broker_control);
                }
                if (status == 0) {
                    status = posix_spawn_file_actions_addclose(
                        &actions, control_read);
                }
                if (status == 0) {
                    status = posix_spawn_file_actions_addclose(
                        &actions, control_write);
                }
                if (status == 0) {
                    broker_argument_storage.reserve(
                        argument_storage.size() + 3U);
                    broker_argument_storage.push_back(
                        spawn_executable.filename().string());
                    broker_argument_storage.push_back(
                        std::to_string(broker_control));
                    broker_argument_storage.push_back(
                        options.executable.string());
                    broker_argument_storage.insert(
                        broker_argument_storage.end(),
                        argument_storage.begin(),
                        argument_storage.end());
                    broker_arguments.reserve(
                        broker_argument_storage.size() + 1U);
                    for (std::string& argument :
                         broker_argument_storage) {
                        broker_arguments.push_back(argument.data());
                    }
                    broker_arguments.push_back(nullptr);
                }
            } catch (...) {
                close_control_descriptors();
                static_cast<void>(
                    cancel_supervised_launch(*launch_intent));
                destroy();
                if (remove_log) {
                    std::filesystem::remove(output, file_error);
                }
                throw;
            }
        }
        pid_t child = -1;
        if (status == 0) {
            status = ::posix_spawn(
                &child, spawn_executable.c_str(), &actions, &attributes,
                options.supervise ? broker_arguments.data()
                                  : arguments.data(),
                environment.data());
        }
        process_detail::close_descriptor(control_read);
        process_detail::close_descriptor(broker_control);
        destroy();
        if (status != 0) {
            process_detail::close_descriptor(control_write);
            if (launch_intent) {
                static_cast<void>(
                    cancel_supervised_launch(*launch_intent));
            }
            if (remove_log) {
                std::filesystem::remove(output, file_error);
            }
            throw process_error("posix_spawn failed for "
                                + spawn_executable.string() + ": "
                                + std::strerror(status));
        }

        const auto stop_unreleased_broker =
            [&control_write, child]() noexcept {
                process_detail::close_descriptor(control_write);
                bool child_reaped = false;
                bool group_absent = false;
                int child_status{};
                const auto graceful_deadline =
                    std::chrono::steady_clock::now()
                    + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now()
                       < graceful_deadline) {
                    if (!child_reaped) {
                        const pid_t waited =
                            ::waitpid(child, &child_status, WNOHANG);
                        if (waited == child
                            || (waited < 0 && errno == ECHILD)) {
                            child_reaped = true;
                        }
                    }
                    if (!group_absent
                        && ::kill(-child, 0) != 0 && errno == ESRCH) {
                        group_absent = true;
                    }
                    if (child_reaped && group_absent) {
                        return true;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                }
                static_cast<void>(::kill(-child, SIGKILL));
                static_cast<void>(::kill(child, SIGKILL));
                const auto force_deadline =
                    std::chrono::steady_clock::now()
                    + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now()
                       < force_deadline) {
                    if (!child_reaped) {
                        const pid_t waited =
                            ::waitpid(child, &child_status, WNOHANG);
                        if (waited == child
                            || (waited < 0 && errno == ECHILD)) {
                            child_reaped = true;
                        }
                    }
                    if (!group_absent
                        && ::kill(-child, 0) != 0 && errno == ESRCH) {
                        group_absent = true;
                    }
                    if (child_reaped && group_absent) {
                        return true;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                }
                return child_reaped && group_absent;
            };

        if (options.supervise
            && !commit_supervised_launch(
                *launch_intent,
                static_cast<std::uint64_t>(child),
                spawn_executable,
                options.supervision_data_directory.value_or(
                    std::filesystem::path{}))) {
            if (stop_unreleased_broker()) {
                static_cast<void>(
                    cancel_supervised_launch(
                        *launch_intent,
                        static_cast<std::uint64_t>(child)));
            }
            if (remove_log) {
                std::filesystem::remove(output, file_error);
            }
            throw process_error(
                "cannot durably register supervised child process");
        }
        if (options.supervise) {
            const bool released =
                process_detail::release_launch_broker(control_write);
            process_detail::close_descriptor(control_write);
            if (!released) {
                if (stop_unreleased_broker()) {
                    static_cast<void>(
                        cancel_supervised_launch(
                            *launch_intent,
                            static_cast<std::uint64_t>(child)));
                }
                if (remove_log) {
                    std::filesystem::remove(output, file_error);
                }
                throw process_error(
                    "cannot release durably registered launch broker");
            }
        }
        return owned_process(
            child, std::move(output), remove_log, options.supervise);
    }

    owned_process(const owned_process&) = delete;
    owned_process& operator=(const owned_process&) = delete;

    owned_process(owned_process&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)),
          process_group_(std::exchange(other.process_group_, -1)),
          supervision_pid_(std::exchange(other.supervision_pid_, -1)),
          output_file_(std::move(other.output_file_)),
          remove_output_(std::exchange(other.remove_output_, false)),
          supervised_(std::exchange(other.supervised_, false)),
          exit_(std::move(other.exit_)) {}

    owned_process& operator=(owned_process&& other) noexcept {
        if (this != &other) {
            terminate(std::chrono::seconds(2));
            remove_owned_log();
            pid_ = std::exchange(other.pid_, -1);
            process_group_ =
                std::exchange(other.process_group_, -1);
            supervision_pid_ =
                std::exchange(other.supervision_pid_, -1);
            output_file_ = std::move(other.output_file_);
            remove_output_ = std::exchange(other.remove_output_, false);
            supervised_ = std::exchange(other.supervised_, false);
            exit_ = std::move(other.exit_);
        }
        return *this;
    }

    ~owned_process() noexcept {
        terminate(std::chrono::seconds(2));
        remove_owned_log();
    }

    /// Wait for completion until the deadline, then attempt cleanup and throw process_timeout.
    [[nodiscard]] process_exit_status wait_until(
        std::chrono::steady_clock::time_point deadline) {
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto completed = try_wait()) {
                return *completed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        terminate(std::chrono::seconds(2));
        throw process_timeout("process deadline exceeded; captured output:\n"
                              + captured_output());
    }

    /// Wait for the given positive duration, attempting cleanup if it expires or is invalid.
    [[nodiscard]] process_exit_status wait_for(std::chrono::milliseconds budget) {
        if (budget <= std::chrono::milliseconds::zero()) {
            terminate(std::chrono::seconds(2));
            throw process_timeout("process deadline must be positive");
        }
        return wait_until(std::chrono::steady_clock::now() + budget);
    }

    /**
     * Poll without waiting for the child.
     * Return its exit status once the leader is reaped, its group is absent and
     * its supervision record has been removed.
     */
    [[nodiscard]] std::optional<process_exit_status> try_wait() noexcept {
        poll_leader();
        if (!exit_ || !is_owned_group_absent()) {
            return std::nullopt;
        }
        release_supervision();
        if (supervised_) {
            return std::nullopt;
        }
        return exit_;
    }

    /**
     * Try SIGTERM, wait for the grace period, then try SIGKILL.
     * Stop waiting after the final cleanup timeout even if a process remains.
     */
    void terminate(std::chrono::milliseconds grace) noexcept {
        if (try_wait()) {
            return;
        }
        signal_owned_group(SIGTERM);
        signal_unreaped_leader(SIGTERM);
        const auto deadline = std::chrono::steady_clock::now()
            + std::max(grace, std::chrono::milliseconds::zero());
        while (std::chrono::steady_clock::now() < deadline) {
            if (try_wait()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        signal_owned_group(SIGKILL);
        signal_unreaped_leader(SIGKILL);
        const auto force_deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < force_deadline) {
            if (try_wait()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        static_cast<void>(try_wait());
    }

    /// Read the combined stdout and stderr capture, or return empty if it cannot be opened.
    [[nodiscard]] std::string captured_output() const {
        std::ifstream input(output_file_, std::ios::binary);
        if (!input) {
            return {};
        }
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }

    /// Return the capture path. Caller-supplied files are kept after destruction.
    [[nodiscard]] const std::filesystem::path& output_file() const noexcept {
        return output_file_;
    }

    /// Return the child PID while unreaped, or -1 after reaping.
    [[nodiscard]] std::int64_t pid() const noexcept {
        return static_cast<std::int64_t>(pid_);
    }

    /// Return the recorded process-group ID, or -1 once it has been cleared.
    [[nodiscard]] std::int64_t process_group() const noexcept {
        return static_cast<std::int64_t>(process_group_);
    }

    /// Check for leader exit without reaping it, or return a previously recorded exit.
    [[nodiscard]] bool has_unreaped_exited_leader() const noexcept {
        if (pid_ <= 1) {
            return exit_.has_value();
        }
        siginfo_t information{};
        int status{};
        do {
            status = ::waitid(
                P_PID, static_cast<id_t>(pid_), &information,
                WEXITED | WNOHANG | WNOWAIT);
        } while (status != 0 && errno == EINTR);
        return status == 0
            && information.si_pid == pid_;
    }

    /**
     * Signal the process group while the direct child remains unreaped.
     * Keeping that child unreaped prevents reuse of its PID as another group
     * identifier.
     */
    [[nodiscard]] bool signal_owned_group_while_unreaped(
        int signal_number) noexcept {
        if (pid_ <= 1 || process_group_ <= 1
            || process_group_ == ::getpgrp()) {
            return false;
        }
        int status{};
        do {
            status = ::kill(-process_group_, signal_number);
        } while (status != 0 && errno == EINTR);
        return status == 0 || errno == ESRCH;
    }

    /**
     * Reap the leader after the caller has verified that its target process
     * tree has ended. Clear the saved process-group ID at the same time and
     * remove the supervision record. Until this call, keep the leader unreaped
     * so its PID cannot be reused.
     */
    [[nodiscard]] bool reap_after_verified_target_termination(
        std::chrono::milliseconds deadline) noexcept {
        const auto finish = std::chrono::steady_clock::now()
            + std::max(deadline, std::chrono::milliseconds::zero());
        for (;;) {
            if (pid_ <= 1) {
                process_group_ = -1;
                release_supervision();
                return !supervised_;
            }
            int status{};
            pid_t waited{};
            do {
                waited = ::waitpid(pid_, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited == pid_) {
                exit_ = decode_status(status);
                pid_ = -1;
                process_group_ = -1;
                release_supervision();
                return !supervised_;
            }
            if (waited < 0 && errno == ECHILD) {
                exit_ = process_exit_status{};
                pid_ = -1;
                process_group_ = -1;
                release_supervision();
                return !supervised_;
            }
            if (waited < 0
                || std::chrono::steady_clock::now() >= finish) {
                return false;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }
    }

private:
    owned_process(
        pid_t pid,
        std::filesystem::path output,
        bool remove_output,
        bool supervised)
        : pid_(pid),
          process_group_(pid),
          supervision_pid_(pid),
          output_file_(std::move(output)),
          remove_output_(remove_output),
          supervised_(supervised) {}

    void poll_leader() noexcept {
        if (pid_ <= 0 || exit_) {
            return;
        }
        int status = 0;
        const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
        if (waited == pid_) {
            exit_ = decode_status(status);
            pid_ = -1;
        } else if (waited < 0 && errno == ECHILD) {
            exit_ = process_exit_status{};
            pid_ = -1;
        }
    }

    [[nodiscard]] bool is_owned_group_absent() noexcept {
        if (process_group_ <= 1) {
            return true;
        }
        if (process_group_ == ::getpgrp()) {
            return false;
        }
        if (::kill(-process_group_, 0) == 0 || errno != ESRCH) {
            return false;
        }
        process_group_ = -1;
        return true;
    }

    void signal_owned_group(int signal) noexcept {
        if (process_group_ <= 1
            || process_group_ == ::getpgrp()) {
            return;
        }
        if (::kill(-process_group_, signal) != 0
            && errno == ESRCH) {
            process_group_ = -1;
        }
    }

    void signal_unreaped_leader(int signal) const noexcept {
        if (pid_ > 1) {
            static_cast<void>(::kill(pid_, signal));
        }
    }

    void release_supervision() noexcept {
        if (!supervised_ || supervision_pid_ <= 1) {
            return;
        }
        if (remove_supervised_process(
                static_cast<std::uint64_t>(supervision_pid_))) {
            supervised_ = false;
        }
    }

    /// @brief Creates and closes a unique private capture file.
    [[nodiscard]] static std::filesystem::path make_temporary_log() {
        std::string pattern =
            (std::filesystem::temp_directory_path()
             / "bioseq-pg-process-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const int descriptor = ::mkstemp(writable.data());
        if (descriptor < 0) {
            throw process_error("cannot create process capture file: "
                                + std::string(std::strerror(errno)));
        }
        static_cast<void>(::close(descriptor));
        return std::filesystem::path(writable.data());
    }

    /// @brief Decodes one waitpid status word.
    [[nodiscard]] static process_exit_status decode_status(int status) noexcept {
        process_exit_status result;
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.signal = WTERMSIG(status);
        }
        return result;
    }

    /// Attempt to remove an internally created capture file and discard filesystem errors.
    void remove_owned_log() noexcept {
        if (!remove_output_ || output_file_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove(output_file_, error);
        remove_output_ = false;
    }

    pid_t pid_{-1};
    pid_t process_group_{-1};
    pid_t supervision_pid_{-1};
    std::filesystem::path output_file_;
    bool remove_output_{};
    bool supervised_{};
    std::optional<process_exit_status> exit_;
};

}  // namespace bioseq::postgres_test
