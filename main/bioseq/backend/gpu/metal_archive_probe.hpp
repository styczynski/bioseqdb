#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace bioseq::gpu::metal_probe {

[[nodiscard]] std::string run_archive_probe(
    std::string_view mode,
    std::string_view metallib_path,
    std::string_view archive_path);

[[nodiscard]] std::string run_archive_probe(
    std::string_view mode,
    std::string_view metallib_path,
    std::string_view archive_path,
    std::string_view entry_name,
    std::uint64_t max_total_threads_per_threadgroup,
    bool threadgroup_size_is_multiple_of_thread_execution_width);

[[nodiscard]] std::string run_exec_child_probe(
    std::string_view helper_path,
    std::string_view mode,
    std::string_view metallib_path,
    std::string_view archive_path);

}  // namespace bioseq::gpu::metal_probe
