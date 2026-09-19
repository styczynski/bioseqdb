/**
 * @file sketch.cppm
 * @brief Defines sketch results, query authority, and seed lookup inputs.
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

/// @brief The 36-byte push-constant block consumed by the tiled-DP shader. Layout is
/// pinned to the GLSL `Params` block.
struct chaining_push_constants {
    std::uint32_t anchor_count;  ///< Number of anchors loaded for chaining.
    std::uint32_t range_count;   ///< Number of sorted anchor ranges.
    std::int32_t max_dist_x;     ///< Maximum anchor distance along the query axis.
    std::int32_t max_dist_y;     ///< Maximum anchor distance along the target axis.
    std::int32_t bandwidth;      ///< DP band half-width in anchor index space.
    std::int32_t max_iter;       ///< Maximum DP iterations per anchor.
    float gap_penalty;           ///< Gap-open penalty passed to the shader.
    float skip_penalty;          ///< Skip penalty passed to the shader.
    std::int32_t max_chain_skip; ///< Nearest-first early-exit limit (minimap2 `max_skip`), or disabled when `<= 0`.
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

/// @brief Push-constant block for the  packed sequence generator kernel.
struct generate_units_push {
    std::uint32_t symbols_per_seq = 0;   ///< Total symbols generated per sequence.
    std::uint32_t units_per_seq = 0;     ///< Packed units emitted per sequence.
    std::uint32_t chunks_per_seq = 0;    ///< Generator chunks scheduled per sequence.
    std::uint32_t chunk_units = 0;       ///< Packed units produced per chunk.
    std::uint32_t letters_per_unit = 0;  ///< Letters encoded in each packed unit.
    std::uint32_t bits_per_char = 0;     ///< Bits per alphabet symbol in packed units.
    std::uint32_t alphabet_size = 0;     ///< Alphabet cardinality used by the generator.
    std::uint32_t total_chunks = 0;      ///< Global chunk count across the batch.
    std::uint32_t chunk_base = 0;        ///< First global chunk index in this dispatch.
    std::uint32_t seed_lo = 0;           ///< Low 32 bits of the deterministic seed.
    std::uint32_t seed_hi = 0;           ///< High 32 bits of the deterministic seed.
    std::uint32_t pad = 0;               ///< Padding to the 48-byte GLSL layout.
};
static_assert(sizeof(generate_units_push) == 48);
static_assert(std::is_standard_layout_v<generate_units_push>);

/// @brief Result of one complete minimizer sketch roundtrip. The
/// resident `minimizer_words` device buffer is moved into the caller's
/// `vulkan_device_minimizer_storage` for seed-lookup reuse.  Host materialisation
/// is deliberately deferred to the caller, after the arena and packed-sequence
/// transients have been released and the exact `count` is known.
struct runtime_sketch_result {
    buffer minimizer_words;              ///< Resident minimizer device buffer moved to the caller.
    std::uint64_t count = 0;             ///< Exact minimizer count from the count pass.
    // Operation-originated lifetime receipt.  A successful roundtrip may hand
    // its standalone output buffer to the caller only after every shared-arena
    // backing buffer has been destroyed.  The caller consumes this receipt
    // before shrinking dispatch admission or reserving exact host storage.
    bool arena_trimmed_before_return = false; ///< Arena idle capacity was released before return.
    bool overflow = false;                   ///< Set when the exact count exceeded caller limits.
};

/// @brief Whether a completed Vulkan sketch must also materialize its packed hashes
/// in a host vector.  Device-only consumers still receive the exact count and
/// resident device buffer. They do not pay for a hash download/decode.
enum class vulkan_sketch_host_materialization : std::uint8_t {
    host_and_device, ///< Download minimizer hashes to host after one queue roundtrip.
    device_only,     ///< Keep minimizer words resident on device across two roundtrips.
};

/// @brief Dispatch statistics recorded by both batch sketch protocols.  The
/// established host-materialized path records count/scan/emit in one submit.
/// device-only mode separates emit so its output can use the exact scanned
/// count.  Default construction is deliberately invalid.
class runtime_sketch_dispatch_stats {
  public:
    /// @brief Constructs an intentionally invalid zero-dispatch statistic set.
    runtime_sketch_dispatch_stats() = default;
    /// @brief Copies immutable dispatch statistics.
    runtime_sketch_dispatch_stats(const runtime_sketch_dispatch_stats&) = default;
    /// @brief Replaces this object with copied dispatch statistics.
    /// @return This statistics object.
    runtime_sketch_dispatch_stats& operator=(const runtime_sketch_dispatch_stats&) = default;

    /// @brief Returns the requested host-materialization policy.
    /// @return Materialization policy recorded for the operation.
    [[nodiscard]] constexpr vulkan_sketch_host_materialization materialization() const noexcept {
        return materialization_;
    }
    /// @brief Returns exact-count dispatches.
    /// @return Number of exact-count dispatches.
    [[nodiscard]] constexpr std::uint64_t count_dispatches() const noexcept {
        return count_dispatches_;
    }
    /// @brief Returns sequences covered by prefix scanning.
    /// @return Number of prefix-scanned sequences.
    [[nodiscard]] constexpr std::uint64_t prefix_scan_sequences() const noexcept {
        return prefix_scan_sequences_;
    }
    /// @brief Returns prefix-scan dispatches.
    /// @return Number of prefix-scan dispatches.
    [[nodiscard]] constexpr std::uint64_t prefix_scan_dispatches() const noexcept {
        return prefix_scan_dispatches_;
    }
    /// @brief Returns minimizer-emission dispatches.
    /// @return Number of emit dispatches.
    [[nodiscard]] constexpr std::uint64_t emit_dispatches() const noexcept {
        return emit_dispatches_;
    }
    /// @brief Returns completed queue roundtrips.
    /// @return Number of completed roundtrips.
    [[nodiscard]] constexpr std::uint64_t completed_roundtrips() const noexcept {
        return completed_roundtrips_;
    }
    /// @brief Validates dispatch shape against the materialization policy.
    /// @return `true` when all dispatch and roundtrip counts form a valid operation.
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
        vulkan_sketch_host_materialization::host_and_device; ///< Recorded host-materialization policy.
    std::uint64_t count_dispatches_ = 0;                     ///< Exact-count dispatches recorded.
    std::uint64_t prefix_scan_sequences_ = 0;                ///< Prefix-scanned sequences recorded.
    std::uint64_t prefix_scan_dispatches_ = 0;               ///< Prefix-scan dispatches recorded.
    std::uint64_t emit_dispatches_ = 0;                      ///< Emit dispatches recorded.
    std::uint64_t completed_roundtrips_ = 0;                 ///< Completed queue roundtrips recorded.
};

static_assert(!runtime_sketch_dispatch_stats{}.is_internally_consistent());

/// @brief Exact per-query partition of one batched query-minimizer image.
struct gpu_seed_query_record_wire {
    std::uint32_t minimizer_offset = 0; ///< Offset into the batched minimizer image.
    std::uint32_t minimizer_count = 0;  ///< Minimizer records belonging to one query.
    std::uint32_t query_len = 0;        ///< Query length in bases for this partition.
    std::uint32_t flags = 0;            ///< Reserved wire flags, zero for valid records.
};
static_assert(sizeof(gpu_seed_query_record_wire) == 16);
static_assert(alignof(gpu_seed_query_record_wire) == alignof(std::uint32_t));

/// @brief Query-to-tile grouping supplied with the exact sketch submission. runtime
/// derives and seals the final query records from this layout plus the device
/// count/scan output. Coordinate callers cannot repartition the resident image.
struct gpu_sketch_query_layout_wire {
    std::uint32_t begin = 0;     ///< First tile index included in the query partition.
    std::uint32_t count = 0;     ///< Number of tiles in the partition.
    std::uint32_t query_len = 0; ///< Query length in bases for the partition.
};

/// @brief Move-only authority for one device-only query sketch.  Only runtime can
/// issue it, after the exact count/scan/emit submissions have completed.  The
/// authority is consumed by either persisted grouped-SoA coordinate execution
/// or its seed-only MPS prefix. The authorized host readback borrows it without
/// consuming it. A copied dispatch counter can therefore never stand in for the
/// resident query buffer that produced the operation.
class device_query_sketch_authorization {
  public:
    /// @brief Constructs an empty invalid authorization.
    device_query_sketch_authorization() = default;
    /// @brief Prevents copying one-shot query authority.
    device_query_sketch_authorization(const device_query_sketch_authorization&) = delete;
    /// @brief Prevents copy-assignment of one-shot query authority.
    device_query_sketch_authorization& operator=(const device_query_sketch_authorization&) = delete;
    /// @brief Transfers query authority and clears the source.
    device_query_sketch_authorization(device_query_sketch_authorization&& other) noexcept {
        move_from(other);
    }
    /// @brief Clears current authority, then transfers authority from another object.
    /// @return This authorization.
    device_query_sketch_authorization&
    operator=(device_query_sketch_authorization&& other) noexcept {
        if (this != &other) {
            clear();
            move_from(other);
        }
        return *this;
    }

    /// @brief Structural, device-output coverage. This deliberately does not claim
    /// the immutable pipeline provenance required by persisted grouped-SoA.
    /// @return `true` when every authorized minimizer span covers device output.
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
    /// @brief Strong one-shot authority accepted by the persisted grouped-SoA consumer.
    /// @return `true` when output coverage and the pipeline token are valid.
    [[nodiscard]] bool is_valid() const noexcept {
        return does_cover_device_output() && pipeline_token_.is_valid_for_query_sketch();
    }
    /// @brief Returns the unique authorization identity.
    /// @return Runtime-issued identity, or zero for an invalid authorization.
    [[nodiscard]] std::uint64_t identity() const noexcept {
        return identity_;
    }
    /// @brief Returns the exact typed minimizer payload covered by this authority.
    /// @return Logical payload bytes, excluding Vulkan's nonzero empty-buffer padding.
    [[nodiscard]] std::uint64_t logical_minimizer_bytes() const noexcept {
        return logical_minimizer_bytes_;
    }
    /// @brief Computes retained host-vector capacity owned by the authorization.
    /// @return Retained bytes, saturated at the maximum `uint64_t` value.
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

    /// @brief Invalidates authority and releases all retained host elements.
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
    /// @brief Transfers every authority field and clears the source.
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

    std::uint64_t identity_ = 0u;                              ///< Runtime-issued one-shot authorization identity.
    runtime* runtime_ = nullptr;                                 ///< Issuing runtime, or null after `clear`.
    VkBuffer buffer_ = VK_NULL_HANDLE;                           ///< Resident minimizer buffer handle.
    std::uint64_t memory_identity_ = 0u;                        ///< Device-memory identity for `buffer_`.
    std::uint64_t buffer_bytes_ = 0u;                           ///< Vulkan buffer range, including required padding.
    std::uint64_t allocation_bytes_ = 0u;                        ///< Physical allocation bytes for `buffer_`.
    std::uint64_t logical_minimizer_bytes_ = 0u;                 ///< Exact record payload excluding Vulkan's empty minimum.
    std::uint64_t minimizer_count_ = 0u;                         ///< Total minimizer records authorized.
    std::uint32_t window_size_ = 0u;                             ///< Sketch window size sealed into authority.
    std::uint32_t kmer_size_ = 0u;                               ///< Sketch k-mer size sealed into authority.
    std::uint32_t minimizer_word_kind_ = 0u;                     ///< Minimizer word encoding kind.
    std::uint32_t minimizer_words32_per_record_ = 0u;            ///< `uint32_t` words per minimizer record.
    std::uint32_t hash_shift_ = 0u;                              ///< Hash shift sealed into authority.
    std::uint32_t pos_bits_ = 0u;                                ///< Encoded-position bit width sealed into authority.
    vulkan_sketch_host_materialization materialization_ =
        vulkan_sketch_host_materialization::host_and_device; ///< Materialization policy at issuance.
    runtime_sketch_dispatch_stats dispatch_stats_{};             ///< Dispatch statistics sealed into authority.
    runtime_pipeline_operation_token pipeline_token_{};            ///< Pipeline provenance token for grouped-SoA.
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid_{};       ///< Device UUID at issuance.
    std::vector<gpu_seed_query_record_wire> query_records_{};      ///< Exact per-query minimizer partitions.
    std::vector<std::uint32_t> query_tile_counts_{};               ///< Tile counts parallel to `query_records_`.
};

static_assert(!std::is_copy_constructible_v<device_query_sketch_authorization>);
static_assert(std::is_nothrow_move_constructible_v<device_query_sketch_authorization>);

/// @brief Resident and optionally host-materialized outputs from a sketch batch.
struct runtime_sketch_batch_result {
    buffer minimizer_words;                         ///< Resident minimizer device buffer for the batch.
    std::vector<std::byte> host_minimizer_bytes;    ///< Host minimizer bytes when materialized.
    std::vector<std::uint32_t> tile_counts;         ///< Per-tile minimizer counts from the scan pass.
    std::vector<std::uint32_t> tile_offsets;        ///< Prefix offsets into the minimizer image.
    runtime_sketch_dispatch_stats dispatch_stats{}; ///< Recorded count, scan, and emit dispatches.
    device_query_sketch_authorization query_authority{}; ///< One-shot device-only query authority.
    std::uint64_t count = 0;                        ///< Exact minimizer count across the batch.
    bool overflow = false;                          ///< Set when the count exceeded caller limits.
};

/// @brief Physical admission plan for one batched GPU frontend sketch.  The arena
/// allocation includes any larger persistent high-water buffer already owned
/// by the runtime. The output allocation uses the runtime-private compute
/// buffer usage flags rather than asking callers to reproduce them.
struct gpu_sketch_batch_allocation_requirements {
    bool valid = false; ///< Whether the planner produced a consistent admission plan.
    std::uint64_t arena_requested_bytes = 0;              ///< Logical arena bytes requested for the batch.
    std::uint64_t arena_allocation_bytes = 0;             ///< Physical arena bytes including runtime high-water.
    std::uint64_t pessimistic_output_requested_bytes = 0; ///< Logical output bytes at caller pessimistic cap.
    std::uint64_t pessimistic_output_allocation_bytes = 0; ///< Physical output bytes at pessimistic cap.
};

/** @brief Owns downloaded anchor bytes and telemetry from one seed lookup. */
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
    // Host operation ownership belongs to the runtime, not its wire ABI. The
    // token is attached only after actual dispatch/fence and readback checks.
    runtime_pipeline_operation_token seed_operation_receipt{};
#endif
};


/// @brief Inputs to the seed-hit lookup roundtrip. The query minimizers
/// arrive either as a resident device buffer (`device_minimizer_words`,
/// all-Vulkan sketch+seed path) or as host bytes to upload once. `slots` and
/// `positions` are the GPU-resident index buffers owned by
/// `gpu_minimizer_index_cache`. The runtime never uploads the index itself.
struct runtime_seed_lookup_inputs {
    std::span<const std::byte> host_minimizer_bytes{}; ///< Host minimizer bytes uploaded once when absent on device.
    const buffer* device_minimizer_words = nullptr;    ///< Resident minimizer buffer for the all-Vulkan path.
    const buffer* slots = nullptr;                     ///< GPU-resident index slot table from the minimizer cache.
    const buffer* positions = nullptr;                 ///< GPU-resident encoded positions buffer.
    std::uint32_t slot_mask = 0;                       ///< Open-addressing mask when `bucket_count` is zero.
    // Nonzero selects hash-sorted dense controls and is the exact control
    // count. Zero selects open addressing via `slot_mask`.
    std::uint32_t bucket_count = 0;                    ///< Dense bucket count when slots are hash-sorted, else zero.
    std::uint32_t position_count = 0;                ///< Number of encoded positions in `positions`.
    std::uint32_t query_minimizer_record_offset = 0; ///< First query record in the bound whole buffer.
    std::uint32_t minimizer_word_kind = 0;             ///< Minimizer word encoding kind for shaders.
    std::uint32_t minimizer_words32_per_record = 0;    ///< `uint32_t` words per minimizer record.
    std::uint32_t hash_shift = 0;                      ///< Right-shift applied before slot indexing.
    std::uint32_t pos_bits = 0;                        ///< Bit width of encoded positions.
    std::uint64_t target_len = 0;                      ///< Target sequence length in bases.
    std::uint64_t target_position_offset = 0;            ///< Base offset added to decoded target positions.
};

} // namespace bioseq::gpu::vulkan
