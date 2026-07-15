#include "gpu/metal_archive.hpp"

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
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
    char* end = nullptr;
    errno = 0;
    const std::uint64_t max_threads = std::strtoull(argv[4], &end, 10);
    if (errno != 0 || end == argv[4] || *end != '\0'
        || argv[4][0] == '-' || max_threads == 0u || max_threads > 1024u
        || (std::string_view{argv[5]} != "0"
            && std::string_view{argv[5]} != "1")
        || metallib_path.empty() || archive_path.empty() || entry_name.empty()
        || metallib_path == archive_path) {
        std::cerr << "invalid archive builder arguments\n";
        return 2;
    }
    const bool threadgroup_multiple = std::string_view{argv[5]} == "1";
    const std::string transcript =
        bioseq::gpu::metal_archive::build_binary_archive(
        metallib_path,
        archive_path,
        entry_name,
        max_threads,
        threadgroup_multiple);
    std::cout << transcript;
    constexpr std::string_view success = "RESULT: ok\n";
    const std::size_t result_marker = transcript.find("RESULT:");
    return transcript.size() >= success.size()
            && std::string_view{transcript}.substr(
                   transcript.size() - success.size()) == success
            && result_marker == transcript.size() - success.size()
        ? 0
        : 1;
}
