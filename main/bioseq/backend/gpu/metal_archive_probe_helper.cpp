#include "gpu/metal_archive_probe.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 4 && argc != 7) {
        std::cerr << "usage: bioseq_gpu_pg_probe_helper <mode> <metallib_path> <archive_path>"
                     " [entry_name max_total_threads_per_threadgroup threadgroup_multiple]\n";
        return 2;
    }

    const std::string_view mode = argv[1];
    const std::string_view metallib_path = argv[2];
    const std::string_view archive_path = argv[3];
    if (argc == 4) {
        std::cout << bioseq::gpu::metal_probe::run_archive_probe(
            mode,
            metallib_path,
            archive_path);
        return 0;
    }

    const std::string_view entry_name = argv[4];
    const std::uint64_t max_threads = std::strtoull(argv[5], nullptr, 10);
    const bool threadgroup_multiple = std::strtoull(argv[6], nullptr, 10) != 0;
    std::cout << bioseq::gpu::metal_probe::run_archive_probe(
        mode,
        metallib_path,
        archive_path,
        entry_name,
        max_threads,
        threadgroup_multiple);
    return 0;
}
