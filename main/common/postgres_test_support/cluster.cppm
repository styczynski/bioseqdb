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
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
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

/// @brief Optional source-neutral Boolean gate after extension installation.
struct post_extension_readiness_probe {
    std::string sql;
    std::chrono::milliseconds deadline{60'000};
    std::chrono::milliseconds poll_interval{20};
};

/// @brief Complete source-neutral configuration for one private PostgreSQL node.
struct cluster_options {
    std::filesystem::path postgres_bin_dir;
    std::filesystem::path preinstalled_stage_root;
    std::filesystem::path preinstalled_stage_manifest;
    std::vector<extension_artifact> extensions;
    std::set<std::string> allowed_runtime_dso_overrides;
    std::vector<std::string> preload_libraries;
    std::vector<std::pair<std::string, std::string>> startup_gucs;
    std::vector<std::pair<std::string, std::string>> process_environment;
    std::function<void(const pre_initdb_context&)> pre_initdb;
    std::optional<post_extension_readiness_probe>
        post_extension_readiness;
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

/// @brief Deterministic fault points for the topology-controller contract.
enum class process_tree_controller_fault {
    none,
    reject_second_descendant_stop,
    identity_drift_before_stop,
    reject_second_descendant_kill,
    reject_root_group_kill,
};

/// @brief Trace produced by the non-signalling controller contract model.
struct process_tree_controller_contract_trace {
    bool freeze_succeeded{};
    bool resume_attempted{};
    bool resume_complete{};
    bool rollback_attempted_after_commit{};
    std::vector<std::uint64_t> stop_order;
    std::vector<std::uint64_t> resume_order;
    std::vector<std::uint64_t> kill_order;
    process_tree_termination_result termination;
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::cluster_detail {

/// @brief Validates options before any temporary directory is allocated.
[[nodiscard]] inline cluster_options validate_options(cluster_options options) {
    if (options.startup_timeout <= std::chrono::seconds::zero()
        || options.stop_timeout <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("cluster deadlines must be positive");
    }
    if (options.post_extension_readiness) {
        const post_extension_readiness_probe& probe =
            *options.post_extension_readiness;
        if (probe.sql.empty()) {
            throw std::invalid_argument(
                "post-extension readiness SQL must not be empty");
        }
        if (probe.deadline <= std::chrono::milliseconds::zero()
            || probe.poll_interval <= std::chrono::milliseconds::zero()
            || probe.poll_interval > probe.deadline) {
            throw std::invalid_argument(
                "post-extension readiness deadlines must be positive and "
                "ordered");
        }
    }
    if (options.preinstalled_stage_root.empty()
        || options.preinstalled_stage_manifest.empty()) {
        throw std::invalid_argument(
            "private PostgreSQL requires an explicit preinstalled stage "
            "root and v2 completion manifest");
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
        const auto executable_status =
            std::filesystem::symlink_status(candidate, error);
        if (error || !std::filesystem::is_regular_file(executable_status)
            || std::filesystem::is_symlink(executable_status)) {
            throw std::invalid_argument(
                "PostgreSQL lifecycle executable is missing or a symlink: "
                + candidate.string());
        }
    }
    return options;
}

/// @brief True only when candidate is a proper descendant of root.
[[nodiscard]] inline bool path_is_strictly_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    while (root_part != root.end()) {
        if (candidate_part == candidate.end()
            || *root_part != *candidate_part) {
            return false;
        }
        ++root_part;
        ++candidate_part;
    }
    return candidate_part != candidate.end();
}

/// @brief Revalidates the attested sharedir immediately before initdb.
[[nodiscard]] inline std::filesystem::path
require_initdb_shared_directory(const extension_stage& stage) {
    const auto require_canonical_directory =
        [](const std::filesystem::path& path,
           std::string_view label) -> std::filesystem::path {
        if (path.empty()) {
            throw std::invalid_argument(
                std::string(label) + " is empty");
        }
        std::error_code error;
        const auto status =
            std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_directory(status)
            || std::filesystem::is_symlink(status)) {
            throw std::invalid_argument(
                std::string(label)
                + " is not a real non-symlink directory: "
                + path.string());
        }
        error.clear();
        const std::filesystem::path canonical =
            std::filesystem::canonical(path, error);
        if (error || canonical != path) {
            throw std::invalid_argument(
                std::string(label)
                + " is not an exact canonical directory: "
                + path.string());
        }
        return canonical;
    };

    const std::filesystem::path stage_root =
        require_canonical_directory(
            stage.stage_root, "attested PostgreSQL stage root");
    const std::filesystem::path shared_directory =
        require_canonical_directory(
            stage.shared_directory, "attested PostgreSQL shared directory");
    if (!path_is_strictly_within(stage_root, shared_directory)) {
        throw std::invalid_argument(
            "attested PostgreSQL shared directory is outside its stage root");
    }
    return shared_directory;
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

/// @brief Reads one complete positive postmaster PID from the owned PGDATA.
[[nodiscard]] inline std::int64_t read_postmaster_pid(
    const std::filesystem::path& data_directory) noexcept {
    try {
        std::ifstream input(data_directory / "postmaster.pid");
        std::string first_line;
        if (!std::getline(input, first_line)) {
            return -1;
        }
        std::int64_t pid{};
        const auto [end, error] = std::from_chars(
            first_line.data(), first_line.data() + first_line.size(), pid);
        if (error != std::errc{}
            || end != first_line.data() + first_line.size()
            || pid <= 1
            || pid > static_cast<std::int64_t>(
                std::numeric_limits<pid_t>::max())) {
            return -1;
        }
        return pid;
    } catch (...) {
        return -1;
    }
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
struct supervised_process_tree_member {
    supervised_process_record process;
    std::uint64_t parent_pid{};
    std::size_t depth{};
};

struct supervised_process_tree {
    supervised_process_record postmaster;
    std::vector<supervised_process_tree_member> members;
    std::vector<std::int64_t> process_groups;
};

enum class process_tree_freeze_outcome {
    frozen,
    failed_resumed,
    failed_resume_incomplete,
};

struct process_tree_freeze_result {
    process_tree_freeze_outcome outcome{
        process_tree_freeze_outcome::failed_resumed};
    std::optional<supervised_process_tree> tree;

    [[nodiscard]] bool frozen() const noexcept {
        return outcome == process_tree_freeze_outcome::frozen
            && tree.has_value();
    }
};

/// @brief Matches the captured run even if that process changed groups.
[[nodiscard]] inline bool process_identity_matches(
    const supervised_process_record& process) noexcept {
    const auto current = capture_supervised_process(
        process.pid, {}, process.data_directory);
    return current && current->start_token == process.start_token;
}

[[nodiscard]] inline bool same_process_identity(
    const supervised_process_record& left,
    const supervised_process_record& right) noexcept {
    return left.pid == right.pid
        && left.start_token == right.start_token
        && left.executable == right.executable;
}

inline void normalize_process_tree_snapshot(
    std::vector<postmaster_process_tree_member>& snapshot) {
    std::sort(
        snapshot.begin(), snapshot.end(),
        [](const postmaster_process_tree_member& left,
           const postmaster_process_tree_member& right) {
            return std::pair{left.pid, left.parent_pid}
                < std::pair{right.pid, right.parent_pid};
        });
}

[[nodiscard]] inline std::optional<std::vector<std::size_t>>
process_tree_depths(
    const std::vector<postmaster_process_tree_member>& snapshot,
    std::uint64_t root_pid) {
    std::vector<std::optional<std::size_t>> pending(snapshot.size());
    const auto root = std::find_if(
        snapshot.begin(), snapshot.end(),
        [root_pid](const postmaster_process_tree_member& member) {
            return member.pid == root_pid;
        });
    if (root == snapshot.end()) {
        return std::nullopt;
    }
    pending[static_cast<std::size_t>(
        std::distance(snapshot.begin(), root))] = 0U;
    for (std::size_t pass = 0; pass < snapshot.size(); ++pass) {
        bool progressed = false;
        for (std::size_t index = 0; index < snapshot.size(); ++index) {
            if (pending[index]) {
                continue;
            }
            const auto parent = std::find_if(
                snapshot.begin(), snapshot.end(),
                [&snapshot, index](
                    const postmaster_process_tree_member& member) {
                    return member.pid == snapshot[index].parent_pid;
                });
            if (parent == snapshot.end()) {
                return std::nullopt;
            }
            const std::size_t parent_index =
                static_cast<std::size_t>(
                    std::distance(snapshot.begin(), parent));
            if (pending[parent_index]) {
                pending[index] = *pending[parent_index] + 1U;
                progressed = true;
            }
        }
        if (!progressed) {
            break;
        }
    }
    std::vector<std::size_t> depths;
    depths.reserve(pending.size());
    for (const auto depth : pending) {
        if (!depth) {
            return std::nullopt;
        }
        depths.push_back(*depth);
    }
    return depths;
}

/// @brief Captures the complete parent-linked tree twice around identities.
[[nodiscard]] inline std::optional<supervised_process_tree>
capture_process_tree(
    const supervised_process_record& anchor) noexcept try {
    if (!process_identity_matches(anchor)) {
        return std::nullopt;
    }
    for (int attempt = 0; attempt != 10; ++attempt) {
        auto first = sample_postmaster_process_tree(anchor.pid);
        if (!first) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        normalize_process_tree_snapshot(*first);
        const auto depths = process_tree_depths(*first, anchor.pid);
        if (!depths) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        supervised_process_tree tree;
        tree.members.reserve(first->size());
        bool complete = true;
        for (std::size_t index = 0; index < first->size(); ++index) {
            const postmaster_process_tree_member topology =
                (*first)[index];
            const auto member = capture_supervised_process(
                topology.pid,
                topology.pid == anchor.pid
                    ? anchor.executable
                    : std::filesystem::path{},
                anchor.data_directory);
            if (!member) {
                complete = false;
                break;
            }
            tree.members.push_back(supervised_process_tree_member{
                .process = *member,
                .parent_pid = topology.parent_pid,
                .depth = (*depths)[index]});
            if (std::find(
                    tree.process_groups.begin(),
                    tree.process_groups.end(),
                    member->process_group)
                == tree.process_groups.end()) {
                tree.process_groups.push_back(member->process_group);
            }
            if (topology.pid == anchor.pid) {
                tree.postmaster = *member;
            }
        }
        if (!complete || tree.postmaster.pid != anchor.pid
            || tree.postmaster.start_token != anchor.start_token
            || tree.postmaster.executable != anchor.executable) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        auto second = sample_postmaster_process_tree(anchor.pid);
        if (!second) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        normalize_process_tree_snapshot(*second);
        if (*first == *second) {
            std::sort(
                tree.members.begin(), tree.members.end(),
                [](const supervised_process_tree_member& left,
                   const supervised_process_tree_member& right) {
                    return std::pair{
                               left.depth, left.process.pid}
                        < std::pair{
                               right.depth, right.process.pid};
                });
            return tree;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
} catch (...) {
    return std::nullopt;
}

/// @brief Signals the broker-owned postmaster group while its leader pins reuse.
[[nodiscard]] inline bool signal_owned_postmaster_group(
    const supervised_process_tree& tree,
    int signal_number,
    spawned_process* owned_group_leader = nullptr) noexcept {
    return owned_group_leader != nullptr
        && owned_group_leader->process_group()
            == tree.postmaster.process_group
        && owned_group_leader
            ->signal_owned_group_while_unreaped(signal_number);
}

[[nodiscard]] inline const supervised_process_tree_member*
find_tree_member(
    const supervised_process_tree& tree,
    std::uint64_t pid) noexcept {
    const auto member = std::find_if(
        tree.members.begin(), tree.members.end(),
        [pid](const supervised_process_tree_member& candidate) {
            return candidate.process.pid == pid;
        });
    return member == tree.members.end() ? nullptr : &*member;
}

inline void merge_tree_member(
    supervised_process_tree& tree,
    const supervised_process_tree_member& member) {
    const auto current = std::find_if(
        tree.members.begin(), tree.members.end(),
        [&member](const supervised_process_tree_member& candidate) {
            return same_process_identity(
                candidate.process, member.process);
        });
    if (current == tree.members.end()) {
        tree.members.push_back(member);
    } else {
        *current = member;
    }
    if (std::find(
            tree.process_groups.begin(), tree.process_groups.end(),
            member.process.process_group)
        == tree.process_groups.end()) {
        tree.process_groups.push_back(member.process.process_group);
    }
    std::sort(
        tree.members.begin(), tree.members.end(),
        [](const supervised_process_tree_member& left,
           const supervised_process_tree_member& right) {
            return std::pair{left.depth, left.process.pid}
                < std::pair{right.depth, right.process.pid};
        });
}

[[nodiscard]] inline bool same_captured_tree(
    const supervised_process_tree& left,
    const supervised_process_tree& right) noexcept {
    if (left.members.size() != right.members.size()) {
        return false;
    }
    for (const supervised_process_tree_member& member : left.members) {
        const auto other = std::find_if(
            right.members.begin(), right.members.end(),
            [&member](const supervised_process_tree_member& candidate) {
                return same_process_identity(
                           member.process, candidate.process)
                    && member.parent_pid == candidate.parent_pid
                    && member.depth == candidate.depth;
            });
        if (other == right.members.end()) {
            return false;
        }
    }
    return true;
}

template <typename DescendantStop>
[[nodiscard]] inline bool stop_process_tree_parent_first(
    supervised_process_tree& tree,
    DescendantStop&& stop_descendant) {
    for (supervised_process_tree_member& member : tree.members) {
        if (same_process_identity(
                member.process, tree.postmaster)) {
            continue;
        }
        const supervised_child_signal_result stopped =
            stop_descendant(member);
        if (stopped.outcome
            != supervised_child_signal_outcome::delivered) {
            return false;
        }
        if (stopped.identity_after) {
            member.process = *stopped.identity_after;
        }
    }
    return true;
}

template <typename DescendantResume, typename RootResume>
[[nodiscard]] inline bool resume_process_tree_child_first(
    const supervised_process_tree& tree,
    DescendantResume&& resume_descendant,
    RootResume&& resume_root) noexcept {
    for (auto iterator = tree.members.rbegin();
         iterator != tree.members.rend(); ++iterator) {
        if (same_process_identity(
                iterator->process, tree.postmaster)) {
            continue;
        }
        if (!resume_descendant(*iterator)) {
            return false;
        }
    }
    return resume_root();
}

/// @brief Resumes children before parents and the broker-owned group last.
[[nodiscard]] inline bool resume_process_tree(
    const supervised_process_tree& tree,
    spawned_process* owned_group_leader = nullptr) noexcept {
    const bool root_resumed =
        resume_process_tree_child_first(
            tree,
            [&tree](
                const supervised_process_tree_member& member) noexcept {
                const auto* parent =
                    find_tree_member(tree, member.parent_pid);
                if (parent == nullptr) {
                    return false;
                }
                if (supervised_process_definitely_absent(
                        member.process)
                    || supervised_child_is_terminal(
                        parent->process, member.process)) {
                    return true;
                }
                return signal_supervised_child_while_parent_stopped(
                           parent->process, member.process, SIGCONT)
                    .accepted();
            },
            [&tree, owned_group_leader]() noexcept {
                return signal_owned_postmaster_group(
                    tree, SIGCONT, owned_group_leader);
            });
    if (!root_resumed) {
        return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now()
        + std::chrono::milliseconds(200);
    while (process_identity_matches(tree.postmaster)
           && supervised_process_stopped(tree.postmaster)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !process_identity_matches(tree.postmaster)
        || !supervised_process_stopped(tree.postmaster);
}

[[nodiscard]] inline process_tree_freeze_result failed_freeze(
    supervised_process_tree tree,
    spawned_process* owned_group_leader) noexcept {
    const bool resumed =
        resume_process_tree(tree, owned_group_leader);
    return {
        .outcome = resumed
            ? process_tree_freeze_outcome::failed_resumed
            : process_tree_freeze_outcome::failed_resume_incomplete,
        .tree = std::move(tree)};
}

/// @brief Freezes a stable anchored tree before any force-cleanup decision.
[[nodiscard]] inline process_tree_freeze_result
freeze_process_tree(
    const supervised_process_record& anchor,
    spawned_process* owned_group_leader = nullptr) noexcept {
    supervised_process_tree frozen;
    try {
        frozen.postmaster = anchor;
        frozen.members.push_back(supervised_process_tree_member{
            .process = anchor, .parent_pid = 0U, .depth = 0U});
        frozen.process_groups.push_back(anchor.process_group);
        if (!signal_owned_postmaster_group(
                frozen, SIGSTOP, owned_group_leader)) {
            return failed_freeze(
                std::move(frozen), owned_group_leader);
        }
        const auto root_stop_deadline =
            std::chrono::steady_clock::now()
            + std::chrono::milliseconds(200);
        while (!supervised_process_stopped(anchor)
               && std::chrono::steady_clock::now()
                   < root_stop_deadline) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }
        if (!supervised_process_stopped(anchor)) {
            return failed_freeze(
                std::move(frozen), owned_group_leader);
        }

        for (int attempt = 0; attempt != 20; ++attempt) {
            auto tree = capture_process_tree(anchor);
            if (!tree) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5));
                continue;
            }
            const bool stopped =
                stop_process_tree_parent_first(
                    *tree,
                    [&tree, &frozen](
                        const supervised_process_tree_member& member) {
                        const auto* parent =
                            find_tree_member(
                                *tree, member.parent_pid);
                        const auto result = parent == nullptr
                            ? supervised_child_signal_result{}
                            : signal_supervised_child_while_parent_stopped(
                                  parent->process,
                                  member.process,
                                  SIGSTOP);
                        if (result.outcome
                            == supervised_child_signal_outcome::
                                delivered) {
                            supervised_process_tree_member
                                stopped_member = member;
                            if (result.identity_after) {
                                stopped_member.process =
                                    *result.identity_after;
                            }
                            merge_tree_member(
                                frozen, stopped_member);
                        }
                        return result;
                    });
            if (!stopped) {
                return failed_freeze(
                    std::move(frozen), owned_group_leader);
            }
            for (const supervised_process_tree_member& member :
                 tree->members) {
                merge_tree_member(frozen, member);
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
            const auto confirmation = capture_process_tree(anchor);
            if (!confirmation
                || !same_captured_tree(*tree, *confirmation)) {
                continue;
            }
            const bool all_stopped = std::all_of(
                confirmation->members.begin(),
                confirmation->members.end(),
                [](const supervised_process_tree_member& member) {
                    return supervised_process_stopped(
                        member.process);
                });
            if (all_stopped) {
                for (const auto& member : confirmation->members) {
                    merge_tree_member(frozen, member);
                }
                frozen.postmaster = confirmation->postmaster;
                return {
                    .outcome =
                        process_tree_freeze_outcome::frozen,
                    .tree = std::move(frozen)};
            }
        }
        return failed_freeze(
            std::move(frozen), owned_group_leader);
    } catch (...) {
        return failed_freeze(
            std::move(frozen), owned_group_leader);
    }
}

/// @brief Refuses historical captures that cannot prove a live fixed point.
[[nodiscard]] inline process_tree_freeze_result
freeze_captured_process_tree(
    const supervised_process_tree&,
    spawned_process* = nullptr) noexcept {
    return {
        .outcome = process_tree_freeze_outcome::failed_resumed,
        .tree = std::nullopt};
}

/// @brief True only after every exact captured identity is gone.
[[nodiscard]] inline bool process_tree_members_absent(
    const supervised_process_tree& tree) noexcept {
    return std::all_of(
        tree.members.begin(), tree.members.end(),
        [](const supervised_process_tree_member& member) {
            return !process_identity_matches(member.process);
        });
}

/// @brief True only after every captured identity and process group is gone.
[[nodiscard]] inline bool process_tree_absent(
    const supervised_process_tree& tree) noexcept {
    const bool members_absent =
        process_tree_members_absent(tree);
    const bool groups_absent = std::all_of(
        tree.process_groups.begin(), tree.process_groups.end(),
        [](std::int64_t group) {
            return supervised_process_group_absent(group);
        });
    return members_absent && groups_absent;
}

/// @brief Waits a bounded interval for every captured identity to vanish.
[[nodiscard]] inline bool wait_for_process_tree_members_absence(
    const supervised_process_tree& tree,
    std::chrono::milliseconds deadline) noexcept {
    const auto finish = std::chrono::steady_clock::now()
        + std::max(deadline, std::chrono::milliseconds::zero());
    while (std::chrono::steady_clock::now() < finish) {
        if (process_tree_members_absent(tree)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return process_tree_members_absent(tree);
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

template <typename DescendantKill, typename RootKill>
[[nodiscard]] inline process_tree_termination_result
terminate_process_tree_child_first(
    const supervised_process_tree& tree,
    DescendantKill&& kill_descendant,
    RootKill&& kill_root) noexcept {
    process_tree_termination_progress progress;
    for (auto iterator = tree.members.rbegin();
         iterator != tree.members.rend(); ++iterator) {
        if (same_process_identity(
                iterator->process, tree.postmaster)) {
            continue;
        }
        const supervised_child_signal_result killed =
            kill_descendant(*iterator);
        if (killed.delivered()) {
            progress.record_kill_delivery();
        } else if (!killed.accepted()) {
            progress.record_failure();
            return progress.finish(false, false);
        }
    }
    if (progress.failed()) {
        return progress.finish(true, false);
    }
    const bool root_group_signaled = kill_root();
    if (root_group_signaled) {
        progress.record_kill_delivery();
    } else {
        progress.record_failure();
    }
    return progress.finish(true, root_group_signaled);
}

/// @brief Crosses the commit boundary at first SIGKILL, then never rolls back.
[[nodiscard]] inline process_tree_termination_result
force_kill_process_tree(
    const supervised_process_tree& tree,
    spawned_process* owned_group_leader = nullptr) noexcept {
    process_tree_termination_progress progress;
    if (owned_group_leader == nullptr
        || owned_group_leader->process_group()
            != tree.postmaster.process_group
        || !process_identity_matches(tree.postmaster)
        || !supervised_process_stopped(tree.postmaster)) {
        progress.record_failure();
        return progress.finish(false, false);
    }
    for (const supervised_process_tree_member& member : tree.members) {
        if (same_process_identity(
                member.process, tree.postmaster)) {
            continue;
        }
        const auto* parent = find_tree_member(tree, member.parent_pid);
        if (parent == nullptr) {
            progress.record_failure();
            return progress.finish(false, false);
        }
        if (supervised_process_definitely_absent(member.process)
            || supervised_child_is_terminal(
                parent->process, member.process)) {
            continue;
        }
        if (!supervised_child_is_pinned(
                parent->process, member.process)) {
            progress.record_failure();
            return progress.finish(false, false);
        }
    }

    return terminate_process_tree_child_first(
        tree,
        [&tree](
            const supervised_process_tree_member& member) noexcept {
            const auto* parent =
                find_tree_member(tree, member.parent_pid);
            if (parent != nullptr
                && (supervised_process_definitely_absent(
                        member.process)
                    || supervised_child_is_terminal(
                        parent->process, member.process))) {
                return supervised_child_signal_result{
                    .outcome = supervised_child_signal_outcome::
                        target_absent};
            }
            return parent == nullptr
                ? supervised_child_signal_result{}
                : signal_supervised_child_while_parent_stopped(
                      parent->process, member.process, SIGKILL);
        },
        [&tree, owned_group_leader]() noexcept {
            return signal_owned_postmaster_group(
                tree, SIGKILL, owned_group_leader);
        });
}

}  // namespace bioseq::postgres_test::cluster_detail

export namespace bioseq::postgres_test {

/// @brief Exercises controller ordering/commit policy without sending signals.
///
/// The caller supplies a real complete topology snapshot; injected outcomes
/// then traverse the same `(depth, pid)` ordering and shared commit tracker as
/// the signalling controller.
[[nodiscard]] inline process_tree_controller_contract_trace
evaluate_process_tree_controller_contract(
    std::vector<postmaster_process_tree_member> topology,
    std::uint64_t root_pid,
    process_tree_controller_fault fault) {
    process_tree_controller_contract_trace trace;
    cluster_detail::normalize_process_tree_snapshot(topology);
    const auto depths =
        cluster_detail::process_tree_depths(topology, root_pid);
    if (!depths) {
        return trace;
    }
    struct ordered_member {
        std::uint64_t pid{};
        std::size_t depth{};
    };
    std::vector<ordered_member> ordered;
    ordered.reserve(topology.size());
    for (std::size_t index = 0; index < topology.size(); ++index) {
        ordered.push_back({
            .pid = topology[index].pid,
            .depth = (*depths)[index]});
    }
    std::sort(
        ordered.begin(), ordered.end(),
        [](const ordered_member& left,
           const ordered_member& right) {
            return std::pair{left.depth, left.pid}
                < std::pair{right.depth, right.pid};
        });
    if (ordered.empty() || ordered.front().pid != root_pid
        || ordered.front().depth != 0U) {
        return trace;
    }

    cluster_detail::supervised_process_tree contract_tree;
    contract_tree.postmaster = supervised_process_record{
        .pid = root_pid,
        .start_token = "controller-contract",
        .executable = "/controller-contract",
        .process_group = static_cast<std::int64_t>(root_pid)};
    for (std::size_t index = 0; index < topology.size(); ++index) {
        contract_tree.members.push_back(
            cluster_detail::supervised_process_tree_member{
                .process = supervised_process_record{
                    .pid = topology[index].pid,
                    .start_token = "controller-contract",
                    .executable = "/controller-contract",
                    .process_group = static_cast<std::int64_t>(
                        topology[index].pid)},
                .parent_pid = topology[index].parent_pid,
                .depth = (*depths)[index]});
    }
    std::sort(
        contract_tree.members.begin(),
        contract_tree.members.end(),
        [](const cluster_detail::supervised_process_tree_member& left,
           const cluster_detail::supervised_process_tree_member& right) {
            return std::pair{left.depth, left.process.pid}
                < std::pair{right.depth, right.process.pid};
        });

    trace.stop_order.push_back(root_pid);
    std::vector<std::uint64_t> stopped_descendants;
    std::size_t descendant_ordinal{};
    trace.freeze_succeeded =
        cluster_detail::stop_process_tree_parent_first(
            contract_tree,
            [&trace, &stopped_descendants,
             &descendant_ordinal, fault](
                const cluster_detail::supervised_process_tree_member&
                    member) {
                ++descendant_ordinal;
                const bool rejected =
                    (fault
                         == process_tree_controller_fault::
                             identity_drift_before_stop
                     && descendant_ordinal == 1U)
                    || (fault
                            == process_tree_controller_fault::
                                reject_second_descendant_stop
                        && descendant_ordinal == 2U);
                if (rejected) {
                    return supervised_child_signal_result{};
                }
                trace.stop_order.push_back(member.process.pid);
                stopped_descendants.push_back(member.process.pid);
                return supervised_child_signal_result{
                    .outcome =
                        supervised_child_signal_outcome::delivered};
            });
    if (!trace.freeze_succeeded) {
        trace.resume_attempted = true;
        cluster_detail::supervised_process_tree rollback_tree;
        rollback_tree.postmaster = contract_tree.postmaster;
        for (const auto& member : contract_tree.members) {
            if (member.process.pid == root_pid
                || std::find(
                       stopped_descendants.begin(),
                       stopped_descendants.end(),
                       member.process.pid)
                    != stopped_descendants.end()) {
                rollback_tree.members.push_back(member);
            }
        }
        trace.resume_complete =
            cluster_detail::resume_process_tree_child_first(
                rollback_tree,
                [&trace, &rollback_tree](
                    const cluster_detail::supervised_process_tree_member&
                        member) {
                    if (cluster_detail::find_tree_member(
                            rollback_tree, member.parent_pid)
                        == nullptr) {
                        return false;
                    }
                    trace.resume_order.push_back(
                        member.process.pid);
                    return true;
                },
                [&trace, root_pid] {
                    trace.resume_order.push_back(root_pid);
                    return true;
                });
        return trace;
    }

    std::size_t kill_ordinal{};
    trace.termination =
        cluster_detail::terminate_process_tree_child_first(
            contract_tree,
            [&trace, &kill_ordinal, fault](
                const cluster_detail::supervised_process_tree_member&
                    member) {
                ++kill_ordinal;
                if (fault
                        == process_tree_controller_fault::
                            reject_second_descendant_kill
                    && kill_ordinal == 2U) {
                    return supervised_child_signal_result{};
                }
                trace.kill_order.push_back(member.process.pid);
                return supervised_child_signal_result{
                    .outcome =
                        supervised_child_signal_outcome::delivered};
            },
            [&trace, root_pid, fault] {
                const bool signaled =
                    fault
                    != process_tree_controller_fault::
                        reject_root_group_kill;
                if (signaled) {
                    trace.kill_order.push_back(root_pid);
                }
                return signaled;
            });
    trace.rollback_attempted_after_commit = false;
    return trace;
}

/// @brief Renders version-portable settings for one staged private cluster.
/// @param port Private postmaster port paired with its Unix socket directory.
/// @param socket_directory Private Unix-domain socket directory.
/// @param runtime_library_directory Authorized per-case DSO directory, if any.
/// @param preload_libraries Validated shared-preload library names.
/// @param startup_gucs Additional validated startup settings.
/// @return Complete configuration text appended after initdb.
[[nodiscard]] inline std::string render_private_cluster_configuration(
    int port,
    const std::filesystem::path& socket_directory,
    const std::optional<std::filesystem::path>& runtime_library_directory,
    std::span<const std::string> preload_libraries,
    std::span<const std::pair<std::string, std::string>> startup_gucs) {
    const std::string dynamic_library_path =
        runtime_library_directory
        ? runtime_library_directory->string() + ":$libdir"
        : "$libdir";
    std::string configuration =
        "\n# Appended by bioseq.test.postgres\n"
        "listen_addresses = ''\n"
        "port = "
        + std::to_string(port)
        + "\nunix_socket_directories = "
        + cluster_detail::configuration_literal(socket_directory.string())
        + "\ndynamic_library_path = "
        + cluster_detail::configuration_literal(
            dynamic_library_path)
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

/// @brief Polls PGDATA until one stable executable-bound postmaster is proven.
[[nodiscard]] inline std::optional<supervised_process_record>
capture_supervised_postmaster_identity(
    const std::filesystem::path& expected_executable,
    const std::filesystem::path& data_directory,
    std::chrono::milliseconds deadline,
    std::chrono::milliseconds poll_interval =
        std::chrono::milliseconds{20}) noexcept {
    try {
        if (expected_executable.empty() || data_directory.empty()
            || deadline < std::chrono::milliseconds::zero()
            || poll_interval <= std::chrono::milliseconds::zero()) {
            return std::nullopt;
        }
        const auto finish =
            std::chrono::steady_clock::now() + deadline;
        for (;;) {
            const std::int64_t pid =
                cluster_detail::read_postmaster_pid(data_directory);
            if (pid > 1) {
                auto process = capture_supervised_process(
                    static_cast<std::uint64_t>(pid),
                    expected_executable, data_directory);
                if (process
                    && cluster_detail::read_postmaster_pid(
                           data_directory)
                        == pid
                    && supervised_postmaster_matches_data_directory(
                        *process)) {
                    return process;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= finish) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(
                std::min(
                    poll_interval,
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(finish - now)));
        }
    } catch (...) {
        return std::nullopt;
    }
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

    /// @brief Attests the suite stage, initializes PGDATA, and writes settings.
    void initialize() {
        if (cleanup_attempted_) {
            throw std::logic_error(
                "initialize cannot reuse a cleaned private cluster");
        }
        require_state(cluster_state::constructed, "initialize");
        std::filesystem::create_directories(data_directory_);
        std::filesystem::create_directories(socket_directory_);
        std::filesystem::create_directories(log_directory_);
        stage_ = attest_preinstalled_extension_stage(
            options_.preinstalled_stage_root,
            options_.preinstalled_stage_manifest,
            options_.postgres_bin_dir,
            root_ / "runtime-library",
            options_.extensions,
            options_.allowed_runtime_dso_overrides);
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
        const std::filesystem::path initdb_shared_directory =
            cluster_detail::require_initdb_shared_directory(*stage_);
        const auto init_exit = run_program(
            "initdb",
            {"--pgdata", data_directory_.string(),
             "-L", initdb_shared_directory.string(),
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
        postmaster_termination_verified_ = false;
        postmaster_cleanup_tree_.reset();
        postmaster_spawn_attempted_ = false;
        if (postmaster_process_) {
            throw std::logic_error(
                "start found an unreleased postmaster launch broker");
        }
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
            const std::filesystem::path postgres =
                program("postgres");
            postmaster_spawn_attempted_ = true;
            postmaster_process_.emplace(
                spawned_process::spawn(process_options{
                    .executable = postgres,
                    .arguments = {"-D", data_directory_.string()},
                    .environment = options_.process_environment,
                    .preferred_path_directory =
                        options_.postgres_bin_dir,
                    .output_file =
                        log_directory_ / "postgres.log",
                    .supervise = true,
                    .supervision_data_directory =
                        data_directory_}));
            const auto startup_finish =
                std::chrono::steady_clock::now()
                + options_.startup_timeout;
            std::string readiness_error;
            while (std::chrono::steady_clock::now()
                   < startup_finish) {
                if (postmaster_process_
                    ->leader_exited_unreaped()) {
                    throw process_error(
                        "postgres exited before readiness:\n"
                        + postmaster_process_->captured_output());
                }
                const auto remaining =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        startup_finish
                        - std::chrono::steady_clock::now());
                const auto candidate =
                    capture_supervised_postmaster_identity(
                        options_.postgres_bin_dir / "postgres",
                        data_directory_,
                        std::min(
                            remaining,
                            std::chrono::milliseconds{50}),
                        std::chrono::milliseconds{5});
                if (!candidate) {
                    continue;
                }
                if (candidate->process_group
                    != postmaster_process_->process_group()) {
                    postmaster_identity_ = candidate;
                    throw process_error(
                        "postmaster did not join its retained launch-broker "
                        "process group");
                }
                try {
                    static_cast<void>(connect(std::max(
                        std::chrono::milliseconds{1},
                        std::min(
                            remaining,
                            std::chrono::milliseconds{250}))));
                    postmaster_identity_ = candidate;
                    break;
                } catch (const std::exception& error) {
                    readiness_error = error.what();
                } catch (...) {
                    readiness_error =
                        "unknown Unix-socket readiness failure";
                }
            }
            if (!postmaster_identity_) {
                throw process_error(
                    "cannot establish identity-verified Unix-socket "
                    "postmaster readiness"
                    + (readiness_error.empty()
                           ? std::string{}
                           : ": " + readiness_error)
                    + "\n"
                    + postmaster_process_->captured_output());
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
        spawned_process* const owned_group_leader =
            owned_postmaster_group_leader();
        if (owned_group_leader == nullptr) {
            throw process_error(
                "retained postmaster launch broker is unavailable for crash");
        }
        auto frozen =
            cluster_detail::freeze_process_tree(
                *postmaster_identity_, owned_group_leader);
        if (!frozen.frozen()) {
            if (frozen.outcome
                    == cluster_detail::process_tree_freeze_outcome::
                        failed_resume_incomplete
                && frozen.tree) {
                postmaster_cleanup_tree_ =
                    std::move(*frozen.tree);
                postmaster_cleanup_requires_force_ = true;
                postmaster_cleanup_requires_refreeze_ = true;
            }
            throw process_error(
                "cannot freeze a stable anchored postmaster tree for crash");
        }
        postmaster_cleanup_tree_ =
            std::move(*frozen.tree);
        const auto termination =
            cluster_detail::force_kill_process_tree(
                *postmaster_cleanup_tree_, owned_group_leader);
        if (termination.outcome
            == process_tree_termination_outcome::precommit_failure) {
            const bool resumed =
                cluster_detail::resume_process_tree(
                    *postmaster_cleanup_tree_, owned_group_leader);
            if (resumed) {
                postmaster_cleanup_tree_.reset();
            } else {
                postmaster_cleanup_requires_force_ = true;
                postmaster_cleanup_requires_refreeze_ = true;
            }
            throw process_error(
                resumed
                ? "cannot SIGKILL frozen identity-verified postmaster tree"
                : "cannot SIGKILL or fully resume frozen "
                  "identity-verified postmaster tree");
        }
        if (!termination.root_group_signaled) {
            postmaster_cleanup_requires_force_ = true;
            postmaster_cleanup_requires_refreeze_ = false;
            throw process_error(
                "postmaster crash committed but frozen termination "
                "requires retry; PGDATA retained at "
                + data_directory_.string());
        }
        postmaster_cleanup_requires_force_ = false;
        postmaster_cleanup_requires_refreeze_ = false;
        if (!finalize_postmaster_termination(
                *postmaster_cleanup_tree_, options_.stop_timeout)) {
            throw process_error(
                "postmaster descendant or process group remains after "
                "identity-verified force cleanup; PGDATA retained at "
                + data_directory_.string());
        }
        publish_stopped_state();
    }

    /// @brief Stops the owned postmaster; never throws from cleanup paths.
    void stop() noexcept try {
        if (state_ != cluster_state::running) {
            return;
        }
        cleanup_error_.clear();
        if (postmaster_cleanup_tree_) {
            if (postmaster_cleanup_requires_force_) {
                spawned_process* const owned_group_leader =
                    owned_postmaster_group_leader();
                if (postmaster_cleanup_requires_refreeze_) {
                    if (!postmaster_identity_) {
                        cleanup_error_ =
                            "cannot recapture partially resumed postmaster "
                            "tree; PGDATA retained at "
                            + data_directory_.string();
                        return;
                    }
                    auto refrozen =
                        cluster_detail::freeze_process_tree(
                            *postmaster_identity_,
                            owned_group_leader);
                    if (!refrozen.frozen()) {
                        cleanup_error_ =
                            "cannot refreeze partially resumed postmaster "
                            "tree; PGDATA retained at "
                            + data_directory_.string();
                        return;
                    }
                    postmaster_cleanup_tree_ =
                        std::move(*refrozen.tree);
                    postmaster_cleanup_requires_refreeze_ = false;
                }
                const auto termination =
                    cluster_detail::force_kill_process_tree(
                        *postmaster_cleanup_tree_,
                        owned_group_leader);
                if (!termination.root_group_signaled) {
                    cleanup_error_ =
                        "committed postmaster termination retry remains "
                        "incomplete; PGDATA retained at "
                        + data_directory_.string();
                    return;
                }
                postmaster_cleanup_requires_force_ = false;
            }
            if (!cluster_detail::wait_for_process_tree_members_absence(
                    *postmaster_cleanup_tree_,
                    options_.stop_timeout)) {
                cleanup_error_ =
                    "signaled postmaster identities have not terminated; "
                    "PGDATA retained at "
                    + data_directory_.string();
                return;
            }
            const bool supervision_released =
                !postmaster_process_
                || postmaster_process_
                       ->reap_after_verified_target_termination(
                           options_.stop_timeout);
            const bool physically_absent =
                cluster_detail::wait_for_process_tree_absence(
                    *postmaster_cleanup_tree_,
                    options_.stop_timeout);
            postmaster_termination_verified_ =
                physically_absent;
            if (physically_absent && supervision_released) {
                publish_stopped_state();
            } else if (!physically_absent) {
                cleanup_error_ =
                    "terminated postmaster process group has not yet "
                    "disappeared; PGDATA retained at "
                    + data_directory_.string();
            } else {
                cleanup_error_ =
                    "physically terminated postmaster launch broker remains "
                    "in the supervision journal";
            }
            return;
        }
        if (postmaster_termination_verified_) {
            const bool supervision_released =
                !postmaster_process_
                || postmaster_process_
                       ->reap_after_verified_target_termination(
                           std::chrono::milliseconds::zero());
            if (supervision_released) {
                publish_stopped_state();
            } else {
                cleanup_error_ =
                    "physically terminated postmaster launch broker remains "
                    "in the supervision journal";
            }
            return;
        }
        spawned_process* const owned_group_leader =
            owned_postmaster_group_leader();
        if (!postmaster_identity_
            && causally_trusted_unanchored_start_) {
            postmaster_identity_ =
                capture_supervised_postmaster_identity(
                    options_.postgres_bin_dir / "postgres",
                    data_directory_, std::chrono::milliseconds{400});
            if (postmaster_identity_) {
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
            }
        }
        if (!postmaster_identity_) {
            if (owned_group_leader == nullptr
                && !postmaster_spawn_attempted_) {
                postmaster_termination_verified_ = true;
                publish_stopped_state();
                return;
            }
            if (owned_group_leader == nullptr
                || owned_group_leader->process_group() <= 1) {
                cleanup_error_ =
                    "started postmaster identity and retained launch broker "
                    "are unavailable after trusted launch recovery; PGDATA "
                    "retained at "
                    + data_directory_.string();
                return;
            }
            cluster_detail::supervised_process_tree owned_tree;
            owned_tree.process_groups.push_back(
                owned_group_leader->process_group());
            if (!owned_group_leader
                     ->signal_owned_group_while_unreaped(SIGKILL)
                || !finalize_postmaster_termination(
                    owned_tree, options_.stop_timeout)) {
                cleanup_error_ =
                    "cannot terminate the causally owned postmaster launch "
                    "group after startup identity recovery failed; PGDATA "
                    "retained at "
                    + data_directory_.string();
                return;
            }
            publish_stopped_state();
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

        const bool members_absent =
            cluster_detail::wait_for_process_tree_members_absence(
            *initial_tree, options_.stop_timeout);
        if (members_absent
            && finalize_postmaster_termination(
                *initial_tree, options_.stop_timeout)) {
            publish_stopped_state();
            return;
        }
        if (postmaster_termination_verified_) {
            if (cleanup_error_.empty()) {
                cleanup_error_ =
                    "postmaster ended but its launch-broker supervision "
                    "journal entry remains dirty";
            }
            return;
        }

        cluster_detail::process_tree_freeze_result force_tree;
        if (cluster_detail::process_identity_matches(
                *postmaster_identity_)) {
            force_tree = cluster_detail::freeze_process_tree(
                *postmaster_identity_, owned_group_leader);
        } else {
            force_tree =
                cluster_detail::freeze_captured_process_tree(
                    *initial_tree, owned_group_leader);
        }
        if (force_tree.outcome
                == cluster_detail::process_tree_freeze_outcome::
                    failed_resume_incomplete
            && force_tree.tree) {
            postmaster_cleanup_tree_ =
                std::move(*force_tree.tree);
            postmaster_cleanup_requires_force_ = true;
            postmaster_cleanup_requires_refreeze_ = true;
        }
        if (force_tree.frozen()) {
            postmaster_cleanup_tree_ =
                std::move(*force_tree.tree);
            const auto termination =
                cluster_detail::force_kill_process_tree(
                    *postmaster_cleanup_tree_,
                    owned_group_leader);
            if (termination.outcome
                == process_tree_termination_outcome::
                    precommit_failure) {
                const bool resumed =
                    cluster_detail::resume_process_tree(
                        *postmaster_cleanup_tree_,
                        owned_group_leader);
                if (resumed) {
                    postmaster_cleanup_tree_.reset();
                } else {
                    postmaster_cleanup_requires_force_ = true;
                    postmaster_cleanup_requires_refreeze_ = true;
                }
                set_cleanup_error_noexcept(
                    resumed
                    ? "cannot SIGKILL identity-verified postmaster tree"
                    : "cannot SIGKILL or fully resume "
                      "identity-verified postmaster tree");
            } else if (!termination.root_group_signaled) {
                postmaster_cleanup_requires_force_ = true;
                postmaster_cleanup_requires_refreeze_ = false;
                set_cleanup_error_noexcept(
                    "committed postmaster termination requires retry");
            } else if (finalize_postmaster_termination(
                           *postmaster_cleanup_tree_,
                           options_.stop_timeout)) {
                publish_stopped_state();
                return;
            }
        }
        cleanup_error_ +=
            (cleanup_error_.empty() ? "" : "; ")
            + std::string(
                "postmaster descendant or process group remains live; "
                "PGDATA retained at ")
            + data_directory_.string();
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
    [[nodiscard]] spawned_process*
    owned_postmaster_group_leader() noexcept {
        return postmaster_process_
            ? &*postmaster_process_ : nullptr;
    }

    /// @brief Reaps the retained broker only after its target tree is gone.
    [[nodiscard]] bool finalize_postmaster_termination(
        const cluster_detail::supervised_process_tree& tree,
        std::chrono::milliseconds deadline) noexcept {
        try {
            postmaster_cleanup_tree_ = tree;
        } catch (...) {
            set_cleanup_error_noexcept(
                "cannot retain the verified postmaster cleanup identity tree");
            return false;
        }
        const auto finish = std::chrono::steady_clock::now()
            + std::max(deadline, std::chrono::milliseconds::zero());
        const auto remaining = [&finish] {
            return std::max(
                std::chrono::milliseconds::zero(),
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    finish - std::chrono::steady_clock::now()));
        };
        if (!cluster_detail::wait_for_process_tree_members_absence(
                tree, remaining())) {
            return false;
        }
        bool supervision_released = true;
        if (postmaster_process_) {
            supervision_released =
                postmaster_process_
                    ->reap_after_verified_target_termination(
                        remaining());
        }
        const bool physically_absent =
            cluster_detail::wait_for_process_tree_absence(
                tree, remaining());
        if (physically_absent) {
            postmaster_termination_verified_ = true;
        }
        if (!supervision_released) {
            set_cleanup_error_noexcept(
                "postmaster launch broker ended but its supervision "
                "journal entry could not be removed");
        }
        return physically_absent && supervision_released;
    }

    /// @brief Durably publishes the physically verified stopped state.
    void publish_stopped_state() {
        std::error_code pidfile_error;
        std::filesystem::remove(
            data_directory_ / "postmaster.pid", pidfile_error);
        if (pidfile_error) {
            throw process_error(
                "cannot remove the verified-dead postmaster pidfile: "
                + pidfile_error.message());
        }
        register_supervised_cluster(supervised_cluster_record{
            .root = root_,
            .data_directory = data_directory_,
            .postmaster_executable =
                options_.postgres_bin_dir / "postgres"});
        state_ = cluster_state::stopped;
        postmaster_identity_.reset();
        postmaster_process_.reset();
        postmaster_cleanup_tree_.reset();
        postmaster_cleanup_requires_force_ = false;
        postmaster_cleanup_requires_refreeze_ = false;
        postmaster_termination_verified_ = false;
        postmaster_spawn_attempted_ = false;
        causally_trusted_unanchored_start_ = false;
        cleanup_error_.clear();
    }

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
            port_, socket_directory_, stage_->runtime_library_directory,
            options_.preload_libraries, options_.startup_gucs);
        configuration.close();
        if (!configuration) {
            throw std::runtime_error("failed writing private postgresql.conf");
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
    std::optional<spawned_process> postmaster_process_;
    std::optional<cluster_detail::supervised_process_tree>
        postmaster_cleanup_tree_;
    int port_{};
    std::string cleanup_error_;
    bool cleanup_attempted_{};
    bool causally_trusted_unanchored_start_{};
    bool postmaster_termination_verified_{};
    bool postmaster_spawn_attempted_{};
    bool postmaster_cleanup_requires_force_{};
    bool postmaster_cleanup_requires_refreeze_{};
    cluster_cleanup_result cleanup_result_;
};

}  // namespace bioseq::postgres_test
