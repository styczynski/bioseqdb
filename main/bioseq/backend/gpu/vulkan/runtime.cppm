/**
 * @file runtime.cppm
 * @brief Vulkan/MoltenVK runtime for the Phase A G3SA tiled-DP chaining kernel.
 *
 * Owns the Vulkan instance / physical device / logical device / compute queue,
 * the fixed chaining descriptor-set + pipeline layout, a per-tile-size compute
 * pipeline cache, and one reusable command buffer + fence. Exposes exactly one
 * hot entry point — `run_chaining_tiled_dp_roundtrip` — which records every
 * H2D copy, the dispatch, and every D2H copy under a single queue lock.
 *
 * Device selection, feature probing, and all per-run failures are runtime-side
 * decisions: init failures set `features_.available == false` with an
 * initialization error, and per-run failures throw `VulkanRuntimeError`. Phase A
 * targets unified memory (Apple Silicon / MoltenVK): device-local buffers come
 * back host-visible+coherent, so the round-trip writes/reads them directly
 * without a staging bounce (DOC0304 §4.1).
 */
module;
#include "common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <dlfcn.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime;

export import bioseq.gpu.vulkan.shader_cache;
export import bioseq.gpu.vulkan.shader_layout;
export import bioseq.gpu.vulkan.shader_variants;
export import bioseq.gpu.vulkan.phase_b_abi;
export import bioseq.gpu.vulkan.phase_c_abi;
import bioseq.common.mlogging;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.instr;  // DOC0310 §21 — phase_c_* sub-region timing labels

export namespace bioseq::gpu::vulkan {

class Runtime;

struct AlignmentCigarBatchResult {
    std::vector<GpuAlignmentCigarResultWire> results{};
    std::vector<std::uint32_t> packed_cigar{};
};

/// Capabilities probed once at init. `available == false` means there is no
/// usable Vulkan device and selected GPU algorithms must fail.
struct VulkanFeatureSet {
    bool available = false;
    bool shader_int64 = false;
    bool scalar_block_layout = false;
    bool timeline_semaphore = false;
    std::uint32_t max_workgroup_invocations = 0;
    std::uint32_t max_workgroup_count_x = 0;
    std::uint32_t max_shared_memory_bytes = 0;
    std::uint64_t device_local_bytes = 0;
    std::array<char, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE> device_name{};
};

/// RAII owner for one (VkBuffer, VkDeviceMemory) pair. Move-only; destruction
/// routes through `Runtime::destroy_buffer` (the single cleanup path).
struct Buffer {
    Buffer() = default;
    Buffer(Runtime* owner, VkBuffer buffer, VkDeviceMemory memory,
           std::uint64_t bytes, void* mapped) noexcept
        : owner(owner), buffer(buffer), memory(memory), bytes(bytes), mapped(mapped) {}
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept { move_from(other); }
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }
    ~Buffer() { reset(); }

    void reset() noexcept;
    [[nodiscard]] bool valid() const noexcept { return buffer != VK_NULL_HANDLE; }

    Runtime* owner = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    void* mapped = nullptr;

private:
    void move_from(Buffer& other) noexcept {
        owner = other.owner;
        buffer = other.buffer;
        memory = other.memory;
        bytes = other.bytes;
        mapped = other.mapped;
        other.owner = nullptr;
        other.buffer = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
        other.bytes = 0;
        other.mapped = nullptr;
    }
};

/// One independent hard chaining range = one dispatched workgroup.
struct GpuChainRange {
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
};

/// Thrown on any per-run Vulkan failure once the device is usable. Selected
/// Vulkan algorithms must propagate this rather than silently using CPU work.
class VulkanRuntimeError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Optional supervisor-owned provider for precompiled MoltenVK metallibs.
/// When installed, runtime pipeline creation asks this hook for a shader module
/// instead of invoking glslang/spirv-cross on the backend worker thread.
struct RuntimeShaderProvider {
    using PrepareFn = std::expected<void, std::string> (*)();
    using LoadFn = std::expected<ShaderModuleLoad, std::string> (*)(
        VkDevice device, const ShaderVariantKey& key, PFN_vkCreateShaderModule create_shader_module);
    PrepareFn prepare_runtime = nullptr;
    LoadFn load_module = nullptr;
};

RuntimeShaderProvider*& runtime_shader_provider_slot() noexcept {
    static RuntimeShaderProvider* provider = nullptr;
    return provider;
}

void install_runtime_shader_provider(RuntimeShaderProvider* provider) noexcept {
    runtime_shader_provider_slot() = provider;
}

/// The 36-byte push-constant block consumed by the tiled-DP shader. Layout is
/// pinned to the GLSL `Params` block.
struct ChainingPushConstants {
    std::uint32_t anchor_count;
    std::uint32_t range_count;
    std::int32_t max_dist_x;
    std::int32_t max_dist_y;
    std::int32_t bandwidth;
    std::int32_t max_iter;
    float gap_penalty;
    float skip_penalty;
    std::int32_t max_chain_skip;  // DOC0311: nearest-first early-exit (minimap2 max_skip); <=0 = disabled
};
static_assert(sizeof(ChainingPushConstants) == 36);
static_assert(offsetof(ChainingPushConstants, anchor_count) == 0);
static_assert(offsetof(ChainingPushConstants, range_count) == 4);
static_assert(offsetof(ChainingPushConstants, max_dist_x) == 8);
static_assert(offsetof(ChainingPushConstants, max_dist_y) == 12);
static_assert(offsetof(ChainingPushConstants, bandwidth) == 16);
static_assert(offsetof(ChainingPushConstants, max_iter) == 20);
static_assert(offsetof(ChainingPushConstants, gap_penalty) == 24);
static_assert(offsetof(ChainingPushConstants, skip_penalty) == 28);
static_assert(offsetof(ChainingPushConstants, max_chain_skip) == 32);

/// Push-constant block for the DOC0335 packed sequence generator kernel.
struct GenerateUnitsPush {
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
static_assert(sizeof(GenerateUnitsPush) == 48);
static_assert(std::is_standard_layout_v<GenerateUnitsPush>);

/// DOC0307 §4.5 — result of the two-dispatch minimizer sketch roundtrip. The
/// resident `minimizer_words` device buffer is moved into the caller's
/// `VulkanDeviceMinimizerStorage` for seed-lookup reuse; `host_minimizer_bytes`
/// is the packed `count` minimizers downloaded for the arena slab.
struct RuntimeSketchResult {
    Buffer minimizer_words;
    std::vector<std::byte> host_minimizer_bytes;
    std::uint64_t count = 0;
    bool overflow = false;
};

struct RuntimeSketchBatchResult {
    Buffer minimizer_words;
    std::vector<std::byte> host_minimizer_bytes;
    std::vector<std::uint32_t> tile_counts;
    std::vector<std::uint32_t> tile_offsets;
    std::uint64_t count = 0;
    bool overflow = false;
};

/// DOC0307 §4.5 — inputs to the seed-hit lookup roundtrip. The query minimizers
/// arrive either as a resident device buffer (`device_minimizer_words`,
/// all-Vulkan sketch+seed path) or as host bytes to upload once. `slots` and
/// `positions` are the GPU-resident index buffers owned by
/// `GpuMinimizerIndexCache`; the runtime never uploads the index itself.
struct RuntimeSeedLookupInputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const Buffer* device_minimizer_words = nullptr;
    const Buffer* slots = nullptr;
    const Buffer* positions = nullptr;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
};

// ===========================================================================
// DOC0310 Phase C — runtime-owned arena (§7)
// ===========================================================================

/// A view into one arena buffer: `(buffer, offset, size)`. Descriptor writes
/// bind exactly this sub-range, so several logical resources can share one
/// physical arena buffer.
struct GpuArenaSlice {
    Buffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    [[nodiscard]] bool valid() const noexcept { return buffer != nullptr; }
};

/// A snapshot of the three arena cursors. `rewind(mark)` restores all three.
struct GpuArenaMark {
    std::uint64_t device_cursor = 0;
    std::uint64_t upload_cursor = 0;
    std::uint64_t download_cursor = 0;
};

/// Per-runtime arena with three geometrically-grown, persistently-mapped
/// buffers (device-local, host-visible upload, host-visible download). Slices
/// are bump-allocated with alignment; `rewind` frees everything past a mark.
/// Growth is allowed only outside an active count/main pair (DOC0310 §7).
class GpuArena {
public:
    explicit GpuArena(Runtime& runtime) noexcept : runtime_(&runtime) {}

    [[nodiscard]] GpuArenaMark mark() const noexcept {
        return GpuArenaMark{device_cursor_, upload_cursor_, download_cursor_};
    }
    void rewind(GpuArenaMark mark) noexcept {
        device_cursor_ = mark.device_cursor;
        upload_cursor_ = mark.upload_cursor;
        download_cursor_ = mark.download_cursor;
    }

    /// Reserve worst-case capacity for one count/main pair before count submit,
    /// so no buffer is reallocated between the two submits. Returns false if any
    /// grow failed (caller declines Phase C).
    [[nodiscard]] bool ensure_capacity(
        std::uint64_t device_bytes, std::uint64_t upload_bytes,
        std::uint64_t download_bytes);

    [[nodiscard]] GpuArenaSlice allocate_device(
        std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] GpuArenaSlice allocate_upload(std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] GpuArenaSlice allocate_download(std::uint64_t bytes, std::uint64_t alignment);

    void bind_storage_slice(
        VkWriteDescriptorSet& write, VkDescriptorBufferInfo& info,
        std::uint32_t binding, VkDescriptorSet dst_set,
        const GpuArenaSlice& slice) const noexcept;

    [[nodiscard]] Buffer& device_buffer() noexcept { return device_; }
    [[nodiscard]] Buffer& upload_buffer() noexcept { return upload_; }
    [[nodiscard]] Buffer& download_buffer() noexcept { return download_; }

private:
    [[nodiscard]] static std::uint64_t align_up(std::uint64_t v, std::uint64_t a) noexcept {
        return a <= 1u ? v : ((v + a - 1u) / a) * a;
    }

    Runtime* runtime_ = nullptr;
    Buffer device_;
    Buffer upload_;
    Buffer download_;
    std::uint64_t device_cursor_ = 0;
    std::uint64_t upload_cursor_ = 0;
    std::uint64_t download_cursor_ = 0;
    std::uint64_t device_capacity_ = 0;
    std::uint64_t upload_capacity_ = 0;
    std::uint64_t download_capacity_ = 0;
};

/// RAII arena reservation: holds the admission permit, marks the arena on
/// construction, and rewinds on destruction. Spans count submit + main submit.
class GpuArenaReservation {
public:
    GpuArenaReservation(GpuArena& arena, bioseq::parallelism::MemoryPermit permit) noexcept
        : arena_(&arena), permit_(std::move(permit)), mark_(arena.mark()) {}
    GpuArenaReservation(const GpuArenaReservation&) = delete;
    GpuArenaReservation& operator=(const GpuArenaReservation&) = delete;
    GpuArenaReservation(GpuArenaReservation&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)),
          permit_(std::move(other.permit_)), mark_(other.mark_) {}
    GpuArenaReservation& operator=(GpuArenaReservation&& other) noexcept {
        if (this != &other) {
            release();
            arena_ = std::exchange(other.arena_, nullptr);
            permit_ = std::move(other.permit_);
            mark_ = other.mark_;
        }
        return *this;
    }
    ~GpuArenaReservation() { release(); }
    void release() noexcept {
        if (arena_ != nullptr) {
            arena_->rewind(mark_);
            arena_ = nullptr;
        }
    }
    [[nodiscard]] GpuArena& arena() noexcept { return *arena_; }

private:
    GpuArena* arena_ = nullptr;
    bioseq::parallelism::MemoryPermit permit_;
    GpuArenaMark mark_{};
};

// ===========================================================================
// DOC0310 Phase C — two-submit resident frontend IO (§11)
// ===========================================================================

/// Storage-free view of a GPU target index (slots/positions buffer slices plus
/// the slot mask / counts). Lets the runtime accept an index without importing
/// `bioseq::indexing` (which would create a module cycle).
struct PhaseCTargetIndexView {
    GpuArenaSlice slots;
    GpuArenaSlice positions;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t valid = 0;
};

/// Plan describing one device radix sort over parallel (key,value) uint64 arrays.
struct GpuSortPlan {
    std::uint64_t element_count = 0;
    std::uint32_t key_bytes = 8;
    std::uint32_t value_bytes = 8;
    std::uint32_t radix_bits = 8;
    std::uint32_t pass_count = 8;
};

/// Coordinate-only chain result plus the selected predecessor trace. The trace
/// entries are anchor indices in reverse chain order, matching the
/// `phase_c_coordinate_reduce` shader scratch buffer.
struct GpuCoordinateChainTraceResult {
    GpuCoordinateOnlyResultWire result{};
    std::vector<std::uint32_t> trace{};
};

/// DOC0346 — GPU sequence sort wire structs. Byte layout pinned to the GLSL
/// `P` push block and `SeqDesc`/`SeqSortRange` declarations in shader_compiler.
struct SeqSortPush {   // 40 bytes; == GLSL pc (seq_sort kernels)
    std::uint32_t count = 0;              // row_count (key stage) / range_count (tiebreak stage)
    std::uint32_t unit_bits = 0;          // 8/16/32/64
    std::uint32_t bits_per_char = 0;      // generic only
    std::uint32_t letters_per_unit = 0;   // generic only
    std::uint32_t char_mask = 0;          // generic only
    std::uint32_t used_mask_lo = 0;
    std::uint32_t used_mask_hi = 0;
    std::uint32_t key_unit_offset = 0;       // dense scatter stage: selected key unit
    std::uint32_t sort_size = 0;           // global bitonic stage: k
    std::uint32_t sort_stride = 0;         // global bitonic stage: j
};
static_assert(sizeof(SeqSortPush) == 40);

struct SeqDescWire {   // 16 bytes; == GLSL SeqDesc. word_offset in uint32 words.
    std::uint32_t word_offset = 0;
    std::uint32_t unit_count = 0;
    std::uint32_t bp_size = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(SeqDescWire) == 16);

struct SeqSortUploadChunk {
    const void* bytes = nullptr;
    std::uint64_t byte_size = 0;
    std::uint64_t padded_byte_size = 0;
};

/// Resolved by the façade from compact_sequence_range<S>::traits.
struct SeqSortKernelParams {
    std::uint32_t alphabet_kind = 0;   // AlphabetKernelKind: dna4_nt4 -> _dna4 kinds; else _generic
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t char_mask = 0;
    std::uint64_t used_storage_mask = 0;
};

/// Result of a GPU-resident target minimizer index build (DOC0310 §12). Owns the
/// open-addressed slot table and the hash-run-ordered positions buffer. The
/// indexing layer adopts these into a `GpuResidentMinimizerIndex` outside the
/// runtime module (which must not import `bioseq::indexing`).
struct GpuBuiltIndex {
    Buffer slots;
    Buffer positions;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t occupied_slots = 0;
    std::uint32_t position_count = 0;
    bool valid = false;
};

/// DOC0345 Patch B — host-visible result of the GPU sketch+two-key-sort roundtrip.
/// Owns the device buffer holding the final sorted run plus the downloaded
/// host bytes (host/Postgres-format persistence happens on the CPU side).
struct GpuSortedPairRun {
    Buffer keys;
    Buffer values;
    std::vector<std::byte> host_key_bytes;
    std::vector<std::byte> host_value_bytes;
    std::uint64_t target_len = 0;
    std::uint32_t pair_count = 0;
    bool valid = false;
};

/// DOC0345 Patch B — device-side ping-pong views for the stable two-key pair
/// sort. Hoisted above `class Runtime` because the private method
/// `record_stable_pair_sort_by_value_then_key_locked` takes it by reference.
struct SortedPairDeviceViews {
    GpuArenaSlice keys;
    GpuArenaSlice values;
    GpuArenaSlice keys_scratch;
    GpuArenaSlice values_scratch;
    bool keys_in_scratch = false;
};

[[nodiscard]] inline const GpuArenaSlice& choose_sorted_keys(
    const SortedPairDeviceViews& views) noexcept
{
    return views.keys_in_scratch ? views.keys_scratch : views.keys;
}

[[nodiscard]] inline const GpuArenaSlice& choose_sorted_values(
    const SortedPairDeviceViews& views) noexcept
{
    return views.keys_in_scratch ? views.values_scratch : views.values;
}

/// Inputs to the GPU seed count/emit/sort roundtrip (DOC0310 §14). The query
/// minimizer words arrive resident (`device_minimizer_words`) or as host bytes;
/// the index slots/positions are the GPU-resident buffers.
struct PhaseCSeedInputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const Buffer* device_minimizer_words = nullptr;
    const Buffer* slots = nullptr;
    const Buffer* positions = nullptr;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t bucket_count = 0;  // >0 ⇒ slots is a hash-sorted dense bucket array
    std::uint64_t target_position_offset = 0;
};

/// Common query-side inputs for a batched resident seed→chain→coordinate run.
/// The query minimizer words are shared by every job in the batch; each job
/// supplies its own resident target index.
struct PhaseCSeedQueryRecordWire {
    std::uint32_t minimizer_offset = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t query_len = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(PhaseCSeedQueryRecordWire) == 16);
static_assert(alignof(PhaseCSeedQueryRecordWire) == alignof(std::uint32_t));

struct PhaseCSeedQueryInputs {
    std::span<const std::byte> host_minimizer_bytes{};
    const Buffer* device_minimizer_words = nullptr;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::span<const PhaseCSeedQueryRecordWire> host_query_records{};
    const Buffer* device_query_records = nullptr;
};

/// One resident target-index job in a batched seed→coordinate run. `slots` is
/// either the open-addressed slot table (`slot_mask != 0`) or the sorted dense
/// bucket table (`bucket_count != 0`) produced by the GPU target-index builder.
struct PhaseCSeedCoordinateJob {
    const Buffer* slots = nullptr;
    const Buffer* positions = nullptr;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t bucket_count = 0;
    std::uint64_t target_position_offset = 0;
    std::uint32_t query_index = 0;
};

struct PhaseCSeedCoordinateBatchStats {
    std::uint64_t problem_count = 0;
    std::uint64_t wave_count = 0;
    std::uint64_t seed_count_dispatches = 0;
    std::uint64_t seed_emit_dispatches = 0;
    std::uint64_t sort_dispatches = 0;
    std::uint64_t range_build_dispatches = 0;
    std::uint64_t chain_dispatches = 0;
    std::uint64_t coordinate_reduce_dispatches = 0;
};

struct PhaseCSeedCoordinateBatchRun {
    std::vector<GpuCoordinateOnlyResultWire> results{};
    PhaseCSeedCoordinateBatchStats stats{};
};

/// The Phase A chaining runtime. One process-wide instance (`Runtime::instance`)
/// owns the device and serializes GPU queue access with `mu_`.
class Runtime {
public:
    Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

    [[nodiscard]] static Runtime& instance();
    [[nodiscard]] const VulkanFeatureSet& features() const noexcept { return features_; }
    [[nodiscard]] bool usable() const noexcept { return features_.available; }
    [[nodiscard]] const std::string& initialization_error() const noexcept {
        return init_error_;
    }
    [[nodiscard]] std::uint64_t chaining_dispatch_count() const noexcept {
        return chaining_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t generate_units_dispatch_count() const noexcept {
        return generate_units_dispatch_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] Buffer create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    [[nodiscard]] Buffer create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    void destroy_buffer(Buffer& buffer) noexcept;

    // One complete Phase A chaining submission. This records all H2D copies,
    // the compute dispatch, all D2H copies, and all barriers into one command
    // buffer lifecycle under one queue lock. Do not call separate public
    // upload/download helpers from the hot chaining path.
    void run_chaining_tiled_dp_roundtrip(
        const ChainingPipelineKey& key,
        Buffer& transfer_scratch,
        Buffer& anchors,
        Buffer& ranges,
        Buffer& best_scores,
        Buffer& predecessors,
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

    [[nodiscard]] GpuCoordinateChainTraceResult run_chaining_coordinate_trace_roundtrip(
        const ChainingPipelineKey& key,
        Buffer& transfer_scratch,
        Buffer& anchors,
        Buffer& ranges,
        Buffer& best_scores,
        Buffer& predecessors,
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

    // DOC0307 Phase B — seed-hit lookup (lookup + bitonic sort). It records
    // every H2D copy, dispatch, barrier, and D2H copy under one queue lock,
    // mirroring the Phase A roundtrip discipline.
    Buffer run_generate_units_roundtrip(
        std::uint32_t word_bits,
        std::span<const std::uint32_t> cdf,
        GenerateUnitsPush push,
        std::uint64_t out_block_bytes);
    void download_slice(
        const Buffer& src,
        std::uint64_t src_byte_offset,
        std::span<std::byte> dst);

    SeedLookupResult run_seed_lookup_roundtrip(
        RuntimeSeedLookupInputs inputs,
        std::span<const std::uint32_t> prefix_counts,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::uint64_t exact_anchor_count);

    // DOC0310 Phase C — accessor for the per-runtime reusable arena (§7).
    [[nodiscard]] GpuArena& phase_c_arena();

    // DOC0310/DOC0351 Phase C — parallel ordered tile sketch (§9). The emit is
    // tile-parallel for every supported alphabet.
    RuntimeSketchResult run_phase_c_sketch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_minimizers,
        std::span<const std::uint32_t> query_gate_words = {},
        std::uint32_t query_gate_mask = 0u);

    RuntimeSketchBatchResult run_phase_c_sketch_tile_batch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::span<const PhaseCSketchTileWire> tiles,
        std::uint64_t max_minimizers);

    // DOC0310 Phase C — build the target minimizer index entirely on device
    // (§12): parallel sketch → device radix sort → race-free GPU slot table.
    // `max_target_minimizers` bounds the pessimistic device allocation.
    GpuBuiltIndex run_phase_c_build_target_index(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_target_minimizers,
        std::span<const std::uint32_t> query_gate_words = {},
        std::uint32_t query_gate_mask = 0u);

    // DOC0345 Patch B — GPU per-segment sorted-pair seam: tiled `emit_mode=2`
    // emit of (kmer, encoded_pos) + a GPU two-key stable radix sort (by encoded
    // pos then kmer -> (kmer, encoded_pos) order). Segment builders still use
    // the host pair sort seam today, but DOC0348's tiled dna5/generic sketch
    // ownership issue is fixed.
    GpuSortedPairRun run_phase_c_sketch_sort_pairs_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_target_minimizers);

    // DOC0346 — order a batch of in-RAM compact sequences on device. Extracts a
    // 64-bit canonical lexicographic key per row, LSD-radix-sorts (reusing the
    // Phase C radix), then breaks equal-key ties with GPU full canonical compares.
    // Runs longer than LOCAL_SIZE_X trigger a device-side global bitonic compare
    // pass over the padded permutation; no host refinement is permitted here.
    [[nodiscard]] std::vector<std::uint32_t> run_sequence_sort_roundtrip(
        std::span<const std::byte> packed_units,
        std::span<const SeqDescWire> descs,
        const SeqSortKernelParams& params);
    [[nodiscard]] std::vector<std::uint32_t> run_sequence_sort_roundtrip(
        std::span<const SeqSortUploadChunk> chunks,
        std::span<const SeqDescWire> descs,
        const SeqSortKernelParams& params);

    // Score/end-point-only banded affine extension batch. Inputs are uint32
    // alphabet ranks; unsupported geometry throws `VulkanRuntimeError` before
    // dispatch, and shader-side rejection is treated as a runtime error.
    [[nodiscard]] std::vector<GpuAlignmentExtensionResultWire>
    run_alignment_extension_roundtrip(
        std::span<const std::uint32_t> query_symbols,
        std::span<const std::uint32_t> target_symbols,
        std::span<const GpuAlignmentExtensionJobWire> jobs,
        const GpuAlignmentExtensionParams& params);

    // DOC0354 Part 2 first slice — CIGAR-capable GPU extension ABI. This
    // exact-match implementation writes packed CIGAR ops on device and rejects
    // unsupported geometry/mismatches; it does not fall back to CPU extension.
    [[nodiscard]] AlignmentCigarBatchResult
    run_alignment_simple_cigar_roundtrip(
        std::span<const std::uint32_t> query_symbols,
        std::span<const std::uint32_t> target_symbols,
        std::span<const GpuAlignmentCigarJobWire> jobs,
        const GpuAlignmentCigarParams& params,
        std::uint32_t packed_cigar_capacity);

    [[nodiscard]] std::uint64_t seq_sort_dispatch_count() const noexcept {
        return seq_sort_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t seq_sort_global_compare_dispatch_count() const noexcept {
        return seq_sort_global_compare_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t seq_sort_dense_scatter_dispatch_count() const noexcept {
        return seq_sort_dense_scatter_dispatch_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t seq_sort_uploaded_bytes_count() const noexcept {
        return seq_sort_uploaded_bytes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t alignment_extension_dispatch_count() const noexcept {
        return alignment_extension_dispatch_count_.load(std::memory_order_relaxed);
    }

    // DOC0310 Phase C — GPU seed count + emit + device anchor sort (§14, §13).
    // Counts anchors on device (no host prepass), prefix-scans, emits anchors,
    // and stable-radix-sorts them by `Anchor::ref_bits()`. Returns the sorted
    // 24-byte anchor bytes.
    SeedLookupResult run_phase_c_seed_roundtrip(
        const PhaseCSeedInputs& inputs,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ);

    GpuCoordinateOnlyResultWire run_phase_c_seed_to_coordinate_roundtrip(
        const PhaseCSeedInputs& inputs,
        std::uint64_t query_len,
        std::uint64_t target_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        const ChainingPipelineKey& chain_key,
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

    PhaseCSeedCoordinateBatchRun run_phase_c_seed_to_coordinate_multi_query_batch_roundtrip(
        const PhaseCSeedQueryInputs& query_inputs,
        std::span<const PhaseCSeedCoordinateJob> jobs,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        const ChainingPipelineKey& chain_key,
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

    std::vector<GpuCoordinateOnlyResultWire> run_phase_c_seed_to_coordinate_batch_roundtrip(
        const PhaseCSeedQueryInputs& query_inputs,
        std::span<const PhaseCSeedCoordinateJob> jobs,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ,
        const ChainingPipelineKey& chain_key,
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

    /// Largest power-of-two anchor count the GPU bitonic sort can handle in one
    /// workgroup on this device (bounded by workgroup invocations + shared mem).
    /// Larger anchor spans are rejected by selected Vulkan seed lookup.
    [[nodiscard]] std::uint32_t gpu_anchor_sort_capacity() const noexcept {
        return gpu_anchor_sort_capacity_;
    }

private:
    /// One cached compute pipeline plus its shader module, keyed by tile/local.
    struct PipelineEntry {
        ChainingPipelineKey key{};
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::string entry_name = "main";
    };

    /// DOC0307 §4.5 — one cached Phase B compute pipeline with its own
    /// descriptor-set layout (variable binding count), pipeline layout (variable
    /// push-constant size), descriptor pool, and a single reusable descriptor
    /// set. Keyed by (kind, local_size, max_span) — the only fields that change
    /// the emitted SPIR-V; alphabet/word routing is a runtime push constant.
    struct PhaseBPipeline {
        ShaderKind kind{};
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
        std::string entry_name = "main";
    };

    // Returns by value: the cached entry is trivially copyable (handles only)
    // and the canonical entry lives in `phase_b_pipelines_`. Returning a
    // reference would dangle the moment a later `get_*` push_back reallocates
    // the vector (the descriptor set / pipeline are still owned by the vector).
    PhaseBPipeline get_phase_b_pipeline_locked(
        ShaderKind kind, std::uint32_t local_size, std::uint32_t max_span);
    void bind_phase_b_descriptors_locked(
        const PhaseBPipeline& entry,
        std::span<const VkBuffer> buffers);
    // DOC0310 Phase C — bind storage descriptors as explicit (buffer, offset,
    // range) so arena-backed slices that share a physical buffer bind correctly.
    void bind_descriptors_offsets_locked(
        const PhaseBPipeline& entry,
        std::span<const VkDescriptorBufferInfo> infos);

    // DOC0310 Phase C — fresh-descriptor-set-per-dispatch pool. Lets one command
    // buffer record many dispatches of the same pipeline with different buffer
    // bindings (ping-pong sort passes) without re-updating a single set in
    // flight. Reset once per frontend op.
    void ensure_phase_c_pool_locked();
    void reset_phase_c_pool_locked();
    [[nodiscard]] VkDescriptorSet alloc_phase_c_set_locked(VkDescriptorSetLayout layout);
    void record_phase_c_dispatch_locked(
        VkCommandBuffer cmd, const PhaseBPipeline& pipe,
        std::span<const VkDescriptorBufferInfo> infos,
        const void* push, std::uint32_t push_size, std::uint32_t group_count);
    // Record one full device radix sort (all passes) into an open command
    // buffer. Sorts the key/value arena slices in place via ping-pong scratch.
    void record_radix_sort_locked(
        VkCommandBuffer cmd,
        const GpuArenaSlice& keys, const GpuArenaSlice& values,
        const GpuArenaSlice& keys_scratch, const GpuArenaSlice& values_scratch,
        const GpuArenaSlice& histogram,
        const GpuSortPlan& plan, std::uint32_t num_workgroups,
        std::uint32_t elems_per_wg, std::uint32_t value_words,
        bool& result_in_scratch);
    // DOC0345 Patch B — two-key stable sort: radix by value (encoded_pos) then
    // radix by key (kmer). Because the second sort is stable the result is
    // (kmer, encoded_pos) ordered.
    void record_stable_pair_sort_by_value_then_key_locked(
        VkCommandBuffer command_buffer,
        SortedPairDeviceViews& views,
        GpuArenaSlice hist,
        GpuSortPlan value_sort_plan,
        GpuSortPlan key_sort_plan,
        std::uint32_t num_workgroups,
        std::uint32_t elems_per_workgroup);

    void init();
    void shutdown() noexcept;
    void resolve_moltenvk_private_api() noexcept;
    [[nodiscard]] bool use_shader_provider_locked() const noexcept;
    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    VkPipeline create_pipeline_locked(const ChainingPipelineKey& key);
    void upload_locked(Buffer& transfer_scratch, Buffer& dst, std::span<const std::byte> bytes,
                       VkAccessFlags dst_access);
    void download_locked(Buffer& transfer_scratch, const Buffer& src, std::span<std::byte> bytes);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    std::vector<PipelineEntry> pipeline_cache_;
    std::vector<PhaseBPipeline> phase_b_pipelines_;
    std::uint32_t gpu_anchor_sort_capacity_ = 0;
    std::optional<GpuArena> phase_c_arena_;  // DOC0310 §7 — lazily created.
    VkDescriptorPool phase_c_pool_ = VK_NULL_HANDLE;  // DOC0310 — fresh-set pool.
    using PFN_vkSetWorkgroupSizeMVK_Bioseq =
        void (VKAPI_PTR*)(VkShaderModule module, std::uint32_t x, std::uint32_t y, std::uint32_t z);
    PFN_vkSetWorkgroupSizeMVK_Bioseq set_workgroup_size_mvk_ = nullptr;
    bool metallib_provider_capable_ = false;

    VulkanFeatureSet features_{};
    std::string init_error_{};
    std::atomic<std::uint64_t> chaining_dispatch_count_{0};
    std::atomic<std::uint64_t> generate_units_dispatch_count_{0};
    std::atomic<std::uint64_t> seq_sort_dispatch_count_{0};  // DOC0346
    std::atomic<std::uint64_t> seq_sort_global_compare_dispatch_count_{0};  // DOC0360
    std::atomic<std::uint64_t> seq_sort_dense_scatter_dispatch_count_{0};  // DOC0360
    std::atomic<std::uint64_t> seq_sort_uploaded_bytes_{0};  // DOC0360 full-payload proof
    std::atomic<std::uint64_t> alignment_extension_dispatch_count_{0};
    std::mutex mu_;
};

[[nodiscard]] inline std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept {
    constexpr std::uint64_t max_value = (std::numeric_limits<std::uint64_t>::max)();
    if (a > max_value - b) return max_value;
    return a + b;
}

[[nodiscard]] inline std::uint64_t sat_max(std::uint64_t a, std::uint64_t b) noexcept {
    return a > b ? a : b;
}

[[nodiscard]] inline std::uint64_t chaining_reserved_bytes(
    std::uint64_t anchor_count,
    std::uint64_t range_count) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t anchors = saturating_mul(anchor_count, 24);
    const std::uint64_t scores  = saturating_mul(anchor_count, 4);
    const std::uint64_t preds   = saturating_mul(anchor_count, 8);
    const std::uint64_t ranges  = saturating_mul(range_count, 8);
    const std::uint64_t device_bytes =
        sat_add(sat_add(sat_add(anchors, scores), preds), ranges);
    const std::uint64_t host_dp_bytes =
        sat_add(sat_add(scores, preds), ranges);
    const std::uint64_t transfer_scratch = sat_max(
        sat_max(anchors, preds),
        sat_max(scores, ranges));
    return sat_add(sat_add(device_bytes, host_dp_bytes), transfer_scratch);
}

[[nodiscard]] inline std::uint64_t chaining_transfer_scratch_bytes(
    std::uint64_t anchor_count,
    std::uint64_t range_count) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t anchors = saturating_mul(anchor_count, 24);
    const std::uint64_t scores  = saturating_mul(anchor_count, 4);
    const std::uint64_t preds   = saturating_mul(anchor_count, 8);
    const std::uint64_t ranges  = saturating_mul(range_count, 8);
    return sat_max(sat_max(anchors, preds), sat_max(scores, ranges));
}

[[nodiscard]] inline std::uint64_t phase_c_seed_exact_reserved_bytes(
    std::uint64_t anchor_count,
    std::uint64_t workgroup_count) noexcept
{
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t anchors = saturating_mul(anchor_count, 24u);
    const std::uint64_t keys = saturating_mul(anchor_count, 8u);
    const std::uint64_t anchor_scratch = anchors;
    const std::uint64_t key_scratch = keys;
    const std::uint64_t histogram =
        saturating_mul(saturating_mul(workgroup_count, 256u), 4u);
    const std::uint64_t host_download = anchors;
    std::uint64_t total = 0;
    total = sat_add(total, anchors);
    total = sat_add(total, keys);
    total = sat_add(total, anchor_scratch);
    total = sat_add(total, key_scratch);
    total = sat_add(total, histogram);
    total = sat_add(total, host_download);
    return total;
}

}  // namespace bioseq::gpu::vulkan

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::gpu::vulkan {

namespace {

/// Throw `VulkanRuntimeError` on any non-success VkResult. A free function
/// rather than a capture-all lambda so it leaves no per-call closure in the hot
/// path and is trivially inlinable.
inline void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw VulkanRuntimeError(
            fmt::format("{}: VkResult {}", what, static_cast<int>(result)));
    }
}

#define BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(X) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindPipeline) \
    X(vkCmdDispatch) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdPushConstants) \
    X(vkCreateBuffer) \
    X(vkCreateCommandPool) \
    X(vkCreateComputePipelines) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateDevice) \
    X(vkCreateFence) \
    X(vkCreateInstance) \
    X(vkCreatePipelineLayout) \
    X(vkCreateShaderModule) \
    X(vkDestroyBuffer) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDescriptorPool) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyDevice) \
    X(vkDestroyFence) \
    X(vkDestroyInstance) \
    X(vkDestroyPipeline) \
    X(vkDestroyPipelineLayout) \
    X(vkDestroyShaderModule) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumeratePhysicalDevices) \
    X(vkFreeMemory) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceQueue) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkMapMemory) \
    X(vkQueueSubmit) \
    X(vkResetCommandBuffer) \
    X(vkResetDescriptorPool) \
    X(vkResetFences) \
    X(vkUnmapMemory) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences)

#define BIOSEQ_DECLARE_VULKAN_FUNCTION(name) inline PFN_##name name = nullptr;
BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(BIOSEQ_DECLARE_VULKAN_FUNCTION)
#undef BIOSEQ_DECLARE_VULKAN_FUNCTION

constexpr const char* kPortabilityEnumeration = "VK_KHR_portability_enumeration";
constexpr const char* kGetPhysicalDeviceProperties2 = "VK_KHR_get_physical_device_properties2";
constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";
constexpr const char* kLayerSettings = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;

/// True if `name` appears in a list of VkExtensionProperties.
[[nodiscard]] bool has_extension(
    const std::vector<VkExtensionProperties>& available, const char* name) noexcept {
    for (const auto& ext : available) {
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}

[[nodiscard]] std::vector<VkExtensionProperties> instance_extensions() {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data()) != VK_SUCCESS) {
        return {};
    }
    return props;
}

[[nodiscard]] std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data()) != VK_SUCCESS) {
        return {};
    }
    return props;
}

/// The vcpkg Vulkan loader used by the bench/test binaries does not search
/// Homebrew's `/opt/homebrew/etc/vulkan/icd.d` on macOS. If the user has not
/// provided an explicit ICD override, install the Homebrew MoltenVK ICD path
/// before the first loader query so `vkCreateInstance` sees the portability ICD.
void ensure_homebrew_moltenvk_icd_visible() {
#if defined(__APPLE__)
    (void)::setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
    if (std::getenv("VK_ICD_FILENAMES") != nullptr) {
        return;
    }
    constexpr const char* candidates[] = {
        "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json",
    };
    for (const char* path : candidates) {
        if (std::FILE* f = std::fopen(path, "r")) {
            std::fclose(f);
            if (::setenv("VK_ICD_FILENAMES", path, 0) == 0) {
                bioseq_log<DBG>("[vulkan_runtime] using MoltenVK ICD manifest {}", path);
            }
            return;
        }
    }
#endif
}

#if defined(__APPLE__)
struct ObjcRuntimeFns {
    void* msg_send = nullptr;
    void* (*get_class)(const char*) = nullptr;
    void* (*sel_register_name)(const char*) = nullptr;
    void* (*object_get_class)(void*) = nullptr;
    void* (*class_get_superclass)(void*) = nullptr;
    void* (*class_get_instance_method)(void*, void*) = nullptr;
    void* (*method_set_implementation)(void*, void*) = nullptr;
};

struct MoltenVkCommandLibraryInterpose {
    ObjcRuntimeFns objc{};
    std::vector<std::byte> metallib;
    void* original_new_library_with_source = nullptr;
    void* original_new_library_with_source_async = nullptr;
    void* sel_utf8_string = nullptr;
    void* sel_new_library_with_data_error = nullptr;
    bool installed = false;
};

MoltenVkCommandLibraryInterpose& moltenvk_command_interpose_state() noexcept {
    static MoltenVkCommandLibraryInterpose state;
    return state;
}

template <class Fn>
[[nodiscard]] Fn objc_msg_cast(void* fn) noexcept {
    return reinterpret_cast<Fn>(fn);
}

extern "C" void* bioseq_moltenvk_new_library_with_source(
    void* self, void* cmd, void* source, void* options, void** error)
{
    auto& state = moltenvk_command_interpose_state();
    using Utf8Fn = const char* (*)(void*, void*);
    const char* text = nullptr;
    if (source != nullptr && state.sel_utf8_string != nullptr) {
        text = objc_msg_cast<Utf8Fn>(state.objc.msg_send)(source, state.sel_utf8_string);
    }
    const bool is_moltenvk_command_source =
        text != nullptr
        && std::strstr(text, "cmdCopyBufferBytes") != nullptr
        && std::strstr(text, "vtxCmdBlitImage") != nullptr;
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] newLibraryWithSource intercepted: text=%d len=%zu is_cmd_source=%d metallib_empty=%d\n",
                 text != nullptr ? 1 : 0,
                 text != nullptr ? std::strlen(text) : 0u,
                 is_moltenvk_command_source ? 1 : 0,
                 state.metallib.empty() ? 1 : 0);
    if (is_moltenvk_command_source && !state.metallib.empty()) {
        using LibraryDataFn = void* (*)(void*, void*, void*, void**);
        dispatch_data_t mtl_data = dispatch_data_create(
            state.metallib.data(),
            state.metallib.size(),
            nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (mtl_data != nullptr) {
            if (void* library = objc_msg_cast<LibraryDataFn>(state.objc.msg_send)(
                    self,
                    state.sel_new_library_with_data_error,
                    static_cast<void*>(mtl_data),
                    error);
                library != nullptr) {
                dispatch_release(mtl_data);
                std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: returned cached command library (newLibraryWithData ok)\n");
                return library;
            }
            dispatch_release(mtl_data);
            std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: newLibraryWithData FAILED, falling through to source compile\n");
        }
    }

    std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: FALL-THROUGH to real newLibraryWithSource (XPC compile)\n");
    using SourceFn = void* (*)(void*, void*, void*, void*, void**);
    return objc_msg_cast<SourceFn>(state.original_new_library_with_source)(
        self, cmd, source, options, error);
}

struct ObjcBlockLiteral {
    void* isa = nullptr;
    int flags = 0;
    int reserved = 0;
    void* invoke = nullptr;
    void* descriptor = nullptr;
};

void invoke_metal_library_completion(void* completion, void* library, void* error) noexcept {
    if (completion == nullptr) {
        return;
    }
    auto* block = static_cast<ObjcBlockLiteral*>(completion);
    if (block->invoke == nullptr) {
        return;
    }
    using InvokeFn = void (*)(void*, void*, void*);
    objc_msg_cast<InvokeFn>(block->invoke)(completion, library, error);
}

extern "C" void bioseq_moltenvk_new_library_with_source_async(
    void* self, void* cmd, void* source, void* options, void* completion)
{
    auto& state = moltenvk_command_interpose_state();
    using Utf8Fn = const char* (*)(void*, void*);
    const char* text = nullptr;
    if (source != nullptr && state.sel_utf8_string != nullptr) {
        text = objc_msg_cast<Utf8Fn>(state.objc.msg_send)(source, state.sel_utf8_string);
    }
    const bool is_moltenvk_command_source =
        text != nullptr
        && std::strstr(text, "cmdCopyBufferBytes") != nullptr
        && std::strstr(text, "vtxCmdBlitImage") != nullptr;
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] newLibraryWithSource async intercepted: text=%d len=%zu is_cmd_source=%d metallib_empty=%d\n",
                 text != nullptr ? 1 : 0,
                 text != nullptr ? std::strlen(text) : 0u,
                 is_moltenvk_command_source ? 1 : 0,
                 state.metallib.empty() ? 1 : 0);
    if (is_moltenvk_command_source && !state.metallib.empty()) {
        using LibraryDataFn = void* (*)(void*, void*, void*, void**);
        dispatch_data_t mtl_data = dispatch_data_create(
            state.metallib.data(),
            state.metallib.size(),
            nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (mtl_data != nullptr) {
            void* error = nullptr;
            void* library = objc_msg_cast<LibraryDataFn>(state.objc.msg_send)(
                self,
                state.sel_new_library_with_data_error,
                static_cast<void*>(mtl_data),
                &error);
            dispatch_release(mtl_data);
            if (library != nullptr) {
                std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: returned cached command library async (newLibraryWithData ok)\n");
                invoke_metal_library_completion(completion, library, nullptr);
                return;
            }
            std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: async newLibraryWithData FAILED, falling through to source compile\n");
        }
    }

    std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: async FALL-THROUGH to real newLibraryWithSource (XPC compile)\n");
    if (state.original_new_library_with_source_async == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: async original IMP missing; completing with null library\n");
        invoke_metal_library_completion(completion, nullptr, nullptr);
        return;
    }
    using SourceAsyncFn = void (*)(void*, void*, void*, void*, void*);
    objc_msg_cast<SourceAsyncFn>(state.original_new_library_with_source_async)(
        self, cmd, source, options, completion);
}

[[nodiscard]] bool read_file_bytes(
    const char* path, std::vector<std::byte>* out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0) {
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(out->data()), size);
    return static_cast<bool>(input);
}

void install_moltenvk_command_library_interpose() noexcept {
    auto& state = moltenvk_command_interpose_state();
    if (state.installed) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: already installed\n");
        return;
    }
    const char* path = std::getenv("BIOSEQ_MOLTENVK_COMMAND_METALLIB");
    std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: BIOSEQ_MOLTENVK_COMMAND_METALLIB=%s\n",
                 path != nullptr ? path : "(null)");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::vector<std::byte> metallib;
    if (!read_file_bytes(path, &metallib)) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: read_file_bytes FAILED for %s\n", path);
        bioseq_log<DBG>("[vulkan_runtime] MoltenVK command metallib unavailable at {}", path);
        return;
    }
    std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: read command metallib ok bytes=%zu\n",
                 metallib.size());

    void* objc_handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
    void* metal_handle =
        ::dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (objc_handle == nullptr || metal_handle == nullptr) {
        return;
    }

    ObjcRuntimeFns objc{};
    objc.msg_send = ::dlsym(objc_handle, "objc_msgSend");
    objc.get_class = reinterpret_cast<decltype(objc.get_class)>(
        ::dlsym(objc_handle, "objc_getClass"));
    objc.sel_register_name = reinterpret_cast<decltype(objc.sel_register_name)>(
        ::dlsym(objc_handle, "sel_registerName"));
    objc.object_get_class = reinterpret_cast<decltype(objc.object_get_class)>(
        ::dlsym(objc_handle, "object_getClass"));
    objc.class_get_superclass = reinterpret_cast<decltype(objc.class_get_superclass)>(
        ::dlsym(objc_handle, "class_getSuperclass"));
    objc.class_get_instance_method = reinterpret_cast<decltype(objc.class_get_instance_method)>(
        ::dlsym(objc_handle, "class_getInstanceMethod"));
    objc.method_set_implementation = reinterpret_cast<decltype(objc.method_set_implementation)>(
        ::dlsym(objc_handle, "method_setImplementation"));
    if (objc.msg_send == nullptr || objc.get_class == nullptr
        || objc.sel_register_name == nullptr || objc.object_get_class == nullptr
        || objc.class_get_superclass == nullptr || objc.class_get_instance_method == nullptr
        || objc.method_set_implementation == nullptr) {
        return;
    }

    using CreateDeviceFn = void* (*)();
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        ::dlsym(metal_handle, "MTLCreateSystemDefaultDevice"));
    if (create_device == nullptr) {
        return;
    }
    void* device = create_device();
    if (device == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: MTLCreateSystemDefaultDevice returned NULL\n");
        return;
    }

    void* sel_source = objc.sel_register_name("newLibraryWithSource:options:error:");
    void* sel_source_async =
        objc.sel_register_name("newLibraryWithSource:options:completionHandler:");
    state.sel_utf8_string = objc.sel_register_name("UTF8String");
    state.sel_new_library_with_data_error = objc.sel_register_name("newLibraryWithData:error:");
    void* cls = objc.object_get_class(device);
    void* method = nullptr;
    void* method_async = nullptr;
    while (cls != nullptr && (method == nullptr || method_async == nullptr)) {
        if (method == nullptr) {
            method = objc.class_get_instance_method(cls, sel_source);
        }
        if (method_async == nullptr) {
            method_async = objc.class_get_instance_method(cls, sel_source_async);
        }
        cls = objc.class_get_superclass(cls);
    }
    if (method == nullptr && method_async == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: newLibraryWithSource methods NOT FOUND on device class\n");
        return;
    }

    state.objc = objc;
    state.metallib = std::move(metallib);
    if (method != nullptr) {
        state.original_new_library_with_source =
            objc.method_set_implementation(
                method,
                reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source));
    }
    if (method_async != nullptr) {
        state.original_new_library_with_source_async =
            objc.method_set_implementation(
                method_async,
                reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source_async));
    }
    state.installed = state.original_new_library_with_source != nullptr
        || state.original_new_library_with_source_async != nullptr;
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] cmd-interpose: swizzle installed=%d sync_original=%p async_original=%p\n",
                 state.installed ? 1 : 0,
                 state.original_new_library_with_source,
                 state.original_new_library_with_source_async);
    if (state.installed) {
        bioseq_log<DBG>(
            "[vulkan_runtime] installed MoltenVK command shader metallib interpose {}",
            path);
    }
}

struct MetalPipelineArchiveInterpose {
    ObjcRuntimeFns objc{};
    std::string archive_dir;
    void* device = nullptr;
    void* archive_array = nullptr;
    void* original_new_pipeline_async = nullptr;
    void* original_new_pipeline_sync = nullptr;
    void* sel_binary_archives = nullptr;
    void* sel_set_binary_archives = nullptr;
    void* sel_compute_function = nullptr;
    void* sel_name = nullptr;
    void* sel_utf8_string = nullptr;
    void* sel_count = nullptr;
    void* sel_max_total_threads = nullptr;
    void* sel_threadgroup_multiple = nullptr;
    std::size_t loaded_archive_file_count = 0;
    bool installed = false;
    std::mutex mu;
};

MetalPipelineArchiveInterpose& metal_pipeline_archive_interpose_state() noexcept {
    static MetalPipelineArchiveInterpose state;
    return state;
}

[[nodiscard]] std::string bioseq_ns_string_utf8(
    ObjcRuntimeFns& objc,
    void* ns_string,
    void* sel_utf8_string)
{
    if (ns_string == nullptr || sel_utf8_string == nullptr) {
        return {};
    }
    using Utf8Fn = const char* (*)(void*, void*);
    const char* text = objc_msg_cast<Utf8Fn>(objc.msg_send)(ns_string, sel_utf8_string);
    return text == nullptr ? std::string{} : std::string(text);
}

[[nodiscard]] void* bioseq_make_ns_string(ObjcRuntimeFns& objc, std::string_view value)
{
    void* ns_string_cls = objc.get_class("NSString");
    if (ns_string_cls == nullptr) {
        return nullptr;
    }
    std::string owned(value);
    using StringFn = void* (*)(void*, void*, const char*);
    return objc_msg_cast<StringFn>(objc.msg_send)(
        ns_string_cls,
        objc.sel_register_name("stringWithUTF8String:"),
        owned.c_str());
}

[[nodiscard]] void* bioseq_make_file_url(ObjcRuntimeFns& objc, const std::filesystem::path& path)
{
    void* url_cls = objc.get_class("NSURL");
    void* ns_path = bioseq_make_ns_string(objc, path.string());
    if (url_cls == nullptr || ns_path == nullptr) {
        return nullptr;
    }
    using UrlFn = void* (*)(void*, void*, void*);
    return objc_msg_cast<UrlFn>(objc.msg_send)(
        url_cls,
        objc.sel_register_name("fileURLWithPath:"),
        ns_path);
}

[[nodiscard]] std::string bioseq_ns_error_description(ObjcRuntimeFns& objc, void* error)
{
    if (error == nullptr) {
        return {};
    }
    using GetObjFn = void* (*)(void*, void*);
    void* desc = objc_msg_cast<GetObjFn>(objc.msg_send)(
        error,
        objc.sel_register_name("localizedDescription"));
    return bioseq_ns_string_utf8(objc, desc, objc.sel_register_name("UTF8String"));
}

[[nodiscard]] std::vector<std::filesystem::path> metal_archive_files(std::string_view dir)
{
    std::vector<std::filesystem::path> paths;
    if (dir.empty()) {
        return paths;
    }
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(std::filesystem::path(dir), ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const std::filesystem::path path = entry.path();
        if (path.extension() == ".metalar") {
            paths.push_back(path);
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

[[nodiscard]] void* load_metal_binary_archive(
    MetalPipelineArchiveInterpose& state,
    const std::filesystem::path& path)
{
    void* desc_cls = state.objc.get_class("MTLBinaryArchiveDescriptor");
    if (desc_cls == nullptr || state.device == nullptr) {
        return nullptr;
    }
    using NewFn = void* (*)(void*, void*);
    void* desc = objc_msg_cast<NewFn>(state.objc.msg_send)(
        desc_cls,
        state.objc.sel_register_name("new"));
    void* url = bioseq_make_file_url(state.objc, path);
    if (desc == nullptr || url == nullptr) {
        return nullptr;
    }
    using SetObjFn = void (*)(void*, void*, void*);
    objc_msg_cast<SetObjFn>(state.objc.msg_send)(
        desc,
        state.objc.sel_register_name("setUrl:"),
        url);
    void* error = nullptr;
    using NewArchiveFn = void* (*)(void*, void*, void*, void**);
    void* archive = objc_msg_cast<NewArchiveFn>(state.objc.msg_send)(
        state.device,
        state.objc.sel_register_name("newBinaryArchiveWithDescriptor:error:"),
        desc,
        &error);
    if (archive == nullptr) {
        bioseq_log<DBG>(
            "[vulkan_runtime] failed to load Metal binary archive {}: {}",
            path.string(),
            bioseq_ns_error_description(state.objc, error));
    }
    return archive;
}

void refresh_metal_binary_archives_locked(MetalPipelineArchiveInterpose& state)
{
    const std::vector<std::filesystem::path> paths = metal_archive_files(state.archive_dir);
    if (state.archive_array != nullptr
        && state.loaded_archive_file_count == paths.size()) {
        return;
    }
    void* array_cls = state.objc.get_class("NSMutableArray");
    if (array_cls == nullptr) {
        return;
    }
    using AllocFn = void* (*)(void*, void*);
    using InitCapacityFn = void* (*)(void*, void*, unsigned long);
    void* allocated = objc_msg_cast<AllocFn>(state.objc.msg_send)(
        array_cls,
        state.objc.sel_register_name("alloc"));
    void* array = objc_msg_cast<InitCapacityFn>(state.objc.msg_send)(
        allocated,
        state.objc.sel_register_name("initWithCapacity:"),
        static_cast<unsigned long>(paths.size()));
    if (array == nullptr) {
        return;
    }

    std::size_t loaded = 0;
    using AddObjectFn = void (*)(void*, void*, void*);
    for (const std::filesystem::path& path : paths) {
        if (void* archive = load_metal_binary_archive(state, path); archive != nullptr) {
            objc_msg_cast<AddObjectFn>(state.objc.msg_send)(
                array,
                state.objc.sel_register_name("addObject:"),
                archive);
            ++loaded;
        }
    }
    state.archive_array = array;
    state.loaded_archive_file_count = paths.size();
    std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: loaded %zu/%zu .metalar archives from %s\n",
                 loaded, paths.size(), state.archive_dir.c_str());
    bioseq_log<DBG>(
        "[vulkan_runtime] loaded {} Metal binary archives from {}",
        loaded,
        state.archive_dir);
}

[[nodiscard]] std::string descriptor_function_name(MetalPipelineArchiveInterpose& state, void* desc)
{
    if (desc == nullptr || state.sel_compute_function == nullptr || state.sel_name == nullptr) {
        return {};
    }
    using GetObjFn = void* (*)(void*, void*);
    void* function = objc_msg_cast<GetObjFn>(state.objc.msg_send)(
        desc,
        state.sel_compute_function);
    void* name = function == nullptr
        ? nullptr
        : objc_msg_cast<GetObjFn>(state.objc.msg_send)(function, state.sel_name);
    return bioseq_ns_string_utf8(state.objc, name, state.sel_utf8_string);
}

void attach_metal_binary_archives_to_descriptor(void* desc)
{
    if (desc == nullptr) {
        return;
    }
    auto& state = metal_pipeline_archive_interpose_state();
    if (!state.installed) {
        return;
    }

    std::lock_guard<std::mutex> guard(state.mu);
    refresh_metal_binary_archives_locked(state);
    if (state.archive_array == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: attach skipped (archive_array==null) function=%s\n",
                     descriptor_function_name(state, desc).c_str());
        return;
    }

    using GetObjFn = void* (*)(void*, void*);
    using CountFn = unsigned long (*)(void*, void*);
    void* existing = objc_msg_cast<GetObjFn>(state.objc.msg_send)(
        desc,
        state.sel_binary_archives);
    if (existing != nullptr) {
        const unsigned long existing_count =
            objc_msg_cast<CountFn>(state.objc.msg_send)(existing, state.sel_count);
        if (existing_count > 0) {
            return;
        }
    }

    const unsigned long archive_count =
        objc_msg_cast<CountFn>(state.objc.msg_send)(state.archive_array, state.sel_count);
    if (archive_count == 0) {
        return;
    }

    unsigned long max_threads = 0;
    signed char threadgroup_multiple = 0;
    if (state.sel_max_total_threads != nullptr) {
        using GetULongFn = unsigned long (*)(void*, void*);
        max_threads = objc_msg_cast<GetULongFn>(state.objc.msg_send)(
            desc,
            state.sel_max_total_threads);
    }
    if (state.sel_threadgroup_multiple != nullptr) {
        using GetBoolFn = signed char (*)(void*, void*);
        threadgroup_multiple = objc_msg_cast<GetBoolFn>(state.objc.msg_send)(
            desc,
            state.sel_threadgroup_multiple);
    }

    using SetObjFn = void (*)(void*, void*, void*);
    objc_msg_cast<SetObjFn>(state.objc.msg_send)(
        desc,
        state.sel_set_binary_archives,
        state.archive_array);
    std::fprintf(stderr,
                 "[bioseq-gpu-diag] pso-interpose: ATTACHED %lu archives to descriptor function=%s max_threads=%lu\n",
                 archive_count, descriptor_function_name(state, desc).c_str(), max_threads);
    bioseq_log<DBG>(
        "[vulkan_runtime] attached {} Metal binary archives to compute descriptor function={} max_threads={} full_subgroups={}",
        archive_count,
        descriptor_function_name(state, desc),
        max_threads,
        threadgroup_multiple != 0);
}

extern "C" void bioseq_metal_new_compute_pipeline_state_async(
    void* self,
    void* cmd,
    void* desc,
    unsigned long options,
    void* completion_handler)
{
    std::fprintf(stderr, "[bioseq-gpu-diag] newComputePipelineStateWithDescriptor ASYNC intercepted\n");
    attach_metal_binary_archives_to_descriptor(desc);
    auto& state = metal_pipeline_archive_interpose_state();
    using AsyncFn = void (*)(void*, void*, void*, unsigned long, void*);
    objc_msg_cast<AsyncFn>(state.original_new_pipeline_async)(
        self,
        cmd,
        desc,
        options,
        completion_handler);
}

extern "C" void* bioseq_metal_new_compute_pipeline_state_sync(
    void* self,
    void* cmd,
    void* desc,
    unsigned long options,
    void** reflection,
    void** error)
{
    std::fprintf(stderr, "[bioseq-gpu-diag] newComputePipelineStateWithDescriptor SYNC intercepted\n");
    attach_metal_binary_archives_to_descriptor(desc);
    auto& state = metal_pipeline_archive_interpose_state();
    using SyncFn = void* (*)(void*, void*, void*, unsigned long, void**, void**);
    return objc_msg_cast<SyncFn>(state.original_new_pipeline_sync)(
        self,
        cmd,
        desc,
        options,
        reflection,
        error);
}

void install_metal_pipeline_archive_interpose() noexcept {
    auto& state = metal_pipeline_archive_interpose_state();
    if (state.installed) {
        return;
    }
    const char* archive_dir = std::getenv("BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR");
    std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: BIOSEQ_MOLTENVK_BINARY_ARCHIVE_DIR=%s\n",
                 archive_dir != nullptr ? archive_dir : "(null)");
    if (archive_dir == nullptr || *archive_dir == '\0') {
        return;
    }

    void* objc_handle = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
    void* metal_handle =
        ::dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (objc_handle == nullptr || metal_handle == nullptr) {
        return;
    }

    ObjcRuntimeFns objc{};
    objc.msg_send = ::dlsym(objc_handle, "objc_msgSend");
    objc.get_class = reinterpret_cast<decltype(objc.get_class)>(
        ::dlsym(objc_handle, "objc_getClass"));
    objc.sel_register_name = reinterpret_cast<decltype(objc.sel_register_name)>(
        ::dlsym(objc_handle, "sel_registerName"));
    objc.object_get_class = reinterpret_cast<decltype(objc.object_get_class)>(
        ::dlsym(objc_handle, "object_getClass"));
    objc.class_get_superclass = reinterpret_cast<decltype(objc.class_get_superclass)>(
        ::dlsym(objc_handle, "class_getSuperclass"));
    objc.class_get_instance_method = reinterpret_cast<decltype(objc.class_get_instance_method)>(
        ::dlsym(objc_handle, "class_getInstanceMethod"));
    objc.method_set_implementation = reinterpret_cast<decltype(objc.method_set_implementation)>(
        ::dlsym(objc_handle, "method_setImplementation"));
    if (objc.msg_send == nullptr || objc.get_class == nullptr
        || objc.sel_register_name == nullptr || objc.object_get_class == nullptr
        || objc.class_get_superclass == nullptr || objc.class_get_instance_method == nullptr
        || objc.method_set_implementation == nullptr) {
        return;
    }

    using CreateDeviceFn = void* (*)();
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        ::dlsym(metal_handle, "MTLCreateSystemDefaultDevice"));
    if (create_device == nullptr) {
        return;
    }
    void* device = create_device();
    if (device == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: MTLCreateSystemDefaultDevice returned NULL\n");
        return;
    }

    void* cls = objc.object_get_class(device);
    void* sel_async =
        objc.sel_register_name("newComputePipelineStateWithDescriptor:options:completionHandler:");
    void* sel_sync =
        objc.sel_register_name("newComputePipelineStateWithDescriptor:options:reflection:error:");
    void* async_method = nullptr;
    void* sync_method = nullptr;
    for (void* cursor = cls; cursor != nullptr && (async_method == nullptr || sync_method == nullptr);
         cursor = objc.class_get_superclass(cursor)) {
        if (async_method == nullptr) {
            async_method = objc.class_get_instance_method(cursor, sel_async);
        }
        if (sync_method == nullptr) {
            sync_method = objc.class_get_instance_method(cursor, sel_sync);
        }
    }
    if (async_method == nullptr && sync_method == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: newComputePipelineStateWithDescriptor methods NOT FOUND\n");
        return;
    }
    std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: methods found async=%d sync=%d\n",
                 async_method != nullptr ? 1 : 0, sync_method != nullptr ? 1 : 0);

    state.objc = objc;
    state.archive_dir = archive_dir;
    state.device = device;
    state.sel_binary_archives = objc.sel_register_name("binaryArchives");
    state.sel_set_binary_archives = objc.sel_register_name("setBinaryArchives:");
    state.sel_compute_function = objc.sel_register_name("computeFunction");
    state.sel_name = objc.sel_register_name("name");
    state.sel_utf8_string = objc.sel_register_name("UTF8String");
    state.sel_count = objc.sel_register_name("count");
    state.sel_max_total_threads = objc.sel_register_name("maxTotalThreadsPerThreadgroup");
    state.sel_threadgroup_multiple =
        objc.sel_register_name("threadGroupSizeIsMultipleOfThreadExecutionWidth");

    if (async_method != nullptr) {
        state.original_new_pipeline_async =
            objc.method_set_implementation(
                async_method,
                reinterpret_cast<void*>(&bioseq_metal_new_compute_pipeline_state_async));
    }
    if (sync_method != nullptr) {
        state.original_new_pipeline_sync =
            objc.method_set_implementation(
                sync_method,
                reinterpret_cast<void*>(&bioseq_metal_new_compute_pipeline_state_sync));
    }
    state.installed = state.original_new_pipeline_async != nullptr
        || state.original_new_pipeline_sync != nullptr;
    std::fprintf(stderr, "[bioseq-gpu-diag] pso-interpose: installed=%d\n", state.installed ? 1 : 0);
    if (!state.installed) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(state.mu);
        refresh_metal_binary_archives_locked(state);
    }
    bioseq_log<DBG>(
        "[vulkan_runtime] installed Metal compute pipeline archive interpose dir={} async={} sync={}",
        state.archive_dir,
        state.original_new_pipeline_async != nullptr,
        state.original_new_pipeline_sync != nullptr);
}
#else
void install_moltenvk_command_library_interpose() noexcept {}
void install_metal_pipeline_archive_interpose() noexcept {}
#endif

void load_vulkan_runtime_functions() {
    std::string missing;
#define BIOSEQ_LOAD_VULKAN_FUNCTION(name) \
    do { \
        if (name == nullptr) { \
            name = reinterpret_cast<PFN_##name>(::dlsym(RTLD_DEFAULT, #name)); \
        } \
        if (name == nullptr) { \
            if (!missing.empty()) missing += ", "; \
            missing += #name; \
        } \
    } while (false);
    BIOSEQ_VULKAN_RUNTIME_FUNCTIONS(BIOSEQ_LOAD_VULKAN_FUNCTION)
#undef BIOSEQ_LOAD_VULKAN_FUNCTION
    if (!missing.empty()) {
        throw VulkanRuntimeError(fmt::format(
            "Vulkan loader is missing required entry points: {}", missing));
    }
}

void ensure_vulkan_loader_loaded_for_runtime() {
    if (::dlsym(RTLD_DEFAULT, "vkCreateInstance") == nullptr) {
#if defined(__APPLE__)
    constexpr const char* candidates[] = {
        "@rpath/libvulkan.1.4.341.dylib",
        "@rpath/libvulkan.1.dylib",
        "@rpath/libvulkan.dylib",
        "/opt/homebrew/lib/libvulkan.dylib",
        "/usr/local/lib/libvulkan.dylib",
    };
#else
    constexpr const char* candidates[] = {
        "libvulkan.so.1",
        "libvulkan.so",
    };
#endif
    std::string errors;
    bool loaded = false;
    for (const char* path : candidates) {
        ::dlerror();
        if (void* handle = ::dlopen(path, RTLD_NOW | RTLD_GLOBAL); handle != nullptr) {
            (void)handle;
            loaded = true;
            break;
        }
        if (const char* err = ::dlerror(); err != nullptr) {
            if (!errors.empty()) errors += "; ";
            errors += path;
            errors += ": ";
            errors += err;
        }
    }
    if (!loaded && ::dlsym(RTLD_DEFAULT, "vkCreateInstance") == nullptr) {
        throw VulkanRuntimeError(fmt::format(
            "Vulkan loader unavailable for backend runtime initialization: {}",
            errors.empty() ? "no dlopen diagnostics" : errors));
    }
    }
    load_vulkan_runtime_functions();
}

[[nodiscard]] std::string symbol_image(const void* symbol) {
#if defined(__APPLE__)
    Dl_info info{};
    if (symbol != nullptr && ::dladdr(symbol, &info) != 0 && info.dli_fname != nullptr) {
        return info.dli_fname;
    }
#else
    (void)symbol;
#endif
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Buffer cleanup
// ---------------------------------------------------------------------------

void Buffer::reset() noexcept {
    if (owner != nullptr && buffer != VK_NULL_HANDLE) {
        owner->destroy_buffer(*this);
    }
    owner = nullptr;
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    bytes = 0;
    mapped = nullptr;
}

// ---------------------------------------------------------------------------
// Runtime lifecycle
// ---------------------------------------------------------------------------

Runtime::Runtime() {
    try {
        init();
    } catch (const std::exception& e) {
        init_error_ = e.what();
        bioseq_log<DBG>("[vulkan_runtime] init failed: {}", e.what());
        shutdown();
        features_ = VulkanFeatureSet{};
    } catch (...) {
        init_error_ = "unknown Vulkan initialization failure";
        bioseq_log<DBG>("[vulkan_runtime] init failed (unknown)");
        shutdown();
        features_ = VulkanFeatureSet{};
    }
}

Runtime::~Runtime() {
    shutdown();
}

Runtime& Runtime::instance() {
    static Runtime runtime;
    return runtime;
}

void Runtime::resolve_moltenvk_private_api() noexcept {
    set_workgroup_size_mvk_ = nullptr;
#if defined(__APPLE__)
    auto resolve_from = [](void* handle) noexcept
        -> PFN_vkSetWorkgroupSizeMVK_Bioseq {
        if (handle == nullptr) return nullptr;
        return reinterpret_cast<PFN_vkSetWorkgroupSizeMVK_Bioseq>(
            ::dlsym(handle, "vkSetWorkgroupSizeMVK"));
    };

    if (auto* fn = resolve_from(RTLD_DEFAULT); fn != nullptr) {
        set_workgroup_size_mvk_ = fn;
        return;
    }
    constexpr const char* candidates[] = {
        "/opt/homebrew/lib/libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib",
    };
    for (const char* path : candidates) {
        void* handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (auto* fn = resolve_from(handle); fn != nullptr) {
            set_workgroup_size_mvk_ = fn;
            return;
        }
    }
#endif
}

bool Runtime::use_shader_provider_locked() const noexcept {
    RuntimeShaderProvider* provider = runtime_shader_provider_slot();
    return provider != nullptr && provider->load_module != nullptr;
}

void prepare_shader_provider_runtime() {
    RuntimeShaderProvider* provider = runtime_shader_provider_slot();
    if (provider == nullptr || provider->prepare_runtime == nullptr) {
        return;
    }
    auto prepared = provider->prepare_runtime();
    if (!prepared) {
        throw VulkanRuntimeError(fmt::format(
            "runtime shader provider prepare failed: {}", prepared.error()));
    }
}

void Runtime::init() {
    // --- Instance ---
    ensure_vulkan_loader_loaded_for_runtime();
    ensure_homebrew_moltenvk_icd_visible();
    prepare_shader_provider_runtime();
#if defined(__APPLE__)
    // MoltenVK can compile its internal command shaders while enumerating or
    // creating the device. In a forked PostgreSQL backend that source compile
    // path reaches MTLCompilerService over XPC and aborts, so install the
    // cached command-library interpose before the first Vulkan loader query that
    // may enter MoltenVK device setup.
    install_moltenvk_command_library_interpose();
#endif
    const auto inst_exts = instance_extensions();
    std::vector<const char*> enabled_inst_exts;
    VkInstanceCreateFlags inst_flags = 0;
    if (has_extension(inst_exts, kPortabilityEnumeration)) {
        enabled_inst_exts.push_back(kPortabilityEnumeration);
        inst_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    if (has_extension(inst_exts, kGetPhysicalDeviceProperties2)) {
        enabled_inst_exts.push_back(kGetPhysicalDeviceProperties2);
    }

    VkBool32 mvk_use_metal_argument_buffers = VK_FALSE;
    VkLayerSettingEXT mvk_settings[] = {{
        "MoltenVK",
        "useMetalArgumentBuffers",
        VK_LAYER_SETTING_TYPE_BOOL32_EXT,
        1u,
        &mvk_use_metal_argument_buffers,
    }};
    VkLayerSettingsCreateInfoEXT layer_settings{};
    layer_settings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layer_settings.settingCount = 1u;
    layer_settings.pSettings = mvk_settings;
#if defined(__APPLE__)
    const bool has_layer_settings = has_extension(inst_exts, kLayerSettings);
    if (has_layer_settings) {
        enabled_inst_exts.push_back(kLayerSettings);
    }
#endif

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "bioseq";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "bioseq-gpu";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo inst_info{};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.flags = inst_flags;
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_inst_exts.size());
    inst_info.ppEnabledExtensionNames =
        enabled_inst_exts.empty() ? nullptr : enabled_inst_exts.data();
#if defined(__APPLE__)
    if (has_layer_settings) {
        inst_info.pNext = &layer_settings;
    }
#endif

    check_vk(vkCreateInstance(&inst_info, nullptr, &instance_), "vkCreateInstance");
    resolve_moltenvk_private_api();

    // --- Physical device selection (compute queue + shaderInt64) ---
    std::uint32_t device_count = 0;
    check_vk(vkEnumeratePhysicalDevices(instance_, &device_count, nullptr),
             "vkEnumeratePhysicalDevices(count)");
    if (device_count == 0) {
        throw VulkanRuntimeError("no Vulkan physical devices");
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    check_vk(vkEnumeratePhysicalDevices(instance_, &device_count, devices.data()),
             "vkEnumeratePhysicalDevices(list)");

    bool device_chosen = false;
    bool device_has_portability_subset = false;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceFeatures candidate_features{};
        vkGetPhysicalDeviceFeatures(candidate, &candidate_features);
        if (candidate_features.shaderInt64 != VK_TRUE) {
            continue;  // Phase A requires native 64-bit ints in the shader
        }

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        if (family_count == 0) continue;
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        std::optional<std::uint32_t> compute_family;
        for (std::uint32_t f = 0; f < family_count; ++f) {
            if (families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                compute_family = f;
                break;
            }
        }
        if (!compute_family) continue;

        physical_ = candidate;
        queue_family_ = *compute_family;
        features_.shader_int64 = true;
        device_has_portability_subset =
            has_extension(device_extensions(candidate), kPortabilitySubset);
        device_chosen = true;
        break;
    }
    if (!device_chosen) {
        throw VulkanRuntimeError("no Vulkan device with a compute queue + shaderInt64");
    }

    // --- Logical device (enable shaderInt64) ---
    std::vector<const char*> enabled_dev_exts;
    if (device_has_portability_subset) {
        enabled_dev_exts.push_back(kPortabilitySubset);
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.shaderInt64 = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_dev_exts.size());
    device_info.ppEnabledExtensionNames =
        enabled_dev_exts.empty() ? nullptr : enabled_dev_exts.data();
    device_info.pEnabledFeatures = &enabled_features;

    install_metal_pipeline_archive_interpose();
    check_vk(vkCreateDevice(physical_, &device_info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    metallib_provider_capable_ = set_workgroup_size_mvk_ != nullptr;

    // --- Limits + scalar-block-layout probe ---
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar_features{};
    scalar_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &scalar_features;
    if (has_extension(inst_exts, kGetPhysicalDeviceProperties2)) {
        vkGetPhysicalDeviceFeatures2(physical_, &features2);
        features_.scalar_block_layout = scalar_features.scalarBlockLayout == VK_TRUE;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    features_.max_workgroup_invocations = props.limits.maxComputeWorkGroupInvocations;
    features_.max_workgroup_count_x = props.limits.maxComputeWorkGroupCount[0];
    features_.max_shared_memory_bytes = props.limits.maxComputeSharedMemorySize;

    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props);
    std::uint64_t device_local = 0;
    for (std::uint32_t h = 0; h < mem_props.memoryHeapCount; ++h) {
        if (mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            device_local = sat_max(device_local, mem_props.memoryHeaps[h].size);
        }
    }
    features_.device_local_bytes = device_local;

    // --- Command pool / buffer / fence ---
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    check_vk(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_),
             "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = command_pool_;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    check_vk(vkAllocateCommandBuffers(device_, &cmd_alloc, &command_buffer_),
             "vkAllocateCommandBuffers");

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check_vk(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence");

    // --- Fixed descriptor-set layout (4 storage buffers) + pipeline layout ---
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (std::uint32_t b = 0; b < 4; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    set_layout_info.pBindings = bindings.data();
    check_vk(vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr,
                                         &descriptor_set_layout_),
             "vkCreateDescriptorSetLayout");

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ChainingPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_),
             "vkCreatePipelineLayout");

    // --- Descriptor pool + one reusable descriptor set ---
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 4;
    VkDescriptorPoolCreateInfo desc_pool_info{};
    desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    desc_pool_info.maxSets = 1;
    desc_pool_info.poolSizeCount = 1;
    desc_pool_info.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(device_, &desc_pool_info, nullptr, &descriptor_pool_),
             "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = descriptor_pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &descriptor_set_layout_;
    check_vk(vkAllocateDescriptorSets(device_, &set_alloc, &descriptor_set_),
             "vkAllocateDescriptorSets");

    // DOC0307 §7.2 — pick the largest power-of-two anchor count the per-query
    // bitonic sort can hold in one workgroup: bounded by workgroup invocations
    // and by shared memory (each lane holds a 24-byte AnchorWire + a 4-byte
    // original index). Anchor spans above this are rejected so selected Vulkan
    // seed lookup cannot hide a CPU sort.
    {
        constexpr std::uint32_t per_lane_shared_bytes = 24u + 4u;
        std::uint32_t capacity = 0;
        for (std::uint32_t candidate = 1; candidate <= 1024u; candidate <<= 1) {
            if (candidate > features_.max_workgroup_invocations) break;
            if (static_cast<std::uint64_t>(candidate) * per_lane_shared_bytes
                > features_.max_shared_memory_bytes) {
                break;
            }
            capacity = candidate;
        }
        gpu_anchor_sort_capacity_ = capacity;
    }

    features_.available = true;
    std::strncpy(features_.device_name.data(), props.deviceName,
                 features_.device_name.size() - 1u);
    features_.device_name.back() = '\0';
    init_error_.clear();
    bioseq_log<DBG>(
        "[vulkan_runtime] device ready: {} (maxWGInvocations={}, maxWGCountX={}, "
        "maxSharedMem={}B, scalarBlockLayout={})",
        props.deviceName, features_.max_workgroup_invocations,
        features_.max_workgroup_count_x, features_.max_shared_memory_bytes,
        features_.scalar_block_layout);
}

void Runtime::shutdown() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        // DOC0310 — release arena buffers (host-visible, owned via Buffer RAII)
        // and the Phase C descriptor pool before the device goes away.
        phase_c_arena_.reset();
        if (phase_c_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, phase_c_pool_, nullptr);
            phase_c_pool_ = VK_NULL_HANDLE;
        }
        for (auto& entry : pipeline_cache_) {
            if (entry.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, entry.pipeline, nullptr);
            if (entry.module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, entry.module, nullptr);
        }
        pipeline_cache_.clear();
        for (auto& entry : phase_b_pipelines_) {
            if (entry.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, entry.pipeline, nullptr);
            if (entry.module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, entry.module, nullptr);
            if (entry.descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            if (entry.pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            if (entry.set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
        }
        phase_b_pipelines_.clear();
        if (descriptor_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        if (descriptor_set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    command_buffer_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    set_workgroup_size_mvk_ = nullptr;
    metallib_provider_capable_ = false;
}

std::uint32_t Runtime::find_memory_type(
    std::uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        const bool type_ok = (type_bits & (1u << i)) != 0;
        const bool flags_ok = (mem_props.memoryTypes[i].propertyFlags & flags) == flags;
        if (type_ok && flags_ok) {
            return i;
        }
    }
    throw VulkanRuntimeError("no compatible Vulkan memory type");
}

// ---------------------------------------------------------------------------
// Buffer allocation
// ---------------------------------------------------------------------------

Buffer Runtime::create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage) {
    if (!features_.available) {
        throw VulkanRuntimeError("create_host_buffer on unavailable device");
    }
    if (bytes == 0) bytes = 1;  // zero-sized VkBuffers are invalid

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(device_, &buf_info, nullptr, &buffer), "vkCreateBuffer(host)");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer, &reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = find_memory_type(
        reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkAllocateMemory(host)");
    }
    if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkBindBufferMemory(host)");
    }
    void* mapped = nullptr;
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkMapMemory(host)");
    }
    return Buffer{this, buffer, memory, bytes, mapped};
}

Buffer Runtime::create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage) {
    if (!features_.available) {
        throw VulkanRuntimeError("create_device_buffer on unavailable device");
    }
    if (bytes == 0) bytes = 1;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = bytes;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(device_, &buf_info, nullptr, &buffer), "vkCreateBuffer(device)");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer, &reqs);

    // Phase A targets unified memory: prefer device-local that is ALSO
    // host-visible+coherent so the round-trip can write/read it directly.
    // Apple Silicon / MoltenVK always satisfies this; the fallback keeps the
    // buffer host-visible (every Vulkan device exposes at least one
    // host-visible+coherent type) so the direct path still applies.
    std::uint32_t mem_type = 0;
    bool found = false;
    try {
        mem_type = find_memory_type(
            reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        found = true;
    } catch (const VulkanRuntimeError&) {
        found = false;
    }
    if (!found) {
        mem_type = find_memory_type(
            reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = mem_type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkAllocateMemory(device)");
    }
    if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer, nullptr);
        throw VulkanRuntimeError("vkBindBufferMemory(device)");
    }
    // Map persistently — on unified memory the device-local buffer is
    // host-visible, so the round-trip writes inputs / reads outputs directly.
    void* mapped = nullptr;
    if (vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        // Not host-visible: leave unmapped. The round-trip rejects this case
        // (Phase A requires unified memory) rather than silently mis-staging.
        mapped = nullptr;
    }
    return Buffer{this, buffer, memory, bytes, mapped};
}

void Runtime::destroy_buffer(Buffer& buffer) noexcept {
    if (device_ == VK_NULL_HANDLE) return;
    if (buffer.mapped != nullptr && buffer.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, buffer.memory);
    }
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Pipeline creation / cache
// ---------------------------------------------------------------------------

VkPipeline Runtime::create_pipeline_locked(const ChainingPipelineKey& key) {
    for (const auto& entry : pipeline_cache_) {
        if (entry.key == key) {
            return entry.pipeline;
        }
    }

    if (key.local_size == 0 || key.local_size > features_.max_workgroup_invocations) {
        throw VulkanRuntimeError(fmt::format(
            "chaining local_size {} exceeds maxComputeWorkGroupInvocations {}",
            key.local_size, features_.max_workgroup_invocations));
    }

    VkShaderModule module = VK_NULL_HANDLE;
    std::string entry_name = "main";
    bool provider_used = false;
    std::uint32_t provider_wg_x = 0;
    std::uint32_t provider_wg_y = 0;
    std::uint32_t provider_wg_z = 0;
    const ShaderVariantKey shader_key =
        make_shader_key(ShaderKind::chain_tiled_dp_u64, key.tile_size, key.local_size, 0u);
    if (use_shader_provider_locked()) {
        if (!metallib_provider_capable_) {
            throw VulkanRuntimeError(
                "runtime shader provider installed but vkSetWorkgroupSizeMVK is unavailable");
        }
        RuntimeShaderProvider* provider = runtime_shader_provider_slot();
        auto loaded = provider->load_module(
            device_, shader_key, vkCreateShaderModule);
        if (!loaded) {
            throw VulkanRuntimeError(fmt::format(
                "shader provider failed for {}: {}",
                cache_file_stem(shader_key), loaded.error()));
        }
        module = loaded->module;
        entry_name = loaded->entry_name.empty() ? std::string{"main"} : loaded->entry_name;
        provider_used = true;
        provider_wg_x = loaded->wg_x;
        provider_wg_y = loaded->wg_y;
        provider_wg_z = loaded->wg_z;
        set_workgroup_size_mvk_(module, loaded->wg_x, loaded->wg_y, loaded->wg_z);
    } else {
        std::vector<std::uint32_t> spirv;
        try {
            spirv = compile_shader_spirv(ShaderCompileRequest{
                ShaderKind::chain_tiled_dp_u64, key.tile_size, key.local_size});
        } catch (const std::exception& e) {
            throw VulkanRuntimeError(fmt::format("shader compile failed: {}", e.what()));
        }

        // Validate the direct-anchor ABI in the emitted SPIR-V before trusting the
        // pipeline. A stride/offset mismatch would silently corrupt the DP read of
        // every anchor; reject rather than hide it behind a conversion buffer.
        const SpirvLayoutScan scan = scan_spirv_layout(spirv);
        if (!scan.has_array_stride(24)) {
            throw VulkanRuntimeError("SPIR-V Anchors array stride != 24");
        }
        if (!scan.has_struct_offsets({0u, 8u, 16u, 20u})) {
            throw VulkanRuntimeError("SPIR-V AnchorWire member offsets != {0,8,16,20}");
        }

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
        module_info.pCode = spirv.data();
        check_vk(vkCreateShaderModule(device_, &module_info, nullptr, &module),
                 "vkCreateShaderModule");
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = entry_name.c_str();

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pr = vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    if (pr != VK_SUCCESS) {
        vkDestroyShaderModule(device_, module, nullptr);
        throw VulkanRuntimeError(
            fmt::format(
                "vkCreateComputePipelines: VkResult {} provider={} entry={} wg={},{},{} vk={} mvk={}",
                static_cast<int>(pr),
                provider_used ? "metallib" : "spirv",
                entry_name,
                provider_wg_x,
                provider_wg_y,
                provider_wg_z,
                symbol_image(reinterpret_cast<const void*>(vkCreateComputePipelines)),
                symbol_image(reinterpret_cast<const void*>(set_workgroup_size_mvk_))));
    }

    pipeline_cache_.push_back(PipelineEntry{key, module, pipeline, entry_name});
    bioseq_log<DBG>("[vulkan_runtime] built chaining pipeline tile={} local={}",
                    key.tile_size, key.local_size);
    return pipeline;
}

// ---------------------------------------------------------------------------
// Upload / download helpers (called under mu_, command buffer already open)
// ---------------------------------------------------------------------------

void Runtime::upload_locked(
    Buffer& transfer_scratch, Buffer& dst, std::span<const std::byte> bytes,
    VkAccessFlags dst_access) {
    (void)dst_access;  // host-coherent + queue submit makes host writes visible
    if (bytes.empty()) return;
    if (dst.mapped == nullptr) {
        // Phase A requires host-visible device buffers (unified memory). The
        // scratch param is the future discrete-GPU bounce; reject cleanly here.
        (void)transfer_scratch;
        throw VulkanRuntimeError("device buffer is not host-visible (Phase A needs unified memory)");
    }
    DEBUG_ASSERT(bytes.size() <= dst.bytes, "upload exceeds destination buffer size");
    std::memcpy(dst.mapped, bytes.data(), bytes.size());
}

void Runtime::download_locked(
    Buffer& transfer_scratch, const Buffer& src, std::span<std::byte> bytes) {
    if (bytes.empty()) return;
    if (src.mapped == nullptr) {
        (void)transfer_scratch;
        throw VulkanRuntimeError("device buffer is not host-visible (Phase A needs unified memory)");
    }
    DEBUG_ASSERT(bytes.size() <= src.bytes, "download exceeds source buffer size");
    std::memcpy(bytes.data(), src.mapped, bytes.size());
}

// ---------------------------------------------------------------------------
// The one hot entry point
// ---------------------------------------------------------------------------

void Runtime::run_chaining_tiled_dp_roundtrip(
    const ChainingPipelineKey& key,
    Buffer& transfer_scratch,
    Buffer& anchors,
    Buffer& ranges,
    Buffer& best_scores,
    Buffer& predecessors,
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
    std::int32_t max_chain_skip) {
    std::lock_guard<std::mutex> guard(mu_);

    if (!features_.available) {
        throw VulkanRuntimeError("run_chaining_tiled_dp_roundtrip on unavailable device");
    }
    // DOC0353 (approach E): ONE WORKGROUP PER RANGE; the kernel grid-strides
    // workgroups over ranges, so the dispatch is min(range_count, device limit)
    // workgroups — range_count is just data, no workgroup-count throw.
    const std::uint32_t chain_groups =
        std::min(range_count, features_.max_workgroup_count_x);
    DEBUG_ASSERT(transfer_scratch.valid(), "transfer scratch buffer must be allocated");

    const VkPipeline pipeline = create_pipeline_locked(key);

    // H2D: write the anchors/ranges and the caller-initialised best-scores /
    // predecessors straight into the host-visible device buffers.
    upload_locked(transfer_scratch, anchors, anchors_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, ranges, ranges_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, best_scores, best_scores_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    upload_locked(transfer_scratch, predecessors, predecessors_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // Point the reusable descriptor set at this call's four buffers.
    const std::array<VkDescriptorBufferInfo, 4> buffer_infos{{
        {anchors.buffer, 0, VK_WHOLE_SIZE},
        {ranges.buffer, 0, VK_WHOLE_SIZE},
        {best_scores.buffer, 0, VK_WHOLE_SIZE},
        {predecessors.buffer, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t b = 0; b < 4; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = descriptor_set_;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &buffer_infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    ChainingPushConstants push{};
    push.anchor_count = anchor_count;
    push.range_count = range_count;
    push.max_dist_x = max_dist_x;
    push.max_dist_y = max_dist_y;
    push.bandwidth = bandwidth;
    push.max_iter = max_iter;
    push.gap_penalty = gap_penalty;
    push.skip_penalty = skip_penalty;
    push.max_chain_skip = max_chain_skip;

    // Record: host-write barrier -> dispatch -> shader-write->host-read barrier.
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host_to_shader,
                         0, nullptr, 0, nullptr);

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
    vkCmdPushConstants(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(command_buffer_, chain_groups, 1, 1);

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shader_to_host,
                         0, nullptr, 0, nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences");

    // D2H: read the propagated scores / predecessors back.
    download_locked(transfer_scratch, best_scores, best_scores_bytes);
    download_locked(transfer_scratch, predecessors, predecessors_bytes);

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
}

GpuCoordinateChainTraceResult Runtime::run_chaining_coordinate_trace_roundtrip(
    const ChainingPipelineKey& key,
    Buffer& transfer_scratch,
    Buffer& anchors,
    Buffer& ranges,
    Buffer& best_scores,
    Buffer& predecessors,
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
    std::uint32_t probe_limit)
{
    std::lock_guard<std::mutex> guard(mu_);

    GpuCoordinateChainTraceResult out{};
    if (!features_.available) {
        throw VulkanRuntimeError("run_chaining_coordinate_trace_roundtrip on unavailable device");
    }

    const std::uint32_t chain_groups =
        std::min(range_count, features_.max_workgroup_count_x);
    DEBUG_ASSERT(transfer_scratch.valid(), "transfer scratch buffer must be allocated");

    const VkPipeline pipeline = create_pipeline_locked(key);
    const PhaseBPipeline coord_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_coordinate_reduce, 128u, 1u);

    upload_locked(transfer_scratch, anchors, anchors_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, ranges, ranges_bytes, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(transfer_scratch, best_scores, best_scores_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    upload_locked(transfer_scratch, predecessors, predecessors_bytes,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    constexpr VkBufferUsageFlags coordinate_trace_buffer_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Buffer visited = create_device_buffer(
        std::uint64_t{anchor_count} * sizeof(std::uint32_t),
        coordinate_trace_buffer_usage);
    Buffer trace = create_device_buffer(
        std::uint64_t{anchor_count} * sizeof(std::uint32_t),
        coordinate_trace_buffer_usage);
    Buffer result_buf = create_device_buffer(
        sizeof(GpuCoordinateOnlyResultWire), coordinate_trace_buffer_usage);
    if (visited.mapped == nullptr || trace.mapped == nullptr || result_buf.mapped == nullptr) {
        throw VulkanRuntimeError("coordinate-only chaining requires host-visible trace buffers");
    }
    std::memset(visited.mapped, 0, static_cast<std::size_t>(visited.bytes));
    std::memset(result_buf.mapped, 0, static_cast<std::size_t>(result_buf.bytes));

    const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
        {anchors.buffer, 0, VK_WHOLE_SIZE},
        {ranges.buffer, 0, VK_WHOLE_SIZE},
        {best_scores.buffer, 0, VK_WHOLE_SIZE},
        {predecessors.buffer, 0, VK_WHOLE_SIZE},
    }};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t b = 0; b < 4; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = descriptor_set_;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &chain_infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    ChainingPushConstants chain_push{};
    chain_push.anchor_count = anchor_count;
    chain_push.range_count = range_count;
    chain_push.max_dist_x = max_dist_x;
    chain_push.max_dist_y = max_dist_y;
    chain_push.bandwidth = bandwidth;
    chain_push.max_iter = max_iter;
    chain_push.gap_penalty = gap_penalty;
    chain_push.skip_penalty = skip_penalty;
    chain_push.max_chain_skip = max_chain_skip;

    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(chain_coord)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(chain_coord)");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        command_buffer_,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &host_to_shader,
        0,
        nullptr,
        0,
        nullptr);

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
    vkCmdPushConstants(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(chain_push), &chain_push);
    vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
    VkMemoryBarrier shader_to_shader{};
    shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_shader.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        command_buffer_,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &shader_to_shader,
        0,
        nullptr,
        0,
        nullptr);

    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
        {anchors.buffer, 0, VK_WHOLE_SIZE},
        {best_scores.buffer, 0, VK_WHOLE_SIZE},
        {predecessors.buffer, 0, VK_WHOLE_SIZE},
        {visited.buffer, 0, visited.bytes},
        {trace.buffer, 0, trace.bytes},
        {result_buf.buffer, 0, result_buf.bytes},
    }};
    GpuCoordinateReducePush coord_push{
        .anchor_count = anchor_count,
        .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
        .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
        .min_chain_score = min_chain_score,
        .min_seed_count = min_seed_count,
        .probe_limit = probe_limit,
        .reserved = 0u,
    };
    record_phase_c_dispatch_locked(
        command_buffer_, coord_pipe, coord_infos,
        &coord_push, sizeof(coord_push), 1u);
    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(
        command_buffer_,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        1,
        &shader_to_host,
        0,
        nullptr,
        0,
        nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "end(chain_coord)");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "submit(chain_coord)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX),
             "wait(chain_coord)");
    check_vk(vkResetFences(device_, 1, &fence_), "reset_fence(chain_coord)");

    std::memcpy(&out.result, result_buf.mapped, sizeof(out.result));
    if (out.result.valid != 0u) {
        if (out.result.seed_count < 0
            || static_cast<std::uint64_t>(out.result.seed_count) > anchor_count) {
            throw VulkanRuntimeError("coordinate-only chaining produced invalid trace length");
        }
        const auto trace_count = static_cast<std::size_t>(out.result.seed_count);
        out.trace.resize(trace_count);
        if (trace_count > 0) {
            std::memcpy(out.trace.data(), trace.mapped, trace_count * sizeof(std::uint32_t));
        }
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(chain_coord_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// ---------------------------------------------------------------------------
// DOC0307 Phase B — seeding roundtrips
// ---------------------------------------------------------------------------

namespace {

struct SeedLookupPush {
    std::uint32_t minimizer_count;
    std::uint32_t words32_per_minimizer;
    std::uint32_t word_kind;
    std::uint32_t hash_shift;
    std::uint32_t pos_bits;
    std::uint32_t query_len;
    std::uint32_t kmer_size;
    std::uint32_t slot_mask;
    std::uint32_t exact_anchor_count;
};
static_assert(sizeof(SeedLookupPush) == 36);

constexpr VkBufferUsageFlags kPhaseBBufferUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

struct AlignmentExtensionPush {
    std::uint32_t job_count = 0;
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
static_assert(sizeof(AlignmentExtensionPush) == 44);
static_assert(std::is_standard_layout_v<AlignmentExtensionPush>);

struct AlignmentCigarPush {
    std::uint32_t job_count = 0;
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
static_assert(sizeof(AlignmentCigarPush) == 60);
static_assert(std::is_standard_layout_v<AlignmentCigarPush>);

[[nodiscard]] AlignmentExtensionPush alignment_extension_push(
    std::uint32_t job_count,
    const GpuAlignmentExtensionParams& params) noexcept
{
    return AlignmentExtensionPush{
        job_count,
        params.max_query_len,
        params.max_target_len,
        params.max_band_width,
        params.alphabet_size,
        params.match_score,
        params.mismatch_penalty,
        params.gap_open_penalty,
        params.gap_extend_penalty,
        params.flags,
        params.reserved0};
}

[[nodiscard]] AlignmentCigarPush alignment_cigar_push(
    std::uint32_t job_count,
    const GpuAlignmentCigarParams& params) noexcept
{
    return AlignmentCigarPush{
        job_count,
        params.alphabet_size,
        params.max_band_width,
        params.tile_query,
        params.tile_target,
        params.match_score,
        params.mismatch_penalty,
        params.gap_open_short,
        params.gap_extend_short,
        params.gap_open_long,
        params.gap_extend_long,
        params.z_drop,
        params.end_bonus,
        params.flags,
        params.reserved};
}

void validate_alignment_extension_score(const char* name, std::int32_t value) {
    constexpr std::int32_t kMaxAbsScore = 1'000'000;
    if (value < 0 || value > kMaxAbsScore) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension {} {} out of range (0..{})",
            name, value, kMaxAbsScore));
    }
}

void validate_alignment_extension_symbols(
    std::span<const std::uint32_t> symbols,
    std::uint32_t offset,
    std::uint32_t length,
    std::uint32_t alphabet_size,
    const char* label,
    std::size_t job_index)
{
    const std::uint64_t begin = offset;
    const std::uint64_t end = begin + static_cast<std::uint64_t>(length);
    if (begin > symbols.size() || end > symbols.size()) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension job {} {} range [{}, {}) exceeds symbol count {}",
            job_index, label, begin, end, symbols.size()));
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint32_t symbol = symbols[static_cast<std::size_t>(offset) + i];
        if (symbol >= alphabet_size) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} {} symbol {} at local offset {} exceeds alphabet_size {}",
                job_index, label, symbol, i, alphabet_size));
        }
    }
}

void validate_alignment_extension_inputs(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const GpuAlignmentExtensionJobWire> jobs,
    const GpuAlignmentExtensionParams& params)
{
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("alignment extension job_count exceeds uint32");
    }
    if (query_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("alignment extension query symbol stream exceeds uint32 offsets");
    }
    if (target_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("alignment extension target symbol stream exceeds uint32 offsets");
    }
    if (params.max_query_len == 0
        || params.max_query_len > k_gpu_alignment_extension_max_len) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension max_query_len {} out of range (1..{})",
            params.max_query_len, k_gpu_alignment_extension_max_len));
    }
    if (params.max_target_len == 0
        || params.max_target_len > k_gpu_alignment_extension_max_len) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension max_target_len {} out of range (1..{})",
            params.max_target_len, k_gpu_alignment_extension_max_len));
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension max_band_width {} exceeds {}",
            params.max_band_width, k_gpu_alignment_extension_max_band_width));
    }
    if (params.alphabet_size == 0 || params.alphabet_size > 256u) {
        throw VulkanRuntimeError(fmt::format(
            "alignment extension alphabet_size {} out of range (1..256)",
            params.alphabet_size));
    }
    validate_alignment_extension_score("match_score", params.match_score);
    validate_alignment_extension_score("mismatch_penalty", params.mismatch_penalty);
    validate_alignment_extension_score("gap_open_penalty", params.gap_open_penalty);
    validate_alignment_extension_score("gap_extend_penalty", params.gap_extend_penalty);
    if (params.match_score == 0) {
        throw VulkanRuntimeError("alignment extension match_score must be positive");
    }
    if (params.flags != 0 || params.reserved0 != 0) {
        throw VulkanRuntimeError("alignment extension params flags/reserved must be zero");
    }

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const GpuAlignmentExtensionJobWire& job = jobs[i];
        if (job.query_len > params.max_query_len) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} query_len {} exceeds max_query_len {}",
                i, job.query_len, params.max_query_len));
        }
        if (job.target_len > params.max_target_len) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} target_len {} exceeds max_target_len {}",
                i, job.target_len, params.max_target_len));
        }
        if (job.band_width > params.max_band_width) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} band_width {} exceeds max_band_width {}",
                i, job.band_width, params.max_band_width));
        }
        if (job.flags != 0 || job.reserved0 != 0 || job.reserved1 != 0) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension job {} flags/reserved must be zero", i));
        }
        validate_alignment_extension_symbols(
            query_symbols, job.query_offset, job.query_len,
            params.alphabet_size, "query", i);
        validate_alignment_extension_symbols(
            target_symbols, job.target_offset, job.target_len,
            params.alphabet_size, "target", i);
    }
}

void validate_alignment_cigar_score(const char* name, std::int32_t value) {
    constexpr std::int32_t kMaxAbsScore = 1'000'000;
    if (value < 0 || value > kMaxAbsScore) {
        throw VulkanRuntimeError(fmt::format(
            "alignment CIGAR extension {} {} out of range (0..{})",
            name, value, kMaxAbsScore));
    }
}

void validate_alignment_cigar_inputs(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const GpuAlignmentCigarJobWire> jobs,
    const GpuAlignmentCigarParams& params,
    std::uint32_t packed_cigar_capacity)
{
    if (jobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("alignment CIGAR extension job_count exceeds uint32");
    }
    if (query_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError(
            "alignment CIGAR extension query symbol stream exceeds uint32 offsets");
    }
    if (target_symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError(
            "alignment CIGAR extension target symbol stream exceeds uint32 offsets");
    }
    if (params.alphabet_size == 0 || params.alphabet_size > 256u) {
        throw VulkanRuntimeError(fmt::format(
            "alignment CIGAR extension alphabet_size {} out of range (1..256)",
            params.alphabet_size));
    }
    if (params.max_band_width > k_gpu_alignment_extension_max_band_width) {
        throw VulkanRuntimeError(fmt::format(
            "alignment CIGAR extension max_band_width {} exceeds {}",
            params.max_band_width, k_gpu_alignment_extension_max_band_width));
    }
    if (params.tile_query == 0 || params.tile_query > k_gpu_alignment_extension_max_len
        || params.tile_target == 0
        || params.tile_target > k_gpu_alignment_extension_max_len) {
        throw VulkanRuntimeError(fmt::format(
            "alignment CIGAR extension tile geometry {}x{} out of range (1..{})",
            params.tile_query, params.tile_target, k_gpu_alignment_extension_max_len));
    }
    validate_alignment_cigar_score("match_score", params.match_score);
    validate_alignment_cigar_score("mismatch_penalty", params.mismatch_penalty);
    validate_alignment_cigar_score("gap_open_short", params.gap_open_short);
    validate_alignment_cigar_score("gap_extend_short", params.gap_extend_short);
    validate_alignment_cigar_score("gap_open_long", params.gap_open_long);
    validate_alignment_cigar_score("gap_extend_long", params.gap_extend_long);
    validate_alignment_cigar_score("z_drop", params.z_drop);
    validate_alignment_cigar_score("end_bonus", params.end_bonus);
    if (params.match_score == 0) {
        throw VulkanRuntimeError("alignment CIGAR extension match_score must be positive");
    }
    if (params.flags != 0 || params.reserved != 0) {
        throw VulkanRuntimeError("alignment CIGAR extension params flags/reserved must be zero");
    }
    if (packed_cigar_capacity == 0) {
        throw VulkanRuntimeError("alignment CIGAR extension packed CIGAR capacity is zero");
    }

    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const GpuAlignmentCigarJobWire& job = jobs[i];
        if (job.query_len > k_gpu_alignment_simple_cigar_max_len
            || job.target_len > k_gpu_alignment_simple_cigar_max_len) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} length {}x{} exceeds {}",
                i, job.query_len, job.target_len,
                k_gpu_alignment_simple_cigar_max_len));
        }
        if (job.band_width > params.max_band_width) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} band_width {} exceeds max_band_width {}",
                i, job.band_width, params.max_band_width));
        }
        if (job.cigar_capacity == 0) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} CIGAR capacity is zero", i));
        }
        const bool bounded_dp_candidate =
            job.query_len <= k_gpu_alignment_extension_max_len
            && job.target_len <= k_gpu_alignment_extension_max_len;
        const std::uint64_t traceback_cells = bounded_dp_candidate
            ? (static_cast<std::uint64_t>(job.query_len) + 1u)
                * (static_cast<std::uint64_t>(job.target_len) + 1u)
            : 0u;
        const std::uint64_t traceback_end =
            static_cast<std::uint64_t>(job.traceback_tile_offset)
            + static_cast<std::uint64_t>(job.traceback_tile_count);
        if (bounded_dp_candidate && job.traceback_tile_count < traceback_cells) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} traceback capacity {} below required {}",
                i, job.traceback_tile_count, traceback_cells));
        }
        if (traceback_end > std::numeric_limits<std::uint32_t>::max()) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} traceback range [{}, {}) exceeds uint32",
                i, job.traceback_tile_offset, traceback_end));
        }
        const std::uint64_t cigar_end =
            static_cast<std::uint64_t>(job.cigar_offset)
            + static_cast<std::uint64_t>(job.cigar_capacity);
        if (cigar_end > packed_cigar_capacity) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension job {} CIGAR range [{}, {}) exceeds capacity {}",
                i, job.cigar_offset, cigar_end, packed_cigar_capacity));
        }
        validate_alignment_extension_symbols(
            query_symbols, job.query_symbol_offset, job.query_len,
            params.alphabet_size, "query", i);
        validate_alignment_extension_symbols(
            target_symbols, job.target_symbol_offset, job.target_len,
            params.alphabet_size, "target", i);
    }
}

/// Device byte footprint for `count` packed minimizers of the given word kind.
[[nodiscard]] std::uint64_t minimizer_device_bytes(
    std::uint32_t word_kind, std::uint32_t words32_per_minimizer,
    std::uint64_t count) noexcept
{
    if (word_kind == 0u) {  // u16: two minimizers packed per 32-bit word
        return ((count + 1u) / 2u) * sizeof(std::uint32_t);
    }
    return count * static_cast<std::uint64_t>(words32_per_minimizer)
         * sizeof(std::uint32_t);
}

/// Host byte footprint for `count` minimizers (one packed element each).
[[nodiscard]] std::uint64_t minimizer_host_bytes(
    std::uint32_t word_kind, std::uint64_t count) noexcept
{
    switch (word_kind) {
        case 0u:  return count * 2u;   // u16
        case 1u:  return count * 4u;   // u32
        case 2u:  return count * 8u;   // u64
        default:  return count * 16u;  // u64x2
    }
}

}  // namespace

Runtime::PhaseBPipeline Runtime::get_phase_b_pipeline_locked(
    ShaderKind kind, std::uint32_t local_size, std::uint32_t max_span)
{
    for (auto& entry : phase_b_pipelines_) {
        if (entry.kind == kind && entry.local_size == local_size
            && entry.max_span == max_span) {
            return entry;
        }
    }
    if (local_size == 0 || local_size > features_.max_workgroup_invocations) {
        throw VulkanRuntimeError(fmt::format(
            "Phase B local_size {} exceeds maxComputeWorkGroupInvocations {}",
            local_size, features_.max_workgroup_invocations));
    }

    const DescriptorLayoutKind layout_kind = descriptor_layout_for(kind);
    const std::uint32_t binding_count = descriptor_binding_count(layout_kind);
    const std::uint32_t push_size = push_constant_size(kind);

    PhaseBPipeline entry;
    entry.kind = kind;
    entry.local_size = local_size;
    entry.max_span = max_span;
    entry.binding_count = binding_count;
    entry.push_size = push_size;

    std::vector<VkDescriptorSetLayoutBinding> bindings(binding_count);
    for (std::uint32_t b = 0; b < binding_count; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = binding_count;
    set_layout_info.pBindings = bindings.data();
    check_vk(vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &entry.set_layout),
             "vkCreateDescriptorSetLayout(phase_b)");

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = push_size;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &entry.set_layout;
    layout_info.pushConstantRangeCount = push_size > 0 ? 1u : 0u;
    layout_info.pPushConstantRanges = push_size > 0 ? &push_range : nullptr;
    check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &entry.pipeline_layout),
             "vkCreatePipelineLayout(phase_b)");

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = binding_count;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &entry.descriptor_pool),
             "vkCreateDescriptorPool(phase_b)");

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = entry.descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &entry.set_layout;
    check_vk(vkAllocateDescriptorSets(device_, &set_alloc, &entry.descriptor_set),
             "vkAllocateDescriptorSets(phase_b)");

    const ShaderVariantKey shader_key = make_shader_key(kind, 384u, local_size, max_span);
    if (use_shader_provider_locked()) {
        if (!metallib_provider_capable_) {
            vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
            throw VulkanRuntimeError(
                "runtime shader provider installed but vkSetWorkgroupSizeMVK is unavailable");
        }
        RuntimeShaderProvider* provider = runtime_shader_provider_slot();
        auto loaded = provider->load_module(
            device_, shader_key, vkCreateShaderModule);
        if (!loaded) {
            vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
            throw VulkanRuntimeError(fmt::format(
                "shader provider failed for {}: {}",
                cache_file_stem(shader_key), loaded.error()));
        }
        entry.module = loaded->module;
        entry.entry_name =
            loaded->entry_name.empty() ? std::string{"main"} : loaded->entry_name;
        set_workgroup_size_mvk_(
            entry.module, loaded->wg_x, loaded->wg_y, loaded->wg_z);
    } else {
        std::vector<std::uint32_t> spirv;
        try {
            ShaderCompileRequest request{};
            request.kind = kind;
            request.tile_size = 384;
            request.local_size = local_size;
            request.max_span = max_span;
            spirv = compile_shader_spirv(request);
        } catch (const std::exception& compile_error) {
            vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
            throw VulkanRuntimeError(fmt::format("phase B shader compile failed: {}",
                                                 compile_error.what()));
        }

        // Pin the SSBO wire ABI: a sort-range-stride mismatch would silently
        // corrupt direct SortRangeWire reads.
        const SpirvLayoutScan scan = scan_spirv_layout(spirv);
        if (kind == ShaderKind::sort_anchors_bitonic && !scan.has_array_stride(8)) {
            vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
            throw VulkanRuntimeError("SPIR-V SortRange array stride != 8");
        }
        if (kind == ShaderKind::generate_units &&
            (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_array_stride(max_span / 8u))) {
            vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
            throw VulkanRuntimeError("SPIR-V generate_units layout mismatch");
        }
        if (kind == ShaderKind::align_extend_banded_affine &&
            (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_binding(2) || !scan.has_binding(3) ||
             !scan.has_array_stride(4) || !scan.has_array_stride(16) ||
             !scan.has_array_stride(32))) {
            vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
            throw VulkanRuntimeError("SPIR-V alignment extension layout mismatch");
        }
        if (kind == ShaderKind::align_extend_simple_cigar &&
            (!scan.has_binding(0) || !scan.has_binding(1) ||
             !scan.has_binding(2) || !scan.has_binding(3) ||
             !scan.has_binding(4) || !scan.has_binding(5) ||
             !scan.has_array_stride(4) ||
             !scan.has_array_stride(40) || !scan.has_array_stride(48))) {
            vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
            throw VulkanRuntimeError("SPIR-V alignment CIGAR extension layout mismatch");
        }

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv.size() * sizeof(std::uint32_t);
        module_info.pCode = spirv.data();
        check_vk(vkCreateShaderModule(device_, &module_info, nullptr, &entry.module),
                 "vkCreateShaderModule(phase_b)");
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = entry.module;
    stage.pName = entry.entry_name.c_str();
    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = entry.pipeline_layout;
    const VkResult pr = vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &entry.pipeline);
    if (pr != VK_SUCCESS) {
        vkDestroyShaderModule(device_, entry.module, nullptr);
        vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
        vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
        throw VulkanRuntimeError(fmt::format(
            "vkCreateComputePipelines(compute): VkResult {}", static_cast<int>(pr)));
    }

    bioseq_log<DBG>("[vulkan_runtime] built compute pipeline kind={} local={} max_span={}",
                    static_cast<int>(kind), local_size, max_span);
    phase_b_pipelines_.push_back(entry);
    return phase_b_pipelines_.back();
}

void Runtime::bind_phase_b_descriptors_locked(
    const PhaseBPipeline& entry, std::span<const VkBuffer> buffers)
{
    DEBUG_ASSERT(buffers.size() == entry.binding_count,
                 "phase B descriptor binding count mismatch");
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (std::size_t b = 0; b < buffers.size(); ++b) {
        infos[b] = VkDescriptorBufferInfo{buffers[b], 0, VK_WHOLE_SIZE};
        writes[b] = VkWriteDescriptorSet{};
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = entry.descriptor_set;
        writes[b].dstBinding = static_cast<std::uint32_t>(b);
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

SeedLookupResult Runtime::run_seed_lookup_roundtrip(
    RuntimeSeedLookupInputs inputs,
    std::span<const std::uint32_t> prefix_counts,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::uint64_t exact_anchor_count)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_seed_lookup_roundtrip on unavailable device");
    }
    if (prefix_counts.size() < 2 || exact_anchor_count == 0) {
        return SeedLookupResult{};
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw VulkanRuntimeError("seed lookup missing resident index buffers");
    }
    const std::uint64_t minimizer_count = prefix_counts.size() - 1u;
    constexpr std::uint32_t seed_local = 128u;
    const std::uint64_t seed_groups =
        (minimizer_count + seed_local - 1u) / seed_local;
    if (seed_groups > features_.max_workgroup_count_x) {
        throw VulkanRuntimeError("seed lookup dispatch exceeds maxComputeWorkGroupCount[0]");
    }

    const PhaseBPipeline seed_pipe =
        get_phase_b_pipeline_locked(ShaderKind::seed_lookup_anchors, seed_local, 256u);

    Buffer scratch = create_host_buffer(4u, kPhaseBBufferUsage);

    Buffer uploaded_min_words;
    const Buffer* min_words = inputs.device_minimizer_words;
    if (min_words == nullptr) {
        const std::uint64_t min_bytes = std::max<std::uint64_t>(
            minimizer_device_bytes(inputs.minimizer_word_kind,
                                   inputs.minimizer_words32_per_record, minimizer_count), 4u);
        uploaded_min_words = create_device_buffer(min_bytes, kPhaseBBufferUsage);
        if (uploaded_min_words.mapped != nullptr) {
            std::memset(uploaded_min_words.mapped, 0,
                        static_cast<std::size_t>(uploaded_min_words.bytes));
        }
        if (!inputs.host_minimizer_bytes.empty()) {
            upload_locked(scratch, uploaded_min_words, inputs.host_minimizer_bytes,
                          VK_ACCESS_SHADER_READ_BIT);
        }
        min_words = &uploaded_min_words;
    }

    Buffer prefix_buf = create_device_buffer(
        std::max<std::uint64_t>(prefix_counts.size() * sizeof(std::uint32_t), 4u),
        kPhaseBBufferUsage);
    upload_locked(scratch, prefix_buf, std::as_bytes(prefix_counts), VK_ACCESS_SHADER_READ_BIT);

    const std::uint64_t anchor_bytes = exact_anchor_count * std::uint64_t{24};
    Buffer anchors_buf = create_device_buffer(
        std::max<std::uint64_t>(anchor_bytes, 24u), kPhaseBBufferUsage);

    const std::uint32_t sort_capacity = gpu_anchor_sort_capacity_;
    const bool gpu_sort = sort_capacity > 0 && exact_anchor_count <= sort_capacity;
    if (!gpu_sort) {
        throw VulkanRuntimeError(fmt::format(
            "Vulkan seed lookup selected but anchor count {} exceeds GPU sort capacity {}",
            exact_anchor_count, sort_capacity));
    }
    Buffer ranges_buf;
    std::optional<PhaseBPipeline> sort_pipe;
    ranges_buf = create_device_buffer(sizeof(SortRangeWire), kPhaseBBufferUsage);
    const SortRangeWire range{0u, static_cast<std::uint32_t>(exact_anchor_count)};
    upload_locked(scratch, ranges_buf,
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(&range), sizeof(range)},
        VK_ACCESS_SHADER_READ_BIT);
    sort_pipe = get_phase_b_pipeline_locked(
        ShaderKind::sort_anchors_bitonic, sort_capacity, 256u);

    {
        const std::array<VkBuffer, 5> seed_bufs{
            min_words->buffer, inputs.slots->buffer, inputs.positions->buffer,
            prefix_buf.buffer, anchors_buf.buffer};
        bind_phase_b_descriptors_locked(seed_pipe, seed_bufs);
        if (gpu_sort) {
            const std::array<VkBuffer, 2> sort_bufs{anchors_buf.buffer, ranges_buf.buffer};
            bind_phase_b_descriptors_locked(*sort_pipe, sort_bufs);
        }
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(seed)");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host_to_shader,
                         0, nullptr, 0, nullptr);

    const SeedLookupPush seed_push{
        static_cast<std::uint32_t>(minimizer_count), inputs.minimizer_words32_per_record,
        inputs.minimizer_word_kind, inputs.hash_shift, inputs.pos_bits,
        static_cast<std::uint32_t>(query_len), kmer_size, inputs.slot_mask,
        static_cast<std::uint32_t>(exact_anchor_count)};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, seed_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            seed_pipe.pipeline_layout, 0, 1, &seed_pipe.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer_, seed_pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(seed_push), &seed_push);
    vkCmdDispatch(command_buffer_, static_cast<std::uint32_t>(seed_groups), 1, 1);

    if (gpu_sort) {
        VkMemoryBarrier shader_to_shader{};
        shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        shader_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &shader_to_shader,
                             0, nullptr, 0, nullptr);
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, sort_pipe->pipeline);
        vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                sort_pipe->pipeline_layout, 0, 1, &sort_pipe->descriptor_set, 0, nullptr);
        vkCmdDispatch(command_buffer_, 1, 1, 1);
    }

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shader_to_host,
                         0, nullptr, 0, nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(seed)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(seed)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(seed)");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(seed)");

    SeedLookupResult result;
    result.count = exact_anchor_count;
    result.host_bytes.resize(static_cast<std::size_t>(anchor_bytes));
    download_locked(scratch, anchors_buf, std::span<std::byte>{
        result.host_bytes.data(), result.host_bytes.size()});

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// ===========================================================================
// DOC0310 Phase C — arena + frontend implementation
// ===========================================================================

namespace {

constexpr VkBufferUsageFlags kPhaseCArenaUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

// Tile span (tile_bp + overlap) must fit the cooperative sketch's shared
// candidate arrays. The sketch runs one workgroup per tile with this local size;
// all lanes hash candidates in parallel into shared memory.
constexpr std::uint32_t kPhaseCSketchTileBp = 256u;
constexpr std::uint32_t kPhaseCSketchLocal = 64u;
constexpr std::uint32_t kPhaseCSketchTileSpanMax = 384u;
constexpr std::uint32_t kPhaseCScanBlock = 256u;
constexpr std::uint32_t kPhaseCSlotOffsetAlign = 256u;

using TileC = PhaseCSketchTileWire;
static_assert(sizeof(TileC) == 24);

struct SketchPushC {
    std::uint32_t emit_mode, window_size, kmer_size, hash_bits, hash_shift, pos_bits;
    std::uint32_t word_kind, words32_per_minimizer, span_value, alphabet_kind;
    std::uint32_t unit_bits, bits_per_char, letters_per_unit, char_mask;
    std::uint32_t tile_count, emit_pairs, query_gate_mask, query_gate_word_count;
};
static_assert(sizeof(SketchPushC) == 72);

struct ScanPushC {
    std::uint32_t element_count, block_size, num_blocks, pass;
};
static_assert(sizeof(ScanPushC) == 16);

struct SeedPushC {
    std::uint32_t words32_per_minimizer, word_kind, hash_shift, pos_bits, kmer_size;
    std::uint32_t slot_mask, max_occ, emit_mode, record_count, bucket_count;
    std::uint32_t target_pos_offset_lo, target_pos_offset_hi, target_ref_bits;
};
static_assert(sizeof(SeedPushC) == 52);

struct PhaseCAnchorSortKeyPlan {
    std::uint32_t target_ref_bits = 63u;
    std::uint32_t pass_count = 8u;
};

[[nodiscard]] PhaseCAnchorSortKeyPlan phase_c_anchor_sort_key_plan(
    std::uint64_t target_len,
    std::uint64_t target_position_offset) noexcept
{
    constexpr std::uint64_t kFirstUncompactCoordinate = std::uint64_t{1} << 63;
    if (target_len == 0u) return PhaseCAnchorSortKeyPlan{};

    const std::uint64_t last_local_coordinate = target_len - 1u;
    if (last_local_coordinate > std::numeric_limits<std::uint64_t>::max()
        - target_position_offset) {
        return PhaseCAnchorSortKeyPlan{};
    }

    const std::uint64_t max_target_coordinate =
        target_position_offset + last_local_coordinate;
    if (max_target_coordinate >= kFirstUncompactCoordinate) {
        return PhaseCAnchorSortKeyPlan{};
    }

    const std::uint32_t target_ref_bits = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(std::bit_width(max_target_coordinate)));
    return PhaseCAnchorSortKeyPlan{
        .target_ref_bits = target_ref_bits,
        .pass_count = std::clamp<std::uint32_t>((target_ref_bits + 8u) / 8u, 1u, 8u),
    };
}

struct SortPushC {
    std::uint32_t element_count, radix_shift, num_workgroups, elems_per_wg, value_words;
};
static_assert(sizeof(SortPushC) == 20);

struct IndexPushC {
    std::uint32_t element_count, slot_mask, slot_capacity, num_buckets, probe_cap, pass;
};
static_assert(sizeof(IndexPushC) == 24);

struct HostQRec {
    std::uint32_t min_offset, min_count, query_len, flags;
};
static_assert(sizeof(HostQRec) == 16);

/// Full compute→compute memory barrier between dependent Phase C dispatches.
void record_compute_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void record_host_to_shader(VkCommandBuffer cmd) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void record_shader_to_host(VkCommandBuffer cmd) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

[[nodiscard]] std::uint32_t ceil_div_u32(std::uint64_t n, std::uint32_t d) noexcept {
    return static_cast<std::uint32_t>((n + d - 1u) / d);
}

}  // namespace

Buffer Runtime::run_generate_units_roundtrip(
    std::uint32_t word_bits,
    std::span<const std::uint32_t> cdf,
    GenerateUnitsPush push,
    std::uint64_t out_block_bytes) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_generate_units_roundtrip on unavailable device");
    }

    constexpr std::uint32_t local = 256u;
    const PhaseBPipeline pipe =
        get_phase_b_pipeline_locked(ShaderKind::generate_units, local, word_bits);

    Buffer out = create_device_buffer(
        std::max<std::uint64_t>(out_block_bytes, 4u), kPhaseBBufferUsage);
    Buffer cdf_buf = create_device_buffer(
        std::max<std::uint64_t>(
            static_cast<std::uint64_t>(cdf.size()) * sizeof(std::uint32_t), 4u),
        kPhaseBBufferUsage);
    Buffer scratch = create_host_buffer(4u, kPhaseBBufferUsage);
    upload_locked(scratch, cdf_buf, std::as_bytes(cdf), VK_ACCESS_SHADER_READ_BIT);

    const std::array<VkBuffer, 2> buffers{out.buffer, cdf_buf.buffer};
    bind_phase_b_descriptors_locked(pipe, buffers);

    const std::uint64_t max_groups = features_.max_workgroup_count_x;
    const std::uint64_t tile_chunks = std::max<std::uint64_t>(1, max_groups * local);

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(gen)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(gen)");
    record_host_to_shader(command_buffer_);
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipe.pipeline_layout, 0, 1, &pipe.descriptor_set, 0, nullptr);
    for (std::uint64_t base = 0; base < push.total_chunks; base += tile_chunks) {
        push.chunk_base = static_cast<std::uint32_t>(base);
        const std::uint64_t n =
            std::min<std::uint64_t>(tile_chunks, push.total_chunks - base);
        const std::uint32_t groups =
            static_cast<std::uint32_t>((n + local - 1) / local);
        vkCmdPushConstants(command_buffer_, pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatch(command_buffer_, groups, 1, 1);
    }
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(gen)");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(gen)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(gen)");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(gen)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(gen-post)");
    if (push.total_chunks != 0) {
        generate_units_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

void Runtime::download_slice(
    const Buffer& src,
    std::uint64_t src_byte_offset,
    std::span<std::byte> dst) {
    if (dst.empty()) {
        return;
    }
    if (src.mapped == nullptr) {
        throw VulkanRuntimeError("download_slice: device buffer not host-visible");
    }
    if (src_byte_offset > src.bytes ||
        dst.size() > static_cast<std::size_t>(src.bytes - src_byte_offset)) {
        throw VulkanRuntimeError("download_slice: out of range");
    }
    DEBUG_ASSERT(src_byte_offset <= src.bytes && dst.size() <= src.bytes - src_byte_offset,
                 "download_slice: out of range");
    std::memcpy(
        dst.data(),
        static_cast<const std::byte*>(src.mapped) + src_byte_offset,
        dst.size());
}

// --- Arena -----------------------------------------------------------------

GpuArena& Runtime::phase_c_arena() {
    if (!phase_c_arena_) {
        phase_c_arena_.emplace(*this);
    }
    return *phase_c_arena_;
}

bool GpuArena::ensure_capacity(
    std::uint64_t device_bytes, std::uint64_t upload_bytes, std::uint64_t download_bytes)
{
    // Only called at op start (cursors already at their rewound baseline). Grows
    // a buffer geometrically when needed and resets just that buffer's cursor;
    // never grows mid-op (DOC0310 §7 forbids it while slices are live).
    try {
        if (device_bytes > device_capacity_) {
            const std::uint64_t want = std::max<std::uint64_t>(device_bytes,
                device_capacity_ == 0 ? device_bytes : device_capacity_ * 2u);
            device_ = runtime_->create_device_buffer(want, kPhaseCArenaUsage);
            if (device_.mapped == nullptr) return false;  // Phase C needs unified memory.
            device_capacity_ = device_.bytes;
            device_cursor_ = 0;
        }
        if (upload_bytes > upload_capacity_) {
            const std::uint64_t want = std::max<std::uint64_t>(upload_bytes,
                upload_capacity_ == 0 ? upload_bytes : upload_capacity_ * 2u);
            upload_ = runtime_->create_host_buffer(
                want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            upload_capacity_ = upload_.bytes;
            upload_cursor_ = 0;
        }
        if (download_bytes > download_capacity_) {
            const std::uint64_t want = std::max<std::uint64_t>(download_bytes,
                download_capacity_ == 0 ? download_bytes : download_capacity_ * 2u);
            download_ = runtime_->create_host_buffer(
                want, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            download_capacity_ = download_.bytes;
            download_cursor_ = 0;
        }
    } catch (const VulkanRuntimeError&) {
        return false;
    }
    return true;
}

GpuArenaSlice GpuArena::allocate_device(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t a = std::max<std::uint64_t>(alignment, kPhaseCSlotOffsetAlign);
    const std::uint64_t offset = align_up(device_cursor_, a);
    if (offset + bytes > device_capacity_) {
        return GpuArenaSlice{};  // caller checks .valid()
    }
    device_cursor_ = offset + bytes;
    return GpuArenaSlice{&device_, offset, bytes};
}

GpuArenaSlice GpuArena::allocate_upload(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t offset = align_up(upload_cursor_, std::max<std::uint64_t>(alignment, 16u));
    if (offset + bytes > upload_capacity_) return GpuArenaSlice{};
    upload_cursor_ = offset + bytes;
    return GpuArenaSlice{&upload_, offset, bytes};
}

GpuArenaSlice GpuArena::allocate_download(std::uint64_t bytes, std::uint64_t alignment) {
    const std::uint64_t offset = align_up(download_cursor_, std::max<std::uint64_t>(alignment, 16u));
    if (offset + bytes > download_capacity_) return GpuArenaSlice{};
    download_cursor_ = offset + bytes;
    return GpuArenaSlice{&download_, offset, bytes};
}

void GpuArena::bind_storage_slice(
    VkWriteDescriptorSet& write, VkDescriptorBufferInfo& info,
    std::uint32_t binding, VkDescriptorSet dst_set, const GpuArenaSlice& slice) const noexcept
{
    info = VkDescriptorBufferInfo{slice.buffer->buffer, slice.offset, slice.size};
    write = VkWriteDescriptorSet{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = dst_set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
}

// --- Phase C descriptor pool + dispatch recorder ---------------------------

void Runtime::ensure_phase_c_pool_locked() {
    if (phase_c_pool_ != VK_NULL_HANDLE) return;
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    size.descriptorCount = 4096u * 9u;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 4096u;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    check_vk(vkCreateDescriptorPool(device_, &info, nullptr, &phase_c_pool_),
             "vkCreateDescriptorPool(phase_c)");
}

void Runtime::reset_phase_c_pool_locked() {
    ensure_phase_c_pool_locked();
    check_vk(vkResetDescriptorPool(device_, phase_c_pool_, 0), "vkResetDescriptorPool(phase_c)");
}

VkDescriptorSet Runtime::alloc_phase_c_set_locked(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = phase_c_pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check_vk(vkAllocateDescriptorSets(device_, &info, &set), "vkAllocateDescriptorSets(phase_c)");
    return set;
}

void Runtime::bind_descriptors_offsets_locked(
    const PhaseBPipeline& entry, std::span<const VkDescriptorBufferInfo> infos)
{
    std::vector<VkWriteDescriptorSet> writes(infos.size());
    for (std::size_t b = 0; b < infos.size(); ++b) {
        writes[b] = VkWriteDescriptorSet{};
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = entry.descriptor_set;
        writes[b].dstBinding = static_cast<std::uint32_t>(b);
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void Runtime::record_phase_c_dispatch_locked(
    VkCommandBuffer cmd, const PhaseBPipeline& pipe,
    std::span<const VkDescriptorBufferInfo> infos,
    const void* push, std::uint32_t push_size, std::uint32_t group_count)
{
    if (group_count == 0) return;
    VkDescriptorSet set = alloc_phase_c_set_locked(pipe.set_layout);
    std::vector<VkWriteDescriptorSet> writes(infos.size());
    for (std::size_t b = 0; b < infos.size(); ++b) {
        writes[b] = VkWriteDescriptorSet{};
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = set;
        writes[b].dstBinding = static_cast<std::uint32_t>(b);
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipe.pipeline_layout, 0, 1, &set, 0, nullptr);
    if (push_size > 0 && push != nullptr) {
        vkCmdPushConstants(cmd, pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, push_size, push);
    }
    vkCmdDispatch(cmd, group_count, 1, 1);
}

void Runtime::record_radix_sort_locked(
    VkCommandBuffer cmd,
    const GpuArenaSlice& keys, const GpuArenaSlice& values,
    const GpuArenaSlice& keys_scratch, const GpuArenaSlice& values_scratch,
    const GpuArenaSlice& histogram,
    const GpuSortPlan& plan, std::uint32_t num_workgroups,
    std::uint32_t elems_per_wg, std::uint32_t value_words, bool& result_in_scratch)
{
    const PhaseBPipeline hist_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_sort_radix_histogram, 1, 1);
    const PhaseBPipeline scan_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_sort_radix_scan, 1, 1);
    const PhaseBPipeline scatter_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_sort_radix_scatter, 1, 1);

    bool in_scratch = false;
    for (std::uint32_t pass = 0; pass < plan.pass_count; ++pass) {
        const GpuArenaSlice& cur_keys = in_scratch ? keys_scratch : keys;
        const GpuArenaSlice& cur_vals = in_scratch ? values_scratch : values;
        const GpuArenaSlice& out_keys = in_scratch ? keys : keys_scratch;
        const GpuArenaSlice& out_vals = in_scratch ? values : values_scratch;

        SortPushC push{
            static_cast<std::uint32_t>(plan.element_count),
            pass * plan.radix_bits, num_workgroups, elems_per_wg, value_words};

        const std::array<VkDescriptorBufferInfo, 5> infos{{
            {cur_keys.buffer->buffer, cur_keys.offset, cur_keys.size},
            {cur_vals.buffer->buffer, cur_vals.offset, cur_vals.size},
            {out_keys.buffer->buffer, out_keys.offset, out_keys.size},
            {out_vals.buffer->buffer, out_vals.offset, out_vals.size},
            {histogram.buffer->buffer, histogram.offset, histogram.size},
        }};
        record_phase_c_dispatch_locked(cmd, hist_pipe, infos, &push, sizeof(push), num_workgroups);
        record_compute_barrier(cmd);
        record_phase_c_dispatch_locked(cmd, scan_pipe, infos, &push, sizeof(push), 1);
        record_compute_barrier(cmd);
        record_phase_c_dispatch_locked(cmd, scatter_pipe, infos, &push, sizeof(push), num_workgroups);
        record_compute_barrier(cmd);
        in_scratch = !in_scratch;
    }
    result_in_scratch = in_scratch;
}

void Runtime::record_stable_pair_sort_by_value_then_key_locked(
    VkCommandBuffer command_buffer,
    SortedPairDeviceViews& views,
    GpuArenaSlice hist,
    GpuSortPlan value_sort_plan,
    GpuSortPlan key_sort_plan,
    std::uint32_t num_workgroups,
    std::uint32_t elems_per_workgroup)
{
    bool value_sorted_in_scratch = false;
    record_radix_sort_locked(
        command_buffer,
        views.values,
        views.keys,
        views.values_scratch,
        views.keys_scratch,
        hist,
        value_sort_plan,
        num_workgroups,
        elems_per_workgroup,
        1u,
        value_sorted_in_scratch);

    record_compute_barrier(command_buffer);

    const GpuArenaSlice key_after_value_sort =
        value_sorted_in_scratch ? views.keys_scratch : views.keys;
    const GpuArenaSlice value_after_value_sort =
        value_sorted_in_scratch ? views.values_scratch : views.values;
    const GpuArenaSlice key_scratch_after_value_sort =
        value_sorted_in_scratch ? views.keys : views.keys_scratch;
    const GpuArenaSlice value_scratch_after_value_sort =
        value_sorted_in_scratch ? views.values : views.values_scratch;

    bool key_sorted_in_scratch = false;
    record_radix_sort_locked(
        command_buffer,
        key_after_value_sort,
        value_after_value_sort,
        key_scratch_after_value_sort,
        value_scratch_after_value_sort,
        hist,
        key_sort_plan,
        num_workgroups,
        elems_per_workgroup,
        1u,
        key_sorted_in_scratch);

    views.keys = key_after_value_sort;
    views.values = value_after_value_sort;
    views.keys_scratch = key_scratch_after_value_sort;
    views.values_scratch = value_scratch_after_value_sort;
    views.keys_in_scratch = key_sorted_in_scratch;
}

namespace {

/// Pick (num_workgroups, elems_per_wg) for a device sort/scan over `n` elements,
/// keeping the workgroup count under the device dispatch limit.
struct WgPlan { std::uint32_t num_workgroups; std::uint32_t elems_per_wg; };
[[nodiscard]] WgPlan plan_workgroups(std::uint64_t n, std::uint32_t max_wg) {
    if (n == 0) return WgPlan{0u, 1u};
    // Small per-workgroup chunks maximise concurrency for the 1-lane histogram
    // and stable scatter; the parallel scan keeps the per-bin reduction cheap
    // even with many workgroups.
    std::uint32_t elems_per_wg = 512u;
    std::uint32_t num_wg = ceil_div_u32(n, elems_per_wg);
    if (num_wg > max_wg) {
        num_wg = max_wg;
        elems_per_wg = ceil_div_u32(n, num_wg);
    }
    return WgPlan{num_wg, elems_per_wg};
}

/// Next power-of-two open-addressed slot capacity for `distinct` buckets,
/// matching `indexing::gpu_slot_capacity` (load factor < 0.5).
[[nodiscard]] std::uint32_t phase_c_slot_capacity(std::uint64_t distinct) {
    const std::uint64_t wanted = sat_add(bioseq::parallelism::saturating_mul(distinct, 2u), 1u);
    std::uint64_t cap = 2;
    while (cap < wanted) cap <<= 1;
    return static_cast<std::uint32_t>(cap);
}

}  // namespace

RuntimeSketchResult Runtime::run_phase_c_sketch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::uint64_t max_minimizers,
    std::span<const std::uint32_t> query_gate_words,
    std::uint32_t query_gate_mask)
{
    std::lock_guard<std::mutex> guard(mu_);
    RuntimeSketchResult result;
    if (!features_.available) {
        throw VulkanRuntimeError("run_phase_c_sketch_roundtrip on unavailable device");
    }
    const std::uint32_t bp = params.bp_size;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    if (bp == 0u || w == 0u || k == 0u || bp < k) { result.count = 0; return result; }
    if (query_gate_words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw VulkanRuntimeError("phase_c sketch query gate exceeds uint32 word count");
    }
    const std::uint32_t query_gate_word_count =
        static_cast<std::uint32_t>(query_gate_words.size());
    const std::uint64_t query_gate_bytes =
        static_cast<std::uint64_t>(query_gate_words.size()) * sizeof(std::uint32_t);

    const std::uint64_t out_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            params.minimizer_word_kind,
            params.minimizer_words32,
            max_minimizers),
        4u);
    const PhaseCSketchMemoryPlan memory_plan = phase_c_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(VulkanRankLut),
        max_minimizers,
        out_bytes,
        PhaseCSketchUse::minimizer_words,
        k,
        w,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.valid) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c sketch memory plan invalid (bp={}, max_min={}, lpu={})",
            bp,
            max_minimizers,
            params.letters_per_unit));
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    // DOC0353: the sketch kernel grid-strides one invocation per tile, so the
    // dispatch is ceil(num_tiles / local) workgroups CLAMPED to the device limit
    // (the shader loop covers any tiles beyond the clamped grid). No throw — a
    // window larger than 64 * max_workgroup_count_x tiles is still processed
    // correctly, just over more grid-stride steps.
    const std::uint32_t sketch_groups = std::min(
        ceil_div_u32(num_tiles, kPhaseCSketchLocal),
        features_.max_workgroup_count_x);
    const std::uint32_t overlap = phase_c_replay_overlap_bp(k, w);
    std::vector<TileC> tiles(num_tiles);
    for (std::uint32_t t = 0; t < num_tiles; ++t) {
        const std::uint32_t lo = t * tile_bp;
        const std::uint32_t hi = std::min(bp, (t + 1u) * tile_bp);
        const std::uint32_t rlo = lo > overlap ? lo - overlap : 0u;
        tiles[t] = TileC{params.seq_word_offset, bp, rlo, lo, hi, 0u};
    }
    const std::uint32_t num_blocks = memory_plan.tiles.scan_block_count;
    Buffer out_words = create_device_buffer(out_bytes, kPhaseBBufferUsage);
    if (out_words.mapped == nullptr) throw VulkanRuntimeError("phase_c sketch non-unified memory");

    GpuArena& arena = phase_c_arena();
    const std::uint64_t arena_bytes = sat_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : sat_add(query_gate_bytes, std::uint64_t{kPhaseCSlotOffsetAlign}));
    if (!arena.ensure_capacity(arena_bytes, 0, 0)) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c arena grow failed (bp={}, arena_bytes={})",
            bp,
            arena_bytes));
    }
    const GpuArenaMark mark = arena.mark();

    const GpuArenaSlice seq_s = arena.allocate_device(memory_plan.seq_bytes, 16u);
    const GpuArenaSlice lut_s = arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const GpuArenaSlice tiles_s = arena.allocate_device(memory_plan.tile_bytes, 16u);
    const GpuArenaSlice counts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const GpuArenaSlice offsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const GpuArenaSlice blocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const GpuArenaSlice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    const GpuArenaSlice pairvals_s =
        arena.allocate_device(memory_plan.arena_pair_value_bytes, 16u);
    const GpuArenaSlice query_gate_s = query_gate_bytes == 0u
        ? GpuArenaSlice{}
        : arena.allocate_device(query_gate_bytes, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid() || !counts_s.valid()
        || !offsets_s.valid() || !blocks_s.valid() || !counters_s.valid()
        || !pairvals_s.valid() || (query_gate_bytes != 0u && !query_gate_s.valid())) {
        arena.rewind(mark);
        throw VulkanRuntimeError(fmt::format(
            "phase_c sketch arena oversub (bp={} arena_bytes={} num_tiles={})",
            bp,
            arena_bytes,
            num_tiles));
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(TileC));
    std::memset(base + counts_s.offset, 0, static_cast<std::size_t>(counts_s.size));
    std::memset(base + counters_s.offset, 0, 8);
    if (query_gate_bytes != 0u) {
        std::memcpy(base + query_gate_s.offset, query_gate_words.data(),
                    static_cast<std::size_t>(query_gate_bytes));
    }
    if (params.minimizer_word_kind == 0u) std::memset(out_words.mapped, 0, static_cast<std::size_t>(out_words.bytes));

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_sketch_tile, kPhaseCSketchLocal, std::max<std::uint32_t>(w, 1u));
    const PhaseBPipeline scan_block_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scan_add_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
    reset_phase_c_pool_locked();

    const GpuArenaSlice& gate_binding_s =
        query_gate_bytes == 0u ? counters_s : query_gate_s;
    const std::array<VkDescriptorBufferInfo, 9> sketch_infos{{
        {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        {out_words.buffer, 0, VK_WHOLE_SIZE},
        {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer, pairvals_s.offset, pairvals_s.size},
        {gate_binding_s.buffer->buffer, gate_binding_s.offset, gate_binding_s.size},
    }};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {blocks_s.buffer->buffer, blocks_s.offset, blocks_s.size},
    }};

    SketchPushC push{
        0u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
        params.minimizer_word_kind, params.minimizer_words32, k, params.alphabet_kind,
        params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
        num_tiles, 0u, query_gate_mask, query_gate_word_count};
    ScanPushC scan0{num_tiles, kPhaseCScanBlock, num_blocks, 0u};
    ScanPushC scan1{num_tiles, kPhaseCScanBlock, num_blocks, 1u};

    static const auto* _qsketch_d = bioseq::instr::register_region_static("phase_c_query_sketch");
    auto _qsketch_s = bioseq::instr::region(_qsketch_d);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(pc_sketch)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_block_pipe, scan_infos, &scan0, sizeof(scan0), num_blocks);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_add_pipe, scan_infos, &scan0, sizeof(scan0), 1u);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_add_pipe, scan_infos, &scan1, sizeof(scan1), ceil_div_u32(num_tiles, 128u));
    record_compute_barrier(command_buffer_);
    push.emit_mode = 1u;
    record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(pc_sketch)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(pc_sketch)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(pc_sketch)");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(pc_sketch)");

    std::uint32_t last_off = 0, last_cnt = 0;
    std::memcpy(&last_off, base + offsets_s.offset + std::uint64_t{num_tiles - 1u} * 4u, 4);
    std::memcpy(&last_cnt, base + counts_s.offset + std::uint64_t{num_tiles - 1u} * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(last_off) + last_cnt;

    if (total > max_minimizers) {
        result.overflow = true;
    } else {
        result.count = total;
        const std::uint64_t host_bytes = minimizer_host_bytes(params.minimizer_word_kind, total);
        result.host_minimizer_bytes.resize(static_cast<std::size_t>(host_bytes));
        if (host_bytes > 0) std::memcpy(result.host_minimizer_bytes.data(), out_words.mapped, static_cast<std::size_t>(host_bytes));
        result.minimizer_words = std::move(out_words);
    }
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

RuntimeSketchBatchResult Runtime::run_phase_c_sketch_tile_batch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::span<const PhaseCSketchTileWire> tiles,
    std::uint64_t max_minimizers)
{
    std::lock_guard<std::mutex> guard(mu_);
    RuntimeSketchBatchResult result;
    if (!features_.available) {
        throw VulkanRuntimeError(
            "run_phase_c_sketch_tile_batch_roundtrip on unavailable device");
    }
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    if (tiles.empty() || w == 0u || k == 0u || max_minimizers == 0u) {
        return result;
    }
    if (tiles.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw VulkanRuntimeError("phase_c sketch tile batch exceeds uint32 tile count");
    }
    if (max_minimizers
        > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw VulkanRuntimeError("phase_c sketch tile batch capacity exceeds ABI");
    }

    const std::uint32_t tile_count = static_cast<std::uint32_t>(tiles.size());
    const std::uint32_t num_blocks =
        std::max<std::uint32_t>(ceil_div_u32(tile_count, kPhaseCScanBlock), 1u);
    const std::uint32_t sketch_groups = std::min(
        ceil_div_u32(tile_count, kPhaseCSketchLocal),
        features_.max_workgroup_count_x);
    if (sketch_groups == 0u) {
        return result;
    }

    const std::uint64_t out_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(
            params.minimizer_word_kind,
            params.minimizer_words32,
            max_minimizers),
        4u);
    auto add_aligned =
        [] (std::uint64_t total, std::uint64_t bytes) -> std::uint64_t {
        return sat_add(
            total,
            phase_c_align_up(
                std::max<std::uint64_t>(bytes, 4u),
                kPhaseCSlotOffsetAlign));
    };
    std::uint64_t arena_bytes = 0u;
    arena_bytes = add_aligned(arena_bytes, packed_sequence.size());
    arena_bytes = add_aligned(arena_bytes, sizeof(VulkanRankLut));
    arena_bytes = add_aligned(
        arena_bytes,
        static_cast<std::uint64_t>(tiles.size()) * sizeof(PhaseCSketchTileWire));
    arena_bytes = add_aligned(arena_bytes, static_cast<std::uint64_t>(tile_count) * 4u);
    arena_bytes = add_aligned(arena_bytes, static_cast<std::uint64_t>(tile_count) * 4u);
    arena_bytes = add_aligned(arena_bytes, static_cast<std::uint64_t>(num_blocks) * 4u);
    arena_bytes = add_aligned(arena_bytes, 8u);
    arena_bytes = add_aligned(arena_bytes, 16u);

    Buffer out_words = create_device_buffer(out_bytes, kPhaseBBufferUsage);
    if (out_words.mapped == nullptr) {
        throw VulkanRuntimeError("phase_c sketch tile batch non-unified output memory");
    }

    GpuArena& arena = phase_c_arena();
    if (!arena.ensure_capacity(arena_bytes, 0u, 0u)) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c sketch tile batch arena grow failed (tiles={}, arena_bytes={})",
            tile_count,
            arena_bytes));
    }
    const GpuArenaMark mark = arena.mark();

    const GpuArenaSlice seq_s = arena.allocate_device(
        std::max<std::uint64_t>(packed_sequence.size(), 4u), 16u);
    const GpuArenaSlice lut_s =
        arena.allocate_device(sizeof(VulkanRankLut), 16u);
    const GpuArenaSlice tiles_s = arena.allocate_device(
        static_cast<std::uint64_t>(tiles.size()) * sizeof(PhaseCSketchTileWire),
        16u);
    const GpuArenaSlice counts_s =
        arena.allocate_device(static_cast<std::uint64_t>(tile_count) * 4u, 16u);
    const GpuArenaSlice offsets_s =
        arena.allocate_device(static_cast<std::uint64_t>(tile_count) * 4u, 16u);
    const GpuArenaSlice blocks_s =
        arena.allocate_device(static_cast<std::uint64_t>(num_blocks) * 4u, 16u);
    const GpuArenaSlice counters_s = arena.allocate_device(8u, 16u);
    const GpuArenaSlice pairvals_s = arena.allocate_device(16u, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid()
        || !counts_s.valid() || !offsets_s.valid() || !blocks_s.valid()
        || !counters_s.valid() || !pairvals_s.valid()) {
        arena.rewind(mark);
        throw VulkanRuntimeError(fmt::format(
            "phase_c sketch tile batch arena oversub (tiles={} arena_bytes={})",
            tile_count,
            arena_bytes));
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    std::memset(base + seq_s.offset, 0, static_cast<std::size_t>(seq_s.size));
    if (!packed_sequence.empty()) {
        std::memcpy(
            base + seq_s.offset,
            packed_sequence.data(),
            packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(
        base + tiles_s.offset,
        tiles.data(),
        tiles.size() * sizeof(PhaseCSketchTileWire));
    std::memset(base + counts_s.offset, 0, static_cast<std::size_t>(counts_s.size));
    std::memset(base + offsets_s.offset, 0, static_cast<std::size_t>(offsets_s.size));
    std::memset(base + blocks_s.offset, 0, static_cast<std::size_t>(blocks_s.size));
    std::memset(base + counters_s.offset, 0, static_cast<std::size_t>(counters_s.size));
    if (params.minimizer_word_kind == 0u) {
        std::memset(out_words.mapped, 0, static_cast<std::size_t>(out_words.bytes));
    }

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_sketch_tile,
        kPhaseCSketchLocal,
        std::max<std::uint32_t>(w, 1u));
    const PhaseBPipeline scan_block_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_prefix_scan_block,
        1u,
        1u);
    const PhaseBPipeline scan_add_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_prefix_scan_add,
        128u,
        1u);
    reset_phase_c_pool_locked();

    const std::array<VkDescriptorBufferInfo, 9> sketch_infos{{
        {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        {out_words.buffer, 0, VK_WHOLE_SIZE},
        {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer, pairvals_s.offset, pairvals_s.size},
        {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
    }};
    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {blocks_s.buffer->buffer, blocks_s.offset, blocks_s.size},
    }};

    SketchPushC push{
        0u,
        w,
        k,
        params.hash_bits,
        params.hash_shift,
        params.pos_bits,
        params.minimizer_word_kind,
        params.minimizer_words32,
        k,
        params.alphabet_kind,
        params.unit_bits,
        params.bits_per_char,
        params.letters_per_unit,
        params.char_mask,
        tile_count,
        0u,
        0u,
        0u};
    ScanPushC scan0{tile_count, kPhaseCScanBlock, num_blocks, 0u};
    ScanPushC scan1{tile_count, kPhaseCScanBlock, num_blocks, 1u};

    static const auto* _qsketch_d =
        bioseq::instr::register_region_static("phase_c_query_sketch_batch");
    auto _qsketch_s = bioseq::instr::region(_qsketch_d);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(pc_sketch_batch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(
        vkBeginCommandBuffer(command_buffer_, &begin),
        "vkBeginCommandBuffer(pc_sketch_batch)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, scan_block_pipe, scan_infos, &scan0, sizeof(scan0), num_blocks);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, scan_add_pipe, scan_infos, &scan0, sizeof(scan0), 1u);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, scan_add_pipe, scan_infos, &scan1, sizeof(scan1),
        ceil_div_u32(tile_count, 128u));
    record_compute_barrier(command_buffer_);
    push.emit_mode = 1u;
    record_phase_c_dispatch_locked(
        command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), sketch_groups);
    record_shader_to_host(command_buffer_);
    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(pc_sketch_batch)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(pc_sketch_batch)");
    check_vk(
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX),
        "vkWaitForFences(pc_sketch_batch)");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(pc_sketch_batch)");

    result.tile_counts.resize(tiles.size());
    result.tile_offsets.resize(tiles.size());
    std::memcpy(
        result.tile_counts.data(),
        base + counts_s.offset,
        result.tile_counts.size() * sizeof(std::uint32_t));
    std::memcpy(
        result.tile_offsets.data(),
        base + offsets_s.offset,
        result.tile_offsets.size() * sizeof(std::uint32_t));
    const std::uint64_t total =
        static_cast<std::uint64_t>(result.tile_offsets.back())
        + static_cast<std::uint64_t>(result.tile_counts.back());

    if (total > max_minimizers) {
        result.overflow = true;
    } else {
        result.count = total;
        const std::uint64_t host_bytes =
            minimizer_host_bytes(params.minimizer_word_kind, total);
        result.host_minimizer_bytes.resize(static_cast<std::size_t>(host_bytes));
        if (host_bytes > 0u) {
            std::memcpy(
                result.host_minimizer_bytes.data(),
                out_words.mapped,
                static_cast<std::size_t>(host_bytes));
        }
        result.minimizer_words = std::move(out_words);
    }
    arena.rewind(mark);
    check_vk(
        vkResetCommandBuffer(command_buffer_, 0),
        "vkResetCommandBuffer(pc_sketch_batch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

namespace {

/// Submit the currently-recorded command buffer and wait. Centralises the
/// begin/end/submit/wait/reset boilerplate for the Phase C multi-submit ops.
void submit_and_wait(VkDevice device, VkQueue queue, VkCommandBuffer cmd, VkFence fence,
                     const char* what) {
    check_vk(vkEndCommandBuffer(cmd), what);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check_vk(vkQueueSubmit(queue, 1, &submit, fence), what);
    check_vk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), what);
    check_vk(vkResetFences(device, 1, &fence), what);
}

}  // namespace

GpuBuiltIndex Runtime::run_phase_c_build_target_index(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::uint64_t max_target_minimizers,
    std::span<const std::uint32_t> query_gate_words,
    std::uint32_t query_gate_mask)
{
    std::lock_guard<std::mutex> guard(mu_);
    static const auto* _run_d = bioseq::instr::register_region_static("phase_c_index_run");
    auto _run_s = bioseq::instr::region(_run_d);
    GpuBuiltIndex out;
    if (!features_.available) throw VulkanRuntimeError("run_phase_c_build_target_index on unavailable device");
    const std::uint32_t bp = params.bp_size;
    const std::uint32_t w = params.window_size;
    const std::uint32_t k = params.kmer_size;
    out.target_len = bp;
    if (bp == 0u || w == 0u || k == 0u || bp < k || max_target_minimizers == 0u) {
        return out;  // empty/invalid shape; selected GPU callers throw.
    }
    if (max_target_minimizers > std::numeric_limits<std::uint32_t>::max()) return out;
    if (query_gate_words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw VulkanRuntimeError("phase_c index query gate exceeds uint32 word count");
    }
    const std::uint32_t query_gate_word_count =
        static_cast<std::uint32_t>(query_gate_words.size());
    const std::uint64_t query_gate_bytes =
        static_cast<std::uint64_t>(query_gate_words.size()) * sizeof(std::uint32_t);

    const PhaseCSketchMemoryPlan memory_plan = phase_c_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(VulkanRankLut),
        max_target_minimizers,
        0u,
        PhaseCSketchUse::target_index_build,
        k,
        w,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.valid) {
        return out;
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    // DOC0353: grid-strided sketch — clamp the dispatch to the device limit.
    const std::uint32_t sketch_groups = std::min(
        ceil_div_u32(num_tiles, kPhaseCSketchLocal),
        features_.max_workgroup_count_x);
    const std::uint32_t overlap = phase_c_replay_overlap_bp(k, w);
    std::vector<TileC> tiles(num_tiles);
    for (std::uint32_t t = 0; t < num_tiles; ++t) {
        const std::uint32_t lo = t * tile_bp;
        const std::uint32_t hi = std::min(bp, (t + 1u) * tile_bp);
        tiles[t] = TileC{params.seq_word_offset, bp, lo > overlap ? lo - overlap : 0u, lo, hi, 0u};
    }
    // Small per-call scaffolding lives in the shared arena (seq + lut + tiles +
    // tile counts/offsets). The large minimizer/sort/index arrays are sized from
    // the EXACT minimizer count (downloaded after the count pass) as standalone
    // device buffers — never a single 14 GB arena buffer that Metal cannot
    // allocate for the XXL_HIFI 500 Mbp target.
    GpuArena& arena = phase_c_arena();
    const std::uint64_t arena_bytes = sat_add(
        memory_plan.arena_scaffold_bytes,
        query_gate_bytes == 0u
            ? 0u
            : sat_add(query_gate_bytes, std::uint64_t{kPhaseCSlotOffsetAlign}));
    if (!arena.ensure_capacity(arena_bytes, 0, 0)) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c index arena grow failed (bp={}, arena_bytes={})",
            bp,
            arena_bytes));
    }
    const GpuArenaMark mark = arena.mark();
    const GpuArenaSlice seq_s = arena.allocate_device(memory_plan.seq_bytes, 16u);
    const GpuArenaSlice lut_s =
        arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const GpuArenaSlice tiles_s =
        arena.allocate_device(memory_plan.tile_bytes, 16u);
    const GpuArenaSlice tcounts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const GpuArenaSlice toffsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const GpuArenaSlice tblocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const GpuArenaSlice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    const GpuArenaSlice query_gate_s = query_gate_bytes == 0u
        ? GpuArenaSlice{}
        : arena.allocate_device(query_gate_bytes, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid() || !tcounts_s.valid()
        || !toffsets_s.valid() || !tblocks_s.valid() || !counters_s.valid()
        || (query_gate_bytes != 0u && !query_gate_s.valid())) {
        bioseq_log<ERR>("[phase_c index] scaffold arena oversubscribed (bp={}, num_tiles={}, "
                        "arena_bytes={}); returning invalid GPU index",
                        bp, num_tiles, arena_bytes);
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(TileC));
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));
    if (query_gate_bytes != 0u) {
        std::memcpy(base + query_gate_s.offset, query_gate_words.data(),
                    static_cast<std::size_t>(query_gate_bytes));
    }

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_sketch_tile, kPhaseCSketchLocal, std::max<std::uint32_t>(w, 1u));

    // Query-gated segment indexes usually retain only a tiny subset of target
    // minimizers. Count them on device first so the hash/position buffers and
    // subsequent radix sort are sized to the useful filtered count, not the
    // full target-segment capacity. Ungated full-target builds keep the original
    // single-pass capacity-sized emit to avoid an extra scan.
    const std::uint64_t cap = max_target_minimizers;
    std::uint64_t emit_capacity = cap;
    const GpuArenaSlice& gate_binding_s =
        query_gate_bytes == 0u ? counters_s : query_gate_s;
    if (query_gate_bytes != 0u) {
        {
            const std::array<std::uint32_t, 4> cinit{0u, 0u, 0u, 0u};
            std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
        }
        {
            static const auto* _rd =
                bioseq::instr::register_region_static("phase_c_target_sketch_count");
            auto _rs = bioseq::instr::region(_rd);
            reset_phase_c_pool_locked();
            const std::array<VkDescriptorBufferInfo, 9> infos{{
                {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
                {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
                {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
                {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},
                {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
                {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
                {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},
                {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
                {gate_binding_s.buffer->buffer, gate_binding_s.offset, gate_binding_s.size},
            }};
            SketchPushC push{3u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
                params.minimizer_word_kind, params.minimizer_words32, k, params.alphabet_kind,
                params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
                num_tiles, 1u, query_gate_mask, query_gate_word_count};
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_count)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_count)");
            record_host_to_shader(command_buffer_);
            record_phase_c_dispatch_locked(
                command_buffer_, sketch_pipe, infos, &push, sizeof(push), sketch_groups);
            record_shader_to_host(command_buffer_);
            submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_count)");
        }
        std::uint32_t filtered_count = 0;
        std::memcpy(&filtered_count, base + counters_s.offset, 4);
        if (filtered_count == 0u) {
            out.valid = true;
            out.occupied_slots = 0u;
            out.position_count = 0u;
            out.slot_mask = 0u;
            arena.rewind(mark);
            return out;
        }
        if (filtered_count > max_target_minimizers) {
            bioseq_log<ERR>("[phase_c idx] filtered tm={} out of range (cap={})",
                            filtered_count, max_target_minimizers);
            arena.rewind(mark);
            return out;
        }
        emit_capacity = filtered_count;
    }
    Buffer keys = create_device_buffer(emit_capacity * 8u, kPhaseBBufferUsage);   // out_words: u64 hash/min
    Buffer vals = create_device_buffer(emit_capacity * 8u, kPhaseBBufferUsage);   // out_pair_vals: u64 pos_strand/min
    if (keys.mapped == nullptr || vals.mapped == nullptr) {
        bioseq_log<ERR>("[phase_c index] capacity buffer alloc failed (cap={}); "
                        "returning invalid GPU index", emit_capacity);
        arena.rewind(mark);
        return out;
    }
    const GpuArenaSlice keys_s{&keys, 0, keys.bytes};
    const GpuArenaSlice vals_s{&vals, 0, vals.bytes};
    {
        const std::array<std::uint32_t, 4> cinit{
            0u, static_cast<std::uint32_t>(emit_capacity), 0u, 0u};
        std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
    }
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_target_sketch");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 9> infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},     // unused (count mode only)
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},             // out_words
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},  // unused (emit_mode 1 only)
            {vals_s.buffer->buffer, vals_s.offset, vals_s.size},             // out_pair_vals
            {gate_binding_s.buffer->buffer, gate_binding_s.offset, gate_binding_s.size},
        }};
        SketchPushC push{2u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
            params.minimizer_word_kind, params.minimizer_words32, k, params.alphabet_kind,
            params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
            num_tiles, 1u, query_gate_mask, query_gate_word_count};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_emit)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_emit)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, infos, &push, sizeof(push), sketch_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_emit)");
    }
    std::uint32_t emit_count = 0;
    std::memcpy(&emit_count, base + counters_s.offset, 4);   // counters[0] = exact minimizer count
    const std::uint64_t tm = emit_count;
    if (tm == 0u) {
        out.valid = true;
        out.occupied_slots = 0u;
        out.position_count = 0u;
        out.slot_mask = 0u;
        arena.rewind(mark);
        return out;
    }
    if (tm > emit_capacity) {
        bioseq_log<ERR>("[phase_c idx] tm={} out of range / overflow (emit_capacity={}, cap={})",
                        tm, emit_capacity, max_target_minimizers);
        arena.rewind(mark); return out;
    }

    // tm-sized scratch + index buffers, now that the exact count is known. keys
    // and vals (the emitted pairs) were allocated to either the query-gated
    // count or the ungated capacity bound above; the radix sort ping-pongs them
    // against tm-sized scratch.
    const WgPlan swg = plan_workgroups(tm, features_.max_workgroup_count_x);
    Buffer keys_scr, vals_scr, hist, flags, scanned, iblocks, buckets, bslot;
    {
        static const auto* _alloc_d = bioseq::instr::register_region_static("phase_c_index_alloc");
        auto _alloc_s = bioseq::instr::region(_alloc_d);
        keys_scr = create_device_buffer(tm * 8u, kPhaseBBufferUsage);
        vals_scr = create_device_buffer(tm * 8u, kPhaseBBufferUsage);
        hist = create_device_buffer(std::uint64_t{swg.num_workgroups} * 256u * 4u, kPhaseBBufferUsage);
        flags = create_device_buffer(tm * 4u, kPhaseBBufferUsage);
        scanned = create_device_buffer(tm * 4u, kPhaseBBufferUsage);
        iblocks = create_device_buffer(std::uint64_t{ceil_div_u32(tm, kPhaseCScanBlock)} * 4u, kPhaseBBufferUsage);
        buckets = create_device_buffer(tm * 16u, kPhaseBBufferUsage);
        bslot = create_device_buffer(tm * 4u, kPhaseBBufferUsage);
    }
    if (keys_scr.mapped == nullptr || vals_scr.mapped == nullptr || hist.mapped == nullptr
        || flags.mapped == nullptr || scanned.mapped == nullptr || iblocks.mapped == nullptr
        || buckets.mapped == nullptr || bslot.mapped == nullptr) {
        bioseq_log<ERR>("[phase_c index] scratch buffer alloc failed (tm={}, num_wg={}); "
                        "returning invalid GPU index",
                        tm, swg.num_workgroups);
        arena.rewind(mark);
        return out;
    }
    const GpuArenaSlice keys_scr_s{&keys_scr, 0, keys_scr.bytes};
    const GpuArenaSlice vals_scr_s{&vals_scr, 0, vals_scr.bytes};
    const GpuArenaSlice hist_s{&hist, 0, hist.bytes};
    const GpuArenaSlice flags_s{&flags, 0, flags.bytes};
    const GpuArenaSlice scanned_s{&scanned, 0, scanned.bytes};
    const GpuArenaSlice iblocks_s{&iblocks, 0, iblocks.bytes};
    const GpuArenaSlice buckets_s{&buckets, 0, buckets.bytes};
    const GpuArenaSlice bslot_s{&bslot, 0, bslot.bytes};

    // --- Submit 2: radix sort pairs by hash, mark runs, scan run-starts ---
    // Only sort the significant hash bytes: for K=15 dna4 the stored hash is
    // ~30 bits → 4 passes, not 8, roughly halving the sort.
    const std::uint32_t hash_passes = std::clamp<std::uint32_t>(
        (params.hash_bits + 7u) / 8u, 1u, 8u);
    GpuSortPlan sort_plan{tm, 8u, 8u, 8u, hash_passes};
    bool sorted_in_scratch = false;
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_index_sort");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx2)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx2)");
        record_radix_sort_locked(command_buffer_, keys_s, vals_s, keys_scr_s, vals_scr_s, hist_s,
                                 sort_plan, swg.num_workgroups, swg.elems_per_wg, 1u, sorted_in_scratch);
        const GpuArenaSlice& sorted_keys = sorted_in_scratch ? keys_scr_s : keys_s;
        const PhaseBPipeline mark_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_index_mark_runs, 128u, 1u);
        const std::array<VkDescriptorBufferInfo, 7> idx_infos{{
            {sorted_keys.buffer->buffer, sorted_keys.offset, sorted_keys.size},
            {flags_s.buffer->buffer, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer, scanned_s.offset, scanned_s.size},
            {buckets_s.buffer->buffer, buckets_s.offset, buckets_s.size},
            {bslot_s.buffer->buffer, bslot_s.offset, bslot_s.size},  // slot_claims placeholder
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},     // slots placeholder
            {bslot_s.buffer->buffer, bslot_s.offset, bslot_s.size},
        }};
        IndexPushC ipush{static_cast<std::uint32_t>(tm), 0u, 0u, 0u, 0u, 0u};
        record_phase_c_dispatch_locked(command_buffer_, mark_pipe, idx_infos, &ipush, sizeof(ipush), ceil_div_u32(tm, 128u));
        record_compute_barrier(command_buffer_);
        const PhaseBPipeline scanb = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
        const PhaseBPipeline scana = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
        const std::uint32_t inb = ceil_div_u32(tm, kPhaseCScanBlock);
        const std::array<VkDescriptorBufferInfo, 3> iscan{{
            {flags_s.buffer->buffer, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer, scanned_s.offset, scanned_s.size},
            {iblocks_s.buffer->buffer, iblocks_s.offset, iblocks_s.size},
        }};
        ScanPushC is0{static_cast<std::uint32_t>(tm), kPhaseCScanBlock, inb, 0u};
        ScanPushC is1{static_cast<std::uint32_t>(tm), kPhaseCScanBlock, inb, 1u};
        record_phase_c_dispatch_locked(command_buffer_, scanb, iscan, &is0, sizeof(is0), inb);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scana, iscan, &is0, sizeof(is0), 1u);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scana, iscan, &is1, sizeof(is1), ceil_div_u32(tm, 128u));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx2)");
    }
    std::uint32_t scan_last = 0, flag_last = 0;
    std::memcpy(&scan_last, static_cast<const std::byte*>(scanned.mapped) + (tm - 1u) * 4u, 4);
    std::memcpy(&flag_last, static_cast<const std::byte*>(flags.mapped) + (tm - 1u) * 4u, 4);
    const std::uint32_t num_buckets = scan_last + flag_last;
    if (num_buckets == 0u) {
        bioseq_log<ERR>("[phase_c idx] num_buckets=0 (tm={})", tm);
        arena.rewind(mark); return out;
    }

    // The dense bucket array is built sorted by hash (the pairs were radix-sorted
    // by hash), so the seed lookup binary-searches it directly — no open-addressed
    // slot table. Building a 134 M-slot table with 50 M atomicCompSwap probes
    // tripped the macOS GPU watchdog (VK_ERROR_DEVICE_LOST) at XXL_HIFI; the
    // sorted-bucket form removes that whole submit. `buckets` becomes the resident
    // index ("slots"); `positions` is the hash-run-ordered pos_strand array.
    // positions = the hash-run-ordered (sorted) pos_strand values. The sorted
    // values already live in `vals`/`vals_scr`; move that buffer straight into
    // the resident index instead of copying ~0.75 GB per build.
    {
        static const auto* _bd = bioseq::instr::register_region_static("phase_c_idx_bucketmem");
        auto _bs = bioseq::instr::region(_bd);
        std::memset(buckets.mapped, 0, static_cast<std::size_t>(std::uint64_t{num_buckets} * 16u));
    }

    // --- Submit 3: emit dense sorted buckets (single light dispatch). ---
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_index_slots");
        auto _rs = bioseq::instr::region(_rd);
        const GpuArenaSlice& sorted_keys = sorted_in_scratch ? keys_scr_s : keys_s;
        const std::array<VkDescriptorBufferInfo, 7> idx_infos{{
            {sorted_keys.buffer->buffer, sorted_keys.offset, sorted_keys.size},
            {flags_s.buffer->buffer, flags_s.offset, flags_s.size},
            {scanned_s.buffer->buffer, scanned_s.offset, scanned_s.size},
            {buckets_s.buffer->buffer, buckets_s.offset, buckets_s.size},
            {bslot_s.buffer->buffer, bslot_s.offset, bslot_s.size},   // unused placeholder
            {buckets_s.buffer->buffer, buckets_s.offset, buckets_s.size},  // unused slots placeholder
            {bslot_s.buffer->buffer, bslot_s.offset, bslot_s.size},
        }};
        IndexPushC ipush{static_cast<std::uint32_t>(tm), 0u, 0u, num_buckets, 0u, 0u};
        reset_phase_c_pool_locked();
        const PhaseBPipeline emit_b = get_phase_b_pipeline_locked(ShaderKind::phase_c_index_emit_buckets, 128u, 1u);
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx3)");
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx3)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, emit_b, idx_infos, &ipush, sizeof(ipush), ceil_div_u32(tm, 128u));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx3_buckets)");
    }

    out.slots = std::move(buckets);   // sorted-by-hash dense buckets = resident index
    out.positions = sorted_in_scratch ? std::move(vals_scr) : std::move(vals);  // sorted pos_strand
    out.slot_mask = 0u;               // 0 ⇒ seed binary-searches `occupied_slots` buckets
    out.occupied_slots = num_buckets;
    out.position_count = static_cast<std::uint32_t>(tm);
    out.valid = true;
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

GpuSortedPairRun Runtime::run_phase_c_sketch_sort_pairs_roundtrip(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::uint64_t max_target_minimizers)
{
    std::lock_guard<std::mutex> guard(mu_);
    static const auto* _run_d =
        bioseq::instr::register_region_static("phase_c_sorted_pair_run");
    auto _run_s = bioseq::instr::region(_run_d);

    GpuSortedPairRun out;
    if (!features_.available) {
        throw VulkanRuntimeError(
            "run_phase_c_sketch_sort_pairs_roundtrip on unavailable device");
    }

    const std::uint32_t bp = params.bp_size;
    const std::uint32_t window_size = params.window_size;
    const std::uint32_t kmer_size = params.kmer_size;
    out.target_len = bp;
    if (bp == 0u || window_size == 0u || kmer_size == 0u || bp < kmer_size
        || max_target_minimizers == 0u) {
        return out;
    }
    if (max_target_minimizers
        > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return out;
    }

    const PhaseCSketchMemoryPlan memory_plan = phase_c_sketch_memory_plan(
        bp,
        packed_sequence.size(),
        sizeof(VulkanRankLut),
        max_target_minimizers,
        0u,
        PhaseCSketchUse::sorted_pairs,
        kmer_size,
        window_size,
        params.letters_per_unit,
        features_.max_workgroup_count_x);
    if (!memory_plan.valid) {
        return out;
    }
    const std::uint32_t tile_bp = memory_plan.tiles.tile_bp;
    const std::uint32_t num_tiles = memory_plan.tiles.tile_count;
    if (num_tiles == 0u) {
        return out;
    }
    // DOC0353: grid-strided sketch — clamp the dispatch to the device limit.
    const std::uint32_t sketch_groups = std::min(
        ceil_div_u32(num_tiles, kPhaseCSketchLocal),
        features_.max_workgroup_count_x);

    const std::uint32_t overlap =
        phase_c_replay_overlap_bp(kmer_size, window_size);
    std::vector<TileC> tiles(num_tiles);
    for (std::uint32_t tile = 0; tile < num_tiles; ++tile) {
        const std::uint32_t lo = tile * tile_bp;
        const std::uint32_t hi = std::min(bp, (tile + 1u) * tile_bp);
        tiles[tile] = TileC{
            params.seq_word_offset,
            bp,
            lo > overlap ? lo - overlap : 0u,
            lo,
            hi,
            0u};
    }

    GpuArena& arena = phase_c_arena();
    if (!arena.ensure_capacity(memory_plan.arena_scaffold_bytes, 0u, 0u)) {
        throw VulkanRuntimeError(fmt::format(
            "phase_c sorted-pair arena grow failed (bp={}, arena_bytes={})",
            bp,
            memory_plan.arena_scaffold_bytes));
    }

    const GpuArenaMark mark = arena.mark();
    const GpuArenaSlice seq_s = arena.allocate_device(memory_plan.seq_bytes, 16u);
    const GpuArenaSlice lut_s =
        arena.allocate_device(memory_plan.rank_lut_bytes, 16u);
    const GpuArenaSlice tiles_s =
        arena.allocate_device(memory_plan.tile_bytes, 16u);
    const GpuArenaSlice tcounts_s =
        arena.allocate_device(memory_plan.tile_count_bytes, 16u);
    const GpuArenaSlice toffsets_s =
        arena.allocate_device(memory_plan.tile_offset_bytes, 16u);
    const GpuArenaSlice tblocks_s =
        arena.allocate_device(memory_plan.scan_block_bytes, 16u);
    const GpuArenaSlice counters_s =
        arena.allocate_device(memory_plan.counter_bytes, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid()
        || !tcounts_s.valid() || !toffsets_s.valid() || !tblocks_s.valid()
        || !counters_s.valid()) {
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) {
        std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    }
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(TileC));
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(
        ShaderKind::phase_c_sketch_tile,
        kPhaseCSketchLocal,
        std::max<std::uint32_t>(window_size, 1u));

    const std::uint64_t capacity = max_target_minimizers;
    Buffer keys = create_device_buffer(capacity * 8u, kPhaseBBufferUsage);
    Buffer values = create_device_buffer(capacity * 8u, kPhaseBBufferUsage);
    if (keys.mapped == nullptr || values.mapped == nullptr) {
        arena.rewind(mark);
        return out;
    }

    const GpuArenaSlice keys_s{&keys, 0u, keys.bytes};
    const GpuArenaSlice values_s{&values, 0u, values.bytes};
    {
        const std::array<std::uint32_t, 4> counters_init{
            0u,
            static_cast<std::uint32_t>(capacity),
            0u,
            0u};
        std::memcpy(base + counters_s.offset, counters_init.data(), sizeof(counters_init));
    }

    {
        static const auto* _sketch_d =
            bioseq::instr::register_region_static("phase_c_sorted_pair_sketch");
        auto _sketch_s = bioseq::instr::region(_sketch_d);
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 9> infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},
            {values_s.buffer->buffer, values_s.offset, values_s.size},
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
        }};
        SketchPushC push{
            2u,
            window_size,
            kmer_size,
            params.hash_bits,
            params.hash_shift,
            params.pos_bits,
            params.minimizer_word_kind,
            params.minimizer_words32,
            kmer_size,
            params.alphabet_kind,
            params.unit_bits,
            params.bits_per_char,
            params.letters_per_unit,
            params.char_mask,
            num_tiles,
            1u,
            0u,
            0u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_emit)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_sorted_pair_emit)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, sketch_pipe, infos, &push, sizeof(push),
            sketch_groups);
        record_shader_to_host(command_buffer_);
        submit_and_wait(
            device_, queue_, command_buffer_, fence_, "submit(pc_sorted_pair_emit)");
    }

    std::uint32_t pair_count32 = 0;
    std::memcpy(&pair_count32, base + counters_s.offset, sizeof(pair_count32));
    const std::uint64_t pair_count = pair_count32;
    // DOC0345 — a window with zero minimizers (e.g. an all-N region) is a
    // LEGITIMATE empty result, not a GPU failure: return valid + empty so the
    // caller emits no pairs rather than throwing VulkanRuntimeError. Only an
    // actual capacity overshoot (count > the worst-case bound) is a real decline.
    if (pair_count == 0u) {
        out.pair_count = 0u;
        out.valid = true;
        arena.rewind(mark);
        return out;
    }
    if (pair_count > max_target_minimizers) {
        arena.rewind(mark);
        return out;
    }

    const WgPlan sort_wg = plan_workgroups(pair_count, features_.max_workgroup_count_x);
    Buffer keys_scratch = create_device_buffer(pair_count * 8u, kPhaseBBufferUsage);
    Buffer values_scratch = create_device_buffer(pair_count * 8u, kPhaseBBufferUsage);
    Buffer histogram = create_device_buffer(
        std::uint64_t{sort_wg.num_workgroups} * 256u * 4u,
        kPhaseBBufferUsage);
    if (keys_scratch.mapped == nullptr || values_scratch.mapped == nullptr
        || histogram.mapped == nullptr) {
        arena.rewind(mark);
        return out;
    }

    SortedPairDeviceViews views{
        .keys = keys_s,
        .values = values_s,
        .keys_scratch = GpuArenaSlice{&keys_scratch, 0u, keys_scratch.bytes},
        .values_scratch = GpuArenaSlice{&values_scratch, 0u, values_scratch.bytes},
        .keys_in_scratch = false,
    };
    const GpuArenaSlice histogram_s{&histogram, 0u, histogram.bytes};
    const GpuSortPlan value_sort_plan{
        .element_count = pair_count,
        .key_bytes = 8u,
        .value_bytes = 8u,
        .radix_bits = 8u,
        .pass_count = 8u,
    };
    const std::uint32_t hash_passes = std::clamp<std::uint32_t>(
        (params.hash_bits + 7u) / 8u,
        1u,
        8u);
    const GpuSortPlan key_sort_plan{
        .element_count = pair_count,
        .key_bytes = 8u,
        .value_bytes = 8u,
        .radix_bits = 8u,
        .pass_count = hash_passes,
    };

    {
        static const auto* _sort_d =
            bioseq::instr::register_region_static("phase_c_sorted_pair_sort");
        auto _sort_s = bioseq::instr::region(_sort_d);
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_sort)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_sorted_pair_sort)");
        record_stable_pair_sort_by_value_then_key_locked(
            command_buffer_,
            views,
            histogram_s,
            value_sort_plan,
            key_sort_plan,
            sort_wg.num_workgroups,
            sort_wg.elems_per_wg);
        record_shader_to_host(command_buffer_);
        submit_and_wait(
            device_, queue_, command_buffer_, fence_, "submit(pc_sorted_pair_sort)");
    }

    const GpuArenaSlice& sorted_keys = choose_sorted_keys(views);
    const GpuArenaSlice& sorted_values = choose_sorted_values(views);
    const std::uint64_t host_bytes = pair_count * sizeof(std::uint64_t);
    out.host_key_bytes.resize(static_cast<std::size_t>(host_bytes));
    out.host_value_bytes.resize(static_cast<std::size_t>(host_bytes));
    std::memcpy(
        out.host_key_bytes.data(),
        static_cast<const std::byte*>(sorted_keys.buffer->mapped) + sorted_keys.offset,
        static_cast<std::size_t>(host_bytes));
    std::memcpy(
        out.host_value_bytes.data(),
        static_cast<const std::byte*>(sorted_values.buffer->mapped) + sorted_values.offset,
        static_cast<std::size_t>(host_bytes));

    const bool sorted_keys_are_scratch = sorted_keys.buffer == &keys_scratch;
    const bool sorted_values_are_scratch = sorted_values.buffer == &values_scratch;
    out.keys = sorted_keys_are_scratch ? std::move(keys_scratch) : std::move(keys);
    out.values =
        sorted_values_are_scratch ? std::move(values_scratch) : std::move(values);
    out.pair_count = pair_count32;
    out.valid = true;
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// DOC0346 — GPU sequence sort. Two submits (modelled on the index build):
//   1: upload seq+descs -> extract 64-bit canonical key per row -> radix sort.
//   host: build equal-key run ranges; seed `index` from the sorted row ids.
//   2: per-run bitonic tie-break over the full canonical compare; if any run is
//      oversized, continue in the same submit with global full-compare bitonic
//      stages over the padded permutation. Then download the permutation.
std::vector<std::uint32_t> Runtime::run_sequence_sort_roundtrip(
    std::span<const std::byte> packed_units,
    std::span<const SeqDescWire> descs,
    const SeqSortKernelParams& params)
{
    const SeqSortUploadChunk chunk{
        packed_units.data(),
        static_cast<std::uint64_t>(packed_units.size()),
        static_cast<std::uint64_t>(packed_units.size())};
    return run_sequence_sort_roundtrip(std::span<const SeqSortUploadChunk>(&chunk, 1u),
                                       descs, params);
}

std::vector<std::uint32_t> Runtime::run_sequence_sort_roundtrip(
    std::span<const SeqSortUploadChunk> chunks,
    std::span<const SeqDescWire> descs,
    const SeqSortKernelParams& params)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_sequence_sort_roundtrip on unavailable device");
    }
    const std::uint32_t n = static_cast<std::uint32_t>(descs.size());
    if (n == 0) return {};
    if (n == 1) return std::vector<std::uint32_t>{0u};

    std::uint64_t packed_unit_bytes = 0;
    for (const SeqSortUploadChunk& chunk : chunks) {
        if (chunk.byte_size > chunk.padded_byte_size) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: invalid upload chunk padding");
        }
        if (chunk.byte_size > 0u && chunk.bytes == nullptr) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: non-empty upload chunk has null data");
        }
        if (packed_unit_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - chunk.padded_byte_size) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: upload chunks exceed uint64");
        }
        packed_unit_bytes += chunk.padded_byte_size;
    }
    seq_sort_uploaded_bytes_.fetch_add(
        packed_unit_bytes,
        std::memory_order_relaxed);

    static constexpr std::uint32_t kSeqSortLocal = 256u;
    const bool dna4 =
        params.alphabet_kind == static_cast<std::uint32_t>(AlphabetKernelKind::dna4_nt4);
    const ShaderKind key_kind = dna4 ? ShaderKind::seq_sort_extract_key_dna4
                                     : ShaderKind::seq_sort_extract_key_generic;
    const ShaderKind tb_kind  = dna4 ? ShaderKind::seq_sort_tiebreak_dna4
                                     : ShaderKind::seq_sort_tiebreak_generic;
    const ShaderKind gc_kind  = dna4 ? ShaderKind::seq_sort_global_compare_dna4
                                     : ShaderKind::seq_sort_global_compare_generic;
    const ShaderKind dense_kind = dna4 ? ShaderKind::seq_sort_dense_scatter_dna4
                                       : ShaderKind::seq_sort_dense_scatter_generic;
    const ShaderKind prefix_kind = dna4 ? ShaderKind::seq_sort_prefix_check_dna4
                                        : ShaderKind::seq_sort_prefix_check_generic;
    const PhaseBPipeline key_pipe = get_phase_b_pipeline_locked(key_kind, kSeqSortLocal, 0u);
    const PhaseBPipeline tb_pipe  = get_phase_b_pipeline_locked(tb_kind, kSeqSortLocal, 0u);
    const PhaseBPipeline gc_pipe  = get_phase_b_pipeline_locked(gc_kind, kSeqSortLocal, 0u);
    const PhaseBPipeline dense_pipe = get_phase_b_pipeline_locked(dense_kind, kSeqSortLocal, 0u);
    const PhaseBPipeline prefix_pipe = get_phase_b_pipeline_locked(prefix_kind, kSeqSortLocal, 0u);
    const WgPlan swg = plan_workgroups(n, features_.max_workgroup_count_x);
    auto next_power_of_two = [](std::uint32_t value) -> std::uint32_t {
        if (value <= 1u) return 1u;
        if (value > 0x80000000u) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: batch too large for padded bitonic index");
        }
        value -= 1u;
        value |= value >> 1u;
        value |= value >> 2u;
        value |= value >> 4u;
        value |= value >> 8u;
        value |= value >> 16u;
        return value + 1u;
    };
    const std::uint32_t padded_n = next_power_of_two(n);

    // Device buffers (unified/host-coherent on Apple). seq is the concatenated,
    // 4-byte-aligned per-row unit blocks viewed as `uint seq_words[]`.
    const std::uint64_t seq_bytes = std::max<std::uint64_t>(packed_unit_bytes, 4u);
    Buffer seq        = create_device_buffer(seq_bytes, kPhaseBBufferUsage);
    Buffer descs_buf  = create_device_buffer(std::uint64_t{n} * 16u, kPhaseBBufferUsage);
    Buffer keys       = create_device_buffer(std::uint64_t{n} * 8u, kPhaseBBufferUsage);
    Buffer vals       = create_device_buffer(std::uint64_t{n} * 8u, kPhaseBBufferUsage);
    Buffer keys_scr   = create_device_buffer(std::uint64_t{n} * 8u, kPhaseBBufferUsage);
    Buffer vals_scr   = create_device_buffer(std::uint64_t{n} * 8u, kPhaseBBufferUsage);
    Buffer hist       = create_device_buffer(std::uint64_t{swg.num_workgroups} * 256u * 4u, kPhaseBBufferUsage);
    Buffer index_buf  = create_device_buffer(std::uint64_t{padded_n} * 4u, kPhaseBBufferUsage);
    Buffer dense_status = create_device_buffer(4u, kPhaseBBufferUsage);
    if (seq.mapped == nullptr || descs_buf.mapped == nullptr || keys.mapped == nullptr
        || vals.mapped == nullptr || keys_scr.mapped == nullptr || vals_scr.mapped == nullptr
        || hist.mapped == nullptr || index_buf.mapped == nullptr
        || dense_status.mapped == nullptr) {
        throw VulkanRuntimeError("run_sequence_sort_roundtrip: device buffer alloc failed");
    }
    auto* seq_base = static_cast<std::byte*>(seq.mapped);
    std::uint64_t write_offset = 0;
    for (const SeqSortUploadChunk& chunk : chunks) {
        if (chunk.byte_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: upload chunk exceeds size_t");
        }
        if (chunk.padded_byte_size - chunk.byte_size >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            throw VulkanRuntimeError("run_sequence_sort_roundtrip: upload padding exceeds size_t");
        }
        const auto* bytes = static_cast<const std::byte*>(chunk.bytes);
        const std::size_t byte_count = static_cast<std::size_t>(chunk.byte_size);
        if (byte_count > 0u)
            std::memcpy(seq_base + write_offset, bytes, byte_count);
        write_offset += chunk.byte_size;
        const std::size_t pad_count =
            static_cast<std::size_t>(chunk.padded_byte_size - chunk.byte_size);
        if (pad_count > 0u)
            std::memset(seq_base + write_offset, 0, pad_count);
        write_offset += chunk.padded_byte_size - chunk.byte_size;
    }
    if (write_offset < seq_bytes)
        std::memset(seq_base + write_offset, 0, static_cast<std::size_t>(seq_bytes - write_offset));
    std::memcpy(descs_buf.mapped, descs.data(), descs.size() * sizeof(SeqDescWire));

    const GpuArenaSlice seq_s{&seq, 0, seq.bytes};
    const GpuArenaSlice descs_s{&descs_buf, 0, descs_buf.bytes};
    const GpuArenaSlice keys_s{&keys, 0, keys.bytes};
    const GpuArenaSlice vals_s{&vals, 0, vals.bytes};
    const GpuArenaSlice keys_scr_s{&keys_scr, 0, keys_scr.bytes};
    const GpuArenaSlice vals_scr_s{&vals_scr, 0, vals_scr.bytes};
    const GpuArenaSlice hist_s{&hist, 0, hist.bytes};
    const GpuArenaSlice index_s{&index_buf, 0, index_buf.bytes};
    const GpuArenaSlice dense_status_s{&dense_status, 0, dense_status.bytes};

    auto bump = [this]() { seq_sort_dispatch_count_.fetch_add(1, std::memory_order_relaxed); };

    // Dense-rank GPU fast path. Offset 0 handles short/front-key batches. The
    // tail offset handles long fixed-length common-prefix batches only when the
    // shader proves the prefix identity and the selected key is [0,n)-dense.
    auto* index_ptr = static_cast<std::uint32_t*>(index_buf.mapped);
    auto* dense_status_ptr = static_cast<std::uint32_t*>(dense_status.mapped);
    const std::uint32_t units_for_64 =
        std::max<std::uint32_t>(1u, (64u + params.unit_bits - 1u) / params.unit_bits);
    std::array<std::uint32_t, 2> dense_offsets{0u, 0u};
    std::uint32_t dense_offset_count = 1u;
    if (descs.front().unit_count > units_for_64) {
        const std::uint32_t tail_offset = descs.front().unit_count - units_for_64;
        if (tail_offset != 0u) dense_offsets[dense_offset_count++] = tail_offset;
    }
    for (std::uint32_t attempt = 0; attempt < dense_offset_count; ++attempt) {
        const std::uint32_t key_offset = dense_offsets[attempt];
        const std::uint64_t prefix_work =
            static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(key_offset);
        if (prefix_work > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            continue;
        }
        std::fill(index_ptr, index_ptr + padded_n, 0xffffffffu);
        *dense_status_ptr = 0u;
        const SeqSortPush dense_push{
            n, params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
            static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
            static_cast<std::uint32_t>(params.used_storage_mask >> 32),
            key_offset, key_offset == 0u ? 0u : 1u, 0u};
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort-dense)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort-dense)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> dense_infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer, descs_s.offset, descs_s.size},
            {index_s.buffer->buffer, index_s.offset, index_s.size},
            {dense_status_s.buffer->buffer, dense_status_s.offset, dense_status_s.size},
        }};
        if (key_offset != 0u) {
            record_phase_c_dispatch_locked(
                command_buffer_, prefix_pipe, dense_infos, &dense_push, sizeof(dense_push),
                ceil_div_u32(static_cast<std::uint32_t>(prefix_work), kSeqSortLocal));
            record_compute_barrier(command_buffer_);
        }
        record_phase_c_dispatch_locked(command_buffer_, dense_pipe, dense_infos,
                                       &dense_push, sizeof(dense_push),
                                       ceil_div_u32(n, kSeqSortLocal));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort-dense)");
        if (*dense_status_ptr == 0u
            && std::find(index_ptr, index_ptr + n, 0xffffffffu) == index_ptr + n) {
            std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
            seq_sort_dense_scatter_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
            bump();
            return perm;
        }
    }

    const SeqSortPush key_push{
        n, params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
        static_cast<std::uint32_t>(params.used_storage_mask >> 32), 0u, 0u, 0u};

    // --- Submit 1: extract keys, then radix sort (key,value=row index) ---
    bool sorted_in_scratch = false;
    {
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort1)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort1)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> key_infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer, descs_s.offset, descs_s.size},
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},
            {vals_s.buffer->buffer, vals_s.offset, vals_s.size},
        }};
        record_phase_c_dispatch_locked(command_buffer_, key_pipe, key_infos,
                                       &key_push, sizeof(key_push), ceil_div_u32(n, kSeqSortLocal));
        record_compute_barrier(command_buffer_);
        const GpuSortPlan sort_plan{n, 8u, 8u, 8u, 8u};   // full 64-bit key, 8 LSD passes
        record_radix_sort_locked(command_buffer_, keys_s, vals_s, keys_scr_s, vals_scr_s, hist_s,
                                 sort_plan, swg.num_workgroups, swg.elems_per_wg, 1u, sorted_in_scratch);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort1)");
    }

    // Sorted keys/vals live in keys/vals or their scratch, per the ping-pong parity.
    const std::uint64_t* sorted_keys =
        static_cast<const std::uint64_t*>((sorted_in_scratch ? keys_scr : keys).mapped);
    const std::uint64_t* sorted_vals =
        static_cast<const std::uint64_t*>((sorted_in_scratch ? vals_scr : vals).mapped);

    // Seed `index` from the sorted row ids (ascending by key, radix-stable on ties).
    for (std::uint32_t i = 0; i < n; ++i)
        index_ptr[i] = static_cast<std::uint32_t>(sorted_vals[i] & 0xffffffffu);
    for (std::uint32_t i = n; i < padded_n; ++i)
        index_ptr[i] = 0xffffffffu;

    // Build equal-key run ranges (only runs of length > 1 need a tie-break).
    struct RunRange { std::uint32_t begin; std::uint32_t len; };
    std::vector<RunRange> ranges;
    bool has_oversized_run = false;
    for (std::uint32_t i = 0; i < n; ) {
        std::uint32_t j = i + 1u;
        while (j < n && sorted_keys[j] == sorted_keys[i]) ++j;
        if (j - i > 1u) {
            const std::uint32_t len = j - i;
            ranges.push_back(RunRange{i, len});
            has_oversized_run = has_oversized_run || len > kSeqSortLocal;
        }
        i = j;
    }

    if (ranges.empty()) {
        // No equal-key collisions: the 64-bit key fully ordered the batch.
        std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
        bump();
        return perm;
    }

    // --- Submit 2: GPU full-compare tie-break. Oversized collisions continue
    // through a device-side global bitonic network before the one host readback.
    Buffer ranges_buf = create_device_buffer(std::uint64_t{ranges.size()} * 8u, kPhaseBBufferUsage);
    if (ranges_buf.mapped == nullptr)
        throw VulkanRuntimeError("run_sequence_sort_roundtrip: ranges buffer alloc failed");
    std::memcpy(ranges_buf.mapped, ranges.data(), ranges.size() * sizeof(RunRange));
    const GpuArenaSlice ranges_s{&ranges_buf, 0, ranges_buf.bytes};
    const SeqSortPush tb_push{
        static_cast<std::uint32_t>(ranges.size()), params.unit_bits, params.bits_per_char,
        params.letters_per_unit, params.char_mask,
        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
        static_cast<std::uint32_t>(params.used_storage_mask >> 32), 0u, 0u, 0u};
    std::uint64_t global_compare_dispatches = 0;
    {
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(seqsort2)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(seqsort2)");
        record_host_to_shader(command_buffer_);
        const std::array<VkDescriptorBufferInfo, 4> tb_infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {descs_s.buffer->buffer, descs_s.offset, descs_s.size},
            {index_s.buffer->buffer, index_s.offset, index_s.size},
            {ranges_s.buffer->buffer, ranges_s.offset, ranges_s.size},
        }};
        record_phase_c_dispatch_locked(command_buffer_, tb_pipe, tb_infos,
                                       &tb_push, sizeof(tb_push),
                                       static_cast<std::uint32_t>(ranges.size()));
        if (has_oversized_run) {
            const std::array<VkDescriptorBufferInfo, 4> gc_infos{{
                {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
                {descs_s.buffer->buffer, descs_s.offset, descs_s.size},
                {index_s.buffer->buffer, index_s.offset, index_s.size},
                {ranges_s.buffer->buffer, ranges_s.offset, ranges_s.size},
            }};
            for (std::uint32_t sort_size = 2u; sort_size <= padded_n; ) {
                for (std::uint32_t stride = sort_size >> 1u; stride > 0u; stride >>= 1u) {
                    record_compute_barrier(command_buffer_);
                    SeqSortPush gc_push{
                        padded_n, params.unit_bits, params.bits_per_char,
                        params.letters_per_unit, params.char_mask,
                        static_cast<std::uint32_t>(params.used_storage_mask & 0xffffffffu),
                        static_cast<std::uint32_t>(params.used_storage_mask >> 32),
                        0u, sort_size, stride};
                    record_phase_c_dispatch_locked(
                        command_buffer_, gc_pipe, gc_infos, &gc_push, sizeof(gc_push),
                        ceil_div_u32(padded_n, kSeqSortLocal));
                    ++global_compare_dispatches;
                }
                if (sort_size == padded_n) break;
                sort_size <<= 1u;
            }
        }
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(seqsort2)");
    }

    if (global_compare_dispatches != 0) {
        seq_sort_global_compare_dispatch_count_.fetch_add(
            global_compare_dispatches, std::memory_order_relaxed);
    }
    index_ptr = static_cast<std::uint32_t*>(index_buf.mapped);  // mapping is stable; re-fetch defensively
    std::vector<std::uint32_t> perm(index_ptr, index_ptr + n);
    bump();
    return perm;
}

std::vector<GpuAlignmentExtensionResultWire> Runtime::run_alignment_extension_roundtrip(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const GpuAlignmentExtensionJobWire> jobs,
    const GpuAlignmentExtensionParams& params)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_alignment_extension_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_alignment_extension_inputs(query_symbols, target_symbols, jobs, params);
    if (features_.max_workgroup_count_x == 0) {
        throw VulkanRuntimeError("alignment extension device reports zero max workgroup count");
    }

    constexpr std::uint32_t kAlignmentExtensionLocal = 1u;
    const PhaseBPipeline pipe = get_phase_b_pipeline_locked(
        ShaderKind::align_extend_banded_affine,
        kAlignmentExtensionLocal,
        params.max_target_len);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(GpuAlignmentExtensionJobWire), 4u);
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(GpuAlignmentExtensionResultWire);

    Buffer query_buf = create_device_buffer(query_bytes, kPhaseBBufferUsage);
    Buffer target_buf = create_device_buffer(target_bytes, kPhaseBBufferUsage);
    Buffer jobs_buf = create_device_buffer(job_bytes, kPhaseBBufferUsage);
    Buffer results_buf = create_device_buffer(result_bytes, kPhaseBBufferUsage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr
        || jobs_buf.mapped == nullptr || results_buf.mapped == nullptr) {
        throw VulkanRuntimeError("run_alignment_extension_roundtrip: device buffer alloc failed");
    }
    if (!query_symbols.empty()) {
        std::memcpy(query_buf.mapped, query_symbols.data(),
                    query_symbols.size() * sizeof(std::uint32_t));
    }
    if (!target_symbols.empty()) {
        std::memcpy(target_buf.mapped, target_symbols.data(),
                    target_symbols.size() * sizeof(std::uint32_t));
    }
    std::memcpy(jobs_buf.mapped, jobs.data(), jobs.size() * sizeof(GpuAlignmentExtensionJobWire));
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));

    const std::array<VkDescriptorBufferInfo, 4> infos{{
        {query_buf.buffer, 0, query_buf.bytes},
        {target_buf.buffer, 0, target_buf.bytes},
        {jobs_buf.buffer, 0, jobs_buf.bytes},
        {results_buf.buffer, 0, results_buf.bytes},
    }};
    const AlignmentExtensionPush push = alignment_extension_push(
        static_cast<std::uint32_t>(jobs.size()), params);
    const std::uint32_t groups = std::min(
        static_cast<std::uint32_t>(jobs.size()), features_.max_workgroup_count_x);

    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_ext)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(align_ext)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_ext)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_ext_post)");

    std::vector<GpuAlignmentExtensionResultWire> results(jobs.size());
    std::memcpy(results.data(), results_buf.mapped, static_cast<std::size_t>(result_bytes));
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].status != k_gpu_alignment_extension_status_ok) {
            throw VulkanRuntimeError(fmt::format(
                "alignment extension shader rejected job {} with status {}",
                i, results[i].status));
        }
    }
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return results;
}

AlignmentCigarBatchResult Runtime::run_alignment_simple_cigar_roundtrip(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols,
    std::span<const GpuAlignmentCigarJobWire> jobs,
    const GpuAlignmentCigarParams& params,
    std::uint32_t packed_cigar_capacity)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_alignment_simple_cigar_roundtrip on unavailable device");
    }
    if (jobs.empty()) {
        return {};
    }
    validate_alignment_cigar_inputs(
        query_symbols, target_symbols, jobs, params, packed_cigar_capacity);
    if (features_.max_workgroup_count_x == 0) {
        throw VulkanRuntimeError(
            "alignment CIGAR extension device reports zero max workgroup count");
    }

    constexpr std::uint32_t kAlignmentCigarLocal = 64u;
    const PhaseBPipeline pipe = get_phase_b_pipeline_locked(
        ShaderKind::align_extend_simple_cigar,
        kAlignmentCigarLocal,
        k_gpu_alignment_extension_max_len);

    const std::uint64_t query_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(query_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t target_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(target_symbols.size()) * sizeof(std::uint32_t), 4u);
    const std::uint64_t job_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(jobs.size()) * sizeof(GpuAlignmentCigarJobWire), 4u);
    const std::uint64_t result_bytes =
        static_cast<std::uint64_t>(jobs.size()) * sizeof(GpuAlignmentCigarResultWire);
    const std::uint64_t cigar_bytes =
        static_cast<std::uint64_t>(packed_cigar_capacity) * sizeof(std::uint32_t);
    std::uint64_t traceback_words = 0;
    for (const GpuAlignmentCigarJobWire& job : jobs) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(job.traceback_tile_offset)
            + static_cast<std::uint64_t>(job.traceback_tile_count);
        traceback_words = std::max(traceback_words, end);
    }
    const std::uint64_t traceback_bytes =
        std::max<std::uint64_t>(traceback_words * sizeof(std::uint32_t), 4u);

    Buffer query_buf = create_device_buffer(query_bytes, kPhaseBBufferUsage);
    Buffer target_buf = create_device_buffer(target_bytes, kPhaseBBufferUsage);
    Buffer jobs_buf = create_device_buffer(job_bytes, kPhaseBBufferUsage);
    Buffer results_buf = create_device_buffer(result_bytes, kPhaseBBufferUsage);
    Buffer cigar_buf = create_device_buffer(cigar_bytes, kPhaseBBufferUsage);
    Buffer traceback_buf = create_device_buffer(traceback_bytes, kPhaseBBufferUsage);
    if (query_buf.mapped == nullptr || target_buf.mapped == nullptr
        || jobs_buf.mapped == nullptr || results_buf.mapped == nullptr
        || cigar_buf.mapped == nullptr || traceback_buf.mapped == nullptr) {
        throw VulkanRuntimeError(
            "run_alignment_simple_cigar_roundtrip: device buffer alloc failed");
    }
    if (!query_symbols.empty()) {
        std::memcpy(query_buf.mapped, query_symbols.data(),
                    query_symbols.size() * sizeof(std::uint32_t));
    }
    if (!target_symbols.empty()) {
        std::memcpy(target_buf.mapped, target_symbols.data(),
                    target_symbols.size() * sizeof(std::uint32_t));
    }
    std::memcpy(jobs_buf.mapped, jobs.data(),
                jobs.size() * sizeof(GpuAlignmentCigarJobWire));
    std::memset(results_buf.mapped, 0, static_cast<std::size_t>(result_bytes));
    std::memset(cigar_buf.mapped, 0, static_cast<std::size_t>(cigar_bytes));

    const std::array<VkDescriptorBufferInfo, 6> infos{{
        {query_buf.buffer, 0, query_buf.bytes},
        {target_buf.buffer, 0, target_buf.bytes},
        {jobs_buf.buffer, 0, jobs_buf.bytes},
        {results_buf.buffer, 0, results_buf.bytes},
        {cigar_buf.buffer, 0, cigar_buf.bytes},
        {traceback_buf.buffer, 0, traceback_buf.bytes},
    }};
    const AlignmentCigarPush push = alignment_cigar_push(
        static_cast<std::uint32_t>(jobs.size()), params);
    const auto job_count_u64 = static_cast<std::uint64_t>(jobs.size());
    const std::uint64_t groups_u64 =
        (job_count_u64 + kAlignmentCigarLocal - 1u) / kAlignmentCigarLocal;
    const std::uint32_t groups = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(groups_u64, features_.max_workgroup_count_x));

    reset_phase_c_pool_locked();
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(align_cigar)");
    record_host_to_shader(command_buffer_);
    record_phase_c_dispatch_locked(
        command_buffer_, pipe, infos, &push, sizeof(push), groups);
    record_shader_to_host(command_buffer_);
    submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(align_cigar)");
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(align_cigar_post)");

    AlignmentCigarBatchResult out{};
    out.results.resize(jobs.size());
    out.packed_cigar.resize(packed_cigar_capacity);
    std::memcpy(out.results.data(), results_buf.mapped,
                static_cast<std::size_t>(result_bytes));
    std::memcpy(out.packed_cigar.data(), cigar_buf.mapped,
                static_cast<std::size_t>(cigar_bytes));
    for (std::size_t i = 0; i < out.results.size(); ++i) {
        if (out.results[i].status != k_gpu_alignment_extension_status_ok) {
            throw VulkanRuntimeError(fmt::format(
                "alignment CIGAR extension shader rejected job {} with status {}",
                i, out.results[i].status));
        }
    }
    alignment_extension_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return out;
}

namespace {

constexpr std::uint32_t kPhaseCSeedCoordinateBatchWave = 4096u;
constexpr bool kPhaseCCoordinateVoteFastPath = true;

struct ChainRangePushLocal {
    std::uint32_t query_count;
    std::uint32_t max_ref_gap;
};
static_assert(sizeof(ChainRangePushLocal) == 8);

struct PhaseCCoordinateVoteJobWire {
    std::uint32_t anchor_offset;
    std::uint32_t anchor_count;
    std::uint32_t query_len_lo;
    std::uint32_t query_len_hi;
};
static_assert(sizeof(PhaseCCoordinateVoteJobWire) == 16);

struct PhaseCSeedCoordinateWaveState {
    std::size_t result_index = 0;
    PhaseCSeedCoordinateJob job{};
    PhaseCSeedQueryRecordWire query_record{};
    std::uint32_t minimizer_count = 0;
    std::uint32_t num_blocks = 0;
    std::uint32_t seed_groups = 0;
    std::uint64_t query_len = 0;
    GpuArenaSlice counts_s{};
    GpuArenaSlice prefix_s{};
    GpuArenaSlice blocks_s{};
    SeedPushC seed_push{};
    std::uint64_t total_anchors = 0;
    std::uint32_t total_anchors_u32 = 0;
    std::uint32_t anchor_offset = 0;
    std::uint32_t vote_result_index = 0;
    WgPlan anchor_wg{};
    Buffer anchors_buf;
    Buffer anchor_keys_buf;
    Buffer anchors_scratch_buf;
    Buffer anchor_keys_scratch_buf;
    Buffer anchor_histogram_buf;
    Buffer ranges_buf;
    Buffer range_count_buf;
    Buffer compacted_anchors_buf;
    Buffer span_buf;
    bool sorted_in_scratch = false;
    std::uint32_t range_count = 0;
    std::uint32_t compacted_anchor_count = 0;
    std::uint32_t active_anchor_count = 0;
    Buffer score_buf;
    Buffer predecessor_buf;
    Buffer visited_buf;
    Buffer trace_buf;
    Buffer result_buf;
    bool skip_seed = false;
    bool has_anchors = false;
    bool has_ranges = false;
	bool chain_active = false;

    [[nodiscard]] GpuArenaSlice anchors_slice() noexcept {
        return GpuArenaSlice{&anchors_buf, 0, anchors_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice anchor_keys_slice() noexcept {
        return GpuArenaSlice{&anchor_keys_buf, 0, anchor_keys_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice anchors_scratch_slice() noexcept {
        return GpuArenaSlice{&anchors_scratch_buf, 0, anchors_scratch_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice anchor_keys_scratch_slice() noexcept {
        return GpuArenaSlice{&anchor_keys_scratch_buf, 0, anchor_keys_scratch_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice anchor_histogram_slice() noexcept {
        return GpuArenaSlice{&anchor_histogram_buf, 0, anchor_histogram_buf.bytes};
    }
    [[nodiscard]] GpuArenaSlice compacted_anchors_slice() noexcept {
        return GpuArenaSlice{&compacted_anchors_buf, 0, compacted_anchors_buf.bytes};
    }
};

[[nodiscard]] bool phase_c_same_coordinate_target(
    const PhaseCSeedCoordinateJob& lhs,
    const PhaseCSeedCoordinateJob& rhs) noexcept
{
    return lhs.slots == rhs.slots
        && lhs.positions == rhs.positions
        && lhs.target_len == rhs.target_len
        && lhs.slot_mask == rhs.slot_mask
        && lhs.position_count == rhs.position_count
        && lhs.bucket_count == rhs.bucket_count
        && lhs.target_position_offset == rhs.target_position_offset;
}

}  // namespace

PhaseCSeedCoordinateBatchRun
Runtime::run_phase_c_seed_to_coordinate_multi_query_batch_roundtrip(
    const PhaseCSeedQueryInputs& query_inputs,
    std::span<const PhaseCSeedCoordinateJob> jobs,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    const ChainingPipelineKey& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError(
            "run_phase_c_seed_to_coordinate_batch_roundtrip on unavailable device");
    }
    if (features_.max_workgroup_count_x == 0u) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate batch has zero workgroup limit");
    }
    if (query_inputs.device_query_records != nullptr) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate device query records are not implemented");
    }
    for (const PhaseCSeedCoordinateJob& job : jobs) {
        if (job.slots == nullptr || job.positions == nullptr) {
            throw VulkanRuntimeError(
                "phase_c seed-to-coordinate batch missing resident index buffers");
        }
        if (job.query_index >= query_inputs.host_query_records.size()) {
            throw VulkanRuntimeError("phase_c seed-to-coordinate job query_index out of range");
        }
    }

    PhaseCSeedCoordinateBatchRun run{};
    run.results.resize(jobs.size());
    run.stats.problem_count = static_cast<std::uint64_t>(jobs.size());
    const std::uint32_t total_mc = query_inputs.minimizer_count;
    if (jobs.empty() || total_mc == 0u || max_occ < 0) {
        return run;
    }
    if (query_inputs.host_query_records.empty()) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate batch missing query records");
    }
    for (const PhaseCSeedQueryRecordWire& qrec : query_inputs.host_query_records) {
        if (qrec.query_len > std::numeric_limits<std::uint32_t>::max()) {
            throw VulkanRuntimeError("phase_c seed-to-coordinate query length exceeds ABI");
        }
        if (qrec.minimizer_count == 0u) {
            continue;
        }
        const std::uint64_t end =
            static_cast<std::uint64_t>(qrec.minimizer_offset) + qrec.minimizer_count;
        if (end > total_mc) {
            throw VulkanRuntimeError("phase_c seed-to-coordinate query minimizer slice exceeds buffer");
        }
    }

    const std::uint64_t min_words_bytes = query_inputs.device_minimizer_words == nullptr
        ? std::max<std::uint64_t>(
              minimizer_device_bytes(
                  query_inputs.minimizer_word_kind,
                  query_inputs.minimizer_words32_per_record,
                  total_mc),
              4u)
        : 0u;
    if (query_inputs.device_minimizer_words == nullptr
        && query_inputs.host_minimizer_bytes.size()
            > static_cast<std::size_t>(min_words_bytes)) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate minimizer upload exceeds buffer");
    }

    const PhaseBPipeline count_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_count, 128u, 1u);
    const PhaseBPipeline emit_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_emit, 128u, 1u);
    const PhaseBPipeline scanb =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scana =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
    const PhaseBPipeline range_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_chain_range_build, 1u, 1u);
    const PhaseBPipeline coord_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_coordinate_reduce, 128u, 1u);
    const PhaseBPipeline vote_coord_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_coordinate_vote_reduce, 128u, 1u);
    const PhaseBPipeline vote_coord_batch_pipe =
        get_phase_b_pipeline_locked(
            ShaderKind::phase_c_coordinate_vote_reduce_batch, 128u, 1u);
    const VkPipeline chain_pipeline = create_pipeline_locked(chain_key);

    constexpr std::uint64_t qrec_stride = kPhaseCSlotOffsetAlign;
    const std::uint64_t query_record_bytes = std::max<std::uint64_t>(
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(query_inputs.host_query_records.size()),
            qrec_stride),
        qrec_stride);

    GpuArena& arena = phase_c_arena();
    std::uint64_t submitted_waves = 0;
    for (std::size_t wave_begin = 0; wave_begin < jobs.size();) {
        std::size_t wave_end =
            std::min<std::size_t>(jobs.size(), wave_begin + kPhaseCSeedCoordinateBatchWave);
        if (kPhaseCCoordinateVoteFastPath && wave_begin + 1u < wave_end) {
            std::size_t same_target_end = wave_begin + 1u;
            while (same_target_end < wave_end
                   && phase_c_same_coordinate_target(
                       jobs[wave_begin], jobs[same_target_end])) {
                ++same_target_end;
            }
            if (same_target_end > wave_begin + 1u) {
                wave_end = same_target_end;
            }
        }
        const std::size_t next_wave_begin = wave_end;
        const std::size_t wave_count = wave_end - wave_begin;
        std::uint64_t per_wave_problem_bytes = 0u;
        for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
            const PhaseCSeedQueryRecordWire& qrec =
                query_inputs.host_query_records[jobs[job_index].query_index];
            const std::uint32_t qblocks =
                std::max<std::uint32_t>(ceil_div_u32(qrec.minimizer_count, kPhaseCScanBlock), 1u);
            per_wave_problem_bytes = sat_add(
                per_wave_problem_bytes,
                sat_add(
                    std::uint64_t{qrec.minimizer_count} * 8u,
                    std::uint64_t{qblocks} * 4u));
        }
        const std::uint64_t arena_bytes = sat_add(
            sat_add(min_words_bytes, query_record_bytes),
            sat_add(
                per_wave_problem_bytes,
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(3u * wave_count + 2u),
                    std::uint64_t{kPhaseCSlotOffsetAlign})));
        if (!arena.ensure_capacity(arena_bytes, 0u, 0u)) {
            throw VulkanRuntimeError("phase_c seed-to-coordinate batch arena grow failed");
        }
        const GpuArenaMark mark = arena.mark();
        try {
            auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
            const GpuArenaSlice qrec_s = arena.allocate_device(query_record_bytes, 16u);
            GpuArenaSlice minw_s{};
            if (query_inputs.device_minimizer_words == nullptr) {
                minw_s = arena.allocate_device(min_words_bytes, 16u);
            }
            if (!qrec_s.valid()
                || (query_inputs.device_minimizer_words == nullptr && !minw_s.valid())) {
                throw VulkanRuntimeError("phase_c seed-to-coordinate batch arena oversubscribed");
            }

            const VkBuffer min_words_buffer = query_inputs.device_minimizer_words != nullptr
                ? query_inputs.device_minimizer_words->buffer
                : minw_s.buffer->buffer;
            const std::uint64_t min_words_offset =
                query_inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
            const std::uint64_t min_words_range =
                query_inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
            if (query_inputs.device_minimizer_words == nullptr) {
                std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
                if (!query_inputs.host_minimizer_bytes.empty()) {
                    std::memcpy(
                        base + minw_s.offset,
                        query_inputs.host_minimizer_bytes.data(),
                        query_inputs.host_minimizer_bytes.size());
                }
            }
            std::memset(base + qrec_s.offset, 0, static_cast<std::size_t>(qrec_s.size));
            for (std::size_t qrec_index = 0;
                 qrec_index < query_inputs.host_query_records.size();
                 ++qrec_index) {
                std::memcpy(
                    base + qrec_s.offset
                        + static_cast<std::uint64_t>(qrec_index) * qrec_stride,
                    &query_inputs.host_query_records[qrec_index],
                    sizeof(PhaseCSeedQueryRecordWire));
            }

            std::vector<PhaseCSeedCoordinateWaveState> states;
            states.reserve(wave_count);
            std::uint32_t seed_active_count = 0;
            for (std::size_t job_index = wave_begin; job_index < wave_end; ++job_index) {
                PhaseCSeedCoordinateWaveState& state = states.emplace_back();
                state.result_index = job_index;
                state.job = jobs[job_index];
                state.query_record =
                    query_inputs.host_query_records[state.job.query_index];
                state.minimizer_count = state.query_record.minimizer_count;
                state.query_len = state.query_record.query_len;
                state.num_blocks = std::max<std::uint32_t>(
                    ceil_div_u32(state.minimizer_count, kPhaseCScanBlock), 1u);
                state.seed_groups = std::max<std::uint32_t>(
                    1u,
                    std::min(
                        ceil_div_u32(state.minimizer_count, 128u),
                        features_.max_workgroup_count_x));
                state.skip_seed =
                    state.job.position_count == 0u || state.minimizer_count == 0u;
                state.counts_s = arena.allocate_device(
                    std::max<std::uint64_t>(state.minimizer_count, 1u) * 4u, 16u);
                state.prefix_s = arena.allocate_device(
                    std::max<std::uint64_t>(state.minimizer_count, 1u) * 4u, 16u);
                state.blocks_s = arena.allocate_device(
                    std::uint64_t{state.num_blocks} * 4u, 16u);
                if (!state.counts_s.valid() || !state.prefix_s.valid()
                    || !state.blocks_s.valid()) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch arena oversubscribed");
                }
                const std::uint32_t target_offset_lo = static_cast<std::uint32_t>(
                    state.job.target_position_offset & 0xffffffffull);
                const std::uint32_t target_offset_hi =
                    static_cast<std::uint32_t>(state.job.target_position_offset >> 32);
                const PhaseCAnchorSortKeyPlan anchor_sort_plan =
                    phase_c_anchor_sort_key_plan(
                        state.job.target_len,
                        state.job.target_position_offset);
                state.seed_push = SeedPushC{
                    query_inputs.minimizer_words32_per_record,
                    query_inputs.minimizer_word_kind,
                    query_inputs.hash_shift,
                    query_inputs.pos_bits,
                    kmer_size,
                    state.job.slot_mask,
                    static_cast<std::uint32_t>(max_occ),
                    0u,
                    1u,
                    state.job.bucket_count,
                    target_offset_lo,
                    target_offset_hi,
                    anchor_sort_plan.target_ref_bits};
                if (!state.skip_seed) {
                    seed_active_count += 1u;
                }
            }

            if (seed_active_count == 0u) {
                arena.rewind(mark);
                wave_begin = next_wave_begin;
                continue;
            }

            if (kPhaseCCoordinateVoteFastPath) {
                std::vector<PhaseCSeedCoordinateWaveState*> active_states;
                active_states.reserve(seed_active_count);
                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (!state.skip_seed) {
                        active_states.push_back(&state);
                    }
                }

                const PhaseCSeedCoordinateWaveState* first_active =
                    active_states.empty() ? nullptr : active_states.front();
                bool same_target_wave = active_states.size() >= 2u;
                if (same_target_wave) {
                    for (const PhaseCSeedCoordinateWaveState* state : active_states) {
                        if (state->job.slots != first_active->job.slots
                            || state->job.positions != first_active->job.positions
                            || state->job.target_len != first_active->job.target_len
                            || state->job.slot_mask != first_active->job.slot_mask
                            || state->job.position_count != first_active->job.position_count
                            || state->job.bucket_count != first_active->job.bucket_count
                            || state->job.target_position_offset
                                != first_active->job.target_position_offset) {
                            same_target_wave = false;
                            break;
                        }
                    }
                }

                if (same_target_wave) {
                    const std::uint32_t total_blocks =
                        std::max<std::uint32_t>(
                            ceil_div_u32(total_mc, kPhaseCScanBlock),
                            1u);
                    const std::uint64_t counts_bytes =
                        std::max<std::uint64_t>(std::uint64_t{total_mc} * 4u, 4u);
                    const std::uint64_t blocks_bytes =
                        std::max<std::uint64_t>(std::uint64_t{total_blocks} * 4u, 4u);
                    const std::uint64_t dense_qrec_bytes =
                        std::max<std::uint64_t>(
                            static_cast<std::uint64_t>(active_states.size())
                                * sizeof(PhaseCSeedQueryRecordWire),
                            sizeof(PhaseCSeedQueryRecordWire));

                    Buffer batch_counts_buf =
                        create_device_buffer(counts_bytes, kPhaseBBufferUsage);
                    Buffer batch_prefix_buf =
                        create_device_buffer(counts_bytes, kPhaseBBufferUsage);
                    Buffer batch_blocks_buf =
                        create_device_buffer(blocks_bytes, kPhaseBBufferUsage);
                    Buffer batch_qrec_buf =
                        create_device_buffer(dense_qrec_bytes, kPhaseBBufferUsage);
                    if (batch_counts_buf.mapped == nullptr
                        || batch_prefix_buf.mapped == nullptr
                        || batch_blocks_buf.mapped == nullptr
                        || batch_qrec_buf.mapped == nullptr) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate multi-record batch non-unified memory");
                    }
                    std::memset(
                        batch_counts_buf.mapped, 0,
                        static_cast<std::size_t>(batch_counts_buf.bytes));
                    std::memset(
                        batch_prefix_buf.mapped, 0,
                        static_cast<std::size_t>(batch_prefix_buf.bytes));
                    std::memset(
                        batch_blocks_buf.mapped, 0,
                        static_cast<std::size_t>(batch_blocks_buf.bytes));
                    std::memset(
                        batch_qrec_buf.mapped, 0,
                        static_cast<std::size_t>(batch_qrec_buf.bytes));
                    for (std::size_t active_index = 0;
                         active_index < active_states.size();
                         ++active_index) {
                        std::memcpy(
                            static_cast<std::byte*>(batch_qrec_buf.mapped)
                                + active_index * sizeof(PhaseCSeedQueryRecordWire),
                            &active_states[active_index]->query_record,
                            sizeof(PhaseCSeedQueryRecordWire));
                    }

                    SeedPushC batch_seed_push = first_active->seed_push;
                    batch_seed_push.record_count =
                        static_cast<std::uint32_t>(active_states.size());
                    batch_seed_push.emit_mode = 0u;
                    const std::array<VkDescriptorBufferInfo, 8> count_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {first_active->job.slots->buffer, 0, VK_WHOLE_SIZE},
                        {first_active->job.positions->buffer, 0, VK_WHOLE_SIZE},
                        {batch_qrec_buf.buffer, 0, batch_qrec_buf.bytes},
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer, 0, batch_prefix_buf.bytes},
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                    }};
                    const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer, 0, batch_prefix_buf.bytes},
                        {batch_blocks_buf.buffer, 0, batch_blocks_buf.bytes},
                    }};
                    ScanPushC scan{
                        total_mc, kPhaseCScanBlock, total_blocks, 0u};

                    reset_phase_c_pool_locked();
                    check_vk(
                        vkResetCommandBuffer(command_buffer_, 0),
                        "reset(pc_seed_coord_mrec1)");
                    VkCommandBufferBeginInfo begin{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    check_vk(
                        vkBeginCommandBuffer(command_buffer_, &begin),
                        "begin(pc_seed_coord_mrec1)");
                    record_host_to_shader(command_buffer_);
                    record_phase_c_dispatch_locked(
                        command_buffer_, count_pipe, count_infos,
                        &batch_seed_push, sizeof(batch_seed_push),
                        static_cast<std::uint32_t>(active_states.size()));
                    run.stats.seed_count_dispatches += 1u;
                    record_compute_barrier(command_buffer_);
                    record_phase_c_dispatch_locked(
                        command_buffer_, scanb, scan_infos,
                        &scan, sizeof(scan), total_blocks);
                    record_compute_barrier(command_buffer_);
                    record_phase_c_dispatch_locked(
                        command_buffer_, scana, scan_infos,
                        &scan, sizeof(scan), 1u);
                    record_compute_barrier(command_buffer_);
                    scan.pass = 1u;
                    record_phase_c_dispatch_locked(
                        command_buffer_, scana, scan_infos,
                        &scan, sizeof(scan), ceil_div_u32(total_mc, 128u));
                    record_shader_to_host(command_buffer_);
                    submit_and_wait(
                        device_, queue_, command_buffer_, fence_,
                        "submit(pc_seed_coord_mrec1)");

                    std::uint32_t wave_total_prefix = 0u;
                    std::uint32_t wave_total_count = 0u;
                    std::memcpy(
                        &wave_total_prefix,
                        static_cast<std::byte*>(batch_prefix_buf.mapped)
                            + std::uint64_t{total_mc - 1u} * 4u,
                        4u);
                    std::memcpy(
                        &wave_total_count,
                        static_cast<std::byte*>(batch_counts_buf.mapped)
                            + std::uint64_t{total_mc - 1u} * 4u,
                        4u);
                    const std::uint64_t wave_total_anchors =
                        static_cast<std::uint64_t>(wave_total_prefix)
                        + wave_total_count;
                    if (wave_total_anchors > std::numeric_limits<std::uint32_t>::max()) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate multi-record anchor overflow");
                    }

                    std::uint32_t anchor_active_count = 0;
                    for (PhaseCSeedCoordinateWaveState* state : active_states) {
                        const std::uint32_t begin_min =
                            state->query_record.minimizer_offset;
                        const std::uint32_t end_min =
                            begin_min + state->query_record.minimizer_count - 1u;
                        std::uint32_t anchor_begin = 0u;
                        std::uint32_t anchor_end_prefix = 0u;
                        std::uint32_t anchor_end_count = 0u;
                        std::memcpy(
                            &anchor_begin,
                            static_cast<std::byte*>(batch_prefix_buf.mapped)
                                + std::uint64_t{begin_min} * 4u,
                            4u);
                        std::memcpy(
                            &anchor_end_prefix,
                            static_cast<std::byte*>(batch_prefix_buf.mapped)
                                + std::uint64_t{end_min} * 4u,
                            4u);
                        std::memcpy(
                            &anchor_end_count,
                            static_cast<std::byte*>(batch_counts_buf.mapped)
                                + std::uint64_t{end_min} * 4u,
                            4u);
                        const std::uint64_t anchor_end =
                            static_cast<std::uint64_t>(anchor_end_prefix)
                            + anchor_end_count;
                        if (anchor_end < anchor_begin) {
                            throw VulkanRuntimeError(
                                "phase_c seed-to-coordinate multi-record invalid prefix");
                        }
                        state->total_anchors =
                            anchor_end - static_cast<std::uint64_t>(anchor_begin);
                        if (state->total_anchors == 0u) {
                            continue;
                        }
                        if (state->total_anchors
                            > std::numeric_limits<std::uint32_t>::max()) {
                            throw VulkanRuntimeError(
                                "phase_c seed-to-coordinate multi-record per-query overflow");
                        }
                        state->total_anchors_u32 =
                            static_cast<std::uint32_t>(state->total_anchors);
                        state->anchor_offset = anchor_begin;
                        state->has_anchors = true;
                        anchor_active_count += 1u;
                    }

                    if (anchor_active_count == 0u || wave_total_anchors == 0u) {
                        arena.rewind(mark);
                        submitted_waves += 1u;
                        run.stats.wave_count += 1u;
                        wave_begin = next_wave_begin;
                        continue;
                    }

                    const std::uint64_t anchor_bytes =
                        wave_total_anchors * 3u * sizeof(std::uint64_t);
                    Buffer batch_anchors_buf =
                        create_device_buffer(anchor_bytes, kPhaseBBufferUsage);
                    Buffer batch_anchor_keys_buf = create_device_buffer(
                        wave_total_anchors * sizeof(std::uint64_t),
                        kPhaseBBufferUsage);
                    if (batch_anchors_buf.mapped == nullptr
                        || batch_anchor_keys_buf.mapped == nullptr) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate multi-record anchor memory");
                    }
                    std::memset(
                        batch_anchors_buf.mapped, 0,
                        static_cast<std::size_t>(batch_anchors_buf.bytes));
                    std::memset(
                        batch_anchor_keys_buf.mapped, 0,
                        static_cast<std::size_t>(batch_anchor_keys_buf.bytes));

                    std::vector<PhaseCCoordinateVoteJobWire> vote_jobs;
                    vote_jobs.reserve(anchor_active_count);
                    for (PhaseCSeedCoordinateWaveState* state : active_states) {
                        if (!state->has_anchors) {
                            continue;
                        }
                        state->vote_result_index =
                            static_cast<std::uint32_t>(vote_jobs.size());
                        vote_jobs.push_back(PhaseCCoordinateVoteJobWire{
                            .anchor_offset = state->anchor_offset,
                            .anchor_count = state->total_anchors_u32,
                            .query_len_lo = static_cast<std::uint32_t>(
                                state->query_len & 0xffffffffull),
                            .query_len_hi = static_cast<std::uint32_t>(
                                state->query_len >> 32),
                        });
                    }
                    const std::uint64_t vote_job_bytes =
                        std::max<std::uint64_t>(
                            static_cast<std::uint64_t>(vote_jobs.size())
                                * sizeof(PhaseCCoordinateVoteJobWire),
                            sizeof(PhaseCCoordinateVoteJobWire));
                    const std::uint64_t vote_result_bytes =
                        std::max<std::uint64_t>(
                            static_cast<std::uint64_t>(vote_jobs.size())
                                * sizeof(GpuCoordinateOnlyResultWire),
                            sizeof(GpuCoordinateOnlyResultWire));
                    Buffer vote_jobs_buf =
                        create_device_buffer(vote_job_bytes, kPhaseBBufferUsage);
                    Buffer vote_results_buf =
                        create_device_buffer(vote_result_bytes, kPhaseBBufferUsage);
                    if (vote_jobs_buf.mapped == nullptr
                        || vote_results_buf.mapped == nullptr) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate multi-record vote memory");
                    }
                    std::memset(
                        vote_jobs_buf.mapped, 0,
                        static_cast<std::size_t>(vote_jobs_buf.bytes));
                    std::memset(
                        vote_results_buf.mapped, 0,
                        static_cast<std::size_t>(vote_results_buf.bytes));
                    std::memcpy(
                        vote_jobs_buf.mapped, vote_jobs.data(),
                        vote_jobs.size() * sizeof(PhaseCCoordinateVoteJobWire));

                    reset_phase_c_pool_locked();
                    check_vk(
                        vkResetCommandBuffer(command_buffer_, 0),
                        "reset(pc_seed_coord_mrec2)");
                    begin = VkCommandBufferBeginInfo{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    check_vk(
                        vkBeginCommandBuffer(command_buffer_, &begin),
                        "begin(pc_seed_coord_mrec2)");
                    record_host_to_shader(command_buffer_);
                    batch_seed_push.emit_mode = 1u;
                    const std::array<VkDescriptorBufferInfo, 8> emit_infos{{
                        {min_words_buffer, min_words_offset, min_words_range},
                        {first_active->job.slots->buffer, 0, VK_WHOLE_SIZE},
                        {first_active->job.positions->buffer, 0, VK_WHOLE_SIZE},
                        {batch_qrec_buf.buffer, 0, batch_qrec_buf.bytes},
                        {batch_counts_buf.buffer, 0, batch_counts_buf.bytes},
                        {batch_prefix_buf.buffer, 0, batch_prefix_buf.bytes},
                        {batch_anchors_buf.buffer, 0, batch_anchors_buf.bytes},
                        {batch_anchor_keys_buf.buffer, 0, batch_anchor_keys_buf.bytes},
                    }};
                    record_phase_c_dispatch_locked(
                        command_buffer_, emit_pipe, emit_infos,
                        &batch_seed_push, sizeof(batch_seed_push),
                        static_cast<std::uint32_t>(active_states.size()));
                    run.stats.seed_emit_dispatches += 1u;
                    record_compute_barrier(command_buffer_);
                    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                        {batch_anchors_buf.buffer, 0, batch_anchors_buf.bytes},
                        {vote_jobs_buf.buffer, 0, vote_jobs_buf.bytes},
                        {batch_anchor_keys_buf.buffer, 0, batch_anchor_keys_buf.bytes},
                        {vote_results_buf.buffer, 0, vote_results_buf.bytes},
                        {vote_results_buf.buffer, 0, vote_results_buf.bytes},
                        {vote_results_buf.buffer, 0, vote_results_buf.bytes},
                    }};
                    GpuCoordinateReducePush coord_push{
                        .anchor_count = static_cast<std::uint32_t>(vote_jobs.size()),
                        .query_len_lo = 0u,
                        .query_len_hi = 0u,
                        .min_chain_score = min_chain_score,
                        .min_seed_count = min_seed_count,
                        .probe_limit = probe_limit,
                        .reserved = 0u,
                    };
                    record_phase_c_dispatch_locked(
                        command_buffer_, vote_coord_batch_pipe, coord_infos,
                        &coord_push, sizeof(coord_push),
                        static_cast<std::uint32_t>(vote_jobs.size()));
                    run.stats.coordinate_reduce_dispatches += 1u;
                    record_shader_to_host(command_buffer_);
                    submit_and_wait(
                        device_, queue_, command_buffer_, fence_,
                        "submit(pc_seed_coord_mrec2)");

                    for (PhaseCSeedCoordinateWaveState* state : active_states) {
                        if (state->has_anchors) {
                            std::memcpy(
                                &run.results[state->result_index],
                                static_cast<std::byte*>(vote_results_buf.mapped)
                                    + static_cast<std::uint64_t>(
                                        state->vote_result_index)
                                        * sizeof(GpuCoordinateOnlyResultWire),
                                sizeof(GpuCoordinateOnlyResultWire));
                        }
                    }

                    arena.rewind(mark);
                    submitted_waves += 1u;
                    run.stats.wave_count += 1u;
                    wave_begin = next_wave_begin;
                    continue;
                }
            }

            reset_phase_c_pool_locked();
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch1)");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch1)");
            record_host_to_shader(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
                    {min_words_buffer, min_words_offset, min_words_range},
                    {state.job.slots->buffer, 0, VK_WHOLE_SIZE},
                    {state.job.positions->buffer, 0, VK_WHOLE_SIZE},
                    {qrec_s.buffer->buffer,
                     qrec_s.offset + std::uint64_t{state.job.query_index}
                         * qrec_stride,
                     sizeof(PhaseCSeedQueryRecordWire)},
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer, state.prefix_s.offset, state.prefix_s.size},
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                }};
                record_phase_c_dispatch_locked(
                    command_buffer_, count_pipe, seed_infos,
                    &state.seed_push, sizeof(state.seed_push), state.seed_groups);
                run.stats.seed_count_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer, state.blocks_s.offset, state.blocks_s.size},
                }};
                ScanPushC scan{
                    state.minimizer_count, kPhaseCScanBlock, state.num_blocks, 0u};
                record_phase_c_dispatch_locked(
                    command_buffer_, scanb, scan_infos, &scan, sizeof(scan), state.num_blocks);
            }
            record_compute_barrier(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer, state.blocks_s.offset, state.blocks_s.size},
                }};
                ScanPushC scan{
                    state.minimizer_count, kPhaseCScanBlock, state.num_blocks, 0u};
                record_phase_c_dispatch_locked(
                    command_buffer_, scana, scan_infos, &scan, sizeof(scan), 1u);
            }
            record_compute_barrier(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer, state.prefix_s.offset, state.prefix_s.size},
                    {state.blocks_s.buffer->buffer, state.blocks_s.offset, state.blocks_s.size},
                }};
                ScanPushC scan{
                    state.minimizer_count, kPhaseCScanBlock, state.num_blocks, 1u};
                record_phase_c_dispatch_locked(
                    command_buffer_, scana, scan_infos, &scan, sizeof(scan),
                    ceil_div_u32(state.minimizer_count, 128u));
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord_batch1)");

            std::uint32_t anchor_active_count = 0;
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.skip_seed) {
                    continue;
                }
                std::uint32_t plast = 0;
                std::uint32_t clast = 0;
                std::memcpy(
                    &plast,
                    base + state.prefix_s.offset
                        + std::uint64_t{state.minimizer_count - 1u} * 4u,
                    4u);
                std::memcpy(
                    &clast,
                    base + state.counts_s.offset
                        + std::uint64_t{state.minimizer_count - 1u} * 4u,
                    4u);
                state.total_anchors = static_cast<std::uint64_t>(plast) + clast;
                if (state.total_anchors == 0u) {
                    continue;
                }
                if (state.total_anchors > std::numeric_limits<std::uint32_t>::max()) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch anchor count overflow");
                }
                state.total_anchors_u32 = static_cast<std::uint32_t>(state.total_anchors);
                state.anchor_wg =
                    plan_workgroups(state.total_anchors, features_.max_workgroup_count_x);
                const std::uint64_t anchor_u64 = state.total_anchors * 3u;
                state.anchors_buf =
                    create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
                state.anchor_keys_buf =
                    create_device_buffer(state.total_anchors * 8u, kPhaseBBufferUsage);
                if (kPhaseCCoordinateVoteFastPath) {
                    state.result_buf = create_device_buffer(
                        sizeof(GpuCoordinateOnlyResultWire), kPhaseBBufferUsage);
                    if (state.anchors_buf.mapped == nullptr
                        || state.anchor_keys_buf.mapped == nullptr
                        || state.result_buf.mapped == nullptr) {
                        throw VulkanRuntimeError(
                            "phase_c seed-to-coordinate vote batch non-unified memory");
                    }
                    std::memset(
                        state.result_buf.mapped, 0,
                        static_cast<std::size_t>(state.result_buf.bytes));
                    state.has_anchors = true;
                    anchor_active_count += 1u;
                    continue;
                }
                state.anchors_scratch_buf =
                    create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
                state.anchor_keys_scratch_buf =
                    create_device_buffer(state.total_anchors * 8u, kPhaseBBufferUsage);
                state.anchor_histogram_buf = create_device_buffer(
                    std::uint64_t{state.anchor_wg.num_workgroups} * 256u * 4u,
                    kPhaseBBufferUsage);
                state.ranges_buf = create_device_buffer(
                    std::max<std::uint64_t>(
                        state.total_anchors * sizeof(GpuChainRangeWire), 8u),
                    kPhaseBBufferUsage);
                state.range_count_buf = create_device_buffer(8u, kPhaseBBufferUsage);
                state.compacted_anchors_buf =
                    create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
                state.span_buf =
                    create_device_buffer(sizeof(GpuChainRangeBuildSpanWire), kPhaseBBufferUsage);
                if (state.anchors_buf.mapped == nullptr
                    || state.anchor_keys_buf.mapped == nullptr
                    || state.anchors_scratch_buf.mapped == nullptr
                    || state.anchor_keys_scratch_buf.mapped == nullptr
                    || state.anchor_histogram_buf.mapped == nullptr
                    || state.ranges_buf.mapped == nullptr
                    || state.range_count_buf.mapped == nullptr
                    || state.compacted_anchors_buf.mapped == nullptr
                    || state.span_buf.mapped == nullptr) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch non-unified anchor memory");
                }
                const GpuChainRangeBuildSpanWire span{
                    .anchor_lo = 0u,
                    .anchor_hi = state.total_anchors_u32,
                    .range_lo = 0u,
                    .range_out = 0u,
                };
                std::memcpy(state.span_buf.mapped, &span, sizeof(span));
                std::memset(state.range_count_buf.mapped, 0, 8u);
                state.has_anchors = true;
                anchor_active_count += 1u;
            }

            if (anchor_active_count == 0u) {
                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }

            reset_phase_c_pool_locked();
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch2)");
            begin = VkCommandBufferBeginInfo{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch2)");
            record_host_to_shader(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                SeedPushC emit_push = state.seed_push;
                emit_push.emit_mode = 1u;
                GpuArenaSlice anchors_s = state.anchors_slice();
                GpuArenaSlice anchor_keys_s = state.anchor_keys_slice();
                const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
                    {min_words_buffer, min_words_offset, min_words_range},
                    {state.job.slots->buffer, 0, VK_WHOLE_SIZE},
                    {state.job.positions->buffer, 0, VK_WHOLE_SIZE},
                    {qrec_s.buffer->buffer,
                     qrec_s.offset + std::uint64_t{state.job.query_index}
                         * qrec_stride,
                     sizeof(PhaseCSeedQueryRecordWire)},
                    {state.counts_s.buffer->buffer, state.counts_s.offset, state.counts_s.size},
                    {state.prefix_s.buffer->buffer, state.prefix_s.offset, state.prefix_s.size},
                    {anchors_s.buffer->buffer, anchors_s.offset, anchors_s.size},
                    {anchor_keys_s.buffer->buffer, anchor_keys_s.offset, anchor_keys_s.size},
                }};
                record_phase_c_dispatch_locked(
                    command_buffer_, emit_pipe, seed_infos,
                    &emit_push, sizeof(emit_push), state.seed_groups);
                run.stats.seed_emit_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            if (kPhaseCCoordinateVoteFastPath) {
                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (!state.has_anchors) {
                        continue;
                    }
                    GpuArenaSlice anchors_s = state.anchors_slice();
                    GpuArenaSlice anchor_keys_s = state.anchor_keys_slice();
                    const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                        {anchors_s.buffer->buffer, anchors_s.offset, anchors_s.size},
                        {anchor_keys_s.buffer->buffer, anchor_keys_s.offset, anchor_keys_s.size},
                        {anchor_keys_s.buffer->buffer, anchor_keys_s.offset, anchor_keys_s.size},
                        {state.result_buf.buffer, 0, state.result_buf.bytes},
                        {state.result_buf.buffer, 0, state.result_buf.bytes},
                        {state.result_buf.buffer, 0, state.result_buf.bytes},
                    }};
                    GpuCoordinateReducePush coord_push{
                        .anchor_count = state.total_anchors_u32,
                        .query_len_lo = static_cast<std::uint32_t>(
                            state.query_len & 0xffffffffull),
                        .query_len_hi = static_cast<std::uint32_t>(
                            state.query_len >> 32),
                        .min_chain_score = min_chain_score,
                        .min_seed_count = min_seed_count,
                        .probe_limit = probe_limit,
                        .reserved = 0u,
                    };
                    record_phase_c_dispatch_locked(
                        command_buffer_, vote_coord_pipe, coord_infos,
                        &coord_push, sizeof(coord_push), 1u);
                    run.stats.coordinate_reduce_dispatches += 1u;
                }
                record_shader_to_host(command_buffer_);
                submit_and_wait(
                    device_, queue_, command_buffer_, fence_,
                    "submit(pc_seed_coord_vote_batch2)");

                for (PhaseCSeedCoordinateWaveState& state : states) {
                    if (state.has_anchors) {
                        std::memcpy(
                            &run.results[state.result_index],
                            state.result_buf.mapped,
                            sizeof(GpuCoordinateOnlyResultWire));
                    }
                }

                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                GpuArenaSlice anchors_s = state.anchors_slice();
                GpuArenaSlice anchor_keys_s = state.anchor_keys_slice();
                GpuArenaSlice anchors_scratch_s = state.anchors_scratch_slice();
                GpuArenaSlice anchor_keys_scratch_s = state.anchor_keys_scratch_slice();
                GpuArenaSlice anchor_histogram_s = state.anchor_histogram_slice();
                const PhaseCAnchorSortKeyPlan anchor_sort_plan =
                    phase_c_anchor_sort_key_plan(
                        state.job.target_len,
                        state.job.target_position_offset);
                GpuSortPlan sort_plan{
                    state.total_anchors, 8u, 24u, 8u, anchor_sort_plan.pass_count};
                record_radix_sort_locked(
                    command_buffer_,
                    anchor_keys_s,
                    anchors_s,
                    anchor_keys_scratch_s,
                    anchors_scratch_s,
                    anchor_histogram_s,
                    sort_plan,
                    state.anchor_wg.num_workgroups,
                    state.anchor_wg.elems_per_wg,
                    3u,
                    state.sorted_in_scratch);
                run.stats.sort_dispatches += 1u;
                const GpuArenaSlice sorted_anchors =
                    state.sorted_in_scratch ? anchors_scratch_s : anchors_s;
                const GpuArenaSlice compacted_anchors_s = state.compacted_anchors_slice();
                const std::array<VkDescriptorBufferInfo, 6> range_infos{{
                    {sorted_anchors.buffer->buffer, sorted_anchors.offset, sorted_anchors.size},
                    {state.range_count_buf.buffer, 0, state.range_count_buf.bytes},
                    {state.ranges_buf.buffer, 0, state.ranges_buf.bytes},
                    {state.range_count_buf.buffer, 0, state.range_count_buf.bytes},
                    {compacted_anchors_s.buffer->buffer, compacted_anchors_s.offset,
                     compacted_anchors_s.size},
                    {state.span_buf.buffer, 0, state.span_buf.bytes},
                }};
                ChainRangePushLocal range_push{
                    .query_count = 1u,
                    .max_ref_gap =
                        static_cast<std::uint32_t>(std::max<std::int32_t>(max_dist_x, 0)),
                };
                record_phase_c_dispatch_locked(
                    command_buffer_, range_pipe, range_infos,
                    &range_push, sizeof(range_push), 1u);
                run.stats.range_build_dispatches += 1u;
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord_batch2)");

            std::uint32_t chain_active_count = 0;
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.has_anchors) {
                    continue;
                }
                std::memcpy(&state.range_count, state.range_count_buf.mapped, 4u);
                std::memcpy(
                    &state.compacted_anchor_count,
                    static_cast<std::byte*>(state.range_count_buf.mapped) + 4u,
                    4u);
                if (state.range_count == 0u) {
                    continue;
                }
                if (state.compacted_anchor_count > state.total_anchors_u32) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch compacted anchor overflow");
                }
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                state.active_anchor_count =
                    use_compacted_anchors ? state.compacted_anchor_count
                                          : state.total_anchors_u32;
                state.score_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::int32_t),
                    kPhaseBBufferUsage);
                state.predecessor_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::int64_t),
                    kPhaseBBufferUsage);
                state.visited_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::uint32_t),
                    kPhaseBBufferUsage);
                state.trace_buf = create_device_buffer(
                    std::uint64_t{state.active_anchor_count} * sizeof(std::uint32_t),
                    kPhaseBBufferUsage);
                state.result_buf =
                    create_device_buffer(sizeof(GpuCoordinateOnlyResultWire), kPhaseBBufferUsage);
                if (state.score_buf.mapped == nullptr
                    || state.predecessor_buf.mapped == nullptr
                    || state.visited_buf.mapped == nullptr
                    || state.trace_buf.mapped == nullptr
                    || state.result_buf.mapped == nullptr) {
                    throw VulkanRuntimeError(
                        "phase_c seed-to-coordinate batch non-unified chain memory");
                }
                state.chain_active = true;
                chain_active_count += 1u;
            }

            if (chain_active_count == 0u) {
                arena.rewind(mark);
                submitted_waves += 1u;
                run.stats.wave_count += 1u;
                wave_begin = next_wave_begin;
                continue;
            }

            reset_phase_c_pool_locked();
            check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch3)");
            begin = VkCommandBufferBeginInfo{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord_batch3)");
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                std::memset(
                    state.score_buf.mapped, 0,
                    static_cast<std::size_t>(state.score_buf.bytes));
                std::memset(
                    state.predecessor_buf.mapped, 0xff,
                    static_cast<std::size_t>(state.predecessor_buf.bytes));
                std::memset(
                    state.visited_buf.mapped, 0,
                    static_cast<std::size_t>(state.visited_buf.bytes));
                std::memset(
                    state.result_buf.mapped, 0,
                    static_cast<std::size_t>(state.result_buf.bytes));
            }
            record_host_to_shader(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                GpuArenaSlice anchors_s = state.anchors_slice();
                GpuArenaSlice anchors_scratch_s = state.anchors_scratch_slice();
                GpuArenaSlice compacted_anchors_s = state.compacted_anchors_slice();
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                const GpuArenaSlice active_anchors = use_compacted_anchors
                    ? compacted_anchors_s
                    : (state.sorted_in_scratch ? anchors_scratch_s : anchors_s);
                const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
                    {active_anchors.buffer->buffer, active_anchors.offset, active_anchors.size},
                    {state.ranges_buf.buffer, 0, state.ranges_buf.bytes},
                    {state.score_buf.buffer, 0, state.score_buf.bytes},
                    {state.predecessor_buf.buffer, 0, state.predecessor_buf.bytes},
                }};
                VkDescriptorSet chain_set = alloc_phase_c_set_locked(descriptor_set_layout_);
                std::array<VkWriteDescriptorSet, 4> writes{};
                for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
                    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[binding].dstSet = chain_set;
                    writes[binding].dstBinding = binding;
                    writes[binding].descriptorCount = 1;
                    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[binding].pBufferInfo = &chain_infos[binding];
                }
                vkUpdateDescriptorSets(
                    device_, static_cast<std::uint32_t>(writes.size()),
                    writes.data(), 0, nullptr);

                ChainingPushConstants chain_push{};
                chain_push.anchor_count = state.active_anchor_count;
                chain_push.range_count = state.range_count;
                chain_push.max_dist_x = max_dist_x;
                chain_push.max_dist_y = max_dist_y;
                chain_push.bandwidth = bandwidth;
                chain_push.max_iter = max_iter;
                chain_push.gap_penalty = gap_penalty;
                chain_push.skip_penalty = skip_penalty;
                chain_push.max_chain_skip = max_chain_skip;
                const std::uint32_t chain_groups =
                    std::min(state.range_count, features_.max_workgroup_count_x);
                vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, chain_pipeline);
                vkCmdBindDescriptorSets(
                    command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_layout_, 0, 1, &chain_set, 0, nullptr);
                vkCmdPushConstants(
                    command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(chain_push), &chain_push);
                vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
                run.stats.chain_dispatches += 1u;
            }
            record_compute_barrier(command_buffer_);
            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (!state.chain_active) {
                    continue;
                }
                GpuArenaSlice anchors_s = state.anchors_slice();
                GpuArenaSlice anchors_scratch_s = state.anchors_scratch_slice();
                GpuArenaSlice compacted_anchors_s = state.compacted_anchors_slice();
                const bool use_compacted_anchors =
                    state.compacted_anchor_count != 0u
                    && state.compacted_anchor_count < state.total_anchors_u32;
                const GpuArenaSlice active_anchors = use_compacted_anchors
                    ? compacted_anchors_s
                    : (state.sorted_in_scratch ? anchors_scratch_s : anchors_s);
                const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
                    {active_anchors.buffer->buffer, active_anchors.offset, active_anchors.size},
                    {state.score_buf.buffer, 0, state.score_buf.bytes},
                    {state.predecessor_buf.buffer, 0, state.predecessor_buf.bytes},
                    {state.visited_buf.buffer, 0, state.visited_buf.bytes},
                    {state.trace_buf.buffer, 0, state.trace_buf.bytes},
                    {state.result_buf.buffer, 0, state.result_buf.bytes},
                }};
                GpuCoordinateReducePush coord_push{
                    .anchor_count = state.active_anchor_count,
                    .query_len_lo = static_cast<std::uint32_t>(state.query_len & 0xffffffffull),
                    .query_len_hi = static_cast<std::uint32_t>(state.query_len >> 32),
                    .min_chain_score = min_chain_score,
                    .min_seed_count = min_seed_count,
                    .probe_limit = probe_limit,
                    .reserved = 0u,
                };
                record_phase_c_dispatch_locked(
                    command_buffer_, coord_pipe, coord_infos,
                    &coord_push, sizeof(coord_push), 1u);
                run.stats.coordinate_reduce_dispatches += 1u;
            }
            record_shader_to_host(command_buffer_);
            submit_and_wait(
                device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord_batch3)");

            for (PhaseCSeedCoordinateWaveState& state : states) {
                if (state.chain_active) {
                    std::memcpy(
                        &run.results[state.result_index],
                        state.result_buf.mapped,
                        sizeof(GpuCoordinateOnlyResultWire));
                }
            }

            arena.rewind(mark);
            submitted_waves += 1u;
            run.stats.wave_count += 1u;
            wave_begin = next_wave_begin;
        } catch (...) {
            arena.rewind(mark);
            throw;
        }
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_batch_post)");
    if (submitted_waves != 0u) {
        chaining_dispatch_count_.fetch_add(submitted_waves, std::memory_order_relaxed);
    }
    return run;
}

std::vector<GpuCoordinateOnlyResultWire>
Runtime::run_phase_c_seed_to_coordinate_batch_roundtrip(
    const PhaseCSeedQueryInputs& query_inputs,
    std::span<const PhaseCSeedCoordinateJob> jobs,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    const ChainingPipelineKey& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit)
{
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate query length exceeds ABI");
    }
    const PhaseCSeedQueryRecordWire qrec{
        .minimizer_offset = 0u,
        .minimizer_count = query_inputs.minimizer_count,
        .query_len = static_cast<std::uint32_t>(query_len),
        .flags = 0u,
    };
    PhaseCSeedQueryInputs forwarded = query_inputs;
    forwarded.host_query_records =
        std::span<const PhaseCSeedQueryRecordWire>{&qrec, 1u};
    forwarded.device_query_records = nullptr;
    return run_phase_c_seed_to_coordinate_multi_query_batch_roundtrip(
        forwarded,
        jobs,
        kmer_size,
        max_occ,
        chain_key,
        max_dist_x,
        max_dist_y,
        bandwidth,
        max_iter,
        gap_penalty,
        skip_penalty,
        max_chain_skip,
        min_chain_score,
        min_seed_count,
        probe_limit).results;
}

GpuCoordinateOnlyResultWire Runtime::run_phase_c_seed_to_coordinate_roundtrip(
    const PhaseCSeedInputs& inputs,
    std::uint64_t query_len,
    std::uint64_t target_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ,
    const ChainingPipelineKey& chain_key,
    std::int32_t max_dist_x,
    std::int32_t max_dist_y,
    std::int32_t bandwidth,
    std::int32_t max_iter,
    float gap_penalty,
    float skip_penalty,
    std::int32_t max_chain_skip,
    std::int32_t min_chain_score,
    std::int32_t min_seed_count,
    std::uint32_t probe_limit)
{
    std::lock_guard<std::mutex> guard(mu_);
    GpuCoordinateOnlyResultWire result{};
    if (!features_.available) {
        throw VulkanRuntimeError("run_phase_c_seed_to_coordinate_roundtrip on unavailable device");
    }
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate missing resident index buffers");
    }
    if (query_len > std::numeric_limits<std::uint32_t>::max()) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate query length exceeds ABI");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    if (mc == 0u || max_occ < 0) return result;

    struct ChainRangePushLocal {
        std::uint32_t query_count;
        std::uint32_t max_ref_gap;
    };
    static_assert(sizeof(ChainRangePushLocal) == 8);

    GpuArena& arena = phase_c_arena();
    const std::uint32_t num_blocks = ceil_div_u32(mc, kPhaseCScanBlock);
    const std::uint64_t min_words_bytes = inputs.device_minimizer_words == nullptr
        ? std::max<std::uint64_t>(
              minimizer_device_bytes(
                  inputs.minimizer_word_kind, inputs.minimizer_words32_per_record, mc),
              4u)
        : 0u;
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u
        + 16u + min_words_bytes + 6u * std::uint64_t{kPhaseCSlotOffsetAlign};
    if (!arena.ensure_capacity(dt, 0, 0)) {
        throw VulkanRuntimeError("phase_c seed-to-coordinate arena grow failed");
    }
    const GpuArenaMark mark = arena.mark();

    const GpuArenaSlice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const GpuArenaSlice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const GpuArenaSlice blocks_s = arena.allocate_device(
        std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const GpuArenaSlice qrec_s = arena.allocate_device(16u, 16u);
    GpuArenaSlice minw_s{};
    if (inputs.device_minimizer_words == nullptr) {
        minw_s = arena.allocate_device(min_words_bytes, 16u);
    }
    if (!counts_s.valid() || !prefix_s.valid() || !blocks_s.valid() || !qrec_s.valid()
        || (inputs.device_minimizer_words == nullptr && !minw_s.valid())) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed-to-coordinate arena oversubscribed");
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    const VkBuffer min_words_buffer = inputs.device_minimizer_words != nullptr
        ? inputs.device_minimizer_words->buffer
        : minw_s.buffer->buffer;
    const std::uint64_t min_words_offset =
        inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
    const std::uint64_t min_words_range =
        inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
    if (inputs.device_minimizer_words == nullptr) {
        std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
        if (!inputs.host_minimizer_bytes.empty()) {
            std::memcpy(base + minw_s.offset,
                        inputs.host_minimizer_bytes.data(),
                        inputs.host_minimizer_bytes.size());
        }
    }
    const HostQRec qrec{0u, mc, static_cast<std::uint32_t>(query_len), 0u};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const PhaseBPipeline count_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_count, 128u, 1u);
    const PhaseBPipeline emit_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_emit, 128u, 1u);
    const PhaseBPipeline scanb =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scana =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
    const PhaseBPipeline range_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_chain_range_build, 1u, 1u);
    const PhaseBPipeline coord_pipe =
        get_phase_b_pipeline_locked(ShaderKind::phase_c_coordinate_reduce, 128u, 1u);
    const VkPipeline chain_pipeline = create_pipeline_locked(chain_key);

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const PhaseCAnchorSortKeyPlan anchor_sort_plan =
        phase_c_anchor_sort_key_plan(
            inputs.target_len != 0u ? inputs.target_len : target_len,
            inputs.target_position_offset);
    SeedPushC cpush{inputs.minimizer_words32_per_record, inputs.minimizer_word_kind,
        inputs.hash_shift, inputs.pos_bits, kmer_size, inputs.slot_mask,
        static_cast<std::uint32_t>(max_occ), 0u, 1u, inputs.bucket_count,
        target_offset_lo, target_offset_hi, anchor_sort_plan.target_ref_bits};
    const std::uint32_t seed_groups = std::max<std::uint32_t>(
        1u,
        std::min(ceil_div_u32(mc, 128u), features_.max_workgroup_count_x));

    {
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer, blocks_s.offset, blocks_s.size},
        }};
        ScanPushC s0{mc, kPhaseCScanBlock, num_blocks, 0u};
        ScanPushC s1{mc, kPhaseCScanBlock, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord1)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord1)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), seed_groups);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, scanb, scan_infos, &s0, sizeof(s0), num_blocks);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, scana, scan_infos, &s0, sizeof(s0), 1u);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, scana, scan_infos, &s1, sizeof(s1), ceil_div_u32(mc, 128u));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord1)");
    }

    std::uint32_t plast = 0, clast = 0;
    std::memcpy(&plast, base + prefix_s.offset + (mc - 1u) * 4u, 4);
    std::memcpy(&clast, base + counts_s.offset + (mc - 1u) * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(plast) + clast;
    if (total == 0u) {
        arena.rewind(mark);
        return result;
    }
    if (total > std::numeric_limits<std::uint32_t>::max()) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed-to-coordinate anchor count overflow");
    }
    const auto total_u32 = static_cast<std::uint32_t>(total);

    const std::uint64_t anchor_u64 = total * 3u;
    const WgPlan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    Buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer akeys_buf = create_device_buffer(total * 8u, kPhaseBBufferUsage);
    Buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer akeys_scr_buf = create_device_buffer(total * 8u, kPhaseBBufferUsage);
    Buffer ahist_buf = create_device_buffer(
        std::uint64_t{awg.num_workgroups} * 256u * 4u, kPhaseBBufferUsage);
    Buffer ranges_buf = create_device_buffer(
        std::max<std::uint64_t>(total * sizeof(GpuChainRangeWire), 8u),
        kPhaseBBufferUsage);
    Buffer range_count_buf = create_device_buffer(8u, kPhaseBBufferUsage);
    Buffer compacted_anchors_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer span_buf = create_device_buffer(sizeof(GpuChainRangeBuildSpanWire), kPhaseBBufferUsage);
    if (anchors_buf.mapped == nullptr || ranges_buf.mapped == nullptr
        || range_count_buf.mapped == nullptr || compacted_anchors_buf.mapped == nullptr
        || span_buf.mapped == nullptr) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed-to-coordinate non-unified memory");
    }
    const GpuArenaSlice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const GpuArenaSlice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const GpuArenaSlice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const GpuArenaSlice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const GpuArenaSlice ahist_s{&ahist_buf, 0, ahist_buf.bytes};
    const GpuArenaSlice compacted_anchors_s{
        &compacted_anchors_buf, 0, compacted_anchors_buf.bytes};

    const GpuChainRangeBuildSpanWire span{
        .anchor_lo = 0u,
        .anchor_hi = total_u32,
        .range_lo = 0u,
        .range_out = 0u,
    };
    std::memcpy(span_buf.mapped, &span, sizeof(span));
    std::memset(range_count_buf.mapped, 0, 8u);

    bool sorted_in_scratch = false;
    {
        reset_phase_c_pool_locked();
        SeedPushC epush = cpush;
        epush.emit_mode = 1u;
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {anchors_s.buffer->buffer, anchors_s.offset, anchors_s.size},
            {akeys_s.buffer->buffer, akeys_s.offset, akeys_s.size},
        }};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord2)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord2)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), seed_groups);
        record_compute_barrier(command_buffer_);
        GpuSortPlan sort_plan{total, 8u, 24u, 8u, anchor_sort_plan.pass_count};
        record_radix_sort_locked(
            command_buffer_, akeys_s, anchors_s, akeys_scr_s, anchors_scr_s, ahist_s,
            sort_plan, awg.num_workgroups, awg.elems_per_wg, 3u, sorted_in_scratch);
        const GpuArenaSlice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
        const std::array<VkDescriptorBufferInfo, 6> range_infos{{
            {sorted_anchors.buffer->buffer, sorted_anchors.offset, sorted_anchors.size},
            {range_count_buf.buffer, 0, range_count_buf.bytes},
            {ranges_buf.buffer, 0, ranges_buf.bytes},
            {range_count_buf.buffer, 0, range_count_buf.bytes},
            {compacted_anchors_s.buffer->buffer, compacted_anchors_s.offset,
             compacted_anchors_s.size},
            {span_buf.buffer, 0, span_buf.bytes},
        }};
        ChainRangePushLocal rpush{
            .query_count = 1u,
            .max_ref_gap = static_cast<std::uint32_t>(std::max<std::int32_t>(max_dist_x, 0)),
        };
        record_phase_c_dispatch_locked(
            command_buffer_, range_pipe, range_infos, &rpush, sizeof(rpush), 1u);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord2)");
    }

    std::uint32_t range_count = 0;
    std::uint32_t compacted_anchor_count = 0;
    std::memcpy(&range_count, range_count_buf.mapped, 4u);
    std::memcpy(&compacted_anchor_count, static_cast<std::byte*>(range_count_buf.mapped) + 4u, 4u);
    if (range_count == 0u) {
        arena.rewind(mark);
        return result;
    }
    if (compacted_anchor_count > total_u32) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed-to-coordinate compacted anchor overflow");
    }
    const bool use_compacted_anchors =
        compacted_anchor_count != 0u && compacted_anchor_count < total_u32;
    const std::uint32_t active_anchor_count =
        use_compacted_anchors ? compacted_anchor_count : total_u32;

    Buffer score_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::int32_t), kPhaseBBufferUsage);
    Buffer pred_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::int64_t), kPhaseBBufferUsage);
    Buffer visited_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::uint32_t), kPhaseBBufferUsage);
    Buffer trace_buf = create_device_buffer(
        std::uint64_t{active_anchor_count} * sizeof(std::uint32_t), kPhaseBBufferUsage);
    Buffer result_buf = create_device_buffer(sizeof(GpuCoordinateOnlyResultWire), kPhaseBBufferUsage);
    const GpuArenaSlice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    const GpuArenaSlice& active_anchors =
        use_compacted_anchors ? compacted_anchors_s : sorted_anchors;

    {
        reset_phase_c_pool_locked();
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord3)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed_coord3)");

        std::memset(score_buf.mapped, 0, static_cast<std::size_t>(score_buf.bytes));
        std::memset(pred_buf.mapped, 0xff, static_cast<std::size_t>(pred_buf.bytes));
        std::memset(visited_buf.mapped, 0, static_cast<std::size_t>(visited_buf.bytes));
        std::memset(result_buf.mapped, 0, static_cast<std::size_t>(result_buf.bytes));
        record_host_to_shader(command_buffer_);

        const std::array<VkDescriptorBufferInfo, 4> chain_infos{{
            {active_anchors.buffer->buffer, active_anchors.offset, active_anchors.size},
            {ranges_buf.buffer, 0, ranges_buf.bytes},
            {score_buf.buffer, 0, score_buf.bytes},
            {pred_buf.buffer, 0, pred_buf.bytes},
        }};
        VkDescriptorSet chain_set = alloc_phase_c_set_locked(descriptor_set_layout_);
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t b = 0; b < 4; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = chain_set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &chain_infos[b];
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        ChainingPushConstants chain_push{};
        chain_push.anchor_count = active_anchor_count;
        chain_push.range_count = range_count;
        chain_push.max_dist_x = max_dist_x;
        chain_push.max_dist_y = max_dist_y;
        chain_push.bandwidth = bandwidth;
        chain_push.max_iter = max_iter;
        chain_push.gap_penalty = gap_penalty;
        chain_push.skip_penalty = skip_penalty;
        chain_push.max_chain_skip = max_chain_skip;
        const std::uint32_t chain_groups =
            std::min(range_count, features_.max_workgroup_count_x);

        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, chain_pipeline);
        vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout_, 0, 1, &chain_set, 0, nullptr);
        vkCmdPushConstants(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(chain_push), &chain_push);
        vkCmdDispatch(command_buffer_, chain_groups, 1, 1);
        record_compute_barrier(command_buffer_);

        const std::array<VkDescriptorBufferInfo, 6> coord_infos{{
            {active_anchors.buffer->buffer, active_anchors.offset, active_anchors.size},
            {score_buf.buffer, 0, score_buf.bytes},
            {pred_buf.buffer, 0, pred_buf.bytes},
            {visited_buf.buffer, 0, visited_buf.bytes},
            {trace_buf.buffer, 0, trace_buf.bytes},
            {result_buf.buffer, 0, result_buf.bytes},
        }};
        GpuCoordinateReducePush coord_push{
            .anchor_count = active_anchor_count,
            .query_len_lo = static_cast<std::uint32_t>(query_len & 0xffffffffull),
            .query_len_hi = static_cast<std::uint32_t>(query_len >> 32),
            .min_chain_score = min_chain_score,
            .min_seed_count = min_seed_count,
            .probe_limit = probe_limit,
            .reserved = 0u,
        };
        record_phase_c_dispatch_locked(
            command_buffer_, coord_pipe, coord_infos,
            &coord_push, sizeof(coord_push), 1u);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed_coord3)");
    }

    std::memcpy(&result, result_buf.mapped, sizeof(result));
    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed_coord_post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

SeedLookupResult Runtime::run_phase_c_seed_roundtrip(
    const PhaseCSeedInputs& inputs,
    std::uint64_t query_len,
    std::uint32_t kmer_size,
    std::int32_t max_occ)
{
    std::lock_guard<std::mutex> guard(mu_);
    SeedLookupResult result;
    if (!features_.available) throw VulkanRuntimeError("run_phase_c_seed_roundtrip on unavailable device");
    if (inputs.slots == nullptr || inputs.positions == nullptr) {
        throw VulkanRuntimeError("phase_c seed missing resident index buffers");
    }
    const std::uint32_t mc = inputs.minimizer_count;
    if (mc == 0u || max_occ < 0) return result;

    GpuArena& arena = phase_c_arena();
    const std::uint32_t num_blocks = ceil_div_u32(mc, kPhaseCScanBlock);
    const std::uint64_t min_words_bytes = inputs.device_minimizer_words == nullptr
        ? std::max<std::uint64_t>(minimizer_device_bytes(inputs.minimizer_word_kind, inputs.minimizer_words32_per_record, mc), 4u)
        : 0u;
    // Each allocate_device call rounds the offset up to kPhaseCSlotOffsetAlign
    // (256), so the five sub-allocations below can need up to 5*256 bytes of
    // alignment padding on top of their byte footprint. Reserve enough slack for
    // that — a flat +256 (the old value) overflowed the arena on a cold call
    // (the last allocation, minw_s, came back invalid and minw_s.buffer->buffer
    // null-dereferenced), masked only because a warmer arena had grown larger.
    std::uint64_t dt = std::uint64_t{mc} * 4u * 2u + std::uint64_t{num_blocks} * 4u + 16u
        + min_words_bytes + 6u * std::uint64_t{kPhaseCSlotOffsetAlign};
    if (!arena.ensure_capacity(dt, 0, 0)) throw VulkanRuntimeError("phase_c seed arena grow failed");
    const GpuArenaMark mark = arena.mark();

    const GpuArenaSlice counts_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const GpuArenaSlice prefix_s = arena.allocate_device(std::uint64_t{mc} * 4u, 16u);
    const GpuArenaSlice blocks_s = arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const GpuArenaSlice qrec_s = arena.allocate_device(16u, 16u);
    GpuArenaSlice minw_s{};
    if (inputs.device_minimizer_words == nullptr) minw_s = arena.allocate_device(min_words_bytes, 16u);
    if (!counts_s.valid() || !prefix_s.valid() || !blocks_s.valid() || !qrec_s.valid()
        || (inputs.device_minimizer_words == nullptr && !minw_s.valid())) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c seed arena oversubscribed");
    }
    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    const VkBuffer min_words_buffer = inputs.device_minimizer_words != nullptr
        ? inputs.device_minimizer_words->buffer
        : minw_s.buffer->buffer;
    const std::uint64_t min_words_offset = inputs.device_minimizer_words != nullptr ? 0u : minw_s.offset;
    const std::uint64_t min_words_range = inputs.device_minimizer_words != nullptr ? VK_WHOLE_SIZE : minw_s.size;
    if (inputs.device_minimizer_words == nullptr) {
        std::memset(base + minw_s.offset, 0, static_cast<std::size_t>(minw_s.size));
        if (!inputs.host_minimizer_bytes.empty()) {
            std::memcpy(base + minw_s.offset, inputs.host_minimizer_bytes.data(), inputs.host_minimizer_bytes.size());
        }
    }
    const HostQRec qrec{0u, mc, static_cast<std::uint32_t>(query_len), 0u};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const PhaseBPipeline count_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_count, 128u, 1u);
    const PhaseBPipeline emit_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_emit, 128u, 1u);
    const PhaseBPipeline scanb = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scana = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);

    const std::uint32_t target_offset_lo =
        static_cast<std::uint32_t>(inputs.target_position_offset & 0xffffffffull);
    const std::uint32_t target_offset_hi =
        static_cast<std::uint32_t>(inputs.target_position_offset >> 32);
    const PhaseCAnchorSortKeyPlan anchor_sort_plan =
        phase_c_anchor_sort_key_plan(inputs.target_len, inputs.target_position_offset);
    SeedPushC cpush{inputs.minimizer_words32_per_record, inputs.minimizer_word_kind,
        inputs.hash_shift, inputs.pos_bits, kmer_size, inputs.slot_mask,
        static_cast<std::uint32_t>(max_occ), 0u, 1u, inputs.bucket_count,
        target_offset_lo, target_offset_hi, anchor_sort_plan.target_ref_bits};
    const std::uint32_t seed_groups = std::max<std::uint32_t>(
        1u,
        std::min(ceil_div_u32(mc, 128u), features_.max_workgroup_count_x));

    // --- Submit 1: seed count + prefix scan ---
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_seed_count");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},  // anchors (unused in count)
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},  // anchor_keys (unused in count)
        }};
        const std::array<VkDescriptorBufferInfo, 3> scan_infos{{
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {blocks_s.buffer->buffer, blocks_s.offset, blocks_s.size},
        }};
        ScanPushC s0{mc, kPhaseCScanBlock, num_blocks, 0u};
        ScanPushC s1{mc, kPhaseCScanBlock, num_blocks, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed1)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed1)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), seed_groups);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scanb, scan_infos, &s0, sizeof(s0), num_blocks);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scana, scan_infos, &s0, sizeof(s0), 1u);
        record_compute_barrier(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, scana, scan_infos, &s1, sizeof(s1), ceil_div_u32(mc, 128u));
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed1)");
    }
    std::uint32_t plast = 0, clast = 0;
    std::memcpy(&plast, base + prefix_s.offset + (mc - 1u) * 4u, 4);
    std::memcpy(&clast, base + counts_s.offset + (mc - 1u) * 4u, 4);
    const std::uint64_t total = static_cast<std::uint64_t>(plast) + clast;
    if (total == 0u) { arena.rewind(mark); return result; }
    if (total > std::numeric_limits<std::uint32_t>::max()) { arena.rewind(mark); throw VulkanRuntimeError("phase_c seed anchor count overflow"); }

    // --- Submit 2: seed emit + device anchor sort by ref_bits ---
    // The exact anchor count is only known after the count submit, so the anchor
    // and sort-scratch buffers are sized exactly here as standalone device
    // buffers (the count/prefix arena slices from submit 1 must stay live, so the
    // arena is never grown mid-op). They are wrapped as offset-0 GpuArenaSlices
    // for the shared dispatch/sort recorders.
    const std::uint64_t anchor_u64 = total * 3u;          // 24-byte anchors as 3 u64
    const WgPlan awg = plan_workgroups(total, features_.max_workgroup_count_x);
    const std::uint64_t exact_reserve =
        phase_c_seed_exact_reserved_bytes(total, awg.num_workgroups);
    auto exact_permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(exact_reserve);
    if (!exact_permit) {
        auto& admission =
            bioseq::parallelism::MemoryAdmissionController::instance();
        arena.rewind(mark);
        throw VulkanRuntimeError(fmt::format(
            "phase_c seed exact anchor memory admission failed for {} bytes "
            "(anchors={} workgroups={} reserved={} allocatable={})",
            exact_reserve,
            total,
            awg.num_workgroups,
            admission.reserved_bytes(),
            admission.allocatable_after_reservations()));
    }
    Buffer anchors_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer akeys_buf = create_device_buffer(total * 8u, kPhaseBBufferUsage);
    Buffer anchors_scr_buf = create_device_buffer(anchor_u64 * 8u, kPhaseBBufferUsage);
    Buffer akeys_scr_buf = create_device_buffer(total * 8u, kPhaseBBufferUsage);
    Buffer ahist_buf = create_device_buffer(std::uint64_t{awg.num_workgroups} * 256u * 4u, kPhaseBBufferUsage);
    if (anchors_buf.mapped == nullptr) { arena.rewind(mark); throw VulkanRuntimeError("phase_c seed non-unified memory"); }
    const GpuArenaSlice anchors_s{&anchors_buf, 0, anchors_buf.bytes};
    const GpuArenaSlice akeys_s{&akeys_buf, 0, akeys_buf.bytes};
    const GpuArenaSlice anchors_scr_s{&anchors_scr_buf, 0, anchors_scr_buf.bytes};
    const GpuArenaSlice akeys_scr_s{&akeys_scr_buf, 0, akeys_scr_buf.bytes};
    const GpuArenaSlice ahist_s{&ahist_buf, 0, ahist_buf.bytes};

    bool sorted_in_scratch = false;
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_seed_emit");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        SeedPushC epush = cpush; epush.emit_mode = 1u;
        const std::array<VkDescriptorBufferInfo, 8> seed_infos{{
            {min_words_buffer, min_words_offset, min_words_range},
            {inputs.slots->buffer, 0, VK_WHOLE_SIZE},
            {inputs.positions->buffer, 0, VK_WHOLE_SIZE},
            {qrec_s.buffer->buffer, qrec_s.offset, qrec_s.size},
            {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
            {prefix_s.buffer->buffer, prefix_s.offset, prefix_s.size},
            {anchors_s.buffer->buffer, anchors_s.offset, anchors_s.size},
            {akeys_s.buffer->buffer, akeys_s.offset, akeys_s.size},
        }};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed2)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_seed2)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), seed_groups);
        record_compute_barrier(command_buffer_);
        GpuSortPlan sort_plan{total, 8u, 24u, 8u, anchor_sort_plan.pass_count};
        record_radix_sort_locked(command_buffer_, akeys_s, anchors_s, akeys_scr_s, anchors_scr_s, ahist_s,
                                 sort_plan, awg.num_workgroups, awg.elems_per_wg, 3u, sorted_in_scratch);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_seed2)");
    }
    const GpuArenaSlice& sorted_anchors = sorted_in_scratch ? anchors_scr_s : anchors_s;
    result.count = total;
    result.host_bytes.resize(static_cast<std::size_t>(total * 24u));
    std::memcpy(result.host_bytes.data(),
                static_cast<const std::byte*>(sorted_anchors.buffer->mapped) + sorted_anchors.offset,
                static_cast<std::size_t>(total * 24u));

    arena.rewind(mark);
    check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_seed-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

}  // namespace bioseq::gpu::vulkan
