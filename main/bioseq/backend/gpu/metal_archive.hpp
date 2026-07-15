#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bioseq::gpu::metal_archive {

// Create a Metal binary archive (.metalar) for the compute shader in metallib_path.
// The qualified archive is written to archive_path. Production callers pass a
// private temporary path and publish it with an fsync+rename only after the
// exact success sentinel is observed.
// Runs purely in the calling process; caller must be an exec-clean child (no inherited
// Metal/dispatch/XPC state) to avoid MTLCompilerService XPC invalidation (DOC0344).
// Returns a line-per-step status transcript; "RESULT: ok" is the success sentinel.
[[nodiscard]] std::string build_binary_archive(
    std::string_view metallib_path,
    std::string_view archive_path,
    std::string_view entry_name,
    std::uint64_t max_total_threads_per_threadgroup,
    bool threadgroup_size_is_multiple_of_thread_execution_width);

}  // namespace bioseq::gpu::metal_archive
