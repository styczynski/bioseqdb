/**
 * @file sneaky_snake.cppm
 * @brief GPU SneakySnake filter for estimating a lower bound on sequence edit distance.
 */
export module bioseq.gpu.vulkan.shader_compiler:alignment_prefilter_sneaky_snake;

import std;

export namespace bioseq::gpu::vulkan {

/// @brief Filter sequence pairs with SneakySnake's edit-distance lower bound.
/// For equal lengths, follow the longest shifted match within each bounded
/// piece, then count an edit to pass the mismatch. Reject when the total
/// exceeds the edit limit.
///
/// Dispatch assigns one thread or an eight-thread team to each job. Unequal
/// lengths use their length difference as the bound. Unsupported inputs remain
/// candidates.
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

} // namespace bioseq::gpu::vulkan
