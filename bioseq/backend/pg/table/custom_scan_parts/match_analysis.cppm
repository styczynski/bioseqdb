/**
 * @file
 * @brief Validates finalized helper carriers and reparameterized custom-path payloads.
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

export module bioseq.postgres.table:custom_scan_match_analysis;

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
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;

import :custom_scan_match_finalization;

using namespace bioseq::postgres::support;

export {

    [[nodiscard]] int validate_finalized_match_owner_descriptor_value(CustomScan* scan,
                                                                      List* descriptor) {
        if (scan == nullptr || descriptor == NIL) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq finalized match owner descriptor is missing")));
        }
        const int stable_id =
            intVal(match_binding_integer(descriptor, k_match_owner_stable_id, "owner.stable_id"));
        Node* recipe =
            list_length(descriptor) == k_bioseq_match_owner_descriptor_fields
                ? reinterpret_cast<Node*>(list_nth(descriptor, k_match_owner_target_recipe))
                : nullptr;
        Node* lineage =
            list_length(descriptor) == k_bioseq_match_owner_descriptor_fields
                ? reinterpret_cast<Node*>(list_nth(descriptor, k_match_owner_base_lineage))
                : nullptr;
        if (list_length(descriptor) != k_bioseq_match_owner_descriptor_fields ||
            intVal(match_binding_integer(descriptor, k_match_owner_magic, "owner.magic")) !=
                k_bioseq_match_owner_descriptor_magic ||
            intVal(match_binding_integer(descriptor, k_match_owner_version, "owner.version")) !=
                k_bioseq_match_owner_descriptor_version ||
            stable_id < 0 || stable_id != scan->scan.plan.plan_node_id || recipe == nullptr ||
            (lineage != nullptr && !IsA(lineage, List)) ||
            !bioseq_is_concrete_rte_varno(static_cast<Index>(intVal(match_binding_integer(
                descriptor, k_match_owner_base_relid, "owner.base_relid"))))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("Bioseq finalized match owner descriptor is invalid")));
        }
        return stable_id;
    }

    [[nodiscard]] int validate_finalized_match_owner_descriptor(CustomScan* scan,
                                                                const int descriptor_index) {
        Node* descriptor_node =
            scan == nullptr || descriptor_index < 0 ||
                    descriptor_index >= list_length(scan->custom_private)
                ? nullptr
                : reinterpret_cast<Node*>(list_nth(scan->custom_private, descriptor_index));
        if (descriptor_node == nullptr || !IsA(descriptor_node, List)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq finalized match owner descriptor is missing")));
        }
        return validate_finalized_match_owner_descriptor_value(
            scan, reinterpret_cast<List*>(descriptor_node));
    }

    [[nodiscard]] Node* initialize_base_match_owner_runtime(CustomScan* scan,
                                                            Relation relation,
                                                            bioseq_scan_runtime& runtime) {
        List* owner_tag = find_base_match_owner_plan_tag(scan);
        if (owner_tag == NIL)
            return nullptr;
        if (relation == nullptr ||
            RelationGetRelid(relation) !=
                static_cast<Oid>(intVal(match_binding_integer(
                    owner_tag, k_base_match_plan_relation_oid, "owner.target_relation_oid")))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan match owner relation identity is invalid")));
        }
        Node* descriptor_node =
            reinterpret_cast<Node*>(list_nth(owner_tag, k_base_match_plan_executor_descriptor));
        Node* sites_node =
            reinterpret_cast<Node*>(list_nth(owner_tag, k_base_match_plan_executor_sites));
        if (descriptor_node == nullptr || !IsA(descriptor_node, List) ||
            (sites_node != nullptr && !IsA(sites_node, List))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan finalized executor match owner image is missing")));
        }
        List* descriptor = reinterpret_cast<List*>(descriptor_node);
        runtime.stable_match_id = validate_finalized_match_owner_descriptor_value(scan, descriptor);
        Node* target_recipe =
            reinterpret_cast<Node*>(list_nth(descriptor, k_match_owner_target_recipe));
        TupleDesc relation_descriptor = RelationGetDescr(relation);
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        List* target_lineage = canonical_match_base_lineage(target_recipe, oids);
        Var* primary_source =
            target_lineage == NIL ? nullptr : reinterpret_cast<Var*>(linitial(target_lineage));
        bool lineage_valid = primary_source != nullptr;
        for (Var* source : postgres_list<Var>(target_lineage)) {
            Form_pg_attribute attribute =
                source != nullptr && source->varattno > 0 &&
                        source->varattno <= relation_descriptor->natts
                    ? TupleDescAttr(relation_descriptor, source->varattno - 1)
                    : nullptr;
            if (source == nullptr || source->varno != scan->scan.scanrelid ||
                source->varattno <= 0 || source->varlevelsup != 0 ||
                !bms_is_empty(source->varnullingrels) ||
                source->vartype != oids.bioseq_seq_type_oid || attribute == nullptr ||
                attribute->attisdropped || attribute->atttypid != source->vartype ||
                attribute->atttypmod != source->vartypmod ||
                attribute->attcollation != source->varcollid) {
                lineage_valid = false;
                break;
            }
        }
        const base_local_sequence_typmod_resolution target_type =
            resolve_base_local_sequence_typmod(target_recipe, oids);
        if (target_recipe == nullptr || exprType(target_recipe) != oids.bioseq_seq_type_oid ||
            !lineage_valid || target_type.invalid || target_type.typmod < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan match owner target recipe is invalid")));
        }
        runtime.match_target_recipe = target_recipe;
        runtime.match_target_lineage = target_lineage;
        runtime.match_target_typmod = target_type.typmod;
        runtime.match_target_evaluated = try_cast_pg_node<Var>(target_recipe) == nullptr;
        runtime.match_sequence_varno = primary_source->varno;
        runtime.match_sequence_attno = primary_source->varattno;
        runtime.match_helper_sites =
            sites_node == nullptr ? NIL : reinterpret_cast<List*>(sites_node);
        for (List* site : postgres_list<List>(runtime.match_helper_sites)) {
            if (site == nullptr || !IsA(site, List) ||
                list_length(site) != k_bioseq_match_helper_site_fields) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqScan match helper site ABI is invalid")));
            }
            for (const int integer_index : {k_match_site_resno,
                                            k_match_site_funcid,
                                            k_match_site_typmod,
                                            k_match_site_collation,
                                            k_match_site_binding_kind,
                                            k_match_site_phid,
                                            k_match_site_stable_id}) {
                (void)match_binding_integer(site, integer_index, "helper.site_integer");
            }
            const int resno = intVal(list_nth(site, k_match_site_resno));
            const Oid funcid = static_cast<Oid>(intVal(list_nth(site, k_match_site_funcid)));
            const auto binding_kind = static_cast<match_helper_binding_kind>(
                intVal(list_nth(site, k_match_site_binding_kind)));
            Node* argument_node =
                reinterpret_cast<Node*>(list_nth(site, k_match_site_canonical_argument));
            Var* argument = try_cast_pg_node<Var>(argument_node);
            const bool exact_sequence_argument =
                argument_node != nullptr && exprType(argument_node) == oids.bioseq_seq_type_oid &&
                equal(argument_node, target_recipe) &&
                binding_kind == match_helper_binding_kind::exact_recipe;
            bool base_lineage_argument = argument != nullptr &&
                                         argument->varno == scan->scan.scanrelid &&
                                         argument->varattno > 0 && argument->varlevelsup == 0 &&
                                         bms_is_empty(argument->varnullingrels) &&
                                         binding_kind == match_helper_binding_kind::unique_lineage;
            if (base_lineage_argument && argument->vartype == oids.bioseq_seq_type_oid) {
                base_lineage_argument = false;
                for (Var* source : postgres_list<Var>(target_lineage)) {
                    if (equal(source, argument)) {
                        base_lineage_argument = true;
                        break;
                    }
                }
            }
            if (resno <= 0 || resno > list_length(scan->scan.plan.targetlist) ||
                !is_bioseq_projection_funcid(funcid, oids) ||
                get_func_rettype(funcid) == InvalidOid ||
                (!exact_sequence_argument && !base_lineage_argument) ||
                intVal(list_nth(site, k_match_site_phid)) != 0 ||
                intVal(list_nth(site, k_match_site_stable_id)) != runtime.stable_match_id) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqScan finalized match helper site is invalid")));
            }
        }
        return target_recipe;
    }

    [[nodiscard]] TargetEntry* finalized_match_helper_resno_entry(List* targetlist,
                                                                  const AttrNumber resno) noexcept {
        if (resno <= 0)
            return nullptr;
        for (TargetEntry* entry : postgres_list<TargetEntry>(targetlist)) {
            if (entry != nullptr && entry->resno == resno)
                return entry;
        }
        return nullptr;
    }

    [[nodiscard]] bool finalized_match_helper_argument_translation_agrees_impl(
        Plan* context, Node* translated, Node* canonical, const unsigned int depth) noexcept {
        if (depth > 16u)
            return false;
        if (translated == nullptr || canonical == nullptr)
            return false;
        Var* translated_var = try_cast_pg_node<Var>(translated);
        Var* canonical_var = try_cast_pg_node<Var>(canonical);
        if (equal(translated, canonical)) {
            return canonical_var == nullptr || bioseq_is_concrete_rte_varno(canonical_var->varno);
        }
        if (translated_var == nullptr || canonical_var == nullptr ||
            translated_var->varattno <= 0 || canonical_var->varattno <= 0 ||
            translated_var->varlevelsup != canonical_var->varlevelsup ||
            !bms_is_empty(translated_var->varnullingrels) ||
            !bms_is_empty(canonical_var->varnullingrels) ||
            translated_var->vartype != canonical_var->vartype ||
            translated_var->vartypmod != canonical_var->vartypmod ||
            translated_var->varcollid != canonical_var->varcollid)
            return false;
        if (!bioseq_is_concrete_rte_varno(canonical_var->varno))
            return false;
        /* A concrete Var can only differ by the statement rtable offset and must
         * retain its physical attribute identity. */
        if (bioseq_is_concrete_rte_varno(translated_var->varno)) {
            return translated_var->varattno == canonical_var->varattno;
        }
        if (context == nullptr)
            return false;

        List* source_tlist = NIL;
        Plan* source_context = context;
        if (translated_var->varno == INDEX_VAR) {
            if (!IsA(context, CustomScan))
                return false;
            source_tlist = reinterpret_cast<CustomScan*>(context)->custom_scan_tlist;
        } else {
            Plan* child = nullptr;
            if (IsA(context, CustomScan)) {
                List* children = reinterpret_cast<CustomScan*>(context)->custom_plans;
                const int child_index = translated_var->varno == OUTER_VAR
                                            ? 0
                                            : (translated_var->varno == INNER_VAR ? 1 : -1);
                child = child_index >= 0 && child_index < list_length(children)
                            ? reinterpret_cast<Plan*>(list_nth(children, child_index))
                            : nullptr;
            } else if (translated_var->varno == OUTER_VAR) {
                child = outerPlan(context);
            } else if (translated_var->varno == INNER_VAR) {
                child = innerPlan(context);
            } else {
                return false;
            }
            if (child == nullptr)
                return false;
            source_context = child;
            source_tlist = child->targetlist;
        }
        TargetEntry* source =
            finalized_match_helper_resno_entry(source_tlist, translated_var->varattno);
        Node* source_expression =
            source == nullptr ? nullptr : reinterpret_cast<Node*>(source->expr);
        if (source_expression == nullptr ||
            exprType(source_expression) != translated_var->vartype ||
            exprTypmod(source_expression) != translated_var->vartypmod ||
            exprCollation(source_expression) != translated_var->varcollid ||
            equal(source_expression, translated))
            return false;
        /* Resolve the exact setrefs slot rather than accepting its type metadata as
         * provenance.  A nested special Var is followed only through the concrete
         * child/scan tlist which owns that slot; cycles and unsupported INDEX_VAR
         * namespaces fail closed. */
        return finalized_match_helper_argument_translation_agrees_impl(
            source_context, source_expression, canonical, depth + 1u);
    }

    [[nodiscard]] bool finalized_match_helper_argument_translation_agrees(
        CustomScan* scan, Node* translated, Node* canonical) noexcept {
        return finalized_match_helper_argument_translation_agrees_impl(
            scan == nullptr ? nullptr : &scan->scan.plan, translated, canonical, 0u);
    }

    [[nodiscard]] constexpr bool
    finalized_match_helper_carrier_shape_supported(const int site_phid,
                                                   const bool supplied_is_phv,
                                                   const int supplied_phid,
                                                   const bool supplied_is_nulling_phv,
                                                   const bool carrier_is_phv,
                                                   const bool identities_equal) noexcept {
        if (site_phid < 0 || carrier_is_phv)
            return false;
        if (site_phid == 0)
            return !supplied_is_phv && identities_equal;
        if (!supplied_is_phv)
            return true; /* setrefs stripped the encoded PHV */
        return !supplied_is_nulling_phv && supplied_phid == site_phid;
    }

    static_assert(finalized_match_helper_carrier_shape_supported(0, false, 0, false, false, true));
    static_assert(
        !finalized_match_helper_carrier_shape_supported(0, false, 0, false, false, false));
    static_assert(finalized_match_helper_carrier_shape_supported(7, true, 7, false, false, false));
    static_assert(!finalized_match_helper_carrier_shape_supported(7, true, 8, false, false, false));
    static_assert(!finalized_match_helper_carrier_shape_supported(7, true, 7, true, false, false));
    static_assert(finalized_match_helper_carrier_shape_supported(7, false, 0, false, false, false));
    static_assert(!finalized_match_helper_carrier_shape_supported(7, false, 0, false, true, true));

    /* custom_scan_tlist, custom_private, and a descendant dummy targetlist cross
     * different setrefs coordinate domains.  A phid-zero site accepts only the
     * exact bare identity.  A positive phid accepts either its live non-nulling
     * owner PHV or the bare descriptor produced when setrefs strips that PHV;
     * both require the leaf carrier's exact canonical argument and full function
     * ABI.  A carrier-side PHV is never a dummy leaf carrier. */
    [[nodiscard]] bool
    finalized_match_helper_carrier_equivalent(CustomScan* scan,
                                              Node* supplied_identity,
                                              Node* carrier_identity,
                                              Node* canonical_argument,
                                              const int site_phid,
                                              const bioseq_stub_oids& oids) noexcept {
        if (scan == nullptr || supplied_identity == nullptr || carrier_identity == nullptr ||
            canonical_argument == nullptr)
            return false;
        PlaceHolderVar* supplied_phv = try_cast_pg_node<PlaceHolderVar>(supplied_identity);
        PlaceHolderVar* carrier_phv = try_cast_pg_node<PlaceHolderVar>(carrier_identity);
        const bool identities_equal = equal(supplied_identity, carrier_identity);
        if (!finalized_match_helper_carrier_shape_supported(
                site_phid,
                supplied_phv != nullptr,
                supplied_phv == nullptr ? 0 : static_cast<int>(supplied_phv->phid),
                supplied_phv != nullptr && !bms_is_empty(supplied_phv->phnullingrels),
                carrier_phv != nullptr,
                identities_equal))
            return false;
        FuncExpr* supplied = supplied_phv == nullptr
                                 ? batch_join_well_formed_match_helper(supplied_identity, oids)
                                 : batch_join_match_helper_phv_function(supplied_phv, oids);
        FuncExpr* carrier = carrier_phv == nullptr
                                ? batch_join_well_formed_match_helper(carrier_identity, oids)
                                : batch_join_match_helper_phv_function(carrier_phv, oids);
        if (supplied == nullptr || carrier == nullptr)
            return false;
        Node* supplied_argument = batch_join_match_helper_argument(supplied);
        Node* carrier_argument = batch_join_match_helper_argument(carrier);
        if (supplied_argument == nullptr || carrier_argument == nullptr)
            return false;
        const bool metadata_agrees =
            supplied->funcid == carrier->funcid &&
            supplied->funcresulttype == carrier->funcresulttype &&
            supplied->funcretset == carrier->funcretset &&
            supplied->funcvariadic == carrier->funcvariadic &&
            supplied->funcformat == carrier->funcformat &&
            supplied->funccollid == carrier->funccollid &&
            supplied->inputcollid == carrier->inputcollid &&
            exprTypmod(supplied_identity) == exprTypmod(carrier_identity) &&
            exprCollation(supplied_identity) == exprCollation(carrier_identity);
        if (!metadata_agrees)
            return false;
        if (site_phid == 0)
            return identities_equal;
        return equal(carrier_argument, canonical_argument) &&
               finalized_match_helper_argument_translation_agrees(
                   scan, supplied_argument, canonical_argument);
    }

    struct finalized_match_helper_carrier_audit {
        bool inspected = false;
        int early_reject = 0;
        int version = -1;
        int private_fields = -1;
        int expected_fields = -1;
        int descriptor_tag = -1;
        int sites_tag = -1;
        int site_count = -1;
        int supplied_tag = -1;
        int carrier_tag = -1;
        int supplied_argument_tag = -1;
        int carrier_argument_tag = -1;
        int canonical_argument_tag = -1;
        int site_resno = -1;
        int site_phid = -1;
        int supplied_phid = 0;
        int carrier_phid = 0;
        bool identity_equal = false;
        bool supplied_argument_canonical = false;
        bool carrier_argument_canonical = false;
        bool equivalent = false;
    };

    [[nodiscard]] std::size_t finalized_batch_owner_matching_site_count(
        CustomScan* scan,
        Node* identity,
        const bioseq_stub_oids& oids,
        finalized_match_helper_carrier_audit* audit = nullptr) {
        if (audit != nullptr) {
            audit->private_fields = scan == nullptr ? -1 : list_length(scan->custom_private);
            audit->carrier_tag = identity == nullptr ? -1 : static_cast<int>(nodeTag(identity));
        }
        if (scan == nullptr || identity == nullptr || scan->methods == nullptr ||
            scan->methods->CustomName == nullptr ||
            strcmp(scan->methods->CustomName, "BioseqBatchJoin") != 0 ||
            scan->custom_private == NIL) {
            if (audit != nullptr)
                audit->early_reject = 1;
            return 0u;
        }
        Node* version_node = reinterpret_cast<Node*>(linitial(scan->custom_private));
        if (version_node == nullptr || !IsA(version_node, Integer)) {
            if (audit != nullptr)
                audit->early_reject = 2;
            return 0u;
        }
        const int version = intVal(version_node);
        const int expected_fields = version == k_bioseq_batch_join_plan_version
                                        ? k_bioseq_batch_join_private_fields
                                        : (version == k_bioseq_per_row_join_plan_version
                                               ? k_bioseq_per_row_join_private_fields
                                               : -1);
        const int descriptor_index =
            version == k_bioseq_batch_join_plan_version
                ? k_batch_join_match_owner_descriptor
                : (version == k_bioseq_per_row_join_plan_version ? k_per_row_match_owner_descriptor
                                                                 : -1);
        const int sites_index =
            version == k_bioseq_batch_join_plan_version
                ? k_batch_join_placeholder_sites
                : (version == k_bioseq_per_row_join_plan_version ? k_per_row_placeholder_sites
                                                                 : -1);
        if (audit != nullptr) {
            audit->version = version;
            audit->expected_fields = expected_fields;
        }
        if (expected_fields < 0 || list_length(scan->custom_private) != expected_fields ||
            descriptor_index < 0 || sites_index < 0) {
            if (audit != nullptr)
                audit->early_reject = 3;
            return 0u;
        }
        Node* descriptor_node =
            reinterpret_cast<Node*>(list_nth(scan->custom_private, descriptor_index));
        Node* sites_node = reinterpret_cast<Node*>(list_nth(scan->custom_private, sites_index));
        if (audit != nullptr) {
            audit->descriptor_tag =
                descriptor_node == nullptr ? -1 : static_cast<int>(nodeTag(descriptor_node));
            audit->sites_tag = sites_node == nullptr ? -1 : static_cast<int>(nodeTag(sites_node));
        }
        if (descriptor_node == nullptr || !IsA(descriptor_node, List)) {
            if (audit != nullptr)
                audit->early_reject = 4;
            return 0u;
        }
        if (sites_node == nullptr || !IsA(sites_node, List)) {
            if (audit != nullptr)
                audit->early_reject = 5;
            return 0u;
        }
        if (audit != nullptr) {
            audit->site_count = list_length(reinterpret_cast<List*>(sites_node));
        }
        const int stable_match_id = validate_finalized_match_owner_descriptor_value(
            scan, reinterpret_cast<List*>(descriptor_node));
        PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
        FuncExpr* helper = placeholder == nullptr
                               ? batch_join_well_formed_match_helper(identity, oids)
                               : batch_join_match_helper_phv_function(placeholder, oids);
        if (helper == nullptr || exprType(identity) != helper->funcresulttype ||
            helper->funcresulttype != get_func_rettype(helper->funcid)) {
            if (audit != nullptr)
                audit->early_reject = 6;
            return 0u;
        }
        Node* helper_argument = batch_join_match_helper_argument(helper);
        std::size_t matches = 0u;
        for (Node* raw_site : postgres_list<Node>(reinterpret_cast<List*>(sites_node))) {
            List* site = raw_site != nullptr && IsA(raw_site, List)
                             ? reinterpret_cast<List*>(raw_site)
                             : NIL;
            if (list_length(site) != k_bioseq_match_helper_site_fields)
                return 2u;
            bool integers_valid = true;
            for (const int index : {k_match_site_resno,
                                    k_match_site_funcid,
                                    k_match_site_typmod,
                                    k_match_site_collation,
                                    k_match_site_binding_kind,
                                    k_match_site_phid,
                                    k_match_site_stable_id}) {
                Node* value = reinterpret_cast<Node*>(list_nth(site, index));
                integers_valid = integers_valid && value != nullptr && IsA(value, Integer);
            }
            Node* canonical_argument =
                reinterpret_cast<Node*>(list_nth(site, k_match_site_canonical_argument));
            if (!integers_valid || canonical_argument == nullptr || helper_argument == nullptr)
                return 2u;
            const int resno = intVal(list_nth(site, k_match_site_resno));
            Node* raw_supplied =
                resno <= 0 || resno > list_length(scan->custom_scan_tlist)
                    ? nullptr
                    : reinterpret_cast<Node*>(list_nth(scan->custom_scan_tlist, resno - 1));
            TargetEntry* supplied = raw_supplied != nullptr && IsA(raw_supplied, TargetEntry)
                                        ? reinterpret_cast<TargetEntry*>(raw_supplied)
                                        : nullptr;
            PlaceHolderVar* supplied_placeholder =
                supplied == nullptr
                    ? nullptr
                    : try_cast_pg_node<PlaceHolderVar>(reinterpret_cast<Node*>(supplied->expr));
            PlaceHolderVar* carrier_placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            const int supplied_phid =
                supplied_placeholder == nullptr ? 0 : static_cast<int>(supplied_placeholder->phid);
            const int carrier_phid =
                carrier_placeholder == nullptr ? 0 : static_cast<int>(carrier_placeholder->phid);
            const int site_phid = intVal(list_nth(site, k_match_site_phid));
            const auto binding_kind = static_cast<match_helper_binding_kind>(
                intVal(list_nth(site, k_match_site_binding_kind)));
            FuncExpr* supplied_helper =
                supplied == nullptr
                    ? nullptr
                    : (supplied_placeholder == nullptr
                           ? batch_join_well_formed_match_helper(
                                 reinterpret_cast<Node*>(supplied->expr), oids)
                           : batch_join_match_helper_phv_function(supplied_placeholder, oids));
            Node* supplied_argument = batch_join_match_helper_argument(supplied_helper);
            const bool equivalent =
                supplied != nullptr &&
                finalized_match_helper_carrier_equivalent(scan,
                                                          reinterpret_cast<Node*>(supplied->expr),
                                                          identity,
                                                          canonical_argument,
                                                          site_phid,
                                                          oids);
            if (audit != nullptr && !audit->inspected) {
                audit->inspected = true;
                audit->supplied_tag = supplied == nullptr || supplied->expr == nullptr
                                          ? -1
                                          : static_cast<int>(nodeTag(supplied->expr));
                audit->carrier_tag = static_cast<int>(nodeTag(identity));
                audit->supplied_argument_tag = supplied_argument == nullptr
                                                   ? -1
                                                   : static_cast<int>(nodeTag(supplied_argument));
                audit->carrier_argument_tag =
                    helper_argument == nullptr ? -1 : static_cast<int>(nodeTag(helper_argument));
                audit->canonical_argument_tag = static_cast<int>(nodeTag(canonical_argument));
                audit->site_resno = resno;
                audit->site_phid = site_phid;
                audit->supplied_phid = supplied_phid;
                audit->carrier_phid = carrier_phid;
                audit->identity_equal = supplied != nullptr && equal(supplied->expr, identity);
                audit->supplied_argument_canonical =
                    supplied_argument != nullptr && equal(supplied_argument, canonical_argument);
                audit->carrier_argument_canonical = equal(helper_argument, canonical_argument);
                audit->equivalent = equivalent;
            }
            /* custom_private retains planner coordinates while custom_scan_tlist and
             * the carrier identity are post-setrefs.  Stable site ownership plus the
             * exact supplied expression is the cross-domain identity; comparing their
             * argument Vars directly would reject valid rtable-offset plans. */
            if (supplied != nullptr && supplied->resno == resno && equivalent &&
                static_cast<Oid>(intVal(list_nth(site, k_match_site_funcid))) == helper->funcid &&
                intVal(list_nth(site, k_match_site_typmod)) == exprTypmod(identity) &&
                static_cast<Oid>(intVal(list_nth(site, k_match_site_collation))) ==
                    exprCollation(identity) &&
                (binding_kind == match_helper_binding_kind::exact_recipe ||
                 binding_kind == match_helper_binding_kind::unique_lineage) &&
                site_phid >= 0 &&
                intVal(list_nth(site, k_match_site_stable_id)) == stable_match_id) {
                matches = std::min<std::size_t>(2u, matches + 1u);
            }
        }
        return matches;
    }

    struct finalized_base_owner_site_context {
        List* sites = NIL;
        Node* target_recipe = nullptr;
        List* target_lineage = NIL;
        Index target_relid = 0;
        int stable_match_id = 0;
    };

    [[nodiscard]] bool
    try_resolve_finalized_base_owner_site_context(CustomScan* scan,
                                                  const bioseq_stub_oids& oids,
                                                  finalized_base_owner_site_context& context) {
        context = {};
        if (scan == nullptr || scan->methods != bioseq_column_scan_scan_methods()) {
            return false;
        }
        List* owner_tag = find_base_match_owner_plan_tag(scan);
        Node* descriptor_node =
            owner_tag == NIL ? nullptr
                             : reinterpret_cast<Node*>(
                                   list_nth(owner_tag, k_base_match_plan_executor_descriptor));
        Node* sites_node =
            owner_tag == NIL
                ? nullptr
                : reinterpret_cast<Node*>(list_nth(owner_tag, k_base_match_plan_executor_sites));
        if (descriptor_node == nullptr || !IsA(descriptor_node, List) || sites_node == nullptr ||
            !IsA(sites_node, List)) {
            return false;
        }
        List* descriptor = reinterpret_cast<List*>(descriptor_node);
        const int stable_match_id =
            validate_finalized_match_owner_descriptor_value(scan, descriptor);
        Node* target_recipe =
            reinterpret_cast<Node*>(list_nth(descriptor, k_match_owner_target_recipe));
        List* target_lineage = canonical_match_base_lineage(target_recipe, oids);
        bool valid_lineage = target_lineage != NIL;
        for (Var* source : postgres_list<Var>(target_lineage)) {
            if (source == nullptr || source->varno != scan->scan.scanrelid ||
                source->varattno <= 0 || source->varlevelsup != 0 ||
                !bms_is_empty(source->varnullingrels) ||
                source->vartype != oids.bioseq_seq_type_oid) {
                valid_lineage = false;
                break;
            }
        }
        if (target_recipe == nullptr || exprType(target_recipe) != oids.bioseq_seq_type_oid ||
            !valid_lineage) {
            return false;
        }
        context = finalized_base_owner_site_context{reinterpret_cast<List*>(sites_node),
                                                    target_recipe,
                                                    target_lineage,
                                                    scan->scan.scanrelid,
                                                    stable_match_id};
        return true;
    }

    [[nodiscard]] bool
    finalized_base_owner_site_matches(List* site,
                                      FuncExpr* helper,
                                      AttrNumber resno,
                                      const finalized_base_owner_site_context& context,
                                      const bioseq_stub_oids& oids,
                                      bool& malformed) noexcept {
        if (site == nullptr || helper == nullptr ||
            list_length(site) != k_bioseq_match_helper_site_fields) {
            malformed = true;
            return false;
        }
        for (const int index : {k_match_site_resno,
                                k_match_site_funcid,
                                k_match_site_typmod,
                                k_match_site_collation,
                                k_match_site_binding_kind,
                                k_match_site_phid,
                                k_match_site_stable_id}) {
            Node* value = reinterpret_cast<Node*>(list_nth(site, index));
            if (value == nullptr || !IsA(value, Integer)) {
                malformed = true;
                return false;
            }
        }
        Node* canonical_argument =
            reinterpret_cast<Node*>(list_nth(site, k_match_site_canonical_argument));
        Node* helper_argument = batch_join_match_helper_argument(helper);
        Var* argument = try_cast_pg_node<Var>(canonical_argument);
        const auto binding_kind = static_cast<match_helper_binding_kind>(
            intVal(list_nth(site, k_match_site_binding_kind)));
        const bool exact_sequence_argument =
            canonical_argument != nullptr &&
            exprType(canonical_argument) == oids.bioseq_seq_type_oid &&
            equal(canonical_argument, context.target_recipe) &&
            binding_kind == match_helper_binding_kind::exact_recipe;
        bool base_lineage_argument = argument != nullptr &&
                                     argument->varno == context.target_relid &&
                                     argument->varattno > 0 && argument->varlevelsup == 0 &&
                                     bms_is_empty(argument->varnullingrels) &&
                                     binding_kind == match_helper_binding_kind::unique_lineage;
        if (base_lineage_argument && argument->vartype == oids.bioseq_seq_type_oid) {
            base_lineage_argument = false;
            for (Var* source : postgres_list<Var>(context.target_lineage)) {
                if (equal(source, argument)) {
                    base_lineage_argument = true;
                    break;
                }
            }
        }
        return helper_argument != nullptr && canonical_argument != nullptr &&
               equal(canonical_argument, helper_argument) &&
               intVal(list_nth(site, k_match_site_resno)) == resno &&
               static_cast<Oid>(intVal(list_nth(site, k_match_site_funcid))) == helper->funcid &&
               intVal(list_nth(site, k_match_site_typmod)) ==
                   exprTypmod(reinterpret_cast<Node*>(helper)) &&
               static_cast<Oid>(intVal(list_nth(site, k_match_site_collation))) ==
                   exprCollation(reinterpret_cast<Node*>(helper)) &&
               intVal(list_nth(site, k_match_site_phid)) == 0 &&
               intVal(list_nth(site, k_match_site_stable_id)) == context.stable_match_id &&
               (exact_sequence_argument || base_lineage_argument);
    }

    struct finalized_base_targetlist_audit {
        finalized_base_owner_site_context context{};
        const bioseq_stub_oids* oids = nullptr;
        AttrNumber resno = InvalidAttrNumber;
        Bitmapset** matched_sites = nullptr;
        bool invalid = false;
    };

    struct finalized_base_targetlist_audit_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              finalized_base_targetlist_audit& audit) const {
            FuncExpr* function = node.try_cast<FuncExpr>();
            if (function == nullptr || audit.oids == nullptr ||
                !is_bioseq_projection_funcid(function->funcid, *audit.oids)) {
                return pg_walk_step::descend;
            }
            FuncExpr* helper =
                batch_join_well_formed_match_helper(reinterpret_cast<Node*>(function), *audit.oids);
            if (helper == nullptr || audit.matched_sites == nullptr) {
                audit.invalid = true;
                return pg_walk_step::stop;
            }
            std::size_t matches = 0u;
            std::size_t matched_index = 0u;
            for (int index = 0; index < list_length(audit.context.sites); ++index) {
                Node* raw_site = reinterpret_cast<Node*>(list_nth(audit.context.sites, index));
                List* site = raw_site != nullptr && IsA(raw_site, List)
                                 ? reinterpret_cast<List*>(raw_site)
                                 : NIL;
                bool malformed = false;
                if (finalized_base_owner_site_matches(
                        site, helper, audit.resno, audit.context, *audit.oids, malformed)) {
                    ++matches;
                    matched_index = static_cast<std::size_t>(index);
                }
                if (malformed) {
                    audit.invalid = true;
                    return pg_walk_step::stop;
                }
            }
            if (matches != 1u) {
                audit.invalid = true;
                return pg_walk_step::stop;
            }
            *audit.matched_sites =
                bms_add_member(*audit.matched_sites, static_cast<int>(matched_index));
            return pg_walk_step::skip_subtree;
        }
    };

    [[nodiscard]] bool
    base_scan_targetlist_has_only_finalized_match_helpers(CustomScan* scan,
                                                          const bioseq_stub_oids& oids) {
        finalized_base_owner_site_context context{};
        if (!try_resolve_finalized_base_owner_site_context(scan, oids, context) ||
            context.sites == NIL)
            return false;
        Bitmapset* matched_sites = nullptr;
        finalized_base_targetlist_audit audit{
            context, &oids, InvalidAttrNumber, &matched_sites, false};
        finalized_base_targetlist_audit_visitor visitor;
        for (TargetEntry* entry : postgres_list<TargetEntry>(scan->scan.plan.targetlist)) {
            if (entry == nullptr || entry->expr == nullptr)
                continue;
            audit.resno = entry->resno;
            walk_pg_expression_including_root(reinterpret_cast<Node*>(entry->expr), audit, visitor);
            if (audit.invalid) {
                bms_free(matched_sites);
                return false;
            }
        }
        const bool all_sites_matched = bms_num_members(matched_sites) == list_length(context.sites);
        bms_free(matched_sites);
        return all_sites_matched;
    }

    [[nodiscard]] std::size_t finalized_base_owner_matching_site_count(
        CustomScan* scan, Node* identity, const bioseq_stub_oids& oids) {
        finalized_base_owner_site_context context{};
        if (!try_resolve_finalized_base_owner_site_context(scan, oids, context) ||
            identity == nullptr)
            return 0u;
        PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
        FuncExpr* helper = placeholder == nullptr
                               ? batch_join_well_formed_match_helper(identity, oids)
                               : batch_join_match_helper_phv_function(placeholder, oids);
        if (helper == nullptr)
            return 0u;
        std::size_t matches = 0u;
        for (Node* raw_site : postgres_list<Node>(context.sites)) {
            List* site = raw_site != nullptr && IsA(raw_site, List)
                             ? reinterpret_cast<List*>(raw_site)
                             : NIL;
            if (list_length(site) != k_bioseq_match_helper_site_fields)
                return 2u;
            const int resno =
                site != NIL &&
                        IsA(reinterpret_cast<Node*>(list_nth(site, k_match_site_resno)), Integer)
                    ? intVal(list_nth(site, k_match_site_resno))
                    : -1;
            Node* raw_supplied =
                resno <= 0 || resno > list_length(scan->scan.plan.targetlist)
                    ? nullptr
                    : reinterpret_cast<Node*>(list_nth(scan->scan.plan.targetlist, resno - 1));
            TargetEntry* supplied = raw_supplied != nullptr && IsA(raw_supplied, TargetEntry)
                                        ? reinterpret_cast<TargetEntry*>(raw_supplied)
                                        : nullptr;
            bool malformed = false;
            if (supplied != nullptr && supplied->resno == resno &&
                equal(supplied->expr, identity) &&
                finalized_base_owner_site_matches(
                    site, helper, static_cast<AttrNumber>(resno), context, oids, malformed)) {
                /* Planner PHV and bare-Func carriers can legitimately collapse to
                 * equal post-setrefs expressions at distinct resnos.  They are two
                 * authoritative carrier sites for one base match_result owner, not two
                 * competing owners.  This routine feeds owner-counting audits, so
                 * classify the owner once while the resno-specific targetlist audit
                 * above continues to validate every individual site. */
                matches = 1u;
            }
            if (malformed)
                return 2u;
        }
        return matches;
    }

    [[nodiscard]] std::size_t finalized_descendant_match_owner_site_count(
        Plan* plan, Node* identity, const bioseq_stub_oids& oids) {
        if (plan == nullptr || identity == nullptr)
            return 0u;
        check_stack_depth();
        auto add = [](std::size_t left, std::size_t right) noexcept {
            return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
        };
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            std::size_t count = finalized_batch_owner_matching_site_count(custom, identity, oids);
            count = add(count, finalized_base_owner_matching_site_count(custom, identity, oids));
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                count =
                    add(count, finalized_descendant_match_owner_site_count(child, identity, oids));
            }
            return count;
        }
        auto search_plan_list = [identity, &oids, &add](List* plans) {
            std::size_t count = 0u;
            for (Plan* child : postgres_list<Plan>(plans)) {
                count =
                    add(count, finalized_descendant_match_owner_site_count(child, identity, oids));
            }
            return count;
        };
        if (IsA(plan, Append)) {
            return search_plan_list(reinterpret_cast<Append*>(plan)->appendplans);
        }
        if (IsA(plan, MergeAppend)) {
            return search_plan_list(reinterpret_cast<MergeAppend*>(plan)->mergeplans);
        }
        if (IsA(plan, BitmapAnd)) {
            return search_plan_list(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans);
        }
        if (IsA(plan, BitmapOr)) {
            return search_plan_list(reinterpret_cast<BitmapOr*>(plan)->bitmapplans);
        }
        if (IsA(plan, SubqueryScan)) {
            /* A SubqueryScan starts a distinct finalized helper-binding scope. */
            return 0u;
        }
        std::size_t count =
            finalized_descendant_match_owner_site_count(outerPlan(plan), identity, oids);
        if (innerPlan(plan) != outerPlan(plan)) {
            count =
                add(count,
                    finalized_descendant_match_owner_site_count(innerPlan(plan), identity, oids));
        }
        return count;
    }

    /* Diagnostic-only counterpart to finalized_descendant_match_owner_site_count.
     * Keep Batch and Base ownership separate so a rejected finalized carrier can
     * explain which owner class, if either, still validates the raw helper identity.
     * Counts saturate at two because the audit only distinguishes zero, unique,
     * and ambiguous ownership. */
    [[nodiscard]] std::size_t finalized_descendant_match_owner_site_count_by_kind(
        Plan* plan, Node* identity, const bioseq_stub_oids& oids, bool batch_owner) {
        if (plan == nullptr || identity == nullptr)
            return 0u;
        check_stack_depth();
        auto add = [](std::size_t left, std::size_t right) noexcept {
            return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
        };
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            std::size_t count =
                batch_owner ? finalized_batch_owner_matching_site_count(custom, identity, oids)
                            : finalized_base_owner_matching_site_count(custom, identity, oids);
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                count = add(count,
                            finalized_descendant_match_owner_site_count_by_kind(
                                child, identity, oids, batch_owner));
            }
            return count;
        }
        auto search_plan_list = [identity, &oids, batch_owner, &add](List* plans) {
            std::size_t count = 0u;
            for (Plan* child : postgres_list<Plan>(plans)) {
                count = add(count,
                            finalized_descendant_match_owner_site_count_by_kind(
                                child, identity, oids, batch_owner));
            }
            return count;
        };
        if (IsA(plan, Append)) {
            return search_plan_list(reinterpret_cast<Append*>(plan)->appendplans);
        }
        if (IsA(plan, MergeAppend)) {
            return search_plan_list(reinterpret_cast<MergeAppend*>(plan)->mergeplans);
        }
        if (IsA(plan, BitmapAnd)) {
            return search_plan_list(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans);
        }
        if (IsA(plan, BitmapOr)) {
            return search_plan_list(reinterpret_cast<BitmapOr*>(plan)->bitmapplans);
        }
        if (IsA(plan, SubqueryScan)) {
            /* Match the finalized helper-binding scope fence used by the verdict. */
            return 0u;
        }
        std::size_t count = finalized_descendant_match_owner_site_count_by_kind(
            outerPlan(plan), identity, oids, batch_owner);
        if (innerPlan(plan) != outerPlan(plan)) {
            count = add(count,
                        finalized_descendant_match_owner_site_count_by_kind(
                            innerPlan(plan), identity, oids, batch_owner));
        }
        return count;
    }

    struct finalized_match_owner_site_counts {
        std::size_t batch = 0u;
        std::size_t base = 0u;
    };

    [[nodiscard]] finalized_match_owner_site_counts finalized_child_match_owner_site_counts(
        Plan* plan, Node* identity, const bioseq_stub_oids& oids) {
        finalized_match_owner_site_counts counts{};
        if (plan == nullptr || identity == nullptr)
            return counts;
        auto add = [](std::size_t left, std::size_t right) noexcept {
            return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
        };
        auto count_child = [&counts, &add, identity, &oids](Plan* child) {
            counts.batch = add(
                counts.batch,
                finalized_descendant_match_owner_site_count_by_kind(child, identity, oids, true));
            counts.base = add(
                counts.base,
                finalized_descendant_match_owner_site_count_by_kind(child, identity, oids, false));
        };
        if (IsA(plan, Append)) {
            for (Plan* child : postgres_list<Plan>(reinterpret_cast<Append*>(plan)->appendplans)) {
                count_child(child);
            }
        } else if (IsA(plan, MergeAppend)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<MergeAppend*>(plan)->mergeplans)) {
                count_child(child);
            }
        } else if (IsA(plan, CustomScan)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<CustomScan*>(plan)->custom_plans)) {
                count_child(child);
            }
        } else if (IsA(plan, BitmapAnd)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans)) {
                count_child(child);
            }
        } else if (IsA(plan, BitmapOr)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapOr*>(plan)->bitmapplans)) {
                count_child(child);
            }
        } else if (!IsA(plan, SubqueryScan)) {
            count_child(outerPlan(plan));
            if (innerPlan(plan) != outerPlan(plan))
                count_child(innerPlan(plan));
        }
        return counts;
    }

    [[nodiscard]] constexpr std::size_t
    saturated_match_owner_site_count_add(std::size_t left, std::size_t right) noexcept {
        return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
    }

    [[nodiscard]] constexpr bool
    dummy_match_helper_has_unique_finalized_owner(std::size_t descendant_matches,
                                                  std::size_t current_parent_matches) noexcept {
        return saturated_match_owner_site_count_add(descendant_matches, current_parent_matches) ==
               1u;
    }

    static_assert(dummy_match_helper_has_unique_finalized_owner(0u, 1u));
    static_assert(!dummy_match_helper_has_unique_finalized_owner(1u, 1u));
    static_assert(!dummy_match_helper_has_unique_finalized_owner(0u, 0u));
    static_assert(!dummy_match_helper_has_unique_finalized_owner(2u, 0u));

    [[nodiscard]] constexpr bool
    dummy_match_helper_is_unique_ancestor_carrier(std::size_t local_base_matches,
                                                  std::size_t ancestor_matches) noexcept {
        return local_base_matches == 0u && ancestor_matches == 1u;
    }

    static_assert(dummy_match_helper_is_unique_ancestor_carrier(0u, 1u));
    static_assert(!dummy_match_helper_is_unique_ancestor_carrier(1u, 0u));
    static_assert(!dummy_match_helper_is_unique_ancestor_carrier(0u, 2u));

    [[nodiscard]] match_owner_finalization*
    finalized_batch_owner_for_ancestor_stack(CustomScan* scan,
                                             std::span<match_owner_finalization> owners) {
        match_owner_finalization* result = nullptr;
        for (match_owner_finalization& owner : owners) {
            if (owner.scan != scan || owner.base_owner_tag != NIL)
                continue;
            if (result != nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("Bioseq finalized ancestor match owner is duplicated"),
                         errdetail("plan_node_id=%d", scan->scan.plan.plan_node_id)));
            }
            result = &owner;
        }
        if (result == nullptr)
            return nullptr;

        Node* version_node = scan->custom_private == NIL
                                 ? nullptr
                                 : reinterpret_cast<Node*>(linitial(scan->custom_private));
        const int version =
            version_node != nullptr && IsA(version_node, Integer) ? intVal(version_node) : -1;
        const int expected_fields = version == k_bioseq_batch_join_plan_version
                                        ? k_bioseq_batch_join_private_fields
                                        : (version == k_bioseq_per_row_join_plan_version
                                               ? k_bioseq_per_row_join_private_fields
                                               : -1);
        const int descriptor_index =
            version == k_bioseq_batch_join_plan_version
                ? k_batch_join_match_owner_descriptor
                : (version == k_bioseq_per_row_join_plan_version ? k_per_row_match_owner_descriptor
                                                                 : -1);
        const int sites_index =
            version == k_bioseq_batch_join_plan_version
                ? k_batch_join_placeholder_sites
                : (version == k_bioseq_per_row_join_plan_version ? k_per_row_placeholder_sites
                                                                 : -1);
        Node* descriptor_node =
            descriptor_index >= 0 && descriptor_index < list_length(scan->custom_private)
                ? reinterpret_cast<Node*>(list_nth(scan->custom_private, descriptor_index))
                : nullptr;
        Node* sites_node =
            sites_index >= 0 && sites_index < list_length(scan->custom_private)
                ? reinterpret_cast<Node*>(list_nth(scan->custom_private, sites_index))
                : nullptr;
        const bool sites_agree = result->sites == NIL
                                     ? sites_node == nullptr
                                     : sites_node == reinterpret_cast<Node*>(result->sites);
        if (scan->methods != bioseq_batch_join_scan_methods() || expected_fields < 0 ||
            list_length(scan->custom_private) != expected_fields || descriptor_node == nullptr ||
            !IsA(descriptor_node, List) ||
            descriptor_node != reinterpret_cast<Node*>(result->descriptor) || !sites_agree ||
            result->stable_match_id < 0 ||
            result->stable_match_id != scan->scan.plan.plan_node_id ||
            result->query_scope_id == 0u) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("Bioseq finalized ancestor match owner is invalid"),
                     errdetail("plan_node_id=%d version=%d query_scope_id=" UINT64_FORMAT,
                               scan->scan.plan.plan_node_id,
                               version,
                               static_cast<uint64>(result->query_scope_id))));
        }
        const int descriptor_stable_id = validate_finalized_match_owner_descriptor_value(
            scan, reinterpret_cast<List*>(descriptor_node));
        if (descriptor_stable_id != result->stable_match_id) {
            ereport(
                ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("Bioseq finalized ancestor match identity disagrees with its descriptor"),
                 errdetail("plan_node_id=%d descriptor_stable_id=%d owner_stable_id=%d",
                           scan->scan.plan.plan_node_id,
                           descriptor_stable_id,
                           result->stable_match_id)));
        }
        return result;
    }

    void neutralize_bioseq_scan_ancestor_helper_carriers(CustomScan* scan,
                                                         List* ancestor_stack,
                                                         const bioseq_stub_oids& oids) {
        if (scan == nullptr || scan->methods != bioseq_column_scan_scan_methods() ||
            ancestor_stack == NIL)
            return;
        if (batch_join_expression_has_match_helper(reinterpret_cast<Node*>(scan->scan.plan.qual),
                                                   oids)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqScan inherited match helper remains executable after setrefs"),
                     errdetail("store=Plan.qual plan_node_id=%d", scan->scan.plan.plan_node_id)));
        }
        if (batch_join_expression_has_match_helper(reinterpret_cast<Node*>(scan->custom_exprs),
                                                   oids)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqScan inherited match helper remains executable after setrefs"),
                     errdetail("store=CustomScan.custom_exprs plan_node_id=%d",
                               scan->scan.plan.plan_node_id)));
        }

        for (TargetEntry* entry : postgres_list<TargetEntry>(scan->scan.plan.targetlist)) {
            Node* identity = entry == nullptr ? nullptr : reinterpret_cast<Node*>(entry->expr);
            if (!batch_join_expression_has_match_helper(identity, oids))
                continue;
            PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            FuncExpr* helper = placeholder == nullptr
                                   ? batch_join_well_formed_match_helper(identity, oids)
                                   : batch_join_match_helper_phv_function(placeholder, oids);
            if (entry == nullptr || helper == nullptr ||
                is_projection_capable_plan(reinterpret_cast<Plan*>(scan))) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg(
                         "BioseqScan inherited match helper carrier is not a whole dummy target"),
                     errdetail("plan_node_id=%d resno=%d",
                               scan->scan.plan.plan_node_id,
                               entry == nullptr ? 0 : entry->resno)));
            }

            const std::size_t local_base_matches =
                finalized_base_owner_matching_site_count(scan, identity, oids);
            std::size_t ancestor_matches = 0u;
            finalized_match_helper_carrier_audit carrier_audit{};
            for (match_owner_finalization* ancestor :
                 postgres_list<match_owner_finalization>(ancestor_stack)) {
                if (ancestor == nullptr || ancestor->scan == nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("Bioseq finalized ancestor stack is invalid")));
                }
                ancestor_matches = saturated_match_owner_site_count_add(
                    ancestor_matches,
                    finalized_batch_owner_matching_site_count(
                        ancestor->scan, identity, oids, &carrier_audit));
            }
            if (local_base_matches > 1u || ancestor_matches > 1u ||
                (local_base_matches == 1u && ancestor_matches != 0u)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan match helper carrier ownership is ambiguous"),
                         errdetail(
                             "plan_node_id=%d resno=%d local_base_matches=%zu ancestor_matches=%zu",
                             scan->scan.plan.plan_node_id,
                             entry->resno,
                             local_base_matches,
                             ancestor_matches)));
            }
            /* A finalized base owner executes this helper at the leaf.  Only raw
             * carriers owned exclusively by an enclosing Batch/PerRow node are
             * non-executable dummy target columns. */
            if (local_base_matches == 1u)
                continue;
            if (!dummy_match_helper_is_unique_ancestor_carrier(local_base_matches,
                                                               ancestor_matches)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan inherited match helper carrier has no unique finalized "
                                "ancestor"),
                         errdetail(
                             "plan_node_id=%d resno=%d local_base_matches=%zu ancestor_matches=%zu "
                             "inspected=%s early_reject=%d version=%d private_fields=%d "
                             "expected_fields=%d descriptor_tag=%d sites_tag=%d site_count=%d "
                             "supplied_tag=%d carrier_tag=%d supplied_arg_tag=%d "
                             "carrier_arg_tag=%d canonical_arg_tag=%d site_resno=%d site_phid=%d "
                             "supplied_phid=%d carrier_phid=%d identity_equal=%s "
                             "supplied_arg_canonical=%s carrier_arg_canonical=%s equivalent=%s",
                             scan->scan.plan.plan_node_id,
                             entry->resno,
                             local_base_matches,
                             ancestor_matches,
                             carrier_audit.inspected ? "true" : "false",
                             carrier_audit.early_reject,
                             carrier_audit.version,
                             carrier_audit.private_fields,
                             carrier_audit.expected_fields,
                             carrier_audit.descriptor_tag,
                             carrier_audit.sites_tag,
                             carrier_audit.site_count,
                             carrier_audit.supplied_tag,
                             carrier_audit.carrier_tag,
                             carrier_audit.supplied_argument_tag,
                             carrier_audit.carrier_argument_tag,
                             carrier_audit.canonical_argument_tag,
                             carrier_audit.site_resno,
                             carrier_audit.site_phid,
                             carrier_audit.supplied_phid,
                             carrier_audit.carrier_phid,
                             carrier_audit.identity_equal ? "true" : "false",
                             carrier_audit.supplied_argument_canonical ? "true" : "false",
                             carrier_audit.carrier_argument_canonical ? "true" : "false",
                             carrier_audit.equivalent ? "true" : "false")));
            }

            const Oid type_oid = exprType(identity);
            const int32 typmod = exprTypmod(identity);
            const Oid collation = exprCollation(identity);
            if (!OidIsValid(type_oid) || type_oid != helper->funcresulttype ||
                type_oid != get_func_rettype(helper->funcid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan inherited match helper carrier type is invalid"),
                         errdetail("plan_node_id=%d resno=%d type_oid=%u helper_type_oid=%u",
                                   scan->scan.plan.plan_node_id,
                                   entry->resno,
                                   type_oid,
                                   helper->funcresulttype)));
            }

            const AttrNumber original_resno = entry->resno;
            char* const original_resname = entry->resname;
            const Index original_sortgroupref = entry->ressortgroupref;
            const Oid original_table_oid = entry->resorigtbl;
            const AttrNumber original_column = entry->resorigcol;
            const bool original_resjunk = entry->resjunk;
            Const* replacement = makeNullConst(type_oid, typmod, collation);
            entry->expr = reinterpret_cast<Expr*>(replacement);
            if (replacement == nullptr || !replacement->constisnull ||
                exprType(reinterpret_cast<Node*>(entry->expr)) != type_oid ||
                exprTypmod(reinterpret_cast<Node*>(entry->expr)) != typmod ||
                exprCollation(reinterpret_cast<Node*>(entry->expr)) != collation ||
                entry->resno != original_resno || entry->resname != original_resname ||
                entry->ressortgroupref != original_sortgroupref ||
                entry->resorigtbl != original_table_oid || entry->resorigcol != original_column ||
                entry->resjunk != original_resjunk) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan inherited match helper carrier neutralization changed "
                                "its slot contract"),
                         errdetail("plan_node_id=%d resno=%d",
                                   scan->scan.plan.plan_node_id,
                                   original_resno)));
            }
        }
    }

    void
    neutralize_ancestor_owned_dummy_match_helpers_plan(Plan* plan,
                                                       std::span<match_owner_finalization> owners,
                                                       List* ancestor_stack,
                                                       const bioseq_stub_oids& oids) {
        if (plan == nullptr)
            return;
        check_stack_depth();
        if (IsA(plan, SubqueryScan)) {
            /* A SubqueryScan is a hard query-scope boundary.  An owner above it can
             * never justify neutralizing an equal local helper spelling below it. */
            neutralize_ancestor_owned_dummy_match_helpers_plan(
                reinterpret_cast<SubqueryScan*>(plan)->subplan, owners, NIL, oids);
            return;
        }
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (custom->methods == bioseq_column_scan_scan_methods()) {
                neutralize_bioseq_scan_ancestor_helper_carriers(custom, ancestor_stack, oids);
            }
            match_owner_finalization* owner =
                finalized_batch_owner_for_ancestor_stack(custom, owners);
            match_owner_finalization* parent_owner =
                ancestor_stack == NIL
                    ? nullptr
                    : reinterpret_cast<match_owner_finalization*>(llast(ancestor_stack));
            if (owner != nullptr && parent_owner != nullptr &&
                parent_owner->query_scope_id != owner->query_scope_id) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq finalized ancestor stack crosses query scopes"),
                     errdetail("parent_scope_id=" UINT64_FORMAT " child_scope_id=" UINT64_FORMAT,
                               static_cast<uint64>(parent_owner->query_scope_id),
                               static_cast<uint64>(owner->query_scope_id))));
            }
            List* child_ancestors =
                owner == nullptr ? ancestor_stack : lappend(list_copy(ancestor_stack), owner);
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                neutralize_ancestor_owned_dummy_match_helpers_plan(
                    child, owners, child_ancestors, oids);
            }
            if (owner != nullptr)
                list_free(child_ancestors);
            return;
        }
        auto visit_plan_list = [owners, ancestor_stack, &oids](List* plans) {
            for (Plan* child : postgres_list<Plan>(plans)) {
                neutralize_ancestor_owned_dummy_match_helpers_plan(
                    child, owners, ancestor_stack, oids);
            }
        };
        if (IsA(plan, Append)) {
            visit_plan_list(reinterpret_cast<Append*>(plan)->appendplans);
            return;
        }
        if (IsA(plan, MergeAppend)) {
            visit_plan_list(reinterpret_cast<MergeAppend*>(plan)->mergeplans);
            return;
        }
        if (IsA(plan, BitmapAnd)) {
            visit_plan_list(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans);
            return;
        }
        if (IsA(plan, BitmapOr)) {
            visit_plan_list(reinterpret_cast<BitmapOr*>(plan)->bitmapplans);
            return;
        }
        neutralize_ancestor_owned_dummy_match_helpers_plan(
            outerPlan(plan), owners, ancestor_stack, oids);
        if (innerPlan(plan) != outerPlan(plan)) {
            neutralize_ancestor_owned_dummy_match_helpers_plan(
                innerPlan(plan), owners, ancestor_stack, oids);
        }
    }

    void neutralize_ancestor_owned_dummy_match_helpers(PlannedStmt* statement,
                                                       std::span<match_owner_finalization> owners) {
        if (statement == nullptr || statement->planTree == nullptr)
            return;
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        neutralize_ancestor_owned_dummy_match_helpers_plan(statement->planTree, owners, NIL, oids);
        for (Plan* subplan : postgres_list<Plan>(statement->subplans)) {
            neutralize_ancestor_owned_dummy_match_helpers_plan(subplan, owners, NIL, oids);
        }
    }

    [[nodiscard]] bool dummy_targetlist_has_only_unique_finalized_owner_helpers(
        Plan* plan, const bioseq_stub_oids& oids, CustomScan* current_parent) {
        if (plan == nullptr || is_projection_capable_plan(plan))
            return false;
        for (TargetEntry* entry : postgres_list<TargetEntry>(plan->targetlist)) {
            Node* identity = entry == nullptr ? nullptr : reinterpret_cast<Node*>(entry->expr);
            if (!batch_join_expression_has_match_helper(identity, oids))
                continue;
            PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            FuncExpr* helper = placeholder == nullptr
                                   ? batch_join_well_formed_match_helper(identity, oids)
                                   : batch_join_match_helper_phv_function(placeholder, oids);
            if (helper == nullptr)
                return false;
            const std::size_t current_parent_matches =
                current_parent == nullptr || IsA(plan, SubqueryScan)
                    ? 0u
                    : finalized_batch_owner_matching_site_count(current_parent, identity, oids);
            std::size_t descendant_matches = 0u;
            auto count_child = [&descendant_matches, identity, &oids](Plan* child) {
                const std::size_t additional =
                    finalized_descendant_match_owner_site_count(child, identity, oids);
                descendant_matches =
                    saturated_match_owner_site_count_add(descendant_matches, additional);
            };
            if (IsA(plan, Append)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<Append*>(plan)->appendplans)) {
                    count_child(child);
                }
            } else if (IsA(plan, MergeAppend)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<MergeAppend*>(plan)->mergeplans)) {
                    count_child(child);
                }
            } else if (IsA(plan, CustomScan)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<CustomScan*>(plan)->custom_plans)) {
                    count_child(child);
                }
            } else if (IsA(plan, BitmapAnd)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans)) {
                    count_child(child);
                }
            } else if (IsA(plan, BitmapOr)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<BitmapOr*>(plan)->bitmapplans)) {
                    count_child(child);
                }
            } else if (IsA(plan, SubqueryScan)) {
                /* Do not derive a parent helper from a separate query scope. */
            } else {
                count_child(outerPlan(plan));
                if (innerPlan(plan) != outerPlan(plan))
                    count_child(innerPlan(plan));
            }
            if (!dummy_match_helper_has_unique_finalized_owner(descendant_matches,
                                                               current_parent_matches))
                return false;
        }
        return true;
    }

    struct batch_join_plan_helper_audit_first_failure {
        const char* reason = "none";
        const char* store = "none";
        const char* custom_name = "none";
        const char* boundary_encountered = "none";
        int plan_node_tag = -1;
        bool projection_capable = false;
        int first_helper_resno = 0;
        bool whole_helper_valid = false;
        std::size_t finalized_batch_descendant_matches = 0u;
        std::size_t finalized_base_descendant_matches = 0u;
        std::size_t current_parent_matches = 0u;
        bool recorded = false;
    };

    void
    record_batch_join_plan_helper_audit_failure(batch_join_plan_helper_audit_first_failure& failure,
                                                Plan* plan,
                                                CustomScan* current_parent,
                                                const bioseq_stub_oids& oids,
                                                const char* reason,
                                                const char* store,
                                                const char* boundary_encountered,
                                                bool inspect_targetlist) {
        if (failure.recorded)
            return;
        failure.recorded = true;
        failure.reason = reason;
        failure.store = store;
        failure.boundary_encountered = boundary_encountered;
        failure.plan_node_tag = plan == nullptr ? -1 : static_cast<int>(nodeTag(plan));
        failure.projection_capable = plan != nullptr && is_projection_capable_plan(plan);
        if (plan != nullptr && IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (custom->methods != nullptr && custom->methods->CustomName != nullptr) {
                failure.custom_name = custom->methods->CustomName;
            }
        }
        if (!inspect_targetlist || plan == nullptr)
            return;

        TargetEntry* first_helper_entry = nullptr;
        bool first_helper_whole = false;
        finalized_match_owner_site_counts first_helper_counts{};
        std::size_t first_helper_parent_matches = 0u;
        for (TargetEntry* entry : postgres_list<TargetEntry>(plan->targetlist)) {
            Node* identity = entry == nullptr ? nullptr : reinterpret_cast<Node*>(entry->expr);
            if (!batch_join_expression_has_match_helper(identity, oids))
                continue;
            PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            FuncExpr* helper = placeholder == nullptr
                                   ? batch_join_well_formed_match_helper(identity, oids)
                                   : batch_join_match_helper_phv_function(placeholder, oids);
            const bool whole_helper = helper != nullptr;
            const finalized_match_owner_site_counts counts =
                finalized_child_match_owner_site_counts(plan, identity, oids);
            const std::size_t parent_matches =
                current_parent == nullptr
                    ? 0u
                    : finalized_batch_owner_matching_site_count(current_parent, identity, oids);
            if (first_helper_entry == nullptr) {
                first_helper_entry = entry;
                first_helper_whole = whole_helper;
                first_helper_counts = counts;
                first_helper_parent_matches = parent_matches;
            }
            const std::size_t descendant_matches =
                saturated_match_owner_site_count_add(counts.batch, counts.base);
            if (failure.projection_capable || !whole_helper ||
                !dummy_match_helper_has_unique_finalized_owner(descendant_matches,
                                                               parent_matches)) {
                first_helper_entry = entry;
                first_helper_whole = whole_helper;
                first_helper_counts = counts;
                first_helper_parent_matches = parent_matches;
                break;
            }
        }
        if (first_helper_entry == nullptr)
            return;
        failure.first_helper_resno = first_helper_entry->resno;
        failure.whole_helper_valid = first_helper_whole;
        failure.finalized_batch_descendant_matches = first_helper_counts.batch;
        failure.finalized_base_descendant_matches = first_helper_counts.base;
        failure.current_parent_matches = first_helper_parent_matches;
    }

    /* Final setrefs must replace every propagated helper identity in an
     * executable Plan expression with the descendant owner's scan-slot Var.
     * A non-projecting node may retain the whole identity in its dummy targetlist,
     * but only when exactly one finalized descendant or current PerRow owner site
     * supplies that column.  Mixed parent/descendant ownership remains ambiguous.
     * Descriptor-only custom_scan_tlist/custom_private fields remain excluded. */
    [[nodiscard]] bool batch_join_plan_has_unowned_executable_match_helper(
        Plan* plan,
        const bioseq_stub_oids& oids,
        CustomScan* current_parent,
        batch_join_plan_helper_audit_first_failure& first_failure,
        const char* boundary_encountered) {
        if (plan == nullptr)
            return false;
        check_stack_depth();
        const bool targetlist_has_helper =
            batch_join_expression_has_match_helper(reinterpret_cast<Node*>(plan->targetlist), oids);
        if (targetlist_has_helper &&
            (!IsA(plan, CustomScan) || !base_scan_targetlist_has_only_finalized_match_helpers(
                                           reinterpret_cast<CustomScan*>(plan), oids)) &&
            !dummy_targetlist_has_only_unique_finalized_owner_helpers(plan, oids, current_parent)) {
            record_batch_join_plan_helper_audit_failure(
                first_failure,
                plan,
                current_parent,
                oids,
                is_projection_capable_plan(plan) ? "projection-capable targetlist"
                                                 : "dummy targetlist ownership validation failed",
                "Plan.targetlist",
                boundary_encountered,
                true);
            return true;
        }
        if (batch_join_expression_has_match_helper(reinterpret_cast<Node*>(plan->qual), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "Plan.qual",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if ((IsA(plan, NestLoop) || IsA(plan, MergeJoin) || IsA(plan, HashJoin)) &&
            batch_join_expression_has_match_helper(
                reinterpret_cast<Node*>(reinterpret_cast<Join*>(plan)->joinqual), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "Join.joinqual",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if (IsA(plan, MergeJoin) &&
            batch_join_expression_has_match_helper(
                reinterpret_cast<Node*>(reinterpret_cast<MergeJoin*>(plan)->mergeclauses), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "MergeJoin.mergeclauses",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if (IsA(plan, HashJoin) &&
            batch_join_expression_has_match_helper(
                reinterpret_cast<Node*>(reinterpret_cast<HashJoin*>(plan)->hashclauses), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "HashJoin.hashclauses",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if (IsA(plan, Result) &&
            batch_join_expression_has_match_helper(
                reinterpret_cast<Node*>(reinterpret_cast<Result*>(plan)->resconstantqual), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "Result.resconstantqual",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (batch_join_expression_has_match_helper(
                    reinterpret_cast<Node*>(custom->custom_exprs), oids)) {
                record_batch_join_plan_helper_audit_failure(first_failure,
                                                            plan,
                                                            current_parent,
                                                            oids,
                                                            "executable expression",
                                                            "CustomScan.custom_exprs",
                                                            boundary_encountered,
                                                            false);
                return true;
            }
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                if (batch_join_plan_has_unowned_executable_match_helper(
                        child, oids, current_parent, first_failure, boundary_encountered)) {
                    return true;
                }
            }
            return false;
        }
        auto search_plan_list =
            [&oids, current_parent, &first_failure, boundary_encountered](List* plans) {
                for (Plan* child : postgres_list<Plan>(plans)) {
                    if (batch_join_plan_has_unowned_executable_match_helper(
                            child, oids, current_parent, first_failure, boundary_encountered)) {
                        return true;
                    }
                }
                return false;
            };
        if (IsA(plan, Append)) {
            return search_plan_list(reinterpret_cast<Append*>(plan)->appendplans);
        }
        if (IsA(plan, MergeAppend)) {
            return search_plan_list(reinterpret_cast<MergeAppend*>(plan)->mergeplans);
        }
        if (IsA(plan, BitmapAnd)) {
            return search_plan_list(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans);
        }
        if (IsA(plan, BitmapOr)) {
            return search_plan_list(reinterpret_cast<BitmapOr*>(plan)->bitmapplans);
        }
        if (IsA(plan, SubqueryScan)) {
            return batch_join_plan_has_unowned_executable_match_helper(
                reinterpret_cast<SubqueryScan*>(plan)->subplan,
                oids,
                nullptr,
                first_failure,
                "SubqueryScan");
        }
        return batch_join_plan_has_unowned_executable_match_helper(
                   outerPlan(plan), oids, current_parent, first_failure, boundary_encountered) ||
               (innerPlan(plan) != outerPlan(plan) &&
                batch_join_plan_has_unowned_executable_match_helper(
                    innerPlan(plan), oids, current_parent, first_failure, boundary_encountered));
    }

    /*
     * reparameterize_mutator changes all varnos referencing the topmost parent
     * of child_rel to instead reference child_rel directly. Both the Var and the
     * RestrictInfo are deep copied before being written (copy-before-write), and
     * a RestrictInfo's clause is recursively rewritten via the cursor.
     */
    struct reparameterize_mutator {
        [[nodiscard]] pg_node_mutation operator()(pg_expression_mutation_cursor cursor,
                                                  pg_node_view node,
                                                  RelOptInfo& child_rel) const {
            if (Var* var = node.try_cast<Var>()) {
                if (bioseq_is_concrete_rte_varno(var->varno) &&
                    bms_is_member(static_cast<int>(var->varno), child_rel.top_parent_relids)) {
                    var = copy_pg_node(var);
                    var->varno = child_rel.relid;
                }
                return pg_node_mutation::replace_with(to_pg_node(var));
            }

            if (RestrictInfo* rinfo = node.try_cast<RestrictInfo>()) {
                rinfo = copy_pg_node(rinfo);
                rinfo->clause =
                    reinterpret_cast<Expr*>(cursor.descend(reinterpret_cast<Node*>(rinfo->clause)));
                return pg_node_mutation::replace_with(to_pg_node(rinfo));
            }

            return pg_node_mutation::descend();
        }
    };

    /*
     * BioseqColumnScanPath_ReparameterizeCustomPathByChild is a method called when a
     * path is reparameterized directly to a child relation, rather than the
     * top-level parent.
     *
     * For instance, let there be a join of two partitioned columnar relations PX
     * and PY. A path for a BioseqColumnScan of PY3 might be parameterized by PX so
     * that the join qual "PY3.a = PX.a" (referencing the parent PX) can be pushed
     * down. But if the planner decides on a partition-wise join, then the path
     * will be reparameterized on the child table PX3 directly.
     *
     * When that happens, we need to update all Vars in the pushed-down quals to
     * reference PX3, not PX, to match the child parameterization. This method
     * notifies us that it needs to be done, and allows us to update the
     * information in custom_private.
     */
    List* bioseq_column_scan_path_reparameterize_custom_path_by_child(PlannerInfo* root,
                                                                      List* custom_private,
                                                                      RelOptInfo* child_rel) {
        reparameterize_mutator mutator;
        return reinterpret_cast<List*>(
            mutate_pg_expression(reinterpret_cast<Node*>(custom_private), *child_rel, mutator));
    }

    List* bioseq_per_row_join_path_reparameterize_custom_path_by_child(PlannerInfo* root,
                                                                       List* custom_private,
                                                                       RelOptInfo* child_rel) {
        if (root == nullptr || root->parse == nullptr || child_rel == nullptr ||
            child_rel->top_parent == nullptr ||
            list_length(custom_private) != k_bioseq_per_row_join_path_private_fields) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch child reparameterization state is invalid")));
        }
        for (const int index : {0, 1, 2, 3, 7, 8, 9}) {
            Node* value = reinterpret_cast<Node*>(list_nth(custom_private, index));
            if (value == nullptr || !IsA(value, Integer)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch child scalar ABI is invalid"),
                         errdetail("field=%d", index)));
            }
        }
        for (const int index : {4, 5, 6, 10, 11, 12}) {
            if (list_nth(custom_private, index) == nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch child expression ABI is invalid"),
                         errdetail("field=%d", index)));
            }
        }

        const Index target_varno = static_cast<Index>(intVal(list_nth(custom_private, 2)));
        const AttrNumber target_storage_attno =
            static_cast<AttrNumber>(intVal(list_nth(custom_private, 3)));
        const Oid target_relation_oid = static_cast<Oid>(intVal(list_nth(custom_private, 1)));
        /* Use PostgreSQL's multilevel appendrel translator, not a varno-only
         * rewrite: attached partitions may have a distinct physical attno map. */
        List* rewritten = reinterpret_cast<List*>(adjust_appendrel_attrs_multilevel(
            root, reinterpret_cast<Node*>(custom_private), child_rel, child_rel->top_parent));
        if (!bioseq_is_concrete_rte_varno(target_varno)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch target child varno is invalid")));
        }
        if (bms_is_member(static_cast<int>(target_varno), child_rel->top_parent_relids)) {
            if (!bioseq_is_concrete_rte_varno(child_rel->relid) ||
                child_rel->relid > static_cast<Index>(list_length(root->parse->rtable))) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch target child RTE is unavailable")));
            }
            RangeTblEntry* child_rte = rt_fetch(child_rel->relid, root->parse->rtable);
            if (child_rte == nullptr || child_rte->rtekind != RTE_RELATION ||
                !OidIsValid(child_rte->relid) ||
                !relation_oid_uses_column_table_access_method(child_rte->relid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                         errmsg("BioseqPerRowMmsearch target partition is not a Bioseq relation")));
            }
            Oid target_type = InvalidOid;
            int32 target_typmod = -1;
            Oid target_collation = InvalidOid;
            get_atttypetypmodcoll(target_relation_oid,
                                  target_storage_attno,
                                  &target_type,
                                  &target_typmod,
                                  &target_collation);
            Var* parent_storage_var = makeVar(target_varno,
                                              target_storage_attno,
                                              target_type,
                                              target_typmod,
                                              target_collation,
                                              0);
            Var* child_storage_var = try_cast_pg_node<Var>(
                adjust_appendrel_attrs_multilevel(root,
                                                  reinterpret_cast<Node*>(parent_storage_var),
                                                  child_rel,
                                                  child_rel->top_parent));
            if (child_storage_var == nullptr || child_storage_var->varno != child_rel->relid ||
                child_storage_var->varattno <= 0 ||
                child_storage_var->vartype != get_cached_stub_oids().bioseq_seq_type_oid) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqPerRowMmsearch target partition column mapping is invalid")));
            }
            lfirst(list_nth_cell(rewritten, 1)) = makeInteger(static_cast<int>(child_rte->relid));
            lfirst(list_nth_cell(rewritten, 2)) =
                makeInteger(static_cast<int>(child_storage_var->varno));
            lfirst(list_nth_cell(rewritten, 3)) =
                makeInteger(static_cast<int>(child_storage_var->varattno));
        }
        return rewritten;
    }

    /** Returns the stable PostgreSQL path callbacks for base Bioseq scans. */
    [[nodiscard]] const CustomPathMethods* bioseq_column_scan_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqScan",
            .PlanCustomPath = bioseq_column_scan_path_plan_custom_path,
            .ReparameterizeCustomPathByChild =
                bioseq_column_scan_path_reparameterize_custom_path_by_child,
        };
        return &methods;
    }

    /** Returns the stable PostgreSQL path callbacks for per-row joins. */
    [[nodiscard]] const CustomPathMethods* bioseq_per_row_join_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqBatchJoin",
            .PlanCustomPath = bioseq_per_row_join_plan_custom_path,
            .ReparameterizeCustomPathByChild =
                bioseq_per_row_join_path_reparameterize_custom_path_by_child,
        };
        return &methods;
    }

} // export custom-scan partition declarations
