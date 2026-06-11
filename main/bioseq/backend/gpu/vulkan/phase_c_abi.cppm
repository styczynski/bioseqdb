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
 * The records are multi-record forms of the Phase B single-sequence sketch:
 * `GpuSequenceViewWire` describes one logical record inside one packed sequence
 * buffer (32-bit word offset + byte counts + unit traits), and the per-record
 * query/target wires carry the output minimizer/anchor slices the count submit
 * fills. `GpuAnchorWire` deliberately mirrors the 24-byte `bioseq::chaining::Anchor`
 * so the device seed-emit writes straight into the chain-DP input buffer with no
 * transcode (DOC0310 §6).
 */
module;

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq_gpu.vulkan.phase_c_abi;

import bioseq_gpu.vulkan.phase_b_abi;

export namespace bioseq::gpu::vulkan {

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
/// the existing Phase B `read_rank_at_bp(... seq_word_offset ...)` helper using
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

/// Push-constant/param block for the Phase C kernels. Carries the Phase B
/// sketch params plus the Phase C-specific limits and the runtime mode toggles
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
