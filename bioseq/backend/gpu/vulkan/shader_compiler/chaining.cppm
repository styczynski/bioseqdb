/**
 * @file chaining.cppm
 * @brief GPU shaders for chaining anchors, choosing coordinates, and computing mapping quality.
 */
module bioseq.gpu.vulkan.shader_compiler:chaining;

import :registry;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Computes the best chain score and predecessor for each anchor.
/// A workgroup processes one independent anchor range at a time. Its 32 lanes
/// compare possible predecessors in parallel, then choose the best link before
/// advancing to the next anchor. `max_iter` limits the backward search.
///
/// Inputs use 24-byte `gpu_anchor_wire` records with 64-bit positions and a
/// span for each anchor. Predecessors are absolute indices used during traceback.
inline constexpr std::string_view k_chain_tiled_dp_body = R"GLSL(
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_ballot : require

layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const uint k_subgroup_lane_count = 32u;
const uint k_no_parent = 0xffffffffu;
const int k_invalid_score = -2147483647 - 1;

struct gpu_anchor_wire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct gpu_chain_range_wire {
    uint begin;
    uint range_len;
};

layout(std430, set = 0, binding = 0) readonly buffer anchors_buffer {
    gpu_anchor_wire anchors[];
};
layout(std430, set = 0, binding = 1) readonly buffer ranges_buffer {
    gpu_chain_range_wire ranges[];
};
layout(std430, set = 0, binding = 2) buffer best_scores_buffer {
    coherent int best_scores[];
};
layout(std430, set = 0, binding = 3) buffer predecessors_buffer {
    int64_t predecessors[];
};

layout(push_constant) uniform push_constants {
    uint anchor_count;
    uint range_count;
    int max_dist_x;
    int max_dist_y;
    int bandwidth;
    int max_iter;
    float gap_penalty;
    float skip_penalty;
    int max_chain_skip;
} pc;

bool anchor_reverse(gpu_anchor_wire a) { return a.is_reverse != 0u; }
uint anchor_span(gpu_anchor_wire a) { return a.span_flags_segment & 0xffu; }

float fast_log2(float x) {
    uint z = floatBitsToUint(x);
    float log_2 = float((z >> 23u) & 255u) - 128.0;
    z &= ~(255u << 23u);
    z += 127u << 23u;
    float f = uintBitsToFloat(z);
    return log_2 + (-0.34484843 * f + 2.02466578) * f - 0.67487759;
}

int chain_score(gpu_anchor_wire current, gpu_anchor_wire pred) {
    if (anchor_reverse(current) != anchor_reverse(pred)) return k_invalid_score;
    if (current.query_position <= pred.query_position) return k_invalid_score;
    if (current.ref_position <= pred.ref_position) return k_invalid_score;

    uint64_t dq64 = current.query_position - pred.query_position;
    uint64_t dr64 = current.ref_position - pred.ref_position;
    if (dq64 > uint64_t(pc.max_dist_x)) return k_invalid_score;
    if (dr64 == uint64_t(0) || dq64 > uint64_t(pc.max_dist_y)) return k_invalid_score;
    if (dr64 > uint64_t(pc.max_dist_x)) return k_invalid_score;

    int dq = int(dq64);
    int dr = int(dr64);
    int dd = dr > dq ? dr - dq : dq - dr;
    if (dd > pc.bandwidth) return k_invalid_score;

    int dg = dr < dq ? dr : dq;
    int q_span = int(anchor_span(pred));
    int score = q_span < dg ? q_span : dg;
    if (dd != 0 || dg > q_span) {
        float lin_pen = pc.gap_penalty * float(dd) + pc.skip_penalty * float(dg);
        float log_pen = dd >= 1 ? fast_log2(float(dd + 1)) : 0.0;
        score -= int(lin_pen + 0.5 * log_pen);
    }
    return score;
}

uint signed_score_key(int score) {
    return uint(score) ^ 0x80000000u;
}

void main() {
    uint lane = gl_SubgroupInvocationID;

    for (uint range_id = gl_WorkGroupID.x; range_id < pc.range_count;
         range_id += gl_NumWorkGroups.x) {
        gpu_chain_range_wire r = ranges[range_id];
        uint begin = r.begin;
        uint end = min(pc.anchor_count, r.begin + r.range_len);
        if (begin >= end) continue;

        for (uint global_i = begin; global_i < end; ++global_i) {
            gpu_anchor_wire a = anchors[global_i];
            int best = max(best_scores[global_i], int(anchor_span(a)));
            uint best_parent = k_no_parent;
            uint64_t scanned = uint64_t(0);
            uint no_improve_batches = 0u;
            uint max_no_improve_batches = pc.max_chain_skip > 0
                ? (uint(pc.max_chain_skip) / k_subgroup_lane_count + 1u)
                : 0xffffffffu;

            while (true) {
                uint64_t remaining = uint64_t(global_i) - uint64_t(begin);
                uint64_t offset = scanned + uint64_t(lane) + uint64_t(1);
                bool in_range = offset <= remaining;

                uint pred = begin;
                gpu_anchor_wire pa = a;
                if (in_range) {
                    pred = global_i - uint(offset);
                    pa = anchors[pred];
                }

                uint first_out = subgroupMin(!in_range ? lane : k_subgroup_lane_count);
                uint first_iter = subgroupMin(
                    (in_range && offset > uint64_t(uint(pc.max_iter))) ? lane : k_subgroup_lane_count);
                uint first_ref = subgroupMin(
                    (in_range
                     && anchor_reverse(a) == anchor_reverse(pa)
                     && a.ref_position > pa.ref_position
                     && a.ref_position - pa.ref_position > uint64_t(pc.max_dist_x))
                        ? lane : k_subgroup_lane_count);
                uint first_stop = min(first_out, min(first_iter, first_ref));
                bool lane_active = in_range && lane < first_stop;

                int candidate_total = k_invalid_score;
                if (lane_active) {
                    int pair = chain_score(a, pa);
                    if (pair != k_invalid_score) {
                        candidate_total = pair + best_scores[pred];
                    }
                }

                uint candidate_score_key = signed_score_key(candidate_total);
                uint max_score_key = subgroupMax(candidate_score_key);
                bool is_max_score_lane = candidate_score_key == max_score_key;
                int batch_score = subgroupMax(is_max_score_lane ? candidate_total : k_invalid_score);
                uint batch_parent = subgroupMax(
                    (is_max_score_lane && lane_active && candidate_total != k_invalid_score) ? pred : 0u);

                bool stop_after_batch = first_stop < k_subgroup_lane_count;
                if (batch_score > best) {
                    best = batch_score;
                    best_parent = batch_parent;
                    no_improve_batches = 0u;
                } else if (pc.max_chain_skip > 0) {
                    ++no_improve_batches;
                    if (no_improve_batches >= max_no_improve_batches) {
                        stop_after_batch = true;
                    }
                }

                if (stop_after_batch) break;
                scanned += uint64_t(k_subgroup_lane_count);
            }

            if (subgroupElect()) {
                best_scores[global_i] = best;
                predecessors[global_i] = best_parent == k_no_parent
                    ? int64_t(-1) : int64_t(best_parent);
            }
            memoryBarrierBuffer();
            barrier();
        }
    }
}
)GLSL";
/// @brief Splits each query's sorted anchors into independent chaining ranges.
/// Starts a new range when the strand changes or a target-position gap exceeds
/// the limit. One thread scans each query. Very long ranges are thinned at
/// regular intervals while keeping both endpoints, which limits chaining work.
inline constexpr std::string_view k_chain_range_build_body = R"GLSL(
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
struct gpu_anchor_wire { uint64_t ref_position; uint64_t query_position; uint is_reverse; uint span_flags_segment; };
struct gpu_chain_range_wire { uint begin; uint range_len; };
struct query_span { uint anchor_lo; uint anchor_hi; uint range_lo; uint range_out; };
layout(std430, set = 0, binding = 0) readonly buffer anchors_buffer { gpu_anchor_wire anchors[]; };
layout(std430, set = 0, binding = 1) buffer counts_buffer { uint counts[]; };
layout(std430, set = 0, binding = 2) buffer output_ranges_buffer { gpu_chain_range_wire ranges[]; };
layout(std430, set = 0, binding = 3) buffer unused_3_buffer { uint unused3[]; };
layout(std430, set = 0, binding = 4) buffer compacted_anchors_buffer { gpu_anchor_wire compacted_anchors[]; };
layout(std430, set = 0, binding = 5) readonly buffer query_spans_buffer { query_span spans[]; };
layout(push_constant) uniform push_constants {
    uint query_count;
    uint max_ref_gap;
} pc;
const uint k_compact_range_threshold = 131072u;
const uint k_compact_range_target = 65536u;
uint chrom_key(gpu_anchor_wire a) { return (a.is_reverse != 0u) ? 1u : 0u; }
bool range_break(gpu_anchor_wire prev, gpu_anchor_wire cur) {
    bool key_changed = chrom_key(cur) != chrom_key(prev);
    bool hard_gap = (cur.ref_position > prev.ref_position)
        && ((cur.ref_position - prev.ref_position) > uint64_t(pc.max_ref_gap));
    return key_changed || hard_gap;
}
uint compact_stride(uint range_len) {
    if (range_len <= k_compact_range_threshold) return 1u;
    uint stride = (range_len + k_compact_range_target - 1u) / k_compact_range_target;
    return max(stride, 1u);
}
uint emit_compacted_range(uint src_begin, uint src_end, uint range_index, uint anchor_out) {
    uint range_len = src_end - src_begin;
    uint stride = compact_stride(range_len);
    uint dst_begin = anchor_out;
    for (uint j = 0u; j < range_len; ++j) {
        if ((j % stride) == 0u || j + 1u == range_len) {
            compacted_anchors[anchor_out] = anchors[src_begin + j];
            anchor_out += 1u;
        }
    }
    ranges[range_index].begin = dst_begin;
    ranges[range_index].range_len = anchor_out - dst_begin;
    return anchor_out;
}
void main() {
    uint qi = gl_WorkGroupID.x;
    if (qi >= pc.query_count) return;
    query_span s = spans[qi];
    uint lo = s.anchor_lo;
    uint hi = s.anchor_hi;
    uint out_idx = s.range_lo;
    uint produced = 0u;
    uint count_idx = qi * 2u;
    if (lo >= hi) {
        counts[count_idx] = 0u;
        counts[count_idx + 1u] = 0u;
        return;
    }
    uint begin = lo;
    bool needs_compaction = false;
    for (uint i = lo + 1u; i < hi; ++i) {
        gpu_anchor_wire prev = anchors[i - 1u];
        gpu_anchor_wire cur = anchors[i];
        if (range_break(prev, cur)) {
            uint range_len = i - begin;
            needs_compaction = needs_compaction || range_len > k_compact_range_threshold;
            ranges[out_idx + produced].begin = begin;
            ranges[out_idx + produced].range_len = range_len;
            produced += 1u;
            begin = i;
        }
    }
    uint last_len = hi - begin;
    needs_compaction = needs_compaction || last_len > k_compact_range_threshold;
    ranges[out_idx + produced].begin = begin;
    ranges[out_idx + produced].range_len = last_len;
    produced += 1u;
    counts[count_idx] = produced;
    counts[count_idx + 1u] = 0u;

    if (!needs_compaction) return;

    produced = 0u;
    uint anchor_out = s.range_out;
    begin = lo;
    for (uint i = lo + 1u; i < hi; ++i) {
        gpu_anchor_wire prev = anchors[i - 1u];
        gpu_anchor_wire cur = anchors[i];
        if (range_break(prev, cur)) {
            anchor_out = emit_compacted_range(
                begin, i, out_idx + produced, anchor_out);
            produced += 1u;
            begin = i;
        }
    }
    anchor_out = emit_compacted_range(begin, hi, out_idx + produced, anchor_out);
    counts[count_idx + 1u] = anchor_out - s.range_out;
}
)GLSL";

/// @brief Chooses mapping coordinates by tracing back the best unvisited chain ends.
/// Stops at the first chain that meets the score and seed-count thresholds, or
/// after `probe_limit` attempts. Writes a compact mapping result and its anchor
/// trace so callers can avoid downloading all chaining scores.
inline constexpr std::string_view k_coordinate_reduce_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

struct gpu_anchor_wire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct coordinate_result {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int seed_count;
    uint is_reverse;
    uint valid;
};

layout(std430, set = 0, binding = 0) readonly buffer anchors_buffer { gpu_anchor_wire anchors[]; };
layout(std430, set = 0, binding = 1) readonly buffer best_scores_buffer { int best_scores[]; };
layout(std430, set = 0, binding = 2) readonly buffer predecessors_buffer { int64_t predecessors[]; };
layout(std430, set = 0, binding = 3) buffer visited_buffer { uint visited[]; };
layout(std430, set = 0, binding = 4) buffer trace_buffer { uint trace[]; };
layout(std430, set = 0, binding = 5) buffer result_buffer { coordinate_result result; };

layout(push_constant) uniform push_constants {
    uint anchor_count;
    uint query_len_lo;
    uint query_len_hi;
    int min_chain_score;
    int min_seed_count;
    uint probe_limit;
    uint reserved;
} pc;

shared int sh_score[compile_time_workgroup_size_x];
shared uint sh_index[compile_time_workgroup_size_x];
shared int selected_score;
shared uint selected_anchor;
shared uint selected_valid;
shared uint done;

uint anchor_span(gpu_anchor_wire a) { return a.span_flags_segment & 0xffu; }

uint64_t query_len() {
    return uint64_t(pc.query_len_lo) | (uint64_t(pc.query_len_hi) << 32);
}

bool better_candidate(int score, uint idx, int best_score, uint best_idx) {
    return score > best_score || (score == best_score && idx > best_idx);
}

void write_result(uint chain_len, int candidate_score) {
    uint first_idx = trace[chain_len - 1u];
    uint last_idx = trace[0u];
    gpu_anchor_wire first_anchor = anchors[first_idx];
    gpu_anchor_wire last_anchor = anchors[last_idx];
    uint first_span_u = anchor_span(first_anchor);
    int64_t first_span = int64_t(first_span_u);

    int64_t ref_start_i = int64_t(first_anchor.ref_position) + int64_t(1) - first_span;
    if (ref_start_i < int64_t(0)) ref_start_i = int64_t(0);
    uint64_t ref_end = last_anchor.ref_position + uint64_t(1);

    int64_t raw_q_start = int64_t(first_anchor.query_position) + int64_t(1) - first_span;
    if (raw_q_start < int64_t(0)) raw_q_start = int64_t(0);
    uint64_t raw_q_end = last_anchor.query_position + uint64_t(1);

    uint64_t q_start;
    uint64_t q_end;
    if (first_anchor.is_reverse == 0u) {
        q_start = uint64_t(raw_q_start);
        q_end = raw_q_end;
    } else {
        uint64_t qlen = query_len();
        q_start = qlen - raw_q_end;
        q_end = qlen - uint64_t(raw_q_start);
    }

    int64_t match_len = int64_t(first_span_u);
    int64_t block_len = int64_t(first_span_u);
    uint prev_idx = first_idx;
    int t = int(chain_len) - 2;
    while (t >= 0) {
        uint cur_idx = trace[uint(t)];
        gpu_anchor_wire prev = anchors[prev_idx];
        gpu_anchor_wire cur = anchors[cur_idx];
        int span = int(anchor_span(cur));
        int target_gap = int(cur.ref_position - prev.ref_position);
        int query_gap = int(cur.query_position - prev.query_position);
        int max_gap = target_gap > query_gap ? target_gap : query_gap;
        int min_gap = target_gap < query_gap ? target_gap : query_gap;
        block_len += int64_t(max_gap);
        match_len += int64_t((target_gap > span && query_gap > span) ? span : min_gap);
        prev_idx = cur_idx;
        --t;
    }

    result.query_start = q_start;
    result.query_end = q_end;
    result.target_start = uint64_t(ref_start_i);
    result.target_end = ref_end;
    result.match_length = uint64_t(match_len);
    result.block_length = uint64_t(block_len);
    result.chain_score = candidate_score;
    result.seed_count = int(chain_len);
    result.is_reverse = first_anchor.is_reverse != 0u ? 1u : 0u;
    result.valid = 1u;
}

void main() {
    uint lane = gl_LocalInvocationID.x;
    if (lane == 0u) {
        result.valid = 0u;
        done = 0u;
    }
    barrier();

    for (uint attempt = 0u; attempt < pc.probe_limit; ++attempt) {
        if (done != 0u) break;

        int local_score = pc.min_chain_score - 1;
        uint local_idx = 0u;
        for (uint i = lane; i < pc.anchor_count; i += compile_time_workgroup_size_x) {
            int score = best_scores[i];
            if (visited[i] == 0u && better_candidate(score, i, local_score, local_idx)) {
                local_score = score;
                local_idx = i;
            }
        }
        sh_score[lane] = local_score;
        sh_index[lane] = local_idx;
        barrier();

        if (lane == 0u) {
            int best_score = pc.min_chain_score - 1;
            uint best_idx = 0u;
            uint valid = 0u;
            for (uint i = 0u; i < compile_time_workgroup_size_x; ++i) {
                if (better_candidate(sh_score[i], sh_index[i], best_score, best_idx)) {
                    best_score = sh_score[i];
                    best_idx = sh_index[i];
                    valid = 1u;
                }
            }
            selected_score = best_score;
            selected_anchor = best_idx;
            selected_valid = valid;
        }
        barrier();

        if (lane == 0u) {
            if (selected_valid == 0u || selected_score < pc.min_chain_score) {
                done = 1u;
            } else {
                uint chain_len = 0u;
                uint stop_idx = 0xffffffffu;
                int64_t trace_idx = int64_t(selected_anchor);
                while (trace_idx >= int64_t(0)
                       && uint64_t(trace_idx) < uint64_t(pc.anchor_count)
                       && chain_len < pc.anchor_count) {
                    uint idx = uint(trace_idx);
                    if (visited[idx] != 0u) {
                        stop_idx = idx;
                        break;
                    }
                    trace[chain_len] = idx;
                    ++chain_len;
                    int64_t next_idx = predecessors[idx];
                    if (next_idx >= trace_idx) {
                        chain_len = 0u;
                        break;
                    }
                    trace_idx = next_idx;
                }

                int candidate_score = selected_score;
                if (stop_idx != 0xffffffffu) {
                    candidate_score -= best_scores[stop_idx];
                }

                if (candidate_score >= pc.min_chain_score
                    && chain_len >= uint(pc.min_seed_count)) {
                    write_result(chain_len, candidate_score);
                    done = 1u;
                } else {
                    for (uint i = 0u; i < chain_len; ++i) {
                        visited[trace[i]] = 1u;
                    }
                }
            }
        }
        barrier();
    }
}
)GLSL";

/// @brief Collects accepted chains and selects candidates for GPU alignment.
/// Classifies overlapping candidates as primary or secondary using their query
/// coverage and chain scores. If accepted chains fill the candidate array, the
/// next accepted chain reports overflow and the host rejects the operation.
inline constexpr std::string_view k_chain_candidate_reduce_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const uint k_status_ok = 0u;
const uint k_status_candidate_overflow = 1u;
const uint k_status_invalid_input = 2u;
const uint k_status_invalid_chain = 3u;
const uint k_status_repetitive_order = 4u;
const uint k_candidate_valid = 1u;
const uint k_candidate_retained = 2u;
const uint k_candidate_primary = 4u;
const uint k_candidate_reverse = 16u;
const uint k_candidate_known_flags = 31u;

struct gpu_anchor_wire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct coordinate_result {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int seed_count;
    uint is_reverse;
    uint valid;
};

struct candidate_header {
    uint candidate_offset;
    uint candidate_count;
    uint retained_count;
    uint primary_count;
    uint alignment_offset;
    uint alignment_count;
    uint repetitive_span;
    uint status;
    uint candidate_capacity;
    uint reserved;
};

struct chain_candidate {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int original_chain_score;
    int seed_count;
    int parent_index;
    int secondary_chain_score;
    int secondary_count;
    int dp_score;
    int dp_max_score;
    int secondary_dp_max;
    uint candidate_index;
    uint problem_index;
    uint flags;
};

layout(std430, set = 0, binding = 0) readonly buffer anchors_buffer {
    gpu_anchor_wire anchors[];
};
layout(std430, set = 0, binding = 1) readonly buffer best_scores_buffer {
    int best_scores[];
};
layout(std430, set = 0, binding = 2) readonly buffer predecessors_buffer {
    int64_t predecessors[];
};
layout(std430, set = 0, binding = 3) buffer visited_buffer {
    uint visited[];
};
layout(std430, set = 0, binding = 4) buffer trace_buffer {
    uint trace[];
};
layout(std430, set = 0, binding = 5) buffer result_buffer {
    coordinate_result results[];
};
layout(std430, set = 0, binding = 6) buffer header_buffer {
    candidate_header headers[];
};
layout(std430, set = 0, binding = 7) buffer candidates_buffer {
    chain_candidate candidates[];
};
layout(std430, set = 0, binding = 8) readonly buffer minimizer_words_buffer {
    uint minimizer_words[];
};
layout(std430, set = 0, binding = 9) readonly buffer repetitive_flags_buffer {
    uint repetitive_flags[];
};

#define result results[0]
#define header headers[0]

layout(push_constant) uniform push_constants {
    uint anchor_count;
    uint query_len_lo;
    uint query_len_hi;
    int min_chain_score;
    int min_seed_count;
    uint candidate_capacity;
    uint problem_index;
    uint minimizer_offset;
    uint minimizer_count;
    uint minimizer_words32_per_record;
    uint minimizer_word_kind;
    uint pos_bits;
    uint kmer_size;
    uint mask_level_bits;
    int mask_len;
    uint pri_ratio_bits;
    int min_diff;
    int best_n;
    uint repetitive_flag_extent;
    uint reserved;
} pc;

shared int sh_score[compile_time_workgroup_size_x];
shared uint sh_index[compile_time_workgroup_size_x];
shared uint primary_indices[compile_time_maximum_span];
shared uint64_t coverage_intervals[compile_time_maximum_span];
shared int selected_score;
shared uint selected_anchor;
shared uint selected_valid;
shared uint accepted_count;
shared uint done;

uint anchor_span(gpu_anchor_wire anchor) {
    return anchor.span_flags_segment & 0xffu;
}

uint64_t query_len() {
    return uint64_t(pc.query_len_lo) | (uint64_t(pc.query_len_hi) << 32u);
}

bool better_candidate(int score, uint index, int prior_score, uint prior_index) {
    return score > prior_score || (score == prior_score && index > prior_index);
}

bool minimizer_position(uint local_index, out uint64_t query_position) {
    query_position = uint64_t(0);
    uint index = pc.minimizer_offset + local_index;
    if (index < pc.minimizer_offset || pc.minimizer_words32_per_record == 0u ||
        pc.minimizer_word_kind > 3u) {
        return false;
    }
    if (pc.minimizer_word_kind == 0u) {
        uint packed = minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        uint mask = pc.pos_bits >= 15u ? 0xffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        query_position = uint64_t(raw & mask) >> 1u;
        return true;
    }
    uint base = index * pc.minimizer_words32_per_record;
    if (base / pc.minimizer_words32_per_record != index) return false;
    if (pc.minimizer_word_kind == 1u) {
        uint mask = pc.pos_bits >= 31u ? 0xffffffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        query_position = uint64_t(minimizer_words[base] & mask) >> 1u;
        return true;
    }
    if (pc.minimizer_word_kind == 2u) {
        uint64_t raw = uint64_t(minimizer_words[base]) |
            (uint64_t(minimizer_words[base + 1u]) << 32u);
        uint64_t mask = pc.pos_bits >= 63u
            ? ~uint64_t(0)
            : ((uint64_t(1) << (pc.pos_bits + 1u)) - uint64_t(1));
        query_position = (raw & mask) >> 1u;
        return true;
    }
    uint64_t raw = uint64_t(minimizer_words[base]) |
        (uint64_t(minimizer_words[base + 1u]) << 32u);
    query_position = raw >> 1u;
    return true;
}

bool build_candidate(uint chain_len, int candidate_score, uint output_index) {
    uint first_index = trace[chain_len - 1u];
    uint last_index = trace[0u];
    gpu_anchor_wire first_anchor = anchors[first_index];
    gpu_anchor_wire last_anchor = anchors[last_index];
    uint first_span = anchor_span(first_anchor);
    uint64_t qlen = query_len();
    if (first_span == 0u || first_anchor.is_reverse > 1u ||
        last_anchor.is_reverse != first_anchor.is_reverse ||
        first_anchor.ref_position + uint64_t(1u) < uint64_t(first_span) ||
        first_anchor.query_position + uint64_t(1u) < uint64_t(first_span)) {
        return false;
    }

    uint64_t ref_start = first_anchor.ref_position + uint64_t(1u) - uint64_t(first_span);
    uint64_t ref_end = last_anchor.ref_position + uint64_t(1u);
    uint64_t raw_q_start =
        first_anchor.query_position + uint64_t(1u) - uint64_t(first_span);
    uint64_t raw_q_end = last_anchor.query_position + uint64_t(1u);
    if (raw_q_start >= raw_q_end || raw_q_end > qlen || ref_start >= ref_end) {
        return false;
    }

    uint64_t q_start = raw_q_start;
    uint64_t q_end = raw_q_end;
    if (first_anchor.is_reverse != 0u) {
        q_start = qlen - raw_q_end;
        q_end = qlen - raw_q_start;
    }

    int64_t match_len = int64_t(first_span);
    int64_t block_len = int64_t(first_span);
    uint previous_index = first_index;
    int trace_position = int(chain_len) - 2;
    while (trace_position >= 0) {
        uint current_index = trace[uint(trace_position)];
        gpu_anchor_wire previous = anchors[previous_index];
        gpu_anchor_wire current = anchors[current_index];
        if (current.is_reverse != first_anchor.is_reverse ||
            current.ref_position < previous.ref_position ||
            current.query_position < previous.query_position) {
            return false;
        }
        int span = int(anchor_span(current));
        int64_t target_gap = int64_t(current.ref_position - previous.ref_position);
        int64_t query_gap = int64_t(current.query_position - previous.query_position);
        int64_t maximum_gap = target_gap > query_gap ? target_gap : query_gap;
        int64_t minimum_gap = target_gap < query_gap ? target_gap : query_gap;
        block_len += maximum_gap;
        match_len += target_gap > int64_t(span) && query_gap > int64_t(span)
            ? int64_t(span) : minimum_gap;
        previous_index = current_index;
        --trace_position;
    }
    if (match_len <= int64_t(0) || block_len <= int64_t(0)) return false;

    chain_candidate candidate;
    candidate.query_start = q_start;
    candidate.query_end = q_end;
    candidate.target_start = ref_start;
    candidate.target_end = ref_end;
    candidate.match_length = uint64_t(match_len);
    candidate.block_length = uint64_t(block_len);
    candidate.chain_score = candidate_score;
    candidate.original_chain_score = candidate_score;
    candidate.seed_count = int(chain_len);
    candidate.parent_index = -1;
    candidate.secondary_chain_score = 0;
    candidate.secondary_count = 0;
    candidate.dp_score = 0;
    candidate.dp_max_score = 0;
    candidate.secondary_dp_max = 0;
    candidate.candidate_index = output_index;
    candidate.problem_index = pc.problem_index;
    candidate.flags = k_candidate_valid |
        (first_anchor.is_reverse != 0u ? k_candidate_reverse : 0u);
    candidates[output_index] = candidate;
    return true;
}

uint overlap_length(uint start_a, uint end_a, uint start_b, uint end_b) {
    uint start = max(start_a, start_b);
    uint end = min(end_a, end_b);
    return end > start ? end - start : 0u;
}

bool classify_and_select() {
    float mask_level = uintBitsToFloat(pc.mask_level_bits);
    float primary_ratio = uintBitsToFloat(pc.pri_ratio_bits);
    if (isnan(mask_level) || isinf(mask_level) || mask_level < 0.0f ||
        isnan(primary_ratio) || isinf(primary_ratio) || primary_ratio <= 0.0f ||
        pc.mask_len < 0 || pc.min_diff < 0 || pc.best_n < 0) {
        return false;
    }
    if (accepted_count == 0u) {
        header.retained_count = 0u;
        header.primary_count = 0u;
        return true;
    }

    uint primary_count = 1u;
    primary_indices[0u] = 0u;
    candidates[0u].parent_index = 0;
    candidates[0u].flags |= k_candidate_primary;

    for (uint candidate_index = 1u; candidate_index < accepted_count; ++candidate_index) {
        chain_candidate current = candidates[candidate_index];
        if (current.query_start > uint64_t(0xffffffffu) ||
            current.query_end > uint64_t(0xffffffffu)) {
            return false;
        }
        uint current_start = uint(current.query_start);
        uint current_end = uint(current.query_end);
        uint coverage_count = 0u;
        for (uint primary_ordinal = 0u;
             primary_ordinal < primary_count;
             ++primary_ordinal) {
            chain_candidate primary = candidates[primary_indices[primary_ordinal]];
            uint primary_start = uint(primary.query_start);
            uint primary_end = uint(primary.query_end);
            uint overlap_start = max(primary_start, current_start);
            uint overlap_end = min(primary_end, current_end);
            if (overlap_end > overlap_start) {
                coverage_intervals[coverage_count++] =
                    (uint64_t(overlap_start) << 32u) | uint64_t(overlap_end);
            }
        }
        for (uint insertion = 1u; insertion < coverage_count; ++insertion) {
            uint64_t key = coverage_intervals[insertion];
            uint cursor = insertion;
            while (cursor > 0u && coverage_intervals[cursor - 1u] > key) {
                coverage_intervals[cursor] = coverage_intervals[cursor - 1u];
                --cursor;
            }
            coverage_intervals[cursor] = key;
        }
        uint uncovered = 0u;
        uint coverage_position = current_start;
        for (uint coverage_index = 0u;
             coverage_index < coverage_count;
             ++coverage_index) {
            uint interval_start = uint(coverage_intervals[coverage_index] >> 32u);
            uint interval_end = uint(coverage_intervals[coverage_index]);
            if (interval_start > coverage_position) {
                uncovered += interval_start - coverage_position;
            }
            coverage_position = max(coverage_position, interval_end);
        }
        if (current_end > coverage_position) {
            uncovered += current_end - coverage_position;
        }

        bool secondary = false;
        for (uint primary_ordinal = 0u;
             primary_ordinal < primary_count;
             ++primary_ordinal) {
            uint primary_index = primary_indices[primary_ordinal];
            chain_candidate primary = candidates[primary_index];
            uint primary_start = uint(primary.query_start);
            uint primary_end = uint(primary.query_end);
            uint overlap = overlap_length(
                current_start, current_end, primary_start, primary_end);
            if (overlap == 0u) continue;
            uint current_span = current_end - current_start;
            uint primary_span = primary_end - primary_start;
            uint minimum_span = min(current_span, primary_span);
            uint maximum_span = max(current_span, primary_span);
            if (minimum_span == 0u || maximum_span == 0u) return false;
            if (float(overlap) / float(minimum_span) -
                    float(uncovered) / float(maximum_span) > mask_level &&
                uncovered <= uint(pc.mask_len)) {
                current.parent_index = primary.parent_index;
                primary.secondary_chain_score =
                    max(primary.secondary_chain_score, current.chain_score);
                if (current.seed_count >= primary.seed_count) {
                    primary.secondary_count += 1;
                }
                candidates[primary_index] = primary;
                secondary = true;
                break;
            }
        }
        if (!secondary) {
            if (primary_count >= pc.candidate_capacity) return false;
            current.parent_index = int(candidate_index);
            current.flags |= k_candidate_primary;
            primary_indices[primary_count++] = candidate_index;
        }
        candidates[candidate_index] = current;
    }

    uint retained_count = 0u;
    uint retained_secondaries = 0u;
    for (uint candidate_index = 0u; candidate_index < accepted_count; ++candidate_index) {
        chain_candidate candidate = candidates[candidate_index];
        bool keep = candidate.parent_index == int(candidate_index);
        if (!keep) {
            if (candidate.parent_index < 0 ||
                uint(candidate.parent_index) >= accepted_count) {
                return false;
            }
            chain_candidate parent = candidates[uint(candidate.parent_index)];
            int64_t score_with_difference =
                int64_t(candidate.chain_score) + int64_t(pc.min_diff);
            bool passes_score =
                float(candidate.chain_score) >=
                    float(parent.chain_score) * primary_ratio ||
                score_with_difference >= int64_t(parent.chain_score);
            bool duplicate =
                candidate.query_start == parent.query_start &&
                candidate.query_end == parent.query_end &&
                candidate.target_start == parent.target_start &&
                candidate.target_end == parent.target_end;
            keep = passes_score && !duplicate &&
                retained_secondaries < uint(pc.best_n);
            if (keep) ++retained_secondaries;
        }
        if (keep) {
            candidate.flags |= k_candidate_retained;
            ++retained_count;
        }
        if ((candidate.flags & ~k_candidate_known_flags) != 0u) return false;
        candidates[candidate_index] = candidate;
    }
    header.retained_count = retained_count;
    header.primary_count = primary_count;
    return retained_count != 0u && (candidates[0u].flags & k_candidate_retained) != 0u;
}

bool compute_repetitive_span(out uint repetitive_span) {
    repetitive_span = 0u;
    if (pc.minimizer_count > pc.repetitive_flag_extent ||
        pc.kmer_size == 0u || pc.pos_bits >= 64u) {
        return false;
    }
    uint64_t previous_position = uint64_t(0);
    bool position_seen = false;
    uint64_t union_start = uint64_t(0);
    uint64_t union_end = uint64_t(0);
    uint64_t total = uint64_t(0);
    for (uint local_index = 0u; local_index < pc.minimizer_count; ++local_index) {
        if (repetitive_flags[local_index] > 1u) return false;
        uint64_t position;
        if (!minimizer_position(local_index, position) || position >= query_len() ||
            (position_seen && position < previous_position)) {
            header.status = k_status_repetitive_order;
            return false;
        }
        previous_position = position;
        position_seen = true;
        if (repetitive_flags[local_index] == 0u) continue;
        uint64_t interval_end = position + uint64_t(1u);
        uint64_t interval_start = interval_end > uint64_t(pc.kmer_size)
            ? interval_end - uint64_t(pc.kmer_size) : uint64_t(0);
        if (union_end == uint64_t(0)) {
            union_start = interval_start;
            union_end = interval_end;
        } else if (interval_start > union_end) {
            total += union_end - union_start;
            union_start = interval_start;
            union_end = interval_end;
        } else {
            union_end = max(union_end, interval_end);
        }
    }
    if (union_end != uint64_t(0)) total += union_end - union_start;
    if (total > uint64_t(0xffffffffu)) return false;
    repetitive_span = uint(total);
    return true;
}

void copy_primary_result() {
    chain_candidate primary = candidates[0u];
    result.query_start = primary.query_start;
    result.query_end = primary.query_end;
    result.target_start = primary.target_start;
    result.target_end = primary.target_end;
    result.match_length = primary.match_length;
    result.block_length = primary.block_length;
    result.chain_score = primary.chain_score;
    result.seed_count = primary.seed_count;
    result.is_reverse =
        (primary.flags & k_candidate_reverse) != 0u ? 1u : 0u;
    result.valid = 1u;
}

void main() {
    uint lane = gl_LocalInvocationID.x;
    if (lane == 0u) {
        header.candidate_offset = 0u;
        header.candidate_count = 0u;
        header.retained_count = 0u;
        header.primary_count = 0u;
        header.alignment_offset = 0u;
        header.alignment_count = 0u;
        header.repetitive_span = 0u;
        header.status = k_status_ok;
        header.candidate_capacity = pc.candidate_capacity;
        header.reserved = 0u;
        result.valid = 0u;
        accepted_count = 0u;
        done = 0u;
        if (pc.anchor_count == 0u ||
            pc.candidate_capacity != compile_time_maximum_span ||
            pc.candidate_capacity == 0u || pc.reserved != 0u ||
            query_len() == uint64_t(0)) {
            header.status = k_status_invalid_input;
            done = 1u;
        }
    }
    barrier();

    while (done == 0u) {
        int local_score = pc.min_chain_score - 1;
        uint local_index = 0u;
        for (uint anchor_index = lane;
             anchor_index < pc.anchor_count;
             anchor_index += compile_time_workgroup_size_x) {
            int score = best_scores[anchor_index];
            if (visited[anchor_index] == 0u &&
                better_candidate(score, anchor_index, local_score, local_index)) {
                local_score = score;
                local_index = anchor_index;
            }
        }
        sh_score[lane] = local_score;
        sh_index[lane] = local_index;
        barrier();

        if (lane == 0u) {
            int best_score = pc.min_chain_score - 1;
            uint best_index = 0u;
            uint valid = 0u;
            for (uint reduction_index = 0u;
                 reduction_index < compile_time_workgroup_size_x;
                 ++reduction_index) {
                if (better_candidate(sh_score[reduction_index],
                                     sh_index[reduction_index],
                                     best_score,
                                     best_index)) {
                    best_score = sh_score[reduction_index];
                    best_index = sh_index[reduction_index];
                    valid = 1u;
                }
            }
            selected_score = best_score;
            selected_anchor = best_index;
            selected_valid = valid;
        }
        barrier();

        if (lane == 0u) {
            if (selected_valid == 0u || selected_score < pc.min_chain_score) {
                done = 1u;
            } else {
                uint chain_len = 0u;
                uint stop_index = 0xffffffffu;
                int64_t trace_index = int64_t(selected_anchor);
                while (trace_index >= int64_t(0) &&
                       uint64_t(trace_index) < uint64_t(pc.anchor_count) &&
                       chain_len < pc.anchor_count) {
                    uint index = uint(trace_index);
                    if (visited[index] != 0u) {
                        stop_index = index;
                        break;
                    }
                    trace[chain_len++] = index;
                    visited[index] = 1u;
                    int64_t next_index = predecessors[index];
                    if (next_index >= trace_index) {
                        header.status = k_status_invalid_chain;
                        done = 1u;
                        chain_len = 0u;
                        break;
                    }
                    trace_index = next_index;
                }
                if (done == 0u && chain_len == pc.anchor_count &&
                    predecessors[trace[chain_len - 1u]] >= int64_t(0)) {
                    header.status = k_status_invalid_chain;
                    done = 1u;
                }
                if (done == 0u) {
                    int candidate_score = selected_score;
                    if (stop_index != 0xffffffffu) {
                        candidate_score -= best_scores[stop_index];
                    }
                    if (candidate_score >= pc.min_chain_score &&
                        chain_len >= uint(pc.min_seed_count)) {
                        if (accepted_count == pc.candidate_capacity) {
                            header.status = k_status_candidate_overflow;
                            done = 1u;
                        } else if (!build_candidate(
                                       chain_len, candidate_score, accepted_count)) {
                            header.status = k_status_invalid_chain;
                            done = 1u;
                        } else {
                            ++accepted_count;
                        }
                    }
                }
            }
        }
        barrier();
    }

    if (lane == 0u && header.status == k_status_ok) {
        header.candidate_count = accepted_count;
        uint repetitive_span = 0u;
        if (!compute_repetitive_span(repetitive_span)) {
            if (header.status == k_status_ok) header.status = k_status_invalid_input;
        } else {
            header.repetitive_span = repetitive_span;
        }
        if (header.status == k_status_ok && !classify_and_select()) {
            header.status = k_status_invalid_input;
        }
        if (header.status == k_status_ok && accepted_count != 0u) {
            copy_primary_result();
        }
    }
}
)GLSL";

/// @brief Reclassifies aligned candidates and computes their mapping quality (MAPQ).
/// Each candidate selected for alignment must match an ordered CIGAR result.
/// Missing or invalid alignment data fails the job. Scores account for competing
/// mappings, alignment identity, and repetitive query sequence before results
/// are published.
inline constexpr std::string_view k_mapping_quality_reduce_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const uint k_status_ok = 0u;
const uint k_status_invalid_input = 2u;
const uint k_status_alignment_mismatch = 5u;
const uint k_status_classification = 6u;
const uint k_status_numeric = 7u;
const uint k_status_output_order = 8u;
const uint k_candidate_valid = 1u;
const uint k_candidate_retained = 2u;
const uint k_candidate_primary = 4u;
const uint k_candidate_aligned = 8u;
const uint k_candidate_known_flags = 31u;

struct candidate_header {
    uint candidate_offset;
    uint candidate_count;
    uint retained_count;
    uint primary_count;
    uint alignment_offset;
    uint alignment_count;
    uint repetitive_span;
    uint status;
    uint candidate_capacity;
    uint reserved;
};
struct chain_candidate {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int original_chain_score;
    int seed_count;
    int parent_index;
    int secondary_chain_score;
    int secondary_count;
    int dp_score;
    int dp_max_score;
    int secondary_dp_max;
    uint candidate_index;
    uint problem_index;
    uint flags;
};
struct alignment_binding {
    uint problem_index;
    uint candidate_index;
};
struct alignment_result {
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
struct mapq_result {
    uint problem_index;
    uint candidate_index;
    int mapq;
    uint status;
    uint is_primary;
    uint publish;
    uint publication_ordinal;
    uint reserved1;
};

layout(std430, set = 0, binding = 0) buffer headers_buffer {
    candidate_header headers[];
};
layout(std430, set = 0, binding = 1) buffer candidates_buffer {
    chain_candidate candidates[];
};
layout(std430, set = 0, binding = 2) readonly buffer bindings_buffer {
    alignment_binding bindings[];
};
layout(std430, set = 0, binding = 3) readonly buffer alignment_results_buffer {
    alignment_result alignment_results[];
};
layout(std430, set = 0, binding = 4) buffer mapq_results_buffer {
    mapq_result mapq_results[];
};

layout(push_constant) uniform push_constants {
    uint problem_count;
    uint candidate_count;
    uint alignment_count;
    int min_chain_score;
    int match_score;
    uint mask_level_bits;
    int mask_len;
    int sub_diff;
    uint pri_ratio_bits;
    int min_diff;
    int best_n;
    uint candidate_capacity;
    uint is_short_read;
    uint is_splice;
    uint reserved;
} pc;

shared uint primary_indices[compile_time_maximum_span];
shared uint ranked_indices[compile_time_maximum_span];
shared uint64_t coverage_intervals[compile_time_maximum_span];

uint overlap_length(uint start_a, uint end_a, uint start_b, uint end_b) {
    uint start = max(start_a, start_b);
    uint end = min(end_a, end_b);
    return end > start ? end - start : 0u;
}

void set_failure(uint problem_index, uint status) {
    candidate_header header = headers[problem_index];
    header.status = status;
    headers[problem_index] = header;
    if (header.candidate_count > pc.candidate_capacity ||
        header.candidate_offset > pc.candidate_count ||
        header.candidate_count > pc.candidate_count - header.candidate_offset) {
        return;
    }
    for (uint local_index = 0u; local_index < header.candidate_count; ++local_index) {
        uint output_index = header.candidate_offset + local_index;
        mapq_results[output_index].problem_index = problem_index;
        mapq_results[output_index].candidate_index = local_index;
        mapq_results[output_index].mapq = 0;
        mapq_results[output_index].status = status;
        mapq_results[output_index].is_primary = 0u;
        mapq_results[output_index].publish = 0u;
        mapq_results[output_index].publication_ordinal = 0xffffffffu;
        mapq_results[output_index].reserved1 = 0u;
    }
}

bool classify_post_alignment(uint problem_index,
                             inout candidate_header header,
                             out uint ranked_count) {
    float mask_level = uintBitsToFloat(pc.mask_level_bits);
    float primary_ratio = uintBitsToFloat(pc.pri_ratio_bits);
    if (isnan(mask_level) || isinf(mask_level) || mask_level < 0.0f ||
        isnan(primary_ratio) || isinf(primary_ratio) || primary_ratio <= 0.0f ||
        pc.mask_len < 0 || pc.sub_diff < 0 || pc.min_diff < 0 || pc.best_n < 0) {
        return false;
    }

    ranked_count = 0u;
    for (uint local_index = 0u; local_index < header.candidate_count; ++local_index) {
        uint candidate_offset = header.candidate_offset + local_index;
        chain_candidate candidate = candidates[candidate_offset];
        if ((candidate.flags & k_candidate_aligned) == 0u) continue;
        candidate.flags &= ~(k_candidate_retained | k_candidate_primary);
        candidate.parent_index = -1;
        // Preserve chain-only evidence accumulated before physical alignment. Candidates
        // filtered before alignment still contribute to the secondary score/count.
        candidate.secondary_dp_max = 0;
        candidates[candidate_offset] = candidate;

        // Keep wire/result indices stable, classify by descending physical DP score,
        // and preserve the authenticated pre-alignment order for equal scores. This
        // is the deterministic Vulkan tie-break; numeric minimap2 MAPQ parity is not
        // part of this ABI's validity contract.
        if (ranked_count >= pc.candidate_capacity) return false;
        uint insertion = ranked_count;
        while (insertion > 0u) {
            uint prior_local_index = ranked_indices[insertion - 1u];
            chain_candidate prior =
                candidates[header.candidate_offset + prior_local_index];
            if (prior.dp_max_score >= candidate.dp_max_score) break;
            ranked_indices[insertion] = prior_local_index;
            --insertion;
        }
        ranked_indices[insertion] = local_index;
        ++ranked_count;
    }
    if (ranked_count == 0u) return false;

    uint primary_count = 0u;
    for (uint ranked_ordinal = 0u; ranked_ordinal < ranked_count; ++ranked_ordinal) {
        uint local_index = ranked_indices[ranked_ordinal];
        uint candidate_offset = header.candidate_offset + local_index;
        chain_candidate candidate = candidates[candidate_offset];
        if (primary_count == 0u) {
            candidate.parent_index = int(local_index);
            candidate.flags |= k_candidate_primary;
            primary_indices[primary_count++] = local_index;
            candidates[candidate_offset] = candidate;
            continue;
        }
        if (candidate.query_start > uint64_t(0xffffffffu) ||
            candidate.query_end > uint64_t(0xffffffffu)) {
            return false;
        }
        uint current_start = uint(candidate.query_start);
        uint current_end = uint(candidate.query_end);
        uint coverage_count = 0u;
        for (uint primary_ordinal = 0u;
             primary_ordinal < primary_count;
             ++primary_ordinal) {
            chain_candidate primary =
                candidates[header.candidate_offset + primary_indices[primary_ordinal]];
            uint overlap_start = max(uint(primary.query_start), current_start);
            uint overlap_end = min(uint(primary.query_end), current_end);
            if (overlap_end > overlap_start) {
                coverage_intervals[coverage_count++] =
                    (uint64_t(overlap_start) << 32u) | uint64_t(overlap_end);
            }
        }
        for (uint insertion = 1u; insertion < coverage_count; ++insertion) {
            uint64_t key = coverage_intervals[insertion];
            uint cursor = insertion;
            while (cursor > 0u && coverage_intervals[cursor - 1u] > key) {
                coverage_intervals[cursor] = coverage_intervals[cursor - 1u];
                --cursor;
            }
            coverage_intervals[cursor] = key;
        }
        uint uncovered = 0u;
        uint coverage_position = current_start;
        for (uint coverage_index = 0u;
             coverage_index < coverage_count;
             ++coverage_index) {
            uint interval_start = uint(coverage_intervals[coverage_index] >> 32u);
            uint interval_end = uint(coverage_intervals[coverage_index]);
            if (interval_start > coverage_position) {
                uncovered += interval_start - coverage_position;
            }
            coverage_position = max(coverage_position, interval_end);
        }
        if (current_end > coverage_position) {
            uncovered += current_end - coverage_position;
        }

        bool secondary = false;
        for (uint primary_ordinal = 0u;
             primary_ordinal < primary_count;
             ++primary_ordinal) {
            uint primary_local_index = primary_indices[primary_ordinal];
            uint primary_offset = header.candidate_offset + primary_local_index;
            chain_candidate primary = candidates[primary_offset];
            uint current_span = current_end - current_start;
            uint primary_start = uint(primary.query_start);
            uint primary_end = uint(primary.query_end);
            uint primary_span = primary_end - primary_start;
            uint overlap = overlap_length(
                current_start, current_end, primary_start, primary_end);
            if (overlap == 0u) continue;
            uint minimum_span = min(current_span, primary_span);
            uint maximum_span = max(current_span, primary_span);
            if (minimum_span == 0u || maximum_span == 0u) return false;
            if (float(overlap) / float(minimum_span) -
                    float(uncovered) / float(maximum_span) > mask_level &&
                uncovered <= uint(pc.mask_len)) {
                candidate.parent_index = primary.parent_index;
                primary.secondary_chain_score =
                    max(primary.secondary_chain_score, candidate.chain_score);
                bool count_secondary =
                    candidate.seed_count >= primary.seed_count;
                bool distinct_alignment =
                    primary.target_start != candidate.target_start ||
                    primary.target_end != candidate.target_end ||
                    overlap != minimum_span;
                if (distinct_alignment) {
                    primary.secondary_dp_max =
                        max(primary.secondary_dp_max, candidate.dp_max_score);
                    if (primary.dp_max_score - candidate.dp_max_score <= pc.sub_diff) {
                        count_secondary = true;
                    }
                }
                if (count_secondary) {
                    primary.secondary_count += 1;
                }
                candidates[primary_offset] = primary;
                secondary = true;
                break;
            }
        }
        if (!secondary) {
            if (primary_count >= pc.candidate_capacity) return false;
            candidate.parent_index = int(local_index);
            // A newly promoted primary starts a fresh secondary count; only the
            // first DP-ranked record preserves its pre-alignment count.
            candidate.secondary_count = 0;
            candidate.flags |= k_candidate_primary;
            primary_indices[primary_count++] = local_index;
        }
        candidates[candidate_offset] = candidate;
    }

    uint retained_count = 0u;
    uint retained_secondaries = 0u;
    for (uint ranked_ordinal = 0u; ranked_ordinal < ranked_count; ++ranked_ordinal) {
        uint local_index = ranked_indices[ranked_ordinal];
        uint candidate_offset = header.candidate_offset + local_index;
        chain_candidate candidate = candidates[candidate_offset];
        if ((candidate.flags & k_candidate_aligned) == 0u) continue;
        bool keep = candidate.parent_index == int(local_index);
        if (!keep) {
            if (candidate.parent_index < 0 ||
                uint(candidate.parent_index) >= header.candidate_count) {
                return false;
            }
            chain_candidate parent =
                candidates[header.candidate_offset + uint(candidate.parent_index)];
            int64_t score_with_difference =
                int64_t(candidate.chain_score) + int64_t(pc.min_diff);
            bool passes_score =
                float(candidate.chain_score) >=
                    float(parent.chain_score) * primary_ratio ||
                score_with_difference >= int64_t(parent.chain_score);
            bool duplicate =
                candidate.query_start == parent.query_start &&
                candidate.query_end == parent.query_end &&
                candidate.target_start == parent.target_start &&
                candidate.target_end == parent.target_end;
            keep = passes_score && !duplicate &&
                retained_secondaries < uint(pc.best_n);
            if (keep) ++retained_secondaries;
        }
        if (keep) {
            candidate.flags |= k_candidate_retained;
            ++retained_count;
        }
        candidates[candidate_offset] = candidate;
    }
    header.retained_count = retained_count;
    header.primary_count = primary_count;
    return primary_count != 0u && retained_count != 0u;
}

void main() {
    uint problem_index = gl_WorkGroupID.x;
    if (problem_index >= pc.problem_count || gl_LocalInvocationID.x != 0u) return;
    candidate_header header = headers[problem_index];
    if (pc.candidate_capacity != compile_time_maximum_span ||
        pc.candidate_capacity == 0u || pc.match_score <= 0 ||
        pc.is_short_read != 0u || pc.is_splice != 0u || pc.reserved != 0u ||
        header.status != k_status_ok || header.reserved != 0u ||
        header.candidate_capacity != pc.candidate_capacity ||
        header.candidate_count > pc.candidate_capacity ||
        header.candidate_offset > pc.candidate_count ||
        header.candidate_count > pc.candidate_count - header.candidate_offset ||
        header.alignment_offset > pc.alignment_count ||
        header.alignment_count > pc.alignment_count - header.alignment_offset ||
        header.alignment_count != header.retained_count) {
        set_failure(problem_index, k_status_invalid_input);
        return;
    }

    uint alignment_cursor = 0u;
    for (uint local_index = 0u; local_index < header.candidate_count; ++local_index) {
        uint candidate_offset = header.candidate_offset + local_index;
        chain_candidate candidate = candidates[candidate_offset];
        mapq_result initial_result;
        initial_result.problem_index = problem_index;
        initial_result.candidate_index = local_index;
        initial_result.mapq = 0;
        initial_result.status = k_status_ok;
        initial_result.is_primary = 0u;
        initial_result.publish = 0u;
        initial_result.publication_ordinal = 0xffffffffu;
        initial_result.reserved1 = 0u;
        mapq_results[candidate_offset] = initial_result;
        if (candidate.problem_index != problem_index ||
            candidate.candidate_index != local_index ||
            (candidate.flags & k_candidate_valid) == 0u ||
            (candidate.flags & ~k_candidate_known_flags) != 0u) {
            set_failure(problem_index, k_status_output_order);
            return;
        }
        if ((candidate.flags & k_candidate_retained) == 0u) continue;
        if (alignment_cursor >= header.alignment_count) {
            set_failure(problem_index, k_status_alignment_mismatch);
            return;
        }
        uint alignment_index = header.alignment_offset + alignment_cursor;
        alignment_binding binding = bindings[alignment_index];
        alignment_result aligned = alignment_results[alignment_index];
        if (binding.problem_index != problem_index ||
            binding.candidate_index != local_index ||
            aligned.status != 0u || aligned.reserved != 0u ||
            aligned.cigar_length == 0u || aligned.block_length == 0u ||
            aligned.num_matches > aligned.block_length ||
            uint64_t(aligned.query_start) != candidate.query_start ||
            uint64_t(aligned.query_end) != candidate.query_end ||
            uint64_t(aligned.target_start) != candidate.target_start ||
            uint64_t(aligned.target_end) != candidate.target_end ||
            aligned.max_score <= 0) {
            set_failure(problem_index, k_status_alignment_mismatch);
            return;
        }
        candidate.dp_score = aligned.score;
        candidate.dp_max_score = aligned.max_score;
        candidate.match_length = uint64_t(aligned.num_matches);
        candidate.block_length = uint64_t(aligned.block_length);
        candidate.flags |= k_candidate_aligned;
        candidates[candidate_offset] = candidate;
        ++alignment_cursor;
    }
    if (alignment_cursor != header.alignment_count) {
        set_failure(problem_index, k_status_alignment_mismatch);
        return;
    }
    if (header.candidate_count == 0u) {
        header.primary_count = 0u;
        header.retained_count = 0u;
        headers[problem_index] = header;
        return;
    }
    uint ranked_count = 0u;
    if (!classify_post_alignment(problem_index, header, ranked_count)) {
        set_failure(problem_index, k_status_classification);
        return;
    }

    int64_t primary_score_sum = int64_t(0);
    for (uint local_index = 0u; local_index < header.candidate_count; ++local_index) {
        chain_candidate candidate =
            candidates[header.candidate_offset + local_index];
        if ((candidate.flags & (k_candidate_retained | k_candidate_primary)) ==
            (k_candidate_retained | k_candidate_primary)) {
            primary_score_sum += int64_t(candidate.chain_score);
        }
    }
    int64_t uniqueness_denominator =
        primary_score_sum + int64_t(header.repetitive_span);
    if (primary_score_sum <= int64_t(0) ||
        uniqueness_denominator <= int64_t(0)) {
        set_failure(problem_index, k_status_numeric);
        return;
    }
    float uniqueness_ratio =
        float(primary_score_sum) / float(uniqueness_denominator);
    if (isnan(uniqueness_ratio) || isinf(uniqueness_ratio)) {
        set_failure(problem_index, k_status_numeric);
        return;
    }

    uint published_candidates = 0u;
    for (uint ranked_ordinal = 0u; ranked_ordinal < ranked_count; ++ranked_ordinal) {
        uint local_index = ranked_indices[ranked_ordinal];
        uint candidate_offset = header.candidate_offset + local_index;
        chain_candidate candidate = candidates[candidate_offset];
        if ((candidate.flags & k_candidate_retained) == 0u) continue;
        bool is_primary = (candidate.flags & k_candidate_primary) != 0u;
        if (!is_primary) {
            mapq_results[candidate_offset].is_primary = 0u;
            mapq_results[candidate_offset].publish = 1u;
            mapq_results[candidate_offset].publication_ordinal = published_candidates;
            ++published_candidates;
            continue;
        }
        if (candidate.original_chain_score <= 0 ||
            candidate.seed_count <= 0 || candidate.dp_max_score <= 0 ||
            candidate.block_length == uint64_t(0)) {
            set_failure(problem_index, k_status_numeric);
            return;
        }
        float penalty_score =
            (candidate.chain_score > 100
                 ? 1.0f
                 : 0.01f * float(candidate.chain_score)) *
            uniqueness_ratio;
        float penalty_seed =
            candidate.seed_count > 10 ? 1.0f : 0.1f * float(candidate.seed_count);
        float penalty = min(penalty_score, penalty_seed);
        int secondary_score =
            max(candidate.secondary_chain_score, pc.min_chain_score);
        float identity =
            float(candidate.match_length) / float(candidate.block_length);
        float mapq_value;
        if (candidate.secondary_dp_max > 0) {
            float ratio =
                float(candidate.secondary_dp_max) * float(secondary_score) /
                float(candidate.dp_max_score) /
                float(candidate.original_chain_score);
            mapq_value =
                identity * penalty * 40.0f * (1.0f - ratio * ratio) *
                log(float(candidate.dp_max_score) / float(pc.match_score));
            float alternative =
                6.02f * identity * identity *
                    float(candidate.dp_max_score - candidate.secondary_dp_max) /
                    float(pc.match_score) +
                0.499f;
            mapq_value = min(mapq_value, alternative);
        } else {
            float ratio =
                float(secondary_score) / float(candidate.original_chain_score);
            mapq_value =
                identity * penalty * 40.0f * (1.0f - ratio) *
                log(float(candidate.dp_max_score) / float(pc.match_score));
        }
        if (isnan(mapq_value) || isinf(mapq_value) ||
            abs(mapq_value) > 2147483000.0f) {
            set_failure(problem_index, k_status_numeric);
            return;
        }
        int quality = int(mapq_value);
        float secondary_penalty =
            4.343f * log(float(candidate.secondary_count + 1)) + 0.499f;
        if (isnan(secondary_penalty) || isinf(secondary_penalty)) {
            set_failure(problem_index, k_status_numeric);
            return;
        }
        quality -= int(secondary_penalty);
        quality = clamp(quality, 0, 60);
        if (candidate.dp_max_score > candidate.secondary_dp_max &&
            quality == 0) {
            quality = 1;
        }
        mapq_results[candidate_offset].mapq = quality;
        mapq_results[candidate_offset].is_primary = 1u;
        mapq_results[candidate_offset].publish = 1u;
        mapq_results[candidate_offset].publication_ordinal = published_candidates;
        ++published_candidates;
    }
    if (published_candidates != header.retained_count) {
        set_failure(problem_index, k_status_classification);
        return;
    }
    header.status = k_status_ok;
    headers[problem_index] = header;
}
)GLSL";

/// @brief Estimates a mapping position from anchors that support a similar diagonal.
/// Counts nearby target-minus-query offsets on the same strand, then uses the
/// best-supported offset to place the full query if it passes the thresholds.
inline constexpr std::string_view k_coordinate_vote_reduce_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

struct gpu_anchor_wire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct coordinate_result {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int seed_count;
    uint is_reverse;
    uint valid;
};

layout(std430, set = 0, binding = 0) readonly buffer anchors_buffer { gpu_anchor_wire anchors[]; };
layout(std430, set = 0, binding = 1) readonly buffer unused_1_buffer { int unused1[]; };
layout(std430, set = 0, binding = 2) readonly buffer unused_2_buffer { int64_t unused2[]; };
layout(std430, set = 0, binding = 3) buffer unused_3_buffer { uint unused3[]; };
layout(std430, set = 0, binding = 4) buffer unused_4_buffer { uint unused4[]; };
layout(std430, set = 0, binding = 5) buffer result_buffer { coordinate_result result; };

layout(push_constant) uniform push_constants {
    uint anchor_count;
    uint query_len_lo;
    uint query_len_hi;
    int min_chain_score;
    int min_seed_count;
    uint probe_limit;
    uint reserved;
} pc;

shared int sh_score[compile_time_workgroup_size_x];
shared uint sh_index[compile_time_workgroup_size_x];
shared int selected_score;
shared uint selected_anchor;

uint anchor_span(gpu_anchor_wire a) { return a.span_flags_segment & 0xffu; }

uint64_t query_len() {
    return uint64_t(pc.query_len_lo) | (uint64_t(pc.query_len_hi) << 32);
}

int64_t anchor_diag(gpu_anchor_wire a) {
    return int64_t(a.ref_position) - int64_t(a.query_position);
}

bool better_candidate(int score, uint idx, int best_score, uint best_idx) {
    return score > best_score || (score == best_score && idx < best_idx);
}

void write_vote_result(uint idx, int score) {
    gpu_anchor_wire a = anchors[idx];
    uint span = anchor_span(a);
    int64_t diag = anchor_diag(a);
    if (diag < int64_t(0)) {
        diag = int64_t(0);
    }
    uint64_t qlen = query_len();
    uint64_t target_start = uint64_t(diag);
    result.query_start = uint64_t(0);
    result.query_end = qlen;
    result.target_start = target_start;
    result.target_end = target_start + qlen;
    result.match_length = qlen;
    result.block_length = qlen;
    result.chain_score = score * int(span == 0u ? 1u : span);
    result.seed_count = score;
    result.is_reverse = a.is_reverse != 0u ? 1u : 0u;
    result.valid = 1u;
}

void main() {
    uint lane = gl_LocalInvocationID.x;
    if (lane == 0u) {
        result.valid = 0u;
        selected_score = 0;
        selected_anchor = 0u;
    }
    barrier();

    int local_score = 0;
    uint local_idx = 0u;
    for (uint i = lane; i < pc.anchor_count; i += compile_time_workgroup_size_x) {
        gpu_anchor_wire cand = anchors[i];
        int64_t cand_diag = anchor_diag(cand);
        int score = 0;
        for (uint j = 0u; j < pc.anchor_count; ++j) {
            gpu_anchor_wire other = anchors[j];
            if (other.is_reverse != cand.is_reverse) {
                continue;
            }
            int64_t d = anchor_diag(other) - cand_diag;
            if (d < int64_t(0)) {
                d = -d;
            }
            if (d <= int64_t(50)) {
                ++score;
            }
        }
        if (better_candidate(score, i, local_score, local_idx)) {
            local_score = score;
            local_idx = i;
        }
    }
    sh_score[lane] = local_score;
    sh_index[lane] = local_idx;
    barrier();

    if (lane == 0u) {
        int best_score = 0;
        uint best_idx = 0u;
        for (uint i = 0u; i < compile_time_workgroup_size_x; ++i) {
            if (better_candidate(sh_score[i], sh_index[i], best_score, best_idx)) {
                best_score = sh_score[i];
                best_idx = sh_index[i];
            }
        }
        selected_score = best_score;
        selected_anchor = best_idx;
    }
    barrier();

    if (lane == 0u) {
        if (pc.anchor_count != 0u
            && selected_score >= pc.min_seed_count
            && selected_score * int(anchor_span(anchors[selected_anchor])) >= pc.min_chain_score) {
            write_vote_result(selected_anchor, selected_score);
        }
    }
}
)GLSL";

/// @brief Estimates one mapping position per job using sampled diagonal votes.
/// Each workgroup compares candidate offsets against at most 32 evenly spaced
/// anchors. It scales the winning vote count to the job size before testing
/// the score and seed-count thresholds.
inline constexpr std::string_view k_coordinate_vote_reduce_batch_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

struct gpu_anchor_wire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct coordinate_result {
    uint64_t query_start;
    uint64_t query_end;
    uint64_t target_start;
    uint64_t target_end;
    uint64_t match_length;
    uint64_t block_length;
    int chain_score;
    int seed_count;
    uint is_reverse;
    uint valid;
};

struct vote_job {
    uint anchor_offset;
    uint anchor_count;
    uint query_len_lo;
    uint query_len_hi;
};

layout(std430, set = 0, binding = 0) readonly buffer anchors_buffer { gpu_anchor_wire anchors[]; };
layout(std430, set = 0, binding = 1) readonly buffer vote_jobs_buffer { vote_job jobs[]; };
layout(std430, set = 0, binding = 2) readonly buffer unused_2_buffer { int unused2[]; };
layout(std430, set = 0, binding = 3) buffer unused_3_buffer { uint unused3[]; };
layout(std430, set = 0, binding = 4) buffer unused_4_buffer { uint unused4[]; };
layout(std430, set = 0, binding = 5) buffer results_buffer { coordinate_result results[]; };

layout(push_constant) uniform push_constants {
    uint job_count;
    uint unused_query_len_lo;
    uint unused_query_len_hi;
    int min_chain_score;
    int min_seed_count;
    uint probe_limit;
    uint reserved;
} pc;

const uint k_vote_sample_limit = 32u;

shared int sh_score[compile_time_workgroup_size_x];
shared uint sh_index[compile_time_workgroup_size_x];
shared int selected_score;
shared uint selected_anchor;

uint anchor_span(gpu_anchor_wire a) { return a.span_flags_segment & 0xffu; }

uint64_t query_len(vote_job job) {
    return uint64_t(job.query_len_lo) | (uint64_t(job.query_len_hi) << 32);
}

gpu_anchor_wire job_anchor(vote_job job, uint idx) {
    return anchors[job.anchor_offset + idx];
}

int64_t anchor_diag(gpu_anchor_wire a) {
    return int64_t(a.ref_position) - int64_t(a.query_position);
}

bool better_candidate(int score, uint idx, int best_score, uint best_idx) {
    return score > best_score || (score == best_score && idx < best_idx);
}

void write_vote_result(uint job_id, vote_job job, uint idx, int score) {
    gpu_anchor_wire a = job_anchor(job, idx);
    uint span = anchor_span(a);
    int64_t diag = anchor_diag(a);
    if (diag < int64_t(0)) {
        diag = int64_t(0);
    }
    uint64_t qlen = query_len(job);
    uint64_t target_start = uint64_t(diag);
    results[job_id].query_start = uint64_t(0);
    results[job_id].query_end = qlen;
    results[job_id].target_start = target_start;
    results[job_id].target_end = target_start + qlen;
    results[job_id].match_length = qlen;
    results[job_id].block_length = qlen;
    results[job_id].chain_score = score * int(span == 0u ? 1u : span);
    results[job_id].seed_count = score;
    results[job_id].is_reverse = a.is_reverse != 0u ? 1u : 0u;
    results[job_id].valid = 1u;
}

void main() {
    uint job_id = gl_WorkGroupID.x;
    if (job_id >= pc.job_count) return;

    vote_job job = jobs[job_id];
    uint lane = gl_LocalInvocationID.x;
    if (lane == 0u) {
        results[job_id].valid = 0u;
        selected_score = 0;
        selected_anchor = 0u;
    }
    barrier();

    uint sample_count = min(job.anchor_count, k_vote_sample_limit);
    int local_score = 0;
    uint local_idx = 0u;
    for (uint i = lane; i < job.anchor_count; i += compile_time_workgroup_size_x) {
        gpu_anchor_wire cand = job_anchor(job, i);
        int64_t cand_diag = anchor_diag(cand);
        int score = 0;
        for (uint s = 0u; s < sample_count; ++s) {
            uint j = (s * job.anchor_count) / sample_count;
            gpu_anchor_wire other = job_anchor(job, j);
            if (other.is_reverse != cand.is_reverse) {
                continue;
            }
            int64_t d = anchor_diag(other) - cand_diag;
            if (d < int64_t(0)) {
                d = -d;
            }
            if (d <= int64_t(50)) {
                ++score;
            }
        }
        if (better_candidate(score, i, local_score, local_idx)) {
            local_score = score;
            local_idx = i;
        }
    }
    sh_score[lane] = local_score;
    sh_index[lane] = local_idx;
    barrier();

    if (lane == 0u) {
        int best_score = 0;
        uint best_idx = 0u;
        for (uint i = 0u; i < compile_time_workgroup_size_x; ++i) {
            if (better_candidate(sh_score[i], sh_index[i], best_score, best_idx)) {
                best_score = sh_score[i];
                best_idx = sh_index[i];
            }
        }
        selected_score = best_score;
        selected_anchor = best_idx;
    }
    barrier();

    if (lane == 0u) {
        uint estimated_score = sample_count == 0u
            ? 0u
            : (uint(selected_score) * job.anchor_count + sample_count - 1u) / sample_count;
        if (job.anchor_count != 0u
            && estimated_score >= uint(pc.min_seed_count)
            && int(estimated_score) * int(anchor_span(job_anchor(job, selected_anchor)))
                >= pc.min_chain_score) {
            write_vote_result(job_id, job, selected_anchor, int(estimated_score));
        }
    }
}
)GLSL";

} // namespace bioseq::gpu::vulkan
