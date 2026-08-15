/**
 * @file pipeline/pipe_parts/execution.cppm
 * @brief Concrete alignment stages and the shared pipeline runner.
 */
export module bioseq.pipeline.pipe:execution;

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

export {

    namespace bioseq::simd::pipe {

    // =============================================================================
    // Concrete stages
    // =============================================================================
    //
    // Each stage's `process*<p_aligner_parameters>(...)` body forwards to the existing kernel
    // function in the target-neutral pipeline or its selected kernel table
    // without renaming the kernel.  Wire-type between stages is `std::span<p_element>`
    // directly; arena ownership is provided by `arena_slab<p_element>` materialised
    // inside each stage before the downstream call.

    /**
     * @brief Minimizer extraction stage.
     *
     * Templated on `p_kernel` — the algorithm-specific call surface exposed by
     * `algorithm_traits<minimizer_algorithm::X>::kernel_type`.
     * The stage body has zero `if constexpr` over enum
     * values. Adding a new minimizer-extraction algorithm is a single trait
     * specialisation in the kernel's own module.
     *
     * The stage ignores its input batch and instead sketches the pre-captured
     * `query_seq_` pointer.
     */
    template <class p_minimizer, class p_kernel, sequence_alphabet p_alphabet>
    struct minimizer_stage
        : stage_base<minimizer_stage<p_minimizer, p_kernel, p_alphabet>, p_minimizer> {
        static constexpr std::string_view stage_name = "minimizer_stage";

        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_;
        int32_t runtime_w;
        int32_t runtime_k;
        // — runtime flags so the Vulkan kernel can gate on
        // `vulkan_enabled` without touching runtime when Vulkan is off. CPU kernels
        // ignore it.
        ::bioseq::simd::minimap2_parameters params;
        match_result_collection* stats_dst = nullptr;
        match_result_collection* host_timing_dst = nullptr;

        // The kernel writes directly into the per-query arena slab via
        // `sketch_collector_arena_adapter`, avoiding an intermediate copy. The
        // specialised path calls `call_constexpr<p_window_size, p_kmer_size, p_minimizer>(...)` so
        // the (p_window_size, p_kmer_size) pair propagates as integer immediates through the
        // window-loop bounds and unrolls.
        template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process(p_batch&& /*ignored*/, emit_token<p_sink>&& token) {
            const std::uint64_t host_stage_started =
                host_timing_dst == nullptr ? 0u : host_stage_now_ns();
            const std::size_t window_size = p_aligner_parameters::generic_fallback
                                                ? static_cast<std::size_t>(runtime_w)
                                                : p_aligner_parameters::window_len;
            const std::size_t kmer_size = p_aligner_parameters::generic_fallback
                                              ? static_cast<std::size_t>(runtime_k)
                                              : p_aligner_parameters::kmer_len;

            // Validate the zero-based position domain before any scalar, simd, or
            // Vulkan kernel can construct a masked minimizer_representation. A query shorter
            // than k is valid and naturally emits an empty minimizer batch.
            require_representable_minimizer_query_length<p_minimizer>(query_seq_.bp_size());

            // — device-minimizer path: only the Vulkan kernel exposes
            // `call_result`. When it returns a result, copy the host minimizers into
            // the arena slab and forward the resident device span as stage-local
            // metadata so seed_hit_stage can skip a second query-minimizer upload.
            if constexpr (requires { p_kernel::may_emit_device_minimizers; }) {
                if constexpr (p_kernel::may_emit_device_minimizers) {
                    if (auto gpu_result = p_kernel::template call_result<p_minimizer>(
                            params, query_seq_, window_size, kmer_size)) {
                        if (stats_dst == nullptr ||
                            !gpu_result->dispatch_token.is_internally_consistent()) {
                            bioseq_log<logging_level::error>(
                                "Vulkan minimizer stage lacks an operation token or stats sink");
                            std::abort();
                        }
                        partial_vulkan_execution_stats stats{};
                        stats.query_sketch_dispatches =
                            gpu_result->dispatch_token.completed_roundtrips;
                        stats_dst->record_partial_vulkan_execution_stats(stats);
                        auto device_slab = make_arena_slab<p_minimizer>();
                        device_slab.reserve(gpu_result->host_minimizers.size());
                        if (!gpu_result->host_minimizers.empty()) {
                            std::memcpy(device_slab.data(),
                                        gpu_result->host_minimizers.data(),
                                        gpu_result->host_minimizers.size() * sizeof(p_minimizer));
                        }
                        device_slab.resize(gpu_result->host_minimizers.size());
                        minimizer_batch_view<p_minimizer> batch{
                            .host = device_slab.as_const_span(),
                            .device =
                                std::optional<
                                    bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>{
                                    gpu_result->device_span()},
                        };
                        // The reservation owns the vector capacity, so destroy
                        // that capacity before releasing its permit. `batch.device`
                        // independently retains the resident device permit.
                        std::vector<p_minimizer>{}.swap(gpu_result->host_minimizers);
                        gpu_result->host_permit.reset();
                        return std::move(token).emit_minimizer_batch(batch);
                    }
                }
            }

            auto sketches_slab = make_arena_slab<p_minimizer>();
            sketches_slab.reserve(detail::estimated_sketch_count(
                query_seq_.bp_size(), static_cast<int32_t>(window_size)));

            sketch_collector_arena_adapter<p_minimizer> collector{&sketches_slab};

            if constexpr (p_kernel::emits_partial_vulkan_execution_stats) {
                const auto dispatch_token = [this, &collector, window_size, kmer_size] {
                    if constexpr (!p_aligner_parameters::generic_fallback) {
                        return p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                                 p_aligner_parameters::kmer_len,
                                                                 p_minimizer>(
                            params, collector, query_seq_);
                    } else {
                        return p_kernel::template call<p_minimizer>(
                            params, collector, query_seq_, window_size, kmer_size);
                    }
                }();
                if (stats_dst == nullptr || !dispatch_token.is_internally_consistent()) {
                    bioseq_log<logging_level::error>(
                        "Vulkan minimizer stage returned no accepted execution token");
                    std::abort();
                }
                partial_vulkan_execution_stats stats{};
                stats.query_sketch_dispatches = dispatch_token.completed_roundtrips;
                stats_dst->record_partial_vulkan_execution_stats(stats);
            } else if constexpr (p_kernel::emits_scalar_minimizer_execution_stats) {
                scalar_minimizer_execution_stats stats{};
                if constexpr (!p_aligner_parameters::generic_fallback) {
                    stats = p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                              p_aligner_parameters::kmer_len,
                                                              p_minimizer>(
                        params, collector, query_seq_);
                } else {
                    stats = p_kernel::template call<p_minimizer>(
                        params, collector, query_seq_, window_size, kmer_size);
                }
                if (stats_dst == nullptr || !stats.is_coherent()) {
                    bioseq_log<logging_level::error>(
                        "scalar minimizer stage returned rejected stats");
                    std::abort();
                }
                stats_dst->record_scalar_query_minimizer_execution_stats(stats);
            } else if constexpr (p_kernel::emits_exact_simd_execution_stats) {
                const auto stats = [this, &collector, window_size, kmer_size] {
                    if constexpr (!p_aligner_parameters::generic_fallback) {
                        return p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                                 p_aligner_parameters::kmer_len,
                                                                 p_minimizer>(
                            params, collector, query_seq_);
                    } else {
                        return p_kernel::template call<p_minimizer>(
                            params, collector, query_seq_, window_size, kmer_size);
                    }
                }();
                if (stats_dst == nullptr || !stats.is_internally_consistent(
                                                /*require_nonempty_vector_work=*/false)) {
                    bioseq_log<logging_level::error>(
                        "exact_simd minimizer stage returned rejected stats");
                    std::abort();
                }
                stats_dst->record_exact_simd_query_execution_stats(stats);
            } else if constexpr (p_kernel::emits_bounded_nthash_execution_stats) {
                const auto stats = [this, &collector, window_size, kmer_size] {
                    if constexpr (!p_aligner_parameters::generic_fallback) {
                        return p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                                 p_aligner_parameters::kmer_len,
                                                                 p_minimizer>(
                            params, collector, query_seq_);
                    } else {
                        return p_kernel::template call<p_minimizer>(
                            params, collector, query_seq_, window_size, kmer_size);
                    }
                }();
                if (stats_dst == nullptr || !stats.is_internally_consistent(
                                                /*require_nonempty_vector_work=*/false)) {
                    bioseq_log<logging_level::error>(
                        "bounded_nthash_simd minimizer stage returned rejected stats");
                    std::abort();
                }
                stats_dst->record_bounded_nthash_query_execution_stats(stats);
            } else if constexpr (!p_aligner_parameters::generic_fallback) {
                p_kernel::template call_constexpr<p_aligner_parameters::window_len,
                                                  p_aligner_parameters::kmer_len,
                                                  p_minimizer>(params, collector, query_seq_);
            } else {
                p_kernel::template call<p_minimizer>(
                    params, collector, query_seq_, window_size, kmer_size);
            }

            if (host_timing_dst != nullptr) {
                host_timing_dst->record_query_sketch_host_time(
                    elapsed_host_stage_ns(host_stage_started));
            }
            return std::move(token).emit_batch(
                std::span<const p_minimizer>{sketches_slab.data(), sketches_slab.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = std::string{p_kernel::algorithm_label()};
            p_kernel::populate_runtime_params({}, algo.parameters);
            algo.computed_properties = p_kernel::template static_properties<p_aligner_parameters>();
            algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
            desc.algorithm = std::move(algo);
            // Reserve target for the sketch arena slab (
            // "estimated_sketch_count").  `query_seq_` is a borrowed pointer; on
            // the describe path the proto is empty so bp_size() is zero — which
            // still yields the +64 safety margin from `estimated_sketch_count`.
            desc.estimated_batch_size = detail::estimated_sketch_count(
                query_seq_.bp_size(), runtime_w > 0 ? runtime_w : 10);
            return desc;
        }
    };

    /**
     * @brief Pass-through stage that decorates downstream with the target index.
     *
     * Converts the sink shape from `on_batch(span<p_minimizer>)` upstream to
     * `on_batch_with_index(span<p_minimizer>, p_index const&)` downstream.  Inlines
     * away at -O3 (zero runtime cost).
     */
    template <class p_index>
    struct index_stage
        : stage_base<index_stage<p_index>,
                     p_index /* pass-through; downstream sees the same sketch span */> {
        static constexpr std::string_view stage_name = "index_stage";

        const p_index* index_;
        bioseq::indexing::gpu_index_cache_identity identity_{};

        template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process(p_batch&& sketches, emit_token<p_sink>&& token) {
            // /  — whole in-memory targets keep pointer
            // identity (`{}`); segmented recompute/assembly passes a nonzero
            // identity so a reused stack index object cannot alias another segment
            // in the GPU resident cache.
            if constexpr (is_minimizer_batch_view_v<p_batch>) {
                return std::move(token).emit_minimizer_batch_with_index(
                    std::forward<p_batch>(sketches), *index_, identity_);
            } else {
                using element_type =
                    std::remove_const_t<typename std::remove_cvref_t<p_batch>::value_type>;
                return std::move(token).emit_batch_with_index(
                    std::span<const element_type>{sketches.data(), sketches.size()},
                    *index_,
                    identity_);
            }
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            desc.extra_description = "target index lookup hand-off";
            return desc;
        }
    };

    enum class source_direct_host_authorization : std::uint8_t {
        none,
        canonical_empty,
        reflected_process_footprint,
        retained_exact_permit,
    };

    /// One operation-scoped ledger for the selected source-direct resident route.
    /// It emits exactly one planning receipt after sort/count and exactly one
    /// terminal receipt after cache/fence/arena cleanup (or unwind).  Checksums are
    /// allocation-integrity observations only. Biological parity remains a
    /// test/benchmark property and is deliberately absent from these receipts.
    struct direct_resident_operation_stats {
        std::uint64_t operation_identity = 0u;
        std::uint64_t minimizer_count = 0u;
        std::uint64_t distinct_count = 0u;
        std::uint64_t logical_control_bytes = 0u;
        std::uint64_t logical_position_bytes = 0u;
        std::uint64_t logical_resident_bytes = 0u;
        std::uint64_t control_requirement_bytes = 0u;
        std::uint64_t position_requirement_bytes = 0u;
        std::uint64_t control_allowance_bytes = 0u;
        std::uint64_t position_allowance_bytes = 0u;
        std::uint64_t conservative_allowance_bytes = 0u;
        std::uint64_t conservative_request_bytes = 0u;
        std::uint64_t reserved_before_bytes = 0u;
        std::uint64_t allocatable_before_bytes = 0u;
        std::uint64_t margin_bytes = 0u;
        std::uint64_t shortfall_bytes = 0u;
        std::uint64_t max_storage_buffer_range = 0u;
        std::uint64_t source_capacity_bytes = 0u;
        std::uint64_t source_reflected_bytes = 0u;
        std::uint64_t source_retained_permit_bytes = 0u;
        std::uint64_t source_releases = 0u;
        std::uint64_t source_released_bytes = 0u;
        std::uint64_t cache_preinsertions = 0u;
        std::uint64_t cache_required_lookups = 0u;
        std::uint64_t cache_required_hits = 0u;
        std::uint64_t mapped_prefix_passes = 0u;
        std::uint64_t mapped_prefix_lookups = 0u;
        std::uint64_t mapped_prefix_comparisons = 0u;
        std::uint64_t expected_seed_consumers = 0u;
        std::uint64_t completed_seed_consumers = 0u;
        std::uint64_t slab_capacity_before = 0u;
        std::uint64_t slab_requested_bytes = 0u;
        std::uint64_t slab_actual_bytes = 0u;
        std::uint64_t slab_retained_permit_bytes = 0u;
        std::uint64_t slab_permit_releases = 0u;
        std::uint64_t seed_reserved_bytes = 0u;
        std::uint64_t seed_planned_vulkan_bytes = 0u;
        std::uint64_t seed_actual_vulkan_bytes = 0u;
        std::uint64_t seed_roundtrips = 0u;
        std::uint64_t zero_query_bypasses = 0u;
        std::uint64_t zero_anchor_bypasses = 0u;
        std::uint64_t seed_fence_completions = 0u;
        std::uint64_t seed_dispatches = 0u;
        std::uint64_t cache_cleanup_releases = 0u;
        std::uint64_t resident_cleanup_bytes = 0u;
        std::uint64_t cpu_index_releases = 0u;
        std::uint64_t cpu_position_bytes_released = 0u;
        std::uint64_t planning_receipts = 0u;
        std::uint64_t terminal_receipts = 0u;
        bioseq::indexing::gpu_source_direct_materialization_stats materialization{};
        source_direct_host_authorization source_authority = source_direct_host_authorization::none;
        bool gpu_target_index_route_checked = false;
        bool gpu_target_index_route_compatible = false;
        bool source_authority_checked = false;
        bool has_valid_source_authorization = false;
        bool runtime_checked = false;
        bool runtime_available = false;
        bool hardware_checked = false;
        bool hardware_available = false;
        bool sorted_shape_checked = false;
        bool is_sorted_shape_accepted = false;
        bool identity_checked = false;
        bool is_identity_accepted = false;
        bool u32_shape_checked = false;
        bool is_u32_shape_accepted = false;
        bool storage_ranges_checked = false;
        bool is_control_range_accepted = false;
        bool is_position_range_accepted = false;
        bool mapping_plan_checked = false;
        bool is_mapping_plan_accepted = false;
        bool admission_snapshot_taken = false;
        bool canonical_empty = false;
        bool is_planning_accepted = false;
        bool route_committed = false;
        bool materialization_started = false;
        bool materialization_completed = false;
        bool resident_published = false;
        bool source_release_completed = false;
        bool source_released_after_publication = false;
        bool empty_bypass = false;
        bool cache_lookup_started = false;
        bool mapped_prefix_started = false;
        bool slab_admission_started = false;
        bool seed_dispatch_started = false;
        bool pipeline_started = false;
        bool pipeline_completed = false;
        bool planning_log_failed = false;
        bool terminal_success = false;

        [[nodiscard]] constexpr std::string_view route_name() const noexcept {
            return canonical_empty ? "empty_no_resident_zero_anchor"
                                   : "full_sorted_dense_direct_mapped";
        }

        [[nodiscard]] static constexpr std::string_view check_status(bool checked,
                                                                     bool accepted) noexcept {
            if (!checked)
                return "not_reached";
            return accepted ? "accepted" : "rejected";
        }

        [[nodiscard]] static constexpr std::string_view
        stage_status(bool checked, bool accepted, bool bypass) noexcept {
            return bypass ? "bypass" : check_status(checked, accepted);
        }

        [[nodiscard]] static constexpr std::string_view
        source_authority_name(source_direct_host_authorization authority) noexcept {
            switch (authority) {
            case source_direct_host_authorization::canonical_empty:
                return "canonical_empty";
            case source_direct_host_authorization::reflected_process_footprint:
                return "reflected_process_footprint";
            case source_direct_host_authorization::retained_exact_permit:
                return "retained_exact_permit";
            case source_direct_host_authorization::none:
                return "none";
            }
            return "unknown";
        }

        [[nodiscard]] static constexpr std::string_view resident_authority_name(
            bioseq::indexing::gpu_resident_accounting_authority authority) noexcept {
            switch (authority) {
            case bioseq::indexing::gpu_resident_accounting_authority::reflected_process_footprint:
                return "reflected_process_footprint";
            case bioseq::indexing::gpu_resident_accounting_authority::retained_exact_permit:
                return "retained_exact_permit";
            case bioseq::indexing::gpu_resident_accounting_authority::none:
                return "none";
            }
            return "unknown";
        }

        void log_planning(std::string_view decision, std::string_view reason) noexcept {
            if (planning_receipts != 0u) {
                planning_log_failed = true;
                return;
            }
            try {
                bioseq_log<logging_level::info>(
                    "[gpu_source_direct_operation_planning] operation={} "
                    "route={} "
                    "source_authority={} source_authority_status={} "
                    "gpu_target_index_route_status={} runtime_status={} device_status={} "
                    "sorted_shape_status={} abi_identity_status={} "
                    "u32_shape_status={} control_range_status={} "
                    "position_range_status={} mapping_plan_status={} "
                    "N={} D={} control_bytes={} position_bytes={} "
                    "logical_bytes={} control_requirement_bytes={} "
                    "position_requirement_bytes={} control_allowance_bytes={} "
                    "position_allowance_bytes={} allowance_bytes={} "
                    "conservative_request_bytes={} reserved_before_bytes={} "
                    "allocatable_before_bytes={} margin_bytes={} "
                    "shortfall_bytes={} max_storage_buffer_range={} "
                    "canonical_empty_bypass={} gate_enabled=0 "
                    "decision={} reason={}",
                    operation_identity,
                    route_name(),
                    source_authority_name(source_authority),
                    check_status(source_authority_checked, has_valid_source_authorization),
                    check_status(gpu_target_index_route_checked, gpu_target_index_route_compatible),
                    check_status(runtime_checked, runtime_available),
                    check_status(hardware_checked, hardware_available),
                    check_status(sorted_shape_checked, is_sorted_shape_accepted),
                    check_status(identity_checked, is_identity_accepted),
                    check_status(u32_shape_checked, is_u32_shape_accepted),
                    check_status(storage_ranges_checked, is_control_range_accepted),
                    check_status(storage_ranges_checked, is_position_range_accepted),
                    check_status(mapping_plan_checked, is_mapping_plan_accepted),
                    minimizer_count,
                    distinct_count,
                    logical_control_bytes,
                    logical_position_bytes,
                    logical_resident_bytes,
                    control_requirement_bytes,
                    position_requirement_bytes,
                    control_allowance_bytes,
                    position_allowance_bytes,
                    conservative_allowance_bytes,
                    conservative_request_bytes,
                    reserved_before_bytes,
                    allocatable_before_bytes,
                    margin_bytes,
                    shortfall_bytes,
                    max_storage_buffer_range,
                    canonical_empty ? 1 : 0,
                    decision,
                    reason);
                planning_receipts = 1u;
            } catch (...) {
                planning_log_failed = true;
            }
        }

        [[nodiscard]] bool terminal_success_invariants() const noexcept {
            const bool explicit_pipeline_bypass =
                expected_seed_consumers == 0u && completed_seed_consumers == 0u &&
                (canonical_empty ? empty_bypass && zero_query_bypasses == 0u
                                 : zero_query_bypasses == 1u);
            const bool pipeline_exact = explicit_pipeline_bypass
                                            ? !pipeline_started && !pipeline_completed
                                            : pipeline_started && pipeline_completed;
            if (planning_receipts != 1u || planning_log_failed || !is_planning_accepted ||
                !route_committed || !source_release_completed || !pipeline_exact ||
                completed_seed_consumers != expected_seed_consumers || cpu_index_releases != 0u ||
                cpu_position_bytes_released != 0u) {
                return false;
            }
            if (canonical_empty) {
                return empty_bypass && !materialization_started && !materialization_completed &&
                       !resident_published && minimizer_count == 0u && distinct_count == 0u &&
                       logical_control_bytes == 0u && logical_position_bytes == 0u &&
                       logical_resident_bytes == 0u && control_requirement_bytes == 0u &&
                       position_requirement_bytes == 0u && control_allowance_bytes == 0u &&
                       position_allowance_bytes == 0u && conservative_allowance_bytes == 0u &&
                       conservative_request_bytes == 0u && source_capacity_bytes == 0u &&
                       source_reflected_bytes == 0u && source_retained_permit_bytes == 0u &&
                       source_releases == 0u && source_released_bytes == 0u &&
                       !source_released_after_publication && cache_preinsertions == 0u &&
                       cache_required_lookups == 0u && cache_required_hits == 0u &&
                       mapped_prefix_passes == 0u && mapped_prefix_lookups == 0u &&
                       mapped_prefix_comparisons == 0u && slab_capacity_before == 0u &&
                       slab_requested_bytes == 0u && slab_actual_bytes == 0u &&
                       slab_retained_permit_bytes == 0u && slab_permit_releases == 0u &&
                       seed_reserved_bytes == 0u && seed_planned_vulkan_bytes == 0u &&
                       seed_actual_vulkan_bytes == 0u && seed_roundtrips == 0u &&
                       zero_anchor_bypasses == 0u && seed_fence_completions == 0u &&
                       seed_dispatches == 0u && !pipeline_started && cache_cleanup_releases == 0u &&
                       resident_cleanup_bytes == 0u;
            }
            if (!materialization_started || !materialization_completed || !resident_published ||
                empty_bypass) {
                return false;
            }
            const std::uint64_t allocation_sum =
                bioseq::common::saturating_add(materialization.actual_slot_allocation_bytes,
                                               materialization.actual_position_allocation_bytes);
            const bool exact_materialization =
                allocation_sum == materialization.actual_allocation_bytes &&
                allocation_sum != (std::numeric_limits<std::uint64_t>::max)() &&
                materialization.logical_slot_bytes == logical_control_bytes &&
                materialization.logical_position_bytes == logical_position_bytes &&
                materialization.logical_resident_bytes == logical_resident_bytes &&
                materialization.control_allocation_requirement_bytes == control_requirement_bytes &&
                materialization.position_allocation_requirement_bytes ==
                    position_requirement_bytes &&
                materialization.control_allocation_allowance_bytes == control_allowance_bytes &&
                materialization.position_allocation_allowance_bytes == position_allowance_bytes &&
                materialization.conservative_allowance_bytes == conservative_allowance_bytes &&
                materialization.conservative_request_bytes == conservative_request_bytes &&
                materialization.actual_slot_allocation_bytes == control_requirement_bytes &&
                materialization.actual_position_allocation_bytes == position_requirement_bytes &&
                materialization.actual_allocation_bytes == conservative_request_bytes &&
                materialization.fully_touched_bytes == materialization.actual_allocation_bytes &&
                materialization.permit_reconciliations == 1u;
            const bool reflected = materialization.authority ==
                                       bioseq::indexing::gpu_resident_accounting_authority::
                                           reflected_process_footprint &&
                                   materialization.reflected_footprint_bytes >=
                                       materialization.actual_allocation_bytes &&
                                   materialization.retained_permit_bytes == 0u;
            const bool retained =
                materialization.authority ==
                    bioseq::indexing::gpu_resident_accounting_authority::retained_exact_permit &&
                materialization.retained_permit_bytes == materialization.actual_allocation_bytes &&
                materialization.reflected_footprint_bytes == 0u;
            const bool slab_exact =
                slab_capacity_before == 0u && slab_requested_bytes == slab_actual_bytes &&
                slab_actual_bytes == slab_retained_permit_bytes &&
                (slab_actual_bytes == 0u ? slab_permit_releases == 0u : slab_permit_releases == 1u);
            const bool zero_query = zero_query_bypasses == 1u;
            const bool seed_memory_exact =
                zero_query ? seed_reserved_bytes == 0u && seed_planned_vulkan_bytes == 0u &&
                                 seed_actual_vulkan_bytes == 0u
                : slab_actual_bytes == 0u
                    ? seed_reserved_bytes == 0u && seed_planned_vulkan_bytes == 0u &&
                          seed_actual_vulkan_bytes == 0u
                    : seed_reserved_bytes != 0u && seed_planned_vulkan_bytes != 0u &&
                          seed_planned_vulkan_bytes == seed_actual_vulkan_bytes;
            const bool zero_anchor =
                !zero_query && seed_roundtrips != 0u && zero_anchor_bypasses == seed_roundtrips;
            const bool lookup_exact =
                zero_query ? cache_required_hits == cache_required_lookups &&
                                 cache_required_lookups == completed_seed_consumers &&
                                 mapped_prefix_passes == 0u
                           : cache_required_lookups >= 1u &&
                                 cache_required_hits == cache_required_lookups &&
                                 cache_required_lookups == completed_seed_consumers &&
                                 mapped_prefix_passes == 2u * cache_required_lookups;
            const bool seed_completion_exact =
                seed_roundtrips == cache_required_lookups &&
                (zero_query ? slab_actual_bytes == 0u && slab_requested_bytes == 0u &&
                                  zero_anchor_bypasses == 0u && seed_dispatches == 0u &&
                                  seed_fence_completions == 0u
                 : slab_actual_bytes == 0u
                     ? zero_anchor_bypasses == seed_roundtrips && seed_dispatches == 0u &&
                           seed_fence_completions == 0u
                     : zero_anchor_bypasses == 0u && seed_dispatches == seed_roundtrips &&
                           seed_fence_completions == seed_roundtrips);
            return exact_materialization && source_releases == 1u &&
                   source_released_bytes == source_capacity_bytes &&
                   source_released_after_publication && cache_preinsertions == 1u && lookup_exact &&
                   (reflected != retained) && slab_exact && seed_memory_exact &&
                   seed_completion_exact && cache_cleanup_releases == 1u &&
                   resident_cleanup_bytes == materialization.actual_allocation_bytes &&
                   materialization.source_checksum != 0u &&
                   materialization.source_checksum == materialization.mapped_checksum &&
                   materialization.complete_source_traversals == 1u &&
                   materialization.complete_mapped_traversals == 1u;
        }

        void log_terminal(std::string_view status, std::string_view reason) noexcept {
            if (terminal_receipts != 0u)
                return;
            const bool invariant_success = terminal_success_invariants();
            const bool accepted = status == "success" && invariant_success;
            const bool explicit_pipeline_bypass =
                expected_seed_consumers == 0u && completed_seed_consumers == 0u &&
                (canonical_empty ? empty_bypass && zero_query_bypasses == 0u
                                 : zero_query_bypasses == 1u);
            const bool zero_anchor_bypass =
                seed_roundtrips != 0u && zero_anchor_bypasses == seed_roundtrips;
            const bool mapped_prefix_bypass = explicit_pipeline_bypass;
            const bool cleanup_checked = source_release_completed || cache_cleanup_releases != 0u ||
                                         resident_cleanup_bytes != 0u;
            const bool is_cleanup_accepted =
                source_release_completed &&
                (!resident_published ||
                 (cache_cleanup_releases == 1u &&
                  resident_cleanup_bytes == materialization.actual_allocation_bytes));
            try {
                bioseq_log<logging_level::info>(
                    "[gpu_source_direct_operation_terminal] operation={} status={} "
                    "reason={} route={} N={} D={} actual_control_allocation_bytes={} "
                    "actual_position_allocation_bytes={} actual_allocation_bytes={} "
                    "permit_reconciliations={} resident_authority={} "
                    "reflected_bytes={} retained_permit_bytes={} fully_touched_bytes={} "
                    "source_checksum={} mapped_checksum={} source_traversals={} "
                    "mapped_traversals={} source_releases={} source_released_bytes={} "
                    "source_released_after_publication={} cache_preinsertions={} "
                    "required_lookups={} required_hits={} required_misses={} "
                    "mapped_prefix_passes={} "
                    "mapped_prefix_lookups={} mapped_prefix_comparisons={} "
                    "slab_before={} slab_requested_bytes={} slab_actual_bytes={} "
                    "slab_retained_permit_bytes={} slab_permit_releases={} "
                    "seed_reserved_bytes={} seed_planned_vulkan_bytes={} "
                    "seed_actual_vulkan_bytes={} "
                    "expected_consumers={} completed_consumers={} seed_roundtrips={} "
                    "seed_dispatches={} seed_fence_completions={} "
                    "zero_query_bypasses={} zero_anchor_bypasses={} empty_bypass={} "
                    "materialization_status={} publication_status={} "
                    "cache_lookup_status={} mapped_prefix_status={} "
                    "slab_admission_status={} seed_dispatch_status={} "
                    "seed_fence_status={} cleanup_status={} pipeline_status={} "
                    "pipeline_completed={} terminal_invariants={} "
                    "cache_cleanup_releases={} resident_cleanup_bytes={}",
                    operation_identity,
                    accepted ? "success" : "failure",
                    reason,
                    route_name(),
                    minimizer_count,
                    distinct_count,
                    materialization.actual_slot_allocation_bytes,
                    materialization.actual_position_allocation_bytes,
                    materialization.actual_allocation_bytes,
                    materialization.permit_reconciliations,
                    resident_authority_name(materialization.authority),
                    materialization.reflected_footprint_bytes,
                    materialization.retained_permit_bytes,
                    materialization.fully_touched_bytes,
                    materialization.source_checksum,
                    materialization.mapped_checksum,
                    materialization.complete_source_traversals,
                    materialization.complete_mapped_traversals,
                    source_releases,
                    source_released_bytes,
                    source_released_after_publication ? 1 : 0,
                    cache_preinsertions,
                    cache_required_lookups,
                    cache_required_hits,
                    cache_required_lookups >= cache_required_hits
                        ? cache_required_lookups - cache_required_hits
                        : 0u,
                    mapped_prefix_passes,
                    mapped_prefix_lookups,
                    mapped_prefix_comparisons,
                    slab_capacity_before,
                    slab_requested_bytes,
                    slab_actual_bytes,
                    slab_retained_permit_bytes,
                    slab_permit_releases,
                    seed_reserved_bytes,
                    seed_planned_vulkan_bytes,
                    seed_actual_vulkan_bytes,
                    expected_seed_consumers,
                    completed_seed_consumers,
                    seed_roundtrips,
                    seed_dispatches,
                    seed_fence_completions,
                    zero_query_bypasses,
                    zero_anchor_bypasses,
                    empty_bypass ? 1 : 0,
                    stage_status(
                        materialization_started, materialization_completed, canonical_empty),
                    stage_status(materialization_completed, resident_published, canonical_empty),
                    stage_status(cache_lookup_started,
                                 cache_required_lookups != 0u &&
                                     cache_required_hits == cache_required_lookups,
                                 explicit_pipeline_bypass),
                    stage_status(mapped_prefix_started,
                                 mapped_prefix_passes == 2u * cache_required_lookups,
                                 mapped_prefix_bypass),
                    stage_status(slab_admission_started,
                                 slab_requested_bytes == slab_actual_bytes &&
                                     slab_actual_bytes == slab_retained_permit_bytes,
                                 explicit_pipeline_bypass || zero_anchor_bypass),
                    stage_status(seed_dispatch_started,
                                 seed_dispatches != 0u && seed_dispatches == seed_roundtrips,
                                 explicit_pipeline_bypass || zero_anchor_bypass),
                    stage_status(seed_dispatch_started,
                                 seed_fence_completions == seed_roundtrips,
                                 explicit_pipeline_bypass || zero_anchor_bypass),
                    check_status(cleanup_checked, is_cleanup_accepted),
                    stage_status(pipeline_started, pipeline_completed, explicit_pipeline_bypass),
                    pipeline_completed ? 1 : 0,
                    invariant_success ? 1 : 0,
                    cache_cleanup_releases,
                    resident_cleanup_bytes);
                terminal_receipts = 1u;
                terminal_success = accepted;
            } catch (...) {
                terminal_success = false;
            }
        }
    };

    /// Last-resort receipt guard for failures before the wrapper's catch handler
    /// can finish its unwind. Cleanup remains owned by the wrapper/PostSeed. This
    /// guard only prevents a selected operation from disappearing without its two
    /// operation-level receipts.
    struct direct_resident_receipt_guard {
        direct_resident_operation_stats* ledger = nullptr;

        ~direct_resident_receipt_guard() noexcept {
            if (ledger == nullptr)
                return;
            if (ledger->planning_receipts == 0u) {
                ledger->log_planning("reject", "unwind_before_planning_complete");
            }
            if (ledger->terminal_receipts == 0u) {
                ledger->log_terminal("failure", "unhandled_operation_unwind");
            }
        }
    };

    /// Release authority for one operation-owned transient CPU target index.
    /// Ordinary transient indexes carry only `index`. The fixed Vulkan handoff
    /// also carries the exact permit that accounts for storage not already
    /// represented by its one-shot process-footprint authority.
    struct transient_target_index_release_authorization {
        bioseq::indexing::minimizer_hashmap_fragment_index* index = nullptr;
        bioseq::parallelism::memory_permit* permit = nullptr;
        std::uint64_t expected_permit_bytes = 0u;
        bool require_exact_permit_bytes = false;
        bioseq::indexing::gpu_minimizer_index_cache* operation_owned_gpu_cache = nullptr;
        direct_resident_operation_stats* direct_ledger = nullptr;

        [[nodiscard]] bool is_valid() const noexcept {
            return index != nullptr && (!require_exact_permit_bytes || permit != nullptr) &&
                   (permit == nullptr ||
                    static_cast<bool>(*permit) &&
                        (!require_exact_permit_bytes || permit->bytes() == expected_permit_bytes));
        }
    };

    /// One operation-owned lifetime boundary between seed lookup and chaining.
    /// Borrowed persisted CPU indexes may use it only to retire an invocation-owned
    /// GPU cache. External caches never receive release authority. Every successful
    /// seed consumer signals exactly once after dropping its local resident handle;
    /// the last consumer deallocates any transient CPU lookup index and clears any
    /// internally-owned GPU cache before downstream chaining begins.
    struct post_seed_owned_lifetime {
        struct release_receipt {
            std::uint64_t gpu_cache_evictions = 0;
            std::uint64_t gpu_resident_bytes_released = 0;
            std::uint64_t cpu_index_releases = 0;
            std::uint64_t cpu_position_bytes_released = 0;
            std::uint64_t cpu_index_permit_releases = 0;
            std::uint64_t cpu_index_permit_bytes_released = 0;
            bool cpu_index_storage_released_before_permit = false;
        };

        std::atomic<std::size_t> remaining_seed_users{0};
        bioseq::indexing::gpu_minimizer_index_cache* owned_gpu_cache = nullptr;
        transient_target_index_release_authorization owned_cpu_index{};
        bioseq::parallelism::memory_permit retained_seed_arena_permit{};

        post_seed_owned_lifetime(std::size_t seed_users,
                                 bioseq::indexing::gpu_minimizer_index_cache* gpu_cache,
                                 transient_target_index_release_authorization cpu_index) noexcept
            : remaining_seed_users(seed_users), owned_gpu_cache(gpu_cache),
              owned_cpu_index(cpu_index) {}

        ~post_seed_owned_lifetime() noexcept {
            if (owned_cpu_index.direct_ledger == nullptr)
                return;
            auto& ledger = *owned_cpu_index.direct_ledger;
            if (owned_gpu_cache != nullptr) {
                const std::uint64_t resident_bytes = owned_gpu_cache->resident_bytes();
                owned_gpu_cache->clear();
                owned_gpu_cache = nullptr;
                if (resident_bytes != 0u) {
                    ledger.cache_cleanup_releases = 1u;
                    ledger.resident_cleanup_bytes = resident_bytes;
                }
            }
            if (retained_seed_arena_permit) {
                ledger.slab_retained_permit_bytes = retained_seed_arena_permit.bytes();
                retained_seed_arena_permit.release();
                ledger.slab_permit_releases = 1u;
            }
            const bool users_completed = remaining_seed_users.load(std::memory_order_relaxed) == 0u;
            const bool accepted = users_completed && ledger.terminal_success_invariants();
            ledger.log_terminal(accepted ? "success" : "failure",
                                accepted ? ledger.expected_seed_consumers == 0u
                                               ? "completed_explicit_zero_consumer_bypass"
                                               : "completed_after_last_seed_fence_and_arena_unwind"
                                : users_completed ? "route_specific_terminal_invariant_failed"
                                                  : "seed_user_not_completed");
        }

        post_seed_owned_lifetime(const post_seed_owned_lifetime&) = delete;
        post_seed_owned_lifetime& operator=(const post_seed_owned_lifetime&) = delete;

        [[nodiscard]] release_receipt complete_seed_user() {
            std::size_t previous = remaining_seed_users.load(std::memory_order_acquire);
            while (previous != 0u &&
                   !remaining_seed_users.compare_exchange_weak(previous,
                                                               previous - 1u,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_acquire)) {
            }
            if (previous == 0u) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "post-seed lifetime user underflow"};
            }
            if (owned_cpu_index.direct_ledger != nullptr) {
                auto& ledger = *owned_cpu_index.direct_ledger;
                ledger.completed_seed_consumers =
                    bioseq::common::saturating_add(ledger.completed_seed_consumers, 1u);
            }
            if (previous != 1u)
                return {};

            release_receipt stats{};
            if (owned_gpu_cache != nullptr) {
                auto* const cache_to_release = owned_gpu_cache;
                owned_gpu_cache = nullptr;
                stats.gpu_resident_bytes_released = cache_to_release->resident_bytes();
                cache_to_release->clear();
                stats.gpu_cache_evictions = stats.gpu_resident_bytes_released != 0u ? 1u : 0u;
                if (owned_cpu_index.direct_ledger != nullptr) {
                    auto& ledger = *owned_cpu_index.direct_ledger;
                    ledger.cache_cleanup_releases = stats.gpu_cache_evictions;
                    ledger.resident_cleanup_bytes = stats.gpu_resident_bytes_released;
                }
            }
            if (owned_cpu_index.index != nullptr) {
                if (!owned_cpu_index.is_valid()) {
                    bioseq_log<logging_level::error>(
                        "post-seed lifetime has invalid CPU index release authority");
                    std::abort();
                }
                const bool had_resident_storage = !owned_cpu_index.index->table_empty() ||
                                                  owned_cpu_index.index->positions.capacity() != 0u;
                stats.cpu_position_bytes_released = bioseq::common::saturating_mul(
                    owned_cpu_index.index->positions.capacity(), sizeof(std::uint64_t));
                owned_cpu_index.index->release_storage();
                stats.cpu_index_releases = had_resident_storage ? 1u : 0u;
                if (owned_cpu_index.permit != nullptr) {
                    if (!owned_cpu_index.index->table_empty() ||
                        owned_cpu_index.index->positions.capacity() != 0u) {
                        bioseq_log<logging_level::error>(
                            "post-seed CPU index permit release preceded storage");
                        std::abort();
                    }
                    stats.cpu_index_storage_released_before_permit = true;
                    stats.cpu_index_permit_bytes_released = owned_cpu_index.permit->bytes();
                    stats.cpu_index_permit_releases =
                        static_cast<bool>(*owned_cpu_index.permit) ? 1u : 0u;
                    owned_cpu_index.permit->release();
                }
            }
            return stats;
        }
    };

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

        ::bioseq::simd::minimap2_parameters params;
        sequence_size_t query_len;
        // — batch/segment-wave-scoped GPU index cache. The outer
        // wave creates one cache, passes `&cache` here, and clears it at wave end.
        // Null on single-shot paths (then the GPU seed path uploads uncached).
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr;
        match_result_collection* stats_dst = nullptr;
        post_seed_owned_lifetime* post_seed_lifetime = nullptr;
        match_result_collection* host_timing_dst = nullptr;

        // — host/device accessors for both the std::span CPU shape and
        // the minimizer_batch_view device shape.
        template <class p_minimizer>
        [[nodiscard, gnu::always_inline]] static std::span<const p_minimizer>
        sketch_span_of(const std::span<const p_minimizer>& s) noexcept {
            return s;
        }
        template <class p_minimizer>
        [[nodiscard, gnu::always_inline]] static std::span<const p_minimizer>
        sketch_span_of(const minimizer_batch_view<p_minimizer>& b) noexcept {
            return b.host;
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
            const int32_t kmer_size = p_aligner_parameters::generic_fallback
                                          ? params.kmer_size
                                          : static_cast<int32_t>(p_aligner_parameters::kmer_len);

            using p_minimizer =
                std::remove_const_t<typename std::remove_cvref_t<p_batch>::value_type>;

            auto sketch_span = sketch_span_of<p_minimizer>(sketches);
            auto device_span = device_span_of<p_minimizer>(sketches);
            direct_resident_operation_stats* const direct_ledger =
                post_seed_lifetime == nullptr ? nullptr
                                              : post_seed_lifetime->owned_cpu_index.direct_ledger;

            // — run the selected GPU seed-hit lookup before the CPU
            // path. If it declines, the selected Vulkan pipeline fails loudly.
            // Only the concrete minimizer_hashmap_fragment_index carries the GPU view.
            if (params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
                bool seed_gpu_declined = false;
                bioseq::seeding::vulkan_seed_lookup_decline seed_decline{
                    .query_length = static_cast<std::uint64_t>(query_len),
                    .query_minimizer_count = static_cast<std::uint64_t>(sketch_span.size()),
                    .kmer_size = kmer_size,
                    .max_occ = params.max_occ,
                    .fused_frontend = params.vulkan_fused_frontend_enabled,
                };
                if constexpr (std::same_as<std::remove_cvref_t<p_index>,
                                           bioseq::indexing::minimizer_hashmap_fragment_index>) {
                    // Strict Vulkan selection owns its anchor allocation. A
                    // prepared fragment view has no resident GPU index and reaches
                    // the fail-closed decline below without allocating CPU output.
                    auto anchors_slab = make_arena_slab<anchor>();
                    if (gpu_index_identity.source_direct_sorted_dense &&
                        gpu_index_identity.canonical_empty) {
                        if (!bioseq::indexing::gpu_source_direct_identity_well_formed(
                                gpu_index_identity) ||
                            !target_index_ref.table_empty() ||
                            !target_index_ref.positions.empty() ||
                            gpu_index_identity.table_size != 0u ||
                            gpu_index_identity.position_count != 0u || gpu_index_cache != nullptr ||
                            direct_ledger == nullptr || params.vulkan_fused_frontend_enabled) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "canonical empty source-direct route is invalid"};
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
                                params.vulkan_fused_frontend_enabled || direct_ledger == nullptr) {
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
                                                               params.vulkan_index_cache_enabled,
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
                                    target_index_ref,
                                    resident_index,
                                    params.vulkan_enabled,
                                    query_len,
                                    params.max_occ,
                                    kmer_size,
                                    anchors_slab,
                                    params.vulkan_fused_frontend_enabled,
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
                            if (post_seed_lifetime == nullptr ||
                                post_seed_lifetime->retained_seed_arena_permit) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                    "source-direct seed permit owner is invalid"};
                            }
                            post_seed_lifetime->retained_seed_arena_permit =
                                std::move(seed_completion.retained_anchor_permit);
                            if (seed_completion.retained_anchor_bytes !=
                                post_seed_lifetime->retained_seed_arena_permit.bytes()) {
                                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                    "source-direct seed permit byte authority is invalid"};
                            }
                        }
                        if (resident_index && seed_completion) {
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
                static_assert(std::same_as<p_minimizer, minimap_sketch_data>,
                              "prepared fragment anchors require canonical query minimizers");
                const std::span<const anchor> ordered_anchors =
                    target_index_ref.ordered_fragment_seed_anchors(
                        std::span<const minimap_sketch_data>{sketch_span.data(),
                                                             sketch_span.size()},
                        query_len,
                        params.max_occ,
                        kmer_size);
                record_cpu_seed_completion();
                // The owning PreparedFragmentAnchorBatch outlives this synchronous
                // pipeline call. Emit its already-canonical span directly: no
                // arena_slab allocation, anchor copy, or global fragment sort.
                return std::move(token).emit_batch(ordered_anchors);
            } else {
                // Ordinary per-hash lookup retains the arena-producing kernel.
                auto anchors_slab = make_arena_slab<anchor>();
                anchors_slab.reserve(detail::estimated_anchor_count(
                    sketch_span.size(),
                    static_cast<std::size_t>(query_len),
                    static_cast<std::size_t>(target_index_ref.target_length()),
                    kmer_size));
                collect_seed_hits<p_minimizer>(sketch_span,
                                               target_index_ref,
                                               query_len,
                                               params.max_occ,
                                               kmer_size,
                                               anchors_slab);
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
            algo.parameters.push_back({"max_occ", std::to_string(params.max_occ)});
            algo.parameters.push_back({"query_len", std::to_string(query_len)});
            desc.algorithm = std::move(algo);
            // anchor reserve target — used by seed_hit_stage's slab (
            // "estimated_anchor_count").  The actual sketch count is unknown at
            // describe time so the reported figure uses query_len/window=10 as
            // the canonical median; the runtime reserve picks a tighter value
            // when the real sketches.size() is available.
            const std::size_t approx_sketch_count = static_cast<std::size_t>(query_len) / 10u + 64u;
            desc.estimated_batch_size =
                detail::estimated_anchor_count(approx_sketch_count,
                                               static_cast<std::size_t>(query_len),
                                               /*target_bp=*/static_cast<std::size_t>(query_len),
                                               params.kmer_size);
            return desc;
        }
    };

    /**
     * @brief Anchor chaining stage.
     *
     * Templated on `p_kernel` — the algorithm-specific call surface exposed by
     * `algorithm_traits<chaining_algorithm::X>::kernel_type`.
     * The stage body has zero `if constexpr` over enum
     * values — adding a new chaining algorithm is a single trait specialisation
     * in the kernel's own module.
     *
     * The chosen kernel writes its `(chains, reordered_anchors)` output directly
     * into caller-provided arena_slab parameters. No intermediate vector is
     * materialised inside the stage.
     */
    template <class p_kernel> struct chain_stage : stage_base<chain_stage<p_kernel>, chain> {
        static constexpr std::string_view stage_name = "chain_stage";

        chaining_parameters cp;
        sequence_size_t query_len = 0;
        match_result_collection* stats_dst = nullptr;
        match_result_collection* host_timing_dst = nullptr;
        const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels = nullptr;

        template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process(p_batch&& anchors_in, emit_token<p_sink>&& token) {
            const std::uint64_t host_stage_started =
                host_timing_dst == nullptr ? 0u : host_stage_now_ns();
            chaining_parameters active_cp = cp;
            active_cp.query_len = query_len;
            const std::span<const anchor> anchors_span{anchors_in.data(), anchors_in.size()};

            auto chains_slab = make_arena_slab<chain>();
            auto reordered_slab = make_arena_slab<anchor>();
            if (active_cp.coordinate_only) {
                chains_slab.reserve(1);
                // Reserve the full upstream bound before the Vulkan backend opens
                // its nested DP arena scope. A >65,536-anchor coordinate trace
                // must never grow this output slab inside that inner scope, whose
                // allocations are reclaimed before the stage emits downstream.
                reordered_slab.reserve(anchors_in.size());
            } else {
                chains_slab.reserve(
                    detail::estimated_chain_count(anchors_in.size(), active_cp.min_seed_count));
                reordered_slab.reserve(anchors_in.size());
            }

            if constexpr (p_kernel::emits_scalar_chaining_execution_stats) {
                if (stats_dst == nullptr) {
                    bioseq_log<logging_level::error>(
                        "scalar chaining stage has no execution-stats sink");
                    std::abort();
                }
                scalar_pipeline_execution_stats stats{};
                p_kernel::call(anchors_span, active_cp, chains_slab, reordered_slab, &stats);
                /* This receipt is written after the kernel returns, so a zero-hit
                 * negative is attested without claiming scalar score/backtrack
                 * work that the kernel correctly skipped. */
                stats.record_scalar_chain_stage_completion(
                    static_cast<std::uint64_t>(anchors_span.size()));
                stats_dst->record_scalar_pipeline_execution_stats(stats);
            } else if constexpr (p_kernel::emits_simd_chaining_execution_stats) {
                if (stats_dst == nullptr) {
                    bioseq_log<logging_level::error>(
                        "simd chaining stage has no execution-stats sink");
                    std::abort();
                }
                ::bioseq::chaining::simd_chaining_execution_stats stats{};
                p_kernel::call(
                    highway_kernels, anchors_span, active_cp, chains_slab, reordered_slab, &stats);
                if (!stats.is_internally_consistent(active_cp.simd_policy,
                                                    /*require_nonempty_vector_work=*/false)) {
                    const auto status = stats.has_consistent_policy_stats(active_cp.simd_policy)
                                            ? simd_chaining_status::invalid_parameters
                                            : simd_chaining_status::invalid_policy;
                    throw simd_chaining_runtime_error(
                        status,
                        "selected policy does not match operation-originated "
                        "simd chaining stats");
                }
                stats_dst->record_simd_chaining_execution_stats(stats);
            } else if constexpr (p_kernel::emits_partial_vulkan_execution_stats) {
                if (stats_dst == nullptr) {
                    bioseq_log<logging_level::error>(
                        "Vulkan chaining stage has no execution-stats sink");
                    std::abort();
                }
                if (p_kernel::call(anchors_span, active_cp, chains_slab, reordered_slab)) {
                    partial_vulkan_execution_stats stats{};
                    stats.chain_score_dispatches = 1;
                    stats_dst->record_partial_vulkan_execution_stats(stats);
                }
            } else {
                p_kernel::call(anchors_span, active_cp, chains_slab, reordered_slab);
            }

            if (host_timing_dst != nullptr) {
                host_timing_dst->record_chain_score_host_time(
                    elapsed_host_stage_ns(host_stage_started));
            }

            return std::move(token).emit_batch_pair(
                std::span<chain>{chains_slab.data(), chains_slab.size()},
                std::span<anchor>{reordered_slab.data(), reordered_slab.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = std::string{p_kernel::algorithm_label()};
            algo.parameters.push_back({"min_chain_score", std::to_string(cp.min_chain_score)});
            algo.parameters.push_back({"min_seed_count", std::to_string(cp.min_seed_count)});
            algo.parameters.push_back({"max_chain_skip", std::to_string(cp.max_chain_skip)});
            algo.parameters.push_back({"max_chain_iter", std::to_string(cp.max_chain_iter)});
            algo.parameters.push_back({"bandwidth", std::to_string(cp.bandwidth)});
            algo.parameters.push_back({"max_gap", std::to_string(cp.max_gap)});
            // Let the selected kernel append its own runtime params (e.g. the
            // Vulkan backend's stage / vulkan_enabled markers) so the
            // EXPLAIN metadata is not dead code. No-op for the scalar/simd/range_minimum_query
            // kernels.
            p_kernel::populate_runtime_params(cp, algo.parameters);
            algo.computed_properties = p_kernel::template static_properties<p_aligner_parameters>();
            algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
            desc.algorithm = std::move(algo);
            // chain stage's two arena slabs are sized from the runtime
            // anchor count (which is unknown at describe time), so we leave
            // `estimated_batch_size` empty rather than fabricate a per-query
            // figure that doesn't reflect the actual reserve.  The
            // `[reserved: ...]` line in EXPLAIN is therefore omitted for
            // chain_stage; the runtime `chains_slab.reserve(...)` call still
            // sizes from `estimated_chain_count(anchors_in.size(), ...)`.
            return desc;
        }
    };

    /**
     * @brief Chain-to-region conversion stage.
     *
     * Wraps the `generate_regions` kernel, which writes its
     * `alignment_region`-per-chain output directly into a caller-provided
     * arena_slab via placement-new. `alignment_region` owns a `std::vector<cigar_operation>`,
     * so it is not trivially-destructible. The slab's destructor walks
     * `[0, len_)` and calls `~alignment_region()` on each live element before the
     * arena reclaims the storage.
     */
    struct region_stage : stage_base<region_stage, alignment_region> {
        static constexpr std::string_view stage_name = "region_stage";

        sequence_size_t query_len = 0;
        match_result_collection* host_timing_dst = nullptr;

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_chain_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink>
        process_pair(p_chain_batch&& chains, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
            if (host_timing_dst != nullptr) {
                host_timing_dst->begin_region_host_stage(host_stage_now_ns());
            }
            // `alignment_region` owns a `std::vector<cigar_operation>` so the slab uses the
            // placement-new pattern; no intermediate `std::vector` is needed.
            auto regions_slab = make_arena_slab<alignment_region>();
            generate_regions(std::span<chain>{chains.data(), chains.size()},
                             std::span<const anchor>{anchors.data(), anchors.size()},
                             query_len,
                             regions_slab);

            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions_slab.data(), regions_slab.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = "Region generation";
            algo.parameters.push_back({"query_len", std::to_string(query_len)});
            desc.algorithm = std::move(algo);
            // One region per chain (exact); the actual chain count is only
            // known at runtime so the describe-time figure is left empty.  The
            // runtime reserve in process_pair sizes the slab from chains.size().
            return desc;
        }
    };

    /**
     * @brief In-place primary/secondary classification stage.
     *
     * Mutates the regions span via `classify_primary_secondary` (which takes
     * std::vector<alignment_region>& and modifies parent / score fields).  The
     * vector-wrapper here copies in and out. The touched region count is bounded
     * by best_n + 16 so the cost is negligible.
     */
    struct primary_secondary_stage : stage_base<primary_secondary_stage, alignment_region> {
        static constexpr std::string_view stage_name = "primary_secondary_stage";

        float mask_level;
        int32_t mask_len;
        int32_t sub_diff_value;

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            // The kernel mutates in place through a span without a vector round-trip.
            classify_primary_secondary(std::span<alignment_region>{regions.data(), regions.size()},
                                       mask_level,
                                       mask_len,
                                       sub_diff_value);
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = "Classify primary/secondary";
            algo.parameters.push_back(
                {"mask_level", std::to_string(static_cast<double>(mask_level))});
            algo.parameters.push_back({"mask_len", std::to_string(mask_len)});
            algo.parameters.push_back({"sub_diff", std::to_string(sub_diff_value)});
            desc.algorithm = std::move(algo);
            return desc;
        }
    };

    /**
     * @brief In-place secondary-region filtering stage.
     */
    struct secondary_filter_stage : stage_base<secondary_filter_stage, alignment_region> {
        static constexpr std::string_view stage_name = "secondary_filter_stage";

        float pri_ratio;
        int32_t min_diff_value;
        int32_t best_n;

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            // The kernel filters in place via a span; the returned effective size
            // gives the downstream stage a shorter prefix.
            const std::size_t new_size = filter_secondary_regions(
                std::span<alignment_region>{regions.data(), regions.size()},
                pri_ratio,
                min_diff_value,
                best_n);
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), new_size},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            stage_algorithm_description algo;
            algo.name = "Filter secondary regions";
            algo.parameters.push_back(
                {"pri_ratio", std::to_string(static_cast<double>(pri_ratio))});
            algo.parameters.push_back({"min_diff", std::to_string(min_diff_value)});
            algo.parameters.push_back({"best_n", std::to_string(best_n)});
            desc.algorithm = std::move(algo);
            return desc;
        }
    };

    /**
     * @brief Conditional divergence-estimation stage.
     */
    template <class p_minimizer, class p_query_minimizers = std::vector<p_minimizer>>
    struct divergence_stage
        : stage_base<divergence_stage<p_minimizer, p_query_minimizers>, alignment_region> {
        static constexpr std::string_view stage_name = "divergence_stage";

        const p_query_minimizers* query_minimizers_;
        sequence_size_t query_len;
        sequence_size_t target_len;
        int32_t runtime_kmer_size;
        bool enabled;

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            if (enabled) {
                const int32_t kmer_size =
                    p_aligner_parameters::generic_fallback
                        ? runtime_kmer_size
                        : static_cast<int32_t>(p_aligner_parameters::kmer_len);
                estimate_divergence_from_seeds<p_minimizer>(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const p_minimizer>{query_minimizers_->data(),
                                                 query_minimizers_->size()},
                    std::span<const anchor>{anchors.data(), anchors.size()},
                    query_len,
                    kmer_size,
                    target_len);
            }
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (enabled) {
                stage_algorithm_description algo;
                algo.name = "Estimate divergence from seeds";
                desc.algorithm = std::move(algo);
            }
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

        ::bioseq::simd::minimap2_parameters params;
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
            if (!params.prefilter_enabled || !params.compute_edit_distance || regions.size() == 0) {
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
                    params,
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
                            .max_edits = params.prefilter_max_edits,
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
            const backend_type selected = params.prefilter_backend == backend_type::automatic
                                              ? backend_type::simd
                                              : params.prefilter_backend;
            const algorithm_type algorithm = resolve_prefilter_algorithm(selected, jobs);
            switch (selected) {
            case backend_type::scalar:
                results = sneaky_snake_scalar_batch(input);
                break;
            case backend_type::simd:
            case backend_type::automatic: {
                if (params.highway_kernels == nullptr ||
                    !params.highway_kernels->has_kernel(::bioseq::simd_dispatch::highway_kernel::
                                                            prefilter_sneaky_snake_simd_batch) ||
                    params.highway_kernels->common.sneaky_snake_simd_batch == nullptr) {
                    throw std::logic_error{"selected Highway table has no SneakySnake kernel"};
                }
                results = params.highway_kernels->common.sneaky_snake_simd_batch(input, {});
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
                    lower_bounds[region_index] > params.prefilter_max_edits) {
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
            if (params.prefilter_enabled) {
                stage_algorithm_description algorithm;
                algorithm.name = "Rank edit-distance prefilter";
                algorithm.parameters.push_back(
                    {"backend", std::string{backend_name(params.prefilter_backend)}});
                algorithm.parameters.push_back({"algorithm",
                                                params.prefilter_backend == backend_type::vulkan
                                                    ? "batch_resolved_gatekeeper_or_sneaky_snake"
                                                    : "sneaky_snake"});
                algorithm.parameters.push_back(
                    {"max_edits", std::to_string(params.prefilter_max_edits)});
                algorithm.parameters.push_back(
                    {"submaze_width",
                     std::to_string(prefilter::k_sneaky_snake_default_submaze_width)});
                algorithm.computed_properties.push_back(
                    {"domain", "mandatory seed-to-seed global fills"});
                algorithm.computed_properties.push_back(
                    {"survivor_check", "exact post-alignment edit distance"});
                algorithm.computed_properties.push_back(
                    {"gatekeeper_auto_domain", "length in {32,64}, E in {2,4}"});
                if (params.prefilter_backend == backend_type::simd ||
                    params.prefilter_backend == backend_type::automatic) {
                    algorithm.highway_kernels = ::bioseq::simd_dispatch::highway_kernel_mask_for(
                        ::bioseq::simd_dispatch::highway_kernel::prefilter_sneaky_snake_simd_batch);
                }
                desc.algorithm = std::move(algorithm);
            }
            return desc;
        }
    };

    /**
     * Authoritative exact edit-distance predicate gate.
     *
     * This runs after traceback has populated `alignment_region::edit_distance` and
     * before the final primary/secondary and MAPQ pass.  Consequently a candidate
     * that does not satisfy the SQL predicate cannot influence the classification
     * or mapping-quality metadata of a candidate that does.  The earlier
     * rank-prefilter stage remains only a conservative work-avoidance gate.
     */
    struct exact_edit_filter_stage : stage_base<exact_edit_filter_stage, alignment_region> {
        static constexpr std::string_view stage_name = "exact_edit_filter_stage";

        ::bioseq::simd::minimap2_parameters params;

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            if (!params.exact_edit_filter_enabled) {
                return std::move(token).emit_batch_pair(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const anchor>{anchors.data(), anchors.size()});
            }
            if (!params.compute_edit_distance) {
                throw std::logic_error("exact edit filter requires exact edit-distance accounting");
            }

            const std::int64_t exclusive_upper_bound =
                params.exact_edit_filter_exclusive_upper_bound;
            std::size_t output_size = 0;
            for (std::size_t index = 0; index < regions.size(); ++index) {
                alignment_region& region = regions.data()[index];
                if (!region.has_alignment) {
                    throw std::runtime_error(
                        "exact edit filter received a region without alignment traceback");
                }
                const bool keep =
                    exclusive_upper_bound > 0 &&
                    region.edit_distance < static_cast<std::uint64_t>(exclusive_upper_bound);
                if (!keep)
                    continue;

                if (output_size != index) {
                    regions.data()[output_size] = std::move(region);
                }
                alignment_region& survivor = regions.data()[output_size];
                survivor.region_id = static_cast<std::int32_t>(output_size);
                survivor.parent = -1;
                survivor.secondary_score = 0;
                survivor.secondary_count = 0;
                survivor.secondary_dp_max = 0;
                survivor.mapq = 0;
                ++output_size;
            }

            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), output_size},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (params.exact_edit_filter_enabled) {
                stage_algorithm_description algorithm;
                algorithm.name = "Exact edit-distance predicate";
                algorithm.parameters.push_back(
                    {"exclusive_upper_bound",
                     std::to_string(params.exact_edit_filter_exclusive_upper_bound)});
                algorithm.computed_properties.push_back(
                    {"classification_domain", "exact predicate survivors"});
                desc.algorithm = std::move(algorithm);
            }
            return desc;
        }
    };

    /**
     * @brief Conditional base-level alignment stage.
     *
     * Algorithm dispatch occurs through `p_kernel = kernel_t<p_alignment_algorithm>`;
     * runtime parameters are populated by `p_kernel::populate_runtime_params(...)`.
     */
    template <class p_kernel, sequence_alphabet p_alphabet>
    struct align_stage : stage_base<align_stage<p_kernel, p_alphabet>, alignment_region> {
        static constexpr std::string_view stage_name = "align_stage";

        ::bioseq::simd::minimap2_parameters params;
        const scoring_matrix<p_alphabet>* sm;
        const ::bioseq::simd::alignment_kernels<p_alphabet>* alignment_kernels_;
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_;
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq_;
        bool enabled;
        match_result_collection* stats_dst_;
        match_result_collection* host_timing_dst_;

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& anchors,
                                           emit_token<p_sink>&& token) {
            if (host_timing_dst_ != nullptr) {
                host_timing_dst_->transition_region_to_alignment_result_host_stage(
                    host_stage_now_ns(), enabled);
            }
            if (enabled) {
                if constexpr (p_kernel::emits_scalar_ksw2_execution_stats) {
                    if (stats_dst_ == nullptr) {
                        bioseq_log<logging_level::error>(
                            "scalar ksw2 stage has no execution-stats sink");
                        std::abort();
                    }
                    scalar_pipeline_execution_stats call_stats{};
                    p_kernel::template call<p_alphabet>(
                        std::span<alignment_region>{regions.data(), regions.size()},
                        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()},
                        query_seq_,
                        target_seq_,
                        *sm,
                        *alignment_kernels_,
                        params,
                        /*forbidden_block_stats=*/nullptr,
                        &call_stats);
                    stats_dst_->record_scalar_pipeline_execution_stats(call_stats);
                } else if constexpr (p_kernel::emits_ksw2_batched_execution_stats) {
                    if (stats_dst_ == nullptr) {
                        bioseq_log<logging_level::error>(
                            "ksw2 batched stage has no execution-stats sink");
                        std::abort();
                    }
                    ksw2_batched_execution_stats call_stats{};
                    p_kernel::template call<p_alphabet>(
                        std::span<alignment_region>{regions.data(), regions.size()},
                        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()},
                        query_seq_,
                        target_seq_,
                        *sm,
                        *alignment_kernels_,
                        params,
                        /*forbidden_block_stats=*/nullptr,
                        /*scalar_execution_stats=*/nullptr,
                        &call_stats);
                    if (!call_stats.is_coherent(
                            /*require_nonempty_work=*/false)) {
                        bioseq_log<logging_level::error>(
                            "ksw2 batched stage produced rejected execution stats");
                        std::abort();
                    }
                    stats_dst_->record_ksw2_batched_execution_stats(call_stats);
                } else if constexpr (p_kernel::emits_block_execution_stats) {
                    if (stats_dst_ == nullptr) {
                        bioseq_log<logging_level::error>(
                            "Block Aligner stage has no execution-stats sink");
                        std::abort();
                    }
                    block_aligner_execution_stats call_stats{};
                    p_kernel::template call<p_alphabet>(
                        std::span<alignment_region>{regions.data(), regions.size()},
                        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()},
                        query_seq_,
                        target_seq_,
                        *sm,
                        *alignment_kernels_,
                        params,
                        &call_stats,
                        /*scalar_execution_stats=*/nullptr);
                    if (!call_stats.is_internally_consistent()) {
                        bioseq_log<logging_level::error>(
                            "Block Aligner stage produced a rejected execution stats");
                        std::abort();
                    }
                    stats_dst_->record_block_aligner_execution_stats(call_stats);
                } else {
                    p_kernel::template call<p_alphabet>(
                        std::span<alignment_region>{regions.data(), regions.size()},
                        std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()},
                        query_seq_,
                        target_seq_,
                        *sm,
                        *alignment_kernels_,
                        params,
                        /*forbidden_block_stats=*/nullptr,
                        /*scalar_execution_stats=*/nullptr);
                }
            }
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const ::bioseq::chaining::anchor>{anchors.data(), anchors.size()});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (enabled) {
                stage_algorithm_description algo;
                algo.name = std::string{p_kernel::algorithm_label()};
                p_kernel::populate_runtime_params(params, algo.parameters);
                algo.computed_properties =
                    p_kernel::template static_properties<p_aligner_parameters>();
                algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
                desc.algorithm = std::move(algo);
            }
            return desc;
        }
    };

    /**
     * @brief Conditional mapping-quality stage.
     *
     * Accepts a `(regions, anchors)` pair upstream but only uses the regions
     * span. It emits the regions span downstream as a single batch.
     */
    struct mapq_stage : stage_base<mapq_stage, alignment_region> {
        static constexpr std::string_view stage_name = "mapq_stage";

        ::bioseq::simd::minimap2_parameters params;
        bool enabled;

        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process(p_region_batch&& regions, emit_token<p_sink>&& token) {
            if (enabled) {
                compute_mapping_quality(
                    std::span<alignment_region>{regions.data(), regions.size()}, params, 0);
            }
            return std::move(token).emit_batch(
                std::span<alignment_region>{regions.data(), regions.size()});
        }

        //: pair → single delegation.  The outer body has no
        // surviving `emit_token<p_sink>` after the inner call moves it, so a
        // second emit is structurally blocked.
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_region_batch,
                  class p_anchor_batch,
                  class p_sink>
        [[gnu::always_inline]]
        emitted_token<p_sink> process_pair(p_region_batch&& regions,
                                           p_anchor_batch&& /*unused*/,
                                           emit_token<p_sink>&& token) {
            return process<p_aligner_parameters>(std::forward<p_region_batch>(regions),
                                                 std::move(token));
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] stage_description describe() const {
            stage_description desc;
            desc.stage_name = std::string{stage_name};
            if (enabled) {
                stage_algorithm_description algo;
                algo.name = "Compute mapping quality (mm_set_mapq2)";
                desc.algorithm = std::move(algo);
            }
            return desc;
        }
    };

    /**
     * @brief Terminal sink: push converted MatchResults into the collection.
     *
     * Not a `bare_stage` — appears as the tail of `make_pipeline<p_aligner_parameters>(...)`.
     */
    struct sink_stage {
        match_result_collection* dst_;
        sequence_size_t query_len = 0;
        sequence_size_t target_len = 0;
        match_result_collection* host_timing_dst_ = nullptr;

        [[gnu::always_inline]] inline void on_batch(std::span<alignment_region> regions) {
            dst_->results.reserve(dst_->results.size() + regions.size());
            // `region_slot` is move-consumed into the converted match_result; the
            // slot is left in a valid-but-moved-from state and reclaimed by
            // whichever owner (arena slab or std::vector) holds the span.
            for (auto& region_slot : regions) {
                dst_->results.push_back(
                    region_to_match_result(std::move(region_slot), query_len, target_len));
            }
            if (host_timing_dst_ != nullptr &&
                host_timing_dst_->is_alignment_result_host_stage_active) {
                host_timing_dst_->finish_alignment_result_host_stage(host_stage_now_ns());
            }
        }

        [[gnu::always_inline]] inline void on_completed() noexcept {
            if (host_timing_dst_ != nullptr &&
                host_timing_dst_->is_alignment_result_host_stage_active) {
                host_timing_dst_->finish_alignment_result_host_stage(host_stage_now_ns());
            }
        }
    };

    // =============================================================================
    // Out-of-line pipeline-description walker
    // =============================================================================

    template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
    [[nodiscard]] pipeline_description
    stage<p_aligner_parameters, p_self, p_sink>::describe() const {
        // Every concrete stage description uses the same parameter specialization
        // as the runtime path.
        pipeline_description d;
        d.stages.push_back(self_.template describe<p_aligner_parameters>());
        if constexpr (requires { sink_.describe(); }) {
            pipeline_description rest = sink_.describe();
            d.stages.insert(d.stages.end(),
                            std::make_move_iterator(rest.stages.begin()),
                            std::make_move_iterator(rest.stages.end()));
        }
        return d;
    }

    /**
     * @brief Injects the p_aligner_parameters computed properties as a leading entry in the
     * pipeline description so the EXPLAIN renderer sees w/k/p_bucket_width beside the first
     * stage.
     */
    template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
    [[nodiscard]] pipeline_description
    describe_with_params(const stage<p_aligner_parameters, p_self, p_sink>& pipeline) {
        auto d = pipeline.describe();
        if (!d.stages.empty()) {
            auto& first = d.stages.front();
            if (!first.algorithm.has_value()) {
                first.algorithm.emplace();
                first.algorithm->name = "pipeline configuration";
            }
            first.algorithm->computed_properties.push_back(
                {"cep.window_len", std::to_string(p_aligner_parameters::window_len)});
            first.algorithm->computed_properties.push_back(
                {"cep.kmer_len", std::to_string(p_aligner_parameters::kmer_len)});
            first.algorithm->computed_properties.push_back(
                {"cep.sequence_len_log2", std::to_string(p_aligner_parameters::sequence_len_log2)});
            first.algorithm->computed_properties.push_back(
                {"cep.specialised",
                 p_aligner_parameters::generic_fallback ? "fallback" : "specialised"});
        }
        return d;
    }

    // =============================================================================
    // make_minimap_pipeline — single source of truth for the pipeline shape
    // =============================================================================

    [[nodiscard]] inline bool
    vulkan_components_requested(const ::bioseq::simd::minimap2_parameters& params) noexcept;

    /**
     * @brief Build the full minimap pipeline value for one (p_aligner_parameters,
     * p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm, p_entry_point).
     *
     * All template parameters are fixed by the caller (the dispatcher cascade).
     * The function is the single place both the run path and the describe path
     * consult to learn what stages are is_available — adding or removing a stage edits
     * only this function.
     */
    template <is_constexpr_aligner_params p_aligner_parameters,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm,
              pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index,
              class p_query_minimizers = std::vector<p_minimizer>>
    [[nodiscard, gnu::always_inline]] inline auto
    make_minimap_pipeline(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                          bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                          const p_query_minimizers& query_minimizers,
                          const p_index& target_index,
                          const scoring_matrix<p_alphabet>& scoring_matrix,
                          const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
                          match_result_collection& output_collection,
                          const ::bioseq::simd::minimap2_parameters& params,
                          sequence_size_t query_len,
                          sequence_size_t target_len,
                          bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
                          bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
                          post_seed_owned_lifetime* post_seed_owned_lifetime = nullptr) {
        match_result_collection* host_timing_dst =
            vulkan_components_requested(params) ? nullptr : &output_collection;
        match_result_collection* chaining_stats_dst = nullptr;
        if constexpr (p_chaining_algorithm == ::bioseq::simd::chaining_algorithm::scalar_minimap2 ||
                      p_chaining_algorithm == ::bioseq::simd::chaining_algorithm::simd_minimap2 ||
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
        auto tail = make_pipeline<p_aligner_parameters>(
            index_stage<p_index>{{}, &target_index, gpu_index_identity},
            seed_hit_stage{{},
                           params,
                           query_len,
                           gpu_index_cache,
                           &output_collection,
                           post_seed_owned_lifetime,
                           host_timing_dst},
            chain_stage<kernel_t<p_chaining_algorithm>>{{},
                                                        params.make_chaining_parameters(),
                                                        query_len,
                                                        chaining_stats_dst,
                                                        host_timing_dst,
                                                        params.highway_kernels},
            region_stage{{}, query_len, host_timing_dst},
            primary_secondary_stage{{}, params.mask_level, params.mask_len, sub_diff(params)},
            secondary_filter_stage{{}, params.pri_ratio, min_diff(params), params.best_n},
            divergence_stage<p_minimizer, p_query_minimizers>{{},
                                                              &query_minimizers,
                                                              query_len,
                                                              target_len,
                                                              params.kmer_size,
                                                              params.compute_divergence},
            prefilter_stage<p_alphabet>{{}, params, query_seq, target_seq, &output_collection},
            align_stage<kernel_t<p_alignment_algorithm>, p_alphabet>{
                {},
                params,
                &scoring_matrix,
                &alignment_kernels,
                query_seq,
                target_seq,
                params.do_cigar || params.compute_edit_distance,
                alignment_stats_dst,
                host_timing_dst},
            exact_edit_filter_stage{{}, params},
            primary_secondary_stage{{}, params.mask_level, params.mask_len, sub_diff(params)},
            secondary_filter_stage{{}, params.pri_ratio, min_diff(params), params.best_n},
            mapq_stage{{}, params, params.compute_mapq},
            sink_stage{&output_collection, query_len, target_len, host_timing_dst});

        if constexpr (p_entry_point == pipeline_entry_point::from_sequence) {
            minimizer_stage<p_minimizer, kernel_t<p_minimizer_algorithm>, p_alphabet> head{
                {},
                query_seq,
                params.window_size,
                params.kmer_size,
                params,
                &output_collection,
                host_timing_dst};
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
               !params.compute_divergence && !params.compute_mapq &&
               !params.compute_edit_distance && !params.exact_edit_filter_enabled &&
               !params.prefilter_enabled && params.max_occ >= 0 &&
               (!params.do_cigar || params.vulkan_extend_enabled);
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

    // Batched execution shares one implementation for all alignment algorithms.
    template <is_constexpr_aligner_params p_aligner_parameters,
              ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
              ::bioseq::simd::chaining_algorithm p_chaining_algorithm,
              ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm,
              pipeline_entry_point p_entry_point,
              sequence_alphabet p_alphabet,
              class p_minimizer,
              class p_index>
    [[nodiscard]] std::vector<match_result_collection> pipeline_run_impl_batch(
        std::span<const bioseq::storage::lazy_sequence_view<p_alphabet>> query_seqs,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        std::span<const std::vector<p_minimizer>> query_minimizers_per_query,
        const p_index& target_index,
        const ::bioseq::simd::minimap2_parameters& params,
        bioseq::indexing::gpu_minimizer_index_cache* external_gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false) {
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

        /// — one GPU index cache for the whole batch wave, shared
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

        /// — build the target minimizer index on the GPU and inject
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
                bool injected = false;
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
                injected = true;
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
        static const std::vector<p_minimizer> empty_minimizers_proto{};

        auto run_one_query = [target_len,
                              &query_seqs,
                              &query_minimizers_per_query,
                              target_seq,
                              &effective_index,
                              &scoring_matrix,
                              &alignment_kernels,
                              &params,
                              gpu_index_cache_ptr,
                              gpu_index_identity,
                              vulkan_target_index_built,
                              vulkan_target_index_ownership,
                              gpu_query_gate_identity](std::size_t qi) {
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
            out.results.reserve(detail::estimated_result_count(query_len, target_len, params));

            const auto& mins_for_this_query =
                (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers)
                    ? query_minimizers_per_query[qi]
                    : empty_minimizers_proto;

            // Short-circuit empty query or target inputs before pipeline assembly.
            if (query_len == 0 || target_len == 0) {
                return out;
            }

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
                                                           params,
                                                           query_len,
                                                           target_len,
                                                           gpu_index_cache_ptr,
                                                           gpu_index_identity);

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
            return out;
        };

        std::vector<match_result_collection> out_per_query;
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
              class p_query_minimizers = std::vector<p_minimizer>>
    [[nodiscard, gnu::always_inline]] inline match_result_collection pipeline_run_impl(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        const p_query_minimizers& query_minimizers,
        const p_index& target_index,
        const ::bioseq::simd::minimap2_parameters& params,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false,
        transient_target_index_release_authorization transient_target_index_owner = {}) {
        std::optional<context::context> owned_context;
        std::optional<context::installed_context_scope> install;
        if (context::current_or_null() == nullptr) {
            owned_context.emplace();
            install.emplace(*owned_context);
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
                bool injected = false;
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
                injected = true;
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
        out.results.reserve(detail::estimated_result_count(query_len, target_len, params));
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
        if (owned_gpu_index_cache || transient_target_index_owner.index != nullptr ||
            transient_target_index_owner.operation_owned_gpu_cache != nullptr ||
            transient_target_index_owner.direct_ledger != nullptr) {
            // CPU-index release authority is supplied only by `minimap2_port`,
            // which owns that transient value. GPU-cache release authority is
            // independent: even a borrowed persisted CPU index may use and then
            // retire the cache constructed by this invocation. An external cache
            // is always borrowed and is never installed here.
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
        auto pipeline = make_minimap_pipeline<p_aligner_parameters,
                                              p_alignment_algorithm,
                                              p_chaining_algorithm,
                                              p_minimizer_algorithm,
                                              p_entry_point,
                                              p_alphabet,
                                              p_minimizer,
                                              p_index,
                                              p_query_minimizers>(
            query_seq,
            target_seq,
            query_minimizers,
            effective_index,
            scoring_matrix,
            alignment_kernels,
            out,
            params,
            query_len,
            target_len,
            gpu_index_cache_ptr,
            gpu_index_identity,
            post_seed_owned_lifetime ? &*post_seed_owned_lifetime : nullptr);

        if constexpr (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers) {
            pipeline.on_batch(query_minimizers);
        } else {
            pipeline.on_batch(std::span<const std::byte>{});
        }
        pipeline.on_completed();
        if (direct_ledger != nullptr) {
            // Seed completion alone is insufficient: this bit is set only after
            // both the batch and the terminal pipeline callback returned normally.
            direct_ledger->pipeline_completed = true;
        }
        return out;
    }

    } // namespace bioseq::simd::pipe

} // export
