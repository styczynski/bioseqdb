/**
 * @file seeding.cppm
 * @brief Compute shaders for finding seed matches and sorting their anchors.
 */
module bioseq.gpu.vulkan.shader_compiler:seeding;

import :registry;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Emit anchors from the minimizer index into ranges assigned by the
/// prefix counts. Each query minimizer looks up its target positions and writes
/// the matching strand and coordinates.
inline constexpr std::string_view k_seed_lookup_anchors_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

struct gpu_minimizer_slot {
    uint64_t hash;
    uint pos_offset;
    uint pos_count;
};

struct gpu_anchor_wire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment; // span | flags<<8 | segment_id<<16
};

layout(std430, set = 0, binding = 0) readonly buffer minimizer_words_buffer {
    uint minimizer_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer slots_buffer { gpu_minimizer_slot slots[]; };
layout(std430, set = 0, binding = 2) readonly buffer positions_buffer { uint64_t positions[]; };
layout(std430, set = 0, binding = 3) readonly buffer prefix_counts_buffer { uint prefix_counts[]; };
layout(std430, set = 0, binding = 4) buffer anchors_buffer { gpu_anchor_wire anchors[]; };
layout(std430, set = 0, binding = 5) buffer anchor_keys_buffer { uint64_t anchor_keys[]; };

layout(push_constant) uniform push_constants {
    uint minimizer_count;
    uint words32_per_minimizer;
    uint word_kind;
    uint hash_shift;
    uint pos_bits;
    uint query_length;
    uint kmer_size;
    uint slot_mask;
    uint hash_major_control_count;
    uint exact_anchor_count;
    uint target_ref_bits;
    uint query_minimizer_record_offset;
} pc;

uint64_t anchor_sort_key(uint reverse, uint64_t ref_position) {
    return (uint64_t(reverse) << pc.target_ref_bits) | ref_position;
}

uint64_t minimizer_fragment_hash(uint index) {
    if (pc.word_kind == 0u) {
        uint packed = minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        return uint64_t(raw) >> pc.hash_shift;
    }
    uint base = index * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        return uint64_t(minimizer_words[base]) >> pc.hash_shift;
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(minimizer_words[base])
                     | (uint64_t(minimizer_words[base + 1u]) << 32);
        return raw >> pc.hash_shift;
    }
    uint64_t high = uint64_t(minimizer_words[base + 2u])
                  | (uint64_t(minimizer_words[base + 3u]) << 32);
    return high >> 8;
}

uint64_t minimizer_fragment_position(uint index) {
    if (pc.word_kind == 0u) {
        uint packed = minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        uint mask = pc.pos_bits >= 15u
            ? 0xffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    uint base = index * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        uint raw = minimizer_words[base];
        uint mask = pc.pos_bits >= 31u
            ? 0xffffffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(minimizer_words[base])
                     | (uint64_t(minimizer_words[base + 1u]) << 32);
        uint64_t mask = pc.pos_bits >= 63u
            ? ~uint64_t(0)
            : ((uint64_t(1) << (pc.pos_bits + 1u)) - uint64_t(1));
        return raw & mask;
    }
    return uint64_t(minimizer_words[base])
         | (uint64_t(minimizer_words[base + 1u]) << 32);
}

uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15UL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9UL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebUL;
    return x ^ (x >> 31);
}

uint find_hash_slot(uint64_t hash) {
    if (pc.hash_major_control_count != 0u) {
        uint lo = 0u;
        uint hi = pc.hash_major_control_count;
        while (lo < hi) {
            uint mid = lo + ((hi - lo) >> 1u);
            if (slots[mid].hash < hash) lo = mid + 1u; else hi = mid;
        }
        if (lo < pc.hash_major_control_count && slots[lo].hash == hash) {
            return lo;
        }
        return 0xffffffffu;
    }
    uint slot_index = uint(splitmix64(hash)) & pc.slot_mask;
    for (uint step = 0u; step <= pc.slot_mask; ++step) {
        gpu_minimizer_slot candidate = slots[slot_index];
        if (candidate.pos_count == 0u) return 0xffffffffu;
        if (candidate.hash == hash) return slot_index;
        slot_index = (slot_index + 1u) & pc.slot_mask;
    }
    return 0xffffffffu;
}

void main() {
    uint minimizer_index = gl_GlobalInvocationID.x;
    if (minimizer_index >= pc.minimizer_count) return;

    uint physical_minimizer_index =
        pc.query_minimizer_record_offset + minimizer_index;
    uint64_t hash = minimizer_fragment_hash(physical_minimizer_index);
    uint slot_index = find_hash_slot(hash);
    if (slot_index == 0xffffffffu) return;

    gpu_minimizer_slot slot = slots[slot_index];
    uint begin = prefix_counts[minimizer_index];
    uint end = prefix_counts[minimizer_index + 1u];
    if (slot.pos_count == 0u || begin == end) return;

    uint64_t query_encoded =
        minimizer_fragment_position(physical_minimizer_index);
    uint64_t query_position_bp = query_encoded >> 1;
    uint query_strand = uint(query_encoded & uint64_t(1));
    uint64_t reverse_base = uint64_t(pc.query_length - 2u + pc.kmer_size);
    uint span_flags_segment = pc.kmer_size & 0xffu;

    for (uint occurrence_index = 0u;
         occurrence_index < slot.pos_count;
         ++occurrence_index) {
        uint output_index = begin + occurrence_index;
        if (output_index >= pc.exact_anchor_count) return;
        uint64_t target_encoded =
            positions[slot.pos_offset + occurrence_index];
        uint target_strand = uint(target_encoded & uint64_t(1));
        uint reverse = target_strand ^ query_strand;
        uint64_t query_position = reverse != 0u
            ? reverse_base - query_position_bp : query_position_bp;
        uint64_t ref_position = target_encoded >> 1;
        anchors[output_index].ref_position = ref_position;
        anchors[output_index].query_position = query_position;
        anchors[output_index].is_reverse = reverse;
        anchors[output_index].span_flags_segment = span_flags_segment;
        anchor_keys[output_index] = anchor_sort_key(reverse, ref_position);
    }
}
)GLSL";

/// @brief Sort one query's anchors by strand and reference position in shared
/// memory. Equal keys keep their input order. The range must fit in one
/// workgroup. The runtime uses GPU radix sorting for larger ranges.
inline constexpr std::string_view k_sort_anchors_bitonic_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

struct gpu_anchor_wire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

struct sort_range {
    uint begin;
    uint range_len;  // NB: not 'length' — that clashes with GLSL's .length() builtin
};

layout(std430, set = 0, binding = 0) buffer anchors_buffer { gpu_anchor_wire anchors[]; };
layout(std430, set = 0, binding = 1) readonly buffer sort_ranges_buffer { sort_range ranges[]; };

shared gpu_anchor_wire sh_anchor[compile_time_workgroup_size_x];
shared uint sh_orig[compile_time_workgroup_size_x];

uint64_t ref_bits(gpu_anchor_wire a) {
    return (uint64_t(a.is_reverse != 0u ? 1u : 0u) << 63)
         | (a.ref_position & 0x7fffffffffffffffUL);
}

bool anchor_less(gpu_anchor_wire a, gpu_anchor_wire b, uint ia, uint ib) {
    uint64_t ka = ref_bits(a);
    uint64_t kb = ref_bits(b);
    if (ka != kb) return ka < kb;
    return ia < ib;
}

gpu_anchor_wire max_anchor() {
    return gpu_anchor_wire(0x7fffffffffffffffUL, 0xffffffffffffffffUL, 1u, 0xffffffffu);
}

void main() {
    uint range_id = gl_WorkGroupID.x;
    uint tid = gl_LocalInvocationID.x;
    sort_range r = ranges[range_id];
    if (r.range_len > compile_time_workgroup_size_x) return;

    if (tid < r.range_len) {
        sh_anchor[tid] = anchors[r.begin + tid];
        sh_orig[tid] = tid;
    } else {
        sh_anchor[tid] = max_anchor();
        sh_orig[tid] = tid;
    }
    barrier();

    for (uint size = 2u; size <= compile_time_workgroup_size_x; size <<= 1u) {
        for (uint stride = size >> 1u; stride > 0u; stride >>= 1u) {
            uint other = tid ^ stride;
            if (other > tid) {
                bool up = (tid & size) == 0u;
                gpu_anchor_wire a = sh_anchor[tid];
                gpu_anchor_wire b = sh_anchor[other];
                uint ia = sh_orig[tid];
                uint ib = sh_orig[other];
                bool should_swap = up
                    ? anchor_less(b, a, ib, ia)
                    : anchor_less(a, b, ia, ib);
                if (should_swap) {
                    sh_anchor[tid] = b;
                    sh_anchor[other] = a;
                    sh_orig[tid] = ib;
                    sh_orig[other] = ia;
                }
            }
            barrier();
        }
    }

    if (tid < r.range_len) {
        anchors[r.begin + tid] = sh_anchor[tid];
    }
}
)GLSL";
/// @brief Count seed matches, then emit anchors into the ranges assigned by
/// an exclusive prefix sum. Counting rejects hashes with more than max_occ
/// target positions. Emission also writes strand and reference-position keys
/// for sorting. Workgroups divide the query minimizers between their lanes.
inline constexpr std::string_view k_seed_count_emit_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

struct gpu_minimizer_slot { uint64_t hash; uint pos_offset; uint pos_count; };
struct minimizer_fragment_query_record {
    uint minimizer_offset;
    uint minimizer_count;
    uint query_length;
    uint flags;
};
struct gpu_anchor_wire { uint64_t ref_position; uint64_t query_position; uint is_reverse; uint span_flags_segment; };

layout(std430, set = 0, binding = 0) readonly buffer minimizer_words_buffer {
    uint minimizer_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer slots_buffer { gpu_minimizer_slot slots[]; };
layout(std430, set = 0, binding = 2) readonly buffer positions_buffer { uint64_t positions[]; };
layout(std430, set = 0, binding = 3) readonly buffer query_records_buffer {
    minimizer_fragment_query_record query_records[];
};
layout(std430, set = 0, binding = 4) buffer counts_buffer { uint counts[]; };
layout(std430, set = 0, binding = 5) readonly buffer prefix_counts_buffer { uint prefix[]; };
layout(std430, set = 0, binding = 6) buffer anchors_buffer { gpu_anchor_wire anchors[]; };
layout(std430, set = 0, binding = 7) buffer anchor_keys_buffer { uint64_t anchor_keys[]; };

layout(push_constant) uniform push_constants {
    uint words32_per_minimizer;
    uint word_kind;
    uint hash_shift;
    uint pos_bits;
    uint kmer_size;
    uint slot_mask;
    uint max_occ;
    uint emit_mode;
    uint query_record_count;
    uint hash_major_control_count;
    uint target_pos_offset_lo;
    uint target_pos_offset_hi;
    uint target_ref_bits;
} pc;

uint64_t target_position_offset() {
    return uint64_t(pc.target_pos_offset_lo)
        | (uint64_t(pc.target_pos_offset_hi) << 32);
}

uint64_t anchor_sort_key(uint reverse, uint64_t ref_position) {
    if (pc.target_ref_bits >= 63u) {
        return (uint64_t(reverse) << 63) | ref_position;
    }
    return (uint64_t(reverse) << pc.target_ref_bits) | ref_position;
}

uint64_t minimizer_fragment_hash(uint index) {
    if (pc.word_kind == 0u) {
        uint packed = minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        return uint64_t(raw) >> pc.hash_shift;
    }
    uint base = index * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        return uint64_t(minimizer_words[base]) >> pc.hash_shift;
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(minimizer_words[base])
            | (uint64_t(minimizer_words[base + 1u]) << 32);
        return raw >> pc.hash_shift;
    }
    uint64_t high = uint64_t(minimizer_words[base + 2u])
        | (uint64_t(minimizer_words[base + 3u]) << 32);
    return high >> 8;
}
uint64_t minimizer_fragment_position(uint index) {
    if (pc.word_kind == 0u) {
        uint packed = minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        uint mask = pc.pos_bits >= 15u
            ? 0xffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    uint base = index * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        uint raw = minimizer_words[base];
        uint mask = pc.pos_bits >= 31u
            ? 0xffffffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(minimizer_words[base])
            | (uint64_t(minimizer_words[base + 1u]) << 32);
        uint64_t mask = pc.pos_bits >= 63u
            ? ~uint64_t(0)
            : ((uint64_t(1) << (pc.pos_bits + 1u)) - uint64_t(1));
        return raw & mask;
    }
    return uint64_t(minimizer_words[base])
        | (uint64_t(minimizer_words[base + 1u]) << 32);
}
uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15UL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9UL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebUL;
    return x ^ (x >> 31);
}
uint find_hash_slot(uint64_t hash) {
    if (pc.hash_major_control_count != 0u) {
        // Hash-major dense controls use binary search. The nonzero control
        // count selects this representation explicitly.
        uint lo = 0u;
        uint hi = pc.hash_major_control_count;
        while (lo < hi) {
            uint mid = lo + ((hi - lo) >> 1u);
            if (slots[mid].hash < hash) lo = mid + 1u; else hi = mid;
        }
        if (lo < pc.hash_major_control_count && slots[lo].hash == hash) {
            return lo;
        }
        return 0xffffffffu;
    }
    // Open-addressed representation → linear probe.
    uint slot_index = uint(splitmix64(hash)) & pc.slot_mask;
    for (uint step = 0u; step <= pc.slot_mask; ++step) {
        gpu_minimizer_slot candidate = slots[slot_index];
        if (candidate.pos_count == 0u) return 0xffffffffu;
        if (candidate.hash == hash) return slot_index;
        slot_index = (slot_index + 1u) & pc.slot_mask;
    }
    return 0xffffffffu;
}

void main() {
    // The fused minimizer-fragment path binds one query record per problem.
    // Each record selects a slice of the concatenated minimizer-word buffer,
    // while counts and prefix offsets remain local to that problem.
    if (pc.query_record_count == 1u) {
        minimizer_fragment_query_record record = query_records[0];
        uint64_t reverse_base = uint64_t(
            record.query_length - 2u + pc.kmer_size);
        uint stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
        for (uint local_index = gl_GlobalInvocationID.x;
             local_index < record.minimizer_count;
             local_index += stride) {
            uint minimizer_index = record.minimizer_offset + local_index;
            uint64_t hash = minimizer_fragment_hash(minimizer_index);
            uint slot_index = find_hash_slot(hash);
            uint accepted_count = 0u;
            if (slot_index != 0xffffffffu) {
                gpu_minimizer_slot slot = slots[slot_index];
                if (slot.pos_count <= pc.max_occ) {
                    accepted_count = slot.pos_count;
                }
            }
            if (pc.emit_mode == 0u) {
                counts[local_index] = accepted_count;
                continue;
            }
            if (accepted_count == 0u) continue;
            gpu_minimizer_slot slot = slots[slot_index];
            uint destination = prefix[local_index];
            uint64_t query_encoded =
                minimizer_fragment_position(minimizer_index);
            uint64_t query_position_bp = query_encoded >> 1;
            uint query_strand = uint(query_encoded & uint64_t(1));
            uint span_flags_segment = pc.kmer_size & 0xffu;
            for (uint occurrence_index = 0u;
                 occurrence_index < accepted_count;
                 ++occurrence_index) {
                uint64_t target_encoded =
                    positions[slot.pos_offset + occurrence_index];
                uint target_strand = uint(target_encoded & uint64_t(1));
                uint reverse = target_strand ^ query_strand;
                uint64_t query_position = reverse != 0u
                    ? reverse_base - query_position_bp : query_position_bp;
                uint64_t ref_position = (target_encoded >> 1)
                    + target_position_offset();
                uint output_index = destination + occurrence_index;
                anchors[output_index].ref_position = ref_position;
                anchors[output_index].query_position = query_position;
                anchors[output_index].is_reverse = reverse;
                anchors[output_index].span_flags_segment = span_flags_segment;
                anchor_keys[output_index] =
                    anchor_sort_key(reverse, ref_position);
            }
        }
        return;
    }

    // Multi-record execution assigns one workgroup to each query record.
    uint record_index = gl_WorkGroupID.x;
    if (record_index >= pc.query_record_count) return;
    minimizer_fragment_query_record record = query_records[record_index];
    uint64_t reverse_base = uint64_t(
        record.query_length - 2u + pc.kmer_size);
    for (uint local_index = gl_LocalInvocationID.x;
         local_index < record.minimizer_count;
         local_index += gl_WorkGroupSize.x) {
        uint minimizer_index = record.minimizer_offset + local_index;
        uint64_t hash = minimizer_fragment_hash(minimizer_index);
        uint slot_index = find_hash_slot(hash);
        uint accepted_count = 0u;
        if (slot_index != 0xffffffffu) {
            gpu_minimizer_slot slot = slots[slot_index];
            if (slot.pos_count <= pc.max_occ) {
                accepted_count = slot.pos_count;
            }
        }
        if (pc.emit_mode == 0u) {
            counts[minimizer_index] = accepted_count;
            continue;
        }
        if (accepted_count == 0u) continue;
        gpu_minimizer_slot slot = slots[slot_index];
        uint destination = prefix[minimizer_index];
        uint64_t query_encoded =
            minimizer_fragment_position(minimizer_index);
        uint64_t query_position_bp = query_encoded >> 1;
        uint query_strand = uint(query_encoded & uint64_t(1));
        uint span_flags_segment = pc.kmer_size & 0xffu;
        for (uint occurrence_index = 0u;
             occurrence_index < accepted_count;
             ++occurrence_index) {
            uint64_t target_encoded =
                positions[slot.pos_offset + occurrence_index];
            uint target_strand = uint(target_encoded & uint64_t(1));
            uint reverse = target_strand ^ query_strand;
            uint64_t query_position = reverse != 0u
                ? reverse_base - query_position_bp : query_position_bp;
            uint64_t ref_position = (target_encoded >> 1)
                + target_position_offset();
            uint output_index = destination + occurrence_index;
            anchors[output_index].ref_position = ref_position;
            anchors[output_index].query_position = query_position;
            anchors[output_index].is_reverse = reverse;
            anchors[output_index].span_flags_segment = span_flags_segment;
            anchor_keys[output_index] = anchor_sort_key(reverse, ref_position);
        }
    }
}
)GLSL";

/// @brief Find seed matches directly in a grouped index's packed arrays.
/// A workgroup handles one query record. Its lanes look up minimizer hashes
/// and add occurrence counts across target segments before applying max_occ.
/// The emission pass reads segment positions again and writes anchors into
/// the prefix-sum ranges. Counts and positions keep their stored widths.
inline constexpr std::string_view k_grouped_soa_seed_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const uint k_grouped_soa_seed_push_abi_revision = 6u;
const uint k_grouped_soa_partition_required_flags = 7u;
const uint k_grouped_soa_status_validation = 1u;
const uint k_grouped_soa_status_query = 2u;
const uint k_grouped_soa_status_control = 4u;
const uint k_grouped_soa_status_position = 8u;
const uint k_grouped_soa_status_emission = 16u;

struct grouped_soa_partition_descriptor {
    uint partition_id;
    uint directory_ordinal;
    uint position_segment;
    uint flags;
    uint64_t hash_byte_offset;
    uint64_t prefix_byte_offset;
    uint64_t count_byte_offset;
    uint64_t position_byte_offset;
    uint control_count;
    uint position_count;
    uint hash_width;
    uint offset_width;
    uint count_width;
    uint position_width;
    uint64_t low_hash;
    uint64_t high_hash;
    uint64_t segment_base_bp;
    uint64_t segment_end_bp;
};
struct minimizer_fragment_query_record {
    uint minimizer_offset;
    uint minimizer_count;
    uint query_length;
    uint flags;
};
struct gpu_anchor_wire {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

layout(std430, set = 0, binding = 0) readonly buffer minimizer_words_buffer {
    uint minimizer_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer partitions_buffer {
    grouped_soa_partition_descriptor partitions[];
};
layout(std430, set = 0, binding = 2) readonly buffer raw_controls_buffer {
    uint control_words[];
};
layout(std430, set = 0, binding = 3) readonly buffer raw_positions_buffer {
    uint position_words[];
};
layout(std430, set = 0, binding = 4) readonly buffer query_records_buffer {
    minimizer_fragment_query_record query_records[];
};
layout(std430, set = 0, binding = 5) buffer counts_buffer {
    uint counts[];
};
layout(std430, set = 0, binding = 6) readonly buffer prefix_counts_buffer {
    uint prefix[];
};
layout(std430, set = 0, binding = 7) buffer anchors_buffer {
    gpu_anchor_wire anchors[];
};
layout(std430, set = 0, binding = 8) buffer anchor_keys_buffer {
    uint64_t anchor_keys[];
};
layout(std430, set = 0, binding = 9) buffer repetitive_flags_buffer {
    uint repetitive_flags[];
};

layout(push_constant) uniform push_constants {
    uint abi_revision;
    uint words32_per_minimizer;
    uint word_kind;
    uint hash_shift;
    uint pos_bits;
    uint kmer_size;
    uint partition_count;
    uint max_occ;
    uint emit_mode;
    uint query_record_count;
    uint target_ref_bits;
    uint query_minimizer_count;
    uint control_word_count;
    uint position_word_count;
    uint anchor_capacity;
    uint max_occ_cap_lo;
    uint max_occ_cap_hi;
    uint status_index;
    uint minimizer_word_extent;
    uint partition_extent;
    uint control_word_extent;
    uint position_word_extent;
    uint query_record_extent;
    uint count_extent;
    uint prefix_extent;
    uint anchor_extent;
    uint anchor_key_extent;
    uint partition_begin;
    uint control_base_word;
    uint position_base_word;
    uint repetitive_word_extent;
} pc;

void grouped_soa_signal_status(uint status_bit) {
    // Both values come from the exact host-bound counts descriptor range.
    if (pc.status_index < pc.count_extent) {
        atomicOr(counts[pc.status_index], status_bit);
    }
}

bool grouped_soa_scalar_width_valid(uint width) {
    return width == 2u || width == 4u || width == 8u;
}

bool grouped_soa_scalar_aligned(uint64_t byte_offset, uint width) {
    return grouped_soa_scalar_width_valid(width)
        && (byte_offset & uint64_t(width - 1u)) == uint64_t(0);
}

bool grouped_soa_scalar_range_valid(
    uint64_t byte_offset,
    uint width,
    uint word_count)
{
    if (!grouped_soa_scalar_aligned(byte_offset, width)) return false;
    uint64_t byte_limit = uint64_t(word_count) * uint64_t(4u);
    return byte_offset <= byte_limit
        && uint64_t(width) <= byte_limit - byte_offset;
}

bool grouped_soa_scalar_address(
    uint64_t base,
    uint index,
    uint width,
    out uint64_t byte_offset)
{
    uint64_t relative = uint64_t(index) * uint64_t(width);
    byte_offset = base + relative;
    return byte_offset >= base;
}

bool grouped_soa_read_control_scalar(
    uint64_t byte_offset,
    uint width,
    out uint64_t value)
{
    value = uint64_t(0);
    if (!grouped_soa_scalar_range_valid(
            byte_offset, width, pc.control_word_count)) {
        return false;
    }
    uint64_t binding_base = uint64_t(pc.control_base_word) * uint64_t(4u);
    if (byte_offset < binding_base || !grouped_soa_scalar_range_valid(
            byte_offset - binding_base, width, pc.control_word_extent)) return false;
    byte_offset -= binding_base;
    uint word_index = uint(byte_offset >> 2u);
    uint shift = uint(byte_offset & uint64_t(3u)) * 8u;
    if (width == 2u) {
        value = uint64_t((control_words[word_index] >> shift) & 0xffffu);
        return true;
    }
    if (width == 4u) {
        value = uint64_t(control_words[word_index]);
        return true;
    }
    value = uint64_t(control_words[word_index])
        | (uint64_t(control_words[word_index + 1u]) << 32u);
    return true;
}

bool grouped_soa_read_position_scalar(
    uint64_t byte_offset,
    uint width,
    out uint64_t value)
{
    value = uint64_t(0);
    if (!grouped_soa_scalar_range_valid(
            byte_offset, width, pc.position_word_count)) {
        return false;
    }
    uint64_t binding_base = uint64_t(pc.position_base_word) * uint64_t(4u);
    if (byte_offset < binding_base || !grouped_soa_scalar_range_valid(
            byte_offset - binding_base, width, pc.position_word_extent)) return false;
    byte_offset -= binding_base;
    uint word_index = uint(byte_offset >> 2u);
    uint shift = uint(byte_offset & uint64_t(3u)) * 8u;
    if (width == 2u) {
        value = uint64_t((position_words[word_index] >> shift) & 0xffffu);
        return true;
    }
    if (width == 4u) {
        value = uint64_t(position_words[word_index]);
        return true;
    }
    value = uint64_t(position_words[word_index])
        | (uint64_t(position_words[word_index + 1u]) << 32u);
    return true;
}

uint64_t grouped_soa_max_occ_cap() {
    return uint64_t(pc.max_occ_cap_lo)
        | (uint64_t(pc.max_occ_cap_hi) << 32u);
}

bool grouped_soa_partition_descriptor_valid(
    grouped_soa_partition_descriptor descriptor)
{
    if (descriptor.flags != k_grouped_soa_partition_required_flags
        || descriptor.control_count == 0u
        || descriptor.position_count == 0u
        || descriptor.low_hash > descriptor.high_hash
        || descriptor.segment_base_bp >= descriptor.segment_end_bp
        || !grouped_soa_scalar_width_valid(descriptor.hash_width)
        || !grouped_soa_scalar_width_valid(descriptor.offset_width)
        || !grouped_soa_scalar_width_valid(descriptor.count_width)
        || !grouped_soa_scalar_width_valid(descriptor.position_width)
        || !grouped_soa_scalar_aligned(
            descriptor.hash_byte_offset, descriptor.hash_width)
        || !grouped_soa_scalar_aligned(
            descriptor.prefix_byte_offset, descriptor.offset_width)
        || !grouped_soa_scalar_aligned(
            descriptor.count_byte_offset, descriptor.count_width)
        || !grouped_soa_scalar_aligned(
            descriptor.position_byte_offset, descriptor.position_width)) {
        return false;
    }
    uint position_bits = descriptor.position_width * 8u;
    uint64_t maximum_segment_span = uint64_t(1)
        << (position_bits - 1u);
    return descriptor.segment_end_bp - descriptor.segment_base_bp
        <= maximum_segment_span;
}

uint64_t minimizer_fragment_hash(uint index) {
    if (pc.word_kind == 0u) {
        uint packed = minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        return uint64_t(raw) >> pc.hash_shift;
    }
    uint base = index * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        return uint64_t(minimizer_words[base]) >> pc.hash_shift;
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(minimizer_words[base])
            | (uint64_t(minimizer_words[base + 1u]) << 32u);
        return raw >> pc.hash_shift;
    }
    uint64_t high = uint64_t(minimizer_words[base + 2u])
        | (uint64_t(minimizer_words[base + 3u]) << 32u);
    return high >> 8u;
}

uint64_t minimizer_fragment_position(uint index) {
    if (pc.word_kind == 0u) {
        uint packed = minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        uint mask = pc.pos_bits >= 15u
            ? 0xffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    uint base = index * pc.words32_per_minimizer;
    if (pc.word_kind == 1u) {
        uint mask = pc.pos_bits >= 31u
            ? 0xffffffffu : ((1u << (pc.pos_bits + 1u)) - 1u);
        return uint64_t(minimizer_words[base] & mask);
    }
    if (pc.word_kind == 2u) {
        uint64_t raw = uint64_t(minimizer_words[base])
            | (uint64_t(minimizer_words[base + 1u]) << 32u);
        uint64_t mask = pc.pos_bits >= 63u
            ? ~uint64_t(0)
            : ((uint64_t(1) << (pc.pos_bits + 1u)) - uint64_t(1));
        return raw & mask;
    }
    return uint64_t(minimizer_words[base])
        | (uint64_t(minimizer_words[base + 1u]) << 32u);
}

uint64_t grouped_soa_expected_minimizer_word_count() {
    if (pc.word_kind > 3u || pc.words32_per_minimizer == 0u) {
        return uint64_t(0);
    }
    if (pc.word_kind == 0u) {
        return (uint64_t(pc.query_minimizer_count) + uint64_t(1u))
            / uint64_t(2u);
    }
    return uint64_t(pc.query_minimizer_count)
        * uint64_t(pc.words32_per_minimizer);
}

bool minimizer_fragment_words_valid(uint index) {
    if (pc.word_kind > 3u || pc.words32_per_minimizer == 0u) return false;
    if (index >= pc.query_minimizer_count) return false;
    if (pc.word_kind == 0u) {
        return uint64_t(index >> 1u)
            < grouped_soa_expected_minimizer_word_count();
    }
    uint required_words = pc.word_kind == 1u
        ? 1u : pc.word_kind == 2u ? 2u : 4u;
    if (pc.words32_per_minimizer < required_words) return false;
    uint64_t end = (uint64_t(index) + uint64_t(1u))
        * uint64_t(pc.words32_per_minimizer);
    return end <= grouped_soa_expected_minimizer_word_count();
}

// Returns 0 for no hash in this partition, 1 for an admitted range, and 2 for
// malformed bytes.  The terminal prefix is authoritative; the saturated count
// is checked against it and the persisted generation cap before use.
uint grouped_soa_find_control(
    grouped_soa_partition_descriptor descriptor,
    uint64_t wanted_hash,
    out uint64_t position_begin,
    out uint64_t position_end,
    out uint64_t saturated_count)
{
    position_begin = uint64_t(0);
    position_end = uint64_t(0);
    saturated_count = uint64_t(0);
    if (!grouped_soa_partition_descriptor_valid(descriptor)) return 2u;
    if (wanted_hash < descriptor.low_hash
        || wanted_hash > descriptor.high_hash) {
        return 0u;
    }
    uint lo = 0u;
    uint hi = descriptor.control_count;
    while (lo < hi) {
        uint mid = lo + ((hi - lo) >> 1u);
        uint64_t address;
        uint64_t stored_hash;
        if (!grouped_soa_scalar_address(
                descriptor.hash_byte_offset,
                mid,
                descriptor.hash_width,
                address)
            || !grouped_soa_read_control_scalar(
                address, descriptor.hash_width, stored_hash)) {
            return 2u;
        }
        if (stored_hash < wanted_hash) lo = mid + 1u; else hi = mid;
    }
    if (lo >= descriptor.control_count) return 0u;
    uint64_t hash_address;
    uint64_t stored_hash;
    if (!grouped_soa_scalar_address(
            descriptor.hash_byte_offset,
            lo,
            descriptor.hash_width,
            hash_address)
        || !grouped_soa_read_control_scalar(
            hash_address, descriptor.hash_width, stored_hash)) {
        return 2u;
    }
    if (stored_hash != wanted_hash) return 0u;

    uint64_t begin_address;
    uint64_t end_address;
    uint64_t count_address;
    if (!grouped_soa_scalar_address(
            descriptor.prefix_byte_offset,
            lo,
            descriptor.offset_width,
            begin_address)
        || !grouped_soa_scalar_address(
            descriptor.prefix_byte_offset,
            lo + 1u,
            descriptor.offset_width,
            end_address)
        || !grouped_soa_scalar_address(
            descriptor.count_byte_offset,
            lo,
            descriptor.count_width,
            count_address)
        || !grouped_soa_read_control_scalar(
            begin_address, descriptor.offset_width, position_begin)
        || !grouped_soa_read_control_scalar(
            end_address, descriptor.offset_width, position_end)
        || !grouped_soa_read_control_scalar(
            count_address, descriptor.count_width, saturated_count)
        || position_begin >= position_end
        || position_end > uint64_t(descriptor.position_count)) {
        return 2u;
    }
    uint64_t exact_count = position_end - position_begin;
    uint64_t cap = grouped_soa_max_occ_cap();
    if (cap == ~uint64_t(0)) return 2u;
    uint64_t expected_saturated = exact_count > cap
        ? cap + uint64_t(1u) : exact_count;
    return saturated_count == expected_saturated ? 1u : 2u;
}

bool grouped_soa_count_hash(
    uint64_t hash,
    out uint accepted_count,
    out bool repetitive)
{
    accepted_count = 0u;
    repetitive = false;
    uint64_t total = uint64_t(0);
    uint64_t threshold = uint64_t(pc.max_occ);
    for (uint partition_index = pc.partition_begin;
         partition_index < pc.partition_count;
         ++partition_index) {
        uint64_t position_begin;
        uint64_t position_end;
        uint64_t saturated_count;
        uint status = grouped_soa_find_control(
            partitions[partition_index],
            hash,
            position_begin,
            position_end,
            saturated_count);
        if (status == 2u) return false;
        if (status == 0u) continue;
        if (saturated_count > threshold) {
            accepted_count = 0u;
            repetitive = true;
            return true;
        }
        uint64_t exact_count = position_end - position_begin;
        if (total > threshold || exact_count > threshold - total) {
            accepted_count = 0u;
            repetitive = true;
            return true;
        }
        total += exact_count;
    }
    accepted_count = uint(total);
    return true;
}

bool grouped_soa_dispatch_valid() {
    uint64_t cap = grouped_soa_max_occ_cap();
    uint64_t expected_minimizer_words =
        grouped_soa_expected_minimizer_word_count();
    return pc.abi_revision == k_grouped_soa_seed_push_abi_revision
        && pc.emit_mode <= 1u
        && pc.kmer_size != 0u
        && pc.kmer_size <= 255u
        && pc.hash_shift < 64u
        && pc.pos_bits < 64u
        && pc.target_ref_bits <= 63u
        && cap != ~uint64_t(0)
        && uint64_t(pc.max_occ) <= cap
        && expected_minimizer_words != uint64_t(0)
        && expected_minimizer_words <= uint64_t(pc.minimizer_word_extent)
        && pc.partition_begin < pc.partition_count
        && pc.partition_count <= pc.partition_extent
        && pc.query_record_count == 1u
        && pc.query_record_count <= pc.query_record_extent
        && pc.query_minimizer_count != 0u
        && pc.status_index != 0u
        && pc.status_index < pc.count_extent
        && pc.status_index == pc.prefix_extent
        && pc.status_index <= pc.query_minimizer_count
        && pc.control_word_count != 0u
        && pc.control_word_extent != 0u
        && uint64_t(pc.control_base_word) + pc.control_word_extent <= uint64_t(pc.control_word_count)
        && pc.position_word_count != 0u
        && pc.position_word_extent != 0u
        && uint64_t(pc.position_base_word) + pc.position_word_extent <= uint64_t(pc.position_word_count)
        && uint64_t(pc.status_index) * uint64_t(2u) <= uint64_t(pc.repetitive_word_extent)
        && pc.anchor_capacity <= pc.anchor_extent
        && pc.anchor_capacity <= pc.anchor_key_extent
        && ((pc.emit_mode == 0u && pc.anchor_capacity == 0u)
            || (pc.emit_mode == 1u && pc.anchor_capacity != 0u));
}

bool grouped_soa_query_record_valid(
    minimizer_fragment_query_record record)
{
    uint64_t query_end = uint64_t(record.minimizer_offset)
        + uint64_t(record.minimizer_count);
    return query_end <= uint64_t(pc.query_minimizer_count)
        && record.minimizer_count == pc.status_index
        && record.query_length >= pc.kmer_size
        && record.flags == 0u;
}
)GLSL"
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                            R"GLSL(

// Strict E2E failure sentinel. Production shader text excludes this block.
// A dedicated diagnostic counts buffer is armed only after problem 0/query 0
// has already produced zero anchors. The first discriminator is the ordinary,
// host-owned in-bounds status word; no trace slot is touched until it matches.
const uint k_benchmark_e2e_trace_request_magic = 0x42535452u;
const uint k_benchmark_e2e_trace_guard_magic = 0x47534f41u;
const uint k_benchmark_e2e_trace_completion_magic = 0x4253444eu;
const uint k_benchmark_e2e_trace_request_magic_word = 0u;
const uint k_benchmark_e2e_trace_guard_magic_word = 1u;
const uint k_benchmark_e2e_trace_completion_magic_word = 2u;
const uint k_benchmark_e2e_trace_requested_partition_ordinal_word = 3u;
const uint k_benchmark_e2e_trace_push_begin = 4u;
const uint k_benchmark_e2e_trace_query_record_begin = 31u;
const uint k_benchmark_e2e_trace_query_words_begin = 35u;
const uint k_benchmark_e2e_trace_query_hash_begin = 39u;
const uint k_benchmark_e2e_trace_partition_descriptor_begin = 42u;
const uint k_benchmark_e2e_trace_lookup_begin = 68u;
const uint k_benchmark_e2e_trace_decision_begin = 90u;
const uint k_benchmark_e2e_trace_word_count = 97u;

bool benchmark_e2e_grouped_soa_trace_status_armed() {
    return pc.status_index < pc.count_extent
        && counts[pc.status_index] == k_benchmark_e2e_trace_request_magic;
}

bool benchmark_e2e_grouped_soa_trace_guarded(uint trace_base) {
    if (trace_base > pc.count_extent
        || k_benchmark_e2e_trace_word_count > pc.count_extent - trace_base) {
        return false;
    }
    return counts[trace_base + k_benchmark_e2e_trace_request_magic_word]
            == k_benchmark_e2e_trace_request_magic
        && counts[trace_base + k_benchmark_e2e_trace_guard_magic_word]
            == k_benchmark_e2e_trace_guard_magic;
}

void benchmark_e2e_grouped_soa_trace_store(
    uint trace_base,
    uint trace_word,
    uint value)
{
    counts[trace_base + trace_word] = value;
}

void benchmark_e2e_grouped_soa_trace_store_u64(
    uint trace_base,
    uint trace_word,
    uint64_t value)
{
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, trace_word, uint(value));
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, trace_word + 1u, uint(value >> 32u));
}

void benchmark_e2e_grouped_soa_trace_capture(
    uint trace_base,
    minimizer_fragment_query_record record)
{
    bool dispatch_valid = grouped_soa_dispatch_valid();
    bool record_valid = grouped_soa_query_record_valid(record);
    uint push_base = k_benchmark_e2e_trace_push_begin;
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 0u, pc.abi_revision);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 1u, pc.words32_per_minimizer);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 2u, pc.word_kind);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 3u, pc.hash_shift);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 4u, pc.pos_bits);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 5u, pc.kmer_size);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 6u, pc.partition_count);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 7u, pc.max_occ);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 8u, pc.emit_mode);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 9u, pc.query_record_count);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 10u, pc.target_ref_bits);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 11u, pc.query_minimizer_count);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 12u, pc.control_word_count);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 13u, pc.position_word_count);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 14u, pc.anchor_capacity);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 15u, pc.max_occ_cap_lo);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 16u, pc.max_occ_cap_hi);
    benchmark_e2e_grouped_soa_trace_store(trace_base, push_base + 17u, pc.status_index);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 18u, pc.minimizer_word_extent);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 19u, pc.partition_extent);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 20u, pc.control_word_extent);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 21u, pc.position_word_extent);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 22u, pc.query_record_extent);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 23u, pc.count_extent);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 24u, pc.prefix_extent);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 25u, pc.anchor_extent);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, push_base + 26u, pc.anchor_key_extent);

    uint query_record_base = k_benchmark_e2e_trace_query_record_begin;
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, query_record_base + 0u, record.minimizer_offset);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, query_record_base + 1u, record.minimizer_count);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, query_record_base + 2u, record.query_length);
    benchmark_e2e_grouped_soa_trace_store(trace_base, query_record_base + 3u, record.flags);

    uint query_words_base = k_benchmark_e2e_trace_query_words_begin;
    for (uint word = 0u; word < 4u; ++word) {
        benchmark_e2e_grouped_soa_trace_store(trace_base, query_words_base + word, 0u);
    }
    uint minimizer_index = record.minimizer_offset;
    bool query_hash_available = record_valid
        && pc.hash_shift < 64u
        && minimizer_fragment_words_valid(minimizer_index);
    if (query_hash_available && pc.word_kind == 0u) {
        uint packed = minimizer_words[minimizer_index >> 1u];
        uint raw = (packed >> ((minimizer_index & 1u) * 16u)) & 0xffffu;
        benchmark_e2e_grouped_soa_trace_store(trace_base, query_words_base, raw);
    } else if (query_hash_available) {
        uint64_t first_word_u64 = uint64_t(minimizer_index)
            * uint64_t(pc.words32_per_minimizer);
        uint first_word = uint(first_word_u64);
        uint required_words = pc.word_kind == 1u
            ? 1u : pc.word_kind == 2u ? 2u : 4u;
        for (uint word = 0u; word < required_words; ++word) {
            benchmark_e2e_grouped_soa_trace_store(
                trace_base, query_words_base + word, minimizer_words[first_word + word]);
        }
    }
    uint64_t wanted_hash = uint64_t(0);
    if (query_hash_available) {
        wanted_hash = minimizer_fragment_hash(minimizer_index);
    }
    uint query_hash_base = k_benchmark_e2e_trace_query_hash_begin;
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, query_hash_base, query_hash_available ? 1u : 0u);
    benchmark_e2e_grouped_soa_trace_store_u64(
        trace_base, query_hash_base + 1u, wanted_hash);

    uint descriptor_base = k_benchmark_e2e_trace_partition_descriptor_begin;
    uint lookup_base = k_benchmark_e2e_trace_lookup_begin;
    uint decision_base = k_benchmark_e2e_trace_decision_begin;
    uint requested_partition =
        counts[trace_base + k_benchmark_e2e_trace_requested_partition_ordinal_word];
    uint descriptor_valid = 0u;
    uint hash_in_range = 0u;
    uint initial_low = 0u;
    uint initial_high = 0u;
    uint last_middle = 0xffffffffu;
    uint final_low = 0u;
    uint final_high = 0u;
    uint key_available = 0u;
    uint64_t key = uint64_t(0);
    uint previous_available = 0u;
    uint64_t previous_key = uint64_t(0);
    uint lookup_result = 2u;
    uint64_t position_begin = uint64_t(0);
    uint64_t position_end = uint64_t(0);
    uint64_t saturated_count = uint64_t(0);
    uint64_t exact_count = uint64_t(0);

    if (dispatch_valid && requested_partition < pc.partition_count) {
        grouped_soa_partition_descriptor descriptor = partitions[requested_partition];
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 0u, descriptor.partition_id);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 1u, descriptor.directory_ordinal);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 2u, descriptor.position_segment);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 3u, descriptor.flags);
        benchmark_e2e_grouped_soa_trace_store_u64(
            trace_base, descriptor_base + 4u, descriptor.hash_byte_offset);
        benchmark_e2e_grouped_soa_trace_store_u64(
            trace_base, descriptor_base + 6u, descriptor.prefix_byte_offset);
        benchmark_e2e_grouped_soa_trace_store_u64(
            trace_base, descriptor_base + 8u, descriptor.count_byte_offset);
        benchmark_e2e_grouped_soa_trace_store_u64(
            trace_base, descriptor_base + 10u, descriptor.position_byte_offset);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 12u, descriptor.control_count);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 13u, descriptor.position_count);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 14u, descriptor.hash_width);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 15u, descriptor.offset_width);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 16u, descriptor.count_width);
        benchmark_e2e_grouped_soa_trace_store(
            trace_base, descriptor_base + 17u, descriptor.position_width);
        benchmark_e2e_grouped_soa_trace_store_u64(
            trace_base, descriptor_base + 18u, descriptor.low_hash);
        benchmark_e2e_grouped_soa_trace_store_u64(
            trace_base, descriptor_base + 20u, descriptor.high_hash);
        benchmark_e2e_grouped_soa_trace_store_u64(
            trace_base, descriptor_base + 22u, descriptor.segment_base_bp);
        benchmark_e2e_grouped_soa_trace_store_u64(
            trace_base, descriptor_base + 24u, descriptor.segment_end_bp);

        bool descriptor_is_valid = grouped_soa_partition_descriptor_valid(descriptor);
        descriptor_valid = descriptor_is_valid ? 1u : 0u;
        hash_in_range = query_hash_available
                && wanted_hash >= descriptor.low_hash
                && wanted_hash <= descriptor.high_hash
            ? 1u : 0u;
        initial_high = descriptor.control_count;
        final_high = initial_high;
        if (descriptor_is_valid && query_hash_available && hash_in_range != 0u) {
            uint lo = 0u;
            uint hi = descriptor.control_count;
            bool search_valid = true;
            while (lo < hi) {
                uint middle = lo + ((hi - lo) >> 1u);
                last_middle = middle;
                uint64_t hash_address;
                uint64_t middle_hash;
                if (!grouped_soa_scalar_address(
                        descriptor.hash_byte_offset,
                        middle,
                        descriptor.hash_width,
                        hash_address)
                    || !grouped_soa_read_control_scalar(
                        hash_address, descriptor.hash_width, middle_hash)) {
                    search_valid = false;
                    break;
                }
                if (middle_hash < wanted_hash) lo = middle + 1u; else hi = middle;
            }
            final_low = lo;
            final_high = hi;
            if (search_valid && lo < descriptor.control_count) {
                uint64_t hash_address;
                search_valid = grouped_soa_scalar_address(
                                   descriptor.hash_byte_offset,
                                   lo,
                                   descriptor.hash_width,
                                   hash_address)
                    && grouped_soa_read_control_scalar(
                        hash_address, descriptor.hash_width, key);
                key_available = search_valid ? 1u : 0u;
                if (search_valid && lo != 0u) {
                    uint64_t previous_address;
                    previous_available =
                            grouped_soa_scalar_address(
                                descriptor.hash_byte_offset,
                                lo - 1u,
                                descriptor.hash_width,
                                previous_address)
                            && grouped_soa_read_control_scalar(
                                previous_address,
                                descriptor.hash_width,
                                previous_key)
                        ? 1u : 0u;
                }
            }
            if (!search_valid) {
                lookup_result = 2u;
            } else if (lo >= descriptor.control_count || key != wanted_hash) {
                lookup_result = 0u;
            } else {
                uint64_t begin_address;
                uint64_t end_address;
                uint64_t count_address;
                bool tuple_valid = grouped_soa_scalar_address(
                                       descriptor.prefix_byte_offset,
                                       lo,
                                       descriptor.offset_width,
                                       begin_address)
                    && grouped_soa_scalar_address(
                        descriptor.prefix_byte_offset,
                        lo + 1u,
                        descriptor.offset_width,
                        end_address)
                    && grouped_soa_scalar_address(
                        descriptor.count_byte_offset,
                        lo,
                        descriptor.count_width,
                        count_address)
                    && grouped_soa_read_control_scalar(
                        begin_address, descriptor.offset_width, position_begin)
                    && grouped_soa_read_control_scalar(
                        end_address, descriptor.offset_width, position_end)
                    && grouped_soa_read_control_scalar(
                        count_address, descriptor.count_width, saturated_count)
                    && position_begin < position_end
                    && position_end <= uint64_t(descriptor.position_count);
                if (tuple_valid) {
                    exact_count = position_end - position_begin;
                    uint64_t cap = grouped_soa_max_occ_cap();
                    uint64_t expected_saturated = exact_count > cap
                        ? cap + uint64_t(1u) : exact_count;
                    tuple_valid = cap != ~uint64_t(0)
                        && saturated_count == expected_saturated;
                }
                lookup_result = tuple_valid ? 1u : 2u;
            }
        } else if (descriptor_is_valid && query_hash_available) {
            lookup_result = 0u;
        }
    }

    benchmark_e2e_grouped_soa_trace_store(
        trace_base, lookup_base + 0u, descriptor_valid);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, lookup_base + 1u, hash_in_range);
    benchmark_e2e_grouped_soa_trace_store(trace_base, lookup_base + 2u, initial_low);
    benchmark_e2e_grouped_soa_trace_store(trace_base, lookup_base + 3u, initial_high);
    benchmark_e2e_grouped_soa_trace_store(trace_base, lookup_base + 4u, last_middle);
    benchmark_e2e_grouped_soa_trace_store(trace_base, lookup_base + 5u, final_low);
    benchmark_e2e_grouped_soa_trace_store(trace_base, lookup_base + 6u, final_high);
    benchmark_e2e_grouped_soa_trace_store(trace_base, lookup_base + 7u, key_available);
    benchmark_e2e_grouped_soa_trace_store_u64(trace_base, lookup_base + 8u, key);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, lookup_base + 10u, previous_available);
    benchmark_e2e_grouped_soa_trace_store_u64(
        trace_base, lookup_base + 11u, previous_key);
    benchmark_e2e_grouped_soa_trace_store(trace_base, lookup_base + 13u, lookup_result);
    benchmark_e2e_grouped_soa_trace_store_u64(
        trace_base, lookup_base + 14u, position_begin);
    benchmark_e2e_grouped_soa_trace_store_u64(
        trace_base, lookup_base + 16u, position_end);
    benchmark_e2e_grouped_soa_trace_store_u64(
        trace_base, lookup_base + 18u, saturated_count);
    benchmark_e2e_grouped_soa_trace_store_u64(
        trace_base, lookup_base + 20u, exact_count);

    uint accepted_count = 0u;
    bool repetitive = false;
    bool aggregate_valid = dispatch_valid
        && record_valid
        && query_hash_available
        && grouped_soa_count_hash(wanted_hash, accepted_count, repetitive);
    uint64_t threshold = uint64_t(pc.max_occ);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base,
        decision_base + 0u,
        lookup_result == 1u && saturated_count > threshold ? 1u : 0u);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base,
        decision_base + 1u,
        lookup_result == 1u && exact_count > threshold ? 1u : 0u);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, decision_base + 2u, aggregate_valid ? 1u : 0u);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, decision_base + 3u, accepted_count);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base,
        decision_base + 4u,
        aggregate_valid && accepted_count == 0u ? 1u : 0u);
    uint diagnostic_status = 0u;
    if (!dispatch_valid) {
        diagnostic_status |= k_grouped_soa_status_validation;
    } else if (!record_valid || !query_hash_available) {
        diagnostic_status |= k_grouped_soa_status_query;
    } else if (!aggregate_valid) {
        diagnostic_status |= k_grouped_soa_status_control;
    }
    benchmark_e2e_grouped_soa_trace_store(
        trace_base,
        decision_base + 5u,
        diagnostic_status == 0u ? accepted_count : 0u);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base, decision_base + 6u, diagnostic_status);
    benchmark_e2e_grouped_soa_trace_store(
        trace_base,
        k_benchmark_e2e_trace_completion_magic_word,
        k_benchmark_e2e_trace_completion_magic);
}
)GLSL"
#endif
                                                            R"GLSL(
uint64_t grouped_soa_anchor_sort_key(uint reverse, uint64_t ref_position) {
    if (pc.target_ref_bits >= 63u) {
        return (uint64_t(reverse) << 63u) | ref_position;
    }
    return (uint64_t(reverse) << pc.target_ref_bits) | ref_position;
}

void main() {
    uint record_index = gl_WorkGroupID.x;
)GLSL"
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                            R"GLSL(
    // The ordinary status slot is always in bounds under the authenticated host
    // contract. Only its diagnostic magic authorizes the subsequent trace-tail
    // reads; all lanes then return without entering production count/emit work.
    if (record_index == 0u
        && benchmark_e2e_grouped_soa_trace_status_armed()) {
        if (gl_LocalInvocationID.x == 0u) {
            uint trace_base = pc.status_index + 1u;
            if (benchmark_e2e_grouped_soa_trace_guarded(trace_base)) {
                minimizer_fragment_query_record trace_record = query_records[0u];
                benchmark_e2e_grouped_soa_trace_capture(trace_base, trace_record);
            }
        }
        return;
    }
)GLSL"
#endif
                                                            R"GLSL(
    bool dispatch_valid = grouped_soa_dispatch_valid();
    if (!dispatch_valid || record_index != 0u) {
        grouped_soa_signal_status(k_grouped_soa_status_validation);
        return;
    }
    minimizer_fragment_query_record record = query_records[record_index];
    bool record_valid = grouped_soa_query_record_valid(record);
    if (!record_valid) {
        grouped_soa_signal_status(k_grouped_soa_status_query);
        return;
    }
    for (uint local_index = gl_LocalInvocationID.x;
         local_index < record.minimizer_count;
         local_index += gl_WorkGroupSize.x) {
        uint minimizer_index = record.minimizer_offset + local_index;
        if (pc.emit_mode == 0u && pc.partition_begin == 0u) {
            counts[local_index] = 0u;
            repetitive_flags[local_index] = 0u;
            repetitive_flags[pc.status_index + local_index] = 0u;
        }
        if (minimizer_index >= pc.query_minimizer_count
            || !minimizer_fragment_words_valid(minimizer_index)) {
            grouped_soa_signal_status(k_grouped_soa_status_query);
            continue;
        }

        uint64_t hash = minimizer_fragment_hash(minimizer_index);
        uint accepted_count = 0u;
        bool repetitive = false;
        if (!grouped_soa_count_hash(hash, accepted_count, repetitive)) {
            grouped_soa_signal_status(k_grouped_soa_status_control);
            continue;
        }
        if (pc.emit_mode == 0u) {
            uint64_t total = uint64_t(counts[local_index]) + uint64_t(accepted_count);
            bool globally_repetitive = repetitive_flags[local_index] != 0u
                || repetitive || total > uint64_t(pc.max_occ);
            counts[local_index] = globally_repetitive ? 0u : uint(total);
            repetitive_flags[local_index] = globally_repetitive ? 1u : 0u;
            continue;
        }
        // Count covers every window before scan. Keep global counts immutable;
        // the second half of the flags buffer owns deterministic emit cursors.
        uint global_count = counts[local_index];
        uint cursor = repetitive_flags[pc.status_index + local_index];
        if (repetitive_flags[local_index] != 0u) {
            if (global_count != 0u || cursor != 0u)
                grouped_soa_signal_status(k_grouped_soa_status_emission);
            continue;
        }
        if (repetitive || cursor > global_count || accepted_count > global_count - cursor
            || (pc.partition_count == pc.partition_extent && cursor + accepted_count != global_count)) {
            grouped_soa_signal_status(k_grouped_soa_status_emission);
            continue;
        }
        if (accepted_count == 0u) continue;

        uint64_t destination = uint64_t(prefix[local_index]) + uint64_t(cursor);
        if (destination > uint64_t(pc.anchor_capacity)
            || uint64_t(accepted_count)
                > uint64_t(pc.anchor_capacity) - destination) {
            grouped_soa_signal_status(k_grouped_soa_status_emission);
            continue;
        }
        uint64_t query_encoded = minimizer_fragment_position(minimizer_index);
        uint64_t query_bp = query_encoded >> 1u;
        uint query_strand = uint(query_encoded & uint64_t(1u));
        if (query_bp >= uint64_t(record.query_length)
            || query_bp + uint64_t(1u) < uint64_t(pc.kmer_size)) {
            grouped_soa_signal_status(k_grouped_soa_status_query);
            continue;
        }
        uint64_t reverse_base = uint64_t(record.query_length)
            + uint64_t(pc.kmer_size) - uint64_t(2u);
        uint emitted = 0u;
        bool emission_valid = true;
        for (uint partition_index = pc.partition_begin;
             emission_valid && partition_index < pc.partition_count;
             ++partition_index) {
            grouped_soa_partition_descriptor descriptor =
                partitions[partition_index];
            uint64_t position_begin;
            uint64_t position_end;
            uint64_t saturated_count;
            uint status = grouped_soa_find_control(
                descriptor,
                hash,
                position_begin,
                position_end,
                saturated_count);
            if (status == 2u) {
                grouped_soa_signal_status(k_grouped_soa_status_control);
                emission_valid = false;
                break;
            }
            if (status == 0u) continue;
            for (uint64_t position_index = position_begin;
                 position_index < position_end;
                 ++position_index) {
                uint64_t position_address;
                uint64_t target_encoded;
                if (!grouped_soa_scalar_address(
                        descriptor.position_byte_offset,
                        uint(position_index),
                        descriptor.position_width,
                        position_address)
                    || !grouped_soa_read_position_scalar(
                        position_address,
                        descriptor.position_width,
                        target_encoded)) {
                    grouped_soa_signal_status(k_grouped_soa_status_position);
                    emission_valid = false;
                    break;
                }
                uint64_t local_bp = target_encoded >> 1u;
                uint64_t segment_span = descriptor.segment_end_bp
                    - descriptor.segment_base_bp;
                if (local_bp >= segment_span || emitted >= accepted_count) {
                    grouped_soa_signal_status(
                        local_bp >= segment_span
                            ? k_grouped_soa_status_position
                            : k_grouped_soa_status_emission);
                    emission_valid = false;
                    break;
                }
                uint target_strand = uint(target_encoded & uint64_t(1u));
                uint reverse = target_strand ^ query_strand;
                uint64_t ref_position = descriptor.segment_base_bp + local_bp;
                uint64_t query_position = reverse != 0u
                    ? reverse_base - query_bp : query_bp;
                uint64_t output_index = destination + uint64_t(emitted);
                anchors[uint(output_index)].ref_position = ref_position;
                anchors[uint(output_index)].query_position = query_position;
                anchors[uint(output_index)].is_reverse = reverse;
                anchors[uint(output_index)].span_flags_segment =
                    pc.kmer_size & 0xffu;
                anchor_keys[uint(output_index)] =
                    grouped_soa_anchor_sort_key(reverse, ref_position);
                ++emitted;
            }
        }
        // A validated immutable resident generation makes this equality
        // deterministic between count and emit.  No CPU/SIMD repair path is
        // provided if the resident bytes change or fail validation.
        if (!emission_valid || emitted != accepted_count) {
            grouped_soa_signal_status(k_grouped_soa_status_emission);
        } else {
            repetitive_flags[pc.status_index + local_index] = cursor + emitted;
        }
    }
}
)GLSL";

/// @brief Shared buffer declarations and bounds-checked reads for the
/// Format-5 grouped index shaders. Combine this text with the control or
/// position phase below to build a complete shader.
inline constexpr std::string_view k_grouped_soa_stream_common_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const uint k_shape_v5_stream_abi_revision = 3u;
const uint k_shape_v5_phase_controls = 1u;
const uint k_shape_v5_phase_positions = 2u;
const uint k_shape_v5_status_contract = 1u;
const uint k_shape_v5_status_query = 2u;
const uint k_shape_v5_status_control = 4u;
const uint k_shape_v5_status_position = 8u;
const uint k_shape_v5_status_authorization = 16u;
const uint k_shape_v5_status_emission = 32u;

struct shape_v5_partition_wire {
    uint partition_id;
    uint directory_ordinal;
    uint position_segment;
    uint control_count;
    uint position_count;
    uint hash_array_offset;
    uint prefix_offset_array_offset;
    uint count_array_offset;
    uint hash_width;
    uint offset_width;
    uint count_width;
    uint position_width;
    uint64_t segment_base;
    uint64_t segment_end;
    uint64_t max_hash;
    uint64_t max_occ_cap;
};
struct shape_v5_query_record {
    uint minimizer_offset;
    uint minimizer_count;
    uint query_length;
    uint flags;
};
struct shape_v5_query_minimizer_h64 {
    uint64_t hash;
    uint query_bp_strand;
    uint occurrence_index;
    uint query_record_index;
    uint reserved_zero;
};
struct shape_v5_query_authorization {
    uint occurrence_count;
    uint accepted_count;
    uint emission_cursor;
    uint reserved_zero;
};
struct shape_v5_authorization_range {
    uint directory_ordinal;
    uint position_segment;
    uint query_record_index;
    uint query_minimizer_index;
    uint position_begin;
    uint position_count;
    uint status;
    uint reserved_zero;
};
struct shape_v5_anchor {
    uint64_t ref_position;
    uint64_t query_position;
    uint is_reverse;
    uint span_flags_segment;
};

layout(std430, set = 0, binding = 0) readonly buffer shape_v5_minimizer_words_buffer {
    uint shape_v5_minimizer_words[];
};
layout(std430, set = 0, binding = 1) readonly buffer shape_v5_descriptors_buffer {
    shape_v5_partition_wire shape_v5_descriptors[];
};
layout(std430, set = 0, binding = 2) readonly buffer shape_v5_payload_buffer {
    uint shape_v5_payload_words[];
};
layout(std430, set = 0, binding = 3) readonly buffer shape_v5_query_records_buffer {
    shape_v5_query_record shape_v5_query_records[];
};
layout(std430, set = 0, binding = 4) buffer shape_v5_query_state_buffer {
    shape_v5_query_authorization shape_v5_query_states[];
};
layout(std430, set = 0, binding = 5) buffer shape_v5_authorization_ranges_buffer {
    shape_v5_authorization_range shape_v5_authorization_ranges[];
};
layout(std430, set = 0, binding = 6) readonly buffer shape_v5_prefix_buffer {
    uint shape_v5_prefix[];
};
layout(std430, set = 0, binding = 7) buffer shape_v5_anchors_buffer {
    shape_v5_anchor shape_v5_anchors[];
};
layout(std430, set = 0, binding = 8) buffer shape_v5_anchor_keys_buffer {
    uint64_t shape_v5_anchor_keys[];
};
layout(std430, set = 0, binding = 9) buffer shape_v5_counters_buffer {
    uint shape_v5_counters[];
};
layout(std430, set = 0, binding = 10) buffer shape_v5_accepted_counts_buffer {
    uint shape_v5_accepted_counts[];
};
layout(std430, set = 0, binding = 11) readonly buffer shape_v5_query_h64_buffer {
    shape_v5_query_minimizer_h64 shape_v5_query_h64[];
};

layout(push_constant) uniform shape_v5_group_push_constants {
    uint abi_revision;
    uint phase;
    uint group_ordinal;
    uint descriptor_count;
    uint payload_bytes;
    uint words32_per_minimizer;
    uint word_kind;
    uint hash_shift;
    uint pos_bits;
    uint kmer_size;
    uint query_minimizer_count;
    uint query_record_count;
    uint max_occ;
    uint target_ref_bits;
    uint minimizer_word_extent;
    uint descriptor_extent;
    uint payload_word_extent;
    uint query_record_extent;
    uint query_state_extent;
    uint authorization_range_extent;
    uint counter_extent;
    uint authorization_count_index;
    uint anchor_count_index;
    uint status_index;
    uint prefix_extent;
    uint anchor_extent;
    uint anchor_key_extent;
    uint accepted_count_extent;
    uint query_wire_kind;
    uint h64_query_extent;
    uint reserved_zero;
} shape_v5_pc;

void shape_v5_signal_status(uint bit) {
    if (shape_v5_pc.status_index < shape_v5_pc.counter_extent) {
        atomicOr(shape_v5_counters[shape_v5_pc.status_index], bit);
    }
}

bool shape_v5_scalar_width_valid(uint width) {
    return width == 2u || width == 4u || width == 8u;
}

uint64_t shape_v5_scalar_max(uint width) {
    return width == 8u
        ? ~uint64_t(0u)
        : (uint64_t(1u) << (width * 8u)) - uint64_t(1u);
}

bool shape_v5_scalar_range_valid(
    uint byte_offset, uint64_t count, uint width)
{
    if (!shape_v5_scalar_width_valid(width)
        || (byte_offset & (width - 1u)) != 0u) {
        return false;
    }
    uint64_t begin = uint64_t(byte_offset);
    uint64_t bytes = count * uint64_t(width);
    uint64_t payload_limit = uint64_t(shape_v5_pc.payload_bytes);
    uint64_t bound_limit = uint64_t(shape_v5_pc.payload_word_extent) * uint64_t(4u);
    return bytes / uint64_t(width) == count
        && begin <= payload_limit
        && bytes <= payload_limit - begin
        && begin <= bound_limit
        && bytes <= bound_limit - begin;
}

bool shape_v5_descriptor_valid(
    shape_v5_partition_wire descriptor, uint expected_phase)
{
    if (shape_v5_pc.phase != expected_phase
        || descriptor.partition_id != descriptor.directory_ordinal
        || descriptor.control_count == 0u
        || descriptor.position_count == 0u
        || descriptor.segment_base >= descriptor.segment_end
        || descriptor.max_occ_cap == ~uint64_t(0)
        || uint64_t(shape_v5_pc.max_occ) > descriptor.max_occ_cap
        || !shape_v5_scalar_width_valid(descriptor.hash_width)
        || !shape_v5_scalar_width_valid(descriptor.offset_width)
        || !shape_v5_scalar_width_valid(descriptor.count_width)
        || !shape_v5_scalar_width_valid(descriptor.position_width)) {
        return false;
    }
    if (descriptor.max_hash > shape_v5_scalar_max(descriptor.hash_width)
        || uint64_t(descriptor.position_count)
            > shape_v5_scalar_max(descriptor.offset_width)
        || descriptor.max_occ_cap >= shape_v5_scalar_max(descriptor.count_width)) {
        return false;
    }
    uint position_bits = descriptor.position_width * 8u;
    uint64_t maximum_segment_span = uint64_t(1u) << (position_bits - 1u);
    if (descriptor.segment_end - descriptor.segment_base > maximum_segment_span) {
        return false;
    }
    if (expected_phase == k_shape_v5_phase_positions) {
        return descriptor.prefix_offset_array_offset == 0u
            && descriptor.count_array_offset == 0u
            && shape_v5_scalar_range_valid(
                descriptor.hash_array_offset,
                uint64_t(descriptor.position_count),
                descriptor.position_width);
    }
    return shape_v5_scalar_range_valid(
               descriptor.hash_array_offset,
               uint64_t(descriptor.control_count),
               descriptor.hash_width)
        && shape_v5_scalar_range_valid(
               descriptor.prefix_offset_array_offset,
               uint64_t(descriptor.control_count) + uint64_t(1u),
               descriptor.offset_width)
        && shape_v5_scalar_range_valid(
               descriptor.count_array_offset,
               uint64_t(descriptor.control_count),
               descriptor.count_width);
}

bool shape_v5_group_dispatch_valid(uint expected_phase) {
    uint64_t payload_bound = uint64_t(shape_v5_pc.payload_word_extent) * uint64_t(4u);
    return shape_v5_pc.abi_revision == k_shape_v5_stream_abi_revision
        && shape_v5_pc.phase == expected_phase
        && shape_v5_pc.reserved_zero == 0u
        && shape_v5_pc.descriptor_count != 0u
        && shape_v5_pc.descriptor_count <= shape_v5_pc.descriptor_extent
        && shape_v5_pc.payload_bytes != 0u
        && uint64_t(shape_v5_pc.payload_bytes) <= payload_bound
        && shape_v5_pc.query_minimizer_count != 0u
        && shape_v5_pc.query_minimizer_count <= shape_v5_pc.query_state_extent
        && shape_v5_pc.query_minimizer_count <= shape_v5_pc.accepted_count_extent
        && shape_v5_pc.query_record_count != 0u
        && shape_v5_pc.query_record_count <= shape_v5_pc.query_record_extent
        && shape_v5_pc.query_wire_kind <= 1u
        && ((shape_v5_pc.query_wire_kind == 0u
                && shape_v5_pc.words32_per_minimizer != 0u
                && shape_v5_pc.word_kind <= 3u)
            || (shape_v5_pc.query_wire_kind == 1u
                && shape_v5_pc.query_minimizer_count <= shape_v5_pc.h64_query_extent))
        && shape_v5_pc.hash_shift < 64u
        && (shape_v5_pc.query_wire_kind == 0u || shape_v5_pc.hash_shift == 0u)
        && shape_v5_pc.pos_bits < 64u
        && shape_v5_pc.kmer_size != 0u
        && shape_v5_pc.kmer_size <= 255u
        && shape_v5_pc.max_occ != 0xffffffffu
        && shape_v5_pc.target_ref_bits <= 63u
        && shape_v5_pc.authorization_count_index < shape_v5_pc.counter_extent
        && shape_v5_pc.anchor_count_index < shape_v5_pc.counter_extent
        && shape_v5_pc.status_index < shape_v5_pc.counter_extent
        && shape_v5_pc.authorization_count_index != shape_v5_pc.anchor_count_index
        && shape_v5_pc.authorization_count_index != shape_v5_pc.status_index
        && shape_v5_pc.anchor_count_index != shape_v5_pc.status_index;
}

bool shape_v5_read_scalar(
    uint byte_offset, uint width, out uint64_t value)
{
    value = uint64_t(0u);
    if (!shape_v5_scalar_range_valid(byte_offset, uint64_t(1u), width)) {
        return false;
    }
    uint word_index = byte_offset >> 2u;
    uint shift = (byte_offset & 3u) * 8u;
    if (width == 2u) {
        value = uint64_t((shape_v5_payload_words[word_index] >> shift) & 0xffffu);
    } else if (width == 4u) {
        value = uint64_t(shape_v5_payload_words[word_index]);
    } else {
        value = uint64_t(shape_v5_payload_words[word_index])
            | (uint64_t(shape_v5_payload_words[word_index + 1u]) << 32u);
    }
    return true;
}

bool shape_v5_scalar_address(
    uint base, uint index, uint width, out uint address)
{
    uint64_t wide = uint64_t(base) + uint64_t(index) * uint64_t(width);
    if (wide > uint64_t(0xffffffffu)) return false;
    address = uint(wide);
    return true;
}

uint64_t shape_v5_expected_minimizer_words() {
    if (shape_v5_pc.query_wire_kind == 1u) return uint64_t(0u);
    if (shape_v5_pc.word_kind == 0u) {
        return (uint64_t(shape_v5_pc.query_minimizer_count) + uint64_t(1u)) / uint64_t(2u);
    }
    return uint64_t(shape_v5_pc.query_minimizer_count)
        * uint64_t(shape_v5_pc.words32_per_minimizer);
}

bool shape_v5_minimizer_valid(uint index) {
    if (index >= shape_v5_pc.query_minimizer_count) return false;
    if (shape_v5_pc.query_wire_kind == 1u) {
        return index < shape_v5_pc.h64_query_extent
            && shape_v5_query_h64[index].reserved_zero == 0u;
    }
    uint64_t expected = shape_v5_expected_minimizer_words();
    if (expected == uint64_t(0u)
        || expected > uint64_t(shape_v5_pc.minimizer_word_extent)) {
        return false;
    }
    if (shape_v5_pc.word_kind == 0u) return uint64_t(index >> 1u) < expected;
    uint required = shape_v5_pc.word_kind == 1u
        ? 1u : shape_v5_pc.word_kind == 2u ? 2u : 4u;
    return shape_v5_pc.words32_per_minimizer >= required
        && (uint64_t(index) + uint64_t(1u))
               * uint64_t(shape_v5_pc.words32_per_minimizer) <= expected;
}

uint64_t shape_v5_minimizer_hash(uint index) {
    if (shape_v5_pc.query_wire_kind == 1u) {
        return shape_v5_query_h64[index].hash;
    }
    if (shape_v5_pc.word_kind == 0u) {
        uint packed = shape_v5_minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        return uint64_t(raw) >> shape_v5_pc.hash_shift;
    }
    uint base = index * shape_v5_pc.words32_per_minimizer;
    if (shape_v5_pc.word_kind == 1u) {
        return uint64_t(shape_v5_minimizer_words[base]) >> shape_v5_pc.hash_shift;
    }
    if (shape_v5_pc.word_kind == 2u) {
        uint64_t raw = uint64_t(shape_v5_minimizer_words[base])
            | (uint64_t(shape_v5_minimizer_words[base + 1u]) << 32u);
        return raw >> shape_v5_pc.hash_shift;
    }
    uint64_t high = uint64_t(shape_v5_minimizer_words[base + 2u])
        | (uint64_t(shape_v5_minimizer_words[base + 3u]) << 32u);
    return high >> 8u;
}

uint64_t shape_v5_minimizer_position(uint index) {
    if (shape_v5_pc.query_wire_kind == 1u) {
        return uint64_t(shape_v5_query_h64[index].query_bp_strand);
    }
    if (shape_v5_pc.word_kind == 0u) {
        uint packed = shape_v5_minimizer_words[index >> 1u];
        uint raw = (packed >> ((index & 1u) * 16u)) & 0xffffu;
        uint mask = shape_v5_pc.pos_bits >= 15u
            ? 0xffffu : ((1u << (shape_v5_pc.pos_bits + 1u)) - 1u);
        return uint64_t(raw & mask);
    }
    uint base = index * shape_v5_pc.words32_per_minimizer;
    if (shape_v5_pc.word_kind == 1u) {
        uint mask = shape_v5_pc.pos_bits >= 31u
            ? 0xffffffffu : ((1u << (shape_v5_pc.pos_bits + 1u)) - 1u);
        return uint64_t(shape_v5_minimizer_words[base] & mask);
    }
    if (shape_v5_pc.word_kind == 2u) {
        uint64_t raw = uint64_t(shape_v5_minimizer_words[base])
            | (uint64_t(shape_v5_minimizer_words[base + 1u]) << 32u);
        uint64_t mask = shape_v5_pc.pos_bits >= 63u
            ? ~uint64_t(0u)
            : ((uint64_t(1u) << (shape_v5_pc.pos_bits + 1u)) - uint64_t(1u));
        return raw & mask;
    }
    return uint64_t(shape_v5_minimizer_words[base])
        | (uint64_t(shape_v5_minimizer_words[base + 1u]) << 32u);
}

bool shape_v5_query_record_valid(shape_v5_query_record record) {
    uint64_t end = uint64_t(record.minimizer_offset) + uint64_t(record.minimizer_count);
    return end <= uint64_t(shape_v5_pc.query_minimizer_count)
        && record.query_length >= shape_v5_pc.kmer_size
        && record.flags == 0u;
}

// Returns 0 for absent, 1 for a valid range, and 2 for malformed controls.
uint shape_v5_find_control(
    shape_v5_partition_wire descriptor,
    uint64_t wanted_hash,
    out uint position_begin,
    out uint position_count)
{
    position_begin = 0u;
    position_count = 0u;
    if (!shape_v5_descriptor_valid(descriptor, k_shape_v5_phase_controls)) return 2u;
    if (wanted_hash > descriptor.max_hash) return 0u;
    uint lo = 0u;
    uint hi = descriptor.control_count;
    while (lo < hi) {
        uint middle = lo + ((hi - lo) >> 1u);
        uint address;
        uint64_t key;
        if (!shape_v5_scalar_address(
                descriptor.hash_array_offset, middle, descriptor.hash_width, address)
            || !shape_v5_read_scalar(address, descriptor.hash_width, key)) {
            return 2u;
        }
        if (key < wanted_hash) lo = middle + 1u; else hi = middle;
    }
    if (lo >= descriptor.control_count) return 0u;
    uint hash_address;
    uint64_t key;
    if (!shape_v5_scalar_address(
            descriptor.hash_array_offset, lo, descriptor.hash_width, hash_address)
        || !shape_v5_read_scalar(hash_address, descriptor.hash_width, key)) {
        return 2u;
    }
    if (key != wanted_hash) return 0u;
    uint begin_address;
    uint end_address;
    uint count_address;
    uint64_t begin_value;
    uint64_t end_value;
    uint64_t saturated_count;
    if (!shape_v5_scalar_address(
            descriptor.prefix_offset_array_offset, lo, descriptor.offset_width, begin_address)
        || !shape_v5_scalar_address(
            descriptor.prefix_offset_array_offset, lo + 1u, descriptor.offset_width, end_address)
        || !shape_v5_scalar_address(
            descriptor.count_array_offset, lo, descriptor.count_width, count_address)
        || !shape_v5_read_scalar(begin_address, descriptor.offset_width, begin_value)
        || !shape_v5_read_scalar(end_address, descriptor.offset_width, end_value)
        || !shape_v5_read_scalar(count_address, descriptor.count_width, saturated_count)
        || begin_value >= end_value
        || end_value > uint64_t(descriptor.position_count)) {
        return 2u;
    }
    uint64_t exact_count = end_value - begin_value;
    uint64_t expected_saturated = exact_count > descriptor.max_occ_cap
        ? descriptor.max_occ_cap + uint64_t(1u) : exact_count;
    if (saturated_count != expected_saturated) return 2u;
    position_begin = uint(begin_value);
    position_count = uint(exact_count);
    return 1u;
}

void shape_v5_saturating_occurrence_add(uint index, uint increment) {
    uint limit = shape_v5_pc.max_occ + 1u;
    uint observed = shape_v5_query_states[index].occurrence_count;
    for (;;) {
        uint wanted = observed >= limit || increment >= limit - observed
            ? limit : observed + increment;
        uint prior = atomicCompSwap(
            shape_v5_query_states[index].occurrence_count, observed, wanted);
        if (prior == observed) return;
        observed = prior;
    }
}

uint64_t shape_v5_anchor_key(uint reverse, uint64_t ref_position) {
    return shape_v5_pc.target_ref_bits >= 63u
        ? (uint64_t(reverse) << 63u) | ref_position
        : (uint64_t(reverse) << shape_v5_pc.target_ref_bits) | ref_position;
}
)GLSL";

/// @brief Look up query hashes in a control group and add their occurrence
/// counts across the target. Save matching position ranges for the later
/// emission pass. The authorization pass decides which hashes pass max_occ.
inline constexpr std::string_view k_grouped_soa_stream_controls_body = R"GLSL(
void main() {
    uint record_index = gl_WorkGroupID.x;
    if (!shape_v5_group_dispatch_valid(k_shape_v5_phase_controls)) {
        shape_v5_signal_status(k_shape_v5_status_contract);
        return;
    }
    for (uint descriptor_index = gl_LocalInvocationID.x;
         descriptor_index < shape_v5_pc.descriptor_count;
         descriptor_index += gl_WorkGroupSize.x) {
        if (!shape_v5_descriptor_valid(
                shape_v5_descriptors[descriptor_index], k_shape_v5_phase_controls)) {
            shape_v5_signal_status(k_shape_v5_status_control);
        }
    }
    barrier();
    if (record_index >= shape_v5_pc.query_record_count) return;
    shape_v5_query_record record = shape_v5_query_records[record_index];
    if (!shape_v5_query_record_valid(record)) {
        shape_v5_signal_status(k_shape_v5_status_query);
        return;
    }
    for (uint local_index = gl_LocalInvocationID.x;
         local_index < record.minimizer_count;
         local_index += gl_WorkGroupSize.x) {
        uint query_minimizer_index = record.minimizer_offset + local_index;
        if (!shape_v5_minimizer_valid(query_minimizer_index)
            || shape_v5_query_states[query_minimizer_index].reserved_zero != 0u) {
            shape_v5_signal_status(k_shape_v5_status_query);
            continue;
        }
        if (shape_v5_pc.query_wire_kind == 1u
            && shape_v5_query_h64[query_minimizer_index].query_record_index != record_index) {
            shape_v5_signal_status(k_shape_v5_status_query);
            continue;
        }
        uint64_t hash = shape_v5_minimizer_hash(query_minimizer_index);
        for (uint descriptor_index = 0u;
             descriptor_index < shape_v5_pc.descriptor_count;
             ++descriptor_index) {
            shape_v5_partition_wire descriptor = shape_v5_descriptors[descriptor_index];
            uint position_begin;
            uint position_count;
            uint found = shape_v5_find_control(
                descriptor, hash, position_begin, position_count);
            if (found == 2u) {
                shape_v5_signal_status(k_shape_v5_status_control);
                continue;
            }
            if (found == 0u) continue;
            shape_v5_saturating_occurrence_add(query_minimizer_index, position_count);
            // A locally over-threshold range can never become authorized and
            // need not consume authorization-output capacity.
            if (position_count > shape_v5_pc.max_occ) continue;
            uint range_index = atomicAdd(
                shape_v5_counters[shape_v5_pc.authorization_count_index], 1u);
            if (range_index >= shape_v5_pc.authorization_range_extent) {
                shape_v5_signal_status(k_shape_v5_status_authorization);
                continue;
            }
            shape_v5_authorization_ranges[range_index].directory_ordinal =
                descriptor.directory_ordinal;
            shape_v5_authorization_ranges[range_index].position_segment =
                descriptor.position_segment;
            shape_v5_authorization_ranges[range_index].query_record_index = record_index;
            shape_v5_authorization_ranges[range_index].query_minimizer_index =
                query_minimizer_index;
            shape_v5_authorization_ranges[range_index].position_begin = position_begin;
            shape_v5_authorization_ranges[range_index].position_count = position_count;
            shape_v5_authorization_ranges[range_index].status = 0u;
            shape_v5_authorization_ranges[range_index].reserved_zero = 0u;
        }
    }
}
)GLSL";

/// @brief After all control groups finish, reject hashes whose total count
/// exceeds max_occ. Publish accepted counts for the prefix sum and reset
/// the cursors used when emitting anchors.
inline constexpr std::string_view k_grouped_soa_stream_authorization_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const uint k_shape_v5_stream_abi_revision = 3u;
const uint k_shape_v5_status_contract = 1u;

struct shape_v5_query_authorization {
    uint occurrence_count;
    uint accepted_count;
    uint emission_cursor;
    uint reserved_zero;
};
layout(std430, set = 0, binding = 4) buffer shape_v5_query_state_buffer {
    shape_v5_query_authorization shape_v5_query_states[];
};
layout(std430, set = 0, binding = 9) buffer shape_v5_counters_buffer {
    uint shape_v5_counters[];
};
layout(std430, set = 0, binding = 10) buffer shape_v5_accepted_counts_buffer {
    uint shape_v5_accepted_counts[];
};
layout(push_constant) uniform shape_v5_authorization_push_constants {
    uint abi_revision;
    uint query_minimizer_count;
    uint max_occ;
    uint query_state_extent;
    uint counter_extent;
    uint status_index;
    uint accepted_count_extent;
    uint reserved_zero0;
    uint reserved_zero1;
} shape_v5_authorization_pc;

void main() {
    bool valid = shape_v5_authorization_pc.abi_revision == k_shape_v5_stream_abi_revision
        && shape_v5_authorization_pc.query_minimizer_count != 0u
        && shape_v5_authorization_pc.query_minimizer_count
            <= shape_v5_authorization_pc.query_state_extent
        && shape_v5_authorization_pc.query_minimizer_count
            <= shape_v5_authorization_pc.accepted_count_extent
        && shape_v5_authorization_pc.max_occ != 0xffffffffu
        && shape_v5_authorization_pc.status_index < shape_v5_authorization_pc.counter_extent
        && shape_v5_authorization_pc.reserved_zero0 == 0u
        && shape_v5_authorization_pc.reserved_zero1 == 0u;
    if (!valid) {
        if (shape_v5_authorization_pc.status_index
            < shape_v5_authorization_pc.counter_extent) {
            atomicOr(shape_v5_counters[shape_v5_authorization_pc.status_index],
                     k_shape_v5_status_contract);
        }
        return;
    }
    uint index = gl_GlobalInvocationID.x;
    if (index >= shape_v5_authorization_pc.query_minimizer_count) return;
    if (shape_v5_query_states[index].reserved_zero != 0u) {
        atomicOr(shape_v5_counters[shape_v5_authorization_pc.status_index],
                 k_shape_v5_status_contract);
        shape_v5_query_states[index].accepted_count = 0u;
        shape_v5_accepted_counts[index] = 0u;
        return;
    }
    uint occurrence_count = shape_v5_query_states[index].occurrence_count;
    uint accepted_count = occurrence_count <= shape_v5_authorization_pc.max_occ
        ? occurrence_count : 0u;
    shape_v5_query_states[index].accepted_count = accepted_count;
    shape_v5_query_states[index].emission_cursor = 0u;
    shape_v5_accepted_counts[index] = accepted_count;
}
)GLSL";

/// @brief Read accepted position ranges and emit anchors for GPU sorting.
/// Add the segment base to each stored position to get its reference coordinate.
/// Check each output range against the space assigned by the prefix sum.
inline constexpr std::string_view k_grouped_soa_stream_positions_body = R"GLSL(
shared uint shape_v5_range_output_base;
shared uint shape_v5_range_is_valid;

void main() {
    if (!shape_v5_group_dispatch_valid(k_shape_v5_phase_positions)) {
        shape_v5_signal_status(k_shape_v5_status_contract);
        return;
    }
    // Runtime dispatches at least one workgroup even when the authorization
    // count is zero, so every descriptor repeats the host's phase/bounds checks.
    for (uint descriptor_index = gl_LocalInvocationID.x;
         descriptor_index < shape_v5_pc.descriptor_count;
         descriptor_index += gl_WorkGroupSize.x) {
        if (!shape_v5_descriptor_valid(
                shape_v5_descriptors[descriptor_index], k_shape_v5_phase_positions)) {
            shape_v5_signal_status(k_shape_v5_status_position);
        }
    }
    barrier();
    uint available_ranges =
        shape_v5_counters[shape_v5_pc.authorization_count_index];
    if (available_ranges > shape_v5_pc.authorization_range_extent) {
        shape_v5_signal_status(k_shape_v5_status_authorization);
        return;
    }
    uint range_index = gl_WorkGroupID.x;
    if (range_index >= available_ranges) return;
    shape_v5_authorization_range range = shape_v5_authorization_ranges[range_index];
    if (range.status != 0u || range.reserved_zero != 0u
        || range.query_minimizer_index >= shape_v5_pc.query_minimizer_count
        || range.query_record_index >= shape_v5_pc.query_record_count) {
        shape_v5_signal_status(k_shape_v5_status_authorization);
        return;
    }
    shape_v5_partition_wire descriptor;
    bool descriptor_found = false;
    for (uint descriptor_index = 0u;
         descriptor_index < shape_v5_pc.descriptor_count;
         ++descriptor_index) {
        shape_v5_partition_wire candidate = shape_v5_descriptors[descriptor_index];
        if (candidate.directory_ordinal == range.directory_ordinal
            && candidate.position_segment == range.position_segment) {
            if (descriptor_found) {
                shape_v5_signal_status(k_shape_v5_status_position);
                return;
            }
            descriptor = candidate;
            descriptor_found = true;
        }
    }
    // This authorization range belongs to another streamed position group.
    if (!descriptor_found) return;
    if (!shape_v5_descriptor_valid(descriptor, k_shape_v5_phase_positions)
        || range.position_count == 0u
        || range.position_count > shape_v5_pc.max_occ
        || range.position_begin > descriptor.position_count
        || range.position_count > descriptor.position_count - range.position_begin) {
        shape_v5_signal_status(k_shape_v5_status_position);
        return;
    }
    uint query_minimizer_index = range.query_minimizer_index;
    uint accepted_count = shape_v5_accepted_counts[query_minimizer_index];
    if (accepted_count == 0u) return;
    if (shape_v5_query_states[query_minimizer_index].accepted_count != accepted_count
        || shape_v5_query_states[query_minimizer_index].reserved_zero != 0u
        || query_minimizer_index >= shape_v5_pc.prefix_extent) {
        shape_v5_signal_status(k_shape_v5_status_authorization);
        return;
    }
    shape_v5_query_record record = shape_v5_query_records[range.query_record_index];
    uint64_t record_end = uint64_t(record.minimizer_offset) + uint64_t(record.minimizer_count);
    if (!shape_v5_query_record_valid(record)
        || query_minimizer_index < record.minimizer_offset
        || uint64_t(query_minimizer_index) >= record_end
        || !shape_v5_minimizer_valid(query_minimizer_index)) {
        shape_v5_signal_status(k_shape_v5_status_query);
        return;
    }
    if (shape_v5_pc.query_wire_kind == 1u
        && shape_v5_query_h64[query_minimizer_index].query_record_index
            != range.query_record_index) {
        shape_v5_signal_status(k_shape_v5_status_query);
        return;
    }

    if (gl_LocalInvocationID.x == 0u) {
        uint prior = atomicAdd(
            shape_v5_query_states[query_minimizer_index].emission_cursor,
            range.position_count);
        uint destination = shape_v5_prefix[query_minimizer_index];
        bool output_valid = prior <= accepted_count
            && range.position_count <= accepted_count - prior
            && destination <= shape_v5_pc.anchor_extent
            && prior <= shape_v5_pc.anchor_extent - destination
            && range.position_count <= shape_v5_pc.anchor_extent - destination - prior
            && destination <= shape_v5_pc.anchor_key_extent
            && prior <= shape_v5_pc.anchor_key_extent - destination
            && range.position_count <= shape_v5_pc.anchor_key_extent - destination - prior;
        shape_v5_range_output_base = destination + prior;
        shape_v5_range_is_valid = output_valid ? 1u : 0u;
        uint old_anchor_count = atomicAdd(
            shape_v5_counters[shape_v5_pc.anchor_count_index], range.position_count);
        bool count_valid = old_anchor_count <= shape_v5_pc.anchor_extent
            && range.position_count <= shape_v5_pc.anchor_extent - old_anchor_count;
        if (!output_valid || !count_valid) {
            shape_v5_range_is_valid = 0u;
            shape_v5_signal_status(k_shape_v5_status_emission);
        }
    }
    barrier();
    if (shape_v5_range_is_valid == 0u) return;

    uint64_t query_encoded = shape_v5_minimizer_position(query_minimizer_index);
    uint64_t query_bp = query_encoded >> 1u;
    uint query_strand = uint(query_encoded & uint64_t(1u));
    if (query_bp >= uint64_t(record.query_length)
        || query_bp + uint64_t(1u) < uint64_t(shape_v5_pc.kmer_size)) {
        shape_v5_signal_status(k_shape_v5_status_query);
        return;
    }
    uint64_t reverse_base = uint64_t(record.query_length)
        + uint64_t(shape_v5_pc.kmer_size) - uint64_t(2u);
    for (uint local = gl_LocalInvocationID.x;
         local < range.position_count;
         local += gl_WorkGroupSize.x) {
        uint position_index = range.position_begin + local;
        uint address;
        uint64_t target_encoded;
        if (!shape_v5_scalar_address(
                descriptor.hash_array_offset,
                position_index,
                descriptor.position_width,
                address)
            || !shape_v5_read_scalar(address, descriptor.position_width, target_encoded)) {
            shape_v5_signal_status(k_shape_v5_status_position);
            continue;
        }
        uint64_t local_bp = target_encoded >> 1u;
        uint64_t segment_span = descriptor.segment_end - descriptor.segment_base;
        if (local_bp >= segment_span) {
            shape_v5_signal_status(k_shape_v5_status_position);
            continue;
        }
        uint target_strand = uint(target_encoded & uint64_t(1u));
        uint reverse = target_strand ^ query_strand;
        uint64_t ref_position = descriptor.segment_base + local_bp;
        if (ref_position >= (uint64_t(1u) << shape_v5_pc.target_ref_bits)) {
            shape_v5_signal_status(k_shape_v5_status_position);
            continue;
        }
        uint64_t query_position = reverse != 0u ? reverse_base - query_bp : query_bp;
        uint output_index = shape_v5_range_output_base + local;
        shape_v5_anchors[output_index].ref_position = ref_position;
        shape_v5_anchors[output_index].query_position = query_position;
        shape_v5_anchors[output_index].is_reverse = reverse;
        shape_v5_anchors[output_index].span_flags_segment = shape_v5_pc.kmer_size & 0xffu;
        shape_v5_anchor_keys[output_index] = shape_v5_anchor_key(reverse, ref_position);
    }
}
)GLSL";

} // namespace bioseq::gpu::vulkan
