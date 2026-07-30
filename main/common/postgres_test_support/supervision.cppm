module;

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.test.postgres:supervision;

export namespace bioseq::postgres_test {

/// @brief One cluster identity persisted for an out-of-process watchdog.
struct supervised_cluster_record {
    std::filesystem::path root;
    std::filesystem::path data_directory;
    std::uint64_t postmaster_pid{};
    bool discover_postmaster_pid{};
};

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::supervision_detail {

struct supervision_state {
    std::mutex mutex;
    std::optional<std::filesystem::path> directory;
    std::vector<supervised_cluster_record> clusters;
    std::vector<std::uint64_t> processes;
};

[[nodiscard]] inline supervision_state& process_state() {
    static supervision_state state;
    return state;
}

[[nodiscard]] inline std::string hex_encode(std::string_view value) {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const unsigned char byte : value) {
        encoded.push_back(digits[(byte >> 4U) & 0x0fU]);
        encoded.push_back(digits[byte & 0x0fU]);
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

void publish_clusters_locked(supervision_state& state) {
    if (!state.directory) {
        return;
    }
    const std::filesystem::path path = *state.directory / "clusters.tsv";
    const std::filesystem::path partial =
        *state.directory / "clusters.tsv.partial";
    std::ofstream output(partial, std::ios::trunc);
    if (!output) {
        return;
    }
    for (const supervised_cluster_record& cluster : state.clusters) {
        output << hex_encode(cluster.root.string()) << '\t'
               << hex_encode(cluster.data_directory.string()) << '\t'
               << cluster.postmaster_pid << '\t'
               << (cluster.discover_postmaster_pid ? 1 : 0) << '\n';
    }
    output.close();
    if (!output) {
        return;
    }
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(partial, path, error);
    }
}

void publish_processes_locked(supervision_state& state) {
    if (!state.directory) {
        return;
    }
    const std::filesystem::path path =
        *state.directory / "processes.tsv";
    const std::filesystem::path partial =
        *state.directory / "processes.tsv.partial";
    std::ofstream output(partial, std::ios::trunc);
    if (!output) {
        return;
    }
    for (const std::uint64_t pid : state.processes) {
        output << pid << '\n';
    }
    output.close();
    if (!output) {
        return;
    }
    std::error_code error;
    std::filesystem::rename(partial, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(partial, path, error);
    }
}

}  // namespace bioseq::postgres_test::supervision_detail

export namespace bioseq::postgres_test {

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
    std::ofstream marker(
        directory / ".bioseq-watchdog-state", std::ios::trunc);
    marker << ::getpid() << '\n';
    marker.close();
    if (!marker) {
        throw std::runtime_error(
            "cannot create watchdog-state ownership marker");
    }
    auto& state = supervision_detail::process_state();
    std::scoped_lock lock(state.mutex);
    state.directory = std::filesystem::absolute(directory);
    state.clusters.clear();
    state.processes.clear();
    supervision_detail::publish_clusters_locked(state);
    supervision_detail::publish_processes_locked(state);
}

/// @brief Disables journals without removing watchdog-owned evidence.
inline void disable_supervision_state() noexcept {
    try {
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        state.directory.reset();
        state.clusters.clear();
        state.processes.clear();
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

/// @brief Publishes one shell-free lifecycle child for timeout cleanup.
inline void register_supervised_process(std::uint64_t pid) noexcept {
    try {
        if (pid <= 1) {
            return;
        }
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        if (!state.directory) {
            return;
        }
        if (std::find(
                state.processes.begin(), state.processes.end(), pid)
            == state.processes.end()) {
            state.processes.push_back(pid);
        }
        supervision_detail::publish_processes_locked(state);
    } catch (...) {
    }
}

/// @brief Removes one reaped lifecycle child from timeout ownership.
inline void unregister_supervised_process(std::uint64_t pid) noexcept {
    try {
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        if (!state.directory) {
            return;
        }
        state.processes.erase(
            std::remove(
                state.processes.begin(), state.processes.end(), pid),
            state.processes.end());
        supervision_detail::publish_processes_locked(state);
    } catch (...) {
    }
}

/// @brief Loads lifecycle process groups that were live at runner freeze.
[[nodiscard]] inline std::vector<std::uint64_t>
load_supervised_processes(const std::filesystem::path& directory) {
    std::ifstream input(directory / "processes.tsv");
    std::vector<std::uint64_t> processes;
    std::string line;
    while (std::getline(input, line)) {
        std::uint64_t pid{};
        const auto [end, error] = std::from_chars(
            line.data(), line.data() + line.size(), pid);
        if (error == std::errc{} && end == line.data() + line.size()
            && pid > 1) {
            processes.push_back(pid);
        }
    }
    return processes;
}

/// @brief Atomically publishes one live private cluster for its watchdog.
inline void register_supervised_cluster(
    supervised_cluster_record cluster) noexcept {
    try {
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        if (!state.directory) {
            return;
        }
        state.clusters.erase(
            std::remove_if(
                state.clusters.begin(), state.clusters.end(),
                [&cluster](const supervised_cluster_record& current) {
                    return current.root == cluster.root;
                }),
            state.clusters.end());
        state.clusters.push_back(std::move(cluster));
        supervision_detail::publish_clusters_locked(state);
    } catch (...) {
    }
}

/// @brief Removes one stopped/cleaned cluster from the watchdog registry.
inline void unregister_supervised_cluster(
    const std::filesystem::path& root) noexcept {
    try {
        auto& state = supervision_detail::process_state();
        std::scoped_lock lock(state.mutex);
        if (!state.directory) {
            return;
        }
        state.clusters.erase(
            std::remove_if(
                state.clusters.begin(), state.clusters.end(),
                [&root](const supervised_cluster_record& current) {
                    return current.root == root;
                }),
            state.clusters.end());
        supervision_detail::publish_clusters_locked(state);
    } catch (...) {
    }
}

/// @brief Loads the last complete live-cluster registry for a watchdog.
[[nodiscard]] inline std::vector<supervised_cluster_record>
load_supervised_clusters(const std::filesystem::path& directory) {
    std::ifstream input(directory / "clusters.tsv");
    std::vector<supervised_cluster_record> clusters;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t first = line.find('\t');
        const std::size_t second =
            first == std::string::npos
            ? std::string::npos : line.find('\t', first + 1U);
        const std::size_t third =
            second == std::string::npos
            ? std::string::npos : line.find('\t', second + 1U);
        if (first == std::string::npos
            || second == std::string::npos
            || third == std::string::npos) {
            continue;
        }
        const auto root =
            supervision_hex_decode(std::string_view(line).substr(0, first));
        const auto data = supervision_hex_decode(
            std::string_view(line).substr(
                first + 1U, second - first - 1U));
        std::uint64_t pid{};
        const std::string_view pid_text =
            std::string_view(line).substr(
                second + 1U, third - second - 1U);
        const std::string_view discover_text =
            std::string_view(line).substr(third + 1U);
        const auto [end, error] = std::from_chars(
            pid_text.data(), pid_text.data() + pid_text.size(), pid);
        if (!root || !data || error != std::errc{}
            || end != pid_text.data() + pid_text.size() || pid == 1) {
            continue;
        }
        clusters.push_back(supervised_cluster_record{
            .root = *root,
            .data_directory = *data,
            .postmaster_pid = pid,
            .discover_postmaster_pid = discover_text == "1"});
    }
    return clusters;
}

/// @brief Removes only an exact watchdog state bearing its ownership marker.
inline bool remove_supervision_state(
    const std::filesystem::path& directory) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(
                directory / ".bioseq-watchdog-state", error)
            || error) {
            return false;
        }
        std::filesystem::remove_all(directory, error);
        return !error;
    } catch (...) {
        return false;
    }
}

}  // namespace bioseq::postgres_test
