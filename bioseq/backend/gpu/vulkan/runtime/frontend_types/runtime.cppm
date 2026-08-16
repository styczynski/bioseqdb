/**
 * @file runtime.cppm
 * @brief Declares Vulkan runtime ownership, operations, caches, and device state.
 */
module;

#include <cstddef>
#include <cstdint>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:frontend_types.runtime;

import :receipts;
import :frontend_types.sketch;
import :frontend_types.arena;
import :frontend_types.indexing;
import :frontend_types.coordinates;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

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
                                                             std::uint32_t group_count_x,
                                                             std::uint32_t group_count_y,
                                                             std::uint32_t group_count_z);
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
