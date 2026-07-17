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
 * transcode (DOC0310 §6). The inactive grouped32 ABI2 records additionally pin
 * direct consumption of format-4 12-byte controls and 4-byte segment-local
 * positions; they expose no production route or constructible execution proof.
 */
module;

#include <bit>
#include <algorithm>
#include <array>
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
// Conservative VkDeviceMemory allocation quantum used by the pure peak plan.
// The runtime still verifies the allocator-reported byte total before dispatch;
// this bound makes the admission plan account for per-buffer padding rather
// than assuming requested range == allocation size.
inline constexpr std::uint32_t k_phase_c_peak_allocation_align = 64u * 1024u;
inline constexpr std::uint32_t k_gpu_alignment_extension_max_len = 512u;
inline constexpr std::uint32_t k_gpu_alignment_cigar_traceback_tile_len = 128u;
inline constexpr std::uint32_t k_gpu_alignment_simple_cigar_max_len = 16'384u;
inline constexpr std::uint32_t k_gpu_alignment_simple_cigar_gap_max = 256u;
inline constexpr std::uint32_t k_gpu_alignment_simple_cigar_gap_op_max = 64u;
inline constexpr std::uint32_t k_gpu_alignment_extension_max_band_width = 512u;
inline constexpr std::uint32_t k_gpu_alignment_extension_status_ok = 0u;
inline constexpr std::uint32_t k_gpu_alignment_extension_status_unsupported_geometry = 1u;
inline constexpr std::uint32_t k_gpu_alignment_extension_status_cigar_overflow = 2u;
inline constexpr std::uint32_t k_gpu_sneaky_snake_submaze_length = 512u;
inline constexpr std::uint32_t k_gpu_sneaky_snake_max_edits = 31u;
inline constexpr std::uint32_t k_gpu_sneaky_snake_status_ok = 0u;
inline constexpr std::uint32_t
    k_gpu_sneaky_snake_status_length_difference_only = 1u;
inline constexpr std::uint32_t k_gpu_sneaky_snake_status_unsupported = 2u;
// DOC0371 persisted-index foundation. The directory is uploaded exactly as the
// header-free revision-3 96-byte little-endian entry stream; the shader reads
// these word offsets directly and returns only stable directory ordinals.
inline constexpr std::uint32_t k_gpu_persisted_directory_entry_bytes = 96u;
inline constexpr std::uint32_t k_gpu_persisted_directory_entry_words = 24u;

// Inactive grouped-hash32/segment-local32 GPU ABI2 foundation. These values
// deliberately repeat the immutable format-4 widths at the Vulkan ABI boundary:
// controls are consumed in-place as three little-endian u32 words and positions
// remain one little-endian `(local_bp << 1) | strand` u32. No host-side decode,
// pair rebuild, offset rebasing, or position globalization is part of this ABI.
inline constexpr std::uint32_t k_gpu_grouped32_abi_version = 2u;
inline constexpr std::uint32_t k_gpu_grouped32_control_bytes = 12u;
inline constexpr std::uint32_t k_gpu_grouped32_position_bytes = 4u;
inline constexpr std::uint32_t k_gpu_grouped32_max_local_bp = 0x7fffffffu;

// Persisted Format-5 is the shape-aware GPU ABI3.  Controls stay in their
// authenticated variable-width SoA byte stream and positions stay encoded as
// segment-local `(bp << 1) | strand` scalars.  A resident loader only rebases
// the four array byte offsets below after concatenating extents; it must not
// decode controls, widen positions, or rebuild a hash table on the CPU.
inline constexpr std::uint32_t k_gpu_shape_v5_abi_version = 3u;
inline constexpr std::uint32_t k_gpu_shape_v5_partition_raw_soa = 1u << 0u;
inline constexpr std::uint32_t
    k_gpu_shape_v5_partition_segment_local = 1u << 1u;
inline constexpr std::uint32_t
    k_gpu_shape_v5_partition_terminal_prefix = 1u << 2u;
inline constexpr std::uint32_t k_gpu_shape_v5_partition_required_flags =
    k_gpu_shape_v5_partition_raw_soa
    | k_gpu_shape_v5_partition_segment_local
    | k_gpu_shape_v5_partition_terminal_prefix;

/// Fail-closed status bits written by the direct Format-5 count/emit shaders.
/// The status word lives at `counts[status_index]`; scan only receives the
/// preceding query-local count range, so it cannot overwrite the evidence.
enum class GpuShapeV5SeedStatus : std::uint32_t {
    none = 0u,
    contract = 1u << 0u,
    query = 1u << 1u,
    control = 1u << 2u,
    position = 1u << 3u,
    emission = 1u << 4u,
};

[[nodiscard]] constexpr std::uint32_t gpu_shape_v5_seed_status_bit(
    GpuShapeV5SeedStatus status) noexcept
{
    return static_cast<std::uint32_t>(status);
}

/// Device descriptor for one admitted Format-5 directory entry.  All byte
/// offsets are absolute in the resident aggregate control/position buffers.
/// Widths are the persisted scalar widths and must be exactly 2, 4, or 8.
/// Three control offsets identify the SoA hash, terminal-prefix, and
/// saturated-count arrays; the fourth offset addresses the raw position
/// extent, all without changing their persisted representation.
struct GpuShapeV5ResidentPartitionDescriptor {
    std::uint32_t partition_id = 0;
    std::uint32_t directory_ordinal = 0;
    std::uint32_t position_segment = 0;
    std::uint32_t flags = 0;
    std::uint64_t hash_byte_offset = 0;
    std::uint64_t prefix_byte_offset = 0;
    std::uint64_t count_byte_offset = 0;
    std::uint64_t position_byte_offset = 0;
    std::uint32_t control_count = 0;
    std::uint32_t position_count = 0;
    std::uint32_t hash_width = 0;
    std::uint32_t offset_width = 0;
    std::uint32_t count_width = 0;
    std::uint32_t position_width = 0;
    std::uint64_t low_hash = 0;
    std::uint64_t high_hash = 0;
    std::uint64_t segment_base_bp = 0;
    std::uint64_t segment_end_bp = 0;
};

/// Push ABI for direct count/emit from resident Format-5 bytes.  The persisted
/// cap is carried independently of the query's `max_occ`: count/emit is legal
/// only when the query threshold is no larger than the cap under which the
/// on-disk saturated-count array was encoded.
struct GpuShapeV5SeedPush {
    std::uint32_t abi_version = k_gpu_shape_v5_abi_version;
    std::uint32_t words32_per_minimizer = 0;
    std::uint32_t word_kind = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t kmer_size = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t max_occ = 0;
    std::uint32_t emit_mode = 0;
    std::uint32_t record_count = 0;
    std::uint32_t target_ref_bits = 0;
    std::uint32_t query_minimizer_count = 0;
    std::uint32_t control_word_count = 0;
    std::uint32_t position_word_count = 0;
    std::uint32_t anchor_capacity = 0;
    std::uint32_t max_occ_cap_lo = 0;
    std::uint32_t max_occ_cap_hi = 0;
    // Query-local counts occupy [0, status_index); this final N+1 word is
    // reserved for fail-closed shader status and is never part of prefix scan.
    std::uint32_t status_index = 0;
};

static_assert(sizeof(GpuShapeV5ResidentPartitionDescriptor) == 104u);
static_assert(alignof(GpuShapeV5ResidentPartitionDescriptor) == 8u);
static_assert(offsetof(
    GpuShapeV5ResidentPartitionDescriptor, hash_byte_offset) == 16u);
static_assert(offsetof(
    GpuShapeV5ResidentPartitionDescriptor, control_count) == 48u);
static_assert(offsetof(
    GpuShapeV5ResidentPartitionDescriptor, low_hash) == 72u);
static_assert(offsetof(
    GpuShapeV5ResidentPartitionDescriptor, segment_end_bp) == 96u);
static_assert(std::is_standard_layout_v<
    GpuShapeV5ResidentPartitionDescriptor>);
static_assert(sizeof(GpuShapeV5SeedPush) == 72u);
static_assert(offsetof(GpuShapeV5SeedPush, abi_version) == 0u);
static_assert(offsetof(GpuShapeV5SeedPush, partition_count) == 24u);
static_assert(offsetof(GpuShapeV5SeedPush, query_minimizer_count) == 44u);
static_assert(offsetof(GpuShapeV5SeedPush, max_occ_cap_lo) == 60u);
static_assert(offsetof(GpuShapeV5SeedPush, status_index) == 68u);

[[nodiscard]] constexpr bool gpu_shape_v5_scalar_width_valid(
    std::uint32_t width) noexcept
{
    return width == 2u || width == 4u || width == 8u;
}

[[nodiscard]] constexpr bool gpu_shape_v5_scalar_range_fits(
    std::uint64_t byte_offset,
    std::uint64_t element_count,
    std::uint32_t width,
    std::uint64_t buffer_bytes) noexcept
{
    if (!gpu_shape_v5_scalar_width_valid(width)
        || byte_offset % width != 0u
        || byte_offset > buffer_bytes
        || element_count
            > ((std::numeric_limits<std::uint64_t>::max)() - byte_offset)
                / width) {
        return false;
    }
    return element_count * width <= buffer_bytes - byte_offset;
}

/// Validate the complete resident descriptor against the aggregate byte
/// buffers before it is uploaded.  This is shape validation only; generation,
/// source, checksum, and device identities remain obligations of the resident
/// generation owner.
[[nodiscard]] constexpr bool gpu_shape_v5_partition_descriptor_valid(
    const GpuShapeV5ResidentPartitionDescriptor& descriptor,
    std::uint64_t control_buffer_bytes,
    std::uint64_t position_buffer_bytes) noexcept
{
    if (descriptor.flags != k_gpu_shape_v5_partition_required_flags
        || descriptor.control_count == 0u
        || descriptor.position_count == 0u
        || descriptor.low_hash > descriptor.high_hash
        || descriptor.segment_base_bp >= descriptor.segment_end_bp
        || !gpu_shape_v5_scalar_width_valid(descriptor.hash_width)
        || !gpu_shape_v5_scalar_width_valid(descriptor.offset_width)
        || !gpu_shape_v5_scalar_width_valid(descriptor.count_width)
        || !gpu_shape_v5_scalar_width_valid(descriptor.position_width)) {
        return false;
    }
    const std::uint64_t position_bits = descriptor.position_width * 8u;
    const std::uint64_t maximum_segment_span =
        std::uint64_t{1} << (position_bits - 1u);
    return descriptor.segment_end_bp - descriptor.segment_base_bp
            <= maximum_segment_span
        && gpu_shape_v5_scalar_range_fits(
            descriptor.hash_byte_offset,
            descriptor.control_count,
            descriptor.hash_width,
            control_buffer_bytes)
        && gpu_shape_v5_scalar_range_fits(
            descriptor.prefix_byte_offset,
            static_cast<std::uint64_t>(descriptor.control_count) + 1u,
            descriptor.offset_width,
            control_buffer_bytes)
        && gpu_shape_v5_scalar_range_fits(
            descriptor.count_byte_offset,
            descriptor.control_count,
            descriptor.count_width,
            control_buffer_bytes)
        && gpu_shape_v5_scalar_range_fits(
            descriptor.position_byte_offset,
            descriptor.position_count,
            descriptor.position_width,
            position_buffer_bytes);
}

enum class GpuGrouped32ValidationError : std::uint32_t {
    none = 0u,
    descriptor_bounds = 1u << 0u,
    descriptor_identity = 1u << 1u,
    segment_bounds = 1u << 2u,
    control_order = 1u << 3u,
    control_positions = 1u << 4u,
    position_bounds = 1u << 5u,
    position_order = 1u << 6u,
    query_bounds = 1u << 7u,
    anchor_capacity = 1u << 8u,
    emitted_count = 1u << 9u,
    occurrence_overflow = 1u << 10u,
    validation_gate = 1u << 11u,
    adapter_state = 1u << 12u,
    indirect_bounds = 1u << 13u,
    sort_ownership = 1u << 14u,
};

enum class GpuGrouped32ValidationCompletion : std::uint32_t {
    pending = 0u,
    accepted = 1u,
    rejected = 2u,
};

enum class GpuGrouped32PrefixAllocationStrategy : std::uint32_t {
    coarse_query_scan = 1u,
    hierarchical_segmented_scan = 2u,
    atomic_reservation_reference_only = 3u,
};

enum class GpuGrouped32PhysicalDeviceKind : std::uint32_t {
    other = 0u,
    integrated_gpu = 1u,
    discrete_gpu = 2u,
    virtual_gpu = 3u,
    cpu = 4u,
};

[[nodiscard]] constexpr GpuGrouped32ValidationError operator|(
    GpuGrouped32ValidationError lhs,
    GpuGrouped32ValidationError rhs) noexcept
{
    return static_cast<GpuGrouped32ValidationError>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

struct GpuGrouped32ControlWire {
    std::uint32_t hash = 0;
    std::uint32_t position_offset = 0;
    std::uint32_t position_count = 0;
};

struct GpuGrouped32PositionWire {
    std::uint32_t local_bp_strand = 0;
};

/// `partition_id` is the stable authenticated directory identity and may be
/// gapped. `directory_ordinal` binds the descriptor to its aggregate-buffer
/// slot; validation requires exact ordinal equality plus strictly increasing
/// stable IDs, never `partition_id == directory_ordinal`.
struct GpuGrouped32PartitionWire {
    std::uint32_t partition_id = 0;
    std::uint32_t directory_ordinal = 0;
    std::uint32_t position_segment = 0;
    std::uint32_t control_offset = 0;
    std::uint32_t control_count = 0;
    std::uint32_t position_offset = 0;
    std::uint32_t position_count = 0;
    std::uint32_t low_hash = 0;
    std::uint32_t high_hash = 0;
    std::uint32_t flags = 0;
    std::uint64_t segment_base_bp = 0;
    std::uint64_t segment_end_bp = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct GpuGrouped32QueryMinimizerWire {
    std::uint32_t hash = 0;
    std::uint32_t query_bp_strand = 0;
    std::uint32_t occurrence_index = 0;
    std::uint32_t query_record_index = 0;
};

/// Dense device state ownership is independent of the trusted, non-dense u64
/// BatchJoin ordinal carried through the terminal result.
struct GpuGrouped32QueryRecordWire {
    std::uint32_t minimizer_offset = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t query_length = 0;
    std::uint32_t query_state_index = 0;
    std::uint32_t occurrence_offset = 0;
    std::uint32_t anchor_base = 0;
    std::uint32_t anchor_capacity = 0;
    std::uint32_t flags = 0;
    std::uint64_t batch_join_ordinal = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct GpuGrouped32OccurrenceStateWire {
    std::uint32_t global_count = 0;
    std::uint32_t accepted_count = 0;
    std::uint32_t anchor_offset = 0;
    std::uint32_t emitted_count = 0;
};

struct GpuGrouped32QueryStateWire {
    std::uint32_t anchor_count = 0;
    std::uint32_t rejected_minimizers = 0;
    std::uint32_t completed_minimizers = 0;
    std::uint32_t status = 0;
};

/// Validation workgroups only increment `completed_workgroups`. A distinct
/// one-workgroup finalize dispatch may publish `completion` and indirect
/// commands after an explicit compute-write -> compute-read barrier. A second
/// compute-write -> indirect/compute barrier precedes every consumer. Every
/// downstream shader independently gates on accepted completion and zero
/// status.
struct GpuGrouped32ValidationStateWire {
    std::uint32_t status = 0;
    std::uint32_t failed_partitions = 0;
    std::uint32_t controls_checked = 0;
    std::uint32_t positions_checked = 0;
    std::uint32_t completed_workgroups = 0;
    std::uint32_t expected_workgroups = 0;
    std::uint32_t completion = 0;
    std::uint32_t reserved = 0;
};

struct GpuGrouped32DispatchIndirectCommandWire {
    std::uint32_t group_count_x = 0;
    std::uint32_t group_count_y = 0;
    std::uint32_t group_count_z = 0;
};

enum class GpuGrouped32IndirectStage : std::uint32_t {
    validate = 0u,
    accumulate_occurrences = 1u,
    finalize_max_occ = 2u,
    allocate_anchor_prefix = 3u,
    build_sort_segments = 4u,
    emit_global_anchors = 5u,
    radix_sort = 6u,
    count = 7u,
};

struct GpuGrouped32GlobalAnchorWire {
    std::uint64_t ref_position = 0;
    std::uint64_t query_position = 0;
    std::uint32_t is_reverse = 0;
    std::uint8_t query_span = 0;
    std::uint8_t flags = 0;
    std::uint16_t segment_id = 0;
};

/// Parallel radix key. Query ownership is redundantly carried and must match
/// the owning `GpuGrouped32SortSegmentWire`; sort kernels may never mix slices.
struct GpuGrouped32AnchorRadixKeyWire {
    std::uint64_t ref_position = 0;
    std::uint32_t is_reverse = 0;
    std::uint32_t query_state_index = 0;
};

struct GpuGrouped32SortSegmentWire {
    std::uint32_t anchor_begin = 0;
    std::uint32_t anchor_count = 0;
    std::uint32_t query_state_index = 0;
    std::uint32_t flags = 0;
    std::uint64_t batch_join_ordinal = 0;
};

/// Target-global transient produced by the persisted format-4 control pass.
/// It retains only the opaque directory ordinal and the position slice needed
/// by the later raw-position pass; no control or position payload is decoded on
/// the host and no sorted-dense/global64 representation is constructed.
struct GpuGrouped32StreamMatchWire {
    std::uint32_t directory_ordinal = 0;
    std::uint32_t query_minimizer_index = 0;
    std::uint32_t position_offset = 0;
    std::uint32_t position_count = 0;
};

/// Device-owned lifecycle state shared by the four persisted streaming
/// operations.  `completion` is zero while controls are accumulating, one
/// after occurrence authorization, and two after target sealing.  Any nonzero
/// status poisons the state permanently.
struct GpuGrouped32StreamStateWire {
    std::uint32_t status = 0;
    std::uint32_t match_count = 0;
    std::uint32_t matched_anchor_upper_bound = 0;
    std::uint32_t authorized_anchor_count = 0;
    std::uint32_t authorized_partition_count = 0;
    std::uint32_t emitted_anchor_count = 0;
    std::uint32_t completion = 0;
    std::uint32_t control_groups_completed = 0;
    std::uint32_t position_groups_completed = 0;
    std::uint32_t authorization_digest_lo = 0;
    std::uint32_t authorization_digest_hi = 0;
    std::uint32_t anchor_digest_lo = 0;
    std::uint32_t anchor_digest_hi = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t reserved2 = 0;
};

/// One push ABI is shared by the four bounded streaming kernels.  The exact
/// raw element widths remain the format-4 12/4 contract; capacities are fixed
/// before the relevant dispatch and are never inferred from buffer length.
struct GpuGrouped32StreamPush {
    std::uint32_t partition_count = 0;
    std::uint32_t control_count = 0;
    std::uint32_t position_count = 0;
    std::uint32_t query_record_count = 0;
    std::uint32_t query_minimizer_count = 0;
    std::uint32_t occurrence_count = 0;
    std::uint32_t match_capacity = 0;
    std::uint32_t directory_entry_count = 0;
    std::uint32_t anchor_capacity = 0;
    std::uint32_t max_occ = 0;
    std::uint32_t kmer_size = 15;
    std::uint32_t group_ordinal = 0;
    std::uint32_t expected_control_groups = 0;
    std::uint32_t expected_position_groups = 0;
    std::uint32_t query_identity_lo = 0;
    std::uint32_t query_identity_hi = 0;
    std::uint32_t control_state_identity_lo = 0;
    std::uint32_t control_state_identity_hi = 0;
    std::uint32_t target_buffer_identity_lo = 0;
    std::uint32_t target_buffer_identity_hi = 0;
};

struct GpuGrouped32FinalResultCigarWire {
    std::uint64_t query_start = 0;
    std::uint64_t query_end = 0;
    std::uint64_t target_start = 0;
    std::uint64_t target_end = 0;
    std::uint64_t match_length = 0;
    std::uint64_t block_length = 0;
    std::int32_t chain_score = 0;
    std::int32_t seed_count = 0;
    std::uint32_t cigar_word_offset = 0;
    std::uint32_t cigar_word_count = 0;
    std::uint64_t batch_join_ordinal = 0;
    std::uint32_t target_record_index = 0;
    std::uint32_t flags = 0;
    std::uint32_t status = 0;
    std::uint32_t reserved = 0;
};

struct GpuGrouped32Abi2Push {
    std::uint32_t partition_count = 0;
    std::uint32_t query_record_count = 0;
    std::uint32_t query_minimizer_capacity = 0;
    std::uint32_t occurrence_count = 0;
    std::uint32_t control_count = 0;
    std::uint32_t position_count = 0;
    std::uint32_t anchor_capacity = 0;
    std::uint32_t max_occ = 0;
    std::uint32_t kmer_size = 15;
    std::uint32_t validation_index = 0;
    std::uint32_t validation_state_count = 0;
    std::uint32_t indirect_base = 0;
    std::uint32_t indirect_command_count = 0;
    std::uint32_t max_partition_groups_y = 0;
    std::uint32_t max_workgroups_x = 0;
    std::uint32_t flags = 0;
    std::uint32_t adapter_state_index = 0;
    std::uint32_t adapter_state_count = 0;
};

/// Host scheduling metadata only: biological count/hash/position decoding is
/// performed by the adapter shader from the existing packed GPU sketch output.
struct GpuGrouped32SketchQueryPlanWire {
    std::uint32_t first_tile = 0;
    std::uint32_t tile_count = 0;
    std::uint32_t query_length = 0;
    std::uint32_t query_state_index = 0;
    std::uint32_t anchor_base = 0;
    std::uint32_t anchor_capacity = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t batch_join_ordinal = 0;
    std::uint64_t reserved1 = 0;
};

struct GpuGrouped32QueryAdapterStateWire {
    std::uint32_t status = 0;
    std::uint32_t completed_records = 0;
    std::uint32_t expected_records = 0;
    std::uint32_t total_minimizers = 0;
    std::uint32_t max_query_minimizers = 0;
    std::uint32_t completion = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

struct GpuGrouped32QueryAdapterPush {
    std::uint32_t query_plan_count = 0;
    std::uint32_t tile_count = 0;
    std::uint32_t packed_minimizer_capacity = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32 = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t kmer_size = 15;
    std::uint32_t adapter_state_index = 0;
    std::uint32_t adapter_state_count = 0;
    std::uint32_t validation_index = 0;
    std::uint32_t validation_state_count = 0;
    std::uint32_t indirect_base = 0;
    std::uint32_t indirect_command_count = 0;
    std::uint32_t max_workgroups_x = 0;
    std::uint32_t max_partition_groups_y = 0;
};

struct GpuGrouped32IndirectPreparePush {
    std::uint32_t partition_count = 0;
    std::uint32_t query_record_count = 0;
    std::uint32_t adapter_state_index = 0;
    std::uint32_t adapter_state_count = 0;
    std::uint32_t validation_index = 0;
    std::uint32_t validation_state_count = 0;
    std::uint32_t indirect_base = 0;
    std::uint32_t indirect_command_count = 0;
    std::uint32_t max_workgroups_x = 0;
    std::uint32_t max_partition_groups_y = 0;
    std::uint32_t local_size_x = 128;
    std::uint32_t flags = 0;
};

struct GpuGrouped32DeviceIdentity {
    std::array<std::uint8_t, 16> device_uuid{};
    GpuGrouped32PhysicalDeviceKind device_kind =
        GpuGrouped32PhysicalDeviceKind::other;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t driver_version = 0;

    [[nodiscard]] bool operator==(
        const GpuGrouped32DeviceIdentity&) const = default;
};

struct GpuGrouped32ResidentIdentity {
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t compatibility_checksum = 0;
    std::uint64_t cache_identity = 0;
    std::uint64_t resident_identity = 0;
    std::uint64_t submission_identity = 0;
    std::uint64_t fence_identity = 0;
    std::uint64_t fence_value = 0;
    GpuGrouped32DeviceIdentity device{};

    [[nodiscard]] bool operator==(
        const GpuGrouped32ResidentIdentity&) const = default;
};

struct GpuGrouped32ExecutionIdentity {
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t compatibility_checksum = 0;
    std::uint64_t cache_identity = 0;
    std::uint64_t resident_identity = 0;
    std::uint64_t query_identity = 0;
    std::uint64_t params_identity = 0;
    std::uint64_t submission_identity = 0;
    std::uint64_t fence_identity = 0;
    std::uint64_t fence_value = 0;
    GpuGrouped32DeviceIdentity device{};

    [[nodiscard]] bool operator==(
        const GpuGrouped32ExecutionIdentity&) const = default;
};

[[nodiscard]] inline bool gpu_grouped32_execution_matches_resident(
    const GpuGrouped32ExecutionIdentity& execution,
    const GpuGrouped32ResidentIdentity& resident) noexcept
{
    return execution.generation_id == resident.generation_id
        && execution.source_identity == resident.source_identity
        && execution.source_content_checksum == resident.source_content_checksum
        && execution.compatibility_checksum == resident.compatibility_checksum
        && execution.cache_identity == resident.cache_identity
        && execution.resident_identity == resident.resident_identity
        && execution.device == resident.device;
}

/// Ordered all-GPU execution stages. A qualifying execution proof requires a
/// non-zero dispatch record for every entry, including radix sort, chaining,
/// alignment, and terminal CIGAR/result materialization.
enum class GpuGrouped32ExecutionStage : std::uint32_t {
    state_clear = 0u,
    query_sketch = 1u,
    query_adapter = 2u,
    query_adapter_finalize = 3u,
    indirect_prepare = 4u,
    resident_validation = 5u,
    validation_finalize = 6u,
    occurrence_accumulate = 7u,
    max_occ_finalize = 8u,
    anchor_prefix = 9u,
    sort_segments = 10u,
    anchor_emit = 11u,
    radix_sort = 12u,
    chaining = 13u,
    alignment = 14u,
    cigar_result = 15u,
    count = 16u,
};

enum class GpuGrouped32BarrierAccess : std::uint32_t {
    none = 0u,
    compute_shader_read = 1u << 0u,
    compute_shader_write = 1u << 1u,
    indirect_command_read = 1u << 2u,
};

enum class GpuGrouped32PipelineScope : std::uint32_t {
    none = 0u,
    compute_shader = 1u << 0u,
    draw_indirect = 1u << 1u,
};

[[nodiscard]] constexpr GpuGrouped32BarrierAccess operator|(
    GpuGrouped32BarrierAccess lhs,
    GpuGrouped32BarrierAccess rhs) noexcept
{
    return static_cast<GpuGrouped32BarrierAccess>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr bool gpu_grouped32_has_barrier_access(
    GpuGrouped32BarrierAccess value,
    GpuGrouped32BarrierAccess required) noexcept
{
    return (static_cast<std::uint32_t>(value)
        & static_cast<std::uint32_t>(required))
        == static_cast<std::uint32_t>(required);
}

[[nodiscard]] constexpr GpuGrouped32PipelineScope operator|(
    GpuGrouped32PipelineScope lhs,
    GpuGrouped32PipelineScope rhs) noexcept
{
    return static_cast<GpuGrouped32PipelineScope>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr bool gpu_grouped32_has_pipeline_scope(
    GpuGrouped32PipelineScope value,
    GpuGrouped32PipelineScope required) noexcept
{
    return (static_cast<std::uint32_t>(value)
        & static_cast<std::uint32_t>(required))
        == static_cast<std::uint32_t>(required);
}

struct GpuGrouped32StageExecutionRecord {
    GpuGrouped32ExecutionStage stage = GpuGrouped32ExecutionStage::state_clear;
    std::uint32_t flags = 0;
    std::uint64_t dispatch_count = 0;
    std::uint64_t submission_identity = 0;
    std::uint64_t fence_identity = 0;
    std::uint64_t fence_value = 0;
    GpuGrouped32DeviceIdentity device{};
};

struct GpuGrouped32BarrierRecord {
    GpuGrouped32ExecutionStage source_stage =
        GpuGrouped32ExecutionStage::state_clear;
    GpuGrouped32ExecutionStage destination_stage =
        GpuGrouped32ExecutionStage::query_sketch;
    GpuGrouped32BarrierAccess source_access =
        GpuGrouped32BarrierAccess::none;
    GpuGrouped32BarrierAccess destination_access =
        GpuGrouped32BarrierAccess::none;
    GpuGrouped32PipelineScope source_pipeline_scope =
        GpuGrouped32PipelineScope::none;
    GpuGrouped32PipelineScope destination_pipeline_scope =
        GpuGrouped32PipelineScope::none;
    std::uint64_t submission_identity = 0;
    std::uint64_t fence_identity = 0;
    std::uint64_t fence_value = 0;
    GpuGrouped32DeviceIdentity device{};
};

inline constexpr std::size_t k_gpu_grouped32_required_barrier_count = 15u;
inline constexpr std::size_t k_gpu_grouped32_foundation_barrier_count = 11u;

struct GpuGrouped32BarrierPlan {
    std::array<GpuGrouped32BarrierRecord,
        k_gpu_grouped32_required_barrier_count> records{};
    std::uint32_t record_count = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
};

struct GpuGrouped32RequiredBarrier {
    GpuGrouped32ExecutionStage source_stage;
    GpuGrouped32ExecutionStage destination_stage;
    GpuGrouped32BarrierAccess source_access;
    GpuGrouped32BarrierAccess destination_access;
};

inline constexpr std::array<GpuGrouped32RequiredBarrier,
    k_gpu_grouped32_required_barrier_count>
k_gpu_grouped32_required_barriers{{
    {GpuGrouped32ExecutionStage::state_clear,
     GpuGrouped32ExecutionStage::query_sketch,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read},
    {GpuGrouped32ExecutionStage::query_sketch,
     GpuGrouped32ExecutionStage::query_adapter,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read},
    {GpuGrouped32ExecutionStage::query_adapter,
     GpuGrouped32ExecutionStage::query_adapter_finalize,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read},
    {GpuGrouped32ExecutionStage::query_adapter_finalize,
     GpuGrouped32ExecutionStage::indirect_prepare,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read},
    {GpuGrouped32ExecutionStage::indirect_prepare,
     GpuGrouped32ExecutionStage::resident_validation,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read
        | GpuGrouped32BarrierAccess::indirect_command_read},
    {GpuGrouped32ExecutionStage::resident_validation,
     GpuGrouped32ExecutionStage::validation_finalize,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read},
    {GpuGrouped32ExecutionStage::validation_finalize,
     GpuGrouped32ExecutionStage::occurrence_accumulate,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read
        | GpuGrouped32BarrierAccess::indirect_command_read},
    {GpuGrouped32ExecutionStage::occurrence_accumulate,
     GpuGrouped32ExecutionStage::max_occ_finalize,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read
        | GpuGrouped32BarrierAccess::indirect_command_read},
    {GpuGrouped32ExecutionStage::max_occ_finalize,
     GpuGrouped32ExecutionStage::anchor_prefix,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read
        | GpuGrouped32BarrierAccess::indirect_command_read},
    {GpuGrouped32ExecutionStage::anchor_prefix,
     GpuGrouped32ExecutionStage::sort_segments,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read
        | GpuGrouped32BarrierAccess::indirect_command_read},
    {GpuGrouped32ExecutionStage::sort_segments,
     GpuGrouped32ExecutionStage::anchor_emit,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read
        | GpuGrouped32BarrierAccess::indirect_command_read},
    {GpuGrouped32ExecutionStage::anchor_emit,
     GpuGrouped32ExecutionStage::radix_sort,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read
        | GpuGrouped32BarrierAccess::indirect_command_read},
    {GpuGrouped32ExecutionStage::radix_sort,
     GpuGrouped32ExecutionStage::chaining,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read},
    {GpuGrouped32ExecutionStage::chaining,
     GpuGrouped32ExecutionStage::alignment,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read},
    {GpuGrouped32ExecutionStage::alignment,
     GpuGrouped32ExecutionStage::cigar_result,
     GpuGrouped32BarrierAccess::compute_shader_write,
     GpuGrouped32BarrierAccess::compute_shader_read},
}};

[[nodiscard]] inline bool gpu_grouped32_stage_record_matches(
    const GpuGrouped32StageExecutionRecord& record,
    GpuGrouped32ExecutionStage expected_stage,
    const GpuGrouped32ExecutionIdentity& identity) noexcept
{
    return record.stage == expected_stage && record.flags == 0u
        && record.dispatch_count != 0u
        && record.submission_identity == identity.submission_identity
        && record.fence_identity == identity.fence_identity
        && record.fence_value == identity.fence_value
        && record.device == identity.device;
}

[[nodiscard]] inline bool gpu_grouped32_barrier_plan_valid_prefix(
    const GpuGrouped32BarrierPlan& plan,
    const GpuGrouped32ExecutionIdentity& identity,
    std::size_t required_count) noexcept
{
    if (required_count > k_gpu_grouped32_required_barrier_count
        || plan.record_count != required_count || plan.reserved0 != 0u
        || plan.reserved1 != 0u) {
        return false;
    }
    for (std::size_t i = 0; i < required_count; ++i) {
        const auto& observed = plan.records[i];
        const auto& required = k_gpu_grouped32_required_barriers[i];
        if (observed.source_stage != required.source_stage
            || observed.destination_stage != required.destination_stage
            || !gpu_grouped32_has_barrier_access(
                observed.source_access, required.source_access)
            || !gpu_grouped32_has_barrier_access(
                observed.destination_access, required.destination_access)
            || !gpu_grouped32_has_pipeline_scope(
                observed.source_pipeline_scope,
                GpuGrouped32PipelineScope::compute_shader)
            || !gpu_grouped32_has_pipeline_scope(
                observed.destination_pipeline_scope,
                GpuGrouped32PipelineScope::compute_shader)
            || (gpu_grouped32_has_barrier_access(
                    required.destination_access,
                    GpuGrouped32BarrierAccess::indirect_command_read)
                && !gpu_grouped32_has_pipeline_scope(
                    observed.destination_pipeline_scope,
                    GpuGrouped32PipelineScope::draw_indirect))
            || observed.submission_identity != identity.submission_identity
            || observed.fence_identity != identity.fence_identity
            || observed.fence_value != identity.fence_value
            || observed.device != identity.device) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool gpu_grouped32_barrier_plan_valid(
    const GpuGrouped32BarrierPlan& plan,
    const GpuGrouped32ExecutionIdentity& identity) noexcept
{
    return gpu_grouped32_barrier_plan_valid_prefix(
        plan, identity, k_gpu_grouped32_required_barrier_count);
}

static_assert(sizeof(GpuGrouped32StageExecutionRecord) == 72);
static_assert(sizeof(GpuGrouped32BarrierRecord) == 80);
static_assert(sizeof(GpuGrouped32BarrierPlan) == 1216);


static_assert(sizeof(GpuGrouped32ControlWire) == 12);
static_assert(offsetof(GpuGrouped32ControlWire, position_offset) == 4);
static_assert(offsetof(GpuGrouped32ControlWire, position_count) == 8);
static_assert(sizeof(GpuGrouped32PositionWire) == 4);
static_assert(sizeof(GpuGrouped32PartitionWire) == 64);
static_assert(offsetof(GpuGrouped32PartitionWire, directory_ordinal) == 4);
static_assert(offsetof(GpuGrouped32PartitionWire, position_segment) == 8);
static_assert(offsetof(GpuGrouped32PartitionWire, control_offset) == 12);
static_assert(offsetof(GpuGrouped32PartitionWire, high_hash) == 32);
static_assert(offsetof(GpuGrouped32PartitionWire, segment_base_bp) == 40);
static_assert(offsetof(GpuGrouped32PartitionWire, segment_end_bp) == 48);
static_assert(offsetof(GpuGrouped32PartitionWire, reserved1) == 60);
static_assert(sizeof(GpuGrouped32QueryMinimizerWire) == 16);
static_assert(sizeof(GpuGrouped32QueryRecordWire) == 48);
static_assert(offsetof(GpuGrouped32QueryRecordWire, query_state_index) == 12);
static_assert(offsetof(GpuGrouped32QueryRecordWire, batch_join_ordinal) == 32);
static_assert(offsetof(GpuGrouped32QueryRecordWire, reserved1) == 44);
static_assert(sizeof(GpuGrouped32OccurrenceStateWire) == 16);
static_assert(sizeof(GpuGrouped32QueryStateWire) == 16);
static_assert(sizeof(GpuGrouped32ValidationStateWire) == 32);
static_assert(offsetof(GpuGrouped32ValidationStateWire, completed_workgroups) == 16);
static_assert(offsetof(GpuGrouped32ValidationStateWire, completion) == 24);
static_assert(sizeof(GpuGrouped32DispatchIndirectCommandWire) == 12);
static_assert(sizeof(GpuGrouped32GlobalAnchorWire) == 24);
static_assert(offsetof(GpuGrouped32GlobalAnchorWire, is_reverse) == 16);
static_assert(offsetof(GpuGrouped32GlobalAnchorWire, query_span) == 20);
static_assert(sizeof(GpuGrouped32AnchorRadixKeyWire) == 16);
static_assert(offsetof(GpuGrouped32AnchorRadixKeyWire, query_state_index) == 12);
static_assert(sizeof(GpuGrouped32SortSegmentWire) == 24);
static_assert(offsetof(GpuGrouped32SortSegmentWire, batch_join_ordinal) == 16);
static_assert(sizeof(GpuGrouped32StreamMatchWire) == 16);
static_assert(offsetof(GpuGrouped32StreamMatchWire, position_offset) == 8);
static_assert(sizeof(GpuGrouped32StreamStateWire) == 64);
static_assert(offsetof(GpuGrouped32StreamStateWire, completion) == 24);
static_assert(offsetof(GpuGrouped32StreamStateWire, anchor_digest_lo) == 44);
static_assert(sizeof(GpuGrouped32StreamPush) == 80);
static_assert(offsetof(GpuGrouped32StreamPush, query_identity_lo) == 56);
static_assert(sizeof(GpuGrouped32FinalResultCigarWire) == 88);
static_assert(offsetof(GpuGrouped32FinalResultCigarWire, chain_score) == 48);
static_assert(offsetof(GpuGrouped32FinalResultCigarWire, batch_join_ordinal) == 64);
static_assert(offsetof(GpuGrouped32FinalResultCigarWire, status) == 80);
static_assert(sizeof(GpuGrouped32Abi2Push) == 72);
static_assert(offsetof(GpuGrouped32Abi2Push, validation_state_count) == 40);
static_assert(offsetof(GpuGrouped32Abi2Push, indirect_base) == 44);
static_assert(offsetof(GpuGrouped32Abi2Push, flags) == 60);
static_assert(offsetof(GpuGrouped32Abi2Push, adapter_state_index) == 64);
static_assert(sizeof(GpuGrouped32SketchQueryPlanWire) == 48);
static_assert(offsetof(GpuGrouped32SketchQueryPlanWire, batch_join_ordinal) == 32);
static_assert(sizeof(GpuGrouped32QueryAdapterStateWire) == 32);
static_assert(sizeof(GpuGrouped32QueryAdapterPush) == 64);
static_assert(sizeof(GpuGrouped32IndirectPreparePush) == 48);
static_assert(sizeof(GpuGrouped32DeviceIdentity) == 32);
static_assert(sizeof(GpuGrouped32ResidentIdentity) == 104);
static_assert(offsetof(GpuGrouped32ResidentIdentity, device) == 72);
static_assert(sizeof(GpuGrouped32ExecutionIdentity) == 120);
static_assert(offsetof(GpuGrouped32ExecutionIdentity, resident_identity) == 40);
static_assert(offsetof(GpuGrouped32ExecutionIdentity, query_identity) == 48);
static_assert(offsetof(GpuGrouped32ExecutionIdentity, device) == 88);
static_assert(std::is_standard_layout_v<GpuGrouped32PartitionWire>);
static_assert(std::is_standard_layout_v<GpuGrouped32QueryRecordWire>);
static_assert(std::is_standard_layout_v<GpuGrouped32FinalResultCigarWire>);

[[nodiscard]] inline bool gpu_grouped32_device_uuid_bound(
    const GpuGrouped32DeviceIdentity& identity) noexcept
{
    return std::any_of(
        identity.device_uuid.begin(), identity.device_uuid.end(),
        [](std::uint8_t byte) noexcept { return byte != 0u; });
}

[[nodiscard]] inline bool gpu_grouped32_hardware_device(
    const GpuGrouped32DeviceIdentity& identity) noexcept
{
    return gpu_grouped32_device_uuid_bound(identity)
        && (identity.device_kind == GpuGrouped32PhysicalDeviceKind::integrated_gpu
            || identity.device_kind
                == GpuGrouped32PhysicalDeviceKind::discrete_gpu);
}

[[nodiscard]] inline bool gpu_grouped32_execution_identity_bound(
    const GpuGrouped32ExecutionIdentity& identity,
    const GpuGrouped32ResidentIdentity& resident) noexcept
{
    return identity.generation_id != 0u && identity.source_identity != 0u
        && identity.source_content_checksum != 0u
        && identity.compatibility_checksum != 0u
        && identity.cache_identity != 0u && identity.resident_identity != 0u
        && identity.query_identity != 0u && identity.params_identity != 0u
        && identity.submission_identity != 0u
        && identity.fence_identity != 0u && identity.fence_value != 0u
        && resident.submission_identity != 0u
        && resident.fence_identity != 0u && resident.fence_value != 0u
        && gpu_grouped32_hardware_device(identity.device)
        && gpu_grouped32_execution_matches_resident(identity, resident);
}

/// No accepted proof factory exists in this inactive foundation. The sole
/// public constructor path creates a rejected test token so move-poisoning can
/// be tested without forging a queue-fence-backed proof.
class GpuGrouped32ValidatedResidentProof final {
public:
    GpuGrouped32ValidatedResidentProof(
        const GpuGrouped32ValidatedResidentProof&) = delete;
    GpuGrouped32ValidatedResidentProof& operator=(
        const GpuGrouped32ValidatedResidentProof&) = delete;

    GpuGrouped32ValidatedResidentProof(
        GpuGrouped32ValidatedResidentProof&& source) noexcept
        : data_(source.data_), moved_from_(source.moved_from_)
    {
        source.poison_after_move();
    }

    GpuGrouped32ValidatedResidentProof& operator=(
        GpuGrouped32ValidatedResidentProof&& source) noexcept
    {
        if (this != &source) {
            data_ = source.data_;
            moved_from_ = source.moved_from_;
            source.poison_after_move();
        }
        return *this;
    }

    [[nodiscard]] static GpuGrouped32ValidatedResidentProof
    rejected_test_token() noexcept { return {}; }

    [[nodiscard]] bool moved_from() const noexcept { return moved_from_; }

    [[nodiscard]] bool accepted() const noexcept {
        const auto& id = data_.identity;
        return !moved_from_
            && data_.abi_version == k_gpu_grouped32_abi_version
            && id.generation_id != 0u && id.source_identity != 0u
            && id.source_content_checksum != 0u
            && id.compatibility_checksum != 0u
            && id.cache_identity != 0u && id.resident_identity != 0u
            && id.submission_identity != 0u && id.fence_identity != 0u
            && id.fence_value != 0u
            && gpu_grouped32_hardware_device(id.device)
            && data_.queue_fence_completed != 0u
            && data_.validation_status == 0u
            && data_.validated_partitions != 0u
            && data_.validated_controls != 0u
            && data_.validated_positions != 0u;
    }

private:
    struct Data {
        GpuGrouped32ResidentIdentity identity{};
        std::uint64_t validated_controls = 0;
        std::uint64_t validated_positions = 0;
        std::uint32_t validated_partitions = 0;
        std::uint32_t validation_status = 0;
        std::uint32_t abi_version = 0;
        std::uint32_t queue_fence_completed = 0;
    };

    GpuGrouped32ValidatedResidentProof() noexcept = default;

    void poison_after_move() noexcept {
        data_ = {};
        moved_from_ = true;
    }

    Data data_{};
    bool moved_from_ = false;
};

/// Inactive pre-radix foundation token. This distinct type can record the
/// clear, sketch, adapter-finalize, validation-finalize, and anchor-emission
/// stages with their exact barriers, but it can never qualify a run as
/// all-GPU. No accepted factory exists yet.
class GpuGrouped32FoundationOnlyProof final {
public:
    GpuGrouped32FoundationOnlyProof(
        const GpuGrouped32FoundationOnlyProof&) = delete;
    GpuGrouped32FoundationOnlyProof& operator=(
        const GpuGrouped32FoundationOnlyProof&) = delete;

    GpuGrouped32FoundationOnlyProof(
        GpuGrouped32FoundationOnlyProof&& source) noexcept
        : data_(source.data_), moved_from_(source.moved_from_)
    {
        source.poison_after_move();
    }

    GpuGrouped32FoundationOnlyProof& operator=(
        GpuGrouped32FoundationOnlyProof&& source) noexcept
    {
        if (this != &source) {
            data_ = source.data_;
            moved_from_ = source.moved_from_;
            source.poison_after_move();
        }
        return *this;
    }

    [[nodiscard]] static GpuGrouped32FoundationOnlyProof
    rejected_test_token() noexcept { return {}; }

    [[nodiscard]] bool moved_from() const noexcept { return moved_from_; }

    [[nodiscard]] bool foundation_validated() const noexcept {
        if (moved_from_
            || data_.abi_version != k_gpu_grouped32_abi_version
            || !gpu_grouped32_execution_identity_bound(
                data_.identity, data_.validated_resident_identity)
            || data_.resident_validation_status != 0u
            || data_.resident_queue_fence_completed == 0u
            || data_.queue_fence_completed == 0u || data_.status != 0u
            || data_.cpu_fallbacks != 0u || data_.simd_fallbacks != 0u
            || !gpu_grouped32_barrier_plan_valid_prefix(
                data_.barriers, data_.identity,
                k_gpu_grouped32_foundation_barrier_count)) {
            return false;
        }
        constexpr std::size_t foundation_stage_count =
            static_cast<std::size_t>(
                GpuGrouped32ExecutionStage::anchor_emit) + 1u;
        for (std::size_t i = 0; i < foundation_stage_count; ++i) {
            if (!gpu_grouped32_stage_record_matches(
                    data_.stages[i],
                    static_cast<GpuGrouped32ExecutionStage>(i),
                    data_.identity)) {
                return false;
            }
        }
        for (std::size_t i = foundation_stage_count;
             i < data_.stages.size(); ++i) {
            if (data_.stages[i].dispatch_count != 0u) return false;
        }
        return true;
    }

    [[nodiscard]] static constexpr bool
    qualifies_as_all_gpu_execution() noexcept { return false; }

private:
    struct Data {
        GpuGrouped32ExecutionIdentity identity{};
        GpuGrouped32ResidentIdentity validated_resident_identity{};
        std::array<GpuGrouped32StageExecutionRecord,
            static_cast<std::size_t>(GpuGrouped32ExecutionStage::count)>
            stages{};
        GpuGrouped32BarrierPlan barriers{};
        std::uint32_t resident_validation_status = 0;
        std::uint32_t resident_queue_fence_completed = 0;
        std::uint32_t status = 0;
        std::uint32_t abi_version = 0;
        std::uint32_t queue_fence_completed = 0;
        std::uint32_t cpu_fallbacks = 0;
        std::uint32_t simd_fallbacks = 0;
    };

    GpuGrouped32FoundationOnlyProof() noexcept = default;

    void poison_after_move() noexcept {
        data_ = {};
        moved_from_ = true;
    }

    Data data_{};
    bool moved_from_ = false;
};

class GpuGrouped32ExecutionProof final {
public:
    GpuGrouped32ExecutionProof(const GpuGrouped32ExecutionProof&) = delete;
    GpuGrouped32ExecutionProof& operator=(
        const GpuGrouped32ExecutionProof&) = delete;

    GpuGrouped32ExecutionProof(GpuGrouped32ExecutionProof&& source) noexcept
        : data_(source.data_), moved_from_(source.moved_from_)
    {
        source.poison_after_move();
    }

    GpuGrouped32ExecutionProof& operator=(
        GpuGrouped32ExecutionProof&& source) noexcept
    {
        if (this != &source) {
            data_ = source.data_;
            moved_from_ = source.moved_from_;
            source.poison_after_move();
        }
        return *this;
    }

    [[nodiscard]] static GpuGrouped32ExecutionProof
    rejected_test_token() noexcept { return {}; }

    [[nodiscard]] bool moved_from() const noexcept { return moved_from_; }

    [[nodiscard]] bool accepted() const noexcept {
        const auto& id = data_.identity;
        if (moved_from_
            || data_.abi_version != k_gpu_grouped32_abi_version
            || !gpu_grouped32_execution_identity_bound(
                id, data_.validated_resident_identity)
            || data_.resident_validation_status != 0u
            || data_.resident_queue_fence_completed == 0u
            || data_.queue_fence_completed == 0u || data_.status != 0u
            || data_.cpu_fallbacks != 0u || data_.simd_fallbacks != 0u
            || !gpu_grouped32_barrier_plan_valid(data_.barriers, id)) {
            return false;
        }
        for (std::size_t i = 0; i < data_.stages.size(); ++i) {
            if (!gpu_grouped32_stage_record_matches(
                    data_.stages[i],
                    static_cast<GpuGrouped32ExecutionStage>(i), id)) {
                return false;
            }
        }
        return true;
    }

private:
    struct Data {
        GpuGrouped32ExecutionIdentity identity{};
        GpuGrouped32ResidentIdentity validated_resident_identity{};
        std::array<GpuGrouped32StageExecutionRecord,
            static_cast<std::size_t>(GpuGrouped32ExecutionStage::count)>
            stages{};
        GpuGrouped32BarrierPlan barriers{};
        std::uint32_t resident_validation_status = 0;
        std::uint32_t resident_queue_fence_completed = 0;
        std::uint32_t status = 0;
        std::uint32_t abi_version = 0;
        std::uint32_t queue_fence_completed = 0;
        std::uint32_t cpu_fallbacks = 0;
        std::uint32_t simd_fallbacks = 0;
    };

    GpuGrouped32ExecutionProof() noexcept = default;

    void poison_after_move() noexcept {
        data_ = {};
        moved_from_ = true;
    }

    Data data_{};
    bool moved_from_ = false;
};

static_assert(!std::is_default_constructible_v<
    GpuGrouped32ValidatedResidentProof>);
static_assert(!std::is_default_constructible_v<
    GpuGrouped32FoundationOnlyProof>);
static_assert(!std::is_default_constructible_v<GpuGrouped32ExecutionProof>);

/// Push constants for the one-workgroup query-minimizer -> directory-ordinal
/// selector. Query hashes remain inside the device buffer throughout the run.
struct GpuPersistedDirectorySelectionPush {
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t directory_entry_count = 0;
    std::uint32_t reserved_directory = 0;
    std::uint32_t bitset_word_count = 0;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(GpuPersistedDirectorySelectionPush) == 32);
static_assert(offsetof(GpuPersistedDirectorySelectionPush, minimizer_count) == 0);
static_assert(offsetof(GpuPersistedDirectorySelectionPush, reserved_directory) == 20);
static_assert(offsetof(GpuPersistedDirectorySelectionPush, reserved) == 28);

[[nodiscard]] inline constexpr std::uint64_t gpu_alignment_cigar_ceil_div_u64(
    const std::uint64_t value,
    const std::uint64_t divisor) noexcept
{
    return divisor == 0 ? 0 : (value + divisor - 1u) / divisor;
}

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
        gpu_alignment_cigar_ceil_div_u64(query_len, tile_query);
    const std::uint64_t target_tiles =
        gpu_alignment_cigar_ceil_div_u64(target_len, tile_target);
    const std::uint64_t boundary_words_per_tile =
        5u * (static_cast<std::uint64_t>(tile_query) + 1u
              + static_cast<std::uint64_t>(tile_target) + 1u);
    const std::uint64_t scratch_words =
        (static_cast<std::uint64_t>(tile_query) + 1u)
        * (static_cast<std::uint64_t>(tile_target) + 1u);
    return query_tiles * target_tiles * boundary_words_per_tile + scratch_words;
}

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
    std::uint64_t target_index_initial_key_bytes = 0;
    std::uint64_t target_index_key_scratch_bytes = 0;
    std::uint64_t target_index_value_scratch_bytes = 0;
    std::uint64_t target_index_histogram_bytes = 0;
    std::uint64_t target_index_flag_scan_bytes = 0;
    std::uint64_t target_index_scan_block_bytes = 0;
    std::uint64_t target_index_bucket_bytes = 0;
    std::uint64_t target_index_sort_peak_bytes = 0;
    std::uint64_t target_index_bucket_peak_bytes = 0;
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

/// Physical X dispatch for a shader whose logical invocations grid-stride.
/// The logical work item count remains in push constants; this value is always
/// legal for the selected device and never silently truncates coverage.
[[nodiscard]] constexpr std::uint32_t phase_c_grid_stride_dispatch_groups(
    std::uint64_t logical_items,
    std::uint32_t local_size,
    std::uint32_t max_workgroups) noexcept
{
    if (logical_items == 0u || local_size == 0u || max_workgroups == 0u) {
        return 0u;
    }
    return std::min(
        phase_c_ceil_div_u32(logical_items, local_size),
        max_workgroups);
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

/// Hard semantic upper bound for a target sketch.  Every emitted minimizer is
/// attached to one canonical k-mer end position, and a position can be emitted
/// at most once by the ordered minimizer state machine (tie rescans enumerate
/// distinct positions).  Query gating can only remove emissions.  Consequently
/// the tile-count prefix sum is representable in uint32 whenever `bp_count` is.
[[nodiscard]] constexpr std::uint64_t phase_c_target_minimizer_hard_bound(
    std::uint64_t bp_count,
    std::uint32_t k) noexcept
{
    if (k == 0u || bp_count < k) {
        return 0u;
    }
    return bp_count - static_cast<std::uint64_t>(k) + 1u;
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
    // DOC0353: the sketch kernel grid-strides one invocation per tile and the
    // runtime clamps the dispatch to maxComputeWorkGroupCount[0], so a window with
    // tile_count > max_workgroups is processed over multiple grid-stride steps —
    // it is no longer a dispatchability failure (was `&& tile_count <= max_workgroups`).
    const bool dispatchable =
        tile_bp != 0u
        && tile_count != 0u
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

    if (use == PhaseCSketchUse::target_index_build) {
        const auto allocation_bytes = [](std::uint64_t bytes) {
            return phase_c_align_up(
                std::max<std::uint64_t>(bytes, 1u),
                k_phase_c_peak_allocation_align);
        };
        const std::uint64_t flag_bytes =
            phase_c_saturating_mul(output_capacity, 4u);
        const std::uint64_t scanned_offset =
            phase_c_align_up(flag_bytes, k_phase_c_slot_offset_align);
        plan.target_index_flag_scan_bytes =
            phase_c_saturating_add(scanned_offset, flag_bytes);
        plan.target_index_initial_key_bytes = std::max(
            plan.pair_key_bytes, plan.target_index_flag_scan_bytes);
        // The non-result key ping-pong allocation is reused as the aligned
        // flags/scanned allocation after the radix fence.
        plan.target_index_key_scratch_bytes = std::max(
            plan.pair_key_bytes, plan.target_index_flag_scan_bytes);
        plan.target_index_value_scratch_bytes = plan.pair_value_bytes;
        const std::uint64_t sort_groups = std::min<std::uint64_t>(
            phase_c_ceil_div_u32(output_capacity, 512u), max_workgroups);
        plan.target_index_histogram_bytes = phase_c_saturating_mul(
            phase_c_saturating_mul(sort_groups, 256u), 4u);
        plan.target_index_scan_block_bytes = phase_c_saturating_mul(
            phase_c_ceil_div_u32(output_capacity, k_phase_c_scan_block), 4u);
        plan.target_index_bucket_bytes =
            phase_c_saturating_mul(output_capacity, 16u);

        std::uint64_t sort_peak = 0u;
        sort_peak = phase_c_saturating_add(
            sort_peak, allocation_bytes(plan.target_index_initial_key_bytes));
        sort_peak = phase_c_saturating_add(
            sort_peak, allocation_bytes(plan.pair_value_bytes));
        sort_peak = phase_c_saturating_add(
            sort_peak, allocation_bytes(plan.target_index_key_scratch_bytes));
        sort_peak = phase_c_saturating_add(
            sort_peak, allocation_bytes(plan.target_index_value_scratch_bytes));
        sort_peak = phase_c_saturating_add(
            sort_peak, allocation_bytes(plan.target_index_histogram_bytes));
        plan.target_index_sort_peak_bytes = sort_peak;

        // After the radix fence the unused value allocation and histogram are
        // released. The non-result key allocation becomes flags+scanned;
        // buckets are allocated at exact distinct-key count D (bounded by C).
        std::uint64_t bucket_peak = 0u;
        bucket_peak = phase_c_saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_initial_key_bytes));
        bucket_peak = phase_c_saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_value_scratch_bytes));
        bucket_peak = phase_c_saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_key_scratch_bytes));
        bucket_peak = phase_c_saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_scan_block_bytes));
        bucket_peak = phase_c_saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_bucket_bytes));
        plan.target_index_bucket_peak_bytes = bucket_peak;
        plan.standalone_device_bytes = std::max(sort_peak, bucket_peak);
        plan.resident_output_bytes = phase_c_saturating_add(
            allocation_bytes(plan.target_index_value_scratch_bytes),
            allocation_bytes(plan.target_index_bucket_bytes));
    } else {
        plan.standalone_device_bytes = phase_c_saturating_add(
            plan.output_word_bytes,
            phase_c_saturating_add(plan.pair_key_bytes, plan.pair_value_bytes));
        plan.resident_output_bytes = plan.standalone_device_bytes;
    }
    // `tiles` and the packed sequence both exist on the host while their arena
    // copies are live.
    plan.host_transient_bytes = phase_c_saturating_add(
        plan.seq_bytes, plan.tile_bytes);
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

/// One query span consumed by the GPU hard-range builder.
struct GpuChainRangeBuildSpanWire {
    std::uint32_t anchor_lo;
    std::uint32_t anchor_hi;
    std::uint32_t range_lo;
    std::uint32_t range_out;
};

/// Compact coordinate-only result emitted after resident seed+chain execution.
struct GpuCoordinateOnlyResultWire {
    std::uint64_t query_start;
    std::uint64_t query_end;
    std::uint64_t target_start;
    std::uint64_t target_end;
    std::uint64_t match_length;
    std::uint64_t block_length;
    std::int32_t chain_score;
    std::int32_t seed_count;
    std::uint32_t is_reverse;
    std::uint32_t valid;
};

/// Push constants for the resident coordinate-only chain reducer.
struct GpuCoordinateReducePush {
    std::uint32_t anchor_count;
    std::uint32_t query_len_lo;
    std::uint32_t query_len_hi;
    std::int32_t min_chain_score;
    std::int32_t min_seed_count;
    std::uint32_t probe_limit;
    std::uint32_t reserved;
};

/// One score/end-point-only affine extension job. `query_offset` and
/// `target_offset` are offsets into the runtime-provided uint32 symbol streams;
/// symbols are caller-normalised alphabet ranks, not dna4-specific encodings.
struct GpuAlignmentExtensionJobWire {
    std::uint32_t query_offset;
    std::uint32_t target_offset;
    std::uint32_t query_len;
    std::uint32_t target_len;
    std::uint32_t band_width;
    std::uint32_t flags;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
};

/// Score and best extension end point for one job. No CIGAR or traceback state
/// is materialised by this kernel surface.
struct GpuAlignmentExtensionResultWire {
    std::int32_t score;
    std::uint32_t query_end;
    std::uint32_t target_end;
    std::uint32_t status;
};

/// Geometry and scoring contract for one alignment-extension batch. The runtime
/// validates every job against these limits before dispatching the Vulkan kernel.
struct GpuAlignmentExtensionParams {
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

/// One rank-buffer SneakySnake filter job. Offsets and lengths are expressed
/// in symbols inside the caller-provided flat uint8 rank streams.
struct GpuSneakySnakeJobWire {
    std::uint32_t query_offset = 0;
    std::uint32_t target_offset = 0;
    std::uint32_t query_length = 0;
    std::uint32_t target_length = 0;
    std::uint32_t max_edits = 0;
};

/// Conservative lower-bound decision for one SneakySnake job. `accepted` is
/// always zero or one; status 1 means only the unequal-length lower bound was
/// evaluated and status 2 means the valid job shape was conservatively kept.
struct GpuSneakySnakeResultWire {
    std::uint32_t lower_bound = 0;
    std::uint32_t status = k_gpu_sneaky_snake_status_ok;
    std::uint32_t accepted = 1;
    std::uint32_t reserved = 0;
};

/// Push constants for one grid-strided SneakySnake batch dispatch.
struct GpuSneakySnakePush {
    std::uint32_t job_count = 0;
    /// Uniform dispatch mode: 1 keeps one independent job per invocation;
    /// 8 assigns one job to each fixed eight-lane logical team.
    std::uint32_t team_size = 1;
};

/// CIGAR-capable extension job ABI (DOC0354 Part 2). This exact-match first
/// implementation uses the same wire shape the future traceback kernel will
/// consume, so host/result/cigar buffers do not need another ABI migration.
struct GpuAlignmentCigarJobWire {
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

struct GpuAlignmentCigarResultWire {
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

struct GpuAlignmentCigarParams {
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
static_assert(std::is_standard_layout_v<GpuChainRangeBuildSpanWire>);
static_assert(sizeof(GpuChainRangeBuildSpanWire) == 16);
static_assert(std::is_standard_layout_v<GpuCoordinateOnlyResultWire>);
static_assert(sizeof(GpuCoordinateOnlyResultWire) == 64);
static_assert(offsetof(GpuCoordinateOnlyResultWire, query_start) == 0);
static_assert(offsetof(GpuCoordinateOnlyResultWire, target_start) == 16);
static_assert(offsetof(GpuCoordinateOnlyResultWire, chain_score) == 48);
static_assert(offsetof(GpuCoordinateOnlyResultWire, valid) == 60);
static_assert(std::is_standard_layout_v<GpuCoordinateReducePush>);
static_assert(sizeof(GpuCoordinateReducePush) == 28);
static_assert(std::is_standard_layout_v<GpuAlignmentExtensionJobWire>);
static_assert(sizeof(GpuAlignmentExtensionJobWire) == 32);
static_assert(offsetof(GpuAlignmentExtensionJobWire, query_offset) == 0);
static_assert(offsetof(GpuAlignmentExtensionJobWire, target_offset) == 4);
static_assert(offsetof(GpuAlignmentExtensionJobWire, query_len) == 8);
static_assert(offsetof(GpuAlignmentExtensionJobWire, target_len) == 12);
static_assert(offsetof(GpuAlignmentExtensionJobWire, band_width) == 16);
static_assert(offsetof(GpuAlignmentExtensionJobWire, flags) == 20);
static_assert(offsetof(GpuAlignmentExtensionJobWire, reserved0) == 24);
static_assert(offsetof(GpuAlignmentExtensionJobWire, reserved1) == 28);
static_assert(std::is_standard_layout_v<GpuAlignmentExtensionResultWire>);
static_assert(sizeof(GpuAlignmentExtensionResultWire) == 16);
static_assert(offsetof(GpuAlignmentExtensionResultWire, score) == 0);
static_assert(offsetof(GpuAlignmentExtensionResultWire, query_end) == 4);
static_assert(offsetof(GpuAlignmentExtensionResultWire, target_end) == 8);
static_assert(offsetof(GpuAlignmentExtensionResultWire, status) == 12);
static_assert(std::is_standard_layout_v<GpuAlignmentExtensionParams>);
static_assert(sizeof(GpuAlignmentExtensionParams) == 40);
static_assert(std::is_standard_layout_v<GpuSneakySnakeJobWire>);
static_assert(sizeof(GpuSneakySnakeJobWire) == 20);
static_assert(offsetof(GpuSneakySnakeJobWire, query_offset) == 0);
static_assert(offsetof(GpuSneakySnakeJobWire, target_offset) == 4);
static_assert(offsetof(GpuSneakySnakeJobWire, query_length) == 8);
static_assert(offsetof(GpuSneakySnakeJobWire, target_length) == 12);
static_assert(offsetof(GpuSneakySnakeJobWire, max_edits) == 16);
static_assert(std::is_standard_layout_v<GpuSneakySnakeResultWire>);
static_assert(sizeof(GpuSneakySnakeResultWire) == 16);
static_assert(offsetof(GpuSneakySnakeResultWire, lower_bound) == 0);
static_assert(offsetof(GpuSneakySnakeResultWire, status) == 4);
static_assert(offsetof(GpuSneakySnakeResultWire, accepted) == 8);
static_assert(offsetof(GpuSneakySnakeResultWire, reserved) == 12);
static_assert(std::is_standard_layout_v<GpuSneakySnakePush>);
static_assert(sizeof(GpuSneakySnakePush) == 8);
static_assert(offsetof(GpuSneakySnakePush, job_count) == 0);
static_assert(offsetof(GpuSneakySnakePush, team_size) == 4);
static_assert(std::is_standard_layout_v<GpuAlignmentCigarJobWire>);
static_assert(sizeof(GpuAlignmentCigarJobWire) == 48);
static_assert(offsetof(GpuAlignmentCigarJobWire, query_symbol_offset) == 0);
static_assert(offsetof(GpuAlignmentCigarJobWire, target_symbol_offset) == 4);
static_assert(offsetof(GpuAlignmentCigarJobWire, query_len) == 8);
static_assert(offsetof(GpuAlignmentCigarJobWire, target_len) == 12);
static_assert(offsetof(GpuAlignmentCigarJobWire, query_global_start) == 16);
static_assert(offsetof(GpuAlignmentCigarJobWire, target_global_start) == 20);
static_assert(offsetof(GpuAlignmentCigarJobWire, band_width) == 24);
static_assert(offsetof(GpuAlignmentCigarJobWire, flags) == 28);
static_assert(offsetof(GpuAlignmentCigarJobWire, cigar_offset) == 32);
static_assert(offsetof(GpuAlignmentCigarJobWire, cigar_capacity) == 36);
static_assert(offsetof(GpuAlignmentCigarJobWire, traceback_tile_offset) == 40);
static_assert(offsetof(GpuAlignmentCigarJobWire, traceback_tile_count) == 44);
static_assert(std::is_standard_layout_v<GpuAlignmentCigarResultWire>);
static_assert(sizeof(GpuAlignmentCigarResultWire) == 40);
static_assert(offsetof(GpuAlignmentCigarResultWire, score) == 0);
static_assert(offsetof(GpuAlignmentCigarResultWire, max_score) == 4);
static_assert(offsetof(GpuAlignmentCigarResultWire, query_start) == 8);
static_assert(offsetof(GpuAlignmentCigarResultWire, query_end) == 12);
static_assert(offsetof(GpuAlignmentCigarResultWire, target_start) == 16);
static_assert(offsetof(GpuAlignmentCigarResultWire, target_end) == 20);
static_assert(offsetof(GpuAlignmentCigarResultWire, cigar_offset) == 24);
static_assert(offsetof(GpuAlignmentCigarResultWire, cigar_length) == 28);
static_assert(offsetof(GpuAlignmentCigarResultWire, status) == 32);
static_assert(offsetof(GpuAlignmentCigarResultWire, reserved) == 36);
static_assert(std::is_standard_layout_v<GpuAlignmentCigarParams>);
static_assert(sizeof(GpuAlignmentCigarParams) == 56);

} // namespace bioseq::gpu::vulkan
