/**
 * @file sketch.cppm
 * @brief Defines sketch results, query authority, and seed lookup inputs.
 */
module;

#include <cstddef>
#include <cstdint>

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
    std::uint32_t anchor_count;
    std::uint32_t range_count;
    std::int32_t max_dist_x;
    std::int32_t max_dist_y;
    std::int32_t bandwidth;
    std::int32_t max_iter;
    float gap_penalty;
    float skip_penalty;
    std::int32_t max_chain_skip; // : nearest-first early-exit (minimap2 max_skip); <=0 = disabled
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
    std::uint32_t symbols_per_seq = 0;
    std::uint32_t units_per_seq = 0;
    std::uint32_t chunks_per_seq = 0;
    std::uint32_t chunk_units = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t total_chunks = 0;
    std::uint32_t chunk_base = 0;
    std::uint32_t seed_lo = 0;
    std::uint32_t seed_hi = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(generate_units_push) == 48);
static_assert(std::is_standard_layout_v<generate_units_push>);

/// @brief Result of one complete minimizer sketch roundtrip. The
/// resident `minimizer_words` device buffer is moved into the caller's
/// `vulkan_device_minimizer_storage` for seed-lookup reuse.  Host materialisation
/// is deliberately deferred to the caller, after the arena and packed-sequence
/// transients have been released and the exact `count` is known.
struct runtime_sketch_result {
    buffer minimizer_words;
    std::uint64_t count = 0;
    // Operation-originated lifetime receipt.  A successful roundtrip may hand
    // its standalone output buffer to the caller only after every shared-arena
    // backing buffer has been destroyed.  The caller consumes this receipt
    // before shrinking dispatch admission or reserving exact host storage.
    bool arena_trimmed_before_return = false;
    bool overflow = false;
};

/// @brief Whether a completed Vulkan sketch must also materialize its packed hashes
/// in a host vector.  Device-only consumers still receive the exact count and
/// resident device buffer. They do not pay for a hash download/decode.
enum class vulkan_sketch_host_materialization : std::uint8_t {
    host_and_device,
    device_only,
};

/// @brief Dispatch statistics recorded by both batch sketch protocols.  The
/// established host-materialized path records count/scan/emit in one submit.
/// device-only mode separates emit so its output can use the exact scanned
/// count.  Default construction is deliberately invalid.
class runtime_sketch_dispatch_stats {
  public:
    runtime_sketch_dispatch_stats() = default;
    runtime_sketch_dispatch_stats(const runtime_sketch_dispatch_stats&) = default;
    runtime_sketch_dispatch_stats& operator=(const runtime_sketch_dispatch_stats&) = default;

    [[nodiscard]] constexpr vulkan_sketch_host_materialization materialization() const noexcept {
        return materialization_;
    }
    [[nodiscard]] constexpr std::uint64_t count_dispatches() const noexcept {
        return count_dispatches_;
    }
    [[nodiscard]] constexpr std::uint64_t prefix_scan_sequences() const noexcept {
        return prefix_scan_sequences_;
    }
    [[nodiscard]] constexpr std::uint64_t prefix_scan_dispatches() const noexcept {
        return prefix_scan_dispatches_;
    }
    [[nodiscard]] constexpr std::uint64_t emit_dispatches() const noexcept {
        return emit_dispatches_;
    }
    [[nodiscard]] constexpr std::uint64_t completed_roundtrips() const noexcept {
        return completed_roundtrips_;
    }
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
        vulkan_sketch_host_materialization::host_and_device;
    std::uint64_t count_dispatches_ = 0;
    std::uint64_t prefix_scan_sequences_ = 0;
    std::uint64_t prefix_scan_dispatches_ = 0;
    std::uint64_t emit_dispatches_ = 0;
    std::uint64_t completed_roundtrips_ = 0;
};

static_assert(!runtime_sketch_dispatch_stats{}.is_internally_consistent());

/// @brief Exact per-query partition of one batched query-minimizer image.
struct gpu_seed_query_record_wire {
    std::uint32_t minimizer_offset = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t query_len = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(gpu_seed_query_record_wire) == 16);
static_assert(alignof(gpu_seed_query_record_wire) == alignof(std::uint32_t));

/// @brief Query-to-tile grouping supplied with the exact sketch submission. runtime
/// derives and seals the final query records from this layout plus the device
/// count/scan output. Coordinate callers cannot repartition the resident image.
struct gpu_sketch_query_layout_wire {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
    std::uint32_t query_len = 0;
};

/// @brief Move-only authority for one device-only query sketch.  Only runtime can
/// issue it, after the exact count/scan/emit submissions have completed.  The
/// authority is consumed by the persisted grouped-SoA coordinate operation. A
/// copied dispatch counter can therefore never stand in for the resident
/// query buffer that produced the operation.
class device_query_sketch_authorization {
  public:
    device_query_sketch_authorization() = default;
    device_query_sketch_authorization(const device_query_sketch_authorization&) = delete;
    device_query_sketch_authorization& operator=(const device_query_sketch_authorization&) = delete;
    device_query_sketch_authorization(device_query_sketch_authorization&& other) noexcept {
        move_from(other);
    }
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
        const bool shape_valid =
            identity_ != 0u && runtime_ != nullptr && buffer_ != VK_NULL_HANDLE &&
            memory_identity_ != 0u && buffer_bytes_ != 0u && allocation_bytes_ >= buffer_bytes_ &&
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
    [[nodiscard]] std::uint64_t identity() const noexcept {
        return identity_;
    }
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

    void clear() noexcept {
        identity_ = 0u;
        runtime_ = nullptr;
        buffer_ = VK_NULL_HANDLE;
        memory_identity_ = 0u;
        buffer_bytes_ = 0u;
        allocation_bytes_ = 0u;
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
    void move_from(device_query_sketch_authorization& other) noexcept {
        identity_ = other.identity_;
        runtime_ = other.runtime_;
        buffer_ = other.buffer_;
        memory_identity_ = other.memory_identity_;
        buffer_bytes_ = other.buffer_bytes_;
        allocation_bytes_ = other.allocation_bytes_;
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

    std::uint64_t identity_ = 0u;
    runtime* runtime_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    std::uint64_t memory_identity_ = 0u;
    std::uint64_t buffer_bytes_ = 0u;
    std::uint64_t allocation_bytes_ = 0u;
    std::uint64_t minimizer_count_ = 0u;
    std::uint32_t window_size_ = 0u;
    std::uint32_t kmer_size_ = 0u;
    std::uint32_t minimizer_word_kind_ = 0u;
    std::uint32_t minimizer_words32_per_record_ = 0u;
    std::uint32_t hash_shift_ = 0u;
    std::uint32_t pos_bits_ = 0u;
    vulkan_sketch_host_materialization materialization_ =
        vulkan_sketch_host_materialization::host_and_device;
    runtime_sketch_dispatch_stats dispatch_stats_{};
    runtime_pipeline_operation_token pipeline_token_{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid_{};
    std::vector<gpu_seed_query_record_wire> query_records_{};
    std::vector<std::uint32_t> query_tile_counts_{};
};

static_assert(!std::is_copy_constructible_v<device_query_sketch_authorization>);
static_assert(std::is_nothrow_move_constructible_v<device_query_sketch_authorization>);

/// @brief Resident and optionally host-materialized outputs from a sketch batch.
struct runtime_sketch_batch_result {
    buffer minimizer_words;
    std::vector<std::byte> host_minimizer_bytes;
    std::vector<std::uint32_t> tile_counts;
    std::vector<std::uint32_t> tile_offsets;
    runtime_sketch_dispatch_stats dispatch_stats{};
    device_query_sketch_authorization query_authority{};
    std::uint64_t count = 0;
    bool overflow = false;
};

/// @brief Physical admission plan for one batched GPU frontend sketch.  The arena
/// allocation includes any larger persistent high-water buffer already owned
/// by the runtime. The output allocation uses the runtime-private compute
/// buffer usage flags rather than asking callers to reproduce them.
struct gpu_sketch_batch_allocation_requirements {
    bool valid = false;
    std::uint64_t arena_requested_bytes = 0;
    std::uint64_t arena_allocation_bytes = 0;
    std::uint64_t pessimistic_output_requested_bytes = 0;
    std::uint64_t pessimistic_output_allocation_bytes = 0;
};

/// @brief Inputs to the seed-hit lookup roundtrip. The query minimizers
/// arrive either as a resident device buffer (`device_minimizer_words`,
/// all-Vulkan sketch+seed path) or as host bytes to upload once. `slots` and
/// `positions` are the GPU-resident index buffers owned by
/// `gpu_minimizer_index_cache`. The runtime never uploads the index itself.
struct runtime_seed_lookup_inputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const buffer* device_minimizer_words = nullptr;
    const buffer* slots = nullptr;
    const buffer* positions = nullptr;
    std::uint32_t slot_mask = 0;
    // Nonzero selects hash-sorted dense controls and is the exact control
    // count. Zero selects open addressing via `slot_mask`.
    std::uint32_t bucket_count = 0;
    std::uint32_t position_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint64_t target_len = 0;
    std::uint64_t target_position_offset = 0;
};

} // namespace bioseq::gpu::vulkan
