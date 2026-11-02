/**
 * @file alignment_prefilter.cppm
 * @brief Exports alignment shaders and assembles the sequence-prefilter shaders.
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
/** Matrix and packed descriptors shared by both existing DP kernels. */
inline constexpr std::string_view k_alignment_native_bindings = R"GLSL(
layout(std430, set=0, binding=6) readonly buffer substitution_matrix_buffer {
    uint model; uint alphabet_code; uint rank_count; uint row_stride;
    uint64_t semantic_identity; uint element_count; uint reserved;
    int values[];
} substitution;
layout(std430, set=0, binding=7) readonly buffer sequence_descriptor_buffer {
    gpu_sequence_view sequence_descriptors[];
};
bool alignment_numeric_contract(uint query_length, uint target_length,
    int match_score, int mismatch_penalty, int gap_open, int gap_extend,
    int gap_open_long, int gap_extend_long, int end_bonus, int z_drop) {
    if (substitution.model > 1u || substitution.reserved != 0u
        || gap_open < 0 || gap_extend <= 0 || gap_open_long < 0 || gap_extend_long <= 0)
        return false;
    int64_t magnitude = max(abs(int64_t(match_score)), abs(int64_t(mismatch_penalty)));
    if (substitution.model == 1u) {
        if (substitution.row_stride != substitution.rank_count
            || uint64_t(substitution.element_count) != uint64_t(substitution.rank_count) * uint64_t(substitution.rank_count))
            return false;
        magnitude = int64_t(0);
        for (uint index = 0u; index < substitution.element_count; ++index) {
            int64_t value = int64_t(substitution.values[index]);
            if (value < int64_t(-127) || value > int64_t(127)) return false;
            magnitude = max(magnitude, abs(value));
        }
    } else if (substitution.row_stride != 0u || substitution.element_count != 0u
        || magnitude > int64_t(127)) return false;
    int64_t maximum_open = max(int64_t(gap_open), int64_t(gap_open_long));
    int64_t maximum_extend = max(int64_t(gap_extend), int64_t(gap_extend_long));
    int64_t step_cost = magnitude + maximum_open + maximum_extend;
    int64_t extras = abs(int64_t(end_bonus)) + max(int64_t(z_drop), int64_t(0));
    int64_t limit = int64_t(1) << 29;
    int64_t steps = int64_t(query_length) + int64_t(target_length);
    return extras < limit && step_cost > int64_t(0)
        && steps <= (limit - int64_t(1) - extras) / step_cost;
}
)GLSL";

/** Assemble native readers and scoring transport without changing push constants. */
[[nodiscard]] std::string native_alignment_body(std::string_view algorithm_body) {
    std::string body;
    body.reserve(k_native_packed_sequence_reader.size() + k_alignment_native_bindings.size() + algorithm_body.size());
    body += k_native_packed_sequence_reader;
    body += k_alignment_native_bindings;
    body += algorithm_body;
    return body;
}

/**
 * @brief Adds the shared input layouts and  rank readers before an algorithm's GLSL body.
 */
[[nodiscard]] std::string rank_prefilter_body(std::string_view algorithm_body) {
    std::string body;
    body.reserve(k_native_packed_sequence_reader.size() + k_native_packed_cursor_reader.size()
        + k_rank_prefilter_preamble.size() + algorithm_body.size());
    body += k_native_packed_sequence_reader;
    body += k_native_packed_cursor_reader;
    body += k_rank_prefilter_preamble;
    body += algorithm_body;
    return body;
}

/**
 * @brief Builds GateKeeper GLSL for a fixed sequence length and edit limit.
 * Inserts those values as defines between the shared preamble and algorithm body.
 */
[[nodiscard]] std::string specialised_gatekeeper_body(std::uint32_t length,
                                                      std::uint32_t max_edits) {
    std::string body;
    body.reserve(k_native_packed_sequence_reader.size() + k_native_packed_cursor_reader.size()
        + k_rank_prefilter_preamble.size() + k_gatekeeper_rank_batch_body.size() + 96u);
    body += k_native_packed_sequence_reader;
    body += k_native_packed_cursor_reader;
    body += k_rank_prefilter_preamble;
    body += fmt::format("#define gatekeeper_specialized_length {}\n"
                        "#define gatekeeper_specialized_edits {}\n",
                        length,
                        max_edits);
    body += k_gatekeeper_rank_batch_body;
    return body;
}
} // namespace bioseq::gpu::vulkan
