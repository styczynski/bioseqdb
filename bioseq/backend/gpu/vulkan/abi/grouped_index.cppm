/**
 * @file vulkan/abi/grouped_index.cppm
 * @brief GPU layouts for grouped indexes with separate hash, count, and position arrays.
 *
 * Partition descriptions locate the arrays already loaded into GPU buffers.
 * Static assertions keep their sizes and field offsets equal to the shader
 * layouts.
 */
module;

#include <cstddef>

export module bioseq.gpu.vulkan.abi:grouped_index;

import std;

export namespace bioseq::gpu::vulkan {

// Grouped-SoA buffers keep the stored field widths. Positions encode the
// segment-local coordinate and strand as `(bp << 1) | strand`. Loading adjusts
// array offsets for the combined GPU buffers while preserving the stored bytes.
/** @brief Version of the grouped-SoA layout kept in GPU memory. */
inline constexpr std::uint32_t k_gpu_grouped_soa_abi_revision = 3u;
/** @brief Version of the seed-shader parameters. Saved grouped indexes use revision 3. */
inline constexpr std::uint32_t k_gpu_grouped_soa_seed_push_abi_revision = 6u;
/** @brief Version of the Format-5 control-then-position stream layout. */
inline constexpr std::uint32_t k_gpu_shape_v5_stream_abi_revision = 3u;
/** @brief Control arrays use their original grouped-SoA encoding. */
inline constexpr std::uint32_t k_gpu_grouped_soa_partition_raw_soa = 1u << 0u;
/** @brief Position values are relative to their segment. */
inline constexpr std::uint32_t k_gpu_grouped_soa_partition_segment_local = 1u << 1u;
/** @brief Prefix offsets include an extra entry marking the end of the last bucket. */
inline constexpr std::uint32_t k_gpu_grouped_soa_partition_terminal_prefix = 1u << 2u;
/** @brief Flags required for every grouped-SoA partition. */
inline constexpr std::uint32_t k_gpu_grouped_soa_partition_required_flags =
    k_gpu_grouped_soa_partition_raw_soa | k_gpu_grouped_soa_partition_segment_local |
    k_gpu_grouped_soa_partition_terminal_prefix;

/**
 * @brief Error bits written by grouped-SoA seed shaders.
 *
 * Any set bit makes the result invalid. The status word follows the query
 * counts and is excluded from their prefix scan.
 */
enum class gpu_grouped_soa_seed_status : std::uint32_t {
    none = 0u,           ///< No seed-shader failure recorded.
    contract = 1u << 0u, ///< Invalid shader parameters or buffer limits.
    query = 1u << 1u,    ///< Query minimizer stream or count invalid.
    control = 1u << 2u,  ///< Control stream decode or bounds failure.
    position = 1u << 3u, ///< Position stream decode or bounds failure.
    emission = 1u << 4u, ///< Anchor or count emission capacity exceeded.
};

/** @brief Selects the control-array pass or the position-array pass. */
enum class gpu_shape_v5_group_phase : std::uint32_t {
    controls = 1u,
    positions = 2u,
};

/** @brief Error bits shared by the control, approval, and position shaders. */
enum class gpu_shape_v5_stream_status : std::uint32_t {
    none = 0u,
    contract = 1u << 0u,
    query = 1u << 1u,
    control = 1u << 2u,
    position = 1u << 3u,
    authorization = 1u << 4u,
    emission = 1u << 5u,
};

[[nodiscard]] constexpr std::uint32_t
encode_gpu_shape_v5_stream_status(gpu_shape_v5_stream_status status) noexcept {
    return static_cast<std::uint32_t>(status);
}

/**
 * @brief Describes one Format-5 partition in the currently bound buffer.
 *
 * During the control pass, `hash_array_offset` points to hashes. During the
 * position pass, it points to positions and the prefix/count offsets must be
 * zero. All offsets are relative to this buffer slot.
 */
struct alignas(8) gpu_shape_v5_partition_wire {
    std::uint32_t partition_id = 0u;
    std::uint32_t directory_ordinal = 0u;
    std::uint32_t position_segment = 0u;
    std::uint32_t control_count = 0u;
    std::uint32_t position_count = 0u;
    std::uint32_t hash_array_offset = 0u;
    std::uint32_t prefix_offset_array_offset = 0u;
    std::uint32_t count_array_offset = 0u;
    std::uint32_t hash_width = 0u;
    std::uint32_t offset_width = 0u;
    std::uint32_t count_width = 0u;
    std::uint32_t position_width = 0u;
    std::uint64_t segment_base = 0u;
    std::uint64_t segment_end = 0u;
    std::uint64_t max_hash = 0u;
    std::uint64_t max_occ_cap = 0u;
};

/** @brief Query-minimizer record for indexes with 64-bit hashes. */
struct alignas(8) gpu_shape_v5_query_minimizer_h64_wire {
    std::uint64_t hash = 0u;
    std::uint32_t query_bp_strand = 0u;
    std::uint32_t occurrence_index = 0u;
    std::uint32_t query_record_index = 0u;
    std::uint32_t reserved_zero = 0u;
};

/** @brief Occurrence counts and output cursor kept between the two passes. */
struct gpu_shape_v5_query_authorization_wire {
    std::uint32_t occurrence_count = 0u;
    std::uint32_t accepted_count = 0u;
    std::uint32_t emission_cursor = 0u;
    std::uint32_t reserved_zero = 0u;
};

/** @brief Position range found by the control pass for a query minimizer. */
struct alignas(8) gpu_shape_v5_authorization_range_wire {
    std::uint32_t directory_ordinal = 0u;
    std::uint32_t position_segment = 0u;
    std::uint32_t query_record_index = 0u;
    std::uint32_t query_minimizer_index = 0u;
    std::uint32_t position_begin = 0u;
    std::uint32_t position_count = 0u;
    std::uint32_t status = 0u;
    std::uint32_t reserved_zero = 0u;
};

/**
 * @brief CPU-side buffer references and limits for one control or position group.
 *
 * The pointers refer to runtime buffer objects. Keeping their types opaque
 * lets this layout module avoid importing the Vulkan runtime.
 */
struct gpu_shape_v5_raw_group_inputs {
    const void* descriptors = nullptr;
    const void* payload = nullptr;
    std::uint32_t descriptor_count = 0u;
    std::uint32_t payload_bytes = 0u;
    std::uint32_t group_ordinal = 0u;
    gpu_shape_v5_group_phase phase = gpu_shape_v5_group_phase::controls;
    std::uint64_t receipt_identity = 0u;
};

/** @brief Push constants shared by control and position group pipelines. */
struct gpu_shape_v5_group_push {
    std::uint32_t abi_revision = k_gpu_shape_v5_stream_abi_revision;
    std::uint32_t phase = 0u;
    std::uint32_t group_ordinal = 0u;
    std::uint32_t descriptor_count = 0u;
    std::uint32_t payload_bytes = 0u;
    std::uint32_t words32_per_minimizer = 0u;
    std::uint32_t word_kind = 0u;
    std::uint32_t hash_shift = 0u;
    std::uint32_t pos_bits = 0u;
    std::uint32_t kmer_size = 0u;
    std::uint32_t query_minimizer_count = 0u;
    std::uint32_t query_record_count = 0u;
    std::uint32_t max_occ = 0u;
    std::uint32_t target_ref_bits = 0u;
    std::uint32_t minimizer_word_extent = 0u;
    std::uint32_t descriptor_extent = 0u;
    std::uint32_t payload_word_extent = 0u;
    std::uint32_t query_record_extent = 0u;
    std::uint32_t query_state_extent = 0u;
    std::uint32_t authorization_range_extent = 0u;
    std::uint32_t counter_extent = 0u;
    std::uint32_t authorization_count_index = 0u;
    std::uint32_t anchor_count_index = 0u;
    std::uint32_t status_index = 0u;
    std::uint32_t prefix_extent = 0u;
    std::uint32_t anchor_extent = 0u;
    std::uint32_t anchor_key_extent = 0u;
    std::uint32_t accepted_count_extent = 0u;
    std::uint32_t query_wire_kind = 0u;
    std::uint32_t h64_query_extent = 0u;
    std::uint32_t reserved_zero = 0u;
};

/** @brief Parameters for deciding which minimizers may emit positions after counting. */
struct gpu_shape_v5_authorization_push {
    std::uint32_t abi_revision = k_gpu_shape_v5_stream_abi_revision;
    std::uint32_t query_minimizer_count = 0u;
    std::uint32_t max_occ = 0u;
    std::uint32_t query_state_extent = 0u;
    std::uint32_t counter_extent = 0u;
    std::uint32_t status_index = 0u;
    std::uint32_t accepted_count_extent = 0u;
    std::uint32_t reserved_zero0 = 0u;
    std::uint32_t reserved_zero1 = 0u;
};

/** @brief Submission details saved after a GPU buffer slot is reused. */
struct alignas(8) gpu_persisted_group_capture_wire {
    std::uint32_t phase = 0u;
    std::uint32_t group_ordinal = 0u;
    std::uint32_t slot_ordinal = 0u;
    std::uint32_t descriptor_count = 0u;
    std::uint64_t allocation_generation = 0u;
    std::uint64_t receipt_identity = 0u;
    std::uint64_t runtime_generation = 0u;
    std::uint64_t submission_identity = 0u;
    std::uint64_t signal_value = 0u;
    std::uint64_t logical_item_count = 0u;
    std::uint64_t payload_bytes = 0u;
    std::uint32_t first_directory_ordinal = 0u;
    std::uint32_t end_directory_ordinal = 0u;
    std::uint32_t status = 0u;
    std::uint32_t reserved_zero0 = 0u;
    std::array<std::byte, 32> payload_sha256{};
    std::array<std::byte, 32> capture_sha256{};
    std::uint64_t reserved_zero1 = 0u;
};

static_assert(sizeof(gpu_shape_v5_partition_wire) == 80u);
static_assert(offsetof(gpu_shape_v5_partition_wire, partition_id) == 0u);
static_assert(offsetof(gpu_shape_v5_partition_wire, hash_array_offset) == 20u);
static_assert(offsetof(gpu_shape_v5_partition_wire, segment_base) == 48u);
static_assert(offsetof(gpu_shape_v5_partition_wire, max_occ_cap) == 72u);
static_assert(std::is_trivially_copyable_v<gpu_shape_v5_partition_wire>);
static_assert(sizeof(gpu_shape_v5_query_minimizer_h64_wire) == 24u);
static_assert(offsetof(gpu_shape_v5_query_minimizer_h64_wire, query_record_index) == 16u);
static_assert(sizeof(gpu_shape_v5_query_authorization_wire) == 16u);
static_assert(sizeof(gpu_shape_v5_authorization_range_wire) == 32u);
static_assert(std::is_trivially_copyable_v<gpu_shape_v5_authorization_range_wire>);
static_assert(std::is_standard_layout_v<gpu_shape_v5_raw_group_inputs>);
static_assert(sizeof(gpu_shape_v5_group_push) == 124u);
static_assert(sizeof(gpu_shape_v5_group_push) <= 128u);
static_assert(offsetof(gpu_shape_v5_group_push, phase) == 4u);
static_assert(offsetof(gpu_shape_v5_group_push, payload_bytes) == 16u);
static_assert(offsetof(gpu_shape_v5_group_push, minimizer_word_extent) == 56u);
static_assert(offsetof(gpu_shape_v5_group_push, authorization_range_extent) == 76u);
static_assert(offsetof(gpu_shape_v5_group_push, accepted_count_extent) == 108u);
static_assert(offsetof(gpu_shape_v5_group_push, query_wire_kind) == 112u);
static_assert(offsetof(gpu_shape_v5_group_push, h64_query_extent) == 116u);
static_assert(sizeof(gpu_shape_v5_authorization_push) == 36u);
static_assert(offsetof(gpu_shape_v5_authorization_push, accepted_count_extent) == 24u);
static_assert(sizeof(gpu_persisted_group_capture_wire) == 160u);
static_assert(offsetof(gpu_persisted_group_capture_wire, allocation_generation) == 16u);
static_assert(offsetof(gpu_persisted_group_capture_wire, payload_sha256) == 88u);
static_assert(offsetof(gpu_persisted_group_capture_wire, capture_sha256) == 120u);
static_assert(offsetof(gpu_persisted_group_capture_wire, reserved_zero1) == 152u);

/**
 * @brief Converts a seed error flag to the unsigned mask stored by the shader.
 */
[[nodiscard]] constexpr std::uint32_t
encode_gpu_grouped_soa_seed_status(gpu_grouped_soa_seed_status status) noexcept {
    return static_cast<std::uint32_t>(status);
}

/**
 * @brief Locates one grouped-SoA partition in the loaded GPU buffers.
 *
 * Offsets are measured in bytes from the start of the combined control or
 * position buffer. Field widths may be 2, 4, or 8 bytes. The prefix array has
 * one extra entry marking the end of the last bucket.
 */
struct gpu_grouped_soa_resident_partition_descriptor {
    std::uint32_t partition_id = 0;         ///< Partition identifier copied from the saved directory.
    std::uint32_t directory_ordinal = 0;    ///< Index of this partition in the directory.
    std::uint32_t position_segment = 0;     ///< Segment index for segment-local positions.
    std::uint32_t flags = 0;                ///< Required grouped-SoA partition flag mask.
    std::uint64_t hash_byte_offset = 0;     ///< First hash scalar byte in the control buffer.
    std::uint64_t prefix_byte_offset = 0;   ///< Byte offset of bucket starts, including the final end offset.
    std::uint64_t count_byte_offset = 0;    ///< Saturated-count array byte offset in controls.
    std::uint64_t position_byte_offset = 0; ///< First position scalar byte in the position buffer.
    std::uint32_t control_count = 0;        ///< Number of control buckets in this partition.
    std::uint32_t position_count = 0;       ///< Number of encoded positions in this partition.
    std::uint32_t hash_width = 0;           ///< Hash scalar width in bytes (2, 4, or 8).
    std::uint32_t offset_width = 0;         ///< Prefix offset scalar width in bytes.
    std::uint32_t count_width = 0;          ///< Count scalar width in bytes.
    std::uint32_t position_width = 0;       ///< Position scalar width in bytes.
    std::uint64_t low_hash = 0;             ///< Inclusive lower hash bound for this partition.
    std::uint64_t high_hash = 0;            ///< Inclusive upper hash bound for this partition.
    std::uint64_t segment_base_bp = 0;      ///< Segment base in base pairs for position decode.
    std::uint64_t segment_end_bp = 0;       ///< Segment end in base pairs for position decode.
};

/**
 * @brief Parameters for counting seed hits and writing anchors from a grouped index.
 *
 * The query occurrence limit must not exceed the cap recorded in the saved index.
 */
struct gpu_grouped_soa_seed_push {
    std::uint32_t abi_revision =
        k_gpu_grouped_soa_seed_push_abi_revision; ///< Push-block revision checked by shaders.
    std::uint32_t words32_per_minimizer = 0;      ///< 32-bit words per query minimizer record.
    std::uint32_t word_kind = 0;             ///< Minimizer word encoding kind from query upload.
    std::uint32_t hash_shift = 0;            ///< Right shift applied before hash lookup.
    std::uint32_t pos_bits = 0;              ///< Position field width used during decode.
    std::uint32_t kmer_size = 0;             ///< Minimizer k-mer length in symbols.
    std::uint32_t partition_count = 0;       ///< Exclusive end partition index for this dispatch.
    std::uint32_t max_occ = 0;               ///< Query-side occurrence threshold for this dispatch.
    std::uint32_t emit_mode = 0;             ///< Zero counts hits. One writes anchors.
    std::uint32_t record_count = 0;          ///< Number of query records covered by the dispatch.
    std::uint32_t target_ref_bits = 0;       ///< Target coordinate bit width for anchor rebasing.
    std::uint32_t query_minimizer_count = 0; ///< Total query minimizers in the bound buffer.
    std::uint32_t control_word_count = 0;    ///< Combined control-buffer size in 32-bit words.
    std::uint32_t position_word_count = 0;   ///< Combined position-buffer size in 32-bit words.
    std::uint32_t anchor_capacity = 0;       ///< Maximum anchors writable by the emit pass.
    std::uint32_t max_occ_cap_lo = 0;        ///< Low 32 bits of the persisted occurrence cap.
    std::uint32_t max_occ_cap_hi = 0;        ///< High 32 bits of the persisted occurrence cap.
    // Query counts occupy [0, status_index). The extra word stores shader
    // errors and is excluded from the prefix scan.
    std::uint32_t status_index = 0; ///< Index of the shader error word after the query counts.
    // Element counts come from the first nine VkDescriptorBufferInfo ranges.
    // Cached Metal shaders use these instead of runtime-array length queries.
    std::uint32_t minimizer_word_extent = 0;
    std::uint32_t partition_extent = 0;
    std::uint32_t control_word_extent = 0;
    std::uint32_t position_word_extent = 0;
    std::uint32_t query_record_extent = 0;
    std::uint32_t count_extent = 0;
    std::uint32_t prefix_extent = 0;
    std::uint32_t anchor_extent = 0;
    std::uint32_t anchor_key_extent = 0;
    std::uint32_t partition_begin = 0; ///< First partition of this bounded dispatch.
    std::uint32_t control_base_word = 0; ///< Start of the bound control range in the combined buffer, in words.
    std::uint32_t position_base_word = 0; ///< Start of the bound position range in the combined buffer, in words.
    std::uint32_t repetitive_word_extent = 0; ///< Buffer length for repetitive flags and per-minimizer output cursors.
};

static_assert(sizeof(gpu_grouped_soa_resident_partition_descriptor) == 104u);
static_assert(alignof(gpu_grouped_soa_resident_partition_descriptor) == 8u);
static_assert(offsetof(gpu_grouped_soa_resident_partition_descriptor, hash_byte_offset) == 16u);
static_assert(offsetof(gpu_grouped_soa_resident_partition_descriptor, control_count) == 48u);
static_assert(offsetof(gpu_grouped_soa_resident_partition_descriptor, low_hash) == 72u);
static_assert(offsetof(gpu_grouped_soa_resident_partition_descriptor, segment_end_bp) == 96u);
static_assert(std::is_standard_layout_v<gpu_grouped_soa_resident_partition_descriptor>);
static_assert(sizeof(gpu_grouped_soa_seed_push) == 124u);
static_assert(sizeof(gpu_grouped_soa_seed_push) <= 128u);
static_assert(offsetof(gpu_grouped_soa_seed_push, abi_revision) == 0u);
static_assert(offsetof(gpu_grouped_soa_seed_push, words32_per_minimizer) == 4u);
static_assert(offsetof(gpu_grouped_soa_seed_push, word_kind) == 8u);
static_assert(offsetof(gpu_grouped_soa_seed_push, hash_shift) == 12u);
static_assert(offsetof(gpu_grouped_soa_seed_push, pos_bits) == 16u);
static_assert(offsetof(gpu_grouped_soa_seed_push, kmer_size) == 20u);
static_assert(offsetof(gpu_grouped_soa_seed_push, partition_count) == 24u);
static_assert(offsetof(gpu_grouped_soa_seed_push, max_occ) == 28u);
static_assert(offsetof(gpu_grouped_soa_seed_push, emit_mode) == 32u);
static_assert(offsetof(gpu_grouped_soa_seed_push, record_count) == 36u);
static_assert(offsetof(gpu_grouped_soa_seed_push, target_ref_bits) == 40u);
static_assert(offsetof(gpu_grouped_soa_seed_push, query_minimizer_count) == 44u);
static_assert(offsetof(gpu_grouped_soa_seed_push, control_word_count) == 48u);
static_assert(offsetof(gpu_grouped_soa_seed_push, position_word_count) == 52u);
static_assert(offsetof(gpu_grouped_soa_seed_push, anchor_capacity) == 56u);
static_assert(offsetof(gpu_grouped_soa_seed_push, max_occ_cap_lo) == 60u);
static_assert(offsetof(gpu_grouped_soa_seed_push, max_occ_cap_hi) == 64u);
static_assert(offsetof(gpu_grouped_soa_seed_push, status_index) == 68u);
static_assert(offsetof(gpu_grouped_soa_seed_push, minimizer_word_extent) == 72u);
static_assert(offsetof(gpu_grouped_soa_seed_push, partition_extent) == 76u);
static_assert(offsetof(gpu_grouped_soa_seed_push, control_word_extent) == 80u);
static_assert(offsetof(gpu_grouped_soa_seed_push, position_word_extent) == 84u);
static_assert(offsetof(gpu_grouped_soa_seed_push, query_record_extent) == 88u);
static_assert(offsetof(gpu_grouped_soa_seed_push, count_extent) == 92u);
static_assert(offsetof(gpu_grouped_soa_seed_push, prefix_extent) == 96u);
static_assert(offsetof(gpu_grouped_soa_seed_push, anchor_extent) == 100u);
static_assert(offsetof(gpu_grouped_soa_seed_push, anchor_key_extent) == 104u);
static_assert(offsetof(gpu_grouped_soa_seed_push, partition_begin) == 108u);
static_assert(offsetof(gpu_grouped_soa_seed_push, control_base_word) == 112u);
static_assert(offsetof(gpu_grouped_soa_seed_push, position_base_word) == 116u);
static_assert(offsetof(gpu_grouped_soa_seed_push, repetitive_word_extent) == 120u);

/**
 * @brief Reports whether a persisted grouped-SoA scalar width is supported.
 * @param[in] width Scalar width in bytes.
 * @return `true` for the two-, four-, and eight-byte wire widths.
 */
[[nodiscard]] constexpr bool is_gpu_grouped_soa_scalar_width_valid(std::uint32_t width) noexcept {
    return width == 2u || width == 4u || width == 8u;
}

/**
 * @brief Checks that a scalar array is aligned and fits inside its buffer.
 * @param[in] byte_offset Start of the array in bytes.
 * @param[in] element_count Number of values.
 * @param[in] width Bytes per value, which must be 2, 4, or 8.
 * @param[in] buffer_bytes Buffer size in bytes.
 *
 * Returns false for unsupported widths, misalignment, arithmetic overflow,
 * or a range beyond the buffer.
 */
[[nodiscard]] constexpr bool
is_gpu_grouped_soa_scalar_range_valid(std::uint64_t byte_offset,
                                      std::uint64_t element_count,
                                      std::uint32_t width,
                                      std::uint64_t buffer_bytes) noexcept {
    if (!is_gpu_grouped_soa_scalar_width_valid(width) || byte_offset % width != 0u ||
        byte_offset > buffer_bytes ||
        element_count > ((std::numeric_limits<std::uint64_t>::max)() - byte_offset) / width) {
        return false;
    }
    return element_count * width <= buffer_bytes - byte_offset;
}

/** @brief Reports whether a value names a legal persisted stream phase. */
[[nodiscard]] constexpr bool
is_gpu_shape_v5_group_phase_valid(gpu_shape_v5_group_phase phase) noexcept {
    return phase == gpu_shape_v5_group_phase::controls ||
           phase == gpu_shape_v5_group_phase::positions;
}

/** @brief Largest unsigned value that fits in the given supported byte width. */
[[nodiscard]] constexpr std::uint64_t gpu_shape_v5_scalar_max(std::uint32_t width) noexcept {
    return width == 8u ? (std::numeric_limits<std::uint64_t>::max)()
                       : (std::uint64_t{1u} << (width * 8u)) - 1u;
}

/** @brief Checks buffer pointers, non-empty counts, phase, and receipt identity. */
[[nodiscard]] constexpr bool
is_gpu_shape_v5_raw_group_inputs_valid(const gpu_shape_v5_raw_group_inputs& inputs) noexcept {
    return inputs.descriptors != nullptr && inputs.payload != nullptr &&
           inputs.descriptor_count != 0u && inputs.payload_bytes != 0u &&
           is_gpu_shape_v5_group_phase_valid(inputs.phase) && inputs.receipt_identity != 0u;
}

/**
 * @brief Checks a partition against the buffer for the requested pass.
 *
 * The caller supplies the phase. Control passes validate hash, prefix, and
 * count arrays. Position passes validate the position array and require the
 * unused offsets to be zero. Widths must fit the recorded counts and segment.
 */
[[nodiscard]] constexpr bool
is_gpu_shape_v5_partition_wire_valid(const gpu_shape_v5_partition_wire& descriptor,
                                     gpu_shape_v5_group_phase phase,
                                     std::uint32_t payload_bytes) noexcept {
    if (!is_gpu_shape_v5_group_phase_valid(phase) ||
        descriptor.partition_id != descriptor.directory_ordinal || descriptor.control_count == 0u ||
        descriptor.position_count == 0u || descriptor.segment_base >= descriptor.segment_end ||
        descriptor.max_occ_cap == (std::numeric_limits<std::uint64_t>::max)() ||
        !is_gpu_grouped_soa_scalar_width_valid(descriptor.hash_width) ||
        !is_gpu_grouped_soa_scalar_width_valid(descriptor.offset_width) ||
        !is_gpu_grouped_soa_scalar_width_valid(descriptor.count_width) ||
        !is_gpu_grouped_soa_scalar_width_valid(descriptor.position_width)) {
        return false;
    }
    if (descriptor.max_hash > gpu_shape_v5_scalar_max(descriptor.hash_width) ||
        descriptor.position_count > gpu_shape_v5_scalar_max(descriptor.offset_width) ||
        descriptor.max_occ_cap >= gpu_shape_v5_scalar_max(descriptor.count_width)) {
        return false;
    }
    const std::uint64_t position_bits = descriptor.position_width * 8u;
    const std::uint64_t maximum_segment_span = std::uint64_t{1u} << (position_bits - 1u);
    if (descriptor.segment_end - descriptor.segment_base > maximum_segment_span) {
        return false;
    }
    if (phase == gpu_shape_v5_group_phase::positions) {
        return descriptor.prefix_offset_array_offset == 0u && descriptor.count_array_offset == 0u &&
               is_gpu_grouped_soa_scalar_range_valid(descriptor.hash_array_offset,
                                                     descriptor.position_count,
                                                     descriptor.position_width,
                                                     payload_bytes);
    }
    return is_gpu_grouped_soa_scalar_range_valid(descriptor.hash_array_offset,
                                                 descriptor.control_count,
                                                 descriptor.hash_width,
                                                 payload_bytes) &&
           is_gpu_grouped_soa_scalar_range_valid(
               descriptor.prefix_offset_array_offset,
               static_cast<std::uint64_t>(descriptor.control_count) + 1u,
               descriptor.offset_width,
               payload_bytes) &&
           is_gpu_grouped_soa_scalar_range_valid(descriptor.count_array_offset,
                                                 descriptor.control_count,
                                                 descriptor.count_width,
                                                 payload_bytes);
}

/**
 * @brief Checks that a partition can be read from the combined GPU buffers.
 *
 * Requires the expected flags, supported field widths, ordered hash and
 * segment bounds, and aligned arrays that fit inside their buffers. The
 * segment length must fit in the position encoding.
 */
[[nodiscard]] constexpr bool is_gpu_grouped_soa_partition_descriptor_valid(
    const gpu_grouped_soa_resident_partition_descriptor& descriptor,
    std::uint64_t control_buffer_bytes,
    std::uint64_t position_buffer_bytes) noexcept {
    if (descriptor.flags != k_gpu_grouped_soa_partition_required_flags ||
        descriptor.control_count == 0u || descriptor.position_count == 0u ||
        descriptor.low_hash > descriptor.high_hash ||
        descriptor.segment_base_bp >= descriptor.segment_end_bp ||
        !is_gpu_grouped_soa_scalar_width_valid(descriptor.hash_width) ||
        !is_gpu_grouped_soa_scalar_width_valid(descriptor.offset_width) ||
        !is_gpu_grouped_soa_scalar_width_valid(descriptor.count_width) ||
        !is_gpu_grouped_soa_scalar_width_valid(descriptor.position_width)) {
        return false;
    }
    const std::uint64_t position_bits = descriptor.position_width * 8u;
    const std::uint64_t maximum_segment_span = std::uint64_t{1} << (position_bits - 1u);
    return descriptor.segment_end_bp - descriptor.segment_base_bp <= maximum_segment_span &&
           is_gpu_grouped_soa_scalar_range_valid(descriptor.hash_byte_offset,
                                                 descriptor.control_count,
                                                 descriptor.hash_width,
                                                 control_buffer_bytes) &&
           is_gpu_grouped_soa_scalar_range_valid(
               descriptor.prefix_byte_offset,
               static_cast<std::uint64_t>(descriptor.control_count) + 1u,
               descriptor.offset_width,
               control_buffer_bytes) &&
           is_gpu_grouped_soa_scalar_range_valid(descriptor.count_byte_offset,
                                                 descriptor.control_count,
                                                 descriptor.count_width,
                                                 control_buffer_bytes) &&
           is_gpu_grouped_soa_scalar_range_valid(descriptor.position_byte_offset,
                                                 descriptor.position_count,
                                                 descriptor.position_width,
                                                 position_buffer_bytes);
}

/// @brief Consecutive partitions covered by aligned control and position buffer ranges.
struct gpu_grouped_soa_buffer_window {
    std::uint32_t partition_begin = 0u;
    std::uint32_t partition_end = 0u;
    std::uint64_t control_offset = 0u;
    std::uint64_t control_bytes = 0u;
    std::uint64_t position_offset = 0u;
    std::uint64_t position_bytes = 0u;
};

/// @brief Finds the largest next group of whole partitions that fits the buffer limits.
/// @param[in] descriptors Stored partition descriptors.
/// @param[in] first First partition not yet covered.
/// @param[in] control_bytes Combined control-buffer size in bytes.
/// @param[in] position_bytes Combined position-buffer size in bytes.
/// @param[in] range_limit Maximum bytes in one storage-buffer binding.
/// @param[in] offset_alignment Required alignment of binding offsets.
/// @return Consecutive partition window, or no value for invalid input or
///         a first partition that cannot fit.
///
/// This reads the descriptors without decoding the index arrays.
[[nodiscard]] inline std::optional<gpu_grouped_soa_buffer_window>
plan_gpu_grouped_soa_buffer_window(std::span<const std::byte> descriptors,
                                   std::uint32_t first,
                                   std::uint64_t control_bytes,
                                   std::uint64_t position_bytes,
                                   std::uint64_t range_limit,
                                   std::uint64_t offset_alignment) noexcept {
    constexpr std::uint64_t stride = sizeof(gpu_grouped_soa_resident_partition_descriptor);
    const std::uint64_t count = descriptors.size() / stride;
    const std::uint64_t alignment = std::max<std::uint64_t>(4u, offset_alignment);
    constexpr std::uint64_t word_limit =
        std::uint64_t{(std::numeric_limits<std::uint32_t>::max)()} * 4u;
    if (descriptors.size() % stride != 0u || first >= count ||
        count > (std::numeric_limits<std::uint32_t>::max)() ||
        !std::has_single_bit(alignment) || range_limit < 4u ||
        control_bytes > word_limit || position_bytes > word_limit) {
        return std::nullopt;
    }
    gpu_grouped_soa_buffer_window window{.partition_begin = first, .partition_end = first};
    std::uint64_t control_end = 0u;
    std::uint64_t position_end = 0u;
    for (std::uint64_t ordinal = first; ordinal < count; ++ordinal) {
        gpu_grouped_soa_resident_partition_descriptor descriptor{};
        std::memcpy(&descriptor, descriptors.data() + ordinal * stride, sizeof(descriptor));
        if (descriptor.partition_id != ordinal || descriptor.directory_ordinal != ordinal ||
            !is_gpu_grouped_soa_partition_descriptor_valid(
                descriptor, control_bytes, position_bytes)) {
            return std::nullopt;
        }
        const std::uint64_t begin_control = std::min({descriptor.hash_byte_offset,
            descriptor.prefix_byte_offset, descriptor.count_byte_offset}) & ~(alignment - 1u);
        const std::uint64_t begin_position = descriptor.position_byte_offset & ~(alignment - 1u);
        const std::uint64_t end_control = (std::max({
            descriptor.hash_byte_offset + std::uint64_t{descriptor.control_count} * descriptor.hash_width,
            descriptor.prefix_byte_offset + (std::uint64_t{descriptor.control_count} + 1u) * descriptor.offset_width,
            descriptor.count_byte_offset + std::uint64_t{descriptor.control_count} * descriptor.count_width}) + 3u) & ~std::uint64_t{3u};
        const std::uint64_t end_position = (descriptor.position_byte_offset +
            std::uint64_t{descriptor.position_count} * descriptor.position_width + 3u) & ~std::uint64_t{3u};
        const std::uint64_t next_control_begin = ordinal == first ? begin_control : std::min(window.control_offset, begin_control);
        const std::uint64_t next_position_begin = ordinal == first ? begin_position : std::min(window.position_offset, begin_position);
        const std::uint64_t next_control_end = std::max(control_end, end_control);
        const std::uint64_t next_position_end = std::max(position_end, end_position);
        if (next_control_end - next_control_begin > range_limit ||
            next_position_end - next_position_begin > range_limit) {
            return ordinal == first ? std::nullopt : std::optional{window};
        }
        window.control_offset = next_control_begin;
        window.position_offset = next_position_begin;
        control_end = next_control_end;
        position_end = next_position_end;
        window.control_bytes = control_end - window.control_offset;
        window.position_bytes = position_end - window.position_offset;
        window.partition_end = static_cast<std::uint32_t>(ordinal + 1u);
    }
    return window;
}

} // namespace bioseq::gpu::vulkan
