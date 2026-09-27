/**
 * @file indexing.cppm
 * @brief GPU index buffers and data passed to seed lookup and sequence sorting.
 */
module;

#include <cstddef>
#include <cstdint>
#include <new>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:frontend_types.indexing;

import :receipts;
import :frontend_types.sketch;
import :frontend_types.arena;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

// GPU index and sorting inputs.

/// @brief Borrow GPU index buffers without taking ownership.
/// Keeping this view in the runtime lets index callers pass buffers without
/// making the runtime import the indexing module.
struct gpu_target_index_view {
    gpu_arena_slice slots;              ///< Open-addressed or dense slot table slice.
    gpu_arena_slice positions;          ///< Encoded positions grouped by minimizer hash.
    std::uint64_t target_len = 0;       ///< Target sequence length in bases.
    std::uint32_t slot_mask = 0;        ///< Open-addressing mask, or zero for dense buckets.
    std::uint32_t position_count = 0;   ///< Number of encoded positions stored.
    std::uint32_t valid = 0;            ///< Nonzero when the caller has supplied a usable index view.
};

/// @brief Parameters for sorting key/value arrays with GPU radix sort.
struct gpu_sort_plan {
    std::uint64_t element_count = 0; ///< Number of key/value pairs to sort.
    std::uint32_t key_bytes = 8;     ///< Key element width in bytes.
    std::uint32_t value_bytes = 8;   ///< Value element width in bytes.
    std::uint32_t radix_bits = 8;    ///< Number of key bits sorted in each pass.
    std::uint32_t pass_count = 8;    ///< Number of radix passes recorded.
};

/// @brief Chain coordinates and the anchor indices used to reach them.
/// The trace follows predecessors from the chain end, matching the
/// `coordinate_reduce` shader output.
struct gpu_coordinate_chain_trace_result {
    gpu_coordinate_result_wire result{};              ///< Coordinates and score of the selected chain.
    std::vector<std::uint32_t> trace{};             ///< Predecessor anchor indices in reverse chain order.
};

/// @brief Parameters copied into the sequence-sort shader push constants.
/// The field order and size must match the GLSL `P` declaration.
struct sequence_sort_push_constants {   // 40 bytes, matching the GLSL sequence-sort push constants.
    std::uint32_t count = 0;            ///< Row count for the key stage, or range count for tiebreak.
    std::uint32_t unit_bits = 0;        ///< Packed unit width in bits (8, 16, 32, or 64).
    std::uint32_t bits_per_char = 0;    ///< Bits per alphabet symbol for generic alphabets only.
    std::uint32_t letters_per_unit = 0; ///< Letters per packed unit for generic alphabets only.
    std::uint32_t char_mask = 0;        ///< Mask used to extract one symbol in the generic shader.
    std::uint32_t used_mask_lo = 0;     ///< Low 32 bits of the mask that excludes unused storage bits.
    std::uint32_t used_mask_hi = 0;     ///< High 32 bits of the mask that excludes unused storage bits.
    std::uint32_t key_unit_offset = 0;  ///< Selected key unit offset for the dense scatter stage.
    std::uint32_t sort_size = 0;        ///< Global bitonic stage sort size `k`.
    std::uint32_t sort_stride = 0;      ///< Global bitonic stage stride `j`.
};
static_assert(sizeof(sequence_sort_push_constants) == 40);

/// @brief Location and length of one packed sequence in the shader input buffer.
struct sequence_descriptor_wire { // 16 bytes, matching GLSL SeqDesc. Offsets count uint32 words.
    std::uint32_t word_offset = 0; ///< Offset into packed units in `uint32_t` words.
    std::uint32_t unit_count = 0;  ///< Number of packed units for the sequence.
    std::uint32_t bp_size = 0;     ///< Sequence length in bases.
    std::uint32_t pad = 0;         ///< Padding to the 16-byte GLSL layout.
};
static_assert(sizeof(sequence_descriptor_wire) == 16);

/// @brief A borrowed block of packed sequence bytes to upload for sorting.
struct sequence_sort_upload_chunk {
    const void* bytes = nullptr;           ///< Borrowed packed-sequence bytes for one chunk.
    std::uint64_t byte_size = 0;           ///< Logical byte length of `bytes`.
    std::uint64_t padded_byte_size = 0;    ///< Device upload size after alignment padding.
};

/// @brief Alphabet and packing parameters used to select and run sequence-sort shaders.
struct sequence_sort_kernel_parameters {
    std::uint32_t alphabet_kind = 0; ///< `alphabet_kernel_kind`: dna4_nt4 maps to `_dna4`, else `_generic`.
    std::uint32_t unit_bits = 0;     ///< Packed unit width in bits (8, 16, 32, or 64).
    std::uint32_t bits_per_char = 0; ///< Bits per alphabet symbol for generic alphabets.
    std::uint32_t letters_per_unit = 0; ///< Letters encoded in each packed unit for generic alphabets.
    std::uint32_t char_mask = 0;     ///< Mask used to extract one packed symbol.
    std::uint64_t used_storage_mask = 0; ///< Mask of storage bits occupied by packed symbols.
};

/// @brief Reasons a GPU target-index build can fail.
enum class gpu_index_build_failure : std::uint8_t {
    none, ///< Build succeeded.
    invalid_shape, ///< Packed sequence or sketch parameters are inconsistent.
    minimizer_capacity_exceeds_u32, ///< Requested minimizer capacity exceeds `uint32_t`.
    memory_plan_invalid, ///< Buffer sizes or host staging requirements are invalid.
    device_arena_allocation_failed, ///< The GPU arena could not provide the required slices.
    output_capacity_overflow, ///< The output count or required storage exceeds supported limits.
    device_buffer_allocation_failed, ///< Standalone device buffer allocation failed.
    count_peak_memory_admission_denied, ///< Memory reservation for the counting pass was refused.
    execution_peak_memory_admission_denied, ///< Memory reservation for index construction was refused.
    peak_allocation_exceeds_admission, ///< Allocations exceeded the reserved memory budget.
    index_invariant_failed, ///< Index construction failed an internal consistency check.
};

/// @brief Number of low hash bits used to choose a query-filter bitmap bit.
inline constexpr std::uint32_t k_gpu_query_gate_bits = 26u;
/// @brief Number of 32-bit words in the query-filter bitmap.
inline constexpr std::uint32_t k_gpu_query_gate_word_count =
    (std::uint32_t{1} << k_gpu_query_gate_bits) / 32u;
/// @brief Mask that maps a minimizer hash to a query-filter bitmap bit.
inline constexpr std::uint32_t k_gpu_query_gate_mask =
    (std::uint32_t{1} << k_gpu_query_gate_bits) - 1u;

/// @brief Query-filter bitmap saved with the target index built from it.
/// Cache reuse checks the bitmap itself after comparing its digest.
struct gpu_index_gate_identity {
    bool enabled = false; ///< Whether the index was filtered using this query bitmap.
    std::uint32_t word_count = 0u; ///< Number of bitmap words when filtering is enabled.
    std::uint32_t mask = 0u;       ///< Mask applied to a hash to find its bitmap bit.
    std::uint64_t digest_lo = 0u;  ///< First 64-bit digest of the bitmap words.
    std::uint64_t digest_hi = 0u;  ///< Second 64-bit digest of the bitmap words.
    std::shared_ptr<const std::vector<std::uint32_t>> canonical_words{}; ///< Shared read-only bitmap words.

    /// @brief Check that the fields describe a disabled filter or a full bitmap.
    /// @return `true` when the flags, sizes and storage agree.
    [[nodiscard]] bool is_well_formed() const noexcept {
        if (!enabled) {
            return word_count == 0u && mask == 0u && digest_lo == 0u && digest_hi == 0u &&
                   !canonical_words;
        }
        return word_count == k_gpu_query_gate_word_count && mask == k_gpu_query_gate_mask &&
               canonical_words && canonical_words->size() == word_count;
    }

    /// @brief Check whether both indexes used the same query-filter bitmap.
    /// @param[in] other Filter identity to compare.
    /// @return `true` when the metadata and bitmap words match.
    [[nodiscard]] bool is_exactly_equal_to(const gpu_index_gate_identity& other) const {
        if (enabled != other.enabled || word_count != other.word_count || mask != other.mask ||
            digest_lo != other.digest_lo || digest_hi != other.digest_hi) {
            return false;
        }
        if (!enabled)
            return true;
        if (!canonical_words || !other.canonical_words)
            return false;
        // The shared owner holds read-only words, so matching owners have identical
        // contents. Different owners need a word-by-word comparison even when
        // their digests match.
        if (canonical_words.get() == other.canonical_words.get())
            return true;
        return *canonical_words == *other.canonical_words;
    }
};

/// @brief GPU dispatches and memory use recorded during one target-index build.
struct gpu_index_build_stats {
    std::uint64_t count_submit_admissions = 0;
    std::uint64_t execution_peak_admissions = 0;
    // Count minimizers and prefix-sum the tile counts before allocating the
    // output and sorting buffers. The CPU reads only the final offset and count.
    std::uint64_t exact_count_dispatches = 0;
    std::uint64_t exact_count_prefix_scan_dispatches = 0;
    std::uint64_t exact_count_control_readbacks = 0;
    // Each tile writes into the range assigned by its prefix sum. The CPU
    // checks tile completion and output counts without reading minimizer data.
    std::uint64_t deterministic_tile_emit_dispatches = 0;
    std::uint64_t atomic_append_dispatches = 0;
    std::uint64_t emit_validation_control_readbacks = 0;
    std::uint64_t counted_minimizer_count = 0;
    std::uint64_t verified_emit_count = 0;
    std::uint64_t count_emit_parity_verified = 0;
    // The exact count determines the allocation. It may exceed the caller
    // estimate, which is recorded here to diagnose underestimated memory use.
    std::uint64_t target_minimizer_capacity_estimate = 0;
    std::uint64_t capacity_estimate_underestimated = 0;
    // Track temporary host storage during the build. The runtime frees its idle
    // arena, copies sequence bytes into mapped GPU storage and releases any owned
    // staging copy. Tile records are written directly into the arena.
    std::uint64_t idle_arena_trim_attempts_before_count = 0;
    std::uint64_t idle_arena_trim_successes_before_count = 0;
    std::uint64_t idle_arena_released_bytes_before_count = 0;
    std::uint64_t pretrim_peak_observed_bytes = 0;
    std::uint64_t direct_sequence_arena_copies = 0;
    std::uint64_t sequence_padding_zeroed_bytes = 0;
    std::uint64_t host_sequence_capacity_releases_before_execution = 0;
    std::uint64_t host_sequence_capacity_released_bytes = 0;
    std::uint64_t direct_tile_arena_fills = 0;
    std::uint64_t host_tile_staging_elided_bytes = 0;
    std::uint64_t count_submit_host_transient_bytes = 0;
    std::uint64_t execution_submit_host_transient_bytes = 0;
    std::uint64_t host_transient_released_before_execution_bytes = 0;
    // Set after all arena buffers are freed under the runtime mutex. The caller
    // can then reduce its reservation to the memory kept by the finished index.
    std::uint64_t arena_trimmed_before_permit_shrink = 0;
    std::uint64_t resident_transfers = 0;
    std::uint64_t adoption_rereserve_attempts = 0;
    std::uint64_t adoption_successes = 0;
    std::uint64_t resident_bytes = 0;
    // Record which filtered or unfiltered GPU passes ran and what they produced.
    std::uint64_t gate_count_dispatches = 0;
    std::uint64_t gated_emit_dispatches = 0;
    std::uint64_t ungated_emit_dispatches = 0;
    std::uint64_t retained_minimizer_count = 0;
    std::uint64_t distinct_minimizer_count = 0;
    std::uint64_t query_gate_enabled = 0;
    std::uint64_t query_gate_word_count = 0;
    std::uint64_t query_gate_mask = 0;
    std::uint64_t query_gate_digest_lo = 0;
    std::uint64_t query_gate_digest_hi = 0;
    std::uint64_t peak_admitted_bytes = 0;
    std::uint64_t peak_observed_bytes = 0;
    // Record allocation accounting carried with the adopted index buffers.
    std::uint64_t materialization_admitted_bytes = 0;
    std::uint64_t materialization_actual_allocation_bytes = 0;
    std::uint64_t materialization_fully_touched_bytes = 0;
    std::uint64_t materialization_reflected_delta_bytes = 0;
    std::uint64_t materialization_permit_released = 0;
    std::uint64_t materialization_host_visible = 0;
    std::uint64_t materialization_host_coherent = 0;
    std::uint64_t materialization_process_footprint_reflected = 0;
};

/// @brief Own the finished GPU target index and its memory reservation.
struct gpu_built_index {
    // Members are destroyed in reverse order, so release the permit last.
    bioseq::parallelism::memory_permit permit; ///< Memory reservation held with the index buffers.
    buffer slots;                              ///< Minimizer lookup table, stored as dense hash-sorted buckets by this builder.
    buffer positions;                          ///< Encoded positions grouped by minimizer hash.
    gpu_index_gate_identity gate_identity{}; ///< Query-filter bitmap used when building this index.
    bioseq::parallelism::memory_materialization_result materialization{}; ///< Record of how host-visible allocation bytes were accounted for.
    gpu_index_build_stats ownership{};       ///< Dispatch and memory-use counters for this build.
    std::uint64_t peak_admission_bytes = 0;  ///< Largest admitted byte budget during the build.
    std::uint64_t observed_device_peak_bytes = 0; ///< Peak device bytes allocated, including arena buffers.
    bioseq::parallelism::memory_admission_failure execution_peak_admission_failure{}; ///< Details of a refused memory reservation.
    std::uint64_t target_len = 0;            ///< Target sequence length in bases.
    std::uint32_t slot_mask = 0;             ///< Slot mask for open addressing, or zero for dense sorted buckets.
    std::uint32_t occupied_slots = 0;        ///< Number of lookup entries, one per distinct minimizer hash.
    std::uint32_t position_count = 0;        ///< Number of stored encoded positions.
    gpu_index_build_failure failure_reason = gpu_index_build_failure::none; ///< Terminal failure, if any.
    bool valid = false;                      ///< Whether construction produced a usable index.

    /// @brief Create an empty result with `valid` set to false.
    gpu_built_index() = default;
    /// @brief Prevents copying Vulkan buffer and permit ownership.
    gpu_built_index(const gpu_built_index&) = delete;
    /// @brief Prevents copy-assignment of Vulkan buffer and permit ownership.
    gpu_built_index& operator=(const gpu_built_index&) = delete;
    /// @brief Move the index buffers and mark the source result invalid.
    gpu_built_index(gpu_built_index&& other) noexcept {
        move_from(other);
    }
    /// @brief Release this index, then take ownership of the other result.
    /// @return This result.
    gpu_built_index& operator=(gpu_built_index&& other) noexcept {
        if (this != &other) {
            // Free the old buffers before returning their reserved memory budget.
            positions.reset();
            slots.reset();
            permit.release();
            move_from(other);
        }
        return *this;
    }

  private:
    /// @brief Move buffers and their build metadata, then invalidate the source.
    void move_from(gpu_built_index& other) noexcept {
        permit = std::move(other.permit);
        slots = std::move(other.slots);
        positions = std::move(other.positions);
        gate_identity = std::move(other.gate_identity);
        materialization = other.materialization;
        ownership = other.ownership;
        peak_admission_bytes = other.peak_admission_bytes;
        observed_device_peak_bytes = other.observed_device_peak_bytes;
        execution_peak_admission_failure = other.execution_peak_admission_failure;
        target_len = other.target_len;
        slot_mask = other.slot_mask;
        occupied_slots = other.occupied_slots;
        position_count = other.position_count;
        failure_reason = other.failure_reason;
        valid = other.valid;
        other.valid = false;
    }
};
static_assert(std::is_standard_layout_v<gpu_built_index>);
static_assert(std::is_nothrow_move_constructible_v<gpu_built_index>);
static_assert(std::is_nothrow_move_assignable_v<gpu_built_index>);
static_assert(offsetof(gpu_built_index, permit) < offsetof(gpu_built_index, slots));
static_assert(offsetof(gpu_built_index, permit) < offsetof(gpu_built_index, positions));

/// @brief Sorted minimizer pairs on the GPU and downloaded copies for storage.
/// Pairs are ordered by hash, then encoded position. Callers can persist the
/// downloaded data without another GPU readback.
struct gpu_sorted_pair_result {
    buffer keys;                         ///< Device buffer containing sorted minimizer hashes.
    buffer values;                       ///< Device buffer holding sorted encoded positions.
    std::vector<std::byte> host_key_bytes;   ///< Optional downloaded key bytes.
    std::vector<std::byte> host_value_bytes; ///< Optional downloaded value bytes.
    std::uint64_t target_len = 0;        ///< Target sequence length in bases.
    std::uint32_t pair_count = 0;        ///< Number of `(kmer, encoded_pos)` pairs.
    bool valid = false;                  ///< Whether sort output is complete and consistent.
};

/// @brief Two pairs of buffer slices alternated during stable radix sorting.
/// `keys_in_scratch` records which pair holds the final result.
struct sorted_pair_device_views {
    gpu_arena_slice keys;           ///< First key buffer.
    gpu_arena_slice values;         ///< First value buffer.
    gpu_arena_slice keys_scratch;   ///< Second key buffer.
    gpu_arena_slice values_scratch; ///< Second value buffer.
    bool keys_in_scratch = false;   ///< Whether the final sorted keys live in `keys_scratch`.
};

/// @brief Return the buffer slice holding the sorted keys.
[[nodiscard]] inline const gpu_arena_slice&
choose_sorted_keys(const sorted_pair_device_views& views) noexcept {
    return views.keys_in_scratch ? views.keys_scratch : views.keys;
}

/// @brief Return the buffer slice holding the sorted values.
[[nodiscard]] inline const gpu_arena_slice&
choose_sorted_values(const sorted_pair_device_views& views) noexcept {
    return views.keys_in_scratch ? views.values_scratch : views.values;
}

/// @brief Query minimizers and a target index for GPU seed lookup.
/// Query minimizers can already be on the GPU or be uploaded from host bytes.
/// The target lookup table and positions must be GPU buffers.
struct gpu_seed_inputs {
    std::span<const std::byte> host_minimizer_bytes{}; ///< Host minimizer bytes uploaded once when resident words are absent.
    const buffer* device_minimizer_words = nullptr;    ///< Resident minimizer device buffer, when already on GPU.
    const buffer* slots = nullptr;                     ///< Target lookup table on the GPU.
    const buffer* positions = nullptr;                 ///< Encoded target positions on the GPU.
    std::uint64_t target_len = 0;                      ///< Target sequence length in bases.
    std::uint32_t slot_mask = 0;                       ///< Open-addressing mask when `bucket_count` is zero.
    std::uint32_t position_count = 0;                  ///< Number of encoded positions in `positions`.
    std::uint32_t minimizer_count = 0;                 ///< Number of query minimizer records.
    std::uint32_t query_minimizer_record_offset = 0;   ///< First query record in the bound whole buffer.
    std::uint32_t minimizer_word_kind = 0;             ///< Encoding used for each query minimizer.
    std::uint32_t minimizer_words32_per_record = 0;    ///< `uint32_t` words per minimizer record.
    std::uint32_t hash_shift = 0;                      ///< Right shift used to extract the hash from a packed minimizer.
    std::uint32_t pos_bits = 0;                        ///< Number of bits used for a minimizer position.
    std::uint32_t bucket_count = 0;                    ///< Dense bucket count when slots are hash-sorted, else zero.
    std::uint64_t target_position_offset = 0;            ///< Base offset added to decoded target positions.
};

} // namespace bioseq::gpu::vulkan
