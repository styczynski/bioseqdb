/**
 * @file alignment_prefilter.cppm
 * @brief Re-exports alignment and rank-prefilter shader sources.
 */
export module bioseq.gpu.vulkan.shader_compiler:alignment_prefilter;

export import :alignment_prefilter_rank_prefilter_preamble;
export import :alignment_prefilter_sneaky_snake;
export import :alignment_prefilter_gatekeeper;
export import :alignment_prefilter_banded_affine;
export import :alignment_prefilter_simple_cigar;

import :registry;
import bioseq.common.logging;
import std;

export namespace bioseq::gpu::vulkan {
/**
 * @brief Composes a rank-prefilter shader from its shared preamble and algorithm body.
 * @param[in] algorithm_body Algorithm-specific GLSL body.
 * @return Complete rank-prefilter GLSL body.
 */
[[nodiscard]] std::string rank_prefilter_body(std::string_view algorithm_body) {
    std::string body;
    body.reserve(k_rank_prefilter_preamble.size() + algorithm_body.size());
    body += k_rank_prefilter_preamble;
    body += algorithm_body;
    return body;
}

/**
 * @brief Composes one fixed-geometry GateKeeper shader body.
 * @param[in] length Compile-time sequence length.
 * @param[in] max_edits Compile-time edit threshold.
 * @return Complete rank-prefilter GLSL body.
 */
[[nodiscard]] std::string specialised_gatekeeper_body(std::uint32_t length,
                                                      std::uint32_t max_edits) {
    std::string body;
    body.reserve(k_rank_prefilter_preamble.size() + k_gatekeeper_rank_batch_body.size() + 96u);
    body += k_rank_prefilter_preamble;
    body += fmt::format("#define gatekeeper_specialized_length {}\n"
                        "#define gatekeeper_specialized_edits {}\n",
                        length,
                        max_edits);
    body += k_gatekeeper_rank_batch_body;
    return body;
}
} // namespace bioseq::gpu::vulkan
