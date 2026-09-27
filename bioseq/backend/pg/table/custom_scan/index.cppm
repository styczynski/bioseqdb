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
            makeVar(target_relid, SelfItemPointerAttributeNumber, TIDOID, -1, InvalidOid, 0)),
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

    return bioseq::postgres::support::named_output{
        bioseq::table::batch_join_projection_guard_probe_row{
            .accepts_exact_helper = accepts_exact_helper,
            .rejects_unrelated_phv = rejects_unrelated_phv,
            .rejects_volatile_mixed_expression = rejects_volatile_mixed_expression,
            .preserves_wrapper_cost_owner = preserves_wrapper_cost_owner,
            .accepts_plain_target_var = accepts_plain_target_var,
            .rejects_plain_target_sequence_var = rejects_plain_target_sequence_var,
            .rejects_whole_row_var = rejects_whole_row_var,
            .rejects_system_var = rejects_system_var,
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
