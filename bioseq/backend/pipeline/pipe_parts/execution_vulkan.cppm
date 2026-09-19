/**
 * @file pipeline/pipe_parts/execution_vulkan.cppm
 * @brief Runs Vulkan stages and validates device route identity.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:execution_vulkan;

import std;
import :stage_graph;
import :minimizer_execution;
import bioseq.common.logging;
import bioseq.common.templating;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.highway.dispatch_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.ksw2_batched;
import bioseq.alignment.block_aligner;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.chaining.vulkan;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.stats;
import bioseq.prefilter.types;
import bioseq.prefilter.sneaky_snake;
import bioseq.prefilter.sneaky_snake.vulkan;
import bioseq.prefilter.gatekeeper;
import bioseq.prefilter.gatekeeper.vulkan;
import bioseq.common.context;
import bioseq.instrumentation;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.seeding.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import :execution_plan_context;

export {

    namespace bioseq::simd::pipe {

    /**
     * @brief Seed-hit collection stage.
     *
     * Wraps the `collect_seed_hits` kernel, which writes its anchor output
     * directly into a caller-provided arena_slab. No `std::vector` is materialised
     * inside the stage, so the kernel's `push_back` traffic lands directly in the
     * pipeline arena.
     */
    struct seed_hit_stage : stage_base<seed_hit_stage, ::bioseq::chaining::anchor> {
        static constexpr std::string_view stage_name = "seed_hit_stage";

        statement_execution_plan_owner execution_plan;

        /// Borrow the sole immutable statement snapshot.
        [[nodiscard, gnu::always_inline]] const minimap2_parameters&
        parameters() const noexcept {
            return execution_plan->frozen_parameters;
        }
        sequence_size_t query_len;
        // Batch- or segment-wave-scoped GPU index cache. The outer
        // wave creates one cache, passes `&cache` here, and clears it at wave end.
        // Null on single-shot paths (then the GPU seed path uploads uncached).
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr;
        match_result_collection* stats_dst = nullptr;
        post_seed_owned_lifetime* post_seed_lifetime = nullptr;
        match_result_collection* host_timing_dst = nullptr;
        route_attempt_memory* attempt_memory = nullptr;
        std::uint32_t submitted_query_ordinal = 0u;

        // Host/device accessors cover both the std::span CPU shape and
        // the minimizer_batch_view device shape.
        template <class p_minimizer>
        [[nodiscard, gnu::always_inline]] static std::span<const p_minimizer>
        sketch_span_of(const std::span<const p_minimizer>& s) noexcept {
            return s;
        }
        template <class p_minimizer>
        [[nodiscard, gnu::always_inline]] static std::span<const p_minimizer>
        sketch_span_of(const minimizer_batch_view<p_minimizer>& b) noexcept {
            return b.as_span();
        }

        template <class p_minimizer>
        [[nodiscard, gnu::always_inline]] static std::optional<
            bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>
        device_span_of(const std::span<const p_minimizer>&) noexcept {
            return std::nullopt;
        }
        template <class p_minimizer>
        [[nodiscard, gnu::always_inline]] static std::optional<
            bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>
        device_span_of(const minimizer_batch_view<p_minimizer>& b) noexcept {
            return b.device;
        }

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_batch,
                  class p_index,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink>
        process_with_index(p_batch&& sketches,
                           const p_index& target_index_ref,
                           bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
                           emit_token<p_sink>&& token) {
            const std::uint64_t host_stage_started =
                host_timing_dst == nullptr ? 0u : host_stage_now_ns();
            const std::int32_t kmer_size =
                p_aligner_parameters::generic_fallback
                    ? parameters().kmer_size
                    : static_cast<std::int32_t>(p_aligner_parameters::kmer_len);

            using p_minimizer =
                std::remove_const_t<typename std::remove_cvref_t<p_batch>::value_type>;

            auto sketch_span = sketch_span_of<p_minimizer>(sketches);
            auto device_span = device_span_of<p_minimizer>(sketches);
            direct_resident_operation_stats* const direct_ledger =
                post_seed_lifetime == nullptr ? nullptr
                                              : post_seed_lifetime->owned_cpu_index.direct_ledger;

            // Run the selected GPU seed-hit lookup before the CPU
            // path. If it declines, the selected Vulkan pipeline fails loudly.
            // Only the concrete minimizer_hashmap_fragment_index carries the GPU view.
            const bool gpu_seed_selected =
                execution_plan->route == seed_chain_route::vulkan_seed_host_chain ||
                execution_plan->route == seed_chain_route::vulkan_seed_vulkan_chain;
            if (gpu_seed_selected) {
                bool seed_gpu_declined = false;
                bioseq::seeding::vulkan_seed_lookup_decline seed_decline{
                    .query_length = static_cast<std::uint64_t>(query_len),
                    .query_minimizer_count = static_cast<std::uint64_t>(sketch_span.size()),
                    .kmer_size = kmer_size,
                    .max_occ = parameters().max_occ,
                    .fused_frontend = parameters().vulkan_fused_frontend_enabled,
                };
                if constexpr (std::same_as<std::remove_cvref_t<p_index>,
                                           bioseq::indexing::minimizer_hashmap_fragment_index>) {
                    // Strict Vulkan selection owns its anchor allocation. A
                    // prepared fragment view has no resident GPU index and reaches
                    // the fail-closed decline below without allocating CPU output.
                    bioseq::parallelism::memory_permit canonical_job_permit{};
                    canonical_seed_batch seed_batch{};
                    if (attempt_memory != nullptr) {
                        attempt_memory->prepare_seed_job_storage(
                            seed_batch.jobs, canonical_job_permit, 1u);
                        if (post_seed_lifetime != nullptr &&
                            post_seed_lifetime->retained_seed_arena_permit) {
                            throw std::logic_error{"campaign MPF retained a legacy slab permit"};
                        }
                    }
                    auto& anchors_slab = seed_batch.anchors;
                    if (gpu_index_identity.source_direct_sorted_dense &&
                        gpu_index_identity.canonical_empty) {
                        if (!bioseq::indexing::gpu_source_direct_identity_well_formed(
                                gpu_index_identity) ||
                            !target_index_ref.table_empty() ||
                            !target_index_ref.positions.empty() ||
                            gpu_index_identity.table_size != 0u ||
                            gpu_index_identity.position_count != 0u || gpu_index_cache != nullptr ||
                            direct_ledger == nullptr || parameters().vulkan_fused_frontend_enabled) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "canonical empty source-direct route is invalid"};
                        }
                        if (!bioseq::seeding::publish_empty_canonical_seed_batch(
                                submitted_query_ordinal, seed_batch,
                                std::move(seed_batch.jobs))) {
                            throw std::logic_error{"empty MPF seed job could not be published"};
                        }
                        direct_ledger->empty_bypass = true;
                        if (post_seed_lifetime != nullptr &&
                            post_seed_lifetime->remaining_seed_users.load(
                                std::memory_order_acquire) != 0u) {
                            static_cast<void>(post_seed_lifetime->complete_seed_user());
                        }
                        return std::move(token).emit_batch(std::span<anchor>{});
                    }
                    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
                    if (bioseq::gpu::vulkan::hardware_device_available(runtime)) {
                        auto key = bioseq::indexing::make_gpu_index_cache_key(target_index_ref,
                                                                              gpu_index_identity);
                        std::shared_ptr<const bioseq::indexing::gpu_resident_minimizer_index>
                            resident_index;
                        // Keep operation-originated materialization statistics
                        // separate from the seed-decline record. The seed lookup
                        // entry point intentionally resets its decline output at
                        // entry, while these statistics must survive a successful
                        // lookup so the benchmark records the one real upload and
                        // leaves later cache hits at zero.
                        bioseq::indexing::gpu_index_upload_decline index_materialization{};
                        if (gpu_index_cache == nullptr) {
                            seed_decline.reason = bioseq::seeding::
                                vulkan_seed_lookup_decline_reason::index_cache_unavailable;
                        } else if (gpu_index_identity.source_direct_sorted_dense) {
                            if (!bioseq::indexing::gpu_source_direct_identity_well_formed(
                                    gpu_index_identity) ||
                                gpu_index_identity.canonical_empty ||
                                parameters().vulkan_fused_frontend_enabled || direct_ledger == nullptr) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                    "source-direct resident seed identity is invalid"};
                            }
                            direct_ledger->cache_lookup_started = true;
                            direct_ledger->cache_required_lookups = bioseq::common::saturating_add(
                                direct_ledger->cache_required_lookups, 1u);
                            auto required =
                                gpu_index_cache->find_required_preinserted(runtime, key);
                            if (required) {
                                resident_index = std::move(*required);
                                direct_ledger->cache_required_hits = bioseq::common::saturating_add(
                                    direct_ledger->cache_required_hits, 1u);
                            } else {
                                seed_decline.reason = bioseq::seeding::
                                    vulkan_seed_lookup_decline_reason::resident_index_unavailable;
                            }
                        } else {
                            resident_index =
                                gpu_index_cache->get_or_upload(runtime,
                                                               target_index_ref,
                                                               key,
                                                               parameters().vulkan_index_cache_enabled,
                                                               &index_materialization);
                            if (!resident_index) {
                                seed_decline.index_upload = index_materialization;
                                seed_decline.reason =
                                    index_materialization.is_declined()
                                        ? bioseq::seeding::vulkan_seed_lookup_decline_reason::
                                              resident_index_upload_declined
                                        : bioseq::seeding::vulkan_seed_lookup_decline_reason::
                                              resident_index_unavailable;
                            }
                        }
                        if (resident_index && gpu_index_identity.source_direct_sorted_dense &&
                            post_seed_lifetime == nullptr) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "source-direct seed route has no arena permit owner"};
                        }
                        if (direct_ledger != nullptr) {
                            if (post_seed_lifetime->retained_seed_arena_permit) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                    "source-direct seed route retained a prior arena permit"};
                            }
                            if (stats_dst == nullptr) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                    "source-direct seed stage has no execution-stats sink"};
                            }
                            direct_ledger->mapped_prefix_started = true;
                            direct_ledger->slab_capacity_before = bioseq::common::saturating_add(
                                direct_ledger->slab_capacity_before,
                                static_cast<std::uint64_t>(anchors_slab.capacity()));
                        }
                        bioseq::seeding::vulkan_seed_lookup_completion seed_completion{false};
                        if (resident_index) {
                            seed_completion =
                                bioseq::seeding::collect_seed_hits_vulkan<p_minimizer>(
                                    sketch_span,
                                    device_span,
                                    std::addressof(target_index_ref),
                                    resident_index,
                                    parameters().vulkan_enabled,
                                    query_len,
                                    parameters().max_occ,
                                    kmer_size,
                                    submitted_query_ordinal,
                                    seed_batch,
                                    attempt_memory != nullptr,
                                    parameters().vulkan_fused_frontend_enabled,
                                    &seed_decline);
                        }
                        if (direct_ledger != nullptr) {
                            direct_ledger->mapped_prefix_passes = bioseq::common::saturating_add(
                                direct_ledger->mapped_prefix_passes,
                                seed_decline.mapped_prefix_authority_passes);
                            direct_ledger->mapped_prefix_lookups = bioseq::common::saturating_add(
                                direct_ledger->mapped_prefix_lookups,
                                seed_decline.mapped_prefix_lookup_count);
                            direct_ledger->mapped_prefix_comparisons =
                                bioseq::common::saturating_add(
                                    direct_ledger->mapped_prefix_comparisons,
                                    seed_decline.mapped_prefix_comparison_count);
                            direct_ledger->slab_requested_bytes = bioseq::common::saturating_add(
                                direct_ledger->slab_requested_bytes,
                                seed_decline.anchors_out_growth_bytes);
                            direct_ledger->slab_actual_bytes = bioseq::common::saturating_add(
                                direct_ledger->slab_actual_bytes,
                                seed_decline.anchors_out_actual_growth_bytes);
                            direct_ledger->slab_retained_permit_bytes =
                                bioseq::common::saturating_add(
                                    direct_ledger->slab_retained_permit_bytes,
                                    seed_decline.anchors_out_retained_permit_bytes);
                            direct_ledger->seed_reserved_bytes = bioseq::common::saturating_add(
                                direct_ledger->seed_reserved_bytes, seed_decline.reserved_bytes);
                            direct_ledger->seed_planned_vulkan_bytes =
                                bioseq::common::saturating_add(
                                    direct_ledger->seed_planned_vulkan_bytes,
                                    seed_decline.planned_vulkan_allocation_bytes);
                            direct_ledger->seed_actual_vulkan_bytes =
                                bioseq::common::saturating_add(
                                    direct_ledger->seed_actual_vulkan_bytes,
                                    seed_decline.actual_vulkan_allocation_bytes);
                            direct_ledger->slab_admission_started =
                                seed_decline.exact_anchor_count != 0u;
                            direct_ledger->seed_dispatches =
                                bioseq::common::saturating_add(direct_ledger->seed_dispatches,
                                                               seed_decline.seed_lookup_dispatches);
                            direct_ledger->seed_dispatch_started =
                                direct_ledger->seed_dispatch_started ||
                                seed_decline.seed_lookup_dispatches != 0u;
                            if (seed_completion) {
                                direct_ledger->seed_roundtrips = bioseq::common::saturating_add(
                                    direct_ledger->seed_roundtrips, 1u);
                                if (seed_decline.exact_anchor_count == 0u) {
                                    direct_ledger->zero_anchor_bypasses =
                                        bioseq::common::saturating_add(
                                            direct_ledger->zero_anchor_bypasses, 1u);
                                }
                                if (seed_decline.seed_lookup_dispatches != 0u) {
                                    direct_ledger->seed_fence_completions =
                                        bioseq::common::saturating_add(
                                            direct_ledger->seed_fence_completions, 1u);
                                }
                            }
                        }
                        if (seed_completion.retained_anchor_permit) {
                            if (seed_completion.retained_anchor_bytes !=
                                seed_completion.retained_anchor_permit.bytes()) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                    "seed permit byte authority is invalid"};
                            }
                            if (attempt_memory != nullptr) {
                                if (!attempt_memory->retention.retain_context_block(
                                        std::move(seed_completion.retained_anchor_permit))) {
                                    throw std::logic_error{"seed block permit lost its attempt owner"};
                                }
                            } else {
                                if (post_seed_lifetime == nullptr ||
                                    post_seed_lifetime->retained_seed_arena_permit) {
                                    throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                        "legacy seed permit owner is invalid"};
                                }
                                post_seed_lifetime->retained_seed_arena_permit =
                                    std::move(seed_completion.retained_anchor_permit);
                            }
                        }
                        if (resident_index && seed_completion) {
                            const std::array<std::uint32_t, 1u> submitted_ordinals{
                                submitted_query_ordinal};
                            if (!canonical_seed_jobs_match_submission(seed_batch, submitted_ordinals)) {
                                throw std::logic_error{"MPF seed jobs differ from the submitted query"};
                            }
                            if (stats_dst == nullptr) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                    "Vulkan seed stage has no execution-stats sink"};
                            }
                            partial_vulkan_execution_stats stats{};
                            stats.seed_lookup_dispatches = seed_decline.seed_lookup_dispatches;
                            stats.seed_open_addressed_lookup_dispatches =
                                seed_decline.open_addressed_lookup_dispatches;
                            stats.seed_sorted_dense_lookup_dispatches =
                                seed_decline.sorted_dense_lookup_dispatches;
                            stats.seed_bitonic_sort_dispatches =
                                seed_decline.sort_stats.bitonic_sort_dispatches;
                            stats.seed_radix_sort_operations =
                                seed_decline.sort_stats.radix_sort_operations;
                            stats.seed_radix_anchor_count =
                                seed_decline.sort_stats.radix_anchor_count;
                            stats.seed_radix_passes = seed_decline.sort_stats.radix_passes;
                            stats.seed_radix_histogram_dispatches =
                                seed_decline.sort_stats.radix_histogram_dispatches;
                            stats.seed_radix_scan_dispatches =
                                seed_decline.sort_stats.radix_scan_dispatches;
                            stats.seed_radix_scatter_dispatches =
                                seed_decline.sort_stats.radix_scatter_dispatches;
                            stats.seed_host_sort_fallbacks =
                                seed_decline.sort_stats.host_sort_fallbacks;
                            if (seed_decline.canonical_anchor_order_checked) {
                                stats.seed_anchor_stats_operations = 1u;
                                stats.seed_canonical_anchor_count =
                                    seed_decline.canonical_anchor_count;
                                stats.seed_canonical_anchor_order_checks = 1u;
                                stats.seed_canonical_anchor_order_accepts =
                                    seed_decline.canonical_anchor_order_accepted ? 1u : 0u;
                                stats.seed_canonical_tie_normalization_runs =
                                    seed_decline.canonical_tie_normalization_runs;
                                stats.seed_canonical_tie_normalization_anchors =
                                    seed_decline.canonical_tie_normalization_anchors;
                                stats.seed_anchor_digest_lane0 =
                                    seed_decline.canonical_anchor_digest_lo;
                                stats.seed_anchor_digest_lane1 =
                                    seed_decline.canonical_anchor_digest_hi;
                            }
                            // `index_upload` is populated only by the operation
                            // that actually materialized a new resident index.
                            // cache_type hits leave it zeroed, so these counters never
                            // multiply one upload by the number of queries reusing
                            // it.
                            const auto& index_upload = index_materialization;
                            if (index_upload.slot_capacity != 0u) {
                                stats.gpu_index_materializations = 1u;
                                if (index_upload.representation ==
                                    bioseq::indexing::gpu_index_representation::open_addressed_70) {
                                    stats.gpu_index_open_addressed_materializations = 1u;
                                } else {
                                    stats.gpu_index_sorted_dense_materializations = 1u;
                                }
                                stats.gpu_index_unique_keys = index_upload.unique_key_count;
                                stats.gpu_index_slot_capacity = index_upload.slot_capacity;
                                stats.gpu_index_logical_resident_bytes =
                                    index_upload.resident_bytes;
                                stats.gpu_index_actual_allocation_bytes =
                                    index_upload.actual_allocation_bytes;
                                stats.gpu_index_fully_touched_bytes =
                                    index_upload.fully_touched_bytes;
                                stats.gpu_index_staging_bytes = index_upload.staging_bytes;
                                stats.gpu_index_permit_releases =
                                    index_upload.materialization_permit_released ? 1u : 0u;
                                stats.gpu_index_process_footprint_reflections =
                                    index_upload.process_footprint_reflected ? 1u : 0u;
                                stats.gpu_index_reflected_footprint_bytes =
                                    index_upload.reflected_footprint_delta_bytes;
                                stats.gpu_index_max_load_ppm = index_upload.load_ppm;
                                stats.gpu_index_max_probe = index_upload.max_probe;
                            }
                            // `emit_batch` is synchronous: without dropping this
                            // local handle first, the multi-gigabyte resident index
                            // survives through chain_stage even if the cache is
                            // cleared.  Seed lookup has completed its Vulkan fence
                            // and copied anchors into `anchors_slab`, so neither
                            // index nor query-device storage is used downstream.
                            resident_index.reset();
                            device_span.reset();
                            if (post_seed_lifetime != nullptr) {
                                const auto release = post_seed_lifetime->complete_seed_user();
                                stats.post_seed_owned_gpu_cache_evictions =
                                    release.gpu_cache_evictions;
                                stats.post_seed_owned_gpu_resident_bytes_released =
                                    release.gpu_resident_bytes_released;
                                stats.post_seed_owned_cpu_index_releases =
                                    release.cpu_index_releases;
                                stats.post_seed_owned_cpu_position_bytes_released =
                                    release.cpu_position_bytes_released;
                            }
                            stats_dst->record_partial_vulkan_execution_stats(stats);
                            return std::move(token).emit_batch(
                                std::span<anchor>{anchors_slab.data(), anchors_slab.size()});
                        }
                        seed_gpu_declined = true;
                    } else {
                        seed_decline.reason = bioseq::seeding::vulkan_seed_lookup_decline_reason::
                            hardware_device_unavailable;
                        seed_gpu_declined = true;
                    }
                } else {
                    seed_decline.reason = bioseq::seeding::vulkan_seed_lookup_decline_reason::
                        resident_index_unavailable;
                    seed_gpu_declined = true;
                }
                if (seed_gpu_declined) {
                    if (!seed_decline.is_declined()) {
                        seed_decline.reason = bioseq::seeding::vulkan_seed_lookup_decline_reason::
                            resident_index_unavailable;
                    }
                    bioseq::seeding::throw_vulkan_seed_lookup_strict_decline("Vulkan seed lookup",
                                                                             seed_decline);
                }
            }

            if (direct_ledger != nullptr) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct resident route cannot fall back to CPU seeding"};
            }

            match_result_collection* const cpu_seed_stats_dst = stats_dst;
            post_seed_owned_lifetime* const cpu_seed_owned_lifetime = post_seed_lifetime;
            match_result_collection* const cpu_seed_host_timing_dst = host_timing_dst;
            const auto record_cpu_seed_completion = [cpu_seed_stats_dst,
                                                     sketch_span,
                                                     cpu_seed_owned_lifetime,
                                                     cpu_seed_host_timing_dst,
                                                     host_stage_started] {
                // A successful strict Vulkan lookup returned above, so this stats
                // can only describe a real CPU seed operation.
                if (cpu_seed_stats_dst == nullptr) {
                    bioseq_log<logging_level::error>("CPU seed stage has no execution-stats sink");
                    std::abort();
                }
                scalar_pipeline_execution_stats stats{};
                stats.record_seed_lookup(static_cast<std::uint64_t>(sketch_span.size()));
                cpu_seed_stats_dst->record_scalar_pipeline_execution_stats(stats);

                if (cpu_seed_owned_lifetime != nullptr) {
                    // CPU-index retirement is a lifetime observation, not Vulkan
                    // execution statistics. Prepared fragment batches are normally
                    // externally owned and leave this pointer null.
                    (void)cpu_seed_owned_lifetime->complete_seed_user();
                }
                if (cpu_seed_host_timing_dst != nullptr) {
                    cpu_seed_host_timing_dst->record_seed_lookup_host_time(
                        elapsed_host_stage_ns(host_stage_started));
                }
            };

            if constexpr (bioseq::indexing::prepared_fragment_anchor_query_index<
                              std::remove_cvref_t<p_index>>) {
                const std::span<const anchor> ordered_anchors =
                    target_index_ref.ordered_fragment_seed_anchors(
                        sketch_span,
                        query_len,
                        parameters().max_occ,
                        kmer_size);
                record_cpu_seed_completion();
                // The owning PreparedFragmentAnchorBatch outlives this synchronous
                // pipeline call. Emit its already-canonical span directly: no
                // arena_slab allocation, anchor copy, or global fragment sort.
                return std::move(token).emit_batch(ordered_anchors);
            } else {
                // Ordinary per-hash lookup retains the arena-producing kernel.
                // The metadata permit precedes its physical vector; neither
                // reserves the logical anchor extent a second time.
                bioseq::parallelism::memory_permit canonical_job_permit{};
                canonical_seed_batch seed_batch{};
                if (attempt_memory != nullptr) {
                    attempt_memory->prepare_seed_job_storage(
                        seed_batch.jobs, canonical_job_permit, 1u);
                } else {
                    seed_batch.jobs.reserve(1u);
                }
                seed_batch.anchors = make_arena_slab<anchor>();
                auto& anchors_slab = seed_batch.anchors;
                anchors_slab.reserve(detail::estimated_anchor_count(
                    sketch_span.size(),
                    static_cast<std::size_t>(query_len),
                    static_cast<std::size_t>(target_index_ref.target_length()),
                    kmer_size));
                collect_seed_hits<p_minimizer>(sketch_span,
                                               target_index_ref,
                                               query_len,
                                               parameters().max_occ,
                                               kmer_size,
                                               anchors_slab);
                seed_batch.jobs.push_back(
                    {submitted_query_ordinal, 0u, anchors_slab.size()});
                const std::array<std::uint32_t, 1u> submitted_ordinals{
                    submitted_query_ordinal};
                if (!canonical_seed_jobs_match_submission(seed_batch, submitted_ordinals)) {
                    throw std::logic_error{"host seed jobs differ from the submitted query"};
                }
                record_cpu_seed_completion();
                return std::move(token).emit_batch(
                    std::span<anchor>{anchors_slab.data(), anchors_slab.size()});
            }
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = "Seed hits + canonical CPU ordering";
            algo.parameters.push_back({"max_occ", std::to_string(parameters().max_occ)});
            algo.parameters.push_back({"query_len", std::to_string(query_len)});
            desc.algorithm = std::move(algo);
            // Reserve the estimated anchor count for the stage slab. The actual
            // sketch count is unknown at
            // describe time so the reported figure uses query_len/window=10 as
            // the canonical median; the runtime reserve picks a tighter value
            // when the real sketches.size() is available.
            const std::size_t approx_sketch_count = static_cast<std::size_t>(query_len) / 10u + 64u;
            desc.estimated_batch_size =
                detail::estimated_anchor_count(approx_sketch_count,
                                               static_cast<std::size_t>(query_len),
                                               /*target_bp=*/static_cast<std::size_t>(query_len),
                                               parameters().kmer_size);
            return desc;
        }
    };

    [[nodiscard]] inline ::bioseq::prefilter::resolved_prefilter_algorithm
    resolve_prefilter_algorithm(::bioseq::prefilter::prefilter_backend backend,
                                std::span<const prefilter::prefilter_job> jobs) noexcept {
        using backend_type = ::bioseq::prefilter::prefilter_backend;
        using algorithm_type = ::bioseq::prefilter::resolved_prefilter_algorithm;
        if (backend != backend_type::vulkan) {
            return algorithm_type::sneaky_snake;
        }
        for (const prefilter::prefilter_job& job : jobs) {
            const bool measured_length = job.query_length == job.target_length &&
                                         (job.query_length == 32u || job.query_length == 64u);
            const bool measured_threshold = job.max_edits == 2u || job.max_edits == 4u;
            if (!measured_length || !measured_threshold) {
                return algorithm_type::sneaky_snake;
            }
        }
        return jobs.empty() ? algorithm_type::sneaky_snake : algorithm_type::gatekeeper;
    }

    /**
     * Query-scoped rank-prefilter lower-bound gate.  Only mandatory global
     * seed-to-seed fills are evaluated. Local left/right extension windows are
     * intentionally excluded.  A region is removed only when the sum of proven
     * per-fill lower bounds exceeds the SQL predicate's maximum edit count.
     */
    template <sequence_alphabet p_alphabet>
    struct prefilter_stage : stage_base<prefilter_stage<p_alphabet>, alignment_region> {
        static constexpr std::string_view stage_name = "prefilter_stage";

        statement_execution_plan_owner execution_plan;

        /// Borrow the sole immutable statement snapshot.
        [[nodiscard, gnu::always_inline]] const minimap2_parameters&
        parameters() const noexcept {
            return execution_plan->frozen_parameters;
        }
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_;
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq_;
        match_result_collection* stats_dst_ = nullptr;

        [[nodiscard]] static constexpr std::string_view
        backend_name(::bioseq::prefilter::prefilter_backend backend) noexcept {
            using backend_type = ::bioseq::prefilter::prefilter_backend;
            switch (backend) {
            case backend_type::automatic:
                return "auto";
            case backend_type::scalar:
                return "scalar";
            case backend_type::simd:
                return "highway_simd";
            case backend_type::vulkan:
                return "vulkan";
            }
            return "unknown";
        }

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            if (!parameters().prefilter_enabled || !parameters().compute_edit_distance || regions.size() == 0) {
                return std::move(token).emit_batch_pair(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const anchor>{anchors.data(), anchors.size()});
            }

            static const auto* prefilter_region =
                bioseq::instrumentation::register_static_region("prefilter");
            auto prefilter_scope = bioseq::instrumentation::region(prefilter_region);

            std::vector<std::uint8_t> query_ranks;
            std::vector<std::uint8_t> target_ranks;
            std::vector<prefilter::prefilter_job> jobs;
            std::vector<std::size_t> job_regions;
            const std::uint64_t max_flat_offset = (std::numeric_limits<std::uint32_t>::max)();

            for (std::size_t region_index = 0; region_index < regions.size(); ++region_index) {
                const alignment_region& region = regions[region_index];
                (void)for_each_mandatory_alignment_gap(
                    region,
                    std::span<const anchor>{anchors.data(), anchors.size()},
                    parameters(),
                    [this,
                     &query_ranks,
                     &target_ranks,
                     &jobs,
                     &job_regions,
                     &region,
                     region_index,
                     max_flat_offset](const mandatory_alignment_gap_window& window) {
                        const std::uint64_t query_length = window.query_end - window.query_start;
                        const std::uint64_t target_length = window.target_end - window.target_start;
                        if (window.query_end > query_seq_.bp_size() ||
                            window.target_end > target_seq_.bp_size() ||
                            query_length > max_flat_offset || target_length > max_flat_offset ||
                            query_ranks.size() > max_flat_offset - query_length ||
                            target_ranks.size() > max_flat_offset - target_length) {
                            /* Unsupported valid geometry is a conservative accept,
                             * never a hidden fallback or false rejection. */
                            return true;
                        }

                        const auto query_offset = static_cast<std::uint32_t>(query_ranks.size());
                        const auto target_offset = static_cast<std::uint32_t>(target_ranks.size());
                        query_ranks.resize(query_ranks.size() +
                                           static_cast<std::size_t>(query_length));
                        target_ranks.resize(target_ranks.size() +
                                            static_cast<std::size_t>(target_length));
                        std::span<std::uint8_t> query_destination{
                            query_ranks.data() + query_offset,
                            static_cast<std::size_t>(query_length)};
                        std::span<std::uint8_t> target_destination{
                            target_ranks.data() + target_offset,
                            static_cast<std::size_t>(target_length)};
                        if (region.is_reverse) {
                            query_seq_.template copy_ranks<true, false>(
                                query_destination,
                                static_cast<std::size_t>(window.query_start),
                                static_cast<std::size_t>(window.query_end));
                        } else {
                            query_seq_.template copy_ranks<false, false>(
                                query_destination,
                                static_cast<std::size_t>(window.query_start),
                                static_cast<std::size_t>(window.query_end));
                        }
                        target_seq_.template copy_ranks<false, false>(
                            target_destination,
                            static_cast<std::size_t>(window.target_start),
                            static_cast<std::size_t>(window.target_end));
                        jobs.push_back(prefilter::prefilter_job{
                            .query_offset = query_offset,
                            .target_offset = target_offset,
                            .query_length = static_cast<std::uint32_t>(query_length),
                            .target_length = static_cast<std::uint32_t>(target_length),
                            .max_edits = parameters().prefilter_max_edits,
                        });
                        job_regions.push_back(region_index);
                        return true;
                    });
            }

            if (jobs.empty()) {
                return std::move(token).emit_batch_pair(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const anchor>{anchors.data(), anchors.size()});
            }

            const prefilter::prefilter_batch_input input{
                .query_ranks = query_ranks,
                .target_ranks = target_ranks,
                .jobs = jobs,
            };
            std::vector<prefilter::prefilter_result> results;
            using backend_type = ::bioseq::prefilter::prefilter_backend;
            using algorithm_type = ::bioseq::prefilter::resolved_prefilter_algorithm;
            const backend_type selected = parameters().prefilter_backend == backend_type::automatic
                                              ? backend_type::simd
                                              : parameters().prefilter_backend;
            const algorithm_type algorithm = resolve_prefilter_algorithm(selected, jobs);
            switch (selected) {
            case backend_type::scalar:
                results = sneaky_snake_scalar_batch(input);
                break;
            case backend_type::simd:
            case backend_type::automatic: {
                if (parameters().highway_kernels == nullptr ||
                    !parameters().highway_kernels->has_kernel(::bioseq::simd_dispatch::highway_kernel::
                                                            prefilter_sneaky_snake_simd_batch) ||
                    parameters().highway_kernels->common.sneaky_snake_simd_batch == nullptr) {
                    throw std::logic_error{"selected Highway table has no SneakySnake kernel"};
                }
                results = parameters().highway_kernels->common.sneaky_snake_simd_batch(input, {});
                break;
            }
            case backend_type::vulkan: {
                if (algorithm == algorithm_type::gatekeeper) {
                    results =
                        prefilter::run_gatekeeper_vulkan_batch(query_ranks, target_ranks, jobs);
                } else {
                    results =
                        prefilter::run_sneaky_snake_vulkan_batch(query_ranks, target_ranks, jobs);
                }
                break;
            }
            }
            if (results.size() != jobs.size()) {
                throw std::runtime_error("rank prefilter backend returned the wrong result count");
            }

            const std::size_t input_region_count = regions.size();
            std::uint64_t evaluated_jobs = 0u;
            std::uint64_t length_64_jobs = 0u;
            std::uint64_t length_difference_jobs = 0u;
            std::uint64_t unsupported_jobs = 0u;
            std::uint64_t rejected_jobs = 0u;
            std::uint64_t mask_rejection_candidates = 0u;
            std::uint64_t exact_guard_activations = 0u;
            std::uint64_t guard_confirmed_rejections = 0u;
            std::uint64_t hamming_exact_rejections = 0u;
            std::vector<std::uint64_t> lower_bounds(regions.size(), 0u);
            std::vector<bool> keep(regions.size(), true);
            for (std::size_t index = 0; index < results.size(); ++index) {
                const prefilter::prefilter_result& result = results[index];
                const std::size_t region_index = job_regions[index];
                switch (static_cast<prefilter::prefilter_status>(result.status)) {
                case prefilter::prefilter_status::success:
                    ++evaluated_jobs;
                    if (jobs[index].query_length == 64u && jobs[index].target_length == 64u) {
                        ++length_64_jobs;
                    }
                    break;
                case prefilter::prefilter_status::conservative_unequal:
                    ++length_difference_jobs;
                    break;
                case prefilter::prefilter_status::unsupported_conservative:
                    ++unsupported_jobs;
                    break;
                default:
                    throw std::runtime_error("rank prefilter backend returned an unknown status");
                }
                if (algorithm == algorithm_type::gatekeeper) {
                    mask_rejection_candidates +=
                        prefilter::is_gatekeeper_mask_rejection_candidate(result) ? 1u : 0u;
                    exact_guard_activations +=
                        prefilter::is_gatekeeper_exact_guard_activated(result) ? 1u : 0u;
                    guard_confirmed_rejections +=
                        prefilter::is_gatekeeper_guard_confirmed_rejection(result) ? 1u : 0u;
                    hamming_exact_rejections +=
                        prefilter::is_gatekeeper_hamming_exact_rejection(result) ? 1u : 0u;
                }
                if (!prefilter::is_prefilter_accepted(result))
                    ++rejected_jobs;
                if (result.status == static_cast<std::uint32_t>(
                                         prefilter::prefilter_status::unsupported_conservative))
                    continue;
                lower_bounds[region_index] = bioseq::common::saturating_add(
                    lower_bounds[region_index], static_cast<std::uint64_t>(result.lower_bound));
                if (!prefilter::is_prefilter_accepted(result) ||
                    lower_bounds[region_index] > parameters().prefilter_max_edits) {
                    keep[region_index] = false;
                }
            }

            std::size_t output_size = 0;
            for (std::size_t index = 0; index < regions.size(); ++index) {
                if (!keep[index])
                    continue;
                if (output_size != index) {
                    regions.data()[output_size] = std::move(regions.data()[index]);
                }
                ++output_size;
            }
            if (stats_dst_ == nullptr) {
                throw std::logic_error("rank prefilter stage has no execution-stats sink");
            }
            ::bioseq::prefilter::prefilter_execution_stats call_stats{};
            call_stats.record_batch(algorithm,
                                    selected,
                                    static_cast<std::uint64_t>(jobs.size()),
                                    evaluated_jobs,
                                    length_difference_jobs,
                                    unsupported_jobs,
                                    rejected_jobs,
                                    static_cast<std::uint64_t>(input_region_count - output_size),
                                    static_cast<std::uint64_t>(output_size),
                                    mask_rejection_candidates,
                                    exact_guard_activations,
                                    guard_confirmed_rejections,
                                    hamming_exact_rejections,
                                    length_64_jobs);
            stats_dst_->record_prefilter_execution_stats(call_stats);
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), output_size},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            using backend_type = ::bioseq::prefilter::prefilter_backend;
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (parameters().prefilter_enabled) {
                stage_algorithm_description algorithm;
                algorithm.name = "Rank edit-distance prefilter";
                algorithm.parameters.push_back(
                    {"backend", std::string{backend_name(parameters().prefilter_backend)}});
                algorithm.parameters.push_back({"algorithm",
                                                parameters().prefilter_backend == backend_type::vulkan
                                                    ? "batch_resolved_gatekeeper_or_sneaky_snake"
                                                    : "sneaky_snake"});
                algorithm.parameters.push_back(
                    {"max_edits", std::to_string(parameters().prefilter_max_edits)});
                algorithm.parameters.push_back(
                    {"submaze_width",
                     std::to_string(prefilter::k_sneaky_snake_default_submaze_width)});
                algorithm.computed_properties.push_back(
                    {"domain", "mandatory seed-to-seed global fills"});
                algorithm.computed_properties.push_back(
                    {"survivor_check", "exact post-alignment edit distance"});
                algorithm.computed_properties.push_back(
                    {"gatekeeper_auto_domain", "length in {32,64}, E in {2,4}"});
                if (parameters().prefilter_backend == backend_type::simd ||
                    parameters().prefilter_backend == backend_type::automatic) {
                    algorithm.highway_kernels = ::bioseq::simd_dispatch::highway_kernel_mask_for(
                        ::bioseq::simd_dispatch::highway_kernel::prefilter_sneaky_snake_simd_batch);
                }
                desc.algorithm = std::move(algorithm);
            }
            return desc;
        }
    };

    /// Return whether the resolved statement asks for any full biological Vulkan
    /// component. `vulkan_enabled` is deliberately absent: it is only the runtime
    /// capability gate, and enabling that gate must not turn an otherwise CPU/simd
    /// tuple into a full GPU-selected statement.  The rank prefilter is also absent:
    /// it is an explicitly requested, self-contained accelerator inside the generic
    /// pipeline and does not authorize GPU-to-CPU fallback for minimization,
    /// chaining, or alignment. The cache toggle is likewise only policy for a
    /// selected seed path.
    [[nodiscard]] inline bool
    vulkan_components_requested(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.vulkan_seed_hits_enabled || params.vulkan_fused_frontend_enabled ||
               params.vulkan_gpu_index_build_enabled || params.vulkan_extend_enabled ||
               params.minimizer_algorithm ==
                   ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer ||
               params.chaining_algorithm ==
                   ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming;
    }

    /// Return whether any Vulkan work can execute for this statement.  This is a
    /// thread-placement/runtime-lifetime predicate, not biological route
    /// admission: a standalone rank prefilter is allowed inside the generic
    /// CPU/simd pipeline while `vulkan_components_requested()` remains false.
    [[nodiscard]] inline bool
    vulkan_accelerator_requested(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return vulkan_components_requested(params) ||
               (params.prefilter_enabled &&
                params.prefilter_backend == ::bioseq::prefilter::prefilter_backend::vulkan);
    }

    /// Exact statement shape that the current dedicated resident GPU route can
    /// execute without entering any generic CPU/simd biological stage. A selected
    /// tuple outside this shape must fail before generic pipeline construction.
    [[nodiscard]] inline bool
    complete_all_gpu_tuple_selected(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.vulkan_enabled && params.vulkan_seed_hits_enabled &&
               params.vulkan_fused_frontend_enabled && params.vulkan_gpu_index_build_enabled &&
               params.minimizer_algorithm ==
                   ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer &&
               params.chaining_algorithm ==
                   ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming &&
               params.base_alignment_algorithm ==
                   ::bioseq::simd::base_alignment_algorithm::block_aligner &&
               // Dedicated target routes resolve the MAPQ projection internally.
               // Persisted grouped-SoA executes its physical Vulkan MAPQ stage;
               // routes without exact evidence remain explicitly unexecuted. The
               // projection never admits a CPU/SIMD biological stage.
               !params.compute_divergence && !params.compute_edit_distance &&
               !params.exact_edit_filter_enabled && !params.prefilter_enabled &&
               params.max_occ >= 0 && (!params.do_cigar || params.vulkan_extend_enabled);
    }

    /// Validate the target-format and algorithm tuple admitted by the dedicated
    /// persisted grouped-SoA GPU preparation route. The caller supplies the live
    /// supported-alphabet pack size so a legitimate zero-based alphabet code is
    /// distinguished from an out-of-range code.
    [[nodiscard]] inline bool is_persisted_grouped_soa_gpu_preparation_tuple_valid(
        const std::uint8_t alphabet_code,
        const std::size_t supported_alphabet_count,
        const std::uint8_t storage_unit_byte_width,
        const ::bioseq::minimizers::minimizer_sketch_backend backend,
        const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return static_cast<std::size_t>(alphabet_code) < supported_alphabet_count &&
               (storage_unit_byte_width == 1u || storage_unit_byte_width == 2u ||
                storage_unit_byte_width == 4u || storage_unit_byte_width == 8u) &&
               params.kmer_size == 15 && params.window_size == 10 &&
               backend == ::bioseq::minimizers::minimizer_sketch_backend::vulkan_segment &&
               complete_all_gpu_tuple_selected(params);
    }

    // The Vulkan frontend's GPU target-index build replaces the CPU
    // `minimizer_hashmap_fragment_index` build for this exact (minimizer, chaining)
    // leaf. runtime-gated; both flags plus the matching algorithm selectors must be
    // set. The runtime device check is done at the call site.
    [[nodiscard]] inline bool
    is_gpu_target_index_build_active(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.vulkan_enabled && params.vulkan_fused_frontend_enabled &&
               params.vulkan_gpu_index_build_enabled &&
               params.minimizer_algorithm ==
                   ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer &&
               params.chaining_algorithm ==
                   ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming;
    }

    [[nodiscard]] inline bool is_gpu_coordinate_only_execution_active(
        const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.vulkan_enabled && params.vulkan_seed_hits_enabled &&
               params.vulkan_fused_frontend_enabled &&
               params.minimizer_algorithm ==
                   ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer &&
               params.chaining_algorithm ==
                   ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming &&
               !params.do_cigar && !params.compute_divergence && !params.compute_mapq &&
               !params.compute_edit_distance && !params.exact_edit_filter_enabled &&
               !params.prefilter_enabled && params.max_occ >= 0;
    }

    struct ordered_sketch_query_gate_storage {
        static constexpr unsigned gate_bits = bioseq::gpu::vulkan::k_gpu_query_gate_bits;

        ordered_sketch_query_gate_storage() = default;
        ordered_sketch_query_gate_storage(const ordered_sketch_query_gate_storage&) = delete;
        ordered_sketch_query_gate_storage&
        operator=(const ordered_sketch_query_gate_storage&) = delete;
        ordered_sketch_query_gate_storage(ordered_sketch_query_gate_storage&&) = delete;
        ordered_sketch_query_gate_storage& operator=(ordered_sketch_query_gate_storage&&) = delete;

        // The only mutable owner is consumed at freeze.  Afterwards every user has
        // a shared_ptr<const vector>; no mutable alias can alter a published gate.
        mutable std::shared_ptr<std::vector<std::uint32_t>> mutable_words{};
        mutable std::shared_ptr<const std::vector<std::uint32_t>> frozen_words{};
        mutable bool frozen = false;
        std::uint32_t mask = 0u;

        void ensure_allocated() {
            if (frozen) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Stage C query gate mutation attempted after freeze");
            }
            if (mutable_words)
                return;
            const std::uint64_t bit_count = std::uint64_t{1} << gate_bits;
            mask = static_cast<std::uint32_t>(bit_count - 1u);
            mutable_words = std::make_shared<std::vector<std::uint32_t>>(
                static_cast<std::size_t>(bit_count / 32u), 0u);
        }

        template <class p_minimizer> void add(std::span<const p_minimizer> minimizers) {
            ensure_allocated();
            cached_identity.reset();
            for (const p_minimizer& minimizer : minimizers) {
                const std::uint64_t bit = minimizer.kmer() & mask;
                (*mutable_words)[static_cast<std::size_t>(bit >> 5u)] |= std::uint32_t{1}
                                                                         << (bit & 31u);
            }
        }

        [[nodiscard]] std::shared_ptr<const std::vector<std::uint32_t>>
        freeze_words() const noexcept {
            if (!frozen) {
                frozen_words =
                    std::shared_ptr<const std::vector<std::uint32_t>>{std::move(mutable_words)};
                frozen = true;
            }
            return frozen_words;
        }

        [[nodiscard]] bioseq::gpu::vulkan::vulkan_sketch_query_gate vulkan_gate() const noexcept {
            const auto gate_identity = canonical_identity();
            const auto owner = freeze_words();
            return bioseq::gpu::vulkan::vulkan_sketch_query_gate{
                owner ? std::span<const std::uint32_t>{owner->data(), owner->size()}
                      : std::span<const std::uint32_t>{},
                mask,
                bioseq::gpu::vulkan::vulkan_sketch_query_gate_identity{
                    .enabled = gate_identity.enabled,
                    .word_count = gate_identity.word_count,
                    .mask = gate_identity.mask,
                    .digest_lo = gate_identity.digest_lo,
                    .digest_hi = gate_identity.digest_hi,
                    .canonical_words = owner,
                },
                true};
        }

        struct identity {
            bool enabled = false;
            std::uint32_t word_count = 0;
            std::uint32_t mask = 0;
            std::uint64_t digest_lo = 0;
            std::uint64_t digest_hi = 0;
            std::uint64_t set_bits = 0;
        };
        mutable std::optional<identity> cached_identity{};

        /// Canonical 128-bit digest over little-endian u32 gate words. The cache is
        /// also bound once per wave, so this is identity metadata, not permission
        /// to share a filtered resident across unrelated waves.
        [[nodiscard]] identity canonical_identity() const noexcept {
            const auto owner = freeze_words();
            if (cached_identity)
                return *cached_identity;
            if (!owner) {
                cached_identity = identity{};
                return *cached_identity;
            }
            std::uint64_t lo = 1469598103934665603ULL;
            std::uint64_t hi = 0x9e3779b97f4a7c15ULL;
            std::uint64_t set_bits = 0u;
            for (const std::uint32_t word : *owner) {
                set_bits += std::popcount(word);
                for (unsigned shift = 0u; shift < 32u; shift += 8u) {
                    const std::uint8_t byte = static_cast<std::uint8_t>(word >> shift);
                    lo = (lo ^ byte) * 1099511628211ULL;
                    hi ^= static_cast<std::uint64_t>(byte) + 0x9e3779b97f4a7c15ULL + (hi << 6u) +
                          (hi >> 2u);
                    hi *= 0xbf58476d1ce4e5b9ULL;
                }
            }
            cached_identity = identity{
                .enabled = true,
                .word_count = static_cast<std::uint32_t>(owner->size()),
                .mask = mask,
                .digest_lo = lo,
                .digest_hi = hi,
                .set_bits = set_bits,
            };
            return *cached_identity;
        }

        [[nodiscard]] std::shared_ptr<const std::vector<std::uint32_t>>
        canonical_words() const noexcept {
            return freeze_words();
        }
    };

    inline void
    bind_gpu_query_gate_identity(bioseq::indexing::gpu_minimizer_index_cache* cache,
                                 const ordered_sketch_query_gate_storage& gate,
                                 const ordered_sketch_query_gate_storage::identity& gate_identity,
                                 bioseq::indexing::gpu_index_cache_identity& identity) {
        identity.gate_enabled = gate_identity.enabled;
        identity.gate_word_count = gate_identity.word_count;
        identity.gate_mask = gate_identity.mask;
        identity.gate_digest_lo = gate_identity.digest_lo;
        identity.gate_digest_hi = gate_identity.digest_hi;
        identity.gate_canonical_words = gate.canonical_words();
        if (cache == nullptr)
            return;
        using cache_type = bioseq::indexing::gpu_minimizer_index_cache;
        auto bound = cache->bind_gate_once(cache_type::gate_identity{
            .enabled = gate_identity.enabled,
            .word_count = gate_identity.word_count,
            .mask = gate_identity.mask,
            .digest_lo = gate_identity.digest_lo,
            .digest_hi = gate_identity.digest_hi,
            .canonical_words = gate.canonical_words(),
        });
        if (!bound) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                fmt::format("Stage C GPU index cache gate binding rejected ({})",
                            bound.error() == cache_type::gate_bind_error::identity_mismatch
                                ? "identity_mismatch"
                            : bound.error() == cache_type::gate_bind_error::invalid_identity
                                ? "invalid_identity"
                                : "cache_not_empty"));
        }
    }

    /// Freeze a query gate behind its immutable canonical owner once per wave.
    /// Segment loops reuse the returned value, so they neither alias mutable words
    /// nor re-hash/copy the 8 MiB gate for every target window.
    [[nodiscard]] inline bioseq::gpu::vulkan::vulkan_sketch_query_gate
    freeze_gpu_query_gate(bioseq::gpu::vulkan::vulkan_sketch_query_gate gate) {
        auto identity = gate.canonical_identity();
        if (!identity.is_well_formed()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Stage C query gate has a malformed canonical identity");
        }
        bioseq::gpu::vulkan::vulkan_sketch_query_gate frozen{};
        frozen.mask = identity.mask;
        frozen.identity = std::move(identity);
        frozen.identity_valid = true;
        if (frozen.identity.canonical_words) {
            frozen.words = std::span<const std::uint32_t>{frozen.identity.canonical_words->data(),
                                                          frozen.identity.canonical_words->size()};
        }
        return frozen;
    }

    /// Bind a segmented adoption key to the runtime build's authoritative frozen
    /// gate identity, after proving it is exactly the gate supplied to the build.
    /// Deriving a default/ungated key for a gated build is an identity mismatch.
    inline void bind_verified_gpu_segment_gate_identity(
        bioseq::indexing::gpu_minimizer_index_cache_key& key,
        const bioseq::gpu::vulkan::gpu_index_gate_identity& expected,
        const bioseq::gpu::vulkan::gpu_index_gate_identity& built) {
        if (!expected.is_well_formed() || !built.is_well_formed() ||
            !built.is_exactly_equal_to(expected)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "segmented GPU target-index gate identity mismatch");
        }
        key.gate_enabled = built.enabled;
        key.gate_word_count = built.word_count;
        key.gate_mask = built.mask;
        key.gate_digest_lo = built.digest_lo;
        key.gate_digest_hi = built.digest_hi;
        key.gate_canonical_words = built.canonical_words;
    }

    [[nodiscard]] inline match_result
    gpu_coordinate_to_match_result(const bioseq::gpu::vulkan::gpu_coordinate_result_wire& coord,
                                   sequence_size_t query_len,
                                   sequence_size_t target_len) {
        return match_result{
            .query_len = query_len,
            .query_start = coord.query_start,
            .query_end = coord.query_end,
            .is_reverse = coord.is_reverse != 0u,
            .target_len = target_len,
            .target_start = coord.target_start,
            .target_end = coord.target_end,
            .num_matches = coord.match_length,
            .block_length = coord.block_length,
            .mapq = 0,
            .chain_score = coord.chain_score,
            .dp_score = 0,
            .dp_max_score = 0,
            .seed_count = coord.seed_count,
            .secondary_chain_score = 0,
            .divergence = -1.0f,
            .is_primary = true,
            .has_cigar = false,
            .cigar = {},
        };
    }

    } // namespace bioseq::simd::pipe

} // export
