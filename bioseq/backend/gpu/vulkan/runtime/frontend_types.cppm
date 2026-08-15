/**
 * @file frontend_types.cppm
 * @brief Defines Vulkan frontend inputs, outputs, arena ownership, and runtime layout.
 */
module;

#include <cstddef>
#include <cstdint>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:frontend_types;

export import :receipts;
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

// ===========================================================================
// Runtime-owned GPU arena
// ===========================================================================

/// @brief A view into one arena buffer: `(buffer, offset, size)`. Descriptor writes
/// bind exactly this sub-range, so several logical resources can share one
/// physical arena buffer.
struct gpu_arena_slice {
    buffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        return buffer != nullptr;
    }
};

/// @brief A snapshot of the three arena cursors. `rewind(mark)` restores all three.
struct gpu_arena_mark {
    std::uint64_t device_cursor = 0;
    std::uint64_t upload_cursor = 0;
    std::uint64_t download_cursor = 0;
};

/// @brief Per-runtime arena with three geometrically-grown, persistently-mapped
/// buffers (device-local, host-visible upload, host-visible download). Slices
/// are bump-allocated with alignment. `rewind` frees everything past a mark.
/// Growth is allowed only outside an active count and execution submission pair.
class gpu_arena {
  public:
    explicit gpu_arena(runtime& runtime) noexcept : runtime_(&runtime) {}

    [[nodiscard]] gpu_arena_mark mark() const noexcept {
        return gpu_arena_mark{device_cursor_, upload_cursor_, download_cursor_};
    }
    void rewind(gpu_arena_mark mark) noexcept {
        device_cursor_ = mark.device_cursor;
        upload_cursor_ = mark.upload_cursor;
        download_cursor_ = mark.download_cursor;
    }

    /// @brief Reserve worst-case capacity for one count and execution submission pair
    /// before count submit,
    /// so no buffer is reallocated between the two submits. Returns false if any
    /// grow failed (caller declines GPU frontend).
    /// @return `true` when every arena buffer satisfies the requested capacity.
    [[nodiscard]] bool ensure_capacity(std::uint64_t device_bytes,
                                       std::uint64_t upload_bytes,
                                       std::uint64_t download_bytes);

    [[nodiscard]] gpu_arena_slice allocate_device(std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] gpu_arena_slice allocate_upload(std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] gpu_arena_slice allocate_download(std::uint64_t bytes, std::uint64_t alignment);

    void bind_storage_slice(VkWriteDescriptorSet& write,
                            VkDescriptorBufferInfo& info,
                            std::uint32_t binding,
                            VkDescriptorSet dst_set,
                            const gpu_arena_slice& slice) const noexcept;

    [[nodiscard]] buffer& device_buffer() noexcept {
        return device_;
    }
    [[nodiscard]] buffer& upload_buffer() noexcept {
        return upload_;
    }
    [[nodiscard]] buffer& download_buffer() noexcept {
        return download_;
    }

    /// @brief Destroy every retained backing buffer after an operation has rewound all
    /// of its slices. Seed lookup is a useful full-lifetime boundary: its output
    /// is copied to owned host storage, while adopted target-index buffers are
    /// standalone allocations and therefore cannot alias this arena. Releasing
    /// the idle arena prevents large transient high-water from leaking across
    /// otherwise independent benchmark/production requests.
    /// @return `true` when all idle backing capacity was released.
    [[nodiscard]] bool release_idle_capacity() noexcept;

  private:
    [[nodiscard]] static std::uint64_t align_up(std::uint64_t value,
                                                std::uint64_t alignment) noexcept {
        if (alignment <= 1u)
            return value;
        return bioseq::common::checked_align_up(value, alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    }

    runtime* runtime_ = nullptr;
    buffer device_;
    buffer upload_;
    buffer download_;
    std::uint64_t device_cursor_ = 0;
    std::uint64_t upload_cursor_ = 0;
    std::uint64_t download_cursor_ = 0;
    std::uint64_t device_capacity_ = 0;
    std::uint64_t upload_capacity_ = 0;
    std::uint64_t download_capacity_ = 0;
};

/// @brief RAII arena reservation: holds the admission permit, marks the arena on
/// construction, and rewinds on destruction. Spans count submit + execution submit.
class gpu_arena_reservation {
  public:
    gpu_arena_reservation(gpu_arena& arena, bioseq::parallelism::memory_permit permit) noexcept
        : arena_(&arena), permit_(std::move(permit)), mark_(arena.mark()) {}
    gpu_arena_reservation(const gpu_arena_reservation&) = delete;
    gpu_arena_reservation& operator=(const gpu_arena_reservation&) = delete;
    gpu_arena_reservation(gpu_arena_reservation&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)), permit_(std::move(other.permit_)),
          mark_(other.mark_) {}
    gpu_arena_reservation& operator=(gpu_arena_reservation&& other) noexcept {
        if (this != &other) {
            release();
            arena_ = std::exchange(other.arena_, nullptr);
            permit_ = std::move(other.permit_);
            mark_ = other.mark_;
        }
        return *this;
    }
    ~gpu_arena_reservation() {
        release();
    }
    void release() noexcept {
        if (arena_ != nullptr) {
            arena_->rewind(mark_);
            arena_ = nullptr;
        }
    }
    [[nodiscard]] gpu_arena& arena() noexcept {
        return *arena_;
    }

  private:
    gpu_arena* arena_ = nullptr;
    bioseq::parallelism::memory_permit permit_;
    gpu_arena_mark mark_{};
};

/// @brief Request-boundary cleanup for operations whose result never aliases the
/// shared GPU frontend arena.  Rewinding alone leaves the arena's allocator-rounded
/// high-water live. This guard also destroys all idle backing buffers. It is
/// intentionally independent of an admission permit so it can be installed
/// before arena growth and covers every return and exception path.
class gpu_arena_trim_guard {
  public:
    explicit gpu_arena_trim_guard(gpu_arena& arena) noexcept
        : arena_(&arena), mark_(arena.mark()) {}
    gpu_arena_trim_guard(const gpu_arena_trim_guard&) = delete;
    gpu_arena_trim_guard& operator=(const gpu_arena_trim_guard&) = delete;
    gpu_arena_trim_guard(gpu_arena_trim_guard&&) = delete;
    gpu_arena_trim_guard& operator=(gpu_arena_trim_guard&&) = delete;
    ~gpu_arena_trim_guard() {
        (void)release_now();
    }

    [[nodiscard]] bool release_now() noexcept {
        if (arena_ == nullptr) {
            return true;
        }
        gpu_arena* arena = std::exchange(arena_, nullptr);
        arena->rewind(mark_);
        return arena->release_idle_capacity();
    }

  private:
    gpu_arena* arena_ = nullptr;
    gpu_arena_mark mark_{};
};

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

/// @brief Common query-side inputs for a batched resident seed→chain→coordinate run.
/// The query minimizer words are shared by every job in the batch. Source-direct jobs
/// may supply distinct slot-table targets. An authenticated grouped-SoA direct
/// operation is intentionally restricted to one resident generation/view.
struct gpu_seed_query_inputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const buffer* device_minimizer_words = nullptr;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t window_size = 0;
    std::span<const gpu_seed_query_record_wire> host_query_records{};
    const buffer* device_query_records = nullptr;
};

/// @brief Neutral resident view of one authenticated persisted grouped-SoA generation.
/// The owner keeps all three buffers alive for the complete batch.  Byte sizes
/// are logical admitted sizes. The backing raw control/position buffers may be
/// padded to whole uint32 words for GLSL access, but their representation is
/// never decoded or rebuilt by this runtime.
struct gpu_grouped_soa_resident_view {
    const buffer* descriptors = nullptr;
    const buffer* controls = nullptr;
    const buffer* positions = nullptr;
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
};

/// @brief Mutually exclusive resident target-index representations accepted by seeding.
enum class gpu_seed_index_representation : std::uint8_t {
    invalid = 0,
    source_direct_sorted_dense = 1,
    grouped_soa = 2,
};

/// @brief One resident target-index job in a batched seed→coordinate run. Source-direct
/// slot-table fields and `grouped_soa` are an XOR: selecting both (or neither) is
/// invalid rather than permission to fall back to another implementation.
struct gpu_seed_coordinate_job {
    const buffer* slots = nullptr;
    const buffer* positions = nullptr;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t bucket_count = 0;
    std::uint64_t target_position_offset = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    const gpu_grouped_soa_resident_view* grouped_soa = nullptr;
};

[[nodiscard]] inline constexpr gpu_seed_index_representation
classify_gpu_seed_index_representation(const gpu_seed_coordinate_job& job) noexcept {
    const bool grouped_soa_only = job.grouped_soa != nullptr && job.slots == nullptr &&
                                  job.positions == nullptr && job.slot_mask == 0u &&
                                  job.position_count == 0u && job.bucket_count == 0u &&
                                  job.target_position_offset == 0u;
    if (grouped_soa_only)
        return gpu_seed_index_representation::grouped_soa;

    const bool source_direct_only =
        job.grouped_soa == nullptr && job.slots != nullptr && job.positions != nullptr;
    return source_direct_only ? gpu_seed_index_representation::source_direct_sorted_dense
                              : gpu_seed_index_representation::invalid;
}

/// @brief Explicit algorithm identity for the seed-to-coordinate backend.  A caller
/// selecting Vulkan tiled-DP must pass `vulkan_tiled_dp`. The approximate
/// coordinate-vote prototype is a distinct algorithm and may never be entered
/// implicitly as an optimization of tiled-DP.
enum class gpu_seed_coordinate_algorithm : std::uint8_t {
    vulkan_tiled_dp = 0,
    coordinate_vote = 1,
};

/// @brief Exact algorithm inputs bound into a persisted grouped-SoA coordinate receipt.
/// Floating-point penalties are carried by bit pattern so NaNs, signed zero,
/// and every other dispatch-affecting value compare without normalization.
struct persisted_grouped_soa_coordinate_config_identity {
    std::uint32_t kmer_size = 0u;
    std::int32_t max_occ = -1;
    gpu_seed_coordinate_algorithm coordinate_algorithm =
        gpu_seed_coordinate_algorithm::vulkan_tiled_dp;
    chaining_pipeline_key chain_key{};
    std::int32_t max_dist_x = 0;
    std::int32_t max_dist_y = 0;
    std::int32_t bandwidth = 0;
    std::int32_t max_iter = 0;
    std::uint32_t gap_penalty_bits = 0u;
    std::uint32_t skip_penalty_bits = 0u;
    std::int32_t max_chain_skip = 0;
    std::int32_t min_chain_score = 0;
    std::int32_t min_seed_count = 0;
    std::uint32_t probe_limit = 0u;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return kmer_size != 0u && kmer_size <= 255u && max_occ >= 0 &&
               coordinate_algorithm == gpu_seed_coordinate_algorithm::vulkan_tiled_dp &&
               chain_key.tile_size != 0u && chain_key.local_size != 0u && probe_limit != 0u;
    }
    [[nodiscard]] bool
    operator==(const persisted_grouped_soa_coordinate_config_identity&) const = default;
};

[[nodiscard]] inline persisted_grouped_soa_coordinate_config_identity
make_persisted_grouped_soa_coordinate_config_identity(
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    gpu_seed_coordinate_algorithm coordinate_algorithm,
    const chaining_pipeline_key& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit) noexcept {
    return persisted_grouped_soa_coordinate_config_identity{
        .kmer_size = kmer_size,
        .max_occ = max_occ,
        .coordinate_algorithm = coordinate_algorithm,
        .chain_key = chain_key,
        .max_dist_x = max_dist_x,
        .max_dist_y = max_dist_y,
        .bandwidth = bandwidth,
        .max_iter = max_iter,
        .gap_penalty_bits = std::bit_cast<std::uint32_t>(gap_penalty),
        .skip_penalty_bits = std::bit_cast<std::uint32_t>(skip_penalty),
        .max_chain_skip = max_chain_skip,
        .min_chain_score = min_chain_score,
        .min_seed_count = min_seed_count,
        .probe_limit = probe_limit,
    };
}

static_assert(!persisted_grouped_soa_coordinate_config_identity{}.is_valid());

/// @brief Dispatch counts emitted by one seed-to-coordinate batch.
struct gpu_seed_coordinate_batch_stats {
    std::uint64_t problem_count = 0;
    std::uint64_t wave_count = 0;
    std::uint64_t seed_count_dispatches = 0;
    std::uint64_t seed_emit_dispatches = 0;
    std::uint64_t sort_dispatches = 0;
    std::uint64_t range_build_dispatches = 0;
    std::uint64_t chain_dispatches = 0;
    std::uint64_t chain_backtrack_dispatches = 0;
    std::uint64_t coordinate_vote_dispatches = 0;
};

[[nodiscard]] inline constexpr bool
are_coordinate_result_wires_equal(const gpu_coordinate_result_wire& lhs,
                                  const gpu_coordinate_result_wire& rhs) noexcept {
    return lhs.query_start == rhs.query_start && lhs.query_end == rhs.query_end &&
           lhs.target_start == rhs.target_start && lhs.target_end == rhs.target_end &&
           lhs.match_length == rhs.match_length && lhs.block_length == rhs.block_length &&
           lhs.chain_score == rhs.chain_score && lhs.seed_count == rhs.seed_count &&
           lhs.is_reverse == rhs.is_reverse && lhs.is_valid == rhs.is_valid;
}

[[nodiscard]] inline constexpr bool
is_coordinate_result_wire_well_formed(const gpu_coordinate_result_wire& result,
                                      const std::uint64_t query_len,
                                      const std::uint64_t target_len) noexcept {
    if (result.is_valid == 0u) {
        return are_coordinate_result_wires_equal(result, gpu_coordinate_result_wire{});
    }
    if (result.is_valid != 1u || result.is_reverse > 1u || result.seed_count <= 0 ||
        result.query_start >= result.query_end || result.query_end > query_len ||
        result.target_start >= result.target_end || result.target_end > target_len ||
        result.match_length > result.block_length) {
        return false;
    }
    const std::uint64_t query_span = result.query_end - result.query_start;
    const std::uint64_t target_span = result.target_end - result.target_start;
    return result.match_length <= query_span && result.match_length <= target_span &&
           result.block_length <= query_span + target_span;
}

/// @brief Exact caller-side identities checked while consuming one sealed operation
/// record.  These are expectations, not receipt: changing any value makes the
/// one-shot consume fail.
struct persisted_grouped_soa_coordinate_job_expectation {
    std::size_t problem_index = 0;
    std::uint64_t query_authority_identity = 0;
    runtime_buffer_identity resident_descriptors{};
    runtime_buffer_identity resident_controls{};
    runtime_buffer_identity resident_positions{};
    runtime_buffer_identity query_minimizer_buffer{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint64_t target_len = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    gpu_seed_query_record_wire query_record{};
    gpu_coordinate_result_wire result{};
    persisted_grouped_soa_coordinate_config_identity configuration{};
};

/// @brief Operation-local audit payload.  runtime fills this from successful Vulkan
/// calls. It is public only so the contract can be golden/tamper tested. It
/// cannot be converted into an operation receipt by callers.
struct persisted_grouped_soa_coordinate_job_audit {
    std::uint64_t operation_identity = 0;
    std::uint64_t query_authority_identity = 0;
    runtime_buffer_identity resident_descriptors{};
    runtime_buffer_identity resident_controls{};
    runtime_buffer_identity resident_positions{};
    runtime_buffer_identity query_minimizer_buffer{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint64_t descriptor_bytes = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t position_count = 0;
    std::uint64_t max_occ_cap = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t abi_revision = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t cold_load_identity = 0;
    std::uint64_t target_len = 0;
    std::size_t problem_index = 0;
    std::uint32_t query_index = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t logical_query_ordinal = 0;
    gpu_seed_query_record_wire query_record{};
    gpu_coordinate_result_wire result{};
    persisted_grouped_soa_coordinate_config_identity configuration{};
    std::uint32_t query_tile_count = 0u;
    std::uint64_t query_count_dispatches = 0;
    std::uint64_t query_scan_dispatches = 0;
    std::uint64_t query_emit_dispatches = 0;
    std::uint64_t query_completed_roundtrips = 0;
    std::uint64_t direct_count_dispatches = 0;
    std::uint64_t direct_scan_dispatches = 0;
    std::uint64_t direct_emit_dispatches = 0;
    std::uint64_t anchor_count = 0;
    std::uint64_t sort_operations = 0;
    std::uint64_t range_build_dispatches = 0;
    std::uint64_t range_count = 0;
    std::uint64_t chain_dispatches = 0;
    std::uint64_t chain_backtrack_dispatches = 0;
    std::uint64_t coordinate_dispatches = 0;
    std::uint64_t count_submit_wait_identity = 0;
    std::uint64_t emit_submit_wait_identity = 0;
    std::uint64_t sort_range_submit_wait_identity = 0;
    std::uint64_t chain_coordinate_submit_wait_identity = 0;
    std::uint64_t count_status_checks = 0;
    std::uint64_t emit_status_checks = 0;
    std::uint64_t result_readbacks = 0;
    std::uint32_t status_or = 0;
    std::uint64_t coordinate_vote_dispatches = 0;
    std::uint64_t cpu_or_simd_biological_calls = 0;
    std::uint64_t recompute_calls = 0;
    std::uint64_t fallback_calls = 0;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        const bool identity_and_query =
            operation_identity != 0u && query_authority_identity != 0u &&
            resident_descriptors.is_valid() && resident_controls.is_valid() &&
            resident_positions.is_valid() && query_minimizer_buffer.is_valid() &&
            resident_descriptors.object != resident_controls.object &&
            resident_descriptors.object != resident_positions.object &&
            resident_controls.object != resident_positions.object &&
            query_minimizer_buffer.object != resident_descriptors.object &&
            query_minimizer_buffer.object != resident_controls.object &&
            query_minimizer_buffer.object != resident_positions.object &&
            resident_descriptors.buffer_handle != resident_controls.buffer_handle &&
            resident_descriptors.buffer_handle != resident_positions.buffer_handle &&
            resident_controls.buffer_handle != resident_positions.buffer_handle &&
            query_minimizer_buffer.buffer_handle != resident_descriptors.buffer_handle &&
            query_minimizer_buffer.buffer_handle != resident_controls.buffer_handle &&
            query_minimizer_buffer.buffer_handle != resident_positions.buffer_handle &&
            resident_descriptors.memory_handle != resident_controls.memory_handle &&
            resident_descriptors.memory_handle != resident_positions.memory_handle &&
            resident_controls.memory_handle != resident_positions.memory_handle &&
            query_minimizer_buffer.memory_handle != resident_descriptors.memory_handle &&
            query_minimizer_buffer.memory_handle != resident_controls.memory_handle &&
            query_minimizer_buffer.memory_handle != resident_positions.memory_handle &&
            device_uuid != std::array<std::uint8_t, VK_UUID_SIZE>{} && descriptor_bytes != 0u &&
            descriptor_bytes <= resident_descriptors.bytes && control_bytes != 0u &&
            control_bytes <= resident_controls.bytes && position_bytes != 0u &&
            position_bytes <= resident_positions.bytes && position_count != 0u &&
            max_occ_cap != (std::numeric_limits<std::uint64_t>::max)() && partition_count != 0u &&
            descriptor_bytes == static_cast<std::uint64_t>(partition_count) *
                                    sizeof(gpu_grouped_soa_resident_partition_descriptor) &&
            abi_revision == k_gpu_grouped_soa_abi_revision && generation_id != 0u &&
            source_identity != 0u && cold_load_identity != 0u && target_len != 0u &&
            query_record.flags == 0u && configuration.is_valid() &&
            static_cast<std::uint64_t>(configuration.max_occ) <= max_occ_cap &&
            query_tile_count != 0u && query_count_dispatches == 1u && query_scan_dispatches == 3u &&
            query_emit_dispatches == 1u && query_completed_roundtrips == 2u && status_or == 0u &&
            coordinate_vote_dispatches == 0u && cpu_or_simd_biological_calls == 0u &&
            recompute_calls == 0u && fallback_calls == 0u;
        if (!identity_and_query)
            return false;
        if (query_record.minimizer_count == 0u) {
            // The canonical persisted-GPU contract requires a genuine
            // per-job partition-selection/seed-lookup dispatch. A query that
            // produced no minimizers cannot supply those operation statistics,
            // so it remains explicitly unsupported rather than being
            // relabelled from batch-level sketch work.
            return false;
        }
        const bool direct_front_end = direct_count_dispatches == 1u &&
                                      direct_scan_dispatches == 3u &&
                                      count_submit_wait_identity != 0u && count_status_checks == 1u;
        if (!direct_front_end)
            return false;
        if (anchor_count == 0u) {
            return direct_emit_dispatches == 0u && sort_operations == 0u &&
                   range_build_dispatches == 0u && range_count == 0u && chain_dispatches == 0u &&
                   chain_backtrack_dispatches == 0u && coordinate_dispatches == 0u &&
                   emit_submit_wait_identity == 0u && sort_range_submit_wait_identity == 0u &&
                   chain_coordinate_submit_wait_identity == 0u && emit_status_checks == 0u &&
                   result_readbacks == 0u &&
                   are_coordinate_result_wires_equal(result, gpu_coordinate_result_wire{});
        }
        const bool emitted_and_ranged =
            direct_emit_dispatches == 1u && sort_operations == 1u && range_build_dispatches == 1u &&
            emit_submit_wait_identity != 0u && sort_range_submit_wait_identity != 0u &&
            count_submit_wait_identity != emit_submit_wait_identity &&
            count_submit_wait_identity != sort_range_submit_wait_identity &&
            emit_submit_wait_identity != sort_range_submit_wait_identity &&
            emit_status_checks == 1u;
        if (!emitted_and_ranged)
            return false;
        if (range_count == 0u) {
            return chain_dispatches == 0u && chain_backtrack_dispatches == 0u &&
                   coordinate_dispatches == 0u && chain_coordinate_submit_wait_identity == 0u &&
                   result_readbacks == 0u &&
                   are_coordinate_result_wires_equal(result, gpu_coordinate_result_wire{});
        }
        return chain_dispatches == 1u && chain_backtrack_dispatches == 1u &&
               coordinate_dispatches == 1u && chain_coordinate_submit_wait_identity != 0u &&
               chain_coordinate_submit_wait_identity != sort_range_submit_wait_identity &&
               chain_coordinate_submit_wait_identity != emit_submit_wait_identity &&
               chain_coordinate_submit_wait_identity != count_submit_wait_identity &&
               result_readbacks == 1u &&
               is_coordinate_result_wire_well_formed(result, query_record.query_len, target_len);
    }
};

/// @brief Unforgeable consumed view of one accepted audit record.
class persisted_grouped_soa_coordinate_job_receipt {
  public:
    persisted_grouped_soa_coordinate_job_receipt() = default;
    persisted_grouped_soa_coordinate_job_receipt(
        const persisted_grouped_soa_coordinate_job_receipt&) = delete;
    persisted_grouped_soa_coordinate_job_receipt&
    operator=(const persisted_grouped_soa_coordinate_job_receipt&) = delete;
    persisted_grouped_soa_coordinate_job_receipt(
        persisted_grouped_soa_coordinate_job_receipt&& other) noexcept {
        move_from(other);
    }
    persisted_grouped_soa_coordinate_job_receipt&
    operator=(persisted_grouped_soa_coordinate_job_receipt&& other) noexcept {
        if (this != &other) {
            audit_ = {};
            pipeline_operation_identity_ = 0u;
            issued_ = false;
            move_from(other);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && pipeline_operation_identity_ != 0u && audit_.is_internally_consistent();
    }
    [[nodiscard]] const persisted_grouped_soa_coordinate_job_audit& audit() const noexcept {
        return audit_;
    }

  private:
    friend class persisted_grouped_soa_coordinate_operation_receipt;

    void move_from(persisted_grouped_soa_coordinate_job_receipt& other) noexcept {
        audit_ = other.audit_;
        issued_ = other.issued_;
        pipeline_operation_identity_ = other.pipeline_operation_identity_;
        other.audit_ = {};
        other.issued_ = false;
        other.pipeline_operation_identity_ = 0u;
    }

    persisted_grouped_soa_coordinate_job_audit audit_{};
    std::uint64_t pipeline_operation_identity_ = 0u;
    bool issued_ = false;
};

/// @brief Aggregate acceptance accounting for a grouped-SoA coordinate operation.
struct persisted_grouped_soa_coordinate_operation_acceptance_audit {
    std::uint64_t operation_identity = 0u;
    std::uint64_t expected_job_count = 0u;
    std::uint64_t consumed_count = 0u;
    std::uint64_t accepted_count = 0u;
    std::uint64_t burned_count = 0u;
    std::uint64_t query_pipeline_receipt_count = 0u;
    std::uint64_t shape_pipeline_receipt_count = 0u;
    std::uint64_t committed_submission_count = 0u;
    std::uint64_t seed_receipt_identity = 0u;

    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        return operation_identity != 0u && expected_job_count != 0u &&
               consumed_count == expected_job_count && accepted_count == expected_job_count &&
               burned_count == 0u && query_pipeline_receipt_count == 3u &&
               shape_pipeline_receipt_count >= 7u && shape_pipeline_receipt_count <= 10u &&
               committed_submission_count >= 3u && seed_receipt_identity != 0u;
    }
    [[nodiscard]] constexpr bool
    operator==(const persisted_grouped_soa_coordinate_operation_acceptance_audit&) const noexcept =
        default;
};

/**
 * @brief Allocation-free source for one persisted GPU operation result.
 *
 * `acceptance` supplies operation and receipt counts. The remaining fields fill
 * `persisted_gpu_operation_stats` query and shape identities, query submission
 * ordinals, environment, cache qualification, and cache metrics.
 */
struct persisted_grouped_soa_coordinate_operation_receipt_preview {
    persisted_grouped_soa_coordinate_operation_acceptance_audit acceptance{};
    std::uint64_t query_pipeline_operation_identity = 0u;
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 2> query_submission_ordinals{};
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    runtime_metal_archive_process_qualification cache_qualification{};
    runtime_gpu_cache_metrics_audit maximum_cache_metrics{};

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return acceptance.is_internally_consistent() && query_pipeline_operation_identity != 0u &&
               shape_pipeline_operation_identity == acceptance.operation_identity &&
               query_pipeline_operation_identity != shape_pipeline_operation_identity &&
               query_submission_ordinals[0] != 0u && query_submission_ordinals[1] != 0u &&
               query_submission_ordinals[0] != query_submission_ordinals[1] && api_version != 0u &&
               driver_version != 0u && seed_size != 0u && manifest_checksum != 0u &&
               cache_qualification.is_internally_consistent() &&
               cache_qualification.vulkan_device_uuid == device_uuid &&
               cache_qualification.api_version == api_version &&
               cache_qualification.driver_version == driver_version &&
               cache_qualification.vendor_id == vendor_id &&
               cache_qualification.device_id == device_id &&
               cache_qualification.pipeline_cache_uuid == pipeline_cache_uuid &&
               cache_qualification.seed_sha256 == seed_sha256 &&
               cache_qualification.seed_size == seed_size &&
               cache_qualification.manifest_checksum == manifest_checksum &&
               maximum_cache_metrics.is_internally_consistent() &&
               maximum_cache_metrics.token_identity == acceptance.operation_identity &&
               std::any_of(device_uuid.begin(),
                           device_uuid.end(),
                           [](const std::uint8_t value) { return value != 0u; }) &&
               std::any_of(pipeline_cache_uuid.begin(),
                           pipeline_cache_uuid.end(),
                           [](const std::uint8_t value) { return value != 0u; }) &&
               std::any_of(seed_sha256.begin(), seed_sha256.end(), [](const std::uint8_t value) {
                   return value != 0u;
               });
    }
    [[nodiscard]] constexpr bool operator==(
        const persisted_grouped_soa_coordinate_operation_receipt_preview&) const noexcept = default;
};

/// @brief Exact full-stage job payload plus its structured Shape submission receipts.
struct persisted_grouped_soa_coordinate_job_receipt_preview {
    persisted_grouped_soa_coordinate_job_audit audit{};
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 4> shape_submission_ordinals{};

    [[nodiscard]] bool is_internally_consistent() const noexcept {
        if (!audit.is_internally_consistent() || audit.anchor_count == 0u ||
            audit.range_count == 0u || audit.result.is_valid != 1u ||
            shape_pipeline_operation_identity == 0u) {
            return false;
        }
        for (std::size_t i = 0u; i < shape_submission_ordinals.size(); ++i) {
            if (shape_submission_ordinals[i] == 0u)
                return false;
            for (std::size_t prior = 0u; prior < i; ++prior) {
                if (shape_submission_ordinals[prior] == shape_submission_ordinals[i]) {
                    return false;
                }
            }
        }
        return true;
    }
};

/// @brief Move-only accepted operation and its immutable environment identity.
class persisted_grouped_soa_coordinate_accepted_operation {
  public:
    persisted_grouped_soa_coordinate_accepted_operation() = default;
    persisted_grouped_soa_coordinate_accepted_operation(
        const persisted_grouped_soa_coordinate_accepted_operation&) = delete;
    persisted_grouped_soa_coordinate_accepted_operation&
    operator=(const persisted_grouped_soa_coordinate_accepted_operation&) = delete;
    persisted_grouped_soa_coordinate_accepted_operation(
        persisted_grouped_soa_coordinate_accepted_operation&& other) noexcept
        : audit_(std::exchange(other.audit_, {})),
          environment_(std::exchange(other.environment_, {})),
          cache_metrics_(std::move(other.cache_metrics_)),
          issued_(std::exchange(other.issued_, false)) {}
    persisted_grouped_soa_coordinate_accepted_operation&
    operator=(persisted_grouped_soa_coordinate_accepted_operation&& other) noexcept {
        if (this != &other) {
            audit_ = std::exchange(other.audit_, {});
            environment_ = std::exchange(other.environment_, {});
            cache_metrics_ = std::move(other.cache_metrics_);
            issued_ = std::exchange(other.issued_, false);
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return issued_ && audit_.is_internally_consistent() &&
               environment_.is_internally_consistent() && cache_metrics_.is_valid();
    }
    [[nodiscard]] const persisted_grouped_soa_coordinate_operation_acceptance_audit&
    audit() const noexcept {
        return audit_;
    }
    [[nodiscard]] const runtime_gpu_execution_environment_snapshot&
    environment_binding() const noexcept {
        return environment_;
    }
    [[nodiscard]] const runtime_gpu_cache_metrics_audit& cache_metrics_audit() const noexcept {
        return cache_metrics_.audit();
    }
    [[nodiscard]] runtime_gpu_cache_metrics_delta take_cache_metrics_delta() noexcept {
        return std::move(cache_metrics_);
    }

  private:
    persisted_grouped_soa_coordinate_operation_acceptance_audit audit_{};
    runtime_gpu_execution_environment_snapshot environment_{};
    runtime_gpu_cache_metrics_delta cache_metrics_{};
    bool issued_ = false;
    friend class runtime;
};

static_assert(!std::is_copy_constructible_v<persisted_grouped_soa_coordinate_accepted_operation>);
static_assert(
    std::is_nothrow_move_constructible_v<persisted_grouped_soa_coordinate_accepted_operation>);

/// @brief runtime-sealed, move-only receipt for one grouped-SoA coordinate batch.  Each
/// logical job can be consumed exactly once and only with matching resident,
/// query-record and result identities.
class persisted_grouped_soa_coordinate_operation_receipt {
  public:
    persisted_grouped_soa_coordinate_operation_receipt() = default;
    persisted_grouped_soa_coordinate_operation_receipt(
        const persisted_grouped_soa_coordinate_operation_receipt&) = delete;
    persisted_grouped_soa_coordinate_operation_receipt&
    operator=(const persisted_grouped_soa_coordinate_operation_receipt&) = delete;
    persisted_grouped_soa_coordinate_operation_receipt(
        persisted_grouped_soa_coordinate_operation_receipt&& other) noexcept {
        move_from(other);
    }
    persisted_grouped_soa_coordinate_operation_receipt&
    operator=(persisted_grouped_soa_coordinate_operation_receipt&& other) noexcept {
        if (this != &other) {
            release_records();
            query_pipeline_token_ = {};
            grouped_soa_pipeline_token_ = {};
            move_from(other);
        }
        return *this;
    }

    [[nodiscard]] bool is_sealed() const noexcept {
        return sealed_ && operation_identity_ != 0u;
    }
    [[nodiscard]] std::size_t job_count() const noexcept {
        return is_sealed() ? records_.size() : 0u;
    }
    [[nodiscard]] std::size_t consumed_count() const noexcept {
        return is_sealed() ? accepted_count_ + burned_count_ : 0u;
    }
    [[nodiscard]] std::size_t accepted_count() const noexcept {
        return is_sealed() ? accepted_count_ : 0u;
    }
    [[nodiscard]] std::size_t burned_count() const noexcept {
        return is_sealed() ? burned_count_ : 0u;
    }
    [[nodiscard]] bool is_fully_consumed() const noexcept {
        return is_sealed() && !records_.empty() &&
               std::all_of(records_.begin(), records_.end(), [](const auto& record) {
                   return record.state != record_state::fresh;
               });
    }
    [[nodiscard]] bool are_all_jobs_accepted() const noexcept {
        return is_sealed() && is_fully_consumed() && accepted_count_ == records_.size() &&
               burned_count_ == 0u && has_valid_causal_tokens();
    }
    [[nodiscard]] runtime_gpu_execution_environment_snapshot environment_preview() const noexcept {
        return preview_environment();
    }
    [[nodiscard]] runtime_gpu_cache_metrics_audit cache_metrics_preview() const noexcept {
        return maximum_cache_metrics();
    }
    [[nodiscard]] persisted_grouped_soa_coordinate_operation_receipt_preview
    operation_receipt_preview() const noexcept {
        persisted_grouped_soa_coordinate_operation_receipt_preview result{};
        if (!are_all_jobs_accepted() || !query_pipeline_token_.is_valid_for_query_sketch() ||
            !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return result;
        }
        const runtime_pipeline_operation_audit& query = query_pipeline_token_.audit();
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit();
        if (query.jobs.size() != 1u || query.jobs.front().committed_submissions != 2u ||
            shape.committed_submissions >
                (std::numeric_limits<std::uint64_t>::max)() - query.committed_submissions) {
            return result;
        }
        result.acceptance = persisted_grouped_soa_coordinate_operation_acceptance_audit{
            .operation_identity = operation_identity_,
            .expected_job_count = static_cast<std::uint64_t>(records_.size()),
            .consumed_count = static_cast<std::uint64_t>(consumed_count()),
            .accepted_count = static_cast<std::uint64_t>(accepted_count()),
            .burned_count = static_cast<std::uint64_t>(burned_count()),
            .query_pipeline_receipt_count = static_cast<std::uint64_t>(query.prepared_count),
            .shape_pipeline_receipt_count = static_cast<std::uint64_t>(shape.prepared_count),
            .committed_submission_count = query.committed_submissions + shape.committed_submissions,
            .seed_receipt_identity = query.seed_receipt_identity,
        };
        result.query_pipeline_operation_identity = query.operation_identity;
        result.shape_pipeline_operation_identity = shape.operation_identity;
        result.query_submission_ordinals[0] = query.jobs.front().submission_ordinals[0];
        result.query_submission_ordinals[1] = query.jobs.front().submission_ordinals[1];
        result.device_uuid = query.device_uuid;
        result.api_version = query.api_version;
        result.driver_version = query.driver_version;
        result.vendor_id = query.vendor_id;
        result.device_id = query.device_id;
        result.pipeline_cache_uuid = query.pipeline_cache_uuid;
        result.seed_sha256 = query.seed_sha256;
        result.seed_size = query.seed_size;
        result.manifest_checksum = query.manifest_checksum;
        if (query.cache_qualification != shape.cache_qualification) {
            return {};
        }
        result.cache_qualification = query.cache_qualification;
        result.maximum_cache_metrics = maximum_cache_metrics();
        return result.is_internally_consistent()
                   ? result
                   : persisted_grouped_soa_coordinate_operation_receipt_preview{};
    }
    [[nodiscard]] std::optional<persisted_grouped_soa_coordinate_job_receipt_preview>
    job_evidence_preview(const std::size_t problem_index) const noexcept {
        if (!are_all_jobs_accepted() || problem_index >= records_.size() ||
            !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return std::nullopt;
        }
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit();
        if (problem_index >= shape.jobs.size())
            return std::nullopt;
        const runtime_pipeline_job_use_audit& use = shape.jobs[problem_index];
        if (use.committed_submissions != 4u ||
            use.submission_operation_identity != shape.operation_identity) {
            return std::nullopt;
        }
        persisted_grouped_soa_coordinate_job_receipt_preview result{
            .audit = records_[problem_index].audit,
            .shape_pipeline_operation_identity = use.submission_operation_identity,
            .shape_submission_ordinals = use.submission_ordinals,
        };
        return result.is_internally_consistent()
                   ? std::optional<persisted_grouped_soa_coordinate_job_receipt_preview>{result}
                   : std::nullopt;
    }

    /// @brief Contract-only test hook. It returns no receipt and therefore
    /// grants no runtime authority. It only lets the portable suite exercise
    /// private sealing, move invalidation, tamper burning, and one-shot consume.
    /// @return `true` when every one-shot receipt check succeeds.
    [[nodiscard]] static bool self_test_one_shot_contract(
        const std::array<persisted_grouped_soa_coordinate_job_audit, 2>& audits,
        const std::array<persisted_grouped_soa_coordinate_job_expectation, 2>& expectations) {
        if (audits[0].operation_identity == 0u ||
            audits[0].operation_identity != audits[1].operation_identity ||
            audits[0].problem_index != 0u || audits[1].problem_index != 1u ||
            !audits[0].is_internally_consistent() || !audits[1].is_internally_consistent()) {
            return false;
        }
        persisted_grouped_soa_coordinate_operation_receipt issued{};
        issued.operation_identity_ = audits[0].operation_identity;
        try {
            issued.admit_records(2u);
        } catch (...) {
            return false;
        }
        issued.records_[0].audit = audits[0];
        issued.records_[1].audit = audits[1];
        issued.test_causal_accepted_ = true;
        issued.sealed_ = true;

        persisted_grouped_soa_coordinate_operation_receipt receipt{std::move(issued)};
        if (issued.is_sealed() || issued.job_count() != 0u || issued.records_capacity_permit_ ||
            !receipt.records_capacity_permit_ || !receipt.is_sealed() ||
            receipt.job_count() != 2u) {
            return false;
        }
        auto first = receipt.consume(expectations[0]);
        if (!first.has_value() || !first->is_valid())
            return false;
        if (receipt.is_fully_consumed())
            return false;
        persisted_grouped_soa_coordinate_job_receipt moved_first{std::move(*first)};
        if (first->is_valid() || !moved_first.is_valid())
            return false;
        if (receipt.consume(expectations[0]).has_value())
            return false;

        auto tampered_second = expectations[1];
        tampered_second.source_identity ^= 1u;
        if (receipt.consume(tampered_second).has_value())
            return false;
        if (receipt.consume(expectations[1]).has_value())
            return false;
        if (!receipt.is_fully_consumed() || receipt.accepted_count() != 1u ||
            receipt.burned_count() != 1u || receipt.are_all_jobs_accepted())
            return false;

        persisted_grouped_soa_coordinate_operation_receipt all_valid{};
        all_valid.operation_identity_ = audits[0].operation_identity;
        try {
            all_valid.admit_records(2u);
        } catch (...) {
            return false;
        }
        all_valid.records_[0].audit = audits[0];
        all_valid.records_[1].audit = audits[1];
        all_valid.test_causal_accepted_ = true;
        all_valid.sealed_ = true;
        auto valid_first = all_valid.consume(expectations[0]);
        auto valid_second = all_valid.consume(expectations[1]);
        return valid_first.has_value() && valid_first->is_valid() && valid_second.has_value() &&
               valid_second->is_valid() && !all_valid.consume(expectations[0]).has_value() &&
               !all_valid.consume(expectations[1]).has_value() && all_valid.is_fully_consumed() &&
               all_valid.accepted_count() == 2u && all_valid.burned_count() == 0u &&
               all_valid.records_capacity_permit_ && all_valid.are_all_jobs_accepted();
    }

    [[nodiscard]] std::optional<persisted_grouped_soa_coordinate_job_receipt>
    consume(const persisted_grouped_soa_coordinate_job_expectation& expected) noexcept {
        if (!is_sealed() || expected.problem_index >= records_.size()) {
            return std::nullopt;
        }
        record& record = records_[expected.problem_index];
        if (record.state != record_state::fresh)
            return std::nullopt;
        const auto& audit = record.audit;
        const auto& query = expected.query_record;
        const bool identities_match =
            audit.problem_index == expected.problem_index &&
            audit.operation_identity == operation_identity_ &&
            audit.query_authority_identity == expected.query_authority_identity &&
            audit.resident_descriptors == expected.resident_descriptors &&
            audit.resident_controls == expected.resident_controls &&
            audit.resident_positions == expected.resident_positions &&
            audit.query_minimizer_buffer == expected.query_minimizer_buffer &&
            audit.device_uuid == expected.device_uuid &&
            audit.descriptor_bytes == expected.descriptor_bytes &&
            audit.control_bytes == expected.control_bytes &&
            audit.position_bytes == expected.position_bytes &&
            audit.position_count == expected.position_count &&
            audit.max_occ_cap == expected.max_occ_cap &&
            audit.partition_count == expected.partition_count &&
            audit.abi_revision == expected.abi_revision &&
            audit.generation_id == expected.generation_id &&
            audit.source_identity == expected.source_identity &&
            audit.cold_load_identity == expected.cold_load_identity &&
            audit.target_len == expected.target_len && audit.query_index == expected.query_index &&
            audit.logical_target_row == expected.logical_target_row &&
            audit.logical_query_ordinal == expected.logical_query_ordinal &&
            audit.query_record.minimizer_offset == query.minimizer_offset &&
            audit.query_record.minimizer_count == query.minimizer_count &&
            audit.query_record.query_len == query.query_len &&
            audit.query_record.flags == query.flags &&
            are_coordinate_result_wires_equal(audit.result, expected.result) &&
            audit.configuration == expected.configuration;
        if (!identities_match || !audit.is_internally_consistent() || !has_valid_causal_tokens()) {
            record.state = record_state::burned;
            ++burned_count_;
            return std::nullopt;
        }
        record.state = record_state::accepted;
        ++accepted_count_;
        persisted_grouped_soa_coordinate_job_receipt job_receipt{};
        job_receipt.audit_ = audit;
        job_receipt.pipeline_operation_identity_ = operation_identity_;
        job_receipt.issued_ = true;
        return job_receipt;
    }

  private:
    friend class runtime;

    enum class record_state : std::uint8_t {
        fresh,
        accepted,
        burned,
    };

    struct record {
        persisted_grouped_soa_coordinate_job_audit audit{};
        record_state state = record_state::fresh;
    };

    struct pipeline_receipt_union {
        std::array<shader_variant_key, k_runtime_pipeline_prepared_slots> keys{};
        std::array<const runtime_metal_archive_pipeline_receipt*, k_runtime_pipeline_prepared_slots>
            receipts{};
        std::size_t count = 0u;
        bool valid = false;
    };

    [[nodiscard]] pipeline_receipt_union receipt_union() const noexcept {
        pipeline_receipt_union result{};
        if (!sealed_ || test_causal_accepted_ ||
            !query_pipeline_token_.is_valid_for_query_sketch() ||
            !grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate()) {
            return result;
        }
        const auto merge = [&result](const runtime_pipeline_operation_token& operation) {
            for (std::size_t i = 0u; i < operation.audit_.prepared_count; ++i) {
                std::size_t slot = result.count;
                for (std::size_t existing = 0u; existing < result.count; ++existing) {
                    if (result.keys[existing] == operation.audit_.prepared[i].shader_key) {
                        slot = existing;
                        break;
                    }
                }
                if (slot == result.count) {
                    if (result.count == result.keys.size())
                        return false;
                    result.keys[result.count] = operation.audit_.prepared[i].shader_key;
                    result.receipts[result.count] = operation.receipts_[i];
                    ++result.count;
                } else if (result.receipts[slot] != operation.receipts_[i]) {
                    return false;
                }
            }
            return true;
        };
        if (!merge(query_pipeline_token_) || !merge(grouped_soa_pipeline_token_) ||
            result.count == 0u ||
            result.count != grouped_soa_pipeline_token_.audit_.prepared_count + 1u) {
            return {};
        }
        result.valid = true;
        return result;
    }

    [[nodiscard]] runtime_gpu_execution_environment_snapshot preview_environment() const noexcept {
        const pipeline_receipt_union union_receipts = receipt_union();
        if (!union_receipts.valid)
            return {};
        const runtime_pipeline_operation_audit& query = query_pipeline_token_.audit_;
        const runtime_pipeline_operation_audit& shape = grouped_soa_pipeline_token_.audit_;
        if (query.device_uuid != shape.device_uuid || query.api_version != shape.api_version ||
            query.driver_version != shape.driver_version || query.vendor_id != shape.vendor_id ||
            query.device_id != shape.device_id ||
            query.pipeline_cache_uuid != shape.pipeline_cache_uuid ||
            query.seed_sha256 != shape.seed_sha256 || query.seed_size != shape.seed_size ||
            query.manifest_checksum != shape.manifest_checksum ||
            query.seed_receipt_identity != shape.seed_receipt_identity ||
            query.archive_set != shape.archive_set ||
            query.cache_qualification != shape.cache_qualification) {
            return {};
        }
        vulkan_feature_set identity{};
        identity.api_version = query.api_version;
        identity.driver_version = query.driver_version;
        identity.vendor_id = query.vendor_id;
        identity.device_id = query.device_id;
        identity.pipeline_cache_uuid = query.pipeline_cache_uuid;
        runtime_gpu_execution_environment_snapshot result{
            .gpu_environment_present = true,
            .gpu_device_uuid = query.device_uuid,
            .gpu_driver_identity = canonical_vulkan_driver_identity(identity),
            .cache_policy = runtime_pipeline_cache_policy::immutable_preseed_all_hit,
            .seed_sha256 = query.seed_sha256,
            .seed_size = query.seed_size,
            .manifest_checksum = query.manifest_checksum,
            .pipeline_cache_all_hit = true,
        };
        return result.is_internally_consistent() ? result
                                                 : runtime_gpu_execution_environment_snapshot{};
    }

    [[nodiscard]] runtime_gpu_cache_metrics_audit maximum_cache_metrics() const noexcept {
        const pipeline_receipt_union union_receipts = receipt_union();
        if (!union_receipts.valid)
            return {};
        runtime_gpu_cache_metrics_audit result{
            .token_identity = operation_identity_,
            .gpu_pipeline_cache_loads = 1u,
            .gpu_pipeline_cache_seed_bytes = query_pipeline_token_.audit_.seed_size,
        };
        const runtime_metal_archive_pipeline_receipt* first = union_receipts.receipts[0];
        if (first == nullptr || !first->is_valid())
            return {};
        result.gpu_pipeline_cache_load_ns = first->audit().seed_load_ns;
        for (std::size_t i = 0u; i < union_receipts.count; ++i) {
            const runtime_metal_archive_pipeline_receipt* receipt = union_receipts.receipts[i];
            if (receipt == nullptr || !receipt->is_valid() ||
                receipt->audit().seed_load_ns != result.gpu_pipeline_cache_load_ns ||
                result.gpu_pipeline_creates == (std::numeric_limits<std::uint64_t>::max)() ||
                receipt->audit().pipeline_create_ns >
                    (std::numeric_limits<std::uint64_t>::max)() - result.gpu_pipeline_creation_ns) {
                return {};
            }
            ++result.gpu_pipeline_creates;
            result.gpu_pipeline_creation_ns += receipt->audit().pipeline_create_ns;
        }
        return result.is_internally_consistent() ? result : runtime_gpu_cache_metrics_audit{};
    }

    void admit_records(std::size_t record_count) {
        using bioseq::common::saturating_mul;
        if (record_count == 0u || !records_.empty() || records_capacity_permit_ ||
            record_count > records_.max_size()) {
            throw vulkan_runtime_error("persisted grouped-SoA operation record shape is invalid");
        }
        const std::uint64_t logical =
            saturating_mul(static_cast<std::uint64_t>(record_count), sizeof(record));
        const std::uint64_t peak =
            saturating_mul(logical, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (logical == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation record admission saturated");
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            peak, &failure);
        if (!permit) {
            throw vulkan_runtime_error(shader_cache_message(
                "persisted grouped-SoA operation record admission denied: reason=",
                bioseq::parallelism::memory_admission_failure_reason_name(failure.reason),
                " requested=",
                failure.requested_bytes));
        }
        try {
            records_.resize(record_count);
        } catch (const std::bad_alloc&) {
            std::vector<record> empty;
            records_.swap(empty);
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation record allocation failed after admission");
        }
        const std::uint64_t retained =
            saturating_mul(static_cast<std::uint64_t>(records_.capacity()), sizeof(record));
        if (retained == (std::numeric_limits<std::uint64_t>::max)() || retained > permit->bytes()) {
            std::vector<record> empty;
            records_.swap(empty);
            throw vulkan_runtime_error(
                "persisted grouped-SoA operation records exceeded admitted capacity");
        }
        permit->shrink_to(retained);
        records_capacity_permit_ = std::move(*permit);
    }

    void release_records() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            records_, records_capacity_permit_);
    }

    [[nodiscard]] bool has_valid_causal_tokens() const noexcept {
        return test_causal_accepted_ ||
               (query_pipeline_token_.is_valid_for_query_sketch() &&
                grouped_soa_pipeline_token_.is_valid_for_grouped_soa_coordinate());
    }

    void move_from(persisted_grouped_soa_coordinate_operation_receipt& other) noexcept {
        operation_identity_ = other.operation_identity_;
        records_ = std::move(other.records_);
        records_capacity_permit_ = std::move(other.records_capacity_permit_);
        query_pipeline_token_ = std::move(other.query_pipeline_token_);
        grouped_soa_pipeline_token_ = std::move(other.grouped_soa_pipeline_token_);
        accepted_count_ = other.accepted_count_;
        burned_count_ = other.burned_count_;
        test_causal_accepted_ = other.test_causal_accepted_;
        sealed_ = other.sealed_;
        other.operation_identity_ = 0u;
        other.records_.clear();
        other.accepted_count_ = 0u;
        other.burned_count_ = 0u;
        other.test_causal_accepted_ = false;
        other.sealed_ = false;
    }

    std::uint64_t operation_identity_ = 0u;
    // Declared before records_: record capacity is destroyed before release.
    bioseq::parallelism::memory_permit records_capacity_permit_{};
    std::vector<record> records_{};
    runtime_pipeline_operation_token query_pipeline_token_{};
    runtime_pipeline_operation_token grouped_soa_pipeline_token_{};
    std::size_t accepted_count_ = 0u;
    std::size_t burned_count_ = 0u;
    bool test_causal_accepted_ = false;
    bool sealed_ = false;
};

static_assert(!std::is_copy_constructible_v<persisted_grouped_soa_coordinate_operation_receipt>);
static_assert(
    std::is_nothrow_move_constructible_v<persisted_grouped_soa_coordinate_operation_receipt>);
static_assert(!persisted_grouped_soa_coordinate_job_audit{}.is_internally_consistent());

namespace runtime_detail {

/// @brief Fixed final-publication payload prepared before the immutable live gate.
/// Dropping this object without committing its claim plan has no side effects.
struct persisted_grouped_soa_coordinate_finalization_plan {
    runtime_gpu_cache_metrics_claim_plan cache_claim_plan{};
    runtime_gpu_execution_environment_snapshot environment{};
    persisted_grouped_soa_coordinate_operation_acceptance_audit acceptance{};
    bool valid = false;

    [[nodiscard]] bool is_valid() const noexcept {
        return valid && cache_claim_plan.is_valid() && environment.is_internally_consistent() &&
               acceptance.is_internally_consistent();
    }
};

static_assert(!std::is_copy_constructible_v<persisted_grouped_soa_coordinate_finalization_plan>);
static_assert(
    std::is_nothrow_move_constructible_v<persisted_grouped_soa_coordinate_finalization_plan>);

} // namespace runtime_detail

/// @brief Coordinate results and causal receipt produced by a batched GPU run.
struct gpu_seed_coordinate_batch_result {
    std::vector<gpu_coordinate_result_wire> results{};
    gpu_seed_coordinate_batch_stats stats{};
    persisted_grouped_soa_coordinate_operation_receipt operation_receipt{};
    /// @brief Dispatch and identity statistics for the direct grouped-SoA route.
    struct grouped_soa_direct_route_stats {
        bool selected = false;
        bool direct_count_scan_emit_completed = false;
        std::uint64_t problem_count = 0;
        std::uint64_t generation_id = 0;
        std::uint64_t source_identity = 0;
        std::uint64_t cold_load_identity = 0;
        std::uint64_t count_dispatches = 0;
        std::uint64_t emit_dispatches = 0;
        std::uint64_t count_status_checks = 0;
        std::uint64_t emit_status_checks = 0;
        std::uint32_t status_or = 0;
        std::uint64_t cpu_or_simd_fallbacks = 0;
    } grouped_soa_direct{};
};

/// @brief The Vulkan compute runtime. One process-wide instance (`runtime::instance`)
/// owns the device and serializes GPU queue access with `mu_`.
class runtime {
  public:
    /// @brief Initializes the Vulkan device and immutable runtime resources.
    runtime();
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    /// @brief Releases pipelines, synchronization objects, buffers, and the device.
    ~runtime();

    /// @brief Returns the process-wide Vulkan runtime instance.
    /// @return Process-wide runtime owner.
    [[nodiscard]] static runtime& instance();
    /// @brief Returns immutable capabilities probed during initialization.
    /// @return Immutable device feature snapshot.
    [[nodiscard]] const vulkan_feature_set& features() const noexcept {
        return features_;
    }
    /// @brief Reports whether a usable Vulkan device was initialized.
    /// @return `true` when initialization selected a usable device.
    [[nodiscard]] bool is_usable() const noexcept {
        return features_.available;
    }
    /// @brief Returns the initialization diagnostic when `is_usable()` is false.
    /// @return Initialization diagnostic, empty after successful initialization.
    [[nodiscard]] const std::string& initialization_error() const noexcept {
        return init_error_;
    }
    /// @brief Returns the number of committed tiled-chaining dispatches.
    /// @return Committed tiled-chaining dispatch count.
    [[nodiscard]] std::uint64_t chaining_dispatch_count() const noexcept {
        return chaining_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns the number of committed packed-unit generator dispatches.
    /// @return Committed packed-unit generator dispatch count.
    [[nodiscard]] std::uint64_t generate_units_dispatch_count() const noexcept {
        return generate_units_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns bytes reserved for process-lifetime pipeline-cache metadata.
    /// @return Current pipeline-cache metadata reservation in bytes.
    [[nodiscard]] std::uint64_t pipeline_cache_metadata_reserved_bytes() {
        std::lock_guard<std::mutex> guard(mu_);
        return pipeline_cache_metadata_permit_.bytes();
    }
    /// @brief Validates and publishes a completed grouped-SoA coordinate operation.
    /// @param[in] receipt Completed operation receipt to consume.
    /// @param[in] expected_preview Immutable publication identity expected by the caller.
    /// @return The accepted operation authority.
    [[nodiscard]] persisted_grouped_soa_coordinate_accepted_operation
    finalize_persisted_grouped_soa_coordinate_operation(
        persisted_grouped_soa_coordinate_operation_receipt&& receipt,
        const persisted_grouped_soa_coordinate_operation_receipt_preview& expected_preview);
    /// @brief Exercises preclaim failure handling without consuming cache metrics.
    /// @return `true` when every rejected mutation leaves claims unchanged.
    [[nodiscard]] static bool self_test_persisted_grouped_soa_finalizer_preclaim_contract(
        runtime_metal_archive_pipeline_audit base_audit);
    /// @brief Exercises partial cache-delta accounting with one immutable audit.
    /// @return `true` when bounded partial deltas retain one-shot semantics.
    [[nodiscard]] static bool
    self_test_gpu_cache_partial_delta_contract(runtime_metal_archive_pipeline_audit base_audit);

    /// @brief Allocates a persistently mapped host-visible Vulkan buffer.
    /// @param[in] bytes Logical byte size.
    /// @param[in] usage Vulkan usage flags.
    /// @return Move-owned host-visible buffer.
    [[nodiscard]] buffer create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    /// @brief Allocates a device buffer with the requested Vulkan usage flags.
    /// @param[in] bytes Logical byte size.
    /// @param[in] usage Vulkan usage flags.
    /// @return Move-owned device buffer.
    [[nodiscard]] buffer create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    /// @brief Returns the physical allocation requirement for a logical buffer size.
    /// @param[in] requested_bytes Logical byte size.
    /// @param[in] usage Vulkan usage flags.
    /// @return Physical device-memory requirement in bytes.
    [[nodiscard]] std::uint64_t buffer_allocation_requirement(std::uint64_t requested_bytes,
                                                              VkBufferUsageFlags usage);
    /// @brief Returns the physical allocation requirement for a GPU arena buffer.
    /// @param[in] requested_bytes Logical arena byte size.
    /// @return Physical device-memory requirement in bytes.
    [[nodiscard]] std::uint64_t gpu_arena_allocation_requirement(std::uint64_t requested_bytes);
    /// @brief Computes complete arena and output admission requirements for a sketch batch.
    /// @return Validated arena, output, and peak allocation requirements.
    [[nodiscard]] gpu_sketch_batch_allocation_requirements
    sketch_batch_allocation_requirements(std::uint64_t packed_sequence_bytes,
                                         std::uint64_t tile_count,
                                         std::uint32_t minimizer_word_kind,
                                         std::uint32_t minimizer_words32,
                                         std::uint64_t max_minimizers);
    /// @brief Releases a runtime-owned Vulkan buffer and clears its identity.
    /// @param[in,out] allocation Buffer owner to release and invalidate.
    void destroy_buffer(buffer& allocation) noexcept;

    /// @brief Runs one complete Vulkan chaining submission. This records all H2D copies,
    /// the compute dispatch, all D2H copies, and all barriers into one command
    /// buffer lifecycle under one queue lock. Do not call separate public
    /// upload/download helpers from the hot chaining path.
    void run_chaining_tiled_dp_roundtrip(const chaining_pipeline_key& key,
                                         buffer& transfer_scratch,
                                         buffer& anchors,
                                         buffer& ranges,
                                         buffer& best_scores,
                                         buffer& predecessors,
                                         std::span<const std::byte> anchors_bytes,
                                         std::span<const std::byte> ranges_bytes,
                                         std::span<std::byte> best_scores_bytes,
                                         std::span<std::byte> predecessors_bytes,
                                         std::uint32_t anchor_count,
                                         std::uint32_t range_count,
                                         std::int32_t max_dist_x,
                                         std::int32_t max_dist_y,
                                         std::int32_t bandwidth,
                                         std::int32_t max_iter,
                                         float gap_penalty,
                                         float skip_penalty,
                                         std::int32_t max_chain_skip = 0);

    /// @brief Runs tiled chaining and returns the selected coordinate trace.
    /// @return Coordinate chain result and predecessor trace.
    [[nodiscard]] gpu_coordinate_chain_trace_result
    run_chaining_coordinate_trace_roundtrip(const chaining_pipeline_key& key,
                                            buffer& transfer_scratch,
                                            buffer& anchors,
                                            buffer& ranges,
                                            buffer& best_scores,
                                            buffer& predecessors,
                                            std::span<const std::byte> anchors_bytes,
                                            std::span<const std::byte> ranges_bytes,
                                            std::span<const std::byte> best_scores_bytes,
                                            std::span<const std::byte> predecessors_bytes,
                                            std::uint32_t anchor_count,
                                            std::uint32_t range_count,
                                            std::uint64_t query_len,
                                            std::int32_t max_dist_x,
                                            std::int32_t max_dist_y,
                                            std::int32_t bandwidth,
                                            std::int32_t max_iter,
                                            float gap_penalty,
                                            float skip_penalty,
                                            std::int32_t max_chain_skip,
                                            std::int32_t min_chain_score,
                                            std::int32_t min_seed_count,
                                            std::uint32_t probe_limit);

    /// @brief Generates packed sequence units entirely on the Vulkan device.
    /// @return Move-owned buffer containing generated packed units.
    buffer run_generate_units_roundtrip(std::uint32_t word_bits,
                                        std::span<const std::uint32_t> cdf,
                                        generate_units_push push,
                                        std::uint64_t out_block_bytes);
    /// @brief Downloads one validated byte slice from a host-visible runtime buffer.
    /// @param[in] src Source allocation.
    /// @param[in] src_byte_offset First source byte.
    /// @param[out] dst Destination span receiving the slice.
    void download_slice(const buffer& src, std::uint64_t src_byte_offset, std::span<std::byte> dst);

    /// @brief Looks up seed hits and returns anchors in canonical device-sorted order.
    /// @return Seed lookup result with canonical anchor ordering.
    seed_lookup_result run_seed_lookup_roundtrip(runtime_seed_lookup_inputs inputs,
                                                 std::span<const std::uint32_t> prefix_counts,
                                                 std::uint64_t query_len,
                                                 std::uint32_t kmer_size,
                                                 std::uint64_t exact_anchor_count,
                                                 const seed_anchor_sort_plan& sort_plan,
                                                 std::uint64_t expected_vulkan_allocation_bytes);

    /// @brief Returns the per-runtime reusable GPU arena.
    /// @return Runtime-owned reusable arena.
    [[nodiscard]] gpu_arena& reusable_gpu_arena();

    /// @brief Runs a parallel ordered sketch for one packed sequence.
    /// @return Resident sketch words and exact count.
    runtime_sketch_result run_sketch_roundtrip(std::span<const std::byte> packed_sequence,
                                               const vulkan_rank_lut& rank_lut,
                                               const vulkan_sketch_params& params,
                                               std::uint64_t max_minimizers,
                                               std::span<const std::uint32_t> query_gate_words = {},
                                               std::uint32_t query_gate_mask = 0u);

    /// @brief Runs an ordered multi-query tile sketch with explicit materialization.
    /// @return Batched sketch buffers, layouts, and dispatch statistics.
    runtime_sketch_batch_result
    run_sketch_tile_batch_roundtrip(std::span<const std::byte> packed_sequence,
                                    const vulkan_rank_lut& rank_lut,
                                    const vulkan_sketch_params& params,
                                    std::span<const gpu_sketch_tile_wire> tiles,
                                    std::span<const gpu_sketch_query_layout_wire> query_layouts,
                                    std::uint64_t max_minimizers,
                                    vulkan_sketch_host_materialization materialization);

    /// @brief Resolves the finite target-index pipeline set before transient admission.
    /// @param[in] params Immutable sketch kernel parameters.
    void prepare_target_index_pipelines(const vulkan_sketch_params& params);

    /// @brief Builds a target minimizer index entirely on the Vulkan device.
    /// @return Move-owned GPU target index and build statistics.
    gpu_built_index build_target_index(std::span<const std::byte> packed_sequence,
                                       std::uint64_t padded_sequence_bytes,
                                       std::uint64_t owned_sequence_staging_bytes,
                                       std::function<void()> release_owned_sequence_staging,
                                       const vulkan_rank_lut& rank_lut,
                                       const vulkan_sketch_params& params,
                                       std::uint64_t max_target_minimizers,
                                       std::span<const std::uint32_t> query_gate_words = {},
                                       std::uint32_t query_gate_mask = 0u,
                                       gpu_index_gate_identity query_gate_identity = {},
                                       bioseq::parallelism::memory_permit* peak_permit = nullptr);

    /// @brief Emits per-segment `(kmer, encoded_pos)` pairs and stably sorts both keys.
    /// @return Sorted resident pairs and optional host materialization.
    gpu_sorted_pair_result
    run_sketch_sort_pairs_roundtrip(std::span<const std::byte> packed_sequence,
                                    const vulkan_rank_lut& rank_lut,
                                    const vulkan_sketch_params& params,
                                    std::uint64_t max_target_minimizers);

    /// @brief Orders one contiguous batch of compact sequences on the device.
    /// @return Sequence ordinals in device-computed order.
    [[nodiscard]] std::vector<std::uint32_t>
    run_sequence_sort_roundtrip(std::span<const std::byte> packed_units,
                                std::span<const sequence_descriptor_wire> descs,
                                const sequence_sort_kernel_parameters& params);
    /// @brief Orders a chunked batch of compact sequences on the device.
    /// @return Sequence ordinals in device-computed order.
    [[nodiscard]] std::vector<std::uint32_t>
    run_sequence_sort_roundtrip(std::span<const sequence_sort_upload_chunk> chunks,
                                std::span<const sequence_descriptor_wire> descs,
                                const sequence_sort_kernel_parameters& params);

    /// @brief Runs score/end-point-only banded affine extension for a batch.
    /// @return One extension result per input job.
    [[nodiscard]] std::vector<gpu_alignment_extension_result_wire>
    run_alignment_extension_roundtrip(std::span<const std::uint32_t> query_symbols,
                                      std::span<const std::uint32_t> target_symbols,
                                      std::span<const gpu_alignment_extension_job_wire> jobs,
                                      const gpu_alignment_extension_params& params);

    /// @brief Rank-buffer SneakySnake lower-bound batch. Valid unequal-length and
    /// unsupported jobs are returned conservatively with an explicit status.
    /// representable malformed ranges fail open in the shader before loads.
    /// @return One lower-bound result per input job.
    [[nodiscard]] std::vector<bioseq::prefilter::prefilter_result>
    run_sneaky_snake_rank_batch_roundtrip(std::span<const std::uint8_t> query_ranks,
                                          std::span<const std::uint8_t> target_ranks,
                                          std::span<const bioseq::prefilter::prefilter_job> jobs);

    /// @brief Rank-buffer GateKeeper batch. The device treats a mask candidate as a
    /// rejection only after the exact bounded-distance guard confirms it.
    /// representable malformed ranges fail open in the shader before loads.
    /// @return One guarded acceptance result per input job.
    [[nodiscard]] std::vector<bioseq::prefilter::prefilter_result>
    run_gatekeeper_rank_batch_roundtrip(std::span<const std::uint8_t> query_ranks,
                                        std::span<const std::uint8_t> target_ranks,
                                        std::span<const bioseq::prefilter::prefilter_job> jobs);

    /// @brief Runs CIGAR-capable exact-match extension without a CPU fallback.
    /// @return Per-job alignment results and packed CIGAR words.
    [[nodiscard]] alignment_cigar_batch_result
    run_alignment_simple_cigar_roundtrip(std::span<const std::uint32_t> query_symbols,
                                         std::span<const std::uint32_t> target_symbols,
                                         std::span<const gpu_alignment_cigar_job_wire> jobs,
                                         const gpu_alignment_cigar_params& params,
                                         std::uint32_t packed_cigar_capacity);

    /// @brief Returns committed sequence-sort dispatches.
    /// @return Committed sequence-sort dispatch count.
    [[nodiscard]] std::uint64_t seq_sort_dispatch_count() const noexcept {
        return seq_sort_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns global full-compare sequence-sort dispatches.
    /// @return Global full-compare dispatch count.
    [[nodiscard]] std::uint64_t seq_sort_global_compare_dispatch_count() const noexcept {
        return seq_sort_global_compare_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns dense-scatter sequence-sort dispatches.
    /// @return Dense-scatter dispatch count.
    [[nodiscard]] std::uint64_t seq_sort_dense_scatter_dispatch_count() const noexcept {
        return seq_sort_dense_scatter_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns bytes uploaded by sequence-sort operations.
    /// @return Uploaded sequence-sort payload bytes.
    [[nodiscard]] std::uint64_t seq_sort_uploaded_bytes_count() const noexcept {
        return seq_sort_uploaded_bytes_.load(std::memory_order_relaxed);
    }
    /// @brief Returns committed alignment-extension dispatches.
    /// @return Committed alignment-extension dispatch count.
    [[nodiscard]] std::uint64_t alignment_extension_dispatch_count() const noexcept {
        return alignment_extension_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns committed SneakySnake prefilter dispatches.
    /// @return Committed SneakySnake dispatch count.
    [[nodiscard]] std::uint64_t sneaky_snake_dispatch_count() const noexcept {
        return sneaky_snake_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns committed GateKeeper prefilter dispatches.
    /// @return Committed GateKeeper dispatch count.
    [[nodiscard]] std::uint64_t gatekeeper_dispatch_count() const noexcept {
        return gatekeeper_dispatch_count_.load(std::memory_order_relaxed);
    }

    /// @brief Counts, emits, and device-sorts seed anchors without a host prepass.
    /// @return Seed lookup result with device-sorted anchors.
    seed_lookup_result run_seed_anchor_roundtrip(const gpu_seed_inputs& inputs,
                                                 std::uint64_t query_len,
                                                 std::uint32_t kmer_size,
                                                 std::int32_t max_occ);
    /// @brief Runs seed lookup and tiled chaining for one coordinate result.
    /// @return Coordinate-only result wire.
    gpu_coordinate_result_wire
    run_seed_to_coordinate_roundtrip(const gpu_seed_inputs& inputs,
                                     std::uint64_t query_len,
                                     std::uint64_t target_len,
                                     std::uint32_t kmer_size,
                                     std::int32_t max_occ,
                                     const chaining_pipeline_key& chain_key,
                                     std::int32_t max_dist_x,
                                     std::int32_t max_dist_y,
                                     std::int32_t bandwidth,
                                     std::int32_t max_iter,
                                     float gap_penalty,
                                     float skip_penalty,
                                     std::int32_t max_chain_skip,
                                     std::int32_t min_chain_score,
                                     std::int32_t min_seed_count,
                                     std::uint32_t probe_limit);

    /// @brief Runs batched seed-to-coordinate jobs and returns their causal receipt.
    /// @return Coordinate results, dispatch statistics, and operation receipt.
    gpu_seed_coordinate_batch_result
    run_seed_to_coordinate_batch_roundtrip(const gpu_seed_query_inputs& query_inputs,
                                           std::span<const gpu_seed_coordinate_job> jobs,
                                           std::uint32_t kmer_size,
                                           std::int32_t max_occ,
                                           gpu_seed_coordinate_algorithm coordinate_algorithm,
                                           const chaining_pipeline_key& chain_key,
                                           std::int32_t max_dist_x,
                                           std::int32_t max_dist_y,
                                           std::int32_t bandwidth,
                                           std::int32_t max_iter,
                                           float gap_penalty,
                                           float skip_penalty,
                                           std::int32_t max_chain_skip,
                                           std::int32_t min_chain_score,
                                           std::int32_t min_seed_count,
                                           std::uint32_t probe_limit,
                                           device_query_sketch_authorization query_authority = {});

    /// @brief Runs batched seed-to-coordinate jobs and returns only result wires.
    /// @return Coordinate result wires in input-job order.
    std::vector<gpu_coordinate_result_wire>
    run_seed_to_coordinate_results_roundtrip(const gpu_seed_query_inputs& query_inputs,
                                             std::span<const gpu_seed_coordinate_job> jobs,
                                             std::uint64_t query_len,
                                             std::uint32_t kmer_size,
                                             std::int32_t max_occ,
                                             gpu_seed_coordinate_algorithm coordinate_algorithm,
                                             const chaining_pipeline_key& chain_key,
                                             std::int32_t max_dist_x,
                                             std::int32_t max_dist_y,
                                             std::int32_t bandwidth,
                                             std::int32_t max_iter,
                                             float gap_penalty,
                                             float skip_penalty,
                                             std::int32_t max_chain_skip,
                                             std::int32_t min_chain_score,
                                             std::int32_t min_seed_count,
                                             std::uint32_t probe_limit);

    /// @brief Largest power-of-two anchor count the GPU bitonic sort can handle in one
    /// workgroup on this device (bounded by workgroup invocations + shared mem).
    /// Larger anchor spans use the scalable stable GPU radix path.
    /// @return Maximum supported in-workgroup anchor count.
    [[nodiscard]] std::uint32_t gpu_anchor_sort_capacity() const noexcept {
        return gpu_anchor_sort_capacity_;
    }

  private:
    [[nodiscard]] static runtime_detail::persisted_grouped_soa_coordinate_finalization_plan
    prepare_persisted_grouped_soa_coordinate_finalization(
        const persisted_grouped_soa_coordinate_operation_receipt& receipt,
        const persisted_grouped_soa_coordinate_operation_receipt_preview& expected_preview,
        const vulkan_feature_set& live_features,
        bool seed_metrics_claimed) noexcept;

    struct pipeline_cache_admission_scope {
        runtime* owner = nullptr;
        explicit pipeline_cache_admission_scope(runtime& runtime) noexcept : owner(&runtime) {}
        pipeline_cache_admission_scope(const pipeline_cache_admission_scope&) = delete;
        pipeline_cache_admission_scope& operator=(const pipeline_cache_admission_scope&) = delete;
        ~pipeline_cache_admission_scope() {
            if (owner != nullptr) {
                owner->reconcile_pipeline_cache_metadata_locked();
            }
        }
    };

    struct owned_pipeline_handles {
        VkDevice device = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;

        owned_pipeline_handles() = default;
        explicit owned_pipeline_handles(VkDevice owner) noexcept : device(owner) {}
        owned_pipeline_handles(const owned_pipeline_handles&) = delete;
        owned_pipeline_handles& operator=(const owned_pipeline_handles&) = delete;
        owned_pipeline_handles(owned_pipeline_handles&& other) noexcept
            : device(std::exchange(other.device, VK_NULL_HANDLE)),
              set_layout(std::exchange(other.set_layout, VK_NULL_HANDLE)),
              pipeline_layout(std::exchange(other.pipeline_layout, VK_NULL_HANDLE)),
              descriptor_pool(std::exchange(other.descriptor_pool, VK_NULL_HANDLE)),
              module(std::exchange(other.module, VK_NULL_HANDLE)),
              pipeline(std::exchange(other.pipeline, VK_NULL_HANDLE)) {}
        owned_pipeline_handles& operator=(owned_pipeline_handles&& other) noexcept {
            if (this != &other) {
                reset();
                device = std::exchange(other.device, VK_NULL_HANDLE);
                set_layout = std::exchange(other.set_layout, VK_NULL_HANDLE);
                pipeline_layout = std::exchange(other.pipeline_layout, VK_NULL_HANDLE);
                descriptor_pool = std::exchange(other.descriptor_pool, VK_NULL_HANDLE);
                module = std::exchange(other.module, VK_NULL_HANDLE);
                pipeline = std::exchange(other.pipeline, VK_NULL_HANDLE);
            }
            return *this;
        }
        ~owned_pipeline_handles() {
            reset();
        }

        void reset() noexcept;
    };

    /// @brief One cached compute pipeline plus its shader module, keyed by tile/local.
    struct pipeline_entry {
        chaining_pipeline_key key{};
        owned_pipeline_handles handles{};
        std::string entry_name = "main";
        std::unique_ptr<const runtime_metal_archive_pipeline_receipt> metal_archive_receipt{};
        std::uint64_t pipeline_create_ns = 0u;

        pipeline_entry() = default;
        pipeline_entry(const pipeline_entry&) = delete;
        pipeline_entry& operator=(const pipeline_entry&) = delete;
        pipeline_entry(pipeline_entry&&) noexcept = default;
        pipeline_entry& operator=(pipeline_entry&&) noexcept = default;
    };

    struct chaining_pipeline {
        VkPipeline pipeline = VK_NULL_HANDLE;
        shader_variant_key shader_key{};
        const runtime_metal_archive_pipeline_receipt* metal_archive_receipt = nullptr;
    };

    /// @brief One cached seed-lookup compute pipeline with its own
    /// descriptor-set layout (variable binding count), pipeline layout (variable
    /// push-constant size), descriptor pool, and a single reusable descriptor
    /// set. Keyed by (kind, local_size, max_span) — the only fields that change
    /// the emitted SPIR-V. Alphabet/word routing is a runtime push constant.
    struct compute_pipeline {
        shader_kind kind{};
        std::uint32_t local_size = 0;
        std::uint32_t max_span = 0;
        std::uint32_t binding_count = 0;
        std::uint32_t push_size = 0;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const runtime_metal_archive_pipeline_receipt* metal_archive_receipt = nullptr;
    };

    struct owned_compute_pipeline {
        shader_kind kind{};
        std::uint32_t local_size = 0;
        std::uint32_t max_span = 0;
        std::uint32_t binding_count = 0;
        std::uint32_t push_size = 0;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        owned_pipeline_handles handles{};
        std::string entry_name = "main";
        std::unique_ptr<const runtime_metal_archive_pipeline_receipt> metal_archive_receipt{};
        std::uint64_t pipeline_create_ns = 0u;

        owned_compute_pipeline() = default;
        owned_compute_pipeline(const owned_compute_pipeline&) = delete;
        owned_compute_pipeline& operator=(const owned_compute_pipeline&) = delete;
        owned_compute_pipeline(owned_compute_pipeline&&) noexcept = default;
        owned_compute_pipeline& operator=(owned_compute_pipeline&&) noexcept = default;

        [[nodiscard]] compute_pipeline view() const noexcept {
            return compute_pipeline{
                .kind = kind,
                .local_size = local_size,
                .max_span = max_span,
                .binding_count = binding_count,
                .push_size = push_size,
                .set_layout = handles.set_layout,
                .pipeline_layout = handles.pipeline_layout,
                .descriptor_pool = handles.descriptor_pool,
                .descriptor_set = descriptor_set,
                .module = handles.module,
                .pipeline = handles.pipeline,
                .metal_archive_receipt = metal_archive_receipt.get(),
            };
        }
    };

    // Returns a POD non-owning view. Canonical move-only owners remain in the
    // cache and transfer Vulkan handles exactly once across reallocation.
    compute_pipeline
    get_compute_pipeline_locked(shader_kind kind, std::uint32_t local_size, std::uint32_t max_span);
    void prepare_target_index_pipelines_locked(const vulkan_sketch_params& params);
    void bind_compute_pipeline_descriptors_locked(const compute_pipeline& entry,
                                                  std::span<const VkBuffer> buffers);
    // GPU frontend — bind storage descriptors as explicit (buffer, offset,
    // range) so arena-backed slices that share a physical buffer bind correctly.
    void bind_descriptors_offsets_locked(const compute_pipeline& entry,
                                         std::span<const VkDescriptorBufferInfo> infos);

    // GPU frontend — fresh-descriptor-set-per-dispatch pool. Lets one command
    // buffer record many dispatches of the same pipeline with different buffer
    // bindings (ping-pong sort passes) without re-updating a single set in
    // flight. Reset once per frontend op.
    void ensure_dispatch_descriptor_pool_locked();
    void reset_dispatch_descriptor_pool_locked();
    [[nodiscard]] VkDescriptorSet
    allocate_dispatch_descriptor_set_locked(VkDescriptorSetLayout layout);
    void record_compute_dispatch_locked(VkCommandBuffer cmd,
                                        const compute_pipeline& pipe,
                                        std::span<const VkDescriptorBufferInfo> infos,
                                        const void* push,
                                        std::uint32_t push_size,
                                        std::uint32_t group_count,
                                        runtime_pipeline_use_collector* collector = nullptr,
                                        std::size_t collector_job_index = 0u);
    // record one full device radix sort (all passes) into an open command
    // buffer. Sorts the key/value arena slices in place via ping-pong scratch.
    void record_radix_sort_locked(VkCommandBuffer cmd,
                                  const gpu_arena_slice& keys,
                                  const gpu_arena_slice& values,
                                  const gpu_arena_slice& keys_scratch,
                                  const gpu_arena_slice& values_scratch,
                                  const gpu_arena_slice& histogram,
                                  const gpu_sort_plan& plan,
                                  std::uint32_t num_workgroups,
                                  std::uint32_t elems_per_wg,
                                  std::uint32_t value_words,
                                  bool& result_in_scratch,
                                  runtime_pipeline_use_collector* collector = nullptr,
                                  std::size_t collector_job_index = 0u);
    // Two-key stable sort: radix by value (encoded_pos) then
    // radix by key (kmer). Because the second sort is stable the result is
    // (kmer, encoded_pos) ordered.
    void record_stable_pair_sort_by_value_then_key_locked(VkCommandBuffer command_buffer,
                                                          sorted_pair_device_views& views,
                                                          gpu_arena_slice hist,
                                                          gpu_sort_plan value_sort_plan,
                                                          gpu_sort_plan key_sort_plan,
                                                          std::uint32_t num_workgroups,
                                                          std::uint32_t elems_per_workgroup);

    void init();
    void shutdown() noexcept;
    void resolve_moltenvk_private_api() noexcept;
    [[nodiscard]] bool should_use_shader_provider_locked() const noexcept;
    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    [[nodiscard]] std::uint64_t buffer_memory_requirement_locked(std::uint64_t bytes,
                                                                 VkBufferUsageFlags usage) const;
    [[nodiscard]] std::uint64_t prefilter_workspace_allocation_bytes_locked() const noexcept;
    void ensure_prefilter_workspace_buffer_locked(buffer& allocation,
                                                  std::uint64_t bytes,
                                                  VkBufferUsageFlags usage);
    void release_prefilter_workspace_locked() noexcept;
    [[nodiscard]] std::vector<bioseq::prefilter::prefilter_result>
    run_rank_prefilter_batch_roundtrip_locked(
        shader_kind kind,
        std::uint32_t push_mode,
        std::size_t replay_index,
        std::span<const std::uint8_t> query_ranks,
        std::span<const std::uint8_t> target_ranks,
        std::span<const bioseq::prefilter::prefilter_job> jobs);
    [[nodiscard]] std::uint64_t pipeline_cache_metadata_bytes_locked() const noexcept;
    void admit_pipeline_cache_growth_locked(std::size_t normal_capacity,
                                            std::size_t compute_pipeline_capacity);
    void reconcile_pipeline_cache_metadata_locked() noexcept;
    chaining_pipeline create_pipeline_locked(const chaining_pipeline_key& key);
    void upload_locked(buffer& transfer_scratch,
                       buffer& dst,
                       std::span<const std::byte> bytes,
                       VkAccessFlags dst_access);
    void download_locked(buffer& transfer_scratch, const buffer& src, std::span<std::byte> bytes);
    [[nodiscard]] std::uint64_t next_operation_identity_locked() {
        constexpr std::uint64_t exhausted = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t identity = operation_token_identity_.load(std::memory_order_relaxed);
        while (identity != exhausted) {
            if (operation_token_identity_.compare_exchange_weak(identity,
                                                                identity + 1u,
                                                                std::memory_order_relaxed,
                                                                std::memory_order_relaxed)) {
                return identity;
            }
        }
        throw vulkan_runtime_error("Vulkan operation receipt identity space exhausted");
    }

    struct prefilter_recorded_command {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        shader_kind kind{};
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        gpu_prefilter_push push{};
        std::uint32_t groups = 0u;
        bool recorded = false;
    };

    struct prefilter_descriptor_binding_cache {
        std::array<VkBuffer, 4> buffers{};
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    };

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    // Two SneakySnake routes, one generic GateKeeper route, and four
    // homogeneous LxE GateKeeper production routes each retain an independent
    // replay identity so interleaved benchmark/production batches do not
    // re-record command buffers when their specialization changes.
    std::array<prefilter_recorded_command, k_rank_prefilter_replay_route_count>
        prefilter_recorded_commands_{};
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    // Declared before both caches so cache-owned strings/receipts/entries are
    // destroyed before their aggregate metadata reservation is returned.
    bioseq::parallelism::memory_permit pipeline_cache_metadata_permit_;
    std::vector<pipeline_entry> pipeline_cache_;
    std::vector<owned_compute_pipeline> compute_pipelines_;
    bool seed_metrics_claimed_ = false;
    std::uint32_t gpu_anchor_sort_capacity_ = 0;
    std::optional<gpu_arena> gpu_arena_; // Lazily created.
    // High-water mapped workspace shared by the synchronous rank-prefilter
    // APIs. Queue access is serialized by mu_, so one runtime-owned set is
    // sufficient for both algorithms and all seven fixed replay routes.
    // Declared before the buffers so reverse destruction order releases every
    // admitted allocation before returning its reservation.
    bioseq::parallelism::memory_permit prefilter_workspace_permit_;
    buffer prefilter_query_buffer_;
    buffer prefilter_target_buffer_;
    buffer prefilter_jobs_buffer_;
    buffer prefilter_results_buffer_;
    // Index 0 owns SneakySnake's descriptor state (both replay modes); index 1
    // owns GateKeeper's. Alternating algorithms therefore does not make the
    // next call pay for an unchanged descriptor-set update.
    std::array<prefilter_descriptor_binding_cache, 2> prefilter_descriptor_binding_cache_{};
    VkDescriptorPool dispatch_descriptor_pool_ = VK_NULL_HANDLE; // Fresh-set pool.
    using set_workgroup_size_mvk_function = void(VKAPI_PTR*)(VkShaderModule module,
                                                             std::uint32_t x,
                                                             std::uint32_t y,
                                                             std::uint32_t z);
    set_workgroup_size_mvk_function set_workgroup_size_mvk_ = nullptr;
    bool metallib_provider_capable_ = false;

    vulkan_feature_set features_{};
    std::string init_error_{};
    std::atomic<std::uint64_t> chaining_dispatch_count_{0};
    std::atomic<std::uint64_t> generate_units_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_global_compare_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_dense_scatter_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_uploaded_bytes_{0}; // Full-payload receipt
    std::atomic<std::uint64_t> alignment_extension_dispatch_count_{0};
    std::atomic<std::uint64_t> sneaky_snake_dispatch_count_{0};
    std::atomic<std::uint64_t> gatekeeper_dispatch_count_{0};
    std::atomic<std::uint64_t> operation_token_identity_{1};
    std::mutex mu_;
};

[[nodiscard]] inline bool hardware_device_available(const runtime& runtime) noexcept {
    return runtime.is_usable() && !runtime.features().software_device;
}

inline void require_hardware_device(const runtime& runtime, const char* operation) {
    if (!runtime.is_usable()) {
        throw vulkan_runtime_error(fmt::format("{} selected but Vulkan runtime is unavailable: {}",
                                               operation,
                                               runtime.initialization_error()));
    }
    if (runtime.features().software_device) {
        throw vulkan_runtime_error(fmt::format(
            "{} selected but active Vulkan device '{}' is a CPU/software device "
            "(VkPhysicalDeviceType={}); explicit GPU execution requires a hardware Vulkan device",
            operation,
            runtime.features().device_name.data(),
            runtime.features().device_type));
    }
}

[[nodiscard]] inline std::uint64_t
seed_anchor_reserved_bytes(std::uint64_t anchor_count, std::uint64_t workgroup_count) noexcept {
    using bioseq::common::saturating_mul;
    const std::uint64_t anchors = saturating_mul(anchor_count, 24u);
    const std::uint64_t keys = saturating_mul(anchor_count, 8u);
    const std::uint64_t anchor_scratch = anchors;
    const std::uint64_t key_scratch = keys;
    const std::uint64_t histogram = saturating_mul(saturating_mul(workgroup_count, 256u), 4u);
    const std::uint64_t host_download = anchors;
    std::uint64_t total = 0;
    total = bioseq::common::saturating_add(total, anchors);
    total = bioseq::common::saturating_add(total, keys);
    total = bioseq::common::saturating_add(total, anchor_scratch);
    total = bioseq::common::saturating_add(total, key_scratch);
    total = bioseq::common::saturating_add(total, histogram);
    total = bioseq::common::saturating_add(total, host_download);
    return total;
}

} // namespace bioseq::gpu::vulkan
