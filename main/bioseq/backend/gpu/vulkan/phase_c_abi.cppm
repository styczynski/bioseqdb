/**
 * @file phase_c_abi.cppm
 * @brief DOC0310 Phase C — primitive POD ABI structs shared by the resident
 *        two-submit Vulkan frontend, the GPU sketch/index-build/seed kernels,
 *        and the device radix sort.
 *
 * Like `phase_b_abi.cppm`, this module sits at the bottom of the Phase C
 * dependency graph: it imports only `bioseq_gpu.vulkan.phase_b_abi` (for
 * `VulkanSketchParams`, `MinimizerWordKind`, `AlphabetKernelKind`) and owns the
 * host-side wire records whose byte layout must stay identical to the GLSL
 * structs the Phase C shaders declare. Every layout that the SPIR-V reflection
 * cares about is pinned with `static_assert` against the field offsets.
 *
 * The records are multi-record forms of the original single-sequence sketch ABI:
 * `GpuSequenceViewWire` describes one logical record inside one packed sequence
 * buffer (32-bit word offset + byte counts + unit traits), and the per-record
 * query/target wires carry the output minimizer/anchor slices the count submit
 * fills. `GpuAnchorWire` deliberately mirrors the 24-byte `bioseq::chaining::Anchor`
 * so the device seed-emit writes straight into the chain-DP input buffer with no
 * transcode (DOC0310 §6).
 */
module;

#include <bit>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.gpu.vulkan.phase_c_abi;

import bioseq.gpu.vulkan.phase_b_abi;

export namespace bioseq::gpu::vulkan {

inline constexpr std::uint32_t k_phase_c_sketch_tile_bp = 256u;
inline constexpr std::uint32_t k_phase_c_sketch_local = 64u;
inline constexpr std::uint32_t k_phase_c_sketch_tile_span_max = 384u;
inline constexpr std::uint32_t k_phase_c_scan_block = 256u;
inline constexpr std::uint32_t k_phase_c_slot_offset_align = 256u;

enum class PhaseCSketchUse : std::uint8_t {
    minimizer_words,
    target_index_build,
    sorted_pairs,
};

/// Host mirror of the GLSL `TileC` record consumed by `phase_c_sketch_tile`.
/// This is distinct from the resident-frontend `SketchTileWire` below.
struct PhaseCSketchTileWire {
    std::uint32_t seq_word_offset = 0;
    std::uint32_t bp_count = 0;
    std::uint32_t replay_lo = 0;
    std::uint32_t tile_lo = 0;
    std::uint32_t tile_hi = 0;
    std::uint32_t out_base = 0;
};
static_assert(sizeof(PhaseCSketchTileWire) == 24);

struct PhaseCSketchTilePlan {
    std::uint32_t tile_bp = 0;
    std::uint32_t tile_count = 0;
    std::uint32_t scan_block_count = 0;
    bool dispatchable = false;
};

struct PhaseCSketchMemoryPlan {
    PhaseCSketchTilePlan tiles{};
    std::uint64_t seq_bytes = 0;
    std::uint64_t rank_lut_bytes = 0;
    std::uint64_t output_word_bytes = 0;
    std::uint64_t pair_key_bytes = 0;
    std::uint64_t pair_value_bytes = 0;
    std::uint64_t tile_bytes = 0;
    std::uint64_t tile_count_bytes = 0;
    std::uint64_t tile_offset_bytes = 0;
    std::uint64_t scan_block_bytes = 0;
    std::uint64_t counter_bytes = 0;
    std::uint64_t arena_pair_value_bytes = 0;
    std::uint64_t arena_scaffold_bytes = 0;
    std::uint64_t standalone_device_bytes = 0;
    std::uint64_t resident_output_bytes = 0;
    std::uint64_t host_transient_bytes = 0;
    std::uint64_t admission_bytes = 0;
    bool valid = false;
};

[[nodiscard]] constexpr std::uint64_t phase_c_saturating_add(
    std::uint64_t left, std::uint64_t right) noexcept
{
    const std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)();
    return left > limit - right ? limit : left + right;
}

[[nodiscard]] constexpr std::uint64_t phase_c_saturating_mul(
    std::uint64_t left, std::uint64_t right) noexcept
{
    if (left == 0u || right == 0u) {
        return 0u;
    }
    const std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)();
    return left > limit / right ? limit : left * right;
}

[[nodiscard]] constexpr std::uint64_t phase_c_align_up(
    std::uint64_t value, std::uint64_t alignment) noexcept
{
    if (alignment <= 1u) {
        return value;
    }
    const std::uint64_t remainder = value % alignment;
    if (remainder == 0u) {
        return value;
    }
    return phase_c_saturating_add(value, alignment - remainder);
}

[[nodiscard]] constexpr std::uint32_t phase_c_ceil_div_u32(
    std::uint64_t value, std::uint32_t divisor) noexcept
{
    return divisor == 0u
        ? 0u
        : static_cast<std::uint32_t>((value + divisor - 1u) / divisor);
}

[[nodiscard]] constexpr std::uint32_t phase_c_replay_overlap_bp(
    std::uint32_t k,
    std::uint32_t window) noexcept
{
    const std::uint64_t trigger_span =
        phase_c_saturating_add(
            phase_c_saturating_add(k, window),
            1u);
    constexpr std::uint32_t max_replay =
        k_phase_c_sketch_tile_span_max > 1u
            ? k_phase_c_sketch_tile_span_max - 1u
            : 1u;
    return trigger_span > max_replay
        ? max_replay
        : static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, trigger_span));
}

[[nodiscard]] constexpr std::uint32_t phase_c_ownership_guard_bp(
    std::uint32_t k,
    std::uint32_t window,
    std::uint32_t letters_per_unit) noexcept
{
    (void)k;
    (void)window;
    (void)letters_per_unit;
    return 0u;
}

[[nodiscard]] constexpr std::uint32_t phase_c_default_tile_bp(
    std::uint32_t k,
    std::uint32_t window,
    std::uint32_t letters_per_unit) noexcept
{
    (void)k;
    (void)window;
    (void)letters_per_unit;
    return k_phase_c_sketch_tile_bp;
}

[[nodiscard]] constexpr std::uint32_t phase_c_adaptive_tile_bp(
    std::uint32_t bp,
    std::uint32_t max_workgroups,
    std::uint32_t default_tile_bp) noexcept
{
    std::uint32_t tile_bp = std::max<std::uint32_t>(default_tile_bp, 1u);
    const std::uint32_t cap = max_workgroups > 1u ? max_workgroups - 1u : 1u;
    if (phase_c_ceil_div_u32(bp, tile_bp) > cap) {
        tile_bp = phase_c_ceil_div_u32(bp, cap);
    }
    constexpr std::uint32_t span_cap =
        k_phase_c_sketch_tile_span_max > 64u
            ? k_phase_c_sketch_tile_span_max - 64u
            : 1u;
    if (tile_bp > span_cap) {
        tile_bp = span_cap;
    }
    return std::max<std::uint32_t>(tile_bp, 1u);
}

[[nodiscard]] constexpr std::uint32_t phase_c_storage_aligned_tile_bp(
    std::uint32_t bp,
    std::uint32_t max_workgroups,
    std::uint32_t default_tile_bp,
    std::uint32_t letters_per_unit,
    std::uint32_t context_bp) noexcept
{
    std::uint32_t tile_bp =
        phase_c_adaptive_tile_bp(bp, max_workgroups, default_tile_bp);
    const std::uint32_t span_cap =
        k_phase_c_sketch_tile_span_max > context_bp
            ? k_phase_c_sketch_tile_span_max - context_bp
            : 1u;
    tile_bp = std::min(tile_bp, span_cap);

    const std::uint32_t lpu = std::max<std::uint32_t>(letters_per_unit, 1u);
    if (lpu <= 1u || tile_bp < lpu) {
        return tile_bp;
    }

    const std::uint32_t aligned = (tile_bp / lpu) * lpu;
    if (aligned == 0u) {
        return tile_bp;
    }
    const std::uint32_t cap = max_workgroups > 1u ? max_workgroups - 1u : 1u;
    return phase_c_ceil_div_u32(bp, aligned) <= cap ? aligned : tile_bp;
}

[[nodiscard]] constexpr PhaseCSketchTilePlan phase_c_tile_plan(
    std::uint64_t bp_count,
    std::uint32_t k,
    std::uint32_t window,
    std::uint32_t letters_per_unit,
    std::uint32_t max_workgroups =
        (std::numeric_limits<std::uint32_t>::max)()) noexcept
{
    if (bp_count == 0u || bp_count > (std::numeric_limits<std::uint32_t>::max)()
        || k == 0u || window == 0u || max_workgroups == 0u) {
        return {};
    }
    const std::uint32_t bp = static_cast<std::uint32_t>(bp_count);
    const std::uint32_t overlap = phase_c_replay_overlap_bp(k, window);
    const std::uint32_t ownership_guard =
        phase_c_ownership_guard_bp(k, window, letters_per_unit);
    const std::uint32_t context_span = phase_c_saturating_add(
        overlap, ownership_guard)
        > (std::numeric_limits<std::uint32_t>::max)()
            ? (std::numeric_limits<std::uint32_t>::max)()
            : overlap + ownership_guard;
    const std::uint32_t default_tile_bp =
        phase_c_default_tile_bp(k, window, letters_per_unit);
    const std::uint32_t tile_bp = phase_c_storage_aligned_tile_bp(
        bp,
        max_workgroups,
        default_tile_bp,
        letters_per_unit,
        context_span);
    const std::uint32_t tile_count = phase_c_ceil_div_u32(bp, tile_bp);
    const bool dispatchable =
        tile_bp != 0u
        && tile_count != 0u
        && tile_count <= max_workgroups
        && phase_c_saturating_add(tile_bp, context_span)
            <= k_phase_c_sketch_tile_span_max;
    return PhaseCSketchTilePlan{
        .tile_bp = tile_bp,
        .tile_count = tile_count,
        .scan_block_count = phase_c_ceil_div_u32(tile_count, k_phase_c_scan_block),
        .dispatchable = dispatchable,
    };
}

[[nodiscard]] constexpr PhaseCSketchMemoryPlan phase_c_sketch_memory_plan(
    std::uint64_t bp_count,
    std::uint64_t sequence_bytes,
    std::uint64_t rank_lut_bytes,
    std::uint64_t output_capacity,
    std::uint64_t minimizer_output_bytes,
    PhaseCSketchUse use,
    std::uint32_t k,
    std::uint32_t window,
    std::uint32_t letters_per_unit,
    std::uint32_t max_workgroups =
        (std::numeric_limits<std::uint32_t>::max)()) noexcept
{
    PhaseCSketchMemoryPlan plan{};
    plan.tiles = phase_c_tile_plan(
        bp_count, k, window, letters_per_unit, max_workgroups);
    if (!plan.tiles.dispatchable) {
        return plan;
    }

    plan.seq_bytes = std::max<std::uint64_t>(sequence_bytes, 4u);
    plan.rank_lut_bytes = rank_lut_bytes;
    plan.tile_bytes = phase_c_saturating_mul(
        plan.tiles.tile_count, sizeof(PhaseCSketchTileWire));
    plan.tile_count_bytes = phase_c_saturating_mul(plan.tiles.tile_count, 4u);
    plan.tile_offset_bytes = phase_c_saturating_mul(plan.tiles.tile_count, 4u);
    plan.scan_block_bytes = phase_c_saturating_mul(
        std::max<std::uint32_t>(plan.tiles.scan_block_count, 1u), 4u);
    plan.counter_bytes =
        use == PhaseCSketchUse::minimizer_words ? 8u : 16u;
    plan.arena_pair_value_bytes =
        use == PhaseCSketchUse::minimizer_words ? 16u : 0u;

    if (use == PhaseCSketchUse::minimizer_words) {
        plan.output_word_bytes = std::max<std::uint64_t>(minimizer_output_bytes, 4u);
    } else {
        plan.pair_key_bytes = phase_c_saturating_mul(output_capacity, 8u);
        plan.pair_value_bytes = phase_c_saturating_mul(output_capacity, 8u);
    }

    const auto add_aligned = [](std::uint64_t total, std::uint64_t bytes) {
        return phase_c_saturating_add(
            total,
            phase_c_align_up(bytes, k_phase_c_slot_offset_align));
    };
    std::uint64_t arena = 0;
    arena = add_aligned(arena, plan.seq_bytes);
    arena = add_aligned(arena, plan.rank_lut_bytes);
    arena = add_aligned(arena, plan.tile_bytes);
    arena = add_aligned(arena, plan.tile_count_bytes);
    arena = add_aligned(arena, plan.tile_offset_bytes);
    arena = add_aligned(arena, plan.scan_block_bytes);
    arena = add_aligned(arena, plan.counter_bytes);
    arena = add_aligned(arena, plan.arena_pair_value_bytes);
    plan.arena_scaffold_bytes = arena;

    plan.standalone_device_bytes = phase_c_saturating_add(
        plan.output_word_bytes,
        phase_c_saturating_add(plan.pair_key_bytes, plan.pair_value_bytes));
    plan.resident_output_bytes = plan.standalone_device_bytes;
    plan.host_transient_bytes = plan.seq_bytes;
    plan.admission_bytes = phase_c_saturating_add(
        plan.arena_scaffold_bytes,
        phase_c_saturating_add(
            plan.standalone_device_bytes, plan.host_transient_bytes));

    const std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)();
    plan.valid = plan.arena_scaffold_bytes != limit
        && plan.standalone_device_bytes != limit
        && plan.admission_bytes != limit;
    return plan;
}

/// Per-record routing flags. `reverse_complement` is reserved for a future
/// strand-split; `target_record`/`query_record` tag which wire array a record
/// came from when the same packed buffer holds both.
enum class PhaseCRecordFlags : std::uint32_t {
    none = 0u,
    reverse_complement = 1u << 0u,
    target_record = 1u << 1u,
    query_record = 1u << 2u,
};

[[nodiscard]] constexpr PhaseCRecordFlags operator|(
    PhaseCRecordFlags a, PhaseCRecordFlags b) noexcept {
    return static_cast<PhaseCRecordFlags>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

/// Multi-record sequence view: a window onto one logical record inside the
/// shared packed-sequence arena buffer. `seq_word_offset` is the 32-bit-word
/// offset of this record's first storage unit; the shader reads ranks through
/// the shared `read_rank_at_bp(... seq_word_offset ...)` helper using
/// `bits_per_char`/`letters_per_unit`/`unit_bits` — it never assumes dna4.
struct GpuSequenceViewWire {
    std::uint64_t seq_word_offset;   ///< uint32 word offset in the packed buffer.
    std::uint64_t byte_count;        ///< visible bytes before round-up padding.
    std::uint64_t padded_byte_count; ///< byte_count rounded to a 4-byte word.
    std::uint64_t unit_count;        ///< number of compact_sequence_range units.
    std::uint64_t bp_offset;         ///< base-pair offset of the logical record.
    std::uint64_t bp_count;          ///< logical record length in base pairs.
    std::uint32_t letters_per_unit;  ///< traits::letters_per_unit.
    std::uint32_t bits_per_char;     ///< traits::bits_per_char.
    std::uint32_t unit_bits;         ///< traits::unit_width.
    std::uint32_t alphabet_size;     ///< traits::alphabet_size.
    std::uint32_t char_mask;         ///< traits::char_mask truncated to 32 bits.
    std::uint32_t padding_letters;   ///< sequence padding().
    std::uint32_t alphabet_kind;     ///< AlphabetKernelKind from the upload view.
    std::uint32_t padding;
};

/// One query record: its sequence view plus the exact device slices the count
/// submit fills (minimizer words, anchors, chain ranges).
struct GpuQueryRecordWire {
    GpuSequenceViewWire seq;
    std::uint64_t minimizer_offset;  ///< output minimizer-word offset (in records).
    std::uint64_t minimizer_count;   ///< filled by count submit.
    std::uint64_t anchor_offset;     ///< output anchor offset.
    std::uint64_t anchor_count;      ///< filled by count submit.
    std::uint64_t chain_range_offset;
    std::uint64_t chain_range_count;
    std::uint32_t record_id;
    std::uint32_t flags;
};

/// One target record: its sequence view plus its minimizer-pair slice.
struct GpuTargetRecordWire {
    GpuSequenceViewWire seq;
    std::uint64_t target_global_bp_offset;
    std::uint64_t target_id;
    std::uint64_t minimizer_pair_offset;
    std::uint64_t minimizer_pair_count;
    std::uint32_t record_id;
    std::uint32_t flags;
};

/// Tile-local window minimum produced by the ordered sketch tile kernel.
struct WindowMinWire {
    std::uint64_t hash;
    std::uint64_t pos_strand_encoded;
    std::uint32_t span;
    std::uint32_t valid;
};

/// Descriptor for one sketch tile (one workgroup). Owns output windows
/// `[first_window, first_window + window_count)`; the shader may read an
/// overlap of `w + k - 2` bases on either side but never emits outside its
/// owned interval (DOC0310 §9 tile boundary rule).
struct SketchTileWire {
    std::uint64_t record_id;
    std::uint64_t first_window;
    std::uint64_t window_count;
    std::uint64_t carry_in_offset;
    std::uint64_t carry_out_offset;
    std::uint64_t flag_offset;
};

/// Carry state that crosses a tile boundary: the current window minimum, its
/// window position, the valid-base counter, the rolling forward/reverse k-mer
/// states, and the last emitted identity.
struct SketchCarryWire {
    WindowMinWire current_min;
    std::uint64_t current_min_window_pos;
    std::uint64_t valid_base_count;
    std::uint64_t rolling_forward;
    std::uint64_t rolling_reverse;
    std::uint64_t last_emitted_identity;
    std::uint32_t initialized;
    std::uint32_t reserved;
};

/// Per-record sketch count downloaded after the count submit.
struct SketchCountWire {
    std::uint64_t minimizer_count;
    std::uint64_t overflow_count;
};

/// One target minimizer pair in the hot 16-byte layout: the 64-bit minimizer
/// hash and `pos_strand_encoded = (last_pos << 1) | strand`. Do not widen to
/// 24 B for the XXL_HIFI path (DOC0310 §6).
struct GpuMinimizerPairWire {
    std::uint64_t hash;
    std::uint64_t pos_strand_encoded;
};

/// Device anchor — byte-identical to the 24-byte `bioseq::chaining::Anchor`
/// `{ref_position, query_position, is_reverse, query_span, flags, segment_id}`.
struct GpuAnchorWire {
    std::uint64_t ref_position;
    std::uint64_t query_position;
    std::uint32_t is_reverse;
    std::uint8_t query_span;
    std::uint8_t flags;
    std::uint16_t segment_id;
};

/// One chain range `{begin, length}`. In GLSL the second field stays named
/// `range_len` to avoid colliding with the `.length()` builtin, but the 8-byte
/// ABI is `{uint32 begin, uint32 length}`.
struct GpuChainRangeWire {
    std::uint32_t begin;
    std::uint32_t length;
};

/// Aggregate counters downloaded after the count submit.
struct GpuPhaseCCounters {
    std::uint64_t query_minimizers;
    std::uint64_t target_minimizers;
    std::uint64_t seed_hits;
    std::uint64_t anchors;
    std::uint64_t chain_ranges;
    std::uint64_t sketch_overflows;
    std::uint64_t seed_overflows;
    std::uint64_t reserved;
};

/// Push-constant/param block for the Phase C kernels. Carries the shared
/// `VulkanSketchParams` plus the Phase C-specific limits and runtime mode toggles
/// (DOC0310 §6, §23 — tile/radix/build-mode are runtime fields, never template
/// axes).
struct VulkanPhaseCParams {
    VulkanSketchParams sketch;
    std::uint32_t max_occ;
    std::uint32_t max_chain_iter;
    std::uint32_t max_ref_gap;
    std::uint32_t max_query_gap;
    std::uint32_t bandwidth;
    std::uint32_t tile_bp;
    std::uint32_t sketch_workgroup_size;
    std::uint32_t seed_workgroup_size;
    std::uint32_t sort_radix_bits;
    std::uint32_t enable_target_index_build;
    std::uint32_t enable_chain_dp;
    std::uint32_t reserved0;
};

static_assert(std::is_standard_layout_v<GpuSequenceViewWire>);
static_assert(std::is_standard_layout_v<GpuQueryRecordWire>);
static_assert(std::is_standard_layout_v<GpuTargetRecordWire>);
static_assert(std::is_standard_layout_v<GpuMinimizerPairWire>);
static_assert(std::is_standard_layout_v<GpuAnchorWire>);
static_assert(sizeof(GpuMinimizerPairWire) == 16);
static_assert(sizeof(GpuAnchorWire) == 24);
static_assert(offsetof(GpuAnchorWire, ref_position) == 0);
static_assert(offsetof(GpuAnchorWire, query_position) == 8);
static_assert(offsetof(GpuAnchorWire, is_reverse) == 16);
static_assert(offsetof(GpuAnchorWire, query_span) == 20);
static_assert(offsetof(GpuAnchorWire, flags) == 21);
static_assert(offsetof(GpuAnchorWire, segment_id) == 22);
static_assert(sizeof(GpuChainRangeWire) == 8);

} // namespace bioseq::gpu::vulkan
