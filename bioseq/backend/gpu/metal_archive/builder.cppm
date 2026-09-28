/**
 * @file metal_archive/builder.cppm
 * @brief Command-line interface for building Metal archives and capturing MoltenVK source.
 */
module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unistd.h>

export module bioseq.gpu.metal_archive.builder;

import bioseq.gpu.metal_archive;

namespace bioseq::gpu::metal_archive {
namespace {

/**
 * @brief Writes the transcript, retrying partial writes and interrupted calls.
 *
 * Stops if wriping fails for another reason.
 */
void write_process_output(
        const int descriptor,
        const std::string_view bytes) noexcept {
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written =
            ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

} // namespace

/**
 * @brief Runs the archive-builder or command-source capture command.
 *
 * Arguments are checked before calling the helper. The  transcript is written
 * to standard output, and success requires exactly one `RESULT:` marker with
 * `RESULT: ok` at the end.
 * @return Zero for success, one for a failed build or capture, or two for
 *         invalid arguments.
 */
export int run_binary_archive_builder(
        const int argument_count,
        char** const arguments) {
    if (argument_count == 4 &&
        std::string_view{arguments[1]} == "--capture-moltenvk-command" &&
        arguments[2] != nullptr && arguments[2][0] != '\0' &&
        arguments[3] != nullptr && arguments[3][0] != '\0') {
        const std::string transcript =
            capture_moltenvk_command_library(arguments[2], arguments[3]);
        write_process_output(STDOUT_FILENO, transcript);
        constexpr std::string_view success_sentinel = "RESULT: ok\n";
        const std::size_t result_marker = transcript.find("RESULT:");
        return transcript.size() >= success_sentinel.size() &&
                       std::string_view{transcript}.substr(
                           transcript.size() - success_sentinel.size()) == success_sentinel &&
                       result_marker == transcript.size() - success_sentinel.size()
                   ? 0
                   : 1;
    }
    if (argument_count != 6) {
        constexpr std::string_view usage_text =
            "usage: bioseq_gpu_archive_builder <metallib_path> <archive_path>"
            " <entry_name> <max_total_threads_per_threadgroup> "
            "<threadgroup_multiple>\n"
            "   or: bioseq_gpu_archive_builder --capture-moltenvk-command "
            "<source_output_path> <loader_image_path>\n";
        write_process_output(STDERR_FILENO, usage_text);
        return 2;
    }

    const std::string_view metallib_path = arguments[1];
    const std::string_view archive_path = arguments[2];
    const std::string_view entry_name = arguments[3];
    char* parse_end = nullptr;
    errno = 0;
    const std::uint64_t maximum_total_threads_per_threadgroup =
        std::strtoull(arguments[4], &parse_end, 10);
    const bool threadgroup_multiple_is_valid =
        std::string_view{arguments[5]} == "0"
        || std::string_view{arguments[5]} == "1";
    if (errno != 0
        || parse_end == arguments[4]
        || *parse_end != '\0'
        || arguments[4][0] == '-'
        || maximum_total_threads_per_threadgroup == 0u
        || maximum_total_threads_per_threadgroup > 1024u
        || !threadgroup_multiple_is_valid
        || metallib_path.empty()
        || archive_path.empty()
        || entry_name.empty()
        || metallib_path == archive_path) {
        constexpr std::string_view invalid_arguments_text =
            "invalid archive builder arguments\n";
        write_process_output(STDERR_FILENO, invalid_arguments_text);
        return 2;
    }

    const bool is_threadgroup_size_multiple_of_thread_execution_width =
        std::string_view{arguments[5]} == "1";
    const std::string transcript = build_binary_archive(
        metallib_path,
        archive_path,
        entry_name,
        maximum_total_threads_per_threadgroup,
        is_threadgroup_size_multiple_of_thread_execution_width);
    write_process_output(STDOUT_FILENO, transcript);

    constexpr std::string_view success_sentinel = "RESULT: ok\n";
    const std::size_t result_marker = transcript.find("RESULT:");
    return transcript.size() >= success_sentinel.size()
            && std::string_view{transcript}.substr(
                transcript.size() - success_sentinel.size()) == success_sentinel
            && result_marker == transcript.size() - success_sentinel.size()
        ? 0
        : 1;
}

} // namespace bioseq::gpu::metal_archive
