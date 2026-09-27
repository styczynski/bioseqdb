/**
 * @file
 * @brief Finalizes parallel and per-stripe scan costs.
 */
module;
#include <bioseq/pg_compat.h>

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
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;
export import :custom_scan_scan_paths_cost_finalization_contracts;

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

            /* Share total scan cost and rows across the expected participants. */
            path->total_cost /= parallel_divisor;

            path->rows = clamp_row_est(path->rows / parallel_divisor);
        }
    }

    /** @brief Estimates a custom scan's read cost from column count and stripes likely to pass its filters. */
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

        /* Pushdown selection has already removed filters with poor column
         * correlation. Use the remaining selectivity to estimate stripes read. */
        double stripes_to_read = clause_sel * bioseq_column_table_stripe_count(relation_id);
        stripes_to_read = std::max(stripes_to_read, 1.0);

        path->rows = rel->tuples;
        path->startup_cost = enable_seqscan ? 0 : disable_cost;
        path->total_cost =
            path->startup_cost + stripes_to_read * bioseq_column_per_stripe_scan_cost(
                                                       rel, relation_id, number_of_columns_read);
    }

    /** @brief Estimates the cost of reading selected columns from an average stored stripe. */
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

        /* An empty table has no stripe width to divide by and no read cost. */
        if (max_column_count == 0) {
            return 0;
        }

        double column_selection_ratio =
            number_of_columns_read / static_cast<double>(max_column_count);
        Cost table_scan_cost =
            static_cast<double>(total_stripe_size) / BLCKSZ * column_selection_ratio;
        Cost per_stripe_scan_cost = table_scan_cost / list_length(stripe_list);

        /* Use the tablespace's sequential page cost to express stripe reads in
         * PostgreSQL planner units. This estimate treats column reads as sequential. */
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
