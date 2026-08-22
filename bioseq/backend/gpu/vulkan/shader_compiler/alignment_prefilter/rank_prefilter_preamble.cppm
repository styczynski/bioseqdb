/**
 * @file rank_prefilter_preamble.cppm
 * @brief Defines the shared rank-prefilter GLSL preamble.
 */
export module bioseq.gpu.vulkan.shader_compiler:alignment_prefilter_rank_prefilter_preamble;

import std;

export namespace bioseq::gpu::vulkan {

/// @brief Shared rank-prefilter GLSL ABI. Both algorithms consume byte-rank streams,
/// the canonical 20-byte job/16-byte result records, and the same bounds-aware
/// 16-byte push block. Every load helper checks the exact logical byte count.
/// algorithm bodies still reject a malformed job before making any load.
inline constexpr std::string_view k_rank_prefilter_preamble = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const uint k_status_ok = 0u;
const uint k_status_length_difference_only = 1u;
const uint k_status_unsupported = 2u;

struct prefilter_job {
    uint query_offset;
    uint target_offset;
    uint query_length;
    uint target_length;
    uint max_edits;
};

struct prefilter_result {
    uint lower_bound;
    uint status;
    uint is_accepted;
    uint diagnostics;
};

layout(std430, set = 0, binding = 0) readonly buffer query_ranks_buffer {
    uint query_rank_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer target_ranks_buffer {
    uint target_rank_words[];
};
layout(std430, set = 0, binding = 2) readonly buffer jobs_buffer {
    prefilter_job jobs[];
};
layout(std430, set = 0, binding = 3) buffer results_buffer {
    prefilter_result results[];
};

layout(push_constant) uniform push_constants {
    uint job_count;
    uint mode;
    uint query_rank_count;
    uint target_rank_count;
} pc;

bool rank_slice_is_in_bounds(uint offset, uint length, uint rank_count) {
    return offset <= rank_count && length <= rank_count - offset;
}

bool prefilter_job_is_in_bounds(prefilter_job job) {
    return rank_slice_is_in_bounds(
               job.query_offset, job.query_length, pc.query_rank_count)
        && rank_slice_is_in_bounds(
               job.target_offset, job.target_length, pc.target_rank_count);
}

uint query_rank(uint rank_index) {
    if (rank_index >= pc.query_rank_count) return 0u;
    uint word = query_rank_words[rank_index >> 2u];
    return (word >> ((rank_index & 3u) << 3u)) & 0xffu;
}

uint target_rank(uint rank_index) {
    if (rank_index >= pc.target_rank_count) return 0u;
    uint word = target_rank_words[rank_index >> 2u];
    return (word >> ((rank_index & 3u) << 3u)) & 0xffu;
}

// These unchecked readers are only used after prefilter_job_is_in_bounds has
// validated the complete slice and the algorithm has bounded its relative index
// to that slice. Centralized validation avoids repeating four push-constant
// comparisons for every byte/quad in short hot kernels.
uint query_rank_unchecked(uint rank_index) {
    uint word = query_rank_words[rank_index >> 2u];
    return (word >> ((rank_index & 3u) << 3u)) & 0xffu;
}

uint target_rank_unchecked(uint rank_index) {
    uint word = target_rank_words[rank_index >> 2u];
    return (word >> ((rank_index & 3u) << 3u)) & 0xffu;
}

uint query_rank_quad_unchecked(uint rank_index) {
    uint word_index = rank_index >> 2u;
    uint bit_offset = (rank_index & 3u) << 3u;
    uint word = query_rank_words[word_index] >> bit_offset;
    if (bit_offset != 0u) {
        word |= query_rank_words[word_index + 1u] << (32u - bit_offset);
    }
    return word;
}

uint target_rank_quad_unchecked(uint rank_index) {
    uint word_index = rank_index >> 2u;
    uint bit_offset = (rank_index & 3u) << 3u;
    uint word = target_rank_words[word_index] >> bit_offset;
    if (bit_offset != 0u) {
        word |= target_rank_words[word_index + 1u] << (32u - bit_offset);
    }
    return word;
}

uint query_rank_quad(uint rank_index) {
    if (rank_index > pc.query_rank_count
        || 4u > pc.query_rank_count - rank_index) {
        return 0u;
    }
    uint word_index = rank_index >> 2u;
    uint bit_offset = (rank_index & 3u) << 3u;
    uint word = query_rank_words[word_index] >> bit_offset;
    if (bit_offset != 0u) {
        word |= query_rank_words[word_index + 1u] << (32u - bit_offset);
    }
    return word;
}

uint target_rank_quad(uint rank_index) {
    if (rank_index > pc.target_rank_count
        || 4u > pc.target_rank_count - rank_index) {
        return 0u;
    }
    uint word_index = rank_index >> 2u;
    uint bit_offset = (rank_index & 3u) << 3u;
    uint word = target_rank_words[word_index] >> bit_offset;
    if (bit_offset != 0u) {
        word |= target_rank_words[word_index + 1u] << (32u - bit_offset);
    }
    return word;
}

void write_prefilter_result(
    uint job_index,
    uint lower_bound,
    uint status,
    bool is_accepted,
    uint diagnostics)
{
    results[job_index].lower_bound = lower_bound;
    results[job_index].status = status;
    results[job_index].is_accepted = is_accepted ? 1u : 0u;
    results[job_index].diagnostics = diagnostics;
}
)GLSL";

} // namespace bioseq::gpu::vulkan
