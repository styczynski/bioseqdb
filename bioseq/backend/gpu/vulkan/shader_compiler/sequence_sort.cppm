/**
 * @file sequence_sort.cppm
 * @brief Owns packed-sequence generation and sequence-sort compute shader bodies.
 */
module bioseq.gpu.vulkan.shader_compiler:sequence_sort;

import :registry;
import std;

namespace bioseq::gpu::vulkan {

/// @brief Generates packed sequence storage from a deterministic rank stream.
inline constexpr std::string_view k_generate_units_body = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x, local_size_y = 1, local_size_z = 1) in;

#define storage_unit_bits compile_time_maximum_span
#if storage_unit_bits == 64
  #define storage_unit uint64_t
#else
  #define storage_unit uint
#endif

layout(std430, set = 0, binding = 0) writeonly buffer output_units_buffer { storage_unit out_units[]; };
layout(std430, set = 0, binding = 1) readonly  buffer cdf_buffer      { uint cdf[]; };

layout(push_constant) uniform push_constants {
    uint symbols_per_seq;
    uint units_per_seq;
    uint chunks_per_seq;
    uint chunk_units;
    uint letters_per_unit;
    uint bits_per_char;
    uint alphabet_size;
    uint total_chunks;
    uint chunk_base;
    uint seed_lo;
    uint seed_hi;
    uint _pad;
} pc;

uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15UL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9UL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebUL;
    return x ^ (x >> 31);
}

uint taus_step(uint z, int s1, int s2, int s3, uint m) {
    uint b = (((z << s1) ^ z) >> s2);
    return (((z & m) << s3) ^ b);
}

uint next_u32(inout uvec4 s) {
    s.x = taus_step(s.x, 13, 19, 12, 4294967294u);
    s.y = taus_step(s.y,  2, 25,  4, 4294967288u);
    s.z = taus_step(s.z,  3, 11, 17, 4294967280u);
    s.w = s.w * 1664525u + 1013904223u;
    return s.x ^ s.y ^ s.z ^ s.w;
}

uvec4 seed_stream(uint64_t seed, uint64_t stream_id) {
    uint64_t a = splitmix64(seed ^ (stream_id * 0x9e3779b97f4a7c15UL));
    uint64_t b = splitmix64(a);
    uvec4 s = uvec4(uint(a), uint(a >> 32), uint(b), uint(b >> 32));
    if (s.x < 129u) s.x += 129u;
    if (s.y < 129u) s.y += 129u;
    if (s.z < 129u) s.z += 129u;
    return s;
}

uint sample_rank(uint u) {
    uint rank = 0u;
    while (rank + 1u < pc.alphabet_size && u >= cdf[rank]) rank += 1u;
    return rank;
}

void main() {
    uint chunk = pc.chunk_base + gl_GlobalInvocationID.x;
    if (chunk >= pc.total_chunks) return;

    uint seq = chunk / pc.chunks_per_seq;
    uint local_chunk = chunk - seq * pc.chunks_per_seq;
    uint seq_unit0 = seq * pc.units_per_seq;
    uint local_u0 = local_chunk * pc.chunk_units;

    uvec4 st = seed_stream(
        (uint64_t(pc.seed_hi) << 32) | uint64_t(pc.seed_lo),
        uint64_t(chunk));

    for (uint u = 0u; u < pc.chunk_units; ++u) {
        uint local_unit = local_u0 + u;
        if (local_unit >= pc.units_per_seq) break;
        uint sym0 = local_unit * pc.letters_per_unit;
        uint valid = min(pc.letters_per_unit, pc.symbols_per_seq - sym0);
        storage_unit acc = storage_unit(0);
        for (uint slot = 0u; slot < valid; ++slot) {
            uint r = sample_rank(next_u32(st));
            acc |= storage_unit(r) << (slot * pc.bits_per_char);
        }
        out_units[seq_unit0 + local_unit] = acc;
    }
}
)GLSL";

// ===========================================================================
// GPU sequence-sort kernels (dna4-optimized and generic)
//
// Each kind's body is composed in shader_body_for from k_sequence_sort_push,
// k_sequence_sort_readers, the selected canonicalizer,
// k_sequence_sort_key_compare, and one stage main. shader_kind selects the
// canonicalizer. Storage width is runtime via pc.unit_bits and
// read_storage_unit64. compile_time_workgroup_size_x comes from the injected
// prelude.
// ===========================================================================

/// @brief Push-constant block (40 bytes). Identical for
/// all seq-sort kinds so one struct serves every stage.
inline constexpr std::string_view k_sequence_sort_push = R"GLSL(
layout(push_constant) uniform sequence_sort_push_constants {
    uint count;              // row_count (key stage) / range_count (tiebreak stage)
    uint unit_bits;          // 8/16/32/64
    uint bits_per_char;      // generic only (dna4 uses const 2)
    uint letters_per_unit;   // generic only
    uint char_mask;          // generic only
    uint used_mask_lo;       // used_storage_mask low 32
    uint used_mask_hi;       // used_storage_mask high 32
    uint key_unit_offset;       // dense scatter stage: first unit for the 64-bit key
    uint sort_size;          // global bitonic stage: k
    uint sort_stride;        // global bitonic stage: j
} pc;
)GLSL";

/// @brief Descriptor bindings + packed-sequence reader + low_symbol_mask. Declared
/// BEFORE the canonicalizer so it can call low_symbol_mask, and before the
/// key/compare snippet which needs sequence_descriptor + read_unit. read_unit() forward-uses
/// unit_bits() (defined in the canon snippet that follows), which is fine in
/// GLSL because the canon snippet is concatenated between this and k_sequence_sort_key_compare.
inline constexpr std::string_view k_sequence_sort_readers = R"GLSL(
struct sequence_descriptor { uint word_offset; uint unit_count; uint bp_size; uint pad; };  // std430, 16B

layout(std430, set=0, binding=0) readonly buffer sequence_words_buffer { uint    seq_words[]; };
layout(std430, set=0, binding=1) readonly buffer descriptors_buffer { sequence_descriptor descs[]; };

uint read_u32_word(uint word_index) { return seq_words[word_index]; }

uint read_storage_unit(uint unit_index, uint ubits, uint byte_offset_words) {
    if (ubits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return lo ^ hi; // unused for 64-bit (read_storage_unit64 handles it)
    }
    if (ubits == 32u) return read_u32_word(byte_offset_words + unit_index);
    if (ubits == 16u) {
        uint w = read_u32_word(byte_offset_words + (unit_index >> 1u));
        return (w >> ((unit_index & 1u) * 16u)) & 0xffffu;
    }
    uint w = read_u32_word(byte_offset_words + (unit_index >> 2u));
    return (w >> ((unit_index & 3u) * 8u)) & 0xffu;
}

uint64_t read_storage_unit64(uint unit_index, uint ubits, uint byte_offset_words) {
    if (ubits == 64u) {
        uint lo = read_u32_word(byte_offset_words + unit_index * 2u);
        uint hi = read_u32_word(byte_offset_words + unit_index * 2u + 1u);
        return (uint64_t(hi) << 32) | uint64_t(lo);
    }
    return uint64_t(read_storage_unit(unit_index, ubits, byte_offset_words));
}

// keep low valid_syms*bpc bits, zero the rest
uint64_t low_symbol_mask(uint valid_syms, uint bpc) {
    uint bits = valid_syms * bpc;
    return bits >= 64u ? ~uint64_t(0) : ((uint64_t(1) << bits) - uint64_t(1));
}
)GLSL";

/// @brief Canonical key + full canonical compare. Uses canon()/lpu()/unit_bits() from
/// the canonicalizer snippet (concatenated immediately before this).
inline constexpr std::string_view k_sequence_sort_key_compare = R"GLSL(
uint64_t read_unit(sequence_descriptor d, uint j) {
    return read_storage_unit64(j, unit_bits(), d.word_offset);
}

// 64-bit lexicographic radix key = first 64 canonical bits (zero-filled tail).
uint64_t canon_key(sequence_descriptor d) {
    uint64_t key = 0ul; uint filled = 0u;
    uint ub = unit_bits();
    for (uint j = 0u; j < d.unit_count && filled < 64u; ++j) {
        uint base0 = j * lpu();
        uint valid = min(lpu(), d.bp_size - base0);
        uint64_t cu = canon(read_unit(d, j), valid);   // valid bases in TOP ub bits
        if (filled + ub <= 64u) key |= cu << (64u - filled - ub);
        else                    key |= cu >> (filled + ub - 64u);
        filled += ub;
    }
    return key;
}

// 64-bit lexicographic radix key starting at a unit offset. This is used by the
// dense scatter fast path after common-prefix validation establishes that all
// earlier units are identical across the batch.
uint64_t canon_key_at(sequence_descriptor d, uint start_unit) {
    uint64_t key = 0ul; uint filled = 0u;
    uint ub = unit_bits();
    for (uint j = start_unit; j < d.unit_count && filled < 64u; ++j) {
        uint base0 = j * lpu();
        if (base0 >= d.bp_size) break;
        uint valid = min(lpu(), d.bp_size - base0);
        uint64_t cu = canon(read_unit(d, j), valid);
        if (filled + ub <= 64u) key |= cu << (64u - filled - ub);
        else                    key |= cu >> (filled + ub - 64u);
        filled += ub;
    }
    return key;
}

// Full canonical compare: -1 / 0 / +1 (matches compare_sequences_scalar exactly).
int canon_cmp(sequence_descriptor a, sequence_descriptor b) {
    uint units = min(a.unit_count, b.unit_count);
    for (uint j = 0u; j < units; ++j) {
        uint base0 = j * lpu();
        uint v = min(min(lpu(), a.bp_size - base0), min(lpu(), b.bp_size - base0));
        uint64_t ca = canon(read_unit(a, j), v);
        uint64_t cb = canon(read_unit(b, j), v);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return a.bp_size < b.bp_size ? -1 : (a.bp_size > b.bp_size ? 1 : 0);  // length tiebreak
}
)GLSL";

/// @brief dna4 canonicalizer: compile-time bits_per_char==2, no masking (used_mask==~0).
inline constexpr std::string_view k_dna4_canonicalizer = R"GLSL(
uint lpu()       { return pc.unit_bits / 2u; }    // bits_per_char == 2
uint unit_bits() { return pc.unit_bits; }

uint64_t canon_dna4(uint64_t x, uint ubits) {     // x has data in low ubits bits
    if (ubits == 64u) {
        x = ((x & 0x3333333333333333UL) << 2)  | ((x & 0xCCCCCCCCCCCCCCCCUL) >> 2);
        x = ((x & 0x0F0F0F0F0F0F0F0FUL) << 4)  | ((x & 0xF0F0F0F0F0F0F0F0UL) >> 4);
        x = ((x & 0x00FF00FF00FF00FFUL) << 8)  | ((x & 0xFF00FF00FF00FF00UL) >> 8);
        x = ((x & 0x0000FFFF0000FFFFUL) << 16) | ((x & 0xFFFF0000FFFF0000UL) >> 16);
        return (x << 32) | (x >> 32);
    }
    if (ubits == 32u) { uint y = uint(x);
        y = ((y & 0x33333333u)<<2)|((y & 0xCCCCCCCCu)>>2);
        y = ((y & 0x0F0F0F0Fu)<<4)|((y & 0xF0F0F0F0u)>>4);
        y = ((y & 0x00FF00FFu)<<8)|((y & 0xFF00FF00u)>>8);
        y = (y<<16)|(y>>16); return uint64_t(y); }
    if (ubits == 16u) { uint y = uint(x) & 0xFFFFu;
        y = ((y & 0x3333u)<<2)|((y & 0xCCCCu)>>2);
        y = ((y & 0x0F0Fu)<<4)|((y & 0xF0F0u)>>4);
        y = ((y & 0x00FFu)<<8)|((y & 0xFF00u)>>8); return uint64_t(y); }
    uint y = uint(x) & 0xFFu;                       // ubits == 8
        y = ((y & 0x33u)<<2)|((y & 0xCCu)>>2);
        y = ((y & 0x0Fu)<<4)|((y & 0xF0u)>>4); return uint64_t(y);
}

uint64_t canon(uint64_t raw, uint valid) {
    uint64_t x = (valid >= lpu()) ? raw : (raw & low_symbol_mask(valid, 2u));
    return canon_dna4(x, pc.unit_bits);
}
)GLSL";

/// @brief generic canonicalizer: alphabet params from push constants, masks used_mask.
inline constexpr std::string_view k_generic_canonicalizer = R"GLSL(
uint lpu()       { return pc.letters_per_unit; }
uint unit_bits() { return pc.unit_bits; }

uint64_t canon_generic(uint64_t raw, uint lp, uint bpc, uint64_t cmask) {
    uint64_t outv = 0ul;
    for (uint k = 0u; k < lp; ++k) {
        uint64_t sym = (raw >> (k * bpc)) & cmask;          // base k (LSB-first in raw)
        outv |= sym << ((lp - 1u - k) * bpc);               // -> reversed slot
    }
    return outv;   // valid bases occupy the TOP lp*bpc bits; low bits = 0
}

uint64_t canon(uint64_t raw, uint valid) {
    uint64_t used = (uint64_t(pc.used_mask_hi) << 32) | uint64_t(pc.used_mask_lo);
    uint64_t m = raw & used;                                // clear unit_leftover_bits
    if (valid < lpu()) m &= low_symbol_mask(valid, pc.bits_per_char);
    return canon_generic(m, lpu(), pc.bits_per_char, uint64_t(pc.char_mask));
}
)GLSL";

/// @brief Kernel A main: one invocation per row -> (key:uint64, value:uint64=row index).
inline constexpr std::string_view k_sequence_sort_key_main = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x) in;
layout(std430, set=0, binding=2) writeonly buffer keys_buffer { uint64_t keys[]; };
layout(std430, set=0, binding=3) writeonly buffer values_buffer { uint64_t vals[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    keys[i] = canon_key(descs[i]);
    vals[i] = uint64_t(i);
}
)GLSL";

/// @brief Kernel B main uses one workgroup per equal-key run and bitonic `canon_cmp`.
inline constexpr std::string_view k_sequence_sort_tiebreak_main = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x) in;
layout(std430, set=0, binding=2) buffer index_buffer           { uint index[];  };       // radix-sorted row ids (in/out)
struct sequence_sort_range { uint begin; uint len; };
layout(std430, set=0, binding=3) readonly buffer ranges_buffer { sequence_sort_range ranges[]; };
shared uint sh_idx[compile_time_workgroup_size_x];
void main() {
    uint rid = gl_WorkGroupID.x; uint t = gl_LocalInvocationID.x;
    if (rid >= pc.count) return;
    sequence_sort_range r = ranges[rid];
    if (r.len > compile_time_workgroup_size_x) return;                       // global GPU stage handles oversized runs
    sh_idx[t] = (t < r.len) ? index[r.begin + t] : 0xffffffffu;   // sentinel sorts last
    barrier();
    for (uint size = 2u; size <= compile_time_workgroup_size_x; size <<= 1u)
      for (uint stride = size >> 1u; stride > 0u; stride >>= 1u) {
        uint o = t ^ stride;
        if (o > t) {
            uint ia = sh_idx[t], ib = sh_idx[o];
            bool up = (t & size) == 0u;
            int c;
            if (ia == 0xffffffffu) c = 1; else if (ib == 0xffffffffu) c = -1;
            else { c = canon_cmp(descs[ia], descs[ib]); if (c == 0) c = (ia < ib) ? -1 : 1; }
            bool do_swap = up ? (c > 0) : (c < 0);
            if (do_swap) { sh_idx[t] = ib; sh_idx[o] = ia; }
        }
        barrier();
      }
    if (t < r.len) index[r.begin + t] = sh_idx[t];
}
)GLSL";

/// @brief Kernel C main: one global bitonic compare-swap stage over the padded index
/// buffer. Used only when a 64-bit key collision run is too large for the
/// per-workgroup tiebreak kernel.
inline constexpr std::string_view k_sequence_sort_global_compare_main = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x) in;
layout(std430, set=0, binding=2) buffer index_buffer { uint index[]; };
layout(std430, set=0, binding=3) readonly buffer unused_ranges_buffer { uint unused_ranges[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    uint other = i ^ pc.sort_stride;
    if (other <= i || other >= pc.count) return;

    uint ia = index[i];
    uint ib = index[other];
    bool up = (i & pc.sort_size) == 0u;
    int c;
    if (ia == 0xffffffffu) c = 1;
    else if (ib == 0xffffffffu) c = -1;
    else {
        c = canon_cmp(descs[ia], descs[ib]);
        if (c == 0) c = (ia < ib) ? -1 : 1;
    }
    bool do_swap = up ? (c > 0) : (c < 0);
    if (do_swap) {
        index[i] = ib;
        index[other] = ia;
    }
}
)GLSL";

/// @brief Kernel D main: dense-rank scatter. This is a verified fast path for batches
/// whose selected 64-bit canonical key is exactly a permutation of [0,count).
/// For non-zero key offsets the shader also validates on device that all earlier
/// units match row 0. Otherwise sorting by a later key would not be lexicographic.
inline constexpr std::string_view k_sequence_sort_dense_scatter_main = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x) in;
layout(std430, set=0, binding=2) buffer index_buffer { uint index[]; };
layout(std430, set=0, binding=3) buffer status_buffer { uint status[]; };

bool prefix_matches_first(sequence_descriptor d) {
    if (pc.key_unit_offset == 0u) return true;
    if (pc.sort_size != 0u) return true;  // prefix_check stage already validated it
    sequence_descriptor first = descs[0];
    if (d.bp_size != first.bp_size || d.unit_count != first.unit_count) return false;
    if (pc.key_unit_offset > d.unit_count) return false;
    for (uint j = 0u; j < pc.key_unit_offset; ++j) {
        uint base0 = j * lpu();
        if (base0 >= d.bp_size) break;
        uint valid = min(lpu(), d.bp_size - base0);
        uint64_t ca = canon(read_unit(d, j), valid);
        uint64_t cb = canon(read_unit(first, j), valid);
        if (ca != cb) return false;
    }
    return true;
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    sequence_descriptor d = descs[i];
    if (!prefix_matches_first(d)) {
        atomicOr(status[0], 2u);
        return;
    }
    uint64_t key = canon_key_at(d, pc.key_unit_offset);
    if ((key >> 32) != 0ul || uint(key) >= pc.count) {
        atomicOr(status[0], 1u);
        return;
    }
    uint dst = uint(key);
    uint old = atomicExchange(index[dst], i);
    if (old != 0xffffffffu) {
        atomicOr(status[0], 1u);
    }
}
)GLSL";

/// @brief Kernel E main: parallel common-prefix validation for dense tail keys. One
/// invocation checks one (row, unit-before-key) pair against row 0 and ORs the
/// status word on any mismatch. Dense scatter then runs after a command-buffer
/// barrier with pc.sort_size != 0 to avoid rechecking the prefix serially.
inline constexpr std::string_view k_sequence_sort_prefix_check_main = R"GLSL(
layout(local_size_x = compile_time_workgroup_size_x) in;
layout(std430, set=0, binding=2) readonly buffer unused_index_buffer { uint unused_index[]; };
layout(std430, set=0, binding=3) buffer status_buffer { uint status[]; };

void main() {
    if (pc.key_unit_offset == 0u) return;
    uint linear_id = gl_GlobalInvocationID.x;
    uint row = linear_id / pc.key_unit_offset;
    uint j = linear_id - row * pc.key_unit_offset;
    if (row >= pc.count) return;

    sequence_descriptor d = descs[row];
    sequence_descriptor first = descs[0];
    if (d.bp_size != first.bp_size || d.unit_count != first.unit_count
        || pc.key_unit_offset > d.unit_count) {
        atomicOr(status[0], 2u);
        return;
    }
    uint base0 = j * lpu();
    if (base0 >= d.bp_size) return;
    uint valid = min(lpu(), d.bp_size - base0);
    uint64_t ca = canon(read_unit(d, j), valid);
    uint64_t cb = canon(read_unit(first, j), valid);
    if (ca != cb) {
        atomicOr(status[0], 2u);
    }
}
)GLSL";

/// @brief Internal sequence-sort program stage selected during body composition.
enum class sequence_sort_stage : std::uint8_t {
    key,
    tiebreak,
    global_compare,
    dense_scatter,
    prefix_check,
};
/// @brief Compose a seq-sort kind's full body from the shared snippets. Order matters:
/// push -> readers/low_symbol_mask -> canon (defines lpu/unit_bits/canon) ->
/// key/compare (uses canon) -> stage main.
[[nodiscard]] std::string seq_sort_body(bool dna4, sequence_sort_stage stage) {
    std::string s;
    s.reserve(k_sequence_sort_push.size() + k_sequence_sort_readers.size() +
              k_sequence_sort_key_compare.size() + 1536);
    s += k_sequence_sort_push;
    s += k_sequence_sort_readers;
    s += (dna4 ? k_dna4_canonicalizer : k_generic_canonicalizer);
    s += k_sequence_sort_key_compare;
    switch (stage) {
    case sequence_sort_stage::key:
        s += k_sequence_sort_key_main;
        break;
    case sequence_sort_stage::tiebreak:
        s += k_sequence_sort_tiebreak_main;
        break;
    case sequence_sort_stage::global_compare:
        s += k_sequence_sort_global_compare_main;
        break;
    case sequence_sort_stage::dense_scatter:
        s += k_sequence_sort_dense_scatter_main;
        break;
    case sequence_sort_stage::prefix_check:
        s += k_sequence_sort_prefix_check_main;
        break;
    }
    return s;
}

} // namespace bioseq::gpu::vulkan
