/**
 * @file
 * @brief Validates and copies structural custom-scan cache payloads.
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

export module bioseq.postgres.table:custom_scan_semantic_payload_cache_validation;

import std;
import bioseq.postgres.configuration.core;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.type;
import bioseq.postgres.support;

import :custom_scan_plan_abi_constants;
import :custom_scan_runtime_match_contracts;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_oid_expression_support_expression_analysis_contracts;
import :custom_scan_planner_aligner_contracts;
import :custom_scan_planner_method_contracts;
import :custom_scan_semantic_payload_payload_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Return the stable base-scan callback table registered with PostgreSQL. */
    const CustomScanMethods* bioseq_customscan_methods(void) {
        return bioseq_column_scan_scan_methods();
    }

    /** @brief Report whether a base scan contains a decodable semantic snapshot. */
    bool bioseq_customscan_has_valid_semantic_payload(const CustomScan* scan) noexcept {
        if (scan == nullptr || scan->methods != bioseq_column_scan_scan_methods()) {
            return false;
        }
        frozen_scan_semantics semantics{};
        return decode_bioseq_semantic_payload(scan->custom_private, &semantics);
    }

    /** @brief Copy the cache-stable private payload for a valid base scan. */
    List* bioseq_customscan_cache_private_payload(const CustomScan* scan) {
        if (!bioseq_customscan_has_valid_semantic_payload(scan)) {
            return NIL;
        }
        auto* private_payload = static_cast<List*>(copyObject(scan->custom_private));
        List* locator_tag = nullptr;
        for (Node* possible_tag : postgres_list<Node>(private_payload)) {
            if (possible_tag == nullptr || !IsA(possible_tag, List))
                continue;
            auto* tag = reinterpret_cast<List*>(possible_tag);
            if (tag == NIL || !IsA(reinterpret_cast<Node*>(linitial(tag)), Integer) ||
                intVal(linitial(tag)) != k_bioseq_locator_passthrough_magic) {
                continue;
            }
            if (locator_tag != nullptr || list_length(tag) != 4 ||
                !IsA(list_nth(tag, 1), Integer) || !IsA(list_nth(tag, 2), Integer) ||
                !IsA(list_nth(tag, 3), Integer) ||
                intVal(list_nth(tag, 1)) != k_bioseq_batch_join_plan_version ||
                intVal(list_nth(tag, 2)) <= 0 || intVal(list_nth(tag, 3)) <= 0) {
                return NIL;
            }
            locator_tag = tag;
        }
        if (locator_tag != nullptr) {
            /* This token pairs a locator child with its parent only inside one
             * planned statement; the monotonic value is not result semantics. */
            lfirst(list_nth_cell(locator_tag, 2)) = makeInteger(0);
        }
        return private_payload;
    }

    /** @brief Return the stable BatchJoin callback table registered with PostgreSQL. */
    const CustomScanMethods* bioseq_batch_join_customscan_methods(void) {
        return bioseq_batch_join_scan_methods();
    }

    /** @brief Checks that a private-data position contains a PostgreSQL Integer node. */
    [[nodiscard]] bool bioseq_batch_join_private_integer(const List* nodes,
                                                         const int index) noexcept {
        if (nodes == nullptr || index < 0 || index >= list_length(nodes)) {
            return false;
        }
        const Node* node = reinterpret_cast<const Node*>(list_nth(nodes, index));
        return node != nullptr && IsA(node, Integer);
    }

    /** @brief Checks target-list shape and consecutive result numbering. */
    [[nodiscard]] bool
    bioseq_batch_join_target_list_is_structural(const List* target_list) noexcept {
        if (target_list == nullptr)
            return false;
        int expected_resno = 1;
        for (const TargetEntry* entry :
             postgres_list<TargetEntry>(const_cast<List*>(target_list))) {
            if (entry == nullptr || !IsA(entry, TargetEntry) || entry->expr == nullptr ||
                entry->resno != expected_resno++) {
                return false;
            }
        }
        return true;
    }

    /** @brief Validates the serialized match-owner descriptor at a private-data position. */
    [[nodiscard]] bool
    bioseq_batch_join_descriptor_is_structural(const CustomScan* scan,
                                               const int descriptor_index) noexcept {
        if (scan == nullptr || descriptor_index < 0 ||
            descriptor_index >= list_length(scan->custom_private)) {
            return false;
        }
        const Node* descriptor_node =
            reinterpret_cast<const Node*>(list_nth(scan->custom_private, descriptor_index));
        if (descriptor_node == nullptr || !IsA(descriptor_node, List)) {
            return false;
        }
        const auto* descriptor = reinterpret_cast<const List*>(descriptor_node);
        if (list_length(descriptor) != k_bioseq_match_owner_descriptor_fields) {
            return false;
        }
        for (const int index : {k_match_owner_magic,
                                k_match_owner_version,
                                k_match_owner_stable_id,
                                k_match_owner_base_relid}) {
            if (!bioseq_batch_join_private_integer(descriptor, index)) {
                return false;
            }
        }
        const int stable_id = intVal(list_nth(descriptor, k_match_owner_stable_id));
        const Node* target_recipe =
            reinterpret_cast<const Node*>(list_nth(descriptor, k_match_owner_target_recipe));
        const Node* base_lineage =
            reinterpret_cast<const Node*>(list_nth(descriptor, k_match_owner_base_lineage));
        return intVal(list_nth(descriptor, k_match_owner_magic)) ==
                   k_bioseq_match_owner_descriptor_magic &&
               intVal(list_nth(descriptor, k_match_owner_version)) ==
                   k_bioseq_match_owner_descriptor_version &&
               (stable_id == 0 || stable_id == scan->scan.plan.plan_node_id) &&
               target_recipe != nullptr && (base_lineage == nullptr || IsA(base_lineage, List)) &&
               bioseq_is_concrete_rte_varno(
                   static_cast<Index>(intVal(list_nth(descriptor, k_match_owner_base_relid))));
    }

    /** @brief Report whether a BatchJoin contains the complete serialized executor contract. */
    bool bioseq_batch_join_customscan_has_valid_semantic_payload(const CustomScan* scan) noexcept {
        if (scan == nullptr || scan->methods != bioseq_batch_join_scan_methods() ||
            scan->scan.scanrelid != 0 || scan->scan.plan.plan_node_id < 0 ||
            outerPlan(&scan->scan.plan) != nullptr || innerPlan(&scan->scan.plan) != nullptr ||
            scan->custom_exprs != NIL || bms_is_empty(scan->custom_relids) ||
            !bioseq_batch_join_target_list_is_structural(scan->scan.plan.targetlist) ||
            !bioseq_batch_join_target_list_is_structural(scan->custom_scan_tlist) ||
            !bioseq_batch_join_private_integer(scan->custom_private, 0)) {
            return false;
        }
        const int version = intVal(linitial(scan->custom_private));
        /* V4 shares the PostgreSQL method table but has a different one-child
         * executor ABI.  Exact source elision is deliberately V3-only until that
         * ABI gets its own complete recipe contract. */
        if (version != k_bioseq_batch_join_plan_version ||
            list_length(scan->custom_private) != k_bioseq_batch_join_private_fields ||
            list_length(scan->custom_plans) != 2 ||
            !bioseq_batch_join_descriptor_is_structural(scan,
                                                        k_batch_join_match_owner_descriptor)) {
            return false;
        }
        for (const int index : {k_batch_join_version,
                                k_batch_join_token,
                                k_batch_join_target_relation_oid,
                                k_batch_join_target_storage_seq_attno,
                                k_batch_join_target_slot_seq_resno,
                                k_batch_join_target_slot_rowno_resno,
                                k_batch_join_query_slot_seq_resno,
                                k_batch_join_target_column_count,
                                k_batch_join_query_column_count,
                                k_batch_join_alphabet_code,
                                k_batch_join_storage_unit_byte_width,
                                k_batch_join_index_read_requirement}) {
            if (!bioseq_batch_join_private_integer(scan->custom_private, index)) {
                return false;
            }
        }
        const auto* target_child =
            reinterpret_cast<const CustomScan*>(linitial(scan->custom_plans));
        const Plan* query_child = static_cast<const Plan*>(lsecond(scan->custom_plans));
        const Node* sites = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_placeholder_sites));
        const Node* mmsearch = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_mmsearch_expr));
        const Node* honest_startup_cost = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_honest_startup_cost));
        const Node* honest_total_cost = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_honest_total_cost));
        const Node* incumbent_cost_floor = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_incumbent_cost_floor));
        return target_child != nullptr && IsA(target_child, CustomScan) &&
               target_child->methods == bioseq_column_scan_scan_methods() &&
               query_child != nullptr &&
               intVal(list_nth(scan->custom_private, k_batch_join_token)) > 0 &&
               OidIsValid(static_cast<Oid>(
                   intVal(list_nth(scan->custom_private, k_batch_join_target_relation_oid)))) &&
               intVal(list_nth(scan->custom_private, k_batch_join_target_storage_seq_attno)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_target_slot_seq_resno)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_target_slot_rowno_resno)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_query_slot_seq_resno)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_target_column_count)) > 0 &&
               intVal(list_nth(scan->custom_private, k_batch_join_query_column_count)) > 0 &&
               bioseq::indexing::index_read_requirement_ordinal_valid(
                   intVal(list_nth(scan->custom_private, k_batch_join_index_read_requirement))) &&
               (sites == nullptr || IsA(sites, List)) && mmsearch != nullptr &&
               IsA(mmsearch, FuncExpr) && honest_startup_cost != nullptr &&
               IsA(honest_startup_cost, Const) && honest_total_cost != nullptr &&
               IsA(honest_total_cost, Const) && incumbent_cost_floor != nullptr &&
               IsA(incumbent_cost_floor, Const);
    }

    /** @brief Copy result semantics while removing planner-only tokens and costs. */
    List* bioseq_batch_join_cache_plan_payload(const CustomScan* scan) {
        if (!bioseq_batch_join_customscan_has_valid_semantic_payload(scan)) {
            return NIL;
        }
        /* The token pairs one BatchJoin with its locator child only inside one
         * planned statement.  It is deliberately monotonic and therefore cannot
         * participate in a cross-planning cache identity.  Child plan semantics
         * are covered independently by the exact-plan provider walk. */
        auto* private_payload = static_cast<List*>(copyObject(scan->custom_private));
        lfirst(list_nth_cell(private_payload, k_batch_join_token)) = makeInteger(0);
        for (const int cost_index : {k_batch_join_honest_startup_cost,
                                     k_batch_join_honest_total_cost,
                                     k_batch_join_incumbent_cost_floor}) {
            /* Planner costs select a path but do not affect its result. */
            lfirst(list_nth_cell(private_payload, cost_index)) = makeInteger(0);
        }
        return list_make5(makeInteger(static_cast<int>(scan->flags)),
                          private_payload,
                          copyObject(scan->scan.plan.targetlist),
                          copyObject(scan->scan.plan.qual),
                          copyObject(scan->custom_scan_tlist));
    }

    /** @brief Reconstruct the frozen execution parameters for a valid BatchJoin. */
    List* bioseq_batch_join_cache_semantic_snapshot(const CustomScan* scan) {
        if (!bioseq_batch_join_customscan_has_valid_semantic_payload(scan)) {
            return NIL;
        }
        auto* mmsearch =
            reinterpret_cast<FuncExpr*>(list_nth(scan->custom_private, k_batch_join_mmsearch_expr));
        bioseq::simd::minimap2_parameters params = build_params_from_gucs();
        bioseq::indexing::index_read_requirement requirement =
            bioseq::indexing::index_read_requirement::allow;
        (void)apply_aligner_options_override(mmsearch, params, requirement, get_cached_stub_oids());
        finalize_simd_chaining_policy(params);
        if (static_cast<int>(requirement) !=
            intVal(list_nth(scan->custom_private, k_batch_join_index_read_requirement))) {
            return NIL;
        }

        /* Match BeginCustomScan exactly: expensive result columns are enabled by
         * the finalized placeholder ABI, not by the post-setrefs target list. */
        params.do_cigar = false;
        params.compute_divergence = false;
        params.compute_mapq = false;
        params.compute_edit_distance = false;
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        const Node* sites_node = reinterpret_cast<const Node*>(
            list_nth(scan->custom_private, k_batch_join_placeholder_sites));
        const List* sites = sites_node == nullptr ? NIL : reinterpret_cast<const List*>(sites_node);
        Bitmapset* seen_resnos = nullptr;
        for (const List* site : postgres_list<List>(const_cast<List*>(sites))) {
            if (site == nullptr || !IsA(site, List) ||
                list_length(site) != k_bioseq_match_helper_site_fields) {
                bms_free(seen_resnos);
                return NIL;
            }
            for (const int index : {k_match_site_resno,
                                    k_match_site_funcid,
                                    k_match_site_typmod,
                                    k_match_site_collation,
                                    k_match_site_binding_kind,
                                    k_match_site_phid,
                                    k_match_site_stable_id}) {
                if (!bioseq_batch_join_private_integer(site, index)) {
                    bms_free(seen_resnos);
                    return NIL;
                }
            }
            const int resno = intVal(list_nth(site, k_match_site_resno));
            const Oid funcid = static_cast<Oid>(intVal(list_nth(site, k_match_site_funcid)));
            const auto binding_kind = static_cast<match_helper_binding_kind>(
                intVal(list_nth(site, k_match_site_binding_kind)));
            const int stable_id = intVal(list_nth(site, k_match_site_stable_id));
            const Node* canonical_argument =
                reinterpret_cast<const Node*>(list_nth(site, k_match_site_canonical_argument));
            if (resno <= 0 || resno > list_length(scan->custom_scan_tlist) ||
                bms_is_member(resno, seen_resnos) || !is_bioseq_projection_funcid(funcid, oids) ||
                canonical_argument == nullptr ||
                (binding_kind != match_helper_binding_kind::exact_recipe &&
                 binding_kind != match_helper_binding_kind::unique_lineage) ||
                (stable_id != 0 && stable_id != scan->scan.plan.plan_node_id)) {
                bms_free(seen_resnos);
                return NIL;
            }
            seen_resnos = bms_add_member(seen_resnos, resno);
            params.do_cigar = params.do_cigar || funcid == oids.cigar_oid ||
                              funcid == oids.has_cigar_oid || funcid == oids.edit_oid;
            params.compute_edit_distance = params.compute_edit_distance || funcid == oids.edit_oid;
            params.compute_divergence = params.compute_divergence || funcid == oids.divergence_oid;
            params.compute_mapq = params.compute_mapq || funcid == oids.mapq_oid;
        }
        bms_free(seen_resnos);
        return make_bioseq_semantic_payload(frozen_scan_semantics{
            .params = params,
            .read_sequence_index = bioseq::settings::index::sequence_read::enabled,
            .index_read_requirement = requirement,
        });
    }

    /** @brief Tracks whether an expression contains a Bioseq projection or mmsearch call. */
    struct projection_presence_probe {
        const bioseq_stub_oids* stub_oids;
        bool found;
    };

    /** @brief Stops an expression walk after finding a scan-time Bioseq call. */
    struct projection_presence_visitor {
        /** @brief Examines one expression node for a projection, mmsearch, or symbol-count call. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              projection_presence_probe& data) const {
            if (data.found)
                return pg_walk_step::stop;
            if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                if (is_bioseq_projection_funcid(fe->funcid, *data.stub_oids) ||
                    is_mmsearch_funcid(fe->funcid, *data.stub_oids) ||
                    fe->funcid == data.stub_oids->symbol_count_oid) {
                    data.found = true;
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Report whether an expression depends on Bioseq scan-time projection state.
     *
     * True if any node in the expression tree references a bioseq.<...>
     * projection FuncExpr (cigar / score / target_*, or symbol_count) or
     * an mmsearch FuncExpr. The vectorised aggregator rewrite path uses
     * this to skip its rewrite when the agg input depends on any of
     * these — the projection FuncExprs are scan-time-rewritten into
     * placeholder Consts that the regular Postgres Aggregate node can
     * evaluate, but the vectorised aggregator expects vector-typed
     * scan output and crashes with a SIGSEGV when fed a regular slot
     * carrying our placeholder Consts.
     */
    bool bioseq_expr_uses_bioseq_projection(Node* node) {
        if (node == nullptr) {
            return false;
        }
        const bioseq_stub_oids* stub_oids = try_get_cached_stub_oids();
        if (stub_oids == nullptr)
            return false;
        projection_presence_probe probe{stub_oids, false};
        projection_presence_visitor visitor;
        walk_pg_expression_including_root(node, probe, visitor);
        return probe.found;
    }

} // export custom-scan partition declarations
