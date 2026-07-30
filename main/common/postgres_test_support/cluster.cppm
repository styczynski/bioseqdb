module;

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

export module bioseq.test.postgres:cluster;

import bioseq.libpq.client;
import :artifact;
import :process;

export namespace bioseq::postgres_test {

/// @brief Complete source-neutral configuration for one private PostgreSQL node.
struct cluster_options {
    std::filesystem::path postgres_bin_dir;
    std::vector<extension_artifact> extensions;
    std::vector<std::string> preload_libraries;
    std::vector<std::pair<std::string, std::string>> startup_gucs;
    std::chrono::seconds startup_timeout{60};
    std::chrono::seconds stop_timeout{30};
};

/// @brief Explicit lifecycle states for a private cluster.
enum class cluster_state {
    constructed,
    initialized,
    running,
    stopped,
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::cluster_detail {

/// @brief Validates options before any temporary directory is allocated.
[[nodiscard]] inline cluster_options validate_options(cluster_options options) {
    if (options.startup_timeout <= std::chrono::seconds::zero()
        || options.stop_timeout <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("cluster deadlines must be positive");
    }
    std::error_code error;
    const auto bin_status =
        std::filesystem::symlink_status(options.postgres_bin_dir, error);
    if (error || !std::filesystem::is_directory(bin_status)
        || std::filesystem::is_symlink(bin_status)) {
        throw std::invalid_argument(
            "PostgreSQL bin directory is not a non-symlink directory: "
            + options.postgres_bin_dir.string());
    }
    options.postgres_bin_dir =
        std::filesystem::canonical(options.postgres_bin_dir);
    for (const std::string_view executable : {"initdb", "pg_ctl", "postgres"}) {
        const std::filesystem::path candidate =
            options.postgres_bin_dir / executable;
        error.clear();
        if (!std::filesystem::is_regular_file(candidate, error) || error) {
            throw std::invalid_argument(
                "PostgreSQL lifecycle executable is missing: "
                + candidate.string());
        }
    }
    return options;
}

/// @brief Creates a unique short root suitable for Unix-domain socket paths.
[[nodiscard]] inline std::filesystem::path make_private_root() {
    std::string pattern =
        (std::filesystem::temp_directory_path()
         / "bioseq-pg-test-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) {
        throw std::runtime_error("cannot create private PostgreSQL root");
    }
    const std::filesystem::path root{created};
    std::ofstream marker(root / ".bioseq-private-cluster", std::ios::trunc);
    marker << root.filename().string() << '\n';
    marker.close();
    if (!marker) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        throw std::runtime_error("cannot create private cluster ownership marker");
    }
    return root;
}

/// @brief Obtains an ephemeral local port; the server itself listens on Unix only.
[[nodiscard]] inline int available_port() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        throw std::runtime_error("cannot create ephemeral port probe socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        static_cast<void>(::close(descriptor));
        throw std::runtime_error("cannot bind ephemeral port probe socket");
    }
    socklen_t size = sizeof(address);
    if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address),
                      &size) != 0) {
        static_cast<void>(::close(descriptor));
        throw std::runtime_error("cannot inspect ephemeral port probe socket");
    }
    static_cast<void>(::close(descriptor));
    return static_cast<int>(ntohs(address.sin_port));
}

/// @brief Quotes one postgresql.conf string literal.
[[nodiscard]] inline std::string configuration_literal(std::string_view value) {
    if (value.find('\n') != std::string_view::npos
        || value.find('\r') != std::string_view::npos) {
        throw std::invalid_argument("PostgreSQL configuration contains newline");
    }
    std::string escaped{"'"};
    for (const char character : value) {
        if (character == '\'') {
            escaped.push_back('\'');
        }
        escaped.push_back(character);
    }
    escaped.push_back('\'');
    return escaped;
}

/// @brief Restricts startup GUC names to PostgreSQL identifier components.
inline void validate_guc_name(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("startup GUC name is empty");
    }
    for (const char character : name) {
        const bool allowed =
            (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '_' || character == '.';
        if (!allowed) {
            throw std::invalid_argument("invalid startup GUC name: "
                                        + std::string(name));
        }
    }
}

/// @brief Validates a preload library's unqualified PostgreSQL name.
inline void validate_library_name(std::string_view name) {
    if (name.empty() || name.size() > 63) {
        throw std::invalid_argument("invalid preload library name");
    }
    for (const char character : name) {
        const bool allowed =
            (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '_';
        if (!allowed) {
            throw std::invalid_argument("invalid preload library name: "
                                        + std::string(name));
        }
    }
}

/// @brief Returns true when an owned pid is no longer live.
[[nodiscard]] inline bool process_absent(std::int64_t pid) noexcept {
    if (pid <= 1) {
        return true;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return false;
    }
    return errno == ESRCH;
}

}  // namespace bioseq::postgres_test::cluster_detail

export namespace bioseq::postgres_test {

/// @brief RAII owner of a staged, private PostgreSQL 18 cluster.
class private_cluster {
public:
    explicit private_cluster(cluster_options options)
        : options_(cluster_detail::validate_options(std::move(options))),
          root_(cluster_detail::make_private_root()),
          data_directory_(root_ / "data"),
          socket_directory_(root_ / "socket"),
          log_directory_(root_ / "log"),
          port_(cluster_detail::available_port()) {}

    ~private_cluster() noexcept {
        stop();
        if (state_ != cluster_state::running) {
            remove_owned_root();
        }
    }

    private_cluster(const private_cluster&) = delete;
    private_cluster& operator=(const private_cluster&) = delete;

    /// @brief Stages artifacts, initializes PGDATA, and writes private settings.
    void initialize() {
        require_state(cluster_state::constructed, "initialize");
        std::filesystem::create_directories(socket_directory_);
        std::filesystem::create_directories(log_directory_);
        stage_ = stage_extension_artifacts(root_ / "stage", options_.extensions);
        const auto init_exit = run_program(
            "initdb",
            {"--pgdata", data_directory_.string(),
             "--username", "postgres",
             "--auth-local=trust",
             "--auth-host=reject",
             "--encoding=UTF8",
             "--no-locale",
             "--no-sync"},
            options_.startup_timeout,
            log_directory_ / "initdb.log");
        if (!init_exit.succeeded()) {
            throw process_error("initdb failed:\n"
                                + read_file(log_directory_ / "initdb.log"));
        }
        write_configuration();
        state_ = cluster_state::initialized;
    }

    /// @brief Starts the initialized or previously stopped postmaster.
    void start() {
        if (state_ != cluster_state::initialized
            && state_ != cluster_state::stopped) {
            throw std::logic_error("start requires initialized or stopped state");
        }
        cleanup_error_.clear();
        state_ = cluster_state::running;
        try {
            const auto exit = run_program(
                "pg_ctl",
                {"--pgdata", data_directory_.string(),
                 "--log", (log_directory_ / "postgres.log").string(),
                 "--wait",
                 "--timeout", std::to_string(options_.startup_timeout.count()),
                 "start"},
                options_.startup_timeout + std::chrono::seconds(5),
                log_directory_ / "pg_ctl_start.log");
            if (!exit.succeeded()) {
                throw process_error("pg_ctl start failed:\n"
                                    + read_file(log_directory_
                                                / "pg_ctl_start.log"));
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    /// @brief Stops and starts the same PGDATA with a bounded lifecycle.
    void restart() {
        require_state(cluster_state::running, "restart");
        stop();
        if (state_ != cluster_state::stopped) {
            throw process_error("private cluster restart could not stop: "
                                + cleanup_error_);
        }
        start();
    }

    /// @brief Simulates an unclean postmaster death for crash-recovery tests.
    void crash_postmaster() {
        require_state(cluster_state::running, "crash_postmaster");
        const std::int64_t postmaster_pid = read_postmaster_pid();
        if (postmaster_pid <= 1) {
            throw process_error("cannot identify owned postmaster for crash");
        }
        if (::kill(static_cast<pid_t>(postmaster_pid), SIGKILL) != 0
            && errno != ESRCH) {
            throw process_error("cannot SIGKILL owned postmaster");
        }
        if (!wait_for_process_absence(
                postmaster_pid, options_.stop_timeout)) {
            throw process_error("postmaster did not exit after SIGKILL");
        }
        state_ = cluster_state::stopped;
    }

    /// @brief Stops the owned postmaster; never throws from cleanup paths.
    void stop() noexcept {
        if (state_ != cluster_state::running) {
            return;
        }
        const std::int64_t postmaster_pid = read_postmaster_pid();
        bool pg_ctl_stopped = false;
        try {
            const auto exit = run_program(
                "pg_ctl",
                {"--pgdata", data_directory_.string(),
                 "--mode", "fast",
                 "--wait",
                 "--timeout", std::to_string(options_.stop_timeout.count()),
                 "stop"},
                options_.stop_timeout + std::chrono::seconds(5),
                log_directory_ / "pg_ctl_stop.log");
            if (!exit.succeeded()) {
                cleanup_error_ = "pg_ctl stop returned nonzero: "
                    + read_file(log_directory_ / "pg_ctl_stop.log");
            } else {
                pg_ctl_stopped = true;
            }
        } catch (const std::exception& error) {
            cleanup_error_ = error.what();
        } catch (...) {
            cleanup_error_ = "unknown pg_ctl stop failure";
        }
        if (!wait_for_process_absence(postmaster_pid, options_.stop_timeout)) {
            if (postmaster_pid > 1) {
                static_cast<void>(
                    ::kill(static_cast<pid_t>(postmaster_pid), SIGINT));
            }
            if (!wait_for_process_absence(
                    postmaster_pid, std::chrono::seconds(2))) {
                if (postmaster_pid > 1) {
                    static_cast<void>(
                        ::kill(static_cast<pid_t>(postmaster_pid), SIGKILL));
                }
                static_cast<void>(wait_for_process_absence(
                    postmaster_pid, std::chrono::seconds(2)));
            }
        }
        if ((postmaster_pid > 1
             && cluster_detail::process_absent(postmaster_pid))
            || (postmaster_pid <= 1 && pg_ctl_stopped)) {
            state_ = cluster_state::stopped;
        } else {
            cleanup_error_ +=
                (cleanup_error_.empty() ? "" : "; ")
                + std::string("postmaster remains live; PGDATA retained at ")
                + data_directory_.string();
        }
    }

    /// @brief Opens a Unix-socket connection under the caller deadline.
    [[nodiscard]] bioseq::libpq::connection connect(
        std::chrono::milliseconds deadline) const {
        require_state(cluster_state::running, "connect");
        return bioseq::libpq::connection(
            bioseq::libpq::unix_socket_conninfo(
                socket_directory_.string(), "postgres", "postgres", port_),
            deadline);
    }

    /// @brief Requests cancellation of one backend through a dedicated admin session.
    void cancel_backend(std::int32_t pid) {
        run_backend_signal("pg_cancel_backend", pid);
    }

    /// @brief Requests termination of one backend through a dedicated admin session.
    void terminate_backend(std::int32_t pid) {
        run_backend_signal("pg_terminate_backend", pid);
    }

    /// @brief Returns the current explicit lifecycle state.
    [[nodiscard]] cluster_state state() const noexcept { return state_; }

    /// @brief Returns the exact PGDATA owned by this fixture.
    [[nodiscard]] const std::filesystem::path& data_directory() const noexcept {
        return data_directory_;
    }

    /// @brief Returns the short Unix-socket directory used by libpq.
    [[nodiscard]] const std::filesystem::path& socket_directory() const noexcept {
        return socket_directory_;
    }

    /// @brief Returns the exact root containing data, stage, and logs.
    [[nodiscard]] const std::filesystem::path& root_directory() const noexcept {
        return root_;
    }

    /// @brief Returns the staged artifact/digest inventory after initialize().
    [[nodiscard]] const extension_stage& staged_artifacts() const {
        if (!stage_) {
            throw std::logic_error("extension artifacts have not been staged");
        }
        return *stage_;
    }

    /// @brief Returns the most recent no-throw cleanup diagnostic.
    [[nodiscard]] std::string_view cleanup_error() const noexcept {
        return cleanup_error_;
    }

    /// @brief Returns the private cluster port paired with its unique socket path.
    [[nodiscard]] int port() const noexcept { return port_; }

private:
    /// @brief Requires one exact lifecycle state.
    void require_state(cluster_state expected, std::string_view operation) const {
        if (state_ != expected) {
            throw std::logic_error(std::string(operation)
                                   + " called in invalid cluster state");
        }
    }

    /// @brief Resolves one required PostgreSQL lifecycle executable.
    [[nodiscard]] std::filesystem::path program(std::string_view name) const {
        const std::filesystem::path candidate =
            options_.postgres_bin_dir / name;
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error) {
            throw process_error("PostgreSQL executable is missing: "
                                + candidate.string());
        }
        return std::filesystem::canonical(candidate);
    }

    /// @brief Runs one lifecycle executable in an owned process group.
    [[nodiscard]] process_exit run_program(
        std::string_view name,
        std::vector<std::string> arguments,
        std::chrono::milliseconds deadline,
        const std::filesystem::path& output) const {
        spawned_process child = spawned_process::spawn(process_options{
            .executable = program(name),
            .arguments = std::move(arguments),
            .environment = {},
            .output_file = output});
        return child.wait_for(deadline);
    }

    /// @brief Writes only validated private-cluster configuration settings.
    void write_configuration() {
        if (!stage_) {
            throw std::logic_error("cannot configure before artifact staging");
        }
        std::ofstream configuration(
            data_directory_ / "postgresql.conf", std::ios::app);
        if (!configuration) {
            throw std::runtime_error("cannot open private postgresql.conf");
        }
        configuration
            << "\n# Appended by bioseq.test.postgres\n"
            << "listen_addresses = ''\n"
            << "port = " << port_ << '\n'
            << "unix_socket_directories = "
            << cluster_detail::configuration_literal(
                   socket_directory_.string()) << '\n'
            << "extension_control_path = "
            << cluster_detail::configuration_literal(
                   stage_->control_directory.string() + ":$system") << '\n'
            << "dynamic_library_path = "
            << cluster_detail::configuration_literal(
                   stage_->library_directory.string() + ":$libdir") << '\n';
        if (!options_.preload_libraries.empty()) {
            std::string preloads;
            for (std::size_t index = 0;
                 index < options_.preload_libraries.size(); ++index) {
                cluster_detail::validate_library_name(
                    options_.preload_libraries[index]);
                if (index != 0) {
                    preloads.push_back(',');
                }
                preloads += options_.preload_libraries[index];
            }
            configuration << "shared_preload_libraries = "
                          << cluster_detail::configuration_literal(preloads)
                          << '\n';
        }
        for (const auto& [name, value] : options_.startup_gucs) {
            cluster_detail::validate_guc_name(name);
            configuration << name << " = "
                          << cluster_detail::configuration_literal(value)
                          << '\n';
        }
        configuration.close();
        if (!configuration) {
            throw std::runtime_error("failed writing private postgresql.conf");
        }
    }

    /// @brief Reads the current postmaster pid from owned PGDATA.
    [[nodiscard]] std::int64_t read_postmaster_pid() const noexcept {
        try {
            std::ifstream input(data_directory_ / "postmaster.pid");
            std::string first_line;
            if (!std::getline(input, first_line)) {
                return -1;
            }
            std::int64_t pid{};
            const auto [end, error] = std::from_chars(
                first_line.data(), first_line.data() + first_line.size(), pid);
            if (error != std::errc{}
                || end != first_line.data() + first_line.size()) {
                return -1;
            }
            return pid;
        } catch (...) {
            return -1;
        }
    }

    /// @brief Waits a bounded interval for the owned postmaster to disappear.
    [[nodiscard]] static bool wait_for_process_absence(
        std::int64_t pid, std::chrono::milliseconds deadline) noexcept {
        const auto finish = std::chrono::steady_clock::now()
            + std::max(deadline, std::chrono::milliseconds::zero());
        while (std::chrono::steady_clock::now() < finish) {
            if (cluster_detail::process_absent(pid)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return cluster_detail::process_absent(pid);
    }

    /// @brief Reads a lifecycle log without throwing during diagnostics.
    [[nodiscard]] static std::string read_file(
        const std::filesystem::path& path) noexcept {
        try {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>());
        } catch (...) {
            return {};
        }
    }

    /// @brief Executes one typed backend signal and requires a true result.
    void run_backend_signal(std::string_view function, std::int32_t pid) {
        require_state(cluster_state::running, function);
        if (pid <= 0) {
            throw std::invalid_argument("backend pid must be positive");
        }
        bioseq::libpq::connection admin = connect(std::chrono::seconds(5));
        const std::string sql = "SELECT " + std::string(function) + "("
            + std::to_string(pid) + ")";
        const bioseq::libpq::result result =
            admin.exec(sql, std::chrono::seconds(5));
        if (result.rows() != 1 || result.columns() != 1
            || result.is_null(0, 0) || result.value(0, 0) != "t") {
            throw process_error(std::string(function)
                                + " did not signal backend "
                                + std::to_string(pid));
        }
    }

    /// @brief Removes only a marker-proven root allocated by this instance.
    void remove_owned_root() noexcept {
        try {
            if (root_.empty()
                || !root_.filename().string().starts_with(
                    "bioseq-pg-test-")
                || !std::filesystem::is_regular_file(
                    root_ / ".bioseq-private-cluster")) {
                return;
            }
            const std::filesystem::path expected_parent =
                std::filesystem::canonical(
                    std::filesystem::temp_directory_path());
            const std::filesystem::path actual_parent =
                std::filesystem::canonical(root_.parent_path());
            if (actual_parent != expected_parent) {
                return;
            }
            std::error_code error;
            std::filesystem::remove_all(root_, error);
            if (error && cleanup_error_.empty()) {
                cleanup_error_ = "failed removing private root: "
                    + error.message();
            }
        } catch (const std::exception& error) {
            if (cleanup_error_.empty()) {
                cleanup_error_ = error.what();
            }
        } catch (...) {
            if (cleanup_error_.empty()) {
                cleanup_error_ = "unknown private-root cleanup failure";
            }
        }
    }

    cluster_options options_;
    cluster_state state_{cluster_state::constructed};
    std::filesystem::path root_;
    std::filesystem::path data_directory_;
    std::filesystem::path socket_directory_;
    std::filesystem::path log_directory_;
    std::optional<extension_stage> stage_;
    int port_{};
    std::string cleanup_error_;
};

}  // namespace bioseq::postgres_test
