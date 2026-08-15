/**
 * @file chaining.cppm
 * @brief Owns chaining and coordinate-reduction compute shader bodies.
 */
module bioseq.gpu.vulkan.shader_compiler:chaining;

import :registry;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Tiled DP score-generation kernel body, beginning after the injected
/// compile-time constants. It consumes the 24-byte `gpu_anchor_wire` ABI with
/// full 64-bit reference/query positions and per-anchor `query_span`, accepts
/// `max_iter` at runtime, and emits absolute predecessor indices for
/// `backtrack_chains`. One workgroup owns one independent hard range, so the
/// tail sweep needs no atomics.
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
/// @brief Chain-range construction. One workgroup (1 invocation) per query
/// serially scans that query's sorted anchor span, emitting `GpuChainRange`
/// records at strand/chromosome changes and hard reference gaps, matching the
/// CPU `build_gpu_chain_ranges` predicate. When any hard range is very long,
/// the shader also performs the same endpoint-preserving thinning used by the
/// CPU-side Vulkan chainer so resident seed-to-coordinate does not feed huge
/// dense ranges into DP.
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

/// @brief Resident coordinate-only finalizer. It mirrors the bounded top-k CPU
/// coordinate extractor, but downloads only one compact mapping record.
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

/// @brief Selects one coordinate by bounded diagonal voting.
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

/// @brief Selects coordinates for a batch of independent diagonal-vote jobs.
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
