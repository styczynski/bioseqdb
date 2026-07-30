module;

#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

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

export namespace bioseq::postgres_test {

/// @brief Failure raised by a lifecycle subprocess.
class process_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Deadline failure raised after the owned process group is terminated.
class process_timeout : public process_error {
public:
    using process_error::process_error;
};

/// @brief Explicit input to one shell-free posix_spawn invocation.
struct process_options {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> environment;
    std::optional<std::filesystem::path> output_file;
};

/// @brief Decoded child exit status.
struct process_exit {
    std::optional<int> exit_code;
    std::optional<int> signal;

    /// @brief True only for an ordinary zero exit.
    [[nodiscard]] bool succeeded() const noexcept {
        return exit_code.has_value() && *exit_code == 0;
    }
};

/// @brief Produces an allowlisted environment with all ambient PG routing removed.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
sanitized_process_environment(
    std::span<const std::pair<std::string, std::string>> overrides = {}) {
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
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(values.size());
    for (auto& [name, value] : values) {
        result.emplace_back(std::move(name), std::move(value));
    }
    return result;
}

/// @brief Move-only posix_spawn child with process-group and log ownership.
class spawned_process {
public:
    /// @brief Spawns an absolute executable in a new process group without a shell.
    [[nodiscard]] static spawned_process spawn(const process_options& options) {
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
        argument_storage.push_back(options.executable.filename().string());
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
            sanitized_process_environment(options.environment);
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
        pid_t child = -1;
        if (status == 0) {
            status = ::posix_spawn(
                &child, options.executable.c_str(), &actions, &attributes,
                arguments.data(), environment.data());
        }
        destroy();
        if (status != 0) {
            if (remove_log) {
                std::filesystem::remove(output, file_error);
            }
            throw process_error("posix_spawn failed for "
                                + options.executable.string() + ": "
                                + std::strerror(status));
        }
        return spawned_process(child, std::move(output), remove_log);
    }

    spawned_process(const spawned_process&) = delete;
    spawned_process& operator=(const spawned_process&) = delete;

    spawned_process(spawned_process&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)),
          output_file_(std::move(other.output_file_)),
          remove_output_(std::exchange(other.remove_output_, false)),
          exit_(std::move(other.exit_)) {}

    spawned_process& operator=(spawned_process&& other) noexcept {
        if (this != &other) {
            terminate(std::chrono::seconds(2));
            remove_owned_log();
            pid_ = std::exchange(other.pid_, -1);
            output_file_ = std::move(other.output_file_);
            remove_output_ = std::exchange(other.remove_output_, false);
            exit_ = std::move(other.exit_);
        }
        return *this;
    }

    ~spawned_process() noexcept {
        terminate(std::chrono::seconds(2));
        remove_owned_log();
    }

    /// @brief Waits until the absolute deadline or terminates and raises on timeout.
    [[nodiscard]] process_exit wait_until(
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

    /// @brief Waits for a positive relative budget.
    [[nodiscard]] process_exit wait_for(std::chrono::milliseconds budget) {
        if (budget <= std::chrono::milliseconds::zero()) {
            terminate(std::chrono::seconds(2));
            throw process_timeout("process deadline must be positive");
        }
        return wait_until(std::chrono::steady_clock::now() + budget);
    }

    /// @brief Polls and reaps the process once it exits.
    [[nodiscard]] std::optional<process_exit> try_wait() noexcept {
        if (exit_) {
            return exit_;
        }
        if (pid_ <= 0) {
            return std::nullopt;
        }
        int status = 0;
        const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
        if (waited == 0) {
            return std::nullopt;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                return std::nullopt;
            }
            if (errno == ECHILD) {
                pid_ = -1;
            }
            return std::nullopt;
        }
        exit_ = decode_status(status);
        pid_ = -1;
        return exit_;
    }

    /// @brief Sends SIGTERM to the child group, then SIGKILLs bounded survivors.
    void terminate(std::chrono::milliseconds grace) noexcept {
        if (pid_ <= 0 || exit_) {
            return;
        }
        const pid_t group = -pid_;
        if (::kill(group, SIGTERM) != 0 && errno != ESRCH) {
            static_cast<void>(::kill(pid_, SIGTERM));
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::max(grace, std::chrono::milliseconds::zero());
        while (std::chrono::steady_clock::now() < deadline) {
            if (try_wait()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (::kill(group, SIGKILL) != 0 && errno != ESRCH) {
            static_cast<void>(::kill(pid_, SIGKILL));
        }
        for (;;) {
            int status = 0;
            const pid_t waited = ::waitpid(pid_, &status, 0);
            if (waited == pid_) {
                exit_ = decode_status(status);
                pid_ = -1;
                return;
            }
            if (waited < 0 && errno == EINTR) {
                continue;
            }
            pid_ = -1;
            return;
        }
    }

    /// @brief Returns all captured combined stdout/stderr currently on disk.
    [[nodiscard]] std::string captured_output() const {
        std::ifstream input(output_file_, std::ios::binary);
        if (!input) {
            return {};
        }
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }

    /// @brief Returns the capture path, which persists when caller-supplied.
    [[nodiscard]] const std::filesystem::path& output_file() const noexcept {
        return output_file_;
    }

    /// @brief Returns the live child pid, or -1 after it has been reaped.
    [[nodiscard]] std::int64_t pid() const noexcept {
        return static_cast<std::int64_t>(pid_);
    }

private:
    spawned_process(pid_t pid, std::filesystem::path output, bool remove_output)
        : pid_(pid),
          output_file_(std::move(output)),
          remove_output_(remove_output) {}

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
    [[nodiscard]] static process_exit decode_status(int status) noexcept {
        process_exit result;
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.signal = WTERMSIG(status);
        }
        return result;
    }

    /// @brief Removes only a capture path allocated internally by this object.
    void remove_owned_log() noexcept {
        if (!remove_output_ || output_file_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove(output_file_, error);
        remove_output_ = false;
    }

    pid_t pid_{-1};
    std::filesystem::path output_file_;
    bool remove_output_{};
    std::optional<process_exit> exit_;
};

}  // namespace bioseq::postgres_test
