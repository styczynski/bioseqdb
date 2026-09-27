/**
 * @file sketch.cppm
 * @brief Defines GPU sketch results and the inputs used to find matching seeds.
 */
module;

#include <cstddef>
#include <cstdint>
#include <new>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:frontend_types.sketch;

import :receipts;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

/// @brief Parameters shared with the chaining shader. Field order must match its 36-byte GLSL block.
struct chaining_push_constants {
    std::uint32_t anchor_count;  ///< Number of anchors loaded for chaining.
    std::uint32_t range_count;   ///< Number of sorted anchor ranges.
    std::int32_t max_dist_x;     ///< Maximum distance betwen anchors in query or target coordinates.
    std::int32_t max_dist_y;     ///< Additional limit on distance between anchors in query coordinates.
    std::int32_t bandwidth;      ///< Maximum difference between query and target distances, in bases.
    std::int32_t max_iter;       ///< Maximum  number of preceding anchors considered for each anchor.
    float gap_penalty;           ///< Penalty per base of difference between query and target distances.
    float skip_penalty;          ///< Penalty for the distance shared by the query and target steps.
    std::int32_t max_chain_skip; ///< Stops after enough predecessor batches fail to improve the score. Disabled at zero or below.
};
static_assert(sizeof(chaining_push_constants) == 36);
static_assert(offsetof(chaining_push_constants, anchor_count) == 0);
static_assert(offsetof(chaining_push_constants, range_count) == 4);
static_assert(offsetof(chaining_push_constants, max_dist_x) == 8);
static_assert(offsetof(chaining_push_constants, max_dist_y) == 12);
static_assert(offsetof(chaining_push_constants, bandwidth) == 16);
static_assert(offsetof(chaining_push_constants, max_iter) == 20);
static_assert(offsetof(chaining_push_constants, gap_penalty) == 24);
static_assert(offsetof(chaining_push_constants, skip_penalty) == 28);
static_assert(offsetof(chaining_push_constants, max_chain_skip) == 32);

/// @brief Parameters for generating packed sequences on the GPU.
struct generate_units_push {
    std::uint32_t symbols_per_seq = 0;   ///< Total symbols generated per sequence.
    std::uint32_t units_per_seq = 0;     ///< Packed units emitted per sequence.
    std::uint32_t chunks_per_seq = 0;    ///< Generator chunks scheduled per sequence.
    std::uint32_t chunk_units = 0;       ///< Packed units produced per chunk.
    std::uint32_t letters_per_unit = 0;  ///< Letters encoded in each packed unit.
    std::uint32_t bits_per_char = 0;     ///< Bits per alphabet symbol in packed units.
    std::uint32_t alphabet_size = 0;     ///< Number of symbols in the alphabet.
    std::uint32_t total_chunks = 0;      ///< Global chunk count across the batch.
    std::uint32_t chunk_base = 0;        ///< First global chunk index in this dispatch.
    std::uint32_t seed_lo = 0;           ///< Low 32 bits of the deterministic seed.
    std::uint32_t seed_hi = 0;           ///< High 32 bits of the deterministic seed.
    std::uint32_t pad = 0;               ///< Padding to the 48-byte GLSL layout.
};
static_assert(sizeof(generate_units_push) == 48);
static_assert(std::is_standard_layout_v<generate_units_push>);

/// @brief Minimizers from one GPU sketch, ready for seed lookup.
/// The caller keeps the device buffer  and can copy its contents to host memory
/// after the temporary sequence buffers are freed.
struct runtime_sketch_result {
    buffer minimizer_words;              ///< Device minimizers owned by the caller after return.
    std::uint64_t count = 0;             ///< Exact minimizer count from the count pass.
    // The caller checks this before reducing the sketch memory reservation or
    // allocating host output. It confirms that temporary, arena buffers have
    // already byen freed.
    bool arena_trimmed_before_return = false; ///< Arena idle capacity was released before return.
    bool overflow = false;                   ///< Set when the exact count exceeded caller limits.
};

/// @brief Chooses whether a sketch batch also returns a host copy of its minimizers.
/// Both modes return the device buffer and exact count
enum class vulkan_sketch_host_materialization : std::uint8_t {
    host_and_device, ///< Return host and device minimizers after one queue  submission.
    device_only,     ///< Keep output on the device, allocating it after a separate counting submission.
};

/// @brief Records the GPU work completed by a sketch batch.
/// A batch counts tiles, scans their counts, then writes minimizers. Keeping
/// output on the device splits this work into two submissions so the output
/// buffer can be sized from the count. A default object describes no completed batch.
class runtime_sketch_dispatch_stats {
  public:
    /// @brief Starts with no recorded dispatches.
    runtime_sketch_dispatch_stats() = default;
    /// @brief Copies the recorded dispatch counts and output mode.
    runtime_sketch_dispatch_stats(const runtime_sketch_dispatch_stats&) = default;
    /// @brief Replaces the recorded dispatch counts and output mode.
    runtime_sketch_dispatch_stats& operator=(const runtime_sketch_dispatch_stats&) = default;

    /// @brief Returns whether the batch requested a host copy.
    [[nodiscard]] constexpr vulkan_sketch_host_materialization materialization() const noexcept {
        return materialization_;
    }
    /// @brief Returns the number of tile-counting dispatches.
    [[nodiscard]] constexpr std::uint64_t count_dispatches() const noexcept {
        return count_dispatches_;
    }
    /// @brief Returns the number of complete prefix-scan operations.
    [[nodiscard]] constexpr std::uint64_t prefix_scan_sequences() const noexcept {
        return prefix_scan_sequences_;
    }
    /// @brief Returns the number  of dispatches used to scan tile counts.
    [[nodiscard]] constexpr std::uint64_t prefix_scan_dispatches() const noexcept {
        return prefix_scan_dispatches_;
    }
    /// @brief Returns the number of dispatches that wrote minimizers.
    [[nodiscard]] constexpr std::uint64_t emit_dispatches() const noexcept {
        return emit_dispatches_;
    }
    /// @brief Returns the number of submissions whose completion was awaited.
    [[nodiscard]] constexpr std::uint64_t completed_roundtrips() const noexcept {
        return completed_roundtrips_;
    }
    /// @brief Checks that the couft, scan, and emit stages completed for the selected output mode.
    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        const bool dispatch_shape_accepted =
            count_dispatches_ == 1u && prefix_scan_sequences_ == 1u &&
            prefix_scan_dispatches_ == 3u && emit_dispatches_ == 1u;
        if (!dispatch_shape_accepted)
            return false;
        switch (materialization_) {
        case vulkan_sketch_host_materialization::host_and_device:
            return completed_roundtrips_ == 1u;
        case vulkan_sketch_host_materialization::device_only:
            return completed_roundtrips_ == 2u;
        }
        return false;
    }

  private:
    friend class runtime;

    vulkan_sketch_host_materialization materialization_ =
        vulkan_sketch_host_materialization::host_and_device; ///< Whether the batch requested a host copy.
    std::uint64_t count_dispatches_ = 0;                     ///< Tile-counting dispatches.
    std::uint64_t prefix_scan_sequences_ = 0;                ///< Complete prefix-scan operations.
    std::uint64_t prefix_scan_dispatches_ = 0;               ///< Prefix-scan dispatches recorded.
    std::uint64_t emit_dispatches_ = 0;                      ///< Dispatches that wrote minimizers.
    std::uint64_t completed_roundtrips_ = 0;                 ///< Submissions with completed fence waits.
};

static_assert(!runtime_sketch_dispatch_stats{}.is_internally_consistent());

/// @brief Locates one query's minimizers in a shared batch buffer.
struct gpu_seed_query_record_wire {
    std::uint32_t minimizer_offset = 0; ///< First minimizer record for this query.
    std::uint32_t minimizer_count = 0;  ///< Minimizer records belonging to one query.
    std::uint32_t query_len = 0;        ///< Query length in bases for this partition.
    std::uint32_t flags = 0;            ///< Reserved wire flags, zero for valid records.
};
static_assert(sizeof(gpu_seed_query_record_wire) == 16);
static_assert(alignof(gpu_seed_query_record_wire) == alignof(std::uint32_t));

/// @brief Groups sketch tiles by query.
/// After counting, the runtime uses these groups to record  each query's exact
/// minimizer range Later seed lookup must use the same query ranges.
struct gpu_sketch_query_layout_wire {
    std::uint32_t begin = 0;     ///< First tile index included in the query partition.
    std::uint32_t count = 0;     ///< Number of tiles in the partition.
    std::uint32_t query_len = 0; ///< Query length in bases for the partition.
};

/// @brief Records which completed GPU sketch produced a query buffer.
/// Only the runtime can fill this object. It ties the buffer and query ranges
/// to the completed dispatches, so seed lookup can check that they, belong
/// together. Moving transfers this one-use record. Host readback can borrow it.
class device_query_sketch_authorization {
  public:
    /// @brief Constructs an empty invalid authorization.
    device_query_sketch_authorization() = default;
    /// @brief Keeps a query completion record from being copied.
    device_query_sketch_authorization(const device_query_sketch_authorization&) = delete;
    /// @brief Keeps a query completion record from being  copied by assignment.
    device_query_sketch_authorization& operator=(const device_query_sketch_authorization&) = delete;
    /// @brief Moves the query completion record and invalidates the source.
    device_query_sketch_authorization(device_query_sketch_authorization&& other) noexcept {
        move_from(other);
    }
    /// @brief Replaces this query completion recrd by moving another one.
    device_query_sketch_authorization&
    operator=(device_query_sketch_authorization&& other) noexcept {
        if (this != &other) {
            clear();
            move_from(other);
        }
        return *this;
    }

    /// @brief Checks that the recorded query ranges cover the device minimizers exactly.
    /// Also checks the buffer size and sketch settings. Pipeline checks are in is_valid().
    [[nodiscard]] bool does_cover_device_output() const noexcept {
        const auto exact_logical_bytes = minimizer_word_kind_ == 0u
            ? bioseq::common::checked_mul(
                  bioseq::common::ceil_div(minimizer_count_, std::uint64_t{2u}),
                  sizeof(std::uint32_t))
            : bioseq::common::checked_mul(
                  minimizer_count_,
                  static_cast<std::uint64_t>(minimizer_words32_per_record_) *
                      sizeof(std::uint32_t));
        const bool shape_valid =
            identity_ != 0u && runtime_ != nullptr && buffer_ != VK_NULL_HANDLE &&
            memory_identity_ != 0u && buffer_bytes_ != 0u && allocation_bytes_ >= buffer_bytes_ &&
            exact_logical_bytes.has_value() &&
            logical_minimizer_bytes_ == *exact_logical_bytes &&
            logical_minimizer_bytes_ <= buffer_bytes_ &&
            materialization_ == vulkan_sketch_host_materialization::device_only &&
            dispatch_stats_.is_internally_consistent() && window_size_ != 0u && kmer_size_ != 0u &&
            minimizer_word_kind_ <= 3u && minimizer_words32_per_record_ != 0u &&
            hash_shift_ < 64u && pos_bits_ < 64u && !query_records_.empty() &&
            query_tile_counts_.size() == query_records_.size();
        if (!shape_valid)
            return false;
        std::uint64_t cursor = 0u;
        for (const auto& record : query_records_) {
            if (record.flags != 0u || record.query_len == 0u || record.minimizer_offset != cursor ||
                record.minimizer_count > (std::numeric_limits<std::uint64_t>::max)() - cursor) {
                return false;
            }
            cursor += record.minimizer_count;
        }
        return cursor == minimizer_count_;
    }
    /// @brief Checks the query ranges and the completed sketch's pipeline record.
    [[nodiscard]] bool is_valid() const noexcept {
        return does_cover_device_output() && pipeline_token_.is_valid_for_query_sketch();
    }
    /// @brief Returns the runtime's identifier for this sketch, or zero after it is cleared.
    [[nodiscard]] std::uint64_t identity() const noexcept {
        return identity_;
    }
    /// @brief Returns minimizer bytes, excluding padding required for an empty Vulkan buffer.
    [[nodiscard]] std::uint64_t logical_minimizer_bytes() const noexcept {
        return logical_minimizer_bytes_;
    }
    /// @brief Returns the bytes reserved  by this object's host vectors.
    /// Returns the largest uint64_t value if the total would overflow.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        constexpr std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (query_records_.capacity() > maximum / sizeof(gpu_seed_query_record_wire) ||
            query_tile_counts_.capacity() > maximum / sizeof(std::uint32_t)) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        const std::uint64_t record_bytes = static_cast<std::uint64_t>(query_records_.capacity()) *
                                           sizeof(gpu_seed_query_record_wire);
        const std::uint64_t tile_count_bytes =
            static_cast<std::uint64_t>(query_tile_counts_.capacity()) * sizeof(std::uint32_t);
        return tile_count_bytes > maximum - record_bytes ? maximum
                                                         : record_bytes + tile_count_bytes;
    }

  private:
    friend class runtime;

    /// @brief Invalidates the record and clears its query lists while keeping their allocated capacity.
    void clear() noexcept {
        identity_ = 0u;
        runtime_ = nullptr;
        buffer_ = VK_NULL_HANDLE;
        memory_identity_ = 0u;
        buffer_bytes_ = 0u;
        allocation_bytes_ = 0u;
        logical_minimizer_bytes_ = 0u;
        minimizer_count_ = 0u;
        window_size_ = 0u;
        kmer_size_ = 0u;
        minimizer_word_kind_ = 0u;
        minimizer_words32_per_record_ = 0u;
        hash_shift_ = 0u;
        pos_bits_ = 0u;
        materialization_ = vulkan_sketch_host_materialization::host_and_device;
        dispatch_stats_ = runtime_sketch_dispatch_stats{};
        pipeline_token_ = runtime_pipeline_operation_token{};
        device_uuid_.fill(0u);
        query_records_.clear();
        query_tile_counts_.clear();
    }
    /// @brief Moves all fields and invalidates the source.
    void move_from(device_query_sketch_authorization& other) noexcept {
        identity_ = other.identity_;
        runtime_ = other.runtime_;
        buffer_ = other.buffer_;
        memory_identity_ = other.memory_identity_;
        buffer_bytes_ = other.buffer_bytes_;
        allocation_bytes_ = other.allocation_bytes_;
        logical_minimizer_bytes_ = other.logical_minimizer_bytes_;
        minimizer_count_ = other.minimizer_count_;
        window_size_ = other.window_size_;
        kmer_size_ = other.kmer_size_;
        minimizer_word_kind_ = other.minimizer_word_kind_;
        minimizer_words32_per_record_ = other.minimizer_words32_per_record_;
        hash_shift_ = other.hash_shift_;
        pos_bits_ = other.pos_bits_;
        materialization_ = other.materialization_;
        dispatch_stats_ = other.dispatch_stats_;
        pipeline_token_ = std::move(other.pipeline_token_);
        device_uuid_ = other.device_uuid_;
        query_records_ = std::move(other.query_records_);
        query_tile_counts_ = std::move(other.query_tile_counts_);
        other.clear();
    }

    std::uint64_t identity_ = 0u;                              ///< Runtime identifier for this one-use query record.
    runtime* runtime_ = nullptr;                                 ///< Issuing runtime, or null after `clear`.
    VkBuffer buffer_ = VK_NULL_HANDLE;                           ///< Resident minimizer buffer handle.
    std::uint64_t memory_identity_ = 0u;                        ///< Device-memory identity for `buffer_`.
    std::uint64_t buffer_bytes_ = 0u;                           ///< Vulkan buffer range, including required padding.
    std::uint64_t allocation_bytes_ = 0u;                        ///< Physical allocation bytes for `buffer_`.
    std::uint64_t logical_minimizer_bytes_ = 0u;                 ///< Minimizer bytes without empty-buffer padding
    std::uint64_t minimizer_count_ = 0u;                         ///< Total minimizer records in the completed sketch.
    std::uint32_t window_size_ = 0u;                             ///< Window size used by the sketch.
    std::uint32_t kmer_size_ = 0u;                               ///< K-mer size used by the sketch.
    std::uint32_t minimizer_word_kind_ = 0u;                     ///< Minimizer word encoding kind.
    std::uint32_t minimizer_words32_per_record_ = 0u;            ///< `uint32_t` words per minimizer record.
    std::uint32_t hash_shift_ = 0u;                              ///< Shift used to extract a minimizer  hash.
    std::uint32_t pos_bits_ = 0u;                                ///< Bits reservid for a minimizer position.
    vulkan_sketch_host_materialization materialization_ =
        vulkan_sketch_host_materialization::host_and_device; ///< Whether the sketch requested a host copy.
    runtime_sketch_dispatch_stats dispatch_stats_{};             ///< Dispatch counts from the completed sketch.
    runtime_pipeline_operation_token pipeline_token_{};            ///< Record of the pipelines used for the completed sketch.
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid_{};       ///< Device UUID at issuance.
    std::vector<gpu_seed_query_record_wire> query_records_{};      ///< Minimizer range belonging to each query.
    std::vector<std::uint32_t> query_tile_counts_{};               ///< Tile counts parallel to `query_records_`.
};

static_assert(!std::is_copy_constructible_v<device_query_sketch_authorization>);
static_assert(std::is_nothrow_move_constructible_v<device_query_sketch_authorization>);

/// @brief Device minimizers from a batch, with a host copy when requested.
struct runtime_sketch_batch_result {
    buffer minimizer_words;                         ///< Device buffer holding, this batch's minimizers.
    std::vector<std::byte> host_minimizer_bytes;    ///< Host copy, filled when requested.
    std::vector<std::uint32_t> tile_counts;         ///< Minimizers counted for each tile.
    std::vector<std::uint32_t> tile_offsets;        ///< Starting minimizer offset for each tile.
    runtime_sketch_dispatch_stats dispatch_stats{}; ///< Recorded count, scan, and emit dispatches.
    device_query_sketch_authorization query_authority{}; ///< One-use record  linking the query buffer to its completed sketch.
    std::uint64_t count = 0;                        ///< Exact minimizer count across the batch.
    bool overflow = false;                          ///< Set when the count exceeded caller limits.
};

/// @brief Memory sizes used to reserve space for a sketch batch.
/// The arena size includes any larger buffer the runtime already keeps.
/// Output sizes include the allocation padding required by Vulkan.
struct gpu_sketch_batch_allocation_requirements {
    bool valid = false; ///< Whether the requested shape has a valid memory plan.
    std::uint64_t arena_requested_bytes = 0;              ///< Logical arena bytes requested for the batch.
    std::uint64_t arena_allocation_bytes = 0;             ///< Arena allocation bytes, including any larger existing buffer.
    std::uint64_t pessimistic_output_requested_bytes = 0; ///< Output bytes at the caller's maximum minimizer count.
    std::uint64_t pessimistic_output_allocation_bytes = 0; ///< Vulkan allocation size for that maximum output.
};

/** @brief Downloaded seed matches and measurements from their GPU lookup. */
struct seed_lookup_result {
    std::vector<std::byte> host_bytes;
    std::uint64_t count = 0;
    std::uint64_t actual_vulkan_allocation_bytes = 0u;
    seed_anchor_canonical_order_audit anchor_order_audit{};
    std::uint64_t canonical_tie_normalization_runs = 0u;
    std::uint64_t canonical_tie_normalization_anchors = 0u;
    std::uint64_t seed_lookup_dispatches = 0;
    std::uint64_t open_addressed_lookup_dispatches = 0;
    std::uint64_t sorted_dense_lookup_dispatches = 0;
    seed_lookup_sort_execution_stats sort_stats{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // Attach  the completed operation record after the dispatch, fence wyit,
    // and host copy have been checked This token stays on the host.
    runtime_pipeline_operation_token seed_operation_receipt{};
#endif
};


/// @brief Query minimizers and a target index used to find matching seeds.
/// The query can use an existing device buffer or host bytes uploaded for this
/// lookup. The caller keeps the target index buffers alive on the device
/// throughout the call.
struct runtime_seed_lookup_inputs {
    std::span<const std::byte> host_minimizer_bytes{}; ///< Host minimizer bytes uploaded once when absent on device.
    const buffer* device_minimizer_words = nullptr;    ///< Existing device minimizers, when the sketch ran on Vulkan.
    const buffer* slots = nullptr;                     ///< Target hash buckets or an open-addressed slot table.
    const buffer* positions = nullptr;                 ///< Device buffer of encoded target positions.
    std::uint32_t slot_mask = 0;                       ///< Open-addressing mask when `bucket_count` is zero.
    // A nonzero count selects binary search over  hash-sorted buckets.
    // Zero selects an open-addressed table indexed with slot_mask.
    std::uint32_t bucket_count = 0;                    ///< Dense bucket count when slots are hash-sorted, else zero.
    std::uint32_t position_count = 0;                ///< Number of encoded positions in `positions`.
    std::uint32_t query_minimizer_record_offset = 0; ///< First query minimizer record within the shared buffer.
    std::uint32_t minimizer_word_kind = 0;             ///< Minimizer word encoding kind for shaders.
    std::uint32_t minimizer_words32_per_record = 0;    ///< `uint32_t` words per minimizer record.
    std::uint32_t hash_shift = 0;                      ///< Right shift used to extract the hash from a minimizer word.
    std::uint32_t pos_bits = 0;                        ///< Bit width of encoded positions.
    std::uint64_t target_len = 0;                      ///< Target sequence length in bases.
    std::uint64_t target_position_offset = 0;            ///< Base offset added to decoded target positions.
};

} // namespace bioseq::gpu::vulkan
