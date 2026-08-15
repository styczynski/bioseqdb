/**
 * @file
 * @brief Defines the retained grouped-SoA resident-index Vulkan ABI.
 */
module;

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

export module bioseq.gpu.vulkan.abi:grouped_index;

export namespace bioseq::gpu::vulkan {

// Grouped-SoA controls retain their authenticated variable-width byte stream,
// and positions remain segment-local `(bp << 1) | strand` scalars. A resident
// loader rebases only aggregate-buffer offsets; it never decodes controls,
// widens positions, or rebuilds a host hash table.
/** @brief Wire revision of the retained grouped-SoA resident representation. */
inline constexpr std::uint32_t k_gpu_grouped_soa_abi_revision = 3u;
/** @brief Flag identifying untranscoded grouped-SoA control streams. */
inline constexpr std::uint32_t k_gpu_grouped_soa_partition_raw_soa = 1u << 0u;
/** @brief Flag identifying segment-local encoded position scalars. */
inline constexpr std::uint32_t
    k_gpu_grouped_soa_partition_segment_local = 1u << 1u;
/** @brief Flag identifying terminal-prefix control arrays. */
inline constexpr std::uint32_t
    k_gpu_grouped_soa_partition_terminal_prefix = 1u << 2u;
/** @brief Flags required on every admitted grouped-SoA partition. */
inline constexpr std::uint32_t k_gpu_grouped_soa_partition_required_flags =
    k_gpu_grouped_soa_partition_raw_soa
    | k_gpu_grouped_soa_partition_segment_local
    | k_gpu_grouped_soa_partition_terminal_prefix;

/**
 * @brief Defines fail-closed status bits written by grouped-SoA seed shaders.
 *
 * The status word is outside the query-local prefix-scan range.
 */
enum class gpu_grouped_soa_seed_status : std::uint32_t {
    none = 0u,
    contract = 1u << 0u,
    query = 1u << 1u,
    control = 1u << 2u,
    position = 1u << 3u,
    emission = 1u << 4u,
};

/**
 * @brief Converts a grouped-SoA seed status to its shader-visible bit mask.
 * @param[in] status Status bit to encode.
 * @return Shader-visible unsigned mask.
 */
[[nodiscard]] constexpr std::uint32_t encode_gpu_grouped_soa_seed_status(
    gpu_grouped_soa_seed_status status) noexcept
{
    return static_cast<std::uint32_t>(status);
}

/**
 * @brief Describes one admitted grouped-SoA partition in resident buffers.
 *
 * Byte offsets are absolute within aggregate control and position buffers.
 * Scalar widths are two, four, or eight bytes, and the four offsets address
 * hash, terminal-prefix, saturated-count, and raw-position arrays.
 */
struct gpu_grouped_soa_resident_partition_descriptor {
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

/**
 * @brief Carries grouped-SoA resident seed count and emit parameters.
 *
 * The persisted occurrence cap is independent of the query threshold. A seed
 * dispatch is legal only when the query threshold does not exceed that cap.
 */
struct gpu_grouped_soa_seed_push {
    std::uint32_t abi_revision = k_gpu_grouped_soa_abi_revision;
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
    // Query-local counts occupy [0, status_index); the final extra word is
    // reserved for fail-closed shader status and is never part of prefix scan.
    std::uint32_t status_index = 0;
};

static_assert(sizeof(gpu_grouped_soa_resident_partition_descriptor) == 104u);
static_assert(alignof(gpu_grouped_soa_resident_partition_descriptor) == 8u);
static_assert(offsetof(
    gpu_grouped_soa_resident_partition_descriptor, hash_byte_offset) == 16u);
static_assert(offsetof(
    gpu_grouped_soa_resident_partition_descriptor, control_count) == 48u);
static_assert(offsetof(
    gpu_grouped_soa_resident_partition_descriptor, low_hash) == 72u);
static_assert(offsetof(
    gpu_grouped_soa_resident_partition_descriptor, segment_end_bp) == 96u);
static_assert(std::is_standard_layout_v<
    gpu_grouped_soa_resident_partition_descriptor>);
static_assert(sizeof(gpu_grouped_soa_seed_push) == 72u);
static_assert(offsetof(gpu_grouped_soa_seed_push, abi_revision) == 0u);
static_assert(offsetof(gpu_grouped_soa_seed_push, partition_count) == 24u);
static_assert(offsetof(gpu_grouped_soa_seed_push, query_minimizer_count) == 44u);
static_assert(offsetof(gpu_grouped_soa_seed_push, max_occ_cap_lo) == 60u);
static_assert(offsetof(gpu_grouped_soa_seed_push, status_index) == 68u);

/**
 * @brief Reports whether a persisted grouped-SoA scalar width is supported.
 * @param[in] width Scalar width in bytes.
 * @return `true` for the two-, four-, and eight-byte wire widths.
 */
[[nodiscard]] constexpr bool is_gpu_grouped_soa_scalar_width_valid(
    std::uint32_t width) noexcept
{
    return width == 2u || width == 4u || width == 8u;
}

/**
 * @brief Reports whether an aligned scalar range fits its aggregate buffer.
 * @param[in] byte_offset First scalar byte within the aggregate buffer.
 * @param[in] element_count Number of fixed-width scalars in the range.
 * @param[in] width Scalar width in bytes.
 * @param[in] buffer_bytes Aggregate buffer extent in bytes.
 * @return `true` when the aligned range is representable and in bounds.
 */
[[nodiscard]] constexpr bool is_gpu_grouped_soa_scalar_range_valid(
    std::uint64_t byte_offset,
    std::uint64_t element_count,
    std::uint32_t width,
    std::uint64_t buffer_bytes) noexcept
{
    if (!is_gpu_grouped_soa_scalar_width_valid(width)
        || byte_offset % width != 0u
        || byte_offset > buffer_bytes
        || element_count
            > ((std::numeric_limits<std::uint64_t>::max)() - byte_offset)
                / width) {
        return false;
    }
    return element_count * width <= buffer_bytes - byte_offset;
}

/**
 * @brief Validates a resident descriptor against aggregate buffer extents.
 * @param[in] descriptor Partition descriptor to validate.
 * @param[in] control_buffer_bytes Aggregate control-buffer extent in bytes.
 * @param[in] position_buffer_bytes Aggregate position-buffer extent in bytes.
 * @return `true` when flags, widths, hash bounds, segment bounds, and all byte
 * ranges satisfy the grouped-SoA upload contract.
 */
[[nodiscard]] constexpr bool is_gpu_grouped_soa_partition_descriptor_valid(
    const gpu_grouped_soa_resident_partition_descriptor& descriptor,
    std::uint64_t control_buffer_bytes,
    std::uint64_t position_buffer_bytes) noexcept
{
    if (descriptor.flags != k_gpu_grouped_soa_partition_required_flags
        || descriptor.control_count == 0u
        || descriptor.position_count == 0u
        || descriptor.low_hash > descriptor.high_hash
        || descriptor.segment_base_bp >= descriptor.segment_end_bp
        || !is_gpu_grouped_soa_scalar_width_valid(descriptor.hash_width)
        || !is_gpu_grouped_soa_scalar_width_valid(descriptor.offset_width)
        || !is_gpu_grouped_soa_scalar_width_valid(descriptor.count_width)
        || !is_gpu_grouped_soa_scalar_width_valid(descriptor.position_width)) {
        return false;
    }
    const std::uint64_t position_bits = descriptor.position_width * 8u;
    const std::uint64_t maximum_segment_span =
        std::uint64_t{1} << (position_bits - 1u);
    return descriptor.segment_end_bp - descriptor.segment_base_bp
            <= maximum_segment_span
        && is_gpu_grouped_soa_scalar_range_valid(
            descriptor.hash_byte_offset,
            descriptor.control_count,
            descriptor.hash_width,
            control_buffer_bytes)
        && is_gpu_grouped_soa_scalar_range_valid(
            descriptor.prefix_byte_offset,
            static_cast<std::uint64_t>(descriptor.control_count) + 1u,
            descriptor.offset_width,
            control_buffer_bytes)
        && is_gpu_grouped_soa_scalar_range_valid(
            descriptor.count_byte_offset,
            descriptor.control_count,
            descriptor.count_width,
            control_buffer_bytes)
        && is_gpu_grouped_soa_scalar_range_valid(
            descriptor.position_byte_offset,
            descriptor.position_count,
            descriptor.position_width,
            position_buffer_bytes);
}

} // namespace bioseq::gpu::vulkan
