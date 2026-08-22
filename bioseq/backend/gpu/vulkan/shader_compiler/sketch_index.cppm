/**
 * @file sketch_index.cppm
 * @brief Owns sketch, scan, radix-sort, and target-index compute shader bodies.
 */
module bioseq.gpu.vulkan.shader_compiler:sketch_index;

import :registry;
import std;

namespace bioseq::gpu::vulkan {

// ===========================================================================
// Resident frontend kernels
// ===========================================================================

/// @brief Ordered tile-replay minimizer sketch. One workgroup per tile
/// (1 active invocation) replays the exact `collect_minimizers_view` state
/// machine over `[replay_lo, loop_hi)`, emitting only minimizers whose trigger
/// position falls in this tile's owned window interval `[tile_lo, tile_hi)`.
/// The left-context overlap (`tile_lo - replay_lo`) reconstructs the ring buffer
/// and the valid-base/init state so the concatenation across tiles in tile
/// order equals the single serial traversal — but it is parallel across tiles (never
/// a one-workgroup-per-sequence serial emit). `emit_mode==0` counts per tile.
/// `emit_mode==1` writes raw `MinimizerRepr` words at `out_base + local`.
// clang-format off
inline constexpr std::string_view k_ordered_sketch_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

struct sketch_tile {
    uint seq_word_offset;
    uint bp_count;
    uint replay_lo;
    uint tile_lo;
    uint tile_hi;
    uint out_base;
};

layout(std430, set = 0, binding = 0) readonly buffer sequence_words_buffer { uint seq_words[]; };
layout(std430, set = 0, binding = 1) readonly buffer rank_lookup_buffer {
    uint rank_to_code[256];
    uint rank_to_rc_code[256];
};
layout(std430, set = 0, binding = 2) readonly buffer tiles_buffer { sketch_tile tiles[]; };
layout(std430, set = 0, binding = 3) buffer tile_counts_buffer { uint tile_counts[]; };
layout(std430, set = 0, binding = 4) buffer output_words_buffer { uint out_words[]; };
layout(std430, set = 0, binding = 5) buffer counters_buffer { uint counters[]; };
layout(std430, set = 0, binding = 6) readonly buffer tile_offsets_buffer { uint tile_offsets[]; };
layout(std430, set = 0, binding = 7) buffer output_pair_values_buffer { uint64_t out_pair_vals[]; };
layout(std430, set = 0, binding = 8) readonly buffer query_gate_buffer { uint query_gate_words[]; };

layout(push_constant) uniform push_constants {
    uint emit_mode;
    uint window_size;
    uint kmer_size;
    uint hash_bits;
    uint hash_shift;
    uint pos_bits;
    uint word_kind;
    uint words32_per_minimizer;
    uint span_value;
    uint alphabet_kind;
    uint unit_bits;
    uint bits_per_char;
    uint letters_per_unit;
    uint char_mask;
    uint tile_count;
    uint emit_pairs;
    uint query_gate_mask;
    uint query_gate_word_count;
} pc;

)GLSL"
R"GLSL(
uint read_u32_word(uint word_index) { return seq_words[word_index]; }
uint read_storage_unit(uint unit_index, uint unit_bits, uint byte_offset_words) {
    if (unit_bits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return lo ^ hi;
    }
    if (unit_bits == 32u) { return read_u32_word(byte_offset_words + unit_index); }
    if (unit_bits == 16u) {
        uint w = read_u32_word(byte_offset_words + (unit_index >> 1u));
        return (w >> ((unit_index & 1u) * 16u)) & 0xffffu;
    }
    uint w = read_u32_word(byte_offset_words + (unit_index >> 2u));
    return (w >> ((unit_index & 3u) * 8u)) & 0xffu;
}
uint64_t read_storage_unit64(uint unit_index, uint unit_bits, uint byte_offset_words) {
    if (unit_bits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return (uint64_t(hi) << 32) | uint64_t(lo);
    }
    return uint64_t(read_storage_unit(unit_index, unit_bits, byte_offset_words));
}
uint read_rank_at_bp(uint bp, uint bits_per_char, uint letters_per_unit,
                     uint unit_bits, uint byte_offset_words, uint char_mask) {
    uint unit_index = bp / letters_per_unit;
    uint in_unit = bp - unit_index * letters_per_unit;
    uint shift = in_unit * bits_per_char;
    uint64_t unit = read_storage_unit64(unit_index, unit_bits, byte_offset_words);
    return uint((unit >> shift) & uint64_t(char_mask));
}
uint64_t minimap_scramble(uint64_t key, uint64_t mask) {
    key = (~key + (key << 21)) & mask;
    key = key ^ (key >> 24);
    key = ((key + (key << 3)) + (key << 8)) & mask;
    key = key ^ (key >> 14);
    key = ((key + (key << 2)) + (key << 4)) & mask;
    key = key ^ (key >> 28);
    key = (key + (key << 31)) & mask;
    return key;
}
uint code_for_rank(uint rank) {
    if (pc.alphabet_kind == 0u) return rank;
    return rank_to_code[rank];
}
uint rc_code_for_rank(uint rank) {
    if (pc.alphabet_kind == 0u) return rank ^ 3u;
    return rank_to_rc_code[rank];
}

uint64_t hash_field_mask() {
    return pc.hash_bits >= 64u ? ~uint64_t(0)
                               : ((uint64_t(1) << pc.hash_bits) - uint64_t(1));
}

// sketch element mirrors the scalar minimizer state machine element.
struct sketch { uint64_t hash; uint pos; uint strand; uint is_max; };
sketch max_sketch() { return sketch(hash_field_mask(), 0u, 0u, 1u); }
bool sketch_leq(sketch a, sketch b) { return a.hash <= b.hash; }
bool sketch_geq(sketch a, sketch b) { return a.hash >= b.hash; }
bool sketch_match_diff(sketch a, sketch b) {
    return a.is_max == 0u && b.is_max == 0u && a.hash == b.hash
        && (a.pos != b.pos || a.strand != b.strand);
}

bool query_gate_accepts(uint64_t hash) {
    if (pc.query_gate_word_count == 0u) return true;
    uint bit = uint(hash & uint64_t(pc.query_gate_mask));
    uint word = bit >> 5u;
    // A malformed non-empty gate must never broaden the accepted target set.
    if (word >= pc.query_gate_word_count) return false;
    return (query_gate_words[word] & (1u << (bit & 31u))) != 0u;
}

		const uint k_maximum_sketch_candidates = 384u;            // small shared footprint -> high core occupancy
	const uint k_target_emit_complete = 0x80000000u;
	const uint k_target_emit_status_capacity = 1u;
	const uint k_target_emit_status_parity = 2u;
	uint g_seq_word_offset;
	uint g_local_count;
	uint g_out_base;
	uint g_owned_lo;
	uint g_owned_hi;
	uint g_expected_count;
	bool g_target_emit_failed;

void fail_target_emit(uint status) {
    // Failure-only telemetry. The successful deterministic target route has no
    // append/status atomics at all.
    atomicOr(counters[2], status);
    g_target_emit_failed = true;
}

void write_minimizer(uint dst, sketch s) {
    // dst is tile-local; the global minimizer index is g_out_base + dst.
    uint gmin = g_out_base + dst;
    if (pc.word_kind == 0u) {
        uint raw = uint((s.hash << pc.hash_shift) | (uint64_t(s.pos) << 1) | uint64_t(s.strand));
        // u16 packs two minimizers per 32-bit word; the boundary word can be
        // shared by two parallel tiles, so the two half-word writes use 32-bit
        // atomics (the runtime zero-fills the output word range before emit).
        uint word = gmin >> 1u;
        uint shift = (gmin & 1u) * 16u;
        atomicAnd(out_words[word], ~(0xffffu << shift));
        atomicOr(out_words[word], (raw & 0xffffu) << shift);
    } else if (pc.word_kind == 1u) {
        uint base = gmin * pc.words32_per_minimizer;
        out_words[base] = uint((s.hash << pc.hash_shift) | (uint64_t(s.pos) << 1) | uint64_t(s.strand));
    } else if (pc.word_kind == 2u) {
        uint base = gmin * pc.words32_per_minimizer;
        uint64_t raw = (s.hash << pc.hash_shift) | (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        out_words[base + 0u] = uint(raw);
        out_words[base + 1u] = uint(raw >> 32);
    } else {
        uint base = gmin * pc.words32_per_minimizer;
        uint64_t low = (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        uint64_t high = (s.hash << 8) | uint64_t(pc.span_value & 0xffu);
        out_words[base + 0u] = uint(low);
        out_words[base + 1u] = uint(low >> 32);
        out_words[base + 2u] = uint(high);
        out_words[base + 3u] = uint(high >> 32);
    }
}

	bool owns_emit_position(sketch_tile t, uint emit_pos) {
	    return emit_pos >= g_owned_lo && emit_pos < g_owned_hi;
	}

	uint replay_overlap_bp() {
	    uint trigger_span = pc.kmer_size + pc.window_size + 1u;
	    return min(trigger_span, k_maximum_sketch_candidates - 1u);
	}

	uint ownership_guard_bp() {
	    return 0u;
	}

void emit_one(sketch_tile t, sketch s, uint emit_pos) {
    if (s.is_max != 0u || !owns_emit_position(t, emit_pos)) return;
    if (!query_gate_accepts(s.hash)) return;
    uint dst = g_local_count;
    g_local_count = g_local_count + 1u;
    // emit_mode 2 serves the independent sorted-pair API. Target-index
    // construction uses mode 0 tile counts + prefix scan followed by mode 1 and
    // does not enter this global append-atomic branch.
    if (pc.emit_mode == 2u) {
        uint gmin = atomicAdd(counters[0], 1u);
        if (gmin >= counters[1]) return;
        out_words[gmin * 2u + 0u] = uint(s.hash);
        out_words[gmin * 2u + 1u] = uint(s.hash >> 32);
        out_pair_vals[gmin] = (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        return;
    }
    if (pc.emit_mode == 1u) {
        if (pc.emit_pairs == 1u) {
            // Target index path: every tile owns exactly the half-open prefix
            // range [offset, offset + counted). Never let a divergent second
            // stage overwrite a neighbouring tile or exceed exact capacity T.
            if (g_target_emit_failed || dst >= g_expected_count) {
                fail_target_emit(k_target_emit_status_parity);
                return;
            }
            uint64_t gmin64 = uint64_t(g_out_base) + uint64_t(dst);
            if (gmin64 >= uint64_t(counters[1])) {
                fail_target_emit(k_target_emit_status_capacity);
                return;
            }
            uint gmin = uint(gmin64);
            out_words[gmin * 2u + 0u] = uint(s.hash);
            out_words[gmin * 2u + 1u] = uint(s.hash >> 32);
            out_pair_vals[gmin] = (uint64_t(s.pos) << 1) | uint64_t(s.strand);
        } else {
            write_minimizer(dst, s);
        }
    }
}

)GLSL"
R"GLSL(
// Rolling-hash ordered minimizer sketch. Each invocation owns one tile
// and runs minimap2's ordered window-minimum state machine over [replay_lo,
// loop_hi), emitting only for owned positions [tile_lo, tile_hi). The canonical
// k-mer hash is maintained incrementally (forward/reverse rolled one base per
// step), and every invocation runs the emit without lane-0 serialization.
// Output is bit-identical to `collect_minimizers_view`:
// `valid_since_reset` (forward-code validity) gates the state machine, while a
// separate `valid_rc` counter
// suppresses the candidate while a reverse-complement-invalid base sits in the
// window (a no-op for dna4/dna5 where rc-invalid implies fwd-invalid).
void process_tile(uint tile_id) {
    sketch_tile t = tiles[tile_id];
    g_seq_word_offset = t.seq_word_offset;
    g_owned_lo = t.tile_lo;
    g_owned_hi = tile_id + 1u >= pc.tile_count ? t.bp_count : t.tile_hi;
    uint loop_hi = tile_id + 1u >= pc.tile_count ? t.bp_count : min(t.bp_count, t.tile_hi);
    g_local_count = 0u;
    g_out_base = (pc.emit_mode == 1u) ? tile_offsets[tile_id] : 0u;
    g_expected_count = 0u;
    g_target_emit_failed = false;
    if (pc.emit_mode == 1u && pc.emit_pairs == 1u) {
        g_expected_count = tile_counts[tile_id];
        uint64_t range_end =
            uint64_t(g_out_base) + uint64_t(g_expected_count);
        if ((g_expected_count & k_target_emit_complete) != 0u
            || range_end > uint64_t(counters[1])) {
            fail_target_emit(k_target_emit_status_capacity);
        }
    }

    // Rolling canonical-kmer hash constants (constant per tile).
    uint step_bits = pc.alphabet_kind == 2u ? pc.bits_per_char : 2u;
    uint roll_bits = pc.kmer_size * step_bits;
    uint scramble_bits = pc.kmer_size * 2u;
    bool hash_ok = (roll_bits <= 64u) && (scramble_bits <= 64u);
    uint64_t roll_mask = (roll_bits >= 64u) ? ~uint64_t(0) : ((uint64_t(1) << roll_bits) - uint64_t(1));
    uint64_t scramble_mask = (scramble_bits >= 64u) ? ~uint64_t(0) : ((uint64_t(1) << scramble_bits) - uint64_t(1));
    uint shift_rc = step_bits * (pc.kmer_size - 1u);
    uint64_t hmask = hash_field_mask();

    uint64_t forward = uint64_t(0);
    uint64_t reverse = uint64_t(0);
    uint valid_rc = 0u;   // consecutive bases whose rc code < 4 (palindrome-safety vs candidate_at)

    sketch ring[compile_time_maximum_span];
    uint buf_pos = 0u;
    uint valid_since_reset = 0u;
    bool init_mode = (t.replay_lo == 0u);
    sketch current_min = max_sketch();
    uint current_min_pos = 0u;

    for (uint seq_pos = t.replay_lo; seq_pos < loop_hi; ++seq_pos) {
        bool is_init = init_mode;

        // --- rolling candidate ending at seq_pos (bit-exact with candidate_at) ---
        uint rank_here = read_rank_at_bp(seq_pos, pc.bits_per_char, pc.letters_per_unit,
                                         pc.unit_bits, g_seq_word_offset, pc.char_mask);
        uint code = code_for_rank(rank_here);
        uint rc = rc_code_for_rank(rank_here);
        bool base_valid = code < 4u;          // matches candidate_at base_valid (forward code only)
        if (code < 4u) {
            forward = ((forward << step_bits) | uint64_t(code)) & roll_mask;
        } else {
            forward = uint64_t(0);
        }
        if (rc < 4u) {
            reverse = (reverse >> step_bits) | (uint64_t(rc) << shift_rc);
            valid_rc = valid_rc + 1u;
        } else {
            reverse = uint64_t(0);
            valid_rc = 0u;
        }
        sketch cand = max_sketch();

        sketch pushed = max_sketch();
        if (!base_valid) {
            valid_since_reset = 0u;
        } else {
            valid_since_reset = valid_since_reset + 1u;
            if (hash_ok && valid_since_reset >= pc.kmer_size && valid_rc >= pc.kmer_size
                && forward != reverse) {
                uint64_t canonical = forward < reverse ? forward : reverse;
                uint64_t h = minimap_scramble(canonical, scramble_mask) & hmask;
                if (h != hmask) cand = sketch(h, seq_pos, forward < reverse ? 0u : 1u, 0u);
            }
            if (valid_since_reset >= pc.kmer_size && cand.is_max == 0u) pushed = cand;
        }
        uint prev_pos = buf_pos;
        ring[buf_pos] = pushed;
        buf_pos = (buf_pos + 1u) % pc.window_size;

        if ((valid_since_reset == pc.window_size + pc.kmer_size - 1u && is_init)
            && current_min.is_max == 0u) {
            for (uint j = buf_pos; j < pc.window_size; ++j) {
                if (sketch_match_diff(current_min, ring[j])) emit_one(t, ring[j], seq_pos);
            }
            for (uint j = 0u; j < buf_pos; ++j) {
                if (sketch_match_diff(current_min, ring[j])) emit_one(t, ring[j], seq_pos);
            }
        }

        if (sketch_leq(pushed, current_min)) {
            if ((valid_since_reset >= pc.window_size + pc.kmer_size || !is_init)
                && current_min.is_max == 0u) {
                emit_one(t, current_min, seq_pos);
            }
            current_min_pos = prev_pos;
            current_min = pushed;
        } else if (prev_pos == current_min_pos) {
            if ((valid_since_reset >= pc.window_size + pc.kmer_size - 1u || !is_init)
                && current_min.is_max == 0u) {
                emit_one(t, current_min, seq_pos);
            }
            current_min = max_sketch();
            for (uint j = buf_pos; j < pc.window_size; ++j) {
                if (sketch_geq(current_min, ring[j])) { current_min = ring[j]; current_min_pos = j; }
            }
            for (uint j = 0u; j < buf_pos; ++j) {
                if (sketch_geq(current_min, ring[j])) { current_min = ring[j]; current_min_pos = j; }
            }
            if ((valid_since_reset >= pc.window_size + pc.kmer_size - 1u || !is_init)
                && current_min.is_max == 0u) {
                for (uint j = buf_pos; j < pc.window_size; ++j) {
                    if (sketch_match_diff(current_min, ring[j])) emit_one(t, ring[j], seq_pos);
                }
                for (uint j = 0u; j < buf_pos; ++j) {
                    if (sketch_match_diff(current_min, ring[j])) emit_one(t, ring[j], seq_pos);
                }
            }
        }

        if (init_mode && valid_since_reset >= pc.window_size + pc.kmer_size) init_mode = false;
    }

    // Final emit belongs to the sequence end; only the owning final tile does it.
    if (loop_hi >= t.bp_count && current_min.is_max == 0u) {
        emit_one(t, current_min, t.bp_count > 0u ? t.bp_count - 1u : 0u);
    }

    if (pc.emit_mode == 0u) {
        tile_counts[tile_id] = g_local_count;
    } else if (pc.emit_mode == 1u && pc.emit_pairs == 1u) {
        // One invocation owns each tile count word, so completion publication
        // needs no atomic. The host accepts the result only when every marker
        // agrees with the adjacent prefix offsets and exact T.
        if (!g_target_emit_failed
            && g_local_count == g_expected_count
            && (g_local_count & k_target_emit_complete) == 0u) {
            tile_counts[tile_id] = k_target_emit_complete | g_local_count;
        } else {
            fail_target_emit(k_target_emit_status_parity);
        }
    }
}

void main() {
    if (pc.window_size == 0u || pc.window_size > compile_time_maximum_span || pc.kmer_size == 0u) return;
    // One invocation per tile, grid-strided so the dispatch workgroup count is
    // free to be ceil(tile_count / local_size) (every lane active) yet still
    // correct for any larger dispatch.
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint tile_id = gl_GlobalInvocationID.x; tile_id < pc.tile_count; tile_id += stride) {
        process_tile(tile_id);
    }
}
)GLSL";
// clang-format on

/// @brief Generic exclusive prefix scan over a `uint` array, producing block totals.
/// Each workgroup exclusive-scans its block into `out_prefix` and writes the
/// block total to `block_sums[wg]`. A second `add` stage adds the scanned block
/// offsets back. Two-level scan (block size = compile_time_workgroup_size_x handled serially by
/// invocation 0 to stay portable on MoltenVK without subgroup ops).
inline constexpr std::string_view k_prefix_scan_block_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer input_data_buffer { uint in_data[]; };
layout(std430, set = 0, binding = 1) buffer output_data_buffer { uint out_prefix[]; };
layout(std430, set = 0, binding = 2) buffer block_sums_buffer { uint block_sums[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint block_size;
    uint num_blocks;
    uint stage;
} pc;
void main() {
    if (gl_LocalInvocationID.x != 0u) return;
    // Grid-stride blocks let callers clamp dispatch X to the physical-device
    // limit without dropping logical scan blocks on XXL inputs.
    for (uint block = gl_WorkGroupID.x;
         block < pc.num_blocks;
         block += gl_NumWorkGroups.x) {
        uint lo = block * pc.block_size;
        uint hi = min(lo + pc.block_size, pc.element_count);
        uint running = 0u;
        for (uint i = lo; i < hi; ++i) {
            out_prefix[i] = running;
            running += in_data[i];
        }
        block_sums[block] = running;
    }
}
)GLSL";

/// @brief Prefix-scan offset addition. Invocation 0 of workgroup 0 exclusive-
/// scans `block_sums` in place, then every element adds its block's offset.
inline constexpr std::string_view k_prefix_scan_add_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer input_data_buffer { uint in_data[]; };
layout(std430, set = 0, binding = 1) buffer output_data_buffer { uint out_prefix[]; };
layout(std430, set = 0, binding = 2) buffer block_sums_buffer { uint block_sums[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint block_size;
    uint num_blocks;
    uint stage;
} pc;
void main() {
    uint gid = gl_GlobalInvocationID.x;
    // stage 0: single invocation exclusive-scans block_sums into block offsets.
    if (pc.stage == 0u) {
        if (gid != 0u) return;
        uint running = 0u;
        for (uint b = 0u; b < pc.num_blocks; ++b) {
            uint v = block_sums[b];
            block_sums[b] = running;
            running += v;
        }
        return;
    }
    // stage 1: add the scanned block offset to every element.
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint i = gid; i < pc.element_count; i += stride) {
        uint block = i / pc.block_size;
        out_prefix[i] += block_sums[block];
    }
}
)GLSL";
/// @brief Device radix sort over parallel (key:uint64, value:uint64)
/// arrays. One 256-lane workgroup processes each contiguous chunk. Lane 0
/// assigns stable scatter destinations and all lanes move the values. Histogram
/// stage: per-chunk 256-bin counts of digit `(key >> radix_shift) & 0xff`.
inline constexpr std::string_view k_radix_sort_histogram_body = R"GLSL(
// Cooperative per-workgroup histogram: 256 threads (one per bin) zero and write
// the shared histogram; all threads count this workgroup's chunk into it with
// shared atomics, so every lane contributes to reading the chunk.
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer keys_in_buffer { uint64_t keys_in[]; };
layout(std430, set = 0, binding = 1) readonly buffer values_in_buffer { uint64_t vals_in[]; };
layout(std430, set = 0, binding = 2) buffer keys_out_buffer { uint64_t keys_out[]; };
layout(std430, set = 0, binding = 3) buffer values_out_buffer { uint64_t vals_out[]; };
layout(std430, set = 0, binding = 4) buffer histogram_buffer { uint histogram[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint radix_shift;
    uint num_workgroups;
    uint elems_per_wg;
    uint value_words;
} pc;
shared uint sh_hist[256];
void main() {
    uint wg = gl_WorkGroupID.x;
    if (wg >= pc.num_workgroups) return;
    uint tid = gl_LocalInvocationID.x;
    sh_hist[tid] = 0u;
    barrier();
    uint64_t lo64 = min(
        uint64_t(wg) * uint64_t(pc.elems_per_wg),
        uint64_t(pc.element_count));
    uint lo = uint(lo64);
    uint hi = uint(min(
        lo64 + uint64_t(pc.elems_per_wg), uint64_t(pc.element_count)));
    uint64_t chunk_n = uint64_t(hi) - uint64_t(lo);
    for (uint64_t off = uint64_t(tid); off < chunk_n; off += uint64_t(256)) {
        uint i = uint(lo64 + off);
        uint digit = uint((keys_in[i] >> pc.radix_shift) & uint64_t(0xff));
        atomicAdd(sh_hist[digit], 1u);
    }
    barrier();
    histogram[wg * 256u + tid] = sh_hist[tid];
}
)GLSL";

/// @brief Multi-level histogram scan. One 256-lane workgroup
/// computes, for every (workgroup, bin), its stable global start offset:
/// `bin_base[bin] + sum_{wg' < wg} hist[wg'][bin]`, where `bin_base` is the
/// exclusive prefix over bin totals. Writes the offsets back into `histogram`.
inline constexpr std::string_view k_radix_sort_scan_body = R"GLSL(
// One workgroup of 256 threads — one thread per radix bin. Each bin's thread
// reduces its column of the per-workgroup histograms and then writes that
// column's per-workgroup global offsets. This keeps the scan O(num_workgroups)
// per thread (256-way parallel) instead of O(num_workgroups * 256) on a single
// lane and keeps work distributed across all radix bins.
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer keys_in_buffer { uint64_t keys_in[]; };
layout(std430, set = 0, binding = 1) readonly buffer values_in_buffer { uint64_t vals_in[]; };
layout(std430, set = 0, binding = 2) buffer keys_out_buffer { uint64_t keys_out[]; };
layout(std430, set = 0, binding = 3) buffer values_out_buffer { uint64_t vals_out[]; };
layout(std430, set = 0, binding = 4) buffer histogram_buffer { uint histogram[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint radix_shift;
    uint num_workgroups;
    uint elems_per_wg;
    uint value_words;
} pc;
shared uint sh_total[256];
shared uint sh_base[256];
void main() {
    uint b = gl_LocalInvocationID.x;          // radix bin (0..255)
    uint sum = 0u;
    for (uint wg = 0u; wg < pc.num_workgroups; ++wg) sum += histogram[wg * 256u + b];
    sh_total[b] = sum;
    barrier();
    if (b == 0u) {
        uint running = 0u;
        for (uint i = 0u; i < 256u; ++i) { sh_base[i] = running; running += sh_total[i]; }
    }
    barrier();
    uint acc = sh_base[b];
    for (uint wg = 0u; wg < pc.num_workgroups; ++wg) {
        uint idx = wg * 256u + b;
        uint c = histogram[idx];
        histogram[idx] = acc;
        acc += c;
    }
}
)GLSL";

/// @brief Stable scatter. Each 256-lane workgroup re-walks its
/// chunk in order, using the per-(wg,bin) global offsets from the scan as a
/// running cursor.
inline constexpr std::string_view k_radix_sort_scatter_body = R"GLSL(
// Cooperative stable scatter. Lane 0 walks the workgroup's chunk in order to
// assign each element its destination (= the per-bin global offset, advanced
// stably), which keeps the radix sort STABLE. All 256 lanes then perform the
// actual (uncoalesced) key/value writes in parallel. The chunk is processed in
// SUB-BLOCKS of 512 so the stable-destination array sh_dst[512] is
// never overrun even when elems_per_wg > 512 — which occurs once num_workgroups
// is clamped to the device maxComputeWorkGroupCount[0] (the Vulkan minimum is
// 65535; Apple Silicon reports ~1e9 so a single block usually suffices, but the
// loop keeps it correct on any device). The per-bin cursor persists across
// sub-blocks, so LSD stability holds across the whole chunk.
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer keys_in_buffer { uint64_t keys_in[]; };
layout(std430, set = 0, binding = 1) readonly buffer values_in_buffer { uint64_t vals_in[]; };
layout(std430, set = 0, binding = 2) buffer keys_out_buffer { uint64_t keys_out[]; };
layout(std430, set = 0, binding = 3) buffer values_out_buffer { uint64_t vals_out[]; };
layout(std430, set = 0, binding = 4) buffer histogram_buffer { uint histogram[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint radix_shift;
    uint num_workgroups;
    uint elems_per_wg;
    uint value_words;
} pc;
shared uint sh_cursor[256];
shared uint sh_dst[512];
void main() {
    uint wg = gl_WorkGroupID.x;
    if (wg >= pc.num_workgroups) return;
    uint tid = gl_LocalInvocationID.x;
    uint64_t lo64 = uint64_t(wg) * uint64_t(pc.elems_per_wg);
    if (lo64 >= uint64_t(pc.element_count)) return;
    uint lo = uint(lo64);
    uint hi = uint(min(
        lo64 + uint64_t(pc.elems_per_wg), uint64_t(pc.element_count)));
    sh_cursor[tid] = histogram[wg * 256u + tid];
    barrier();
    uint vw = pc.value_words;
    for (uint block_lo = lo; block_lo < hi;) {
        uint block_hi = hi - block_lo < 512u ? hi : block_lo + 512u;
        uint block_n = block_hi - block_lo;
        if (tid == 0u) {
            for (uint i = 0u; i < block_n; ++i) {
                uint digit = uint((keys_in[block_lo + i] >> pc.radix_shift) & uint64_t(0xff));
                sh_dst[i] = sh_cursor[digit];
                sh_cursor[digit] = sh_cursor[digit] + 1u;
            }
        }
        barrier();
        for (uint i = tid; i < block_n; i += 256u) {
            uint dst = sh_dst[i];
            keys_out[dst] = keys_in[block_lo + i];
            for (uint w = 0u; w < vw; ++w) vals_out[dst * vw + w] = vals_in[(block_lo + i) * vw + w];
        }
        barrier();  // writes must finish before the next sub-block overwrites sh_dst
        block_lo = block_hi;
    }
}
)GLSL";

/// @brief GPU target-index run marking. `flags[i] = 1` where a
/// new hash run begins in the sorted key array.
inline constexpr std::string_view k_index_mark_runs_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;
struct bucket_record { uint64_t hash; uint pos_offset; uint pos_count; };
struct serialized_bucket { uint64_t hash; uint pos_offset; uint pos_count; };
layout(std430, set = 0, binding = 0) readonly buffer sorted_keys_buffer { uint64_t sorted_keys[]; };
layout(std430, set = 0, binding = 1) buffer flags_buffer { uint flags[]; };
layout(std430, set = 0, binding = 2) readonly buffer scanned_buffer { uint scanned[]; };
layout(std430, set = 0, binding = 3) buffer buckets_buffer { bucket_record buckets[]; };
layout(std430, set = 0, binding = 4) buffer slot_claims_buffer { uint slot_claims[]; };
layout(std430, set = 0, binding = 5) buffer slots_buffer { serialized_bucket slots[]; };
layout(std430, set = 0, binding = 6) buffer bucket_slot_buffer { uint bucket_slot[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint slot_mask;
    uint slot_capacity;
    uint num_buckets;
    uint probe_cap;
    uint stage;
} pc;
void main() {
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint i = gl_GlobalInvocationID.x;
         i < pc.element_count;
         i += stride) {
        flags[i] =
            (i == 0u || sorted_keys[i] != sorted_keys[i - 1u]) ? 1u : 0u;
    }
}
)GLSL";

/// @brief Dense bucket emission. Each element atomically adds to its bucket's
/// `pos_count`. Run starts set the bucket `hash`/`pos_offset` (the sorted
/// position index). `bucket_index = inclusive_scan(flags) - 1`.
inline constexpr std::string_view k_index_emit_buckets_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;
struct bucket_record { uint64_t hash; uint pos_offset; uint pos_count; };
struct serialized_bucket { uint64_t hash; uint pos_offset; uint pos_count; };
layout(std430, set = 0, binding = 0) readonly buffer sorted_keys_buffer { uint64_t sorted_keys[]; };
layout(std430, set = 0, binding = 1) readonly buffer flags_buffer { uint flags[]; };
layout(std430, set = 0, binding = 2) readonly buffer scanned_buffer { uint scanned[]; };
layout(std430, set = 0, binding = 3) buffer buckets_buffer { bucket_record buckets[]; };
layout(std430, set = 0, binding = 4) buffer slot_claims_buffer { uint slot_claims[]; };
layout(std430, set = 0, binding = 5) buffer slots_buffer { serialized_bucket slots[]; };
layout(std430, set = 0, binding = 6) buffer bucket_slot_buffer { uint bucket_slot[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint slot_mask;
    uint slot_capacity;
    uint num_buckets;
    uint probe_cap;
    uint stage;
} pc;
void main() {
    uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
    for (uint i = gl_GlobalInvocationID.x;
         i < pc.element_count;
         i += stride) {
        uint bidx = scanned[i] + flags[i] - 1u;
        atomicAdd(buckets[bidx].pos_count, 1u);
        if (flags[i] == 1u) {
            buckets[bidx].hash = sorted_keys[i];
            buckets[bidx].pos_offset = i;
        }
    }
}
)GLSL";

/// @brief Race-free slot claim via a 32-bit atomicCompSwap linear probe.
inline constexpr std::string_view k_index_claim_slots_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;
struct bucket_record { uint64_t hash; uint pos_offset; uint pos_count; };
struct serialized_bucket { uint64_t hash; uint pos_offset; uint pos_count; };
layout(std430, set = 0, binding = 0) readonly buffer sorted_keys_buffer { uint64_t sorted_keys[]; };
layout(std430, set = 0, binding = 1) buffer flags_buffer { uint flags[]; };
layout(std430, set = 0, binding = 2) readonly buffer scanned_buffer { uint scanned[]; };
layout(std430, set = 0, binding = 3) readonly buffer buckets_buffer { bucket_record buckets[]; };
layout(std430, set = 0, binding = 4) buffer slot_claims_buffer { uint slot_claims[]; };
layout(std430, set = 0, binding = 5) buffer slots_buffer { serialized_bucket slots[]; };
layout(std430, set = 0, binding = 6) buffer bucket_slot_buffer { uint bucket_slot[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint slot_mask;
    uint slot_capacity;
    uint num_buckets;
    uint probe_cap;
    uint stage;
} pc;
uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15UL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9UL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebUL;
    return x ^ (x >> 31);
}
void main() {
    uint b = gl_GlobalInvocationID.x;
    if (b >= pc.num_buckets) return;
    uint p = uint(splitmix64(buckets[b].hash)) & pc.slot_mask;
    for (uint step = 0u; step < pc.probe_cap; ++step) {
        uint old = atomicCompSwap(slot_claims[p], 0xffffffffu, b);
        if (old == 0xffffffffu) { bucket_slot[b] = p; return; }
        p = (p + 1u) & pc.slot_mask;
    }
    bucket_slot[b] = 0xffffffffu;
}
)GLSL";

/// @brief Write a serialized_bucket into its claimed slot.
inline constexpr std::string_view k_index_emit_slots_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;
struct bucket_record { uint64_t hash; uint pos_offset; uint pos_count; };
struct serialized_bucket { uint64_t hash; uint pos_offset; uint pos_count; };
layout(std430, set = 0, binding = 0) readonly buffer sorted_keys_buffer { uint64_t sorted_keys[]; };
layout(std430, set = 0, binding = 1) buffer flags_buffer { uint flags[]; };
layout(std430, set = 0, binding = 2) readonly buffer scanned_buffer { uint scanned[]; };
layout(std430, set = 0, binding = 3) readonly buffer buckets_buffer { bucket_record buckets[]; };
layout(std430, set = 0, binding = 4) buffer slot_claims_buffer { uint slot_claims[]; };
layout(std430, set = 0, binding = 5) buffer slots_buffer { serialized_bucket slots[]; };
layout(std430, set = 0, binding = 6) readonly buffer bucket_slot_buffer { uint bucket_slot[]; };
layout(push_constant) uniform push_constants {
    uint element_count;
    uint slot_mask;
    uint slot_capacity;
    uint num_buckets;
    uint probe_cap;
    uint stage;
} pc;
void main() {
    uint b = gl_GlobalInvocationID.x;
    if (b >= pc.num_buckets) return;
    uint p = bucket_slot[b];
    if (p == 0xffffffffu) return;
    slots[p].hash = buckets[b].hash;
    slots[p].pos_offset = buckets[b].pos_offset;
    slots[p].pos_count = buckets[b].pos_count;
}
)GLSL";

} // namespace bioseq::gpu::vulkan
