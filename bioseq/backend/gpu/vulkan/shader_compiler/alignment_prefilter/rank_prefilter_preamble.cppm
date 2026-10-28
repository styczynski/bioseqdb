/**
 * @file rank_prefilter_preamble.cppm
 * @brief Shared GPU input layouts and rank readers for the sequence prefilters.
 */
export module bioseq.gpu.vulkan.shader_compiler:alignment_prefilter_rank_prefilter_preamble;

import std;

export namespace bioseq::gpu::vulkan {

/** Native packed descriptor and byte-safe decoder shared by GPU alignment kernels. */
inline constexpr std::string_view k_native_packed_sequence_reader = R"GLSL(
struct gpu_sequence_view {
    uint64_t seq_word_offset;
    uint64_t byte_count;
    uint64_t padded_byte_count;
    uint64_t unit_count;
    uint64_t bp_offset;
    uint64_t bp_count;
    uint letters_per_unit;
    uint bits_per_char;
    uint unit_bits;
    uint alphabet_size;
    uint char_mask;
    uint padding_letters;
    uint alphabet_kind;
    uint leading_symbol_offset;
    uint alphabet_code;
    uint seed_policy_id;
};
uint packed_sequence_byte(uint64_t byte_index, bool query);
uint native_packed_rank(gpu_sequence_view sequence, uint position, bool query) {
    uint64_t slot = uint64_t(sequence.leading_symbol_offset) + uint64_t(position);
    uint64_t unit_index = slot / uint64_t(sequence.letters_per_unit);
    uint bit_offset = uint(slot % uint64_t(sequence.letters_per_unit)) * sequence.bits_per_char;
    uint64_t byte_index = sequence.seq_word_offset * uint64_t(4)
        + unit_index * uint64_t(sequence.unit_bits / 8u) + uint64_t(bit_offset / 8u);
    uint within_byte = bit_offset & 7u;
    uint rank = packed_sequence_byte(byte_index, query) >> within_byte;
    if (within_byte + sequence.bits_per_char > 8u)
        rank |= packed_sequence_byte(byte_index + uint64_t(1), query) << (8u - within_byte);
    return rank & sequence.char_mask;
}
)GLSL";

/** Shared packed input, checked jobs and register-only rank quads for prefilters. */
inline constexpr std::string_view k_rank_prefilter_preamble = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;
const uint k_status_ok = 0u;
const uint k_status_length_difference_only = 1u;
const uint k_status_unsupported = 2u;
struct prefilter_job {
    uint query_record_index;
    uint target_record_index;
    uint query_start;
    uint target_start;
    uint query_length;
    uint target_length;
    uint max_edits;
    uint orientation;
};
struct prefilter_result { uint lower_bound; uint status; uint is_accepted; uint diagnostics; };
layout(std430, set = 0, binding = 0) readonly buffer packed_words_buffer { uint packed_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer query_records_buffer { gpu_sequence_view query_records[]; };
layout(std430, set = 0, binding = 2) readonly buffer jobs_buffer { prefilter_job jobs[]; };
layout(std430, set = 0, binding = 3) buffer results_buffer { prefilter_result results[]; };
layout(std430, set = 0, binding = 4) readonly buffer target_records_buffer { gpu_sequence_view target_records[]; };
layout(std430, set = 0, binding = 5) readonly buffer rank_lookup_buffer {
    uint rank_to_code[256];
    uint rank_to_rc_code[256];
    uint forward_seeds[256];
    uint rank_count; uint alphabet_code; uint seed_policy_id; uint canonical_reverse_complement;
};
layout(push_constant) uniform push_constants {
    uint job_count; uint mode; uint query_record_count; uint target_record_count;
} pc;
uint packed_sequence_byte(uint64_t byte_index, bool query) {
    return (packed_words[uint(byte_index >> 2u)] >> ((uint(byte_index) & 3u) * 8u)) & 255u;
}
bool prefilter_job_is_in_bounds(prefilter_job job) {
    if (job.query_record_index >= pc.query_record_count || job.target_record_index >= pc.target_record_count)
        return false;
    gpu_sequence_view query = query_records[job.query_record_index];
    gpu_sequence_view target = target_records[job.target_record_index];
    return uint64_t(job.query_start) <= query.bp_count
        && uint64_t(job.query_length) <= query.bp_count - uint64_t(job.query_start)
        && uint64_t(job.target_start) <= target.bp_count
        && uint64_t(job.target_length) <= target.bp_count - uint64_t(job.target_start)
        && (job.orientation == 0u || (job.orientation == 3u && canonical_reverse_complement != 0u));
}
uint query_rank_unchecked(prefilter_job job, uint rank_index) {
    gpu_sequence_view sequence = query_records[job.query_record_index];
    if (job.orientation == 3u) {
        uint rank = native_packed_rank(sequence, uint(sequence.bp_count) - 1u - rank_index, true);
        return rank_to_rc_code[rank];
    }
    return native_packed_rank(sequence, rank_index, true);
}
uint target_rank_unchecked(prefilter_job job, uint rank_index) {
    return native_packed_rank(target_records[job.target_record_index], rank_index, false);
}
uint query_rank(prefilter_job job, uint rank_index) { return query_rank_unchecked(job, rank_index); }
uint target_rank(prefilter_job job, uint rank_index) { return target_rank_unchecked(job, rank_index); }
uint query_rank_quad_unchecked(prefilter_job job, uint rank_index) {
    return query_rank_unchecked(job, rank_index)
        | (query_rank_unchecked(job, rank_index + 1u) << 8u)
        | (query_rank_unchecked(job, rank_index + 2u) << 16u)
        | (query_rank_unchecked(job, rank_index + 3u) << 24u);
}
uint target_rank_quad_unchecked(prefilter_job job, uint rank_index) {
    return target_rank_unchecked(job, rank_index)
        | (target_rank_unchecked(job, rank_index + 1u) << 8u)
        | (target_rank_unchecked(job, rank_index + 2u) << 16u)
        | (target_rank_unchecked(job, rank_index + 3u) << 24u);
}
uint query_rank_quad(prefilter_job job, uint rank_index) { return query_rank_quad_unchecked(job, rank_index); }
uint target_rank_quad(prefilter_job job, uint rank_index) { return target_rank_quad_unchecked(job, rank_index); }
void write_prefilter_result(uint job_index, uint lower_bound, uint status, bool is_accepted, uint diagnostics) {
    results[job_index].lower_bound = lower_bound;
    results[job_index].status = status;
    results[job_index].is_accepted = is_accepted ? 1u : 0u;
    results[job_index].diagnostics = diagnostics;
}
)GLSL";

} // namespace bioseq::gpu::vulkan
