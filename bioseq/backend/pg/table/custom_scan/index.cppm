/**
 * @file
 * @brief SQL test probes for match-helper planning and plan translation.
 */
module;
// Include Highway here so modules importing its target-specific types
// share the same translation-unit definitions.
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

export module bioseq.postgres.table:custom_scan;

import std;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.codegen.sql.annotation_model;

export import :custom_scan_support;
export import :custom_scan_slice_projection;
export import :custom_scan_join_paths;
export import :custom_scan_scan_paths;
export import :custom_scan_plan_builders;
export import :custom_scan_match_ownership;
export import :custom_scan_execution_method_contracts;
export import :custom_scan_planner_method_contracts;
export import :custom_scan_sql_probe_contracts;

using namespace bioseq::postgres::support;

export namespace bioseq::table::sql_routines {

bioseq::postgres::support::named_output<bioseq::table::batch_join_projection_guard_probe_row>
    batch_join_projection_guard_probe() {

    constexpr Index target_relid = 1;
    const bioseq_stub_oids& oids = get_cached_stub_oids();
    auto* exact_helper =
        make_batch_join_projection_guard_phv(oids.score_oid, INT4OID, target_relid, 1);
    const batch_join_projection_guard_fixture exact = make_batch_join_projection_guard_fixture(
        target_relid, list_make1(exact_helper), 13.0, 37.0, 5.0);
    const batch_join_bioseq_path_candidate exact_candidate =
        find_unparameterized_bioseq_custom_path(exact.rel, 2);
    const bool accepts_exact_helper = exact_candidate.scan == exact.scan;
    const bool preserves_wrapper_cost_owner =
        exact_candidate.cost_owner == &exact.projection->path &&
        exact_candidate.cost_owner->startup_cost == 13.0 &&
        exact_candidate.cost_owner->total_cost == 37.0 && exact_candidate.cost_owner->rows == 5.0;

    auto* unrelated_placeholder = makeNode(PlaceHolderVar);
    unrelated_placeholder->phexpr =
        reinterpret_cast<Expr*>(makeVar(target_relid, 1, INT4OID, -1, InvalidOid, 0));
    unrelated_placeholder->phrels = bms_make_singleton(static_cast<int>(target_relid));
    unrelated_placeholder->phnullingrels = nullptr;
    unrelated_placeholder->phid = 2;
    unrelated_placeholder->phlevelsup = 0;
    const batch_join_projection_guard_fixture unrelated = make_batch_join_projection_guard_fixture(
        target_relid,
        list_make2(make_batch_join_projection_guard_phv(oids.score_oid, INT4OID, target_relid, 3),
                   unrelated_placeholder),
        13.0,
        37.0,
        5.0);
    const bool rejects_unrelated_phv =
        find_unparameterized_bioseq_custom_path(unrelated.rel, 2).scan == nullptr;

    const Oid nextval_oid = DatumGetObjectId(
        DirectFunctionCall1(regprocedurein, CStringGetDatum("pg_catalog.nextval(regclass)")));
    auto* nextval = makeFuncExpr(nextval_oid,
                                 INT8OID,
                                 list_make1(makeConst(REGCLASSOID,
                                                      -1,
                                                      InvalidOid,
                                                      static_cast<int16>(sizeof(Oid)),
                                                      ObjectIdGetDatum(InvalidOid),
                                                      false,
                                                      true)),
                                 InvalidOid,
                                 InvalidOid,
                                 COERCE_EXPLICIT_CALL);
    auto* volatile_mixed = makeNode(CoalesceExpr);
    volatile_mixed->coalescetype = INT8OID;
    volatile_mixed->coalescecollid = InvalidOid;
    volatile_mixed->args = list_make2(
        make_batch_join_projection_guard_phv(oids.query_len_oid, INT8OID, target_relid, 4),
        nextval);
    volatile_mixed->location = -1;
    const batch_join_projection_guard_fixture volatile_fixture =
        make_batch_join_projection_guard_fixture(
            target_relid, list_make1(volatile_mixed), 13.0, 37.0, 5.0);
    const bool rejects_volatile_mixed_expression =
        find_unparameterized_bioseq_custom_path(volatile_fixture.rel, 2).scan == nullptr;

    const batch_join_projection_guard_fixture plain_target_var =
        make_batch_join_projection_guard_fixture(
            target_relid,
            list_make1(makeVar(target_relid, 2, INT4OID, -1, InvalidOid, 0)),
            13.0,
            37.0,
            5.0);
    const batch_join_bioseq_path_candidate plain_candidate =
        find_unparameterized_bioseq_custom_path(plain_target_var.rel, /*target_seq_attno=*/3);
    const bool accepts_plain_target_var =
        plain_candidate.scan == plain_target_var.scan &&
        plain_candidate.locator_target == plain_target_var.projection->path.pathtarget;

    const batch_join_projection_guard_fixture plain_target_sequence_var =
        make_batch_join_projection_guard_fixture(
            target_relid,
            list_make1(makeVar(target_relid, 3, INT4OID, -1, InvalidOid, 0)),
            13.0,
            37.0,
            5.0);
    const bool rejects_plain_target_sequence_var =
        find_unparameterized_bioseq_custom_path(plain_target_sequence_var.rel,
                                                /*target_seq_attno=*/3)
            .scan == nullptr;

    const batch_join_projection_guard_fixture whole_row_var =
        make_batch_join_projection_guard_fixture(
            target_relid,
            list_make1(makeVar(target_relid, InvalidAttrNumber, RECORDOID, -1, InvalidOid, 0)),
            13.0,
            37.0,
            5.0);
    const bool rejects_whole_row_var =
        find_unparameterized_bioseq_custom_path(whole_row_var.rel, /*target_seq_attno=*/3).scan ==
        nullptr;

    const batch_join_projection_guard_fixture system_var = make_batch_join_projection_guard_fixture(
        target_relid,
        list_make1(
            makeVar(target_relid, MinTransactionIdAttributeNumber, XIDOID, -1, InvalidOid, 0)),
        13.0,
        37.0,
        5.0);
    const bool rejects_system_var =
        find_unparameterized_bioseq_custom_path(system_var.rel, /*target_seq_attno=*/3).scan ==
        nullptr;

    Var* nulled_var = makeVar(target_relid, 2, INT4OID, -1, InvalidOid, 0);
    nulled_var->varnullingrels = bms_make_singleton(static_cast<int>(target_relid));
    const batch_join_projection_guard_fixture nulling_var =
        make_batch_join_projection_guard_fixture(
            target_relid, list_make1(nulled_var), 13.0, 37.0, 5.0);
    const bool rejects_nulling_var =
        find_unparameterized_bioseq_custom_path(nulling_var.rel, /*target_seq_attno=*/3).scan ==
        nullptr;

    // Row markers use ordinary child attributes after projection. Exercise
    // both the new admitted case and every exact-identity rejection gate.
    const auto accepts_row_marker = [target_relid](Var* marker) {
        const auto fixture = make_batch_join_projection_guard_fixture(
            target_relid, list_make1(marker), 13.0, 37.0, 5.0);
        const auto candidate = find_unparameterized_bioseq_custom_path(
            fixture.rel, /*target_seq_attno=*/3);
        return candidate.scan == fixture.scan &&
               candidate.cost_owner == &fixture.projection->path &&
               candidate.locator_target == fixture.projection->path.pathtarget;
    };
    bool accepts_exact_row_markers = true;
    bool rejects_malformed_row_markers = true;
    for (Var* marker : std::array{
             makeVar(target_relid, SelfItemPointerAttributeNumber, TIDOID, -1, InvalidOid, 0),
             makeVar(target_relid, TableOidAttributeNumber, OIDOID, -1, InvalidOid, 0)}) {
        accepts_exact_row_markers = accepts_row_marker(marker) && accepts_exact_row_markers;
        Var* null_extended = static_cast<Var*>(copyObject(marker));
        null_extended->varnullingrels = bms_make_singleton(static_cast<int>(target_relid));
        for (Var* malformed : std::array{
                 makeVar(target_relid, marker->varattno, INT4OID, -1, InvalidOid, 0),
                 makeVar(target_relid, marker->varattno, marker->vartype, 0, InvalidOid, 0),
                 makeVar(target_relid, marker->varattno, marker->vartype, -1,
                         static_cast<Oid>(1), 0),
                 makeVar(target_relid, marker->varattno, marker->vartype, -1, InvalidOid, 1),
                 makeVar(target_relid + 1, marker->varattno, marker->vartype, -1, InvalidOid, 0),
                 makeVar(OUTER_VAR, marker->varattno, marker->vartype, -1, InvalidOid, 0),
                 null_extended}) {
            rejects_malformed_row_markers =
                !accepts_row_marker(malformed) && rejects_malformed_row_markers;
        }
    }

    return bioseq::postgres::support::named_output{
        bioseq::table::batch_join_projection_guard_probe_row{
            .accepts_exact_helper = accepts_exact_helper,
            .rejects_unrelated_phv = rejects_unrelated_phv,
            .rejects_volatile_mixed_expression = rejects_volatile_mixed_expression,
            .preserves_wrapper_cost_owner = preserves_wrapper_cost_owner,
            .accepts_plain_target_var = accepts_plain_target_var && accepts_exact_row_markers,
            .rejects_plain_target_sequence_var = rejects_plain_target_sequence_var,
            .rejects_whole_row_var = rejects_whole_row_var,
            .rejects_system_var = rejects_system_var && rejects_malformed_row_markers,
            .rejects_nulling_var = rejects_nulling_var,
        }};
}

} // namespace bioseq::table::sql_routines
export namespace bioseq::table::sql_routines {

bioseq::postgres::support::named_output<bioseq::table::match_helper_wrapper_propagation_probe_row>
    match_helper_wrapper_propagation_probe() {

    const bioseq_stub_oids& oids = get_cached_stub_oids();
    auto* planner_root = makeNode(PlannerInfo);
    planner_root->parse = makeNode(Query);
    planner_root->glob = makeNode(PlannerGlobal);
    planner_root->query_level = 1;

    auto* left_rel = match_helper_wrapper_probe_rel(RELOPT_BASEREL, 1, bms_make_singleton(1));
    auto* right_rel = match_helper_wrapper_probe_rel(RELOPT_BASEREL, 2, bms_make_singleton(2));
    auto* sibling_rel = match_helper_wrapper_probe_rel(RELOPT_BASEREL, 3, bms_make_singleton(3));
    auto* join_rel = match_helper_wrapper_probe_rel(
        RELOPT_JOINREL, 0, bms_union(bms_make_singleton(1), bms_make_singleton(2)));

    /* Use the registered query_len helper with a null sequence argument.
     * Its  type and expression identity are valid without constructing a range
     * table for this cost-propagation test. */
    Node* recipe = reinterpret_cast<Node*>(makeNullConst(oids.bioseq_seq_type_oid, -1, InvalidOid));
    auto* helper = makeFuncExpr(oids.query_len_oid,
                                get_func_rettype(oids.query_len_oid),
                                list_make1(copyObject(recipe)),
                                InvalidOid,
                                InvalidOid,
                                COERCE_EXPLICIT_CALL);
    Node* helper_identity = reinterpret_cast<Node*>(helper);

    auto* left = makeNode(Path);
    match_helper_wrapper_probe_init_path(left, T_SeqScan, left_rel, create_empty_pathtarget());
    auto* right = makeNode(Path);
    match_helper_wrapper_probe_init_path(right, T_SeqScan, right_rel, create_empty_pathtarget());

    auto* owner = makeNode(CustomPath);
    match_helper_wrapper_probe_init_path(
        &owner->path, T_CustomScan, join_rel, create_empty_pathtarget());
    owner->custom_paths = list_make2(left, right);
    owner->custom_restrictinfo = NIL;
    owner->custom_private = list_make5(makeInteger(k_bioseq_batch_join_plan_version),
                                       makeInteger(1),
                                       makeInteger(InvalidOid),
                                       makeInteger(1),
                                       copyObject(recipe));
    while (list_length(owner->custom_private) < k_bioseq_batch_join_path_private_fields) {
        owner->custom_private = lappend(owner->custom_private, makeInteger(0));
    }
    owner->methods = bioseq_batch_join_path_methods();

    auto* projection = makeNode(ProjectionPath);
    match_helper_wrapper_probe_init_path(
        &projection->path, T_Result, join_rel, create_empty_pathtarget());
    projection->subpath = &owner->path;
    projection->dummypp = false;

    auto* material = makeNode(MaterialPath);
    match_helper_wrapper_probe_init_path(
        &material->path, T_Material, join_rel, create_empty_pathtarget());
    material->subpath = &projection->path;

    auto* memoize = makeNode(MemoizePath);
    match_helper_wrapper_probe_init_path(
        &memoize->path, T_Memoize, join_rel, create_empty_pathtarget());
    memoize->subpath = &material->path;
    memoize->hash_operators = NIL;
    memoize->param_exprs = NIL;
    memoize->singlerow = false;
    memoize->binary_mode = false;
#if PG_VERSION_NUM >= 190000
    memoize->est_calls = 2.0;
#else
    memoize->calls = 2.0;
#endif
    memoize->est_entries = 1;

    auto* unique = makeNode(UniquePath);
    match_helper_wrapper_probe_init_path(
        &unique->path, T_Unique, join_rel, create_empty_pathtarget());
    unique->subpath = &memoize->path;
#if PG_VERSION_NUM >= 190000
    unique->numkeys = 0;
#else
    unique->umethod = UNIQUE_PATH_SORT;
    unique->in_operators = NIL;
    unique->uniq_exprs = NIL;
#endif

    auto* sibling = makeNode(Path);
    match_helper_wrapper_probe_init_path(
        sibling, T_SeqScan, sibling_rel, create_empty_pathtarget());
    PathTarget* const sibling_target = sibling->pathtarget;

    auto* append = makeNode(AppendPath);
    match_helper_wrapper_probe_init_path(
        &append->path, T_Append, join_rel, create_empty_pathtarget());
    append->subpaths = list_make2(unique, sibling);
    append->first_partial_path = 2;
    append->limit_tuples = -1.0;

    per_row_descendant_ownership ownership{};
    per_row_analyze_descendant_exact_owner(&append->path, recipe, ownership);
    const bool exact_owner_found = ownership.matches == 1u && ownership.owner == owner &&
                                   !ownership.invalid_owner_abi && !ownership.unsupported_wrapper;

    bool applied = false;
    bool invalid = false;
    Path* cloned_path = per_row_clone_path_to_helper_owner(
        planner_root, &append->path, owner, helper_identity, applied, invalid);
    auto* cloned_append =
        IsA(cloned_path, AppendPath) ? reinterpret_cast<AppendPath*>(cloned_path) : nullptr;
    Path* cloned_unique_path = cloned_append != nullptr && list_length(cloned_append->subpaths) == 2
                                   ? reinterpret_cast<Path*>(linitial(cloned_append->subpaths))
                                   : nullptr;
    auto* cloned_unique = cloned_unique_path != nullptr && IsA(cloned_unique_path, UniquePath)
                              ? reinterpret_cast<UniquePath*>(cloned_unique_path)
                              : nullptr;
    auto* cloned_memoize = cloned_unique != nullptr && IsA(cloned_unique->subpath, MemoizePath)
                               ? reinterpret_cast<MemoizePath*>(cloned_unique->subpath)
                               : nullptr;
    auto* cloned_material = cloned_memoize != nullptr && IsA(cloned_memoize->subpath, MaterialPath)
                                ? reinterpret_cast<MaterialPath*>(cloned_memoize->subpath)
                                : nullptr;
    auto* cloned_projection =
        cloned_material != nullptr && IsA(cloned_material->subpath, ProjectionPath)
            ? reinterpret_cast<ProjectionPath*>(cloned_material->subpath)
            : nullptr;
    auto* cloned_owner = cloned_projection != nullptr && IsA(cloned_projection->subpath, CustomPath)
                             ? reinterpret_cast<CustomPath*>(cloned_projection->subpath)
                             : nullptr;

    auto helper_available = [helper_identity](const Path* path) {
        return path != nullptr && path_target_contains_expression(
                                      path->pathtarget, reinterpret_cast<Expr*>(helper_identity));
    };
    auto cow_clone = [](const Path* clone, const Path* source) {
        return clone != nullptr && source != nullptr && clone != source &&
               clone->pathtarget != source->pathtarget;
    };
    const bool clone_applied = applied && !invalid && cloned_append != nullptr;
    const bool owner_cloned =
        cow_clone(cloned_owner == nullptr ? nullptr : &cloned_owner->path, &owner->path);
    const bool projection_cloned = cow_clone(
        cloned_projection == nullptr ? nullptr : &cloned_projection->path, &projection->path);
    const bool material_cloned =
        cow_clone(cloned_material == nullptr ? nullptr : &cloned_material->path, &material->path);
    const bool memoize_cloned =
        cow_clone(cloned_memoize == nullptr ? nullptr : &cloned_memoize->path, &memoize->path);
    const bool unique_cloned =
        cow_clone(cloned_unique == nullptr ? nullptr : &cloned_unique->path, &unique->path);
    const bool append_cloned =
        cow_clone(cloned_append == nullptr ? nullptr : &cloned_append->path, &append->path);
    const bool helper_on_owner =
        helper_available(cloned_owner == nullptr ? nullptr : &cloned_owner->path);
    const bool helper_on_projection =
        helper_available(cloned_projection == nullptr ? nullptr : &cloned_projection->path);
    const bool helper_on_material =
        helper_available(cloned_material == nullptr ? nullptr : &cloned_material->path);
    const bool helper_on_memoize =
        helper_available(cloned_memoize == nullptr ? nullptr : &cloned_memoize->path);
    const bool helper_on_unique =
        helper_available(cloned_unique == nullptr ? nullptr : &cloned_unique->path);
    const bool helper_on_append =
        helper_available(cloned_append == nullptr ? nullptr : &cloned_append->path);
    const bool originals_unchanged =
        !helper_available(&owner->path) && !helper_available(&projection->path) &&
        !helper_available(&material->path) && !helper_available(&memoize->path) &&
        !helper_available(&unique->path) && !helper_available(&append->path);
    Path* cloned_sibling = cloned_append != nullptr && list_length(cloned_append->subpaths) == 2
                               ? reinterpret_cast<Path*>(lsecond(cloned_append->subpaths))
                               : nullptr;
    const bool sibling_unchanged = cloned_sibling == sibling &&
                                   cloned_sibling->pathtarget == sibling_target &&
                                   !helper_available(cloned_sibling);

    /* Reject a helper with two matching owners. Also reject an unknown
     * CustomPath, whose evalation behavior this analysis cannot inspect. */
    auto* second_owner = makeNode(CustomPath);
    *second_owner = *owner;
    second_owner->path.pathtarget = create_empty_pathtarget();
    second_owner->custom_private = static_cast<List*>(copyObject(owner->custom_private));
    auto* duplicate_append = makeNode(AppendPath);
    match_helper_wrapper_probe_init_path(
        &duplicate_append->path, T_Append, join_rel, create_empty_pathtarget());
    duplicate_append->subpaths = list_make2(owner, second_owner);
    duplicate_append->first_partial_path = 2;
    duplicate_append->limit_tuples = -1.0;
    per_row_descendant_ownership duplicate_ownership{};
    per_row_analyze_descendant_exact_owner(&duplicate_append->path, recipe, duplicate_ownership);
    const bool duplicate_owner_rejected = duplicate_ownership.matches == 2u &&
                                          !duplicate_ownership.invalid_owner_abi &&
                                          !duplicate_ownership.unsupported_wrapper;

    auto* unknown = makeNode(CustomPath);
    match_helper_wrapper_probe_init_path(
        &unknown->path, T_CustomScan, join_rel, create_empty_pathtarget());
    unknown->custom_paths = list_make1(owner);
    unknown->methods = &match_helper_wrapper_probe_unknown_path_methods();
    per_row_descendant_ownership unknown_ownership{};
    per_row_analyze_descendant_exact_owner(&unknown->path, recipe, unknown_ownership);
    const bool unknown_wrapper_rejected = unknown_ownership.matches == 0u &&
                                          unknown_ownership.unsupported_wrapper &&
                                          !unknown_ownership.invalid_owner_abi;

    /* GroupResultPath stores executable conditions outside its output list.
     * Add a second mmsearch there to check that the analysis finds it and
     * rejects the claim that one BioseqScan owns all matching work */
    auto* bmo_target = makeVar(
        /*varno=*/1,
        /*varattno=*/1,
        oids.bioseq_seq_type_oid,
        /*vartypmod=*/-1,
        /*varcollid=*/InvalidOid,
        /*varlevelsup=*/0);
    auto* bmo_query = makeVar(
        /*varno=*/2,
        /*varattno=*/1,
        oids.bioseq_seq_type_oid,
        /*vartypmod=*/-1,
        /*varcollid=*/InvalidOid,
        /*varlevelsup=*/0);
    auto* bmo_mmsearch = makeFuncExpr(oids.mmsearch_oid,
                                      BOOLOID,
                                      list_make2(bmo_target, bmo_query),
                                      InvalidOid,
                                      InvalidOid,
                                      COERCE_EXPLICIT_CALL);
    auto* bmo_restriction = makeNode(RestrictInfo);
    bmo_restriction->clause = reinterpret_cast<Expr*>(bmo_mmsearch);
    bmo_restriction->rinfo_serial = 71;
    auto* bmo_param = makeNode(ParamPathInfo);
    bmo_param->ppi_req_outer = bms_make_singleton(2);
    bmo_param->ppi_clauses = list_make1(bmo_restriction);
    auto* bmo_owner = makeNode(CustomPath);
    match_helper_wrapper_probe_init_path(
        &bmo_owner->path, T_CustomScan, left_rel, create_empty_pathtarget());
    bmo_owner->path.param_info = bmo_param;
    bmo_owner->custom_paths = NIL;
    bmo_owner->custom_restrictinfo = list_make1(bmo_restriction);
    bmo_owner->custom_private = list_make2(NIL, NIL);
    bmo_owner->custom_private = lappend(bmo_owner->custom_private,
                                        list_make3(makeInteger(k_bioseq_mmsearch_owner_magic),
                                                   makeInteger(k_bioseq_mmsearch_owner_version),
                                                   list_make1(copyObject(bmo_mmsearch))));
    bmo_owner->methods = bioseq_column_scan_path_methods();

    auto* hidden_group = makeNode(GroupResultPath);
    match_helper_wrapper_probe_init_path(
        &hidden_group->path, T_Result, sibling_rel, create_empty_pathtarget());
    hidden_group->quals = list_make1(copyObject(bmo_mmsearch));
    auto* bmo_append = makeNode(AppendPath);
    match_helper_wrapper_probe_init_path(
        &bmo_append->path, T_Append, join_rel, create_empty_pathtarget());
    bmo_append->subpaths = list_make2(bmo_owner, hidden_group);
    bmo_append->first_partial_path = 2;
    bmo_append->limit_tuples = -1.0;
    exact_parameterized_bmo_ownership group_result_analysis{};
    count_exact_parameterized_bmo_owners(planner_root,
                                         &bmo_append->path,
                                         bmo_restriction,
                                         group_result_analysis,
                                         /*admit_known_wrapper_owner=*/true);
    const bool group_result_qual_rejected =
        group_result_analysis.matches == 1u && group_result_analysis.invalid &&
        (group_result_analysis.rejection_mask &
         static_cast<std::uint32_t>(k_exact_bmo_nested_join_clause)) != 0u;

    /* Keep an existing (T-Q owner) equality-join U path while considering
     * (T-U)-Q, whose target is buried. Exercise both algorithms directly so
     * planner costing cannot hide a missing top-level path-kind check. */
    auto* retained_rel = match_helper_wrapper_probe_rel(
        RELOPT_JOINREL, 0, bms_union(join_rel->relids, sibling_rel->relids));
    auto* buried_rel = match_helper_wrapper_probe_rel(
        RELOPT_JOINREL, 0, bms_union(left_rel->relids, sibling_rel->relids));
    auto* owned_nested = makeNode(NestPath);
    match_helper_wrapper_probe_init_path(
        &owned_nested->jpath.path, T_NestLoop, join_rel, create_empty_pathtarget());
    owned_nested->jpath.jointype = JOIN_INNER;
    owned_nested->jpath.outerjoinpath = right;
    owned_nested->jpath.innerjoinpath = &bmo_owner->path;
    auto* unowned_nested = makeNode(NestPath);
    *unowned_nested = *owned_nested;
    unowned_nested->jpath.innerjoinpath = left;
    auto* buried_nested = makeNode(NestPath);
    match_helper_wrapper_probe_init_path(
        &buried_nested->jpath.path, T_NestLoop, buried_rel, create_empty_pathtarget());
    buried_nested->jpath.jointype = JOIN_INNER;
    buried_nested->jpath.outerjoinpath = &bmo_owner->path;
    buried_nested->jpath.innerjoinpath = sibling;
    auto* unowned_buried = makeNode(NestPath);
    *unowned_buried = *buried_nested;
    unowned_buried->jpath.outerjoinpath = left;
    auto* unowned_current = makeNode(NestPath);
    match_helper_wrapper_probe_init_path(
        &unowned_current->jpath.path, T_NestLoop, retained_rel, create_empty_pathtarget());
    unowned_current->jpath.jointype = JOIN_INNER;
    unowned_current->jpath.outerjoinpath = &unowned_buried->jpath.path;
    unowned_current->jpath.innerjoinpath = right;
    unowned_current->jpath.joinrestrictinfo = list_make1(bmo_restriction);

    const Oid equality_oid = OpernameGetOprid(
        list_make2(makeString(pstrdup("pg_catalog")), makeString(pstrdup("="))), INT4OID, INT4OID);
    auto* equality = makeNode(RestrictInfo);
    equality->clause = make_opclause(
        equality_oid, BOOLOID, false,
        reinterpret_cast<Expr*>(makeVar(1, 2, INT4OID, -1, InvalidOid, 0)),
        reinterpret_cast<Expr*>(makeVar(3, 2, INT4OID, -1, InvalidOid, 0)),
        InvalidOid, InvalidOid);
    equality->rinfo_serial = 72;
    auto make_foreign_join = [retained_rel, sibling](
                                 bool hash, Path* child, List* clauses,
                                 List* algorithm_clauses) -> JoinPath* {
        JoinPath* join = nullptr;
        if (hash) {
            auto* path = makeNode(HashPath);
            path->path_hashclauses = algorithm_clauses;
            join = &path->jpath;
        } else {
            auto* path = makeNode(MergePath);
            path->path_mergeclauses = algorithm_clauses;
            join = &path->jpath;
        }
        match_helper_wrapper_probe_init_path(
            &join->path, hash ? T_HashJoin : T_MergeJoin, retained_rel,
            create_empty_pathtarget());
        join->jointype = JOIN_INNER;
        join->outerjoinpath = child;
        join->innerjoinpath = sibling;
        join->joinrestrictinfo = clauses;
        return join;
    };
    auto retain_foreign = [planner_root, retained_rel, buried_rel, right_rel,
                           bmo_restriction, bmo_mmsearch, unowned_current](
                              JoinPath* path, exact_parameterized_bmo_retainer_audit& audit) {
        retained_rel->pathlist = list_make2(path, unowned_current);
        JoinPathExtraData extra{};
        extra.restrictlist = list_make1(bmo_restriction);
        return retain_join_paths_with_custom_restriction_owner(
            planner_root, retained_rel, buried_rel, right_rel, &extra,
            bmo_restriction, bmo_mmsearch, &audit);
    };
    // Result plan tags do not imply a ProjectionPath allocation. Keep the
    // concrete leaf types safe even when GroupResultPath has a nonempty qual.
    auto* bare_result = makeNode(Path);
    match_helper_wrapper_probe_init_path(
        bare_result, T_Result, sibling_rel, create_empty_pathtarget());
    auto* group_result = makeNode(GroupResultPath);
    match_helper_wrapper_probe_init_path(
        &group_result->path, T_Result, sibling_rel, create_empty_pathtarget());
    group_result->quals = list_make1(makeBoolConst(true, false));
    auto* gather = makeNode(GatherPath);
    match_helper_wrapper_probe_init_path(
        &gather->path, T_Gather, sibling_rel, create_empty_pathtarget());
    gather->subpath = sibling;
    auto* gather_merge = makeNode(GatherMergePath);
    match_helper_wrapper_probe_init_path(
        &gather_merge->path, T_GatherMerge, sibling_rel, create_empty_pathtarget());
    gather_merge->subpath = sibling;
    auto* parallel_unique = makeNode(UniquePath);
    *parallel_unique = *unique;
    match_helper_wrapper_probe_init_path(
        &parallel_unique->path, T_Unique, sibling_rel, create_empty_pathtarget());
    parallel_unique->subpath = sibling;
    auto* parallel_project_set = makeNode(ProjectSetPath);
    match_helper_wrapper_probe_init_path(
        &parallel_project_set->path, T_ProjectSet, sibling_rel, create_empty_pathtarget());
    parallel_project_set->subpath = sibling;
    bool foreign_hash_preserved = false;
    bool foreign_merge_preserved = false;
    bool foreign_root_mmsearch_rejected = true;
    bool foreign_algorithm_mmsearch_rejected = true;
    bool foreign_unowned_rejected = true;
    bool non_nest_current_pair_rejected = true;
    bool bare_result_preserved = true;
    bool group_result_preserved = true;
    bool unique_gather_rejected =
        !batch_join_path_contains_parallel_execution(&parallel_unique->path);
    bool project_set_gather_rejected =
        !batch_join_path_contains_parallel_execution(&parallel_project_set->path);
    for (const bool hash : {true, false}) {
        JoinPath* preserved = make_foreign_join(
            hash, &owned_nested->jpath.path, list_make1(equality), list_make1(equality));
        exact_parameterized_bmo_retainer_audit positive{};
        const bool kept = OidIsValid(equality_oid) && retain_foreign(preserved, positive) &&
            positive.safe_foreign_paths == 1u && positive.retained_paths == 1u &&
            positive.foreign_owner_matches == 1u && positive.nest_paths == 1u &&
            positive.current_pair_paths == 1u && positive.foreign_decomposition_paths == 1u &&
            positive.reconstructed_bmo_attempts == 0u &&
            list_length(retained_rel->pathlist) == 1 &&
            linitial(retained_rel->pathlist) == preserved &&
            preserved->outerjoinpath == &owned_nested->jpath.path &&
            preserved->innerjoinpath == sibling &&
            preserved->path.startup_cost == 1.0 && preserved->path.total_cost == 2.0 &&
            list_length(preserved->joinrestrictinfo) == 1 &&
            linitial(preserved->joinrestrictinfo) == equality;
        if (hash)
            foreign_hash_preserved = kept;
        else
            foreign_merge_preserved = kept;

        exact_parameterized_bmo_retainer_audit root_clause{};
        const bool root_rejected = !retain_foreign(make_foreign_join(
            hash, &owned_nested->jpath.path, list_make2(equality, bmo_restriction),
            list_make1(equality)), root_clause);
        foreign_root_mmsearch_rejected = foreign_root_mmsearch_rejected && root_rejected &&
            root_clause.retained_paths == 0u && root_clause.root_owned_clause_paths == 2u &&
            (root_clause.foreign_rejection_mask & (1u << 1)) != 0u;

        exact_parameterized_bmo_retainer_audit algorithm_clause{};
        const bool algorithm_rejected = !retain_foreign(make_foreign_join(
            hash, &owned_nested->jpath.path, list_make1(equality),
            list_make1(bmo_restriction)), algorithm_clause);
        foreign_algorithm_mmsearch_rejected = foreign_algorithm_mmsearch_rejected &&
            algorithm_rejected && algorithm_clause.retained_paths == 0u &&
            (algorithm_clause.foreign_owner_rejection_mask &
             static_cast<std::uint32_t>(k_exact_bmo_nested_join_clause)) != 0u;

        exact_parameterized_bmo_retainer_audit no_owner{};
        const bool unowned_rejected = !retain_foreign(make_foreign_join(
            hash, &unowned_nested->jpath.path, list_make1(equality), list_make1(equality)),
            no_owner);
        foreign_unowned_rejected = foreign_unowned_rejected && unowned_rejected &&
            no_owner.retained_paths == 0u && no_owner.foreign_owner_matches == 0u &&
            (no_owner.foreign_rejection_mask & (1u << 4)) != 0u;

        JoinPath* current_pair = make_foreign_join(
            hash, &buried_nested->jpath.path, NIL, NIL);
        current_pair->outerjoinpath = right;
        current_pair->innerjoinpath = &buried_nested->jpath.path;
        exact_parameterized_bmo_retainer_audit current{};
        const bool current_rejected = !retain_foreign(current_pair, current);
        non_nest_current_pair_rejected = non_nest_current_pair_rejected && current_rejected &&
            current.current_pair_paths == 2u && current.retained_paths == 0u &&
            current.foreign_decomposition_paths == 0u;

        for (Path* result_leaf : {bare_result, &group_result->path}) {
            JoinPath* result_join = make_foreign_join(
                hash, &owned_nested->jpath.path, list_make1(equality), list_make1(equality));
            result_join->innerjoinpath = result_leaf;
            exact_parameterized_bmo_retainer_audit result_audit{};
            const bool result_kept = !batch_join_path_contains_parallel_execution(result_leaf) &&
                retain_foreign(result_join, result_audit) &&
                result_audit.safe_foreign_paths == 1u && result_audit.retained_paths == 1u &&
                list_length(retained_rel->pathlist) == 1 &&
                linitial(retained_rel->pathlist) == result_join;
            if (result_leaf == bare_result)
                bare_result_preserved = bare_result_preserved && result_kept;
            else
                group_result_preserved = group_result_preserved && result_kept;
        }
        for (Path* parallel_child : {&gather->path, &gather_merge->path}) {
            parallel_unique->subpath = parallel_child;
            parallel_project_set->subpath = parallel_child;
            for (Path* wrapper : {&parallel_unique->path, &parallel_project_set->path}) {
                JoinPath* parallel_join = make_foreign_join(
                    hash, &owned_nested->jpath.path, list_make1(equality), list_make1(equality));
                parallel_join->innerjoinpath = wrapper;
                exact_parameterized_bmo_retainer_audit parallel_audit{};
                const bool parallel_rejected = batch_join_path_contains_parallel_execution(wrapper) &&
                    !retain_foreign(parallel_join, parallel_audit) &&
                    parallel_audit.retained_paths == 0u &&
                    (parallel_audit.foreign_rejection_mask & (1u << 2)) != 0u;
                if (wrapper == &parallel_unique->path)
                    unique_gather_rejected = unique_gather_rejected && parallel_rejected;
                else
                    project_set_gather_rejected = project_set_gather_rejected && parallel_rejected;
            }
        }
    }

    // Restriction lookup follows wrappers independently of execution admission.
    // Gather remains forbidden by the parallel guard tested above.
    const bool descendant_result_leaves_safe =
        !per_row_descendant_owns_restriction(bare_result, bmo_restriction) &&
        !per_row_descendant_owns_restriction(&group_result->path, bmo_restriction);
    auto* copied_restriction = makeNode(RestrictInfo);
    *copied_restriction = *bmo_restriction;
    auto* ppi_only_result = makeNode(Path);
    *ppi_only_result = *bare_result;
    ppi_only_result->param_info = bmo_param;
    auto* custom_only_owner = makeNode(CustomPath);
    *custom_only_owner = *bmo_owner;
    custom_only_owner->path.param_info = nullptr;
    bool descendant_restriction_identity_preserved =
        per_row_descendant_owns_restriction(ppi_only_result, bmo_restriction) &&
        per_row_descendant_owns_restriction(&custom_only_owner->path, bmo_restriction) &&
        per_row_descendant_owns_restriction(&unowned_current->jpath.path, bmo_restriction) &&
        !per_row_descendant_owns_restriction(ppi_only_result, copied_restriction) &&
        !per_row_descendant_owns_restriction(&custom_only_owner->path, copied_restriction) &&
        !per_row_descendant_owns_restriction(&unowned_current->jpath.path, copied_restriction);
    auto* restriction_unique = makeNode(UniquePath);
    *restriction_unique = *unique;
    match_helper_wrapper_probe_init_path(
        &restriction_unique->path, T_Unique, left_rel, create_empty_pathtarget());
    restriction_unique->subpath = &bmo_owner->path;
    auto* restriction_project_set = makeNode(ProjectSetPath);
    match_helper_wrapper_probe_init_path(
        &restriction_project_set->path, T_ProjectSet, left_rel, create_empty_pathtarget());
    restriction_project_set->subpath = &restriction_unique->path;
    auto* restriction_gather = makeNode(GatherPath);
    match_helper_wrapper_probe_init_path(
        &restriction_gather->path, T_Gather, left_rel, create_empty_pathtarget());
    restriction_gather->subpath = &restriction_project_set->path;
    auto* restriction_gather_merge = makeNode(GatherMergePath);
    match_helper_wrapper_probe_init_path(
        &restriction_gather_merge->path, T_GatherMerge, left_rel, create_empty_pathtarget());
    restriction_gather_merge->subpath = &restriction_gather->path;
    bool descendant_wrapped_owner_found = true;
    for (Path* wrapper : {&restriction_unique->path, &restriction_project_set->path,
                         &restriction_gather->path, &restriction_gather_merge->path}) {
        descendant_wrapped_owner_found = descendant_wrapped_owner_found &&
            per_row_descendant_owns_restriction(wrapper, bmo_restriction);
        descendant_restriction_identity_preserved = descendant_restriction_identity_preserved &&
            !per_row_descendant_owns_restriction(wrapper, copied_restriction);
    }

    return bioseq::postgres::support::named_output{
        bioseq::table::match_helper_wrapper_propagation_probe_row{
            .exact_owner_found = exact_owner_found,
            .clone_applied = clone_applied,
            .owner_cloned = owner_cloned,
            .projection_cloned = projection_cloned,
            .material_cloned = material_cloned,
            .memoize_cloned = memoize_cloned,
            .unique_cloned = unique_cloned,
            .append_cloned = append_cloned,
            .helper_on_owner = helper_on_owner,
            .helper_on_projection = helper_on_projection,
            .helper_on_material = helper_on_material,
            .helper_on_memoize = helper_on_memoize,
            .helper_on_unique = helper_on_unique,
            .helper_on_append = helper_on_append,
            .originals_unchanged = originals_unchanged,
            .sibling_unchanged = sibling_unchanged,
            .duplicate_owner_rejected = duplicate_owner_rejected,
            .unknown_wrapper_rejected = unknown_wrapper_rejected,
            .group_result_qual_rejected = group_result_qual_rejected,
            .foreign_hash_preserved = foreign_hash_preserved,
            .foreign_merge_preserved = foreign_merge_preserved,
            .foreign_root_mmsearch_rejected = foreign_root_mmsearch_rejected,
            .foreign_algorithm_mmsearch_rejected = foreign_algorithm_mmsearch_rejected,
            .foreign_unowned_rejected = foreign_unowned_rejected,
            .non_nest_current_pair_rejected = non_nest_current_pair_rejected,
            .bare_result_preserved = bare_result_preserved,
            .group_result_preserved = group_result_preserved,
            .unique_gather_rejected = unique_gather_rejected,
            .project_set_gather_rejected = project_set_gather_rejected,
            .descendant_result_leaves_safe = descendant_result_leaves_safe,
            .descendant_wrapped_owner_found = descendant_wrapped_owner_found,
            .descendant_restriction_identity_preserved = descendant_restriction_identity_preserved,
        }};
}

} // namespace bioseq::table::sql_routines
export namespace bioseq::table::sql_routines {

bioseq::postgres::support::named_output<bioseq::table::base_match_executor_translation_probe_row>
    base_match_executor_translation_probe() {
    const bioseq_stub_oids& oids = get_cached_stub_oids();
    constexpr Index planner_varno = 1;
    constexpr Index executor_varno = 9;
    Var* planner_target = makeVar(planner_varno,
                                  1,
                                  oids.bioseq_seq_type_oid,
                                  /*vartypmod=*/17,
                                  InvalidOid,
                                  0);
    Var* executor_target = makeVar(executor_varno,
                                   1,
                                   oids.bioseq_seq_type_oid,
                                   /*vartypmod=*/17,
                                   InvalidOid,
                                   0);
    Var* wrong_target_attno = makeVar(executor_varno,
                                      2,
                                      oids.bioseq_seq_type_oid,
                                      /*vartypmod=*/17,
                                      InvalidOid,
                                      0);
    Var* planner_marker = makeVar(planner_varno, 2, INT4OID, -1, InvalidOid, 0);
    Var* executor_marker = makeVar(executor_varno, 2, INT4OID, -1, InvalidOid, 0);
    Var* wrong_alias_marker = makeVar(executor_varno + 1, 2, INT4OID, -1, InvalidOid, 0);
    Var* wrong_attno_marker = makeVar(executor_varno, 3, INT4OID, -1, InvalidOid, 0);
    const bool target_offset_accepted = base_match_target_var_translation_agrees(
        planner_target, executor_target, executor_varno, oids);
    const bool target_attno_disagreement_rejected = !base_match_target_var_translation_agrees(
        planner_target, wrong_target_attno, executor_varno, oids);
    const bool exact_site_offset_accepted =
        base_match_site_var_translation_agrees(planner_target,
                                               executor_target,
                                               planner_target,
                                               executor_target,
                                               match_helper_binding_kind::exact_recipe,
                                               oids);
    const bool lineage_alias_disagreement_rejected =
        base_match_site_var_translation_agrees(planner_marker,
                                               executor_marker,
                                               planner_target,
                                               executor_target,
                                               match_helper_binding_kind::unique_lineage,
                                               oids) &&
        !base_match_site_var_translation_agrees(planner_marker,
                                                wrong_alias_marker,
                                                planner_target,
                                                executor_target,
                                                match_helper_binding_kind::unique_lineage,
                                                oids);

    auto* outer_source = makeNode(Result);
    outer_source->plan.targetlist =
        list_make2(makeTargetEntry(reinterpret_cast<Expr*>(copyObject(executor_marker)),
                                   /*resno=*/1,
                                   nullptr,
                                   false),
                   makeTargetEntry(reinterpret_cast<Expr*>(copyObject(wrong_attno_marker)),
                                   /*resno=*/2,
                                   nullptr,
                                   false));
    auto* inner_source = makeNode(Result);
    inner_source->plan.targetlist = static_cast<List*>(copyObject(outer_source->plan.targetlist));
    auto* carrier_scan = makeNode(CustomScan);
    carrier_scan->custom_plans = list_make2(&outer_source->plan, &inner_source->plan);
    carrier_scan->custom_scan_tlist = static_cast<List*>(copyObject(outer_source->plan.targetlist));
    auto special_var = [](const Index varno, const AttrNumber attno) {
        return reinterpret_cast<Node*>(makeVar(varno, attno, INT4OID, -1, InvalidOid, 0));
    };
    const bool outer_slot_accepted = finalized_match_helper_argument_translation_agrees(
        carrier_scan, special_var(OUTER_VAR, 1), reinterpret_cast<Node*>(planner_marker));
    const bool outer_wrong_slot_rejected = !finalized_match_helper_argument_translation_agrees(
        carrier_scan, special_var(OUTER_VAR, 2), reinterpret_cast<Node*>(planner_marker));
    const bool inner_slot_accepted = finalized_match_helper_argument_translation_agrees(
        carrier_scan, special_var(INNER_VAR, 1), reinterpret_cast<Node*>(planner_marker));
    const bool inner_wrong_slot_rejected = !finalized_match_helper_argument_translation_agrees(
        carrier_scan, special_var(INNER_VAR, 2), reinterpret_cast<Node*>(planner_marker));
    const bool index_slot_accepted = finalized_match_helper_argument_translation_agrees(
        carrier_scan, special_var(INDEX_VAR, 1), reinterpret_cast<Node*>(planner_marker));
    const bool index_wrong_slot_rejected = !finalized_match_helper_argument_translation_agrees(
        carrier_scan, special_var(INDEX_VAR, 2), reinterpret_cast<Node*>(planner_marker));
    const bool special_out_of_range_rejected = !finalized_match_helper_argument_translation_agrees(
        carrier_scan, special_var(OUTER_VAR, 3), reinterpret_cast<Node*>(planner_marker));
    return bioseq::postgres::support::named_output{
        bioseq::table::base_match_executor_translation_probe_row{
            .target_offset_accepted = target_offset_accepted,
            .target_attno_disagreement_rejected = target_attno_disagreement_rejected,
            .exact_site_offset_accepted = exact_site_offset_accepted,
            .lineage_alias_disagreement_rejected = lineage_alias_disagreement_rejected,
            .outer_slot_accepted = outer_slot_accepted,
            .outer_wrong_slot_rejected = outer_wrong_slot_rejected,
            .inner_slot_accepted = inner_slot_accepted,
            .inner_wrong_slot_rejected = inner_wrong_slot_rejected,
            .index_slot_accepted = index_slot_accepted,
            .index_wrong_slot_rejected = index_wrong_slot_rejected,
            .special_out_of_range_rejected = special_out_of_range_rejected,
        }};
}

} // namespace bioseq::table::sql_routines
export namespace bioseq::table::sql_routines {

bioseq::postgres::support::named_output<
    bioseq::table::match_owner_finalization_scope_diagnostics_row>
    match_owner_finalization_scope_diagnostics(bool reset) {
#ifdef BIOSEQ_TEST_INDEXES
    if (reset) {
        match_owner_finalization_scopes_entered = 0u;
        match_owner_finalization_scopes_exited = 0u;
    }
    const std::uint64_t active =
        match_owner_finalization_scopes_entered >= match_owner_finalization_scopes_exited
            ? match_owner_finalization_scopes_entered - match_owner_finalization_scopes_exited
            : std::numeric_limits<std::uint64_t>::max();
    return bioseq::postgres::support::named_output{
        bioseq::table::match_owner_finalization_scope_diagnostics_row{
            .scopes_entered = static_cast<std::int64_t>(match_owner_finalization_scopes_entered),
            .scopes_exited = static_cast<std::int64_t>(match_owner_finalization_scopes_exited),
            .scopes_active = static_cast<std::int64_t>(active),
        }};
#else
    static_cast<void>(reset);
    bioseq::postgres::support::raise_sql_error(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        {
            .message = "match-owner finalization diagnostics require a test build",
            .detail = "The extension was built without BIOSEQ_TEST_INDEXES.",
        });
#endif
}

} // namespace bioseq::table::sql_routines
