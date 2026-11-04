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
// A positive constant is admitted only when every uploaded descriptor has that width.
uint native_packed_rank_bits(gpu_sequence_view sequence) {
    return compile_time_native_rank_bits == 0u
        ? sequence.bits_per_char : compile_time_native_rank_bits;
}
uint native_packed_rank_mask(gpu_sequence_view sequence) {
    return compile_time_native_rank_bits == 0u
        ? sequence.char_mask : (1u << compile_time_native_rank_bits) - 1u;
}
uint packed_sequence_byte(uint64_t byte_index, bool query);
// Rank widths dividing a byte have no padding between native storage units.
// Borrow the same bytes as 32-bit transport words, retaining the logical origin
// and length. The host admits the constant only for matching descriptors. Padded
// widths and mixed-width dispatches retain their original native geometry.
gpu_sequence_view native_packed_transport_view(gpu_sequence_view sequence) {
    if (compile_time_native_rank_bits != 0u
        && (compile_time_native_rank_bits & (compile_time_native_rank_bits - 1u)) == 0u) {
        const uint slots = 32u / max(1u, compile_time_native_rank_bits);
        sequence.seq_word_offset += uint64_t(sequence.leading_symbol_offset / slots);
        sequence.leading_symbol_offset %= slots;
        sequence.letters_per_unit = slots;
        sequence.unit_bits = 32u;
    }
    return sequence;
}
// Native storage cursor: divide once, then advance through slots and unit padding.
// Quotient/remainder precede the leading offset so UINT32_MAX positions cannot
// wrap. Logical positions remain 64-bit. Every alignment/prefilter dispatch
// bounds its physical buffers by the uint32 maxStorageBufferRange device limit,
// so admitted byte offsets and cursor updates fit uint32.
struct native_packed_cursor {
    uint unit_byte_index;
    uint slot;
};
native_packed_cursor native_packed_begin(gpu_sequence_view sequence, uint64_t position) {
    uint64_t unit_index;
    uint slot;
    bool binary_geometry = (sequence.letters_per_unit & (sequence.letters_per_unit - 1u)) == 0u;
    if (binary_geometry) {
        unit_index = position >> uint(findLSB(sequence.letters_per_unit));
        slot = uint(position) & (sequence.letters_per_unit - 1u);
    } else if (position <= uint64_t(0xffffffffu)) {
        uint local_position = uint(position);
        uint local_unit_index = binary_geometry
            ? local_position >> uint(findLSB(sequence.letters_per_unit))
            : local_position / sequence.letters_per_unit;
        unit_index = uint64_t(local_unit_index);
        slot = local_position - local_unit_index * sequence.letters_per_unit;
    } else {
        unit_index = binary_geometry
            ? position >> uint(findLSB(sequence.letters_per_unit))
            : position / uint64_t(sequence.letters_per_unit);
        slot = uint(position - unit_index * uint64_t(sequence.letters_per_unit));
    }
    slot += sequence.leading_symbol_offset;
    if (slot >= sequence.letters_per_unit) {
        ++unit_index;
        slot -= sequence.letters_per_unit;
    }
    return native_packed_cursor(
        uint(sequence.seq_word_offset) * 4u
            + uint(unit_index) * (sequence.unit_bits / 8u),
        slot);
}
uint native_packed_rank(gpu_sequence_view sequence, uint64_t position, bool query) {
    sequence = native_packed_transport_view(sequence);
    native_packed_cursor cursor = native_packed_begin(sequence, position);
    uint bit_offset = cursor.slot * native_packed_rank_bits(sequence);
    uint64_t byte_index = cursor.unit_byte_index + uint64_t(bit_offset / 8u);
    uint within_byte = bit_offset & 7u;
    uint rank = packed_sequence_byte(byte_index, query) >> within_byte;
    if (within_byte + native_packed_rank_bits(sequence) > 8u)
        rank |= packed_sequence_byte(byte_index + uint64_t(1), query) << (8u - within_byte);
    return rank & native_packed_rank_mask(sequence);
}
)GLSL";

/** Shared native cursor/chunk reads for shaders with one immutable packed-word arena. */
inline constexpr std::string_view k_native_packed_cursor_reader = R"GLSL(
uint native_packed_arena_word(uint64_t word_index);
// Retain a packed word across adjacent chunks without expanding source storage.
struct native_packed_word_cache {
    uint word_index;
    uint word;
};
uint cached_native_packed_word(uint word_index, inout native_packed_word_cache cache) {
    if (word_index != cache.word_index) {
        cache.word_index = word_index;
        cache.word = native_packed_arena_word(word_index);
    }
    return cache.word;
}
// Read at most 32 active rank bits. Continuous layouts may cross a transport
// word; padded layouts stay inside their native unit. The caller bounds the
// logical interval, and inactive slots and spare unit bits are masked.
uint native_packed_chunk_bits(
    gpu_sequence_view sequence,
    native_packed_cursor cursor,
    uint first_slot,
    uint count,
    inout native_packed_word_cache cache)
{
    uint bit_offset = first_slot * native_packed_rank_bits(sequence);
    uint bit_count = count * native_packed_rank_bits(sequence);
    uint byte_index = cursor.unit_byte_index + (bit_offset >> 3u);
    uint word_index = byte_index >> 2u;
    uint word_shift = (uint(byte_index) & 3u) * 8u + (bit_offset & 7u);
    uint bits = cached_native_packed_word(word_index, cache) >> word_shift;
    if (word_shift + bit_count > 32u)
        bits |= cached_native_packed_word(word_index + 1u, cache) << (32u - word_shift);
    return bits & (0xffffffffu >> (32u - bit_count));
}
// Advance by at most one unit's capacity, allowing a contiguous chunk to cross
// its boundary. Padded layouts still pass only the remaining active slots.
// Callers skip the final advance, so no pointer moves beyond its logical span.
void advance_native_packed_cursor(
    gpu_sequence_view sequence,
    inout native_packed_cursor cursor,
    uint count,
    bool reverse)
{
    const bool continuous_transport = compile_time_native_rank_bits != 0u
        && (compile_time_native_rank_bits & (compile_time_native_rank_bits - 1u)) == 0u;
    if (reverse) {
        if (count <= cursor.slot) {
            cursor.slot -= count;
        } else {
            cursor.unit_byte_index -= sequence.unit_bits / 8u;
            cursor.slot = continuous_transport
                ? cursor.slot + sequence.letters_per_unit - count : sequence.letters_per_unit - 1u;
        }
    } else {
        cursor.slot += count;
        if (cursor.slot >= sequence.letters_per_unit) {
            cursor.slot = continuous_transport ? cursor.slot - sequence.letters_per_unit : 0u;
            cursor.unit_byte_index += sequence.unit_bits / 8u;
        }
    }
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
uint native_packed_arena_word(uint64_t word_index) {
    return packed_words[uint(word_index)];
}
bool prefilter_job_is_in_bounds(prefilter_job job) {
    if (job.query_record_index >= pc.query_record_count || job.target_record_index >= pc.target_record_count)
        return false;
    // validate_prefilter_inputs already checked every immutable job's complete
    // coordinates and orientation before upload. Do not reload both 64-bit
    // sequence descriptors just to repeat those checks in each shader job.
    return true;
}
// Normalize only a bounded register chunk when rank widths or orientation differ.
uint native_packed_chunk_rank_bytes(
    uint packed,
    gpu_sequence_view sequence,
    uint count,
    bool reverse_complement)
{
    uint ranks = 0u;
    for (uint lane = 0u; lane < count; ++lane) {
        uint source_lane = reverse_complement ? count - 1u - lane : lane;
        uint rank = (packed >> (source_lane * native_packed_rank_bits(sequence))) & native_packed_rank_mask(sequence);
        if (reverse_complement) rank = rank_to_rc_code[rank];
        ranks |= rank << (lane * 8u);
    }
    return ranks;
}
// Four logical ranks remain in registers. Reuse each loaded packed word across
// slots, including non-power-of-two letters/unit and cross-word symbols.
// Caller has already proved [position, position + 4) is inside this descriptor.
uint native_packed_rank_quad(gpu_sequence_view sequence, uint64_t position) {
    sequence = native_packed_transport_view(sequence);
    native_packed_cursor cursor = native_packed_begin(sequence, position);
    uint initial_bit = cursor.slot * native_packed_rank_bits(sequence);
    uint64_t word_index =
        (cursor.unit_byte_index + uint64_t(initial_bit >> 3u)) >> 2u;
    native_packed_word_cache cache = native_packed_word_cache(uint(word_index), packed_words[uint(word_index)]);
    uint expanded_ranks = 0u;
    for (uint lane = 0u; lane < 4u; ++lane) {
        uint rank = native_packed_chunk_bits(sequence, cursor, cursor.slot, 1u, cache);
        expanded_ranks |= rank << (lane * 8u);
        if (lane + 1u < 4u) {
            ++cursor.slot;
            if (cursor.slot == sequence.letters_per_unit) {
                cursor.slot = 0u;
                cursor.unit_byte_index += sequence.unit_bits / 8u;
            }
        }
    }
    return expanded_ranks;
}
uint query_rank_unchecked(prefilter_job job, uint rank_index) {
    gpu_sequence_view sequence = query_records[job.query_record_index];
    if (job.orientation == 3u) {
        uint rank = native_packed_rank(sequence, sequence.bp_count - uint64_t(1) - uint64_t(rank_index), true);
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
    gpu_sequence_view sequence = query_records[job.query_record_index];
    if (job.orientation == 3u) {
        uint forward = native_packed_rank_quad(sequence,
            sequence.bp_count - uint64_t(rank_index) - uint64_t(4));
        return rank_to_rc_code[(forward >> 24u) & 255u]
            | (rank_to_rc_code[(forward >> 16u) & 255u] << 8u)
            | (rank_to_rc_code[(forward >> 8u) & 255u] << 16u)
            | (rank_to_rc_code[forward & 255u] << 24u);
    }
    return native_packed_rank_quad(sequence, uint64_t(rank_index));
}
uint target_rank_quad_unchecked(prefilter_job job, uint rank_index) {
    return native_packed_rank_quad(target_records[job.target_record_index], uint64_t(rank_index));
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
