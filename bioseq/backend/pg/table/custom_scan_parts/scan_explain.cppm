/**
 * @file
 * @brief Renders Bioseq scan and pipeline execution details for EXPLAIN.
 */
module;
// Include Highway before the module declaration so its target definitions
// share the global module context across imports.
#include <bioseq/pg_compat.h>
#include <hwy/highway.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_scan_explain;

import std;
import bioseq.postgres.table.io;
import :access_method_contract;
import bioseq.postgres.configuration.core;
import bioseq.common.logging;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.type;
import bioseq.parallelism.mode;
import bioseq.pipeline.algorithm_identity_contract;
import bioseq.pipeline.query_sketch_plan_contract;
import bioseq.pipeline.stats;
import bioseq.postgres.scheduler.dispatch_buffer.runtime_contract;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.storage.indexing.admin.policy_catalog;
import bioseq.postgres.table.types;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences.compact_sequence_typmod;

import :custom_scan_execution_stats_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_oid_expression_support_semantic_analysis_contracts;
import :custom_scan_oid_expression_support_storage_pruning_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_semantic_payload_explain_callback_contracts;
import :custom_scan_semantic_payload_payload_contracts;
import :custom_scan_semantic_payload_query_sketch_route_contracts;

using namespace bioseq::postgres::support;

export {

    /**
     * @brief Shows the prefilter algorithm, dispatch counts and filtering results.
     *
     * Emits output only when the pipeline recorded prefilter statistics.
     * The counters include rejected regions and exact-distance checks, with flags
     * for counter overflow and inconsistent totals.
     */
    void explain_prefilter_execution_stats(
        const bioseq::pipeline_stats::pipeline_operational_stats& operational, ExplainState* es) {
        if (!operational.prefilter_execution_stats_available)
            return;
        const auto& stats = operational.prefilter_stats;
        ExplainPropertyBool("Prefilter Stats Available", true, es);
        ExplainPropertyText("Prefilter Resolved Algorithm", stats.resolved_algorithm_name(), es);
        ExplainPropertyText("Prefilter Resolved Backend", stats.resolved_backend_name(), es);
        ExplainPropertyUInteger("Prefilter Dispatches", nullptr, stats.dispatches, es);
        ExplainPropertyUInteger(
            "Prefilter SneakySnake Dispatches", nullptr, stats.sneaky_snake_dispatches, es);
        ExplainPropertyUInteger(
            "Prefilter GateKeeper Dispatches", nullptr, stats.gatekeeper_dispatches, es);
        ExplainPropertyUInteger(
            "Prefilter Scalar Dispatches", nullptr, stats.scalar_dispatches, es);
        ExplainPropertyUInteger("Prefilter SIMD Dispatches", nullptr, stats.simd_dispatches, es);
        ExplainPropertyUInteger(
            "Prefilter Vulkan Dispatches", nullptr, stats.vulkan_dispatches, es);
        ExplainPropertyUInteger("Prefilter Jobs", nullptr, stats.jobs, es);
        ExplainPropertyUInteger("Prefilter Evaluated Jobs", nullptr, stats.evaluated_jobs, es);
        ExplainPropertyUInteger("Prefilter Length 64 Jobs", nullptr, stats.length_64_jobs, es);
        ExplainPropertyUInteger(
            "Prefilter Unequal-Length Jobs", nullptr, stats.length_difference_jobs, es);
        ExplainPropertyUInteger("Prefilter Unsupported Jobs", nullptr, stats.unsupported_jobs, es);
        ExplainPropertyUInteger("Prefilter Rejected Jobs", nullptr, stats.rejected_jobs, es);
        ExplainPropertyUInteger("Prefilter Rejected Regions", nullptr, stats.rejected_regions, es);
        ExplainPropertyUInteger(
            "Prefilter Surviving Regions", nullptr, stats.surviving_regions, es);
        ExplainPropertyUInteger(
            "Prefilter Mask Rejection Candidates", nullptr, stats.mask_rejection_candidates, es);
        ExplainPropertyUInteger(
            "Prefilter Exact Guard Activations", nullptr, stats.exact_guard_activations, es);
        ExplainPropertyUInteger(
            "Prefilter Guard Confirmed Rejections", nullptr, stats.guard_confirmed_rejections, es);
        ExplainPropertyUInteger(
            "Prefilter Hamming Exact Rejections", nullptr, stats.hamming_exact_rejections, es);
        ExplainPropertyBool("Prefilter Stats Saturated", stats.is_saturated, es);
        ExplainPropertyBool("Prefilter Stats Consistent", stats.is_internally_consistent(), es);
    }

    /**
     * @brief Appends the names of the Highway targets selected by @p mask.
     *
     * Separates names and existing destination text with commas. If the mask
     * selects no targets, replaces @p destination with "none".
     */
    void append_highway_target_names(std::string& destination, const std::int64_t mask) {
        const auto names = bioseq::simd_dispatch::highway_target_names(mask);
        if (names.empty()) {
            destination = "none";
            return;
        }
        for (const std::string_view name : names) {
            if (!destination.empty())
                destination += ", ";
            destination.append(name);
        }
    }

    /** @brief Returns comma-separated Highway target names, or "none" for an empty mask. */
    [[nodiscard]] std::string highway_target_names(const std::int64_t mask) {
        std::string names;
        append_highway_target_names(names, mask);
        return names.empty() ? std::string{"none"} : names;
    }

    /** @brief Returns the first Highway target name in the mask, or "none" if none match. */
    [[nodiscard]] std::string highway_target_name(const std::int64_t target) {
        if (target == 0)
            return "none";
        const auto names = bioseq::simd_dispatch::highway_target_names(target);
        return names.empty() ? std::string{"none"} : std::string{names.front()};
    }

    /**
     * @brief Finds the query-sketch kernels when the pipeline has no QuerySketchStage.
     *
     * Uses the saved aligner parameters and the planned query-sketch route.
     * Raises a PostgreSQL error if @p params is null or the route has no kernel mapping.
     */
    [[nodiscard]] bioseq::simd_dispatch::highway_kernel_mask
    planned_query_minimizer_highway_kernels(
        const bioseq::simd::minimap2_parameters* params,
        const bioseq::simd::pipe::query_sketch_route query_sketch_route) {
        if (params == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Highway EXPLAIN has no frozen aligner parameters")));
        }
        const auto resolved = bioseq::simd::pipe::planned_query_minimizer_highway_kernels(
            *params, query_sketch_route);
        if (!resolved.has_value()) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Highway EXPLAIN cannot resolve the query minimizer route"),
                     errdetail("minimizer_algorithm=%u alphabet_code=%u window_size=%d "
                               "kmer_size=%d query_sketch_route=%u",
                               static_cast<unsigned>(params->minimizer_algorithm),
                               static_cast<unsigned>(params->alphabet_code),
                               params->window_size,
                               params->kmer_size,
                               static_cast<unsigned>(query_sketch_route))));
        }
        return *resolved;
    }

    /** @brief Returns the Highway kernels for this stage, or an empty mask if it has no algorithm. */
    [[nodiscard]] bioseq::simd_dispatch::highway_kernel_mask
    highway_kernels_for_explain_stage(const bioseq::simd::pipe::stage_description& stage) {
        return stage.algorithm.has_value() ? stage.algorithm->highway_kernels
                                           : bioseq::simd_dispatch::highway_kernel_mask{};
    }

    /**
     * @brief Calls @p visitor(kernel, stage_name) for each planned Highway function.
     *
     * Labels the fallback query kernels "query sketch", then visits the algorithms
     * in @p pipeline_desc when present. A kernel used by several stages produces
     * one call for each stage, so EXPLAIN can show where it runs.
     */
    template <class p_visitor>
    void visit_planned_highway_functions(
        const bioseq::simd_dispatch::highway_kernel_mask& top_level_query_kernels,
        const bioseq::simd::pipe::pipeline_description* pipeline_desc,
        p_visitor&& visitor) {
        // Expands one kernel mask into individual visitor calls.
        const auto visit_kernel_set =
            [&visitor](const bioseq::simd_dispatch::highway_kernel_mask& kernels,
                       const char* stage_name) {
                for (std::size_t kernel_index = 0;
                     kernel_index < bioseq::simd_dispatch::highway_kernel_count;
                     ++kernel_index) {
                    if (!kernels[kernel_index])
                        continue;
                    visitor(static_cast<bioseq::simd_dispatch::highway_kernel>(kernel_index),
                            stage_name);
                }
            };

        visit_kernel_set(top_level_query_kernels, "query sketch");
        if (pipeline_desc == nullptr)
            return;
        for (const auto& stage : pipeline_desc->stages) {
            if (!stage.algorithm.has_value())
                continue;
            visit_kernel_set(highway_kernels_for_explain_stage(stage), stage.stage_name.c_str());
        }
    }

    /**
     * @brief Prints enabled pipeline stages in text or structured EXPLAIN output.
     *
     * Uses the saved stage descriptions for algorithm settings, computed properties
     * and reserved batch sizes. When @p stage_highway_functions_already_rendered
     * is true, omits Highway functions already printed by the caller.
     * The caller handles a missing pipeline description.
     */
    void explain_pipeline_stage_descriptions(
        const bioseq::simd::pipe::pipeline_description& pipeline_desc,
        const std::string& highway_planned_architecture,
        ExplainState* es,
        const bool stage_highway_functions_already_rendered) {
        if (es->format == EXPLAIN_FORMAT_TEXT) {
            for (const auto& stage : pipeline_desc.stages) {
                /* Disabled stages carry neither an algorithm nor explanatory text. */
                if (!stage.algorithm.has_value() && !stage.extra_description.has_value()) {
                    continue;
                }

                appendStringInfoSpaces(es->str, es->indent * 2);
                if (stage.algorithm.has_value()) {
                    const auto stage_highway_kernels =
                        highway_kernels_for_explain_stage(stage);
                    appendStringInfo(es->str,
                                     "| %s: %s\n",
                                     stage.stage_name.c_str(),
                                     stage.algorithm->name.c_str());
                    for (const auto& parameter : stage.algorithm->parameters) {
                        appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
                        appendStringInfo(es->str,
                                         "%s: %s\n",
                                         parameter.name.c_str(),
                                         parameter.value.c_str());
                    }
                    for (const auto& property : stage.algorithm->computed_properties) {
                        appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
                        appendStringInfo(es->str,
                                         "[%s: %s]\n",
                                         property.name.c_str(),
                                         property.value.c_str());
                    }
                    if (!stage_highway_functions_already_rendered) {
                        for (std::size_t kernel_index = 0;
                             kernel_index < bioseq::simd_dispatch::highway_kernel_count;
                             ++kernel_index) {
                            if (!stage_highway_kernels[kernel_index]) {
                                continue;
                            }
                            const auto kernel =
                                static_cast<bioseq::simd_dispatch::highway_kernel>(kernel_index);
                            const std::string_view function =
                                bioseq::simd_dispatch::highway_kernel_name(kernel);
                            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
                            appendStringInfo(
                                es->str,
                                "Highway Function: %.*s [planned architecture: %s]\n",
                                static_cast<int>(function.size()),
                                function.data(),
                                highway_planned_architecture.c_str());
                        }
                    }
                } else {
                    appendStringInfo(es->str,
                                     "| %s — %s\n",
                                     stage.stage_name.c_str(),
                                     stage.extra_description->c_str());
                }
                if (stage.estimated_batch_size.has_value()) {
                    appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
                    appendStringInfo(
                        es->str, "[reserved: %zu]\n", *stage.estimated_batch_size);
                }
            }
            return;
        }

        ExplainOpenGroup("Pipeline Stages", "Pipeline Stages", false, es);
        for (const auto& stage : pipeline_desc.stages) {
            if (!stage.algorithm.has_value() && !stage.extra_description.has_value()) {
                continue;
            }
            /* Each stage is an unlabeled object inside the Pipeline Stages array. */
            ExplainOpenGroup("Stage", nullptr, true, es);
            ExplainPropertyText("Stage Name", stage.stage_name.c_str(), es);
            if (stage.algorithm.has_value()) {
                const auto stage_highway_kernels = highway_kernels_for_explain_stage(stage);
                ExplainPropertyText("Algorithm", stage.algorithm->name.c_str(), es);
                for (const auto& parameter : stage.algorithm->parameters) {
                    ExplainPropertyText(
                        parameter.name.c_str(), parameter.value.c_str(), es);
                }
                for (const auto& property : stage.algorithm->computed_properties) {
                    ExplainPropertyText(property.name.c_str(), property.value.c_str(), es);
                }
                if (!stage_highway_functions_already_rendered &&
                    stage_highway_kernels.any()) {
                    ExplainOpenGroup(
                        "Highway Functions", "Highway Functions", false, es);
                    for (std::size_t kernel_index = 0;
                         kernel_index < bioseq::simd_dispatch::highway_kernel_count;
                         ++kernel_index) {
                        if (!stage_highway_kernels[kernel_index]) {
                            continue;
                        }
                        const auto kernel =
                            static_cast<bioseq::simd_dispatch::highway_kernel>(kernel_index);
                        const std::string_view function =
                            bioseq::simd_dispatch::highway_kernel_name(kernel);
                        const std::string function_text{function};
                        ExplainOpenGroup("Highway Function", nullptr, true, es);
                        ExplainPropertyText("Function", function_text.c_str(), es);
                        ExplainPropertyText("Planned Architecture",
                                            highway_planned_architecture.c_str(),
                                            es);
                        ExplainCloseGroup("Highway Function", nullptr, true, es);
                    }
                    ExplainCloseGroup(
                        "Highway Functions", "Highway Functions", false, es);
                }
            } else if (stage.extra_description.has_value()) {
                ExplainPropertyText("Description", stage.extra_description->c_str(), es);
            }
            if (stage.estimated_batch_size.has_value()) {
                ExplainPropertyUInteger("Estimated Batch Size",
                                        nullptr,
                                        static_cast<uint64_t>(*stage.estimated_batch_size),
                                        es);
            }
            ExplainCloseGroup("Stage", nullptr, true, es);
        }
        ExplainCloseGroup("Pipeline Stages", "Pipeline Stages", false, es);
    }

    /**
     * @brief Shows recorded alignment work and the plan settings needed to interpret it.
     *
     * Reports actual backend and index use, job counts and GPU dispatches, plus host
     * timing and prefilter results. If @p highway_dispatch_already_rendered is false,
     * also prints planned Highway functions and available stage descriptions.
     * That path requires saved aligner parameters when no QuerySketchStage is present
     * and rejects duplicate query-sketch stages.
     */
    void explain_pipeline_execution_stats(
        const bioseq::pipeline_stats::pipeline_execution_stats& stats,
        ExplainState* es,
        const bioseq::simd::minimap2_parameters* aligner_params,
        const bioseq::simd::pipe::pipeline_description* pipeline_desc,
        const bioseq::simd::pipe::query_sketch_route query_sketch_route,
        const bool highway_dispatch_already_rendered) {
        const auto requested_backend =
            bioseq::pipeline_stats::executed_backend_name(stats.requested_backend);
        const auto actual_backend =
            bioseq::pipeline_stats::executed_backend_name(stats.actual_backend);
        const auto requested_source =
            bioseq::pipeline_stats::target_index_source_name(stats.requested_target_index_source);
        const auto actual_source =
            bioseq::pipeline_stats::target_index_source_name(stats.actual_target_index_source);
        const auto& operational = stats.operational;
        const auto& query_sketch_timing = operational.timing[bioseq::pipeline_stats::stage_index(
            bioseq::pipeline_stats::pipeline_stage::query_sketch)];
        const auto& target_index_build_timing =
            operational.timing[bioseq::pipeline_stats::stage_index(
                bioseq::pipeline_stats::pipeline_stage::target_index_build)];

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        // Give structured EXPLAIN the saved algorithm settings for batch and per-row
        // scans. ColumnScan prints this group in its own pipeline renderer.
        if (es->format != EXPLAIN_FORMAT_TEXT
            && !highway_dispatch_already_rendered && aligner_params != nullptr) {
            const auto pipeline_scheduler = bioseq::postgres::scheduler::snapshot();
            const std::string pipeline_mode{
                bioseq::parallelism::concurrency_mode_name(pipeline_scheduler.mode)};
            ExplainOpenGroup("Bioseq Pipeline", "Bioseq Pipeline", true, es);
            ExplainPropertyText(
                "Chaining Algorithm",
                bioseq::simd::chaining_algorithm_name(aligner_params->chaining_algorithm), es);
            ExplainPropertyText(
                "Base Alignment Algorithm",
                bioseq::simd::base_alignment_algorithm_name(
                    aligner_params->base_alignment_algorithm), es);
            ExplainPropertyText("Pipeline Concurrency Mode", pipeline_mode.c_str(), es);
            ExplainPropertyUInteger(
                "Max Concurrency", nullptr,
                static_cast<std::uint64_t>(pipeline_scheduler.max_threads), es);
            ExplainCloseGroup("Bioseq Pipeline", "Bioseq Pipeline", true, es);
        }
#endif

        /* Batch and per-row scans print their planned kernels here. ColumnScan
         * has already printed them with its pipeline tree. */
        if (!highway_dispatch_already_rendered) {
            const std::size_t query_sketch_stage_owners =
                pipeline_desc == nullptr
                    ? 0u
                    : bioseq::simd::pipe::query_sketch_stage_owner_count(*pipeline_desc);
            if (query_sketch_stage_owners > 1u) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("Highway EXPLAIN has duplicate query-sketch stage owners"),
                         errdetail("owners=%zu", query_sketch_stage_owners)));
            }
            const auto top_level_query_kernels =
                query_sketch_stage_owners == 1u
                    ? bioseq::simd_dispatch::highway_kernel_mask{}
                    : planned_query_minimizer_highway_kernels(aligner_params, query_sketch_route);
            /* Use the union to decide whether to open the array. Visit each stage
             * separately so shared kernels still appear under every stage that uses them. */
            auto all_planned_kernels = top_level_query_kernels;
            if (pipeline_desc != nullptr) {
                for (const auto& stage : pipeline_desc->stages) {
                    if (stage.algorithm.has_value()) {
                        all_planned_kernels |= highway_kernels_for_explain_stage(stage);
                    }
                }
            }
            /* Validate the plan before allocating C++ strings. PostgreSQL ERROR
             * skips C++ destructors when it jumps out of this call. */
            const auto highway_plan = bioseq::scheduler::highway_dispatch_plan_for_plan();
            const std::string planned = highway_target_name(highway_plan.planned_target);
            const std::string supported = highway_target_names(highway_plan.cpu_supported_targets);
            const std::string compiled = highway_target_names(highway_plan.compiled_targets);
            const std::string runnable = highway_target_names(highway_plan.runnable_targets);
            if (es->format == EXPLAIN_FORMAT_TEXT) {
                const auto print_plan_property = [es](const char* name, const std::string& value) {
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfo(es->str, "%s: %s\n", name, value.c_str());
                };
                print_plan_property("Planned Architecture", planned);
                print_plan_property("CPU-Supported Targets", supported);
                print_plan_property("Highway Compiled Targets", compiled);
                print_plan_property("Runnable Targets", runnable);
                visit_planned_highway_functions(
                    top_level_query_kernels,
                    pipeline_desc,
                    [es, &planned](const bioseq::simd_dispatch::highway_kernel kernel,
                                   const char* stage_name) {
                        const std::string_view function =
                            bioseq::simd_dispatch::highway_kernel_name(kernel);
                        appendStringInfoSpaces(es->str, es->indent * 2);
                        appendStringInfo(es->str,
                                         "Highway Function: %.*s [planned architecture: %s] "
                                         "[stage: %s]\n",
                                         static_cast<int>(function.size()),
                                         function.data(),
                                         planned.c_str(),
                                         stage_name);
                    });
                if (stats.environment.is_highway_executed_target_mixed) {
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfoString(es->str, "* Executed Architecture: mixed\n");
                } else if (stats.environment.highway_executed_target != 0) {
                    const std::string executed =
                        highway_target_name(stats.environment.highway_executed_target);
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfo(es->str, "* Executed Architecture: %s\n", executed.c_str());
                }
            } else {
                ExplainOpenGroup("Highway SIMD Dispatch", "Highway SIMD Dispatch", true, es);
                ExplainPropertyText("Planned Architecture", planned.c_str(), es);
                ExplainPropertyText("CPU-Supported Targets", supported.c_str(), es);
                ExplainPropertyText("Highway Compiled Targets", compiled.c_str(), es);
                ExplainPropertyText("Runnable Targets", runnable.c_str(), es);
                if (all_planned_kernels.any()) {
                    ExplainOpenGroup(
                        "Planned Highway Functions", "Planned Highway Functions", false, es);
                    visit_planned_highway_functions(
                        top_level_query_kernels,
                        pipeline_desc,
                        [es, &planned](const bioseq::simd_dispatch::highway_kernel kernel,
                                       const char* stage_name) {
                            const std::string_view function =
                                bioseq::simd_dispatch::highway_kernel_name(kernel);
                            const std::string function_text{function};
                            ExplainOpenGroup("Highway Function", nullptr, true, es);
                            ExplainPropertyText("Function", function_text.c_str(), es);
                            ExplainPropertyText("Planned Architecture", planned.c_str(), es);
                            ExplainPropertyText("Stage", stage_name, es);
                            ExplainCloseGroup("Highway Function", nullptr, true, es);
                        });
                    ExplainCloseGroup(
                        "Planned Highway Functions", "Planned Highway Functions", false, es);
                }
                if (stats.environment.is_highway_executed_target_mixed) {
                    ExplainPropertyText("Executed Architecture", "mixed", es);
                } else if (stats.environment.highway_executed_target != 0) {
                    const std::string executed =
                        highway_target_name(stats.environment.highway_executed_target);
                    ExplainPropertyText("Executed Architecture", executed.c_str(), es);
                }
                ExplainCloseGroup("Highway SIMD Dispatch", "Highway SIMD Dispatch", true, es);
            }
            if (pipeline_desc != nullptr) {
                explain_pipeline_stage_descriptions(
                    *pipeline_desc,
                    planned,
                    es,
                    /*stage_highway_functions_already_rendered=*/true);
            }
        }

        if (es->format == EXPLAIN_FORMAT_TEXT) {
            appendStringInfoSpaces(es->str, es->indent * 2);
            appendStringInfo(es->str,
                             "* Execution statistics [requested_backend=%.*s actual_backend=%.*s "
                             "requested_index_source=%.*s actual_index_source=%.*s "
                             "attempted=%llu fully_attested=%llu unattested=%llu "
                             "environment_mixed=%s persisted_identity_mixed=%s "
                             "identity_conflict=%s saturated=%s]\n",
                             static_cast<int>(requested_backend.size()),
                             requested_backend.data(),
                             static_cast<int>(actual_backend.size()),
                             actual_backend.data(),
                             static_cast<int>(requested_source.size()),
                             requested_source.data(),
                             static_cast<int>(actual_source.size()),
                             actual_source.data(),
                             static_cast<unsigned long long>(stats.attempted_alignment_jobs),
                             static_cast<unsigned long long>(stats.fully_attested_alignment_jobs),
                             static_cast<unsigned long long>(stats.unattested_alignment_jobs),
                             stats.environment.is_mixed_invalid ? "true" : "false",
                             stats.has_mixed_persisted_identity ? "true" : "false",
                             operational.has_operational_identity_conflict ? "true" : "false",
                             operational.are_stats_saturated ? "true" : "false");
            if (stats.unattested_alignment_jobs != 0) {
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(
                    es->str,
                    "* First unattested [target_row=%llu query_ordinal=%llu]\n",
                    static_cast<unsigned long long>(stats.first_unattested_target_row),
                    static_cast<unsigned long long>(stats.first_unattested_query_ordinal));
            }
            appendStringInfoSpaces(es->str, es->indent * 2);
            appendStringInfo(
                es->str,
                "* Index outcomes [targets=%llu persisted=%llu missing_persisted=%llu "
                "recomputed=%llu]\n",
                static_cast<unsigned long long>(operational.target_rows),
                static_cast<unsigned long long>(operational.persisted_index_rows),
                static_cast<unsigned long long>(operational.missing_persisted_index_rows),
                static_cast<unsigned long long>(operational.recomputed_target_index_rows));
            appendStringInfoSpaces(es->str, es->indent * 2);
            appendStringInfo(
                es->str,
                "* GPU dispatch [seed=%llu chain=%llu chain_backtrack=%llu "
                "coordinate_vote=%llu extend=%llu backend_declines=%llu "
                "backend_fallbacks=%llu]\n",
                static_cast<unsigned long long>(operational.gpu_seed_dispatches),
                static_cast<unsigned long long>(operational.gpu_chain_dispatches),
                static_cast<unsigned long long>(operational.gpu_chain_backtrack_dispatches),
                static_cast<unsigned long long>(operational.gpu_coordinate_vote_dispatches),
                static_cast<unsigned long long>(operational.gpu_extend_dispatches),
                static_cast<unsigned long long>(operational.backend_declines),
                static_cast<unsigned long long>(operational.backend_fallbacks));
            /* Print these GPU counts as individual EXPLAIN properties as well.
             * They count recorded dispatches, so selecting an algorithm alone leaves them zero. */
            ExplainPropertyUInteger(
                "GPU Chain Dispatches", nullptr, operational.gpu_chain_dispatches, es);
            ExplainPropertyUInteger("GPU Chain Backtrack Dispatches",
                                    nullptr,
                                    operational.gpu_chain_backtrack_dispatches,
                                    es);
            ExplainPropertyUInteger("GPU Coordinate Vote Dispatches",
                                    nullptr,
                                    operational.gpu_coordinate_vote_dispatches,
                                    es);
            appendStringInfoSpaces(es->str, es->indent * 2);
            appendStringInfo(
                es->str,
                "* Host stats [query_sketch_ns=%llu target_index_build_ns=%llu "
                "query_hash_readbacks=%llu query_symbol_packing=%llu "
                "target_symbol_packing=%llu alignment_classification=%llu "
                "cigar_construction=%llu]\n",
                static_cast<unsigned long long>(query_sketch_timing.host_wall_ns),
                static_cast<unsigned long long>(target_index_build_timing.host_wall_ns),
                static_cast<unsigned long long>(operational.host_query_hash_readbacks),
                static_cast<unsigned long long>(operational.host_query_symbol_packing_calls),
                static_cast<unsigned long long>(operational.host_target_symbol_packing_calls),
                static_cast<unsigned long long>(
                    operational.host_alignment_job_classification_calls),
                static_cast<unsigned long long>(operational.host_cigar_construction_calls));
            explain_prefilter_execution_stats(operational, es);
            return;
        }

        ExplainOpenGroup("Bioseq Execution Statistics", "Bioseq Execution Statistics", true, es);
        ExplainPropertyText("Requested Backend", requested_backend.data(), es);
        ExplainPropertyText("Actual Backend", actual_backend.data(), es);
        ExplainPropertyText("Requested Target Index Source", requested_source.data(), es);
        ExplainPropertyText("Actual Target Index Source", actual_source.data(), es);
        ExplainPropertyUInteger(
            "Attempted Alignment Jobs", nullptr, stats.attempted_alignment_jobs, es);
        ExplainPropertyUInteger(
            "Fully Attested Alignment Jobs", nullptr, stats.fully_attested_alignment_jobs, es);
        ExplainPropertyUInteger(
            "Unattested Alignment Jobs", nullptr, stats.unattested_alignment_jobs, es);
        if (stats.unattested_alignment_jobs != 0) {
            ExplainPropertyUInteger(
                "First Unattested Target Row", nullptr, stats.first_unattested_target_row, es);
            ExplainPropertyUInteger("First Unattested Query Ordinal",
                                    nullptr,
                                    stats.first_unattested_query_ordinal,
                                    es);
        }
        ExplainPropertyBool("Environment Mixed Invalid", stats.environment.is_mixed_invalid, es);
        ExplainPropertyBool("Mixed Persisted Identity", stats.has_mixed_persisted_identity, es);
        ExplainPropertyBool(
            "Operational Identity Conflict", operational.has_operational_identity_conflict, es);
        ExplainPropertyBool("Stats Saturated", operational.are_stats_saturated, es);
        ExplainPropertyUInteger("Target Rows", nullptr, operational.target_rows, es);
        ExplainPropertyUInteger(
            "Persisted Index Rows", nullptr, operational.persisted_index_rows, es);
        ExplainPropertyUInteger(
            "Missing Persisted Index Rows", nullptr, operational.missing_persisted_index_rows, es);
        ExplainPropertyUInteger(
            "Recomputed Target Index Rows", nullptr, operational.recomputed_target_index_rows, es);
        ExplainPropertyUInteger(
            "GPU Seed Dispatches", nullptr, operational.gpu_seed_dispatches, es);
        ExplainPropertyUInteger(
            "GPU Chain Dispatches", nullptr, operational.gpu_chain_dispatches, es);
        ExplainPropertyUInteger("GPU Chain Backtrack Dispatches",
                                nullptr,
                                operational.gpu_chain_backtrack_dispatches,
                                es);
        ExplainPropertyUInteger("GPU Coordinate Vote Dispatches",
                                nullptr,
                                operational.gpu_coordinate_vote_dispatches,
                                es);
        ExplainPropertyUInteger(
            "GPU Extend Dispatches", nullptr, operational.gpu_extend_dispatches, es);
        ExplainPropertyUInteger("Backend Declines", nullptr, operational.backend_declines, es);
        ExplainPropertyUInteger("Backend Fallbacks", nullptr, operational.backend_fallbacks, es);
        ExplainPropertyUInteger(
            "Query Sketch Host Wall Ns", nullptr, query_sketch_timing.host_wall_ns, es);
        ExplainPropertyUInteger(
            "Target Index Build Host Wall Ns", nullptr, target_index_build_timing.host_wall_ns, es);
        ExplainPropertyUInteger(
            "Host Query Hash Readbacks", nullptr, operational.host_query_hash_readbacks, es);
        ExplainPropertyUInteger("Host Query Symbol Packing Calls",
                                nullptr,
                                operational.host_query_symbol_packing_calls,
                                es);
        ExplainPropertyUInteger("Host Target Symbol Packing Calls",
                                nullptr,
                                operational.host_target_symbol_packing_calls,
                                es);
        ExplainPropertyUInteger("Host Alignment Job Classification Calls",
                                nullptr,
                                operational.host_alignment_job_classification_calls,
                                es);
        ExplainPropertyUInteger("Host CIGAR Construction Calls",
                                nullptr,
                                operational.host_cigar_construction_calls,
                                es);
        explain_prefilter_execution_stats(operational, es);
        ExplainCloseGroup("Bioseq Execution Statistics", "Bioseq Execution Statistics", true, es);
    }

    /**
     * @brief Explains the column scan using its plan and available runtime state.
     *
     * Shows projected columns, filters and symbol_count metadata lookups. For mmsearch,
     * adds estimated index use and sequence-width conversion, then the saved pipeline
     * and recorded execution statistics when available. Reads the plan for information
     * that must also appear before execution. Cache counters are global.
     * @p ancestors supplies the context for printing plan expressions.
     */
    void bioseq_column_scan_explain_custom_scan(CustomScanState* node,
                                                List* ancestors,
                                                ExplainState* es) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_ExplainCustomScan() entered");
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);

        List* context = set_deparse_context_planstate(
            es->deparse_cxt, reinterpret_cast<Node*>(&node->ss.ps), ancestors);

        List* projected_columns = bioseq_column_var_needed(bioseq_scan_state);
        const char* projected_columns_str =
            bioseq_column_projected_columns_str(context, projected_columns);
        ExplainPropertyText("Bioseq Projected Columns", projected_columns_str, es);

        CustomScan* cscan = expect_pg_node<CustomScan>(to_pg_node(node->ss.ps.plan));
        List* chunk_group_filter = storage_prunable_quals_without_mmsearch(
            postgres_list<List>(cscan->custom_exprs).get_second(), get_cached_stub_oids());
        if (chunk_group_filter != nullptr) {
            const char* pushdown_clauses_str =
                bioseq_column_pushdown_clauses_str(context, chunk_group_filter);
            ExplainPropertyText("Bioseq Chunk Group Filters", pushdown_clauses_str, es);

            column_scan_handle bioseq_column_scan_desc =
                reinterpret_cast<column_scan_handle>(node->ss.ss_currentScanDesc);
            if (bioseq_column_scan_desc != nullptr) {
                ExplainPropertyInteger("Bioseq Chunk Groups Removed by Filter",
                                       nullptr,
                                       filtered_chunk_group_count(bioseq_column_scan_desc),
                                       es);
            }
        }

        if (bioseq_scan_state->vectorization.is_enabled &&
            !bioseq_scan_state->vectorization.qualifiers.is_empty()) {
            const char* vectorized_where_clauses = bioseq_column_pushdown_clauses_str(
                context, bioseq_scan_state->vectorization.qualifiers.get());
            ExplainPropertyText("Bioseq Vectorized Filter", vectorized_where_clauses, es);
        }

        /* Read symbol_count calls from the plan so plain EXPLAIN lists metadata
         * lookups too. The executor fills symbol_count_sites only on its first scan. */
        {
            StringInfoData meta_buf;
            initStringInfo(&meta_buf);
            bool first = true;
            const bioseq_stub_oids& oids_snapshot = get_cached_stub_oids();
            Relation scan_rel = node->ss.ss_currentRelation;
            for (auto tgt : postgres_list<TargetEntry>(node->ss.ps.plan->targetlist)) {
                if (tgt == nullptr) {
                    continue;
                }
                FuncExpr* fe = try_cast_pg_node<FuncExpr>(to_pg_node(tgt->expr));
                if (fe == nullptr || fe->funcid != oids_snapshot.symbol_count_oid ||
                    list_length(fe->args) != 2) {
                    continue;
                }
                postgres_list<Node> args(fe->args);
                Var* col_var = try_cast_pg_node<Var>(args.get_first());
                Const* letter_const = try_cast_pg_node<Const>(args.get_second());
                if (col_var == nullptr || letter_const == nullptr) {
                    continue;
                }
                if (letter_const->constisnull || col_var->vartypmod < 0) {
                    continue;
                }
                text* letter_detoasted =
                    reinterpret_cast<text*>(PG_DETOAST_DATUM(letter_const->constvalue));
                if (VARSIZE_ANY_EXHDR(letter_detoasted) != 1) {
                    if (reinterpret_cast<void*>(letter_detoasted) !=
                        reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
                        pfree(letter_detoasted);
                    }
                    continue;
                }
                const char letter_char = *VARDATA_ANY(letter_detoasted);
                if (reinterpret_cast<void*>(letter_detoasted) !=
                    reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
                    pfree(letter_detoasted);
                }
                const compact_sequence_stdc_type_params typmod =
                    compact_sequence_stdc_type_params::decode(col_var->vartypmod);
                const int rank =
                    bioseq::storage::indexing::letter_to_rank(typmod.alphabet_code, letter_char);
                if (rank < 0) {
                    continue;
                }
                std::string col_name;
                if (scan_rel != nullptr && col_var->varattno >= 1) {
                    char* attname_cstr = get_attname(
                        RelationGetRelid(scan_rel), col_var->varattno, /*missing_ok=*/true);
                    if (attname_cstr != nullptr) {
                        col_name.assign(attname_cstr);
                        pfree(attname_cstr);
                    }
                }
                if (!first) {
                    appendStringInfoString(&meta_buf, ", ");
                }
                first = false;
                appendStringInfo(&meta_buf,
                                 "%s.symbol_count('%c', rank=%d)",
                                 col_name.empty() ? "<unknown>" : col_name.c_str(),
                                 letter_char,
                                 rank);
            }
            if (!first) {
                ExplainPropertyText("Bioseq Fetched From Meta", meta_buf.data, es);
            }
            pfree(meta_buf.data);
        }

        /* Show index-use estimates and the alignment pipeline only for mmsearch.
         * A scan that only reads symbol_count metadata does not use the minimizer index. */
        /* Reuse the scan snapshot for the index summary and pipeline details. */
        scan_state* explain_scan_state = scan_state::from_custom_scan_state(node);
        const bioseq_scan_runtime* explain_runtime =
            (explain_scan_state != nullptr && explain_scan_state->runtime_state != nullptr)
                ? static_cast<const bioseq_scan_runtime*>(explain_scan_state->runtime_state)
                : nullptr;
        const per_scan_aligner_state* aligner_snapshot =
            (explain_runtime != nullptr) ? &explain_runtime->aligner_state : nullptr;
        frozen_scan_semantics explain_frozen_semantics{};
        const bool explain_semantics_valid =
            aligner_snapshot != nullptr && aligner_snapshot->execution_plan
                ? true
                : decode_bioseq_semantic_payload(cscan->custom_private, &explain_frozen_semantics);
        const bool explain_read_sequence_index =
            aligner_snapshot != nullptr && aligner_snapshot->execution_plan
                ? explain_runtime->buffer.reads_sequence_index()
            : explain_semantics_valid ? explain_frozen_semantics.read_sequence_index
                                      : false;
        const bioseq::indexing::index_read_requirement explain_index_read_requirement =
            aligner_snapshot != nullptr && aligner_snapshot->execution_plan
                ? aligner_snapshot->index_read_requirement
            : explain_semantics_valid ? explain_frozen_semantics.index_read_requirement
                                      : bioseq::indexing::index_read_requirement::allow;

        {
            /* Use saved feature flags when available. Otherwise inspect the plan,
             * including mmsearch calls kept separately from ordinary scan filters. */
            const bioseq_stub_oids& explain_oids = get_cached_stub_oids();
            List* explain_owned_mmsearch =
                cscan != nullptr && list_length(cscan->custom_exprs) >= 4
                    ? reinterpret_cast<List*>(list_nth(cscan->custom_exprs, 3))
                    : NIL;
            scan_feature_flags features =
                (aligner_snapshot != nullptr && aligner_snapshot->execution_plan)
                    ? aligner_snapshot->features
                    : detect_feature_flags(
                          node->ss.ps.plan->targetlist, node->ss.ps.plan->qual, explain_oids);
            features.has_mmsearch = features.has_mmsearch || explain_owned_mmsearch != NIL;
            bool evaluated_operand_owner =
                explain_runtime != nullptr && explain_runtime->match_evaluated_submission;
            if (!evaluated_operand_owner && explain_owned_mmsearch != NIL) {
                FuncExpr* owned = try_cast_pg_node<FuncExpr>(
                    reinterpret_cast<Node*>(linitial(explain_owned_mmsearch)));
                if (owned != nullptr && list_length(owned->args) >= 2) {
                    Node* target = reinterpret_cast<Node*>(linitial(owned->args));
                    Node* query = reinterpret_cast<Node*>(lsecond(owned->args));
                    Var* query_var = try_cast_pg_node<Var>(query);
                    evaluated_operand_owner =
                        try_cast_pg_node<Var>(target) == nullptr ||
                        (try_cast_pg_node<Const>(query) == nullptr &&
                         try_cast_pg_node<Param>(query) == nullptr && query_var == nullptr);
                }
            }

            /* Only mmsearch needs the minimizer-index summary. */
            if (features.has_mmsearch) {
                ExplainPropertyText(
                    "Bioseq Index Read Requirement",
                    bioseq::indexing::index_read_requirement_name(explain_index_read_requirement)
                        .data(),
                    es);
                if (evaluated_operand_owner) {
                    ExplainPropertyText(
                        "Bioseq Match Operand Evaluation",
                        "base-local ExprState (target then query; persisted target index absent)",
                        es);
                }
                /* Infer query-width conversion from the plan so it is visible before
                 * execution has filled the runtime conversion buffer. */
                {
                    const explain_typmod_coalesce coalesce_hint = explain_detect_typmod_coalesce(
                        explain_owned_mmsearch != NIL ? explain_owned_mmsearch
                                                      : node->ss.ps.plan->qual,
                        explain_oids);
                    if (coalesce_hint.active) {
                        const auto coa_name = pgseq_supported_alphabets::name_of_alphabet_code(
                            coalesce_hint.col_alpha);
                        StringInfoData coa_buf;
                        initStringInfo(&coa_buf);
                        appendStringInfo(&coa_buf,
                                         "coalesce to seq('%.*s', %u)",
                                         static_cast<int>(coa_name.size()),
                                         coa_name.data(),
                                         static_cast<unsigned>(coalesce_hint.col_byte_width) * 8u);
                        ExplainPropertyText("Bioseq Typmod Coalesce", coa_buf.data, es);
                        pfree(coa_buf.data);
                    }
                }
                Oid user_relid = InvalidOid;
                double rel_tuples = -1.0;
                if (node->ss.ss_currentRelation != nullptr) {
                    user_relid = RelationGetRelid(node->ss.ss_currentRelation);
                    if (node->ss.ss_currentRelation->rd_rel != nullptr) {
                        rel_tuples =
                            static_cast<double>(node->ss.ss_currentRelation->rd_rel->reltuples);
                    }
                } else if (cscan != nullptr && node->ss.ps.state != nullptr) {
                    RangeTblEntry* rte =
                        rt_fetch(cscan->scan.scanrelid, node->ss.ps.state->es_range_table);
                    if (rte != nullptr) {
                        user_relid = rte->relid;
                    }
                }

                if (user_relid != InvalidOid) {
                    if (rel_tuples < 0.0) {
                        HeapTuple cls_tup = SearchSysCache1(
                            RELOID,
                            bioseq::postgres::support::pg_datum::from_oid(user_relid).raw());
                        if (HeapTupleIsValid(cls_tup)) {
                            Form_pg_class cls = reinterpret_cast<Form_pg_class>(GETSTRUCT(cls_tup));
                            rel_tuples = static_cast<double>(cls->reltuples);
                            ReleaseSysCache(cls_tup);
                        }
                    }
                    /* Estimate how many rows can reuse stored minimizers. Evaluated targets
                     * and disabled index reads report full recomputation. These percentages
                     * describe expected index use, while execution counters record completed work. */
                    const float index_presence = bioseq::storage::indexing::estimate_index_presence(
                        user_relid, rel_tuples, bioseq::indexing::k_default_active_read_index);
                    float fetched_pct = 0.0f;
                    float recomputed_pct = 100.0f;
                    const bool runtime_coalesce_active =
                        (explain_runtime != nullptr) && explain_runtime->buffer.coalesce_active();
                    if (evaluated_operand_owner) {
                        ExplainPropertyText(
                            "Bioseq Recomputed",
                            "minimizers 100% (evaluated logical view requires index absence)",
                            es);
                    } else if (explain_read_sequence_index) {
                        fetched_pct = index_presence * 100.0f;
                        recomputed_pct = (1.0f - index_presence) * 100.0f;
                        if (fetched_pct > 0.0f) {
                            StringInfoData presence_buf;
                            initStringInfo(&presence_buf);
                            appendStringInfo(&presence_buf,
                                             "minimizers (index presence: %.0f%%",
                                             static_cast<double>(fetched_pct));
                            if (runtime_coalesce_active && explain_runtime != nullptr) {
                                /* Include the query conversion used at runtime beside the estimate
                                 * of rows with stored minimizers. */
                                const auto col_alpha =
                                    explain_runtime->buffer.mmsearch_alphabet_code();
                                const auto col_width =
                                    explain_runtime->buffer.mmsearch_storage_unit_byte_width();
                                // The buffer reports storage-unit width in bytes. SQL seq typmods use
                                // bits, so four bytes must print as 32.
                                const auto width_to_bits = [](uint8_t byte_width) {
                                    return static_cast<unsigned>(byte_width) * 8u;
                                };
                                const auto col_name =
                                    pgseq_supported_alphabets::name_of_alphabet_code(col_alpha);
                                appendStringInfo(&presence_buf,
                                                 ", coalesce to seq('%.*s', %u)",
                                                 static_cast<int>(col_name.size()),
                                                 col_name.data(),
                                                 width_to_bits(col_width));
                            }
                            appendStringInfoString(&presence_buf, ")");
                            ExplainPropertyText(
                                "Bioseq Fetched From Sequence Index", presence_buf.data, es);
                            pfree(presence_buf.data);
                        }
                        if (recomputed_pct > 0.0f) {
                            StringInfoData recompute_buf;
                            initStringInfo(&recompute_buf);
                            appendStringInfo(&recompute_buf,
                                             "minimizers %.0f%%",
                                             static_cast<double>(recomputed_pct));
                            ExplainPropertyText("Bioseq Recomputed", recompute_buf.data, es);
                            pfree(recompute_buf.data);
                        }
                    } else {
                        /* Index reads are disabled in this scan. */
                        ExplainPropertyText("Bioseq Recomputed",
                                            "minimizers 100% (read_sequence_index disabled)",
                                            es);
                    }
                }

                {
                    const auto scheduler = bioseq::postgres::scheduler::snapshot();
                    const auto scheduler_mode =
                        bioseq::parallelism::concurrency_mode_name(scheduler.mode);
                    StringInfoData scheduler_buf;
                    initStringInfo(&scheduler_buf);
                    appendStringInfo(
                        &scheduler_buf,
                        "supervisor=%s mode=%.*s workers=%u/%u batches=%llu/%llu awaiting=%llu "
                        "queries=%llu/%llu qps=%.3f async_persist_waiting=%llu "
                        "index_memory_reserved=%llu",
                        scheduler.is_supervisor ? "on" : "local",
                        static_cast<int>(scheduler_mode.size()),
                        scheduler_mode.data(),
                        static_cast<unsigned>(scheduler.current_threads),
                        static_cast<unsigned>(scheduler.max_threads),
                        static_cast<unsigned long long>(scheduler.batches_processed),
                        static_cast<unsigned long long>(scheduler.batches_scheduled),
                        static_cast<unsigned long long>(scheduler.batches_awaiting),
                        static_cast<unsigned long long>(scheduler.queries_processed),
                        static_cast<unsigned long long>(scheduler.queries_waiting),
                        scheduler.queries_per_sec,
                        static_cast<unsigned long long>(scheduler.async_persist_jobs_waiting),
                        static_cast<unsigned long long>(scheduler.index_memory_reserved_bytes));
                    ExplainPropertyText("Bioseq Scheduler", scheduler_buf.data, es);
                    pfree(scheduler_buf.data);
                }
            }
        }

        /* Show the mmsearch pipeline from this scan's saved parameters, so later
         * setting changes cannot alter the explanation of its algorithm choices. */
        if (aligner_snapshot != nullptr && aligner_snapshot->execution_plan &&
            aligner_snapshot->features.has_mmsearch) {
            using bioseq::simd::base_alignment_algorithm;
            using bioseq::simd::base_alignment_algorithm_name;
            using bioseq::simd::chaining_algorithm;
            using bioseq::simd::chaining_algorithm_name;
            const auto& params = aligner_snapshot->execution_plan->frozen_parameters;
            const auto& features = aligner_snapshot->features;
            const auto highway_plan = bioseq::scheduler::highway_dispatch_plan_for_plan();
            const std::string highway_planned_architecture =
                highway_target_name(highway_plan.planned_target);
            const std::string highway_cpu_supported_architectures =
                highway_target_names(highway_plan.cpu_supported_targets);
            const std::string highway_compiled_architectures =
                highway_target_names(highway_plan.compiled_targets);
            const std::string highway_runnable_architectures =
                highway_target_names(highway_plan.runnable_targets);
            /* Print stages from the saved pipeline description. Without it, text
             * EXPLAIN can still show the chosen chaining and base-alignment algorithms. */
            const bool has_pipeline_desc = aligner_snapshot->pipeline_desc.has_value();
            const std::size_t query_sketch_stage_owners =
                has_pipeline_desc ? bioseq::simd::pipe::query_sketch_stage_owner_count(
                                        *aligner_snapshot->pipeline_desc)
                                  : 0u;
            if (query_sketch_stage_owners > 1u) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("Highway EXPLAIN has duplicate query-sketch stage owners"),
                         errdetail("owners=%zu", query_sketch_stage_owners)));
            }
            /* QuerySketchStage supplies its own kernels. Resolve a fallback list
             * only when that stage is absent from the description. */
            const auto top_level_query_highway_kernels =
                query_sketch_stage_owners == 1u
                    ? bioseq::simd_dispatch::highway_kernel_mask{}
                    : planned_query_minimizer_highway_kernels(
                          &params, query_sketch_plan_route_for(explain_index_read_requirement));
            const auto pipeline_scheduler = bioseq::postgres::scheduler::snapshot();
            const auto pipeline_mode =
                bioseq::parallelism::concurrency_mode_name(pipeline_scheduler.mode);
            const std::string pipeline_mode_text{pipeline_mode};
            const char* pipeline_label =
                pipeline_scheduler.mode ==
                        bioseq::parallelism::pipeline_concurrency_mode::single_thread
                    ? "Single thread"
                : pipeline_scheduler.mode ==
                        bioseq::parallelism::pipeline_concurrency_mode::dispatch_parallel
                    ? "Parallel dispatch"
                    : "Task-parallel";

            if (es->format == EXPLAIN_FORMAT_TEXT) {
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfoString(es->str, "One-to-many alignment:\n");
                ++es->indent;
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(es->str,
                                 "%s [mode=%.*s, workers=%u, index_memory_reserved=%llu bytes]:\n",
                                 pipeline_label,
                                 static_cast<int>(pipeline_mode.size()),
                                 pipeline_mode.data(),
                                 static_cast<unsigned>(pipeline_scheduler.max_threads),
                                 static_cast<unsigned long long>(
                                     pipeline_scheduler.index_memory_reserved_bytes));
                ++es->indent;
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(
                    es->str, "Planned Architecture: %s\n", highway_planned_architecture.c_str());
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(es->str,
                                 "CPU-Supported Targets: %s\n",
                                 highway_cpu_supported_architectures.c_str());
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(es->str,
                                 "Highway Compiled Targets: %s\n",
                                 highway_compiled_architectures.c_str());
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(
                    es->str, "Runnable Targets: %s\n", highway_runnable_architectures.c_str());
                for (std::size_t kernel_index = 0;
                     kernel_index < bioseq::simd_dispatch::highway_kernel_count;
                     ++kernel_index) {
                    if (!top_level_query_highway_kernels[kernel_index])
                        continue;
                    const auto kernel =
                        static_cast<bioseq::simd_dispatch::highway_kernel>(kernel_index);
                    const std::string_view function =
                        bioseq::simd_dispatch::highway_kernel_name(kernel);
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfo(es->str,
                                     "Highway Function: %.*s [planned architecture: %s] "
                                     "[stage: query sketch]\n",
                                     static_cast<int>(function.size()),
                                     function.data(),
                                     highway_planned_architecture.c_str());
                }
                if (explain_runtime != nullptr) {
                    const auto& environment = explain_runtime->buffer.execution_stats().environment;
                    if (environment.is_highway_executed_target_mixed) {
                        appendStringInfoSpaces(es->str, es->indent * 2);
                        appendStringInfoString(es->str, "* Executed Architecture: mixed\n");
                    } else if (environment.highway_executed_target != 0) {
                        const std::string executed =
                            highway_target_name(environment.highway_executed_target);
                        appendStringInfoSpaces(es->str, es->indent * 2);
                        appendStringInfo(
                            es->str, "* Executed Architecture: %s\n", executed.c_str());
                    }
                }

                /* Warn when index reads are enabled and the requested k-mer or window
                 * size differs from the fixed 15/10 values reported for stored indexes. */
                const bool sidecar_path_active = explain_read_sequence_index;
                const bool user_kw_differs =
                    sidecar_path_active && (params.kmer_size != 15 || params.window_size != 10);
                if (user_kw_differs) {
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfoString(es->str,
                                           "NOTE: sidecar rows pin kmer_size=15 window_size=10\n");
                }

                if (has_pipeline_desc) {
                    explain_pipeline_stage_descriptions(
                        *aligner_snapshot->pipeline_desc,
                        highway_planned_architecture,
                        es,
                        /*stage_highway_functions_already_rendered=*/false);
                } else {
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfo(
                        es->str,
                        "| Pipeline (chaining=%s, base_alignment=%s)\n",
                        chaining_algorithm_name(params.chaining_algorithm),
                        base_alignment_algorithm_name(params.base_alignment_algorithm));
                }

                /* Add row and index-use counts to the submission line once the buffer
                 * reports completed targets. Before then, print the submission label alone. */
                auto print_submit = [es](const char* label) {
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfo(es->str, "* %s\n", label);
                };
                if (explain_runtime != nullptr) {
                    const auto& stats = explain_runtime->buffer.execution_stats();
                    const auto& operational = stats.operational;
                    if (operational.target_rows > 0) {
                        StringInfoData submit_label;
                        initStringInfo(&submit_label);
                        appendStringInfo(
                            &submit_label,
                            "Submit results [targets=%llu persisted_index=%llu recomputed=%llu]",
                            static_cast<unsigned long long>(operational.target_rows),
                            static_cast<unsigned long long>(operational.persisted_index_rows),
                            static_cast<unsigned long long>(
                                operational.recomputed_target_index_rows));
                        print_submit(submit_label.data);
                        pfree(submit_label.data);
                    } else {
                        print_submit("Submit results");
                    }
                    explain_pipeline_execution_stats(
                        stats,
                        es,
                        &params,
                        aligner_snapshot->pipeline_desc.has_value()
                            ? &*aligner_snapshot->pipeline_desc
                            : nullptr,
                        query_sketch_plan_route_for(explain_index_read_requirement),
                        /*highway_dispatch_already_rendered=*/true);
                } else {
                    print_submit("Submit results");
                }

                if (params.enable_profiling) {
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfoString(es->str, "Runtime:\n");
                    ++es->indent;
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    appendStringInfo(es->str,
                                     "profiling: enabled (see bioseq_log<logging_level::debug> for "
                                     "per-stage μs/ns counters)\n");
                    appendStringInfoSpaces(es->str, es->indent * 2);
                    if (explain_runtime != nullptr) {
                        const auto& stats = explain_runtime->buffer.execution_stats();
                        const auto& operational = stats.operational;
                        appendStringInfo(
                            es->str,
                            "batch summary: target_rows=%llu, persisted_index=%llu, "
                            "recomputed=%llu, attempted_jobs=%llu, unattested_jobs=%llu\n",
                            static_cast<unsigned long long>(operational.target_rows),
                            static_cast<unsigned long long>(operational.persisted_index_rows),
                            static_cast<unsigned long long>(
                                operational.recomputed_target_index_rows),
                            static_cast<unsigned long long>(stats.attempted_alignment_jobs),
                            static_cast<unsigned long long>(stats.unattested_alignment_jobs));
                    }
                    --es->indent;
                }

                es->indent -= 2;
                (void)features; // The mmsearch flag controls this pipeline section.
            } else {
                /* Use the same stage descriptions for JSON, YAML and XML output. */
                ExplainOpenGroup("Bioseq Pipeline", "Bioseq Pipeline", true, es);
                ExplainPropertyText(
                    "Chaining Algorithm", chaining_algorithm_name(params.chaining_algorithm), es);
                ExplainPropertyText("Base Alignment Algorithm",
                                    base_alignment_algorithm_name(params.base_alignment_algorithm),
                                    es);
                ExplainPropertyText("Pipeline Concurrency Mode", pipeline_mode_text.c_str(), es);
                ExplainPropertyUInteger("Max Concurrency",
                                        nullptr,
                                        static_cast<uint64_t>(pipeline_scheduler.max_threads),
                                        es);
                ExplainPropertyUInteger(
                    "Index Memory Reserved Bytes",
                    nullptr,
                    static_cast<uint64_t>(pipeline_scheduler.index_memory_reserved_bytes),
                    es);
                ExplainOpenGroup("Highway SIMD Dispatch", "Highway SIMD Dispatch", true, es);
                ExplainPropertyText(
                    "Planned Architecture", highway_planned_architecture.c_str(), es);
                ExplainPropertyText(
                    "CPU-Supported Targets", highway_cpu_supported_architectures.c_str(), es);
                ExplainPropertyText(
                    "Highway Compiled Targets", highway_compiled_architectures.c_str(), es);
                ExplainPropertyText("Runnable Targets", highway_runnable_architectures.c_str(), es);
                if (top_level_query_highway_kernels.any()) {
                    ExplainOpenGroup(
                        "Planned Highway Functions", "Planned Highway Functions", false, es);
                    for (std::size_t kernel_index = 0;
                         kernel_index < bioseq::simd_dispatch::highway_kernel_count;
                         ++kernel_index) {
                        if (!top_level_query_highway_kernels[kernel_index])
                            continue;
                        const auto kernel =
                            static_cast<bioseq::simd_dispatch::highway_kernel>(kernel_index);
                        const std::string_view function =
                            bioseq::simd_dispatch::highway_kernel_name(kernel);
                        const std::string function_text{function};
                        ExplainOpenGroup("Highway Function", nullptr, true, es);
                        ExplainPropertyText("Function", function_text.c_str(), es);
                        ExplainPropertyText(
                            "Planned Architecture", highway_planned_architecture.c_str(), es);
                        ExplainPropertyText("Stage", "query sketch", es);
                        ExplainCloseGroup("Highway Function", nullptr, true, es);
                    }
                    ExplainCloseGroup(
                        "Planned Highway Functions", "Planned Highway Functions", false, es);
                }
                if (explain_runtime != nullptr) {
                    const auto& environment = explain_runtime->buffer.execution_stats().environment;
                    if (environment.is_highway_executed_target_mixed) {
                        ExplainPropertyText("Executed Architecture", "mixed", es);
                    } else if (environment.highway_executed_target != 0) {
                        const std::string executed =
                            highway_target_name(environment.highway_executed_target);
                        ExplainPropertyText("Executed Architecture", executed.c_str(), es);
                    }
                }
                ExplainCloseGroup("Highway SIMD Dispatch", "Highway SIMD Dispatch", true, es);
                if (has_pipeline_desc) {
                    explain_pipeline_stage_descriptions(
                        *aligner_snapshot->pipeline_desc,
                        highway_planned_architecture,
                        es,
                        /*stage_highway_functions_already_rendered=*/false);
                }
                ExplainCloseGroup("Bioseq Pipeline", "Bioseq Pipeline", true, es);
                if (explain_runtime != nullptr) {
                    const auto& stats = explain_runtime->buffer.execution_stats();
                    explain_pipeline_execution_stats(
                        stats,
                        es,
                        &params,
                        aligner_snapshot->pipeline_desc.has_value()
                            ? &*aligner_snapshot->pipeline_desc
                            : nullptr,
                        query_sketch_plan_route_for(explain_index_read_requirement),
                        /*highway_dispatch_already_rendered=*/true);
                }
            }
        }

        if (bioseq::settings::column_cache::enabled) {
            ExplainOpenGroup("Cache", "Cache", true, es);
            if (es->format == EXPLAIN_FORMAT_TEXT) {
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(es->str, "->  Cache\n");
                es->indent += 3;
            }

            column_cache_statistics* statistics = get_column_cache_statistics();

            ExplainPropertyUInteger("Cache Hits", nullptr, statistics->hits, es);

            ExplainPropertyUInteger("Cache Misses", nullptr, statistics->misses, es);

            ExplainPropertyUInteger("Cache Evictions", nullptr, statistics->evictions, es);

            ExplainPropertyUInteger("Cache Writes", nullptr, statistics->writes, es);

            ExplainPropertyUInteger(
                "Cache Maximum Size", nullptr, statistics->maximum_cache_size, es);

            ExplainPropertyUInteger(
                "Cache Ending Size", nullptr, statistics->ending_cache_size, es);

            ExplainPropertyUInteger("Total Cache Entries", nullptr, statistics->entries, es);
            ExplainCloseGroup("Cache", "Cache", true, es);
            if (es->format == EXPLAIN_FORMAT_TEXT) {
                es->indent -= 3;
            }
        }
    }

} // export custom-scan partition declarations
