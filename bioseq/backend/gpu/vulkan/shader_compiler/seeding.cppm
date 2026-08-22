/**
 * @file seeding.cppm
 * @brief Owns anchor lookup, anchor sort, and seed count/emit compute shader bodies.
 */
module bioseq.gpu.vulkan.shader_compiler:seeding;

import :registry;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Counts and emits anchors from the resident minimizer slot table.
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

    uint64_t hash = minimizer_fragment_hash(minimizer_index);
    uint slot_index = find_hash_slot(hash);
    if (slot_index == 0xffffffffu) return;

    gpu_minimizer_slot slot = slots[slot_index];
    uint begin = prefix_counts[minimizer_index];
    uint end = prefix_counts[minimizer_index + 1u];
    if (slot.pos_count == 0u || begin == end) return;

    uint64_t query_encoded = minimizer_fragment_position(minimizer_index);
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

/// @brief Per-query bitonic anchor sort. One workgroup sorts up to
/// compile_time_workgroup_size_x anchors by `ref_bits()` in shared memory. Larger anchor sets
/// are routed to the stable device radix path. Host sorting is forbidden.
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
/// @brief GPU seed count/emit. One workgroup per query record. Local
/// invocations stride over that record's minimizers. `emit_mode==0` writes the
/// per-minimizer anchor count (clamped by `max_occ`) to `counts`. `emit_mode==1`
/// reads the exclusive `prefix` and emits `gpu_anchor_wire` records plus their
/// compact `(strand, ref)` sort key.
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

/// @brief Direct persisted grouped-SoA seed count/emit. One workgroup owns one
/// query record and its lanes stride over query minimizers.  Each minimizer is
/// matched against the selected resident partition descriptors, binary-searches
/// the raw variable-width SoA hash arrays, and sums occurrences across every
/// position segment before applying `max_occ`.  Emit rereads the authenticated
/// prefix ranges and variable-width segment-local positions directly. It never
/// constructs a CPU-decoded slot table or a widened/globalized position array.
inline constexpr std::string_view k_grouped_soa_seed_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

const uint k_grouped_soa_abi_revision = 3u;
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
} pc;

void grouped_soa_signal_status(uint status_bit) {
    if (pc.status_index < uint(counts.length())) {
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

bool minimizer_fragment_words_valid(uint index) {
    if (pc.word_kind > 3u || pc.words32_per_minimizer == 0u) return false;
    if (pc.word_kind == 0u) {
        return uint64_t(index >> 1u) < uint64_t(minimizer_words.length());
    }
    uint required_words = pc.word_kind == 1u
        ? 1u : pc.word_kind == 2u ? 2u : 4u;
    if (pc.words32_per_minimizer < required_words) return false;
    uint64_t end = (uint64_t(index) + uint64_t(1u))
        * uint64_t(pc.words32_per_minimizer);
    return end <= uint64_t(minimizer_words.length());
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

bool grouped_soa_count_hash(uint64_t hash, out uint accepted_count) {
    accepted_count = 0u;
    uint64_t total = uint64_t(0);
    uint64_t threshold = uint64_t(pc.max_occ);
    for (uint partition_index = 0u;
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
            return true;
        }
        uint64_t exact_count = position_end - position_begin;
        if (total > threshold || exact_count > threshold - total) {
            accepted_count = 0u;
            return true;
        }
        total += exact_count;
    }
    accepted_count = uint(total);
    return true;
}

uint64_t grouped_soa_anchor_sort_key(uint reverse, uint64_t ref_position) {
    if (pc.target_ref_bits >= 63u) {
        return (uint64_t(reverse) << 63u) | ref_position;
    }
    return (uint64_t(reverse) << pc.target_ref_bits) | ref_position;
}

bool grouped_soa_dispatch_valid() {
    uint64_t cap = grouped_soa_max_occ_cap();
    return pc.abi_revision == k_grouped_soa_abi_revision
        && pc.emit_mode <= 1u
        && pc.kmer_size != 0u
        && pc.kmer_size <= 255u
        && pc.hash_shift < 64u
        && pc.pos_bits < 64u
        && pc.target_ref_bits <= 63u
        && cap != ~uint64_t(0)
        && uint64_t(pc.max_occ) <= cap
        && pc.partition_count != 0u
        && pc.partition_count <= uint(partitions.length())
        && pc.query_record_count == 1u
        && uint(query_records.length()) >= 1u
        && pc.query_minimizer_count != 0u
        && pc.status_index != 0u
        && pc.status_index < uint(counts.length())
        && pc.status_index == uint(prefix.length())
        && pc.control_word_count <= uint(control_words.length())
        && pc.position_word_count <= uint(position_words.length())
        && pc.anchor_capacity <= uint(anchors.length())
        && pc.anchor_capacity <= uint(anchor_keys.length());
}

void main() {
    uint record_index = gl_WorkGroupID.x;
    bool dispatch_valid = grouped_soa_dispatch_valid();
    if (!dispatch_valid || record_index != 0u) {
        grouped_soa_signal_status(k_grouped_soa_status_validation);
        return;
    }
    minimizer_fragment_query_record record = query_records[record_index];
    uint64_t query_end = uint64_t(record.minimizer_offset)
        + uint64_t(record.minimizer_count);
    bool record_valid = query_end <= uint64_t(pc.query_minimizer_count)
        && record.minimizer_count == pc.status_index
        && record.query_length >= pc.kmer_size
        && record.flags == 0u;
    if (!record_valid) {
        grouped_soa_signal_status(k_grouped_soa_status_query);
        return;
    }

    for (uint local_index = gl_LocalInvocationID.x;
         local_index < record.minimizer_count;
         local_index += gl_WorkGroupSize.x) {
        uint minimizer_index = record.minimizer_offset + local_index;
        if (pc.emit_mode == 0u) counts[local_index] = 0u;
        if (minimizer_index >= pc.query_minimizer_count
            || !minimizer_fragment_words_valid(minimizer_index)) {
            grouped_soa_signal_status(k_grouped_soa_status_query);
            continue;
        }

        uint64_t hash = minimizer_fragment_hash(minimizer_index);
        uint accepted_count = 0u;
        if (!grouped_soa_count_hash(hash, accepted_count)) {
            grouped_soa_signal_status(k_grouped_soa_status_control);
            continue;
        }
        if (pc.emit_mode == 0u) {
            counts[local_index] = accepted_count;
            continue;
        }
        if (counts[local_index] != accepted_count) {
            grouped_soa_signal_status(k_grouped_soa_status_emission);
            continue;
        }
        if (accepted_count == 0u) continue;

        uint64_t destination = uint64_t(prefix[local_index]);
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
        for (uint partition_index = 0u;
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
        }
    }
}
)GLSL";

} // namespace bioseq::gpu::vulkan
