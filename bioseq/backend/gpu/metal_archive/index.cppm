/**
 * @file metal_archive/index.cppm
 * @brief Public Metal binary-archive construction contract.
 */
module;

#include <cstdint>
#include <string>
#include <string_view>

export module bioseq.gpu.metal_archive;

export namespace bioseq::gpu::metal_archive {

/** @brief Protocol revision printed in the archive-builder qualification transcript. */
inline constexpr std::uint32_t k_archive_helper_wire_revision = 2u;
/** @brief Helper identity printed beside the protocol revision. */
inline constexpr std::string_view k_archive_helper_build_identity = "bioseq_gpu_archive_builder";

/**
 * @brief Creates and qualifies a Metal binary archive for one compute entry
 * point.
 *
 * The caller supplies a private temporary output path and publishes the file
 * only after observing the terminal `RESULT: ok` sentinel. This function must
 * run in an exec-clean child so no Metal, dispatch, or XPC state is inherited
 * from a forked database backend.
 *
 * @param[in] metallib_path Readable Metal-library input path.
 * @param[in] archive_path Distinct private path for the serialized archive.
 * @param[in] entry_name Compute entry-point name stored in the pipeline
 * descriptor.
 * @param[in] maximum_total_threads_per_threadgroup Positive Metal thread-group
 * limit.
 * @param[in] is_threadgroup_size_multiple_of_thread_execution_width Whether the
 * entry-point thread-group size is a multiple of the device execution width.
 * @return A line-oriented qualification transcript.
 * @warning Call only from an exec-clean process.
 */
[[nodiscard]] auto build_binary_archive(std::string_view metallib_path,
                                        std::string_view archive_path,
                                        std::string_view entry_name,
                                        std::uint64_t maximum_total_threads_per_threadgroup,
                                        bool is_threadgroup_size_multiple_of_thread_execution_width)
    -> std::string;

} // namespace bioseq::gpu::metal_archive
