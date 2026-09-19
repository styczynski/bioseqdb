/**
 * @file simple_cigar.cppm
 * @brief Defines the simple-CIGAR alignment-extension GLSL body.
 */
export module bioseq.gpu.vulkan.shader_compiler:alignment_prefilter_simple_cigar;

import std;

export namespace bioseq::gpu::vulkan {

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
const uint k_job_packed_inputs = 1u << 0u;
const uint k_job_reverse_target = 1u << 1u;
const uint k_job_flag_mask = k_job_packed_inputs | k_job_reverse_target;
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
    uint query_packed_byte_count;
    uint target_packed_byte_count;
    uint query_leading_symbol_offset;
    uint target_leading_symbol_offset;
    uint query_storage_unit_width;
    uint target_storage_unit_width;
    uint bits_per_symbol;
    uint problem_index;
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
    uint num_matches;
    uint block_length;
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
    uint complement_rank_words[8];
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
    results[job_index].num_matches = 0u;
    results[job_index].block_length = 0u;
}

uint packed_byte(bool query_side, uint byte_index) {
    uint word = query_side ? query_symbols[byte_index >> 2u]
                           : target_symbols[byte_index >> 2u];
    return (word >> ((byte_index & 3u) * 8u)) & 255u;
}

uint packed_rank(bool query_side,
                 uint byte_offset,
                 uint storage_unit_width,
                 uint leading_symbol_offset,
                 uint bits_per_symbol,
                 uint logical_index) {
    uint letters_per_unit = (storage_unit_width * 8u) / bits_per_symbol;
    uint packed_index = leading_symbol_offset + logical_index;
    uint unit_index = packed_index / letters_per_unit;
    uint symbol_in_unit = packed_index - unit_index * letters_per_unit;
    uint bit_offset = symbol_in_unit * bits_per_symbol;
    uint source_byte = byte_offset + unit_index * storage_unit_width + (bit_offset >> 3u);
    uint pair = packed_byte(query_side, source_byte);
    if ((bit_offset & 7u) + bits_per_symbol > 8u) {
        pair |= packed_byte(query_side, source_byte + 1u) << 8u;
    }
    return (pair >> (bit_offset & 7u)) & ((1u << bits_per_symbol) - 1u);
}

uint complement_rank(uint rank) {
    uint word = pc.complement_rank_words[rank >> 2u];
    return (word >> ((rank & 3u) * 8u)) & 255u;
}

uint query_symbol_at(alignment_cigar_job job, uint logical_index) {
    if ((job.flags & k_job_packed_inputs) == 0u) {
        return query_symbols[job.query_symbol_offset + logical_index];
    }
    return packed_rank(true,
                       job.query_symbol_offset,
                       job.query_storage_unit_width,
                       job.query_leading_symbol_offset,
                       job.bits_per_symbol,
                       logical_index);
}

uint target_symbol_at(alignment_cigar_job job, uint logical_index) {
    if ((job.flags & k_job_packed_inputs) == 0u) {
        return target_symbols[job.target_symbol_offset + logical_index];
    }
    bool reverse = (job.flags & k_job_reverse_target) != 0u;
    uint source_index = reverse ? job.target_len - 1u - logical_index : logical_index;
    uint rank = packed_rank(false,
                            job.target_symbol_offset,
                            job.target_storage_unit_width,
                            job.target_leading_symbol_offset,
                            job.bits_per_symbol,
                            source_index);
    return reverse ? complement_rank(rank) : rank;
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
    uint query_cursor = 0u;
    uint target_cursor = 0u;
    uint num_matches = 0u;
    uint block_length = 0u;
    for (uint cigar_index = 0u; cigar_index < cigar_length; ++cigar_index) {
        uint entry = packed_cigar[job.cigar_offset + cigar_index];
        uint operation = entry & 15u;
        uint operation_length = entry >> 4u;
        block_length += operation_length;
        if (operation == k_cigar_match) {
            for (uint symbol = 0u; symbol < operation_length; ++symbol) {
                if (query_symbol_at(job, query_cursor + symbol)
                    == target_symbol_at(job, target_cursor + symbol)) {
                    ++num_matches;
                }
            }
            query_cursor += operation_length;
            target_cursor += operation_length;
        } else if (operation == k_cigar_insert) {
            query_cursor += operation_length;
        } else if (operation == k_cigar_delete) {
            target_cursor += operation_length;
        }
    }
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
    results[job_index].num_matches = num_matches;
    results[job_index].block_length = block_length;
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

bool position_within_band(alignment_cigar_job job, uint query_pos, uint target_pos) {
    uint delta = query_pos > target_pos ? query_pos - target_pos : target_pos - query_pos;
    return delta <= job.band_width;
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
           && query_symbol_at(job, query_pos + run)
              == target_symbol_at(job, target_pos + run)) {
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
    if (query_pos > job.query_len || target_pos > job.target_len ||
        !position_within_band(job, query_pos, target_pos)) {
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
    if (query_pos > job.query_len || target_pos > job.target_len ||
        !position_within_band(job, query_pos, target_pos)) {
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
    if (query_pos > job.query_len || target_pos > job.target_len ||
        !position_within_band(job, query_pos, target_pos)) {
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
            if (!position_within_band(job, query_pos, target_pos)) {
                return false;
            }
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
            if (!position_within_band(job, query_pos, target_pos)) {
                return false;
            }
            continue;
        }

        if (target_pos + 1u < job.target_len
            && query_symbol_at(job, query_pos)
                == target_symbol_at(job, target_pos + 1u)) {
            if (!append_forward_cigar(job, cigar_length, 1u, k_cigar_delete)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(1u));
            ++target_pos;
            ++gap_count;
            if (!position_within_band(job, query_pos, target_pos)) {
                return false;
            }
            continue;
        }
        if (query_pos + 1u < job.query_len
            && query_symbol_at(job, query_pos + 1u)
                == target_symbol_at(job, target_pos)) {
            if (!append_forward_cigar(job, cigar_length, 1u, k_cigar_insert)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            gap_cost += uint(best_gap_cost_value(1u));
            ++query_pos;
            ++gap_count;
            if (!position_within_band(job, query_pos, target_pos)) {
                return false;
            }
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
            if (!position_within_band(job, query_pos, target_pos)) {
                return false;
            }
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
            if (!position_within_band(job, query_pos, target_pos)) {
                return false;
            }
            continue;
        }

        uint best_op = 99u;
        uint best_len = 0u;
        uint best_run = 0u;
        for (uint gap_len = 1u; gap_len <= k_maximum_simple_gap; ++gap_len) {
            if (target_pos + gap_len < job.target_len
                && query_symbol_at(job, query_pos)
                    == target_symbol_at(job, target_pos + gap_len)) {
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
                && query_symbol_at(job, query_pos + gap_len)
                    == target_symbol_at(job, target_pos)) {
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
        if (!position_within_band(job, query_pos, target_pos)) {
            return false;
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

uint gridded_tile_slots(alignment_cigar_job job) {
    uint band_cells = pc.tile_query + 2u * job.band_width;
    return min_u(gridded_target_tiles(job),
                 ceil_div_u(band_cells, pc.tile_target) + 1u);
}

uint gridded_min_tile_col(alignment_cigar_job job, uint tile_row) {
    uint first_query = tile_row * pc.tile_query + 1u;
    uint first_target = first_query > job.band_width
        ? first_query - job.band_width
        : 1u;
    return (first_target - 1u) / pc.tile_target;
}

uint gridded_max_tile_col(alignment_cigar_job job, uint tile_row) {
    uint query_end = min_u(job.query_len, (tile_row + 1u) * pc.tile_query);
    uint target_end = min_u(job.target_len, query_end + job.band_width);
    return (target_end - 1u) / pc.tile_target;
}

bool gridded_tile_is_active(alignment_cigar_job job, uint tile_row, uint tile_col) {
    return tile_col >= gridded_min_tile_col(job, tile_row)
        && tile_col <= gridded_max_tile_col(job, tile_row);
}

uint gridded_record_words() {
    return 5u * ((pc.tile_query + 1u) + (pc.tile_target + 1u));
}

uint gridded_boundary_words(alignment_cigar_job job) {
    return gridded_query_tiles(job) * gridded_tile_slots(job)
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

int add_reachable_score(int value, int delta) {
    return value == k_negative_infinity ? k_negative_infinity : value + delta;
}

int subtract_reachable_cost(int value, int cost) {
    return value == k_negative_infinity ? k_negative_infinity : value - cost;
}

uint tile_base(alignment_cigar_job job, uint tile_row, uint tile_col) {
    uint slot = tile_col - gridded_min_tile_col(job, tile_row);
    return job.traceback_tile_offset
        + (tile_row * gridded_tile_slots(job) + slot)
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

void set_negative_state(
    out int h,
    out int i1,
    out int i2,
    out int d1,
    out int d2) {
    h = k_negative_infinity;
    i1 = k_negative_infinity;
    i2 = k_negative_infinity;
    d1 = k_negative_infinity;
    d2 = k_negative_infinity;
}

void init_axis_state(
    alignment_cigar_job job,
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
        if (query_pos > job.band_width) {
            set_negative_state(h, i1, i2, d1, d2);
            return;
        }
        i1 = -gap_cost_short(query_pos);
        i2 = -gap_cost_long(query_pos);
        h = i2 > i1 ? i2 : i1;
        d1 = k_negative_infinity;
        d2 = k_negative_infinity;
        return;
    }
    if (query_pos == 0u) {
        if (target_pos > job.band_width) {
            set_negative_state(h, i1, i2, d1, d2);
            return;
        }
        d1 = -gap_cost_short(target_pos);
        d2 = -gap_cost_long(target_pos);
        h = d2 > d1 ? d2 : d1;
        i1 = k_negative_infinity;
        i2 = k_negative_infinity;
        return;
    }
    set_negative_state(h, i1, i2, d1, d2);
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
        init_axis_state(job, 0u, tile_col * pc.tile_target + local_target,
                        h, i1, i2, d1, d2);
        return;
    }
    uint above = tile_row - 1u;
    if (!gridded_tile_is_active(job, above, tile_col)) {
        set_negative_state(h, i1, i2, d1, d2);
        return;
    }
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
        init_axis_state(job, tile_row * pc.tile_query + local_query, 0u,
                        h, i1, i2, d1, d2);
        return;
    }
    uint left = tile_col - 1u;
    if (!gridded_tile_is_active(job, tile_row, left)) {
        set_negative_state(h, i1, i2, d1, d2);
        return;
    }
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
            uint global_query = query_origin + local_query;
            uint global_target = target_origin + local_target;
            uint coordinate_delta = global_query > global_target
                ? global_query - global_target
                : global_target - global_query;
            if (coordinate_delta > job.band_width) {
                set_negative_state(h_cur[local_target],
                                   i1_cur[local_target],
                                   i2_cur[local_target],
                                   d1_cur[local_target],
                                   d2_cur[local_target]);
                if (store_trace) {
                    traceback[gridded_scratch_index(
                        job, local_query, local_target, scratch_cols)] = 7u;
                }
                continue;
            }
            int i1_open = subtract_reachable_cost(
                h_prev[local_target], pc.gap_open_short + pc.gap_extend_short);
            int i1_extend = subtract_reachable_cost(
                i1_prev[local_target], pc.gap_extend_short);
            bool i1_cont = i1_extend >= i1_open;
            i1_cur[local_target] = i1_cont ? i1_extend : i1_open;

            int i2_open = subtract_reachable_cost(
                h_prev[local_target], pc.gap_open_long + pc.gap_extend_long);
            int i2_extend = subtract_reachable_cost(
                i2_prev[local_target], pc.gap_extend_long);
            bool i2_cont = i2_extend >= i2_open;
            i2_cur[local_target] = i2_cont ? i2_extend : i2_open;

            int d1_open = subtract_reachable_cost(
                h_cur[local_target - 1u], pc.gap_open_short + pc.gap_extend_short);
            int d1_extend = subtract_reachable_cost(
                d1_cur[local_target - 1u], pc.gap_extend_short);
            bool d1_cont = d1_extend >= d1_open;
            d1_cur[local_target] = d1_cont ? d1_extend : d1_open;

            int d2_open = subtract_reachable_cost(
                h_cur[local_target - 1u], pc.gap_open_long + pc.gap_extend_long);
            int d2_extend = subtract_reachable_cost(
                d2_cur[local_target - 1u], pc.gap_extend_long);
            bool d2_cont = d2_extend >= d2_open;
            d2_cur[local_target] = d2_cont ? d2_extend : d2_open;

            uint q = query_symbol_at(job, query_origin + local_query - 1u);
            uint t = target_symbol_at(job, target_origin + local_target - 1u);
            int best_score = add_reachable_score(
                h_prev[local_target - 1u], substitution_score(q, t));
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
    uint length_delta = job.query_len > job.target_len
        ? job.query_len - job.target_len
        : job.target_len - job.query_len;
    uint required_words = gridded_boundary_words(job) + gridded_scratch_words();
    if (length_delta > job.band_width || job.traceback_tile_count < required_words) {
        write_reject(job_index, k_status_unsupported_geometry);
        return true;
    }

    uint query_tiles = gridded_query_tiles(job);
    for (uint tile_row = 0u; tile_row < query_tiles; ++tile_row) {
        uint first_tile_col = gridded_min_tile_col(job, tile_row);
        uint last_tile_col = gridded_max_tile_col(job, tile_row);
        for (uint tile_col = first_tile_col; tile_col <= last_tile_col; ++tile_col) {
            compute_gridded_tile(job, tile_row, tile_col, true, false);
        }
    }

    uint last_tile_row = query_tiles - 1u;
    uint last_tile_col = gridded_target_tiles(job) - 1u;
    uint last_target = job.target_len - last_tile_col * pc.tile_target;
    int score = decode_score(
        traceback[bottom_state_index(job, last_tile_row, last_tile_col, last_target, 0u)]);
    if (score == k_negative_infinity) {
        write_reject(job_index, k_status_unsupported_geometry);
        return true;
    }

    uint global_query = job.query_len;
    uint global_target = job.target_len;
    uint state = 0u;
    uint cigar_length = 0u;

    while (global_query > 0u || global_target > 0u) {
        if (global_query == 0u) {
            if (global_target > job.band_width) {
                write_reject(job_index, k_status_unsupported_geometry);
                return true;
            }
            if (!append_reverse_cigar(job, cigar_length, k_cigar_delete)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            --global_target;
            continue;
        }
        if (global_target == 0u) {
            if (global_query > job.band_width) {
                write_reject(job_index, k_status_unsupported_geometry);
                return true;
            }
            if (!append_reverse_cigar(job, cigar_length, k_cigar_insert)) {
                write_reject(job_index, k_status_cigar_overflow);
                return true;
            }
            --global_query;
            continue;
        }

        uint tile_row = (global_query - 1u) / pc.tile_query;
        uint tile_col = (global_target - 1u) / pc.tile_target;
        if (!gridded_tile_is_active(job, tile_row, tile_col)) {
            write_reject(job_index, k_status_unsupported_geometry);
            return true;
        }
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
    uint length_delta = job.query_len > job.target_len
        ? job.query_len - job.target_len
        : job.target_len - job.query_len;
    if (pc.alphabet_size == 0u || pc.flags != 0u || pc.reserved != 0u
        || (job.flags & ~k_job_flag_mask) != 0u
        || ((job.flags & k_job_reverse_target) != 0u
            && (job.flags & k_job_packed_inputs) == 0u)
        || job.query_len == 0u || job.target_len == 0u
        || length_delta > job.band_width
        || job.band_width > pc.max_band_width) {
        write_reject(job_index, k_status_unsupported_geometry);
        return;
    }
    if (job.cigar_capacity == 0u) {
        write_reject(job_index, k_status_cigar_overflow);
        return;
    }

    for (uint i = 0u; i < job.query_len; ++i) {
        if (query_symbol_at(job, i) >= pc.alphabet_size) {
            write_reject(job_index, k_status_unsupported_geometry);
            return;
        }
    }
    for (uint i = 0u; i < job.target_len; ++i) {
        if (target_symbol_at(job, i) >= pc.alphabet_size) {
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

            uint q = query_symbol_at(job, i - 1u);
            uint t = target_symbol_at(job, j - 1u);
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

} // namespace bioseq::gpu::vulkan
