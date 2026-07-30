module;

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc.h>
#endif

#include <charconv>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
import :report;
import :resource_sampler;
import :supervision;

export namespace bioseq::postgres_test {

/// @brief Exact marker-owned paths available immediately before initdb.
struct pre_initdb_context {
    const std::filesystem::path& root_directory;
    const std::filesystem::path& data_directory;
    const std::filesystem::path& socket_directory;
    const std::filesystem::path& log_directory;
};

/// @brief Complete source-neutral configuration for one private PostgreSQL node.
struct cluster_options {
    std::filesystem::path postgres_bin_dir;
    std::vector<extension_artifact> extensions;
    std::vector<std::string> preload_libraries;
    std::vector<std::pair<std::string, std::string>> startup_gucs;
    std::vector<std::pair<std::string, std::string>> process_environment;
    std::function<void(const pre_initdb_context&)> pre_initdb;
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

/// @brief Terminal no-throw cleanup result for one owned private cluster.
struct cluster_cleanup_result {
    bool succeeded{};
    std::string detail;
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

/// @brief Removes a just-created marker-owned root before any process exists.
[[nodiscard]] inline bool remove_preprocess_private_root(
    const std::filesystem::path& root) noexcept try {
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return !error;
    }
    if (root.empty()
        || !root.filename().string().starts_with("bioseq-pg-test-")
        || !std::filesystem::is_regular_file(
            root / ".bioseq-private-cluster")) {
        return false;
    }
    const std::filesystem::path expected_parent =
        std::filesystem::canonical(std::filesystem::temp_directory_path());
    const std::filesystem::path actual_parent =
        std::filesystem::canonical(root.parent_path());
    if (actual_parent != expected_parent) {
        return false;
    }
    error.clear();
    std::filesystem::remove_all(root, error);
    return !error && !std::filesystem::exists(root, error) && !error;
} catch (...) {
    return false;
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

/// @brief PID-reuse-resistant snapshot of a stable postmaster process tree.
struct supervised_process_tree {
    supervised_process_record postmaster;
    std::vector<supervised_process_record> members;
    std::vector<std::int64_t> process_groups;
};

/// @brief Matches the captured run even if that process changed groups.
[[nodiscard]] inline bool process_identity_matches(
    const supervised_process_record& process) noexcept {
    const auto current = capture_supervised_process(
        process.pid, {}, process.data_directory);
    return current && current->start_token == process.start_token;
}

/// @brief Captures the complete tree twice so transient forks cannot be missed.
[[nodiscard]] inline std::optional<supervised_process_tree>
capture_process_tree(
    const supervised_process_record& anchor) noexcept try {
    if (!process_identity_matches(anchor)) {
        return std::nullopt;
    }
    for (int attempt = 0; attempt != 20; ++attempt) {
        auto first_pids =
            sample_postmaster_process_tree_pids(anchor.pid);
        if (!first_pids
            || std::find(
                   first_pids->begin(), first_pids->end(), anchor.pid)
                == first_pids->end()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::sort(first_pids->begin(), first_pids->end());
        first_pids->erase(
            std::unique(first_pids->begin(), first_pids->end()),
            first_pids->end());

        supervised_process_tree tree;
        tree.members.reserve(first_pids->size());
        bool complete = true;
        for (const std::uint64_t pid : *first_pids) {
            const auto member = capture_supervised_process(
                pid,
                pid == anchor.pid ? anchor.executable
                                  : std::filesystem::path{},
                anchor.data_directory);
            if (!member) {
                complete = false;
                break;
            }
            tree.members.push_back(*member);
            if (std::find(
                    tree.process_groups.begin(),
                    tree.process_groups.end(),
                    member->process_group)
                == tree.process_groups.end()) {
                tree.process_groups.push_back(member->process_group);
            }
            if (pid == anchor.pid) {
                tree.postmaster = *member;
            }
        }
        if (!complete || tree.postmaster.pid != anchor.pid
            || tree.postmaster.start_token != anchor.start_token
            || tree.postmaster.executable != anchor.executable) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        auto second_pids =
            sample_postmaster_process_tree_pids(anchor.pid);
        if (!second_pids) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::sort(second_pids->begin(), second_pids->end());
        second_pids->erase(
            std::unique(second_pids->begin(), second_pids->end()),
            second_pids->end());
        if (*first_pids == *second_pids) {
            return tree;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
} catch (...) {
    return std::nullopt;
}

/// @brief True when the exact captured process is kernel-stopped.
[[nodiscard]] inline bool process_identity_stopped(
    const supervised_process_record& process) noexcept try {
    if (!process_identity_matches(process)) {
        return false;
    }
#if defined(__linux__)
    std::ifstream input(
        std::filesystem::path("/proc")
        / std::to_string(process.pid) / "stat");
    std::string line;
    if (!input || !std::getline(input, line)) {
        return false;
    }
    const std::size_t command_end = line.rfind(')');
    if (command_end == std::string::npos
        || command_end + 2U >= line.size()) {
        return false;
    }
    const char state = line[command_end + 2U];
    return state == 'T' || state == 't';
#elif defined(__APPLE__)
    proc_bsdinfo information{};
    return ::proc_pidinfo(
               static_cast<int>(process.pid), PROC_PIDTBSDINFO, 0,
               &information, sizeof(information))
            == static_cast<int>(sizeof(information))
        && information.pbi_status == SSTOP;
#else
    return false;
#endif
} catch (...) {
    return false;
}

/// @brief Resumes every still-matching identity without group-wide signaling.
inline void resume_processes(
    const std::vector<supervised_process_record>& processes) noexcept {
    for (const supervised_process_record& process : processes) {
        if (process_identity_matches(process)) {
            static_cast<void>(
                ::kill(static_cast<pid_t>(process.pid), SIGCONT));
        }
    }
}

[[nodiscard]] inline bool signal_matching_process(
    const supervised_process_record& process, int signal_number) noexcept;

/// @brief Signals groups only while one exact captured member anchors each PGID.
[[nodiscard]] inline bool signal_anchored_process_groups(
    const supervised_process_tree& tree, int signal_number) noexcept {
    bool succeeded = true;
    const std::int64_t own_process_group =
        static_cast<std::int64_t>(::getpgrp());
    for (const std::int64_t group : tree.process_groups) {
        bool anchored = false;
        for (const supervised_process_record& member : tree.members) {
            const auto current = capture_supervised_process(
                member.pid, {}, member.data_directory);
            if (current && current->start_token == member.start_token
                && current->process_group == group) {
                anchored = true;
                break;
            }
        }
        if (!anchored) {
            continue;
        }
        if (group <= 1 || group == own_process_group
            || (::kill(-static_cast<pid_t>(group), signal_number) != 0
                && errno != ESRCH)) {
            succeeded = false;
        }
    }
    return succeeded;
}

/// @brief Freezes a stable anchored tree before any force-cleanup decision.
[[nodiscard]] inline std::optional<supervised_process_tree>
freeze_process_tree(
    const supervised_process_record& anchor) noexcept {
    std::vector<supervised_process_record> stopped;
    supervised_process_tree frozen;
    try {
        frozen.postmaster = anchor;
        frozen.members.push_back(anchor);
        frozen.process_groups.push_back(anchor.process_group);
        stopped.push_back(anchor);
        if (!signal_anchored_process_groups(frozen, SIGSTOP)) {
            return std::nullopt;
        }
        for (int attempt = 0; attempt != 50; ++attempt) {
            const auto tree = capture_process_tree(anchor);
            if (!tree) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            for (const supervised_process_record& member : tree->members) {
                const auto known = std::find_if(
                    stopped.begin(), stopped.end(),
                    [&member](const supervised_process_record& candidate) {
                        return candidate.pid == member.pid
                            && candidate.start_token == member.start_token;
                    });
                if (known == stopped.end()) {
                    stopped.push_back(member);
                    frozen.members.push_back(member);
                }
                if (std::find(
                        frozen.process_groups.begin(),
                        frozen.process_groups.end(),
                        member.process_group)
                    == frozen.process_groups.end()) {
                    frozen.process_groups.push_back(member.process_group);
                }
            }
            if (!signal_anchored_process_groups(frozen, SIGSTOP)) {
                static_cast<void>(
                    signal_anchored_process_groups(frozen, SIGCONT));
                resume_processes(stopped);
                return std::nullopt;
            }
            for (const supervised_process_record& member : tree->members) {
                if (!signal_matching_process(member, SIGSTOP)) {
                    static_cast<void>(
                        signal_anchored_process_groups(frozen, SIGCONT));
                    resume_processes(stopped);
                    return std::nullopt;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto confirmation = capture_process_tree(anchor);
            if (!confirmation
                || confirmation->members.size() != tree->members.size()) {
                continue;
            }
            bool identical_and_stopped = true;
            for (std::size_t index = 0;
                 index < tree->members.size(); ++index) {
                const supervised_process_record& before =
                    tree->members[index];
                const supervised_process_record& after =
                    confirmation->members[index];
                if (before.pid != after.pid
                    || before.start_token != after.start_token
                    || !process_identity_stopped(after)) {
                    identical_and_stopped = false;
                    break;
                }
            }
            if (identical_and_stopped) {
                return frozen;
            }
        }
        static_cast<void>(
            signal_anchored_process_groups(frozen, SIGCONT));
        resume_processes(stopped);
        return std::nullopt;
    } catch (...) {
        static_cast<void>(
            signal_anchored_process_groups(frozen, SIGCONT));
        resume_processes(stopped);
        return std::nullopt;
    }
}

/// @brief True only after every captured identity and process group is gone.
[[nodiscard]] inline bool process_tree_absent(
    const supervised_process_tree& tree) noexcept {
    const bool members_absent = std::all_of(
        tree.members.begin(), tree.members.end(),
        [](const supervised_process_record& member) {
            return !process_identity_matches(member);
        });
    const bool groups_absent = std::all_of(
        tree.process_groups.begin(), tree.process_groups.end(),
        [](std::int64_t group) {
            return supervised_process_group_absent(group);
        });
    return members_absent && groups_absent;
}

/// @brief Waits a bounded interval for the captured tree and groups to vanish.
[[nodiscard]] inline bool wait_for_process_tree_absence(
    const supervised_process_tree& tree,
    std::chrono::milliseconds deadline) noexcept {
    const auto finish = std::chrono::steady_clock::now()
        + std::max(deadline, std::chrono::milliseconds::zero());
    while (std::chrono::steady_clock::now() < finish) {
        if (process_tree_absent(tree)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return process_tree_absent(tree);
}

/// @brief Sends a signal only while the captured PID still has its identity.
[[nodiscard]] inline bool signal_matching_process(
    const supervised_process_record& process, int signal_number) noexcept {
    if (!process_identity_matches(process)) {
        return true;
    }
    return ::kill(static_cast<pid_t>(process.pid), signal_number) == 0
        || errno == ESRCH;
}

/// @brief Force-kills only groups anchored by a still-matching owned member.
[[nodiscard]] inline bool force_kill_process_tree(
    const supervised_process_tree& tree) noexcept {
    bool succeeded = true;
    const std::int64_t own_process_group =
        static_cast<std::int64_t>(::getpgrp());
    for (const std::int64_t group : tree.process_groups) {
        bool anchored = false;
        for (const supervised_process_record& member : tree.members) {
            const auto current = capture_supervised_process(
                member.pid, {}, member.data_directory);
            if (member.process_group == group && current
                && current->start_token == member.start_token
                && current->process_group == group) {
                anchored = true;
                break;
            }
        }
        if (!anchored) {
            continue;
        }
        if (group > 1 && group != own_process_group) {
            if (::kill(-static_cast<pid_t>(group), SIGKILL) == 0
                || errno == ESRCH) {
                continue;
            }
            succeeded = false;
        }
    }
    for (const supervised_process_record& member : tree.members) {
        succeeded =
            signal_matching_process(member, SIGKILL) && succeeded;
    }
    return succeeded;
}

}  // namespace bioseq::postgres_test::cluster_detail

export namespace bioseq::postgres_test {

/// @brief Renders version-portable settings for one staged private cluster.
/// @param port Private postmaster port paired with its Unix socket directory.
/// @param socket_directory Private Unix-domain socket directory.
/// @param staged_library_directory Per-case DSO directory searched first.
/// @param preload_libraries Validated shared-preload library names.
/// @param startup_gucs Additional validated startup settings.
/// @return Complete configuration text appended after initdb.
[[nodiscard]] inline std::string render_private_cluster_configuration(
    int port,
    const std::filesystem::path& socket_directory,
    const std::filesystem::path& staged_library_directory,
    std::span<const std::string> preload_libraries,
    std::span<const std::pair<std::string, std::string>> startup_gucs) {
    std::string configuration =
        "\n# Appended by bioseq.test.postgres\n"
        "listen_addresses = ''\n"
        "port = "
        + std::to_string(port)
        + "\nunix_socket_directories = "
        + cluster_detail::configuration_literal(socket_directory.string())
        + "\ndynamic_library_path = "
        + cluster_detail::configuration_literal(
            staged_library_directory.string() + ":$libdir")
        + '\n';
    if (!preload_libraries.empty()) {
        std::string preloads;
        for (std::size_t index = 0;
             index < preload_libraries.size(); ++index) {
            cluster_detail::validate_library_name(preload_libraries[index]);
            if (index != 0) {
                preloads.push_back(',');
            }
            preloads += preload_libraries[index];
        }
        configuration += "shared_preload_libraries = "
            + cluster_detail::configuration_literal(preloads) + '\n';
    }
    for (const auto& [name, value] : startup_gucs) {
        cluster_detail::validate_guc_name(name);
        configuration += name + " = "
            + cluster_detail::configuration_literal(value) + '\n';
    }
    return configuration;
}

/// @brief RAII owner of a staged, private PostgreSQL cluster.
class private_cluster {
public:
    explicit private_cluster(cluster_options options)
        : options_(cluster_detail::validate_options(std::move(options))),
          root_(cluster_detail::make_private_root()),
          data_directory_(root_ / "data"),
          socket_directory_(root_ / "socket"),
          log_directory_(root_ / "log"),
          port_(cluster_detail::available_port()) {
        try {
            register_supervised_cluster(supervised_cluster_record{
                .root = root_,
                .data_directory = data_directory_,
                .postmaster_executable =
                    options_.postgres_bin_dir / "postgres"});
        } catch (...) {
            static_cast<void>(unregister_supervised_cluster(root_));
            if (!cluster_detail::remove_preprocess_private_root(root_)) {
                throw std::runtime_error(
                    "supervision registration failed and the marker-owned "
                    "private root could not be removed");
            }
            throw;
        }
    }

    ~private_cluster() noexcept {
        static_cast<void>(cleanup());
    }

    private_cluster(const private_cluster&) = delete;
    private_cluster& operator=(const private_cluster&) = delete;

    /// @brief Stages artifacts, initializes PGDATA, and writes private settings.
    void initialize() {
        if (cleanup_attempted_) {
            throw std::logic_error(
                "initialize cannot reuse a cleaned private cluster");
        }
        require_state(cluster_state::constructed, "initialize");
        std::filesystem::create_directories(data_directory_);
        std::filesystem::create_directories(socket_directory_);
        std::filesystem::create_directories(log_directory_);
        stage_ = stage_extension_artifacts(root_ / "stage", options_.extensions);
        if (automatic_report_collection_enabled()) {
            record_artifact_stage(*stage_);
        }
        if (options_.pre_initdb) {
            options_.pre_initdb(pre_initdb_context{
                .root_directory = root_,
                .data_directory = data_directory_,
                .socket_directory = socket_directory_,
                .log_directory = log_directory_});
        }
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
        if (cleanup_attempted_) {
            throw std::logic_error(
                "start cannot reuse a cleaned private cluster");
        }
        if (state_ != cluster_state::initialized
            && state_ != cluster_state::stopped) {
            throw std::logic_error("start requires initialized or stopped state");
        }
        cleanup_error_.clear();
        causally_trusted_unanchored_start_ = false;
        std::error_code pidfile_error;
        const bool pidfile_exists = std::filesystem::exists(
            data_directory_ / "postmaster.pid", pidfile_error);
        if (pidfile_error || pidfile_exists) {
            throw process_error(
                "refusing startup with a pre-existing postmaster pidfile");
        }
        state_ = cluster_state::running;
        causally_trusted_unanchored_start_ = true;
        try {
            register_supervised_cluster(supervised_cluster_record{
                .root = root_,
                .data_directory = data_directory_,
                .discover_postmaster_pid = true,
                .postmaster_executable =
                    options_.postgres_bin_dir / "postgres"});
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
            const std::int64_t postmaster_pid = read_postmaster_pid();
            for (int attempt = 0; attempt != 20; ++attempt) {
                postmaster_identity_ = capture_supervised_process(
                    static_cast<std::uint64_t>(postmaster_pid),
                    options_.postgres_bin_dir / "postgres",
                    data_directory_);
                if (postmaster_identity_
                    && read_postmaster_pid() == postmaster_pid
                    && supervised_postmaster_matches_data_directory(
                        *postmaster_identity_)) {
                    break;
                }
                postmaster_identity_.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!postmaster_identity_) {
                throw process_error(
                    "cannot persist the started postmaster identity");
            }
            causally_trusted_unanchored_start_ = false;
            register_supervised_cluster(supervised_cluster_record{
                .root = root_,
                .data_directory = data_directory_,
                .postmaster_pid = postmaster_identity_->pid,
                .postmaster_start_token =
                    postmaster_identity_->start_token,
                .postmaster_executable =
                    postmaster_identity_->executable,
                .postmaster_process_group =
                    postmaster_identity_->process_group});
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
        if (!postmaster_identity_) {
            throw process_error(
                "started postmaster identity is unavailable for crash");
        }
        const auto tree =
            cluster_detail::freeze_process_tree(*postmaster_identity_);
        if (!tree) {
            throw process_error(
                "cannot freeze a stable anchored postmaster tree for crash");
        }
        if (!cluster_detail::signal_matching_process(
                tree->postmaster, SIGKILL)) {
            static_cast<void>(
                cluster_detail::signal_anchored_process_groups(
                    *tree, SIGCONT));
            cluster_detail::resume_processes(tree->members);
            throw process_error(
                "cannot SIGKILL identity-verified owned postmaster");
        }
        static_cast<void>(cluster_detail::signal_matching_process(
            tree->postmaster, SIGCONT));
        static_cast<void>(
            cluster_detail::signal_anchored_process_groups(
                *tree, SIGCONT));
        cluster_detail::resume_processes(tree->members);
        bool absent = cluster_detail::wait_for_process_tree_absence(
            *tree, std::chrono::seconds(2));
        if (!absent) {
            static_cast<void>(
                cluster_detail::force_kill_process_tree(*tree));
            absent = cluster_detail::wait_for_process_tree_absence(
                *tree, options_.stop_timeout);
        }
        if (!absent) {
            throw process_error(
                "postmaster descendant or process group remains after "
                "identity-verified force cleanup; PGDATA retained at "
                + data_directory_.string());
        }
        std::error_code pidfile_error;
        std::filesystem::remove(
            data_directory_ / "postmaster.pid", pidfile_error);
        if (pidfile_error) {
            throw process_error(
                "cannot remove the verified-dead postmaster pidfile: "
                + pidfile_error.message());
        }
        state_ = cluster_state::stopped;
        postmaster_identity_.reset();
        causally_trusted_unanchored_start_ = false;
        register_supervised_cluster(supervised_cluster_record{
            .root = root_,
            .data_directory = data_directory_,
            .postmaster_executable =
                options_.postgres_bin_dir / "postgres"});
    }

    /// @brief Stops the owned postmaster; never throws from cleanup paths.
    void stop() noexcept try {
        if (state_ != cluster_state::running) {
            return;
        }
        cleanup_error_.clear();
        if (!postmaster_identity_
            && causally_trusted_unanchored_start_) {
            for (int attempt = 0; attempt != 20; ++attempt) {
                const std::int64_t postmaster_pid =
                    read_postmaster_pid();
                if (postmaster_pid <= 1) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                    continue;
                }
                postmaster_identity_ = capture_supervised_process(
                    static_cast<std::uint64_t>(postmaster_pid),
                    options_.postgres_bin_dir / "postgres",
                    data_directory_);
                if (postmaster_identity_
                    && read_postmaster_pid() == postmaster_pid
                    && supervised_postmaster_matches_data_directory(
                        *postmaster_identity_)) {
                    register_supervised_cluster(supervised_cluster_record{
                        .root = root_,
                        .data_directory = data_directory_,
                        .postmaster_pid = postmaster_identity_->pid,
                        .postmaster_start_token =
                            postmaster_identity_->start_token,
                        .postmaster_executable =
                            postmaster_identity_->executable,
                        .postmaster_process_group =
                            postmaster_identity_->process_group});
                    causally_trusted_unanchored_start_ = false;
                    break;
                }
                postmaster_identity_.reset();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(20));
            }
        }
        if (!postmaster_identity_) {
            cleanup_error_ =
                "started postmaster identity is unavailable after trusted "
                "launch recovery; PGDATA retained at "
                + data_directory_.string();
            return;
        }
        const auto initial_tree =
            cluster_detail::capture_process_tree(*postmaster_identity_);
        if (!initial_tree) {
            cleanup_error_ =
                "cannot capture a stable identity-verified postmaster tree; "
                "PGDATA retained at "
                + data_directory_.string();
            return;
        }

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
            }
        } catch (const std::exception& error) {
            cleanup_error_ = error.what();
        } catch (...) {
            cleanup_error_ = "unknown pg_ctl stop failure";
        }

        bool absent = cluster_detail::wait_for_process_tree_absence(
            *initial_tree, options_.stop_timeout);
        if (!absent) {
            std::optional<cluster_detail::supervised_process_tree> force_tree;
            if (cluster_detail::process_identity_matches(
                    *postmaster_identity_)) {
                force_tree = cluster_detail::freeze_process_tree(
                    *postmaster_identity_);
            } else {
                force_tree = initial_tree;
            }
            if (force_tree) {
                if (!cluster_detail::force_kill_process_tree(*force_tree)) {
                    set_cleanup_error_noexcept(
                        "cannot SIGKILL identity-verified postmaster tree");
                }
                static_cast<void>(
                    cluster_detail::signal_anchored_process_groups(
                        *force_tree, SIGCONT));
                cluster_detail::resume_processes(force_tree->members);
                absent = cluster_detail::wait_for_process_tree_absence(
                    *force_tree, options_.stop_timeout);
            } else {
                absent = false;
            }
        }
        if (absent) {
            cleanup_error_.clear();
            state_ = cluster_state::stopped;
            postmaster_identity_.reset();
            causally_trusted_unanchored_start_ = false;
            register_supervised_cluster(supervised_cluster_record{
                .root = root_,
                .data_directory = data_directory_,
                .postmaster_executable =
                    options_.postgres_bin_dir / "postgres"});
        } else {
            cleanup_error_ +=
                (cleanup_error_.empty() ? "" : "; ")
                + std::string(
                    "postmaster descendant or process group remains live; "
                    "PGDATA retained at ")
                + data_directory_.string();
        }
    } catch (...) {
        set_cleanup_error_noexcept(
            "private cluster stop failed while recording diagnostics");
    }

    /// @brief Retries bounded stop/removal until cleanup is verified complete.
    [[nodiscard]] const cluster_cleanup_result& cleanup() noexcept try {
        if (cleanup_result_.succeeded) {
            return cleanup_result_;
        }
        cleanup_attempted_ = true;
        cleanup_result_ = {};
        cleanup_error_.clear();
        stop();
        if (state_ != cluster_state::running) {
            remove_owned_root();
        }
        std::error_code error;
        const bool root_exists = std::filesystem::exists(root_, error);
        cleanup_result_.succeeded =
            state_ != cluster_state::running
            && !root_exists
            && !error
            && cleanup_error_.empty();
        if (cleanup_result_.succeeded) {
            cleanup_result_.detail =
                "postmaster stopped and private root removed";
        } else if (!cleanup_error_.empty()) {
            cleanup_result_.detail = cleanup_error_;
        } else if (state_ == cluster_state::running) {
            cleanup_result_.detail =
                "postmaster remains live; private root retained";
        } else if (error) {
            cleanup_result_.detail =
                "cannot verify private-root cleanup: " + error.message();
        } else {
            cleanup_result_.detail =
                "private root remains after cleanup";
        }
        return cleanup_result_;
    } catch (...) {
        cleanup_result_.succeeded = false;
        cleanup_result_.detail.clear();
        set_cleanup_error_noexcept(
            "private cluster cleanup failed while recording diagnostics");
        return cleanup_result_;
    }

    /// @brief Opens a Unix-socket connection under the caller deadline.
    [[nodiscard]] bioseq::libpq::connection connect(
        std::chrono::milliseconds deadline) const {
        require_state(cluster_state::running, "connect");
        bioseq::libpq::connection connection(
            bioseq::libpq::unix_socket_conninfo(
                socket_directory_.string(), "postgres", "postgres", port_),
            deadline);
        attach_process_sql_transcript(connection, "cluster");
        return connection;
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
            .environment = options_.process_environment,
            .preferred_path_directory = options_.postgres_bin_dir,
            .output_file = output,
            .supervise = true,
            .supervision_data_directory = data_directory_});
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
        configuration << render_private_cluster_configuration(
            port_, socket_directory_, stage_->library_directory,
            options_.preload_libraries, options_.startup_gucs);
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

    /// @brief Records cleanup diagnostics without allowing allocation failures out.
    void set_cleanup_error_noexcept(std::string_view message) noexcept {
        try {
            if (cleanup_error_.empty()) {
                cleanup_error_.assign(message);
            }
        } catch (...) {
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
            if (!unregister_supervised_cluster(root_)) {
                set_cleanup_error_noexcept(
                    "failed removing private cluster from supervision; "
                    "private root retained for retry");
                return;
            }
            std::error_code error;
            std::filesystem::remove_all(root_, error);
            if (error && cleanup_error_.empty()) {
                cleanup_error_ = "failed removing private root: "
                    + error.message();
            }
        } catch (const std::exception& error) {
            set_cleanup_error_noexcept(error.what());
        } catch (...) {
            set_cleanup_error_noexcept(
                "unknown private-root cleanup failure");
        }
    }

    cluster_options options_;
    cluster_state state_{cluster_state::constructed};
    std::filesystem::path root_;
    std::filesystem::path data_directory_;
    std::filesystem::path socket_directory_;
    std::filesystem::path log_directory_;
    std::optional<extension_stage> stage_;
    std::optional<supervised_process_record> postmaster_identity_;
    int port_{};
    std::string cleanup_error_;
    bool cleanup_attempted_{};
    bool causally_trusted_unanchored_start_{};
    cluster_cleanup_result cleanup_result_;
};

}  // namespace bioseq::postgres_test
