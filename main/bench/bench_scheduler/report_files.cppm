/**
 * @file report_files.cppm
 * @brief File-system + git utilities shared across bench harnesses.
 *
 * Pulled out of `bench_main.cpp` so any future bench target (the IO
 * bench, future micro-bench drivers) gets the same numbered-report
 * convention and self-stamping commit hash. Pure infrastructure, no
 * domain knowledge.
 */
module;

#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>

export module bench.scheduler.report_files;

export {

namespace bioseq::bench_scheduler {

/**
 * @brief Pick the next free `report_<N>.json` in `dir`.
 *
 * Walks the directory once, parses any existing `<prefix><N>.json`
 * filenames, returns `<prefix><max + 1>.json`. Creates the directory
 * on demand.
 *
 * `prefix` defaults to `"report_build_"` to match the existing aligner
 * bench. The IO bench can override it via the second parameter.
 */
[[nodiscard]] inline std::string next_numbered_report_path(
    const std::filesystem::path& dir,
    std::string_view prefix = "report_build_")
{
    // Build the regex once per call. The pattern is parameterised by
    // `prefix` so the function works for any caller-chosen filename
    // shape; for the aligner bench this collapses to `report_build_(\d+)\.json`.
    const std::string pattern_text =
        std::string{"^"} + std::string{prefix} + R"(\d+\.json$)";
    const std::regex name_pattern{pattern_text};
    const std::regex index_pattern{
        std::string{"^"} + std::string{prefix} + R"((\d+)\.json$)"};

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    long long max_index = 0;
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (!std::regex_match(name, name_pattern)) continue;
            std::smatch match;
            if (!std::regex_match(name, match, index_pattern)) continue;
            // `std::from_chars` rather than `std::stoll` keeps the
            // function exception-free per CODE_STYLE.md §1.
            const std::string index_str = match[1].str();
            long long parsed_index = 0;
            const auto result = std::from_chars(
                index_str.data(),
                index_str.data() + index_str.size(),
                parsed_index);
            if (result.ec != std::errc{}) continue;
            if (parsed_index > max_index) max_index = parsed_index;
        }
    }
    return (dir / (std::string{prefix} + std::to_string(max_index + 1) + ".json")).string();
}

/**
 * @brief Resolve the current commit hash by spawning `git rev-parse HEAD`.
 *
 * `repo_root` is the absolute path to the repository working tree;
 * `git -C <root>` makes the call work regardless of cwd (the bench
 * binary is launched from `build/<mode>/`, not the source root).
 *
 * Returns `"unknown"` on any failure: missing `git`, detached
 * worktree, repository deleted, etc. Reports never block on the git
 * check — they self-stamp with the best information available.
 */
[[nodiscard]] inline std::string detect_git_commit_hash(std::string_view repo_root) {
    std::string command = "git -C \"";
    command.append(repo_root);
    // `2>/dev/null` swallows git's "not a git repository" stderr noise.
    command.append("\" rev-parse HEAD 2>/dev/null");
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) return std::string{"unknown"};
    std::array<char, 64> buffer{};
    std::string commit_hash;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        commit_hash.append(buffer.data());
    }
    const int rc = ::pclose(pipe);
    if (rc != 0) return std::string{"unknown"};
    while (!commit_hash.empty() &&
           (commit_hash.back() == '\n' || commit_hash.back() == '\r' ||
            commit_hash.back() == ' '  || commit_hash.back() == '\t')) {
        commit_hash.pop_back();
    }
    if (commit_hash.empty()) return std::string{"unknown"};
    return commit_hash;
}

}  // namespace bioseq::bench_scheduler

}  // export
