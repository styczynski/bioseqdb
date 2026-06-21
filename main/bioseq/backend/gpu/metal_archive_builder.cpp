#include "gpu/metal_archive.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

// Exec-clean helper for bioseq GPU metal binary archive creation.
// Called from PostgreSQL backends via fork+execv to avoid MTLCompilerService
// XPC invalidation that occurs in fork-without-exec PG backends (DOC0344).
//
// Usage: bioseq_gpu_archive_builder <metallib_path> <archive_path>
//        <entry_name> <max_total_threads_per_threadgroup> <threadgroup_multiple>
int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: bioseq_gpu_archive_builder <metallib_path> <archive_path>"
                     " <entry_name> <max_total_threads_per_threadgroup> <threadgroup_multiple>\n";
        return 2;
    }

    const std::string_view metallib_path = argv[1];
    const std::string_view archive_path = argv[2];
    const std::string_view entry_name = argv[3];
    const std::uint64_t max_threads = std::strtoull(argv[4], nullptr, 10);
    const bool threadgroup_multiple = std::strtoull(argv[5], nullptr, 10) != 0;
    std::cout << bioseq::gpu::metal_archive::build_binary_archive(
        metallib_path,
        archive_path,
        entry_name,
        max_threads,
        threadgroup_multiple);
    return 0;
}
