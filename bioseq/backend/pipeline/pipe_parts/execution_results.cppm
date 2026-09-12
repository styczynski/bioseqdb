/**
 * @file pipeline/pipe_parts/execution_results.cppm
 * @brief Assembles pipeline results and operation statistics.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:execution_results;

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
import :execution_scalar_highway;
import :execution_vulkan;

export {

    namespace bioseq::simd::pipe {

    // =============================================================================
    // make_minimap_pipeline — single source of truth for the pipeline shape
    // =============================================================================

    [[nodiscard]] inline bool
    vulkan_components_requested(const ::bioseq::simd::minimap2_parameters& params) noexcept;

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /// Pipeline description also constructs stages; only execution may attest work.
    [[gnu::always_inline]] inline void attest_benchmark_e2e_pipeline_execution(
        const statement_execution_plan_owner& execution_plan) {
        const auto& params = execution_plan->frozen_parameters;
        if (params.downstream_active_wall != nullptr &&
            !params.downstream_active_wall->attest_benchmark_e2e_statement_plan_checkpoint(
                execution_plan,
                benchmark_e2e_statement_plan_checkpoint::pipeline)) {
            throw std::logic_error{"post-seed observation changed the statement control block"};
        }
        const auto binding = current_benchmark_e2e_operation_binding();
        if (binding.is_available) {
            const auto observed_owner = binding.statement_owner.lock();
            if (!observed_owner || observed_owner.get() != execution_plan.get() ||
                observed_owner.owner_before(execution_plan) ||
                execution_plan.owner_before(observed_owner)) {
                throw std::logic_error{"post-seed job attribution belongs to a different statement"};
            }
        }
    }
#endif

    /**
     * @brief Construct the one selected chaining and ordinary host suffix.
     *
     * Query storage and the immutable statement snapshot are retained by the
     * graph even when divergence is disabled. This factor never seeds again.
     */
    template <is_constexpr_aligner_params p_aligner_parameters,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              sequence_alphabet p_alphabet,
              class p_minimizer>
    [[nodiscard, gnu::always_inline]] inline auto
    make_minimap_post_seed_pipeline(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        minimizer_batch_view<p_minimizer> query_minimizers,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
        match_result_collection& output_collection,
        statement_execution_plan_owner execution_plan,
        sequence_size_t query_len,
        sequence_size_t target_len,
        post_seed_owned_lifetime* owned_lifetime,
        route_attempt_memory* attempt_memory
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        , ::bioseq::gpu::vulkan::runtime_pipeline_operation_token* completed_chaining = nullptr
#endif
        ) {
        if (!execution_plan || !query_minimizers.has_host_owner()) {
            throw std::invalid_argument("post-seed pipeline requires plan and query ownership");
        }
        const auto& params = execution_plan->frozen_parameters;
        // The caller retains these owners beyond graph completion. Physical
        // allocations use the installed context's backing hook, not a second
        // logical-slab reservation at this construction boundary.
        (void)owned_lifetime;
        match_result_collection* host_timing_dst =
            &output_collection;
        match_result_collection* chaining_stats_dst = nullptr;
        if constexpr (p_chaining_algorithm == ::bioseq::simd::chaining_algorithm::scalar_minimap2 ||
                      p_chaining_algorithm == ::bioseq::simd::chaining_algorithm::simd_minimap2 ||
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                      p_chaining_algorithm ==
                          ::bioseq::simd::chaining_algorithm::range_minimum_query ||
#endif
                      p_chaining_algorithm ==
                          ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming) {
            chaining_stats_dst = &output_collection;
        }
        match_result_collection* alignment_stats_dst = nullptr;
        if constexpr (kernel_t<p_alignment_algorithm>::emits_block_execution_stats ||
                      kernel_t<p_alignment_algorithm>::emits_scalar_ksw2_execution_stats ||
                      kernel_t<p_alignment_algorithm>::emits_ksw2_batched_execution_stats) {
            alignment_stats_dst = &output_collection;
        }
        return make_pipeline<p_aligner_parameters>(
            chain_stage<kernel_t<p_chaining_algorithm>>{{},
                                                        params.make_chaining_parameters(),
                                                        query_len,
                                                        chaining_stats_dst,
                                                        host_timing_dst,
                                                        params.highway_kernels,
                                                        attempt_memory
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                        , completed_chaining
#endif
                                                        },
            region_stage{{}, query_len, host_timing_dst},
            primary_secondary_stage{{}, params.mask_level, params.mask_len, sub_diff(params)},
            secondary_filter_stage{{}, params.pri_ratio, min_diff(params), params.best_n},
            divergence_stage<p_minimizer, minimizer_batch_view<p_minimizer>>{{},
                                                              std::move(query_minimizers),
                                                              query_len,
                                                              target_len,
                                                              params.kmer_size,
                                                              params.compute_divergence},
            prefilter_stage<p_alphabet>{{}, execution_plan, query_seq, target_seq, &output_collection},
            align_stage<kernel_t<p_alignment_algorithm>, p_alphabet>{
                {},
                execution_plan,
                &scoring_matrix,
                &alignment_kernels,
                query_seq,
                target_seq,
                params.do_cigar || params.compute_edit_distance,
                alignment_stats_dst,
                host_timing_dst,
                attempt_memory},
            exact_edit_filter_stage{{}, execution_plan},
            primary_secondary_stage{{}, params.mask_level, params.mask_len, sub_diff(params)},
            secondary_filter_stage{{}, params.pri_ratio, min_diff(params), params.best_n},
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            mapq_stage{{}, execution_plan, params.compute_mapq, &output_collection},
#else
            mapq_stage{{}, execution_plan, params.compute_mapq},
#endif
            sink_stage{&output_collection, query_len, target_len, host_timing_dst,
                       execution_plan});
    }

    /**
     * @brief Runs exactly one authenticated canonical job through the shared suffix.
     *
     * The wave caller owns the canonical batch, query/target storage, physical
     * attempt and result capacity. No index or seeding stage is constructed here.
     * Empty jobs still enter the selected chaining stage exactly once.
     */
    template <is_constexpr_aligner_params p_aligner_parameters,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              sequence_alphabet p_alphabet,
              class p_minimizer>
    [[gnu::always_inline]] inline void run_minimap_post_seed_job(
        const canonical_seed_batch& seeds,
        std::size_t job_index,
        std::uint32_t expected_ordinal,
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        minimizer_batch_view<p_minimizer> query_minimizers,
        statement_execution_plan_owner execution_plan,
        match_result_collection& output,
        route_attempt_memory& attempt
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        , ::bioseq::gpu::vulkan::runtime_pipeline_operation_token* completed_chaining = nullptr
#endif
        ) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (completed_chaining != nullptr && completed_chaining->audit().kind !=
            ::bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid) {
            throw std::invalid_argument{"post-seed completion output already has authority"};
        }
        if (output.completed_vulkan_chaining.audit().kind !=
                ::bioseq::gpu::vulkan::runtime_pipeline_operation_kind::invalid ||
            !output.additional_completed_vulkan_chaining.empty()) {
            throw std::invalid_argument{"post-seed result already has chaining authority"};
        }
        ::bioseq::gpu::vulkan::runtime_pipeline_operation_token pending_chaining;
#endif
        if (!execution_plan || !query_minimizers.has_host_owner() ||
            job_index >= seeds.jobs.size() || !output.results.empty() ||
            context::current_or_null() != std::addressof(attempt.fresh_context)) {
            throw std::invalid_argument{"post-seed job has no fresh owned execution scope"};
        }
        const auto& params = execution_plan->frozen_parameters;
        const auto& job = seeds.jobs[job_index];
        if (params.base_alignment_algorithm != p_alignment_algorithm ||
            params.chaining_algorithm != p_chaining_algorithm ||
            job.ordinal != expected_ordinal || job.offset > seeds.anchors.size() ||
            job.count > seeds.anchors.size() - job.offset) {
            throw std::invalid_argument{"post-seed job differs from its selected statement"};
        }
        const std::span<const anchor> all_anchors{seeds.anchors.data(), seeds.anchors.size()};
        const auto job_anchors = all_anchors.subspan(job.offset, job.count);
        const auto& scoring = thread_local_scoring_matrix<p_alphabet>(params);
        const auto& alignment = detail::resolve_alignment_kernels<p_alphabet>(params);
        auto chain_scope = context::current().push<context::arena::chain>();
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        // Preserve any caller-admitted capacity, but keep the entire suffix
        // attempt private until its terminal callback succeeds.
        static_assert(std::is_nothrow_move_assignable_v<match_result_collection>);
        match_result_collection pending_output{std::move(output)};
        output = match_result_collection{};
        auto& suffix_output = pending_output;
#else
        auto& suffix_output = output;
#endif
        if (params.downstream_active_wall != nullptr) {
            suffix_output.attach_downstream_active_wall_tracker(*params.downstream_active_wall);
        }
        try {
            auto suffix = make_minimap_post_seed_pipeline<p_aligner_parameters,
                p_alignment_algorithm, p_chaining_algorithm, p_alphabet, p_minimizer>(
                    query_seq, target_seq, std::move(query_minimizers), scoring, alignment,
                    suffix_output, execution_plan, query_seq.bp_size(), target_seq.bp_size(),
                    nullptr, std::addressof(attempt)
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                    , &pending_chaining
#endif
                    );
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            attest_benchmark_e2e_pipeline_execution(execution_plan);
#endif
            suffix.on_batch(job_anchors);
            suffix.on_completed();
        } catch (...) {
            suffix_output.detach_downstream_active_wall_tracker();
            throw;
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (!suffix_output.mapq_execution_available) {
            suffix_output.record_mapq_execution(false);
        }
        suffix_output.detach_downstream_active_wall_tracker();
        output = std::move(pending_output);
        if (completed_chaining != nullptr) {
            // The caller receives this authority only after the entire selected
            // host suffix succeeds. Empty/host-chain jobs retain an unissued token.
            *completed_chaining = std::move(pending_chaining);
        } else {
            output.completed_vulkan_chaining = std::move(pending_chaining);
        }
#endif
        output.detach_downstream_active_wall_tracker();
    }

    /** @brief Prepend existing index/seed work to the one post-seed pipeline. */
    template <is_constexpr_aligner_params p_aligner_parameters,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm,
              pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index>
    [[nodiscard, gnu::always_inline]] inline auto
    make_minimap_pipeline(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        minimizer_batch_view<p_minimizer> query_minimizers,
        const p_index& target_index,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
        match_result_collection& output_collection,
        statement_execution_plan_owner execution_plan,
        sequence_size_t query_len,
        sequence_size_t target_len,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        post_seed_owned_lifetime* owned_lifetime = nullptr,
        route_attempt_memory* attempt_memory = nullptr
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        , ::bioseq::gpu::vulkan::runtime_pipeline_operation_token* completed_chaining = nullptr
#endif
        ) {
        if (!execution_plan) {
            throw std::invalid_argument("pipeline requires statement plan ownership");
        }
        const auto& params = execution_plan->frozen_parameters;
        std::uint32_t submitted_query_ordinal = 0u;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const auto operation_binding = current_benchmark_e2e_operation_binding();
        if (operation_binding.is_available) {
            if (operation_binding.origin.query_ordinal >
                (std::numeric_limits<std::uint32_t>::max)()) {
                throw std::length_error{"seed query ordinal exceeds the submitted wire domain"};
            }
            submitted_query_ordinal =
                static_cast<std::uint32_t>(operation_binding.origin.query_ordinal);
        }
#endif
        auto post_seed = make_minimap_post_seed_pipeline<p_aligner_parameters,
            p_alignment_algorithm, p_chaining_algorithm, p_alphabet, p_minimizer>(
                query_seq, target_seq, std::move(query_minimizers), scoring_matrix,
                alignment_kernels, output_collection, execution_plan, query_len,
                target_len, owned_lifetime, attempt_memory
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                , completed_chaining
#endif
                );
        auto tail = make_pipeline<p_aligner_parameters>(
            index_stage<p_index>{{}, &target_index, gpu_index_identity},
            seed_hit_stage{{}, execution_plan, query_len, gpu_index_cache,
                           &output_collection, owned_lifetime, &output_collection,
                           attempt_memory, submitted_query_ordinal},
            std::move(post_seed));
        if constexpr (p_entry_point == pipeline_entry_point::from_sequence) {
            minimizer_stage<p_minimizer, kernel_t<p_minimizer_algorithm>, p_alphabet> head{
                {}, query_seq, params.window_size, params.kmer_size, execution_plan,
                &output_collection, &output_collection};
            return head.template attach<p_aligner_parameters>(std::move(tail));
        } else {
            return tail;
        }
    }

    // =============================================================================
    // pipeline_run_impl_batch — batched leaf dispatcher
    // =============================================================================

    /**
     * @brief Run the pipeline over N queries against one shared target index.
     *
     * Opens a single outer `Arena::chain` scope for the whole batch and a nested
     * inner scope per query so each query's slabs are reclaimed before the next
     * iteration's pipeline is built.  Per-query results are appended to
     * `out_per_query` in input order.
     */
    // Batched execution shares one implementation for all alignment algorithms.
    template <is_constexpr_aligner_params p_aligner_parameters,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm,
              pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index>
    [[nodiscard]] match_result_collection_vector pipeline_run_impl_batch(
        std::span<const bioseq::storage::lazy_sequence_view<p_alphabet>> query_seqs,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        std::span<const minimizer_batch_view<p_minimizer>> query_minimizers_per_query,
        const p_index& target_index,
        statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* external_gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false) {
        if (!execution_plan) {
            throw std::invalid_argument("pipeline execution requires statement plan ownership");
        }
        const auto& params = execution_plan->frozen_parameters;
        std::optional<context::context> owned_context;
        std::optional<context::installed_context_scope> install;
        if (context::current_or_null() == nullptr) {
            owned_context.emplace();
            install.emplace(*owned_context);
        }
        auto outer_chain_scope = context::current().push<context::arena::chain>();

        bioseq_log<logging_level::debug>(
            "[bioseq] pipe: pipeline_run_impl_batch entry — "
            "p_aligner_parameters(p_bucket_width={},p_kmer_size={},p_window_size={},fallback={}) "
            "p_alignment_algorithm={} p_chaining_algorithm={} p_minimizer_algorithm={} "
            "p_entry_point={} queries={} target_bp={}",
            p_aligner_parameters::sequence_len_log2,
            p_aligner_parameters::kmer_len,
            p_aligner_parameters::window_len,
            p_aligner_parameters::generic_fallback ? 1 : 0,
            static_cast<int>(p_alignment_algorithm),
            static_cast<int>(p_chaining_algorithm),
            static_cast<int>(p_minimizer_algorithm),
            static_cast<int>(p_entry_point),
            query_seqs.size(),
            target_seq.bp_size());

        const sequence_size_t target_len = target_seq.bp_size();
        const auto& scoring_matrix = thread_local_scoring_matrix<p_alphabet>(params);
        // Resolve once for the entire alphabet-homogeneous query wave. Every
        // per-query pipeline borrows this immutable typed binding.
        const auto& alignment_kernels = detail::resolve_alignment_kernels<p_alphabet>(params);

        /// One GPU index cache for the whole batch wave, shared
        // across queries that look up the same target index. When a Vulkan seed
        // path is selected, runtime/device failure is reported before scalar work
        // can masquerade as a GPU row.
        std::optional<bioseq::indexing::gpu_minimizer_index_cache> owned_gpu_index_cache;
        if (external_gpu_index_cache == nullptr && params.vulkan_enabled &&
            params.vulkan_seed_hits_enabled) {
            bioseq::gpu::vulkan::runtime& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
            std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
                                      ? gpu_rt.features().device_local_bytes / 2u
                                      : (std::uint64_t{512} << 20);
            if (limit < (std::uint64_t{512} << 20))
                limit = std::uint64_t{512} << 20;
            if (limit > (std::uint64_t{4} << 30))
                limit = std::uint64_t{4} << 30;
            owned_gpu_index_cache.emplace(limit);
        }
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache_ptr =
            external_gpu_index_cache != nullptr
                ? external_gpu_index_cache
                : (owned_gpu_index_cache ? &*owned_gpu_index_cache : nullptr);

        /// Build the target minimizer index on the GPU and inject
        // it into the wave cache under the key the seed_hit_stage will compute, so the
        // CPU `minimizer_hashmap_fragment_index` build (skipped upstream in
        // `minimap2_port` for this leaf) is never paid. If the device build declines
        // this selected GPU path fails loudly.
        ordered_sketch_query_gate_storage gpu_query_gate;
        // A production target index is complete and query-independent.  Keep the
        // gate type for explicitly named direct-matching experiments, but publish
        // the ordinary/no-index build under the canonical disabled identity.
        const auto gpu_query_gate_identity = gpu_query_gate.canonical_identity();
        if (is_gpu_target_index_build_active(params)) {
            bind_gpu_query_gate_identity(
                gpu_index_cache_ptr, gpu_query_gate, gpu_query_gate_identity, gpu_index_identity);
        }

        const p_index* effective_index_ptr = &target_index;
        bool vulkan_target_index_built = false;
        bioseq::gpu::vulkan::gpu_index_build_stats vulkan_target_index_ownership{};
        if constexpr (std::same_as<std::remove_cvref_t<p_index>,
                                   bioseq::indexing::minimizer_hashmap_fragment_index>) {
            if (gpu_index_cache_ptr != nullptr && is_gpu_target_index_build_active(params) &&
                allow_gpu_target_index_build && target_index.table_empty() && target_len > 0) {
                auto& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
                bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
                auto built = bioseq::gpu::vulkan::build_gpu_index_on_device<p_minimizer>(
                    target_seq,
                    static_cast<std::size_t>(params.window_size),
                    static_cast<std::size_t>(params.kmer_size),
                    gpu_query_gate.vulkan_gate(),
                    &decline);
                if (!built) {
                    throw_vulkan_sketch_declined("Stage C GPU target-index build", decline);
                }
                auto key =
                    bioseq::indexing::make_gpu_index_cache_key(target_index, gpu_index_identity);
                auto adopted =
                    bioseq::indexing::adopt_gpu_built_index(gpu_rt, std::move(*built), key);
                if (!adopted) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                        "Stage C GPU target-index adoption rejected ({})",
                        bioseq::indexing::gpu_built_index_adoption_error_name(adopted.error())));
                }
                auto resident = std::move(*adopted);
                vulkan_target_index_ownership = resident->ownership;
                resident->target_len = target_len;
                auto inserted = gpu_index_cache_ptr->insert_built(key, resident);
                if (!inserted) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        fmt::format("Stage C GPU target-index cache insertion rejected ({})",
                                    static_cast<unsigned>(inserted.error())));
                }
                vulkan_target_index_built = true;
                bioseq_log<logging_level::error>(
                    "[gpu_target_index] GPU index injected: slot_mask={} pos={} target_len={}",
                    resident->slot_mask,
                    resident->position_count,
                    target_len);
            }
        }
        const p_index& effective_index = *effective_index_ptr;

        // Empty proto for the from_sequence path's unused minimizers argument.
        static const auto empty_minimizers_proto = minimizer_batch_view<p_minimizer>::from_shared_host(
            std::make_shared<const std::vector<p_minimizer>>(), 0u, 0u);
        auto run_one_query = [target_len,
                              &query_seqs,
                              &query_minimizers_per_query,
                              target_seq,
                              &effective_index,
                              &scoring_matrix,
                              &alignment_kernels,
                              execution_plan,
                              gpu_index_cache_ptr,
                              gpu_index_identity,
                              vulkan_target_index_built,
                              vulkan_target_index_ownership,
                              gpu_query_gate_identity](std::size_t qi) {
            const auto& params = execution_plan->frozen_parameters;
            const auto query_seq = query_seqs[qi];
            const sequence_size_t query_len = query_seq.bp_size();

            auto per_query_chain_scope = context::current().push<context::arena::chain>();

            match_result_collection out;
            if (vulkan_target_index_built && is_ordered_target_build_owner_query(qi)) {
                partial_vulkan_execution_stats stats{};
                stats.target_sketch_dispatches = 1;
                stats.target_index_build_dispatches = 1;
                stats.gpu_index_count_admissions =
                    vulkan_target_index_ownership.count_submit_admissions;
                stats.gpu_index_main_peak_admissions =
                    vulkan_target_index_ownership.execution_peak_admissions;
                stats.gpu_index_resident_transfers =
                    vulkan_target_index_ownership.resident_transfers;
                stats.gpu_index_adoption_rereserve_attempts =
                    vulkan_target_index_ownership.adoption_rereserve_attempts;
                stats.gpu_index_adoption_successes =
                    vulkan_target_index_ownership.adoption_successes;
                stats.gpu_index_owned_resident_bytes = vulkan_target_index_ownership.resident_bytes;
                stats.gpu_index_gate_enabled = vulkan_target_index_ownership.query_gate_enabled;
                stats.gpu_index_gate_word_count =
                    vulkan_target_index_ownership.query_gate_word_count;
                stats.gpu_index_gate_mask = vulkan_target_index_ownership.query_gate_mask;
                stats.gpu_index_gate_set_bits = gpu_query_gate_identity.set_bits;
                stats.gpu_index_gate_digest_lo = vulkan_target_index_ownership.query_gate_digest_lo;
                stats.gpu_index_gate_digest_hi = vulkan_target_index_ownership.query_gate_digest_hi;
                stats.gpu_index_gate_count_dispatches =
                    vulkan_target_index_ownership.gate_count_dispatches;
                stats.gpu_index_gated_emit_dispatches =
                    vulkan_target_index_ownership.gated_emit_dispatches;
                stats.gpu_index_ungated_emit_dispatches =
                    vulkan_target_index_ownership.ungated_emit_dispatches;
                stats.gpu_index_exact_count_dispatches =
                    vulkan_target_index_ownership.exact_count_dispatches;
                stats.gpu_index_exact_count_prefix_scan_dispatches =
                    vulkan_target_index_ownership.exact_count_prefix_scan_dispatches;
                stats.gpu_index_exact_count_control_readbacks =
                    vulkan_target_index_ownership.exact_count_control_readbacks;
                stats.gpu_index_deterministic_tile_emit_dispatches =
                    vulkan_target_index_ownership.deterministic_tile_emit_dispatches;
                stats.gpu_index_atomic_append_dispatches =
                    vulkan_target_index_ownership.atomic_append_dispatches;
                stats.gpu_index_emit_validation_control_readbacks =
                    vulkan_target_index_ownership.emit_validation_control_readbacks;
                stats.gpu_index_counted_minimizer_count =
                    vulkan_target_index_ownership.counted_minimizer_count;
                stats.gpu_index_verified_emit_count =
                    vulkan_target_index_ownership.verified_emit_count;
                stats.gpu_index_count_emit_parity_verified =
                    vulkan_target_index_ownership.count_emit_parity_verified;
                stats.gpu_index_target_minimizer_capacity_estimate =
                    vulkan_target_index_ownership.target_minimizer_capacity_estimate;
                stats.gpu_index_capacity_estimate_underestimated =
                    vulkan_target_index_ownership.capacity_estimate_underestimated;
                stats.gpu_index_arena_trimmed_before_permit_shrink =
                    vulkan_target_index_ownership.arena_trimmed_before_permit_shrink;
                stats.gpu_index_retained_minimizer_count =
                    vulkan_target_index_ownership.retained_minimizer_count;
                stats.gpu_index_distinct_minimizer_count =
                    vulkan_target_index_ownership.distinct_minimizer_count;
                out.record_partial_vulkan_execution_stats(stats);
            }
#if !defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            out.results.reserve(detail::estimated_result_count(query_len, target_len, params));
#endif

            const auto& mins_for_this_query =
                (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers)
                    ? query_minimizers_per_query[qi]
                    : empty_minimizers_proto;

            // Short-circuit empty query or target inputs before pipeline assembly.
            if (query_len == 0 || target_len == 0) {
                return out;
            }

            if (params.downstream_active_wall != nullptr) {
                out.attach_downstream_active_wall_tracker(*params.downstream_active_wall);
            }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            ::bioseq::gpu::vulkan::runtime_pipeline_operation_token pending_chaining;
#endif
            auto pipeline = make_minimap_pipeline<p_aligner_parameters,
                                                  p_alignment_algorithm,
                                                  p_chaining_algorithm,
                                                  p_minimizer_algorithm,
                                                  p_entry_point,
                                                  p_alphabet,
                                                  p_minimizer,
                                                  p_index>(query_seq,
                                                           target_seq,
                                                           mins_for_this_query,
                                                           effective_index,
                                                           scoring_matrix,
                                                           alignment_kernels,
                                                           out,
                                                           execution_plan,
                                                           query_len,
                                                           target_len,
                                                           gpu_index_cache_ptr,
                                                           gpu_index_identity
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                                                           , nullptr, nullptr,
                                                           &pending_chaining
#endif
                                                           );

            try {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                attest_benchmark_e2e_pipeline_execution(execution_plan);
#endif
                if constexpr (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers) {
                    pipeline.on_batch(std::span<const p_minimizer>{mins_for_this_query.data(),
                                                                   mins_for_this_query.size()});
                } else {
                    // from_sequence: minimizer_stage's process ignores the input and
                    // sketches the cached query_seq_ pointer instead; pass an empty
                    // span of bytes as a placeholder.
                    pipeline.on_batch(std::span<const std::byte>{});
                }
                pipeline.on_completed();
            } catch (...) {
                out.detach_downstream_active_wall_tracker();
                throw;
            }
            // Prebuilt minimizers are operation-owned by their producer. That
            // producer publishes their exact identity only after this completed
            // collection returns; the selected template is not evidence of how
            // caller-supplied minimizers were made.
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if (!out.mapq_execution_available) {
                // A normally completed pipeline with no MAPQ-stage batch is an
                // exact non-execution observation, not evidence that the
                // selected flag ran an operation.
                out.record_mapq_execution(false);
            }
            out.completed_vulkan_chaining = std::move(pending_chaining);
#endif
            out.detach_downstream_active_wall_tracker();
            return out;
        };

        match_result_collection_vector out_per_query;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        out_per_query.set_logical_bounds(query_seqs.size(), query_seqs.size());
#endif
        const bool may_parallel_batch =
            !vulkan_accelerator_requested(params) &&
            bioseq::parallelism::has_parallel_tasks(
                bioseq::parallelism::global_pool::instance().get_mode()) &&
            query_seqs.size() >= bioseq::parallelism::task_parallel_minimums::k_query_count &&
            bioseq::storage::is_worker_shareable(target_seq) &&
            std::ranges::all_of(query_seqs, [](const auto p_query) {
                return bioseq::storage::is_worker_shareable(p_query);
            });

        if (may_parallel_batch) {
            out_per_query.resize(query_seqs.size());
            bioseq::parallelism::parallel_for_pipeline(
                0,
                query_seqs.size(),
                1,
                bioseq::parallelism::task_parallel_minimums::k_query_count,
                bioseq::parallelism::task_parallel_region::query_batch,
                [&out_per_query, &run_one_query](std::size_t lo, std::size_t hi) {
                    for (std::size_t qi = lo; qi < hi; ++qi) {
                        out_per_query[qi] = run_one_query(qi);
                    }
                });
        } else {
            out_per_query.reserve(query_seqs.size());
            for (std::size_t qi = 0; qi < query_seqs.size(); ++qi) {
                out_per_query.push_back(run_one_query(qi));
            }
        }

        // — release the wave's resident GPU index handles.
        if (owned_gpu_index_cache) {
            owned_gpu_index_cache->clear();
        }
        return out_per_query;
    }

    /**
     * @brief Single-query convenience wrapper around `pipeline_run_impl_batch`.
     */
    template <is_constexpr_aligner_params p_aligner_parameters,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm,
              pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index,
              class p_query_minimizers = minimizer_batch_view<p_minimizer>>
    [[nodiscard, gnu::always_inline]] inline match_result_collection pipeline_run_impl(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        p_query_minimizers query_minimizers,
        const p_index& target_index,
        statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false,
        transient_target_index_release_authorization transient_target_index_owner = {}) {
        if (!execution_plan) {
            throw std::invalid_argument("pipeline execution requires statement plan ownership");
        }
        const auto& params = execution_plan->frozen_parameters;
        std::optional<context::context> owned_context;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        std::optional<route_attempt_memory> campaign_attempt;
#endif
        std::optional<context::installed_context_scope> install;
        route_attempt_memory* attempt_memory = nullptr;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        campaign_attempt.emplace();
        attempt_memory = std::addressof(*campaign_attempt);
        install.emplace(campaign_attempt->fresh_context);
#else
        if (context::current_or_null() == nullptr) {
            owned_context.emplace();
            install.emplace(*owned_context);
        }
#endif
        static_assert(std::same_as<p_query_minimizers, minimizer_batch_view<p_minimizer>>);
        if (!query_minimizers.has_host_owner()) {
            throw std::invalid_argument("pipeline execution requires real query ownership");
        }
        // The locally owned cache must outlive PostSeed's raw cache pointer, while
        // PostSeed itself must outlive every Arena::chain that backs an admitted
        // anchor slab. Reverse destruction therefore requires this exact order.
        std::optional<bioseq::indexing::gpu_minimizer_index_cache> owned_gpu_index_cache;
        // Declared before the Arena::chain scope so any exact retained anchor
        // permit is released only after that arena has physically unwound.
        std::optional<post_seed_owned_lifetime> post_seed_owned_lifetime;
        auto outer_chain_scope = context::current().push<context::arena::chain>();

        const sequence_size_t target_len = target_seq.bp_size();
        const sequence_size_t query_len = query_seq.bp_size();
        const auto& scoring_matrix = thread_local_scoring_matrix<p_alphabet>(params);
        const auto& alignment_kernels = detail::resolve_alignment_kernels<p_alphabet>(params);

        if (gpu_index_cache == nullptr && params.vulkan_enabled &&
            params.vulkan_seed_hits_enabled &&
            !(gpu_index_identity.source_direct_sorted_dense &&
              gpu_index_identity.canonical_empty)) {
            bioseq::gpu::vulkan::runtime& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
            std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
                                      ? gpu_rt.features().device_local_bytes / 2u
                                      : (std::uint64_t{512} << 20);
            if (limit < (std::uint64_t{512} << 20))
                limit = std::uint64_t{512} << 20;
            if (limit > (std::uint64_t{4} << 30))
                limit = std::uint64_t{4} << 30;
            owned_gpu_index_cache.emplace(limit);
        }
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache_ptr =
            gpu_index_cache != nullptr
                ? gpu_index_cache
                : (owned_gpu_index_cache ? &*owned_gpu_index_cache : nullptr);

        ordered_sketch_query_gate_storage gpu_query_gate;
        // Fair no-index semantics require the full query-independent target index.
        const auto gpu_query_gate_identity = gpu_query_gate.canonical_identity();
        if (is_gpu_target_index_build_active(params)) {
            bind_gpu_query_gate_identity(
                gpu_index_cache_ptr, gpu_query_gate, gpu_query_gate_identity, gpu_index_identity);
        }

        const p_index* effective_index_ptr = &target_index;
        bool vulkan_target_index_built = false;
        bioseq::gpu::vulkan::gpu_index_build_stats vulkan_target_index_ownership{};
        if constexpr (std::same_as<std::remove_cvref_t<p_index>,
                                   bioseq::indexing::minimizer_hashmap_fragment_index>) {
            if (gpu_index_cache_ptr != nullptr && is_gpu_target_index_build_active(params) &&
                allow_gpu_target_index_build && target_index.table_empty() && target_len > 0) {
                auto& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
                bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
                auto built = bioseq::gpu::vulkan::build_gpu_index_on_device<p_minimizer>(
                    target_seq,
                    static_cast<std::size_t>(params.window_size),
                    static_cast<std::size_t>(params.kmer_size),
                    gpu_query_gate.vulkan_gate(),
                    &decline);
                if (!built) {
                    throw_vulkan_sketch_declined("Stage C GPU target-index build", decline);
                }
                auto key =
                    bioseq::indexing::make_gpu_index_cache_key(target_index, gpu_index_identity);
                auto adopted =
                    bioseq::indexing::adopt_gpu_built_index(gpu_rt, std::move(*built), key);
                if (!adopted) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                        "Stage C GPU target-index adoption rejected ({})",
                        bioseq::indexing::gpu_built_index_adoption_error_name(adopted.error())));
                }
                auto resident = std::move(*adopted);
                vulkan_target_index_ownership = resident->ownership;
                resident->target_len = target_len;
                auto inserted = gpu_index_cache_ptr->insert_built(key, resident);
                if (!inserted) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        fmt::format("Stage C GPU target-index cache insertion rejected ({})",
                                    static_cast<unsigned>(inserted.error())));
                }
                vulkan_target_index_built = true;
                bioseq_log<logging_level::error>(
                    "[gpu_target_index] GPU index injected: slot_mask={} pos={} target_len={}",
                    resident->slot_mask,
                    resident->position_count,
                    target_len);
            }
        }
        const p_index& effective_index = *effective_index_ptr;

        match_result_collection out;
        if (vulkan_target_index_built) {
            partial_vulkan_execution_stats stats{};
            stats.target_sketch_dispatches = 1;
            stats.target_index_build_dispatches = 1;
            stats.gpu_index_count_admissions =
                vulkan_target_index_ownership.count_submit_admissions;
            stats.gpu_index_main_peak_admissions =
                vulkan_target_index_ownership.execution_peak_admissions;
            stats.gpu_index_resident_transfers = vulkan_target_index_ownership.resident_transfers;
            stats.gpu_index_adoption_rereserve_attempts =
                vulkan_target_index_ownership.adoption_rereserve_attempts;
            stats.gpu_index_adoption_successes = vulkan_target_index_ownership.adoption_successes;
            stats.gpu_index_owned_resident_bytes = vulkan_target_index_ownership.resident_bytes;
            stats.gpu_index_gate_enabled = vulkan_target_index_ownership.query_gate_enabled;
            stats.gpu_index_gate_word_count = vulkan_target_index_ownership.query_gate_word_count;
            stats.gpu_index_gate_mask = vulkan_target_index_ownership.query_gate_mask;
            stats.gpu_index_gate_set_bits = gpu_query_gate_identity.set_bits;
            stats.gpu_index_gate_digest_lo = vulkan_target_index_ownership.query_gate_digest_lo;
            stats.gpu_index_gate_digest_hi = vulkan_target_index_ownership.query_gate_digest_hi;
            stats.gpu_index_gate_count_dispatches =
                vulkan_target_index_ownership.gate_count_dispatches;
            stats.gpu_index_gated_emit_dispatches =
                vulkan_target_index_ownership.gated_emit_dispatches;
            stats.gpu_index_ungated_emit_dispatches =
                vulkan_target_index_ownership.ungated_emit_dispatches;
            stats.gpu_index_exact_count_dispatches =
                vulkan_target_index_ownership.exact_count_dispatches;
            stats.gpu_index_exact_count_prefix_scan_dispatches =
                vulkan_target_index_ownership.exact_count_prefix_scan_dispatches;
            stats.gpu_index_exact_count_control_readbacks =
                vulkan_target_index_ownership.exact_count_control_readbacks;
            stats.gpu_index_deterministic_tile_emit_dispatches =
                vulkan_target_index_ownership.deterministic_tile_emit_dispatches;
            stats.gpu_index_atomic_append_dispatches =
                vulkan_target_index_ownership.atomic_append_dispatches;
            stats.gpu_index_emit_validation_control_readbacks =
                vulkan_target_index_ownership.emit_validation_control_readbacks;
            stats.gpu_index_counted_minimizer_count =
                vulkan_target_index_ownership.counted_minimizer_count;
            stats.gpu_index_verified_emit_count = vulkan_target_index_ownership.verified_emit_count;
            stats.gpu_index_count_emit_parity_verified =
                vulkan_target_index_ownership.count_emit_parity_verified;
            stats.gpu_index_target_minimizer_capacity_estimate =
                vulkan_target_index_ownership.target_minimizer_capacity_estimate;
            stats.gpu_index_capacity_estimate_underestimated =
                vulkan_target_index_ownership.capacity_estimate_underestimated;
            stats.gpu_index_arena_trimmed_before_permit_shrink =
                vulkan_target_index_ownership.arena_trimmed_before_permit_shrink;
            stats.gpu_index_retained_minimizer_count =
                vulkan_target_index_ownership.retained_minimizer_count;
            stats.gpu_index_distinct_minimizer_count =
                vulkan_target_index_ownership.distinct_minimizer_count;
            out.record_partial_vulkan_execution_stats(stats);
        }
#if !defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        out.results.reserve(detail::estimated_result_count(query_len, target_len, params));
#endif
        direct_resident_operation_stats* const direct_ledger =
            transient_target_index_owner.direct_ledger;
        const bool direct_empty_bypass = direct_ledger != nullptr &&
                                         gpu_index_identity.source_direct_sorted_dense &&
                                         gpu_index_identity.canonical_empty;
        const bool early_zero_consumer_bypass =
            direct_empty_bypass || query_len == 0 || target_len == 0;
        const std::size_t actual_seed_consumers = early_zero_consumer_bypass ? 0u : 1u;
        if (direct_ledger != nullptr) {
            direct_ledger->expected_seed_consumers = actual_seed_consumers;
            if (direct_empty_bypass) {
                direct_ledger->empty_bypass = true;
            } else if (early_zero_consumer_bypass) {
                direct_ledger->zero_query_bypasses = 1u;
            }
        }
        if (execution_plan->route == seed_chain_route::vulkan_seed_host_chain ||
            execution_plan->route == seed_chain_route::vulkan_seed_vulkan_chain ||
            owned_gpu_index_cache || transient_target_index_owner.index != nullptr ||
            transient_target_index_owner.operation_owned_gpu_cache != nullptr ||
            transient_target_index_owner.direct_ledger != nullptr) {
            // CPU-index release authority is supplied only by `minimap2_port`,
            // which owns that transient value. GPU-cache release authority is
            // independent: even a borrowed persisted CPU index may use and then
            // retire the cache constructed by this invocation. An external cache
            // is always borrowed and is never installed here. With such a cache,
            // the empty release authority still retains legacy anchor admission
            // through the chain arena lifetime; it does not own the caller's index.
            post_seed_owned_lifetime.emplace(
                actual_seed_consumers,
                owned_gpu_index_cache ? &*owned_gpu_index_cache
                                      : transient_target_index_owner.operation_owned_gpu_cache,
                transient_target_index_owner);
        }

        if (early_zero_consumer_bypass) {
            return out;
        }

        if constexpr (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers &&
                      std::same_as<std::remove_cvref_t<p_index>,
                                   bioseq::indexing::minimizer_hashmap_fragment_index>) {
            // The resident seed-to-coordinate kernel is only profitable once the
            // target index is genuinely resident/GPU-built. Using it on the current
            // vulkan_segment host-materialised sparse segment indexes repeats
            // submit/readback orchestration per segment and is slower than the
            // normal Vulkan seed+chain path ( 2026-06-25).
            if (is_gpu_coordinate_only_execution_active(params) && allow_gpu_target_index_build &&
                effective_index.table_empty()) {
                auto& runtime = bioseq::gpu::vulkan::runtime::instance();
                if (!bioseq::gpu::vulkan::hardware_device_available(runtime)) {
                    throw_vulkan_declined("Stage C resident seed-to-coordinate");
                }
                auto key =
                    bioseq::indexing::make_gpu_index_cache_key(effective_index, gpu_index_identity);
                auto resident_index =
                    gpu_index_cache_ptr == nullptr
                        ? nullptr
                        : gpu_index_cache_ptr->get_or_upload(
                              runtime, effective_index, key, params.vulkan_index_cache_enabled);
                if (!resident_index) {
                    throw_vulkan_declined("Stage C resident seed-to-coordinate index");
                }

                std::span<const p_minimizer> sketch_span{query_minimizers.data(),
                                                         query_minimizers.size()};
                std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>
                    device_span{};
                if constexpr (is_minimizer_batch_view_v<p_query_minimizers>) {
                    device_span = query_minimizers.device;
                }
                const bioseq::gpu::vulkan::buffer* device_words =
                    (device_span && device_span->words() != nullptr &&
                     device_span->count() == sketch_span.size())
                        ? device_span->words()
                        : nullptr;
                bioseq::gpu::vulkan::gpu_seed_inputs inputs{
                    .host_minimizer_bytes = device_words != nullptr ? std::span<const std::byte>{}
                                                                    : std::as_bytes(sketch_span),
                    .device_minimizer_words = device_words,
                    .slots = &resident_index->slots,
                    .positions = &resident_index->positions,
                    .target_len = resident_index->target_len,
                    .slot_mask = resident_index->slot_mask,
                    .position_count = resident_index->position_count,
                    .minimizer_count = static_cast<std::uint32_t>(sketch_span.size()),
                    .minimizer_word_kind = static_cast<std::uint32_t>(
                        bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_word_kind),
                    .minimizer_words32_per_record = bioseq::gpu::vulkan::minimizer_wire_traits<
                        p_minimizer>::k_words32_per_minimizer,
                    .hash_shift =
                        bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_hash_shift,
                    .pos_bits =
                        bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_position_bits,
                    .bucket_count = resident_index->bucket_count,
                    .target_position_offset = resident_index->target_position_offset,
                };

                auto cp = params.make_chaining_parameters();
                std::int32_t max_dist_x = cp.max_gap;
                std::int32_t max_dist_y = cp.max_gap;
                if (max_dist_x < cp.bandwidth)
                    max_dist_x = cp.bandwidth;
                if (max_dist_y < cp.bandwidth)
                    max_dist_y = cp.bandwidth;
                const auto chain_key =
                    bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
                const auto coord = runtime.run_seed_to_coordinate_roundtrip(
                    inputs,
                    query_len,
                    target_len,
                    static_cast<std::uint32_t>(params.kmer_size),
                    params.max_occ,
                    chain_key,
                    max_dist_x,
                    max_dist_y,
                    cp.bandwidth,
                    std::min<std::int32_t>(cp.max_chain_iter, 3000),
                    cp.gap_penalty,
                    cp.skip_penalty,
                    cp.max_chain_skip,
                    cp.min_chain_score,
                    cp.min_seed_count,
                    16u);
                if (coord.is_valid != 0u) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                    out.prepare_result_append(1u);
#endif
                    out.results.push_back(
                        gpu_coordinate_to_match_result(coord, query_len, target_len));
                }
                return out;
            }
        }

        if (direct_ledger != nullptr) {
            direct_ledger->pipeline_started = true;
        }
        auto per_query_chain_scope = context::current().push<context::arena::chain>();
        if (params.downstream_active_wall != nullptr) {
            out.attach_downstream_active_wall_tracker(*params.downstream_active_wall);
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        ::bioseq::gpu::vulkan::runtime_pipeline_operation_token pending_chaining;
#endif
        auto pipeline = make_minimap_pipeline<p_aligner_parameters,
                                              p_alignment_algorithm,
                                              p_chaining_algorithm,
                                              p_minimizer_algorithm,
                                              p_entry_point,
                                              p_alphabet,
                                              p_minimizer,
                                              p_index>(
            query_seq,
            target_seq,
            query_minimizers,
            effective_index,
            scoring_matrix,
            alignment_kernels,
            out,
            execution_plan,
            query_len,
            target_len,
            gpu_index_cache_ptr,
            gpu_index_identity,
            post_seed_owned_lifetime ? &*post_seed_owned_lifetime : nullptr,
            attempt_memory
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            , &pending_chaining
#endif
            );

        try {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            attest_benchmark_e2e_pipeline_execution(execution_plan);
#endif
            if constexpr (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers) {
                pipeline.on_batch(query_minimizers);
            } else {
                pipeline.on_batch(std::span<const std::byte>{});
            }
            pipeline.on_completed();
        } catch (...) {
            out.detach_downstream_active_wall_tracker();
            throw;
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (!out.mapq_execution_available) {
            out.record_mapq_execution(false);
        }
        out.completed_vulkan_chaining = std::move(pending_chaining);
#endif
        out.detach_downstream_active_wall_tracker();
        if (direct_ledger != nullptr) {
            // Seed completion alone is insufficient: this bit is set only after
            // both the batch and the terminal pipeline callback returned normally.
            direct_ledger->pipeline_completed = true;
        }
        return out;
    }

    } // namespace bioseq::simd::pipe

} // export
