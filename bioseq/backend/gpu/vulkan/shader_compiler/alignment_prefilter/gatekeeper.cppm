/**
 * @file gatekeeper.cppm
 * @brief Defines the GateKeeper rank-prefilter GLSL body.
 */
export module bioseq.gpu.vulkan.shader_compiler:alignment_prefilter_gatekeeper;

import std;

export namespace bioseq::gpu::vulkan {

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

} // namespace bioseq::gpu::vulkan
