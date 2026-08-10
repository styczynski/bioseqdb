module;

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

export module bioseq.test.postgres:supervision;

import bioseq.common.durable_file_io;

export namespace bioseq::postgres_test {

/// @brief Process identity stable across PID reuse and safe to signal.
struct supervised_process_identity {
    std::uint64_t pid{};
    std::string start_token;
    std::filesystem::path executable;
    std::int64_t process_group{};
    std::filesystem::path data_directory;
};

/// @brief Result of signalling a child whose exact parent is kernel-stopped.
enum class supervised_child_signal_outcome {
    delivered,
    target_absent,
    rejected,
    delivered_unverified,
};

/// @brief Identity evidence refreshed immediately after a pinned child signal.
struct supervised_child_signal_result {
    supervised_child_signal_outcome outcome{
        supervised_child_signal_outcome::rejected};
    std::optional<supervised_process_identity> identity_after;

    [[nodiscard]] bool is_accepted() const noexcept {
        return outcome == supervised_child_signal_outcome::delivered
            || outcome
                == supervised_child_signal_outcome::target_absent;
    }

    [[nodiscard]] bool is_delivered() const noexcept {
        return outcome == supervised_child_signal_outcome::delivered
            || outcome
                == supervised_child_signal_outcome::delivered_unverified;
    }
};

/// @brief Commit-aware terminal state of a frozen process-tree kill.
enum class process_tree_termination_outcome {
    precommit_failure,
    committed_complete,
    committed_incomplete,
};

/// @brief Evidence returned after a frozen-tree termination attempt.
struct process_tree_termination_result {
    process_tree_termination_outcome outcome{
        process_tree_termination_outcome::precommit_failure};
    std::size_t delivered_kills{};
    bool root_group_signaled{};

    [[nodiscard]] bool is_committed() const noexcept {
        return outcome
            != process_tree_termination_outcome::precommit_failure;
    }
};

/// @brief Tracks the irreversible boundary at the first delivered SIGKILL.
class process_tree_termination_progress {
public:
    void record_kill_delivery() noexcept {
        ++delivered_kills_;
    }

    void record_failure() noexcept {
        failed_ = true;
    }

    [[nodiscard]] std::size_t delivered_kills() const noexcept {
        return delivered_kills_;
    }

    [[nodiscard]] bool has_failed() const noexcept {
        return failed_;
    }

    [[nodiscard]] process_tree_termination_result finish(
        bool traversal_complete,
        bool root_group_signaled) const noexcept {
        process_tree_termination_outcome result =
            process_tree_termination_outcome::precommit_failure;
        if (delivered_kills_ == 0U) {
            result =
                process_tree_termination_outcome::precommit_failure;
        } else if (!failed_ && traversal_complete
                   && root_group_signaled) {
            result = process_tree_termination_outcome::
                committed_complete;
        } else {
            result = process_tree_termination_outcome::
                committed_incomplete;
        }
        return {
            .outcome = result,
            .delivered_kills = delivered_kills_,
            .root_group_signaled = root_group_signaled};
    }

private:
    std::size_t delivered_kills_{};
    bool failed_{};
};

/// @brief One cluster identity persisted for an out-of-process watchdog.
struct supervised_cluster_identity {
    std::filesystem::path root;
    std::filesystem::path data_directory;
    std::uint64_t postmaster_pid{};
    bool discover_postmaster_pid{};
    std::string postmaster_start_token;
    std::filesystem::path postmaster_executable;
    std::int64_t postmaster_process_group{};
};

/// @brief Test-only fault stage for fail-closed registry contracts.
enum class supervision_journal_fault {
    none,
    create_staging_file,
    write_staging_file,
    publish_destination,
    fsync_staging_file,
    close_staging_file,
    fsync_directory,
    registry_clean_transition_fsync_directory,
    decision_rollback_remove,
    decision_reconciliation_fsync_directory,
};

/// @brief One generation-consistent live-resource registry snapshot.
struct supervision_registry_snapshot {
    std::uint64_t generation{};
    std::vector<supervised_cluster_identity> clusters;
    std::vector<supervised_process_identity> processes;
};

/// @brief Opaque durable intent created before a supervised posix_spawn.
struct supervision_launch_intent {
    std::uint64_t id{};
    bool supervision_required{};
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::supervision_detail {

namespace durable_io = bioseq::common::durable_file_io;

struct supervision_state {
    std::mutex mutex;
    std::optional<std::filesystem::path> directory;
    std::vector<supervised_cluster_identity> clusters;
    std::vector<supervised_process_identity> processes;
    std::uint64_t generation{};
    std::uint64_t next_launch_id{};
    std::vector<std::uint64_t> pending_process_launches;
};

[[nodiscard]] inline supervision_state& process_state() {
    static supervision_state state;
    return state;
}

[[nodiscard]] inline std::mutex& snapshot_publish_mutex() {
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] inline std::atomic<supervision_journal_fault>&
journal_fault() {
    static std::atomic<supervision_journal_fault> fault{
        supervision_journal_fault::none};
    return fault;
}

/// @brief One synchronous durable-publication fault-injection context.
struct journal_fault_context {
    supervision_journal_fault fault{supervision_journal_fault::none};
    bool delay_directory_sync{};
};

enum class directory_fsync_checkpoint {
    ordinary,
    registry_clean_transition,
    decision_reconciliation,
};

/**
 * @brief Maps supervision fault stages onto canonical durable-file operations.
 *
 * The callback injects before the matching durable-file operation. Decision
 * directory-fsync failures retain their observable publication window so the
 * cross-process reader contract continues to exercise rollback under flock.
 */
[[nodiscard]] inline int inject_journal_fault(
    void* opaque,
    durable_io::file_operation operation) noexcept {
    const auto& context =
        *static_cast<const journal_fault_context*>(opaque);
    bool matches = false;
    switch (context.fault) {
        case supervision_journal_fault::none:
            break;
        case supervision_journal_fault::create_staging_file:
            matches =
                operation == durable_io::file_operation::create_staging_file;
            break;
        case supervision_journal_fault::write_staging_file:
            matches =
                operation == durable_io::file_operation::positional_write;
            break;
        case supervision_journal_fault::publish_destination:
            matches =
                operation == durable_io::file_operation::rename_destination
                || operation == durable_io::file_operation::link_destination;
            break;
        case supervision_journal_fault::fsync_staging_file:
            matches =
                operation == durable_io::file_operation::fsync_file;
            break;
        case supervision_journal_fault::close_staging_file:
            matches =
                operation == durable_io::file_operation::close_file;
            break;
        case supervision_journal_fault::fsync_directory:
            matches =
                operation == durable_io::file_operation::fsync_directory;
            if (matches && context.delay_directory_sync) {
                try {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(200));
                } catch (...) {
                }
            }
            break;
        case supervision_journal_fault::
                registry_clean_transition_fsync_directory:
            break;
        case supervision_journal_fault::decision_rollback_remove:
        case supervision_journal_fault::
                decision_reconciliation_fsync_directory:
            matches =
                operation == durable_io::file_operation::fsync_directory;
            if (matches && context.delay_directory_sync) {
                try {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(200));
                } catch (...) {
                }
            }
            break;
    }
    return matches ? EIO : 0;
}

/// @brief Returns the byte view consumed by canonical durable publications.
[[nodiscard]] inline std::span<const std::byte> payload_bytes(
    std::string_view payload) noexcept {
    return std::as_bytes(
        std::span<const char>{payload.data(), payload.size()});
}

/// @brief Reads one bounded regular file without following its final symlink.
[[nodiscard]] inline std::optional<std::string> read_stable_text(
    const std::filesystem::path& path,
    std::uint64_t minimum_bytes,
    std::uint64_t maximum_bytes) noexcept {
    try {
        const auto file = durable_io::read_stable_regular_file(
            path,
            durable_io::stable_read_options{
                .minimum_bytes = minimum_bytes,
                .maximum_bytes = maximum_bytes});
        if (!file) {
            return std::nullopt;
        }
        std::string payload(file->bytes.size(), '\0');
        if (!file->bytes.empty()) {
            std::memcpy(
                payload.data(), file->bytes.data(), file->bytes.size());
        }
        return payload;
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Validates the exact marker format through a stable no-follow read.
[[nodiscard]] inline bool is_valid_watchdog_state_marker(
    const std::filesystem::path& path) noexcept {
    const auto payload = read_stable_text(path, 2U, 32U);
    if (!payload || payload->back() != '\n'
        || payload->find('\n') != payload->size() - 1U) {
        return false;
    }
    const std::string_view digits{
        payload->data(), payload->size() - 1U};
    std::uint64_t owner_pid{};
    const auto [end, error] = std::from_chars(
        digits.data(), digits.data() + digits.size(), owner_pid);
    return error == std::errc{}
        && end == digits.data() + digits.size()
        && owner_pid > 0U;
}

/// @brief Checks a directory entry without following its final symlink.
[[nodiscard]] inline std::optional<bool> is_path_entry_present_no_follow(
    const std::filesystem::path& path) noexcept {
    try {
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory
            || (!error
                && status.type()
                    == std::filesystem::file_type::not_found)) {
            return false;
        }
        if (error
            || status.type() == std::filesystem::file_type::none) {
            return std::nullopt;
        }
        return true;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] inline std::string hex_encode(std::string_view value) {
    constexpr std::string_view k_digits{"0123456789abcdef"};
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const unsigned char byte : value) {
        encoded.push_back(k_digits[(byte >> 4U) & 0x0fU]);
        encoded.push_back(k_digits[byte & 0x0fU]);
    }
    return encoded;
}

[[nodiscard]] inline std::optional<std::string> hex_decode(
    std::string_view value) {
    if (value.size() % 2U != 0) {
        return std::nullopt;
    }
    const auto nibble = [](char character) -> std::optional<unsigned char> {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned char>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<unsigned char>(character - 'a' + 10);
        }
        if (character >= 'A' && character <= 'F') {
            return static_cast<unsigned char>(character - 'A' + 10);
        }
        return std::nullopt;
    };
    std::string decoded;
    decoded.reserve(value.size() / 2U);
    for (std::size_t index = 0; index < value.size(); index += 2U) {
        const auto high = nibble(value[index]);
        const auto low = nibble(value[index + 1U]);
        if (!high || !low) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<char>((*high << 4U) | *low));
    }
    return decoded;
}

[[nodiscard]] std::optional<std::string> process_start_token(
    std::uint64_t pid) {
#if defined(__linux__)
    std::ifstream input(
        std::filesystem::path("/proc") / std::to_string(pid) / "stat");
    std::string line;
    if (!input || !std::getline(input, line)) {
        return std::nullopt;
    }
    const std::size_t command_end = line.rfind(')');
    if (command_end == std::string::npos
        || command_end + 2U >= line.size()) {
        return std::nullopt;
    }
    std::istringstream fields(line.substr(command_end + 2U));
    std::string value;
    for (int field = 3; field <= 21; ++field) {
        fields >> value;
    }
    fields >> value;
    if (!fields || value.empty()) {
        return std::nullopt;
    }
    return value;
#elif defined(__APPLE__)
    proc_bsdinfo information{};
    if (::proc_pidinfo(
            static_cast<int>(pid), PROC_PIDTBSDINFO, 0,
            &information, sizeof(information))
        != static_cast<int>(sizeof(information))) {
        return std::nullopt;
    }
    return std::to_string(information.pbi_start_tvsec) + ":"
        + std::to_string(information.pbi_start_tvusec);
#else
    static_cast<void>(pid);
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<std::uint64_t> process_parent_pid(
    std::uint64_t pid) {
#if defined(__linux__)
    std::ifstream input(
        std::filesystem::path("/proc") / std::to_string(pid) / "stat");
    std::string line;
    if (!input || !std::getline(input, line)) {
        return std::nullopt;
    }
    const std::size_t command_end = line.rfind(')');
    if (command_end == std::string::npos
        || command_end + 2U >= line.size()) {
        return std::nullopt;
    }
    std::istringstream fields(line.substr(command_end + 2U));
    char state{};
    std::uint64_t parent_pid{};
    fields >> state >> parent_pid;
    if (!fields || parent_pid <= 1U) {
        return std::nullopt;
    }
    return parent_pid;
#elif defined(__APPLE__)
    proc_bsdinfo information{};
    if (::proc_pidinfo(
            static_cast<int>(pid), PROC_PIDTBSDINFO, 0,
            &information, sizeof(information))
        != static_cast<int>(sizeof(information))
        || information.pbi_ppid <= 1) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(information.pbi_ppid);
#else
    static_cast<void>(pid);
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<bool> is_process_zombie(
    std::uint64_t pid) {
#if defined(__linux__)
    std::ifstream input(
        std::filesystem::path("/proc") / std::to_string(pid) / "stat");
    std::string line;
    if (!input || !std::getline(input, line)) {
        return std::nullopt;
    }
    const std::size_t command_end = line.rfind(')');
    if (command_end == std::string::npos
        || command_end + 2U >= line.size()) {
        return std::nullopt;
    }
    return line[command_end + 2U] == 'Z';
#elif defined(__APPLE__)
    proc_bsdinfo information{};
    if (::proc_pidinfo(
            static_cast<int>(pid), PROC_PIDTBSDINFO, 0,
            &information, sizeof(information))
        != static_cast<int>(sizeof(information))) {
        return std::nullopt;
    }
    return information.pbi_status == SZOMB;
#else
    static_cast<void>(pid);
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<std::filesystem::path> process_executable(
    std::uint64_t pid) {
    try {
#if defined(__linux__)
        return std::filesystem::canonical(
            std::filesystem::path("/proc") / std::to_string(pid) / "exe");
#elif defined(__APPLE__)
        std::vector<char> path(PROC_PIDPATHINFO_MAXSIZE);
        const int length = ::proc_pidpath(
            static_cast<int>(pid), path.data(),
            static_cast<std::uint32_t>(path.size()));
        if (length <= 0) {
            return std::nullopt;
        }
        return std::filesystem::canonical(path.data());
#else
        static_cast<void>(pid);
        return std::nullopt;
#endif
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::filesystem::path> canonical_expected(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    try {
        return std::filesystem::canonical(path);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::vector<std::string>> process_arguments(
    std::uint64_t pid) {
    try {
#if defined(__linux__)
        std::ifstream input(
            std::filesystem::path("/proc") / std::to_string(pid)
                / "cmdline",
            std::ios::binary);
        if (!input) {
            return std::nullopt;
        }
        const std::string payload{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        if (input.bad() || payload.size() > 1024U * 1024U) {
            return std::nullopt;
        }
        std::vector<std::string> arguments;
        std::size_t begin = 0;
        while (begin < payload.size()) {
            const std::size_t end = payload.find('\0', begin);
            if (end == std::string::npos) {
                return std::nullopt;
            }
            arguments.emplace_back(
                payload.substr(begin, end - begin));
            begin = end + 1U;
        }
        return arguments;
#elif defined(__APPLE__)
        if (pid > static_cast<std::uint64_t>(
                std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        int query[] = {
            CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
        std::size_t size{};
        if (::sysctl(query, 3, nullptr, &size, nullptr, 0) != 0
            || size < sizeof(int) || size > 1024U * 1024U) {
            return std::nullopt;
        }
        std::vector<char> payload(size);
        if (::sysctl(
                query, 3, payload.data(), &size, nullptr, 0)
            != 0
            || size < sizeof(int)) {
            return std::nullopt;
        }
        int argument_count{};
        std::memcpy(
            &argument_count, payload.data(), sizeof(argument_count));
        if (argument_count <= 0 || argument_count > 4096) {
            return std::nullopt;
        }
        std::size_t cursor = sizeof(argument_count);
        while (cursor < size && payload[cursor] != '\0') {
            ++cursor;
        }
        while (cursor < size && payload[cursor] == '\0') {
            ++cursor;
        }
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argument_count));
        for (int index = 0; index < argument_count; ++index) {
            const std::size_t begin = cursor;
            while (cursor < size && payload[cursor] != '\0') {
                ++cursor;
            }
            if (cursor >= size) {
                return std::nullopt;
            }
            arguments.emplace_back(
                payload.data() + begin, cursor - begin);
            while (cursor < size && payload[cursor] == '\0') {
                ++cursor;
            }
        }
        return arguments;
#else
        static_cast<void>(pid);
        return std::nullopt;
#endif
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool is_process_using_data_directory(
    std::uint64_t pid,
    const std::filesystem::path& data_directory) {
    const auto arguments = process_arguments(pid);
    const auto canonical_data = canonical_expected(data_directory);
    if (!arguments || !canonical_data) {
        return false;
    }
    for (std::size_t index = 0; index < arguments->size(); ++index) {
        std::optional<std::filesystem::path> supplied;
        if ((*arguments)[index] == "-D"
            && index + 1U < arguments->size()) {
            supplied = (*arguments)[index + 1U];
        } else if ((*arguments)[index].starts_with("-D")
                   && (*arguments)[index].size() > 2U) {
            supplied = (*arguments)[index].substr(2U);
        }
        if (supplied) {
            const auto canonical_supplied =
                canonical_expected(*supplied);
            return canonical_supplied
                && *canonical_supplied == *canonical_data;
        }
    }
    return false;
}

[[nodiscard]] inline bool fsync_directory(
    const std::filesystem::path& directory,
    directory_fsync_checkpoint checkpoint =
        directory_fsync_checkpoint::ordinary) noexcept {
    try {
        const supervision_journal_fault configured_fault =
            journal_fault().load(std::memory_order_relaxed);
        const bool inject =
            (checkpoint
                    == directory_fsync_checkpoint::
                        registry_clean_transition
                && configured_fault
                    == supervision_journal_fault::
                        registry_clean_transition_fsync_directory)
            || (checkpoint
                    == directory_fsync_checkpoint::
                        decision_reconciliation
                && configured_fault
                    == supervision_journal_fault::
                        decision_reconciliation_fsync_directory);
        journal_fault_context context{
            .fault = inject
                ? supervision_journal_fault::fsync_directory
                : supervision_journal_fault::none};
        const durable_io::fault_injection_hooks hooks{
            .context = &context,
            .before_operation = inject_journal_fault};
        const auto synced =
            durable_io::fsync_directory(directory, hooks);
        return synced.has_value();
    } catch (...) {
        return false;
    }
}

class decision_lock_guard {
public:
    explicit decision_lock_guard(int descriptor) noexcept
        : descriptor_(descriptor) {}

    decision_lock_guard(const decision_lock_guard&) = delete;
    decision_lock_guard& operator=(const decision_lock_guard&) = delete;

    decision_lock_guard(decision_lock_guard&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    ~decision_lock_guard() noexcept {
        if (descriptor_ >= 0) {
            static_cast<void>(::flock(descriptor_, LOCK_UN));
            static_cast<void>(::close(descriptor_));
        }
    }

private:
    int descriptor_{};
};

/// @brief Direct-child member that prevents one owned PGID from being reused.
class process_group_pin {
public:
    process_group_pin(pid_t child, int release_descriptor) noexcept
        : child_(child), release_descriptor_(release_descriptor) {}

    process_group_pin(const process_group_pin&) = delete;
    process_group_pin& operator=(const process_group_pin&) = delete;

    process_group_pin(process_group_pin&& other) noexcept
        : child_(std::exchange(other.child_, -1)),
          release_descriptor_(
              std::exchange(other.release_descriptor_, -1)) {}

    process_group_pin& operator=(process_group_pin&&) = delete;

    ~process_group_pin() noexcept {
        release_and_reap();
    }

    /// @brief Resumes the sentinel after a group-wide stop before releasing it.
    void resume_after_group_stop() noexcept {
        if (child_ > 1) {
            static_cast<void>(::kill(child_, SIGCONT));
        }
    }

private:
    void release_and_reap() noexcept {
        if (release_descriptor_ >= 0) {
            static_cast<void>(::close(release_descriptor_));
            release_descriptor_ = -1;
        }
        if (child_ <= 1) {
            return;
        }
        static_cast<void>(::kill(child_, SIGCONT));
        int status{};
        pid_t waited{};
        for (int attempt = 0; attempt != 100; ++attempt) {
            do {
                waited = ::waitpid(child_, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited == child_ || (waited < 0 && errno == ECHILD)) {
                child_ = -1;
                return;
            }
            if (waited < 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        static_cast<void>(::kill(child_, SIGCONT));
        static_cast<void>(::kill(child_, SIGKILL));
        do {
            waited = ::waitpid(child_, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0 && errno != ECHILD) {
            child_ = -1;
            return;
        }
        child_ = -1;
    }

    pid_t child_{-1};
    int release_descriptor_{-1};
};

/// @brief Joins a direct child to an existing same-session group and holds it.
[[nodiscard]] inline std::optional<process_group_pin> pin_process_group(
    std::int64_t process_group) noexcept {
    if (process_group <= 1
        || process_group
            > static_cast<std::int64_t>(
                std::numeric_limits<pid_t>::max())
        || process_group == static_cast<std::int64_t>(::getpgrp())) {
        return std::nullopt;
    }
    int ready[2]{-1, -1};
    int release[2]{-1, -1};
    if (::pipe(ready) != 0 || ::pipe(release) != 0) {
        if (ready[0] >= 0) {
            static_cast<void>(::close(ready[0]));
            static_cast<void>(::close(ready[1]));
        }
        if (release[0] >= 0) {
            static_cast<void>(::close(release[0]));
            static_cast<void>(::close(release[1]));
        }
        return std::nullopt;
    }
    for (const int descriptor :
         {ready[0], ready[1], release[0], release[1]}) {
        const int flags = ::fcntl(descriptor, F_GETFD);
        if (flags < 0
            || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
            static_cast<void>(::close(ready[0]));
            static_cast<void>(::close(ready[1]));
            static_cast<void>(::close(release[0]));
            static_cast<void>(::close(release[1]));
            return std::nullopt;
        }
    }

    const pid_t child = ::fork();
    if (child < 0) {
        static_cast<void>(::close(ready[0]));
        static_cast<void>(::close(ready[1]));
        static_cast<void>(::close(release[0]));
        static_cast<void>(::close(release[1]));
        return std::nullopt;
    }
    if (child == 0) {
        static_cast<void>(::close(ready[0]));
        static_cast<void>(::close(release[1]));
        const char result =
            ::setpgid(0, static_cast<pid_t>(process_group)) == 0
            ? 'R' : 'E';
        ssize_t written{};
        do {
            written = ::write(ready[1], &result, sizeof(result));
        } while (written < 0 && errno == EINTR);
        static_cast<void>(::close(ready[1]));
        if (result != 'R' || written != 1) {
            ::_exit(125);
        }
        char ignored{};
        ssize_t consumed{};
        do {
            consumed = ::read(release[0], &ignored, sizeof(ignored));
        } while (consumed < 0 && errno == EINTR);
        static_cast<void>(::close(release[0]));
        ::_exit(consumed < 0 ? 126 : 0);
    }

    static_cast<void>(::close(ready[1]));
    static_cast<void>(::close(release[0]));
    char result{};
    ssize_t consumed{};
    do {
        consumed = ::read(ready[0], &result, sizeof(result));
    } while (consumed < 0 && errno == EINTR);
    static_cast<void>(::close(ready[0]));
    if (consumed != 1 || result != 'R'
        || ::getpgid(child) != static_cast<pid_t>(process_group)) {
        static_cast<void>(::close(release[1]));
        int status{};
        pid_t waited{};
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        return std::nullopt;
    }
    return process_group_pin{child, release[1]};
}

[[nodiscard]] inline std::optional<decision_lock_guard>
acquire_decision_lock(
    const std::filesystem::path& directory,
    int operation) noexcept {
    const int descriptor = ::open(
        (directory / ".decision.lock").c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return std::nullopt;
    }
    int status{};
    do {
        status = ::flock(descriptor, operation);
    } while (status != 0 && errno == EINTR);
    if (status != 0) {
        static_cast<void>(::close(descriptor));
        return std::nullopt;
    }
    return decision_lock_guard{descriptor};
}

[[nodiscard]] inline bool is_valid_decision_file(
    const std::filesystem::path& path) noexcept {
    try {
        const auto payload = read_stable_text(path, 1U, 1024U);
        constexpr std::string_view k_prefix{
            "bioseq-supervision-decision-v1\t"};
        if (!payload || !payload->starts_with(k_prefix)
            || payload->back() != '\n'
            || payload->find('\n') != payload->size() - 1U) {
            return false;
        }
        const std::string_view encoded(
            payload->data() + k_prefix.size(),
            payload->size() - k_prefix.size() - 1U);
        const auto owner = hex_decode(encoded);
        return owner && !owner->empty() && owner->size() <= 256U;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] inline bool write_durable_file(
    const std::filesystem::path& path,
    std::string_view payload,
    bool inject_fault) noexcept {
    try {
        journal_fault_context context{
            .fault = inject_fault
                ? journal_fault().load(std::memory_order_relaxed)
                : supervision_journal_fault::none};
        const durable_io::fault_injection_hooks hooks{
            .context = &context,
            .before_operation = inject_journal_fault};
        const auto published = durable_io::durable_replace_file(
            path, payload_bytes(payload), {}, hooks);
        return published
            && published->publication
                == durable_io::publication_state::durable;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] inline std::string registry_header(
    std::uint64_t generation) {
    return "bioseq-supervision-registry-v1\t"
        + std::to_string(generation) + "\n";
}

[[nodiscard]] inline std::string serialize_clusters(
    const supervision_state& state) {
    std::ostringstream output;
    output << registry_header(state.generation);
    for (const supervised_cluster_identity& cluster : state.clusters) {
        output << hex_encode(cluster.root.string()) << '\t'
               << hex_encode(cluster.data_directory.string()) << '\t'
               << cluster.postmaster_pid << '\t'
               << (cluster.discover_postmaster_pid ? 1 : 0) << '\t'
               << hex_encode(cluster.postmaster_start_token) << '\t'
               << hex_encode(cluster.postmaster_executable.string()) << '\t'
               << cluster.postmaster_process_group << '\n';
    }
    return output.str();
}

[[nodiscard]] inline std::string serialize_processes(
    const supervision_state& state) {
    std::ostringstream output;
    output << registry_header(state.generation);
    for (const supervised_process_identity& process : state.processes) {
        output << process.pid << '\t'
               << hex_encode(process.start_token) << '\t'
               << hex_encode(process.executable.string()) << '\t'
               << process.process_group << '\t'
               << hex_encode(process.data_directory.string()) << '\n';
    }
    return output.str();
}

[[nodiscard]] inline bool begin_registry_update_locked(
    supervision_state& state) noexcept {
    if (!state.directory
        || state.generation
            == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const std::uint64_t generation = state.generation + 1U;
    if (!write_durable_file(
            *state.directory / "registries.dirty",
            std::to_string(generation) + "\n", false)) {
        return false;
    }
    state.generation = generation;
    return true;
}

[[nodiscard]] inline bool publish_registries_locked(
    supervision_state& state) noexcept {
    try {
        if (!state.directory
            || !write_durable_file(
                *state.directory / "clusters.tsv",
                serialize_clusters(state), true)
            || !write_durable_file(
                *state.directory / "processes.tsv",
                serialize_processes(state), true)) {
            return false;
        }
        if (!state.pending_process_launches.empty()) {
            return true;
        }
        if (!write_durable_file(
                *state.directory / "registries.clean",
                registry_header(state.generation), true)) {
            return false;
        }
        std::error_code remove_error;
        const bool removed = std::filesystem::remove(
            *state.directory / "registries.dirty", remove_error);
        if (!removed || remove_error) {
            return false;
        }
        if (fsync_directory(
                *state.directory,
                directory_fsync_checkpoint::
                    registry_clean_transition)) {
            return true;
        }
        // The clean transition was visible but not durable. Re-establish the
        // same generation's dirty marker durably before reporting failure so
        // an out-of-process watchdog cannot accept the clean registry pair.
        static_cast<void>(write_durable_file(
            *state.directory / "registries.dirty",
            std::to_string(state.generation) + "\n",
            false));
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace bioseq::postgres_test::supervision_detail

export namespace bioseq::postgres_test {

/**
 * @brief Exposes the no-follow classifier to native portability contracts.
 *
 * Normal absence is `false`; an entry is `true`; every lookup failure other
 * than ENOENT remains indeterminate (`nullopt`) and therefore fail-closed.
 */
[[nodiscard]] inline std::optional<bool>
is_supervision_path_entry_present_no_follow_for_testing(
    const std::filesystem::path& path) noexcept {
    return supervision_detail::is_path_entry_present_no_follow(path);
}

/// @brief Selects one deterministic registry I/O failure for contracts.
inline void set_supervision_journal_fault_for_testing(
    supervision_journal_fault fault) noexcept {
    supervision_detail::journal_fault().store(
        fault, std::memory_order_relaxed);
}

/// @brief Encodes arbitrary journal text without delimiter ambiguity.
[[nodiscard]] inline std::string supervision_hex_encode(
    std::string_view value) {
    return supervision_detail::hex_encode(value);
}

/// @brief Decodes one journal field; malformed input returns nullopt.
[[nodiscard]] inline std::optional<std::string> supervision_hex_decode(
    std::string_view value) {
    return supervision_detail::hex_decode(value);
}

/// @brief Enables crash journals in one exact marker-owned state directory.
inline void configure_supervision_state(
    const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const std::filesystem::path absolute =
        std::filesystem::absolute(directory);
    if (!supervision_detail::write_durable_file(
            absolute / ".bioseq-watchdog-state",
            std::to_string(::getpid()) + "\n", false)) {
        throw std::runtime_error(
            "cannot create watchdog-state ownership marker");
    }
    auto& state = supervision_detail::process_state();
    std::scoped_lock lock(state.mutex);
    state.directory = absolute;
    state.clusters.clear();
    state.processes.clear();
    state.generation = 0;
    state.next_launch_id = 0;
    state.pending_process_launches.clear();
    if (!supervision_detail::begin_registry_update_locked(state)
        || !supervision_detail::publish_registries_locked(state)) {
        throw std::runtime_error(
            "cannot initialize durable live-resource registries");
    }
}

/// @brief Disables journals without removing watchdog-owned evidence.
inline void disable_supervision_state() noexcept {
    try {
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        state.directory.reset();
        state.clusters.clear();
        state.processes.clear();
        state.generation = 0;
        state.next_launch_id = 0;
        state.pending_process_launches.clear();
    } catch (...) {
    }
}

/// @brief Returns the active crash-journal directory, if any.
[[nodiscard]] inline std::optional<std::filesystem::path>
supervision_state_directory() {
    auto& state = supervision_detail::process_state();
    std::scoped_lock lock(state.mutex);
    return state.directory;
}

/// @brief Captures a PID-reuse-resistant identity and verifies its executable.
[[nodiscard]] inline std::optional<supervised_process_identity>
capture_supervised_process_identity(
    std::uint64_t pid,
    const std::filesystem::path& expected_executable = {},
    const std::filesystem::path& data_directory = {}) noexcept {
    try {
        if (pid <= 1
            || pid > static_cast<std::uint64_t>(
                std::numeric_limits<pid_t>::max())) {
            return std::nullopt;
        }
        const auto start_token =
            supervision_detail::process_start_token(pid);
        const auto executable =
            supervision_detail::process_executable(pid);
        const pid_t group = ::getpgid(static_cast<pid_t>(pid));
        if (!start_token || start_token->empty() || !executable
            || group <= 1) {
            return std::nullopt;
        }
        if (!expected_executable.empty()) {
            const auto canonical_expected =
                supervision_detail::canonical_expected(
                    expected_executable);
            if (!canonical_expected || *canonical_expected != *executable) {
                return std::nullopt;
            }
        }
        return supervised_process_identity{
            .pid = pid,
            .start_token = *start_token,
            .executable = *executable,
            .process_group = static_cast<std::int64_t>(group),
            .data_directory = data_directory};
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief True only while the PID still denotes the captured executable/run.
[[nodiscard]] inline bool is_supervised_process_identity_current(
    const supervised_process_identity& process) noexcept {
    if (process.pid <= 1 || process.start_token.empty()
        || process.executable.empty() || process.process_group <= 1) {
        return false;
    }
    const auto current = capture_supervised_process_identity(
        process.pid, process.executable, process.data_directory);
    return current
        && current->start_token == process.start_token;
}

/// @brief True only while the identity remains in its captured process group.
[[nodiscard]] inline bool is_supervised_process_group_current(
    const supervised_process_identity& process) noexcept {
    if (!is_supervised_process_identity_current(process)) {
        return false;
    }
    const auto current = capture_supervised_process_identity(
        process.pid, process.executable, process.data_directory);
    return current
        && current->start_token == process.start_token
        && current->process_group == process.process_group;
}

/// @brief True only while the exact captured process is kernel-stopped.
[[nodiscard]] inline bool is_supervised_process_stopped(
    const supervised_process_identity& process) noexcept {
    try {
        if (!is_supervised_process_identity_current(process)) {
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
}

/// @brief True only when the recorded PID currently has no process.
[[nodiscard]] inline bool is_supervised_process_absent(
    const supervised_process_identity& process) noexcept {
    if (process.pid <= 1
        || process.pid
            > static_cast<std::uint64_t>(
                std::numeric_limits<pid_t>::max())) {
        return true;
    }
    int presence{};
    do {
        presence = ::kill(static_cast<pid_t>(process.pid), 0);
    } while (presence != 0 && errno == EINTR);
    return presence != 0 && errno == ESRCH;
}

/// @brief True when the stopped exact parent pins this exact zombie child.
[[nodiscard]] inline bool is_supervised_child_terminal(
    const supervised_process_identity& parent,
    const supervised_process_identity& child) noexcept {
    try {
        const auto current_parent = capture_supervised_process_identity(
            parent.pid, {}, parent.data_directory);
        const auto start_token =
            supervision_detail::process_start_token(child.pid);
        const auto parent_pid =
            supervision_detail::process_parent_pid(child.pid);
        const auto zombie =
            supervision_detail::is_process_zombie(child.pid);
        return current_parent && start_token && parent_pid && zombie
            && current_parent->start_token == parent.start_token
            && current_parent->executable == parent.executable
            && is_supervised_process_stopped(*current_parent)
            && *start_token == child.start_token
            && *parent_pid == parent.pid && *zombie;
    } catch (...) {
        return false;
    }
}

/// @brief True only while a stopped exact parent pins this exact child PID.
[[nodiscard]] inline bool is_supervised_child_pinned(
    const supervised_process_identity& parent,
    const supervised_process_identity& child) noexcept {
    try {
        const auto current_parent = capture_supervised_process_identity(
            parent.pid, {}, parent.data_directory);
        const auto current_child = capture_supervised_process_identity(
            child.pid, {}, child.data_directory);
        const auto current_parent_pid =
            supervision_detail::process_parent_pid(child.pid);
        return current_parent && current_child && current_parent_pid
            && current_parent->start_token == parent.start_token
            && current_parent->executable == parent.executable
            && current_child->start_token == child.start_token
            && current_child->executable == child.executable
            && *current_parent_pid == parent.pid
            && is_supervised_process_stopped(*current_parent);
    } catch (...) {
        return false;
    }
}

/// @brief Signals an exact child only while its stopped parent pins PID reuse.
///
/// The parent identity, stopped state, and live PPID edge are checked directly
/// before and after delivery.  A stale child is rejected.  Once SIGKILL was
/// delivered, an inconclusive post-check is reported as committed rather than
/// as a rollback-safe failure.
[[nodiscard]] inline supervised_child_signal_result
signal_supervised_child_while_parent_stopped(
    const supervised_process_identity& parent,
    const supervised_process_identity& child,
    int signal_number) noexcept {
    try {
        if (!is_supervised_child_pinned(parent, child)) {
            if (is_supervised_child_terminal(parent, child)
                || is_supervised_process_absent(child)) {
                return {
                    .outcome =
                        supervised_child_signal_outcome::target_absent};
            }
            return {};
        }
        int status{};
        do {
            status = ::kill(
                static_cast<pid_t>(child.pid), signal_number);
        } while (status != 0 && errno == EINTR);
        if (status != 0) {
            return {
                .outcome = errno == ESRCH
                    ? supervised_child_signal_outcome::target_absent
                    : supervised_child_signal_outcome::rejected};
        }

        if (signal_number == SIGKILL) {
            const auto parent_after = capture_supervised_process_identity(
                parent.pid, {}, parent.data_directory);
            const bool parent_remains_stopped =
                parent_after
                && parent_after->start_token == parent.start_token
                && parent_after->executable == parent.executable
                && is_supervised_process_stopped(*parent_after);
            return {
                .outcome = parent_remains_stopped
                    ? supervised_child_signal_outcome::delivered
                    : supervised_child_signal_outcome::
                          delivered_unverified};
        }

        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::milliseconds(200);
        do {
            const auto parent_after = capture_supervised_process_identity(
                parent.pid, {}, parent.data_directory);
            if (!parent_after
                || parent_after->start_token != parent.start_token
                || parent_after->executable != parent.executable
                || !is_supervised_process_stopped(*parent_after)) {
                return {
                    .outcome = supervised_child_signal_outcome::
                        delivered_unverified};
            }
            const auto after = capture_supervised_process_identity(
                child.pid, {}, child.data_directory);
            const auto parent_pid =
                supervision_detail::process_parent_pid(child.pid);
            if (after
                && after->start_token == child.start_token
                && after->executable == child.executable
                && parent_pid && *parent_pid == parent.pid) {
                const bool expected_stop_state =
                    signal_number != SIGSTOP
                        || is_supervised_process_stopped(*after);
                const bool expected_continue_state =
                    signal_number != SIGCONT
                        || !is_supervised_process_stopped(*after);
                if (expected_stop_state
                    && expected_continue_state) {
                    return {
                        .outcome =
                            supervised_child_signal_outcome::delivered,
                        .identity_after = *after};
                }
            } else if (after
                       || supervision_detail::process_start_token(
                              child.pid)) {
                return {
                    .outcome = supervised_child_signal_outcome::
                        delivered_unverified};
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        return {
            .outcome = supervised_child_signal_outcome::
                delivered_unverified};
    } catch (...) {
        return {};
    }
}

/// @brief Signals a PGID while a verified direct-child sentinel prevents reuse.
[[nodiscard]] inline bool signal_supervised_process_group(
    std::int64_t process_group,
    std::span<const supervised_process_identity> anchors,
    int signal_number) noexcept {
    try {
        if (process_group <= 1
            || process_group
                > static_cast<std::int64_t>(
                    std::numeric_limits<pid_t>::max())
            || process_group
                == static_cast<std::int64_t>(::getpgrp())) {
            return false;
        }
        int presence{};
        do {
            presence = ::kill(
                -static_cast<pid_t>(process_group), 0);
        } while (presence != 0 && errno == EINTR);
        if (presence != 0 && errno == ESRCH) {
            return true;
        }
        auto pin =
            supervision_detail::pin_process_group(process_group);
        if (!pin) {
            return false;
        }
        const bool anchored = std::ranges::any_of(
            anchors,
            [process_group](
                const supervised_process_identity& anchor) {
                if (anchor.process_group != process_group) {
                    return false;
                }
                const auto current = capture_supervised_process_identity(
                    anchor.pid, anchor.executable,
                    anchor.data_directory);
                return current
                    && current->start_token == anchor.start_token
                    && current->process_group == process_group;
            });
        if (!anchored) {
            return true;
        }
        int status{};
        do {
            status = ::kill(
                -static_cast<pid_t>(process_group), signal_number);
        } while (status != 0 && errno == EINTR);
        const int signal_error = errno;
        if (signal_number == SIGSTOP) {
            pin->resume_after_group_stop();
        }
        return status == 0 || signal_error == ESRCH;
    } catch (...) {
        return false;
    }
}

/// @brief Verifies that a live postgres identity owns this exact PGDATA.
///
/// Verification requires identity continuity, a stable postmaster.pid whose
/// first two fields name the PID and canonical PGDATA, a valid start-time
/// field, and an OS-observed `-D` argument naming the same directory.
[[nodiscard]] inline bool is_supervised_postmaster_bound_to_data_directory(
    const supervised_process_identity& process) noexcept {
    try {
        if (process.data_directory.empty()
            || !is_supervised_process_identity_current(process)
            || !supervision_detail::is_process_using_data_directory(
                process.pid, process.data_directory)) {
            return false;
        }
        std::ifstream input(
            process.data_directory / "postmaster.pid");
        std::string pid_line;
        std::string data_line;
        std::string start_line;
        if (!std::getline(input, pid_line)
            || !std::getline(input, data_line)
            || !std::getline(input, start_line)) {
            return false;
        }
        std::uint64_t pid{};
        const auto [pid_end, pid_error] = std::from_chars(
            pid_line.data(), pid_line.data() + pid_line.size(), pid);
        std::uint64_t start_time{};
        const auto [start_end, start_error] = std::from_chars(
            start_line.data(),
            start_line.data() + start_line.size(), start_time);
        if (pid_error != std::errc{}
            || pid_end != pid_line.data() + pid_line.size()
            || pid != process.pid || start_error != std::errc{}
            || start_end != start_line.data() + start_line.size()
            || start_time == 0) {
            return false;
        }
        const auto canonical_data =
            supervision_detail::canonical_expected(
                process.data_directory);
        const auto canonical_pidfile_data =
            supervision_detail::canonical_expected(
                std::filesystem::path(data_line));
        return canonical_data && canonical_pidfile_data
            && *canonical_data == *canonical_pidfile_data;
    } catch (...) {
        return false;
    }
}

/// @brief True only when no process remains in the recorded process group.
[[nodiscard]] inline bool is_supervised_process_group_absent(
    std::int64_t process_group) noexcept {
    if (process_group <= 1
        || process_group
            > static_cast<std::int64_t>(
                std::numeric_limits<pid_t>::max())) {
        return true;
    }
    if (::kill(-static_cast<pid_t>(process_group), 0) == 0) {
        return false;
    }
    return errno == ESRCH;
}

/// @brief Atomically elects disarm or timeout ownership for one watchdog.
[[nodiscard]] inline bool try_claim_supervision_decision(
    const std::filesystem::path& directory,
    std::string_view owner) noexcept {
    try {
        if (owner.empty() || owner.size() > 256U
            || !supervision_detail::is_valid_watchdog_state_marker(
                directory / ".bioseq-watchdog-state")) {
            return false;
        }
        const auto decision_lock =
            supervision_detail::acquire_decision_lock(
                directory, LOCK_EX);
        if (!decision_lock) {
            return false;
        }
        const std::filesystem::path decision =
            directory / "decision";
        const auto decision_exists =
            supervision_detail::is_path_entry_present_no_follow(decision);
        if (!decision_exists) {
            return false;
        }
        if (*decision_exists) {
            if (supervision_detail::is_valid_decision_file(decision)) {
                return false;
            }
            std::error_code decision_error;
            if (!std::filesystem::remove(
                    decision, decision_error)
                || decision_error
                || !supervision_detail::fsync_directory(directory)) {
                return false;
            }
        }
        supervision_detail::journal_fault_context fault_context{
            .fault = supervision_detail::journal_fault().load(
                std::memory_order_relaxed),
            .delay_directory_sync = true};
        const bioseq::common::durable_file_io::fault_injection_hooks hooks{
            .context = &fault_context,
            .before_operation =
                supervision_detail::inject_journal_fault};
        const std::string payload =
            "bioseq-supervision-decision-v1\t"
            + supervision_detail::hex_encode(owner) + "\n";
        const auto published =
            bioseq::common::durable_file_io::durable_publish_file(
            decision,
            supervision_detail::payload_bytes(payload),
            bioseq::common::durable_file_io::publication_policy::
                exclusive_no_replace,
            {},
            hooks);
        if (published) {
            return published->publication
                == bioseq::common::durable_file_io::publication_state::
                    durable;
        }
        if (published.error().publication
            != bioseq::common::durable_file_io::publication_state::
                visible_not_durable) {
            return false;
        }
        const supervision_journal_fault configured_fault =
            supervision_detail::journal_fault().load(
                std::memory_order_relaxed);
        std::error_code remove_error;
        bool decision_removed = false;
        if (configured_fault
                == supervision_journal_fault::
                    decision_rollback_remove
            || configured_fault
                == supervision_journal_fault::
                    decision_reconciliation_fsync_directory) {
            remove_error = std::make_error_code(std::errc::io_error);
        } else {
            decision_removed =
                std::filesystem::remove(decision, remove_error);
        }
        bool decision_absent =
            decision_removed || !remove_error;
        bool reconciliation_durable =
            supervision_detail::fsync_directory(
                directory,
                supervision_detail::directory_fsync_checkpoint::
                    decision_reconciliation);
        if (!reconciliation_durable) {
            // A visible decision may be accepted only after a successful
            // directory fsync. Retry rollback, then reconcile the namespace
            // without the one-shot test fault before deciding the receipt.
            if (!decision_absent) {
                remove_error.clear();
                decision_removed =
                    std::filesystem::remove(decision, remove_error);
                decision_absent =
                    decision_removed || !remove_error;
            }
            reconciliation_durable =
                supervision_detail::fsync_directory(directory);
        }
        return !decision_absent
            && reconciliation_durable
            && supervision_detail::is_valid_decision_file(decision);
    } catch (...) {
        return false;
    }
}

/// @brief Reports whether either side has already claimed the watchdog.
[[nodiscard]] inline bool is_supervision_decision_claimed(
    const std::filesystem::path& directory) noexcept {
    try {
        if (!supervision_detail::is_valid_watchdog_state_marker(
                directory / ".bioseq-watchdog-state")) {
            return false;
        }
        const auto decision_lock =
            supervision_detail::acquire_decision_lock(
                directory, LOCK_SH);
        return decision_lock
            && supervision_detail::is_valid_decision_file(
                directory / "decision");
    } catch (...) {
        return false;
    }
}

/// @brief Atomically replaces one bounded watchdog evidence snapshot.
[[nodiscard]] inline bool publish_supervision_snapshot(
    std::string_view basename,
    std::string_view payload) noexcept {
    try {
        if (basename.empty() || basename.size() > 80U
            || payload.size() > 64U * 1024U * 1024U) {
            return false;
        }
        for (const char character : basename) {
            const bool valid =
                (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '-' || character == '.';
            if (!valid) {
                return false;
            }
        }
        std::scoped_lock publish_lock(
            supervision_detail::snapshot_publish_mutex());
        const auto directory = supervision_state_directory();
        if (!directory) {
            return false;
        }
        const std::filesystem::path destination =
            *directory / std::string(basename);
        const auto published =
            bioseq::common::durable_file_io::durable_replace_file(
            destination, supervision_detail::payload_bytes(payload));
        return published
            && published->publication
                == bioseq::common::durable_file_io::publication_state::
                    durable;
    } catch (...) {
        return false;
    }
}

/// @brief Loads only a complete final watchdog evidence snapshot.
[[nodiscard]] inline std::optional<std::string>
load_supervision_snapshot(
    const std::filesystem::path& directory,
    std::string_view basename) noexcept {
    try {
        if (basename.empty() || basename.size() > 80U) {
            return std::nullopt;
        }
        for (const char character : basename) {
            const bool valid =
                (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '-' || character == '.';
            if (!valid) {
                return std::nullopt;
            }
        }
        return supervision_detail::read_stable_text(
            directory / std::string(basename),
            0U,
            64U * 1024U * 1024U);
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Durably marks a supervised child launch before posix_spawn.
[[nodiscard]] inline std::optional<supervision_launch_intent>
begin_supervised_launch() noexcept {
    try {
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        if (!state.directory) {
            return supervision_launch_intent{};
        }
        if (state.next_launch_id
                == std::numeric_limits<std::uint64_t>::max()
            || !supervision_detail::begin_registry_update_locked(
                state)) {
            return std::nullopt;
        }
        const std::uint64_t id = ++state.next_launch_id;
        state.pending_process_launches.push_back(id);
        return supervision_launch_intent{
            .id = id,
            .supervision_required = true};
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Commits the exact spawned child into its pre-launch dirty generation.
inline bool commit_supervised_launch(
    supervision_launch_intent intent,
    std::uint64_t pid,
    const std::filesystem::path& executable = {},
    const std::filesystem::path& data_directory = {}) noexcept {
    try {
        if (!intent.supervision_required) {
            return true;
        }
        if (intent.id == 0 || pid <= 1) {
            return false;
        }
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        const auto pending = std::ranges::find(
            state.pending_process_launches, intent.id);
        if (!state.directory
            || pending == state.pending_process_launches.end()) {
            return false;
        }
        const auto process =
            capture_supervised_process_identity(pid, executable, data_directory);
        if (!process) {
            return false;
        }
        state.processes.erase(
            std::remove_if(
                state.processes.begin(), state.processes.end(),
                [pid](const supervised_process_identity& current) {
                    return current.pid == pid;
                }),
            state.processes.end());
        state.processes.push_back(*process);
        state.pending_process_launches.erase(pending);
        return !state.pending_process_launches.empty()
            || supervision_detail::publish_registries_locked(state);
    } catch (...) {
        return false;
    }
}

/// @brief Cancels one failed launch while preserving dirty state on I/O error.
inline bool cancel_supervised_launch(
    supervision_launch_intent intent,
    std::uint64_t pid = 0) noexcept {
    try {
        if (!intent.supervision_required) {
            return true;
        }
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        if (!state.directory) {
            return false;
        }
        const auto pending = std::ranges::find(
            state.pending_process_launches, intent.id);
        if (pending != state.pending_process_launches.end()) {
            state.pending_process_launches.erase(pending);
        }
        if (pid > 1) {
            state.processes.erase(
                std::remove_if(
                    state.processes.begin(), state.processes.end(),
                    [pid](const supervised_process_identity& current) {
                        return current.pid == pid;
                    }),
                state.processes.end());
        }
        return !state.pending_process_launches.empty()
            || supervision_detail::publish_registries_locked(state);
    } catch (...) {
        return false;
    }
}

/// @brief Removes one reaped lifecycle child from timeout ownership.
inline bool remove_supervised_process(std::uint64_t pid) noexcept {
    try {
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        if (!state.directory) {
            return true;
        }
        if (std::ranges::none_of(
                state.processes,
                [pid](const supervised_process_identity& current) {
                    return current.pid == pid;
                })) {
            return supervision_detail::begin_registry_update_locked(state)
                && supervision_detail::publish_registries_locked(state);
        }
        auto candidate = state.processes;
        candidate.erase(
            std::remove_if(
                candidate.begin(), candidate.end(),
                [pid](const supervised_process_identity& current) {
                    return current.pid == pid;
                }),
            candidate.end());
        if (!supervision_detail::begin_registry_update_locked(state)) {
            return false;
        }
        state.processes.swap(candidate);
        if (supervision_detail::publish_registries_locked(state)) {
            return true;
        }
        state.processes.swap(candidate);
        return false;
    } catch (...) {
        return false;
    }
}

/// @brief Atomically publishes one live private cluster for its watchdog.
inline void register_supervised_cluster(
    supervised_cluster_identity cluster) {
    auto& state = supervision_detail::process_state();
    std::scoped_lock lock(state.mutex);
    if (!state.directory) {
        return;
    }
    if (cluster.postmaster_pid > 1
        && cluster.postmaster_start_token.empty()) {
        if (const auto process = capture_supervised_process_identity(
                cluster.postmaster_pid,
                cluster.postmaster_executable,
                cluster.data_directory);
            process
            && is_supervised_postmaster_bound_to_data_directory(
                *process)) {
            cluster.postmaster_start_token = process->start_token;
            cluster.postmaster_executable = process->executable;
            cluster.postmaster_process_group =
                process->process_group;
        }
    }
    if (cluster.postmaster_pid > 1
        && cluster.postmaster_start_token.empty()
        && !cluster.discover_postmaster_pid) {
        throw std::runtime_error(
            "cannot journal an unverified postmaster identity");
    }
    auto candidate = state.clusters;
    candidate.erase(
        std::remove_if(
            candidate.begin(), candidate.end(),
            [&cluster](const supervised_cluster_identity& current) {
                return current.root == cluster.root;
            }),
        candidate.end());
    candidate.push_back(std::move(cluster));
    if (!supervision_detail::begin_registry_update_locked(state)) {
        throw std::runtime_error(
            "cannot mark live-resource registries dirty");
    }
    state.clusters.swap(candidate);
    if (!supervision_detail::publish_registries_locked(state)) {
        state.clusters.swap(candidate);
        throw std::runtime_error(
            "cannot durably publish live-resource registries");
    }
}

/// @brief Removes one stopped/cleaned cluster from the watchdog registry.
inline bool remove_supervised_cluster(
    const std::filesystem::path& root) noexcept {
    try {
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        if (!state.directory) {
            return true;
        }
        if (std::ranges::none_of(
                state.clusters,
                [&root](const supervised_cluster_identity& current) {
                    return current.root == root;
                })) {
            return supervision_detail::begin_registry_update_locked(state)
                && supervision_detail::publish_registries_locked(state);
        }
        auto candidate = state.clusters;
        candidate.erase(
            std::remove_if(
                candidate.begin(), candidate.end(),
                [&root](const supervised_cluster_identity& current) {
                    return current.root == root;
                }),
            candidate.end());
        if (!supervision_detail::begin_registry_update_locked(state)) {
            return false;
        }
        state.clusters.swap(candidate);
        if (supervision_detail::publish_registries_locked(state)) {
            return true;
        }
        state.clusters.swap(candidate);
        return false;
    } catch (...) {
        return false;
    }
}

/// @brief Strictly loads one clean, generation-consistent registry pair.
[[nodiscard]] inline std::optional<supervision_registry_snapshot>
load_supervision_registry_snapshot(
    const std::filesystem::path& directory) noexcept {
    try {
        if (!supervision_detail::is_valid_watchdog_state_marker(
                directory / ".bioseq-watchdog-state")) {
            return std::nullopt;
        }
        const auto dirty =
            supervision_detail::is_path_entry_present_no_follow(
                directory / "registries.dirty");
        if (!dirty || *dirty) {
            return std::nullopt;
        }
        for (const std::string_view partial :
             {std::string_view{"registries.dirty.partial"},
              std::string_view{"registries.clean.partial"},
              std::string_view{"clusters.tsv.partial"},
              std::string_view{"processes.tsv.partial"}}) {
            const auto partial_exists =
                supervision_detail::is_path_entry_present_no_follow(
                    directory / std::string(partial));
            if (!partial_exists || *partial_exists) {
                return std::nullopt;
            }
        }
        const auto clean = load_supervision_snapshot(
            directory, "registries.clean");
        const auto cluster_payload = load_supervision_snapshot(
            directory, "clusters.tsv");
        const auto process_payload = load_supervision_snapshot(
            directory, "processes.tsv");
        if (!clean || !cluster_payload || !process_payload) {
            return std::nullopt;
        }
        constexpr std::string_view k_prefix{
            "bioseq-supervision-registry-v1\t"};
        const auto parse_header =
            [k_prefix](std::string_view line)
                -> std::optional<std::uint64_t> {
            if (!line.starts_with(k_prefix)) {
                return std::nullopt;
            }
            line.remove_prefix(k_prefix.size());
            std::uint64_t generation{};
            const auto [end, error] = std::from_chars(
                line.data(), line.data() + line.size(), generation);
            if (error != std::errc{}
                || end != line.data() + line.size()
                || generation == 0) {
                return std::nullopt;
            }
            return generation;
        };
        if (clean->empty() || clean->back() != '\n'
            || clean->find('\n') != clean->size() - 1U) {
            return std::nullopt;
        }
        const auto generation = parse_header(
            std::string_view(*clean).substr(0, clean->size() - 1U));
        if (!generation) {
            return std::nullopt;
        }
        const auto journal_rows =
            [&parse_header, generation](
                const std::string& payload)
                -> std::optional<std::vector<std::string_view>> {
            if (payload.empty() || payload.back() != '\n') {
                return std::nullopt;
            }
            const std::size_t header_end = payload.find('\n');
            if (header_end == std::string::npos
                || parse_header(std::string_view(payload).substr(
                       0, header_end))
                    != generation) {
                return std::nullopt;
            }
            std::vector<std::string_view> rows;
            std::size_t begin = header_end + 1U;
            while (begin < payload.size()) {
                const std::size_t end = payload.find('\n', begin);
                if (end == std::string::npos || end == begin) {
                    return std::nullopt;
                }
                rows.push_back(
                    std::string_view(payload).substr(
                        begin, end - begin));
                begin = end + 1U;
            }
            return rows;
        };
        const auto cluster_rows = journal_rows(*cluster_payload);
        const auto process_rows = journal_rows(*process_payload);
        if (!cluster_rows || !process_rows) {
            return std::nullopt;
        }
        const auto split_fields =
            [](std::string_view line) {
            std::vector<std::string_view> fields;
            std::size_t begin = 0;
            while (begin <= line.size()) {
                const std::size_t end = line.find('\t', begin);
                fields.push_back(line.substr(
                    begin,
                    end == std::string::npos
                        ? std::string::npos : end - begin));
                if (end == std::string::npos) {
                    break;
                }
                begin = end + 1U;
            }
            return fields;
        };

        supervision_registry_snapshot snapshot{
            .generation = *generation};
        snapshot.clusters.reserve(cluster_rows->size());
        for (const std::string_view row : *cluster_rows) {
            const auto fields = split_fields(row);
            if (fields.size() != 7U) {
                return std::nullopt;
            }
            const auto root = supervision_hex_decode(fields[0]);
            const auto data = supervision_hex_decode(fields[1]);
            const auto start_token = supervision_hex_decode(fields[4]);
            const auto executable = supervision_hex_decode(fields[5]);
            std::uint64_t pid{};
            const auto [pid_end, pid_error] = std::from_chars(
                fields[2].data(),
                fields[2].data() + fields[2].size(), pid);
            std::int64_t process_group{};
            const auto [group_end, group_error] = std::from_chars(
                fields[6].data(),
                fields[6].data() + fields[6].size(),
                process_group);
            if (!root || root->empty() || !data || data->empty()
                || !start_token || !executable
                || pid_error != std::errc{}
                || pid_end != fields[2].data() + fields[2].size()
                || (fields[3] != "0" && fields[3] != "1")
                || group_error != std::errc{}
                || group_end
                    != fields[6].data() + fields[6].size()
                || pid == 1
                || pid > static_cast<std::uint64_t>(
                    std::numeric_limits<pid_t>::max())
                || process_group < 0
                || process_group > static_cast<std::int64_t>(
                    std::numeric_limits<pid_t>::max())
                || (pid > 1 && fields[3] == "0"
                    && (start_token->empty() || executable->empty()
                        || process_group <= 1))
                || std::ranges::any_of(
                    snapshot.clusters,
                    [&root](const supervised_cluster_identity& current) {
                        return current.root == *root;
                    })) {
                return std::nullopt;
            }
            snapshot.clusters.push_back(supervised_cluster_identity{
                .root = *root,
                .data_directory = *data,
                .postmaster_pid = pid,
                .discover_postmaster_pid = fields[3] == "1",
                .postmaster_start_token = std::move(*start_token),
                .postmaster_executable = std::move(*executable),
                .postmaster_process_group = process_group});
        }
        snapshot.processes.reserve(process_rows->size());
        for (const std::string_view row : *process_rows) {
            const auto fields = split_fields(row);
            if (fields.size() != 5U) {
                return std::nullopt;
            }
            std::uint64_t pid{};
            const auto [pid_end, pid_error] = std::from_chars(
                fields[0].data(),
                fields[0].data() + fields[0].size(), pid);
            std::int64_t process_group{};
            const auto [group_end, group_error] = std::from_chars(
                fields[3].data(),
                fields[3].data() + fields[3].size(),
                process_group);
            const auto start_token = supervision_hex_decode(fields[1]);
            const auto executable = supervision_hex_decode(fields[2]);
            const auto data_directory =
                supervision_hex_decode(fields[4]);
            if (pid_error != std::errc{}
                || pid_end != fields[0].data() + fields[0].size()
                || group_error != std::errc{}
                || group_end
                    != fields[3].data() + fields[3].size()
                || pid <= 1
                || pid > static_cast<std::uint64_t>(
                    std::numeric_limits<pid_t>::max())
                || process_group <= 1
                || process_group > static_cast<std::int64_t>(
                    std::numeric_limits<pid_t>::max())
                || !start_token || start_token->empty()
                || !executable || executable->empty()
                || !data_directory
                || std::ranges::any_of(
                    snapshot.processes,
                    [pid](const supervised_process_identity& current) {
                        return current.pid == pid;
                    })) {
                return std::nullopt;
            }
            snapshot.processes.push_back(supervised_process_identity{
                .pid = pid,
                .start_token = std::move(*start_token),
                .executable = std::move(*executable),
                .process_group = process_group,
                .data_directory = std::move(*data_directory)});
        }
        return snapshot;
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Verifies both live-resource journals are clean, valid, and empty.
[[nodiscard]] inline bool is_supervision_registry_empty(
    const std::filesystem::path& directory) noexcept {
    const auto snapshot = load_supervision_registry_snapshot(directory);
    return snapshot && snapshot->clusters.empty()
        && snapshot->processes.empty();
}

/// @brief Removes only a clean state with no registered live resources.
inline bool remove_supervision_state(
    const std::filesystem::path& directory) noexcept {
    try {
        if (!is_supervision_registry_empty(directory)) {
            return false;
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return !error;
    } catch (...) {
        return false;
    }
}

}  // namespace bioseq::postgres_test
