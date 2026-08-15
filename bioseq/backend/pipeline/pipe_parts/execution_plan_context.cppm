/**
 * @file pipeline/pipe_parts/execution_plan_context.cppm
 * @brief Defines execution plans, ownership contexts, and lifetime contracts.
 */
export module bioseq.pipeline.pipe:execution_plan_context;

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
                                accepted          ? ledger.expected_seed_consumers == 0u
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

    } // namespace bioseq::simd::pipe

} // export
