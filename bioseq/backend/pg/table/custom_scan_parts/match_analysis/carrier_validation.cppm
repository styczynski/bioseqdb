/**
 * @file
 * @brief Validates finalized match-owner descriptors and carrier equivalence.
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

export module bioseq.postgres.table:custom_scan_match_analysis_carrier_validation;

import std;
import bioseq.postgres.support;

import :custom_scan_plan_abi_constants;
import :custom_scan_runtime_match_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_match_finalization_contracts;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_join_ownership_semantic_admission_contracts;
export import :custom_scan_match_analysis_carrier_validation_contracts;

using namespace bioseq::postgres::support;

export {

    /** @brief Validates a finalized owner descriptor and returns its stable plan-node id. */
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

    /** @brief Validates an owner descriptor stored at a CustomScan private-data index. */
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

    /**
     * @brief Validates the base-owner executor image and installs its runtime bindings.
     * @return The finalized target recipe, or null when the scan has no base owner.
     */
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
                    ? bioseq::postgres::support::tuple_descriptor_attribute(relation_descriptor, source->varattno - 1)
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

    /** @brief Finds the target entry with an exact positive result number. */
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

    /** @brief Follows setrefs slot coordinates to verify one canonical helper argument. */
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

    /** @brief Verifies that a translated helper argument resolves to its canonical source. */
    [[nodiscard]] bool finalized_match_helper_argument_translation_agrees(
        CustomScan* scan, Node* translated, Node* canonical) noexcept {
        return finalized_match_helper_argument_translation_agrees_impl(
            scan == nullptr ? nullptr : &scan->scan.plan, translated, canonical, 0u);
    }

    /** @brief Checks allowed PHV and bare-identity shapes for a finalized carrier site. */
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

    /**
     * @brief Checks whether a scan entry and dummy carrier encode the same finalized helper.
     *
     * custom_scan_tlist, custom_private, and a descendant dummy targetlist cross
     * different setrefs coordinate domains.  A phid-zero site accepts only the
     * exact bare identity.  A positive phid accepts either its live non-nulling
     * owner PHV or the bare descriptor produced when setrefs strips that PHV;
     * both require the leaf carrier's exact canonical argument and full function
     * ABI. A carrier-side PHV is never a dummy leaf carrier.
     */
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

    /** @brief Counts finalized sites in one BatchJoin that own a helper carrier identity. */
    [[nodiscard]] std::size_t finalized_batch_owner_matching_site_count(
        CustomScan* scan,
        Node* identity,
        const bioseq_stub_oids& oids,
        finalized_match_helper_carrier_audit* audit) {
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

} // export custom-scan partition declarations
