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
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
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
#include <dispatch/dispatch.h>
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime;

export import bioseq.gpu.vulkan.shader_cache;
export import bioseq.gpu.vulkan.shader_layout;
export import bioseq.gpu.vulkan.shader_variants;
import bioseq.common.mlogging;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.phase_b_abi;
import bioseq.gpu.vulkan.phase_c_abi;
import bioseq.instr;  // DOC0310 §21 — phase_c_* sub-region timing labels

export namespace bioseq::gpu::vulkan {

class Runtime;

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
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    std::uint32_t minimizer_count = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32_per_record = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t bucket_count = 0;  // >0 ⇒ slots is a hash-sorted dense bucket array
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

    // DOC0307 Phase B — two-dispatch minimizer sketch (candidate + exact emit)
    // and seed-hit lookup (lookup + bitonic sort). Both record every H2D copy,
    // dispatch, barrier, and D2H copy under one queue lock, mirroring the Phase A
    // roundtrip discipline. Transient buffers are owned internally; resident
    // index buffers and memory-budget permits are owned by the caller.
    RuntimeSketchResult run_minimizer_sketch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_minimizers);
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

    // DOC0310 Phase C — parallel ordered tile sketch (§9). Drop-in replacement
    // for `run_minimizer_sketch_roundtrip`: same inputs/result, but the emit is
    // tile-parallel (no one-workgroup-per-sequence serial pass).
    RuntimeSketchResult run_phase_c_sketch_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_minimizers);

    // DOC0310 Phase C — build the target minimizer index entirely on device
    // (§12): parallel sketch → device radix sort → race-free GPU slot table.
    // `max_target_minimizers` bounds the pessimistic device allocation.
    GpuBuiltIndex run_phase_c_build_target_index(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_target_minimizers);

    // DOC0345 Patch B — GPU per-segment sorted-pair seam: tiled `emit_mode=2`
    // emit of (kmer, encoded_pos) + a GPU two-key stable radix sort (by
    // encoded_pos then kmer → (kmer, encoded_pos) order). RETAINED but currently
    // BYPASSED: the tiled emit over-emits ~8% for dna5, so the shipped index
    // builders emit via the non-tiled Phase B sketch and sort the pairs on the
    // host instead (see DOC0348). This entry point re-enables once the tiled
    // emit is fixed.
    GpuSortedPairRun run_phase_c_sketch_sort_pairs_roundtrip(
        std::span<const std::byte> packed_sequence,
        const VulkanRankLut& rank_lut,
        const VulkanSketchParams& params,
        std::uint64_t max_target_minimizers);

    // DOC0310 Phase C — GPU seed count + emit + device anchor sort (§14, §13).
    // Counts anchors on device (no host prepass), prefix-scans, emits anchors,
    // and stable-radix-sorts them by `Anchor::ref_bits()`. Returns the sorted
    // 24-byte anchor bytes.
    SeedLookupResult run_phase_c_seed_roundtrip(
        const PhaseCSeedInputs& inputs,
        std::uint64_t query_len,
        std::uint32_t kmer_size,
        std::int32_t max_occ);

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
    state.sel_utf8_string = objc.sel_register_name("UTF8String");
    state.sel_new_library_with_data_error = objc.sel_register_name("newLibraryWithData:error:");
    void* cls = objc.object_get_class(device);
    void* method = nullptr;
    while (cls != nullptr && method == nullptr) {
        method = objc.class_get_instance_method(cls, sel_source);
        cls = objc.class_get_superclass(cls);
    }
    if (method == nullptr) {
        std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: newLibraryWithSource:options:error: method NOT FOUND on device class\n");
        return;
    }

    state.objc = objc;
    state.metallib = std::move(metallib);
    state.original_new_library_with_source =
        objc.method_set_implementation(
            method,
            reinterpret_cast<void*>(&bioseq_moltenvk_new_library_with_source));
    state.installed = state.original_new_library_with_source != nullptr;
    std::fprintf(stderr, "[bioseq-gpu-diag] cmd-interpose: swizzle installed=%d original=%p\n",
                 state.installed ? 1 : 0, state.original_new_library_with_source);
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
    prepare_shader_provider_runtime();

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

    install_moltenvk_command_library_interpose();
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
    if (range_count > features_.max_workgroup_count_x) {
        throw VulkanRuntimeError(fmt::format(
            "range_count {} exceeds maxComputeWorkGroupCount[0] {}",
            range_count, features_.max_workgroup_count_x));
    }
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
    vkCmdDispatch(command_buffer_, range_count, 1, 1);

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

// ---------------------------------------------------------------------------
// DOC0307 Phase B — seeding roundtrips
// ---------------------------------------------------------------------------

namespace {

/// Push-constant blocks for the Phase B shaders. Each mirrors its GLSL `Params`
/// uniform byte-for-byte (see `push_constant_size` in shader_compiler.cppm).
struct SketchCandidatePush {
    std::uint32_t bp_size;
    std::uint32_t window_size;
    std::uint32_t kmer_size;
    std::uint32_t unit_bits;
    std::uint32_t bits_per_char;
    std::uint32_t letters_per_unit;
    std::uint32_t alphabet_size;
    std::uint32_t alphabet_kind;
    std::uint32_t seq_word_offset;
    std::uint32_t char_mask;
};
static_assert(sizeof(SketchCandidatePush) == 40);

struct SketchEmitPush {
    std::uint32_t bp_size;
    std::uint32_t window_size;
    std::uint32_t kmer_size;
    std::uint32_t pos_bits;
    std::uint32_t hash_bits;
    std::uint32_t hash_shift;
    std::uint32_t word_kind;
    std::uint32_t words32_per_minimizer;
    std::uint32_t span_value;
    std::uint32_t max_minimizers;
};
static_assert(sizeof(SketchEmitPush) == 40);

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

        // Pin the SSBO wire ABI: a candidate-stride / sort-range-stride mismatch
        // would silently corrupt the direct CandidateWire / SortRangeWire reads.
        const SpirvLayoutScan scan = scan_spirv_layout(spirv);
        if (kind == ShaderKind::minimizer_candidates
            && !scan.has_array_stride(candidate_wire_stride)) {
            vkDestroyDescriptorPool(device_, entry.descriptor_pool, nullptr);
            vkDestroyPipelineLayout(device_, entry.pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device_, entry.set_layout, nullptr);
            throw VulkanRuntimeError("SPIR-V Candidate array stride != candidate_wire_stride (32)");
        }
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
            "vkCreateComputePipelines(phase_b): VkResult {}", static_cast<int>(pr)));
    }

    bioseq_log<DBG>("[vulkan_runtime] built phase B pipeline kind={} local={} max_span={}",
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

RuntimeSketchResult Runtime::run_minimizer_sketch_roundtrip(
    std::span<const std::byte> packed_sequence,
    const VulkanRankLut& rank_lut,
    const VulkanSketchParams& params,
    std::uint64_t max_minimizers)
{
    std::lock_guard<std::mutex> guard(mu_);
    if (!features_.available) {
        throw VulkanRuntimeError("run_minimizer_sketch_roundtrip on unavailable device");
    }

    const std::uint32_t bp_size = params.bp_size;
    constexpr std::uint32_t candidate_local = 128u;
    const std::uint64_t candidate_groups =
        (static_cast<std::uint64_t>(bp_size) + candidate_local - 1u) / candidate_local;
    if (candidate_groups > features_.max_workgroup_count_x) {
        throw VulkanRuntimeError("minimizer sketch dispatch exceeds maxComputeWorkGroupCount[0]");
    }

    const PhaseBPipeline cand_pipe =
        get_phase_b_pipeline_locked(ShaderKind::minimizer_candidates, candidate_local, 256u);
    const PhaseBPipeline emit_pipe =
        get_phase_b_pipeline_locked(ShaderKind::minimizer_emit_exact, 1u, 256u);

    const std::uint64_t out_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(params.minimizer_word_kind, params.minimizer_words32, max_minimizers), 4u);
    const std::uint64_t cand_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(bp_size) * sizeof(CandidateWire), 32u);
    const std::uint64_t seq_bytes = std::max<std::uint64_t>(packed_sequence.size(), 4u);

    Buffer scratch = create_host_buffer(4u, kPhaseBBufferUsage);
    Buffer seq_buf = create_device_buffer(seq_bytes, kPhaseBBufferUsage);
    Buffer cand_buf = create_device_buffer(cand_bytes, kPhaseBBufferUsage);
    Buffer lut_buf = create_device_buffer(sizeof(VulkanRankLut), kPhaseBBufferUsage);
    Buffer out_buf = create_device_buffer(out_bytes, kPhaseBBufferUsage);
    Buffer counter_buf = create_device_buffer(8u, kPhaseBBufferUsage);

    upload_locked(scratch, seq_buf, packed_sequence, VK_ACCESS_SHADER_READ_BIT);
    upload_locked(scratch, lut_buf,
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(&rank_lut), sizeof(rank_lut)},
        VK_ACCESS_SHADER_READ_BIT);
    {
        const std::array<std::uint32_t, 2> zero{0u, 0u};
        upload_locked(scratch, counter_buf,
            std::as_bytes(std::span<const std::uint32_t>{zero.data(), zero.size()}),
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    {
        const std::array<VkBuffer, 3> cand_bufs{seq_buf.buffer, cand_buf.buffer, lut_buf.buffer};
        bind_phase_b_descriptors_locked(cand_pipe, cand_bufs);
        const std::array<VkBuffer, 3> emit_bufs{cand_buf.buffer, out_buf.buffer, counter_buf.buffer};
        bind_phase_b_descriptors_locked(emit_pipe, emit_bufs);
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(sketch)");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer(sketch)");

    VkMemoryBarrier host_to_shader{};
    host_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_shader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host_to_shader,
                         0, nullptr, 0, nullptr);

    const SketchCandidatePush cand_push{
        bp_size, params.window_size, params.kmer_size, params.unit_bits,
        params.bits_per_char, params.letters_per_unit, params.alphabet_size,
        params.alphabet_kind, params.seq_word_offset, params.char_mask};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, cand_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cand_pipe.pipeline_layout, 0, 1, &cand_pipe.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer_, cand_pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(cand_push), &cand_push);
    vkCmdDispatch(command_buffer_, static_cast<std::uint32_t>(candidate_groups), 1, 1);

    VkMemoryBarrier shader_to_shader{};
    shader_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_shader.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &shader_to_shader,
                         0, nullptr, 0, nullptr);

    const SketchEmitPush emit_push{
        bp_size, params.window_size, params.kmer_size, params.pos_bits, params.hash_bits,
        params.hash_shift, params.minimizer_word_kind, params.minimizer_words32,
        params.kmer_size, static_cast<std::uint32_t>(max_minimizers)};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, emit_pipe.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            emit_pipe.pipeline_layout, 0, 1, &emit_pipe.descriptor_set, 0, nullptr);
    vkCmdPushConstants(command_buffer_, emit_pipe.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(emit_push), &emit_push);
    vkCmdDispatch(command_buffer_, 1, 1, 1);

    VkMemoryBarrier shader_to_host{};
    shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &shader_to_host,
                         0, nullptr, 0, nullptr);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(sketch)");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(sketch)");
    check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(sketch)");
    check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(sketch)");

    std::array<std::uint32_t, 2> counter{0u, 0u};
    download_locked(scratch, counter_buf,
        std::as_writable_bytes(std::span<std::uint32_t>{counter.data(), counter.size()}));

    RuntimeSketchResult result;
    result.overflow = counter[1] != 0u;
    if (!result.overflow) {
        result.count = std::min<std::uint64_t>(counter[0], max_minimizers);
        const std::uint64_t host_bytes =
            minimizer_host_bytes(params.minimizer_word_kind, result.count);
        result.host_minimizer_bytes.resize(static_cast<std::size_t>(host_bytes));
        if (host_bytes > 0) {
            download_locked(scratch, out_buf, std::span<std::byte>{
                result.host_minimizer_bytes.data(), result.host_minimizer_bytes.size()});
        }
        result.minimizer_words = std::move(out_buf);
    }

    check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(sketch-post)");
    chaining_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    return result;
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
// candidate arrays (SKETCH_CAND_MAX = 1536 in the GLSL). 1280 leaves room for
// the w+k overlap. The sketch runs one workgroup per tile with this local size;
// all lanes hash candidates in parallel into shared memory.
constexpr std::uint32_t kPhaseCSketchTileBp = 256u;
constexpr std::uint32_t kPhaseCSketchLocal = 64u;
constexpr std::uint32_t kPhaseCSketchTileSpanMax = 384u;
constexpr std::uint32_t kPhaseCScanBlock = 256u;
constexpr std::uint32_t kPhaseCSlotOffsetAlign = 256u;

/// One tile descriptor (host mirror of the GLSL `TileC`): owned base range
/// `[tile_lo, tile_hi)` plus the left-context replay start and output base.
struct TileC {
    std::uint32_t seq_word_offset;
    std::uint32_t bp_count;
    std::uint32_t replay_lo;
    std::uint32_t tile_lo;
    std::uint32_t tile_hi;
    std::uint32_t out_base;
};
static_assert(sizeof(TileC) == 24);

struct SketchPushC {
    std::uint32_t emit_mode, window_size, kmer_size, hash_bits, hash_shift, pos_bits;
    std::uint32_t word_kind, words32_per_minimizer, span_value, alphabet_kind;
    std::uint32_t unit_bits, bits_per_char, letters_per_unit, char_mask;
    std::uint32_t tile_count, emit_pairs;
};
static_assert(sizeof(SketchPushC) == 64);

struct ScanPushC {
    std::uint32_t element_count, block_size, num_blocks, pass;
};
static_assert(sizeof(ScanPushC) == 16);

struct SeedPushC {
    std::uint32_t words32_per_minimizer, word_kind, hash_shift, pos_bits, kmer_size;
    std::uint32_t slot_mask, max_occ, emit_mode, record_count, bucket_count;
};
static_assert(sizeof(SeedPushC) == 40);

struct SortPushC {
    std::uint32_t element_count, radix_shift, num_workgroups, elems_per_wg, value_words;
};
static_assert(sizeof(SortPushC) == 20);

struct IndexPushC {
    std::uint32_t element_count, slot_mask, slot_capacity, num_buckets, probe_cap, pass;
};
static_assert(sizeof(IndexPushC) == 24);

struct HostQRec {
    std::uint32_t min_offset, min_count, anchor_offset, query_len;
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

/// Pick a sketch tile size so the per-sequence tile count (one workgroup per
/// tile) never exceeds the device dispatch limit. Grows past the default tile
/// for very long sequences (e.g. the XXL_HIFI 500 Mbp target would otherwise
/// need ~244k workgroups against MoltenVK's ~65535 cap). Each enlarged tile just
/// runs a longer serial replay; parallelism across tiles is preserved.
[[nodiscard]] std::uint32_t adaptive_sketch_tile_bp(
    std::uint32_t bp, std::uint32_t max_workgroups, std::uint32_t default_tile_bp) noexcept {
    std::uint32_t tile_bp = default_tile_bp;
    const std::uint32_t cap = max_workgroups > 1u ? max_workgroups - 1u : 1u;
    if (ceil_div_u32(bp, tile_bp) > cap) {
        tile_bp = ceil_div_u32(bp, cap);
    }
    // The cooperative sketch keeps the whole tile span (tile_bp + overlap) in
    // shared memory, so the tile cannot grow past the shared-array bound. If a
    // (tiny-dispatch-limit) device would need a larger tile, the caller's
    // `num_tiles > max_workgroup_count_x` guard declines Phase C.
    const std::uint32_t span_cap = kPhaseCSketchTileSpanMax - 64u;
    if (tile_bp > span_cap) tile_bp = span_cap;
    return std::max<std::uint32_t>(tile_bp, 1u);
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
    size.descriptorCount = 256u * 8u;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 256u;
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
    std::uint64_t max_minimizers)
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

    const std::uint32_t tile_bp =
        adaptive_sketch_tile_bp(bp, features_.max_workgroup_count_x, kPhaseCSketchTileBp);
    const std::uint32_t num_tiles = ceil_div_u32(bp, tile_bp);
    if (num_tiles > features_.max_workgroup_count_x) {
        throw VulkanRuntimeError("phase_c sketch tile count exceeds dispatch limit");
    }
    const std::uint32_t overlap = w + k;
    std::vector<TileC> tiles(num_tiles);
    for (std::uint32_t t = 0; t < num_tiles; ++t) {
        const std::uint32_t lo = t * tile_bp;
        const std::uint32_t hi = std::min(bp, (t + 1u) * tile_bp);
        const std::uint32_t rlo = lo > overlap ? lo - overlap : 0u;
        tiles[t] = TileC{params.seq_word_offset, bp, rlo, lo, hi, 0u};
    }
    const std::uint32_t num_blocks = ceil_div_u32(num_tiles, kPhaseCScanBlock);

    const std::uint64_t out_bytes = std::max<std::uint64_t>(
        minimizer_device_bytes(params.minimizer_word_kind, params.minimizer_words32, max_minimizers), 4u);
    Buffer out_words = create_device_buffer(out_bytes, kPhaseBBufferUsage);
    if (out_words.mapped == nullptr) throw VulkanRuntimeError("phase_c sketch non-unified memory");

    GpuArena& arena = phase_c_arena();
    const std::uint64_t seq_bytes = std::max<std::uint64_t>(packed_sequence.size(), 4u);
    const std::uint64_t device_total =
        seq_bytes + sizeof(VulkanRankLut) + std::uint64_t{num_tiles} * 24u
        + std::uint64_t{num_tiles} * 4u + std::uint64_t{num_tiles} * 4u
        + std::max<std::uint64_t>(num_blocks, 1u) * 4u + 8u + 16u
        + 8u * std::uint64_t{kPhaseCSlotOffsetAlign};
    if (!arena.ensure_capacity(device_total, 0, 0)) throw VulkanRuntimeError("phase_c arena grow failed");
    const GpuArenaMark mark = arena.mark();

    const GpuArenaSlice seq_s = arena.allocate_device(seq_bytes, 16u);
    const GpuArenaSlice lut_s = arena.allocate_device(sizeof(VulkanRankLut), 16u);
    const GpuArenaSlice tiles_s = arena.allocate_device(std::uint64_t{num_tiles} * 24u, 16u);
    const GpuArenaSlice counts_s = arena.allocate_device(std::uint64_t{num_tiles} * 4u, 16u);
    const GpuArenaSlice offsets_s = arena.allocate_device(std::uint64_t{num_tiles} * 4u, 16u);
    const GpuArenaSlice blocks_s = arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const GpuArenaSlice counters_s = arena.allocate_device(8u, 16u);
    const GpuArenaSlice pairvals_s = arena.allocate_device(16u, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid() || !counts_s.valid()
        || !offsets_s.valid() || !blocks_s.valid() || !counters_s.valid()
        || !pairvals_s.valid()) {
        arena.rewind(mark);
        throw VulkanRuntimeError("phase_c sketch arena oversub");
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(TileC));
    std::memset(base + counts_s.offset, 0, static_cast<std::size_t>(counts_s.size));
    std::memset(base + counters_s.offset, 0, 8);
    if (params.minimizer_word_kind == 0u) std::memset(out_words.mapped, 0, static_cast<std::size_t>(out_words.bytes));

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_sketch_tile, kPhaseCSketchLocal, std::max<std::uint32_t>(w, 1u));
    const PhaseBPipeline scan_block_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scan_add_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);
    reset_phase_c_pool_locked();

    const std::array<VkDescriptorBufferInfo, 8> sketch_infos{{
        {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
        {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
        {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
        {counts_s.buffer->buffer, counts_s.offset, counts_s.size},
        {out_words.buffer, 0, VK_WHOLE_SIZE},
        {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
        {offsets_s.buffer->buffer, offsets_s.offset, offsets_s.size},
        {pairvals_s.buffer->buffer, pairvals_s.offset, pairvals_s.size},
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
        num_tiles, 0u};
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
    record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), num_tiles);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_block_pipe, scan_infos, &scan0, sizeof(scan0), num_blocks);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_add_pipe, scan_infos, &scan0, sizeof(scan0), 1u);
    record_compute_barrier(command_buffer_);
    record_phase_c_dispatch_locked(command_buffer_, scan_add_pipe, scan_infos, &scan1, sizeof(scan1), ceil_div_u32(num_tiles, 128u));
    record_compute_barrier(command_buffer_);
    push.emit_mode = 1u;
    record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, sketch_infos, &push, sizeof(push), num_tiles);
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
    std::uint64_t max_target_minimizers)
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

    const std::uint32_t tile_bp =
        adaptive_sketch_tile_bp(bp, features_.max_workgroup_count_x, kPhaseCSketchTileBp);
    const std::uint32_t num_tiles = ceil_div_u32(bp, tile_bp);
    if (num_tiles > features_.max_workgroup_count_x) return out;
    const std::uint32_t overlap = w + k;
    std::vector<TileC> tiles(num_tiles);
    for (std::uint32_t t = 0; t < num_tiles; ++t) {
        const std::uint32_t lo = t * tile_bp;
        const std::uint32_t hi = std::min(bp, (t + 1u) * tile_bp);
        tiles[t] = TileC{params.seq_word_offset, bp, lo > overlap ? lo - overlap : 0u, lo, hi, 0u};
    }
    const std::uint32_t num_blocks = ceil_div_u32(num_tiles, kPhaseCScanBlock);

    // Small per-call scaffolding lives in the shared arena (seq + lut + tiles +
    // tile counts/offsets). The large minimizer/sort/index arrays are sized from
    // the EXACT minimizer count (downloaded after the count pass) as standalone
    // device buffers — never a single 14 GB arena buffer that Metal cannot
    // allocate for the XXL_HIFI 500 Mbp target.
    GpuArena& arena = phase_c_arena();
    const std::uint64_t seq_bytes = std::max<std::uint64_t>(packed_sequence.size(), 4u);
    std::uint64_t scaffold = seq_bytes + sizeof(VulkanRankLut)
        + std::uint64_t{num_tiles} * 24u + std::uint64_t{num_tiles} * 4u * 2u
        + std::max<std::uint64_t>(num_blocks, 1u) * 4u + 16u + 4096u;
    if (!arena.ensure_capacity(scaffold, 0, 0)) throw VulkanRuntimeError("phase_c index arena grow failed");
    const GpuArenaMark mark = arena.mark();
    const GpuArenaSlice seq_s = arena.allocate_device(seq_bytes, 16u);
    const GpuArenaSlice lut_s = arena.allocate_device(sizeof(VulkanRankLut), 16u);
    const GpuArenaSlice tiles_s = arena.allocate_device(std::uint64_t{num_tiles} * 24u, 16u);
    const GpuArenaSlice tcounts_s = arena.allocate_device(std::uint64_t{num_tiles} * 4u, 16u);
    const GpuArenaSlice toffsets_s = arena.allocate_device(std::uint64_t{num_tiles} * 4u, 16u);
    const GpuArenaSlice tblocks_s = arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const GpuArenaSlice counters_s = arena.allocate_device(16u, 16u);
    if (!seq_s.valid() || !lut_s.valid() || !tiles_s.valid() || !tcounts_s.valid()
        || !toffsets_s.valid() || !tblocks_s.valid() || !counters_s.valid()) {
        bioseq_log<ERR>("[phase_c index] scaffold arena oversubscribed (bp={}, num_tiles={}, "
                        "seq_bytes={}); returning invalid GPU index",
                        bp, num_tiles, seq_bytes);
        arena.rewind(mark);
        return out;
    }

    auto* base = static_cast<std::byte*>(arena.device_buffer().mapped);
    if (!packed_sequence.empty()) std::memcpy(base + seq_s.offset, packed_sequence.data(), packed_sequence.size());
    std::memcpy(base + lut_s.offset, &rank_lut, sizeof(rank_lut));
    std::memcpy(base + tiles_s.offset, tiles.data(), tiles.size() * sizeof(TileC));
    std::memset(base + tcounts_s.offset, 0, static_cast<std::size_t>(tcounts_s.size));

    const PhaseBPipeline sketch_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_sketch_tile, kPhaseCSketchLocal, std::max<std::uint32_t>(w, 1u));

    // --- Single-pass sketch (DOC0311): emit (hash, pos_strand) pairs directly,
    // each minimizer atomically reserving its output slot in keys/vals. There is
    // no separate count pass or tile-offset scan — the pairs are radix-sorted by
    // hash next, so the (now arbitrary) emission order is irrelevant. keys/vals
    // are sized to the capacity estimate; the exact count tm is read back from
    // counters[0]; counters[1] guards against an under-estimate. ---
    const std::uint64_t cap = max_target_minimizers;
    Buffer keys = create_device_buffer(cap * 8u, kPhaseBBufferUsage);   // out_words: u64 hash/min
    Buffer vals = create_device_buffer(cap * 8u, kPhaseBBufferUsage);   // out_pair_vals: u64 pos_strand/min
    if (keys.mapped == nullptr || vals.mapped == nullptr) {
        bioseq_log<ERR>("[phase_c index] capacity buffer alloc failed (cap={}); "
                        "returning invalid GPU index", cap);
        arena.rewind(mark);
        return out;
    }
    const GpuArenaSlice keys_s{&keys, 0, keys.bytes};
    const GpuArenaSlice vals_s{&vals, 0, vals.bytes};
    {
        const std::array<std::uint32_t, 4> cinit{0u, static_cast<std::uint32_t>(cap), 0u, 0u};
        std::memcpy(base + counters_s.offset, cinit.data(), sizeof(cinit));
    }
    {
        static const auto* _rd = bioseq::instr::register_region_static("phase_c_target_sketch");
        auto _rs = bioseq::instr::region(_rd);
        reset_phase_c_pool_locked();
        const std::array<VkDescriptorBufferInfo, 8> infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},     // unused (count mode only)
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},             // out_words
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},  // unused (emit_mode 1 only)
            {vals_s.buffer->buffer, vals_s.offset, vals_s.size},             // out_pair_vals
        }};
        SketchPushC push{2u, w, k, params.hash_bits, params.hash_shift, params.pos_bits,
            params.minimizer_word_kind, params.minimizer_words32, k, params.alphabet_kind,
            params.unit_bits, params.bits_per_char, params.letters_per_unit, params.char_mask,
            num_tiles, 1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_idx_emit)");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_idx_emit)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(command_buffer_, sketch_pipe, infos, &push, sizeof(push), num_tiles);
        record_shader_to_host(command_buffer_);
        submit_and_wait(device_, queue_, command_buffer_, fence_, "submit(pc_idx_emit)");
    }
    std::uint32_t emit_count = 0;
    std::memcpy(&emit_count, base + counters_s.offset, 4);   // counters[0] = exact minimizer count
    const std::uint64_t tm = emit_count;
    if (tm == 0u || tm > max_target_minimizers) {
        bioseq_log<ERR>("[phase_c idx] tm={} out of range / overflow (cap={})", tm, max_target_minimizers);
        arena.rewind(mark); return out;
    }

    // tm-sized scratch + index buffers, now that the exact count is known. keys
    // and vals (the emitted pairs) were already allocated to the capacity bound
    // above; the radix sort ping-pongs them against tm-sized scratch.
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

    const std::uint32_t tile_bp =
        adaptive_sketch_tile_bp(bp, features_.max_workgroup_count_x, kPhaseCSketchTileBp);
    const std::uint32_t num_tiles = ceil_div_u32(bp, tile_bp);
    if (num_tiles == 0u || num_tiles > features_.max_workgroup_count_x) {
        return out;
    }

    const std::uint32_t overlap = window_size + kmer_size;
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

    const std::uint32_t num_blocks = ceil_div_u32(num_tiles, kPhaseCScanBlock);
    GpuArena& arena = phase_c_arena();
    const std::uint64_t seq_bytes =
        std::max<std::uint64_t>(packed_sequence.size(), 4u);
    const std::uint64_t scaffold =
        seq_bytes
        + sizeof(VulkanRankLut)
        + std::uint64_t{num_tiles} * 24u
        + std::uint64_t{num_tiles} * 4u * 2u
        + std::max<std::uint64_t>(num_blocks, 1u) * 4u
        + 16u
        + 4096u;
    if (!arena.ensure_capacity(scaffold, 0u, 0u)) {
        throw VulkanRuntimeError("phase_c sorted-pair arena grow failed");
    }

    const GpuArenaMark mark = arena.mark();
    const GpuArenaSlice seq_s = arena.allocate_device(seq_bytes, 16u);
    const GpuArenaSlice lut_s = arena.allocate_device(sizeof(VulkanRankLut), 16u);
    const GpuArenaSlice tiles_s =
        arena.allocate_device(std::uint64_t{num_tiles} * 24u, 16u);
    const GpuArenaSlice tcounts_s =
        arena.allocate_device(std::uint64_t{num_tiles} * 4u, 16u);
    const GpuArenaSlice toffsets_s =
        arena.allocate_device(std::uint64_t{num_tiles} * 4u, 16u);
    const GpuArenaSlice tblocks_s =
        arena.allocate_device(std::max<std::uint64_t>(num_blocks, 1u) * 4u, 16u);
    const GpuArenaSlice counters_s = arena.allocate_device(16u, 16u);
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
        const std::array<VkDescriptorBufferInfo, 8> infos{{
            {seq_s.buffer->buffer, seq_s.offset, seq_s.size},
            {lut_s.buffer->buffer, lut_s.offset, lut_s.size},
            {tiles_s.buffer->buffer, tiles_s.offset, tiles_s.size},
            {tcounts_s.buffer->buffer, tcounts_s.offset, tcounts_s.size},
            {keys_s.buffer->buffer, keys_s.offset, keys_s.size},
            {counters_s.buffer->buffer, counters_s.offset, counters_s.size},
            {toffsets_s.buffer->buffer, toffsets_s.offset, toffsets_s.size},
            {values_s.buffer->buffer, values_s.offset, values_s.size},
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
            1u};
        check_vk(vkResetCommandBuffer(command_buffer_, 0), "reset(pc_sorted_pair_emit)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "begin(pc_sorted_pair_emit)");
        record_host_to_shader(command_buffer_);
        record_phase_c_dispatch_locked(
            command_buffer_, sketch_pipe, infos, &push, sizeof(push), num_tiles);
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
    const HostQRec qrec{0u, mc, 0u, static_cast<std::uint32_t>(query_len)};
    std::memcpy(base + qrec_s.offset, &qrec, sizeof(qrec));

    const PhaseBPipeline count_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_count, 128u, 1u);
    const PhaseBPipeline emit_pipe = get_phase_b_pipeline_locked(ShaderKind::phase_c_seed_emit, 128u, 1u);
    const PhaseBPipeline scanb = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_block, 1u, 1u);
    const PhaseBPipeline scana = get_phase_b_pipeline_locked(ShaderKind::phase_c_prefix_scan_add, 128u, 1u);

    SeedPushC cpush{inputs.minimizer_words32_per_record, inputs.minimizer_word_kind, inputs.hash_shift,
        inputs.pos_bits, kmer_size, inputs.slot_mask, static_cast<std::uint32_t>(max_occ), 0u, 1u,
        inputs.bucket_count};

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
        record_phase_c_dispatch_locked(command_buffer_, count_pipe, seed_infos, &cpush, sizeof(cpush), 1u);
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
        record_phase_c_dispatch_locked(command_buffer_, emit_pipe, seed_infos, &epush, sizeof(epush), 1u);
        record_compute_barrier(command_buffer_);
        GpuSortPlan sort_plan{total, 8u, 24u, 8u, 8u};
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
