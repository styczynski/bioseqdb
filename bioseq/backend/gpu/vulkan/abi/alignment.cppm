/**
 * @file
 * @brief Defines Vulkan chaining, coordinate, extension, and CIGAR ABI records.
 */
module;

#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.gpu.vulkan.abi:alignment;

import bioseq.common.checked_arithmetic;

export namespace bioseq::gpu::vulkan {

/** @brief Maximum sequence length handled by the extension kernel. */
inline constexpr std::uint32_t k_gpu_alignment_extension_max_len = 512u;
/** @brief Sequence length represented by one CIGAR traceback tile. */
inline constexpr std::uint32_t k_gpu_alignment_cigar_traceback_tile_len = 128u;
/** @brief Maximum sequence length handled by the simple CIGAR kernel. */
inline constexpr std::uint32_t k_gpu_alignment_simple_cigar_max_len = 16'384u;
/** @brief Maximum gap length represented by the simple CIGAR kernel. */
inline constexpr std::uint32_t k_gpu_alignment_simple_cigar_gap_max = 256u;
/** @brief Maximum simple-CIGAR gap operations emitted by one alignment. */
inline constexpr std::uint32_t k_gpu_alignment_simple_cigar_gap_op_max = 64u;
/** @brief Maximum band width admitted by the extension kernel. */
inline constexpr std::uint32_t k_gpu_alignment_extension_max_band_width = 512u;
/** @brief Shader status indicating successful alignment extension. */
inline constexpr std::uint32_t k_gpu_alignment_extension_status_ok = 0u;
/** @brief Shader status indicating unsupported alignment geometry. */
inline constexpr std::uint32_t
    k_gpu_alignment_extension_status_unsupported_geometry = 1u;
/** @brief Shader status indicating insufficient CIGAR output capacity. */
inline constexpr std::uint32_t
    k_gpu_alignment_extension_status_cigar_overflow = 2u;
/**
 * @brief Computes traceback words for a gridded alignment job.
 * @return Required boundary and scratch words, or zero for invalid geometry.
 */
[[nodiscard]] inline constexpr std::uint64_t
gpu_alignment_cigar_gridded_traceback_words(
    const std::uint32_t query_len,
    const std::uint32_t target_len,
    const std::uint32_t tile_query,
    const std::uint32_t tile_target) noexcept
{
    if (query_len == 0 || target_len == 0 || tile_query == 0 || tile_target == 0) {
        return 0;
    }
    const std::uint64_t query_tiles =
        bioseq::common::ceil_div(query_len, tile_query);
    const std::uint64_t target_tiles =
        bioseq::common::ceil_div(target_len, tile_target);
    const std::uint64_t boundary_words_per_tile =
        5u * (static_cast<std::uint64_t>(tile_query) + 1u
              + static_cast<std::uint64_t>(tile_target) + 1u);
    const std::uint64_t scratch_words =
        (static_cast<std::uint64_t>(tile_query) + 1u)
        * (static_cast<std::uint64_t>(tile_target) + 1u);
    return query_tiles * target_tiles * boundary_words_per_tile + scratch_words;
}
/** @brief Mirrors the byte-identical 24-byte chaining anchor on the device. */
struct gpu_anchor_wire {
    std::uint64_t ref_position;
    std::uint64_t query_position;
    std::uint32_t is_reverse;
    std::uint8_t query_span;
    std::uint8_t flags;
    std::uint16_t segment_id;
};

/** @brief Stores one 8-byte chain range as `{begin, length}`. */
struct gpu_chain_range_wire {
    std::uint32_t begin;
    std::uint32_t length;
};

/** @brief Stores one query span consumed by the device hard-range builder. */
struct gpu_chain_range_build_span_wire {
    std::uint32_t anchor_lo;
    std::uint32_t anchor_hi;
    std::uint32_t range_lo;
    std::uint32_t range_out;
};

/** @brief Stores a compact coordinate-only seed-and-chain result. */
struct gpu_coordinate_result_wire {
    std::uint64_t query_start;
    std::uint64_t query_end;
    std::uint64_t target_start;
    std::uint64_t target_end;
    std::uint64_t match_length;
    std::uint64_t block_length;
    std::int32_t chain_score;
    std::int32_t seed_count;
    std::uint32_t is_reverse;
    std::uint32_t is_valid;
};

/** @brief Carries coordinate-only chain-reduction parameters. */
struct gpu_coordinate_reduce_push {
    std::uint32_t anchor_count;
    std::uint32_t query_len_lo;
    std::uint32_t query_len_hi;
    std::int32_t min_chain_score;
    std::int32_t min_seed_count;
    std::uint32_t probe_limit;
    std::uint32_t reserved;
};

/**
 * @brief Describes one score-and-endpoint-only affine extension job.
 *
 * Offsets address caller-normalized alphabet-rank streams.
 */
struct gpu_alignment_extension_job_wire {
    std::uint32_t query_offset;
    std::uint32_t target_offset;
    std::uint32_t query_len;
    std::uint32_t target_len;
    std::uint32_t band_width;
    std::uint32_t flags;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
};

/** @brief Stores the score and best endpoint of one extension job. */
struct gpu_alignment_extension_result_wire {
    std::int32_t score;
    std::uint32_t query_end;
    std::uint32_t target_end;
    std::uint32_t status;
};

/** @brief Defines geometry and scoring limits for an extension batch. */
struct gpu_alignment_extension_params {
    std::uint32_t max_query_len = 0;
    std::uint32_t max_target_len = 0;
    std::uint32_t max_band_width = 0;
    std::uint32_t alphabet_size = 0;
    std::int32_t match_score = 0;
    std::int32_t mismatch_penalty = 0;
    std::int32_t gap_open_penalty = 0;
    std::int32_t gap_extend_penalty = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved0 = 0;
};

/** @brief Describes one CIGAR-capable extension and traceback job. */
struct gpu_alignment_cigar_job_wire {
    std::uint32_t query_symbol_offset = 0;
    std::uint32_t target_symbol_offset = 0;
    std::uint32_t query_len = 0;
    std::uint32_t target_len = 0;
    std::uint32_t query_global_start = 0;
    std::uint32_t target_global_start = 0;
    std::uint32_t band_width = 0;
    std::uint32_t flags = 0;
    std::uint32_t cigar_offset = 0;
    std::uint32_t cigar_capacity = 0;
    std::uint32_t traceback_tile_offset = 0;
    std::uint32_t traceback_tile_count = 0;
};

/** @brief Stores score, coordinate, status, and CIGAR output for one job. */
struct gpu_alignment_cigar_result_wire {
    std::int32_t score = 0;
    std::int32_t max_score = 0;
    std::uint32_t query_start = 0;
    std::uint32_t query_end = 0;
    std::uint32_t target_start = 0;
    std::uint32_t target_end = 0;
    std::uint32_t cigar_offset = 0;
    std::uint32_t cigar_length = 0;
    std::uint32_t status = 0;
    std::uint32_t reserved = 0;
};

/** @brief Defines scoring and traceback geometry for CIGAR alignment. */
struct gpu_alignment_cigar_params {
    std::uint32_t alphabet_size = 0;
    std::uint32_t max_band_width = 0;
    std::uint32_t tile_query = 0;
    std::uint32_t tile_target = 0;
    std::int32_t match_score = 0;
    std::int32_t mismatch_penalty = 0;
    std::int32_t gap_open_short = 0;
    std::int32_t gap_extend_short = 0;
    std::int32_t gap_open_long = 0;
    std::int32_t gap_extend_long = 0;
    std::int32_t z_drop = 0;
    std::int32_t end_bonus = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};
static_assert(std::is_standard_layout_v<gpu_anchor_wire>);
static_assert(sizeof(gpu_anchor_wire) == 24);
static_assert(offsetof(gpu_anchor_wire, ref_position) == 0);
static_assert(offsetof(gpu_anchor_wire, query_position) == 8);
static_assert(offsetof(gpu_anchor_wire, is_reverse) == 16);
static_assert(offsetof(gpu_anchor_wire, query_span) == 20);
static_assert(offsetof(gpu_anchor_wire, flags) == 21);
static_assert(offsetof(gpu_anchor_wire, segment_id) == 22);
static_assert(sizeof(gpu_chain_range_wire) == 8);
static_assert(std::is_standard_layout_v<gpu_chain_range_build_span_wire>);
static_assert(sizeof(gpu_chain_range_build_span_wire) == 16);
static_assert(std::is_standard_layout_v<gpu_coordinate_result_wire>);
static_assert(sizeof(gpu_coordinate_result_wire) == 64);
static_assert(offsetof(gpu_coordinate_result_wire, query_start) == 0);
static_assert(offsetof(gpu_coordinate_result_wire, target_start) == 16);
static_assert(offsetof(gpu_coordinate_result_wire, chain_score) == 48);
static_assert(offsetof(gpu_coordinate_result_wire, is_valid) == 60);
static_assert(std::is_standard_layout_v<gpu_coordinate_reduce_push>);
static_assert(sizeof(gpu_coordinate_reduce_push) == 28);
static_assert(std::is_standard_layout_v<gpu_alignment_extension_job_wire>);
static_assert(sizeof(gpu_alignment_extension_job_wire) == 32);
static_assert(offsetof(gpu_alignment_extension_job_wire, query_offset) == 0);
static_assert(offsetof(gpu_alignment_extension_job_wire, target_offset) == 4);
static_assert(offsetof(gpu_alignment_extension_job_wire, query_len) == 8);
static_assert(offsetof(gpu_alignment_extension_job_wire, target_len) == 12);
static_assert(offsetof(gpu_alignment_extension_job_wire, band_width) == 16);
static_assert(offsetof(gpu_alignment_extension_job_wire, flags) == 20);
static_assert(offsetof(gpu_alignment_extension_job_wire, reserved0) == 24);
static_assert(offsetof(gpu_alignment_extension_job_wire, reserved1) == 28);
static_assert(std::is_standard_layout_v<gpu_alignment_extension_result_wire>);
static_assert(sizeof(gpu_alignment_extension_result_wire) == 16);
static_assert(offsetof(gpu_alignment_extension_result_wire, score) == 0);
static_assert(offsetof(gpu_alignment_extension_result_wire, query_end) == 4);
static_assert(offsetof(gpu_alignment_extension_result_wire, target_end) == 8);
static_assert(offsetof(gpu_alignment_extension_result_wire, status) == 12);
static_assert(std::is_standard_layout_v<gpu_alignment_extension_params>);
static_assert(sizeof(gpu_alignment_extension_params) == 40);
static_assert(std::is_standard_layout_v<gpu_alignment_cigar_job_wire>);
static_assert(sizeof(gpu_alignment_cigar_job_wire) == 48);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_symbol_offset) == 0);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_symbol_offset) == 4);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_len) == 8);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_len) == 12);
static_assert(offsetof(gpu_alignment_cigar_job_wire, query_global_start) == 16);
static_assert(offsetof(gpu_alignment_cigar_job_wire, target_global_start) == 20);
static_assert(offsetof(gpu_alignment_cigar_job_wire, band_width) == 24);
static_assert(offsetof(gpu_alignment_cigar_job_wire, flags) == 28);
static_assert(offsetof(gpu_alignment_cigar_job_wire, cigar_offset) == 32);
static_assert(offsetof(gpu_alignment_cigar_job_wire, cigar_capacity) == 36);
static_assert(offsetof(gpu_alignment_cigar_job_wire, traceback_tile_offset) == 40);
static_assert(offsetof(gpu_alignment_cigar_job_wire, traceback_tile_count) == 44);
static_assert(std::is_standard_layout_v<gpu_alignment_cigar_result_wire>);
static_assert(sizeof(gpu_alignment_cigar_result_wire) == 40);
static_assert(offsetof(gpu_alignment_cigar_result_wire, score) == 0);
static_assert(offsetof(gpu_alignment_cigar_result_wire, max_score) == 4);
static_assert(offsetof(gpu_alignment_cigar_result_wire, query_start) == 8);
static_assert(offsetof(gpu_alignment_cigar_result_wire, query_end) == 12);
static_assert(offsetof(gpu_alignment_cigar_result_wire, target_start) == 16);
static_assert(offsetof(gpu_alignment_cigar_result_wire, target_end) == 20);
static_assert(offsetof(gpu_alignment_cigar_result_wire, cigar_offset) == 24);
static_assert(offsetof(gpu_alignment_cigar_result_wire, cigar_length) == 28);
static_assert(offsetof(gpu_alignment_cigar_result_wire, status) == 32);
static_assert(offsetof(gpu_alignment_cigar_result_wire, reserved) == 36);
static_assert(std::is_standard_layout_v<gpu_alignment_cigar_params>);
static_assert(sizeof(gpu_alignment_cigar_params) == 56);

} // namespace bioseq::gpu::vulkan
