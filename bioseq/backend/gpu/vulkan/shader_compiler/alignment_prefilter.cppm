/**
 * @file alignment_prefilter.cppm
 * @brief Owns rank-prefilter and alignment-extension compute shader bodies.
 */
module bioseq.gpu.vulkan.shader_compiler:alignment_prefilter;

import :registry;
import bioseq.common.logging;
import std;

namespace bioseq::gpu::vulkan {

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

/// @brief Rank-buffer sneaky_snake_buffer lower-bound filter. A uniform push-constant mode
/// keeps low-edit batches on independent scalar invocations and assigns high-
/// edit batches to fixed eight-lane logical teams. Equal-length inputs use
/// bounded compile_time_maximum_span-symbol submazes. Unequal and unsupported shapes remain
/// conservative. Team-mode barriers are uniform and the implementation has no
/// subgroup-size assumption or atomics.
inline constexpr std::string_view k_sneaky_snake_rank_batch_body = R"GLSL(
const uint k_maximum_supported_edits = 31u;
const uint k_team_size = 8u;
const uint k_team_count = compile_time_workgroup_size_x / k_team_size;
shared prefilter_job sh_job[k_team_count];
shared uint sh_position[k_team_count];
shared uint sh_submaze_end[k_team_count];
shared uint sh_escape_round[k_team_count];
shared uint sh_lower_bound[k_team_count];
shared uint sh_active[k_team_count];
shared uint sh_lane_best[compile_time_workgroup_size_x];
shared uint sh_any_active;

uint matching_run(
    prefilter_job job,
    uint position,
    uint submaze_end,
    uint query_cursor)
{
    uint run_limit = min(
        submaze_end - position,
        job.query_length - query_cursor);
    uint run = 0u;
    uint target_start = job.target_offset + position;
    uint query_start = job.query_offset + query_cursor;
    while (run < run_limit && ((target_start + run) & 3u) != 0u) {
        if (target_rank(target_start + run)
            != query_rank(query_start + run)) {
            return run;
        }
        ++run;
    }
    while (run_limit - run >= 4u) {
        uint query_word = query_rank_quad(query_start + run);
        uint target_word = target_rank_words[(target_start + run) >> 2u];
        uint different = query_word ^ target_word;
        if (different != 0u) {
            return run + (uint(findLSB(different)) >> 3u);
        }
        run += 4u;
    }
    while (run < run_limit) {
        if (target_rank(target_start + run)
            != query_rank(query_start + run)) {
            break;
        }
        ++run;
    }
    return run;
}

void write_result(
    uint job_index,
    uint lower_bound,
    uint status,
    bool is_accepted)
{
    write_prefilter_result(
        job_index, lower_bound, status, is_accepted, 0u);
}

void solve_job_scalar(uint job_index) {
    prefilter_job job = jobs[job_index];
    if (!prefilter_job_is_in_bounds(job)
        || job.max_edits > k_maximum_supported_edits || compile_time_maximum_span == 0) {
        write_result(job_index, 0u, k_status_unsupported, true);
        return;
    }

    if (job.query_length != job.target_length) {
        uint length_difference = job.query_length > job.target_length
            ? job.query_length - job.target_length
            : job.target_length - job.query_length;
        write_result(
            job_index,
            length_difference,
            k_status_length_difference_only,
            length_difference <= job.max_edits);
        return;
    }

    uint lower_bound = 0u;
    uint submaze_begin = 0u;
    while (submaze_begin < job.query_length) {
        uint submaze_end = submaze_begin + min(
            uint(compile_time_maximum_span), job.query_length - submaze_begin);
        uint position = submaze_begin;
        uint escape_round = 0u;

        while (position < submaze_end && escape_round < uint(compile_time_maximum_span)) {
            uint remaining_in_submaze = submaze_end - position;
            uint best_run = matching_run(
                job, position, submaze_end, position);
            uint max_shift = min(job.max_edits, job.query_length);
            for (uint shift = 1u;
                 shift <= max_shift && best_run < remaining_in_submaze;
                 ++shift) {
                if (shift <= position) {
                    best_run = max(
                        best_run,
                        matching_run(
                            job,
                            position,
                            submaze_end,
                            position - shift));
                }
                if (best_run == remaining_in_submaze) break;
                if (shift < job.query_length - position) {
                    best_run = max(
                        best_run,
                        matching_run(
                            job,
                            position,
                            submaze_end,
                            position + shift));
                }
            }

            if (best_run == remaining_in_submaze) {
                position = submaze_end;
                break;
            }

            ++lower_bound;
            if (lower_bound > job.max_edits) {
                write_result(job_index, lower_bound, k_status_ok, false);
                return;
            }
            position += best_run + 1u;
            ++escape_round;
        }
        submaze_begin = submaze_end;
    }

    write_result(job_index, lower_bound, k_status_ok, true);
}

void main() {
    if (pc.mode != k_team_size) {
        uint64_t stride = uint64_t(gl_NumWorkGroups.x)
            * uint64_t(gl_WorkGroupSize.x);
        if (stride == uint64_t(0)) return;
        for (uint64_t job_index = uint64_t(gl_GlobalInvocationID.x);
             job_index < uint64_t(pc.job_count);
             job_index += stride) {
            solve_job_scalar(uint(job_index));
        }
        return;
    }

    uint lane = gl_LocalInvocationID.x;
    uint team = lane / k_team_size;
    uint team_lane = lane & (k_team_size - 1u);
    uint64_t block_stride = uint64_t(gl_NumWorkGroups.x)
        * uint64_t(k_team_count);
    if (block_stride == uint64_t(0)) return;

    for (uint64_t block = uint64_t(gl_WorkGroupID.x)
             * uint64_t(k_team_count);
         block < uint64_t(pc.job_count);
         block += block_stride) {
        uint64_t job_wide = block + uint64_t(team);
        bool team_has_job = job_wide < uint64_t(pc.job_count);
        uint job_index = uint(job_wide);

        if (team_lane == 0u) {
            sh_active[team] = 0u;
            if (team_has_job) {
                prefilter_job job = jobs[job_index];
                sh_job[team] = job;
                if (!prefilter_job_is_in_bounds(job)
                    || job.max_edits > k_maximum_supported_edits
                    || compile_time_maximum_span == 0) {
                    write_result(job_index, 0u, k_status_unsupported, true);
                } else if (job.query_length != job.target_length) {
                    uint length_difference = job.query_length > job.target_length
                        ? job.query_length - job.target_length
                        : job.target_length - job.query_length;
                    write_result(
                        job_index,
                        length_difference,
                        k_status_length_difference_only,
                        length_difference <= job.max_edits);
                } else if (job.query_length == 0u) {
                    write_result(job_index, 0u, k_status_ok, true);
                } else {
                    sh_position[team] = 0u;
                    sh_submaze_end[team] = min(
                        uint(compile_time_maximum_span), job.query_length);
                    sh_escape_round[team] = 0u;
                    sh_lower_bound[team] = 0u;
                    sh_active[team] = 1u;
                }
            }
        }
        barrier();

        while (true) {
            if (lane == 0u) {
                sh_any_active = 0u;
                for (uint slot = 0u; slot < k_team_count; ++slot) {
                    sh_any_active |= sh_active[slot];
                }
            }
            barrier();
            if (sh_any_active == 0u) break;

            uint local_best = 0u;
            if (sh_active[team] != 0u) {
                prefilter_job job = sh_job[team];
                uint position = sh_position[team];
                uint submaze_end = sh_submaze_end[team];
                uint diagonal_count = 2u * job.max_edits + 1u;
                for (uint ordinal = team_lane;
                     ordinal < diagonal_count;
                     ordinal += k_team_size) {
                    uint query_cursor = position;
                    bool valid = true;
                    if (ordinal != 0u) {
                        uint shift = (ordinal + 1u) >> 1u;
                        bool before = (ordinal & 1u) != 0u;
                        if (before) {
                            if (shift > position) {
                                valid = false;
                            } else {
                                query_cursor = position - shift;
                            }
                        } else {
                            if (shift >= job.query_length - position) {
                                valid = false;
                            } else {
                                query_cursor = position + shift;
                            }
                        }
                    }
                    if (valid) {
                        local_best = max(
                            local_best,
                            matching_run(
                                job,
                                position,
                                submaze_end,
                                query_cursor));
                    }
                }
            }
            sh_lane_best[lane] = local_best;
            barrier();

            if (team_lane == 0u && sh_active[team] != 0u) {
                uint best_run = 0u;
                uint first_lane = team * k_team_size;
                for (uint offset = 0u; offset < k_team_size; ++offset) {
                    best_run = max(
                        best_run,
                        sh_lane_best[first_lane + offset]);
                }

                prefilter_job job = sh_job[team];
                uint position = sh_position[team];
                uint submaze_end = sh_submaze_end[team];
                uint remaining_in_submaze = submaze_end - position;
                if (best_run == remaining_in_submaze) {
                    sh_position[team] = submaze_end;
                } else {
                    uint lower_bound = sh_lower_bound[team] + 1u;
                    sh_lower_bound[team] = lower_bound;
                    if (lower_bound > job.max_edits) {
                        write_result(
                            job_index, lower_bound, k_status_ok, false);
                        sh_active[team] = 0u;
                    } else {
                        sh_position[team] = position + best_run + 1u;
                        sh_escape_round[team] += 1u;
                    }
                }

                if (sh_active[team] != 0u
                    && (sh_position[team] >= submaze_end
                        || sh_escape_round[team] >= uint(compile_time_maximum_span))) {
                    uint next_begin = submaze_end;
                    if (next_begin >= job.query_length) {
                        write_result(
                            job_index,
                            sh_lower_bound[team],
                            k_status_ok,
                            true);
                        sh_active[team] = 0u;
                    } else {
                        sh_position[team] = next_begin;
                        sh_submaze_end[team] = next_begin + min(
                            uint(compile_time_maximum_span),
                            job.query_length - next_begin);
                        sh_escape_round[team] = 0u;
                    }
                }
            }
            barrier();
        }
    }
}
)GLSL";

/// @brief Clean-room GateKeeper threshold filter over caller-normalised rank bytes.
/// One invocation owns one job. Mismatch masks stream through four uint words.
/// a mask rejection is authoritative only after the exact fixed-band guard.
inline constexpr std::string_view k_gatekeeper_rank_batch_body = R"GLSL(
const uint k_maximum_gatekeeper_length = 128u;
const uint k_maximum_gatekeeper_edits = 8u;
const uint k_mask_rejection_candidate_mask = 0x00010000u;
const uint k_exact_guard_activated_mask = 0x00020000u;
const uint k_guard_confirmed_rejection_mask = 0x00040000u;
const uint k_hamming_exact_rejection_mask = 0x00080000u;

uint packed_byte_mismatch_bits(uint difference) {
    // Fold every byte independently to its low bit, then compact byte-low bits
    // 0/8/16/24 into result bits 0/1/2/3. The largest cumulative right shift
    // is seven, so a set bit never crosses a byte boundary while folding.
    difference |= difference >> 4u;
    difference |= difference >> 2u;
    difference |= difference >> 1u;
    uint byte_low_bits = difference & 0x01010101u;
    return (byte_low_bits & 0x1u)
        | ((byte_low_bits >> 7u) & 0x2u)
        | ((byte_low_bits >> 14u) & 0x4u)
        | ((byte_low_bits >> 21u) & 0x8u);
}

void set_mask_bit(inout uvec4 mask, uint position) {
    mask[position >> 5u] |= 1u << (position & 31u);
}

void append_mismatch_quad(
    inout uvec4 mask,
    uint output_position,
    uint mismatch_bits)
{
    uint word_index = output_position >> 5u;
    uint word_shift = output_position & 31u;
    mask[word_index] |= mismatch_bits << word_shift;
    if (word_shift > 28u && word_index + 1u < 4u) {
        mask[word_index + 1u] |= mismatch_bits >> (32u - word_shift);
    }
}

uvec4 mismatch_mask_segment(
    prefilter_job job,
    uint query_relative,
    uint target_relative,
    uint count,
    uint output_position)
{
    uvec4 mask = uvec4(0u);
    uint position = 0u;
    while (count - position >= 4u) {
        uint query_word = query_rank_quad_unchecked(
            job.query_offset + query_relative + position);
        uint target_word = target_rank_quad_unchecked(
            job.target_offset + target_relative + position);
        append_mismatch_quad(
            mask,
            output_position + position,
            packed_byte_mismatch_bits(query_word ^ target_word));
        position += 4u;
    }
    while (position < count) {
        if (query_rank_unchecked(
                job.query_offset + query_relative + position)
            != target_rank_unchecked(
                job.target_offset + target_relative + position)) {
            set_mask_bit(mask, output_position + position);
        }
        ++position;
    }
    return mask;
}

// Aligned production jobs load each rank word exactly once. Diagonal masks are
// then formed from register-resident packed bytes in four-word vectors; the E2
// and E4 paths perform 16 and 32 total scalar loads, respectively. Unaligned
// direct-API jobs use the generic bounds-aware path below.
struct packed_ranks_32 {
    uvec4 first;
    uvec4 second;
};

struct packed_ranks_64 {
    uvec4 first;
    uvec4 second;
    uvec4 third;
    uvec4 fourth;
};

// Raw (unamended, boundary-free) mismatch masks indexed by exact wavefront
// diagonal k=query_position-target_position. L32 uses only the low component.
struct exact_diagonal_masks {
    uvec2 negative_two;
    uvec2 negative_one;
    uvec2 zero;
    uvec2 positive_one;
    uvec2 positive_two;
};

uvec4 compact_mismatch_nibbles(uvec4 difference) {
    difference |= difference >> 4u;
    difference |= difference >> 2u;
    difference |= difference >> 1u;
    uvec4 byte_low_bits = difference & uvec4(0x01010101u);
    return (byte_low_bits & uvec4(0x1u))
        | ((byte_low_bits >> 7u) & uvec4(0x2u))
        | ((byte_low_bits >> 14u) & uvec4(0x4u))
        | ((byte_low_bits >> 21u) & uvec4(0x8u));
}

uint pack_mismatch_nibbles(uvec4 nibbles) {
    return nibbles.x | (nibbles.y << 4u)
        | (nibbles.z << 8u) | (nibbles.w << 12u);
}

packed_ranks_32 load_query_ranks_32(prefilter_job job) {
    uint base = job.query_offset >> 2u;
    packed_ranks_32 ranks;
    ranks.first = uvec4(
        query_rank_words[base], query_rank_words[base + 1u],
        query_rank_words[base + 2u], query_rank_words[base + 3u]);
    ranks.second = uvec4(
        query_rank_words[base + 4u], query_rank_words[base + 5u],
        query_rank_words[base + 6u], query_rank_words[base + 7u]);
    return ranks;
}

packed_ranks_32 load_target_ranks_32(prefilter_job job) {
    uint base = job.target_offset >> 2u;
    packed_ranks_32 ranks;
    ranks.first = uvec4(
        target_rank_words[base], target_rank_words[base + 1u],
        target_rank_words[base + 2u], target_rank_words[base + 3u]);
    ranks.second = uvec4(
        target_rank_words[base + 4u], target_rank_words[base + 5u],
        target_rank_words[base + 6u], target_rank_words[base + 7u]);
    return ranks;
}

packed_ranks_64 load_query_ranks_64(prefilter_job job) {
    uint base = job.query_offset >> 2u;
    packed_ranks_64 ranks;
    ranks.first = uvec4(
        query_rank_words[base], query_rank_words[base + 1u],
        query_rank_words[base + 2u], query_rank_words[base + 3u]);
    ranks.second = uvec4(
        query_rank_words[base + 4u], query_rank_words[base + 5u],
        query_rank_words[base + 6u], query_rank_words[base + 7u]);
    ranks.third = uvec4(
        query_rank_words[base + 8u], query_rank_words[base + 9u],
        query_rank_words[base + 10u], query_rank_words[base + 11u]);
    ranks.fourth = uvec4(
        query_rank_words[base + 12u], query_rank_words[base + 13u],
        query_rank_words[base + 14u], query_rank_words[base + 15u]);
    return ranks;
}

packed_ranks_64 load_target_ranks_64(prefilter_job job) {
    uint base = job.target_offset >> 2u;
    packed_ranks_64 ranks;
    ranks.first = uvec4(
        target_rank_words[base], target_rank_words[base + 1u],
        target_rank_words[base + 2u], target_rank_words[base + 3u]);
    ranks.second = uvec4(
        target_rank_words[base + 4u], target_rank_words[base + 5u],
        target_rank_words[base + 6u], target_rank_words[base + 7u]);
    ranks.third = uvec4(
        target_rank_words[base + 8u], target_rank_words[base + 9u],
        target_rank_words[base + 10u], target_rank_words[base + 11u]);
    ranks.fourth = uvec4(
        target_rank_words[base + 12u], target_rank_words[base + 13u],
        target_rank_words[base + 14u], target_rank_words[base + 15u]);
    return ranks;
}

packed_ranks_32 shift_rank_bytes_32(packed_ranks_32 ranks, uint shift) {
    packed_ranks_32 next_words;
    next_words.first = uvec4(
        ranks.first.y, ranks.first.z, ranks.first.w, ranks.second.x);
    next_words.second = uvec4(
        ranks.second.y, ranks.second.z, ranks.second.w, 0u);
    if (shift == 4u) return next_words;
    uint bits = shift << 3u;
    packed_ranks_32 shifted;
    shifted.first = (ranks.first >> bits)
        | (next_words.first << (32u - bits));
    shifted.second = (ranks.second >> bits)
        | (next_words.second << (32u - bits));
    return shifted;
}

packed_ranks_64 shift_rank_bytes_64(packed_ranks_64 ranks, uint shift) {
    packed_ranks_64 next_words;
    next_words.first = uvec4(
        ranks.first.y, ranks.first.z, ranks.first.w, ranks.second.x);
    next_words.second = uvec4(
        ranks.second.y, ranks.second.z, ranks.second.w, ranks.third.x);
    next_words.third = uvec4(
        ranks.third.y, ranks.third.z, ranks.third.w, ranks.fourth.x);
    next_words.fourth = uvec4(
        ranks.fourth.y, ranks.fourth.z, ranks.fourth.w, 0u);
    if (shift == 4u) return next_words;
    uint bits = shift << 3u;
    packed_ranks_64 shifted;
    shifted.first = (ranks.first >> bits)
        | (next_words.first << (32u - bits));
    shifted.second = (ranks.second >> bits)
        | (next_words.second << (32u - bits));
    shifted.third = (ranks.third >> bits)
        | (next_words.third << (32u - bits));
    shifted.fourth = (ranks.fourth >> bits)
        | (next_words.fourth << (32u - bits));
    return shifted;
}

uint mismatch_mask_32(packed_ranks_32 query, packed_ranks_32 target) {
    uvec4 first = compact_mismatch_nibbles(query.first ^ target.first);
    uvec4 second = compact_mismatch_nibbles(query.second ^ target.second);
    return pack_mismatch_nibbles(first)
        | (pack_mismatch_nibbles(second) << 16u);
}

uvec2 mismatch_mask_64(packed_ranks_64 query, packed_ranks_64 target) {
    uvec4 first = compact_mismatch_nibbles(query.first ^ target.first);
    uvec4 second = compact_mismatch_nibbles(query.second ^ target.second);
    uvec4 third = compact_mismatch_nibbles(query.third ^ target.third);
    uvec4 fourth = compact_mismatch_nibbles(query.fourth ^ target.fourth);
    return uvec2(
        pack_mismatch_nibbles(first)
            | (pack_mismatch_nibbles(second) << 16u),
        pack_mismatch_nibbles(third)
            | (pack_mismatch_nibbles(fourth) << 16u));
}

uvec2 shift_left_64(uvec2 value, uint shift);

uint diagonal_mismatch_mask_32(
    packed_ranks_32 query,
    packed_ranks_32 target,
    uint shift,
    bool shift_target)
{
    uint mismatch = shift_target
        ? mismatch_mask_32(query, shift_rank_bytes_32(target, shift))
        : mismatch_mask_32(shift_rank_bytes_32(query, shift), target);
    mismatch &= 0xffffffffu >> shift;
    return shift_target ? mismatch << shift : mismatch;
}

uvec2 diagonal_mismatch_mask_64(
    packed_ranks_64 query,
    packed_ranks_64 target,
    uint shift,
    bool shift_target)
{
    uvec2 mismatch = shift_target
        ? mismatch_mask_64(query, shift_rank_bytes_64(target, shift))
        : mismatch_mask_64(shift_rank_bytes_64(query, shift), target);
    mismatch.y &= 0xffffffffu >> shift;
    return shift_target ? shift_left_64(mismatch, shift) : mismatch;
}

void set_mask_bit_64(inout uvec2 mask, uint position) {
    if (position < 32u) {
        mask.x |= 1u << position;
    } else {
        mask.y |= 1u << (position - 32u);
    }
}

uint amend_internal_short_zero_runs_32(uint mismatch) {
    uint left_one = mismatch << 1u;
    uint right_one = mismatch >> 1u;
    uint right_two = mismatch >> 2u;
    uint single = (~mismatch) & left_one & right_one;
    uint pair_start = (~mismatch) & (~right_one) & left_one & right_two;
    return mismatch | single | pair_start | (pair_start << 1u);
}

uvec2 shift_left_64(uvec2 value, uint shift) {
    return uvec2(
        value.x << shift,
        (value.y << shift) | (value.x >> (32u - shift)));
}

uvec2 shift_right_64(uvec2 value, uint shift) {
    return uvec2(
        (value.x >> shift) | (value.y << (32u - shift)),
        value.y >> shift);
}

uvec2 amend_internal_short_zero_runs_64(uvec2 mismatch) {
    uvec2 left_one = shift_left_64(mismatch, 1u);
    uvec2 right_one = shift_right_64(mismatch, 1u);
    uvec2 right_two = shift_right_64(mismatch, 2u);
    uvec2 single = (~mismatch) & left_one & right_one;
    uvec2 pair_start = (~mismatch) & (~right_one) & left_one & right_two;
    return mismatch | single | pair_start | shift_left_64(pair_start, 1u);
}

void intersect_shift_32(
    packed_ranks_32 query,
    packed_ranks_32 target,
    uint shift,
    out uint negative_raw,
    out uint positive_raw,
    inout uint final_mask)
{
    negative_raw = diagonal_mismatch_mask_32(
        query, target, shift, true);
    uint shifted_mask = amend_internal_short_zero_runs_32(negative_raw);
    shifted_mask |= 1u << (shift - 1u);
    final_mask &= shifted_mask;

    positive_raw = diagonal_mismatch_mask_32(
        query, target, shift, false);
    shifted_mask = amend_internal_short_zero_runs_32(positive_raw);
    shifted_mask |= 1u << (32u - shift);
    final_mask &= shifted_mask;
}

void intersect_shift_64(
    packed_ranks_64 query,
    packed_ranks_64 target,
    uint shift,
    out uvec2 negative_raw,
    out uvec2 positive_raw,
    inout uvec2 final_mask)
{
    negative_raw = diagonal_mismatch_mask_64(
        query, target, shift, true);
    uvec2 shifted_mask = amend_internal_short_zero_runs_64(negative_raw);
    set_mask_bit_64(shifted_mask, shift - 1u);
    final_mask &= shifted_mask;

    positive_raw = diagonal_mismatch_mask_64(
        query, target, shift, false);
    shifted_mask = amend_internal_short_zero_runs_64(positive_raw);
    set_mask_bit_64(shifted_mask, 64u - shift);
    final_mask &= shifted_mask;
}

uvec4 valid_mask(uint length) {
    uvec4 mask = uvec4(0u);
    uint full_words = length >> 5u;
    uint tail_bits = length & 31u;
    for (uint word_index = 0u; word_index < 4u; ++word_index) {
        if (word_index < full_words) {
            mask[word_index] = 0xffffffffu;
        } else if (word_index == full_words && tail_bits != 0u) {
            mask[word_index] = (1u << tail_bits) - 1u;
        }
    }
    return mask;
}

uvec4 mask_shift_left(uvec4 value, uint shift) {
    uvec4 shifted = uvec4(0u);
    for (uint word_index = 0u; word_index < 4u; ++word_index) {
        shifted[word_index] = value[word_index] << shift;
        if (word_index != 0u) {
            shifted[word_index] |= value[word_index - 1u] >> (32u - shift);
        }
    }
    return shifted;
}

uvec4 mask_shift_right(uvec4 value, uint shift) {
    uvec4 shifted = uvec4(0u);
    for (uint word_index = 0u; word_index < 4u; ++word_index) {
        shifted[word_index] = value[word_index] >> shift;
        if (word_index + 1u < 4u) {
            shifted[word_index] |= value[word_index + 1u] << (32u - shift);
        }
    }
    return shifted;
}

uvec4 amend_internal_short_zero_runs(uvec4 mismatch, uint length) {
    uvec4 left_one = mask_shift_left(mismatch, 1u);
    uvec4 right_one = mask_shift_right(mismatch, 1u);
    uvec4 right_two = mask_shift_right(mismatch, 2u);
    uvec4 single = (~mismatch) & left_one & right_one;
    uvec4 pair_start = (~mismatch) & (~right_one) & left_one & right_two;
    return (mismatch | single | pair_start
            | mask_shift_left(pair_start, 1u))
        & valid_mask(length);
}

uint mask_popcount(uvec4 mask) {
    return uint(bitCount(mask.x) + bitCount(mask.y)
        + bitCount(mask.z) + bitCount(mask.w));
}

uint nibble_cost(uint nibble) {
    if (nibble == 0u) return 0u;
    if (nibble == 0x5u || nibble == 0x6u || nibble == 0x9u
        || nibble == 0xau || nibble == 0xbu || nibble == 0xdu) {
        return 2u;
    }
    return 1u;
}

uint capped_word_estimate(
    uint word,
    uint estimate,
    uint max_edits)
{
    for (uint nibble_index = 0u; nibble_index < 8u; ++nibble_index) {
        estimate += nibble_cost(word & 0xfu);
        if (estimate > max_edits) return max_edits + 1u;
        word >>= 4u;
    }
    return estimate;
}

uint capped_mask_estimate_32(uint mask, uint max_edits) {
    return capped_word_estimate(mask, 0u, max_edits);
}

uint capped_mask_estimate_64(uvec2 mask, uint max_edits) {
    uint estimate = capped_word_estimate(mask.x, 0u, max_edits);
    if (estimate > max_edits) return max_edits + 1u;
    return capped_word_estimate(mask.y, estimate, max_edits);
}

uint gatekeeper_mask_estimate_32(
    prefilter_job job,
    out uint hamming_distance,
    out exact_diagonal_masks exact_masks)
{
#ifdef gatekeeper_specialized_edits
    const uint active_max_edits = uint(gatekeeper_specialized_edits);
#else
    uint active_max_edits = job.max_edits;
#endif
    packed_ranks_32 query = load_query_ranks_32(job);
    packed_ranks_32 target = load_target_ranks_32(job);
    uint hamming = mismatch_mask_32(query, target);
    exact_masks.zero = uvec2(hamming, 0u);
    hamming_distance = uint(bitCount(hamming));
    if (hamming_distance <= active_max_edits) return hamming_distance;

    uint final_mask = amend_internal_short_zero_runs_32(hamming);
    uint negative_raw = 0u;
    uint positive_raw = 0u;
    intersect_shift_32(
        query, target, 1u, negative_raw, positive_raw, final_mask);
    exact_masks.negative_one = uvec2(negative_raw, 0u);
    exact_masks.positive_one = uvec2(positive_raw, 0u);
    intersect_shift_32(
        query, target, 2u, negative_raw, positive_raw, final_mask);
    exact_masks.negative_two = uvec2(negative_raw, 0u);
    exact_masks.positive_two = uvec2(positive_raw, 0u);
#ifdef gatekeeper_specialized_edits
#if gatekeeper_specialized_edits == 4
    intersect_shift_32(
        query, target, 3u, negative_raw, positive_raw, final_mask);
    intersect_shift_32(
        query, target, 4u, negative_raw, positive_raw, final_mask);
#endif
#else
    if (active_max_edits == 4u) {
        intersect_shift_32(
            query, target, 3u, negative_raw, positive_raw, final_mask);
        intersect_shift_32(
            query, target, 4u, negative_raw, positive_raw, final_mask);
    }
#endif
    return capped_mask_estimate_32(final_mask, active_max_edits);
}

uint gatekeeper_mask_estimate_64(
    prefilter_job job,
    out uint hamming_distance,
    out exact_diagonal_masks exact_masks)
{
#ifdef gatekeeper_specialized_edits
    const uint active_max_edits = uint(gatekeeper_specialized_edits);
#else
    uint active_max_edits = job.max_edits;
#endif
    packed_ranks_64 query = load_query_ranks_64(job);
    packed_ranks_64 target = load_target_ranks_64(job);
    uvec2 hamming = mismatch_mask_64(query, target);
    exact_masks.zero = hamming;
    hamming_distance = uint(bitCount(hamming.x) + bitCount(hamming.y));
    if (hamming_distance <= active_max_edits) return hamming_distance;

    uvec2 final_mask = amend_internal_short_zero_runs_64(hamming);
    uvec2 negative_raw = uvec2(0u);
    uvec2 positive_raw = uvec2(0u);
    intersect_shift_64(
        query, target, 1u, negative_raw, positive_raw, final_mask);
    exact_masks.negative_one = negative_raw;
    exact_masks.positive_one = positive_raw;
    intersect_shift_64(
        query, target, 2u, negative_raw, positive_raw, final_mask);
    exact_masks.negative_two = negative_raw;
    exact_masks.positive_two = positive_raw;
#ifdef gatekeeper_specialized_edits
#if gatekeeper_specialized_edits == 4
    intersect_shift_64(
        query, target, 3u, negative_raw, positive_raw, final_mask);
    intersect_shift_64(
        query, target, 4u, negative_raw, positive_raw, final_mask);
#endif
#else
    if (active_max_edits == 4u) {
        intersect_shift_64(
            query, target, 3u, negative_raw, positive_raw, final_mask);
        intersect_shift_64(
            query, target, 4u, negative_raw, positive_raw, final_mask);
    }
#endif
    return capped_mask_estimate_64(final_mask, active_max_edits);
}

uint capped_mask_estimate(uvec4 mask, uint length, uint max_edits) {
    uint estimate = 0u;
    uint word_count = (length + 31u) >> 5u;
    for (uint word_index = 0u; word_index < word_count; ++word_index) {
        uint word = mask[word_index];
        for (uint nibble_index = 0u; nibble_index < 8u; ++nibble_index) {
            estimate += nibble_cost((word >> (4u * nibble_index)) & 0xfu);
            if (estimate > max_edits) return max_edits + 1u;
        }
    }
    return estimate;
}

#ifndef gatekeeper_specialized_length
uint saturated_increment(uint value, uint cap) {
    return value >= cap ? cap : value + 1u;
}
#endif

uvec2 exact_mask_for_diagonal(
    exact_diagonal_masks masks,
    int diagonal)
{
    if (diagonal <= -2) return masks.negative_two;
    if (diagonal == -1) return masks.negative_one;
    if (diagonal == 0) return masks.zero;
    if (diagonal == 1) return masks.positive_one;
    return masks.positive_two;
}

uint mismatch_distance_from_target_position(
    uvec2 mismatch,
    uint target_position,
    uint remaining)
{
    if (remaining == 0u || target_position >= 64u) return 0u;
    if (target_position < 32u) {
        uint low_suffix = mismatch.x >> target_position;
        if (low_suffix != 0u) {
            return min(remaining, uint(findLSB(low_suffix)));
        }
        uint high_distance = 32u - target_position;
        if (mismatch.y != 0u) {
            high_distance += uint(findLSB(mismatch.y));
        } else {
            high_distance = remaining;
        }
        return min(remaining, high_distance);
    }
    uint high_suffix = mismatch.y >> (target_position - 32u);
    return high_suffix == 0u
        ? remaining
        : min(remaining, uint(findLSB(high_suffix)));
}

uint exact_matching_run_from_masks(
    prefilter_job job,
    exact_diagonal_masks masks,
    uint query_position,
    uint target_position)
{
    uint remaining = min(
        job.query_length - query_position,
        job.target_length - target_position);
    int diagonal = int(query_position) - int(target_position);
    return mismatch_distance_from_target_position(
        exact_mask_for_diagonal(masks, diagonal),
        target_position,
        remaining);
}

int bounded_wavefront_position(
    prefilter_job job,
    int diagonal,
    int query_position)
{
    if (query_position < 0 || query_position > int(job.query_length)) {
        return -1;
    }
    int target_position = query_position - diagonal;
    if (target_position < 0 || target_position > int(job.target_length)) {
        return -1;
    }
    return query_position;
}

int extend_wavefront_position(
    prefilter_job job,
    exact_diagonal_masks masks,
    int diagonal,
    int query_position)
{
    query_position = bounded_wavefront_position(
        job, diagonal, query_position);
    if (query_position < 0) return -1;
    uint target_position = uint(query_position - diagonal);
    return query_position + int(exact_matching_run_from_masks(
        job, masks, uint(query_position), target_position));
}

// Advance one exact Levenshtein wavefront cell. Diagonal k is
// query_position-target_position. The three predecessors are substitution on
// k, query deletion on k-1, and target insertion on k+1. For one score layer,
// the farthest valid query position dominates every shorter path on that same
// diagonal before a packed exact-match extension.
int next_wavefront_position(
    prefilter_job job,
    exact_diagonal_masks masks,
    int diagonal,
    int substitution,
    int deletion,
    int insertion)
{
    int best = -1;
    if (substitution >= 0) {
        best = max(best, bounded_wavefront_position(
            job, diagonal, substitution + 1));
    }
    if (deletion >= 0) {
        best = max(best, bounded_wavefront_position(
            job, diagonal, deletion + 1));
    }
    if (insertion >= 0) {
        best = max(best, bounded_wavefront_position(
            job, diagonal, insertion));
    }
    return extend_wavefront_position(job, masks, diagonal, best);
}

// The production E2/E4 guard is an exact scalar-specialised wavefront. It has
// no private arrays or dynamic diagonal indexing. Diagonals that cannot return
// to the equal-length final diagonal with the remaining score are pruned, so
// E2 evaluates four non-initial cells and E4 evaluates twelve. The generic API
// retains the fixed-band DP
// below for E1/E3/E5..E8.
bool exact_edit_distance_e2_or_e4_is_above_threshold(
    prefilter_job job,
    exact_diagonal_masks masks)
{
    const int invalid = -1;
    const int length = int(job.query_length);

    int score0_0 = extend_wavefront_position(job, masks, 0, 0);
    if (score0_0 >= length) return false;

    int score1_m1 = next_wavefront_position(
        job, masks, -1, invalid, invalid, score0_0);
    int score1_0 = next_wavefront_position(
        job, masks, 0, score0_0, invalid, invalid);
    int score1_p1 = next_wavefront_position(
        job, masks, 1, invalid, score0_0, invalid);
    if (score1_0 >= length) return false;

    int score2_0 = next_wavefront_position(
        job, masks, 0, score1_0, score1_m1, score1_p1);
#if defined(gatekeeper_specialized_edits) \
    && gatekeeper_specialized_edits == 2
    return score2_0 < length;
#else
    if (score2_0 >= length) return false;
#ifndef gatekeeper_specialized_edits
    if (job.max_edits == 2u) {
        return score2_0 < length;
    }
#endif

    int score2_m2 = next_wavefront_position(
        job, masks, -2, invalid, invalid, score1_m1);
    int score2_m1 = next_wavefront_position(
        job, masks, -1, score1_m1, invalid, score1_0);
    int score2_p1 = next_wavefront_position(
        job, masks, 1, score1_p1, score1_0, invalid);
    int score2_p2 = next_wavefront_position(
        job, masks, 2, invalid, score1_p1, invalid);
    int score3_m1 = next_wavefront_position(
        job, masks, -1, score2_m1, score2_m2, score2_0);
    int score3_0 = next_wavefront_position(
        job, masks, 0, score2_0, score2_m1, score2_p1);
    int score3_p1 = next_wavefront_position(
        job, masks, 1, score2_p1, score2_0, score2_p2);
    if (score3_0 >= length) return false;

    int score4_0 = next_wavefront_position(
        job, masks, 0, score3_0, score3_m1, score3_p1);
    return score4_0 < length;
#endif
}

#ifndef gatekeeper_specialized_length
bool exact_edit_distance_is_above_threshold(prefilter_job job) {
    uint query_length = job.query_length;
    uint target_length = job.target_length;
    uint max_edits = job.max_edits;
    uint length_difference = query_length > target_length
        ? query_length - target_length
        : target_length - query_length;
    if (length_difference > max_edits) return true;
    if (max_edits >= max(query_length, target_length)) return false;

    uint cap = max_edits + 1u;
    uint center = max_edits + 1u;
    uint width = 2u * max_edits + 3u;
    uint previous[19];
    uint current[19];
    for (uint slot = 0u; slot < 19u; ++slot) previous[slot] = cap;
    for (uint column = 0u; column <= min(target_length, max_edits);
         ++column) {
        previous[center + column] = column;
    }

    for (uint row = 1u; row <= query_length; ++row) {
        for (uint slot = 0u; slot < 19u; ++slot) current[slot] = cap;
        uint column_begin = row > max_edits ? row - max_edits : 0u;
        uint column_end = min(target_length, row + max_edits);
        for (uint column = column_begin; column <= column_end; ++column) {
            int diagonal = int(column) - int(row);
            uint slot = uint(int(center) + diagonal);
            if (column == 0u) {
                current[slot] = min(row, cap);
            } else {
                uint deletion = saturated_increment(previous[slot + 1u], cap);
                uint insertion = saturated_increment(current[slot - 1u], cap);
                uint substitution = previous[slot];
                if (query_rank(job.query_offset + row - 1u)
                    != target_rank(job.target_offset + column - 1u)) {
                    substitution = saturated_increment(substitution, cap);
                }
                current[slot] = min(deletion, min(insertion, substitution));
            }
        }
        for (uint slot = 0u; slot < width; ++slot) {
            previous[slot] = current[slot];
        }
    }

    uint final_slot = uint(
        int(center) + int(target_length) - int(query_length));
    return previous[final_slot] > max_edits;
}
#endif

#ifndef gatekeeper_specialized_length
void solve_gatekeeper_job(uint job_index) {
    prefilter_job job = jobs[job_index];
    if (!prefilter_job_is_in_bounds(job)) {
        write_prefilter_result(
            job_index, 0u, k_status_unsupported, true, 0u);
        return;
    }
    if (job.query_length != job.target_length) {
        uint length_difference = job.query_length > job.target_length
            ? job.query_length - job.target_length
            : job.target_length - job.query_length;
        write_prefilter_result(
            job_index,
            length_difference,
            k_status_length_difference_only,
            length_difference <= job.max_edits,
            0u);
        return;
    }
    if (job.query_length == 0u) {
        write_prefilter_result(job_index, 0u, k_status_ok, true, 0u);
        return;
    }
    if (pc.mode != 0u || uint(compile_time_maximum_span) != k_maximum_gatekeeper_length
        || job.query_length > k_maximum_gatekeeper_length
        || job.max_edits > k_maximum_gatekeeper_edits) {
        write_prefilter_result(
            job_index, 0u, k_status_unsupported, true, 0u);
        return;
    }

    uint length = job.query_length;
    uvec4 hamming = mismatch_mask_segment(job, 0u, 0u, length, 0u)
        & valid_mask(length);
    uint hamming_distance = mask_popcount(hamming);
    if (hamming_distance <= job.max_edits) {
        write_prefilter_result(
            job_index, 0u, k_status_ok, true, hamming_distance);
        return;
    }
    if (job.max_edits == 0u) {
        write_prefilter_result(
            job_index,
            1u,
            k_status_ok,
            false,
            1u | k_hamming_exact_rejection_mask);
        return;
    }

    uvec4 final_mask = amend_internal_short_zero_runs(hamming, length);
    for (uint shift = 1u; shift <= job.max_edits; ++shift) {
        if (shift >= length) break;
        uint compared_length = length - shift;

        uvec4 shifted_mask = mismatch_mask_segment(
            job, 0u, shift, compared_length, shift);
        shifted_mask = amend_internal_short_zero_runs(
            shifted_mask, length);
        set_mask_bit(shifted_mask, shift - 1u);
        final_mask &= shifted_mask;

        shifted_mask = mismatch_mask_segment(
            job, shift, 0u, compared_length, 0u);
        shifted_mask = amend_internal_short_zero_runs(
            shifted_mask, length);
        set_mask_bit(shifted_mask, length - shift);
        final_mask &= shifted_mask;
    }
    final_mask &= valid_mask(length);
    uint estimate = capped_mask_estimate(
        final_mask, length, job.max_edits);
    if (estimate <= job.max_edits) {
        write_prefilter_result(
            job_index, 0u, k_status_ok, true, estimate);
        return;
    }

    uint diagnostics = estimate | k_mask_rejection_candidate_mask
        | k_exact_guard_activated_mask;
    if (exact_edit_distance_is_above_threshold(job)) {
        write_prefilter_result(
            job_index,
            job.max_edits + 1u,
            k_status_ok,
            false,
            diagnostics | k_guard_confirmed_rejection_mask);
        return;
    }
    write_prefilter_result(
        job_index, 0u, k_status_ok, true, diagnostics);
}
#else
void solve_gatekeeper_job(uint job_index) {
    prefilter_job job = jobs[job_index];
    if (!prefilter_job_is_in_bounds(job)) {
        write_prefilter_result(
            job_index, 0u, k_status_unsupported, true, 0u);
        return;
    }
    if (job.query_length != job.target_length) {
        uint length_difference = job.query_length > job.target_length
            ? job.query_length - job.target_length
            : job.target_length - job.query_length;
        write_prefilter_result(
            job_index,
            length_difference,
            k_status_length_difference_only,
            length_difference <= job.max_edits,
            0u);
        return;
    }
    if (job.query_length == 0u) {
        write_prefilter_result(job_index, 0u, k_status_ok, true, 0u);
        return;
    }
    if (pc.mode != 0u || uint(compile_time_maximum_span) != k_maximum_gatekeeper_length
        || job.query_length != uint(gatekeeper_specialized_length)
        || job.max_edits != uint(gatekeeper_specialized_edits)
        || (job.query_offset & 3u) != 0u
        || (job.target_offset & 3u) != 0u) {
        write_prefilter_result(
            job_index, 0u, k_status_unsupported, true, 0u);
        return;
    }

    const uint active_max_edits = uint(gatekeeper_specialized_edits);
    uint hamming_distance = 0u;
    exact_diagonal_masks exact_masks;
#if gatekeeper_specialized_length == 32
    uint estimate = gatekeeper_mask_estimate_32(
        job, hamming_distance, exact_masks);
#else
    uint estimate = gatekeeper_mask_estimate_64(
        job, hamming_distance, exact_masks);
#endif
    if (hamming_distance <= active_max_edits) {
        write_prefilter_result(
            job_index, 0u, k_status_ok, true, hamming_distance);
        return;
    }
    if (estimate <= active_max_edits) {
        write_prefilter_result(
            job_index, 0u, k_status_ok, true, estimate);
        return;
    }

    uint diagnostics = estimate | k_mask_rejection_candidate_mask
        | k_exact_guard_activated_mask;
    if (exact_edit_distance_e2_or_e4_is_above_threshold(
            job, exact_masks)) {
        write_prefilter_result(
            job_index,
            active_max_edits + 1u,
            k_status_ok,
            false,
            diagnostics | k_guard_confirmed_rejection_mask);
        return;
    }
    write_prefilter_result(
        job_index, 0u, k_status_ok, true, diagnostics);
}
#endif

void main() {
    uint64_t stride = uint64_t(gl_NumWorkGroups.x)
        * uint64_t(gl_WorkGroupSize.x);
    if (stride == uint64_t(0)) return;
    for (uint64_t job_index = uint64_t(gl_GlobalInvocationID.x);
         job_index < uint64_t(pc.job_count);
         job_index += stride) {
        solve_gatekeeper_job(uint(job_index));
    }
}
)GLSL";

/// @brief Score/end-point-only banded affine extension. This prototype runs one
/// scalar DP per job and dispatches the batch as grid-strided workgroups. The
/// ABI is intentionally symbol-rank based (`uint` per symbol) so parent
/// integration can feed any alphabet without dna4-specific decoding here.
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

/// @brief Extends alignments and emits bounded simple CIGAR programs.
inline constexpr std::string_view k_alignment_extension_simple_cigar_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const int k_negative_infinity = -1073741824;
const uint k_status_ok = 0u;
const uint k_status_unsupported_geometry = 1u;
const uint k_status_cigar_overflow = 2u;
const uint k_cigar_match = 0u;
const uint k_cigar_insert = 1u;
const uint k_cigar_delete = 2u;
const uint k_maximum_simple_gap = 256u;
const uint k_maximum_simple_gap_operations = 64u;
const uint k_minimum_simple_gap_anchors = 8u;

struct alignment_cigar_job {
    uint query_symbol_offset;
    uint target_symbol_offset;
    uint query_len;
    uint target_len;
    uint query_global_start;
    uint target_global_start;
    uint band_width;
    uint flags;
    uint cigar_offset;
    uint cigar_capacity;
    uint traceback_tile_offset;
    uint traceback_tile_count;
};

struct alignment_cigar_result {
    int score;
    int max_score;
    uint query_start;
    uint query_end;
    uint target_start;
    uint target_end;
    uint cigar_offset;
    uint cigar_length;
    uint status;
    uint reserved;
};

layout(std430, set = 0, binding = 0) readonly buffer query_symbols_buffer { uint query_symbols[]; };
layout(std430, set = 0, binding = 1) readonly buffer target_symbols_buffer { uint target_symbols[]; };
layout(std430, set = 0, binding = 2) readonly buffer jobs_buffer { alignment_cigar_job jobs[]; };
layout(std430, set = 0, binding = 3) buffer results_buffer { alignment_cigar_result results[]; };
layout(std430, set = 0, binding = 4) buffer packed_cigar_buffer { uint packed_cigar[]; };
layout(std430, set = 0, binding = 5) buffer traceback_buffer { uint traceback[]; };

layout(push_constant) uniform push_constants {
    uint job_count;
    uint alphabet_size;
    uint max_band_width;
    uint tile_query;
    uint tile_target;
    int match_score;
    int mismatch_penalty;
    int gap_open_short;
    int gap_extend_short;
    int gap_open_long;
    int gap_extend_long;
    int z_drop;
    int end_bonus;
    uint flags;
    uint reserved;
} pc;

void write_reject(uint job_index, uint status) {
    results[job_index].score = 0;
    results[job_index].max_score = 0;
    results[job_index].query_start = 0u;
    results[job_index].query_end = 0u;
    results[job_index].target_start = 0u;
    results[job_index].target_end = 0u;
    results[job_index].cigar_offset = 0u;
    results[job_index].cigar_length = 0u;
    results[job_index].status = status;
    results[job_index].reserved = 0u;
}

int substitution_score(uint query_symbol, uint target_symbol) {
    return query_symbol == target_symbol ? pc.match_score : -pc.mismatch_penalty;
}

int gap_cost_short(uint len) {
    return pc.gap_open_short + pc.gap_extend_short * int(len);
}

int gap_cost_long(uint len) {
    return pc.gap_open_long + pc.gap_extend_long * int(len);
}

uint traceback_index(alignment_cigar_job job, uint target_cols, uint query_i, uint target_j) {
    return job.traceback_tile_offset + query_i * target_cols + target_j;
}

uint best_gap_state(uint len, bool insertion) {
    int short_cost = gap_cost_short(len);
    int long_cost = gap_cost_long(len);
    if (long_cost < short_cost) {
        return insertion ? 3u : 4u;
    }
    return insertion ? 1u : 2u;
}

void write_success(
    uint job_index,
    alignment_cigar_job job,
    int score,
    uint cigar_length) {
    results[job_index].score = score;
    results[job_index].max_score = score;
    results[job_index].query_start = job.query_global_start;
    results[job_index].query_end = job.query_global_start + job.query_len;
    results[job_index].target_start = job.target_global_start;
    results[job_index].target_end = job.target_global_start + job.target_len;
    results[job_index].cigar_offset = job.cigar_offset;
    results[job_index].cigar_length = cigar_length;
    results[job_index].status = k_status_ok;
    results[job_index].reserved = 0u;
}

bool append_reverse_cigar(alignment_cigar_job job, inout uint cigar_length, uint op) {
    if (cigar_length > 0u) {
        uint last_index = job.cigar_offset + cigar_length - 1u;
        if ((packed_cigar[last_index] & 15u) == op) {
            packed_cigar[last_index] += 16u;
            return true;
        }
    }
    if (cigar_length >= job.cigar_capacity) {
        return false;
    }
    packed_cigar[job.cigar_offset + cigar_length] = (1u << 4) | op;
    ++cigar_length;
    return true;
}

void reverse_cigar_slice(alignment_cigar_job job, uint cigar_length) {
    for (uint i = 0u; i < cigar_length / 2u; ++i) {
        uint left_index = job.cigar_offset + i;
        uint right_index = job.cigar_offset + cigar_length - 1u - i;
        uint tmp = packed_cigar[left_index];
        packed_cigar[left_index] = packed_cigar[right_index];
        packed_cigar[right_index] = tmp;
    }
}

int best_gap_cost_value(uint len) {
    int short_cost = gap_cost_short(len);
    int long_cost = gap_cost_long(len);
    return long_cost < short_cost ? long_cost : short_cost;
}

bool append_forward_cigar(
    alignment_cigar_job job,
    inout uint cigar_length,
    uint len,
    uint op) {
    if (len == 0u) {
        return true;
    }
    if (cigar_length > 0u) {
        uint last_index = job.cigar_offset + cigar_length - 1u;
        if ((packed_cigar[last_index] & 15u) == op) {
            packed_cigar[last_index] += (len << 4u);
            return true;
        }
    }
    if (cigar_length >= job.cigar_capacity) {
        return false;
    }
    packed_cigar[job.cigar_offset + cigar_length] = (len << 4u) | op;
    ++cigar_length;
    return true;
}

uint common_match_run(alignment_cigar_job job, uint query_pos, uint target_pos) {
    uint run = 0u;
    while (query_pos + run < job.query_len
           && target_pos + run < job.target_len
           && query_symbols[job.query_symbol_offset + query_pos + run]
              == target_symbols[job.target_symbol_offset + target_pos + run]) {
        ++run;
    }
    return run;
}

bool try_one_gap_script(
    alignment_cigar_job job,
    uint gap_op,
    uint gap_len,
    out uint prefix_len,
    out uint suffix_len) {
    prefix_len = common_match_run(job, 0u, 0u);
    uint query_pos = prefix_len;
    uint target_pos = prefix_len;
    if (gap_op == k_cigar_insert) {
        query_pos += gap_len;
    } else {
        target_pos += gap_len;
    }
    if (query_pos > job.query_len || target_pos > job.target_len) {
        return false;
    }
    suffix_len = common_match_run(job, query_pos, target_pos);
    return query_pos + suffix_len == job.query_len
        && target_pos + suffix_len == job.target_len;
}

bool try_two_gap_script(
    alignment_cigar_job job,
    uint first_op,
    uint first_len,
    uint second_op,
    uint second_len,
    out uint prefix_len,
    out uint mid_len,
    out uint suffix_len) {
    prefix_len = common_match_run(job, 0u, 0u);
    uint query_pos = prefix_len;
    uint target_pos = prefix_len;
    if (first_op == k_cigar_insert) {
        query_pos += first_len;
    } else {
        target_pos += first_len;
    }
    if (query_pos > job.query_len || target_pos > job.target_len) {
        return false;
    }
    mid_len = common_match_run(job, query_pos, target_pos);
    query_pos += mid_len;
    target_pos += mid_len;
    if (second_op == k_cigar_insert) {
        query_pos += second_len;
    } else {
        target_pos += second_len;
    }
    if (query_pos > job.query_len || target_pos > job.target_len) {
        return false;
    }
    suffix_len = common_match_run(job, query_pos, target_pos);
    return query_pos + suffix_len == job.query_len
        && target_pos + suffix_len == job.target_len;
}

bool better_greedy_gap_candidate(
    uint run_len,
    uint gap_len,
    uint op,
    uint best_run,
    uint best_len,
    uint best_op) {
    return run_len > best_run
        || (run_len == best_run
            && (best_len == 0u || gap_len < best_len
                || (gap_len == best_len && op < best_op)));
}

bool try_emit_unit_gap_walk_cigar(uint job_index, alignment_cigar_job job) {
    uint query_pos = 0u;
    uint target_pos = 0u;
    uint cigar_length = 0u;
    uint match_count = 0u;
    uint gap_cost = 0u;
    uint gap_count = 0u;

    while (query_pos < job.query_len || target_pos < job.target_len) {
        uint run_len = 0u;
        if (query_pos < job.query_len && target_pos < job.target_len) {
            run_len = common_match_run(job, query_pos, target_pos);
        }
        if (run_len != 0u) {
            if (!append_forward_cigar(job, cigar_length, run_len, k_cigar_match)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            query_pos += run_len;
            target_pos += run_len;
            match_count += run_len;
            continue;
        }

        if (gap_count >= k_maximum_simple_gap_operations) {
            return false;
        }

        if (query_pos == job.query_len) {
            uint gap_len = job.target_len - target_pos;
            if (!append_forward_cigar(job, cigar_length, gap_len, k_cigar_delete)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(gap_len));
            target_pos = job.target_len;
            ++gap_count;
            continue;
        }
        if (target_pos == job.target_len) {
            uint gap_len = job.query_len - query_pos;
            if (!append_forward_cigar(job, cigar_length, gap_len, k_cigar_insert)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(gap_len));
            query_pos = job.query_len;
            ++gap_count;
            continue;
        }

        if (target_pos + 1u < job.target_len
            && query_symbols[job.query_symbol_offset + query_pos]
                == target_symbols[job.target_symbol_offset + target_pos + 1u]) {
            if (!append_forward_cigar(job, cigar_length, 1u, k_cigar_delete)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(1u));
            ++target_pos;
            ++gap_count;
            continue;
        }
        if (query_pos + 1u < job.query_len
            && query_symbols[job.query_symbol_offset + query_pos + 1u]
                == target_symbols[job.target_symbol_offset + target_pos]) {
            if (!append_forward_cigar(job, cigar_length, 1u, k_cigar_insert)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(1u));
            ++query_pos;
            ++gap_count;
            continue;
        }

        return false;
    }

    int score = int(match_count) * pc.match_score - int(gap_cost);
    write_success(job_index, job, score, cigar_length);
    return true;
}

bool try_emit_greedy_gap_cigar(uint job_index, alignment_cigar_job job) {
    uint query_pos = 0u;
    uint target_pos = 0u;
    uint cigar_length = 0u;
    uint match_count = 0u;
    uint gap_cost = 0u;
    uint gap_count = 0u;

    while (query_pos < job.query_len || target_pos < job.target_len) {
        uint run_len = 0u;
        if (query_pos < job.query_len && target_pos < job.target_len) {
            run_len = common_match_run(job, query_pos, target_pos);
        }
        if (run_len != 0u) {
            if (!append_forward_cigar(job, cigar_length, run_len, k_cigar_match)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            query_pos += run_len;
            target_pos += run_len;
            match_count += run_len;
            continue;
        }

        if (query_pos == job.query_len) {
            uint gap_len = job.target_len - target_pos;
            if (!append_forward_cigar(job, cigar_length, gap_len, k_cigar_delete)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(gap_len));
            target_pos = job.target_len;
            ++gap_count;
            continue;
        }
        if (target_pos == job.target_len) {
            uint gap_len = job.query_len - query_pos;
            if (!append_forward_cigar(job, cigar_length, gap_len, k_cigar_insert)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(gap_len));
            query_pos = job.query_len;
            ++gap_count;
            continue;
        }

        uint best_op = 99u;
        uint best_len = 0u;
        uint best_run = 0u;
        for (uint gap_len = 1u; gap_len <= k_maximum_simple_gap; ++gap_len) {
            if (target_pos + gap_len < job.target_len
                && query_symbols[job.query_symbol_offset + query_pos]
                    == target_symbols[job.target_symbol_offset + target_pos + gap_len]) {
                uint candidate_run =
                    common_match_run(job, query_pos, target_pos + gap_len);
                if (better_greedy_gap_candidate(
                        candidate_run, gap_len, k_cigar_delete,
                        best_run, best_len, best_op)) {
                    best_run = candidate_run;
                    best_len = gap_len;
                    best_op = k_cigar_delete;
                }
            }
            if (query_pos + gap_len < job.query_len
                && query_symbols[job.query_symbol_offset + query_pos + gap_len]
                    == target_symbols[job.target_symbol_offset + target_pos]) {
                uint candidate_run =
                    common_match_run(job, query_pos + gap_len, target_pos);
                if (better_greedy_gap_candidate(
                        candidate_run, gap_len, k_cigar_insert,
                        best_run, best_len, best_op)) {
                    best_run = candidate_run;
                    best_len = gap_len;
                    best_op = k_cigar_insert;
                }
            }
        }

        if (best_len == 0u || best_run < k_minimum_simple_gap_anchors
            || gap_count >= k_maximum_simple_gap_operations) {
            return false;
        }
        if (!append_forward_cigar(job, cigar_length, best_len, best_op)) {
            write_reject(job_index, k_status_cigar_overflow);
            return true;
        }
        gap_cost += uint(best_gap_cost_value(best_len));
        ++gap_count;
        if (best_op == k_cigar_insert) {
            query_pos += best_len;
        } else {
            target_pos += best_len;
        }
    }

    int score = int(match_count) * pc.match_score - int(gap_cost);
    write_success(job_index, job, score, cigar_length);
    return true;
}

bool emit_simple_script_success(
    uint job_index,
    alignment_cigar_job job,
    uint prefix_len,
    uint first_op,
    uint first_len,
    uint mid_len,
    uint second_op,
    uint second_len,
    uint suffix_len,
    uint gap_count) {
    uint cigar_length = 0u;
    if (!append_forward_cigar(job, cigar_length, prefix_len, k_cigar_match)
        || (gap_count >= 1u
            && !append_forward_cigar(job, cigar_length, first_len, first_op))
        || !append_forward_cigar(job, cigar_length, mid_len, k_cigar_match)
        || (gap_count >= 2u
            && !append_forward_cigar(job, cigar_length, second_len, second_op))
        || !append_forward_cigar(job, cigar_length, suffix_len, k_cigar_match)) {
        return false;
    }

    int score = int(prefix_len + mid_len + suffix_len) * pc.match_score;
    if (gap_count >= 1u) {
        score -= best_gap_cost_value(first_len);
    }
    if (gap_count >= 2u) {
        score -= best_gap_cost_value(second_len);
    }
    write_success(job_index, job, score, cigar_length);
    return true;
}

bool try_emit_simple_cigar(uint job_index, alignment_cigar_job job) {
    if (job.cigar_capacity == 0u) {
        write_reject(job_index, k_status_cigar_overflow);
        return true;
    }

    uint exact_len = common_match_run(job, 0u, 0u);
    if (exact_len == job.query_len && exact_len == job.target_len) {
        return emit_simple_script_success(
            job_index, job,
            exact_len, k_cigar_match, 0u,
            0u, k_cigar_match, 0u,
            0u, 0u);
    }

    int net_query = int(job.query_len) - int(job.target_len);
    int abs_net_query = net_query < 0 ? -net_query : net_query;
    if (abs_net_query > int(k_maximum_simple_gap)) {
        return false;
    }

    if (net_query > 0) {
        uint prefix_len = 0u;
        uint suffix_len = 0u;
        uint gap_len = uint(net_query);
        if (try_one_gap_script(
                job, k_cigar_insert, gap_len, prefix_len, suffix_len)) {
            return emit_simple_script_success(
                job_index, job,
                prefix_len, k_cigar_insert, gap_len,
                suffix_len, k_cigar_match, 0u,
                0u, 1u);
        }
    } else if (net_query < 0) {
        uint prefix_len = 0u;
        uint suffix_len = 0u;
        uint gap_len = uint(-net_query);
        if (try_one_gap_script(
                job, k_cigar_delete, gap_len, prefix_len, suffix_len)) {
            return emit_simple_script_success(
                job_index, job,
                prefix_len, k_cigar_delete, gap_len,
                suffix_len, k_cigar_match, 0u,
                0u, 1u);
        }
    }

    for (uint del_len = 1u; del_len <= k_maximum_simple_gap; ++del_len) {
        int ins_len_i = int(del_len) + net_query;
        if (ins_len_i <= 0 || ins_len_i > int(k_maximum_simple_gap)) {
            continue;
        }
        uint ins_len = uint(ins_len_i);

        uint prefix_len = 0u;
        uint mid_len = 0u;
        uint suffix_len = 0u;
        if (try_two_gap_script(
                job,
                k_cigar_delete, del_len,
                k_cigar_insert, ins_len,
                prefix_len, mid_len, suffix_len)) {
            return emit_simple_script_success(
                job_index, job,
                prefix_len, k_cigar_delete, del_len,
                mid_len, k_cigar_insert, ins_len,
                suffix_len, 2u);
        }

        prefix_len = 0u;
        mid_len = 0u;
        suffix_len = 0u;
        if (try_two_gap_script(
                job,
                k_cigar_insert, ins_len,
                k_cigar_delete, del_len,
                prefix_len, mid_len, suffix_len)) {
            return emit_simple_script_success(
                job_index, job,
                prefix_len, k_cigar_insert, ins_len,
                mid_len, k_cigar_delete, del_len,
                suffix_len, 2u);
        }
    }

    if (job.query_len != job.target_len
        && try_emit_unit_gap_walk_cigar(job_index, job)) {
        return true;
    }

    if (try_emit_greedy_gap_cigar(job_index, job)) {
        return true;
    }

    return false;
}

uint ceil_div_u(uint value, uint divisor) {
    return divisor == 0u ? 0u : (value + divisor - 1u) / divisor;
}

uint min_u(uint a, uint b) {
    return a < b ? a : b;
}

uint gridded_query_tiles(alignment_cigar_job job) {
    return ceil_div_u(job.query_len, pc.tile_query);
}

uint gridded_target_tiles(alignment_cigar_job job) {
    return ceil_div_u(job.target_len, pc.tile_target);
}

uint gridded_record_words() {
    return 5u * ((pc.tile_query + 1u) + (pc.tile_target + 1u));
}

uint gridded_boundary_words(alignment_cigar_job job) {
    return gridded_query_tiles(job) * gridded_target_tiles(job)
        * gridded_record_words();
}

uint gridded_scratch_words() {
    return (pc.tile_query + 1u) * (pc.tile_target + 1u);
}

uint encode_score(int value) {
    return uint(value + 1073741824);
}

int decode_score(uint value) {
    return int(value) - 1073741824;
}

uint tile_base(alignment_cigar_job job, uint tile_row, uint tile_col) {
    return job.traceback_tile_offset
        + (tile_row * gridded_target_tiles(job) + tile_col)
            * gridded_record_words();
}

uint bottom_state_index(
    alignment_cigar_job job,
    uint tile_row,
    uint tile_col,
    uint local_target,
    uint state_index) {
    return tile_base(job, tile_row, tile_col)
        + local_target * 5u + state_index;
}

uint right_state_index(
    alignment_cigar_job job,
    uint tile_row,
    uint tile_col,
    uint local_query,
    uint state_index) {
    return tile_base(job, tile_row, tile_col)
        + 5u * (pc.tile_target + 1u)
        + local_query * 5u + state_index;
}

uint gridded_scratch_offset(alignment_cigar_job job) {
    return job.traceback_tile_offset + gridded_boundary_words(job);
}

uint gridded_scratch_index(
    alignment_cigar_job job,
    uint local_query,
    uint local_target,
    uint scratch_cols) {
    return gridded_scratch_offset(job) + local_query * scratch_cols + local_target;
}

void init_axis_state(
    uint query_pos,
    uint target_pos,
    out int h,
    out int i1,
    out int i2,
    out int d1,
    out int d2) {
    if (query_pos == 0u && target_pos == 0u) {
        h = 0;
        i1 = k_negative_infinity;
        i2 = k_negative_infinity;
        d1 = k_negative_infinity;
        d2 = k_negative_infinity;
        return;
    }
    if (target_pos == 0u) {
        i1 = -gap_cost_short(query_pos);
        i2 = -gap_cost_long(query_pos);
        h = i2 > i1 ? i2 : i1;
        d1 = k_negative_infinity;
        d2 = k_negative_infinity;
        return;
    }
    if (query_pos == 0u) {
        d1 = -gap_cost_short(target_pos);
        d2 = -gap_cost_long(target_pos);
        h = d2 > d1 ? d2 : d1;
        i1 = k_negative_infinity;
        i2 = k_negative_infinity;
        return;
    }
    h = k_negative_infinity;
    i1 = k_negative_infinity;
    i2 = k_negative_infinity;
    d1 = k_negative_infinity;
    d2 = k_negative_infinity;
}

void load_top_state(
    alignment_cigar_job job,
    uint tile_row,
    uint tile_col,
    uint local_target,
    out int h,
    out int i1,
    out int i2,
    out int d1,
    out int d2) {
    if (tile_row == 0u) {
        init_axis_state(0u, tile_col * pc.tile_target + local_target,
                        h, i1, i2, d1, d2);
        return;
    }
    uint above = tile_row - 1u;
    h = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 0u)]);
    i1 = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 1u)]);
    i2 = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 2u)]);
    d1 = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 3u)]);
    d2 = decode_score(traceback[bottom_state_index(job, above, tile_col, local_target, 4u)]);
}

void load_left_state(
    alignment_cigar_job job,
    uint tile_row,
    uint tile_col,
    uint local_query,
    out int h,
    out int i1,
    out int i2,
    out int d1,
    out int d2) {
    if (tile_col == 0u) {
        init_axis_state(tile_row * pc.tile_query + local_query, 0u,
                        h, i1, i2, d1, d2);
        return;
    }
    uint left = tile_col - 1u;
    h = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 0u)]);
    i1 = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 1u)]);
    i2 = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 2u)]);
    d1 = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 3u)]);
    d2 = decode_score(traceback[right_state_index(job, tile_row, left, local_query, 4u)]);
}

void store_bottom_state(
    alignment_cigar_job job,
    uint tile_row,
    uint tile_col,
    uint local_target,
    int h,
    int i1,
    int i2,
    int d1,
    int d2) {
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 0u)] = encode_score(h);
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 1u)] = encode_score(i1);
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 2u)] = encode_score(i2);
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 3u)] = encode_score(d1);
    traceback[bottom_state_index(job, tile_row, tile_col, local_target, 4u)] = encode_score(d2);
}

void store_right_state(
    alignment_cigar_job job,
    uint tile_row,
    uint tile_col,
    uint local_query,
    int h,
    int i1,
    int i2,
    int d1,
    int d2) {
    traceback[right_state_index(job, tile_row, tile_col, local_query, 0u)] = encode_score(h);
    traceback[right_state_index(job, tile_row, tile_col, local_query, 1u)] = encode_score(i1);
    traceback[right_state_index(job, tile_row, tile_col, local_query, 2u)] = encode_score(i2);
    traceback[right_state_index(job, tile_row, tile_col, local_query, 3u)] = encode_score(d1);
    traceback[right_state_index(job, tile_row, tile_col, local_query, 4u)] = encode_score(d2);
}

void compute_gridded_tile(
    alignment_cigar_job job,
    uint tile_row,
    uint tile_col,
    bool store_boundaries,
    bool store_trace) {
    uint query_origin = tile_row * pc.tile_query;
    uint target_origin = tile_col * pc.tile_target;
    uint query_rows = min_u(pc.tile_query, job.query_len - query_origin);
    uint target_cols_active = min_u(pc.tile_target, job.target_len - target_origin);
    uint scratch_cols = target_cols_active + 1u;

    int h_prev[compile_time_maximum_span + 1];
    int h_cur[compile_time_maximum_span + 1];
    int i1_prev[compile_time_maximum_span + 1];
    int i1_cur[compile_time_maximum_span + 1];
    int i2_prev[compile_time_maximum_span + 1];
    int i2_cur[compile_time_maximum_span + 1];
    int d1_cur[compile_time_maximum_span + 1];
    int d2_cur[compile_time_maximum_span + 1];

    int unused_d1 = k_negative_infinity;
    int unused_d2 = k_negative_infinity;
    for (uint local_target = 0u; local_target <= target_cols_active; ++local_target) {
        load_top_state(job, tile_row, tile_col, local_target,
                       h_prev[local_target], i1_prev[local_target],
                       i2_prev[local_target], unused_d1, unused_d2);
    }

    if (store_boundaries) {
        int top_h = k_negative_infinity;
        int top_i1 = k_negative_infinity;
        int top_i2 = k_negative_infinity;
        int top_d1 = k_negative_infinity;
        int top_d2 = k_negative_infinity;
        load_top_state(job, tile_row, tile_col, target_cols_active,
                       top_h, top_i1, top_i2, top_d1, top_d2);
        store_right_state(job, tile_row, tile_col, 0u,
                          top_h, top_i1, top_i2, top_d1, top_d2);
    }

    for (uint local_query = 1u; local_query <= query_rows; ++local_query) {
        load_left_state(job, tile_row, tile_col, local_query,
                        h_cur[0], i1_cur[0], i2_cur[0],
                        d1_cur[0], d2_cur[0]);

        for (uint local_target = 1u; local_target <= target_cols_active; ++local_target) {
            int i1_open = h_prev[local_target] - pc.gap_open_short - pc.gap_extend_short;
            int i1_extend = i1_prev[local_target] - pc.gap_extend_short;
            bool i1_cont = i1_extend >= i1_open;
            i1_cur[local_target] = i1_cont ? i1_extend : i1_open;

            int i2_open = h_prev[local_target] - pc.gap_open_long - pc.gap_extend_long;
            int i2_extend = i2_prev[local_target] - pc.gap_extend_long;
            bool i2_cont = i2_extend >= i2_open;
            i2_cur[local_target] = i2_cont ? i2_extend : i2_open;

            int d1_open = h_cur[local_target - 1u] - pc.gap_open_short - pc.gap_extend_short;
            int d1_extend = d1_cur[local_target - 1u] - pc.gap_extend_short;
            bool d1_cont = d1_extend >= d1_open;
            d1_cur[local_target] = d1_cont ? d1_extend : d1_open;

            int d2_open = h_cur[local_target - 1u] - pc.gap_open_long - pc.gap_extend_long;
            int d2_extend = d2_cur[local_target - 1u] - pc.gap_extend_long;
            bool d2_cont = d2_extend >= d2_open;
            d2_cur[local_target] = d2_cont ? d2_extend : d2_open;

            uint q = query_symbols[job.query_symbol_offset + query_origin + local_query - 1u];
            uint t = target_symbols[job.target_symbol_offset + target_origin + local_target - 1u];
            int best_score = h_prev[local_target - 1u] + substitution_score(q, t);
            uint best_source = 0u;
            if (i1_cur[local_target] > best_score) {
                best_score = i1_cur[local_target];
                best_source = 1u;
            }
            if (d1_cur[local_target] > best_score) {
                best_score = d1_cur[local_target];
                best_source = 2u;
            }
            if (i2_cur[local_target] > best_score) {
                best_score = i2_cur[local_target];
                best_source = 3u;
            }
            if (d2_cur[local_target] > best_score) {
                best_score = d2_cur[local_target];
                best_source = 4u;
            }
            h_cur[local_target] = best_score;

            if (store_trace) {
                uint trace = best_source;
                if (i1_cont) trace |= 1u << 3u;
                if (d1_cont) trace |= 1u << 4u;
                if (i2_cont) trace |= 1u << 5u;
                if (d2_cont) trace |= 1u << 6u;
                traceback[gridded_scratch_index(
                    job, local_query, local_target, scratch_cols)] = trace;
            }
        }

        if (store_boundaries) {
            store_right_state(job, tile_row, tile_col, local_query,
                              h_cur[target_cols_active],
                              i1_cur[target_cols_active],
                              i2_cur[target_cols_active],
                              d1_cur[target_cols_active],
                              d2_cur[target_cols_active]);
        }

        for (uint local_target = 0u; local_target <= target_cols_active; ++local_target) {
            h_prev[local_target] = h_cur[local_target];
            i1_prev[local_target] = i1_cur[local_target];
            i2_prev[local_target] = i2_cur[local_target];
        }
    }

    if (store_boundaries) {
        for (uint local_target = 0u; local_target <= target_cols_active; ++local_target) {
            store_bottom_state(job, tile_row, tile_col, local_target,
                               h_prev[local_target],
                               i1_prev[local_target],
                               i2_prev[local_target],
                               d1_cur[local_target],
                               d2_cur[local_target]);
        }
    }
}

bool solve_gridded_dp(uint job_index, alignment_cigar_job job) {
    if (pc.tile_query == 0u || pc.tile_query > compile_time_maximum_span
        || pc.tile_target == 0u || pc.tile_target > compile_time_maximum_span) {
        write_reject(job_index, k_status_unsupported_geometry);
        return true;
    }
    uint required_words = gridded_boundary_words(job) + gridded_scratch_words();
    if (job.traceback_tile_count < required_words) {
        write_reject(job_index, k_status_unsupported_geometry);
        return true;
    }

    uint query_tiles = gridded_query_tiles(job);
    uint target_tiles = gridded_target_tiles(job);
    for (uint tile_row = 0u; tile_row < query_tiles; ++tile_row) {
        for (uint tile_col = 0u; tile_col < target_tiles; ++tile_col) {
            compute_gridded_tile(job, tile_row, tile_col, true, false);
        }
    }

    uint last_tile_row = query_tiles - 1u;
    uint last_tile_col = target_tiles - 1u;
    uint last_target = job.target_len - last_tile_col * pc.tile_target;
    int score = decode_score(
        traceback[bottom_state_index(job, last_tile_row, last_tile_col, last_target, 0u)]);

    uint global_query = job.query_len;
    uint global_target = job.target_len;
    uint state = 0u;
    uint cigar_length = 0u;

    while (global_query > 0u || global_target > 0u) {
        if (global_query == 0u) {
            if (!append_reverse_cigar(job, cigar_length, k_cigar_delete)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            --global_target;
            continue;
        }
        if (global_target == 0u) {
            if (!append_reverse_cigar(job, cigar_length, k_cigar_insert)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            --global_query;
            continue;
        }

        uint tile_row = (global_query - 1u) / pc.tile_query;
        uint tile_col = (global_target - 1u) / pc.tile_target;
        uint query_origin = tile_row * pc.tile_query;
        uint target_origin = tile_col * pc.tile_target;
        uint target_cols_active = min_u(pc.tile_target, job.target_len - target_origin);
        uint scratch_cols = target_cols_active + 1u;
        compute_gridded_tile(job, tile_row, tile_col, false, true);

        while (global_query > query_origin || global_target > target_origin) {
            uint local_query = global_query - query_origin;
            uint local_target = global_target - target_origin;
            if (local_query == 0u || local_target == 0u) {
                break;
            }
            uint trace = traceback[gridded_scratch_index(
                job, local_query, local_target, scratch_cols)];
            if (state == 0u) {
                uint source = trace & 7u;
                if (source == 0u) {
                    if (!append_reverse_cigar(job, cigar_length, k_cigar_match)) {
                        write_reject(job_index, k_status_cigar_overflow);
                        return true;
                    }
                    --global_query;
                    --global_target;
                } else if (source == 1u || source == 3u) {
                    state = source;
                } else if (source == 2u || source == 4u) {
                    state = source;
                } else {
                    write_reject(job_index, k_status_unsupported_geometry);
                    return true;
                }
            } else if (state == 1u || state == 3u) {
                if (!append_reverse_cigar(job, cigar_length, k_cigar_insert)) {
                    write_reject(job_index, k_status_cigar_overflow);
                    return true;
                }
                bool cont = state == 1u ? ((trace & (1u << 3u)) != 0u)
                                        : ((trace & (1u << 5u)) != 0u);
                --global_query;
                state = cont ? state : 0u;
            } else {
                if (!append_reverse_cigar(job, cigar_length, k_cigar_delete)) {
                    write_reject(job_index, k_status_cigar_overflow);
                    return true;
                }
                bool cont = state == 2u ? ((trace & (1u << 4u)) != 0u)
                                        : ((trace & (1u << 6u)) != 0u);
                --global_target;
                state = cont ? state : 0u;
            }
        }
    }

    reverse_cigar_slice(job, cigar_length);
    write_success(job_index, job, score, cigar_length);
    return true;
}

void solve_bounded_dp(uint job_index) {
    alignment_cigar_job job = jobs[job_index];
    if (pc.alphabet_size == 0u || pc.flags != 0u || pc.reserved != 0u
        || job.flags != 0u || job.query_len == 0u || job.target_len == 0u
        || job.band_width > pc.max_band_width) {
        write_reject(job_index, k_status_unsupported_geometry);
        return;
    }
    if (job.cigar_capacity == 0u) {
        write_reject(job_index, k_status_cigar_overflow);
        return;
    }

    for (uint i = 0u; i < job.query_len; ++i) {
        if (query_symbols[job.query_symbol_offset + i] >= pc.alphabet_size) {
            write_reject(job_index, k_status_unsupported_geometry);
            return;
        }
    }
    for (uint i = 0u; i < job.target_len; ++i) {
        if (target_symbols[job.target_symbol_offset + i] >= pc.alphabet_size) {
            write_reject(job_index, k_status_unsupported_geometry);
            return;
        }
    }

    if (try_emit_simple_cigar(job_index, job)) {
        return;
    }

    uint target_cols = job.target_len + 1u;
    uint traceback_cells = (job.query_len + 1u) * target_cols;
    if (job.query_len > compile_time_maximum_span || job.target_len > compile_time_maximum_span) {
        solve_gridded_dp(job_index, job);
        return;
    }
    if (job.traceback_tile_count < traceback_cells) {
        write_reject(job_index, k_status_unsupported_geometry);
        return;
    }

    int h_prev[compile_time_maximum_span + 1];
    int h_cur[compile_time_maximum_span + 1];
    int i1_prev[compile_time_maximum_span + 1];
    int i1_cur[compile_time_maximum_span + 1];
    int i2_prev[compile_time_maximum_span + 1];
    int i2_cur[compile_time_maximum_span + 1];
    int d1_cur[compile_time_maximum_span + 1];
    int d2_cur[compile_time_maximum_span + 1];

    h_prev[0] = 0;
    i1_prev[0] = k_negative_infinity;
    i2_prev[0] = k_negative_infinity;
    traceback[traceback_index(job, target_cols, 0u, 0u)] = 0u;
    for (uint j = 1u; j <= job.target_len; ++j) {
        int short_score = -gap_cost_short(j);
        int long_score = -gap_cost_long(j);
        bool use_long = long_score > short_score;
        h_prev[j] = use_long ? long_score : short_score;
        i1_prev[j] = k_negative_infinity;
        i2_prev[j] = k_negative_infinity;
        uint source = use_long ? 4u : 2u;
        uint cont_bit = j > 1u ? (use_long ? (1u << 6u) : (1u << 4u)) : 0u;
        traceback[traceback_index(job, target_cols, 0u, j)] = source | cont_bit;
    }

    for (uint i = 1u; i <= job.query_len; ++i) {
        int short_score = -gap_cost_short(i);
        int long_score = -gap_cost_long(i);
        bool use_long = long_score > short_score;
        h_cur[0] = use_long ? long_score : short_score;
        i1_cur[0] = short_score;
        i2_cur[0] = long_score;
        d1_cur[0] = k_negative_infinity;
        d2_cur[0] = k_negative_infinity;
        uint source = use_long ? 3u : 1u;
        uint cont_bit = i > 1u ? (use_long ? (1u << 5u) : (1u << 3u)) : 0u;
        traceback[traceback_index(job, target_cols, i, 0u)] = source | cont_bit;

        for (uint j = 1u; j <= job.target_len; ++j) {
            int i1_open = h_prev[j] - pc.gap_open_short - pc.gap_extend_short;
            int i1_extend = i1_prev[j] - pc.gap_extend_short;
            bool i1_cont = i1_extend >= i1_open;
            i1_cur[j] = i1_cont ? i1_extend : i1_open;

            int i2_open = h_prev[j] - pc.gap_open_long - pc.gap_extend_long;
            int i2_extend = i2_prev[j] - pc.gap_extend_long;
            bool i2_cont = i2_extend >= i2_open;
            i2_cur[j] = i2_cont ? i2_extend : i2_open;

            int d1_open = h_cur[j - 1u] - pc.gap_open_short - pc.gap_extend_short;
            int d1_extend = d1_cur[j - 1u] - pc.gap_extend_short;
            bool d1_cont = d1_extend >= d1_open;
            d1_cur[j] = d1_cont ? d1_extend : d1_open;

            int d2_open = h_cur[j - 1u] - pc.gap_open_long - pc.gap_extend_long;
            int d2_extend = d2_cur[j - 1u] - pc.gap_extend_long;
            bool d2_cont = d2_extend >= d2_open;
            d2_cur[j] = d2_cont ? d2_extend : d2_open;

            uint q = query_symbols[job.query_symbol_offset + i - 1u];
            uint t = target_symbols[job.target_symbol_offset + j - 1u];
            int best_score = h_prev[j - 1u] + substitution_score(q, t);
            uint best_source = 0u;
            if (i1_cur[j] > best_score) {
                best_score = i1_cur[j];
                best_source = 1u;
            }
            if (d1_cur[j] > best_score) {
                best_score = d1_cur[j];
                best_source = 2u;
            }
            if (i2_cur[j] > best_score) {
                best_score = i2_cur[j];
                best_source = 3u;
            }
            if (d2_cur[j] > best_score) {
                best_score = d2_cur[j];
                best_source = 4u;
            }
            h_cur[j] = best_score;

            uint trace = best_source;
            if (i1_cont) trace |= 1u << 3u;
            if (d1_cont) trace |= 1u << 4u;
            if (i2_cont) trace |= 1u << 5u;
            if (d2_cont) trace |= 1u << 6u;
            traceback[traceback_index(job, target_cols, i, j)] = trace;
        }

        for (uint j = 0u; j <= job.target_len; ++j) {
            h_prev[j] = h_cur[j];
            i1_prev[j] = i1_cur[j];
            i2_prev[j] = i2_cur[j];
        }
    }

    uint i = job.query_len;
    uint j = job.target_len;
    uint state = 0u;
    uint cigar_length = 0u;
    while (i > 0u || j > 0u) {
        uint trace = traceback[traceback_index(job, target_cols, i, j)];
        if (state == 0u) {
            uint source = trace & 7u;
            if (source == 0u && i > 0u && j > 0u) {
                if (!append_reverse_cigar(job, cigar_length, k_cigar_match)) {
                    write_reject(job_index, k_status_cigar_overflow);
                    return;
                }
                --i;
                --j;
            } else if ((source == 1u || source == 3u) && i > 0u) {
                state = source;
            } else if ((source == 2u || source == 4u) && j > 0u) {
                state = source;
            } else if (i > 0u) {
                state = best_gap_state(i, true);
            } else if (j > 0u) {
                state = best_gap_state(j, false);
            } else {
                break;
            }
        } else if (state == 1u || state == 3u) {
            if (!append_reverse_cigar(job, cigar_length, k_cigar_insert)) {
                write_reject(job_index, k_status_cigar_overflow);
                return;
            }
            bool cont = state == 1u ? ((trace & (1u << 3u)) != 0u)
                                    : ((trace & (1u << 5u)) != 0u);
            --i;
            state = cont ? state : 0u;
        } else {
            if (!append_reverse_cigar(job, cigar_length, k_cigar_delete)) {
                write_reject(job_index, k_status_cigar_overflow);
                return;
            }
            bool cont = state == 2u ? ((trace & (1u << 4u)) != 0u)
                                    : ((trace & (1u << 6u)) != 0u);
            --j;
            state = cont ? state : 0u;
        }
    }

    reverse_cigar_slice(job, cigar_length);
    write_success(job_index, job, h_prev[job.target_len], cigar_length);
}

void main() {
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    if (stride == 0u) return;
    for (uint job_index = gl_GlobalInvocationID.x; job_index < pc.job_count; job_index += stride) {
        solve_bounded_dp(job_index);
    }
}
)GLSL";
/// @brief Compose one rank-prefilter shader from the single bounds-aware wire/load
/// preamble and its algorithm-specific body.
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
