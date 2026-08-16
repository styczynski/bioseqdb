/**
 * @file
 * @brief Finalizes parallel and per-stripe scan costs.
 */
module;
// Anchor Highway in this global module fragment so target-specific definitions
// imported through multiple module graphs share one translation-unit context.
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

export module bioseq.postgres.table:custom_scan_scan_paths_cost_finalization;

import std;
import bioseq.postgres.table.io;
import :access_method;
import :access_common;
import :utils;
import bioseq.postgres.column.table_access;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.vectorization.execution;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.external.path_builder_slot;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.table.types;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.stats;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.sequences.cigar;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;
import bioseq.postgres.storage.scanner;
import bioseq.indexing.type;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.admin;
import bioseq.instrumentation.summary_writer;
import bioseq.common.seqan_adapter;
import bioseq.postgres.table.batch_sort_scan;

import :custom_scan_support;
import :custom_scan_slice_projection;
import :custom_scan_join_paths;

import :custom_scan_scan_paths_admission;
import :custom_scan_scan_paths_path_construction;

using namespace bioseq::postgres::support;

export {
    /** @brief Divides scan cost and row estimates across planned parallel participants. */
    void adjust_bioseq_column_parallel_scan_cost(Path* path) {
        /* Adjust costing for parallelism, if used. */
        if (path->parallel_workers > 0) {
            double parallel_divisor = path->parallel_workers;

            if (parallel_leader_participation) {
                double leader_contribution;

                leader_contribution = 1.0 - (0.3 * path->parallel_workers);
                if (leader_contribution > 0)
                    parallel_divisor += leader_contribution;
            }

            /* The CPU cost is divided among all the workers. */
            path->total_cost /= parallel_divisor;

            path->rows = clamp_row_est(path->rows / parallel_divisor);
        }
    }

    /** @brief Costs a custom scan from projected columns, restriction selectivity, and stripes. */
    void cost_bioseq_column_scan(PlannerInfo* root,
                                 RelOptInfo* rel,
                                 Oid relation_id,
                                 CustomPath* cpath,
                                 int number_of_columns_read,
                                 int n_clauses) {
        Path* path = &cpath->path;

        List* all_clauses = postgres_list<List>(cpath->custom_private).get_second();
        Selectivity clause_sel =
            clauselist_selectivity(root, all_clauses, rel->relid, JOIN_INNER, nullptr);

        /*
         * We already filtered out clauses where the overall selectivity would be
         * misleading, such as inequalities involving an uncorrelated column. So
         * we can apply the selectivity directly to the number of stripes.
         */
        double stripes_to_read = clause_sel * bioseq_column_table_stripe_count(relation_id);
        stripes_to_read = std::max(stripes_to_read, 1.0);

        path->rows = rel->tuples;
        path->startup_cost = enable_seqscan ? 0 : disable_cost;
        path->total_cost =
            path->startup_cost + stripes_to_read * bioseq_column_per_stripe_scan_cost(
                                                       rel, relation_id, number_of_columns_read);
    }

    /** @brief Estimates the tablespace-adjusted cost of reading selected columns from one stripe. */
    Cost bioseq_column_per_stripe_scan_cost(RelOptInfo* rel,
                                            Oid relation_id,
                                            int number_of_columns_read) {
        Relation relation = RelationIdGetRelation(relation_id);
        List* stripe_list = list_stripes_for_relation(relation->rd_locator, ForwardScanDirection);
        RelationClose(relation);

        uint32_t max_column_count = 0;
        uint64_t total_stripe_size = 0;
        for (auto stripe_metadata : postgres_list<stripe_metadata>(stripe_list)) {
            total_stripe_size += stripe_metadata->data_length;
            max_column_count = std::max(max_column_count, stripe_metadata->column_count);
        }

        /*
         * When no stripes are in the table we don't have a count in maxColumnCount. To
         * prevent a division by zero turning into a NaN we keep the ratio on zero.
         * This will result in a cost of 0 for scanning the table which is a reasonable
         * cost on an empty table.
         */
        if (max_column_count == 0) {
            return 0;
        }

        double column_selection_ratio =
            number_of_columns_read / static_cast<double>(max_column_count);
        Cost table_scan_cost =
            static_cast<double>(total_stripe_size) / BLCKSZ * column_selection_ratio;
        Cost per_stripe_scan_cost = table_scan_cost / list_length(stripe_list);

        /*
         * Finally, multiply the cost of reading a single stripe by seq page read
         * cost to make our estimation scale compatible with postgres.
         * Since we are calculating the cost for a single stripe here, we use seq
         * page cost instead of random page cost. This is because, random page
         * access only happens when switching between columns, which is pretty
         * much neglactable.
         */
        double rel_space_seq_page_cost;
        get_tablespace_page_costs(rel->reltablespace, nullptr, &rel_space_seq_page_cost);
        per_stripe_scan_cost = per_stripe_scan_cost * rel_space_seq_page_cost;

        return per_stripe_scan_cost;
    }

    /** @brief Returns the number of stored stripes for a columnar relation. */
    uint64_t bioseq_column_table_stripe_count(Oid relation_id) {
        Relation relation = RelationIdGetRelation(relation_id);
        List* stripe_list = list_stripes_for_relation(relation->rd_locator, ForwardScanDirection);
        int stripe_count = list_length(stripe_list);
        RelationClose(relation);

        return stripe_count;
    }

} // export custom-scan partition declarations
