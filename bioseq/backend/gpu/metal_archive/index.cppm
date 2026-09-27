/**
 * @file metal_archive/index.cppm
 * @brief Builds Metal binary archives and captures MoltenVK command-library  source.
 */
module;

#include <cstdint>
#include <string>
#include <string_view>

export module bioseq.gpu.metal_archive;

export namespace bioseq::gpu::metal_archive {

/** @brief Protocol version printed in the archive-builder transcript. */
inline constexpr std::uint32_t k_archive_helper_wire_revision = 2u;
/** @brief Helper identity printed beside the protocol revision. */
inline constexpr std::string_view k_archive_helper_build_identity = "bioseq_gpu_archive_builder";
/** @brief Version of the command-source capture protocol. */
inline constexpr std::uint32_t k_command_capture_wire_revision = 1u;

/**
 * @brief Builds a Metal binary archive and checks that it supplies the requested pipeline.
 *
 * Run this in a fresh process started with `exec`, so it cannot inherit Metal,
 * dispatch, or XPC state from a forked database backend. The caller may publish
 * the private output file only after the transcript ends in `RESULT: ok`.
 *
 * @param[in] metallib_path Readable Metal-library  file.
 * @param[in] archive_path Separate private output path.
 * @param[in] entry_name Compute entry-point name.
 * @param[in] maximum_total_threads_per_threadgroup Positive thread-group limit.
 * @param[in] is_threadgroup_size_multiple_of_thread_execution_width Whether the
 *         group size is a multiple of the device execution width.
 * @return Build and validation transcript.
 * @warning Call only from a fresh process started with `exec`.
 */
[[nodiscard]] auto build_binary_archive(std::string_view metallib_path,
                                        std::string_view archive_path,
                                        std::string_view entry_name,
                                        std::uint64_t maximum_total_threads_per_threadgroup,
                                        bool is_threadgroup_size_multiple_of_thread_execution_width)
    -> std::string;

/**
 * @brief Captures the source and compile options for MoltenVK's internal command library.
 *
 * A fresh helper, process intercepts the Metal compile request while creating
 * a Vulkan device, records it, and calls the original compiler. Exactly one
 * matching request must pass  validaion before its source is saved
 *
 * @param[in] source_output_path New private file for the UTF-8 source bytes.
 * @param[in] loader_image_path Canonical path of the runtime's selected Vulkan loader.
 * @return Transcript with compiler options and helper, device, and library
 *         identities. Ends in `RESULT: ok` after the source is written and synced.
 * @warning Call only from a fresh process started with `exec`.
 */
[[nodiscard]] auto capture_moltenvk_command_library(std::string_view source_output_path,
                                                    std::string_view loader_image_path)
    -> std::string;

} // namespace bioseq::gpu::metal_archive
