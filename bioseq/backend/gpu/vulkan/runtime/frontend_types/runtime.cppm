/**
 * @file runtime.cppm
 * @brief Declares GPU operations and the Vulkan resources shared between them.
 */
module;

#include <cstddef>
#include <cstdint>
#include <new>

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

/// @brief Owns the Vulkan device and shared resources for GPU computation.
/// `instance()` supplies the process-wide runtime. GPU operations hold `mu_`
/// while using the shared queue and pipeline caches.
class runtime {
  public:
    /// @brief Initializes the device, keeping a diagnostic if initialization fails.
    runtime();
    /// @brief Prevents copying the process-wide Vulkan runtime.
    runtime(const runtime&) = delete;
    /// @brief Prevents assignment from copying the Vulkan runtime.
    runtime& operator=(const runtime&) = delete;
    /// @brief Waits for the device and releases the runtime's Vulkan resources.
    ~runtime();

    /// @brief Returns the process-wide runtime, initializing it on first use.
    [[nodiscard]] static runtime& instance();
    /// @brief Returns the  device capabilities recorded during initialization.
    [[nodiscard]] const vulkan_feature_set& features() const noexcept {
        return features_;
    }
    /// @brief Reports whether device initialization succeeded.
    [[nodiscard]] bool is_usable() const noexcept {
        return features_.available;
    }
    /// @brief Returns the initialization error, or an empty string on success.
    [[nodiscard]] const std::string& initialization_error() const noexcept {
        return init_error_;
    }
    /// @brief Returns the shared completion counter for sketching, indexing, seeding, and chaining.
    /// Batched coordinate work adds its completed submission waves.
    [[nodiscard]] std::uint64_t chaining_dispatch_count() const noexcept {
        return chaining_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns the number of completed packed-sequence generation calls with nonempty work.
    [[nodiscard]] std::uint64_t generate_units_dispatch_count() const noexcept {
        return generate_units_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns the memory reserved for cached pipeline metadata.
    [[nodiscard]] std::uint64_t pipeline_cache_metadata_reserved_bytes() {
        std::lock_guard<std::mutex> guard(mu_);
        return pipeline_cache_metadata_permit_.bytes();
    }
    /// @brief Accepts a completed coordinate operation akter checking its expected result and GPU state.
    /// On success, consumes `receipt` and returns the accepted operation with its
    /// cache costs. A failed check leaves the receipt and reporting flags unchanged.
    [[nodiscard]] persisted_grouped_soa_coordinate_accepted_operation
    finalize_persisted_grouped_soa_coordinate_operation(
        persisted_grouped_soa_coordinate_operation_receipt&& receipt,
        const persisted_grouped_soa_coordinate_operation_receipt_preview& expected_preview);
    /// @brief Tests that rejected coordinate receipts  leave cache costs available to report
    [[nodiscard]] static bool self_test_persisted_grouped_soa_finalizer_preclaim_contract(
        runtime_metal_archive_pipeline_audit base_audit);
    /// @brief Tests partial cache-cost records and prevents reporting them twice.
    [[nodiscard]] static bool
    self_test_gpu_cache_partial_delta_contract(runtime_metal_archive_pipeline_audit base_audit);

    /// @brief Allocates a buffer mapped for CPU reads and writes.
    /// `bytes` is the logical size and `usage` selects the Vulkan buffer uses.
    /// A zero-size request allocates one byte. The returned buffer owns the allocation.
    [[nodiscard]] buffer create_host_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    /// @brief Allocates a GPU buffer with the requested size and Vulkan uses.
    /// Prefers device-local memory that the CPU can also access. Check `mapped`
    /// before accessing it from the CPU, since mapping can, fail.
    [[nodiscard]] buffer create_device_buffer(std::uint64_t bytes, VkBufferUsageFlags usage);
    /// @brief Queries the device-memory allocation needed for this size and buffer usage.
    /// The result can include Vulkan alignment  and padding.
    [[nodiscard]] std::uint64_t buffer_allocation_requirement(std::uint64_t requested_bytes,
                                                              VkBufferUsageFlags usage);
    /// @brief Returns the arena allocation charge, including any larger existing buffer.
    [[nodiscard]] std::uint64_t gpu_arena_allocation_requirement(std::uint64_t requested_bytes);
    /// @brief Computes the memory needed for a sketch batch and its output.
    /// Includes existing arena capacity and the output allocation for
    /// max_minimizers in the requested word format.
    [[nodiscard]] gpu_sketch_batch_allocation_requirements
    sketch_batch_allocation_requirements(std::uint64_t packed_sequence_bytes,
                                         std::uint64_t tile_count,
                                         std::uint32_t minimizer_word_kind,
                                         std::uint32_t minimizer_words32,
                                         std::uint64_t max_minimizers);
    /// @brief Frees the Vulkan buffer and memory held by `allocation`.
    void destroy_buffer(buffer& allocation) noexcept;

    /// @brief Computes chaining scores and predicessor links, then copies them to the caller.
    /// Uploads and readback use mapped buffers around one compute submission.
    /// The runtime holds the queue lock until the operation finishes.
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
                                         std::int32_t max_chain_skip = 0
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                         , runtime_pipeline_operation_token* completed_operation = nullptr
#endif
                                         );

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    /// Checks the proposed environment and cache costs before they are reported.
    /// The callback may throw. It must avoid allocation  and calls into the runtime,
    /// whose queue/cache mutex is held.
    using selected_pipeline_publication_validator = void (*)(
        void*, const runtime_gpu_execution_environment_snapshot&,
        const runtime_gpu_cache_metrics_audit&);

#if defined(BIOSEQ_TEST_INDEXES)
    /// @brief Tests failed completion checks followed by a successful one-time cost report
    [[nodiscard]] static bool self_test_selected_pipeline_finalizer_transaction_contract(
        runtime_metal_archive_pipeline_audit base_audit);
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /**
     * @brief Accepts completed query-sketch, seed, or chaining work and returns its cache costs.
     * Additional operations must share the GPU and cache. Counts each pipeline's
     * creation cost once. The optional callback checks the proposed environment and
     * costs before the final runtime check. Failure leaves tokens and reporting flags
     * unchanged. Success consumes them together.
     * Coordinate, alignment, and MAPQ operations use separate finalizers.
     */
    [[nodiscard]] std::pair<runtime_gpu_execution_environment_snapshot,
                            runtime_gpu_cache_metrics_delta>
    finalize_selected_pipeline_operation(
        runtime_pipeline_operation_token&& token,
        selected_pipeline_publication_validator validate_publication = nullptr,
        void* publication_context = nullptr,
        std::span<runtime_pipeline_operation_token> additional_operations = {});

    /**
     * @brief Accepts a shared query sketch after its last MPF consumer has finished.
     *
     * Returns its GPU environment and unreported  cache costs, then consumes the
     * authorization. The stored-index seed path passes the query token back in
     * its seed result because it takes ownership of the authorization earlier.
     */
    [[nodiscard]] std::pair<runtime_gpu_execution_environment_snapshot,
                            runtime_gpu_cache_metrics_delta>
    finalize_selected_query_sketch(
        device_query_sketch_authorization&& authorization,
        selected_pipeline_publication_validator validate_publication = nullptr,
        void* publication_context = nullptr,
        std::span<runtime_pipeline_operation_token> additional_operations = {});

    /**
     * @brief Accepts several query sketches and their completed operations together.
     *
     * Checks every authorization, and operation before calling the publication
     * validator and the final runtime check. Failure leaves them available for
     * another attempt. Success consumes thhm and returns their unreported costs.
     */
    [[nodiscard]] std::pair<runtime_gpu_execution_environment_snapshot,
                            runtime_gpu_cache_metrics_delta>
    finalize_selected_query_sketches(
        std::span<device_query_sketch_authorization> authorizations,
        selected_pipeline_publication_validator validate_publication = nullptr,
        void* publication_context = nullptr,
        std::span<runtime_pipeline_operation_token> additional_operations = {});
#endif
#endif

    /// @brief Chains the anchors and returns the chosen coordinates and predecessor  trace.
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

    /// @brief Generates packed sequence data on the GPU using the supplied symbol distribution.
    /// Returns the buffer that owns the generated data.
    buffer run_generate_units_roundtrip(std::uint32_t word_bits,
                                        std::span<const std::uint32_t> cdf,
                                        generate_units_push push,
                                        std::uint64_t out_block_bytes);
    /// @brief Copies a byte range from a mapped buffer into `dst`.
    /// Checks the range and mapping before reading. GPU work writing this buffer
    /// must already be complete.
    void download_slice(const buffer& src, std::uint64_t src_byte_offset, std::span<std::byte> dst);

    /// @brief Emits seed anchors using the caller's prefix counts, then sorts them
    /// The GPU sorts target positions and strands. The CPU orders ties and checks
    /// the final anchor order before returning the anchors.
    seed_lookup_result run_seed_lookup_roundtrip(runtime_seed_lookup_inputs inputs,
                                                 std::span<const std::uint32_t> prefix_counts,
                                                 std::uint64_t query_len,
                                                 std::uint32_t kmer_size,
                                                 std::uint64_t exact_anchor_count,
                                                 const seed_anchor_sort_plan& sort_plan,
                                                 std::uint64_t expected_vulkan_allocation_bytes
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                 , const device_query_sketch_authorization* query_authorization = nullptr,
                                                 std::uint32_t query_index = 0u
#endif
                                                 );

    /// @brief Returns the runtime's scratch arena, creating it on first use.
    [[nodiscard]] gpu_arena& reusable_gpu_arena();

    /// @brief Builds ordered minimizers for one packed sequence.
    /// Returns a GPU buffer and minimizer count, or reports output  overflow.
    /// Optional query-hash bits restrict which minimizers are emitted.
    runtime_sketch_result run_sketch_roundtrip(std::span<const std::byte> packed_sequence,
                                               const vulkan_rank_lut& rank_lut,
                                               const vulkan_sketch_params& params,
                                               std::uint64_t max_minimizers,
                                               std::span<const std::uint32_t> query_gate_words = {},
                                               std::uint32_t query_gate_mask = 0u);

    /// @brief Builds ordered minimizers for a batch of query sequences.
    /// `materialization` selects a host copy or device-only output. Device-only
    /// results also carry authorization for later seed operations.
    runtime_sketch_batch_result
    run_sketch_tile_batch_roundtrip(std::span<const std::byte> packed_sequence,
                                    const vulkan_rank_lut& rank_lut,
                                    const vulkan_sketch_params& params,
                                    std::span<const gpu_sketch_tile_wire> tiles,
                                    std::span<const gpu_sketch_query_layout_wire> query_layouts,
                                    std::uint64_t max_minimizers,
                                    vulkan_sketch_host_materialization materialization);

    /// @brief Copies query minimizers to the CPU after checking their authorization.
    /// The buffer, word format, and query records must match the completed sketch.
    /// `destination` must have the exact byte size. The authorization stays valid
    /// for later seed work, and  this copy issues no public pireline receipt.
    void readback_authorized_query_minimizers(
        const device_query_sketch_authorization& authorization,
        const buffer& device_words,
        std::uint64_t expected_minimizer_count,
        std::uint32_t expected_word_kind,
        std::uint32_t expected_words32_per_record,
        std::uint32_t expected_hash_shift,
        std::uint32_t expected_pos_bits,
        std::span<const gpu_seed_query_record_wire> expected_query_records,
        std::span<std::byte> destination);

#if defined(BIOSEQ_TEST_INDEXES)
    /// @brief Tests that authorized readback accepts only the exact completed copy.
    [[nodiscard]] static bool self_test_authorized_readback_receipt_contract();
#endif

    /// @brief Loads target-index pipelines before reserving memory for the build.
    void prepare_target_index_pipelines(const vulkan_sketch_params& params);

    /// @brief Builds a GPU minimizer index with hash-sorted buckets and position lists.
    /// Counts minimizers first to size the output. `peak_permit` must cover the
    /// build. If sequence staging is owned, its callback releases it after upload.
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

    /// @brief Emits minimizer pairs and sorts by hash, then encoded position within each hash
    /// Returns the GPU buffers and a host copy of the pairs.
    gpu_sorted_pair_result
    run_sketch_sort_pairs_roundtrip(std::span<const std::byte> packed_sequence,
                                    const vulkan_rank_lut& rank_lut,
                                    const vulkan_sketch_params& params,
                                    std::uint64_t max_target_minimizers);

    /// @brief Sorts a contiguous batch of packed sequences and returns their  original ordinals.
    [[nodiscard]] std::vector<std::uint32_t>
    run_sequence_sort_roundtrip(std::span<const std::byte> packed_units,
                                std::span<const sequence_descriptor_wire> descs,
                                const sequence_sort_kernel_parameters& params);
    /// @brief Sorts packed sequences supplied in chunks and returns their original ordinals.
    [[nodiscard]] std::vector<std::uint32_t>
    run_sequence_sort_roundtrip(std::span<const sequence_sort_upload_chunk> chunks,
                                std::span<const sequence_descriptor_wire> descs,
                                const sequence_sort_kernel_parameters& params);

    /// @brief Computes banded local alignment with affine gap penalties.
    /// Returns scores and endpoints for, each job.
    [[nodiscard]] std::vector<gpu_alignment_extension_result_wire>
    run_alignment_extension_roundtrip(std::span<const std::uint32_t> query_symbols,
                                      std::span<const std::uint32_t> target_symbols,
                                      std::span<const gpu_alignment_extension_job_wire> jobs,
                                      const gpu_alignment_extension_params& params);

    /// @brief Runs SneakySnake to rule out sequence pairs beyond the edit limit.
    /// Each result includes a lower bound and status. Unsupported cases are kept
    /// for later alignment, and invalid ranges are checked before reading sequence data.
    [[nodiscard]] std::vector<bioseq::prefilter::prefilter_result>
    run_sneaky_snake_rank_batch_roundtrip(std::span<const std::uint8_t> query_ranks,
                                          std::span<const std::uint8_t> target_ranks,
                                          std::span<const bioseq::prefilter::prefilter_job> jobs);

    /// @brief Runs GateKeeper to filter pairs before alignment.
    /// A mask-based rejection needs confirmation from the bounded-distance  check.
    /// Unsupported cases are kept, and invalid ranges are checkhd before reading data.
    [[nodiscard]] std::vector<bioseq::prefilter::prefilter_result>
    run_gatekeeper_rank_batch_roundtrip(std::span<const std::uint8_t> query_ranks,
                                        std::span<const std::uint8_t> target_ranks,
                                        std::span<const bioseq::prefilter::prefilter_job> jobs);

    /// @brief Aligns expanded symbol arrays on the GPU and returns packed CIGAR strings.
    /// Large batches run in waves to limit traceback memory.
    [[nodiscard]] alignment_cigar_batch_result
    run_alignment_simple_cigar_roundtrip(std::span<const std::uint32_t> query_symbols,
                                         std::span<const std::uint32_t> target_symbols,
                                         std::span<const gpu_alignment_cigar_job_wire> jobs,
                                         const gpu_alignment_cigar_params& params,
                                         std::uint32_t packed_cigar_capacity);

    /// @brief Aligns packed sequence windows and attaches completion records to the coordinate receipt.
    /// Each job must match a recorded coordinate candidate. Returns alignment
    /// results and the packed CIGAR strings read from the GPU.
    [[nodiscard]] alignment_cigar_batch_result run_persisted_alignment_cigar_roundtrip(
        std::span<const std::byte> query_packed,
        std::span<const std::byte> target_packed,
        std::span<const gpu_alignment_cigar_job_wire> jobs,
        std::span<const gpu_alignment_candidate_binding_wire> candidate_bindings,
        std::span<const gpu_chain_candidate_header_wire> candidate_headers,
        std::span<const gpu_chain_candidate_wire> candidates,
        const gpu_alignment_cigar_params& params,
        std::uint32_t packed_cigar_capacity,
        persisted_grouped_soa_coordinate_operation_receipt& coordinate_receipt);

    /// @brief Classifies primary and secondary alignments and computes MAPQ on the GPU.
    /// Attaches the completed work to `coordinate_receipt`.
    [[nodiscard]] gpu_mapq_batch_result run_persisted_mapq_roundtrip(
        std::span<const gpu_chain_candidate_header_wire> headers,
        std::span<const gpu_chain_candidate_wire> candidates,
        std::span<const gpu_alignment_candidate_binding_wire> candidate_bindings,
        std::span<const gpu_alignment_cigar_result_wire> alignment_results,
        const gpu_persisted_mapq_config& config,
        std::int32_t min_chain_score,
        persisted_grouped_soa_coordinate_operation_receipt& coordinate_receipt);

#ifdef BIOSEQ_TEST_INDEXES
    /// @brief Tests packed-input alignment with a chosen target for traceback memory per submission.
    /// Returns CIGAR results and submission counts without attaching an operation receipt
    [[nodiscard]] alignment_cigar_batch_result run_alignment_packed_cigar_roundtrip_for_test(
        std::span<const std::byte> query_packed,
        std::span<const std::byte> target_packed,
        std::span<const gpu_alignment_cigar_job_wire> jobs,
        const gpu_alignment_cigar_params& params,
        std::uint32_t packed_cigar_capacity,
        std::uint64_t preferred_traceback_wave_bytes);
#endif

    /// @brief Returns the number  of completed sequence-sort calls.
    [[nodiscard]] std::uint64_t seq_sort_dispatch_count() const noexcept {
        return seq_sort_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns global bitonic-sort dispatches that compared full sequences.
    [[nodiscard]] std::uint64_t seq_sort_global_compare_dispatch_count() const noexcept {
        return seq_sort_global_compare_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns dispatches that sorted sequences by scattering unique dense keys.
    [[nodiscard]] std::uint64_t seq_sort_dense_scatter_dispatch_count() const noexcept {
        return seq_sort_dense_scatter_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns the packed-sequence bytes uploaded by sorting operations.
    [[nodiscard]] std::uint64_t seq_sort_uploaded_bytes_count() const noexcept {
        return seq_sort_uploaded_bytes_.load(std::memory_order_relaxed);
    }
    /// @brief Returns completed score-only alignment and CIGAR dispatches.
    [[nodiscard]] std::uint64_t alignment_extension_dispatch_count() const noexcept {
        return alignment_extension_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns the number of completed SneakySnake dispatches.
    [[nodiscard]] std::uint64_t sneaky_snake_dispatch_count() const noexcept {
        return sneaky_snake_dispatch_count_.load(std::memory_order_relaxed);
    }
    /// @brief Returns the number of completed GateKeeper dispatches.
    [[nodiscard]] std::uint64_t gatekeeper_dispatch_count() const noexcept {
        return gatekeeper_dispatch_count_.load(std::memory_order_relaxed);
    }

    /// @brief Counts seed hits on the GPU, emits the anchors, and sorts them.
    /// The CPU orders anchors with equal target references and checks the final
    /// order. Minimizers exceeding `max_occ` target occurrences are skipped.
    seed_lookup_result run_seed_anchor_roundtrip(const gpu_seed_inputs& inputs,
                                                 std::uint64_t query_len,
                                                 std::uint32_t kmer_size,
                                                 std::int32_t max_occ
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                 , const device_query_sketch_authorization* query_authorization = nullptr,
                                                 std::uint32_t query_index = 0u
#endif
                                                 );
    /// @brief Finds seed hits, chains them, and returns the chosen mapping coordinates.
    /// An empty result means no chain passed the thresholds.
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

    /// @brief Finds mapping coordinates for a batch of query/target jobs.
    /// Uses the selected chaining or diagonal-vote algorithm. Returns coordinates
    /// and dispatch counts, with completion receipts for supported GPU routes.
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
                                           device_query_sketch_authorization query_authority = {},
                                           gpu_persisted_mapq_config mapq_config = {});

    /// @brief Finds and sorts anchors from  the stored grouped minimizer index.
    /// Takes ownership of the qury authorization. Returns anchors grouped by
    /// query, with CPU-ordered ties and a receipt for the completed seed work.
    [[nodiscard]] grouped_soa_seed_anchor_result run_grouped_soa_seed_anchor_roundtrip(
        const gpu_grouped_soa_resident_view& target,
        const gpu_seed_query_inputs& query_inputs,
        std::int32_t max_occ,
        std::uint32_t kmer_size,
        device_query_sketch_authorization query_authorization);

    /// @brief Runs coordinate jobs for one query and returns results in job, order.
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

    /// @brief Returns the largest anchor batch sortable in one bitonic workgroup.
    /// The limit is a power of two set by device thread and shared-memory limits.
    /// Larger batches use radix sort.
    [[nodiscard]] std::uint32_t gpu_anchor_sort_capacity() const noexcept {
        return gpu_anchor_sort_capacity_;
    }

  private:
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    using selected_pipeline_live_validator = void (*)(void*);

    /// @brief Checks completed operations and prepares their cache costs for reporting.
    /// Commits only after publication and runtime checks succeed. Failure leaves
    /// the input tokens and reporting flags unchanged The caller holds the mutex.
    [[nodiscard]] static std::pair<runtime_gpu_execution_environment_snapshot,
                                   runtime_gpu_cache_metrics_delta>
    finalize_selected_pipeline_operation_transaction(
        runtime_pipeline_operation_token&& token,
        selected_pipeline_publication_validator validate_publication,
        void* publication_context,
        std::span<runtime_pipeline_operation_token> additional_operations,
        std::span<device_query_sketch_authorization> additional_query_authorizations,
        const vulkan_feature_set& live_features,
        bool& seed_metrics_claimed,
        selected_pipeline_live_validator validate_live,
        void* live_context);
#endif

    /// @brief Checks the coordinate receipt against the expected result and  current GPU.
    /// Returns a plan for acceptance and cache costs without changing reporting flags.
    /// A failed check returns an invalid plan.
    [[nodiscard]] static runtime_detail::persisted_grouped_soa_coordinate_finalization_plan
    prepare_persisted_grouped_soa_coordinate_finalization(
        const persisted_grouped_soa_coordinate_operation_receipt& receipt,
        const persisted_grouped_soa_coordinate_operation_receipt_preview& expected_preview,
        const vulkan_feature_set& live_features,
        bool seed_metrics_claimed) noexcept;

    struct pipeline_cache_admission_scope {
        runtime* owner = nullptr;
        /// @brief Updates the cache memory reservation when this scope ends.
        explicit pipeline_cache_admission_scope(runtime& runtime) noexcept : owner(&runtime) {}
        /// @brief Prevents copying a reconciliation scope.
        pipeline_cache_admission_scope(const pipeline_cache_admission_scope&) = delete;
        /// @brief Prevents assignment from copying this scope.
        pipeline_cache_admission_scope& operator=(const pipeline_cache_admission_scope&) = delete;
        /// @brief Adjusts the cache reservation to the metadata still held by its entries.
        ~pipeline_cache_admission_scope() {
            if (owner != nullptr) {
                owner->reconcile_pipeline_cache_metadata_locked();
            }
        }
    };

    struct owned_pipeline_handles {
        VkDevice device = VK_NULL_HANDLE;                 ///< Vulkan device owning every handle below.
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE; ///< Descriptor set layout for the pipeline.
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE; ///< Layout of descriptor sets and push constants.
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE; ///< Descriptor pool for reusable sets.
        VkShaderModule module = VK_NULL_HANDLE;            ///< Shader module loaded for the pipeline.
        VkPipeline pipeline = VK_NULL_HANDLE;                ///< Compute pipeline handle.

        /// @brief Creates an owner with no Vulkan handles.
        owned_pipeline_handles() = default;
        /// @brief Creates an empty handle owner for `owner`.
        explicit owned_pipeline_handles(VkDevice owner) noexcept : device(owner) {}
        /// @brief Prevents copying Vulkan handle ownership.
        owned_pipeline_handles(const owned_pipeline_handles&) = delete;
        /// @brief Prevents assignment from duplicating owned Vulkan handles.
        owned_pipeline_handles& operator=(const owned_pipeline_handles&) = delete;
        /// @brief Transfers all Vulkan handles and clears the source.
        owned_pipeline_handles(owned_pipeline_handles&& other) noexcept
            : device(std::exchange(other.device, VK_NULL_HANDLE)),
              set_layout(std::exchange(other.set_layout, VK_NULL_HANDLE)),
              pipeline_layout(std::exchange(other.pipeline_layout, VK_NULL_HANDLE)),
              descriptor_pool(std::exchange(other.descriptor_pool, VK_NULL_HANDLE)),
              module(std::exchange(other.module, VK_NULL_HANDLE)),
              pipeline(std::exchange(other.pipeline, VK_NULL_HANDLE)) {}
        /// @brief Destroys the current handles and takes those from `other`.
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
        /// @brief Destroys every still-owned Vulkan handle.
        ~owned_pipeline_handles() {
            reset();
        }

        /// @brief Destroys the pipelinc and its supporting objects, then clears the handles.
        void reset() noexcept;
    };

    /// @brief Cached tiled-chaining pipeline, keyed  by tile and workgroup size.
    struct pipeline_entry {
        chaining_pipeline_key key{}; ///< Tile and workgroup sizes for the chaining shader.
        owned_pipeline_handles handles{}; ///< Owned Vulkan handles for the cached pipeline.
        std::string entry_name = "main"; ///< Shader entry point recorded at creation.
        std::unique_ptr<const runtime_metal_archive_pipeline_receipt> metal_archive_receipt{}; ///< Creation receipt for Metal or native Vulkan execution.
        std::uint64_t pipeline_create_ns = 0u; ///< Nanoseconds spent creating the pipeline.

        /// @brief Creates an empty pipeline cache entry.
        pipeline_entry() = default;
        /// @brief Prevents copying cached pipeline ownership.
        pipeline_entry(const pipeline_entry&) = delete;
        /// @brief Prevents assignment from copying a cached pipeline.
        pipeline_entry& operator=(const pipeline_entry&) = delete;
        /// @brief Transfers the cache entry and its Vulkan resources.
        pipeline_entry(pipeline_entry&&) noexcept = default;
        /// @brief Replaces the entry by taking another entry's resources.
        pipeline_entry& operator=(pipeline_entry&&) noexcept = default;
    };

    struct chaining_pipeline {
        VkPipeline pipeline = VK_NULL_HANDLE; ///< Borrowed compute pipeline for one chaining dispatch.
        shader_variant_key shader_key{};      ///< Shader identity used to create the pipeline.
        const runtime_metal_archive_pipeline_receipt* metal_archive_receipt = nullptr; ///< Borrowed pipeline creation receipt for Metal or native Vulkan.
    };

    /// @brief Borrowed handles for a cached compute pipeline and its reusable descriptor set.
    /// The cache key is `(kind, local_size, max_span)`. Alphabet and word-format
    /// choices are passed to dispatches as  push constants. The cache owns these handles.
    struct compute_pipeline {
        shader_kind kind{};                         ///< Shader, algorithm selected by the cache key
        std::uint32_t local_size = 0;               ///< Workgroup-size selector stored in the shader key.
        std::uint32_t max_span = 0;                 ///< Shader-specific size or mode used when generating the variant.
        std::uint32_t binding_count = 0;            ///< Number of storage-buffer bindings.
        std::uint32_t push_size = 0;                ///< Push-constant byte size.
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE; ///< Borrowed descriptor set layout.
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE; ///< Borrowed pipeline layout.
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE; ///< Borrowed descriptor pool.
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;   ///< Reusable descriptor set for the entry.
        VkShaderModule module = VK_NULL_HANDLE;            ///< Borrowed shader module.
        VkPipeline pipeline = VK_NULL_HANDLE;            ///< Borrowed compute pipeline.
        const runtime_metal_archive_pipeline_receipt* metal_archive_receipt = nullptr; ///< Borrowed pipeline creation receipt for Metal or native Vulkan.
    };

    struct owned_compute_pipeline {
        shader_kind kind{};                         ///< Shader algorithm owned by this cache entry.
        std::uint32_t local_size = 0;               ///< Workgroup-size selector stored in the shader key.
        std::uint32_t max_span = 0;                 ///< Shader-specific size or mode used when generating the variant.
        std::uint32_t binding_count = 0;            ///< Number of storage-buffer bindings.
        std::uint32_t push_size = 0;                ///< Push-constant byte size.
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE; ///< Reusable descriptor set owned by the entry.
        owned_pipeline_handles handles{};           ///< Owned Vulkan handles for the pipeline.
        std::string entry_name = "main";            ///< Shader entry point recorded at creation.
        std::unique_ptr<const runtime_metal_archive_pipeline_receipt> metal_archive_receipt{}; ///< Creation receipt for Metal or  native Vulkan executio.
        std::uint64_t pipeline_create_ns = 0u;      ///< Nanoseconds spent creating the pipeline.

        /// @brief Creates an entry with no compute pipeline.
        owned_compute_pipeline() = default;
        /// @brief Prevents copying compute-pipeline ownership.
        owned_compute_pipeline(const owned_compute_pipeline&) = delete;
        /// @brief Prevents assignment from duplicating a compute pipeline.
        owned_compute_pipeline& operator=(const owned_compute_pipeline&) = delete;
        /// @brief Transfers compute-pipeline ownership.
        owned_compute_pipeline(owned_compute_pipeline&&) noexcept = default;
        /// @brief Replaces this entry by taking another compute pipeline.
        owned_compute_pipeline& operator=(owned_compute_pipeline&&) noexcept = default;

        /// @brief Returns handles for dispatch while keeping ownership in this cache entry.
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

    // Cache entries keep ownership when the vector grows. Returned views borrow
    // their Vulkan handles and pipeline receipts.
    /// @brief Finds or creates the requested compute pipeline and returns its handles.
    compute_pipeline
    get_compute_pipeline_locked(shader_kind kind, std::uint32_t local_size, std::uint32_t max_span);
    /// @brief Loads all pipeline variants needed to build a target index.
    void prepare_target_index_pipelines_locked(const vulkan_sketch_params& params);
    /// @brief Binds each supplied buffer in full to the pipeline's reusable descriptor set.
    void bind_compute_pipeline_descriptors_locked(const compute_pipeline& entry,
                                                  std::span<const VkBuffer> buffers);
    // Arena slices can share one buffer. Bind  each slice with its own offset
    // and range so a dispatch accesses only the intended slice
    /// @brief Updates the reusable descriptor set with the supplied buffer ranges.
    void bind_descriptors_offsets_locked(const compute_pipeline& entry,
                                         std::span<const VkDescriptorBufferInfo> infos);

    // Give each dispatch its own descriptor set so one command buffer can use
    // different bindings for the same pipeline, as in alternating sort buffers.
    // Reset the pool between completed operations.
    /// @brief Creates the dispatch descriptor pool on first use.
    void ensure_dispatch_descriptor_pool_locked();
    /// @brief Frees all descriptor sets in the pool after an operation completes.
    void reset_dispatch_descriptor_pool_locked();
    /// @brief Allocates a descriptor set for one recorded dispatch.
    [[nodiscard]] VkDescriptorSet
    allocate_dispatch_descriptor_set_locked(VkDescriptorSetLayout layout);
    /// @brief Records a compute dispatch with its own descriptors and push constants.
    /// If a collector  is supplied, also records the pipeline use for that job.
    void record_compute_dispatch_locked(VkCommandBuffer cmd,
                                        const compute_pipeline& pipe,
                                        std::span<const VkDescriptorBufferInfo> infos,
                                        const void* push,
                                        std::uint32_t push_size,
                                        std::uint32_t group_count,
                                        runtime_pipeline_use_collector* collector = nullptr,
                                        std::size_t collector_job_index = 0u);
    /// @brief Uploads and aligns one CIGAR batch, thes reads its results under the runtime lock.
    /// Accepts packed bytes or, expanded ranks. The caller supplies a reservation
    /// covering this batch's buffers.
    [[nodiscard]] alignment_cigar_batch_result run_alignment_cigar_payload_roundtrip_locked(
        std::span<const std::byte> query_payload,
        std::span<const std::byte> target_payload,
        std::span<const gpu_alignment_cigar_job_wire> jobs,
        const gpu_alignment_cigar_params& params,
        std::uint32_t packed_cigar_capacity,
        std::span<const std::size_t> collector_job_indices,
        bool allow_unsupported_geometry,
        bioseq::parallelism::memory_permit transient_permit,
        runtime_pipeline_use_collector* collector);
    /// @brief Splits a CIGAR batch into submissions with memory reserved before allocation.
    /// Groups jobs around the preferred traceback size. Jobs can first run without
    /// traceback storage, then retry with storage when needed.
    [[nodiscard]] alignment_cigar_batch_result
    run_alignment_cigar_payload_waved_locked(std::span<const std::byte> query_payload,
                                             std::span<const std::byte> target_payload,
                                             std::span<const gpu_alignment_cigar_job_wire> jobs,
                                             const gpu_alignment_cigar_params& params,
                                             std::uint32_t packed_cigar_capacity,
                                             std::uint64_t preferred_traceback_wave_bytes,
                                             runtime_pipeline_use_collector* collector);
    // Record every radix pass into the open command buffer. Passes alternate
    // between the input slices and scratch slices.
    /// @brief Records a radix sort  and reports which buffers hold the result.
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
    // Sort encoded positions first, then stably sort hashes. Equal hashes keep
    // their position order, giving (hash, encoded_pos) order
    /// @brief Records the pair sort and updates the views to the final buffers.
    void record_stable_pair_sort_by_value_then_key_locked(VkCommandBuffer command_buffer,
                                                          sorted_pair_device_views& views,
                                                          gpu_arena_slice hist,
                                                          gpu_sort_plan value_sort_plan,
                                                          gpu_sort_plan key_sort_plan,
                                                          std::uint32_t num_workgroups,
                                                          std::uint32_t elems_per_workgroup);

    /// @brief Selects a Vulkan device and creates the resources shared by GPU operations.
    void init();
    /// @brief Waits for the device and frees all runtime-owned Vulkan resources.
    void shutdown() noexcept;
    /// @brief Finds MoltenVK's optional function for setting a shader's workgroup size.
    void resolve_moltenvk_private_api() noexcept;
    /// @brief Checks whether an installed shader provider can load modules.
    [[nodiscard]] bool should_use_shader_provider_locked() const noexcept;
    /// @brief Finds a memory type allowed by `type_bits` with all requested properties.
    /// Throws if none is available.
    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    /// @brief Queries Vulkan's allocation size for a buffer of this size and usage.
    [[nodiscard]] std::uint64_t buffer_memory_requirement_locked(std::uint64_t bytes,
                                                                 VkBufferUsageFlags usage) const;
    /// @brief Checks that this buffer  and query layout belong to the autorized sketch.
    /// Returns the exact data size without consuming the authorization.
    [[nodiscard]] std::uint64_t validate_query_sketch_authorization_locked(
        const device_query_sketch_authorization& authorization,
        const buffer& device_words,
        std::uint64_t expected_minimizer_count,
        std::uint32_t expected_word_kind,
        std::uint32_t expected_words32_per_record,
        std::uint32_t expected_hash_shift,
        std::uint32_t expected_pos_bits,
        std::span<const gpu_seed_query_record_wire> expected_query_records) const;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /// @brief Checks that an MPF query slice matches its authorized sketch.
    /// Returns the query authorization ID while keeping it valid for other consumers.
    [[nodiscard]] std::uint64_t validate_seed_query_authorization_locked(
        const device_query_sketch_authorization& authorization,
        const buffer* device_words,
        const gpu_seed_query_record_wire& submitted_record,
        std::uint32_t query_index,
        std::uint32_t kmer_size,
        std::uint32_t word_kind,
        std::uint32_t words32_per_record,
        std::uint32_t hash_shift,
        std::uint32_t pos_bits) const;
#endif
    /// @brief Returns the device-memory bytes held by the reusable prefilter buffers.
    [[nodiscard]] std::uint64_t prefilter_workspace_allocation_bytes_locked() const noexcept;
    /// @brief Grows a prefilter buffer if needed, reserving memory before, allocation.
    void ensure_prefilter_workspace_buffer_locked(buffer& allocation,
                                                  std::uint64_t bytes,
                                                  VkBufferUsageFlags usage);
    /// @brief Frees prefilter buffers and releases their memory reservation.
    void release_prefilter_workspace_locked() noexcept;
    /// @brief Runs a prefilter batch using the selected shader and reusable workspace.
    /// Returns one  checked result per job. Unsupported cases remain candidates.
    [[nodiscard]] std::vector<bioseq::prefilter::prefilter_result>
    run_rank_prefilter_batch_roundtrip_locked(
        shader_kind kind,
        std::uint32_t push_mode,
        std::size_t replay_index,
        std::span<const std::uint8_t> query_ranks,
        std::span<const std::uint8_t> target_ranks,
        std::span<const bioseq::prefilter::prefilter_job> jobs);
    /// @brief Returns the memory charge for metadata held by both pipeline caches.
    [[nodiscard]] std::uint64_t pipeline_cache_metadata_bytes_locked() const noexcept;
    /// @brief Reserves memory before either pipeline cache grows.
    void admit_pipeline_cache_growth_locked(std::size_t normal_capacity,
                                            std::size_t compute_pipeline_capacity);
    /// @brief Adjusts the cache reservation to the memory still held by its entries.
    void reconcile_pipeline_cache_metadata_locked() noexcept;
    /// @brief Finds or creates a tiled-chaining pipeline owned by the cache.
    /// Returns borrowed handles and the shader identity
    chaining_pipeline create_pipeline_locked(const chaining_pipeline_key& key);
    /// @brief Copies host bytes into a mapped GPU buffer.
    void upload_locked(buffer& transfer_scratch,
                       buffer& dst,
                       std::span<const std::byte> bytes,
                       VkAccessFlags dst_access);
    /// @brief Copies bytes from a mapped GPU buffer into caller storage.
    void download_locked(buffer& transfer_scratch, const buffer& src, std::span<std::byte> bytes);
    /// @brief Reserves a unique nonzero operation ID.
    /// Throws when the ID range is  exhausted.
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
        VkCommandBuffer command_buffer = VK_NULL_HANDLE; ///< Command buffer containing the recorded dispatch.
        shader_kind kind{};                              ///< Prefilter algorithm recorded in this command.
        VkPipeline pipeline = VK_NULL_HANDLE;            ///< Pipeline bound when the command was recorded.
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE; ///< Pipeline layout bound with the dispatch.
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE; ///< Descriptor set bound for the dispatch.
        gpu_prefilter_push push{};                       ///< Push constants stored in this command recording.
        std::uint32_t groups = 0u;                       ///< Workgroup count stored in this command.
        bool recorded = false;                           ///< A completed command recording is available for reise.
    };

    struct prefilter_descriptor_binding_cache {
        std::array<VkBuffer, 4> buffers{}; ///< Last bound query, target, job, and result buffers for one algorithm.
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE; ///< Descriptor set updated with `buffers`.
    };

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    // Keep separate command recordings for two SneakySnake modes, generic
    // GateKeeper, and four GateKeeper length/edit-limit combinations. Switching
    // routes can then reuse a recording whose inputs and bindings still match.
    std::array<prefilter_recorded_command, k_rank_prefilter_replay_route_count>
        prefilter_recorded_commands_{};
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    // Members are destroyed in reverse order. Keep the reservation until  both
    // caches have released their entries and allocated storage.
    bioseq::parallelism::memory_permit pipeline_cache_metadata_permit_;
    std::vector<pipeline_entry> pipeline_cache_;
    std::vector<owned_compute_pipeline> compute_pipelines_;
    bool seed_metrics_claimed_ = false;
    std::uint32_t gpu_anchor_sort_capacity_ = 0;
    std::optional<gpu_arena> gpu_arena_; // Created on first use.
    // Both prefilters share mapped workspace because mu_ serializes their
    // operations. Keep its reservation before the buffers so destruction frees
    // every buffer before returning the reserved memory.
    bioseq::parallelism::memory_permit prefilter_workspace_permit_;
    buffer prefilter_query_buffer_;
    buffer prefilter_target_buffer_;
    buffer prefilter_jobs_buffer_;
    buffer prefilter_results_buffer_;
    // Keep separate descriptor caches for SneakySnake and GateKeeper. Switching
    // algorithms then preserves each set's bindings when the buffers are unchanged.
    std::array<prefilter_descriptor_binding_cache, 2> prefilter_descriptor_binding_cache_{};
    VkDescriptorPool dispatch_descriptor_pool_ = VK_NULL_HANDLE; // Supplies a separate descriptor set for each dispatch.
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
    std::atomic<std::uint64_t> seq_sort_uploaded_bytes_{0}; // Packed-sequence payload bytes uploaded by sorting.
    std::atomic<std::uint64_t> alignment_extension_dispatch_count_{0};
    std::atomic<std::uint64_t> sneaky_snake_dispatch_count_{0};
    std::atomic<std::uint64_t> gatekeeper_dispatch_count_{0};
    std::atomic<std::uint64_t> operation_token_identity_{1};
    std::mutex mu_;

    friend class gpu_arena;
};

/// @brief Reports whether the runtime has an available hardware GPU.
[[nodiscard]] inline bool hardware_device_available(const runtime& runtime) noexcept {
    return runtime.is_usable() && !runtime.features().software_device;
}

/// @brief Throws if this operation lacks an available,  hardware GPU.
/// `operation` names the operation in the error message.
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

/// @brief Estimates memory for GPU anchor sorting and its host result.
/// Includes anchors, sort keys, scratch buffers, and workgroup histograms.
/// Returns the largest 64-bit value on overflow
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
