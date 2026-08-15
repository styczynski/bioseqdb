/**
 * @file indexing.cppm
 * @brief Defines resident index, sequence-sort, and seed wire contracts.
 */
module;

#include <cstddef>
#include <cstdint>

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

// ===========================================================================
// Two-submit resident frontend I/O
// ===========================================================================

/// @brief Storage-free view of a GPU target index (slots/positions buffer slices plus
/// the slot mask / counts). Lets the runtime accept an index without importing
/// `bioseq::indexing` (which would create a module cycle).
struct gpu_target_index_view {
    gpu_arena_slice slots;
    gpu_arena_slice positions;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t valid = 0;
};

/// @brief Plan describing one device radix sort over parallel (key,value) uint64 arrays.
struct gpu_sort_plan {
    std::uint64_t element_count = 0;
    std::uint32_t key_bytes = 8;
    std::uint32_t value_bytes = 8;
    std::uint32_t radix_bits = 8;
    std::uint32_t pass_count = 8;
};

/// @brief Coordinate-only chain result plus the selected predecessor trace. The trace
/// entries are anchor indices in reverse chain order, matching the
/// `coordinate_reduce` shader scratch buffer.
struct gpu_coordinate_chain_trace_result {
    gpu_coordinate_result_wire result{};
    std::vector<std::uint32_t> trace{};
};

/// @brief GPU sequence-sort wire structs. Byte layout is pinned to the GLSL
/// `P` push block and `SeqDesc`/`SeqSortRange` declarations in shader_compiler.
struct sequence_sort_push_constants {   // 40 bytes; == GLSL pc (seq_sort kernels)
    std::uint32_t count = 0;            // row_count (key stage) / range_count (tiebreak stage)
    std::uint32_t unit_bits = 0;        // 8/16/32/64
    std::uint32_t bits_per_char = 0;    // generic only
    std::uint32_t letters_per_unit = 0; // generic only
    std::uint32_t char_mask = 0;        // generic only
    std::uint32_t used_mask_lo = 0;
    std::uint32_t used_mask_hi = 0;
    std::uint32_t key_unit_offset = 0; // dense scatter stage: selected key unit
    std::uint32_t sort_size = 0;       // global bitonic stage: k
    std::uint32_t sort_stride = 0;     // global bitonic stage: j
};
static_assert(sizeof(sequence_sort_push_constants) == 40);

/// @brief Packed sequence descriptor consumed by sequence-sort shaders.
struct sequence_descriptor_wire { // 16 bytes; == GLSL SeqDesc. word_offset in uint32 words.
    std::uint32_t word_offset = 0;
    std::uint32_t unit_count = 0;
    std::uint32_t bp_size = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(sequence_descriptor_wire) == 16);

/// @brief One borrowed byte range participating in a sequence-sort upload.
struct sequence_sort_upload_chunk {
    const void* bytes = nullptr;
    std::uint64_t byte_size = 0;
    std::uint64_t padded_byte_size = 0;
};

/// @brief Resolved by the façade from compact_sequence_range<S>::traits.
struct sequence_sort_kernel_parameters {
    std::uint32_t alphabet_kind = 0; // alphabet_kernel_kind: dna4_nt4 -> _dna4 kinds; else _generic
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t char_mask = 0;
    std::uint64_t used_storage_mask = 0;
};

/// @brief Result of a GPU-resident target minimizer index build. Owns the
/// open-addressed slot table and the hash-run-ordered positions buffer. The
/// indexing layer adopts these into a `gpu_resident_minimizer_index` outside the
/// runtime module (which must not import `bioseq::indexing`).
enum class gpu_index_build_failure : std::uint8_t {
    none,
    invalid_shape,
    minimizer_capacity_exceeds_u32,
    memory_plan_invalid,
    device_arena_allocation_failed,
    output_capacity_overflow,
    device_buffer_allocation_failed,
    count_peak_memory_admission_denied,
    execution_peak_memory_admission_denied,
    peak_allocation_exceeds_admission,
    index_invariant_failed,
};

/// @brief Canonical query-gate identity carried with every GPU-built index.  The
/// digest accelerates rejection, but immutable exact words remain authoritative.
inline constexpr std::uint32_t k_gpu_query_gate_bits = 26u;
inline constexpr std::uint32_t k_gpu_query_gate_word_count =
    (std::uint32_t{1} << k_gpu_query_gate_bits) / 32u;
inline constexpr std::uint32_t k_gpu_query_gate_mask =
    (std::uint32_t{1} << k_gpu_query_gate_bits) - 1u;

/// @brief Immutable canonical query-gate identity for a GPU-built index.
struct gpu_index_gate_identity {
    bool enabled = false;
    std::uint32_t word_count = 0u;
    std::uint32_t mask = 0u;
    std::uint64_t digest_lo = 0u;
    std::uint64_t digest_hi = 0u;
    std::shared_ptr<const std::vector<std::uint32_t>> canonical_words{};

    [[nodiscard]] bool is_well_formed() const noexcept {
        if (!enabled) {
            return word_count == 0u && mask == 0u && digest_lo == 0u && digest_hi == 0u &&
                   !canonical_words;
        }
        return word_count == k_gpu_query_gate_word_count && mask == k_gpu_query_gate_mask &&
               canonical_words && canonical_words->size() == word_count;
    }

    [[nodiscard]] bool is_exactly_equal_to(const gpu_index_gate_identity& other) const {
        if (enabled != other.enabled || word_count != other.word_count || mask != other.mask ||
            digest_lo != other.digest_lo || digest_hi != other.digest_hi) {
            return false;
        }
        if (!enabled)
            return true;
        if (!canonical_words || !other.canonical_words)
            return false;
        // Freeze guarantees there is no mutable alias. Reusing the identical
        // immutable owner is exact identity and avoids an 8 MiB cache-hit scan;
        // distinct owners still compare every word after the digest prefilter.
        if (canonical_words.get() == other.canonical_words.get())
            return true;
        return *canonical_words == *other.canonical_words;
    }
};

/// @brief Dispatch, admission, and lifetime statistics for one GPU index build.
struct gpu_index_build_stats {
    std::uint64_t count_submit_admissions = 0;
    std::uint64_t execution_peak_admissions = 0;
    // Exactly one device tile-count dispatch, one three-dispatch prefix scan,
    // and one eight-byte mapped control read establish T before the large
    // emit/sort allocations. No minimizer payload is materialised on the host.
    std::uint64_t exact_count_dispatches = 0;
    std::uint64_t exact_count_prefix_scan_dispatches = 0;
    std::uint64_t exact_count_control_readbacks = 0;
    // Non-empty target builds emit deterministically into prefix-owned tile
    // ranges.  The control readback validates only offsets/completion markers
    // and the device status word; it never reads hashes or positions.
    std::uint64_t deterministic_tile_emit_dispatches = 0;
    std::uint64_t atomic_append_dispatches = 0;
    std::uint64_t emit_validation_control_readbacks = 0;
    std::uint64_t counted_minimizer_count = 0;
    std::uint64_t verified_emit_count = 0;
    std::uint64_t count_emit_parity_verified = 0;
    // The caller's capacity estimate is a planning hint, never a semantic cap.
    // Exact count T may exceed it and then drives checked allocation/admission.
    std::uint64_t target_minimizer_capacity_estimate = 0;
    std::uint64_t capacity_estimate_underestimated = 0;
    // Deterministic host-scratch lifetime statistics for target-index builds.
    // A prior idle arena high-water is destroyed before count admission; the
    // packed sequence is copied directly from the borrowed input and only its
    // device padding is zeroed; tile records are filled directly in mapped
    // arena storage, so exact execution grow_to sees no host tile owner.
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
    // Set inside runtime under its mutex only after the runtime scope has
    // rewound and destroyed every arena backing buffer, before the façade can
    // shrink the peak permit to resident R.
    std::uint64_t arena_trimmed_before_permit_shrink = 0;
    std::uint64_t resident_transfers = 0;
    std::uint64_t adoption_rereserve_attempts = 0;
    std::uint64_t adoption_successes = 0;
    std::uint64_t resident_bytes = 0;
    // Operation dispatch and shape statistics. Admission counters alone do not
    // establish that the query-gated GPU kernels ran.
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
    // Immutable materialization statistics propagated through zero-copy adoption.
    std::uint64_t materialization_admitted_bytes = 0;
    std::uint64_t materialization_actual_allocation_bytes = 0;
    std::uint64_t materialization_fully_touched_bytes = 0;
    std::uint64_t materialization_reflected_delta_bytes = 0;
    std::uint64_t materialization_permit_released = 0;
    std::uint64_t materialization_host_visible = 0;
    std::uint64_t materialization_host_coherent = 0;
    std::uint64_t materialization_process_footprint_reflected = 0;
};

/// @brief Move-owned buffers, permits, and identity for a GPU-built target index.
struct gpu_built_index {
    // Declared first so ownership is released only after both buffers.
    bioseq::parallelism::memory_permit permit;
    buffer slots;
    buffer positions;
    gpu_index_gate_identity gate_identity{};
    bioseq::parallelism::memory_materialization_result materialization{};
    gpu_index_build_stats ownership{};
    std::uint64_t peak_admission_bytes = 0;
    std::uint64_t observed_device_peak_bytes = 0;
    bioseq::parallelism::memory_admission_failure execution_peak_admission_failure{};
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t occupied_slots = 0;
    std::uint32_t position_count = 0;
    gpu_index_build_failure failure_reason = gpu_index_build_failure::none;
    bool valid = false;

    gpu_built_index() = default;
    gpu_built_index(const gpu_built_index&) = delete;
    gpu_built_index& operator=(const gpu_built_index&) = delete;
    gpu_built_index(gpu_built_index&& other) noexcept {
        move_from(other);
    }
    gpu_built_index& operator=(gpu_built_index&& other) noexcept {
        if (this != &other) {
            // The destination permit owns the destination buffers' admission.
            // Destroy those buffers first; releasing the permit first creates a
            // transient live-but-unadmitted allocation window.
            positions.reset();
            slots.reset();
            permit.release();
            move_from(other);
        }
        return *this;
    }

  private:
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

/// @brief Host-visible result of the GPU sketch and stable two-key sort roundtrip.
/// Owns the device buffer holding the final sorted run plus the downloaded
/// host bytes (host/Postgres-format persistence happens on the CPU side).
struct gpu_sorted_pair_result {
    buffer keys;
    buffer values;
    std::vector<std::byte> host_key_bytes;
    std::vector<std::byte> host_value_bytes;
    std::uint64_t target_len = 0;
    std::uint32_t pair_count = 0;
    bool valid = false;
};

/// @brief Device-side ping-pong views for the stable two-key pair
/// sort. Hoisted above `class runtime` because the private method
/// `record_stable_pair_sort_by_value_then_key_locked` takes it by reference.
struct sorted_pair_device_views {
    gpu_arena_slice keys;
    gpu_arena_slice values;
    gpu_arena_slice keys_scratch;
    gpu_arena_slice values_scratch;
    bool keys_in_scratch = false;
};

[[nodiscard]] inline const gpu_arena_slice&
choose_sorted_keys(const sorted_pair_device_views& views) noexcept {
    return views.keys_in_scratch ? views.keys_scratch : views.keys;
}

[[nodiscard]] inline const gpu_arena_slice&
choose_sorted_values(const sorted_pair_device_views& views) noexcept {
    return views.keys_in_scratch ? views.values_scratch : views.values;
}

/// @brief Inputs to the GPU seed count, emit, and sort roundtrip. The query
/// minimizer words arrive resident (`device_minimizer_words`) or as host bytes.
/// the index slots/positions are the GPU-resident buffers.
struct gpu_seed_inputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const buffer* device_minimizer_words = nullptr;
    const buffer* slots = nullptr;
    const buffer* positions = nullptr;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t bucket_count = 0; // >0 ⇒ slots is a hash-sorted dense bucket array
    std::uint64_t target_position_offset = 0;
};

} // namespace bioseq::gpu::vulkan
