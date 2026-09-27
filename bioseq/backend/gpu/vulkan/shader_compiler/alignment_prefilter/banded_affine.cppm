/**
 * @file banded_affine.cppm
 * @brief GPU shader for alignment scores and endpoints with affine gap penalties.
 */
export module bioseq.gpu.vulkan.shader_compiler:alignment_prefilter_banded_affine;

import std;

export namespace bioseq::gpu::vulkan {

/// @brief Finds the best local alignment score and endpoints within a diagonal band.
/// One GPU thread solves each job using dynamic programming. Workgroups take
/// further jobs when the batch exceeds the dispatch size. Inputs use one integer
/// rank per symbol, so the same shader supports different alphabets.
inline constexpr std::string_view k_alignment_extension_banded_affine_body = R"GLSL(
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

const int k_negative_infinity = -1073741824;
const uint k_status_ok = 0u;
const uint k_status_unsupported_geometry = 1u;

struct alignment_extension_job {
    uint query_offset;
    uint target_offset;
    uint query_len;
    uint target_len;
    uint band_width;
    uint flags;
    uint reserved0;
    uint reserved1;
};

struct alignment_extension_result {
    int score;
    uint query_end;
    uint target_end;
    uint status;
};

layout(std430, set = 0, binding = 0) readonly buffer query_symbols_buffer { uint query_symbols[]; };
layout(std430, set = 0, binding = 1) readonly buffer target_symbols_buffer { uint target_symbols[]; };
layout(std430, set = 0, binding = 2) readonly buffer jobs_buffer { alignment_extension_job jobs[]; };
layout(std430, set = 0, binding = 3) buffer results_buffer { alignment_extension_result results[]; };

layout(push_constant) uniform push_constants {
    uint job_count;
    uint max_query_len;
    uint max_target_len;
    uint max_band_width;
    uint alphabet_size;
    int match_score;
    int mismatch_penalty;
    int gap_open_penalty;
    int gap_extend_penalty;
    uint flags;
    uint reserved0;
} pc;

int bioseq_max3(int a, int b, int c) {
    return max(max(a, b), c);
}

int substitution_score(uint query_symbol, uint target_symbol) {
    return query_symbol == target_symbol ? pc.match_score : -pc.mismatch_penalty;
}

void reject_job(uint job_index) {
    results[job_index].score = 0;
    results[job_index].query_end = 0u;
    results[job_index].target_end = 0u;
    results[job_index].status = k_status_unsupported_geometry;
}

void solve_job(uint job_index) {
    alignment_extension_job job = jobs[job_index];
    if (job.query_len > pc.max_query_len || job.target_len > pc.max_target_len
        || job.band_width > pc.max_band_width || job.flags != 0u
        || job.reserved0 != 0u || job.reserved1 != 0u
        || pc.flags != 0u || pc.reserved0 != 0u || pc.alphabet_size == 0u) {
        reject_job(job_index);
        return;
    }

    int prev_m[compile_time_maximum_span + 1];
    int prev_i[compile_time_maximum_span + 1];
    int prev_d[compile_time_maximum_span + 1];
    int curr_m[compile_time_maximum_span + 1];
    int curr_i[compile_time_maximum_span + 1];
    int curr_d[compile_time_maximum_span + 1];

    for (uint j = 0u; j <= job.target_len; ++j) {
        prev_m[j] = 0;
        prev_i[j] = k_negative_infinity;
        prev_d[j] = k_negative_infinity;
        curr_m[j] = 0;
        curr_i[j] = k_negative_infinity;
        curr_d[j] = k_negative_infinity;
    }

    int best_score = 0;
    uint best_query_end = 0u;
    uint best_target_end = 0u;
    int gap_open_total = pc.gap_open_penalty + pc.gap_extend_penalty;

    for (uint i = 1u; i <= job.query_len; ++i) {
        curr_m[0] = 0;
        curr_i[0] = k_negative_infinity;
        curr_d[0] = k_negative_infinity;
        uint query_symbol = query_symbols[job.query_offset + i - 1u];
        if (query_symbol >= pc.alphabet_size) {
            reject_job(job_index);
            return;
        }
        for (uint j = 1u; j <= job.target_len; ++j) {
            uint delta = i > j ? (i - j) : (j - i);
            if (delta > job.band_width) {
                curr_m[j] = 0;
                curr_i[j] = k_negative_infinity;
                curr_d[j] = k_negative_infinity;
                continue;
            }

            uint target_symbol = target_symbols[job.target_offset + j - 1u];
            if (target_symbol >= pc.alphabet_size) {
                reject_job(job_index);
                return;
            }
            int diag = bioseq_max3(
                prev_m[j - 1u], prev_i[j - 1u], prev_d[j - 1u])
                + substitution_score(query_symbol, target_symbol);
            int match_state = max(0, diag);
            int insert_state = max(
                max(prev_m[j] - gap_open_total, prev_i[j] - pc.gap_extend_penalty), 0);
            int delete_state = max(
                max(curr_m[j - 1u] - gap_open_total, curr_d[j - 1u] - pc.gap_extend_penalty), 0);

            curr_m[j] = match_state;
            curr_i[j] = insert_state;
            curr_d[j] = delete_state;

            int cell_best = bioseq_max3(
                match_state, insert_state, delete_state);
            if (cell_best > best_score) {
                best_score = cell_best;
                best_query_end = i;
                best_target_end = j;
            }
        }
        for (uint j = 0u; j <= job.target_len; ++j) {
            prev_m[j] = curr_m[j];
            prev_i[j] = curr_i[j];
            prev_d[j] = curr_d[j];
        }
    }

    results[job_index].score = best_score;
    results[job_index].query_end = best_query_end;
    results[job_index].target_end = best_target_end;
    results[job_index].status = k_status_ok;
}

void main() {
    uint stride = gl_NumWorkGroups.x;
    if (stride == 0u) return;
    for (uint job_index = gl_WorkGroupID.x; job_index < pc.job_count; job_index += stride) {
        solve_job(job_index);
    }
}
)GLSL";

} // namespace bioseq::gpu::vulkan
